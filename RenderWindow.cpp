// RenderWindow.cpp -- see RenderWindow.h for the design overview.

#include "RenderWindow.h"

#include "LatencyTracker.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")

namespace {

constexpr wchar_t kWindowClassName[] = L"MidnightSunRenderWindow";
constexpr wchar_t kWindowTitle[]     = L"MidnightSun";

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

uint64_t SteadyMilliseconds()
{
    return GetTickCount64();
}

// Largest rect with the video's aspect ratio that fits the monitor,
// centered -- classic letterbox/pillarbox.
RECT ComputeLetterboxRect(UINT videoW, UINT videoH, int monW, int monH)
{
    RECT r = { 0, 0, monW, monH };
    if (videoW == 0 || videoH == 0 || monW <= 0 || monH <= 0)
        return r;

    const double videoAspect   = static_cast<double>(videoW) / static_cast<double>(videoH);
    const double monitorAspect = static_cast<double>(monW) / static_cast<double>(monH);

    if (videoAspect > monitorAspect) {
        const int h = static_cast<int>(monW / videoAspect + 0.5);
        r.top    = (monH - h) / 2;
        r.bottom = r.top + h;
    } else if (videoAspect < monitorAspect) {
        const int w = static_cast<int>(monH * videoAspect + 0.5);
        r.left  = (monW - w) / 2;
        r.right = r.left + w;
    }
    return r;
}

} // namespace

// ---------------------------------------------------------------------------
// Error recording
// ---------------------------------------------------------------------------

bool RenderWindow::setError(const char* where, HRESULT hr)
{
    std::string message = std::string(where) + " failed: " + HresultString(hr);
    LogF("[RenderWindow] %s", message.c_str());
    {
        std::lock_guard<std::mutex> lock(m_errorMutex);
        m_lastError = std::move(message);
    }
    return false;
}

bool RenderWindow::setError(const char* where, const char* detail)
{
    std::string message = std::string(where) + " failed: " + detail;
    LogF("[RenderWindow] %s", message.c_str());
    {
        std::lock_guard<std::mutex> lock(m_errorMutex);
        m_lastError = std::move(message);
    }
    return false;
}

std::string RenderWindow::lastError() const
{
    std::lock_guard<std::mutex> lock(m_errorMutex);
    return m_lastError;
}

// ---------------------------------------------------------------------------
// Window procedure
// ---------------------------------------------------------------------------

LRESULT CALLBACK RenderWindow::wndProcThunk(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (msg == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(create->lpCreateParams));
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }

    auto* self = reinterpret_cast<RenderWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (self)
        return self->handleMessage(hwnd, msg, wParam, lParam);
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT RenderWindow::handleMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_KEYDOWN:
        // Shift+ESC quits; plain ESC belongs to the game (menus etc.) and
        // travels to the server through InputCapture like any other key.
        if (wParam == VK_ESCAPE && (GetKeyState(VK_SHIFT) & 0x8000)) {
            LogF("[RenderWindow] Shift+ESC pressed; close requested");
            m_closeRequested.store(true, std::memory_order_release);
            return 0;
        }
        if (wParam == VK_F1 && (lParam & (1u << 30)) == 0) { // ignore key repeat
            const bool visible = !m_overlayVisible.load(std::memory_order_relaxed);
            m_overlayVisible.store(visible, std::memory_order_relaxed);
            LogF("[RenderWindow] stats overlay %s", visible ? "shown" : "hidden");
            return 0;
        }
        if (wParam == VK_F2 && (lParam & (1u << 30)) == 0) { // ignore key repeat
            const bool absolute = !m_absoluteMouse.load(std::memory_order_relaxed);
            m_absoluteMouse.store(absolute, std::memory_order_relaxed);
            LogF("[RenderWindow] mouse mode: %s",
                 absolute ? "absolute (desktop)" : "relative (game)");
            return 0;
        }
        break;

    case WM_CLOSE:
        // The window itself is torn down by shutdown(), on the owning
        // thread, after the app loop sees shouldClose().
        m_closeRequested.store(true, std::memory_order_release);
        return 0;

    case WM_SETCURSOR:
        // The streamed desktop renders its own cursor; the local one on
        // top would just be a second, dead pointer.
        SetCursor(nullptr);
        return TRUE;

    case WM_MOUSEMOVE:
        // Absolute mode: map the local cursor onto the remote monitor.
        if (m_absoluteMouse.load(std::memory_order_relaxed))
            forwardPointer(lParam);
        return 0;

    case WM_MOUSEWHEEL:
        // The only path that carries precision-touchpad scrolling (raw
        // input never sees it); real mice arrive here too while we are
        // foreground, so InputCapture takes ALL wheel input from us.
        if (onWheel)
            onWheel(GET_WHEEL_DELTA_WPARAM(wParam), 0);
        return 0;

    case WM_MOUSEHWHEEL:
        if (onWheel)
            onWheel(0, GET_WHEEL_DELTA_WPARAM(wParam));
        return 0;

    case WM_ACTIVATE:
        // Confine the (hidden) cursor to our window while we own the
        // foreground: a drifting click can never activate another local
        // window or land on a second monitor. Released on deactivate so
        // Alt-Tab keeps working normally.
        applyCursorClip(LOWORD(wParam) != WA_INACTIVE);
        break;

    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

