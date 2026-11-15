// Crypto.h
//
// Optional per-datagram encryption + authentication for the GSMR
// transport, shared by StreamSender (server) and StreamReceiver (client).
//
// Scheme: AES-256-GCM via Windows CNG (bcrypt.dll -- no third-party
// crypto). Both machines configure the same pre-shared key string ("psk"
// in midnightsun_config.json); an empty or missing PSK disables the
// whole layer, and seal()/open() degrade to plain memcpy pass-through so
// the call sites stay branch-free.
//
// Wire layout with encryption enabled (see NetworkProtocol.h):
//
//   [18B PacketHeader, plaintext]  -- authenticated as GCM AAD, so a
//                                     tampered header fails the tag check
//   [12B nonce]                    -- 4B random per-process salt + 8B counter
//   [N  B ciphertext]              -- the payload (may be empty: keepalives)
//   [16B GCM tag]
//
// Key derivation: SHA-256(psk || ":c2s") and SHA-256(psk || ":s2c") --
// one key per direction, so the two independent nonce counters can never
// collide on the same key. No replay protection (LAN-focused tool; the
// sequence window drops stale media anyway) and no forward secrecy --
// this is meant to stop eavesdropping and the "first datagram wins"
// session hijack, not to be TLS.
//
// Threading: seal() and open() are internally serialized per direction
// (one mutex each), so any number of threads may call them.
//
// Requires: MSVC 2022, C++20, bcrypt.lib (linked via #pragma comment in
// the .cpp).

#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>

class PacketCrypto {
public:
    PacketCrypto() = default;
    ~PacketCrypto();

    PacketCrypto(const PacketCrypto&) = delete;
    PacketCrypto& operator=(const PacketCrypto&) = delete;

    // Empty psk => disabled (returns true, seal/open pass through).
    // isServer picks the encrypt/decrypt direction keys. Returns false
    // with lastError() set if CNG setup failed -- treat that as fatal
    // rather than silently streaming plaintext.
    bool init(const std::string& psk, bool isServer);

    bool enabled() const { return m_enabled; }

    // `dgram` = plaintext datagram (18-byte header + payload). `out` must
    // hold len + kCryptoOverhead bytes; in-place (out == dgram) is NOT
    // allowed. Returns the output datagram size, or 0 on failure.
    // Disabled mode: copies through and returns len.
    size_t seal(const uint8_t* dgram, size_t len, uint8_t* out);

    // Reverse of seal(). `dgram` = received datagram, `out` must hold
    // `len` bytes. Returns the plaintext datagram size (header +
    // payload), or 0 if the datagram is malformed or FAILS
    // AUTHENTICATION -- the caller must drop it without touching any
    // state. Disabled mode: copies through and returns len.
    size_t open(const uint8_t* dgram, size_t len, uint8_t* out);

    std::string lastError() const;

private:
    static constexpr size_t kNonceSize = 12;
    static constexpr size_t kTagSize   = 16;
    static constexpr size_t kKeySize   = 32;

    bool deriveKey(const std::string& material, uint8_t (&key)[kKeySize]);
    void setError(std::string message);
    void teardown();

    BCRYPT_ALG_HANDLE m_aesAlg     = nullptr;
    BCRYPT_KEY_HANDLE m_encryptKey = nullptr;
    BCRYPT_KEY_HANDLE m_decryptKey = nullptr;

    uint8_t               m_nonceSalt[4] = {};
    std::atomic<uint64_t> m_nonceCounter{0};

    std::mutex m_encryptMutex;
    std::mutex m_decryptMutex;

    bool m_enabled = false;

    mutable std::mutex m_errorMutex;
    std::string        m_lastError;
};
