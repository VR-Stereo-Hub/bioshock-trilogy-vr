// core/vr/apilayer_guard.cpp
//
// ===========================================================================
// A 64-BIT IMPLICIT API LAYER KILLS VR IN THIS 32-BIT PROCESS. FIND IT FIRST.
//
// Ported from the Dishonored VR mod (src/core/vr/apilayer_guard.cpp there),
// where it was measured 2026-09-06. An OBS mirror capture layer had registered
// itself as an IMPLICIT OpenXR API layer:
//
//   HKCU\SOFTWARE\Khronos\OpenXR\1\ApiLayers\Implicit
//     C:\Users\<user>\AppData\Local\OpenXR-OBSMirror\
//       XR_APILAYER_NOVENDOR_OBSMirror.json = 0        (0 means ENABLED)
//
// Its library is x64 only. HKCU\SOFTWARE is NOT subject to WOW64 registry
// redirection, so this 32-bit process sees the same key a 64-bit one does, the
// loader tries to load a 64-bit DLL into it, and xrCreateInstance fails with
// XR_ERROR_FILE_ACCESS_ERROR (-32).
//
// The failure is at INSTANCE creation, which is before any runtime is chosen,
// so it takes down the native runtime AND the SteamVR shim with the same error
// - and the shim's "is SteamVR installed?" message sends the diagnosis in
// entirely the wrong direction. An implicit layer is opt-out by design: nobody
// asked for it, and nothing in this mod's configuration mentions it.
//
// All three games here are 32-bit, so this is core rather than per-game, and it
// changes nothing on a machine with no such layer registered: the scan reports
// and returns.
//
// WHAT THIS DOES, AND WHAT IT DELIBERATELY DOES NOT.
//
// It reads the implicit-layer registry keys, reads each enabled layer's
// manifest, resolves its library, and reads the PE header's machine type. A
// layer whose library is not x86 cannot possibly load here, so its manifest's
// own `disable_environment` variable is set in THIS PROCESS before the loader
// runs. That is the documented opt-out and the layer's author chose the name.
//
// It does NOT write the registry. The layer stays installed and keeps working
// for every 64-bit application on the machine; only this process skips it. A
// mod that quietly disabled someone's capture software system-wide to fix its
// own launch would be worse than the bug.
//
// A layer with no `disable_environment` cannot be opted out of, and that is
// reported as the actionable thing it is rather than swallowed.
//
// LANE: called from framework::init() immediately before vr::init_instance().
// The first OpenXR call LoadLibrary's the runtime and every implicit layer, so
// this has to be ahead of it; it is well past the loader lock, where advapi32
// and the file APIs are usable.
//
// The opt-out switch is xr.ini [runtime] disable_bad_api_layers=0/1, beside the
// runtime mode key and for the same reason: an F10 preset save rewrites
// vrpreset.ini wholesale and would silently drop a key it does not know.
// ===========================================================================

#include "core/vr/apilayer_guard.h"

#include "core/util/log.h"

#include <windows.h>
#include <shlobj.h>

#include <cstdio>
#include <cstring>

