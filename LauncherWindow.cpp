// LauncherWindow.cpp -- see LauncherWindow.h for the design overview.

#include "LauncherWindow.h"
#include "resource.h"

#include <ws2tcpip.h>
#include <dwmapi.h>
#include <windowsx.h> // GET_X_LPARAM / GET_Y_LPARAM

#include <cmath>    // cosf/sinf (dişli simgesi)
#include <cstdio>
#include <cstdlib>  // strtol
#include <cwchar>   // wcstol / swprintf_s
#include <cwctype>
#include <iterator> // std::size

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "user32.lib")

namespace {

constexpr wchar_t kClassName[] = L"MidnightSunLauncher";

// --- geometry (DIP; pencere DPI'a göre ölçeklenir) ---------------------------
constexpr int kW = 640;
constexpr int kH = 600;

constexpr D2D1_RECT_F kHeaderRc  = {0.0f, 0.0f, 640.0f, 72.0f};
constexpr D2D1_RECT_F kGearRc    = {548.0f, 16.0f, 580.0f, 48.0f};
constexpr D2D1_RECT_F kCloseRc   = {592.0f, 16.0f, 624.0f, 48.0f};
constexpr D2D1_RECT_F kHostRc    = {32.0f, 84.0f, 608.0f, 140.0f};
constexpr D2D1_RECT_F kListRc    = {32.0f, 178.0f, 608.0f, 390.0f};
constexpr D2D1_RECT_F kInputRc   = {32.0f, 430.0f, 452.0f, 476.0f};
constexpr D2D1_RECT_F kConnectRc = {468.0f, 430.0f, 608.0f, 476.0f};
constexpr float       kRowH      = 50.0f;
constexpr size_t      kMaxRows   = 4;
constexpr size_t      kMaxInput  = 21; // "XXXX-XXXX-XXXX" or "255.255.255.255:65535"

// --- ayarlar sayfası geometrisi ----------------------------------------------
constexpr float kSetLabelX  = 48.0f;
constexpr float kSetCtrlL   = 300.0f;
constexpr float kSetCtrlR   = 560.0f;
constexpr float kSetRowH    = 52.0f;
constexpr float kSetTop     = 116.0f;
// alan sırası: 0 bitrate, 1 çözünürlük, 2 fps, 3 port, 4 psk, 5 onay
inline D2D1_RECT_F SetCtrlRect(int index)
{
    const float top = kSetTop + static_cast<float>(index) * kSetRowH;
    return {kSetCtrlL, top, kSetCtrlR, top + 38.0f};
}
constexpr D2D1_RECT_F kCancelRc = {332.0f, 520.0f, 460.0f, 560.0f};
constexpr D2D1_RECT_F kSaveRc   = {476.0f, 520.0f, 608.0f, 560.0f};

constexpr uint32_t kHeightOptions[] = {0, 720, 900, 1080, 1440, 2160};
constexpr uint32_t kFpsOptions[]    = {30, 60, 120, 144};
constexpr size_t   kMaxPskLen       = 48;

// --- palette ----------------------------------------------------------------
constexpr UINT32 kColBg         = 0x0E1116;
constexpr UINT32 kColPanel      = 0x161B22;
constexpr UINT32 kColPanelHover = 0x1F2833;
constexpr UINT32 kColBorder     = 0x2A313B;
constexpr UINT32 kColText       = 0xE8EAED;
constexpr UINT32 kColDim        = 0x8B95A1;
constexpr UINT32 kColAccent     = 0x3FA9F5;
constexpr UINT32 kColAccentHov  = 0x63BBFA;
constexpr UINT32 kColDanger     = 0xE06C60;
constexpr UINT32 kColLive       = 0x53C783; // canlı sunucu yeşili
constexpr UINT32 kColSun        = 0xF2A65A; // logo güneşi
constexpr UINT32 kColTag        = 0x2C3A2E; // PSK tag backdrop (greenish)

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

// Fallback definitions for older SDKs; harmless where already defined.
#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif
#ifndef DWMWA_WINDOW_CORNER_PREFERENCE
#define DWMWA_WINDOW_CORNER_PREFERENCE 33
#endif

std::string Narrow(const std::wstring& wide)
{
    if (wide.empty())
        return {};
    char buf[256] = {};
    WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, buf, sizeof(buf) - 1,
                        nullptr, nullptr);
    return buf;
}

std::wstring Widen(const std::string& utf8)
{
    if (utf8.empty())
        return {};
    wchar_t buf[256] = {};
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, buf, 255);
    return buf;
}

std::wstring FormatUInt(uint32_t value)
{
    wchar_t buf[16];
    swprintf_s(buf, L"%u", value);
    return buf;
}

} // namespace

// ---------------------------------------------------------------------------
// Lifecycle / modal loop
// ---------------------------------------------------------------------------

LauncherWindow::~LauncherWindow()
{
    m_stopDiscovery.store(true, std::memory_order_release);
    if (m_discoveryThread.joinable())
        m_discoveryThread.join();
    releaseGraphics();
    if (m_hwnd)
        DestroyWindow(m_hwnd);
    if (m_classAtom)
        UnregisterClassW(kClassName, GetModuleHandleW(nullptr));
}

LaunchChoice LauncherWindow::run(AppConfig& config)
{
    m_config = &config;

    if (!createWindow() || !createGraphics())
        return m_choice; // action=Cancel; the caller quits

    m_stopDiscovery.store(false, std::memory_order_release);
    m_discoveryThread = std::thread(&LauncherWindow::discoveryThreadProc, this);

    SetTimer(m_hwnd, 1, 400, nullptr); // caret blink + scan spinner
    ShowWindow(m_hwnd, SW_SHOW);
    SetForegroundWindow(m_hwnd);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    m_stopDiscovery.store(true, std::memory_order_release);
    if (m_discoveryThread.joinable())
        m_discoveryThread.join();
    releaseGraphics();
    if (m_hwnd) { // WM_DESTROY already ran; handle is gone
        m_hwnd = nullptr;
    }
    if (m_classAtom) {
        UnregisterClassW(kClassName, GetModuleHandleW(nullptr));
        m_classAtom = 0;
    }
    m_config = nullptr;
    return m_choice;
}

