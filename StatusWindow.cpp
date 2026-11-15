// StatusWindow.cpp -- see StatusWindow.h for the design overview.

#include "StatusWindow.h"
#include "resource.h"

#include <dwmapi.h>
#include <shellapi.h>  // Shell_NotifyIconW (tepsi simgesi)
#include <windowsx.h>  // GET_X_LPARAM / GET_Y_LPARAM

#include <cstdio>

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "shell32.lib")

namespace {

constexpr wchar_t kClassName[] = L"MidnightSunStatus";

// Tepsi geri çağrısı + menü kimlikleri
constexpr UINT kTrayMsg        = WM_APP + 1;
constexpr UINT kApprovalMsg    = WM_APP + 2; // requestApproval -> UI thread
constexpr UINT kMenuRestore    = 1;
constexpr UINT kMenuCopyCode   = 2;
constexpr UINT kMenuStop       = 3;

// --- geometry (DIP; pencere DPI'a göre ölçeklenir) ---------------------------
constexpr int kW = 460;
constexpr int kH = 380;

constexpr D2D1_RECT_F kHeaderRc = {0.0f, 0.0f, 460.0f, 64.0f};
constexpr D2D1_RECT_F kCloseRc  = {412.0f, 14.0f, 444.0f, 46.0f};
constexpr D2D1_RECT_F kMinRc    = {374.0f, 14.0f, 406.0f, 46.0f};
constexpr D2D1_RECT_F kCodeRc   = {32.0f, 128.0f, 428.0f, 208.0f};
// Onay istemi paneli + düğmeleri (panel kod alanını örter)
constexpr D2D1_RECT_F kPromptRc = {32.0f, 104.0f, 428.0f, 262.0f};
constexpr D2D1_RECT_F kAllowRc  = {96.0f, 208.0f, 236.0f, 246.0f};
constexpr D2D1_RECT_F kDenyRc   = {252.0f, 208.0f, 364.0f, 246.0f};

// --- palette (same family as LauncherWindow) --------------------------------
constexpr UINT32 kColBg         = 0x0E1116;
constexpr UINT32 kColPanel      = 0x161B22;
constexpr UINT32 kColPanelHover = 0x1F2833;
constexpr UINT32 kColBorder     = 0x2A313B;
constexpr UINT32 kColText       = 0xE8EAED;
constexpr UINT32 kColDim        = 0x8B95A1;
constexpr UINT32 kColAccent     = 0x3FA9F5;
constexpr UINT32 kColDanger     = 0xE06C60;
constexpr UINT32 kColLive       = 0x53C783; // streaming green
constexpr UINT32 kColWaiting    = 0xE0A34C; // waiting amber

D2D1_COLOR_F Col(UINT32 rgb, float a = 1.0f)
{
    return D2D1::ColorF(((rgb >> 16) & 0xFF) / 255.0f,
                        ((rgb >> 8) & 0xFF) / 255.0f,
                        (rgb & 0xFF) / 255.0f, a);
}

bool PointIn(const D2D1_RECT_F& rc, float x, float y)
{
    return x >= rc.left && x < rc.right && y >= rc.top && y < rc.bottom;
}

#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif
#ifndef DWMWA_WINDOW_CORNER_PREFERENCE
#define DWMWA_WINDOW_CORNER_PREFERENCE 33
#endif

std::wstring Widen(const std::string& utf8)
{
    if (utf8.empty())
        return {};
    wchar_t buf[160] = {};
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, buf, 159);
    return buf;
}

} // namespace

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

StatusWindow::~StatusWindow()
{
    stop();
}

bool StatusWindow::start(const std::string& pairingCode, const std::string& localIp,
                         uint16_t streamPort)
{
    if (m_thread.joinable())
        return true;

    m_code = Widen(pairingCode);
    char addr[64];
    sprintf_s(addr, "%s:%u", localIp.empty() ? "?" : localIp.c_str(), streamPort);
    m_address = Widen(addr);

    m_closeRequested.store(false, std::memory_order_release);
    m_approvalDecision.store(0, std::memory_order_release);
    std::promise<bool> ready;
    std::future<bool> readyResult = ready.get_future();
    m_thread = std::thread(&StatusWindow::threadProc, this, std::move(ready));
    if (!readyResult.get()) {
        m_thread.join();
        return false;
    }
    return true;
}

