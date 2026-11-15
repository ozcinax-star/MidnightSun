// VideoDecoder.h
//
// Client-side H.264 video decoder for the game streaming pipeline. Consumes
// the EncodedFrame units that StreamReceiver delivers (onVideoFrame),
// decodes them through the Media Foundation H.264 decoder MFT, and hands
// the resulting NV12 ID3D11Texture2D to onDecodedFrame -- the texture that
// RenderWindow::presentFrame() converts and presents.
//
// Wiring -- decode() is thread-safe, the callback fires on whatever thread
// called decode() (GSMR-Recv in this project):
//
//   VideoDecoder decoder;
//   decoder.onDecodedFrame = [&](ID3D11Texture2D* tex, uint64_t tsUs) { ... };
//   decoder.initialize(device);
//   receiver.onVideoFrame = [&](EncodedFrame f) { decoder.decode(f); };
//
// Decode path: the primary path is D3D11VA hardware decode -- the MFT is
// D3D11-aware, gets the device through an IMFDXGIDeviceManager
// (MFCreateDXGIDeviceManager + MFT_MESSAGE_SET_D3D_MANAGER), and outputs
// NV12 directly into GPU memory. The decoded surface (an array slice of
// the MFT's internal allocator) is copied GPU-to-GPU into a small ring of
// our own NV12 textures -- one CopySubresourceRegion, no CPU readback --
// because the MFT recycles its allocator samples and a borrowed slice
// must not outlive ProcessOutput. If the device manager handshake fails
// (no video support, software adapter), the MFT falls back to system-
// memory software decode and the frame is uploaded through one staging
// texture instead; everything downstream is identical.
//
// Output texture lifetime: the ring holds kOutputRingSize (3) textures, so
// the pointer given to onDecodedFrame stays valid (kept alive by the
// decoder's ComPtr) and its CONTENTS stay valid until two more frames have
// been decoded. The render side either presents it promptly or copies it.
// Textures are released on shutdown() and on resolution changes; consumers
// that hold one across those (the render thread's "last frame") must
// AddRef it (store a ComPtr), which keeps the object alive even after the
// decoder drops it.
//
// Resolution changes: the MFT signals MF_E_TRANSFORM_STREAM_CHANGE, the
// output type is renegotiated (NV12 again, new size, display aperture
// honored so coded 1088-row frames render as 1080), and the output ring is
// rebuilt. The first frame after the change comes out at the new size with
// no caller involvement. Decode errors latch a flush request that is
// serviced at the next IDR (MFT_MESSAGE_COMMAND_FLUSH), so a corrupted
// reference chain never outlives one keyframe interval.
//
// Threading model:
//   - initialize()/shutdown() from the control thread.
//   - decode() from one thread (the receiver thread). A single mutex
//     serializes decode() against shutdown(); the callback runs under it,
//     so keep onDecodedFrame fast (store + signal, no GPU stalls).
//   - The MFT submits GPU work on the shared immediate context;
//     initialize() enables ID3D11Multithread protection on the device so
//     this is safe alongside the render thread (and is also required by
//     Media Foundation for D3D11 decoding).
//
// Requires: MSVC 2022, C++20, Windows 10. Links mf.lib, mfplat.lib,
// mfuuid.lib, d3d11.lib via #pragma comment in the .cpp.

#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <d3d11.h>
#include <mfidl.h>
#include <mftransform.h>
#include <wrl/client.h>

#include "NetworkProtocol.h"

#include <cstdint>
#include <functional>
#include <mutex>
#include <string>

class VideoDecoder {
public:
    VideoDecoder() = default;
    ~VideoDecoder(); // calls shutdown()

    VideoDecoder(const VideoDecoder&) = delete;
    VideoDecoder& operator=(const VideoDecoder&) = delete;

    // Set before the first decode(); invoked on the decode() caller's
    // thread for every frame that comes out of the MFT, in decode order.
    // The texture is NV12, display-cropped, on the device passed to
    // initialize() -- see the lifetime contract above.
    std::function<void(ID3D11Texture2D*, uint64_t tsUs)> onDecodedFrame;

