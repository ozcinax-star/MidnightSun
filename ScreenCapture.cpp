// ScreenCapture.cpp -- see ScreenCapture.h for the design overview.

#include "ScreenCapture.h"

#include <windows.h>
#include <d3d11_4.h>   // ID3D11Multithread

#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

namespace {

// ---------------------------------------------------------------------------
// Logging. Goes to the debugger and stderr; swap the body for your app's
// logger. Kept free of any allocation beyond the stack buffer so it is safe
// to call from the capture thread's hot path (though we only log on errors
// and state changes, never per-frame).
// ---------------------------------------------------------------------------
void LogF(const char* fmt, ...)
{
    char buf[1024];
    va_list args;
    va_start(args, fmt);
    // Cap at sizeof-3 so there is always room to append "\n\0" below,
    // even when the message is truncated.
    int n = _vsnprintf_s(buf, sizeof(buf), sizeof(buf) - 3, fmt, args);
    va_end(args);
    if (n < 0) n = static_cast<int>(strlen(buf)); // truncated; buffer is still null-terminated
    // Ensure a trailing newline for both sinks.
    if (n == 0 || buf[n - 1] != '\n') {
        buf[n] = '\n';
        buf[n + 1] = '\0';
    }
    OutputDebugStringA(buf);
    fputs(buf, stderr);
}

std::string HrToString(HRESULT hr)
{
    char sysMsg[512] = {};
    DWORD len = FormatMessageA(
        FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, static_cast<DWORD>(hr),
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        sysMsg, static_cast<DWORD>(sizeof(sysMsg)), nullptr);
    // Strip FormatMessage's trailing CR/LF.
    while (len > 0 && (sysMsg[len - 1] == '\r' || sysMsg[len - 1] == '\n'))
        sysMsg[--len] = '\0';

    char buf[600];
    if (len > 0)
        sprintf_s(buf, "0x%08X (%s)", static_cast<unsigned>(hr), sysMsg);
    else
        sprintf_s(buf, "0x%08X", static_cast<unsigned>(hr));
    return buf;
}

void LogHr(const char* what, HRESULT hr)
{
    LogF("[ScreenCapture] %s failed: %s", what, HrToString(hr).c_str());
}

// Backoff used whenever the duplication cannot be (re)created: monitor
// asleep, secure desktop (UAC prompt / lock screen / Ctrl+Alt+Del), mode
// change in progress, or another app holding the duplication. 250ms keeps
// reconnection snappy without spinning the CPU while the screen is off.
constexpr DWORD kReinitBackoffMs = 250;

// AcquireNextFrame timeout MUST be zero: DXGI holds the D3D11 device's
// internal lock while it blocks waiting for the next desktop frame, and
// NVENC's D3D11 interop calls (map/submit/lock-bitstream) on the encode
// thread serialize behind that lock. A blocking timeout here therefore
// stalls every encode by up to one source-frame interval (measured: encode
// times quantized at 40 ms multiples streaming 25 fps content). Poll with
// timeout 0 and sleep OUTSIDE the call instead.
constexpr UINT kAcquireTimeoutMs = 0;

// Sleep between empty polls (WAIT_TIMEOUT). 1 ms keeps added capture
// latency negligible without spinning a core; the process runs under
// timeBeginPeriod(1).
constexpr DWORD kIdlePollMs = 1;

} // namespace

// ===========================================================================
// Public API
// ===========================================================================

ScreenCapture::ScreenCapture() = default;

ScreenCapture::~ScreenCapture()
{
    stopCapture();
}

bool ScreenCapture::startCapture()
{
    if (m_running.load(std::memory_order_acquire)) {
        LogF("[ScreenCapture] startCapture called while already running");
        return true;
    }

    // Device creation happens synchronously so the caller gets a meaningful
    // failure for "this machine cannot capture at all". Duplication setup is
    // deliberately left to the capture thread because its failures are
    // usually transient and must be retried anyway.
    if (!initializeDevice()) {
        return false;
    }

    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq); // cannot fail on XP+
    m_qpcFrequency = freq.QuadPart;

    m_running.store(true, std::memory_order_release);
    m_captureThread = std::thread(&ScreenCapture::captureThreadProc, this);
    return true;
}

