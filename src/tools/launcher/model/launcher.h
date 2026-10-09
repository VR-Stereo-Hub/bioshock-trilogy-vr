#pragma once
#include <windows.h>
#include <map>
#include <string>
#include <vector>
#include "sys/gpu.h"
#include "sys/process.h"

namespace bvr::launcher {
inline constexpr wchar_t kGameExe[] = L"BioshockHD.exe";
inline constexpr wchar_t kSteamUrl[] = L"steam://rungameid/409710";
inline constexpr wchar_t kReleasesUrl[] = L"https://github.com/VR-Stereo-Hub/bioshock-trilogy-vr/releases";
inline constexpr wchar_t kRecord[] = L"bioshockvr-install.json";
inline constexpr wchar_t kDisabledMod[] = L"bioshockvr.dll.disabled";

struct Environment {
    std::wstring gameDir, dataDir, gameIni;
};
struct Choices {
    // Empty / zero means preserve, never infer an edit from a UI preselection.
    std::string runtime;
    unsigned width = 0, height = 0;
    std::string headset;
    std::map<std::string, float> preferences;
    bool dirty() const { return !runtime.empty() || width || height || !headset.empty() || !preferences.empty(); }
};
struct PayloadFile {
    std::wstring name;
    std::vector<unsigned char> bytes;
    std::string hash;
};
struct Payload {
    std::vector<PayloadFile> files;
    std::string version, build, configuration;
    bool valid = false;
    std::string error;
};
struct Detection {
    Environment env;
    bool found = false, installed = false, managed = false, disabled = false;
    bool matches = false, payloadOk = false, writable = false, nativePresent = false;
    bool steamVr = false, vdxr = false, oldModConflict = false, gameIniValid = false;
    process::Running running = process::Running::Unknown;
    unsigned width = 0, height = 0;
    std::string source, runtime = "auto", headset, installedVersion, installedBuild;
    std::string installedHash, activeRuntime, error;
    gpu::Info graphics;
    std::map<std::string, float> preferences;
};
enum class Status { Ok, Note, Warning, Failed };
struct Step { Status status; std::string title, detail; };
struct Report {
    bool ok = true;
    DWORD error = 0;
    std::vector<Step> steps;
    std::wstring backupDir;
    void add(Status status, const std::string& title, const std::string& detail = "");
    void fail(const std::string& title, const std::string& detail, DWORD code = 0);
};

Payload embedded_payload();
Environment resolve_environment(Environment env);
Detection detect(const Environment& env, const Payload& payload);
bool pe32_file(const std::wstring& path);
std::wstring normalise_game_dir(const std::wstring& chosen);
std::vector<std::wstring> steam_libraries();
// All operations re-detect immediately before planning writes. No cached UI
// ownership or running-process observation authorizes a mutation.
Report apply(const Environment& env, const Payload& payload, const std::string& operation,
             const Choices& choices = {});
std::string report_json(const Report& report);
bool parse_report(const std::string& json, Report* report);
std::string detection_json(const Detection& detection, const Payload& payload);
bool valid_choices(const Choices& choices, std::string* why);
bool patch_preferences(const std::string& source, const std::map<std::string, float>& edits,
                       std::string* output, std::string* why);
void read_preferences(const Environment& env, std::map<std::string, float>* values);
// Test-only fault injection is linked exclusively into the host-test target.
#ifdef BVR_LAUNCHER_TESTS
void fail_transaction_after(int count);
#endif
}
