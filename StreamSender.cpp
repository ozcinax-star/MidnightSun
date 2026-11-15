// StreamSender.cpp -- see StreamSender.h for the design overview.

#include "StreamSender.h"

#include <windows.h> // SetThreadDescription; winsock2.h already in our header

#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstring>

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

std::string AddrToString(const sockaddr_in& addr)
{
    char ip[INET_ADDRSTRLEN] = "?";
    inet_ntop(AF_INET, &addr.sin_addr, ip, sizeof(ip));
    char buf[80];
    sprintf_s(buf, "%s:%u", ip, static_cast<unsigned>(ntohs(addr.sin_port)));
    return buf;
}

// Non-blocking mode is required (the loops use WSAPoll); the buffer sizes
// and the DSCP EF marking are best-effort -- IP_TOS in particular is
// ignored by Windows unless the qWAVE policy allows it, so a failure there
// is logged and tolerated.
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
// OutboundQueue
// ---------------------------------------------------------------------------

StreamSender::OutboundQueue::~OutboundQueue()
{
    // Only reached after both threads are joined, so plain draining is safe.
    while (OutboundItem* item = dequeue())
        delete item;
}

bool StreamSender::OutboundQueue::enqueue(OutboundItem* item,
                                          std::atomic<uint32_t>& droppedFrames)
{
    // Bounded patience for the pathological case where the ring is full and
    // the oldest entry is a keyframe we refuse to drop: the consumer drains
    // a slot in microseconds when healthy, so this only expires if the send
    // thread is wedged (dead socket), at which point dropping is honest.
    int keyframeWaitBudget = 4000;

    for (;;) {
        const uint64_t t = m_tail.load(std::memory_order_relaxed);
        const uint64_t h = m_head.load(std::memory_order_acquire);

        if (t - h < kCapacity) {
            const size_t slot = static_cast<size_t>(t % kCapacity);
            m_slotKeyframe[slot].store(item->keyframe, std::memory_order_relaxed);
            m_slots[slot].store(item, std::memory_order_release);
            m_tail.store(t + 1, std::memory_order_release);
            return true;
        }

        // Full. Decide who to drop. The mirrored flag (not the pointer,
        // which the consumer may be freeing right now) tells us whether the
        // oldest entry is a keyframe.
        const size_t headSlot = static_cast<size_t>(h % kCapacity);
        if (m_slotKeyframe[headSlot].load(std::memory_order_relaxed)) {
            if (!item->keyframe) {
                if (item->type == PacketType::Video)
                    droppedFrames.fetch_add(1, std::memory_order_relaxed);
                delete item;
                return false;
            }
            if (--keyframeWaitBudget > 0) {
                std::this_thread::yield();
                continue;
            }
            LogF("[StreamSender] queue wedged full of keyframes; dropping incoming keyframe");
            droppedFrames.fetch_add(1, std::memory_order_relaxed);
            delete item;
            return false;
        }

        // Steal the oldest entry: whoever's exchange returns non-null owns
        // it; the head CAS that loses (because the consumer advanced first)
        // fails benignly.
        OutboundItem* victim = m_slots[headSlot].exchange(nullptr, std::memory_order_acq_rel);
        uint64_t expected = h;
        m_head.compare_exchange_strong(expected, h + 1, std::memory_order_acq_rel,
                                       std::memory_order_relaxed);
        if (victim) {
            if (victim->type == PacketType::Video)
                droppedFrames.fetch_add(1, std::memory_order_relaxed);
            delete victim;
        }
        // Loop: there is room (or the consumer made room) now.
    }
}

StreamSender::OutboundItem* StreamSender::OutboundQueue::dequeue()
{
    for (;;) {
        const uint64_t h = m_head.load(std::memory_order_relaxed);
        if (h == m_tail.load(std::memory_order_acquire))
            return nullptr;
        OutboundItem* item =
            m_slots[static_cast<size_t>(h % kCapacity)].exchange(nullptr, std::memory_order_acq_rel);
        uint64_t expected = h;
        m_head.compare_exchange_strong(expected, h + 1, std::memory_order_acq_rel,
                                       std::memory_order_relaxed);
        if (item)
            return item;
        // Slot was stolen by the producer's drop-oldest path; try the next.
    }
}