void ScreenCapture::stopCapture()
{
    m_running.store(false, std::memory_order_release);
    if (m_captureThread.joinable())
        m_captureThread.join();

    releaseDuplication();

    {
        std::lock_guard<std::mutex> lock(m_frameMutex);
        for (auto& slot : m_slots)
            slot.texture.Reset();
        m_consumerHeldTexture.Reset();
        m_hasFrame = false;
        m_hasNewFrame = false;
        m_frameWidth = 0;
        m_frameHeight = 0;
    }

    releaseDevice();
}

bool ScreenCapture::getLatestFrame(CaptureFrame& outFrame)
{
    std::lock_guard<std::mutex> lock(m_frameMutex);

    if (!m_hasFrame)
        return false;

    if (m_hasNewFrame) {
        // Take ownership of the newest completed slot and give our old read
        // slot back to the producer side. After this swap the capture thread
        // may copy into the slot we just released -- which is exactly why
        // the previous frame's texture pointer is documented as invalid from
        // this point on.
        std::swap(m_readIndex, m_latestIndex);
        m_hasNewFrame = false;
    }

    BufferSlot& slot = m_slots[m_readIndex];
    if (!slot.texture) // ring was rebuilt mid-mode-change; nothing valid yet
        return false;

    // Pin the texture so a resolution-change rebuild on the capture thread
    // can't destroy it while the consumer is still using it.
    m_consumerHeldTexture = slot.texture;

    outFrame.texture      = slot.texture.Get();
    outFrame.timestamp_us = slot.timestampUs;
    outFrame.width        = m_frameWidth;
    outFrame.height       = m_frameHeight;
    return true;
}

// ===========================================================================
// Device / duplication setup
// ===========================================================================

bool ScreenCapture::initializeDevice()
{
    // Find the primary output first so the device is created on the adapter
    // that actually drives that monitor. Desktop duplication only works when
    // device and output share an adapter, which matters on multi-GPU
    // machines (laptops with hybrid graphics, multi-card desktops).
    ComPtr<IDXGIFactory1> factory;
    HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    if (FAILED(hr)) {
        LogHr("CreateDXGIFactory1", hr);
        return false;
    }

    ComPtr<IDXGIAdapter1> primaryAdapter;
    ComPtr<IDXGIOutput1>  primaryOutput;

    for (UINT a = 0; ; ++a) {
        ComPtr<IDXGIAdapter1> adapter;
        if (factory->EnumAdapters1(a, &adapter) == DXGI_ERROR_NOT_FOUND)
            break;

        for (UINT o = 0; ; ++o) {
            ComPtr<IDXGIOutput> output;
            if (adapter->EnumOutputs(o, &output) == DXGI_ERROR_NOT_FOUND)
                break;

            DXGI_OUTPUT_DESC desc;
            if (FAILED(output->GetDesc(&desc)) || !desc.AttachedToDesktop)
                continue;

            // The primary monitor is identified via GetMonitorInfo rather
            // than assuming DesktopCoordinates starts at (0,0) -- that
            // happens to be true today but is an implementation detail.
            MONITORINFO mi = { sizeof(mi) };
            if (GetMonitorInfoW(desc.Monitor, &mi) && (mi.dwFlags & MONITORINFOF_PRIMARY)) {
                hr = output.As(&primaryOutput);
                if (FAILED(hr)) {
                    // IDXGIOutput1 is required for DuplicateOutput; absent
                    // only on pre-Win8 systems, which we do not support.
                    LogHr("IDXGIOutput -> IDXGIOutput1 (Desktop Duplication unsupported on this OS)", hr);
                    return false;
                }
                primaryAdapter = adapter;
                break;
            }
        }
        if (primaryOutput)
            break;
    }

    if (!primaryOutput) {
        LogF("[ScreenCapture] no primary monitor found (headless session?)");
        return false;
    }

    UINT createFlags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#if defined(_DEBUG)
    createFlags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

    // 11.0 is the floor: desktop duplication itself only needs feature
    // level 9.1, but a streaming pipeline (compute-shader color conversion,
    // hardware encoders) realistically needs 11.0+ anyway.
    const D3D_FEATURE_LEVEL levels[] = {
        D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
    };

    hr = D3D11CreateDevice(
        primaryAdapter.Get(),
        D3D_DRIVER_TYPE_UNKNOWN, // mandatory when an explicit adapter is given
        nullptr, createFlags,
        levels, ARRAYSIZE(levels),
        D3D11_SDK_VERSION,
        &m_device, nullptr, &m_context);

#if defined(_DEBUG)
    if (hr == DXGI_ERROR_SDK_COMPONENT_MISSING) {
        // Debug layer not installed on this machine; retry without it
        // rather than failing a debug build outright.
        createFlags &= ~D3D11_CREATE_DEVICE_DEBUG;
        hr = D3D11CreateDevice(primaryAdapter.Get(), D3D_DRIVER_TYPE_UNKNOWN,
                               nullptr, createFlags, levels, ARRAYSIZE(levels),
                               D3D11_SDK_VERSION, &m_device, nullptr, &m_context);
    }
#endif

    if (FAILED(hr)) {
        LogHr("D3D11CreateDevice", hr);
        return false;
    }

    // The capture thread uses the immediate context for CopyResource while
    // the consumer may submit work on the same device from its own thread
    // (SRV creation, encoder submission). Multithread protection makes the
    // runtime serialize context access internally; the cost is a critical
    // section per call, negligible at one copy per frame.
    ComPtr<ID3D11Multithread> mt;
    if (SUCCEEDED(m_context.As(&mt)))
        mt->SetMultithreadProtected(TRUE);

    m_output = primaryOutput;

    DXGI_OUTPUT_DESC outDesc;
    if (SUCCEEDED(m_output->GetDesc(&outDesc))) {
        LogF("[ScreenCapture] capturing primary output, desktop rect %ldx%ld",
             outDesc.DesktopCoordinates.right - outDesc.DesktopCoordinates.left,
             outDesc.DesktopCoordinates.bottom - outDesc.DesktopCoordinates.top);
    }
    return true;
}

