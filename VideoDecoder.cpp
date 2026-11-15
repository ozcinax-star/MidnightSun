// VideoDecoder.cpp -- see VideoDecoder.h for the design overview.

#include "VideoDecoder.h"

#include <d3d11_4.h> // ID3D11Multithread
#include <mfapi.h>
#include <mferror.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>

#pragma comment(lib, "mf.lib")
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "ole32.lib") // CoTaskMemFree for the MFTEnumEx activate list

namespace {

// ---------------------------------------------------------------------------
// Logging. Same shape as the other modules: debugger + stderr, stack buffer
// only, called on errors and state changes -- never per frame.
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

std::string HresultString(HRESULT hr)
{
    char sysMsg[512] = {};
    DWORD len = FormatMessageA(
        FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, static_cast<DWORD>(hr),
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        sysMsg, static_cast<DWORD>(sizeof(sysMsg)), nullptr);
    while (len > 0 && (sysMsg[len - 1] == '\r' || sysMsg[len - 1] == '\n'))
        sysMsg[--len] = '\0';

    char buf[600];
    if (len > 0)
        sprintf_s(buf, "0x%08lX (%s)", static_cast<unsigned long>(hr), sysMsg);
    else
        sprintf_s(buf, "0x%08lX", static_cast<unsigned long>(hr));
    return buf;
}

} // namespace

// ---------------------------------------------------------------------------
// Error recording
// ---------------------------------------------------------------------------

bool VideoDecoder::setError(const char* where, HRESULT hr)
{
    std::string message = std::string(where) + " failed: " + HresultString(hr);
    LogF("[VideoDecoder] %s", message.c_str());
    {
        std::lock_guard<std::mutex> lock(m_errorMutex);
        m_lastError = std::move(message);
    }
    return false;
}

bool VideoDecoder::setError(const char* where, const char* detail)
{
    std::string message = std::string(where) + " failed: " + detail;
    LogF("[VideoDecoder] %s", message.c_str());
    {
        std::lock_guard<std::mutex> lock(m_errorMutex);
        m_lastError = std::move(message);
    }
    return false;
}

std::string VideoDecoder::lastError() const
{
    std::lock_guard<std::mutex> lock(m_errorMutex);
    return m_lastError;
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

VideoDecoder::~VideoDecoder()
{
    shutdown();
}

bool VideoDecoder::initialize(ID3D11Device* device)
{
    if (!device)
        return setError("initialize", "device is null");

    std::lock_guard<std::mutex> lock(m_mutex);
    releaseEverythingLocked();

    HRESULT hr = MFStartup(MF_VERSION, MFSTARTUP_LITE);
    if (FAILED(hr))
        return setError("MFStartup", hr);
    m_mfStarted = true;

    m_device = device;
    m_device->GetImmediateContext(&m_context);

    // Media Foundation requires multithread protection on the device for
    // D3D11 decoding, and the decode thread shares the immediate context
    // with the render thread anyway.
    {
        ComPtr<ID3D11Multithread> mt;
        if (SUCCEEDED(m_context.As(&mt)))
            mt->SetMultithreadProtected(TRUE);
    }

    if (!createTransform() || !setInputType() || !negotiateOutputType()) {
        releaseEverythingLocked();
        return false; // setError already recorded the cause
    }

    HRESULT hrMsg = m_transform->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    if (FAILED(hrMsg)) {
        setError("ProcessMessage(NOTIFY_BEGIN_STREAMING)", hrMsg);
        releaseEverythingLocked();
        return false;
    }
    hrMsg = m_transform->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    if (FAILED(hrMsg)) {
        setError("ProcessMessage(NOTIFY_START_OF_STREAM)", hrMsg);
        releaseEverythingLocked();
        return false;
    }

    m_initialized = true;
    LogF("[VideoDecoder] initialized: %s decode, output %ux%u NV12",
         m_hardware ? "D3D11VA hardware" : "software (MF fallback)",
         m_displayWidth, m_displayHeight);
    return true;
}

void VideoDecoder::shutdown()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    releaseEverythingLocked();
}