void StatusWindow::stop()
{
    if (!m_thread.joinable())
        return;
    if (m_hwnd)
        PostMessageW(m_hwnd, WM_CLOSE, 0, 0);
    m_thread.join();
}

void StatusWindow::setStreaming(bool streaming, const std::string& clientAddr)
{
    std::lock_guard<std::mutex> lock(m_stateMutex);
    m_streaming  = streaming;
    m_clientAddr = Widen(clientAddr);
}

void StatusWindow::setStats(float kbps, float rttMs, float lossPct)
{
    std::lock_guard<std::mutex> lock(m_stateMutex);
    m_kbps    = kbps;
    m_rttMs   = rttMs;
    m_lossPct = lossPct;
}

void StatusWindow::requestApproval(const std::string& clientAddr)
{
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        m_approvalActive = true;
        m_approvalAddr   = Widen(clientAddr);
    }
    m_approvalDecision.store(0, std::memory_order_release);
    if (m_hwnd)
        PostMessageW(m_hwnd, kApprovalMsg, 0, 0); // UI thread'de göster/parlat
}

int StatusWindow::takeApprovalDecision()
{
    return m_approvalDecision.exchange(0, std::memory_order_acq_rel);
}

// ---------------------------------------------------------------------------
// UI thread
// ---------------------------------------------------------------------------

void StatusWindow::threadProc(std::promise<bool> ready)
{
    SetThreadDescription(GetCurrentThread(), L"Status-UI");

    if (!createWindow() || !createGraphics()) {
        releaseGraphics();
        if (m_hwnd) {
            DestroyWindow(m_hwnd);
            m_hwnd = nullptr;
        }
        ready.set_value(false);
        return;
    }

    SetTimer(m_hwnd, 1, 500, nullptr); // stats refresh + copied-hint expiry
    ShowWindow(m_hwnd, SW_SHOW);
    ready.set_value(true);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    releaseGraphics();
    if (m_classAtom) {
        UnregisterClassW(kClassName, GetModuleHandleW(nullptr));
        m_classAtom = 0;
    }
}

