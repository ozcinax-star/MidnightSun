// VideoScaler.cpp -- see VideoScaler.h for the API contract.

#include "VideoScaler.h"

#include <cstdarg>
#include <cstdio>

#pragma comment(lib, "d3d11.lib")

namespace {

void LogF(const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    fputc('\n', stderr);
}

} // namespace

VideoScaler::~VideoScaler()
{
    shutdown();
}

bool VideoScaler::setError(const char* what, HRESULT hr)
{
    char buf[160];
    sprintf_s(buf, "%s failed: 0x%08lX", what, hr);
    m_lastError = buf;
    LogF("[VideoScaler] %s", buf);
    return false;
}

bool VideoScaler::initialize(ID3D11Device* device, uint32_t targetHeight)
{
    if (!device || targetHeight == 0) {
        m_lastError = "invalid arguments";
        return false;
    }

    HRESULT hr = device->QueryInterface(IID_PPV_ARGS(&m_videoDevice));
    if (FAILED(hr))
        return setError("ID3D11Device -> ID3D11VideoDevice (no video support on this device)", hr);

    ComPtr<ID3D11DeviceContext> context;
    device->GetImmediateContext(&context);
    hr = context.As(&m_videoContext);
    if (FAILED(hr)) {
        m_videoDevice.Reset();
        return setError("ID3D11DeviceContext -> ID3D11VideoContext", hr);
    }

    m_device       = device;
    m_targetHeight = targetHeight;
    return true;
}

void VideoScaler::shutdown()
{
    m_inputViews.clear();
    m_outputView.Reset();
    m_output.Reset();
    m_vp.Reset();
    m_vpEnum.Reset();
    m_videoContext.Reset();
    m_videoDevice.Reset();
    m_device.Reset();
    m_inWidth = m_inHeight = m_outWidth = m_outHeight = 0;
}

bool VideoScaler::rebuild(uint32_t inWidth, uint32_t inHeight)
{
    m_inputViews.clear();
    m_outputView.Reset();
    m_output.Reset();
    m_vp.Reset();
    m_vpEnum.Reset();

    // Output: target height, width from the input aspect ratio, both even
    // (H.264 / NV12 want even dimensions).
    const uint32_t outHeight = m_targetHeight & ~1u;
    uint32_t outWidth = static_cast<uint32_t>(
        (static_cast<uint64_t>(inWidth) * outHeight + inHeight / 2) / inHeight);
    outWidth &= ~1u;
    if (outWidth == 0 || outHeight == 0) {
        m_lastError = "degenerate output size";
        return false;
    }

    D3D11_VIDEO_PROCESSOR_CONTENT_DESC desc = {};
    desc.InputFrameFormat            = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    desc.InputFrameRate.Numerator    = 60;
    desc.InputFrameRate.Denominator  = 1;
    desc.InputWidth                  = inWidth;
    desc.InputHeight                 = inHeight;
    desc.OutputFrameRate.Numerator   = 60;
    desc.OutputFrameRate.Denominator = 1;
    desc.OutputWidth                 = outWidth;
    desc.OutputHeight                = outHeight;
    desc.Usage                       = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;

    HRESULT hr = m_videoDevice->CreateVideoProcessorEnumerator(&desc, &m_vpEnum);
    if (FAILED(hr))
        return setError("CreateVideoProcessorEnumerator", hr);

    hr = m_videoDevice->CreateVideoProcessor(m_vpEnum.Get(), 0, &m_vp);
    if (FAILED(hr))
        return setError("CreateVideoProcessor", hr);

    D3D11_TEXTURE2D_DESC texDesc = {};
    texDesc.Width            = outWidth;
    texDesc.Height           = outHeight;
    texDesc.MipLevels        = 1;
    texDesc.ArraySize        = 1;
    texDesc.Format           = DXGI_FORMAT_B8G8R8A8_UNORM; // same as capture
    texDesc.SampleDesc.Count = 1;
    texDesc.Usage            = D3D11_USAGE_DEFAULT;
    // RENDER_TARGET is required for a video processor output view;
    // SHADER_RESOURCE keeps the texture usable by NVENC's interop.
    texDesc.BindFlags        = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

    hr = m_device->CreateTexture2D(&texDesc, nullptr, &m_output);
    if (FAILED(hr))
        return setError("CreateTexture2D(scaled output)", hr);

    D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC outViewDesc = {};
    outViewDesc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
    hr = m_videoDevice->CreateVideoProcessorOutputView(
        m_output.Get(), m_vpEnum.Get(), &outViewDesc, &m_outputView);
    if (FAILED(hr))
        return setError("CreateVideoProcessorOutputView", hr);

    // Geometry: full input rect -> full output rect. Auto processing off so
    // no driver "enhancement" (sharpening, denoise) sneaks into the stream.
    const RECT srcRect = { 0, 0, static_cast<LONG>(inWidth), static_cast<LONG>(inHeight) };
    const RECT dstRect = { 0, 0, static_cast<LONG>(outWidth), static_cast<LONG>(outHeight) };
    m_videoContext->VideoProcessorSetStreamSourceRect(m_vp.Get(), 0, TRUE, &srcRect);
    m_videoContext->VideoProcessorSetStreamDestRect(m_vp.Get(), 0, TRUE, &dstRect);
    m_videoContext->VideoProcessorSetOutputTargetRect(m_vp.Get(), TRUE, &dstRect);
    m_videoContext->VideoProcessorSetStreamAutoProcessingMode(m_vp.Get(), 0, FALSE);

    m_inWidth   = inWidth;
    m_inHeight  = inHeight;
    m_outWidth  = outWidth;
    m_outHeight = outHeight;
    LogF("[VideoScaler] %ux%u -> %ux%u (BGRA, GPU video processor)",
         inWidth, inHeight, outWidth, outHeight);
    return true;
}