    // Starts Media Foundation, instantiates the H.264 decoder MFT, hooks
    // it to `device` for D3D11VA (falling back to software decode if that
    // fails -- logged, not fatal), and negotiates H.264-in / NV12-out.
    // Returns false with lastError() set on hard failures (no decoder MFT
    // registered, type negotiation rejected). Re-initializing an already
    // initialized decoder shuts it down first.
    bool initialize(ID3D11Device* device);

    // Releases the MFT, the device manager, all output textures, and shuts
    // Media Foundation down. Safe to call when not initialized.
    void shutdown();

    // Feeds one encoded frame (H.264 Annex B) and delivers every decoded
    // frame it produces through onDecodedFrame. Errors are logged and the
    // frame is dropped; the decoder resynchronizes at the next keyframe.
    // No-op when not initialized.
    void decode(const EncodedFrame& frame);

    bool isInitialized() const { return m_initialized; }

    // True when the D3D11VA path is active (false = software fallback).
    bool isHardwareAccelerated() const { return m_hardware; }

    // Most recent failure description; read after initialize() returns
    // false or when frames stop coming out.
    std::string lastError() const;

private:
    template <typename T>
    using ComPtr = Microsoft::WRL::ComPtr<T>;

    // Three slots: one being rendered, one just delivered, one being
    // written -- mirrors the capture-side triple buffer.
    static constexpr size_t kOutputRingSize = 3;

    bool createTransform();
    bool attachDeviceManager();
    bool setInputType();
    bool negotiateOutputType();     // also re-run on MF_E_TRANSFORM_STREAM_CHANGE
    void drainOutputs();
    void deliver(IMFSample* sample);
    bool deliverHardware(IMFMediaBuffer* buffer, ID3D11Texture2D** outTexture);
    bool deliverSoftware(IMFMediaBuffer* buffer, ID3D11Texture2D** outTexture);
    bool createSoftwareSample(ComPtr<IMFSample>& outSample);
    bool ensureOutputRing();
    bool ensureStaging();
    void releaseOutputResources();
    void releaseEverythingLocked();

    // Records "`where` failed: <hr + FormatMessageA text>" in m_lastError,
    // logs it, and returns false so call sites can `return setError(...)`.
    bool setError(const char* where, HRESULT hr);
    bool setError(const char* where, const char* detail);

    // Serializes decode() against initialize()/shutdown().
    std::mutex m_mutex;

    // --- device / MFT (guarded by m_mutex after initialize()) ---
    ComPtr<ID3D11Device>          m_device;
    ComPtr<ID3D11DeviceContext>   m_context;
    ComPtr<IMFTransform>          m_transform;
    ComPtr<IMFDXGIDeviceManager>  m_deviceManager;
    UINT                          m_resetToken = 0;
    MFT_OUTPUT_STREAM_INFO        m_streamInfo = {};
    bool m_mfStarted       = false;
    bool m_initialized     = false;
    bool m_hardware        = false;
    bool m_providesSamples = false; // MFT allocates output samples (HW path)

    // --- negotiated format (coded size vs display aperture) ---
    UINT m_codedWidth    = 0;
    UINT m_codedHeight   = 0;
    UINT m_displayWidth  = 0;
    UINT m_displayHeight = 0;
    UINT m_apertureX     = 0;
    UINT m_apertureY     = 0;

    // --- output ring + software-path staging ---
    ComPtr<ID3D11Texture2D> m_outputRing[kOutputRingSize];
    size_t                  m_ringIndex = 0;
    ComPtr<ID3D11Texture2D> m_staging;

    // --- streaming state ---
    bool     m_flushOnNextKeyframe = false; // latched on decode errors
    uint64_t m_lastInputTsUs       = 0;
    uint32_t m_decodeFailures      = 0;     // throttles error logging

    mutable std::mutex m_errorMutex;
    std::string        m_lastError;
};