bool StatusWindow::createWindow()
{
    const HINSTANCE instance = GetModuleHandleW(nullptr);

    WNDCLASSEXW wc = {};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = &StatusWindow::wndProcThunk;
    wc.hInstance     = instance;
    wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon         = LoadIconW(instance, MAKEINTRESOURCEW(IDI_APPICON));
    wc.lpszClassName = kClassName;
    m_classAtom = RegisterClassExW(&wc);
    if (m_classAtom == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        return false;

    // Önce sistem DPI'ıyla boyutlandır; pencere kurulunca kendi
    // monitörünün DPI'ı okunur ve applyDpi ile düzeltilir.
    const UINT sysDpi = GetDpiForSystem();
    m_scale = static_cast<float>(sysDpi) / 96.0f;
    const int w = static_cast<int>(kW * m_scale + 0.5f);
    const int h = static_cast<int>(kH * m_scale + 0.5f);
    const int screenW = GetSystemMetrics(SM_CXSCREEN);
    m_hwnd = CreateWindowExW(0, kClassName, L"MidnightSun Sunucu", WS_POPUP,
                             screenW - w - 48, 48, w, h,
                             nullptr, nullptr, instance, this);
    if (!m_hwnd)
        return false;

    const UINT dpi = GetDpiForWindow(m_hwnd);
    if (dpi != sysDpi)
        applyDpi(dpi, nullptr);

    const BOOL dark = TRUE;
    DwmSetWindowAttribute(m_hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
    const int corner = 2; // DWMWCP_ROUND
    DwmSetWindowAttribute(m_hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner));
    return true;
}

bool StatusWindow::createGraphics()
{
    HRESULT hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
                                   m_d2dFactory.GetAddressOf());
    if (FAILED(hr))
        return false;
    RECT rc = {};
    GetClientRect(m_hwnd, &rc);
    hr = m_d2dFactory->CreateHwndRenderTarget(
        D2D1::RenderTargetProperties(),
        D2D1::HwndRenderTargetProperties(
            m_hwnd, D2D1::SizeU(static_cast<UINT32>(rc.right - rc.left),
                                static_cast<UINT32>(rc.bottom - rc.top))),
        &m_rt);
    if (FAILED(hr))
        return false;
    // DIP koordinatlarıyla çizim: hedefin DPI'ı pencerenin gerçek DPI'ı
    // olunca D2D tüm geometriyi kendisi ölçekler.
    const float dpi = 96.0f * m_scale;
    m_rt->SetDpi(dpi, dpi);

    hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                             reinterpret_cast<IUnknown**>(m_dwrite.GetAddressOf()));
    if (FAILED(hr))
        return false;

    auto makeFormat = [&](const wchar_t* family, float size, DWRITE_FONT_WEIGHT weight,
                          ComPtr<IDWriteTextFormat>& out) {
        return SUCCEEDED(m_dwrite->CreateTextFormat(
            family, nullptr, weight, DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL, size, L"tr-tr", &out));
    };
    if (!makeFormat(L"Segoe UI", 22.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, m_fmtTitle) ||
        !makeFormat(L"Segoe UI", 14.0f, DWRITE_FONT_WEIGHT_NORMAL, m_fmtBody) ||
        !makeFormat(L"Segoe UI", 12.0f, DWRITE_FONT_WEIGHT_NORMAL, m_fmtSmall) ||
        !makeFormat(L"Consolas", 34.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, m_fmtCode))
        return false;
    m_fmtCode->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);

    return SUCCEEDED(m_rt->CreateSolidColorBrush(Col(kColText), &m_brush));
}

void StatusWindow::releaseGraphics()
{
    m_brush.Reset();
    m_fmtCode.Reset();
    m_fmtSmall.Reset();
    m_fmtBody.Reset();
    m_fmtTitle.Reset();
    m_dwrite.Reset();
    m_rt.Reset();
    m_d2dFactory.Reset();
}

void StatusWindow::applyDpi(UINT dpi, const RECT* suggested)
{
    m_scale = static_cast<float>(dpi) / 96.0f;
    const int w = static_cast<int>(kW * m_scale + 0.5f);
    const int h = static_cast<int>(kH * m_scale + 0.5f);
    if (suggested)
        SetWindowPos(m_hwnd, nullptr, suggested->left, suggested->top, w, h,
                     SWP_NOZORDER | SWP_NOACTIVATE);
    else
        SetWindowPos(m_hwnd, nullptr, 0, 0, w, h,
                     SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOMOVE);
    if (m_rt) {
        m_rt->Resize(D2D1::SizeU(static_cast<UINT32>(w), static_cast<UINT32>(h)));
        m_rt->SetDpi(96.0f * m_scale, 96.0f * m_scale);
    }
}

// ---------------------------------------------------------------------------
// Tray icon
// ---------------------------------------------------------------------------

void StatusWindow::addTrayIcon()
{
    if (m_inTray)
        return;
    NOTIFYICONDATAW nid = {};
    nid.cbSize           = sizeof(nid);
    nid.hWnd             = m_hwnd;
    nid.uID              = 1;
    nid.uFlags           = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = kTrayMsg;
    nid.hIcon = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_APPICON));
    wcscpy_s(nid.szTip, L"MidnightSun — yayın sürüyor");
    Shell_NotifyIconW(NIM_ADD, &nid);
    m_inTray = true;
    ShowWindow(m_hwnd, SW_HIDE);
}

void StatusWindow::removeTrayIcon()
{
    if (!m_inTray)
        return;
    NOTIFYICONDATAW nid = {};
    nid.cbSize = sizeof(nid);
    nid.hWnd   = m_hwnd;
    nid.uID    = 1;
    Shell_NotifyIconW(NIM_DELETE, &nid);
    m_inTray = false;
}

