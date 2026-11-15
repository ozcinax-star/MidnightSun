// RenderWindow.h
//
// Client-side presentation for the game streaming pipeline. Owns a
// fullscreen borderless window on the primary monitor, converts the
// decoder's NV12 textures to BGRA on the GPU through ID3D11VideoProcessor
// (no shaders, no CPU touch), draws an optional stats overlay with
// Direct2D/DirectWrite, and presents through a flip-model DXGI swap chain
// with Present(0, DXGI_PRESENT_DO_NOT_WAIT) so the render loop is never
// blocked by vsync.
//
// Wiring -- the main thread is the render thread:
//
//   RenderWindow window;
//   window.initialize(device, 1920, 1080);
//   while (!window.shouldClose()) {
//       window.presentFrame(latestNv12, tsUs);   // nullptr is fine
//       window.setStatsOverlay(receiver.getStats());
//   }
//   window.shutdown();
//
// Threading model:
//   - initialize()/presentFrame()/shutdown() must all be called from ONE
//     thread (the thread that owns the window -- Win32 ties the message
//     queue to the creating thread). presentFrame() pumps the message
//     loop with PeekMessage on every call, so call it regularly (pass
//     nullptr when no new frame arrived: the last frame is re-presented
//     and the window stays responsive).
//   - setStatsOverlay() and shouldClose() may be called from any thread.
//
// Input handling: Shift+ESC requests close (shouldClose() flips to true;
// the window itself is destroyed by shutdown(), on the owning thread). F1
// toggles the stats overlay, F2 toggles absolute/relative mouse mode.
// WM_CLOSE behaves like Shift+ESC. The cursor is hidden while over the
// window (the streamed desktop has its own, composited into the video)
// and clipped to the window while it is the foreground window, so a
// stray click can never land on another local window or monitor.
//
// Mouse forwarding hooks (wired by ClientSession to InputCapture):
//   - onWheel(vertical, horizontal): WM_MOUSEWHEEL/WM_MOUSEHWHEEL deltas.
//     This is the ONLY path that carries precision-touchpad two-finger
//     scrolling, which never reaches the Raw Input mouse stream.
//   - onPointerAbs(nx, ny): local cursor position from WM_MOUSEMOVE,
//     mapped through the current letterbox rect to normalized 0..65535
//     coordinates on the remote monitor. Only fired in absolute mode.
// Both fire on the render thread; keep them cheap (they write atomics).
//
// Frame handling: presentFrame() reads the texture's size, (re)builds the
// video processor when it changes, letterboxes into the monitor rect with
// BT.709 limited-range -> full-range RGB conversion, and caches one
// VideoProcessorInputView per texture pointer -- the decoder's 3-texture
// ring saturates the cache immediately, so per-frame view creation
// disappears. The texture passed in is AddRef'd (kept as "last frame")
// until the next new frame, which keeps repaints valid even if the
// decoder rebuilds its ring mid-stream.
//
// Overlay: FPS (frames actually presented), RTT, packet loss and bitrate
// from the last setStatsOverlay() snapshot, plus the per-stage pipeline
// latency breakdown carried in StreamStats::latency (capture/encode/
// network/decode/render, total and p99 -- stages this process cannot
// measure show as "--"; see LatencyTracker.h). Drawn top-left on a
// semi-transparent backdrop. Direct2D renders into the same back buffer
// between VideoProcessorBlt and Present -- no extra composition pass.
// presentFrame() also stamps Stage::RENDER_START / RENDER_END on the
// LatencyTracker for every new frame it presents.
//
// Requires: MSVC 2022, C++20, Windows 10. Links d3d11.lib, dxgi.lib,
// d2d1.lib, dwrite.lib via #pragma comment in the .cpp.

#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <d3d11.h>
#include <dxgi1_5.h> // IDXGIFactory5 / DXGI_FEATURE_PRESENT_ALLOW_TEARING
#include <d2d1.h>
#include <dwrite.h>
#include <wrl/client.h>

#include "NetworkProtocol.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

class RenderWindow {
public:
    RenderWindow() = default;
    ~RenderWindow(); // calls shutdown()

    // Set before the first presentFrame() call (messages are only pumped
    // there); both fire on the render thread.
    std::function<void(int vertical, int horizontal)>  onWheel;
    std::function<void(uint16_t nx, uint16_t ny)>      onPointerAbs;

    RenderWindow(const RenderWindow&) = delete;
    RenderWindow& operator=(const RenderWindow&) = delete;

    // Creates the fullscreen borderless window on the primary monitor, the
    // flip-model swap chain on `device`, the video processor (sized for
    // width x height until the first real frame corrects it), and the
    // overlay resources. Returns false with lastError() set on failure.
    // `device` must be the device the decoded textures live on and must
    // have been created with D3D11_CREATE_DEVICE_VIDEO_SUPPORT (for the
    // video processor) and D3D11_CREATE_DEVICE_BGRA_SUPPORT (for Direct2D).
    bool initialize(ID3D11Device* device, int width, int height);

