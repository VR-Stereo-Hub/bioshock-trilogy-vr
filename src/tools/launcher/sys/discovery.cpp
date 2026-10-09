// Steam discovery follows Dishonored's launcher, adapted to app 409710 and
// Build/Final. A folder name alone never authorizes installation.
#include "model/launcher.h"
#include "sys/fs.h"
#include "sys/game_ini.h"
#include <json/json.h>
#include <algorithm>
#include <cstdlib>
#include <memory>

namespace bvr::launcher {
namespace {
std::wstring registry_string(HKEY hive, const wchar_t* key, const wchar_t* name, REGSAM view) {
    HKEY h = nullptr;
    if (RegOpenKeyExW(hive, key, 0, KEY_READ | view, &h) != ERROR_SUCCESS) return {};
    wchar_t value[32768]{}; DWORD size = sizeof(value);
    const auto result = RegGetValueW(h, nullptr, name, RRF_RT_REG_SZ, nullptr, value, &size);
    RegCloseKey(h);
    return result == ERROR_SUCCESS ? value : L"";
}
std::string text_file(const std::wstring& path) {
    std::vector<unsigned char> bytes;
    if (fs::file_size(path) > 8 * 1024 * 1024 || !fs::read_file(path, &bytes, nullptr)) return {};
    return {bytes.begin(), bytes.end()};
}
// Valve's quoted tokens, including escaped backslashes and quotes. This also
// reads installdir from the app manifest instead of assuming the folder name.
std::vector<std::string> tokens(const std::string& input) {
    std::vector<std::string> result;
    for (size_t i = 0; i < input.size(); ++i) {
        if (input[i] != '"') continue;
        std::string value;
        for (++i; i < input.size() && input[i] != '"'; ++i) {
            if (input[i] == '\\' && i + 1 < input.size()) ++i;
            value.push_back(input[i]);
        }
        result.push_back(std::move(value));
    }
    return result;
}
std::wstring find_game() {
    for (const auto& lib : steam_libraries()) {
        const auto parts = tokens(text_file(fs::join(lib, L"steamapps\\appmanifest_409710.acf")));
        for (size_t i = 0; i + 1 < parts.size(); ++i) if (parts[i] == "installdir") {
            const auto folder = fs::widen(parts[i + 1]);
            if (folder.empty() || folder == L"." || folder == L".." || folder.find_first_of(L"\\/:") != std::wstring::npos) continue;
            const auto found = normalise_game_dir(fs::join(fs::join(lib, L"steamapps\\common"), folder));
            if (!found.empty()) return found;
        }
    }
    return {};
}
bool read_json(const std::wstring& file, Json::Value* out) {
    const auto text = text_file(file);
    Json::CharReaderBuilder b; b["rejectDupKeys"] = true; b["failIfExtra"] = true;
    std::unique_ptr<Json::CharReader> reader(b.newCharReader()); std::string error;
    return reader->parse(text.data(), text.data() + text.size(), out, &error) && out->isObject();
}
bool contains(const std::wstring& file, const char* marker) {
    std::vector<unsigned char> bytes;
    return fs::read_file(file, &bytes, nullptr) && fs::contains_ascii(bytes.data(), bytes.size(), marker);
}
}
std::vector<std::wstring> steam_libraries() {
    std::wstring root;
    for (HKEY hive : {HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE}) {
        for (REGSAM view : {KEY_WOW64_32KEY, KEY_WOW64_64KEY}) {
            for (const auto* name : {L"SteamPath", L"InstallPath"}) {
                auto path = registry_string(hive, L"Software\\Valve\\Steam", name, view);
                if (root.empty() && fs::is_dir(path)) root = path;
            }
        }
    }
    if (root.empty()) root = fs::join(fs::env(L"ProgramFiles(x86)"), L"Steam");
    if (!fs::is_dir(root)) return {};
    std::vector<std::wstring> result{root};
    const auto parts = tokens(text_file(fs::join(root, L"steamapps\\libraryfolders.vdf")));
    for (size_t i = 0; i + 1 < parts.size(); ++i) if (parts[i] == "path") {
        const auto path = fs::widen(parts[i + 1]);
        if (std::none_of(result.begin(), result.end(), [&](const auto& p) { return fs::iequals(p, path); })) result.push_back(path);
    }
    return result;
}
bool pe32_file(const std::wstring& path) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    IMAGE_DOS_HEADER dos{}; DWORD got = 0;
    bool ok = ReadFile(file, &dos, sizeof(dos), &got, nullptr) && got == sizeof(dos) &&
        dos.e_magic == IMAGE_DOS_SIGNATURE && dos.e_lfanew >= sizeof(dos) && dos.e_lfanew < 1024 * 1024;
    IMAGE_NT_HEADERS32 nt{};
    if (ok) {
        SetFilePointer(file, dos.e_lfanew, nullptr, FILE_BEGIN);
        ok = ReadFile(file, &nt, sizeof(nt), &got, nullptr) && got == sizeof(nt) &&
            nt.Signature == IMAGE_NT_SIGNATURE && nt.FileHeader.Machine == IMAGE_FILE_MACHINE_I386 &&
            nt.OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR32_MAGIC;
    }
    CloseHandle(file); return ok;
}
std::wstring normalise_game_dir(const std::wstring& chosen) {
    if (chosen.empty()) return {};
    for (const auto* tail : {L"", L"Build\\Final", L"Final"}) {
        const auto dir = fs::join(fs::strip_trailing_slashes(chosen), tail);
        if (pe32_file(fs::join(dir, kGameExe))) return dir;
    }
    return {};
}
Environment resolve_environment(Environment env) {
    if (env.gameDir.empty()) env.gameDir = find_game();
    else {
        const auto found = normalise_game_dir(env.gameDir);
        if (!found.empty()) env.gameDir = found;
    }
    if (env.dataDir.empty()) env.dataDir = fs::join(fs::known_folder(FOLDERID_LocalAppData), L"BioshockVR");
    if (env.gameIni.empty()) {
        const auto roaming = fs::known_folder(FOLDERID_RoamingAppData);
        const auto docs = fs::known_folder(FOLDERID_Documents);
        const std::wstring paths[] = {
            fs::join(roaming, L"BioshockHD\\Bioshock\\Bioshock.ini"),
            fs::join(roaming, L"Bioshock\\Bioshock.ini"),
            fs::join(docs, L"BioshockHD\\Bioshock\\Bioshock.ini"),
            fs::join(docs, L"BioShock Remastered\\Bioshock.ini")};
        for (const auto& p : paths) {
            GameIni ini; std::wstring why; std::string value;
            if (GameIni::load(p, &ini, &why) && ini.get_unique("WinDrv.WindowsClient", "WindowedViewportX", &value)) {
                env.gameIni = p; break;
            }
        }
        if (env.gameIni.empty()) env.gameIni = paths[0];
    }
    return env;
}
Detection detect(const Environment& requested, const Payload& payload) {
    Detection d; d.env = resolve_environment(requested);
    d.found = !d.env.gameDir.empty() && pe32_file(fs::join(d.env.gameDir, kGameExe));
    d.source = requested.gameDir.empty() ? "Steam library" : "Selected folder";
    d.running = process::is_running(kGameExe);
    d.payloadOk = payload.valid;
    d.graphics = gpu::primary();
    d.activeRuntime = fs::narrow(registry_string(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Khronos\\OpenXR\\1", L"ActiveRuntime", KEY_WOW64_32KEY));
    d.nativePresent = fs::is_file(fs::widen(d.activeRuntime));
    auto vd = fs::join(fs::env(L"ProgramW6432"), L"Virtual Desktop Streamer\\OpenXR\\virtualdesktop-openxr-32.json");
    if (fs::env(L"ProgramW6432").empty()) vd = fs::join(fs::env(L"ProgramFiles"), L"Virtual Desktop Streamer\\OpenXR\\virtualdesktop-openxr-32.json");
    d.vdxr = fs::is_file(vd);
    for (const auto& p : steam_libraries()) if (fs::is_dir(fs::join(p, L"steamapps\\common\\SteamVR"))) d.steamVr = true;
    GameIni runtime; std::wstring why; std::string value;
    if (GameIni::load(fs::join(d.env.dataDir, L"xr.ini"), &runtime, &why) && runtime.get_unique("runtime", "mode", &value)) d.runtime = value;
    GameIni settings;
    if (GameIni::load(fs::join(d.env.dataDir, L"launcher.ini"), &settings, &why) && settings.get_unique("Headset", "Model", &value)) d.headset = value;
    GameIni ini;
    if (GameIni::load(d.env.gameIni, &ini, &why)) {
        std::string w, h, fw, fh;
        d.gameIniValid = ini.get_unique("WinDrv.WindowsClient", "WindowedViewportX", &w) &&
            ini.get_unique("WinDrv.WindowsClient", "WindowedViewportY", &h) &&
            ini.get_unique("WinDrv.WindowsClient", "FullscreenViewportX", &fw) &&
            ini.get_unique("WinDrv.WindowsClient", "FullscreenViewportY", &fh);
        std::string fullscreen;
        const bool full = ini.get_unique("WinDrv.WindowsClient", "StartupFullscreen", &fullscreen) && _stricmp(fullscreen.c_str(), "True") == 0;
        if (d.gameIniValid) { d.width = strtoul((full ? fw : w).c_str(), nullptr, 10); d.height = strtoul((full ? fh : h).c_str(), nullptr, 10); }
    }
    read_preferences(d.env, &d.preferences);
    if (!d.found) { d.error = "Choose the BioShock Remastered folder containing the 32 bit BioshockHD.exe."; return d; }
    d.writable = fs::probe_writable(d.env.gameDir, nullptr);
    const auto proxy = fs::join(d.env.gameDir, L"xinput1_3.dll");
    const auto mod = fs::join(d.env.gameDir, L"bioshockvr.dll");
    const auto disabled = fs::join(d.env.gameDir, kDisabledMod);
    d.disabled = !fs::exists(mod) && fs::is_file(disabled);
    const auto installed = d.disabled ? disabled : mod;
    fs::sha256_file(installed, &d.installedHash, nullptr);
    // The unique forwarding-proxy message establishes the existing trilogy
    // loader's identity; a neighboring INI is not sufficient evidence.
    const bool knownProxy = contains(proxy, "[bioshockvr-proxy] failed to load bioshockvr.dll");
    d.installed = knownProxy && pe32_file(installed);
    Json::Value record;
    d.managed = read_json(fs::join(d.env.gameDir, kRecord), &record) && record["product"].isString() &&
        record["product"].asString() == "BioShockRemasteredVR" && record["schema"].isInt() && record["schema"].asInt() == 1;
    if (d.managed) {
        if (record["version"].isString()) d.installedVersion = record["version"].asString();
        if (record["build"].isString()) d.installedBuild = record["build"].asString();
    }
    if (d.installedVersion.empty() && d.installed) d.installedVersion = "Existing installation";
    d.matches = d.installed && payload.valid;
    for (const auto& f : payload.files) {
        std::string hash;
        const auto name = d.disabled && f.name == L"bioshockvr.dll" ? kDisabledMod : f.name;
        if (!fs::sha256_file(fs::join(d.env.gameDir, name), &hash, nullptr) || hash != f.hash) d.matches = false;
    }
    // The retired dxgi loader would load a second VR implementation. Only
    // identify that loader's exact log marker; do not classify every dxgi.dll.
    // The retired proxy's marker is UTF-16 in its PE .rdata.
    const std::wstring marker = L"BioshockVR_loader.log";
    std::vector<unsigned char> oldProxy;
    if (fs::read_file(fs::join(d.env.gameDir, L"dxgi.dll"), &oldProxy, nullptr)) {
        const auto* first = reinterpret_cast<const unsigned char*>(marker.data());
        d.oldModConflict = std::search(oldProxy.begin(), oldProxy.end(), first, first + marker.size()*sizeof(wchar_t)) != oldProxy.end();
    }
    if (d.oldModConflict) d.error = "An older BioShock VR DXGI loader is present. Remove that mod before installing this one.";
    return d;
}
}
