// LauncherWindow.h
//
// The unified app's home screen: a dark, borderless Direct2D window shown
// between sessions. One window covers both roles:
//
//   - "Bu bilgisayarı yayınla" card: start hosting this machine's screen
//     (returns LaunchAction::Host; the caller runs HostSession).
//   - "Cihazlar": named saved devices (config'deki saved_devices) and
//     servers found by LAN auto-discovery (DiscoverServers on a background
//     thread, refreshed ~every 1.5 s) in one live list; clicking connects,
//     no typing at all. Saved rows show a "×" to forget the device.
//   - Pairing code / manual entry: one input line that accepts either a
//     pairing code ("XXXX-XXXX-XXXX", shown in the host's status window)
//     or a raw "ip[:port]". Enter or the Bağlan button connects.
//   - Ayarlar (dişli simgesi): kalite (bitrate/çözünürlük/FPS), port, PSK
//     ve bağlantı onayı — hepsi uygulama içinden düzenlenir, JSON'a el
//     sürülmez. Kaydet, config'i yerinde günceller ve configChanged'i
//     işaretler (dosyayı çağıran yazar).
//
// run() is modal: it owns a message loop on the calling thread and
// returns when the user picked an action (Host, or Connect with ip/port
// filled), or closed the window / pressed Esc (Cancel). The window is
// destroyed before run() returns, so the next screen can take over.
//
// Visual style: borderless WS_POPUP window (drag anywhere on the header),
// DWM rounded corners + dark title bar where the OS supports them,
// Segoe UI + Consolas via DirectWrite, hover states on rows/buttons.
// Per-monitor DPI: layout constants are DIPs; the window sizes itself by
// the monitor scale, the D2D target renders at native DPI, and
// WM_DPICHANGED rescales live.
//
// Threading: everything Win32/D2D happens on the run() thread. The only
// background worker is the discovery thread, which fills m_servers under
// m_mutex and pokes the window with InvalidateRect.
//
// Requires: MSVC 2022, C++20. Links d2d1.lib, dwrite.lib, dwmapi.lib via
// #pragma comment in the .cpp.

#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "Discovery.h" // winsock2.h before windows.h
#include "AppConfig.h"

#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <wrl/client.h>

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

enum class LaunchAction {
    Cancel,  // window closed / Esc: quit the app
    Connect, // ip/port filled: run a client session
    Host,    // start hosting this machine
};

struct LaunchChoice {
    LaunchAction action = LaunchAction::Cancel;
    std::string  ip;   // valid when action == Connect
    uint16_t     port = 0;
    std::string  name; // Connect: seçilen cihazın görünen adı ("" = bilinmiyor)
    // Ayarlar kaydedildi ya da kayıtlı cihaz silindi: çağıran
    // SaveAppConfig ile dosyaya yazmalı (Cancel'da bile olabilir).
    bool configChanged = false;
};

class LauncherWindow {
public:
    LauncherWindow() = default;
    ~LauncherWindow();

    LauncherWindow(const LauncherWindow&) = delete;
    LauncherWindow& operator=(const LauncherWindow&) = delete;

    // Modal: blocks on its own message loop until a choice is made or the
    // window is closed. `config` is edited in place by the settings page
    // and by saved-device deletion; LaunchChoice::configChanged reports
    // whether the caller should persist it.
    LaunchChoice run(AppConfig& config);

private:
    template <typename T>
    using ComPtr = Microsoft::WRL::ComPtr<T>;

    enum class Page { Main, Settings };

    struct Row { // one clickable device row, layout computed per paint
        std::wstring title;      // kayıtlı ad ya da keşif hostname'i
        std::string  ip;
        uint16_t     port  = 0;
        bool         live  = false; // şu anda ağda görünüyor
        bool         saved = false; // config'deki saved_devices'tan
        bool         psk   = false; // keşif "PSK gerekli" bayrağı
        size_t       savedIndex = 0; // saved==true iken config dizini
    };

    // Ayarlar sayfası: odaklanabilir metin alanları.
    enum SettingsField : int {
        kFieldNone = -1,
        kFieldBitrate = 0,
        kFieldPort,
        kFieldPsk,
        kFieldCount
    };

    static LRESULT CALLBACK wndProcThunk(HWND, UINT, WPARAM, LPARAM);
    LRESULT handleMessage(HWND, UINT, WPARAM, LPARAM);

    bool createWindow();
    bool createGraphics();
    void releaseGraphics();
    void applyDpi(UINT dpi, const RECT* suggested);
    void paint();
    void paintMain();
    void paintSettings();
    void drawRow(const Row& row, const D2D1_RECT_F& rect, bool hovered,
                 bool hoveredDelete);
    void drawChrome(); // başlık + logo + sağ üst düğmeler (iki sayfada ortak)
    void discoveryThreadProc();

    void handleChar(wchar_t c);          // main: kod/ip satırı
    void handleSettingsChar(wchar_t c);  // settings: odaklı alan
    void attemptConnect();               // parse input -> finish or show error
    void choose(std::string ip, uint16_t port, std::string name);

    void enterSettings();  // config -> düzenleme tamponları
    bool saveSettings();   // doğrula + config'e yaz; false = hata gösterildi
    void cycleHeight(int direction);
    void cycleFps(int direction);

    std::vector<Row> currentRows(); // saved + discovered, under lock
    int  hitTestRow(float x, float y);
    bool hitTestRowDelete(float x, float y, int row); // satırdaki × üstünde mi

    // --- window / graphics (run() thread) ---
    HWND  m_hwnd      = nullptr;
    ATOM  m_classAtom = 0;
    float m_scale     = 1.0f; // DPI/96
    ComPtr<ID2D1Factory>          m_d2dFactory;
    ComPtr<ID2D1HwndRenderTarget> m_rt;
    ComPtr<IDWriteFactory>        m_dwrite;
    ComPtr<IDWriteTextFormat>     m_fmtTitle;
    ComPtr<IDWriteTextFormat>     m_fmtBody;
    ComPtr<IDWriteTextFormat>     m_fmtSmall;
    ComPtr<IDWriteTextFormat>     m_fmtCode;
    ComPtr<ID2D1SolidColorBrush>  m_brush; // recolored on the fly

    // --- state (run() thread unless noted) ---
    Page         m_page = Page::Main;
    std::wstring m_input;           // typed code/ip (uppercased)
    std::wstring m_error;           // last parse error, cleared on typing
    int          m_hoverRow       = -1;
    bool         m_hoverRowDelete = false;
    bool         m_hoverConnect   = false;
    bool         m_hoverClose     = false;
    bool         m_hoverGear      = false;
    bool         m_hoverHost      = false;
    int          m_hoverControl   = -1; // settings: hangi kontrol (paint sırası)
    bool         m_caretOn        = true;
    int          m_spinnerPhase   = 0;
    LaunchChoice m_choice;
    AppConfig*   m_config = nullptr; // run() süresince geçerli

    // --- settings draft (Kaydet'e kadar config'e dokunulmaz) ---
    std::wstring m_editBitrate;
    std::wstring m_editPort;
    std::wstring m_editPsk;
    uint32_t     m_draftHeight   = 1080;
    uint32_t     m_draftFps      = 60;
    bool         m_draftApproval = true;
    int          m_focusField    = kFieldNone;
    std::wstring m_settingsError;

    // --- discovery worker ---
    std::thread                   m_discoveryThread;
    std::atomic<bool>             m_stopDiscovery{false};
    std::mutex                    m_mutex; // guards m_servers
    std::vector<DiscoveredServer> m_servers;
    std::atomic<bool>             m_scannedOnce{false};
};
