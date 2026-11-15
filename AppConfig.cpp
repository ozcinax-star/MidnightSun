// AppConfig.cpp -- see AppConfig.h for the design overview.

#include "AppConfig.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace {

bool ReadFileText(const char* path, std::string& out)
{
    std::ifstream file(path, std::ios::binary);
    if (!file)
        return false;
    std::ostringstream ss;
    ss << file.rdbuf();
    out = ss.str();
    return true;
}

bool FindJsonNumber(const std::string& json, const char* key, long long& out)
{
    const std::string needle = std::string("\"") + key + "\"";
    size_t pos = json.find(needle);
    if (pos == std::string::npos)
        return false;
    pos = json.find(':', pos + needle.size());
    if (pos == std::string::npos)
        return false;
    char* end = nullptr;
    const long long value = strtoll(json.c_str() + pos + 1, &end, 10);
    if (end == json.c_str() + pos + 1)
        return false;
    out = value;
    return true;
}

// "..."-içi metni kaçış çözerek okur; `pos` açılış tırnağının indeksi,
// dönüşte kapanış tırnağının bir sonrası. Sadece \" ve \\ desteklenir
// (Save tarafı da yalnızca bunları üretir).
bool ParseQuoted(const std::string& json, size_t& pos, std::string& out)
{
    if (pos >= json.size() || json[pos] != '"')
        return false;
    out.clear();
    for (size_t i = pos + 1; i < json.size(); ++i) {
        const char c = json[i];
        if (c == '\\' && i + 1 < json.size()) {
            out.push_back(json[i + 1]);
            ++i;
            continue;
        }
        if (c == '"') {
            pos = i + 1;
            return true;
        }
        out.push_back(c);
    }
    return false; // kapanış tırnağı yok
}

bool FindJsonString(const std::string& json, const char* key, std::string& out)
{
    const std::string needle = std::string("\"") + key + "\"";
    size_t pos = json.find(needle);
    if (pos == std::string::npos)
        return false;
    pos = json.find(':', pos + needle.size());
    if (pos == std::string::npos)
        return false;
    size_t open = json.find('"', pos + 1);
    if (open == std::string::npos)
        return false;
    std::string value;
    if (!ParseQuoted(json, open, value))
        return false;
    out = std::move(value);
    return !out.empty();
}

// Yazarken " ve \ kaçışlanır; kontrol karakterleri boşluğa çevrilir
// (elle yazılmış dosyada bile parser'ı kırmasınlar).
std::string EscapeJson(const std::string& raw)
{
    std::string out;
    out.reserve(raw.size() + 4);
    for (char c : raw) {
        if (c == '"' || c == '\\')
            out.push_back('\\');
        out.push_back(static_cast<unsigned char>(c) < 0x20 ? ' ' : c);
    }
    return out;
}

// "saved_devices": [ {...}, {...} ] dizisini okur. Alan sırasından
// bağımsızdır; her {...} bloğu kendi içinde taranır.
void ParseSavedDevices(const std::string& json, std::vector<SavedDevice>& out)
{
    const std::string needle = "\"saved_devices\"";
    size_t pos = json.find(needle);
    if (pos == std::string::npos)
        return;
    pos = json.find('[', pos + needle.size());
    if (pos == std::string::npos)
        return;
    const size_t arrayEnd = json.find(']', pos);
    if (arrayEnd == std::string::npos)
        return;

    size_t cursor = pos;
    while (out.size() < kMaxSavedDevices) {
        const size_t objOpen = json.find('{', cursor);
        if (objOpen == std::string::npos || objOpen > arrayEnd)
            break;
        const size_t objClose = json.find('}', objOpen);
        if (objClose == std::string::npos || objClose > arrayEnd)
            break;
        const std::string obj = json.substr(objOpen, objClose - objOpen + 1);

        SavedDevice device;
        FindJsonString(obj, "name", device.name);
        FindJsonString(obj, "ip", device.ip);
        long long value = 0;
        if (FindJsonNumber(obj, "port", value) && value > 0 && value <= 65535)
            device.port = static_cast<uint16_t>(value);
        if (!device.ip.empty()) {
            if (device.name.empty())
                device.name = device.ip;
            out.push_back(std::move(device));
        }
        cursor = objClose + 1;
    }
}

} // namespace