void StatusWindow::restoreFromTray()
{
    removeTrayIcon();
    ShowWindow(m_hwnd, SW_SHOW);
    SetForegroundWindow(m_hwnd);
}

void StatusWindow::showTrayMenu()
{
    HMENU menu = CreatePopupMenu();
    if (!menu)
        return;
    AppendMenuW(menu, MF_STRING, kMenuRestore, L"Pencereyi göster");
    AppendMenuW(menu, MF_STRING, kMenuCopyCode, L"Eşleşme kodunu kopyala");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kMenuStop, L"Yayını durdur");
    POINT pt;
    GetCursorPos(&pt);
    // TrackPopupMenu'nun kapanabilmesi için pencere öne alınmalı (MS
    // dokümanlarındaki bilinen kural).
    SetForegroundWindow(m_hwnd);
    const UINT cmd = static_cast<UINT>(TrackPopupMenu(
        menu, TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY, pt.x, pt.y, 0,
        m_hwnd, nullptr));
    DestroyMenu(menu);
    switch (cmd) {
    case kMenuRestore:  restoreFromTray(); break;
    case kMenuCopyCode: copyCodeToClipboard(); break;
    case kMenuStop:     PostMessageW(m_hwnd, WM_CLOSE, 0, 0); break;
    default: break;
    }
}

void StatusWindow::copyCodeToClipboard()
{
    if (m_code.empty() || !OpenClipboard(m_hwnd))
        return;
    EmptyClipboard();
    const size_t bytes = (m_code.size() + 1) * sizeof(wchar_t);
    if (HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, bytes)) {
        if (void* p = GlobalLock(mem)) {
            memcpy(p, m_code.c_str(), bytes);
            GlobalUnlock(mem);
            SetClipboardData(CF_UNICODETEXT, mem); // clipboard owns mem now
        } else {
            GlobalFree(mem);
        }
    }
    CloseClipboard();
    m_copiedUntil = GetTickCount64() + 1500;
    InvalidateRect(m_hwnd, nullptr, FALSE);
}

// ---------------------------------------------------------------------------
// Window procedure
// ---------------------------------------------------------------------------

