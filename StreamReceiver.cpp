// StreamReceiver.cpp -- see StreamReceiver.h for the design overview.

#include "StreamReceiver.h"

#include <windows.h> // SetThreadDescription; winsock2.h already in our header

#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <utility>

#pragma comment(lib, "ws2_32.lib")

namespace {

// ---------------------------------------------------------------------------
// Logging. Same shape as the other modules: debugger + stderr, stack buffer
// only, called on errors and state changes -- never per packet.
// ---------------------------------------------------------------------------
void LogF(const char* fmt, ...)
{
    char buf[1024];
    va_list args;
    va_start(args, fmt);
    int n = _vsnprintf_s(buf, sizeof(buf), sizeof(buf) - 3, fmt, args);
    va_end(args);
    if (n < 0) n = static_cast<int>(strlen(buf));
    if (n == 0 || buf[n - 1] != '\n') {
        buf[n] = '\n';
        buf[n + 1] = '\0';
    }
    OutputDebugStringA(buf);
    fputs(buf, stderr);
}

std::string WsaErrorString(int err)
{
    char sysMsg[512] = {};
    DWORD len = FormatMessageA(
        FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, static_cast<DWORD>(err),
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        sysMsg, static_cast<DWORD>(sizeof(sysMsg)), nullptr);
    while (len > 0 && (sysMsg[len - 1] == '\r' || sysMsg[len - 1] == '\n'))
        sysMsg[--len] = '\0';

    char buf[600];
    if (len > 0)
        sprintf_s(buf, "%d (%s)", err, sysMsg);
    else
        sprintf_s(buf, "%d", err);
    return buf;
}

uint64_t SteadyMicros()
{
    using namespace std::chrono;
    return static_cast<uint64_t>(
        duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count());
}

bool ConfigureSocket(SOCKET s, const char* tag)
{
    u_long nonBlocking = 1;
    if (ioctlsocket(s, FIONBIO, &nonBlocking) == SOCKET_ERROR) {
        LogF("[%s] ioctlsocket(FIONBIO) failed: %s", tag,
             WsaErrorString(WSAGetLastError()).c_str());
        return false;
    }

    constexpr int kBufBytes = 4 * 1024 * 1024;
    if (setsockopt(s, SOL_SOCKET, SO_RCVBUF,
                   reinterpret_cast<const char*>(&kBufBytes), sizeof(kBufBytes)) == SOCKET_ERROR)
        LogF("[%s] setsockopt(SO_RCVBUF) failed: %s", tag,
             WsaErrorString(WSAGetLastError()).c_str());
    if (setsockopt(s, SOL_SOCKET, SO_SNDBUF,
                   reinterpret_cast<const char*>(&kBufBytes), sizeof(kBufBytes)) == SOCKET_ERROR)
        LogF("[%s] setsockopt(SO_SNDBUF) failed: %s", tag,
             WsaErrorString(WSAGetLastError()).c_str());

    constexpr int kDscpEf = 0xB8; // expedited forwarding
    if (setsockopt(s, IPPROTO_IP, IP_TOS,
                   reinterpret_cast<const char*>(&kDscpEf), sizeof(kDscpEf)) == SOCKET_ERROR)
        LogF("[%s] setsockopt(IP_TOS=0xB8) not applied (%s) -- QoS marking disabled",
             tag, WsaErrorString(WSAGetLastError()).c_str());

    return true;
}

} // namespace

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

StreamReceiver::StreamReceiver(const std::string& serverIp, uint16_t serverPort)
    : m_serverIp(serverIp), m_serverPort(serverPort)
{
}

StreamReceiver::~StreamReceiver()
{
    stop();
}

