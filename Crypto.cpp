// Crypto.cpp -- see Crypto.h for the design overview.

#include "Crypto.h"

#include "NetworkProtocol.h" // kPacketHeaderSize, kCryptoOverhead

#include <cstdio>
#include <cstring>

#pragma comment(lib, "bcrypt.lib")

namespace {

#ifndef NT_SUCCESS
#define NT_SUCCESS(status) (((NTSTATUS)(status)) >= 0)
#endif

std::string NtStatusString(NTSTATUS status)
{
    char buf[32];
    sprintf_s(buf, "0x%08lX", static_cast<unsigned long>(status));
    return buf;
}

} // namespace

PacketCrypto::~PacketCrypto()
{
    teardown();
}

void PacketCrypto::teardown()
{
    if (m_encryptKey) {
        BCryptDestroyKey(m_encryptKey);
        m_encryptKey = nullptr;
    }
    if (m_decryptKey) {
        BCryptDestroyKey(m_decryptKey);
        m_decryptKey = nullptr;
    }
    if (m_aesAlg) {
        BCryptCloseAlgorithmProvider(m_aesAlg, 0);
        m_aesAlg = nullptr;
    }
    m_enabled = false;
}

std::string PacketCrypto::lastError() const
{
    std::lock_guard<std::mutex> lock(m_errorMutex);
    return m_lastError;
}

void PacketCrypto::setError(std::string message)
{
    std::lock_guard<std::mutex> lock(m_errorMutex);
    m_lastError = std::move(message);
}

// SHA-256 of `material` into `key`.
bool PacketCrypto::deriveKey(const std::string& material, uint8_t (&key)[kKeySize])
{
    BCRYPT_ALG_HANDLE  shaAlg  = nullptr;
    BCRYPT_HASH_HANDLE hash    = nullptr;
    NTSTATUS status = BCryptOpenAlgorithmProvider(&shaAlg, BCRYPT_SHA256_ALGORITHM,
                                                  nullptr, 0);
    if (!NT_SUCCESS(status)) {
        setError("BCryptOpenAlgorithmProvider(SHA256) failed: " + NtStatusString(status));
        return false;
    }
    status = BCryptCreateHash(shaAlg, &hash, nullptr, 0, nullptr, 0, 0);
    if (NT_SUCCESS(status)) {
        status = BCryptHashData(hash,
                                reinterpret_cast<PUCHAR>(const_cast<char*>(material.data())),
                                static_cast<ULONG>(material.size()), 0);
    }
    if (NT_SUCCESS(status))
        status = BCryptFinishHash(hash, key, kKeySize, 0);
    if (hash)
        BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(shaAlg, 0);
    if (!NT_SUCCESS(status)) {
        setError("SHA-256 key derivation failed: " + NtStatusString(status));
        return false;
    }
    return true;
}

bool PacketCrypto::init(const std::string& psk, bool isServer)
{
    teardown();
    if (psk.empty())
        return true; // encryption off by configuration

    NTSTATUS status = BCryptOpenAlgorithmProvider(&m_aesAlg, BCRYPT_AES_ALGORITHM,
                                                  nullptr, 0);
    if (!NT_SUCCESS(status)) {
        setError("BCryptOpenAlgorithmProvider(AES) failed: " + NtStatusString(status));
        teardown();
        return false;
    }
    status = BCryptSetProperty(m_aesAlg, BCRYPT_CHAINING_MODE,
                               reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(BCRYPT_CHAIN_MODE_GCM)),
                               sizeof(BCRYPT_CHAIN_MODE_GCM), 0);
    if (!NT_SUCCESS(status)) {
        setError("BCryptSetProperty(GCM) failed: " + NtStatusString(status));
        teardown();
        return false;
    }

    // One key per direction: the client encrypts with c2s, the server
    // with s2c, so the independent nonce counters never collide on a key.
    uint8_t keyC2S[kKeySize], keyS2C[kKeySize];
    if (!deriveKey(psk + ":c2s", keyC2S) || !deriveKey(psk + ":s2c", keyS2C)) {
        teardown();
        return false;
    }
    uint8_t* encKeyBytes = isServer ? keyS2C : keyC2S;
    uint8_t* decKeyBytes = isServer ? keyC2S : keyS2C;

    status = BCryptGenerateSymmetricKey(m_aesAlg, &m_encryptKey, nullptr, 0,
                                        encKeyBytes, kKeySize, 0);
    if (NT_SUCCESS(status))
        status = BCryptGenerateSymmetricKey(m_aesAlg, &m_decryptKey, nullptr, 0,
                                            decKeyBytes, kKeySize, 0);
    SecureZeroMemory(keyC2S, sizeof(keyC2S));
    SecureZeroMemory(keyS2C, sizeof(keyS2C));
    if (!NT_SUCCESS(status)) {
        setError("BCryptGenerateSymmetricKey failed: " + NtStatusString(status));
        teardown();
        return false;
    }

    status = BCryptGenRandom(nullptr, m_nonceSalt, sizeof(m_nonceSalt),
                             BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    if (!NT_SUCCESS(status)) {
        setError("BCryptGenRandom failed: " + NtStatusString(status));
        teardown();
        return false;
    }
    m_nonceCounter.store(0, std::memory_order_relaxed);

    m_enabled = true;
    return true;
}