LRESULT CALLBACK StatusWindow::wndProcThunk(HWND hwnd, UINT msg, WPARAM wParam,
                                            LPARAM lParam)
{
    if (msg == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(create->lpCreateParams));
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
    auto* self = reinterpret_cast<StatusWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (self)
        return self->handleMessage(hwnd, msg, wParam, lParam);
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT StatusWindow::handleMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(hwnd, &ps);
        paint();
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_ERASEBKGND:
        return 1;

    case WM_DPICHANGED: {
        const RECT* suggested = reinterpret_cast<const RECT*>(lParam);
        applyDpi(HIWORD(wParam), suggested);
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }

    case WM_NCHITTEST: {
        POINT pt = {static_cast<LONG>(GET_X_LPARAM(lParam)),
                    static_cast<LONG>(GET_Y_LPARAM(lParam))};
        ScreenToClient(hwnd, &pt);
        const float x = static_cast<float>(pt.x) / m_scale;
        const float y = static_cast<float>(pt.y) / m_scale;
        bool prompt;
        {
            std::lock_guard<std::mutex> lock(m_stateMutex);
            prompt = m_approvalActive;
        }
        if (PointIn(kCloseRc, x, y) || PointIn(kMinRc, x, y) ||
            PointIn(kCodeRc, x, y) ||
            (prompt && (PointIn(kAllowRc, x, y) || PointIn(kDenyRc, x, y))))
            return HTCLIENT;
        return HTCAPTION; // the whole window drags (it is small)
    }

    case WM_MOUSEMOVE: {
        const float x = static_cast<float>(GET_X_LPARAM(lParam)) / m_scale;
        const float y = static_cast<float>(GET_Y_LPARAM(lParam)) / m_scale;
        const bool close = PointIn(kCloseRc, x, y);
        const bool minim = PointIn(kMinRc, x, y);
        const bool code  = PointIn(kCodeRc, x, y);
        const bool allow = PointIn(kAllowRc, x, y);
        const bool deny  = PointIn(kDenyRc, x, y);
        if (close != m_hoverClose || minim != m_hoverMin || code != m_hoverCode ||
            allow != m_hoverAllow || deny != m_hoverDeny) {
            m_hoverClose = close;
            m_hoverMin   = minim;
            m_hoverCode  = code;
            m_hoverAllow = allow;
            m_hoverDeny  = deny;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    }

    case WM_LBUTTONDOWN: {
        const float x = static_cast<float>(GET_X_LPARAM(lParam)) / m_scale;
        const float y = static_cast<float>(GET_Y_LPARAM(lParam)) / m_scale;
        if (PointIn(kCloseRc, x, y)) {
            PostMessageW(hwnd, WM_CLOSE, 0, 0);
            return 0;
        }
        if (PointIn(kMinRc, x, y)) {
            addTrayIcon();
            return 0;
        }
        bool prompt;
        {
            std::lock_guard<std::mutex> lock(m_stateMutex);
            prompt = m_approvalActive;
        }
        if (prompt && (PointIn(kAllowRc, x, y) || PointIn(kDenyRc, x, y))) {
            const bool allow = PointIn(kAllowRc, x, y);
            {
                std::lock_guard<std::mutex> lock(m_stateMutex);
                m_approvalActive = false;
            }
            m_approvalDecision.store(allow ? 1 : -1, std::memory_order_release);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        if (!prompt && PointIn(kCodeRc, x, y)) {
            copyCodeToClipboard();
            return 0;
        }
        return 0;
    }

    case WM_TIMER:
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;

    default:
        break;
    }

    if (msg == kTrayMsg) {
        if (LOWORD(lParam) == WM_LBUTTONUP)
            restoreFromTray();
        else if (LOWORD(lParam) == WM_RBUTTONUP)
            showTrayMenu();
        return 0;
    }
    if (msg == kApprovalMsg) {
        // Onay istemi geldi: tepsideyse pencereyi geri getir, görev
        // çubuğunda parlat, hemen yeniden çiz.
        if (m_inTray)
            restoreFromTray();
        FLASHWINFO flash = {sizeof(flash), m_hwnd, FLASHW_ALL | FLASHW_TIMERNOFG, 0, 0};
        FlashWindowEx(&flash);
        InvalidateRect(m_hwnd, nullptr, FALSE);
        return 0;
    }
    if (msg == WM_CLOSE) {
        // The user closing this window means "stop the server".
        removeTrayIcon();
        m_closeRequested.store(true, std::memory_order_release);
        DestroyWindow(hwnd);
        return 0;
    }
    if (msg == WM_DESTROY) {
        KillTimer(hwnd, 1);
        removeTrayIcon();
        m_hwnd = nullptr;
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// ---------------------------------------------------------------------------
// Painting
// ---------------------------------------------------------------------------

void StatusWindow::paint()
{
    if (!m_rt)
        return;

    // Snapshot the live state once per paint.
    bool         streaming, approval;
    std::wstring clientAddr, approvalAddr;
    float        kbps, rtt, loss;
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        streaming    = m_streaming;
        clientAddr   = m_clientAddr;
        approval     = m_approvalActive;
        approvalAddr = m_approvalAddr;
        kbps = m_kbps; rtt = m_rttMs; loss = m_lossPct;
    }

    auto drawStr = [&](const std::wstring& s, IDWriteTextFormat* fmt,
                       const D2D1_RECT_F& rc) {
        m_rt->DrawTextW(s.c_str(), static_cast<UINT32>(s.size()), fmt, rc,
                        m_brush.Get());
    };

    m_rt->BeginDraw();
    m_rt->Clear(Col(kColBg));

    // --- header ---
    m_brush->SetColor(Col(kColText));
    drawStr(L"MidnightSun", m_fmtTitle.Get(), D2D1::RectF(28.0f, 14.0f, 320.0f, 44.0f));
    m_brush->SetColor(Col(kColDim));
    drawStr(L"Sunucu", m_fmtSmall.Get(), D2D1::RectF(30.0f, 42.0f, 320.0f, 58.0f));

    // Küçült düğmesi: tepsiye indirir.
    m_brush->SetColor(Col(m_hoverMin ? kColAccent : kColDim));
    const float my = (kMinRc.top + kMinRc.bottom) / 2.0f + 4.0f;
    const float mx = (kMinRc.left + kMinRc.right) / 2.0f;
    m_rt->DrawLine(D2D1::Point2F(mx - 6.0f, my), D2D1::Point2F(mx + 6.0f, my),
                   m_brush.Get(), 1.6f);

    m_brush->SetColor(Col(m_hoverClose ? kColDanger : kColDim));
    const float cx = (kCloseRc.left + kCloseRc.right) / 2.0f;
    const float cy = (kCloseRc.top + kCloseRc.bottom) / 2.0f;
    m_rt->DrawLine(D2D1::Point2F(cx - 6.0f, cy - 6.0f), D2D1::Point2F(cx + 6.0f, cy + 6.0f),
                   m_brush.Get(), 1.6f);
    m_rt->DrawLine(D2D1::Point2F(cx - 6.0f, cy + 6.0f), D2D1::Point2F(cx + 6.0f, cy - 6.0f),
                   m_brush.Get(), 1.6f);

    // --- state pill ---
    std::wstring state = streaming ? L"Yayında" : L"İstemci bekleniyor";
    if (streaming && !clientAddr.empty())
        state += L"  —  " + clientAddr;
    m_brush->SetColor(Col(streaming ? kColLive : kColWaiting));
    m_rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(38.0f, 88.0f), 5.0f, 5.0f),
                      m_brush.Get());
    m_brush->SetColor(Col(kColText));
    drawStr(state, m_fmtBody.Get(), D2D1::RectF(54.0f, 78.0f, 428.0f, 100.0f));

    if (approval) {
        // --- bağlantı onayı istemi (kod alanının yerini alır) ---
        const D2D1_ROUNDED_RECT promptRr = {kPromptRc, 10.0f, 10.0f};
        m_brush->SetColor(Col(kColPanel));
        m_rt->FillRoundedRectangle(promptRr, m_brush.Get());
        m_brush->SetColor(Col(kColWaiting));
        m_rt->DrawRoundedRectangle(promptRr, m_brush.Get(), 1.5f);

        m_brush->SetColor(Col(kColText));
        drawStr(L"Bağlantı isteği", m_fmtBody.Get(),
                D2D1::RectF(kPromptRc.left + 20.0f, kPromptRc.top + 16.0f,
                            kPromptRc.right - 20.0f, kPromptRc.top + 40.0f));
        std::wstring who = approvalAddr + L" bu yayını izlemek istiyor.";
        m_brush->SetColor(Col(kColDim));
        drawStr(who, m_fmtBody.Get(),
                D2D1::RectF(kPromptRc.left + 20.0f, kPromptRc.top + 44.0f,
                            kPromptRc.right - 20.0f, kPromptRc.top + 92.0f));

        const D2D1_ROUNDED_RECT allowRr = {kAllowRc, 8.0f, 8.0f};
        m_brush->SetColor(Col(m_hoverAllow ? kColLive : kColLive, m_hoverAllow ? 1.0f : 0.85f));
        m_rt->FillRoundedRectangle(allowRr, m_brush.Get());
        m_brush->SetColor(Col(0x0B0F13));
        m_fmtBody->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        drawStr(L"İzin ver", m_fmtBody.Get(),
                D2D1::RectF(kAllowRc.left, kAllowRc.top + 8.0f, kAllowRc.right,
                            kAllowRc.bottom));
        const D2D1_ROUNDED_RECT denyRr = {kDenyRc, 8.0f, 8.0f};
        m_brush->SetColor(Col(kColPanelHover));
        m_rt->FillRoundedRectangle(denyRr, m_brush.Get());
        m_brush->SetColor(Col(m_hoverDeny ? kColDanger : kColBorder));
        m_rt->DrawRoundedRectangle(denyRr, m_brush.Get(), 1.2f);
        m_brush->SetColor(Col(m_hoverDeny ? kColDanger : kColText));
        drawStr(L"Reddet", m_fmtBody.Get(),
                D2D1::RectF(kDenyRc.left, kDenyRc.top + 8.0f, kDenyRc.right,
                            kDenyRc.bottom));
        m_fmtBody->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    } else {
        // --- pairing code (click to copy) ---
        m_brush->SetColor(Col(kColDim));
        drawStr(L"EŞLEŞME KODU", m_fmtSmall.Get(),
                D2D1::RectF(32.0f, 110.0f, 300.0f, 126.0f));

        const D2D1_ROUNDED_RECT codeRr = {kCodeRc, 10.0f, 10.0f};
        m_brush->SetColor(Col(m_hoverCode ? kColPanelHover : kColPanel));
        m_rt->FillRoundedRectangle(codeRr, m_brush.Get());
        m_brush->SetColor(Col(kColBorder));
        m_rt->DrawRoundedRectangle(codeRr, m_brush.Get(), 1.0f);

        if (m_code.empty()) {
            m_brush->SetColor(Col(kColDim));
            drawStr(L"— yerel IP bulunamadı —", m_fmtBody.Get(),
                    D2D1::RectF(kCodeRc.left, kCodeRc.top + 28.0f, kCodeRc.right,
                                kCodeRc.bottom));
        } else {
            m_brush->SetColor(Col(kColAccent));
            drawStr(m_code, m_fmtCode.Get(),
                    D2D1::RectF(kCodeRc.left, kCodeRc.top + 20.0f, kCodeRc.right,
                                kCodeRc.bottom));
        }
        const bool showCopied = GetTickCount64() < m_copiedUntil;
        m_brush->SetColor(Col(showCopied ? kColLive : kColDim,
                              showCopied ? 1.0f : 0.8f));
        drawStr(showCopied ? L"Kopyalandı ✓" : L"Kopyalamak için tıkla",
                m_fmtSmall.Get(), D2D1::RectF(32.0f, 214.0f, 428.0f, 232.0f));

        // --- address + discovery hint ---
        std::wstring addrLine = L"Adres: " + m_address +
                                L"   ·   Aynı ağdaki istemcilerde otomatik görünür";
        m_brush->SetColor(Col(kColDim));
        drawStr(addrLine, m_fmtSmall.Get(), D2D1::RectF(32.0f, 244.0f, 428.0f, 262.0f));
    }

    // --- divider + stats ---
    m_brush->SetColor(Col(kColBorder));
    m_rt->DrawLine(D2D1::Point2F(32.0f, 284.0f), D2D1::Point2F(428.0f, 284.0f),
                   m_brush.Get(), 1.0f);

    wchar_t stats[160];
    if (streaming)
        swprintf_s(stats, L"%.0f kbps   ·   rtt %.1f ms   ·   kayıp %%%.2f",
                   static_cast<double>(kbps), static_cast<double>(rtt),
                   static_cast<double>(loss));
    else
        swprintf_s(stats, L"İstemciyi başlatın; kodu girin ya da listeden seçin.");
    m_brush->SetColor(Col(kColText, 0.9f));
    drawStr(stats, m_fmtBody.Get(), D2D1::RectF(32.0f, 298.0f, 428.0f, 322.0f));

    m_brush->SetColor(Col(kColDim, 0.8f));
    drawStr(L"— tepsiye küçültür   ·   Pencereyi kapatmak yayını durdurur",
            m_fmtSmall.Get(),
            D2D1::RectF(32.0f, static_cast<float>(kH) - 34.0f, 428.0f,
                        static_cast<float>(kH) - 14.0f));

    const HRESULT hr = m_rt->EndDraw();
    if (hr == static_cast<HRESULT>(D2DERR_RECREATE_TARGET)) {
        releaseGraphics();
        createGraphics();
    }
}