bool StreamReceiver::start()
{
    if (m_running.load(std::memory_order_acquire))
        return true;

    WSADATA wsaData = {};
    int rc = WSAStartup(MAKEWORD(2, 2), &wsaData);
    if (rc != 0) {
        setError("WSAStartup failed: " + WsaErrorString(rc));
        LogF("[StreamReceiver] %s", lastError().c_str());
        return false;
    }
    m_wsaInitialized = true;

    // getaddrinfo handles dotted quads and hostnames alike.
    addrinfo hints = {};
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_protocol = IPPROTO_UDP;
    addrinfo* resolved = nullptr;
    const std::string portStr = std::to_string(m_serverPort);
    rc = getaddrinfo(m_serverIp.c_str(), portStr.c_str(), &hints, &resolved);
    if (rc != 0 || !resolved) {
        setError("getaddrinfo(" + m_serverIp + ") failed: " + WsaErrorString(rc));
        LogF("[StreamReceiver] %s", lastError().c_str());
        stop();
        return false;
    }

    m_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (m_socket == INVALID_SOCKET) {
        setError("socket() failed: " + WsaErrorString(WSAGetLastError()));
        LogF("[StreamReceiver] %s", lastError().c_str());
        freeaddrinfo(resolved);
        stop();
        return false;
    }

    // Connecting a UDP socket pins the peer (send()/recv() instead of
    // sendto()/recvfrom()) and makes the kernel filter out datagrams from
    // anyone but the server.
    if (connect(m_socket, resolved->ai_addr, static_cast<int>(resolved->ai_addrlen)) ==
        SOCKET_ERROR) {
        setError("connect(" + m_serverIp + ":" + portStr +
                 ") failed: " + WsaErrorString(WSAGetLastError()));
        LogF("[StreamReceiver] %s", lastError().c_str());
        freeaddrinfo(resolved);
        stop();
        return false;
    }
    freeaddrinfo(resolved);

    if (!ConfigureSocket(m_socket, "StreamReceiver")) {
        setError("socket configuration failed");
        stop();
        return false;
    }

    // Fresh per-session state so a restart (or a restarted server with
    // reset sequence numbers) is not mistaken for ancient out-of-window
    // traffic.
    m_videoSeqValid   = false;
    m_audioSeqValid   = false;
    m_hasDelivered    = false;
    m_disconnected    = false;
    m_videoTimestamps = {};
    m_audioTimestamps = {};

    m_stopRequested.store(false, std::memory_order_release);
    m_running.store(true, std::memory_order_release);
    m_recvThread  = std::thread(&StreamReceiver::recvThreadProc, this);
    m_statsThread = std::thread(&StreamReceiver::statsThreadProc, this);

    LogF("[StreamReceiver] connecting to %s:%u", m_serverIp.c_str(),
         static_cast<unsigned>(m_serverPort));
    return true;
}