bool LauncherWindow::createWindow()
{
    const HINSTANCE instance = GetModuleHandleW(nullptr);

    WNDCLASSEXW wc = {};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = &LauncherWindow::wndProcThunk;
    wc.hInstance     = instance;
    wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon         = LoadIconW(instance, MAKEINTRESOURCEW(IDI_APPICON));
    wc.lpszClassName = kClassName;
    m_classAtom = RegisterClassExW(&wc);
    if (m_classAtom == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        return false;

    const UINT sysDpi = GetDpiForSystem();
    m_scale = static_cast<float>(sysDpi) / 96.0f;
    const int w = static_cast<int>(kW * m_scale + 0.5f);
    const int h = static_cast<int>(kH * m_scale + 0.5f);
    const int screenW = GetSystemMetrics(SM_CXSCREEN);
    const int screenH = GetSystemMetrics(SM_CYSCREEN);
    m_hwnd = CreateWindowExW(0, kClassName, L"MidnightSun", WS_POPUP,
                             (screenW - w) / 2, (screenH - h) / 2, w, h,
                             nullptr, nullptr, instance, this);
    if (!m_hwnd)
        return false;

    const UINT dpi = GetDpiForWindow(m_hwnd);
    if (dpi != sysDpi)
        applyDpi(dpi, nullptr);

    // Best effort polish on Win11: rounded corners + dark chrome.
    const BOOL dark = TRUE;
    DwmSetWindowAttribute(m_hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
    const int corner = 2; // DWMWCP_ROUND
    DwmSetWindowAttribute(m_hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner));
    return true;
}

bool LauncherWindow::createGraphics()
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
    // DIP koordinatlarıyla çizim (bkz. StatusWindow::createGraphics).
    m_rt->SetDpi(96.0f * m_scale, 96.0f * m_scale);

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
    if (!makeFormat(L"Segoe UI", 26.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, m_fmtTitle) ||
        !makeFormat(L"Segoe UI", 15.0f, DWRITE_FONT_WEIGHT_NORMAL, m_fmtBody) ||
        !makeFormat(L"Segoe UI", 12.0f, DWRITE_FONT_WEIGHT_NORMAL, m_fmtSmall) ||
        !makeFormat(L"Consolas", 20.0f, DWRITE_FONT_WEIGHT_NORMAL, m_fmtCode))
        return false;

    return SUCCEEDED(m_rt->CreateSolidColorBrush(Col(kColText), &m_brush));
}

void LauncherWindow::releaseGraphics()
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

void LauncherWindow::applyDpi(UINT dpi, const RECT* suggested)
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
// Discovery worker
// ---------------------------------------------------------------------------

void LauncherWindow::discoveryThreadProc()
{
    SetThreadDescription(GetCurrentThread(), L"Launcher-Discovery");
    while (!m_stopDiscovery.load(std::memory_order_acquire)) {
        std::vector<DiscoveredServer> found = DiscoverServers(900);
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_servers = std::move(found);
        }
        m_scannedOnce.store(true, std::memory_order_release);
        if (m_hwnd)
            InvalidateRect(m_hwnd, nullptr, FALSE);
        for (int i = 0; i < 7 && !m_stopDiscovery.load(std::memory_order_acquire); ++i)
            Sleep(100);
    }
}

std::vector<LauncherWindow::Row> LauncherWindow::currentRows()
{
    std::vector<Row> rows;
    std::lock_guard<std::mutex> lock(m_mutex);

    // Önce kayıtlı cihazlar (ağda görünüyorsa "canlı" işaretlenir)...
    for (size_t i = 0; i < m_config->savedDevices.size() && rows.size() < kMaxRows;
         ++i) {
        const SavedDevice& device = m_config->savedDevices[i];
        Row row;
        row.title      = Widen(device.name);
        row.ip         = device.ip;
        row.port       = device.port;
        row.saved      = true;
        row.savedIndex = i;
        for (const DiscoveredServer& server : m_servers) {
            if (server.ip == device.ip && server.port == device.port) {
                row.live = true;
                row.psk  = server.pskRequired;
                break;
            }
        }
        rows.push_back(std::move(row));
    }

    // ...sonra keşfedilen (henüz kaydedilmemiş) sunucular.
    for (const DiscoveredServer& server : m_servers) {
        if (rows.size() >= kMaxRows)
            break;
        bool alreadySaved = false;
        for (const SavedDevice& device : m_config->savedDevices) {
            if (server.ip == device.ip && server.port == device.port) {
                alreadySaved = true;
                break;
            }
        }
        if (alreadySaved)
            continue;
        Row row;
        row.title = Widen(server.hostname);
        row.ip    = server.ip;
        row.port  = server.port;
        row.live  = true;
        row.psk   = server.pskRequired;
        rows.push_back(std::move(row));
    }
    return rows;
}

// ---------------------------------------------------------------------------
// Window procedure
// ---------------------------------------------------------------------------

