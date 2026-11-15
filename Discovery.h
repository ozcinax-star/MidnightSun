// Discovery.h
//
// IP-free pairing for the MidnightSun pipeline. Two mechanisms, both
// independent of the GSMR media transport:
//
//   1. LAN auto-discovery ("GSDS"/"GSDR" datagrams on UDP port
//      kDiscoveryPort): the client broadcasts a probe, every server
//      answers with its hostname, stream port and whether a PSK is
//      required. Same-subnet devices therefore pair with zero typing.
//
//   2. Pairing code: a short human-typable code (Crockford base32,
//      "XXXX-XXXX-XXXX") that encodes the server's IPv4 + stream port +
//      CRC-8. The server shows it in its status window; typing it on the
//      client replaces knowing the IP. The code carries an *address*, so
//      it works wherever that address is reachable (LAN, VPN, port
//      forward) -- it is not internet NAT traversal.
//
// Wire format (all little-endian):
//   probe   : "GSDS" u8 version(=1)                        [5 bytes]
//   response: "GSDR" u8 version(=1) u16 streamPort u8 flags(bit0=PSK)
//             u8 nameLen, name[nameLen] (UTF-8 hostname)   [9+n bytes]
//
// Threading: DiscoveryResponder runs one background thread; start()/
// stop() from the control thread. DiscoverServers() is synchronous and
// blocks its caller for ~timeoutMs (call it from a worker thread in UI
// code). Both do their own WSAStartup/WSACleanup (refcounted by winsock).
//
// Requires: MSVC 2022, C++20, Winsock2 (ws2_32.lib via #pragma comment
// in the .cpp).

#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

// Fixed, shared by both ends. Distinct from the (configurable) stream port.
constexpr uint16_t kDiscoveryPort = 47800;

struct DiscoveredServer {
    std::string ip;          // dotted quad
    uint16_t    port = 0;    // GSMR stream port
    std::string hostname;    // server machine name (UTF-8)
    bool        pskRequired = false;
};

// ---------------------------------------------------------------------------
// Server side: answers discovery probes with this machine's info.
// ---------------------------------------------------------------------------
class DiscoveryResponder {
public:
    DiscoveryResponder() = default;
    ~DiscoveryResponder(); // calls stop()

    DiscoveryResponder(const DiscoveryResponder&) = delete;
    DiscoveryResponder& operator=(const DiscoveryResponder&) = delete;

    // Binds UDP kDiscoveryPort (SO_REUSEADDR) and starts the reply thread.
    // Failure is non-fatal for the app: log it and continue without LAN
    // auto-discovery (pairing code still works).
    bool start(uint16_t streamPort, bool pskRequired);
    void stop();

    std::string lastError() const { return m_lastError; }

private:
    void threadProc();

    SOCKET            m_socket = INVALID_SOCKET;
    uint16_t          m_streamPort  = 0;
    bool              m_pskRequired = false;
    std::string       m_hostname;
    std::string       m_lastError;
    std::thread       m_thread;
    std::atomic<bool> m_stopRequested{false};
    bool              m_wsaInitialized = false;
};

// ---------------------------------------------------------------------------
// Client side: broadcast a probe and collect answers for ~timeoutMs.
// Duplicate servers (same ip:port) are folded. Blocking call.
// ---------------------------------------------------------------------------
std::vector<DiscoveredServer> DiscoverServers(uint32_t timeoutMs);

// ---------------------------------------------------------------------------
// Pairing code: (IPv4, port) <-> "XXXX-XXXX-XXXX"
// 7 payload bytes (ip4 + port2 + crc1) in Crockford base32 (no I/L/O/U),
// grouped by dashes. Decode tolerates lowercase, missing dashes and the
// usual 0/O 1/I confusions.
// ---------------------------------------------------------------------------
std::string EncodePairingCode(const std::string& dottedIp, uint16_t port);
bool DecodePairingCode(const std::string& code, std::string& outIp, uint16_t& outPort);

// Local IPv4 the default route uses (connected-UDP-socket trick; no
// packets are sent). Empty string when it cannot be determined.
std::string GetLocalIpv4();