void VideoDecoder::releaseEverythingLocked()
{
    if (m_transform) {
        // Best-effort: tell the MFT the stream ended so it can release
        // its sample allocator cleanly. Failures here are uninteresting.
        m_transform->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
        m_transform->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
    }
    m_transform.Reset();
    m_deviceManager.Reset();
    releaseOutputResources();
    m_context.Reset();
    m_device.Reset();

    m_streamInfo           = {};
    m_resetToken           = 0;
    m_hardware             = false;
    m_providesSamples      = false;
    m_initialized          = false;
    m_flushOnNextKeyframe  = false;
    m_lastInputTsUs        = 0;
    m_decodeFailures       = 0;
    m_codedWidth = m_codedHeight = 0;
    m_displayWidth = m_displayHeight = 0;
    m_apertureX = m_apertureY = 0;

    if (m_mfStarted) {
        MFShutdown();
        m_mfStarted = false;
    }
}

void VideoDecoder::releaseOutputResources()
{
    for (auto& tex : m_outputRing)
        tex.Reset();
    m_staging.Reset();
    m_ringIndex = 0;
}

// ---------------------------------------------------------------------------
// MFT setup
// ---------------------------------------------------------------------------

bool VideoDecoder::createTransform()
{
    MFT_REGISTER_TYPE_INFO inputInfo = { MFMediaType_Video, MFVideoFormat_H264 };

    IMFActivate** activates = nullptr;
    UINT32 count = 0;
    HRESULT hr = MFTEnumEx(MFT_CATEGORY_VIDEO_DECODER,
                           MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_LOCALMFT |
                               MFT_ENUM_FLAG_SORTANDFILTER,
                           &inputInfo, nullptr, &activates, &count);
    if (FAILED(hr))
        return setError("MFTEnumEx(MFT_CATEGORY_VIDEO_DECODER)", hr);
    if (count == 0)
        return setError("MFTEnumEx", "no H.264 decoder MFT registered");

    hr = activates[0]->ActivateObject(IID_PPV_ARGS(&m_transform));
    for (UINT32 i = 0; i < count; ++i)
        activates[i]->Release();
    CoTaskMemFree(activates);
    if (FAILED(hr))
        return setError("IMFActivate::ActivateObject", hr);

    return attachDeviceManager();
}

bool VideoDecoder::attachDeviceManager()
{
    // Hardware (D3D11VA) is best-effort: any failure on this path drops to
    // software decode, which is a quality-of-service change, not an error.
    ComPtr<IMFAttributes> attrs;
    HRESULT hr = m_transform->GetAttributes(&attrs);
    if (FAILED(hr) || !attrs) {
        LogF("[VideoDecoder] decoder MFT exposes no attributes; using software decode");
        return true;
    }

    // Low-latency mode: one-in/one-out, no internal frame queueing. Set
    // regardless of the decode path.
    attrs->SetUINT32(MF_LOW_LATENCY, TRUE);

    if (MFGetAttributeUINT32(attrs.Get(), MF_SA_D3D11_AWARE, FALSE) == FALSE) {
        LogF("[VideoDecoder] decoder MFT is not D3D11-aware; using software decode");
        return true;
    }

    hr = MFCreateDXGIDeviceManager(&m_resetToken, &m_deviceManager);
    if (FAILED(hr)) {
        LogF("[VideoDecoder] MFCreateDXGIDeviceManager failed: %s; using software decode",
             HresultString(hr).c_str());
        m_deviceManager.Reset();
        return true;
    }

    hr = m_deviceManager->ResetDevice(m_device.Get(), m_resetToken);
    if (FAILED(hr)) {
        LogF("[VideoDecoder] IMFDXGIDeviceManager::ResetDevice failed: %s; "
             "using software decode (was the device created with "
             "D3D11_CREATE_DEVICE_VIDEO_SUPPORT?)", HresultString(hr).c_str());
        m_deviceManager.Reset();
        return true;
    }

    hr = m_transform->ProcessMessage(
        MFT_MESSAGE_SET_D3D_MANAGER,
        reinterpret_cast<ULONG_PTR>(m_deviceManager.Get()));
    if (FAILED(hr)) {
        LogF("[VideoDecoder] MFT_MESSAGE_SET_D3D_MANAGER failed: %s; using software decode",
             HresultString(hr).c_str());
        m_deviceManager.Reset();
        return true;
    }

    m_hardware = true;
    return true;
}

