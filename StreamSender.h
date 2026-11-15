// StreamSender.h
//
// Server-side UDP transport for the game streaming pipeline. Takes encoded
// video frames (and raw PCM audio), fragments them per NetworkProtocol.h,
// and sends them to the most recently seen client.
//
// Connection model: the sender binds its own port and never initiates --
// the FIRST datagram that arrives from any (ip, port) pair becomes the
// client ("connect" event, logged); a later datagram from a different pair
// switches the target. There is no handshake and no reliability: this is a
// fire-and-forget media transport, loss recovery is the application's job
// (call NvencEncoder::requestKeyframe() when the receiver reports a gap).
//
// Threading model:
//   - start()/stop() from the control thread. stop() flushes whatever is
//     still queued (if a client is known), then joins both worker threads.
//   - sendFrame()/sendAudio() must be called from ONE producer thread (the
//     capture/encode consumer thread in this project). The queue between
//     producer and send thread is a fixed 64-slot lock-free ring.
//   - sendKeepalive() and getStats() may be called from any thread.
//   - "GSMR-Send" thread: drains the queue, fragments, sendto()s; also
//     polls the socket (WSAPoll, 10 ms) for inbound datagrams -- client
//     discovery, keepalive pings (echoed back) and echoes (RTT), and input
//     packets. Sends an automatic keepalive ping after 500 ms of idle.
//   - "GSMR-SendStats" thread: folds the rolling counters into a
//     StreamStats snapshot once per second.
//
// Overflow policy: if the ring is full (client/network too slow), the
// oldest queued frame is dropped to make room -- unless that frame is a
// keyframe, in which case the incoming non-keyframe is dropped instead.
// Keyframes are never silently discarded; in the pathological case where
// the ring is full of keyframes, the producer waits briefly for the send
// thread to drain one (bounded, then drops with a logged warning).
//
// Requires: MSVC 2022, C++20, Winsock2 (ws2_32.lib -- linked via #pragma
// comment in the .cpp).

#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>

#include "Crypto.h"
#include "NetworkProtocol.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

class StreamSender {
public:
    explicit StreamSender(uint16_t listenPort);
    ~StreamSender(); // calls stop() if still running

    StreamSender(const StreamSender&) = delete;
    StreamSender& operator=(const StreamSender&) = delete;

    // Optional, set before start(): delivers reassembled INPUT packets from
    // the client (mouse/keyboard/gamepad), invoked on the send thread.
    // Input packets are expected to be single-fragment (<= kMaxPayloadSize
    // bytes); larger ones are dropped with a warning.
    std::function<void(std::vector<uint8_t> payload, uint64_t timestampUs)> onInputPacket;

    // Optional, set before start(); both invoked on the send thread.
    // onKeyframeRequest: the client detected unconcealable loss -- wire it
    // to NvencEncoder::requestKeyframe(). onReceiverReport: the client's
    // 1 Hz loss report (frames expected/completed over its last window) --
    // feeds the adaptive bitrate controller.
    std::function<void()>                                   onKeyframeRequest;
    std::function<void(uint32_t expected, uint32_t completed)> onReceiverReport;

    // Optional, set before start(): with setRequireApproval(true), fires
    // (send thread, once per candidate) when a NEW endpoint wants to become
    // the streaming target. The endpoint stays "pending" -- its datagrams
    // are ignored -- until resolvePending() is called from any thread.
    std::function<void(const std::string& addr)> onClientRequest;

    // Host-side connect approval. Off by default (legacy behavior: first
    // valid datagram wins). Call before start().
    void setRequireApproval(bool require) { m_requireApproval = require; }

    // Resolves the pending endpoint announced via onClientRequest.
    // allow=true: it becomes the streaming target and its IP is trusted
    // for the rest of the session (reconnects skip the prompt).
    // allow=false: its IP is ignored for the rest of the session.
    // Any thread; no-op when nothing is pending.
    void resolvePending(bool allow);

    // Configures transport encryption from the pre-shared key (empty =
    // off). Call before start(). With a PSK set, inbound datagrams that
    // fail authentication are ignored completely -- including for client
    // discovery, which closes the "any datagram becomes the streaming
    // target" hole.
    bool setPsk(const std::string& psk);

    // Initializes Winsock, binds the UDP socket to listenPort, applies the
    // 4 MB kernel buffers + DSCP EF QoS marking, and starts the worker
    // threads. Returns false with lastError() set on failure.
    bool start();

    // Graceful shutdown: lets the send thread flush everything still queued
    // (when a client is known), joins the threads, closes the socket. Safe
    // to call when not running.
    void stop();

    // Enqueues one encoded video frame. Never blocks on the network; see
    // the overflow policy above. Call from the single producer thread.
    // Takes the frame by value so the caller can std::move() the bitstream
    // in; the payload is moved (not copied) into the outbound ring.
    void sendFrame(EncodedFrame frame);

    // Enqueues one audio packet (raw PCM or encoded -- the transport does
    // not care). Same producer-thread and overflow rules as sendFrame().
    void sendAudio(const uint8_t* pcm, size_t bytes, uint64_t timestampUs);

    // Sends a keepalive ping immediately (any thread). The send thread also
    // does this automatically after 500 ms without traffic, so calling it
    // manually is optional.
    void sendKeepalive();