LRESULT CALLBACK LauncherWindow::wndProcThunk(HWND hwnd, UINT msg, WPARAM wParam,
                                              LPARAM lParam)
{
    if (msg == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(create->lpCreateParams));
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
    auto* self = reinterpret_cast<LauncherWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (self)
        return self->handleMessage(hwnd, msg, wParam, lParam);
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT LauncherWindow::handleMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
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
        return 1; // D2D paints everything; avoid flicker

    case WM_DPICHANGED: {
        const RECT* suggested = reinterpret_cast<const RECT*>(lParam);
        applyDpi(HIWORD(wParam), suggested);
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }

    case WM_NCHITTEST: {
        // Borderless window: the header doubles as the title bar (drag),
        // except over the buttons.
        POINT pt = {static_cast<LONG>(GET_X_LPARAM(lParam)),
                    static_cast<LONG>(GET_Y_LPARAM(lParam))};
        ScreenToClient(hwnd, &pt);
        const float x = static_cast<float>(pt.x) / m_scale;
        const float y = static_cast<float>(pt.y) / m_scale;
        if (PointIn(kCloseRc, x, y) || PointIn(kGearRc, x, y))
            return HTCLIENT;
        if (PointIn(kHeaderRc, x, y))
            return HTCAPTION;
        return HTCLIENT;
    }

    case WM_MOUSEMOVE: {
        const float x = static_cast<float>(GET_X_LPARAM(lParam)) / m_scale;
        const float y = static_cast<float>(GET_Y_LPARAM(lParam)) / m_scale;
        bool changed = false;
        const bool close = PointIn(kCloseRc, x, y);
        const bool gear  = PointIn(kGearRc, x, y);
        if (close != m_hoverClose || gear != m_hoverGear) {
            m_hoverClose = close;
            m_hoverGear  = gear;
            changed = true;
        }
        if (m_page == Page::Main) {
            const int  row     = hitTestRow(x, y);
            const bool rowDel  = row >= 0 && hitTestRowDelete(x, y, row);
            const bool connect = PointIn(kConnectRc, x, y);
            const bool host    = PointIn(kHostRc, x, y);
            if (row != m_hoverRow || rowDel != m_hoverRowDelete ||
                connect != m_hoverConnect || host != m_hoverHost) {
                m_hoverRow       = row;
                m_hoverRowDelete = rowDel;
                m_hoverConnect   = connect;
                m_hoverHost      = host;
                changed = true;
            }
        } else {
            int control = -1;
            for (int i = 0; i < 6; ++i) {
                if (PointIn(SetCtrlRect(i), x, y)) {
                    control = i;
                    break;
                }
            }
            if (PointIn(kSaveRc, x, y))
                control = 10;
            else if (PointIn(kCancelRc, x, y))
                control = 11;
            if (control != m_hoverControl) {
                m_hoverControl = control;
                changed = true;
            }
        }
        if (changed)
            InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }

    case WM_LBUTTONDOWN: {
        const float x = static_cast<float>(GET_X_LPARAM(lParam)) / m_scale;
        const float y = static_cast<float>(GET_Y_LPARAM(lParam)) / m_scale;
        if (PointIn(kCloseRc, x, y)) {
            DestroyWindow(hwnd);
            return 0;
        }
        if (PointIn(kGearRc, x, y)) {
            if (m_page == Page::Main)
                enterSettings();
            else
                m_page = Page::Main; // geri (kaydetmeden)
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        if (m_page == Page::Settings) {
            m_settingsError.clear();
            if (PointIn(kSaveRc, x, y)) {
                if (saveSettings()) {
                    m_page = Page::Main;
                }
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            if (PointIn(kCancelRc, x, y)) {
                m_page = Page::Main;
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            // Metin alanı odağı + cycler okları + onay anahtarı.
            m_focusField = kFieldNone;
            if (PointIn(SetCtrlRect(0), x, y))
                m_focusField = kFieldBitrate;
            else if (PointIn(SetCtrlRect(3), x, y))
                m_focusField = kFieldPort;
            else if (PointIn(SetCtrlRect(4), x, y))
                m_focusField = kFieldPsk;
            else if (PointIn(SetCtrlRect(1), x, y)) {
                const D2D1_RECT_F rc = SetCtrlRect(1);
                cycleHeight(x < (rc.left + rc.right) / 2.0f ? -1 : +1);
            } else if (PointIn(SetCtrlRect(2), x, y)) {
                const D2D1_RECT_F rc = SetCtrlRect(2);
                cycleFps(x < (rc.left + rc.right) / 2.0f ? -1 : +1);
            } else if (PointIn(SetCtrlRect(5), x, y)) {
                m_draftApproval = !m_draftApproval;
            }
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        if (PointIn(kHostRc, x, y)) {
            m_choice.action = LaunchAction::Host;
            DestroyWindow(hwnd);
            return 0;
        }
        if (PointIn(kConnectRc, x, y)) {
            attemptConnect();
            return 0;
        }
        const int row = hitTestRow(x, y);
        if (row >= 0) {
            std::vector<Row> rows = currentRows();
            if (row < static_cast<int>(rows.size())) {
                Row& hit = rows[static_cast<size_t>(row)];
                if (hit.saved && hitTestRowDelete(x, y, row)) {
                    // Kayıtlı cihazı unut: config'ten sil, dosyaya yazmayı
                    // çağırana bırak (configChanged).
                    if (RemoveSavedDevice(*m_config, hit.savedIndex)) {
                        m_choice.configChanged = true;
                        m_hoverRow             = -1;
                        m_hoverRowDelete       = false;
                        InvalidateRect(hwnd, nullptr, FALSE);
                    }
                    return 0;
                }
                choose(hit.ip, hit.port, Narrow(hit.title));
            }
        }
        return 0;
    }

    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE) {
            if (m_page == Page::Settings) {
                m_page = Page::Main; // ayarları kaydetmeden geri dön
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            DestroyWindow(hwnd);
            return 0;
        }
        if (wParam == VK_RETURN) {
            if (m_page == Page::Settings) {
                if (saveSettings())
                    m_page = Page::Main;
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            attemptConnect();
            return 0;
        }
        break;

    case WM_CHAR:
        if (m_page == Page::Settings)
            handleSettingsChar(static_cast<wchar_t>(wParam));
        else
            handleChar(static_cast<wchar_t>(wParam));
        return 0;

    case WM_TIMER:
        m_caretOn = !m_caretOn;
        ++m_spinnerPhase;
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;

    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        KillTimer(hwnd, 1);
        m_hwnd = nullptr;
        PostQuitMessage(0);
        return 0;

    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// ---------------------------------------------------------------------------
// Input handling
// ---------------------------------------------------------------------------

void LauncherWindow::handleChar(wchar_t c)
{
    if (c == 0x08) { // backspace
        if (!m_input.empty()) {
            m_input.pop_back();
            m_error.clear();
            InvalidateRect(m_hwnd, nullptr, FALSE);
        }
        return;
    }
    if (c == 0x16) { // Ctrl+V: paste the code from the clipboard
        if (OpenClipboard(m_hwnd)) {
            if (HANDLE data = GetClipboardData(CF_UNICODETEXT)) {
                if (const wchar_t* text = static_cast<const wchar_t*>(GlobalLock(data))) {
                    for (const wchar_t* p = text; *p && m_input.size() < kMaxInput; ++p)
                        handleChar(*p); // reuses the same filtering below
                    GlobalUnlock(data);
                }
            }
            CloseClipboard();
        }
        return;
    }

    const wchar_t upper = static_cast<wchar_t>(std::towupper(c));
    const bool valid = (upper >= L'0' && upper <= L'9') ||
                       (upper >= L'A' && upper <= L'Z') ||
                       upper == L'.' || upper == L':' || upper == L'-';
    if (!valid || m_input.size() >= kMaxInput)
        return;
    m_input.push_back(upper);
    m_error.clear();
    InvalidateRect(m_hwnd, nullptr, FALSE);
}

void LauncherWindow::handleSettingsChar(wchar_t c)
{
    std::wstring* target = nullptr;
    size_t maxLen = 0;
    bool digitsOnly = false;
    switch (m_focusField) {
    case kFieldBitrate: target = &m_editBitrate; maxLen = 6;  digitsOnly = true; break;
    case kFieldPort:    target = &m_editPort;    maxLen = 5;  digitsOnly = true; break;
    case kFieldPsk:     target = &m_editPsk;     maxLen = kMaxPskLen;            break;
    default: return;
    }

    if (c == 0x08) { // backspace
        if (!target->empty()) {
            target->pop_back();
            InvalidateRect(m_hwnd, nullptr, FALSE);
        }
        return;
    }
    if (c == 0x16) { // Ctrl+V (PSK yapıştırmak için kullanışlı)
        if (OpenClipboard(m_hwnd)) {
            if (HANDLE data = GetClipboardData(CF_UNICODETEXT)) {
                if (const wchar_t* text = static_cast<const wchar_t*>(GlobalLock(data))) {
                    for (const wchar_t* p = text; *p; ++p)
                        handleSettingsChar(*p);
                    GlobalUnlock(data);
                }
            }
            CloseClipboard();
        }
        return;
    }
    if (c < 0x20) // diğer kontrol karakterleri
        return;
    if (digitsOnly && (c < L'0' || c > L'9'))
        return;
    if (target->size() >= maxLen)
        return;
    target->push_back(c);
    InvalidateRect(m_hwnd, nullptr, FALSE);
}

void LauncherWindow::attemptConnect()
{
    if (m_input.empty()) {
        m_error = L"Bir eşleşme kodu ya da IP adresi girin.";
        InvalidateRect(m_hwnd, nullptr, FALSE);
        return;
    }

    const std::string text = Narrow(m_input);
    if (text.find('.') != std::string::npos) {
        // Manual "ip[:port]" entry.
        std::string ip   = text;
        uint16_t    port = m_config->serverPort ? m_config->serverPort : 9000;
        if (const size_t colon = text.find(':'); colon != std::string::npos) {
            ip = text.substr(0, colon);
            const long parsed = strtol(text.c_str() + colon + 1, nullptr, 10);
            if (parsed <= 0 || parsed > 65535) {
                m_error = L"Geçersiz port numarası.";
                InvalidateRect(m_hwnd, nullptr, FALSE);
                return;
            }
            port = static_cast<uint16_t>(parsed);
        }
        in_addr addr = {};
        if (inet_pton(AF_INET, ip.c_str(), &addr) != 1) {
            m_error = L"Geçersiz IP adresi.";
            InvalidateRect(m_hwnd, nullptr, FALSE);
            return;
        }
        choose(ip, port, std::string());
        return;
    }

    std::string ip;
    uint16_t    port = 0;
    if (!DecodePairingCode(text, ip, port)) {
        m_error = L"Kod çözülemedi — yazımı kontrol edin (12 karakter).";
        InvalidateRect(m_hwnd, nullptr, FALSE);
        return;
    }
    choose(ip, port, std::string());
}

void LauncherWindow::choose(std::string ip, uint16_t port, std::string name)
{
    m_choice.action = LaunchAction::Connect;
    m_choice.ip     = std::move(ip);
    m_choice.port   = port;
    m_choice.name   = std::move(name);
    DestroyWindow(m_hwnd);
}

// ---------------------------------------------------------------------------
// Settings page logic
// ---------------------------------------------------------------------------

void LauncherWindow::enterSettings()
{
    m_page          = Page::Settings;
    m_editBitrate   = FormatUInt(m_config->bitrateKbps);
    m_editPort      = FormatUInt(m_config->port);
    m_editPsk       = Widen(m_config->psk);
    m_draftHeight   = m_config->encodeHeight;
    m_draftFps      = m_config->fpsTarget;
    m_draftApproval = m_config->requireApproval;
    m_focusField    = kFieldNone;
    m_settingsError.clear();
}

bool LauncherWindow::saveSettings()
{
    const long bitrate = m_editBitrate.empty()
                             ? 0 : wcstol(m_editBitrate.c_str(), nullptr, 10);
    if (bitrate < 500 || bitrate > 500'000) {
        m_settingsError = L"Bit hızı 500 ile 500000 kbps arasında olmalı.";
        return false;
    }
    const long port = m_editPort.empty()
                          ? 0 : wcstol(m_editPort.c_str(), nullptr, 10);
    if (port <= 0 || port > 65535) {
        m_settingsError = L"Port 1 ile 65535 arasında olmalı.";
        return false;
    }

    m_config->bitrateKbps     = static_cast<uint32_t>(bitrate);
    m_config->port            = static_cast<uint16_t>(port);
    m_config->psk             = Narrow(m_editPsk);
    m_config->encodeHeight    = m_draftHeight;
    m_config->fpsTarget       = m_draftFps;
    m_config->requireApproval = m_draftApproval;
    m_choice.configChanged    = true;
    return true;
}

void LauncherWindow::cycleHeight(int direction)
{
    constexpr int n = static_cast<int>(std::size(kHeightOptions));
    // Mevcut değere eşit ya da ondan küçük en büyük seçeneğin dizini.
    int idx = 0;
    for (int i = 0; i < n; ++i)
        if (kHeightOptions[i] <= m_draftHeight)
            idx = i;
    idx = (idx + direction + n) % n;
    m_draftHeight = kHeightOptions[idx];
}

void LauncherWindow::cycleFps(int direction)
{
    constexpr int n = static_cast<int>(std::size(kFpsOptions));
    int idx = 0;
    for (int i = 0; i < n; ++i)
        if (kFpsOptions[i] <= m_draftFps)
            idx = i;
    idx = (idx + direction + n) % n;
    m_draftFps = kFpsOptions[idx];
}

// ---------------------------------------------------------------------------
// Hit testing
// ---------------------------------------------------------------------------

int LauncherWindow::hitTestRow(float x, float y)
{
    if (!PointIn(kListRc, x, y))
        return -1;
    const int row = static_cast<int>((y - kListRc.top) / kRowH);
    if (row < 0 || row >= static_cast<int>(kMaxRows))
        return -1;
    // Only rows that actually exist are clickable; verified again on click.
    std::vector<Row> rows = currentRows();
    return row < static_cast<int>(rows.size()) ? row : -1;
}

bool LauncherWindow::hitTestRowDelete(float x, float y, int row)
{
    if (row < 0)
        return false;
    const float top = kListRc.top + static_cast<float>(row) * kRowH;
    const D2D1_RECT_F del = {kListRc.right - 44.0f, top + 12.0f,
                             kListRc.right - 12.0f, top + kRowH - 12.0f};
    return PointIn(del, x, y);
}

// ---------------------------------------------------------------------------
// Painting
// ---------------------------------------------------------------------------

void LauncherWindow::drawChrome()
{
    auto drawStr = [&](const std::wstring& s, IDWriteTextFormat* fmt,
                       const D2D1_RECT_F& rc) {
        m_rt->DrawTextW(s.c_str(), static_cast<UINT32>(s.size()), fmt, rc,
                        m_brush.Get());
    };

    // Logo: ufuk çizgisi üstünde doğan güneş (ikonun mini hali).
    const float lx = 44.0f, ly = 38.0f, lr = 11.0f;
    m_brush->SetColor(Col(kColSun));
    m_rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(lx, ly), lr, lr), m_brush.Get());
    m_brush->SetColor(Col(kColBg));
    m_rt->FillRectangle(D2D1::RectF(lx - lr - 2.0f, ly + 3.0f, lx + lr + 2.0f,
                                    ly + lr + 4.0f),
                        m_brush.Get());
    m_brush->SetColor(Col(kColSun, 0.85f));
    m_rt->DrawLine(D2D1::Point2F(lx - lr - 2.0f, ly + 4.0f),
                   D2D1::Point2F(lx + lr + 2.0f, ly + 4.0f), m_brush.Get(), 1.6f);
    m_brush->SetColor(Col(kColSun, 0.35f));
    m_rt->DrawLine(D2D1::Point2F(lx - lr + 2.0f, ly + 9.0f),
                   D2D1::Point2F(lx + lr - 2.0f, ly + 9.0f), m_brush.Get(), 1.4f);

    m_brush->SetColor(Col(kColText));
    drawStr(L"MidnightSun", m_fmtTitle.Get(), D2D1::RectF(66.0f, 18.0f, 400.0f, 52.0f));
    m_brush->SetColor(Col(kColDim));
    drawStr(m_page == Page::Main ? L"Yayınla ya da bağlan" : L"Ayarlar",
            m_fmtSmall.Get(), D2D1::RectF(68.0f, 50.0f, 400.0f, 68.0f));

    // Dişli / geri düğmesi.
    const float gx = (kGearRc.left + kGearRc.right) / 2.0f;
    const float gy = (kGearRc.top + kGearRc.bottom) / 2.0f;
    m_brush->SetColor(Col(m_hoverGear ? kColAccent : kColDim));
    if (m_page == Page::Main) {
        // Dişli: çember + 8 kısa diş.
        m_rt->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(gx, gy), 6.5f, 6.5f),
                          m_brush.Get(), 1.6f);
        m_rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(gx, gy), 2.2f, 2.2f),
                          m_brush.Get());
        for (int i = 0; i < 8; ++i) {
            const float a  = static_cast<float>(i) * 0.785398f; // 45°
            const float ca = cosf(a), sa = sinf(a);
            m_rt->DrawLine(D2D1::Point2F(gx + ca * 7.5f, gy + sa * 7.5f),
                           D2D1::Point2F(gx + ca * 10.5f, gy + sa * 10.5f),
                           m_brush.Get(), 1.8f);
        }
    } else {
        // Geri oku.
        m_rt->DrawLine(D2D1::Point2F(gx + 4.0f, gy - 7.0f),
                       D2D1::Point2F(gx - 4.0f, gy), m_brush.Get(), 1.8f);
        m_rt->DrawLine(D2D1::Point2F(gx - 4.0f, gy),
                       D2D1::Point2F(gx + 4.0f, gy + 7.0f), m_brush.Get(), 1.8f);
    }

    // Close "button": two crossing lines, hover tint.
    m_brush->SetColor(Col(m_hoverClose ? kColDanger : kColDim));
    const float cx = (kCloseRc.left + kCloseRc.right) / 2.0f;
    const float cy = (kCloseRc.top + kCloseRc.bottom) / 2.0f;
    m_rt->DrawLine(D2D1::Point2F(cx - 6.0f, cy - 6.0f), D2D1::Point2F(cx + 6.0f, cy + 6.0f),
                   m_brush.Get(), 1.6f);
    m_rt->DrawLine(D2D1::Point2F(cx - 6.0f, cy + 6.0f), D2D1::Point2F(cx + 6.0f, cy - 6.0f),
                   m_brush.Get(), 1.6f);
}