bool VideoDecoder::setInputType()
{
    ComPtr<IMFMediaType> inputType;
    HRESULT hr = MFCreateMediaType(&inputType);
    if (FAILED(hr))
        return setError("MFCreateMediaType(input)", hr);

    hr = inputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    if (FAILED(hr))
        return setError("SetGUID(MF_MT_MAJOR_TYPE)", hr);
    hr = inputType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    if (FAILED(hr))
        return setError("SetGUID(MF_MT_SUBTYPE)", hr);
    // The stream is progressive by construction (desktop capture), but the
    // decoder parses the real interlacing from the SPS either way.
    hr = inputType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_MixedInterlaceOrProgressive);
    if (FAILED(hr))
        return setError("SetUINT32(MF_MT_INTERLACE_MODE)", hr);

    hr = m_transform->SetInputType(0, inputType.Get(), 0);
    if (FAILED(hr))
        return setError("IMFTransform::SetInputType", hr);
    return true;
}

bool VideoDecoder::negotiateOutputType()
{
    for (DWORD i = 0;; ++i) {
        ComPtr<IMFMediaType> type;
        HRESULT hr = m_transform->GetOutputAvailableType(0, i, &type);
        if (hr == MF_E_NO_MORE_TYPES)
            return setError("GetOutputAvailableType", "decoder offers no NV12 output type");
        if (FAILED(hr))
            return setError("GetOutputAvailableType", hr);

        GUID subtype = {};
        if (FAILED(type->GetGUID(MF_MT_SUBTYPE, &subtype)) || subtype != MFVideoFormat_NV12)
            continue;

        hr = m_transform->SetOutputType(0, type.Get(), 0);
        if (FAILED(hr))
            return setError("IMFTransform::SetOutputType", hr);

        UINT32 width = 0, height = 0;
        hr = MFGetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, &width, &height);
        if (FAILED(hr))
            return setError("MFGetAttributeSize(MF_MT_FRAME_SIZE)", hr);

        m_codedWidth    = width;
        m_codedHeight   = height;
        m_displayWidth  = width;
        m_displayHeight = height;
        m_apertureX     = 0;
        m_apertureY     = 0;

        // 1080p H.264 is coded as 1920x1088; the aperture crops the 8
        // padding rows so they never reach the screen.
        MFVideoArea aperture = {};
        if (SUCCEEDED(type->GetBlob(MF_MT_MINIMUM_DISPLAY_APERTURE,
                                    reinterpret_cast<UINT8*>(&aperture),
                                    sizeof(aperture), nullptr)) &&
            aperture.Area.cx > 0 && aperture.Area.cy > 0) {
            m_apertureX     = aperture.OffsetX.value > 0
                                  ? static_cast<UINT>(aperture.OffsetX.value) : 0;
            m_apertureY     = aperture.OffsetY.value > 0
                                  ? static_cast<UINT>(aperture.OffsetY.value) : 0;
            m_displayWidth  = static_cast<UINT>(aperture.Area.cx);
            m_displayHeight = static_cast<UINT>(aperture.Area.cy);
        }

        hr = m_transform->GetOutputStreamInfo(0, &m_streamInfo);
        if (FAILED(hr))
            return setError("GetOutputStreamInfo", hr);
        m_providesSamples =
            (m_streamInfo.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES |
                                     MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0;

        // Old ring/staging textures are the previous resolution; rebuild
        // lazily on the next delivered frame.
        releaseOutputResources();
        return true;
    }
}

// ---------------------------------------------------------------------------
// Decode
// ---------------------------------------------------------------------------

