#include "framework.h"

#include "core/hooks/d3d11_hook.h"
#include "core/input/xinput_bridge.h"
#include "core/ui/overlay.h"
#include "core/util/crash.h"
#include "core/util/log.h"
#include "core/vr/openxr_runtime.h"
#include "game/adapter_registry.h"
#include "game/igame_adapter.h"

#include <windows.h>
#include <MinHook.h>

namespace bvr::framework {
namespace {

// PE header fields of an already-loaded module. TimeDateStamp is the field that
// actually identifies a build when a version string is ambiguous - it is how the
// session-23 crash report was finally attributed to a release.
const IMAGE_NT_HEADERS* nt_headers(HMODULE mod) {
    if (!mod) return nullptr;
    auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(mod);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return nullptr;
    auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(reinterpret_cast<const BYTE*>(mod) +
                                                         dos->e_lfanew);
    return nt->Signature == IMAGE_NT_SIGNATURE ? nt : nullptr;
}

HMODULE self_module() {
    HMODULE mod = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&self_module), &mod);
    return mod;
}

// One line that answers the questions every external crash report raises before
// anyone can start: which build, which Windows, how many cores, how much address
// space. Cheap, and it costs a round trip with the reporter if it is missing.
void log_environment() {
    HMODULE self = self_module();
    const IMAGE_NT_HEADERS* selfNt = nt_headers(self);
    BVR_LOG("build: %s (%s) module %p pe-timestamp 0x%08X", BVR_VERSION, BVR_BUILD_ID, self,
            selfNt ? selfNt->FileHeader.TimeDateStamp : 0u);

    // RtlGetVersion, not GetVersionEx: the latter lies without a manifest.
    RTL_OSVERSIONINFOW os{sizeof(os)};
    if (HMODULE ntdll = GetModuleHandleW(L"ntdll.dll")) {
        using Fn = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);
        if (auto fn = reinterpret_cast<Fn>(GetProcAddress(ntdll, "RtlGetVersion"))) fn(&os);
    }

    SYSTEM_INFO si{};
    GetNativeSystemInfo(&si);
    MEMORYSTATUSEX mem{sizeof(mem)};
    GlobalMemoryStatusEx(&mem);

    const IMAGE_NT_HEADERS* hostNt = nt_headers(GetModuleHandleW(nullptr));
    const bool laa = hostNt && (hostNt->FileHeader.Characteristics & IMAGE_FILE_LARGE_ADDRESS_AWARE);

    BVR_LOG("env: Windows %u.%u build %u, %u cores, %llu MB RAM, host LAA=%s "
            "(user address space %llu MB)",
            os.dwMajorVersion, os.dwMinorVersion, os.dwBuildNumber, si.dwNumberOfProcessors,
            static_cast<unsigned long long>(mem.ullTotalPhys >> 20), laa ? "yes" : "NO",
            static_cast<unsigned long long>(
                reinterpret_cast<uintptr_t>(si.lpMaximumApplicationAddress) >> 20));

    // THE HOST EXE FINGERPRINT (session 27). Until now the log identified the
    // mod's build but said nothing about the game's, while the adapters trust
    // roughly a hundred absolute RVAs derived from one specific build. Every
    // storefront ships the same file NAME, so exe-name matching cannot tell a
    // Steam build from an Epic or GOG one - which means a different binary is
    // detected as supported and then mis-scanned. This line is what lets a
    // user's log answer "is this the build our addresses are for?" in one look.
    if (hostNt) {
        LARGE_INTEGER fileSize{};
        wchar_t exeW[MAX_PATH]{};
        if (GetModuleFileNameW(nullptr, exeW, MAX_PATH)) {
            WIN32_FILE_ATTRIBUTE_DATA fad{};
            if (GetFileAttributesExW(exeW, GetFileExInfoStandard, &fad)) {
                fileSize.HighPart = static_cast<LONG>(fad.nFileSizeHigh);
                fileSize.LowPart = fad.nFileSizeLow;
            }
        }
        BVR_LOG("host exe: pe-timestamp 0x%08X size-of-image 0x%08X checksum 0x%08X "
                "file-bytes %llu",
                hostNt->FileHeader.TimeDateStamp, hostNt->OptionalHeader.SizeOfImage,
                hostNt->OptionalHeader.CheckSum,
                static_cast<unsigned long long>(fileSize.QuadPart));
    } else {
        BVR_LOG("host exe: PE headers unreadable - the build cannot be identified");
    }
}

} // namespace

void init() {
    // The exception filter goes on FIRST (session 27). Everything below it -
    // shell32 path lookups, MoveFileExW of the previous log, the PE header walk -
    // used to run with no handler installed, so a fault in any of it produced
    // neither a dump nor a log line.
    crash::install();

    // Host detection is silent by contract (the log does not exist yet); the
    // game layer supplies the per-game data subdir so two games never share a
    // log/config folder.
    log::init(game::host_data_subdir());
    BVR_LOG("bioshockvr %s starting", BVR_VERSION);

    char exePath[MAX_PATH];
    GetModuleFileNameA(nullptr, exePath, MAX_PATH);
    BVR_LOG("host process: %s (base %p)", exePath, GetModuleHandleW(nullptr));

    log_environment();

    MH_STATUS status = MH_Initialize();
    if (status != MH_OK) {
        BVR_LOG("MH_Initialize failed: %s - mod disabled, game runs flat",
                MH_StatusToString(status));
        return;
    }
    BVR_LOG("MinHook initialized");

    input::init(); // fail-soft: missing proxy seam just disables synthetic input

    game::init_adapter(); // fail-soft: scan/hook failure is logged, game runs flat

    // The swapchain hooks go in BEFORE the OpenXR instance (session 27). The
    // first OpenXR call LoadLibrary's the runtime and every implicit API layer,
    // and there is no timeout anywhere in that path - on a machine where the
    // runtime is waiting on its streamer it can block indefinitely. With the
    // old order that cost us the Present detour, and with it crash::rearm(),
    // whose only caller is that detour: the session then ran with whatever
    // exception filter had displaced ours (CSERHelper) and produced no dumps at
    // all. This way a hanging runtime costs VR and nothing else.
    if (!d3d11_hook::install()) {
        BVR_LOG("D3D11 hook install failed - mod disabled, game runs flat");
        return;
    }

    vr::init_instance(); // fail-soft: no runtime just means flat mode

    BVR_LOG("init complete; waiting for first Present");
}

} // namespace bvr::framework
