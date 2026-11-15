// VideoScaler.h
//
// Server-side GPU downscale pass between ScreenCapture and NvencEncoder.
// The capture delivers the desktop at native resolution (e.g. 2560x1440);
// when the client display is smaller (e.g. 1080p) encoding at native size
// wastes bitrate and NVENC time. scale() blits the captured BGRA texture
// onto a smaller BGRA target through the D3D11 video processor -- one GPU
// pass, no CPU involvement -- and the encoder consumes the small texture
// exactly like it would the capture ring texture.
//
// Wiring (all on the Pipeline-Encode thread, same device as the capture):
//
//   VideoScaler scaler;
//   scaler.initialize(capture.device(), 1080);
//   ...
//   ID3D11Texture2D* tex = scaler.scale(frame.texture); // owned by scaler
//   encoder.encode(tex);
//
// Ownership / lifetime: the returned texture is owned by the scaler and its
// contents stay valid until the next scale() call -- which matches the
// synchronous encoder (encode() returns only after NVENC read the input).
// When the input is already at or below the target height, scale() returns
// the input pointer unchanged (zero-cost passthrough).
//
// Resolution changes: an input size change rebuilds the video processor and
// the output texture transparently; output width follows the input aspect
// ratio rounded down to even (H.264 wants even dimensions).
//
// Threading: initialize()/scale()/shutdown() from one thread (the encode
// thread). The device must have ID3D11Multithread protection enabled
// (ScreenCapture::startCapture() already does this), since the blit is
// submitted on the shared immediate context.
//
// Requires: MSVC 2022, C++20, Windows 10. Links d3d11.lib via #pragma
// comment in the .cpp.

#pragma once

#include <d3d11.h>
#include <wrl/client.h>

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

class VideoScaler {
public:
    VideoScaler() = default;
    ~VideoScaler();

    VideoScaler(const VideoScaler&)            = delete;
    VideoScaler& operator=(const VideoScaler&) = delete;

    // device: the ScreenCapture / NvencEncoder device. targetHeight: encode
    // height (e.g. 1080); width follows the input aspect ratio. Returns
    // false if the device exposes no video processing support.
    bool initialize(ID3D11Device* device, uint32_t targetHeight);
    void shutdown();

    // Returns the scaled BGRA texture (owned by the scaler, contents valid
    // until the next scale() call), the input itself when no scaling is
    // needed, or nullptr on failure (see lastError()).
    ID3D11Texture2D* scale(ID3D11Texture2D* input);

    const std::string& lastError() const { return m_lastError; }

private:
    bool rebuild(uint32_t inWidth, uint32_t inHeight);
    ID3D11VideoProcessorInputView* lookupInputView(ID3D11Texture2D* texture);
    bool setError(const char* what, HRESULT hr);

    template <typename T> using ComPtr = Microsoft::WRL::ComPtr<T>;

    ComPtr<ID3D11Device>                  m_device;
    ComPtr<ID3D11VideoDevice>             m_videoDevice;
    ComPtr<ID3D11VideoContext>            m_videoContext;
    ComPtr<ID3D11VideoProcessorEnumerator> m_vpEnum;
    ComPtr<ID3D11VideoProcessor>          m_vp;
    ComPtr<ID3D11Texture2D>               m_output;
    ComPtr<ID3D11VideoProcessorOutputView> m_outputView;

    // Input views cached per source texture (the capture ring has 3); the
    // cache is cleared wholesale when it grows past ring size (ring rebuild
    // after a mode change left stale entries).
    static constexpr size_t kMaxCachedInputViews = 8;
    std::vector<std::pair<ID3D11Texture2D*, ComPtr<ID3D11VideoProcessorInputView>>> m_inputViews;

    uint32_t m_targetHeight = 0;
    uint32_t m_inWidth      = 0;
    uint32_t m_inHeight     = 0;
    uint32_t m_outWidth     = 0;
    uint32_t m_outHeight    = 0;

    std::string m_lastError;
};