bool ScreenCapture::initializeDuplication()
{
    HRESULT hr = m_output->DuplicateOutput(m_device.Get(), &m_duplication);
    if (FAILED(hr)) {
        // Expected transient cases, retried by the caller with backoff:
        //   E_ACCESSDENIED                      - secure desktop (UAC, lock screen)
        //   DXGI_ERROR_NOT_CURRENTLY_AVAILABLE  - too many concurrent duplications
        //   DXGI_ERROR_SESSION_DISCONNECTED     - RDP/session switch
        //   DXGI_ERROR_ACCESS_LOST              - mode change racing us
        // DXGI_ERROR_UNSUPPORTED is persistent (e.g. some driver/IGP combos)
        // but retrying is still harmless, so all failures take the same path.
        LogHr("IDXGIOutput1::DuplicateOutput", hr);
        return false;
    }

    DXGI_OUTDUPL_DESC duplDesc;
    m_duplication->GetDesc(&duplDesc);

    if (duplDesc.Rotation != DXGI_MODE_ROTATION_IDENTITY &&
        duplDesc.Rotation != DXGI_MODE_ROTATION_UNSPECIFIED) {
        // The duplication surface is delivered unrotated; a rotated monitor
        // would need a rotation pass (shader) before encoding. Out of scope
        // here -- surface it loudly instead of streaming sideways video.
        LogF("[ScreenCapture] WARNING: primary monitor is rotated (%d); frames are delivered unrotated",
             static_cast<int>(duplDesc.Rotation));
    }

    if (duplDesc.DesktopImageInSystemMemory) {
        // Old iGPU path where the desktop image lives in system memory.
        // CopyResource still works (the runtime uploads), just slower.
        LogF("[ScreenCapture] note: desktop image is in system memory on this adapter");
    }

    if (!createFrameBuffers(duplDesc.ModeDesc.Width,
                            duplDesc.ModeDesc.Height,
                            duplDesc.ModeDesc.Format)) {
        releaseDuplication();
        return false;
    }

    LogF("[ScreenCapture] duplication ready: %ux%u, format %d",
         duplDesc.ModeDesc.Width, duplDesc.ModeDesc.Height,
         static_cast<int>(duplDesc.ModeDesc.Format));
    return true;
}