void VideoDecoder::decode(const EncodedFrame& frame)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_initialized || !m_transform || frame.nalUnits.empty())
        return;

    // A decode error poisons the reference chain; throw the MFT's state
    // away at the next IDR, which is self-contained by definition.
    if (m_flushOnNextKeyframe && frame.isKeyframe) {
        HRESULT hrFlush = m_transform->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
        if (FAILED(hrFlush))
            setError("ProcessMessage(COMMAND_FLUSH)", hrFlush);
        m_transform->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
        m_flushOnNextKeyframe = false;
    }

    ComPtr<IMFMediaBuffer> buffer;
    HRESULT hr = MFCreateMemoryBuffer(static_cast<DWORD>(frame.nalUnits.size()), &buffer);
    if (FAILED(hr)) {
        setError("MFCreateMemoryBuffer", hr);
        return;
    }

    BYTE* dst = nullptr;
    DWORD maxLength = 0;
    hr = buffer->Lock(&dst, &maxLength, nullptr);
    if (FAILED(hr)) {
        setError("IMFMediaBuffer::Lock", hr);
        return;
    }
    memcpy(dst, frame.nalUnits.data(), frame.nalUnits.size());
    buffer->Unlock();
    hr = buffer->SetCurrentLength(static_cast<DWORD>(frame.nalUnits.size()));
    if (FAILED(hr)) {
        setError("IMFMediaBuffer::SetCurrentLength", hr);
        return;
    }

    ComPtr<IMFSample> sample;
    hr = MFCreateSample(&sample);
    if (FAILED(hr)) {
        setError("MFCreateSample", hr);
        return;
    }
    hr = sample->AddBuffer(buffer.Get());
    if (FAILED(hr)) {
        setError("IMFSample::AddBuffer", hr);
        return;
    }
    // MF sample time is in 100 ns units; the capture timestamp rides
    // through the decoder so presentation keeps the original clock.
    sample->SetSampleTime(static_cast<LONGLONG>(frame.timestampUs) * 10);
    if (frame.isKeyframe)
        sample->SetUINT32(MFSampleExtension_CleanPoint, TRUE);
    m_lastInputTsUs = frame.timestampUs;

    hr = m_transform->ProcessInput(0, sample.Get(), 0);
    if (hr == MF_E_NOTACCEPTING) {
        // The MFT is holding finished output; drain and feed again.
        drainOutputs();
        hr = m_transform->ProcessInput(0, sample.Get(), 0);
    }
    if (FAILED(hr)) {
        ++m_decodeFailures;
        if (m_decodeFailures <= 3 || m_decodeFailures % 300 == 0)
            setError("IMFTransform::ProcessInput", hr);
        m_flushOnNextKeyframe = true;
        return;
    }

    drainOutputs();
}

void VideoDecoder::drainOutputs()
{
    for (;;) {
        ComPtr<IMFSample> ourSample;
        MFT_OUTPUT_DATA_BUFFER output = {};
        output.dwStreamID = 0;

        if (!m_providesSamples) {
            if (!createSoftwareSample(ourSample))
                return; // setError already recorded
            output.pSample = ourSample.Get();
        }

        DWORD status = 0;
        HRESULT hr = m_transform->ProcessOutput(0, 1, &output, &status);
        if (output.pEvents) {
            output.pEvents->Release();
            output.pEvents = nullptr;
        }

        if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT)
            return;

        if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
            // Resolution (or aperture) changed. Renegotiate and try again;
            // the pending frame comes out at the new size.
            if (!negotiateOutputType()) {
                m_flushOnNextKeyframe = true;
                return;
            }
            LogF("[VideoDecoder] stream change: output now %ux%u (coded %ux%u)",
                 m_displayWidth, m_displayHeight, m_codedWidth, m_codedHeight);
            continue;
        }

        if (FAILED(hr)) {
            ++m_decodeFailures;
            if (m_decodeFailures <= 3 || m_decodeFailures % 300 == 0)
                setError("IMFTransform::ProcessOutput", hr);
            m_flushOnNextKeyframe = true;
            return;
        }

        IMFSample* result = output.pSample ? output.pSample : ourSample.Get();
        if (result)
            deliver(result);
        if (m_providesSamples && output.pSample)
            output.pSample->Release(); // MFT-allocated; ProcessOutput gave us a reference
    }
}

bool VideoDecoder::createSoftwareSample(ComPtr<IMFSample>& outSample)
{
    DWORD size = m_streamInfo.cbSize;
    const DWORD minSize = m_codedWidth * m_codedHeight * 3 / 2;
    if (size < minSize)
        size = minSize;
    if (size == 0)
        size = 4096; // type not negotiated yet; the stream change will fix sizes

    ComPtr<IMFMediaBuffer> buffer;
    const DWORD alignment = m_streamInfo.cbAlignment > 1 ? m_streamInfo.cbAlignment - 1 : 0;
    HRESULT hr = MFCreateAlignedMemoryBuffer(size, alignment, &buffer);
    if (FAILED(hr))
        return setError("MFCreateAlignedMemoryBuffer", hr);

    hr = MFCreateSample(&outSample);
    if (FAILED(hr))
        return setError("MFCreateSample(output)", hr);
    hr = outSample->AddBuffer(buffer.Get());
    if (FAILED(hr))
        return setError("IMFSample::AddBuffer(output)", hr);
    return true;
}

// ---------------------------------------------------------------------------
// Delivery
// ---------------------------------------------------------------------------

