// AppConfig.h
//
// One config file for the unified MidnightSun app (midnightsun_config.json
// next to the executable). The same binary hosts a stream or connects to
// one, so the former server_config.json / client_config.json keys live
// together here:
//
//   {
//       "port": 9000,            // host: GSMR stream port this machine listens on
//       "bitrate_kbps": 20000,   // host: adaptive controller MAXIMUM
//       "fps_target": 60,        // host: capture/encode pacing
//       "encode_height": 1080,   // host: GPU downscale before NVENC (0 = native)
//       "require_approval": 1,   // host: yeni istemci için "İzin ver/Reddet" sor
//       "psk": "",               // both: AES-256-GCM transport key ("" = off;
//                                //       must match on both machines)
//       "server_ip": "",         // client: last successful target ("Kayıtlı
//                                //         adres" row in the launcher)
//       "server_port": 9000,     // client: last successful target's port
//       "saved_devices": [       // client: named remembered targets (launcher
//           {"name": "...",      //         list; upserted on every connect)
//            "ip": "...", "port": 9000}
//       ],
//       "latency_csv": "",       // optional per-frame latency log path
//       "show_console": 0        // 1 = attach a console for the logs
//   }
//
// Missing file or keys fall back to the defaults above. Save() rewrites
// the whole file (it is small and fully known), preserving every field.
// String values are JSON-escaped (quote/backslash) on save and unescaped
// on load, so device names with arbitrary characters round-trip.
//
// The launcher's settings screen edits every host/client knob in place,
// so users never open this file by hand.
//
// Requires: MSVC 2022, C++20. No OS dependencies in this header.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

// One remembered connect target, shown as a named row in the launcher.
struct SavedDevice {
    std::string name; // kullanıcıya görünen ad (keşif hostname'i ya da ip)
    std::string ip;
    uint16_t    port = 9000;
};

struct AppConfig {
    // --- shared ---
    uint16_t    port = 9000;
    std::string psk;
    std::string latencyCsv;
    bool        showConsole = false;

    // --- host (stream source) ---
    uint32_t bitrateKbps     = 20'000;
    uint32_t fpsTarget       = 60;
    uint32_t encodeHeight    = 1080;
    bool     requireApproval = true; // yeni istemciye yayın öncesi onay sor

    // --- client (viewer) ---
    std::string serverIp;          // remembered last target; empty = none yet
    uint16_t    serverPort = 9000; // remembered last target's port
    std::vector<SavedDevice> savedDevices; // named list, newest first
};

// En fazla bu kadar kayıtlı cihaz tutulur (launcher listesi taşmasın).
inline constexpr size_t kMaxSavedDevices = 6;

// Loads `path`; missing file/keys keep their defaults (logged to stderr).
AppConfig LoadAppConfig(const char* path);

// Best-effort rewrite of `path` with every field; failures are logged.
void SaveAppConfig(const char* path, const AppConfig& config);

// Adds or refreshes (ip,port) at the top of savedDevices. An existing
// entry keeps its name unless `name` is non-empty. Trims to
// kMaxSavedDevices. Returns true if the list changed.
bool UpsertSavedDevice(AppConfig& config, const std::string& name,
                       const std::string& ip, uint16_t port);

// Removes the device at `index`; returns true if removed.
bool RemoveSavedDevice(AppConfig& config, size_t index);