void RenderWindow::applyCursorClip(bool clip)
{
    if (clip && m_hwnd) {
        RECT rect = {};
        if (GetWindowRect(m_hwnd, &rect)) {
            ClipCursor(&rect);
            m_cursorClipped = true;
        }
    } else if (m_cursorClipped) {
        ClipCursor(nullptr);
        m_cursorClipped = false;
    }
}

void RenderWindow::forwardPointer(LPARAM lParam)
{
    if (!onPointerAbs)
        return;

    // Client coords -> position inside the letterbox rect -> normalized
    // 0..65535 on the remote monitor. Points in the black bars clamp to
    // the nearest video edge.
    const int x = static_cast<int>(static_cast<short>(LOWORD(lParam)));
    const int y = static_cast<int>(static_cast<short>(HIWORD(lParam)));

    RECT dest = m_destRect;
    if (dest.right <= dest.left || dest.bottom <= dest.top) {
        dest.left = 0;
        dest.top  = 0;
        dest.right  = m_monitorWidth;
        dest.bottom = m_monitorHeight;
    }
    const int w = dest.right - dest.left;
    const int h = dest.bottom - dest.top;
    if (w <= 1 || h <= 1)
        return;

    int px = x - dest.left;
    int py = y - dest.top;
    px = px < 0 ? 0 : (px > w - 1 ? w - 1 : px);
    py = py < 0 ? 0 : (py > h - 1 ? h - 1 : py);

    const uint16_t nx = static_cast<uint16_t>(MulDiv(px, 65535, w - 1));
    const uint16_t ny = static_cast<uint16_t>(MulDiv(py, 65535, h - 1));
    onPointerAbs(nx, ny);
}