namespace bvr::vr {
namespace {

// The PE machine type of a DLL, without loading it. 0 if it cannot be read.
WORD layer_machine(const char* path) {
    HANDLE f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return 0;
    WORD machine = 0;
    DWORD got = 0;
    LONG peOff = 0;
    BYTE mz[2] = {0, 0};
    if (ReadFile(f, mz, 2, &got, nullptr) && got == 2 && mz[0] == 'M' && mz[1] == 'Z' &&
        SetFilePointer(f, 0x3C, nullptr, FILE_BEGIN) != INVALID_SET_FILE_POINTER &&
        ReadFile(f, &peOff, 4, &got, nullptr) && got == 4 && peOff > 0 &&
        SetFilePointer(f, peOff, nullptr, FILE_BEGIN) != INVALID_SET_FILE_POINTER) {
        DWORD sig = 0;
        if (ReadFile(f, &sig, 4, &got, nullptr) && got == 4 && sig == 0x00004550)
            ReadFile(f, &machine, 2, &got, nullptr);
    }
    CloseHandle(f);
    return machine;
}

// Pull one string value out of a flat JSON manifest. These files are small,
// hand-written and shallow, so a scan for "key" followed by its string is
// enough - and a parser that can be wrong in a new way is not wanted in a path
// whose whole job is to stop a launch failing.
bool json_str(const char* json, const char* key, char* out, size_t cap) {
    out[0] = 0;
    char pat[64];
    _snprintf_s(pat, sizeof(pat), _TRUNCATE, "\"%s\"", key);
    const char* p = strstr(json, pat);
    if (!p) return false;
    p = strchr(p + strlen(pat), ':');
    if (!p) return false;
    p++;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    if (*p != '"') return false;
    p++;
    size_t n = 0;
    while (*p && *p != '"' && n + 1 < cap) {
        if (*p == '\\' && p[1]) { // the manifests escape backslashes
            p++;
            out[n++] = *p++;
            continue;
        }
        out[n++] = *p++;
    }
    out[n] = 0;
    return n > 0;
}

bool read_manifest(const char* path, char* buf, size_t cap) {
    HANDLE f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD got = 0;
    const BOOL ok = ReadFile(f, buf, static_cast<DWORD>(cap - 1), &got, nullptr);
    CloseHandle(f);
    if (!ok) return false;
    buf[got] = 0;
    return got > 0;
}

// xr.ini [runtime] disable_bad_api_layers. Defaults to on; the file usually
// does not exist, and that is the intended setup.
bool guard_enabled() {
    wchar_t local[MAX_PATH];
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, local))) return true;
    wchar_t ini[MAX_PATH];
    swprintf_s(ini, L"%s\\BioshockVR\\xr.ini", local);
    if (GetFileAttributesW(ini) == INVALID_FILE_ATTRIBUTES) return true;
    return GetPrivateProfileIntW(L"runtime", L"disable_bad_api_layers", 1, ini) != 0;
}

// One implicit layer: is it loadable here, and if not, can it be switched off?
void check_layer(const char* manifest, DWORD disabled, const char* where, bool enabled, int* bad,
                 int* stuck) {
    if (disabled) {
        BVR_LOG("apilayer: %s '%s' is already disabled by its registry value (%lu; 0 would "
                "mean enabled)",
                where, manifest, static_cast<unsigned long>(disabled));
        return;
    }

    char json[8192];
    if (!read_manifest(manifest, json, sizeof(json))) {
        BVR_LOG("apilayer: %s '%s' is registered ENABLED but its manifest will not read. The "
                "loader will hit the same wall and xrCreateInstance can fail with "
                "XR_ERROR_FILE_ACCESS_ERROR (-32) for every runtime, native and shim alike.",
                where, manifest);
        (*stuck)++;
        return;
    }

    char lib[MAX_PATH] = "", name[128] = "", dis[128] = "";
    json_str(json, "library_path", lib, sizeof(lib));
    json_str(json, "name", name, sizeof(name));
    json_str(json, "disable_environment", dis, sizeof(dis));
    if (!lib[0]) return;

    // A relative library_path is relative to the manifest's own directory.
    char full[MAX_PATH * 2];
    if (lib[1] == ':' || (lib[0] == '\\' && lib[1] == '\\')) {
        _snprintf_s(full, sizeof(full), _TRUNCATE, "%s", lib);
    } else {
        char dir[MAX_PATH];
        _snprintf_s(dir, MAX_PATH, _TRUNCATE, "%s", manifest);
        char* slash = strrchr(dir, '\\');
        if (slash) *slash = 0;
        else dir[0] = 0;
        const char* rel = lib;
        if (rel[0] == '.' && rel[1] == '\\') rel += 2;
        _snprintf_s(full, sizeof(full), _TRUNCATE, "%s\\%s", dir, rel);
    }

    const WORD m = layer_machine(full);
    const bool loadable = (m == IMAGE_FILE_MACHINE_I386);
    BVR_LOG("apilayer: %s '%s' -> %s (%s)%s", where, name[0] ? name : manifest, full,
            m == IMAGE_FILE_MACHINE_I386    ? "x86, loadable here"
            : m == IMAGE_FILE_MACHINE_AMD64 ? "x64 - CANNOT load in this 32-bit process"
            : m == 0                        ? "the library could not be read"
                                            : "an unexpected machine type",
            loadable ? "" : "  <-- this is enough to fail xrCreateInstance");
    if (loadable) return;

    (*bad)++;

    if (!dis[0]) {
        BVR_LOG("apilayer: '%s' cannot load here and its manifest declares no "
                "disable_environment, so there is no way to opt this process out of it. VR "
                "will not start while it is enabled. Set its registry value under %s to 1 to "
                "disable it, or uninstall the layer - this mod will not edit the registry for "
                "you.",
                name[0] ? name : manifest, where);
        (*stuck)++;
        return;
    }

    if (!enabled) {
        BVR_LOG("apilayer: '%s' cannot load in a 32-bit process and xr.ini [runtime] "
                "disable_bad_api_layers=0 says to leave it alone. Expect xrCreateInstance to "
                "fail with XrResult(-32) for every runtime and the game to run flat. Set the "
                "key back to 1 to have this handled.",
                name[0] ? name : manifest);
        (*stuck)++;
        return;
    }

    SetEnvironmentVariableA(dis, "1");
    BVR_LOG("apilayer: DISABLED '%s' for this process by setting %s=1, the opt-out its own "
            "manifest declares. Nothing was written to the registry: the layer stays "
            "installed and keeps working for every 64-bit application. If VR still refuses, "
            "this was not the cause and the xr: lines below say what is.",
            name[0] ? name : manifest, dis);
    BVR_LOG("apilayer:   THE COST, so it is not a mystery later: a capture or overlay layer "
            "that is switched off cannot see this game's OpenXR session, and tools built on "
            "one will report that no OpenXR application is running - some of them guess at a "
            "different VR API rather than saying they cannot see it. That is not fixable by "
            "configuration: a 64-bit layer can never load into a 32-bit process. Record the "
            "game window, or capture from the headset runtime's own mirror, instead.");
}

