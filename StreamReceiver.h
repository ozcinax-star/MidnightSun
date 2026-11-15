// StreamReceiver.h
//
// Client-side UDP transport for the game streaming pipeline. Receives the
// fragmented GSMR packet stream from StreamSender, reassembles logical
// units, reorders video through a small jitter buffer, and delivers
// completed frames through callbacks.
//
// Connection model: start() "connects" the UDP socket to the server and
// immediately sends a keepalive ping -- that first datagram is what makes
// StreamSender discover this client and begin streaming. There is no
// handshake; if nothing arrives for 3 seconds the receiver declares a
// disconnect (onDisconnect fires once per episode) and keeps pinging the
// server once a second until traffic resumes.
//
// Threading model:
//   - start()/stop() from the control thread.
//   - "GSMR-Recv" thread: WSAPoll (10 ms) + drain loop. ALL callbacks
//     (onVideoFrame, onAudioPacket, onDisconnect) are invoked on this
//     thread -- keep them fast (hand off to a decoder queue) or the
//     socket buffer backs up.
//   - "GSMR-RecvStats" thread: folds rolling counters into a StreamStats
//     snapshot once per second.
//   - sendInput() and getStats() may be called from any thread.
//
// Video delivery pipeline (per packet):
//   1. Sequence window: packets older than (max_seq - 8) are discarded
//      immediately; late-but-in-window packets count as out-of-order and
//      still reassemble.
//   2. Reassembly map keyed by sequence; an incomplete unit whose first
//      fragment arrived more than 80 ms ago is evicted with a warning
//      (checked on every receive-loop pass).
//   3. Jitter buffer: 4 slots ordered by timestamp. A frame whose sequence
//      directly follows the last delivered one is released immediately
//      (zero added latency on a healthy link); after a loss, delivery
//      resumes when the buffer fills or the held frame ages past 50 ms.
//      Frames older than what was already delivered are discarded --
//      onVideoFrame never goes backwards.
//
// Loss handling: the receiver asks the server for a fresh IDR on its own
// (KeepaliveKind::kKeyframeRequest, throttled to one per 250 ms) whenever
// an incomplete frame is evicted or delivery had to skip a gap; the
// application can also call requestKeyframe() directly (e.g. while the
// decoder is not primed yet). The stats thread additionally ships a 1 Hz
// receiver report (frames expected/completed) that drives the server's
// adaptive bitrate controller.
//
// Encryption: call setPsk() before start(). With a non-empty PSK every
// datagram in both directions is AES-256-GCM sealed (see Crypto.h);
// inbound datagrams that fail authentication are dropped before any
// state is touched.
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

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

class StreamReceiver {
public:
    StreamReceiver(const std::string& serverIp, uint16_t serverPort);
    ~StreamReceiver(); // calls stop() if still running

    StreamReceiver(const StreamReceiver&) = delete;
    StreamReceiver& operator=(const StreamReceiver&) = delete;

    // Set before start(); all invoked on the recv thread.
    std::function<void(EncodedFrame)>                          onVideoFrame;
    std::function<void(std::vector<uint8_t>, uint64_t tsUs)>   onAudioPacket;
    std::function<void()>                                      onDisconnect;

    // Configures transport encryption from the pre-shared key (empty =
    // off). Call before start(); returns false with lastError() set if
    // CNG setup failed.
    bool setPsk(const std::string& psk);

    // Initializes Winsock, resolves and connects to the server, applies the
    // socket options, sends the discovery keepalive, and starts the worker
    // threads. Returns false with lastError() set on failure.
    bool start();

    // Asks the server for a fresh IDR (throttled internally to one request
    // per 250 ms). Called automatically on detected loss; safe from any
    // thread.
    void requestKeyframe();

    // Joins the threads and closes the socket. Safe when not running.
    void stop();

    // Fire-and-forget: sends one input packet (mouse/keyboard/gamepad) to
    // the server. Callable from any thread (the input thread, typically).
    // Packets larger than one datagram payload are fragmented on the wire,
    // but the server only accepts single-fragment input -- keep input
    // packets <= kMaxPayloadSize bytes.
    void sendInput(const std::vector<uint8_t>& inputPacket);

    // Thread-safe snapshot, refreshed once per second.
    StreamStats getStats() const;

    bool isRunning() const { return m_running.load(std::memory_order_acquire); }

    // Most recent failure description (start() errors, socket errors).
    std::string lastError() const;

private:
    static constexpr size_t   kJitterSlots         = 4;
    static constexpr int32_t  kReorderWindow       = 8;         // sequences
    static constexpr uint64_t kSlotTimeoutUs       = 80'000;    // reassembly eviction
    static constexpr uint64_t kJitterHoldUs        = 50'000;    // max wait for a gap
    static constexpr uint64_t kDisconnectTimeoutUs = 3'000'000;
    static constexpr uint64_t kReconnectPingUs     = 1'000'000;
    static constexpr uint64_t kKeepalivePingUs     = 500'000;