ID3D11VideoProcessorInputView* VideoScaler::lookupInputView(ID3D11Texture2D* texture)
{
    for (const auto& entry : m_inputViews) {
        if (entry.first == texture)
            return entry.second.Get();
    }

    D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC inDesc = {};
    inDesc.FourCC               = 0; // derive from the texture format
    inDesc.ViewDimension        = D3D11_VPIV_DIMENSION_TEXTURE2D;
    inDesc.Texture2D.MipSlice   = 0;
    inDesc.Texture2D.ArraySlice = 0;

    ComPtr<ID3D11VideoProcessorInputView> view;
    HRESULT hr = m_videoDevice->CreateVideoProcessorInputView(
        texture, m_vpEnum.Get(), &inDesc, &view);
    if (FAILED(hr)) {
        setError("CreateVideoProcessorInputView", hr);
        return nullptr;
    }

    if (m_inputViews.size() >= kMaxCachedInputViews)
        m_inputViews.clear(); // capture ring rebuilt; drop stale entries
    m_inputViews.emplace_back(texture, view);
    return m_inputViews.back().second.Get();
}

ID3D11Texture2D* VideoScaler::scale(ID3D11Texture2D* input)
{
    if (!input || !m_videoDevice) {
        m_lastError = "not initialized";
        return nullptr;
    }

    D3D11_TEXTURE2D_DESC desc = {};
    input->GetDesc(&desc);

    // Passthrough when the source is already at or below the target height:
    // never upscale, never burn a blit for nothing.
    if (desc.Height <= m_targetHeight)
        return input;

    if (desc.Width != m_inWidth || desc.Height != m_inHeight) {
        if (!rebuild(desc.Width, desc.Height))
            return nullptr;
    }

    ID3D11VideoProcessorInputView* inputView = lookupInputView(input);
    if (!inputView)
        return nullptr;

    D3D11_VIDEO_PROCESSOR_STREAM stream = {};
    stream.Enable        = TRUE;
    stream.pInputSurface = inputView;

    HRESULT hr = m_videoContext->VideoProcessorBlt(
        m_vp.Get(), m_outputView.Get(), 0, 1, &stream);
    if (FAILED(hr)) {
        setError("VideoProcessorBlt", hr);
        return nullptr;
    }

    return m_output.Get();
}