bool VideoDecoder::ensureOutputRing()
{
    if (m_outputRing[0]) {
        D3D11_TEXTURE2D_DESC desc;
        m_outputRing[0]->GetDesc(&desc);
        if (desc.Width == m_displayWidth && desc.Height == m_displayHeight)
            return true;
        releaseOutputResources();
    }

    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width            = m_displayWidth;
    desc.Height           = m_displayHeight;
    desc.MipLevels        = 1;
    desc.ArraySize        = 1;
    desc.Format           = DXGI_FORMAT_NV12;
    desc.SampleDesc.Count = 1;
    desc.Usage            = D3D11_USAGE_DEFAULT;

    // These textures are fed to RenderWindow's ID3D11VideoProcessor as input
    // views. Several drivers (notably Intel iGPUs) reject
    // CreateVideoProcessorInputView with E_INVALIDARG unless the resource was
    // created with a video-capable bind flag, so try the most capable
    // combination first and degrade until the driver accepts one.
    const UINT bindCandidates[] = {
        D3D11_BIND_DECODER | D3D11_BIND_SHADER_RESOURCE,
        D3D11_BIND_DECODER,
        D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE,
        D3D11_BIND_SHADER_RESOURCE,
    };

    HRESULT hr = E_FAIL;
    for (UINT bind : bindCandidates) {
        desc.BindFlags = bind;
        hr = S_OK;
        for (auto& tex : m_outputRing) {
            hr = m_device->CreateTexture2D(&desc, nullptr, &tex);
            if (FAILED(hr))
                break;
        }
        if (SUCCEEDED(hr)) {
            LogF("[VideoDecoder] output ring bind flags: 0x%X", bind);
            break;
        }
        releaseOutputResources();
    }
    if (FAILED(hr))
        return setError("CreateTexture2D(output ring)", hr);

    m_ringIndex = 0;
    return true;
}

bool VideoDecoder::ensureStaging()
{
    if (m_staging) {
        D3D11_TEXTURE2D_DESC desc;
        m_staging->GetDesc(&desc);
        if (desc.Width == m_displayWidth && desc.Height == m_displayHeight)
            return true;
        m_staging.Reset();
    }

    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width            = m_displayWidth;
    desc.Height           = m_displayHeight;
    desc.MipLevels        = 1;
    desc.ArraySize        = 1;
    desc.Format           = DXGI_FORMAT_NV12;
    desc.SampleDesc.Count = 1;
    desc.Usage            = D3D11_USAGE_STAGING;
    desc.CPUAccessFlags   = D3D11_CPU_ACCESS_WRITE;

    HRESULT hr = m_device->CreateTexture2D(&desc, nullptr, &m_staging);
    if (FAILED(hr))
        return setError("CreateTexture2D(staging)", hr);
    return true;
}

bool VideoDecoder::deliverHardware(IMFMediaBuffer* buffer, ID3D11Texture2D** outTexture)
{
    ComPtr<IMFDXGIBuffer> dxgiBuffer;
    HRESULT hr = buffer->QueryInterface(IID_PPV_ARGS(&dxgiBuffer));
    if (FAILED(hr))
        return setError("deliverHardware", "output buffer is not a DXGI buffer");

    ComPtr<ID3D11Texture2D> source;
    hr = dxgiBuffer->GetResource(IID_PPV_ARGS(&source));
    if (FAILED(hr))
        return setError("IMFDXGIBuffer::GetResource", hr);
    UINT subresource = 0;
    hr = dxgiBuffer->GetSubresourceIndex(&subresource);
    if (FAILED(hr))
        return setError("IMFDXGIBuffer::GetSubresourceIndex", hr);

    if (!ensureOutputRing())
        return false;

    ID3D11Texture2D* dst = m_outputRing[m_ringIndex].Get();
    m_ringIndex = (m_ringIndex + 1) % kOutputRingSize;

    // One GPU-to-GPU copy out of the MFT's recycled allocator slice,
    // cropped to the display aperture. No CPU readback.
    D3D11_BOX box = {};
    box.left   = m_apertureX;
    box.top    = m_apertureY;
    box.front  = 0;
    box.right  = m_apertureX + m_displayWidth;
    box.bottom = m_apertureY + m_displayHeight;
    box.back   = 1;
    m_context->CopySubresourceRegion(dst, 0, 0, 0, 0, source.Get(), subresource, &box);

    *outTexture = dst;
    return true;
}

