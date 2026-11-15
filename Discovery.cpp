// Discovery.cpp -- see Discovery.h for the design overview.

#include "Discovery.h"

#include <ws2tcpip.h>
#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>

#pragma comment(lib, "ws2_32.lib")

namespace {

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

constexpr uint8_t kDiscoveryVersion = 1;
constexpr char    kProbeMagic[4]    = {'G', 'S', 'D', 'S'};
constexpr char    kReplyMagic[4]    = {'G', 'S', 'D', 'R'};
constexpr size_t  kProbeSize        = 5;
constexpr size_t  kMaxNameLen       = 64;

// Crockford base32: no I, L, O, U -- unambiguous to read aloud or type.
constexpr char kBase32[] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";

int Base32Value(char c)
{
    if (c >= 'a' && c <= 'z')
        c = static_cast<char>(c - 'a' + 'A');
    // Common confusions map to their Crockford canonical digit.
    if (c == 'O') c = '0';
    if (c == 'I' || c == 'L') c = '1';
    for (int i = 0; i < 32; ++i)
        if (kBase32[i] == c)
            return i;
    return -1;
}

// CRC-8 (poly 0x07), tiny integrity check so a typo is rejected instead
// of silently connecting to a wrong address.
uint8_t Crc8(const uint8_t* data, size_t len)
{
    uint8_t crc = 0;
    for (size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (int b = 0; b < 8; ++b)
            crc = static_cast<uint8_t>((crc & 0x80) ? (crc << 1) ^ 0x07 : crc << 1);
    }
    return crc;
}

std::string HostNameUtf8()
{
    wchar_t wide[MAX_COMPUTERNAME_LENGTH + 1] = {};
    DWORD len = MAX_COMPUTERNAME_LENGTH + 1;
    if (!GetComputerNameW(wide, &len))
        return "MidnightSun";
    // Geniş ara tampon: dar tampona sığmayan ad dönüşümü tamamen
    // başarısız olur (bu yüzden önce bol yere çevir, sonra kısalt).
    char utf8[4 * (MAX_COMPUTERNAME_LENGTH + 1)] = {};
    WideCharToMultiByte(CP_UTF8, 0, wide, -1, utf8, sizeof(utf8) - 1, nullptr, nullptr);
    std::string name = utf8[0] ? utf8 : "MidnightSun";
    // kMaxNameLen'e UTF-8 sınırında kısalt: çok baytlı bir karakterin
    // ortasından kesmek istemcide bozuk metin gösterir.
    if (name.size() > kMaxNameLen) {
        size_t cut = kMaxNameLen;
        while (cut > 0 && (static_cast<uint8_t>(name[cut]) & 0xC0) == 0x80)
            --cut; // devam baytlarının başına çekil
        name.resize(cut);
    }
    return name;
}

} // namespace

// ---------------------------------------------------------------------------
// DiscoveryResponder (server)
// ---------------------------------------------------------------------------

DiscoveryResponder::~DiscoveryResponder()
{
    stop();
}

bool DiscoveryResponder::start(uint16_t streamPort, bool pskRequired)
{
    if (m_thread.joinable())
        return true;

    WSADATA wsaData = {};
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        m_lastError = "WSAStartup failed";
        return false;
    }
    m_wsaInitialized = true;

    m_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (m_socket == INVALID_SOCKET) {
        m_lastError = "socket() failed";
        stop();
        return false;
    }

    // REUSEADDR: a second server instance on the same box should not kill
    // discovery for the first; last binder wins the reply, which is fine.
    const int one = 1;
    setsockopt(m_socket, SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<const char*>(&one), sizeof(one));

    sockaddr_in local = {};
    local.sin_family      = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    local.sin_port        = htons(kDiscoveryPort);
    if (bind(m_socket, reinterpret_cast<const sockaddr*>(&local), sizeof(local)) ==
        SOCKET_ERROR) {
        char msg[64];
        sprintf_s(msg, "bind(udp %u) failed: %d", kDiscoveryPort, WSAGetLastError());
        m_lastError = msg;
        stop();
        return false;
    }

    // Blocking socket + 250 ms receive timeout: the thread stays simple
    // (no poll loop) and still notices stop() promptly.
    const DWORD timeoutMs = 250;
    setsockopt(m_socket, SOL_SOCKET, SO_RCVTIMEO,
               reinterpret_cast<const char*>(&timeoutMs), sizeof(timeoutMs));

    m_streamPort  = streamPort;
    m_pskRequired = pskRequired;
    m_hostname    = HostNameUtf8();
    if (m_hostname.size() > kMaxNameLen)
        m_hostname.resize(kMaxNameLen);

    m_stopRequested.store(false, std::memory_order_release);
    m_thread = std::thread(&DiscoveryResponder::threadProc, this);
    LogF("[Discovery] responder up on udp %u (host '%s', stream port %u)",
         kDiscoveryPort, m_hostname.c_str(), streamPort);
    return true;
}