void scan_key(HKEY root, const char* subkey, REGSAM view, const char* where, bool enabled,
              int* seen, int* bad, int* stuck) {
    HKEY k = nullptr;
    if (RegOpenKeyExA(root, subkey, 0, KEY_READ | view, &k) != ERROR_SUCCESS) return;
    for (DWORD i = 0;; i++) {
        char name[MAX_PATH * 2];
        DWORD nlen = sizeof(name), type = 0, data = 0, dlen = sizeof(data);
        const LONG r =
            RegEnumValueA(k, i, name, &nlen, nullptr, &type, reinterpret_cast<LPBYTE>(&data),
                          &dlen);
        if (r == ERROR_NO_MORE_ITEMS) break;
        if (r != ERROR_SUCCESS) continue;
        if (type != REG_DWORD) continue;
        (*seen)++;
        check_layer(name, data, where, enabled, bad, stuck);
    }
    RegCloseKey(k);
}

} // namespace

// Every place the OpenXR loader looks for implicit layers. HKCU\SOFTWARE is not
// WOW64-redirected, so the plain key and the WOW6432Node one are both read for
// HKLM and the same list is walked for HKCU - a layer registered in any of them
// reaches this process.
void apilayer_guard() {
    static const char* kPath = "SOFTWARE\\Khronos\\OpenXR\\1\\ApiLayers\\Implicit";

    // The scan always runs and always reports - knowing which layers are
    // registered and which of them cannot load here is worth having in every
    // log, and it costs three registry reads. Only the ACT of disabling one is
    // behind the key.
    const bool enabled = guard_enabled();
    if (!enabled)
        BVR_LOG("apilayer: xr.ini [runtime] disable_bad_api_layers=0 - layers will be reported "
                "but not switched off");

    int seen = 0, bad = 0, stuck = 0;
    scan_key(HKEY_CURRENT_USER, kPath, 0, "HKCU", enabled, &seen, &bad, &stuck);
    scan_key(HKEY_LOCAL_MACHINE, kPath, KEY_WOW64_32KEY, "HKLM(32)", enabled, &seen, &bad, &stuck);
    scan_key(HKEY_LOCAL_MACHINE, kPath, KEY_WOW64_64KEY, "HKLM(64)", enabled, &seen, &bad, &stuck);

    if (!seen) {
        BVR_LOG("apilayer: no implicit OpenXR API layers are registered, so none can be the "
                "reason VR fails to start");
        return;
    }

    BVR_LOG("apilayer: %d implicit layer(s) registered, %d unloadable in a 32-bit process, %d "
            "of those with no way to opt out. An implicit layer is opt-out by design - nothing "
            "in this mod's configuration asks for one - and a single unloadable layer fails "
            "xrCreateInstance for EVERY runtime, which reads as 'no VR at all' rather than as "
            "a layer problem.",
            seen, bad, stuck);
}

} // namespace bvr::vr