void RenderWindow::pumpMessages()
{
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

// ---------------------------------------------------------------------------
// Initialization
// ---------------------------------------------------------------------------

RenderWindow::~RenderWindow()
{
    shutdown();
}

bool RenderWindow::initialize(ID3D11Device* device, int width, int height)
{
    if (!device)
        return setError("initialize", "device is null");
    if (m_initialized)
        shutdown();

    m_device = device;
    m_device->GetImmediateContext(&m_context);

    HRESULT hr = m_device.As(&m_videoDevice);
    if (FAILED(hr)) {
        setError("QueryInterface(ID3D11VideoDevice)", hr);
        shutdown();
        return false;
    }
    hr = m_context.As(&m_videoContext);
    if (FAILED(hr)) {
        setError("QueryInterface(ID3D11VideoContext)", hr);
        shutdown();
        return false;
    }

    if (!createWindowOnPrimaryMonitor() || !createSwapChain() || !createOverlayResources()) {
        shutdown();
        return false;
    }

    // Pre-build the video processor for the expected stream size; the
    // first real frame corrects it if the hint was wrong.
    if (width > 0 && height > 0) {
        if (!ensureVideoProcessor(static_cast<UINT>(width), static_cast<UINT>(height))) {
            shutdown();
            return false;
        }
    }

    m_fpsWindowStartMs = SteadyMilliseconds();
    m_initialized      = true;
    LogF("[RenderWindow] initialized: %dx%d borderless fullscreen, video hint %dx%d",
         m_monitorWidth, m_monitorHeight, width, height);
    return true;
}

bool RenderWindow::createWindowOnPrimaryMonitor()
{
    const HINSTANCE instance = GetModuleHandleW(nullptr);

    WNDCLASSEXW wc = {};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = &RenderWindow::wndProcThunk;
    wc.hInstance     = instance;
    wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    wc.lpszClassName = kWindowClassName;

    m_classAtom = RegisterClassExW(&wc);
    if (m_classAtom == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        return setError("RegisterClassExW", HRESULT_FROM_WIN32(GetLastError()));

    m_monitorWidth  = GetSystemMetrics(SM_CXSCREEN);
    m_monitorHeight = GetSystemMetrics(SM_CYSCREEN);
    if (m_monitorWidth <= 0 || m_monitorHeight <= 0)
        return setError("GetSystemMetrics", "no primary monitor metrics");

    m_hwnd = CreateWindowExW(0, kWindowClassName, kWindowTitle, WS_POPUP,
                             0, 0, m_monitorWidth, m_monitorHeight,
                             nullptr, nullptr, instance, this);
    if (!m_hwnd)
        return setError("CreateWindowExW", HRESULT_FROM_WIN32(GetLastError()));

    ShowWindow(m_hwnd, SW_SHOW);
    SetForegroundWindow(m_hwnd);
    UpdateWindow(m_hwnd);
    applyCursorClip(true); // we are foreground now; WM_ACTIVATE keeps it updated
    return true;
}

bool RenderWindow::createSwapChain()
{
    ComPtr<IDXGIDevice> dxgiDevice;
    HRESULT hr = m_device.As(&dxgiDevice);
    if (FAILED(hr))
        return setError("QueryInterface(IDXGIDevice)", hr);

    ComPtr<IDXGIAdapter> adapter;
    hr = dxgiDevice->GetAdapter(&adapter);
    if (FAILED(hr))
        return setError("IDXGIDevice::GetAdapter", hr);

    ComPtr<IDXGIFactory2> factory;
    hr = adapter->GetParent(IID_PPV_ARGS(&factory));
    if (FAILED(hr))
        return setError("IDXGIAdapter::GetParent(IDXGIFactory2)", hr);

    // Tearing support (Win10 1607+, driver dependent): lets Present bypass
    // the compositor's vblank wait in borderless fullscreen -- up to one
    // vsync less display latency. Best effort; absent = old behavior.
    {
        ComPtr<IDXGIFactory5> factory5;
        BOOL tearing = FALSE;
        if (SUCCEEDED(factory.As(&factory5)) &&
            SUCCEEDED(factory5->CheckFeatureSupport(
                DXGI_FEATURE_PRESENT_ALLOW_TEARING, &tearing, sizeof(tearing))) &&
            tearing)
            m_allowTearing = true;
    }

    DXGI_SWAP_CHAIN_DESC1 desc = {};
    desc.Width            = static_cast<UINT>(m_monitorWidth);
    desc.Height           = static_cast<UINT>(m_monitorHeight);
    desc.Format           = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage      = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount      = 2;
    desc.SwapEffect       = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    desc.AlphaMode        = DXGI_ALPHA_MODE_IGNORE;
    desc.Scaling          = DXGI_SCALING_STRETCH;
    desc.Flags            = m_allowTearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0u;

    hr = factory->CreateSwapChainForHwnd(m_device.Get(), m_hwnd, &desc,
                                         nullptr, nullptr, &m_swapChain);
    if (FAILED(hr))
        return setError("CreateSwapChainForHwnd", hr);

    // Exclusive fullscreen transitions would fight the borderless model.
    factory->MakeWindowAssociation(m_hwnd, DXGI_MWA_NO_ALT_ENTER);

    // Flip model: the runtime rotates the buffers internally, so buffer 0
    // is always the writable back buffer; views created on it stay valid.
    hr = m_swapChain->GetBuffer(0, IID_PPV_ARGS(&m_backBuffer));
    if (FAILED(hr))
        return setError("IDXGISwapChain1::GetBuffer", hr);

    hr = m_device->CreateRenderTargetView(m_backBuffer.Get(), nullptr, &m_backBufferRtv);
    if (FAILED(hr))
        return setError("CreateRenderTargetView(back buffer)", hr);
    return true;
}

bool RenderWindow::createOverlayResources()
{
    D2D1_FACTORY_OPTIONS options = {};
    HRESULT hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
                                   __uuidof(ID2D1Factory), &options,
                                   reinterpret_cast<void**>(m_d2dFactory.GetAddressOf()));
    if (FAILED(hr))
        return setError("D2D1CreateFactory", hr);

    ComPtr<IDXGISurface> surface;
    hr = m_backBuffer.As(&surface);
    if (FAILED(hr))
        return setError("QueryInterface(IDXGISurface)", hr);

    const D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE),
        96.0f, 96.0f);
    hr = m_d2dFactory->CreateDxgiSurfaceRenderTarget(surface.Get(), &props, &m_d2dTarget);
    if (FAILED(hr))
        return setError("CreateDxgiSurfaceRenderTarget", hr);

    hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                             reinterpret_cast<IUnknown**>(m_dwriteFactory.GetAddressOf()));
    if (FAILED(hr))
        return setError("DWriteCreateFactory", hr);

    hr = m_dwriteFactory->CreateTextFormat(
        L"Consolas", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL, 14.0f, L"en-us", &m_textFormat);
    if (FAILED(hr))
        return setError("CreateTextFormat", hr);
    // Default NEAR alignment: the overlay is a multi-line block anchored
    // to the top-left of its backdrop.

    hr = m_d2dTarget->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), &m_textBrush);
    if (FAILED(hr))
        return setError("CreateSolidColorBrush(text)", hr);
    hr = m_d2dTarget->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.55f),
                                            &m_backdropBrush);
    if (FAILED(hr))
        return setError("CreateSolidColorBrush(backdrop)", hr);
    return true;
}