void StreamReceiver::stop()
{
    m_stopRequested.store(true, std::memory_order_release);
    if (m_recvThread.joinable())
        m_recvThread.join();
    if (m_statsThread.joinable())
        m_statsThread.join();
    m_running.store(false, std::memory_order_release);

    if (m_socket != INVALID_SOCKET) {
        closesocket(m_socket);
        m_socket = INVALID_SOCKET;
    }
    if (m_wsaInitialized) {
        WSACleanup();
        m_wsaInitialized = false;
    }

    m_videoSlots.clear();
    m_audioSlots.clear();
    for (PendingFrame& slot : m_jitter)
        slot = {};
    m_jitterCount = 0;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

bool StreamReceiver::setPsk(const std::string& psk)
{
    if (m_running.load(std::memory_order_acquire)) {
        setError("setPsk must be called before start()");
        return false;
    }
    if (!m_crypto.init(psk, /*isServer=*/false)) {
        setError("crypto init failed: " + m_crypto.lastError());
        LogF("[StreamReceiver] %s", lastError().c_str());
        return false;
    }
    if (m_crypto.enabled())
        LogF("[StreamReceiver] transport encryption enabled (AES-256-GCM)");
    return true;
}

void StreamReceiver::requestKeyframe()
{
    if (!m_running.load(std::memory_order_acquire))
        return;

    // At most one request per 250 ms, no matter how many loss events fire.
    constexpr uint64_t kThrottleUs = 250'000;
    const uint64_t nowUs = SteadyMicros();
    uint64_t last = m_lastKeyframeReqUs.load(std::memory_order_relaxed);
    if (nowUs - last < kThrottleUs)
        return;
    if (!m_lastKeyframeReqUs.compare_exchange_strong(last, nowUs,
                                                     std::memory_order_relaxed))
        return; // another thread just sent one

    sendControl(KeepaliveKind::kKeyframeRequest, nullptr, 0);
}

void StreamReceiver::sendControl(uint16_t kind, const uint8_t* payload,
                                 size_t payloadBytes)
{
    if (payloadBytes > kMaxPayloadSize)
        return;
    PacketHeader header;
    header.type        = PacketType::Keepalive;
    header.sequence    = m_keepaliveSeq.fetch_add(1, std::memory_order_relaxed);
    header.fragmentId  = kind;
    header.totalFrags  = 1;
    header.timestampUs = static_cast<uint32_t>(SteadyMicros());
    header.flags       = 0;
    uint8_t dgram[kMaxDatagramSize];
    serialize(header, dgram);
    if (payloadBytes > 0)
        memcpy(dgram + kPacketHeaderSize, payload, payloadBytes);
    sendRaw(dgram, static_cast<int>(kPacketHeaderSize + payloadBytes));
}

void StreamReceiver::sendInput(const std::vector<uint8_t>& inputPacket)
{
    if (!m_running.load(std::memory_order_acquire))
        return;

    const size_t bytes = inputPacket.size();
    const size_t totalFrags =
        bytes == 0 ? 1 : (bytes + kMaxPayloadSize - 1) / kMaxPayloadSize;
    if (totalFrags > 0xFFFF)
        return; // absurd; silently refuse rather than flood the link

    const uint32_t sequence = m_inputSeq.fetch_add(1, std::memory_order_relaxed);
    const uint32_t ts32     = static_cast<uint32_t>(SteadyMicros());

    uint8_t dgram[kMaxDatagramSize];
    size_t offset = 0;
    for (size_t i = 0; i < totalFrags; ++i) {
        const size_t chunk = (bytes - offset < kMaxPayloadSize) ? bytes - offset
                                                                : kMaxPayloadSize;
        PacketHeader header;
        header.type        = PacketType::Input;
        header.sequence    = sequence;
        header.fragmentId  = static_cast<uint16_t>(i);
        header.totalFrags  = static_cast<uint16_t>(totalFrags);
        header.timestampUs = ts32;
        header.flags       = (i == totalFrags - 1) ? PacketFlags::kLastFrag : 0;
        serialize(header, dgram);
        if (chunk > 0)
            memcpy(dgram + kPacketHeaderSize, inputPacket.data() + offset, chunk);
        if (!sendRaw(dgram, static_cast<int>(kPacketHeaderSize + chunk)))
            return; // fire-and-forget: logged inside sendRaw, nothing to retry
        offset += chunk;
    }
}

StreamStats StreamReceiver::getStats() const
{
    std::lock_guard<std::mutex> lock(m_statsMutex);
    return m_stats;
}

std::string StreamReceiver::lastError() const
{
    std::lock_guard<std::mutex> lock(m_errorMutex);
    return m_lastError;
}

void StreamReceiver::setError(std::string message)
{
    std::lock_guard<std::mutex> lock(m_errorMutex);
    m_lastError = std::move(message);
}

// ---------------------------------------------------------------------------
// Receive thread
// ---------------------------------------------------------------------------

void StreamReceiver::recvThreadProc()
{
    SetThreadDescription(GetCurrentThread(), L"GSMR-Recv");

    // The discovery ping: the server learns our address from this.
    uint64_t nowUs = SteadyMicros();
    sendKeepalivePing(nowUs);

    uint64_t lastPacketUs = nowUs;
    uint64_t lastPingUs   = nowUs;

    while (!m_stopRequested.load(std::memory_order_acquire)) {
        WSAPOLLFD pfd = {};
        pfd.fd     = m_socket;
        pfd.events = POLLRDNORM;
        const int pollRc = WSAPoll(&pfd, 1, 10);

        nowUs = SteadyMicros();
        if (pollRc == SOCKET_ERROR) {
            LogF("[StreamReceiver] WSAPoll failed: %s -- recv thread exiting",
                 WsaErrorString(WSAGetLastError()).c_str());
            setError("WSAPoll failed: " + WsaErrorString(WSAGetLastError()));
            return;
        }

        if (pollRc > 0 && (pfd.revents & (POLLRDNORM | POLLERR))) {
            for (;;) {
                uint8_t buf[kMaxDatagramSize + 64];
                uint8_t plain[kMaxDatagramSize + 64];
                const int n = recv(m_socket, reinterpret_cast<char*>(buf), sizeof(buf), 0);
                if (n == SOCKET_ERROR) {
                    const int err = WSAGetLastError();
                    if (err == WSAEWOULDBLOCK)
                        break;
                    if (err == WSAECONNRESET)
                        continue; // server port closed (ICMP unreachable); keep pinging
                    LogF("[StreamReceiver] recv failed: %s", WsaErrorString(err).c_str());
                    break;
                }

                // Authenticate + decrypt first (pass-through when no PSK);
                // a datagram that fails the tag check updates NOTHING --
                // not even the liveness clock.
                const size_t plainLen =
                    m_crypto.open(buf, static_cast<size_t>(n), plain);
                if (plainLen == 0)
                    continue;

                m_bytesReceived.fetch_add(static_cast<uint64_t>(n),
                                          std::memory_order_relaxed);
                lastPacketUs = nowUs;
                if (m_disconnected) {
                    m_disconnected = false;
                    LogF("[StreamReceiver] traffic resumed -- reconnected");
                }

                PacketHeader header;
                if (!deserialize(plain, plainLen, header))
                    continue;
                handlePacket(header, plain + kPacketHeaderSize,
                             plainLen - kPacketHeaderSize, nowUs);
            }
        }

        evictStaleSlots(nowUs);
        flushJitterBuffer(nowUs); // releases frames whose gap-wait timed out

        if (nowUs - lastPacketUs > kDisconnectTimeoutUs) {
            if (!m_disconnected) {
                m_disconnected = true;
                LogF("[StreamReceiver] no traffic for 3 s -- disconnected");
                if (onDisconnect)
                    onDisconnect();
            }
            if (nowUs - lastPingUs >= kReconnectPingUs) {
                sendKeepalivePing(nowUs); // wake the server / re-register
                lastPingUs = nowUs;
            }
        } else if (nowUs - lastPingUs >= kKeepalivePingUs) {
            // Steady ping keeps NAT mappings open and the RTT estimate fresh.
            sendKeepalivePing(nowUs);
            lastPingUs = nowUs;
        }
    }
}

void StreamReceiver::handlePacket(const PacketHeader& header, const uint8_t* payload,
                                  size_t payloadBytes, uint64_t nowUs)
{
    switch (header.type) {
    case PacketType::Video:     handleVideo(header, payload, payloadBytes, nowUs); break;
    case PacketType::Audio:     handleAudio(header, payload, payloadBytes, nowUs); break;
    case PacketType::Keepalive: handleKeepalive(header, nowUs);                    break;
    case PacketType::Input:     break; // input flows client->server only
    }
}

bool StreamReceiver::checkSequenceWindow(uint32_t sequence, uint32_t& maxSeq,
                                         bool& maxSeqValid, bool countExpected)
{
    if (!maxSeqValid) {
        maxSeqValid = true;
        maxSeq      = sequence;
        if (countExpected)
            m_windowExpectedFrames.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    const int32_t delta = static_cast<int32_t>(sequence - maxSeq); // wrap-safe
    if (delta == 0)
        return true; // another fragment of the newest unit -- in order
    if (delta > 0) {
        maxSeq = sequence;
        // Every sequence we advance past was expected on the wire, whether
        // or not its packets ever arrive -- that is what loss is measured
        // against. Video only: audio counts here would make the receiver
        // report compare apples to oranges (expected = video+audio,
        // completed = video) and drive the bitrate controller to minimum.
        if (countExpected)
            m_windowExpectedFrames.fetch_add(static_cast<uint32_t>(delta),
                                             std::memory_order_relaxed);
        return true;
    }
    m_outOfOrderCount.fetch_add(1, std::memory_order_relaxed);
    return delta >= -kReorderWindow; // older than the window: discard
}

bool StreamReceiver::reassemble(std::unordered_map<uint32_t, ReassemblySlot>& slots,
                                const PacketHeader& header, const uint8_t* payload,
                                size_t payloadBytes, uint64_t nowUs,
                                ReassemblySlot& outSlot)
{
    // Validate the fragment's shape before touching any state. Every
    // non-last fragment must be exactly full-sized -- that is what makes
    // offset = fragmentId * kMaxPayloadSize reassembly valid.
    if (header.totalFrags == 0 || header.fragmentId >= header.totalFrags)
        return false;
    const bool isLast = header.fragmentId == header.totalFrags - 1;
    if (isLast != ((header.flags & PacketFlags::kLastFrag) != 0))
        return false;
    if (!isLast && payloadBytes != kMaxPayloadSize)
        return false;
    if (payloadBytes > kMaxPayloadSize)
        return false;

    ReassemblySlot& slot = slots[header.sequence];
    if (slot.totalFrags == 0) {
        slot.init(header.totalFrags, header.timestampUs, nowUs);
    } else if (slot.totalFrags != header.totalFrags ||
               slot.timestampUs != header.timestampUs) {
        // Same sequence, different unit shape: stale wrap-around collision
        // or corruption. Restart the slot with the new fragment's identity.
        slot.init(header.totalFrags, header.timestampUs, nowUs);
    }

    if (!slot.markReceived(header.fragmentId))
        return false; // duplicate fragment

    memcpy(slot.data.data() +
               static_cast<size_t>(header.fragmentId) * kMaxPayloadSize,
           payload, payloadBytes);
    slot.flags |= header.flags;
    if (isLast) {
        slot.sawLastFrag  = true;
        slot.lastFragSize = static_cast<uint32_t>(payloadBytes);
    }

    if (!slot.complete())
        return false;

    outSlot = std::move(slot);
    slots.erase(header.sequence);
    return true;
}

void StreamReceiver::handleVideo(const PacketHeader& header, const uint8_t* payload,
                                 size_t payloadBytes, uint64_t nowUs)
{
    if (!checkSequenceWindow(header.sequence, m_maxVideoSeq, m_videoSeqValid,
                             /*countExpected=*/true))
        return;

    ReassemblySlot unit;
    if (!reassemble(m_videoSlots, header, payload, payloadBytes, nowUs, unit))
        return;

    m_windowCompletedFrames.fetch_add(1, std::memory_order_relaxed);

    PendingFrame pending;
    pending.frame.nalUnits = std::move(unit.data);
    pending.frame.nalUnits.resize(unit.assembledSize());
    pending.frame.timestampUs = m_videoTimestamps.unwrap(unit.timestampUs);
    pending.frame.isKeyframe  = (unit.flags & PacketFlags::kKeyframe) != 0;
    pending.sequence          = header.sequence;
    pending.arrivalUs         = nowUs;
    insertIntoJitterBuffer(std::move(pending));
}

void StreamReceiver::handleAudio(const PacketHeader& header, const uint8_t* payload,
                                 size_t payloadBytes, uint64_t nowUs)
{
    if (!checkSequenceWindow(header.sequence, m_maxAudioSeq, m_audioSeqValid,
                             /*countExpected=*/false))
        return;

    ReassemblySlot unit;
    if (!reassemble(m_audioSlots, header, payload, payloadBytes, nowUs, unit))
        return;

    // Audio goes straight out: it is small, frequent, and the playback
    // layer has its own buffering; the video jitter buffer would only add
    // latency here.
    if (onAudioPacket) {
        std::vector<uint8_t> data = std::move(unit.data);
        data.resize(unit.assembledSize());
        onAudioPacket(std::move(data), m_audioTimestamps.unwrap(unit.timestampUs));
    }
}

void StreamReceiver::handleKeepalive(const PacketHeader& header, uint64_t nowUs)
{
    if (header.fragmentId == 0) {
        // Ping from the server: echo it back unchanged so the server can
        // compute RTT from its own clock.
        PacketHeader echo;
        echo.type        = PacketType::Keepalive;
        echo.sequence    = header.sequence;
        echo.fragmentId  = 1;
        echo.totalFrags  = 1;
        echo.timestampUs = header.timestampUs;
        echo.flags       = 0;
        uint8_t dgram[kPacketHeaderSize];
        serialize(echo, dgram);
        sendRaw(dgram, sizeof(dgram));
    } else if (header.fragmentId == KeepaliveKind::kPong) {
        // Echo of one of our pings.
        const uint32_t elapsed = static_cast<uint32_t>(nowUs) - header.timestampUs;
        if (elapsed < 10'000'000) // sanity: ignore >10s (stale/garbled)
            m_rttMs.store(static_cast<float>(elapsed) / 1000.0f,
                          std::memory_order_relaxed);
    }
    // kKeyframeRequest / kReceiverReport flow client -> server only; ignore.
}

void StreamReceiver::evictStaleSlots(uint64_t nowUs)
{
    for (auto it = m_videoSlots.begin(); it != m_videoSlots.end();) {
        if (nowUs - it->second.firstArrivalUs > kSlotTimeoutUs) {
            LogF("[StreamReceiver] evicting incomplete video frame seq=%u "
                 "(%u/%u fragments after 80 ms)",
                 it->first, static_cast<unsigned>(it->second.receivedCount),
                 static_cast<unsigned>(it->second.totalFrags));
            m_droppedFrames.fetch_add(1, std::memory_order_relaxed);
            it = m_videoSlots.erase(it);
            requestKeyframe(); // unconcealable loss; ask for a fresh IDR
        } else {
            ++it;
        }
    }
    for (auto it = m_audioSlots.begin(); it != m_audioSlots.end();) {
        if (nowUs - it->second.firstArrivalUs > kSlotTimeoutUs) {
            LogF("[StreamReceiver] evicting incomplete audio packet seq=%u", it->first);
            it = m_audioSlots.erase(it);
        } else {
            ++it;
        }
    }
}

// ---------------------------------------------------------------------------
// Jitter buffer
// ---------------------------------------------------------------------------

void StreamReceiver::insertIntoJitterBuffer(PendingFrame&& pending)
{
    // Never go backwards: anything at or before the delivery point is dead.
    if (m_hasDelivered &&
        (static_cast<int32_t>(pending.sequence - m_lastDeliveredSeq) <= 0 ||
         pending.frame.timestampUs <= m_lastDeliveredTs)) {
        m_droppedFrames.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    if (m_jitterCount == kJitterSlots) {
        // Full. A frame older than everything buffered loses outright;
        // otherwise force the oldest out to make room (accepting the gap).
        if (pending.frame.timestampUs < m_jitter[0].frame.timestampUs) {
            m_droppedFrames.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        PendingFrame oldest = std::move(m_jitter[0]);
        for (size_t i = 1; i < m_jitterCount; ++i)
            m_jitter[i - 1] = std::move(m_jitter[i]);
        --m_jitterCount;
        deliverFrame(std::move(oldest));
    }

    // Insertion sort by timestamp (sequence as tiebreaker); 4 slots, so the
    // shifting is trivial.
    size_t pos = m_jitterCount;
    while (pos > 0 &&
           (m_jitter[pos - 1].frame.timestampUs > pending.frame.timestampUs ||
            (m_jitter[pos - 1].frame.timestampUs == pending.frame.timestampUs &&
             static_cast<int32_t>(m_jitter[pos - 1].sequence - pending.sequence) > 0))) {
        m_jitter[pos] = std::move(m_jitter[pos - 1]);
        --pos;
    }
    m_jitter[pos] = std::move(pending);
    ++m_jitterCount;

    flushJitterBuffer(SteadyMicros());
}

void StreamReceiver::flushJitterBuffer(uint64_t nowUs)
{
    while (m_jitterCount > 0) {
        const PendingFrame& head = m_jitter[0];
        const bool contiguous =
            !m_hasDelivered || head.sequence == m_lastDeliveredSeq + 1;
        const bool agedOut = nowUs - head.arrivalUs > kJitterHoldUs;
        if (!contiguous && !agedOut)
            break; // hold for the gap to fill (or for the timeout)

        PendingFrame frame = std::move(m_jitter[0]);
        for (size_t i = 1; i < m_jitterCount; ++i)
            m_jitter[i - 1] = std::move(m_jitter[i]);
        --m_jitterCount;
        // A forced delivery above may have moved the delivery point past
        // this frame (only possible with duplicate timestamps); honor the
        // never-backwards contract over delivering it.
        if (m_hasDelivered &&
            static_cast<int32_t>(frame.sequence - m_lastDeliveredSeq) <= 0) {
            m_droppedFrames.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        if (!contiguous) {
            LogF("[StreamReceiver] gap at seq=%u never filled; resuming at seq=%u",
                 m_lastDeliveredSeq + 1, frame.sequence);
            requestKeyframe(); // reference chain is broken until an IDR
        }
        deliverFrame(std::move(frame));
    }
}

void StreamReceiver::deliverFrame(PendingFrame&& pending)
{
    m_lastDeliveredSeq = pending.sequence;
    m_lastDeliveredTs  = pending.frame.timestampUs;
    m_hasDelivered     = true;
    if (onVideoFrame)
        onVideoFrame(std::move(pending.frame));
}

// ---------------------------------------------------------------------------
// Outbound
// ---------------------------------------------------------------------------

void StreamReceiver::sendKeepalivePing(uint64_t nowUs)
{
    PacketHeader header;
    header.type        = PacketType::Keepalive;
    header.sequence    = m_keepaliveSeq.fetch_add(1, std::memory_order_relaxed);
    header.fragmentId  = 0; // ping; server echoes with fragmentId = 1
    header.totalFrags  = 1;
    header.timestampUs = static_cast<uint32_t>(nowUs);
    header.flags       = 0;
    uint8_t dgram[kPacketHeaderSize];
    serialize(header, dgram);
    sendRaw(dgram, sizeof(dgram));
}

bool StreamReceiver::sendRaw(const uint8_t* data, int bytes)
{
    // Seal (encrypt + authenticate) when a PSK is configured; pass-through
    // copy otherwise. Sealing adds kCryptoOverhead, which kMaxPayloadSize
    // already reserves, so the result always fits one datagram.
    uint8_t sealed[kMaxDatagramSize + kCryptoOverhead];
    const size_t sealedLen = m_crypto.seal(data, static_cast<size_t>(bytes), sealed);
    if (sealedLen == 0) {
        LogF("[StreamReceiver] seal failed: %s", m_crypto.lastError().c_str());
        return false;
    }
    data  = sealed;
    bytes = static_cast<int>(sealedLen);

    for (int attempt = 0; attempt < 5; ++attempt) {
        const int sent = send(m_socket, reinterpret_cast<const char*>(data), bytes, 0);
        if (sent != SOCKET_ERROR)
            return true;
        const int err = WSAGetLastError();
        if (err == WSAEWOULDBLOCK) {
            WSAPOLLFD pfd = {};
            pfd.fd     = m_socket;
            pfd.events = POLLWRNORM;
            WSAPoll(&pfd, 1, 10);
            continue;
        }
        if (err == WSAECONNRESET)
            return false; // server gone; the liveness logic handles it
        LogF("[StreamReceiver] send failed: %s", WsaErrorString(err).c_str());
        setError("send failed: " + WsaErrorString(err));
        return false;
    }
    LogF("[StreamReceiver] send stalled (kernel buffer full for >40ms); dropping");
    return false;
}

// ---------------------------------------------------------------------------
// Stats thread
// ---------------------------------------------------------------------------

void StreamReceiver::statsThreadProc()
{
    SetThreadDescription(GetCurrentThread(), L"GSMR-RecvStats");

    uint64_t prevBytes  = 0;
    uint64_t lastFoldUs = SteadyMicros();

    while (!m_stopRequested.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        const uint64_t nowUs = SteadyMicros();
        if (nowUs - lastFoldUs < 1'000'000)
            continue;

        const double elapsedSec = static_cast<double>(nowUs - lastFoldUs) / 1e6;
        lastFoldUs = nowUs;

        const uint64_t bytes = m_bytesReceived.load(std::memory_order_relaxed);
        const double   kbps  = static_cast<double>(bytes - prevBytes) * 8.0 / 1000.0 / elapsedSec;
        prevBytes = bytes;

        // Loss over the last window: frames whose sequence numbers passed by
        // versus frames that fully reassembled. (Sequence numbers advance
        // per frame, so this is frame-granularity loss -- the finest the
        // protocol can observe.)
        const uint32_t expected  = m_windowExpectedFrames.exchange(0, std::memory_order_relaxed);
        const uint32_t completed = m_windowCompletedFrames.exchange(0, std::memory_order_relaxed);
        float lossPct = 0.0f;
        if (expected > 0 && completed < expected)
            lossPct = static_cast<float>(expected - completed) * 100.0f /
                      static_cast<float>(expected);

        // Receiver report: feeds the server's adaptive bitrate controller.
        // Sent even when expected == 0 (idle desktop) -- "no loss observed"
        // is exactly what lets the server ramp the bitrate back up.
        uint8_t report[kReceiverReportSize];
        writeU32LE(report, expected);
        writeU32LE(report + 4, completed);
        sendControl(KeepaliveKind::kReceiverReport, report, sizeof(report));

        std::lock_guard<std::mutex> lock(m_statsMutex);
        m_stats.rttMs           = m_rttMs.load(std::memory_order_relaxed);
        m_stats.packetLossPct   = lossPct;
        m_stats.bitrateKbps     = static_cast<float>(kbps);
        m_stats.droppedFrames   = m_droppedFrames.load(std::memory_order_relaxed);
        m_stats.outOfOrderCount = m_outOfOrderCount.load(std::memory_order_relaxed);
    }
}
