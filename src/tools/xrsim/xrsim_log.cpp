// bvr_xrsim32 logging and data directory.
//
// Shape copied from core/util/log.cpp so the two logs read alike and
// tools/tail-log.ps1 works on both: [HH:MM:SS.mmm] prefix, _SH_DENYNO so the
// file can be followed live, fflush on every line, one generation of history in
// xrsim.prev.log, and an OutputDebugStringA mirror.

#include "xrsim_common.h"

#include <shlobj.h>
#include <cstdarg>
#include <cstdio>
#include <share.h>

namespace xrsim::log {
namespace {

std::mutex g_mutex;
FILE* g_file = nullptr;
wchar_t g_dir[MAX_PATH] = {};
bool g_initDone = false;

} // namespace

void init() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_initDone) return;
    g_initDone = true;

    // BVR_XRSIM_DIR exists so a self-test process and a live game session cannot
    // fight over one state.json, and so a parallel Infinite session can have its
    // own. Absent, everything lands in the usual data dir.
    DWORD n = GetEnvironmentVariableW(L"BVR_XRSIM_DIR", g_dir, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) {
        wchar_t local[MAX_PATH];
        if (FAILED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, local))) return;
        swprintf_s(g_dir, L"%s\\BioshockVR", local);
        CreateDirectoryW(g_dir, nullptr);
        wcscat_s(g_dir, L"\\xrsim");
    }
    CreateDirectoryW(g_dir, nullptr);

    wchar_t captures[MAX_PATH];
    swprintf_s(captures, L"%s\\capture", g_dir);
    CreateDirectoryW(captures, nullptr);

    wchar_t path[MAX_PATH], prev[MAX_PATH];
    swprintf_s(path, L"%s\\xrsim.log", g_dir);
    swprintf_s(prev, L"%s\\xrsim.prev.log", g_dir);
    MoveFileExW(path, prev, MOVEFILE_REPLACE_EXISTING); // no-op on first run

    g_file = _wfsopen(path, L"w", _SH_DENYNO);
}

const wchar_t* dir() { return g_dir; }

void write(const char* fmt, ...) {
    char message[2048];
    va_list args;
    va_start(args, fmt);
    vsnprintf_s(message, _TRUNCATE, fmt, args);
    va_end(args);

    SYSTEMTIME st;
    GetLocalTime(&st);

    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_file) {
        fprintf(g_file, "[%02u:%02u:%02u.%03u] %s\n",
                st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, message);
        fflush(g_file);
    }
    char dbg[2100];
    sprintf_s(dbg, "[xrsim] %s\n", message);
    OutputDebugStringA(dbg);
}

} // namespace xrsim::log

namespace xrsim {

XrTime now_xr_time() {
    // XrTime on Windows is nanoseconds off the QPC epoch. Keeping the real QPC
    // base (rather than starting at zero) means predictedDisplayTime values are
    // in the same units and rough magnitude the mod sees from VDXR, so any
    // arithmetic it does on them behaves the same.
    static LARGE_INTEGER freq = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return f;
    }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return static_cast<XrTime>((c.QuadPart * 1000000000LL) / freq.QuadPart);
}

uint64_t now_ms() { return static_cast<uint64_t>(GetTickCount64()); }

XrResult on_seh(const char* what) {
    static std::atomic<uint32_t> s_count{0};
    const uint32_t n = ++s_count;
    if (n <= 8) {
        XRSIM_LOG("xrsim: SEH FAULT inside %s (#%u) - returning RUNTIME_FAILURE and continuing", what, n);
    }
    return XR_ERROR_RUNTIME_FAILURE;
}

void rig_defaults(Rig& r) {
    r = Rig{};
    r.head = pose_identity();
    r.head.p = Vec3{0.0f, 1.6f, 0.0f}; // standing eye height
    r.headValid = true;

    for (int h = 0; h < 2; ++h) {
        r.handValid[h] = true;
        r.handFollowsHead[h] = true;
        // Hands parked where a relaxed grip sits: out to the side, below the
        // eyes, slightly forward. Signed on X so left is negative.
        r.handOffset[h] = Vec3{(h == 0) ? -0.20f : 0.20f, -0.30f, -0.35f};
        r.grip[h] = pose_identity();
        r.aim[h] = pose_identity();
        // The aim pose runs along the pointing direction; the grip pose runs
        // along the controller HANDLE, and on Touch those are tens of degrees
        // apart (ENGINE_NOTES "Grip pose vs aim pose", M6). 40 degrees of pitch
        // is the usual separation, so a sim aim ray lands where a real one does.
        r.aimTrimPitch[h] = -40.0f;
        r.aimTrimYaw[h] = 0.0f;
    }

    r.ipdM = 0.063f;
    r.worldScale = 1.0f;

    // The Quest 3 optics live HERE, not at the one call site that happens to run
    // first. They were originally set only in rig_staging_init(), so `reset`,
    // `fov quest3` and `hands reset` - all of which go through rig_defaults -
    // silently zeroed the field of view and every capture came out black. A
    // default that only one caller applies is not a default.
    //
    // PINNED to this machine's measured VDXR values (session 37, closing the
    // session-34 open item): the mod's real-headset log line reads
    // "headset fov half-angles h=54.0 v=55.0" (docs/bioshock2/ENGINE_NOTES.md,
    // Quest 3 via Virtual Desktop). The published-figures guess this replaces
    // (h=55 v=48) made the sim eye WIDE and SHORT while the real eye is
    // essentially square - which flips which branch of the mod's FOV
    // circumscription wins, so FOV-derived sim numbers disagreed with the
    // headset. Outward asymmetry shape kept (10 deg inward reduction); the
    // MAX half-angles are what the mod consumes and they now match. With
    // these the mod's line must read h=54.0 v=55.0.
    r.fov[0] = Fov{deg2rad(-54.0f), deg2rad(44.0f), deg2rad(55.0f), deg2rad(-55.0f)};
    r.fov[1] = Fov{deg2rad(-44.0f), deg2rad(54.0f), deg2rad(55.0f), deg2rad(-55.0f)};
}

bool fov_is_degenerate(const Fov& f) {
    return (f.angleRight - f.angleLeft) < 1e-4f || (f.angleUp - f.angleDown) < 1e-4f;
}

} // namespace xrsim