bool ScreenCapture::createFrameBuffers(UINT width, UINT height, DXGI_FORMAT format)
{
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width            = width;
    desc.Height           = height;
    desc.MipLevels        = 1;
    desc.ArraySize        = 1;
    desc.Format           = format; // desktop format, B8G8R8A8_UNORM in practice
    desc.SampleDesc.Count = 1;
    desc.Usage            = D3D11_USAGE_DEFAULT; // GPU-only, as required
    // SHADER_RESOURCE so the consumer can sample/convert (BGRA -> NV12 for
    // the encoder); RENDER_TARGET so it can also be a blit/compose target.
    // To share frames with an encoder on a *different* D3D device, add
    // D3D11_RESOURCE_MISC_SHARED_NTHANDLE | _SHARED_KEYEDMUTEX here and
    // fence the handoff -- same-device pipelines (the common case) need
    // neither.
    desc.BindFlags        = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;

    BufferSlot newSlots[kNumBuffers];
    for (int i = 0; i < kNumBuffers; ++i) {
        HRESULT hr = m_device->CreateTexture2D(&desc, nullptr, &newSlots[i].texture);
        if (FAILED(hr)) {
            LogHr("CreateTexture2D (frame ring buffer)", hr);
            return false;
        }
    }

    // Swap the whole ring in under the lock. Old textures die here except
    // the one pinned by m_consumerHeldTexture, which survives until the
    // consumer's next getLatestFrame() -- that is what makes a resolution
    // change safe while the consumer is mid-frame.
    std::lock_guard<std::mutex> lock(m_frameMutex);
    for (int i = 0; i < kNumBuffers; ++i)
        m_slots[i] = std::move(newSlots[i]);
    m_writeIndex  = 0;
    m_latestIndex = 1;
    m_readIndex   = 2;
    m_hasFrame    = false; // frames from the old mode are gone
    m_hasNewFrame = false;
    m_frameWidth  = static_cast<int>(width);
    m_frameHeight = static_cast<int>(height);
    return true;
}

void ScreenCapture::releaseDuplication()
{
    if (m_duplication) {
        // Harmless if no frame is held; required if we bail between
        // AcquireNextFrame and ReleaseFrame.
        m_duplication->ReleaseFrame();
        m_duplication.Reset();
    }
}

void ScreenCapture::releaseDevice()
{
    m_output.Reset();
    if (m_context) {
        m_context->ClearState();
        m_context->Flush();
    }
    m_context.Reset();
    m_device.Reset();
}

// ===========================================================================
// Capture thread
// ===========================================================================

UINT64 ScreenCapture::qpcToMicroseconds(LONGLONG qpc) const
{
    // Split the conversion to avoid 64-bit overflow: qpc * 1'000'000
    // overflows after ~10 days of uptime with a 10MHz QPC.
    const LONGLONG whole = (qpc / m_qpcFrequency) * 1'000'000;
    const LONGLONG part  = (qpc % m_qpcFrequency) * 1'000'000 / m_qpcFrequency;
    return static_cast<UINT64>(whole + part);
}

void ScreenCapture::publishFrame(UINT64 timestampUs)
{
    {
        std::lock_guard<std::mutex> lock(m_frameMutex);
        m_slots[m_writeIndex].timestampUs = timestampUs;
        // The just-written slot becomes "latest"; the old latest (which the
        // consumer never saw if m_hasNewFrame was still set -- i.e. the consumer
        // is slower than the capture rate) is recycled as the next write target.
        // This is how "no frame drops" is achieved on the producer side: the
        // capture thread always has a free buffer and never waits.
        std::swap(m_writeIndex, m_latestIndex);
        m_hasFrame = true;
        m_hasNewFrame = true;
    }
    m_frameCv.notify_one(); // wake a consumer blocked in waitForNewFrame()
}

bool ScreenCapture::waitForNewFrame(uint32_t timeoutMs)
{
    std::unique_lock<std::mutex> lock(m_frameMutex);
    if (m_hasNewFrame)
        return true;
    m_frameCv.wait_for(lock, std::chrono::milliseconds(timeoutMs),
                       [this] { return m_hasNewFrame; });
    return m_hasNewFrame;
}

// ===========================================================================
// Cursor compositing
// ===========================================================================