    // Destroys the window and releases every GPU object. Must be called on
    // the thread that called initialize(). Safe to call when not
    // initialized; called automatically by the destructor.
    void shutdown();

    // Pumps the message loop, converts and letterboxes `nv12Frame` (or
    // re-presents the previous frame when nullptr), draws the overlay,
    // and presents without waiting for vblank. Failures are logged and
    // the frame is skipped; device-removed requests close.
    void presentFrame(ID3D11Texture2D* nv12Frame, uint64_t tsUs);

    // Publishes the stats drawn by the overlay on every subsequent frame.
    // Thread-safe.
    void setStatsOverlay(const StreamStats& stats);

    // True once Shift+ESC was pressed, the window was closed, or the
    // device was lost. Thread-safe.
    bool shouldClose() const { return m_closeRequested.load(std::memory_order_acquire); }

    // Current mouse mode (F2 toggles it): true = absolute/desktop, false =
    // relative/game. Thread-safe; ClientSession polls this each loop and
    // pushes it into InputCapture.
    bool absoluteMouse() const { return m_absoluteMouse.load(std::memory_order_relaxed); }

    // Most recent failure description; read after initialize() fails.
    std::string lastError() const;

private:
    template <typename T>
    using ComPtr = Microsoft::WRL::ComPtr<T>;

    // The decoder cycles 3 output textures; 8 leaves headroom for a ring
    // rebuild without the cache thrashing.
    static constexpr size_t kMaxCachedInputViews = 8;

    static LRESULT CALLBACK wndProcThunk(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    LRESULT handleMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

    void pumpMessages();
    void applyCursorClip(bool clip); // ClipCursor to the window rect / release
    void forwardPointer(LPARAM lParam); // WM_MOUSEMOVE -> onPointerAbs
    bool createWindowOnPrimaryMonitor();
    bool createSwapChain();
    bool createOverlayResources();
    void releaseOverlayResources();
    bool ensureVideoProcessor(UINT videoWidth, UINT videoHeight);
    ID3D11VideoProcessorInputView* lookupInputView(ID3D11Texture2D* texture);
    void drawOverlay();

    bool setError(const char* where, HRESULT hr);
    bool setError(const char* where, const char* detail);

    // --- device (render thread after initialize()) ---
    ComPtr<ID3D11Device>        m_device;
    ComPtr<ID3D11DeviceContext> m_context;
    ComPtr<ID3D11VideoDevice>   m_videoDevice;
    ComPtr<ID3D11VideoContext>  m_videoContext;

    // --- window ---
    HWND m_hwnd          = nullptr;
    ATOM m_classAtom     = 0;
    int  m_monitorWidth  = 0;
    int  m_monitorHeight = 0;

    // --- swap chain / back buffer ---
    ComPtr<IDXGISwapChain1>        m_swapChain;
    ComPtr<ID3D11Texture2D>        m_backBuffer;
    ComPtr<ID3D11RenderTargetView> m_backBufferRtv;

    // --- video processor (rebuilt on video size change) ---
    ComPtr<ID3D11VideoProcessorEnumerator> m_vpEnum;
    ComPtr<ID3D11VideoProcessor>           m_vp;
    ComPtr<ID3D11VideoProcessorOutputView> m_vpOutputView;
    std::vector<std::pair<ID3D11Texture2D*, ComPtr<ID3D11VideoProcessorInputView>>>
         m_inputViews;
    UINT m_videoWidth  = 0;
    UINT m_videoHeight = 0;
    RECT m_destRect    = {}; // letterbox rect the video occupies on screen

    // --- last presented frame (kept alive for repaints) ---
    ComPtr<ID3D11Texture2D> m_lastFrame;
    uint64_t                m_lastFrameTsUs = 0;

    // --- overlay (Direct2D on the back buffer) ---
    ComPtr<ID2D1Factory>          m_d2dFactory;
    ComPtr<ID2D1RenderTarget>     m_d2dTarget;
    ComPtr<IDWriteFactory>        m_dwriteFactory;
    ComPtr<IDWriteTextFormat>     m_textFormat;
    ComPtr<ID2D1SolidColorBrush>  m_textBrush;
    ComPtr<ID2D1SolidColorBrush>  m_backdropBrush;
    std::atomic<bool>             m_overlayVisible{true};

    mutable std::mutex m_statsMutex;
    StreamStats        m_stats;

    // --- FPS (frames presented in the last rolling second) ---
    uint32_t m_fpsFrames        = 0;
    uint32_t m_displayFps       = 0;
    uint64_t m_fpsWindowStartMs = 0;

    // --- control / throttled logging ---
    std::atomic<bool> m_closeRequested{false};
    std::atomic<bool> m_absoluteMouse{true}; // F2 toggles; default desktop mode
    bool              m_cursorClipped = false; // render thread only
    bool              m_allowTearing  = false; // DXGI tearing support (latency)
    bool              m_initialized     = false;
    uint32_t          m_bltFailures     = 0;
    uint32_t          m_presentFailures = 0;

    mutable std::mutex m_errorMutex;
    std::string        m_lastError;
};
