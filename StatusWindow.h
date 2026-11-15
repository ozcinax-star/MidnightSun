// StatusWindow.h
//
// Server-side status window: a small dark Direct2D panel that replaces
// "read the console and type IPs". It shows, at a glance:
//
//   - state pill: amber "İstemci bekleniyor" / green "Yayında — <addr>"
//   - the PAIRING CODE, large and click-to-copy (see Discovery.h) -- the
//     thing a user types on the client instead of an IP address
//   - the local address and a reminder that same-LAN clients see this
//     machine automatically
//   - live stats (bitrate / RTT / loss) refreshed by the server loop
//
// Extras:
//   - Bağlantı onayı: requestApproval(addr) (any thread) raises an in-window
//     "X bağlanmak istiyor — İzin ver / Reddet" prompt (restores from tray
//     and flashes). The host loop polls takeApprovalDecision() and forwards
//     the answer to StreamSender::resolvePending().
//   - Sistem tepsisi: the "—" button hides the window to the notification
//     area; the tray icon restores it (left click) or offers Geri yükle /
//     Kodu kopyala / Yayını durdur (right click).
//   - Per-monitor DPI: layout constants are DIPs; the window sizes itself
//     by the monitor scale, the D2D target renders at native DPI, and
//     WM_DPICHANGED rescales live.
//
// Threading: start() spawns a dedicated UI thread that owns the window,
// the D2D objects and the message loop; stop() posts WM_CLOSE and joins.
// setStreaming()/setStats()/requestApproval()/takeApprovalDecision() may
// be called from any thread (mutex/atomic-guarded; the UI repaints on a
// 500 ms timer). Closing the window sets closeRequested() -- the server
// main loop treats that as Ctrl+C.
//
// Requires: MSVC 2022, C++20. Links d2d1.lib, dwrite.lib, dwmapi.lib,
// shell32.lib via #pragma comment in the .cpp.

#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <wrl/client.h>

#include <atomic>
#include <cstdint>
#include <future>
#include <mutex>
#include <string>
#include <thread>

class StatusWindow {
public:
    StatusWindow() = default;
    ~StatusWindow(); // calls stop()

    StatusWindow(const StatusWindow&) = delete;
    StatusWindow& operator=(const StatusWindow&) = delete;

    // Spawns the UI thread and blocks until the window is up (or failed).
    // Failure is non-fatal for the server: log and continue headless.
    bool start(const std::string& pairingCode, const std::string& localIp,
               uint16_t streamPort);

    // Posts WM_CLOSE and joins the UI thread. Safe when not running.
    void stop();

    // Any thread.
    void setStreaming(bool streaming, const std::string& clientAddr);
    void setStats(float kbps, float rttMs, float lossPct);

    // Any thread: shows the connect-approval prompt for `clientAddr`
    // (restores the window from the tray and flashes the taskbar).
    void requestApproval(const std::string& clientAddr);

    // Any thread, one-shot: +1 = İzin ver, -1 = Reddet, 0 = no decision
    // yet. The host loop polls this and calls StreamSender::resolvePending.
    int takeApprovalDecision();

    // True once the user closed the window; the server should shut down.
    bool closeRequested() const
    {
        return m_closeRequested.load(std::memory_order_acquire);
    }

private:
    template <typename T>
    using ComPtr = Microsoft::WRL::ComPtr<T>;

    static LRESULT CALLBACK wndProcThunk(HWND, UINT, WPARAM, LPARAM);
    LRESULT handleMessage(HWND, UINT, WPARAM, LPARAM);

    void threadProc(std::promise<bool> ready);
    bool createWindow();
    bool createGraphics();
    void releaseGraphics();
    void applyDpi(UINT dpi, const RECT* suggested); // pencere+RT yeniden ölçekle
    void paint();
    void copyCodeToClipboard();
    void addTrayIcon();
    void removeTrayIcon();
    void restoreFromTray();
    void showTrayMenu();

    // --- fixed content (set before the thread starts) ---
    std::wstring m_code;      // grouped pairing code
    std::wstring m_address;   // "ip:port"

    // --- live state (m_stateMutex) ---
    std::mutex   m_stateMutex;
    bool         m_streaming = false;
    std::wstring m_clientAddr;
    float        m_kbps    = 0.0f;
    float        m_rttMs   = 0.0f;
    float        m_lossPct = 0.0f;
    bool         m_approvalActive = false; // onay istemi görünür
    std::wstring m_approvalAddr;

    std::atomic<int> m_approvalDecision{0}; // +1 izin, -1 ret, 0 yok

    // --- UI thread objects ---
    HWND  m_hwnd      = nullptr;
    ATOM  m_classAtom = 0;
    float m_scale     = 1.0f; // DPI/96; fare koordinatları bununla DIP'e iner
    bool  m_inTray    = false;
    ComPtr<ID2D1Factory>          m_d2dFactory;
    ComPtr<ID2D1HwndRenderTarget> m_rt;
    ComPtr<IDWriteFactory>        m_dwrite;
    ComPtr<IDWriteTextFormat>     m_fmtTitle;
    ComPtr<IDWriteTextFormat>     m_fmtBody;
    ComPtr<IDWriteTextFormat>     m_fmtSmall;
    ComPtr<IDWriteTextFormat>     m_fmtCode;
    ComPtr<ID2D1SolidColorBrush>  m_brush;
    bool     m_hoverClose  = false;
    bool     m_hoverMin    = false;
    bool     m_hoverCode   = false;
    bool     m_hoverAllow  = false;
    bool     m_hoverDeny   = false;
    uint64_t m_copiedUntil = 0; // GetTickCount64 deadline for "Kopyalandı"

    std::thread       m_thread;
    std::atomic<bool> m_closeRequested{false};
};