void ScreenCapture::updateCursor(const DXGI_OUTDUPL_FRAME_INFO& frameInfo)
{
    // Position/visibility ride along on every frame that has mouse news.
    if (frameInfo.LastMouseUpdateTime.QuadPart != 0) {
        m_cursorVisible = frameInfo.PointerPosition.Visible != FALSE;
        if (m_cursorVisible) {
            m_cursorX = frameInfo.PointerPosition.Position.x;
            m_cursorY = frameInfo.PointerPosition.Position.y;
        }
    }

    // A new shape is only delivered when it changes (and on the first frame
    // after duplication starts while the cursor is on this output).
    if (frameInfo.PointerShapeBufferSize > 0) {
        m_cursorShape.resize(frameInfo.PointerShapeBufferSize);
        UINT required = 0;
        const HRESULT hr = m_duplication->GetFramePointerShape(
            frameInfo.PointerShapeBufferSize, m_cursorShape.data(),
            &required, &m_cursorShapeInfo);
        if (FAILED(hr)) {
            LogHr("IDXGIOutputDuplication::GetFramePointerShape", hr);
            m_cursorShape.clear();
        }
    }
}

void ScreenCapture::compositeCursor(ID3D11Texture2D* target)
{
    if (!m_cursorVisible || m_cursorShape.empty() || !target)
        return;

    // Monochrome shapes stack the AND mask above the XOR mask, so the
    // visible cursor is only half the reported height.
    const bool mono =
        m_cursorShapeInfo.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME;
    const UINT shapeW = m_cursorShapeInfo.Width;
    const UINT shapeH = mono ? m_cursorShapeInfo.Height / 2
                             : m_cursorShapeInfo.Height;
    if (shapeW == 0 || shapeH == 0)
        return;

    D3D11_TEXTURE2D_DESC targetDesc = {};
    target->GetDesc(&targetDesc);
    if (targetDesc.Format != DXGI_FORMAT_B8G8R8A8_UNORM)
        return; // the blend below assumes BGRA bytes

    // Cursor rectangle clipped to the frame.
    const int x = m_cursorX, y = m_cursorY;
    const int left   = x > 0 ? x : 0;
    const int top    = y > 0 ? y : 0;
    int right  = x + static_cast<int>(shapeW);
    int bottom = y + static_cast<int>(shapeH);
    if (right  > static_cast<int>(targetDesc.Width))
        right  = static_cast<int>(targetDesc.Width);
    if (bottom > static_cast<int>(targetDesc.Height))
        bottom = static_cast<int>(targetDesc.Height);
    if (left >= right || top >= bottom)
        return; // entirely off-screen

    const UINT patchW  = static_cast<UINT>(right - left);
    const UINT patchH  = static_cast<UINT>(bottom - top);
    const UINT srcOffX = static_cast<UINT>(left - x); // clipped-off shape part
    const UINT srcOffY = static_cast<UINT>(top - y);

    // Staging texture big enough for the patch (grows on demand; cursor
    // shapes are 32..256 px, so this allocates once in practice).
    if (!m_cursorStaging || m_cursorStagingW < patchW || m_cursorStagingH < patchH) {
        m_cursorStaging.Reset();
        D3D11_TEXTURE2D_DESC sd = {};
        sd.Width            = patchW > 64 ? patchW : 64;
        sd.Height           = patchH > 64 ? patchH : 64;
        sd.MipLevels        = 1;
        sd.ArraySize        = 1;
        sd.Format           = targetDesc.Format;
        sd.SampleDesc.Count = 1;
        sd.Usage            = D3D11_USAGE_STAGING;
        sd.CPUAccessFlags   = D3D11_CPU_ACCESS_READ | D3D11_CPU_ACCESS_WRITE;
        const HRESULT hr = m_device->CreateTexture2D(&sd, nullptr, &m_cursorStaging);
        if (FAILED(hr)) {
            LogHr("CreateTexture2D(cursor staging)", hr);
            return;
        }
        m_cursorStagingW = sd.Width;
        m_cursorStagingH = sd.Height;
    }

    // GPU -> staging: only the pixels under the cursor.
    D3D11_BOX srcBox = {};
    srcBox.left = static_cast<UINT>(left);  srcBox.right  = static_cast<UINT>(right);
    srcBox.top  = static_cast<UINT>(top);   srcBox.bottom = static_cast<UINT>(bottom);
    srcBox.front = 0; srcBox.back = 1;
    m_context->CopySubresourceRegion(m_cursorStaging.Get(), 0, 0, 0, 0,
                                     target, 0, &srcBox);

    D3D11_MAPPED_SUBRESOURCE mapped = {};
    HRESULT hr = m_context->Map(m_cursorStaging.Get(), 0, D3D11_MAP_READ_WRITE, 0, &mapped);
    if (FAILED(hr)) {
        LogHr("Map(cursor staging)", hr);
        return;
    }

    const uint8_t* shape      = m_cursorShape.data();
    const UINT     shapePitch = m_cursorShapeInfo.Pitch;

    for (UINT row = 0; row < patchH; ++row) {
        uint8_t*   dstRow = static_cast<uint8_t*>(mapped.pData) + row * mapped.RowPitch;
        const UINT sy     = srcOffY + row;

        switch (m_cursorShapeInfo.Type) {
        case DXGI_OUTDUPL_POINTER_SHAPE_TYPE_COLOR: {
            // 32bpp BGRA with straight alpha.
            const uint8_t* srcRow = shape + sy * shapePitch;
            for (UINT col = 0; col < patchW; ++col) {
                const uint8_t* s = srcRow + (srcOffX + col) * 4;
                const uint32_t a = s[3];
                if (a == 0)
                    continue;
                uint8_t* d = dstRow + col * 4;
                d[0] = static_cast<uint8_t>((s[0] * a + d[0] * (255 - a) + 127) / 255);
                d[1] = static_cast<uint8_t>((s[1] * a + d[1] * (255 - a) + 127) / 255);
                d[2] = static_cast<uint8_t>((s[2] * a + d[2] * (255 - a) + 127) / 255);
            }
            break;
        }
        case DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MASKED_COLOR: {
            // 32bpp; mask byte 0xFF = XOR the RGB with the screen pixel,
            // 0 = replace the screen pixel with the RGB value.
            const uint8_t* srcRow = shape + sy * shapePitch;
            for (UINT col = 0; col < patchW; ++col) {
                const uint8_t* s = srcRow + (srcOffX + col) * 4;
                uint8_t* d = dstRow + col * 4;
                if (s[3] == 0xFF) {
                    d[0] ^= s[0]; d[1] ^= s[1]; d[2] ^= s[2];
                } else {
                    d[0] = s[0]; d[1] = s[1]; d[2] = s[2];
                }
            }
            break;
        }
        case DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME: {
            // 1bpp AND mask (rows 0..H-1) over 1bpp XOR mask (rows H..2H-1):
            // dst = (dst AND and) XOR xor, expanded to all color channels.
            const uint8_t* andRow = shape + sy * shapePitch;
            const uint8_t* xorRow = shape + (sy + shapeH) * shapePitch;
            for (UINT col = 0; col < patchW; ++col) {
                const UINT    sx     = srcOffX + col;
                const uint8_t bit    = static_cast<uint8_t>(0x80u >> (sx & 7));
                const uint8_t andVal = (andRow[sx / 8] & bit) ? 0xFF : 0x00;
                const uint8_t xorVal = (xorRow[sx / 8] & bit) ? 0xFF : 0x00;
                uint8_t* d = dstRow + col * 4;
                d[0] = static_cast<uint8_t>((d[0] & andVal) ^ xorVal);
                d[1] = static_cast<uint8_t>((d[1] & andVal) ^ xorVal);
                d[2] = static_cast<uint8_t>((d[2] & andVal) ^ xorVal);
            }
            break;
        }
        default:
            break;
        }
    }

    m_context->Unmap(m_cursorStaging.Get(), 0);

    // Staging -> GPU: write the blended patch back.
    D3D11_BOX patchBox = {};
    patchBox.left = 0; patchBox.right  = patchW;
    patchBox.top  = 0; patchBox.bottom = patchH;
    patchBox.front = 0; patchBox.back = 1;
    m_context->CopySubresourceRegion(target, 0,
                                     static_cast<UINT>(left),
                                     static_cast<UINT>(top), 0,
                                     m_cursorStaging.Get(), 0, &patchBox);
}