    // Reconstructs a 64-bit microsecond timeline from the 32-bit wire
    // timestamps (which wrap every ~71 minutes). Signed-delta accumulation
    // tolerates out-of-order arrivals around the wrap point.
    struct TimestampUnwrapper {
        uint64_t value  = 0;
        uint32_t last32 = 0;
        bool     primed = false;

        uint64_t unwrap(uint32_t ts32) {
            if (!primed) {
                primed = true;
                value  = ts32;
            } else {
                value += static_cast<int32_t>(ts32 - last32);
            }
            last32 = ts32;
            return value;
        }
    };

    struct PendingFrame {
        EncodedFrame frame;
        uint32_t     sequence  = 0;
        uint64_t     arrivalUs = 0;
    };

    void recvThreadProc();
    void statsThreadProc();
    void handlePacket(const PacketHeader& header, const uint8_t* payload,
                      size_t payloadBytes, uint64_t nowUs);
    void handleVideo(const PacketHeader& header, const uint8_t* payload,
                     size_t payloadBytes, uint64_t nowUs);
    void handleAudio(const PacketHeader& header, const uint8_t* payload,
                     size_t payloadBytes, uint64_t nowUs);
    void handleKeepalive(const PacketHeader& header, uint64_t nowUs);

    // Validates one fragment and folds it into `slots`. Returns true and
    // fills `outSlot` (moving it out of the map) when the unit completes.
    bool reassemble(std::unordered_map<uint32_t, ReassemblySlot>& slots,
                    const PacketHeader& header, const uint8_t* payload,
                    size_t payloadBytes, uint64_t nowUs, ReassemblySlot& outSlot);

    // Applies the (max_seq - 8) window. Returns false if the packet must be
    // discarded; counts out-of-order arrivals. `countExpected` must be true
    // ONLY for video: the receiver report compares expected-vs-completed
    // VIDEO frames, so audio sequences must not inflate the expected count.
    bool checkSequenceWindow(uint32_t sequence, uint32_t& maxSeq, bool& maxSeqValid,
                             bool countExpected);

    void evictStaleSlots(uint64_t nowUs);
    void insertIntoJitterBuffer(PendingFrame&& pending);
    void flushJitterBuffer(uint64_t nowUs);
    void deliverFrame(PendingFrame&& pending);
    void sendKeepalivePing(uint64_t nowUs);
    // One keepalive-family control datagram (kind = KeepaliveKind::*).
    void sendControl(uint16_t kind, const uint8_t* payload, size_t payloadBytes);
    bool sendRaw(const uint8_t* data, int bytes); // seals when a PSK is set
    void setError(std::string message);

    // --- configuration / socket ---
    std::string m_serverIp;
    uint16_t    m_serverPort;
    SOCKET      m_socket = INVALID_SOCKET;
    bool        m_wsaInitialized = false;

    // --- reassembly + ordering (recv thread only) ---
    std::unordered_map<uint32_t, ReassemblySlot> m_videoSlots;
    std::unordered_map<uint32_t, ReassemblySlot> m_audioSlots;
    uint32_t m_maxVideoSeq = 0;
    uint32_t m_maxAudioSeq = 0;
    bool     m_videoSeqValid = false;
    bool     m_audioSeqValid = false;
    TimestampUnwrapper m_videoTimestamps;
    TimestampUnwrapper m_audioTimestamps;

    // --- jitter buffer (recv thread only), sorted by frame timestamp ---
    std::array<PendingFrame, kJitterSlots> m_jitter;
    size_t   m_jitterCount = 0;
    uint32_t m_lastDeliveredSeq = 0;
    uint64_t m_lastDeliveredTs  = 0;
    bool     m_hasDelivered     = false;

    // --- connection liveness (recv thread only) ---
    bool m_disconnected = false;

    // --- rolling stats ---
    std::atomic<uint64_t> m_bytesReceived{0};
    std::atomic<uint32_t> m_droppedFrames{0};
    std::atomic<uint32_t> m_outOfOrderCount{0};
    std::atomic<uint32_t> m_windowExpectedFrames{0}; // reset each stats fold
    std::atomic<uint32_t> m_windowCompletedFrames{0};
    std::atomic<float>    m_rttMs{0.0f};

    mutable std::mutex m_statsMutex;
    StreamStats        m_stats;

    mutable std::mutex m_errorMutex;
    std::string        m_lastError;

    // --- outbound sequencing (any thread) ---
    std::atomic<uint32_t> m_inputSeq{0};
    std::atomic<uint32_t> m_keepaliveSeq{0};
    std::atomic<uint64_t> m_lastKeyframeReqUs{0}; // requestKeyframe() throttle

    // --- transport encryption (configured before start()) ---
    PacketCrypto m_crypto;

    // --- thread control ---
    std::thread       m_recvThread;
    std::thread       m_statsThread;
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_stopRequested{false};
};
