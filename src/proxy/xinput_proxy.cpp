// xinput1_3.dll proxy shim.
//
// BioshockHD.exe statically imports XINPUT1_3.dll by ordinal (2, 3), so a DLL
// with this name in the game folder is loaded at process start. We forward
// every export to the real DLL in the system directory and load bioshockvr.dll
// (the actual mod) from our own folder.
//
// Injection vector proven on this exe by itsloopyo/bioshock-remastered-headtracking (MIT).
//
// Ordinals in xinput_proxy.def mirror the real xinput1_3.dll export table —
// the game's ordinal imports resolve against our table, so they must match.

#include <windows.h>
#include <mutex>

namespace {

using GetStateFn = DWORD(WINAPI*)(DWORD, void*);
using SetStateFn = DWORD(WINAPI*)(DWORD, void*);
using GetCapabilitiesFn = DWORD(WINAPI*)(DWORD, DWORD, void*);
using EnableFn = void(WINAPI*)(BOOL);
using GetDSoundGuidsFn = DWORD(WINAPI*)(DWORD, GUID*, GUID*);
using GetBatteryInfoFn = DWORD(WINAPI*)(DWORD, BYTE, void*);
using GetKeystrokeFn = DWORD(WINAPI*)(DWORD, DWORD, void*);

struct RealXInput {
    HMODULE module = nullptr;
    GetStateFn GetState = nullptr;
    GetStateFn GetStateEx = nullptr;   // ordinal 100, same shape as GetState
    SetStateFn SetState = nullptr;
    GetCapabilitiesFn GetCapabilities = nullptr;
    EnableFn Enable = nullptr;
    GetDSoundGuidsFn GetDSoundGuids = nullptr;
    GetBatteryInfoFn GetBatteryInfo = nullptr;
    GetKeystrokeFn GetKeystroke = nullptr;
};

RealXInput g_real;
std::once_flag g_initOnce;

// Seam for the mod: called after the real XInputGetState so bioshockvr.dll can
// compose synthetic controller state (motion controllers as gamepad) later.
using PostGetStateHook = void(WINAPI*)(DWORD userIndex, void* state, DWORD* result);
PostGetStateHook g_postGetState = nullptr;

void LoadReal() {
    // GetSystemDirectory returns the WOW64-redirected system dir for a 32-bit
    // process, i.e. C:\Windows\SysWOW64 — exactly the real 32-bit DLL we want.
    wchar_t path[MAX_PATH];
    UINT len = GetSystemDirectoryW(path, MAX_PATH);
    if (len == 0 || len >= MAX_PATH - 16) return;
    lstrcatW(path, L"\\xinput1_3.dll");

    g_real.module = LoadLibraryW(path);
    if (!g_real.module) {
        OutputDebugStringA("[bioshockvr-proxy] failed to load real xinput1_3.dll\n");
        return;
    }
    g_real.GetState = reinterpret_cast<GetStateFn>(GetProcAddress(g_real.module, "XInputGetState"));
    g_real.GetStateEx = reinterpret_cast<GetStateFn>(GetProcAddress(g_real.module, MAKEINTRESOURCEA(100)));
    g_real.SetState = reinterpret_cast<SetStateFn>(GetProcAddress(g_real.module, "XInputSetState"));
    g_real.GetCapabilities = reinterpret_cast<GetCapabilitiesFn>(GetProcAddress(g_real.module, "XInputGetCapabilities"));
    g_real.Enable = reinterpret_cast<EnableFn>(GetProcAddress(g_real.module, "XInputEnable"));
    g_real.GetDSoundGuids = reinterpret_cast<GetDSoundGuidsFn>(GetProcAddress(g_real.module, "XInputGetDSoundAudioDeviceGuids"));
    g_real.GetBatteryInfo = reinterpret_cast<GetBatteryInfoFn>(GetProcAddress(g_real.module, "XInputGetBatteryInformation"));
    g_real.GetKeystroke = reinterpret_cast<GetKeystrokeFn>(GetProcAddress(g_real.module, "XInputGetKeystroke"));
}

void LoadMod() {
    // bioshockvr.dll sits next to this shim in the game folder; load it by full
    // path so we never pick up a stray copy from the DLL search path.
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                       reinterpret_cast<LPCWSTR>(&LoadMod), &self);
    wchar_t path[MAX_PATH];
    DWORD len = GetModuleFileNameW(self, path, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) return;
    wchar_t* slash = wcsrchr(path, L'\\');
    if (!slash || (slash - path) + 16 >= MAX_PATH) return;
    lstrcpyW(slash + 1, L"bioshockvr.dll");