bool VideoDecoder::deliverSoftware(IMFMediaBuffer* buffer, ID3D11Texture2D** outTexture)
{
    if (!ensureOutputRing() || !ensureStaging())
        return false;

    // Prefer the 2D buffer interface (real pitch); fall back to the flat
    // buffer with the assumption pitch == coded width.
    BYTE* scan0 = nullptr;
    LONG  pitch = 0;
    bool  locked2d = false;
    ComPtr<IMF2DBuffer> buffer2d;
    if (SUCCEEDED(buffer->QueryInterface(IID_PPV_ARGS(&buffer2d))) &&
        SUCCEEDED(buffer2d->Lock2D(&scan0, &pitch))) {
        locked2d = true;
    } else {
        buffer2d.Reset();
        DWORD length = 0;
        HRESULT hr = buffer->Lock(&scan0, nullptr, &length);
        if (FAILED(hr))
            return setError("IMFMediaBuffer::Lock(output)", hr);
        pitch = static_cast<LONG>(m_codedWidth);
    }

    auto unlock = [&]() {
        if (locked2d)
            buffer2d->Unlock2D();
        else
            buffer->Unlock();
    };

    if (pitch <= 0) {
        unlock();
        return setError("deliverSoftware", "bottom-up NV12 output is not supported");
    }

    D3D11_MAPPED_SUBRESOURCE mapped = {};
    HRESULT hr = m_context->Map(m_staging.Get(), 0, D3D11_MAP_WRITE, 0, &mapped);
    if (FAILED(hr)) {
        unlock();
        return setError("Map(staging)", hr);
    }

    const size_t srcPitch = static_cast<size_t>(pitch);
    BYTE* dstY  = static_cast<BYTE*>(mapped.pData);
    BYTE* dstUV = dstY + static_cast<size_t>(mapped.RowPitch) * m_displayHeight;
    const BYTE* srcY  = scan0 + static_cast<size_t>(m_apertureY) * srcPitch + m_apertureX;
    const BYTE* srcUV = scan0 + srcPitch * m_codedHeight +
                        static_cast<size_t>(m_apertureY / 2) * srcPitch + m_apertureX;

    for (UINT y = 0; y < m_displayHeight; ++y)
        memcpy(dstY + static_cast<size_t>(y) * mapped.RowPitch,
               srcY + static_cast<size_t>(y) * srcPitch, m_displayWidth);
    for (UINT y = 0; y < m_displayHeight / 2; ++y)
        memcpy(dstUV + static_cast<size_t>(y) * mapped.RowPitch,
               srcUV + static_cast<size_t>(y) * srcPitch, m_displayWidth);

    m_context->Unmap(m_staging.Get(), 0);
    unlock();

    ID3D11Texture2D* dst = m_outputRing[m_ringIndex].Get();
    m_ringIndex = (m_ringIndex + 1) % kOutputRingSize;
    m_context->CopyResource(dst, m_staging.Get());

    *outTexture = dst;
    return true;
}

void VideoDecoder::deliver(IMFSample* sample)
{
    ComPtr<IMFMediaBuffer> buffer;
    HRESULT hr = sample->GetBufferByIndex(0, &buffer);
    if (FAILED(hr)) {
        setError("IMFSample::GetBufferByIndex", hr);
        return;
    }

    // A DXGI-backed buffer means the sample lives in GPU memory (D3D11VA
    // path); anything else is a system-memory sample from software decode.
    ComPtr<IMFDXGIBuffer> probe;
    const bool gpuSample = SUCCEEDED(buffer->QueryInterface(IID_PPV_ARGS(&probe)));
    probe.Reset();

    ID3D11Texture2D* texture = nullptr;
    const bool ok = gpuSample ? deliverHardware(buffer.Get(), &texture)
                              : deliverSoftware(buffer.Get(), &texture);
    if (!ok || !texture)
        return; // setError recorded the cause

    uint64_t tsUs = m_lastInputTsUs;
    LONGLONG sampleTime = 0;
    if (SUCCEEDED(sample->GetSampleTime(&sampleTime)) && sampleTime >= 0)
        tsUs = static_cast<uint64_t>(sampleTime) / 10;

    m_decodeFailures = 0;
    if (onDecodedFrame)
        onDecodedFrame(texture, tsUs);
}