void ScreenCapture::captureThreadProc()
{
    SetThreadDescription(GetCurrentThread(), L"ScreenCapture");
    // Above-normal, not TIME_CRITICAL: we must win against render threads
    // for low latency, but a starved system would cause worse stutter than
    // a slightly late capture.
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);

    while (m_running.load(std::memory_order_acquire)) {

        // ---- (re)establish duplication ---------------------------------
        if (!m_duplication) {
            if (!m_device) {
                // Device was lost (GPU reset / driver update); rebuild from
                // scratch. The primary monitor may also have changed.
                if (!initializeDevice()) {
                    Sleep(kReinitBackoffMs);
                    continue;
                }
            }
            if (!initializeDuplication()) {
                Sleep(kReinitBackoffMs);
                continue;
            }
        }

        // ---- acquire one desktop frame ----------------------------------
        DXGI_OUTDUPL_FRAME_INFO frameInfo = {};
        ComPtr<IDXGIResource>   desktopResource;
        HRESULT hr = m_duplication->AcquireNextFrame(kAcquireTimeoutMs,
                                                     &frameInfo, &desktopResource);

        if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
            // Nothing changed on screen -- a static desktop presents no new
            // frames. Not an error. Sleep outside AcquireNextFrame so the
            // device lock stays free for the encode thread, then poll again.
            Sleep(kIdlePollMs);
            continue;
        }

        if (hr == DXGI_ERROR_ACCESS_LOST) {
            // Resolution change, fullscreen exclusive transition, desktop
            // switch, or monitor power-off. Recreate the duplication (and
            // the ring, since dimensions may differ) on the next iteration.
            LogF("[ScreenCapture] duplication access lost (mode change / desktop switch); reinitializing");
            releaseDuplication();
            continue;
        }

        if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
            HRESULT reason = m_device ? m_device->GetDeviceRemovedReason() : hr;
            LogF("[ScreenCapture] device lost: %s; recreating device", HrToString(reason).c_str());
            releaseDuplication();
            releaseDevice();
            continue;
        }

        if (FAILED(hr)) {
            // Unexpected failure: log it, tear down the duplication and let
            // the reinit path sort it out rather than spinning on the error.
            LogHr("IDXGIOutputDuplication::AcquireNextFrame", hr);
            releaseDuplication();
            Sleep(kReinitBackoffMs);
            continue;
        }

        // ---- copy & publish ---------------------------------------------
        // LastPresentTime == 0 means this "frame" carries only mouse-pointer
        // updates, not new desktop pixels (the duplication API does not
        // composite the cursor into the image). Pointer-only updates still
        // produce a frame here: the desktop pixels are unchanged but the
        // cursor we composite has moved, and the client must see that.
        updateCursor(frameInfo); // must run before ReleaseFrame

        const bool desktopChanged = frameInfo.LastPresentTime.QuadPart != 0;
        const bool cursorMoved    = frameInfo.LastMouseUpdateTime.QuadPart != 0;
        if (desktopChanged || cursorMoved) {
            ComPtr<ID3D11Texture2D> desktopTexture;
            hr = desktopResource.As(&desktopTexture);
            if (SUCCEEDED(hr)) {
                // Full-texture GPU->GPU copy into our ring. ~0.1-0.3ms of
                // GPU time at 4K and near-zero CPU time, comfortably inside
                // the 2ms budget. CopyResource is legal here because the
                // ring textures match the duplication surface exactly
                // (same size, format, sample count) -- guaranteed because
                // createFrameBuffers() was fed from the duplication desc.
                // Reading m_writeIndex without the lock is safe: only this
                // thread ever modifies it (publishFrame / createFrameBuffers
                // both run on the capture thread).
                m_context->CopyResource(m_slots[m_writeIndex].texture.Get(),
                                        desktopTexture.Get());
            } else {
                LogHr("IDXGIResource -> ID3D11Texture2D", hr);
            }

            // ReleaseFrame must happen promptly: while a frame is held, DWM
            // cannot hand us the next one, and holding it long can stall
            // composition. The copy above was already queued on the context,
            // and queued commands keep their source resource referenced, so
            // releasing here is safe and is the documented usage pattern.
            m_duplication->ReleaseFrame();

            if (SUCCEEDED(hr)) {
                // Blend the cached cursor into our copy (never into the
                // shared duplication surface).
                compositeCursor(m_slots[m_writeIndex].texture.Get());

                const LONGLONG qpc = desktopChanged
                    ? frameInfo.LastPresentTime.QuadPart
                    : frameInfo.LastMouseUpdateTime.QuadPart;
                publishFrame(qpcToMicroseconds(qpc));
            }
        } else {
            m_duplication->ReleaseFrame();
        }
    }
}