void RenderWindow::releaseOverlayResources()
{
    m_backdropBrush.Reset();
    m_textBrush.Reset();
    m_textFormat.Reset();
    m_dwriteFactory.Reset();
    m_d2dTarget.Reset();
    m_d2dFactory.Reset();
}

void RenderWindow::shutdown()
{
    applyCursorClip(false); // never leave the cursor confined after exit

    m_inputViews.clear();
    m_vpOutputView.Reset();
    m_vp.Reset();
    m_vpEnum.Reset();
    m_lastFrame.Reset();

    releaseOverlayResources();

    m_backBufferRtv.Reset();
    m_backBuffer.Reset();
    m_swapChain.Reset();

    if (m_hwnd) {
        DestroyWindow(m_hwnd);
        m_hwnd = nullptr;
        // Drain the destroy messages so the class can unregister.
        pumpMessages();
    }
    if (m_classAtom) {
        UnregisterClassW(kWindowClassName, GetModuleHandleW(nullptr));
        m_classAtom = 0;
    }

    m_videoContext.Reset();
    m_videoDevice.Reset();
    m_context.Reset();
    m_device.Reset();

    m_videoWidth = m_videoHeight = 0;
    m_initialized = false;
}

// ---------------------------------------------------------------------------
// Video processor
// ---------------------------------------------------------------------------

bool RenderWindow::ensureVideoProcessor(UINT videoWidth, UINT videoHeight)
{
    if (m_vp && videoWidth == m_videoWidth && videoHeight == m_videoHeight)
        return true;

    m_inputViews.clear();
    m_vpOutputView.Reset();
    m_vp.Reset();
    m_vpEnum.Reset();

    D3D11_VIDEO_PROCESSOR_CONTENT_DESC content = {};
    content.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    content.InputWidth       = videoWidth;
    content.InputHeight      = videoHeight;
    content.OutputWidth      = static_cast<UINT>(m_monitorWidth);
    content.OutputHeight     = static_cast<UINT>(m_monitorHeight);
    content.InputFrameRate   = { 60, 1 };
    content.OutputFrameRate  = { 60, 1 };
    content.Usage            = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;

    HRESULT hr = m_videoDevice->CreateVideoProcessorEnumerator(&content, &m_vpEnum);
    if (FAILED(hr))
        return setError("CreateVideoProcessorEnumerator", hr);

    hr = m_videoDevice->CreateVideoProcessor(m_vpEnum.Get(), 0, &m_vp);
    if (FAILED(hr))
        return setError("CreateVideoProcessor", hr);

    D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC outputDesc = {};
    outputDesc.ViewDimension      = D3D11_VPOV_DIMENSION_TEXTURE2D;
    outputDesc.Texture2D.MipSlice = 0;
    hr = m_videoDevice->CreateVideoProcessorOutputView(m_backBuffer.Get(), m_vpEnum.Get(),
                                                       &outputDesc, &m_vpOutputView);
    if (FAILED(hr))
        return setError("CreateVideoProcessorOutputView", hr);

    // H.264 from the NVENC side is BT.709 studio range; the back buffer
    // wants full-range RGB.
    D3D11_VIDEO_PROCESSOR_COLOR_SPACE inputCs = {};
    inputCs.YCbCr_Matrix  = 1; // BT.709
    inputCs.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235;
    m_videoContext->VideoProcessorSetStreamColorSpace(m_vp.Get(), 0, &inputCs);

    D3D11_VIDEO_PROCESSOR_COLOR_SPACE outputCs = {};
    outputCs.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_0_255;
    m_videoContext->VideoProcessorSetOutputColorSpace(m_vp.Get(), &outputCs);

    m_videoContext->VideoProcessorSetStreamFrameFormat(
        m_vp.Get(), 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);

    RECT source = { 0, 0, static_cast<LONG>(videoWidth), static_cast<LONG>(videoHeight) };
    m_videoContext->VideoProcessorSetStreamSourceRect(m_vp.Get(), 0, TRUE, &source);

    const RECT dest = ComputeLetterboxRect(videoWidth, videoHeight,
                                           m_monitorWidth, m_monitorHeight);
    m_videoContext->VideoProcessorSetStreamDestRect(m_vp.Get(), 0, TRUE, &dest);
    m_destRect = dest; // pointer mapping (forwardPointer) uses this rect

    RECT target = { 0, 0, m_monitorWidth, m_monitorHeight };
    m_videoContext->VideoProcessorSetOutputTargetRect(m_vp.Get(), TRUE, &target);

    m_videoWidth  = videoWidth;
    m_videoHeight = videoHeight;
    LogF("[RenderWindow] video processor: %ux%u -> %dx%d (dest %ld,%ld-%ld,%ld)",
         videoWidth, videoHeight, m_monitorWidth, m_monitorHeight,
         dest.left, dest.top, dest.right, dest.bottom);
    return true;
}