void LauncherWindow::drawRow(const Row& row, const D2D1_RECT_F& rect, bool hovered,
                             bool hoveredDelete)
{
    auto drawStr = [&](const std::wstring& s, IDWriteTextFormat* fmt,
                       const D2D1_RECT_F& rc) {
        m_rt->DrawTextW(s.c_str(), static_cast<UINT32>(s.size()), fmt, rc,
                        m_brush.Get());
    };

    const D2D1_ROUNDED_RECT rr = {rect, 8.0f, 8.0f};
    m_brush->SetColor(Col(hovered ? kColPanelHover : kColPanel));
    m_rt->FillRoundedRectangle(rr, m_brush.Get());
    m_brush->SetColor(Col(kColBorder));
    m_rt->DrawRoundedRectangle(rr, m_brush.Get(), 1.0f);

    // Durum noktası: canlı = yeşil, kayıtlı-ama-çevrimdışı = soluk.
    m_brush->SetColor(row.live ? Col(kColLive) : Col(kColDim, 0.5f));
    m_rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(rect.left + 22.0f,
                                                  (rect.top + rect.bottom) / 2.0f),
                                    4.0f, 4.0f),
                      m_brush.Get());

    char addr[64];
    sprintf_s(addr, "%s:%u", row.ip.c_str(), row.port);
    std::wstring addrW = Widen(addr);
    if (row.saved)
        addrW += row.live ? L"  ·  çevrimiçi" : L"  ·  kayıtlı";

    D2D1_RECT_F nameRc = {rect.left + 40.0f, rect.top + 6.0f, rect.right - 120.0f,
                          rect.top + 28.0f};
    m_brush->SetColor(Col(kColText));
    drawStr(row.title, m_fmtBody.Get(), nameRc);

    D2D1_RECT_F addrRc = {rect.left + 40.0f, rect.top + 27.0f, rect.right - 120.0f,
                          rect.bottom - 2.0f};
    m_brush->SetColor(Col(kColDim));
    drawStr(addrW, m_fmtSmall.Get(), addrRc);

    if (row.psk) {
        const D2D1_RECT_F tag = {rect.right - 116.0f, rect.top + 13.0f,
                                 rect.right - 56.0f, rect.top + 35.0f};
        const D2D1_ROUNDED_RECT tagRr = {tag, 6.0f, 6.0f};
        m_brush->SetColor(Col(kColTag));
        m_rt->FillRoundedRectangle(tagRr, m_brush.Get());
        m_brush->SetColor(Col(kColDim));
        m_fmtSmall->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        drawStr(L"PSK", m_fmtSmall.Get(),
                D2D1::RectF(tag.left, tag.top + 3.0f, tag.right, tag.bottom));
        m_fmtSmall->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    }

    // Kayıtlı cihaz: sağda "×" (unut). Yalnızca satır hover'dayken görünür.
    if (row.saved && hovered) {
        const float dx = rect.right - 28.0f;
        const float dy = (rect.top + rect.bottom) / 2.0f;
        m_brush->SetColor(Col(hoveredDelete ? kColDanger : kColDim,
                              hoveredDelete ? 1.0f : 0.7f));
        m_rt->DrawLine(D2D1::Point2F(dx - 5.0f, dy - 5.0f),
                       D2D1::Point2F(dx + 5.0f, dy + 5.0f), m_brush.Get(), 1.6f);
        m_rt->DrawLine(D2D1::Point2F(dx - 5.0f, dy + 5.0f),
                       D2D1::Point2F(dx + 5.0f, dy - 5.0f), m_brush.Get(), 1.6f);
    }
}