size_t PacketCrypto::seal(const uint8_t* dgram, size_t len, uint8_t* out)
{
    if (!dgram || !out || len < kPacketHeaderSize)
        return 0;
    if (!m_enabled) {
        memcpy(out, dgram, len);
        return len;
    }
    static_assert(kNonceSize + kTagSize == kCryptoOverhead,
                  "wire overhead constant out of sync");

    const uint8_t* payload      = dgram + kPacketHeaderSize;
    const ULONG    payloadBytes = static_cast<ULONG>(len - kPacketHeaderSize);

    // out = [header][nonce][ciphertext][tag]
    memcpy(out, dgram, kPacketHeaderSize);
    uint8_t* nonce = out + kPacketHeaderSize;
    uint8_t* ct    = nonce + kNonceSize;
    uint8_t* tag   = ct + payloadBytes;

    memcpy(nonce, m_nonceSalt, sizeof(m_nonceSalt));
    const uint64_t counter = m_nonceCounter.fetch_add(1, std::memory_order_relaxed);
    for (int i = 0; i < 8; ++i)
        nonce[4 + i] = static_cast<uint8_t>(counter >> (8 * i));

    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info;
    BCRYPT_INIT_AUTH_MODE_INFO(info);
    info.pbNonce    = nonce;
    info.cbNonce    = kNonceSize;
    info.pbAuthData = out; // the plaintext header is the AAD
    info.cbAuthData = static_cast<ULONG>(kPacketHeaderSize);
    info.pbTag      = tag;
    info.cbTag      = kTagSize;

    ULONG written = 0;
    NTSTATUS status;
    {
        std::lock_guard<std::mutex> lock(m_encryptMutex);
        status = BCryptEncrypt(m_encryptKey,
                               const_cast<PUCHAR>(payload), payloadBytes,
                               &info, nullptr, 0,
                               ct, payloadBytes, &written, 0);
    }
    if (!NT_SUCCESS(status) || written != payloadBytes) {
        setError("BCryptEncrypt failed: " + NtStatusString(status));
        return 0;
    }
    return len + kCryptoOverhead;
}

size_t PacketCrypto::open(const uint8_t* dgram, size_t len, uint8_t* out)
{
    if (!dgram || !out || len < kPacketHeaderSize)
        return 0;
    if (!m_enabled) {
        memcpy(out, dgram, len);
        return len;
    }
    if (len < kPacketHeaderSize + kCryptoOverhead)
        return 0; // too short to carry nonce + tag: not ours

    const uint8_t* nonce   = dgram + kPacketHeaderSize;
    const uint8_t* ct      = nonce + kNonceSize;
    const ULONG    ctBytes = static_cast<ULONG>(len - kPacketHeaderSize - kCryptoOverhead);
    const uint8_t* tag     = ct + ctBytes;

    memcpy(out, dgram, kPacketHeaderSize);

    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info;
    BCRYPT_INIT_AUTH_MODE_INFO(info);
    info.pbNonce    = const_cast<PUCHAR>(nonce);
    info.cbNonce    = kNonceSize;
    info.pbAuthData = const_cast<PUCHAR>(dgram);
    info.cbAuthData = static_cast<ULONG>(kPacketHeaderSize);
    info.pbTag      = const_cast<PUCHAR>(tag);
    info.cbTag      = kTagSize;

    ULONG written = 0;
    NTSTATUS status;
    {
        std::lock_guard<std::mutex> lock(m_decryptMutex);
        status = BCryptDecrypt(m_decryptKey,
                               const_cast<PUCHAR>(ct), ctBytes,
                               &info, nullptr, 0,
                               out + kPacketHeaderSize, ctBytes, &written, 0);
    }
    if (!NT_SUCCESS(status) || written != ctBytes)
        return 0; // auth failure or corruption: caller drops silently
    return kPacketHeaderSize + ctBytes;
}