bool StreamSender::OutboundQueue::empty() const
{
    return m_head.load(std::memory_order_acquire) == m_tail.load(std::memory_order_acquire);
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

StreamSender::StreamSender(uint16_t listenPort)
    : m_listenPort(listenPort)
{
}

StreamSender::~StreamSender()
{
    stop();
}

bool StreamSender::start()
{
    if (m_running.load(std::memory_order_acquire))
        return true;

    WSADATA wsaData = {};
    int rc = WSAStartup(MAKEWORD(2, 2), &wsaData);
    if (rc != 0) {
        setError("WSAStartup failed: " + WsaErrorString(rc));
        LogF("[StreamSender] %s", lastError().c_str());
        return false;
    }
    m_wsaInitialized = true;

    m_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (m_socket == INVALID_SOCKET) {
        setError("socket() failed: " + WsaErrorString(WSAGetLastError()));
        LogF("[StreamSender] %s", lastError().c_str());
        stop();
        return false;
    }

    sockaddr_in local = {};
    local.sin_family      = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    local.sin_port        = htons(m_listenPort);
    if (bind(m_socket, reinterpret_cast<const sockaddr*>(&local), sizeof(local)) == SOCKET_ERROR) {
        setError("bind(port " + std::to_string(m_listenPort) +
                 ") failed: " + WsaErrorString(WSAGetLastError()));
        LogF("[StreamSender] %s", lastError().c_str());
        stop();
        return false;
    }

    if (!ConfigureSocket(m_socket, "StreamSender")) {
        setError("socket configuration failed");
        stop();
        return false;
    }

    m_stopRequested.store(false, std::memory_order_release);
    m_running.store(true, std::memory_order_release);
    m_sendThread  = std::thread(&StreamSender::sendThreadProc, this);
    m_statsThread = std::thread(&StreamSender::statsThreadProc, this);

    LogF("[StreamSender] listening on UDP port %u", static_cast<unsigned>(m_listenPort));
    return true;
}

void StreamSender::stop()
{
    m_stopRequested.store(true, std::memory_order_release);
    if (m_sendThread.joinable())
        m_sendThread.join();
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
    {
        std::lock_guard<std::mutex> lock(m_clientMutex);
        m_clientKnown = false;
    }
}

// ---------------------------------------------------------------------------
// Producer API
// ---------------------------------------------------------------------------

void StreamSender::sendFrame(EncodedFrame frame)
{
    if (!m_running.load(std::memory_order_acquire) ||
        m_stopRequested.load(std::memory_order_acquire)) {
        m_droppedFrames.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    auto* item        = new OutboundItem();
    item->type        = PacketType::Video;
    item->payload     = std::move(frame.nalUnits); // no per-frame copy
    item->timestampUs = frame.timestampUs;
    item->keyframe    = frame.isKeyframe;
    m_queue.enqueue(item, m_droppedFrames);
}

void StreamSender::sendAudio(const uint8_t* pcm, size_t bytes, uint64_t timestampUs)
{
    if (!pcm || bytes == 0)
        return;
    if (!m_running.load(std::memory_order_acquire) ||
        m_stopRequested.load(std::memory_order_acquire))
        return;
    auto* item        = new OutboundItem();
    item->type        = PacketType::Audio;
    item->payload.assign(pcm, pcm + bytes);
    item->timestampUs = timestampUs;
    item->keyframe    = false;
    m_queue.enqueue(item, m_droppedFrames);
}

void StreamSender::sendKeepalive()
{
    if (!m_running.load(std::memory_order_acquire))
        return;
    sendKeepalivePing(SteadyMicros());
}

bool StreamSender::setPsk(const std::string& psk)
{
    if (m_running.load(std::memory_order_acquire)) {
        setError("setPsk must be called before start()");
        return false;
    }
    if (!m_crypto.init(psk, /*isServer=*/true)) {
        setError("crypto init failed: " + m_crypto.lastError());
        LogF("[StreamSender] %s", lastError().c_str());
        return false;
    }
    if (m_crypto.enabled())
        LogF("[StreamSender] transport encryption enabled (AES-256-GCM); "
             "unauthenticated datagrams are ignored");
    return true;
}

StreamStats StreamSender::getStats() const
{
    std::lock_guard<std::mutex> lock(m_statsMutex);
    return m_stats;
}

std::string StreamSender::clientAddress() const
{
    std::lock_guard<std::mutex> lock(m_clientMutex);
    if (!m_clientKnown)
        return {};
    return AddrToString(m_clientAddr);
}

void StreamSender::resolvePending(bool allow)
{
    std::string addr;
    {
        std::lock_guard<std::mutex> lock(m_clientMutex);
        if (!m_pendingKnown)
            return;
        addr = AddrToString(m_pendingAddr);
        if (allow) {
            m_approvedIps.push_back(m_pendingAddr.sin_addr.s_addr);
            m_clientAddr  = m_pendingAddr;
            m_clientKnown = true;
        } else {
            m_deniedIps.push_back(m_pendingAddr.sin_addr.s_addr);
        }
        m_pendingKnown = false;
    }
    LogF("[StreamSender] pending client %s %s", addr.c_str(),
         allow ? "approved" : "rejected");
}

std::string StreamSender::lastError() const
{
    std::lock_guard<std::mutex> lock(m_errorMutex);
    return m_lastError;
}

void StreamSender::setError(std::string message)
{
    std::lock_guard<std::mutex> lock(m_errorMutex);
    m_lastError = std::move(message);
}

// ---------------------------------------------------------------------------
// Send thread
// ---------------------------------------------------------------------------

void StreamSender::sendThreadProc()
{
    SetThreadDescription(GetCurrentThread(), L"GSMR-Send");

    uint64_t lastTrafficUs = SteadyMicros();

    for (;;) {
        const bool stopping = m_stopRequested.load(std::memory_order_acquire);

        WSAPOLLFD pfd = {};
        pfd.fd     = m_socket;
        pfd.events = POLLRDNORM;
        // While stopping we only flush the queue; don't add poll latency.
        const int pollRc = WSAPoll(&pfd, 1, stopping ? 0 : 10);

        const uint64_t nowUs = SteadyMicros();
        if (pollRc == SOCKET_ERROR) {
            LogF("[StreamSender] WSAPoll failed: %s -- send thread exiting",
                 WsaErrorString(WSAGetLastError()).c_str());
            return;
        }
        if (pollRc > 0 && (pfd.revents & (POLLRDNORM | POLLERR)))
            drainInbound(nowUs);

        bool clientKnown;
        {
            std::lock_guard<std::mutex> lock(m_clientMutex);
            clientKnown = m_clientKnown;
        }

        while (OutboundItem* item = m_queue.dequeue()) {
            if (!clientKnown) {
                if (!m_warnedNoClient) {
                    LogF("[StreamSender] dropping frames: no client connected yet");
                    m_warnedNoClient = true;
                }
                if (item->type == PacketType::Video)
                    m_droppedFrames.fetch_add(1, std::memory_order_relaxed);
                delete item;
                continue;
            }
            m_warnedNoClient = false;

            uint32_t& seqCounter =
                (item->type == PacketType::Video) ? m_videoSeq : m_audioSeq;
            const uint32_t sequence = seqCounter++;
            transmit(item->type, sequence, item->payload.data(), item->payload.size(),
                     static_cast<uint32_t>(item->timestampUs), item->keyframe);
            delete item;
            lastTrafficUs = SteadyMicros();
        }

        if (stopping) {
            if (m_queue.empty())
                return;
            continue; // a producer published between the drain and the check
        }

        if (clientKnown && nowUs - lastTrafficUs >= 500'000) {
            sendKeepalivePing(nowUs);
            lastTrafficUs = nowUs;
        }
    }
}

void StreamSender::drainInbound(uint64_t nowUs)
{
    for (;;) {
        uint8_t buf[kMaxDatagramSize + 64];
        uint8_t plain[kMaxDatagramSize + 64];
        sockaddr_in from = {};
        int fromLen = sizeof(from);
        const int n = recvfrom(m_socket, reinterpret_cast<char*>(buf), sizeof(buf), 0,
                               reinterpret_cast<sockaddr*>(&from), &fromLen);
        if (n == SOCKET_ERROR) {
            const int err = WSAGetLastError();
            if (err == WSAEWOULDBLOCK)
                return;
            if (err == WSAECONNRESET)
                continue; // ICMP port-unreachable from an earlier sendto; harmless
            LogF("[StreamSender] recvfrom failed: %s", WsaErrorString(err).c_str());
            return;
        }

        // Authenticate + decrypt BEFORE anything else -- especially before
        // client discovery. With a PSK set, a datagram that fails the tag
        // check cannot become the streaming target.
        const size_t plainLen = m_crypto.open(buf, static_cast<size_t>(n), plain);
        if (plainLen == 0)
            continue; // not ours / wrong key; ignore silently

        PacketHeader header;
        if (!deserialize(plain, plainLen, header))
            continue; // not ours; ignore silently (port scans, stray traffic)

        handleInbound(header, plain + kPacketHeaderSize,
                      plainLen - kPacketHeaderSize, from, nowUs);
    }
}

void StreamSender::handleInbound(const PacketHeader& header, const uint8_t* payload,
                                 size_t payloadBytes, const sockaddr_in& from,
                                 uint64_t nowUs)
{
    // Client discovery: any valid GSMR datagram from a new (ip, port) pair
    // becomes the streaming target -- unless approval is required, in which
    // case the endpoint waits in "pending" (its datagrams are ignored, ping
    // echoes included) until resolvePending() decides.
    std::string announcePending;
    {
        std::lock_guard<std::mutex> lock(m_clientMutex);
        const bool sameAsClient =
            m_clientKnown &&
            from.sin_addr.s_addr == m_clientAddr.sin_addr.s_addr &&
            from.sin_port == m_clientAddr.sin_port;
        if (!sameAsClient) {
            const uint32_t ip = from.sin_addr.s_addr;
            auto contains = [](const std::vector<uint32_t>& list, uint32_t value) {
                for (uint32_t entry : list)
                    if (entry == value)
                        return true;
                return false;
            };
            if (contains(m_deniedIps, ip))
                return; // bu oturumda reddedildi; tamamen yok say
            if (!m_requireApproval || contains(m_approvedIps, ip)) {
                m_clientAddr  = from;
                m_clientKnown = true;
                LogF("[StreamSender] client connected: %s",
                     AddrToString(from).c_str());
            } else {
                if (!m_pendingKnown) {
                    m_pendingAddr  = from;
                    m_pendingKnown = true;
                    announcePending = AddrToString(from);
                }
                // Beklemede (ya da başka biri beklemedeyken gelen ikinci
                // aday): onay çıkana dek hiçbir paketi işleme. Bildirim
                // kilidin DIŞINDA yapılır (callback clientAddress() gibi
                // aynı kilidi alan çağrılar yapabilir).
                if (announcePending.empty())
                    return;
            }
        }
    }
    if (!announcePending.empty()) {
        LogF("[StreamSender] client awaiting approval: %s", announcePending.c_str());
        if (onClientRequest)
            onClientRequest(announcePending);
        return;
    }

    switch (header.type) {
    case PacketType::Keepalive: {
        if (header.fragmentId == KeepaliveKind::kPing) {
            // Ping from the client: echo it back unchanged so the client
            // can compute RTT from its own clock.
            PacketHeader echo;
            echo.type        = PacketType::Keepalive;
            echo.sequence    = header.sequence;
            echo.fragmentId  = KeepaliveKind::kPong;
            echo.totalFrags  = 1;
            echo.timestampUs = header.timestampUs;
            echo.flags       = 0;
            uint8_t dgram[kPacketHeaderSize];
            serialize(echo, dgram);
            sendDatagramTo(dgram, sizeof(dgram), from);
        } else if (header.fragmentId == KeepaliveKind::kPong) {
            // Echo of one of our pings: timestamp is our clock, low 32 bits.
            const uint32_t elapsed = static_cast<uint32_t>(nowUs) - header.timestampUs;
            if (elapsed < 10'000'000) // sanity: ignore >10s (stale/garbled)
                m_rttMs.store(static_cast<float>(elapsed) / 1000.0f,
                              std::memory_order_relaxed);
        } else if (header.fragmentId == KeepaliveKind::kKeyframeRequest) {
            if (onKeyframeRequest)
                onKeyframeRequest();
        } else if (header.fragmentId == KeepaliveKind::kReceiverReport &&
                   payloadBytes >= kReceiverReportSize) {
            const uint32_t expected  = readU32LE(payload);
            const uint32_t completed = readU32LE(payload + 4);
            float lossPct = 0.0f;
            if (expected > 0 && completed < expected)
                lossPct = static_cast<float>(expected - completed) * 100.0f /
                          static_cast<float>(expected);
            m_reportedLossPct.store(lossPct, std::memory_order_relaxed);
            if (onReceiverReport)
                onReceiverReport(expected, completed);
        }
        break;
    }
    case PacketType::Input: {
        if (header.totalFrags != 1) {
            LogF("[StreamSender] dropping fragmented input packet (%u fragments) -- "
                 "input must fit one datagram",
                 static_cast<unsigned>(header.totalFrags));
            break;
        }
        if (m_inputSeqValid &&
            static_cast<int32_t>(header.sequence - m_lastInputSeq) <= 0)
            m_outOfOrderCount.fetch_add(1, std::memory_order_relaxed);
        else {
            m_lastInputSeq  = header.sequence;
            m_inputSeqValid = true;
        }
        if (onInputPacket)
            onInputPacket(std::vector<uint8_t>(payload, payload + payloadBytes),
                          header.timestampUs);
        break;
    }
    case PacketType::Video:
    case PacketType::Audio:
        break; // media flows server->client only; ignore
    }
}

bool StreamSender::transmit(PacketType type, uint32_t sequence, const uint8_t* payload,
                            size_t bytes, uint32_t timestampUs32, bool keyframe)
{
    const size_t totalFrags = bytes == 0 ? 1 : (bytes + kMaxPayloadSize - 1) / kMaxPayloadSize;
    if (totalFrags > 0xFFFF) {
        LogF("[StreamSender] unit of %zu bytes exceeds the 65535-fragment limit; dropped",
             bytes);
        if (type == PacketType::Video)
            m_droppedFrames.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    uint8_t dgram[kMaxDatagramSize];
    size_t offset = 0;
    for (size_t i = 0; i < totalFrags; ++i) {
        const size_t chunk = (bytes - offset < kMaxPayloadSize) ? bytes - offset
                                                                : kMaxPayloadSize;
        const bool last = (i == totalFrags - 1);

        PacketHeader header;
        header.type        = type;
        header.sequence    = sequence;
        header.fragmentId  = static_cast<uint16_t>(i);
        header.totalFrags  = static_cast<uint16_t>(totalFrags);
        header.timestampUs = timestampUs32;
        header.flags       = static_cast<uint8_t>((keyframe ? PacketFlags::kKeyframe : 0) |
                                                  (last ? PacketFlags::kLastFrag : 0));
        serialize(header, dgram);
        if (chunk > 0)
            memcpy(dgram + kPacketHeaderSize, payload + offset, chunk);

        if (!sendDatagram(dgram, static_cast<int>(kPacketHeaderSize + chunk))) {
            if (type == PacketType::Video)
                m_droppedFrames.fetch_add(1, std::memory_order_relaxed);
            return false; // remaining fragments are useless without this one
        }
        offset += chunk;
    }
    return true;
}

bool StreamSender::sendDatagram(const uint8_t* datagram, int bytes)
{
    sockaddr_in dest;
    {
        std::lock_guard<std::mutex> lock(m_clientMutex);
        if (!m_clientKnown)
            return false;
        dest = m_clientAddr;
    }
    return sendDatagramTo(datagram, bytes, dest);
}

bool StreamSender::sendDatagramTo(const uint8_t* datagram, int bytes,
                                  const sockaddr_in& to)
{
    const sockaddr_in dest = to;

    // Seal (encrypt + authenticate) when a PSK is configured; pass-through
    // copy otherwise. kMaxPayloadSize reserves the overhead, so the sealed
    // datagram always fits.
    uint8_t sealed[kMaxDatagramSize + kCryptoOverhead];
    const size_t sealedLen = m_crypto.seal(datagram, static_cast<size_t>(bytes), sealed);
    if (sealedLen == 0) {
        LogF("[StreamSender] seal failed: %s", m_crypto.lastError().c_str());
        return false;
    }
    datagram = sealed;
    bytes    = static_cast<int>(sealedLen);

    for (int attempt = 0; attempt < 5; ++attempt) {
        const int sent = sendto(m_socket, reinterpret_cast<const char*>(datagram), bytes, 0,
                                reinterpret_cast<const sockaddr*>(&dest), sizeof(dest));
        if (sent != SOCKET_ERROR) {
            m_bytesSent.fetch_add(static_cast<uint64_t>(sent), std::memory_order_relaxed);
            return true;
        }
        const int err = WSAGetLastError();
        if (err == WSAEWOULDBLOCK) {
            // Kernel send buffer full (4 MB backlog -- severe congestion).
            // Wait briefly for drain rather than spinning.
            WSAPOLLFD pfd = {};
            pfd.fd     = m_socket;
            pfd.events = POLLWRNORM;
            WSAPoll(&pfd, 1, 10);
            continue;
        }
        LogF("[StreamSender] sendto failed: %s", WsaErrorString(err).c_str());
        setError("sendto failed: " + WsaErrorString(err));
        return false;
    }
    LogF("[StreamSender] sendto stalled (kernel buffer full for >40ms); dropping unit");
    return false;
}

void StreamSender::sendKeepalivePing(uint64_t nowUs)
{
    PacketHeader header;
    header.type        = PacketType::Keepalive;
    header.sequence    = m_keepaliveSeq.fetch_add(1, std::memory_order_relaxed);
    header.fragmentId  = 0; // ping; peer echoes with fragmentId = 1
    header.totalFrags  = 1;
    header.timestampUs = static_cast<uint32_t>(nowUs);
    header.flags       = 0;
    uint8_t dgram[kPacketHeaderSize];
    serialize(header, dgram);
    sendDatagram(dgram, sizeof(dgram));
}

// ---------------------------------------------------------------------------
// Stats thread
// ---------------------------------------------------------------------------

void StreamSender::statsThreadProc()
{
    SetThreadDescription(GetCurrentThread(), L"GSMR-SendStats");

    uint64_t prevBytes  = 0;
    uint64_t lastFoldUs = SteadyMicros();

    while (!m_stopRequested.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        const uint64_t nowUs = SteadyMicros();
        if (nowUs - lastFoldUs < 1'000'000)
            continue;

        const double elapsedSec = static_cast<double>(nowUs - lastFoldUs) / 1e6;
        lastFoldUs = nowUs;

        const uint64_t bytes = m_bytesSent.load(std::memory_order_relaxed);
        const double   kbps  = static_cast<double>(bytes - prevBytes) * 8.0 / 1000.0 / elapsedSec;
        prevBytes = bytes;

        std::lock_guard<std::mutex> lock(m_statsMutex);
        m_stats.rttMs           = m_rttMs.load(std::memory_order_relaxed);
        // Loss as observed by the client, from its 1 Hz receiver reports.
        m_stats.packetLossPct   = m_reportedLossPct.load(std::memory_order_relaxed);
        m_stats.bitrateKbps     = static_cast<float>(kbps);
        m_stats.droppedFrames   = m_droppedFrames.load(std::memory_order_relaxed);
        m_stats.outOfOrderCount = m_outOfOrderCount.load(std::memory_order_relaxed);
    }
}