void LauncherWindow::paintMain()
{
    auto drawStr = [&](const std::wstring& s, IDWriteTextFormat* fmt,
                       const D2D1_RECT_F& rc) {
        m_rt->DrawTextW(s.c_str(), static_cast<UINT32>(s.size()), fmt, rc,
                        m_brush.Get());
    };

    // --- host card: this machine as the stream source ---
    {
        const D2D1_ROUNDED_RECT hostRr = {kHostRc, 10.0f, 10.0f};
        m_brush->SetColor(Col(m_hoverHost ? kColPanelHover : kColPanel));
        m_rt->FillRoundedRectangle(hostRr, m_brush.Get());
        m_brush->SetColor(Col(m_hoverHost ? kColAccent : kColBorder));
        m_rt->DrawRoundedRectangle(hostRr, m_brush.Get(), m_hoverHost ? 1.5f : 1.0f);

        // "Broadcast" glyph: a filled dot with two arcs suggested by rings.
        const D2D1_POINT_2F center = D2D1::Point2F(kHostRc.left + 34.0f,
                                                   (kHostRc.top + kHostRc.bottom) / 2.0f);
        m_brush->SetColor(Col(kColAccent));
        m_rt->FillEllipse(D2D1::Ellipse(center, 4.0f, 4.0f), m_brush.Get());
        m_brush->SetColor(Col(kColAccent, 0.55f));
        m_rt->DrawEllipse(D2D1::Ellipse(center, 10.0f, 10.0f), m_brush.Get(), 1.4f);
        m_brush->SetColor(Col(kColAccent, 0.25f));
        m_rt->DrawEllipse(D2D1::Ellipse(center, 16.0f, 16.0f), m_brush.Get(), 1.4f);

        m_brush->SetColor(Col(kColText));
        drawStr(L"Bu bilgisayarı yayınla", m_fmtBody.Get(),
                D2D1::RectF(kHostRc.left + 64.0f, kHostRc.top + 8.0f,
                            kHostRc.right - 40.0f, kHostRc.top + 32.0f));
        m_brush->SetColor(Col(kColDim));
        drawStr(L"Ekranı paylaş; eşleşme kodu açılan pencerede görünür",
                m_fmtSmall.Get(),
                D2D1::RectF(kHostRc.left + 64.0f, kHostRc.top + 31.0f,
                            kHostRc.right - 40.0f, kHostRc.bottom - 6.0f));

        // Chevron on the right.
        m_brush->SetColor(Col(m_hoverHost ? kColAccent : kColDim));
        const float chx = kHostRc.right - 28.0f;
        const float chy = (kHostRc.top + kHostRc.bottom) / 2.0f;
        m_rt->DrawLine(D2D1::Point2F(chx - 4.0f, chy - 6.0f),
                       D2D1::Point2F(chx + 2.0f, chy), m_brush.Get(), 1.8f);
        m_rt->DrawLine(D2D1::Point2F(chx + 2.0f, chy),
                       D2D1::Point2F(chx - 4.0f, chy + 6.0f), m_brush.Get(), 1.8f);
    }

    // --- devices (saved + discovered) ---
    m_brush->SetColor(Col(kColDim));
    drawStr(L"CİHAZLAR", m_fmtSmall.Get(), D2D1::RectF(32.0f, 156.0f, 400.0f, 174.0f));

    std::vector<Row> rows = currentRows();
    if (rows.empty()) {
        // Empty state: scanning indicator (animated dots) or "not found".
        std::wstring text = m_scannedOnce.load(std::memory_order_acquire)
                                ? L"Ağda yayın bulunamadı — karşı tarafta yayını "
                                  L"başlatın ya da aşağıya kod girin"
                                : L"Ağ taranıyor";
        if (!m_scannedOnce.load(std::memory_order_acquire))
            text.append(static_cast<size_t>(m_spinnerPhase % 4), L'.');
        m_brush->SetColor(Col(kColDim));
        drawStr(text, m_fmtBody.Get(),
                D2D1::RectF(kListRc.left + 8.0f, kListRc.top + 16.0f,
                            kListRc.right, kListRc.top + 60.0f));
    } else {
        for (size_t i = 0; i < rows.size() && i < kMaxRows; ++i) {
            const float top = kListRc.top + static_cast<float>(i) * kRowH;
            const D2D1_RECT_F rect = {kListRc.left, top + 2.0f, kListRc.right,
                                      top + kRowH - 4.0f};
            const bool hovered = static_cast<int>(i) == m_hoverRow;
            drawRow(rows[i], rect, hovered, hovered && m_hoverRowDelete);
        }
    }

    // --- code / IP entry ---
    m_brush->SetColor(Col(kColDim));
    drawStr(L"EŞLEŞME KODU VEYA IP ADRESİ", m_fmtSmall.Get(),
            D2D1::RectF(32.0f, 408.0f, 460.0f, 426.0f));

    const D2D1_ROUNDED_RECT inputRr = {kInputRc, 8.0f, 8.0f};
    m_brush->SetColor(Col(kColPanel));
    m_rt->FillRoundedRectangle(inputRr, m_brush.Get());
    m_brush->SetColor(Col(m_error.empty() ? kColBorder : kColDanger));
    m_rt->DrawRoundedRectangle(inputRr, m_brush.Get(), 1.0f);

    const D2D1_RECT_F textRc = {kInputRc.left + 14.0f, kInputRc.top + 10.0f,
                                kInputRc.right - 14.0f, kInputRc.bottom - 8.0f};
    if (m_input.empty()) {
        m_brush->SetColor(Col(kColDim, 0.55f));
        drawStr(L"XXXX-XXXX-XXXX  ·  192.168.1.20", m_fmtCode.Get(), textRc);
    } else {
        m_brush->SetColor(Col(kColText));
        drawStr(m_input, m_fmtCode.Get(), textRc);
    }
    if (m_caretOn) {
        // Caret after the typed text (Consolas is monospaced: 20 px font
        // ≈ 11 px advance per glyph -- close enough for a caret).
        const float caretX = textRc.left + static_cast<float>(m_input.size()) * 11.0f + 1.0f;
        if (caretX < textRc.right) {
            m_brush->SetColor(Col(kColAccent));
            m_rt->DrawLine(D2D1::Point2F(caretX, kInputRc.top + 11.0f),
                           D2D1::Point2F(caretX, kInputRc.bottom - 11.0f),
                           m_brush.Get(), 1.6f);
        }
    }

    // --- connect button ---
    const D2D1_ROUNDED_RECT btnRr = {kConnectRc, 8.0f, 8.0f};
    m_brush->SetColor(Col(m_hoverConnect ? kColAccentHov : kColAccent));
    m_rt->FillRoundedRectangle(btnRr, m_brush.Get());
    m_brush->SetColor(Col(0x0B0F13));
    m_fmtBody->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    drawStr(L"Bağlan", m_fmtBody.Get(),
            D2D1::RectF(kConnectRc.left, kConnectRc.top + 11.0f,
                        kConnectRc.right, kConnectRc.bottom));
    m_fmtBody->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);

    // --- error + footer ---
    if (!m_error.empty()) {
        m_brush->SetColor(Col(kColDanger));
        drawStr(m_error, m_fmtSmall.Get(),
                D2D1::RectF(32.0f, 484.0f, 608.0f, 502.0f));
    }
    m_brush->SetColor(Col(kColDim, 0.8f));
    drawStr(L"Kod, yayınlayan tarafın penceresinde görünür  ·  Kalite ve şifre "
            L"için sağ üstteki dişliye tıklayın  ·  İzlerken Shift+ESC çıkar",
            m_fmtSmall.Get(),
            D2D1::RectF(32.0f, static_cast<float>(kH) - 40.0f, 608.0f,
                        static_cast<float>(kH) - 16.0f));
}