void DiscoveryResponder::stop()
{
    m_stopRequested.store(true, std::memory_order_release);
    if (m_thread.joinable())
        m_thread.join();
    if (m_socket != INVALID_SOCKET) {
        closesocket(m_socket);
        m_socket = INVALID_SOCKET;
    }
    if (m_wsaInitialized) {
        WSACleanup();
        m_wsaInitialized = false;
    }
}

void DiscoveryResponder::threadProc()
{
    SetThreadDescription(GetCurrentThread(), L"GSMR-Discovery");

    // Reply datagram is constant for the process lifetime; build it once.
    uint8_t reply[9 + kMaxNameLen];
    memcpy(reply, kReplyMagic, 4);
    reply[4] = kDiscoveryVersion;
    reply[5] = static_cast<uint8_t>(m_streamPort);
    reply[6] = static_cast<uint8_t>(m_streamPort >> 8);
    reply[7] = m_pskRequired ? 1 : 0;
    reply[8] = static_cast<uint8_t>(m_hostname.size());
    memcpy(reply + 9, m_hostname.data(), m_hostname.size());
    const int replyLen = static_cast<int>(9 + m_hostname.size());

    while (!m_stopRequested.load(std::memory_order_acquire)) {
        uint8_t buf[64];
        sockaddr_in from = {};
        int fromLen = sizeof(from);
        const int n = recvfrom(m_socket, reinterpret_cast<char*>(buf), sizeof(buf), 0,
                               reinterpret_cast<sockaddr*>(&from), &fromLen);
        if (n == SOCKET_ERROR) {
            const int err = WSAGetLastError();
            if (err == WSAETIMEDOUT || err == WSAECONNRESET)
                continue;
            LogF("[Discovery] recvfrom failed: %d -- responder exiting", err);
            return;
        }
        if (n < static_cast<int>(kProbeSize) || memcmp(buf, kProbeMagic, 4) != 0 ||
            buf[4] != kDiscoveryVersion)
            continue; // stray traffic; ignore

        sendto(m_socket, reinterpret_cast<const char*>(reply), replyLen, 0,
               reinterpret_cast<const sockaddr*>(&from), fromLen);
    }
}

// ---------------------------------------------------------------------------
// DiscoverServers (client)
// ---------------------------------------------------------------------------

std::vector<DiscoveredServer> DiscoverServers(uint32_t timeoutMs)
{
    std::vector<DiscoveredServer> found;

    WSADATA wsaData = {};
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0)
        return found;

    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) {
        WSACleanup();
        return found;
    }

    const int one = 1;
    setsockopt(s, SOL_SOCKET, SO_BROADCAST,
               reinterpret_cast<const char*>(&one), sizeof(one));

    uint8_t probe[kProbeSize];
    memcpy(probe, kProbeMagic, 4);
    probe[4] = kDiscoveryVersion;

    sockaddr_in bcast = {};
    bcast.sin_family      = AF_INET;
    bcast.sin_addr.s_addr = htonl(INADDR_BROADCAST);
    bcast.sin_port        = htons(kDiscoveryPort);
    sendto(s, reinterpret_cast<const char*>(probe), sizeof(probe), 0,
           reinterpret_cast<const sockaddr*>(&bcast), sizeof(bcast));
    // Loopback too: a server on THIS machine does not hear the broadcast
    // on all configurations.
    sockaddr_in self = bcast;
    inet_pton(AF_INET, "127.0.0.1", &self.sin_addr);
    sendto(s, reinterpret_cast<const char*>(probe), sizeof(probe), 0,
           reinterpret_cast<const sockaddr*>(&self), sizeof(self));

    const ULONGLONG deadline = GetTickCount64() + timeoutMs;
    for (;;) {
        const ULONGLONG now = GetTickCount64();
        if (now >= deadline)
            break;
        DWORD waitMs = static_cast<DWORD>(deadline - now);
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO,
                   reinterpret_cast<const char*>(&waitMs), sizeof(waitMs));

        uint8_t buf[9 + kMaxNameLen + 16];
        sockaddr_in from = {};
        int fromLen = sizeof(from);
        const int n = recvfrom(s, reinterpret_cast<char*>(buf), sizeof(buf), 0,
                               reinterpret_cast<sockaddr*>(&from), &fromLen);
        if (n == SOCKET_ERROR) {
            const int err = WSAGetLastError();
            if (err == WSAECONNRESET)
                continue; // ICMP unreachable from the loopback probe; harmless
            break;        // timeout (or real error): done collecting
        }
        if (n < 9 || memcmp(buf, kReplyMagic, 4) != 0 || buf[4] != kDiscoveryVersion)
            continue;

        DiscoveredServer server;
        server.port        = static_cast<uint16_t>(buf[5] | (buf[6] << 8));
        server.pskRequired = buf[7] != 0;
        const size_t nameLen = buf[8];
        if (nameLen <= kMaxNameLen && 9 + nameLen <= static_cast<size_t>(n))
            server.hostname.assign(reinterpret_cast<const char*>(buf + 9), nameLen);
        char ip[INET_ADDRSTRLEN] = {};
        inet_ntop(AF_INET, &from.sin_addr, ip, sizeof(ip));
        server.ip = ip;

        bool duplicate = false;
        for (const DiscoveredServer& known : found)
            if (known.ip == server.ip && known.port == server.port)
                duplicate = true;
        if (!duplicate)
            found.push_back(std::move(server));
    }

    closesocket(s);
    WSACleanup();
    return found;
}