AppConfig LoadAppConfig(const char* path)
{
    AppConfig config;
    std::string json;
    if (!ReadFileText(path, json)) {
        fprintf(stderr, "[config] %s not found; using defaults\n", path);
        return config;
    }

    long long value = 0;
    if (FindJsonNumber(json, "port", value) && value > 0 && value <= 65535)
        config.port = static_cast<uint16_t>(value);
    if (FindJsonNumber(json, "bitrate_kbps", value) && value >= 500 && value <= 500'000)
        config.bitrateKbps = static_cast<uint32_t>(value);
    if (FindJsonNumber(json, "fps_target", value) && value >= 1 && value <= 240)
        config.fpsTarget = static_cast<uint32_t>(value);
    if (FindJsonNumber(json, "encode_height", value) && value >= 0 && value <= 2160)
        config.encodeHeight = static_cast<uint32_t>(value);
    if (FindJsonNumber(json, "require_approval", value))
        config.requireApproval = value != 0;
    if (FindJsonNumber(json, "server_port", value) && value > 0 && value <= 65535)
        config.serverPort = static_cast<uint16_t>(value);
    if (FindJsonNumber(json, "show_console", value))
        config.showConsole = value != 0;
    FindJsonString(json, "psk", config.psk);
    FindJsonString(json, "server_ip", config.serverIp);
    FindJsonString(json, "latency_csv", config.latencyCsv);
    ParseSavedDevices(json, config.savedDevices);
    return config;
}

void SaveAppConfig(const char* path, const AppConfig& config)
{
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) {
        fprintf(stderr, "[config] could not write %s (read-only?)\n", path);
        return;
    }
    file << "{\n";
    file << "    \"port\": " << config.port << ",\n";
    file << "    \"bitrate_kbps\": " << config.bitrateKbps << ",\n";
    file << "    \"fps_target\": " << config.fpsTarget << ",\n";
    file << "    \"encode_height\": " << config.encodeHeight << ",\n";
    file << "    \"require_approval\": " << (config.requireApproval ? 1 : 0) << ",\n";
    file << "    \"psk\": \"" << EscapeJson(config.psk) << "\",\n";
    file << "    \"server_ip\": \"" << EscapeJson(config.serverIp) << "\",\n";
    file << "    \"server_port\": " << config.serverPort << ",\n";
    file << "    \"saved_devices\": [";
    for (size_t i = 0; i < config.savedDevices.size(); ++i) {
        const SavedDevice& device = config.savedDevices[i];
        file << (i ? ",\n        " : "\n        ");
        file << "{\"name\": \"" << EscapeJson(device.name) << "\", \"ip\": \""
             << EscapeJson(device.ip) << "\", \"port\": " << device.port << "}";
    }
    file << (config.savedDevices.empty() ? "],\n" : "\n    ],\n");
    file << "    \"latency_csv\": \"" << EscapeJson(config.latencyCsv) << "\",\n";
    file << "    \"show_console\": " << (config.showConsole ? 1 : 0) << "\n";
    file << "}\n";
}

bool UpsertSavedDevice(AppConfig& config, const std::string& name,
                       const std::string& ip, uint16_t port)
{
    if (ip.empty())
        return false;
    std::string keepName = name;
    for (size_t i = 0; i < config.savedDevices.size(); ++i) {
        if (config.savedDevices[i].ip == ip && config.savedDevices[i].port == port) {
            if (keepName.empty())
                keepName = config.savedDevices[i].name; // adı koru
            const bool changed = i != 0 || config.savedDevices[i].name != keepName;
            config.savedDevices.erase(config.savedDevices.begin() +
                                      static_cast<ptrdiff_t>(i));
            SavedDevice device{keepName.empty() ? ip : keepName, ip, port};
            config.savedDevices.insert(config.savedDevices.begin(), std::move(device));
            return changed;
        }
    }
    SavedDevice device{keepName.empty() ? ip : keepName, ip, port};
    config.savedDevices.insert(config.savedDevices.begin(), std::move(device));
    if (config.savedDevices.size() > kMaxSavedDevices)
        config.savedDevices.resize(kMaxSavedDevices);
    return true;
}

bool RemoveSavedDevice(AppConfig& config, size_t index)
{
    if (index >= config.savedDevices.size())
        return false;
    config.savedDevices.erase(config.savedDevices.begin() +
                              static_cast<ptrdiff_t>(index));
    return true;
}