void LauncherWindow::paintSettings()
{
    auto drawStr = [&](const std::wstring& s, IDWriteTextFormat* fmt,
                       const D2D1_RECT_F& rc) {
        m_rt->DrawTextW(s.c_str(), static_cast<UINT32>(s.size()), fmt, rc,
                        m_brush.Get());
    };
    auto centered = [&](const std::wstring& s, const D2D1_RECT_F& rc, float pad) {
        m_fmtBody->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        drawStr(s, m_fmtBody.Get(), D2D1::RectF(rc.left, rc.top + pad, rc.right, rc.bottom));
        m_fmtBody->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    };

    m_brush->SetColor(Col(kColDim));
    drawStr(L"AYARLAR — her iki rol için", m_fmtSmall.Get(),
            D2D1::RectF(32.0f, 92.0f, 500.0f, 110.0f));

    const wchar_t* labels[6] = {
        L"En yüksek bit hızı",   // 0
        L"Kodlama çözünürlüğü",  // 1
        L"FPS hedefi",           // 2
        L"Bağlantı portu",       // 3
        L"Şifre (PSK)",          // 4
        L"Bağlantı onayı",       // 5
    };
    for (int i = 0; i < 6; ++i) {
        const D2D1_RECT_F ctrl = SetCtrlRect(i);
        m_brush->SetColor(Col(kColText, 0.9f));
        drawStr(labels[i], m_fmtBody.Get(),
                D2D1::RectF(kSetLabelX, ctrl.top + 8.0f, kSetCtrlL - 12.0f,
                            ctrl.bottom));
    }

    // Metin alanları: 0 bitrate, 3 port, 4 psk.
    struct Field {
        int           index;
        SettingsField id;
        const std::wstring* text;
        const wchar_t* suffix;   // kutunun sağına çizilen ek ("kbps")
        const wchar_t* hint;     // boşken soluk ipucu
    };
    const Field fields[3] = {
        {0, kFieldBitrate, &m_editBitrate, L"kbps", nullptr},
        {3, kFieldPort,    &m_editPort,    nullptr, nullptr},
        {4, kFieldPsk,     &m_editPsk,     nullptr, L"boş = şifreleme kapalı"},
    };
    for (const Field& f : fields) {
        const D2D1_RECT_F rc = SetCtrlRect(f.index);
        const bool focused = m_focusField == f.id;
        const D2D1_ROUNDED_RECT rr = {rc, 8.0f, 8.0f};
        m_brush->SetColor(Col(m_hoverControl == f.index && !focused
                                  ? kColPanelHover : kColPanel));
        m_rt->FillRoundedRectangle(rr, m_brush.Get());
        m_brush->SetColor(Col(focused ? kColAccent : kColBorder));
        m_rt->DrawRoundedRectangle(rr, m_brush.Get(), focused ? 1.5f : 1.0f);

        const D2D1_RECT_F textRc = {rc.left + 12.0f, rc.top + 8.0f,
                                    rc.right - 12.0f, rc.bottom - 4.0f};
        if (f.text->empty() && f.hint) {
            m_brush->SetColor(Col(kColDim, 0.55f));
            drawStr(f.hint, m_fmtBody.Get(), textRc);
        } else {
            m_brush->SetColor(Col(kColText));
            drawStr(*f.text, m_fmtBody.Get(), textRc);
        }
        if (focused && m_caretOn) {
            // Orantısız yazıtipi: imleç konumu DWrite ölçümüyle bulunur.
            float advance = 0.0f;
            if (!f.text->empty()) {
                ComPtr<IDWriteTextLayout> layout;
                if (SUCCEEDED(m_dwrite->CreateTextLayout(
                        f.text->c_str(), static_cast<UINT32>(f.text->size()),
                        m_fmtBody.Get(), 10000.0f, 100.0f, &layout))) {
                    DWRITE_TEXT_METRICS metrics = {};
                    if (SUCCEEDED(layout->GetMetrics(&metrics)))
                        advance = metrics.widthIncludingTrailingWhitespace;
                }
            }
            const float caretX = textRc.left + advance + 1.0f;
            if (caretX < textRc.right) {
                m_brush->SetColor(Col(kColAccent));
                m_rt->DrawLine(D2D1::Point2F(caretX, rc.top + 9.0f),
                               D2D1::Point2F(caretX, rc.bottom - 9.0f),
                               m_brush.Get(), 1.6f);
            }
        }
        if (f.suffix) {
            m_brush->SetColor(Col(kColDim));
            drawStr(f.suffix, m_fmtBody.Get(),
                    D2D1::RectF(rc.right + 10.0f, rc.top + 8.0f, rc.right + 70.0f,
                                rc.bottom));
        }
    }

    // Cycler'lar: 1 çözünürlük, 2 fps.
    for (int index = 1; index <= 2; ++index) {
        const D2D1_RECT_F rc = SetCtrlRect(index);
        const D2D1_ROUNDED_RECT rr = {rc, 8.0f, 8.0f};
        m_brush->SetColor(Col(m_hoverControl == index ? kColPanelHover : kColPanel));
        m_rt->FillRoundedRectangle(rr, m_brush.Get());
        m_brush->SetColor(Col(kColBorder));
        m_rt->DrawRoundedRectangle(rr, m_brush.Get(), 1.0f);

        std::wstring value;
        if (index == 1)
            value = m_draftHeight == 0 ? L"Yerel (ölçekleme yok)"
                                       : FormatUInt(m_draftHeight) + L"p";
        else
            value = FormatUInt(m_draftFps) + L" fps";
        m_brush->SetColor(Col(kColText));
        centered(value, rc, 8.0f);

        // ‹ › okları.
        m_brush->SetColor(Col(kColDim));
        const float ay = (rc.top + rc.bottom) / 2.0f;
        m_rt->DrawLine(D2D1::Point2F(rc.left + 18.0f, ay - 5.0f),
                       D2D1::Point2F(rc.left + 12.0f, ay), m_brush.Get(), 1.8f);
        m_rt->DrawLine(D2D1::Point2F(rc.left + 12.0f, ay),
                       D2D1::Point2F(rc.left + 18.0f, ay + 5.0f), m_brush.Get(), 1.8f);
        m_rt->DrawLine(D2D1::Point2F(rc.right - 18.0f, ay - 5.0f),
                       D2D1::Point2F(rc.right - 12.0f, ay), m_brush.Get(), 1.8f);
        m_rt->DrawLine(D2D1::Point2F(rc.right - 12.0f, ay),
                       D2D1::Point2F(rc.right - 18.0f, ay + 5.0f), m_brush.Get(), 1.8f);
    }

    // Onay anahtarı (5).
    {
        const D2D1_RECT_F rc = SetCtrlRect(5);
        const D2D1_RECT_F track = {rc.left, rc.top + 7.0f, rc.left + 48.0f,
                                   rc.top + 31.0f};
        const D2D1_ROUNDED_RECT trackRr = {track, 12.0f, 12.0f};
        m_brush->SetColor(m_draftApproval ? Col(kColAccent)
                                          : Col(kColPanelHover));
        m_rt->FillRoundedRectangle(trackRr, m_brush.Get());
        m_brush->SetColor(Col(kColBorder));
        m_rt->DrawRoundedRectangle(trackRr, m_brush.Get(), 1.0f);
        const float knobX = m_draftApproval ? track.right - 12.0f : track.left + 12.0f;
        m_brush->SetColor(Col(m_draftApproval ? 0x0B0F13 : kColDim));
        m_rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(knobX,
                                                      (track.top + track.bottom) / 2.0f),
                                        8.0f, 8.0f),
                          m_brush.Get());
        m_brush->SetColor(Col(kColText, 0.9f));
        drawStr(m_draftApproval ? L"Açık — izleyici için onay istenir"
                                : L"Kapalı — ilk bağlanan izler",
                m_fmtBody.Get(),
                D2D1::RectF(track.right + 14.0f, rc.top + 8.0f, kSetCtrlR + 40.0f,
                            rc.bottom));
    }

    // Açıklama + hata + düğmeler.
    m_brush->SetColor(Col(kColDim, 0.8f));
    drawStr(L"Bit hızı/çözünürlük/FPS yayını, port her iki rolü, şifre iki uçta "
            L"aynı olmak üzere aktarımı etkiler. Değişiklikler bir sonraki "
            L"oturumda geçerli olur.",
            m_fmtSmall.Get(), D2D1::RectF(48.0f, 440.0f, 592.0f, 486.0f));
    if (!m_settingsError.empty()) {
        m_brush->SetColor(Col(kColDanger));
        drawStr(m_settingsError, m_fmtSmall.Get(),
                D2D1::RectF(48.0f, 494.0f, 592.0f, 512.0f));
    }

    const D2D1_ROUNDED_RECT cancelRr = {kCancelRc, 8.0f, 8.0f};
    m_brush->SetColor(Col(m_hoverControl == 11 ? kColPanelHover : kColPanel));
    m_rt->FillRoundedRectangle(cancelRr, m_brush.Get());
    m_brush->SetColor(Col(kColBorder));
    m_rt->DrawRoundedRectangle(cancelRr, m_brush.Get(), 1.0f);
    m_brush->SetColor(Col(kColText));
    centered(L"Vazgeç", kCancelRc, 9.0f);

    const D2D1_ROUNDED_RECT saveRr = {kSaveRc, 8.0f, 8.0f};
    m_brush->SetColor(Col(m_hoverControl == 10 ? kColAccentHov : kColAccent));
    m_rt->FillRoundedRectangle(saveRr, m_brush.Get());
    m_brush->SetColor(Col(0x0B0F13));
    centered(L"Kaydet", kSaveRc, 9.0f);
}

void LauncherWindow::paint()
{
    if (!m_rt)
        return;

    m_rt->BeginDraw();
    m_rt->Clear(Col(kColBg));
    drawChrome();
    if (m_page == Page::Main)
        paintMain();
    else
        paintSettings();

    const HRESULT hr = m_rt->EndDraw();
    if (hr == static_cast<HRESULT>(D2DERR_RECREATE_TARGET)) {
        releaseGraphics();
        createGraphics();
    }
}