ID3D11VideoProcessorInputView* RenderWindow::lookupInputView(ID3D11Texture2D* texture)
{
    for (const auto& entry : m_inputViews) {
        if (entry.first == texture)
            return entry.second.Get();
    }

    D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC inputDesc = {};
    inputDesc.FourCC               = 0; // derive from the texture format (NV12)
    inputDesc.ViewDimension        = D3D11_VPIV_DIMENSION_TEXTURE2D;
    inputDesc.Texture2D.MipSlice   = 0;
    inputDesc.Texture2D.ArraySlice = 0;

    ComPtr<ID3D11VideoProcessorInputView> view;
    HRESULT hr = m_videoDevice->CreateVideoProcessorInputView(texture, m_vpEnum.Get(),
                                                              &inputDesc, &view);
    if (FAILED(hr)) {
        setError("CreateVideoProcessorInputView", hr);
        return nullptr;
    }

    if (m_inputViews.size() >= kMaxCachedInputViews)
        m_inputViews.clear(); // decoder rebuilt its ring; stale pointers, start over
    m_inputViews.emplace_back(texture, view);
    return m_inputViews.back().second.Get();
}

// ---------------------------------------------------------------------------
// Per-frame path
// ---------------------------------------------------------------------------

void RenderWindow::presentFrame(ID3D11Texture2D* nv12Frame, uint64_t tsUs)
{
    pumpMessages();

    if (!m_initialized || !m_swapChain ||
        m_closeRequested.load(std::memory_order_acquire))
        return;

    const bool newFrame = nv12Frame != nullptr;
    ID3D11Texture2D* frame = newFrame ? nv12Frame : m_lastFrame.Get();

    if (newFrame)
        LatencyTracker::get().mark(tsUs, Stage::RENDER_START);

    const float black[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    m_context->ClearRenderTargetView(m_backBufferRtv.Get(), black);

    if (frame) {
        D3D11_TEXTURE2D_DESC desc;
        frame->GetDesc(&desc);
        if (ensureVideoProcessor(desc.Width, desc.Height)) {
            if (ID3D11VideoProcessorInputView* view = lookupInputView(frame)) {
                D3D11_VIDEO_PROCESSOR_STREAM stream = {};
                stream.Enable        = TRUE;
                stream.pInputSurface = view;
                HRESULT hr = m_videoContext->VideoProcessorBlt(
                    m_vp.Get(), m_vpOutputView.Get(), 0, 1, &stream);
                if (FAILED(hr)) {
                    ++m_bltFailures;
                    if (m_bltFailures <= 3 || m_bltFailures % 300 == 0)
                        setError("VideoProcessorBlt", hr);
                }
            }
        }
        if (newFrame) {
            m_lastFrame     = nv12Frame; // AddRef: survives decoder ring rebuilds
            m_lastFrameTsUs = tsUs;
            ++m_fpsFrames;
        }
    }

    const uint64_t nowMs = SteadyMilliseconds();
    if (nowMs - m_fpsWindowStartMs >= 1000) {
        m_displayFps       = m_fpsFrames;
        m_fpsFrames        = 0;
        m_fpsWindowStartMs = nowMs;
    }

    if (m_overlayVisible.load(std::memory_order_relaxed))
        drawOverlay();

    const UINT presentFlags = DXGI_PRESENT_DO_NOT_WAIT |
                              (m_allowTearing ? DXGI_PRESENT_ALLOW_TEARING : 0u);
    HRESULT hr = m_swapChain->Present(0, presentFlags);
    if (newFrame)
        LatencyTracker::get().mark(tsUs, Stage::RENDER_END);
    if (hr == DXGI_ERROR_WAS_STILL_DRAWING) {
        // The previous present hasn't finished; skipping keeps latency flat.
    } else if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
        setError("IDXGISwapChain1::Present",
                 hr == DXGI_ERROR_DEVICE_REMOVED ? m_device->GetDeviceRemovedReason() : hr);
        LogF("[RenderWindow] device lost; requesting close");
        m_closeRequested.store(true, std::memory_order_release);
    } else if (FAILED(hr)) {
        ++m_presentFailures;
        if (m_presentFailures <= 3 || m_presentFailures % 300 == 0)
            setError("IDXGISwapChain1::Present", hr);
    }
}

