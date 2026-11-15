// NvencEncoder.h
//
// NVENC H.264 hardware encoder for ultra-low-latency game streaming.
//
// Talks to nvEncodeAPI directly (no FFmpeg). Designed to sit behind
// ScreenCapture: the ID3D11Texture2D handed to encode() must live on the
// same D3D11 device that was passed to initialize() -- in this project,
// ScreenCapture::device(). Input textures are registered with NVENC and
// encoded zero-copy: no staging textures, no CPU readback, no per-frame
// allocation on the GPU path. The only copy is the encoded bitstream out
// of NVENC's output buffer into the returned std::vector.
//
// Encoder configuration (fixed by design, for streaming):
//   - H.264, preset P4 + ultra-low-latency tuning
//   - CBR with a single-frame VBV (vbvBufferSize = bitrate / fps), so the
//     rate control never builds up queued bits that add latency
//   - Spatial AQ + CABAC: AQ moves bits into flat/smooth areas where CBR
//     blocking ("grain") is most visible; CABAC buys ~10% bitrate for free
//   - Periodic intra refresh (a rolling column of intra macroblocks)
//     instead of periodic IDR frames: the stream continuously self-heals
//     without the visible quality pulse a full IDR causes under a
//     one-frame VBV. Full IDRs still happen on the first frame, on
//     resolution changes, and on requestKeyframe().
//   - Infinite GOP; SPS/PPS repeated on every IDR so a client can join
//     the stream at any keyframe
//   - No B-frames, synchronous mode, one frame in flight: encode()
//     returns the bitstream for exactly the frame it was given
//   - setBitrate() reconfigures the session in place (no IDR, no reset)
//     for the adaptive bitrate controller
//
// Output is an H.264 Annex B byte stream (start-code delimited NAL
// units), ready to packetize for the network or feed to any decoder.
//
// Threading model:
//   - initialize()/shutdown()/encode() must all be called from one thread
//     (the consumer thread that calls ScreenCapture::getLatestFrame()).
//   - requestKeyframe() may be called from any thread.
//   - encode() submits GPU work on the shared device; ScreenCapture
//     enables ID3D11Multithread protection, so this is safe alongside
//     the capture thread's copies.
//
// Input lifetime: encode() is synchronous -- the texture is fully read by
// the time it returns, so a CaptureFrame texture only needs to stay valid
// for the duration of the call (the ScreenCapture contract guarantees
// more than that). Registered textures are cached and kept alive by the
// encoder between calls; this is what makes re-registering the capture
// ring's three textures free after the first frame of each.
//
// Resolution changes: if a frame arrives with a different size than the
// previous one, the encoder is torn down and rebuilt transparently; the
// next returned bitstream starts with a fresh IDR + SPS/PPS.
//
// Latency: synchronous single-frame-in-flight with ULL tuning encodes a
// 1080p BGRA frame in roughly 2-3 ms on an RTX 4070, inside the 4 ms
// budget for 60 fps streaming with headroom.
//
// Requires: MSVC 2022, C++20, NVIDIA Video Codec SDK headers in
// extern/nvenc/ (nvEncodeAPI.h), an NVENC-capable NVIDIA GPU, and a
// driver at least as new as the SDK headers. nvEncodeAPI64.dll is loaded
// at runtime -- there is no import library to link.

#pragma once

#include <d3d11.h>
#include <wrl/client.h>

#include "extern/nvenc/nvEncodeAPI.h"

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

class NvencEncoder {
public:
    struct Config {
        uint32_t bitrateBps   = 20'000'000; // CBR target (and max) bitrate
        uint32_t frameRateNum = 60;         // rate-control frame rate, numerator
        uint32_t frameRateDen = 1;          // rate-control frame rate, denominator
    };

    NvencEncoder() = default;
    ~NvencEncoder();

    // Non-copyable, non-movable: owns NVENC session handles that embed
    // this object's device binding.
    NvencEncoder(const NvencEncoder&) = delete;
    NvencEncoder& operator=(const NvencEncoder&) = delete;

    // Loads nvEncodeAPI64.dll, verifies the driver supports the SDK
    // version these headers were built against, and opens an encode
    // session on `device`. Frame dimensions are taken from the first
    // texture passed to encode(), so no size is needed here. Returns
    // false with lastError() set on failure (no NVIDIA driver, GPU
    // without NVENC, driver too old, ...). Calling initialize() on an
    // already-initialized encoder shuts it down and starts over.
    bool initialize(ID3D11Device* device, const Config& config = {});