    if (!LoadLibraryW(path)) {
        OutputDebugStringA("[bioshockvr-proxy] failed to load bioshockvr.dll\n");
    }
}

void EnsureInit() {
    std::call_once(g_initOnce, [] {
        LoadReal();
        LoadMod();
    });
}

DWORD WINAPI InitThread(LPVOID) {
    EnsureInit();
    return 0;
}

} // namespace

extern "C" {

DWORD WINAPI XInputGetState(DWORD userIndex, void* state) {
    EnsureInit();
    DWORD result = g_real.GetState ? g_real.GetState(userIndex, state)
                                   : ERROR_DEVICE_NOT_CONNECTED;
    if (g_postGetState) g_postGetState(userIndex, state, &result);
    return result;
}

DWORD WINAPI XInputGetStateEx(DWORD userIndex, void* state) {
    EnsureInit();
    DWORD result = g_real.GetStateEx ? g_real.GetStateEx(userIndex, state)
                                     : ERROR_DEVICE_NOT_CONNECTED;
    if (g_postGetState) g_postGetState(userIndex, state, &result);
    return result;
}

DWORD WINAPI XInputSetState(DWORD userIndex, void* vibration) {
    EnsureInit();
    return g_real.SetState ? g_real.SetState(userIndex, vibration)
                           : ERROR_DEVICE_NOT_CONNECTED;
}

DWORD WINAPI XInputGetCapabilities(DWORD userIndex, DWORD flags, void* caps) {
    EnsureInit();
    return g_real.GetCapabilities ? g_real.GetCapabilities(userIndex, flags, caps)
                                  : ERROR_DEVICE_NOT_CONNECTED;
}

void WINAPI XInputEnable(BOOL enable) {
    EnsureInit();
    if (g_real.Enable) g_real.Enable(enable);
}

DWORD WINAPI XInputGetDSoundAudioDeviceGuids(DWORD userIndex, GUID* render, GUID* capture) {
    EnsureInit();
    return g_real.GetDSoundGuids ? g_real.GetDSoundGuids(userIndex, render, capture)
                                 : ERROR_DEVICE_NOT_CONNECTED;
}

DWORD WINAPI XInputGetBatteryInformation(DWORD userIndex, BYTE devType, void* battery) {
    EnsureInit();
    return g_real.GetBatteryInfo ? g_real.GetBatteryInfo(userIndex, devType, battery)
                                 : ERROR_DEVICE_NOT_CONNECTED;
}

DWORD WINAPI XInputGetKeystroke(DWORD userIndex, DWORD reserved, void* keystroke) {
    EnsureInit();
    return g_real.GetKeystroke ? g_real.GetKeystroke(userIndex, reserved, keystroke)
                               : ERROR_DEVICE_NOT_CONNECTED;
}

void WINAPI BVR_SetPostGetStateHook(PostGetStateHook hook) {
    g_postGetState = hook;
}

} // extern "C"

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(instance);
        // LoadLibrary is illegal under loader lock; do the real work on a
        // thread that starts once the loader releases it. Exports that get
        // called earlier fall back to lazy EnsureInit().
        HANDLE thread = CreateThread(nullptr, 0, InitThread, nullptr, 0, nullptr);
        if (thread) CloseHandle(thread);
    }
    return TRUE;
}
