// app_main.cpp
//
// Single entry point for the unified MidnightSun app (MidnightSun.exe).
// There is no separate server/client anymore: the launcher's home screen
// offers "Bu bilgisayarı yayınla" (HostSession) and the connect paths
// (LAN discovery list / pairing code / ip:port -> ClientSession), and the
// app returns to the launcher after every session:
//
//   LauncherWindow ──Host────> RunHostSession   (status window = stop)
//        ^  |
//        |  └──Connect───────> RunClientSession (Shift+ESC = end)
//        └───── session over ─────┘
//
// GUI subsystem (WinMain): no console window by default. Set
// "show_console": 1 in midnightsun_config.json to attach one for the
// pipeline logs (also enables Ctrl+C to quit).
//
// Config: midnightsun_config.json next to the executable -- one file for
// both roles; see AppConfig.h for the keys. The last successful connect
// target is written back ("Kayıtlı adres" row in the launcher).

#include "AppConfig.h"
#include "ClientSession.h"
#include "HostSession.h"
#include "LatencyTracker.h"
#include "LauncherWindow.h"

#include <windows.h>
#include <timeapi.h>

#include <atomic>
#include <cstdio>
#include <string>

#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "ole32.lib")

namespace {

std::atomic<bool> g_interrupted{false};

BOOL WINAPI ConsoleCtrlHandler(DWORD ctrlType)
{
    switch (ctrlType) {
    case CTRL_C_EVENT:
    case CTRL_BREAK_EVENT:
    case CTRL_CLOSE_EVENT:
    case CTRL_SHUTDOWN_EVENT:
        g_interrupted.store(true, std::memory_order_release);
        return TRUE;
    default:
        return FALSE;
    }
}

// Optional console for the printf/stderr pipeline logs (GUI subsystem
// has none by default).
void AttachLogConsole()
{
    if (!AllocConsole())
        return;
    FILE* stream = nullptr;
    freopen_s(&stream, "CONOUT$", "w", stdout);
    freopen_s(&stream, "CONOUT$", "w", stderr);
    SetConsoleTitleW(L"MidnightSun log");
    SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE);
}

// UTF-8 (or plain ASCII) config string -> the wide path enableCsvLog wants.
std::wstring WidenUtf8(const std::string& utf8)
{
    if (utf8.empty())
        return {};
    const int chars = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(),
                                          static_cast<int>(utf8.size()), nullptr, 0);
    if (chars <= 0)
        return {};
    std::wstring wide(static_cast<size_t>(chars), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()),
                        wide.data(), chars);
    return wide;
}

constexpr char kConfigPath[] = "midnightsun_config.json";

} // namespace

// Plain WinMain (not wWinMain): it is the MSVC default entry for the
// WINDOWS subsystem without _UNICODE, so no /ENTRY override is needed.
// The command line is unused either way.
int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int)
{
    SetThreadDescription(GetCurrentThread(), L"Main");
    timeBeginPeriod(1); // sharpen every sleep_until in the pipelines

    AppConfig config = LoadAppConfig(kConfigPath);
    if (config.showConsole)
        AttachLogConsole();
    printf("[app] MidnightSun -- unified host/client\n");
    if (!config.latencyCsv.empty())
        LatencyTracker::get().enableCsvLog(WidenUtf8(config.latencyCsv));

    // COM for the whole process lifetime: the client session's decoder
    // (Media Foundation) needs it on this thread; host-session threads
    // initialize their own apartments.
    const HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr)) {
        fprintf(stderr, "[app] CoInitializeEx failed: 0x%08lX\n",
                static_cast<unsigned long>(hr));
        timeEndPeriod(1);
        return 1;
    }

    // Eski tekli "server_ip" kaydını isimli cihaz listesine taşı (bir
    // kereye mahsus; dosya bir sonraki değişiklikte yazılır).
    if (config.savedDevices.empty() && !config.serverIp.empty())
        UpsertSavedDevice(config, config.serverIp, config.serverIp,
                          config.serverPort);

    // Launcher -> session -> launcher, until the launcher is dismissed.
    while (!g_interrupted.load(std::memory_order_acquire)) {
        LauncherWindow launcher;
        const LaunchChoice choice = launcher.run(config);
        // Ayarlar ekranı / cihaz silme config'i değiştirmiş olabilir --
        // Cancel dahil her aksiyondan önce kalıcılaştır.
        if (choice.configChanged)
            SaveAppConfig(kConfigPath, config);
        if (g_interrupted.load(std::memory_order_acquire) ||
            choice.action == LaunchAction::Cancel)
            break;

        if (choice.action == LaunchAction::Host) {
            RunHostSession(config, g_interrupted);
            continue;
        }

        // Connect: hedefi isimli cihaz listesine ve "son hedef" alanlarına
        // yaz (launcher'da tek tıkla yeniden bağlanma).
        bool dirty = UpsertSavedDevice(config, choice.name, choice.ip, choice.port);
        if (choice.ip != config.serverIp || choice.port != config.serverPort) {
            config.serverIp   = choice.ip;
            config.serverPort = choice.port;
            dirty = true;
        }
        if (dirty)
            SaveAppConfig(kConfigPath, config);
        RunClientSession(config, choice.ip, choice.port, g_interrupted);
    }

    CoUninitialize();
    timeEndPeriod(1);
    printf("[app] clean exit\n");
    return 0;
}