    // Flushes and destroys the encode session and releases all cached
    // texture registrations. Safe to call when not initialized; called
    // automatically by the destructor.
    void shutdown();

    // Encodes one frame and returns its Annex B NAL units. The texture
    // must be DXGI_FORMAT_B8G8R8A8_UNORM (or R8G8B8A8_UNORM) on the
    // device passed to initialize(). Returns an empty vector on failure
    // -- check lastError(). The first frame, and the first frame after a
    // resolution change, is automatically an IDR with SPS/PPS.
    std::vector<uint8_t> encode(ID3D11Texture2D* frame);

    // Forces the next encode() to produce an IDR frame with SPS/PPS
    // (e.g. when a new client connects or after packet loss). Callable
    // from any thread. If the next encode() fails, the request is kept
    // for the one after it.
    void requestKeyframe() { m_forceIdr.store(true, std::memory_order_release); }

    // Changes the CBR target (and the matching one-frame VBV) without
    // rebuilding the session or forcing an IDR; applied by the next
    // encode(). Callable from any thread -- this is the adaptive bitrate
    // hook. Values are clamped to [1 Mbps, 500 Mbps].
    void setBitrate(uint32_t bitrateBps);

    // The bitrate most recently applied to the session (0 before the
    // first encode). Any thread.
    uint32_t currentBitrate() const {
        return m_currentBitrate.load(std::memory_order_relaxed);
    }

    // Description of the most recent failure: the failing call, the
    // NVENCSTATUS name, and the driver's own last-error text when
    // available. Only meaningful after initialize()/encode() returned
    // failure; read it from the encoding thread.
    const std::string& lastError() const { return m_lastError; }

    bool isInitialized() const { return m_encoder != nullptr; }

private:
    template <typename T>
    using ComPtr = Microsoft::WRL::ComPtr<T>;

    // A capture-side texture that has been registered with NVENC. The
    // ComPtr keeps the texture alive while NVENC holds a handle to it,
    // even if the producer rebuilds its ring; capped at kMaxRegistrations
    // entries, after which the cache is flushed wholesale.
    struct Registration {
        ComPtr<ID3D11Texture2D> texture;
        NV_ENC_REGISTERED_PTR   handle = nullptr;
    };
    static constexpr size_t kMaxRegistrations = 8;

    bool loadApi();                                       // DLL + function list + version check
    bool openSession();                                   // NvEncOpenEncodeSessionEx on m_device
    bool initializeEncoder(uint32_t width, uint32_t height); // preset/RC/GOP setup + output buffer
    void applyPendingBitrate();                           // NvEncReconfigureEncoder, encode thread
    void destroyEncoderSession();                         // EOS flush, unregister, destroy session
    NV_ENC_REGISTERED_PTR registerOrLookup(ID3D11Texture2D* texture,
                                           const D3D11_TEXTURE2D_DESC& desc);

    // Records "`where` failed: <status name>: <driver detail>" in
    // m_lastError and returns false so call sites can `return setError(...)`.
    bool setError(const char* where, NVENCSTATUS status);
    bool setError(const char* where, const char* detail);
    static const char* statusToString(NVENCSTATUS status);

    // --- API / session ---
    HMODULE                      m_nvencLib = nullptr;
    NV_ENCODE_API_FUNCTION_LIST  m_nvenc = {};
    ComPtr<ID3D11Device>         m_device;
    void*                        m_encoder = nullptr; // NVENC session handle
    bool                         m_encoderInitialized = false;
    Config                       m_config;

    // --- per-resolution state (rebuilt on size change) ---
    uint32_t                     m_width  = 0;
    uint32_t                     m_height = 0;
    NV_ENC_OUTPUT_PTR            m_bitstream = nullptr;
    std::vector<Registration>    m_registrations;

    // Live copies of the session parameters, kept for in-place
    // reconfiguration (setBitrate). m_initParams.encodeConfig always
    // points at m_encodeConfig. Encode thread only.
    NV_ENC_INITIALIZE_PARAMS     m_initParams   = {};
    NV_ENC_CONFIG                m_encodeConfig = {};

    // --- streaming state ---
    std::atomic<bool>            m_forceIdr{false};
    std::atomic<uint32_t>        m_pendingBitrate{0}; // 0 = no change requested
    std::atomic<uint32_t>        m_currentBitrate{0};
    uint64_t                     m_frameIndex = 0;
    std::string                  m_lastError;
};