void RenderWindow::setStatsOverlay(const StreamStats& stats)
{
    std::lock_guard<std::mutex> lock(m_statsMutex);
    m_stats = stats;
}

void RenderWindow::drawOverlay()
{
    if (!m_d2dTarget)
        return;

    StreamStats stats;
    {
        std::lock_guard<std::mutex> lock(m_statsMutex);
        stats = m_stats;
    }

    // One compact line pinned to the top edge: transport stats plus the
    // end-to-end latency from LatencyTracker ("--" while unmeasured).
    const LatencyReport& latency = stats.latency;
    const wchar_t* mouseMode =
        m_absoluteMouse.load(std::memory_order_relaxed) ? L"abs" : L"rel";
    wchar_t text[256];
    if (latency.trackedFrames > 0 && latency.totalMs >= 0.0f) {
        swprintf_s(text,
                   L"FPS %u | rtt %.1f ms | loss %.2f%% | %.0f kbps | "
                   L"lat %.1f ms (p99 %.1f) | mouse %s (F2)",
                   m_displayFps, static_cast<double>(stats.rttMs),
                   static_cast<double>(stats.packetLossPct),
                   static_cast<double>(stats.bitrateKbps),
                   static_cast<double>(latency.totalMs),
                   static_cast<double>(latency.p99TotalMs),
                   mouseMode);
    } else {
        swprintf_s(text,
                   L"FPS %u | rtt %.1f ms | loss %.2f%% | %.0f kbps | lat -- | "
                   L"mouse %s (F2)",
                   m_displayFps, static_cast<double>(stats.rttMs),
                   static_cast<double>(stats.packetLossPct),
                   static_cast<double>(stats.bitrateKbps),
                   mouseMode);
    }

    m_d2dTarget->BeginDraw();
    const D2D1_RECT_F backdrop = D2D1::RectF(8.0f, 8.0f, 700.0f, 33.0f);
    m_d2dTarget->FillRectangle(backdrop, m_backdropBrush.Get());
    const D2D1_RECT_F textRect = D2D1::RectF(16.0f, 12.0f, 692.0f, 31.0f);
    m_d2dTarget->DrawText(text, static_cast<UINT32>(wcslen(text)),
                          m_textFormat.Get(), textRect, m_textBrush.Get());
    HRESULT hr = m_d2dTarget->EndDraw();
    if (hr == static_cast<HRESULT>(D2DERR_RECREATE_TARGET)) {
        LogF("[RenderWindow] D2D target lost; recreating overlay resources");
        releaseOverlayResources();
        createOverlayResources();
    } else if (FAILED(hr)) {
        ++m_presentFailures;
        if (m_presentFailures <= 3 || m_presentFailures % 300 == 0)
            setError("ID2D1RenderTarget::EndDraw", hr);
    }
}