    // Thread-safe snapshot, refreshed once per second. packetLossPct
    // reflects the client's receiver reports.
    StreamStats getStats() const;

    // "ip:port" of the current streaming target, or empty when no client
    // is known yet. Any thread (used by the server status window).
    std::string clientAddress() const;

    bool isRunning() const { return m_running.load(std::memory_order_acquire); }

    // Most recent failure description (start() errors, socket errors). Set
    // from whichever thread failed; read it after a method reports failure.
    std::string lastError() const;

private:
    // One queued logical unit (a video frame or an audio packet).
    struct OutboundItem {
        PacketType           type = PacketType::Video;
        std::vector<uint8_t> payload;
        uint64_t             timestampUs = 0;
        bool                 keyframe    = false;
    };

    // Fixed 64-slot ring between the producer thread and the send thread.
    //
    // Lock-free protocol: head/tail are monotonically increasing counters
    // (masked into the slot array), each slot is an atomic OutboundItem
    // pointer. The consumer takes a slot with exchange(nullptr) and then
    // CASes head forward. Drop-oldest works because the producer may do the
    // exact same exchange+CAS on the head slot when the ring is full:
    // whichever side's exchange returns non-null owns the item, the losing
    // CAS fails benignly, and a consumer that finds a stolen (null) slot
    // just advances and retries. Counters never repeat, so there is no ABA.
    class OutboundQueue {
    public:
        static constexpr size_t kCapacity = 64;

        ~OutboundQueue();

        // Enqueues `item` (takes ownership). When full, drops the oldest
        // queued item to make room, except never a keyframe -- see the
        // policy in the class comment. Increments `droppedFrames` for every
        // video item discarded (either the victim or the incoming one).
        // Returns false only if `item` itself was dropped.
        bool enqueue(OutboundItem* item, std::atomic<uint32_t>& droppedFrames);

        // Returns the oldest item (caller owns it) or nullptr if empty.
        // Single consumer.
        OutboundItem* dequeue();

        bool empty() const;

    private:
        std::atomic<OutboundItem*> m_slots[kCapacity] = {};
        // Mirrors each slot's keyframe flag so the drop-oldest decision
        // never dereferences a pointer the consumer may be freeing. Written
        // by the producer before publishing the slot; a slot's flag stays
        // valid for that entry until the producer itself reuses the slot.
        std::atomic<bool>          m_slotKeyframe[kCapacity] = {};
        std::atomic<uint64_t>      m_head{0};
        std::atomic<uint64_t>      m_tail{0};
    };

    void sendThreadProc();
    void statsThreadProc();
    void drainInbound(uint64_t nowUs);
    void handleInbound(const PacketHeader& header, const uint8_t* payload,
                       size_t payloadBytes, const sockaddr_in& from, uint64_t nowUs);
    // Fragments and transmits one logical unit to the current client.
    // Returns false (with the failure logged) if the socket send failed.
    bool transmit(PacketType type, uint32_t sequence, const uint8_t* payload,
                  size_t bytes, uint32_t timestampUs32, bool keyframe);
    bool sendDatagram(const uint8_t* datagram, int bytes); // seals when PSK set
    bool sendDatagramTo(const uint8_t* datagram, int bytes, const sockaddr_in& to);
    void sendKeepalivePing(uint64_t nowUs);
    void setError(std::string message);

    // --- configuration / socket ---
    uint16_t m_listenPort;
    SOCKET   m_socket = INVALID_SOCKET;
    bool     m_wsaInitialized = false;

    // --- client address + approval state (guarded by m_clientMutex) ---
    mutable std::mutex m_clientMutex;
    sockaddr_in        m_clientAddr = {};
    bool               m_clientKnown = false;
    bool               m_requireApproval = false; // start() öncesi ayarlanır
    sockaddr_in        m_pendingAddr = {};
    bool               m_pendingKnown = false;
    std::vector<uint32_t> m_approvedIps; // oturum boyu güvenilen IP'ler
    std::vector<uint32_t> m_deniedIps;   // oturum boyu engellenen IP'ler

    // --- queue / sequencing (send thread owns the counters) ---
    OutboundQueue         m_queue;
    uint32_t              m_videoSeq = 0;
    uint32_t              m_audioSeq = 0;
    uint32_t              m_inputSeq = 0; // reserved: server->client input/haptics
    std::atomic<uint32_t> m_keepaliveSeq{0}; // atomic: sendKeepalive() is any-thread
    bool                  m_warnedNoClient = false; // send thread only; throttles logging

    // --- rolling stats (written by send thread, folded by stats thread) ---
    std::atomic<uint64_t> m_bytesSent{0};
    std::atomic<uint32_t> m_droppedFrames{0};
    std::atomic<uint32_t> m_outOfOrderCount{0}; // input packets arriving late
    std::atomic<float>    m_rttMs{0.0f};
    std::atomic<float>    m_reportedLossPct{0.0f}; // from client receiver reports
    uint32_t              m_lastInputSeq = 0;
    bool                  m_inputSeqValid = false;

    // --- transport encryption (configured before start()) ---
    PacketCrypto m_crypto;

    mutable std::mutex m_statsMutex;
    StreamStats        m_stats; // last folded snapshot

    mutable std::mutex m_errorMutex;
    std::string        m_lastError;

    // --- thread control ---
    std::thread       m_sendThread;
    std::thread       m_statsThread;
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_stopRequested{false};
};