// ---------------------------------------------------------------------------
// Pairing code
// ---------------------------------------------------------------------------

std::string EncodePairingCode(const std::string& dottedIp, uint16_t port)
{
    in_addr addr = {};
    if (inet_pton(AF_INET, dottedIp.c_str(), &addr) != 1)
        return {};

    // Payload: ip(4, network order) + port(2, LE) + crc(1) = 7 bytes.
    uint8_t payload[7];
    memcpy(payload, &addr.s_addr, 4);
    payload[4] = static_cast<uint8_t>(port);
    payload[5] = static_cast<uint8_t>(port >> 8);
    payload[6] = Crc8(payload, 6);

    // 7 bytes = 56 bits -> 12 base32 chars (60 bits, top 4 padded zero).
    std::string code;
    uint32_t acc  = 0;
    int      bits = 0;
    for (uint8_t byte : payload) {
        acc = (acc << 8) | byte;
        bits += 8;
        while (bits >= 5) {
            code.push_back(kBase32[(acc >> (bits - 5)) & 31]);
            bits -= 5;
        }
    }
    if (bits > 0)
        code.push_back(kBase32[(acc << (5 - bits)) & 31]);

    // Group as XXXX-XXXX-XXXX for readability.
    std::string grouped;
    for (size_t i = 0; i < code.size(); ++i) {
        if (i > 0 && i % 4 == 0)
            grouped.push_back('-');
        grouped.push_back(code[i]);
    }
    return grouped;
}

bool DecodePairingCode(const std::string& code, std::string& outIp, uint16_t& outPort)
{
    // Strip separators/whitespace, canonicalize, base32-decode.
    std::string clean;
    for (char c : code) {
        if (c == '-' || c == ' ' || c == '\t')
            continue;
        clean.push_back(c);
    }
    if (clean.size() != 12)
        return false;

    uint64_t acc  = 0;
    int      bits = 0;
    uint8_t  payload[8];
    size_t   out = 0;
    for (char c : clean) {
        const int v = Base32Value(c);
        if (v < 0)
            return false;
        acc = (acc << 5) | static_cast<uint64_t>(v);
        bits += 5;
        if (bits >= 8) {
            if (out >= sizeof(payload))
                return false;
            payload[out++] = static_cast<uint8_t>(acc >> (bits - 8));
            bits -= 8;
        }
    }
    if (out != 7)
        return false;
    if (Crc8(payload, 6) != payload[6])
        return false; // typo

    in_addr addr = {};
    memcpy(&addr.s_addr, payload, 4);
    char ip[INET_ADDRSTRLEN] = {};
    inet_ntop(AF_INET, &addr, ip, sizeof(ip));
    outIp   = ip;
    outPort = static_cast<uint16_t>(payload[4] | (payload[5] << 8));
    return outPort != 0;
}

// ---------------------------------------------------------------------------
// GetLocalIpv4
// ---------------------------------------------------------------------------

std::string GetLocalIpv4()
{
    WSADATA wsaData = {};
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0)
        return {};

    std::string result;
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s != INVALID_SOCKET) {
        // Connecting a UDP socket sends nothing; it just makes the stack
        // pick the outgoing interface for that destination.
        sockaddr_in target = {};
        target.sin_family = AF_INET;
        target.sin_port   = htons(53);
        inet_pton(AF_INET, "8.8.8.8", &target.sin_addr);
        if (connect(s, reinterpret_cast<const sockaddr*>(&target), sizeof(target)) == 0) {
            sockaddr_in local = {};
            int len = sizeof(local);
            if (getsockname(s, reinterpret_cast<sockaddr*>(&local), &len) == 0) {
                char ip[INET_ADDRSTRLEN] = {};
                if (inet_ntop(AF_INET, &local.sin_addr, ip, sizeof(ip)))
                    result = ip;
            }
        }
        closesocket(s);
    }
    WSACleanup();
    return result;
}
