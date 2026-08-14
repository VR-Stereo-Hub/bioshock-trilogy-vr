// ============================================================================
//  ovrshim_main.cpp - instance/session/space/swapchain/frame entry points.
//
//  Adapted from BioVRDev/Bioshock-Remastered-VR OpenXRShim/src/shim_main.cpp
//  with the author's permission (see THIRD_PARTY_NOTICES.md). Deltas vs donor:
//    - runtime shape: no dllexports here; dispatch lives in ovrshim_negotiate
//    - NEW: xrEnumerateInstanceExtensionProperties (the mod's FIRST call),
//      xrGetSystemProperties, xrResultToString
//    - quad cap raised 8 -> kMaxQuadLayers (16) with a log-once overflow
//    - eye targets follow LATER larger swapchains too (BS2 mid-session
//      resolution change); xrDestroySwapchain actually frees
//    - log file in %LOCALAPPDATA%\BioshockVR\ovrshim.log
// ============================================================================
#include "ovrshim.h"
#include <cstdarg>
#include <cstring>
#include <cmath>
#include <cstdlib>
#include <share.h>
#include <vector>
#include <string>

VrApi     g_vr;
ShimState g_st;

// ---------------------------------------------------------------- logging
static char g_moduleDir[MAX_PATH] = {};
static FILE* g_log = nullptr;
static CRITICAL_SECTION g_logCs;
static bool g_logCsInit = false;

extern "C" IMAGE_DOS_HEADER __ImageBase;

static void EnsureModuleDir()
{
    if (g_moduleDir[0]) return;
    char path[MAX_PATH] = {};
    GetModuleFileNameA((HINSTANCE)&__ImageBase, path, MAX_PATH);
    strcpy_s(g_moduleDir, path);
    char* slash = strrchr(g_moduleDir, '\\');
    if (slash) *slash = 0;
}

const char* ShimModuleDir()
{
    EnsureModuleDir();
    return g_moduleDir;
}

void ShimLog(const char* fmt, ...)
{
    if (!g_logCsInit) { InitializeCriticalSection(&g_logCs); g_logCsInit = true; }
    EnterCriticalSection(&g_logCs);
    if (!g_log)
    {
        // %LOCALAPPDATA%\BioshockVR\ovrshim.log - beside the mod's other logs
        // (game folders may not be writable). Fall back to the module dir.
        char path[MAX_PATH] = {};
        char base[MAX_PATH] = {};
        DWORD n = GetEnvironmentVariableA("LOCALAPPDATA", base, MAX_PATH);
        if (n > 0 && n < MAX_PATH)
        {
            char dir[MAX_PATH];
            _snprintf_s(dir, MAX_PATH, _TRUNCATE, "%s\\BioshockVR", base);
            CreateDirectoryA(dir, nullptr);
            _snprintf_s(path, MAX_PATH, _TRUNCATE, "%s\\ovrshim.log", dir);
        }
        else
        {
            EnsureModuleDir();
            _snprintf_s(path, MAX_PATH, _TRUNCATE, "%s\\ovrshim.log", g_moduleDir);
        }
        g_log = _fsopen(path, "w", _SH_DENYNO); // tailable while the game runs
    }

    if (g_log)
    {
        SYSTEMTIME t; GetLocalTime(&t);
        fprintf(g_log, "[%02u:%02u:%02u.%03u] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
        va_list a; va_start(a, fmt);
        vfprintf(g_log, fmt, a);
        va_end(a);
        fprintf(g_log, "\n");
        fflush(g_log);
    }
    LeaveCriticalSection(&g_logCs);
}

// ---------------------------------------------------------------- math
M34 M34_Identity()
{
    M34 r = {};
    r.m[0][0] = r.m[1][1] = r.m[2][2] = 1.f;
    return r;
}

M34 M34_Mul(const M34& a, const M34& b)
{
    M34 r;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 4; ++j)
        {
            r.m[i][j] = a.m[i][0] * b.m[0][j] + a.m[i][1] * b.m[1][j] + a.m[i][2] * b.m[2][j];
            if (j == 3) r.m[i][j] += a.m[i][3];
        }
    return r;
}

M34 M34_InvRigid(const M34& a)
{
    M34 r;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            r.m[i][j] = a.m[j][i];                       // transpose rotation
    for (int i = 0; i < 3; ++i)
        r.m[i][3] = -(r.m[i][0] * a.m[0][3] + r.m[i][1] * a.m[1][3] + r.m[i][2] * a.m[2][3]);
    return r;
}

void M34_XformPoint(const M34& a, const float in[3], float out[3])
{
    for (int i = 0; i < 3; ++i)
        out[i] = a.m[i][0] * in[0] + a.m[i][1] * in[1] + a.m[i][2] * in[2] + a.m[i][3];
}

M34 M34_FromQuatPos(const float q[4], const float p[3])
{
    const float x = q[0], y = q[1], z = q[2], w = q[3];
    M34 r;
    r.m[0][0] = 1 - 2 * (y * y + z * z); r.m[0][1] = 2 * (x * y - z * w);     r.m[0][2] = 2 * (x * z + y * w);     r.m[0][3] = p[0];
    r.m[1][0] = 2 * (x * y + z * w);     r.m[1][1] = 1 - 2 * (x * x + z * z); r.m[1][2] = 2 * (y * z - x * w);     r.m[1][3] = p[1];
    r.m[2][0] = 2 * (x * z - y * w);     r.m[2][1] = 2 * (y * z + x * w);     r.m[2][2] = 1 - 2 * (x * x + y * y); r.m[2][3] = p[2];
    return r;
}

void M34_ToQuatPos(const M34& a, float q[4], float p[3])
{
    const float tr = a.m[0][0] + a.m[1][1] + a.m[2][2];
    if (tr > 0.f)
    {
        float s = sqrtf(tr + 1.f) * 2.f;
        q[3] = 0.25f * s;
        q[0] = (a.m[2][1] - a.m[1][2]) / s;
        q[1] = (a.m[0][2] - a.m[2][0]) / s;
        q[2] = (a.m[1][0] - a.m[0][1]) / s;
    }
    else if (a.m[0][0] > a.m[1][1] && a.m[0][0] > a.m[2][2])
    {
        float s = sqrtf(1.f + a.m[0][0] - a.m[1][1] - a.m[2][2]) * 2.f;
        q[3] = (a.m[2][1] - a.m[1][2]) / s;
        q[0] = 0.25f * s;
        q[1] = (a.m[0][1] + a.m[1][0]) / s;
        q[2] = (a.m[0][2] + a.m[2][0]) / s;
    }
    else if (a.m[1][1] > a.m[2][2])
    {
        float s = sqrtf(1.f + a.m[1][1] - a.m[0][0] - a.m[2][2]) * 2.f;
        q[3] = (a.m[0][2] - a.m[2][0]) / s;
        q[0] = (a.m[0][1] + a.m[1][0]) / s;
        q[1] = 0.25f * s;
        q[2] = (a.m[1][2] + a.m[2][1]) / s;
    }
    else
    {
        float s = sqrtf(1.f + a.m[2][2] - a.m[0][0] - a.m[1][1]) * 2.f;
        q[3] = (a.m[1][0] - a.m[0][1]) / s;
        q[0] = (a.m[0][2] + a.m[2][0]) / s;
        q[1] = (a.m[1][2] + a.m[2][1]) / s;
        q[2] = 0.25f * s;
    }
    p[0] = a.m[0][3]; p[1] = a.m[1][3]; p[2] = a.m[2][3];
}

M34 M34_FromVr(const vr::HmdMatrix34_t& v)
{
    M34 r;
    memcpy(r.m, v.m, sizeof(r.m));
    return r;
}

// ---------------------------------------------------------------- events
static XrSessionState g_evQueue[32];
static int g_evHead = 0, g_evTail = 0;

void Events_Push(XrSessionState s)
{
    const int next = (g_evTail + 1) % 32;
    if (next == g_evHead) return;              // full: drop (should never happen)
    g_evQueue[g_evTail] = s;
    g_evTail = next;
}

bool Events_Pop(XrSessionState* out)
{
    if (g_evHead == g_evTail) return false;
    *out = g_evQueue[g_evHead];
    g_evHead = (g_evHead + 1) % 32;
    return true;
}

// ---------------------------------------------------------------- OpenVR init
// NOTE: the openvr_api.dll ENTRY POINTS are __cdecl (VR_CALLTYPE); only the
// FnTable methods are __stdcall. Getting this wrong imbalances the x86 stack
// on the first call - it crashed the donor's game on startup before this was
// fixed. Keep both conventions exactly as below.
typedef intptr_t (__cdecl *PFN_VR_InitInternal2)(EVRInitError*, EVRApplicationType, const char*);
typedef void     (__cdecl *PFN_VR_ShutdownInternal)();
typedef intptr_t (__cdecl *PFN_VR_GetGenericInterface)(const char*, EVRInitError*);
typedef bool     (__cdecl *PFN_VR_IsInterfaceVersionValid)(const char*);
typedef const char* (__cdecl *PFN_VR_GetVRInitErrorAsEnglishDescription)(EVRInitError);
typedef bool     (__cdecl *PFN_VR_IsHmdPresent)();

static PFN_VR_ShutdownInternal g_vrShutdown = nullptr;

static bool VrConnect()
{
    if (g_vr.ok) return true;

    EnsureModuleDir();
    char dllPath[MAX_PATH];
    _snprintf_s(dllPath, MAX_PATH, _TRUNCATE, "%s\\openvr_api.dll", g_moduleDir);
    g_vr.dll = LoadLibraryA(dllPath);
    if (!g_vr.dll) g_vr.dll = LoadLibraryA("openvr_api.dll");
    if (!g_vr.dll)
    {
        SLOG("!!! openvr_api.dll not found next to bvr_steamvr32.dll - reinstall the mod package");
        return false;
    }

    auto init  = (PFN_VR_InitInternal2)GetProcAddress(g_vr.dll, "VR_InitInternal2");
    auto getIf = (PFN_VR_GetGenericInterface)GetProcAddress(g_vr.dll, "VR_GetGenericInterface");
    auto errStr= (PFN_VR_GetVRInitErrorAsEnglishDescription)GetProcAddress(g_vr.dll, "VR_GetVRInitErrorAsEnglishDescription");
    g_vrShutdown = (PFN_VR_ShutdownInternal)GetProcAddress(g_vr.dll, "VR_ShutdownInternal");
    if (!init || !getIf)
    {
        SLOG("!!! openvr_api.dll is missing entry points (wrong dll?)");
        return false;
    }

    EVRInitError err = EVRInitError_VRInitError_None;
    init(&err, EVRApplicationType_VRApplication_Scene, "");
    if (err != EVRInitError_VRInitError_None)
    {
        SLOG("!!! VR_InitInternal2 failed: %d (%s)", (int)err,
             errStr ? errStr(err) : "?");
        SLOG("!!! Is SteamVR installed and the headset connected?");
        return false;
    }

    char buf[128];
    _snprintf_s(buf, 128, _TRUNCATE, "FnTable:%s", vr::IVRSystem_Version);
    g_vr.sys = (VR_IVRSystem_FnTable*)getIf(buf, &err);
    _snprintf_s(buf, 128, _TRUNCATE, "FnTable:%s", vr::IVRCompositor_Version);
    g_vr.comp = (VR_IVRCompositor_FnTable*)getIf(buf, &err);
    _snprintf_s(buf, 128, _TRUNCATE, "FnTable:%s", vr::IVRInput_Version);
    g_vr.input = (VR_IVRInput_FnTable*)getIf(buf, &err);
    _snprintf_s(buf, 128, _TRUNCATE, "FnTable:%s", vr::IVROverlay_Version);
    g_vr.ovl = (VR_IVROverlay_FnTable*)getIf(buf, &err);   // optional

    if (!g_vr.sys || !g_vr.comp || !g_vr.input)
    {
        SLOG("!!! VR_GetGenericInterface failed (sys=%p comp=%p input=%p) v(%s/%s/%s)",
             g_vr.sys, g_vr.comp, g_vr.input,
             vr::IVRSystem_Version, vr::IVRCompositor_Version, vr::IVRInput_Version);
        return false;
    }

    g_vr.comp->SetTrackingSpace(ETrackingUniverseOrigin_TrackingUniverseStanding);
    g_vr.ok = true;
    SLOG("OpenVR connected. Interfaces: %s / %s / %s",
         vr::IVRSystem_Version, vr::IVRCompositor_Version, vr::IVRInput_Version);
    return true;
}

// Full disconnect, so SteamVR stops counting us as a scene app. CONTRACT
// MISMATCH THIS SOLVES (found in the s62 null-driver E2E): an OpenXR app that
// gets STOPPING ends its session and keeps running, waiting for READY - but
// an OpenVR scene app that received VREvent_Quit and does not EXIT the
// process is hard-killed by vrserver ~5s later, no dump, no WER. These games
// must survive SteamVR quitting (they can run flat), so on Quit we let the
// mod tear the session down and then sever the OpenVR connection here. The
// mod's 5s bring-up retry + EnsureVr() below reconnect if SteamVR returns.
static bool g_quitReceived = false;

static void VrDisconnect(const char* why)
{
    if (!g_vr.ok && !g_vr.sys) return;
    SLOG("OpenVR disconnect (%s)", why);
    if (g_vrShutdown) g_vrShutdown();
    g_vr.sys = nullptr;
    g_vr.comp = nullptr;
    g_vr.input = nullptr;
    g_vr.ovl = nullptr;
    g_vr.ok = false;
    // Force a fresh geometry cache (and eye-target rebuild) on reconnect -
    // the next SteamVR may drive a different headset.
    g_st.rtW = g_st.rtH = 0;
    g_st.haveOrigin = false;
    g_st.hmdValid = false;
}

static bool EnsureVr()
{
    return g_vr.ok || VrConnect();
}

// ---------------------------------------------------------------- helpers
static const XrInstance  kInstance = (XrInstance)0x5601;
static const XrSession   kSession  = (XrSession)0x5602;
static const XrSystemId  kSystemId = 1;

static std::vector<std::string>* g_paths = nullptr;   // XrPath = index+1

static XrTime NowXrTime()
{
    if (!g_st.qpf.QuadPart) { QueryPerformanceFrequency(&g_st.qpf); QueryPerformanceCounter(&g_st.qpc0); }
    LARGE_INTEGER t; QueryPerformanceCounter(&t);
    const double ns = (double)(t.QuadPart - g_st.qpc0.QuadPart) * 1e9 / (double)g_st.qpf.QuadPart;
    return (XrTime)ns + 1;
}

static void CacheHmdGeometry()
{
    g_vr.sys->GetRecommendedRenderTargetSize(&g_st.rtW, &g_st.rtH);

    // The runtime's recommendation is only a STARTING point here. The eye
    // targets are sized from the game's actual swapchain in xrCreateSwapchain
    // below, because this shim's job is to composite an already-rendered frame
    // - downscaling it first and letting the compositor scale it back up is a
    // pure resampling loss. Donor MEASURED: 2750x2850 into 1780x1908 targets
    // showed as 66% in Virtual Desktop; matching the source showed 102%.
    for (int e = 0; e < 2; ++e)
    {
        HmdMatrix34_t ehC = g_vr.sys->GetEyeToHeadTransform((EVREye)e);
        memcpy(g_st.eyeToHead[e].m, ehC.m, sizeof(g_st.eyeToHead[e].m));

        float l, r, t, b;
        g_vr.sys->GetProjectionRaw((EVREye)e, &l, &r, &t, &b);

        // OPENVR'S PARAMETER NAMES ARE BACKWARDS. Valve's own documentation
        // says so: pfTop is the BOTTOM (-Y) clipping edge and pfBottom is the
        // TOP (+Y) edge. A normal headset returns pfTop ~ -1, pfBottom ~ +1.
        //
        // So the conversion to OpenXR (up positive, down negative) is a direct
        // assignment, NOT a negation:
        //     U = b        D = t
        //
        // The donor's old code did U = -t, D = -b. On a VERTICALLY SYMMETRIC
        // headset that is arithmetically identical (b == -t), which is why it
        // went unnoticed for weeks. On an ASYMMETRIC one it MIRRORS the
        // vertical optical centre - and a reflected principal point turns into
        // vertical scale and keystone during the compositor's rotational
        // reprojection. That is the "look down and it shrinks, look up and it
        // stretches" bug. The SteamVR desktop mirror looked normal throughout.
        //
        // The old `if (U < D) swap` guard is deliberately GONE. It could not
        // detect a mirrored centre - both orderings look valid - so all it
        // did was make a wrong convention look plausible. Validate and shout
        // instead. DO NOT reintroduce the negation or the swap.
        const float U = b;
        const float D = t;

        SLOG("eye %d GetProjectionRaw: L%.4f R%.4f pfTop%.4f pfBottom%.4f",
            e, l, r, t, b);

        if (!(l < r && D < U))
            SLOG("!!! eye %d INVALID projection: L%.4f R%.4f D%.4f U%.4f - "
                "this headset does not follow the documented OpenVR convention.",
                e, l, r, D, U);

        g_st.rawL[e] = l; g_st.rawR[e] = r; g_st.rawU[e] = U; g_st.rawD[e] = D;

        g_st.eyeFov[e].angleLeft  = atanf(l);
        g_st.eyeFov[e].angleRight = atanf(r);
        g_st.eyeFov[e].angleUp    = atanf(U);
        g_st.eyeFov[e].angleDown  = atanf(D);
        SLOG("eye %d raw tangents L%.3f R%.3f U%.3f D%.3f  eyeToHead x=%.4f",
             e, l, r, U, D, g_st.eyeToHead[e].m[0][3]);
    }
    ETrackedPropertyError perr = ETrackedPropertyError_TrackedProp_Success;
    const float hz = g_vr.sys->GetFloatTrackedDeviceProperty(0,
        ETrackedDeviceProperty_Prop_DisplayFrequency_Float, &perr);
    if (perr == ETrackedPropertyError_TrackedProp_Success && hz > 30.f) g_st.displayHz = hz;
    SLOG("recommended per-eye %ux%u, display %.1f Hz", g_st.rtW, g_st.rtH, g_st.displayHz);
}

static void PumpVrEvents()
{
    if (!g_vr.ok) return;
    vr::VREvent_t ev;
    while (g_vr.sys->PollNextEvent((VREvent_t*)&ev, sizeof(ev)))
    {
        if (ev.eventType == vr::VREvent_Quit)
        {
            SLOG("SteamVR requested quit - acknowledging; will disconnect at "
                 "session destroy so the game can keep running flat");
            g_vr.sys->AcknowledgeQuit_Exiting();
            g_quitReceived = true;
            Events_Push(XR_SESSION_STATE_STOPPING);
        }
    }
}

// ============================================================================
//  OpenXR entry points (dispatched from ovrshim_negotiate.cpp; no dllexports)
// ============================================================================

// NEW vs donor. The loader calls this while loading the runtime, and it is
// also the mod's very FIRST OpenXR call - if it fails the mod logs "no 32-bit
// OpenXR runtime reachable" and the game runs flat.
OVRSHIM_FN(shim_EnumerateInstanceExtensionProperties)(
    const char* layerName, uint32_t capacity, uint32_t* count,
    XrExtensionProperties* props)
{
    if (layerName) return XR_ERROR_API_LAYER_NOT_PRESENT;
    if (!count) return XR_ERROR_VALIDATION_FAILURE;
    *count = 1;
    if (capacity == 0 || !props) return XR_SUCCESS;
    if (capacity < 1) return XR_ERROR_SIZE_INSUFFICIENT;
    strncpy_s(props[0].extensionName, XR_KHR_D3D11_ENABLE_EXTENSION_NAME, _TRUNCATE);
    props[0].extensionVersion = XR_KHR_D3D11_enable_SPEC_VERSION;
    return XR_SUCCESS;
}

OVRSHIM_FN(shim_CreateInstance)(
    const XrInstanceCreateInfo* createInfo, XrInstance* instance)
{
    SLOG("=== bvr_steamvr32 OpenXR->OpenVR shim (build %s %s) ===", __DATE__, __TIME__);
    if (!createInfo || !instance) return XR_ERROR_VALIDATION_FAILURE;
    SLOG("xrCreateInstance app='%s' extensions=%u",
         createInfo->applicationInfo.applicationName, createInfo->enabledExtensionCount);
    for (uint32_t i = 0; i < createInfo->enabledExtensionCount; ++i)
        SLOG("  ext: %s", createInfo->enabledExtensionNames[i]);

    if (!VrConnect()) return XR_ERROR_INITIALIZATION_FAILED;

    if (!g_paths) g_paths = new std::vector<std::string>();
    g_st.instanceAlive = true;
    *instance = kInstance;
    return XR_SUCCESS;
}

OVRSHIM_FN(shim_DestroyInstance)(XrInstance)
{
    SLOG("xrDestroyInstance");
    Render_Shutdown();
    if (g_vr.ok && g_vrShutdown) g_vrShutdown();
    g_vr.ok = false;
    g_st.instanceAlive = false;
    return XR_SUCCESS;
}

OVRSHIM_FN(shim_GetInstanceProperties)(XrInstance, XrInstanceProperties* p)
{
    if (!p) return XR_ERROR_VALIDATION_FAILURE;
    // The mod's "instance created on runtime '<name>'" line identifies the
    // shim by this string; TROUBLESHOOTING greps for it.
    strncpy_s(p->runtimeName, "BioshockVR SteamVR shim (OpenVR)", _TRUNCATE);
    p->runtimeVersion = XR_MAKE_VERSION(0, 9, 0);
    return XR_SUCCESS;
}

// NEW vs donor: every res_str() log line in the mod goes through this.
OVRSHIM_FN(shim_ResultToString)(XrInstance, XrResult value,
                                char buffer[XR_MAX_RESULT_STRING_SIZE])
{
    if (!buffer) return XR_ERROR_VALIDATION_FAILURE;
    const char* s = nullptr;
    switch (value)
    {
    case XR_SUCCESS: s = "XR_SUCCESS"; break;
    case XR_EVENT_UNAVAILABLE: s = "XR_EVENT_UNAVAILABLE"; break;
    case XR_SESSION_NOT_FOCUSED: s = "XR_SESSION_NOT_FOCUSED"; break;
    case XR_ERROR_VALIDATION_FAILURE: s = "XR_ERROR_VALIDATION_FAILURE"; break;
    case XR_ERROR_RUNTIME_FAILURE: s = "XR_ERROR_RUNTIME_FAILURE"; break;
    case XR_ERROR_INITIALIZATION_FAILED: s = "XR_ERROR_INITIALIZATION_FAILED"; break;
    case XR_ERROR_FUNCTION_UNSUPPORTED: s = "XR_ERROR_FUNCTION_UNSUPPORTED"; break;
    case XR_ERROR_HANDLE_INVALID: s = "XR_ERROR_HANDLE_INVALID"; break;
    case XR_ERROR_INSTANCE_LOST: s = "XR_ERROR_INSTANCE_LOST"; break;
    case XR_ERROR_SESSION_LOST: s = "XR_ERROR_SESSION_LOST"; break;
    case XR_ERROR_SYSTEM_INVALID: s = "XR_ERROR_SYSTEM_INVALID"; break;
    case XR_ERROR_SIZE_INSUFFICIENT: s = "XR_ERROR_SIZE_INSUFFICIENT"; break;
    case XR_ERROR_LIMIT_REACHED: s = "XR_ERROR_LIMIT_REACHED"; break;
    case XR_ERROR_FORM_FACTOR_UNAVAILABLE: s = "XR_ERROR_FORM_FACTOR_UNAVAILABLE"; break;
    case XR_ERROR_FORM_FACTOR_UNSUPPORTED: s = "XR_ERROR_FORM_FACTOR_UNSUPPORTED"; break;
    case XR_ERROR_GRAPHICS_DEVICE_INVALID: s = "XR_ERROR_GRAPHICS_DEVICE_INVALID"; break;
    case XR_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED: s = "XR_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED"; break;
    case XR_ERROR_REFERENCE_SPACE_UNSUPPORTED: s = "XR_ERROR_REFERENCE_SPACE_UNSUPPORTED"; break;
    case XR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED: s = "XR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED"; break;
    case XR_ERROR_ACTIONSETS_ALREADY_ATTACHED: s = "XR_ERROR_ACTIONSETS_ALREADY_ATTACHED"; break;
    case XR_ERROR_PATH_UNSUPPORTED: s = "XR_ERROR_PATH_UNSUPPORTED"; break;
    case XR_ERROR_POSE_INVALID: s = "XR_ERROR_POSE_INVALID"; break;
    case XR_ERROR_EXTENSION_NOT_PRESENT: s = "XR_ERROR_EXTENSION_NOT_PRESENT"; break;
    case XR_ERROR_API_LAYER_NOT_PRESENT: s = "XR_ERROR_API_LAYER_NOT_PRESENT"; break;
    case XR_ERROR_RUNTIME_UNAVAILABLE: s = "XR_ERROR_RUNTIME_UNAVAILABLE"; break;
    default: break;
    }
    if (s)
        strncpy_s(buffer, XR_MAX_RESULT_STRING_SIZE, s, _TRUNCATE);
    else
        sprintf_s(buffer, XR_MAX_RESULT_STRING_SIZE,
                  value < 0 ? "XR_UNKNOWN_FAILURE_%d" : "XR_UNKNOWN_SUCCESS_%d",
                  (int)value);
    return XR_SUCCESS;
}

OVRSHIM_FN(shim_GetSystem)(XrInstance, const XrSystemGetInfo* info, XrSystemId* out)
{
    if (!info || !out) return XR_ERROR_VALIDATION_FAILURE;
    if (info->formFactor != XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY)
        return XR_ERROR_FORM_FACTOR_UNSUPPORTED;
    *out = kSystemId;
    return XR_SUCCESS;
}

// NEW vs donor: the mod logs the system name and layer budget from this.
OVRSHIM_FN(shim_GetSystemProperties)(XrInstance, XrSystemId, XrSystemProperties* p)
{
    if (!p) return XR_ERROR_VALIDATION_FAILURE;
    p->systemId = kSystemId;
    p->vendorId = 0;
    strncpy_s(p->systemName, "SteamVR (OpenVR shim)", _TRUNCATE);
    if (g_vr.ok)
    {
        ETrackedPropertyError perr = ETrackedPropertyError_TrackedProp_Success;
        char model[XR_MAX_SYSTEM_NAME_SIZE] = {};
        g_vr.sys->GetStringTrackedDeviceProperty(0,
            ETrackedDeviceProperty_Prop_ModelNumber_String, model, sizeof(model), &perr);
        if (perr == ETrackedPropertyError_TrackedProp_Success && model[0])
            strncpy_s(p->systemName, model, _TRUNCATE);
    }
    p->graphicsProperties.maxLayerCount = kMaxQuadLayers;
    p->graphicsProperties.maxSwapchainImageWidth = 8192;
    p->graphicsProperties.maxSwapchainImageHeight = 8192;
    p->trackingProperties.orientationTracking = XR_TRUE;
    p->trackingProperties.positionTracking = XR_TRUE;
    return XR_SUCCESS;
}

OVRSHIM_FN(shim_GetD3D11GraphicsRequirementsKHR)(
    XrInstance, XrSystemId, XrGraphicsRequirementsD3D11KHR* req)
{
    if (!req) return XR_ERROR_VALIDATION_FAILURE;
    uint64_t luid = 0;
    g_vr.sys->GetOutputDevice(&luid, ETextureType_TextureType_DirectX, nullptr);
    memcpy(&req->adapterLuid, &luid, sizeof(LUID));
    req->minFeatureLevel = D3D_FEATURE_LEVEL_11_0;
    SLOG("xrGetD3D11GraphicsRequirementsKHR luid=%08X:%08X",
         (unsigned)(luid >> 32), (unsigned)(luid & 0xFFFFFFFF));
    return XR_SUCCESS;
}

OVRSHIM_FN(shim_EnumerateViewConfigurationViews)(
    XrInstance, XrSystemId, XrViewConfigurationType type,
    uint32_t capacity, uint32_t* count, XrViewConfigurationView* views)
{
    if (type != XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO)
        return XR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED;
    if (count) *count = 2;
    if (capacity == 0 || !views) return XR_SUCCESS;
    if (capacity < 2) return XR_ERROR_SIZE_INSUFFICIENT;

    if (!g_st.rtW) CacheHmdGeometry();
    for (int i = 0; i < 2; ++i)
    {
        views[i].recommendedImageRectWidth  = g_st.rtW;
        views[i].recommendedImageRectHeight = g_st.rtH;
        views[i].maxImageRectWidth  = 8192;
        views[i].maxImageRectHeight = 8192;
        views[i].recommendedSwapchainSampleCount = 1;
        views[i].maxSwapchainSampleCount = 1;
    }
    return XR_SUCCESS;
}

OVRSHIM_FN(shim_CreateSession)(
    XrInstance, const XrSessionCreateInfo* info, XrSession* out)
{
    if (!info || !out) return XR_ERROR_VALIDATION_FAILURE;

    const XrGraphicsBindingD3D11KHR* bind = nullptr;
    for (const XrBaseInStructure* s = (const XrBaseInStructure*)info->next; s;
         s = (const XrBaseInStructure*)s->next)
        if (s->type == XR_TYPE_GRAPHICS_BINDING_D3D11_KHR)
            bind = (const XrGraphicsBindingD3D11KHR*)s;

    if (!bind || !bind->device)
    {
        SLOG("!!! xrCreateSession: no D3D11 graphics binding");
        return XR_ERROR_GRAPHICS_DEVICE_INVALID;
    }

    g_st.dev = bind->device;
    g_st.dev->GetImmediateContext(&g_st.ctx);
    if (!g_st.rtW) CacheHmdGeometry();

    g_st.sessionAlive = true;
    *out = kSession;

    Events_Push(XR_SESSION_STATE_IDLE);
    Events_Push(XR_SESSION_STATE_READY);
    SLOG("xrCreateSession ok (device=%p)", g_st.dev);
    return XR_SUCCESS;
}

OVRSHIM_FN(shim_DestroySession)(XrSession)
{
    g_st.sessionAlive = g_st.sessionBegun = false;
    if (g_st.ctx) { g_st.ctx->Release(); g_st.ctx = nullptr; }
    return XR_SUCCESS;
}

OVRSHIM_FN(shim_BeginSession)(XrSession, const XrSessionBeginInfo*)
{
    g_st.sessionBegun = true;
    Events_Push(XR_SESSION_STATE_SYNCHRONIZED);
    Events_Push(XR_SESSION_STATE_VISIBLE);
    Events_Push(XR_SESSION_STATE_FOCUSED);
    SLOG("xrBeginSession");
    return XR_SUCCESS;
}

OVRSHIM_FN(shim_EndSession)(XrSession)
{
    g_st.sessionBegun = false;
    Events_Push(XR_SESSION_STATE_EXITING);
    SLOG("xrEndSession");
    return XR_SUCCESS;
}

OVRSHIM_FN(shim_PollEvent)(XrInstance, XrEventDataBuffer* ev)
{
    if (!ev) return XR_ERROR_VALIDATION_FAILURE;
    PumpVrEvents();

    XrSessionState s;
    if (!Events_Pop(&s)) return XR_EVENT_UNAVAILABLE;

    XrEventDataSessionStateChanged* ssc = (XrEventDataSessionStateChanged*)ev;
    memset(ev, 0, sizeof(*ev));
    ssc->type = XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED;
    ssc->session = kSession;
    ssc->state = s;
    ssc->time = NowXrTime();
    return XR_SUCCESS;
}

// ---------------------------------------------------------------- spaces
OVRSHIM_FN(shim_CreateReferenceSpace)(
    XrSession, const XrReferenceSpaceCreateInfo* info, XrSpace* out)
{
    if (!info || !out) return XR_ERROR_VALIDATION_FAILURE;
    SpaceRec* s = new SpaceRec();
    if (info->referenceSpaceType == XR_REFERENCE_SPACE_TYPE_LOCAL)
        s->kind = SPACE_REF_LOCAL;
    else if (info->referenceSpaceType == XR_REFERENCE_SPACE_TYPE_VIEW)
        s->kind = SPACE_REF_VIEW;
    else { delete s; return XR_ERROR_REFERENCE_SPACE_UNSUPPORTED; }
    *out = (XrSpace)s;
    SLOG("xrCreateReferenceSpace kind=%d", s->kind);
    return XR_SUCCESS;
}

OVRSHIM_FN(shim_DestroySpace)(XrSpace sp)
{
    delete (SpaceRec*)sp;
    return XR_SUCCESS;
}

// ---------------------------------------------------------------- swapchains
OVRSHIM_FN(shim_EnumerateSwapchainFormats)(
    XrSession, uint32_t capacity, uint32_t* count, int64_t* formats)
{
    static const int64_t fmts[] = {
        DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,   // 29 - the mod's first choice
        DXGI_FORMAT_R8G8B8A8_UNORM,        // 28
        DXGI_FORMAT_B8G8R8A8_UNORM_SRGB,   // 91
        DXGI_FORMAT_B8G8R8A8_UNORM,        // 87
    };
    if (count) *count = 4;
    if (capacity == 0 || !formats) return XR_SUCCESS;
    if (capacity < 4) return XR_ERROR_SIZE_INSUFFICIENT;
    memcpy(formats, fmts, sizeof(fmts));
    return XR_SUCCESS;
}

OVRSHIM_FN(shim_CreateSwapchain)(
    XrSession, const XrSwapchainCreateInfo* info, XrSwapchain* out)
{
    if (!info || !out || !g_st.dev) return XR_ERROR_VALIDATION_FAILURE;

    // Backing texture is TYPELESS so the mod can CopyResource from UNORM game
    // surfaces into it regardless of the *_SRGB view format it asked for. We
    // sample it as UNORM (raw gamma values) and Submit with ColorSpace_Gamma,
    // so the numbers pass through untouched end to end.
    DXGI_FORMAT typeless = DXGI_FORMAT_R8G8B8A8_TYPELESS;
    DXGI_FORMAT srvFmt   = DXGI_FORMAT_R8G8B8A8_UNORM;
    switch (info->format)
    {
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        typeless = DXGI_FORMAT_R8G8B8A8_TYPELESS; srvFmt = DXGI_FORMAT_R8G8B8A8_UNORM; break;
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        typeless = DXGI_FORMAT_B8G8R8A8_TYPELESS; srvFmt = DXGI_FORMAT_B8G8R8A8_UNORM; break;
    default:
        SLOG("!!! xrCreateSwapchain: unsupported format %d", (int)info->format);
        return XR_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED;
    }

    D3D11_TEXTURE2D_DESC td = {};
    td.Width = info->width;
    td.Height = info->height;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = typeless;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;

    SwapchainRec* sc = new SwapchainRec();
    sc->w = info->width; sc->h = info->height; sc->fmt = info->format;

    HRESULT hr = g_st.dev->CreateTexture2D(&td, nullptr, &sc->tex);
    if (FAILED(hr))
    {
        SLOG("!!! xrCreateSwapchain CreateTexture2D failed 0x%08X (%ux%u)",
             (unsigned)hr, info->width, info->height);
        delete sc;
        return XR_ERROR_RUNTIME_FAILURE;
    }

    D3D11_SHADER_RESOURCE_VIEW_DESC sd = {};
    sd.Format = srvFmt;
    sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    sd.Texture2D.MipLevels = 1;
    hr = g_st.dev->CreateShaderResourceView(sc->tex, &sd, &sc->srv);
    if (FAILED(hr))
    {
        SLOG("!!! xrCreateSwapchain CreateSRV failed 0x%08X", (unsigned)hr);
        sc->tex->Release();
        delete sc;
        return XR_ERROR_RUNTIME_FAILURE;
    }

    *out = (XrSwapchain)sc;
    SLOG("xrCreateSwapchain %ux%u fmt %d -> ok", info->width, info->height, (int)info->format);

    // Size the eye targets to the LARGEST swapchain the app asks for - that is
    // its eye buffer. Small ones (the laser dots, the HUD) are ignored via the
    // >=512 gate. Unlike the donor this also fires AFTER Render_Init: the BS2
    // mid-session resolution change destroys and recreates both eye swapchains
    // at the new size, and the renderer re-creates its targets on mismatch
    // (donor behavior was size-once, then silently dropped copies -> black
    // headset; see LOG-PLAYBOOK.md in their repo).
    if (info->width >= 512 && info->height >= 512 &&
        (info->width > g_st.rtW || info->height > g_st.rtH))
    {
        g_st.rtW = info->width;
        g_st.rtH = info->height;
        SLOG("eye targets will be %ux%u (matched to the app's swapchain)",
            g_st.rtW, g_st.rtH);
    }
    return XR_SUCCESS;
}

OVRSHIM_FN(shim_DestroySwapchain)(XrSwapchain h)
{
    SwapchainRec* sc = (SwapchainRec*)h;
    if (sc)
    {
        if (sc->srv) sc->srv->Release();
        if (sc->tex) sc->tex->Release();
        delete sc;
    }
    return XR_SUCCESS;
}

OVRSHIM_FN(shim_EnumerateSwapchainImages)(
    XrSwapchain h, uint32_t capacity, uint32_t* count, XrSwapchainImageBaseHeader* images)
{
    SwapchainRec* sc = (SwapchainRec*)h;
    if (!sc) return XR_ERROR_HANDLE_INVALID;
    if (count) *count = 1;
    if (capacity == 0 || !images) return XR_SUCCESS;
    XrSwapchainImageD3D11KHR* d = (XrSwapchainImageD3D11KHR*)images;
    d[0].texture = sc->tex;
    return XR_SUCCESS;
}

OVRSHIM_FN(shim_AcquireSwapchainImage)(
    XrSwapchain h, const XrSwapchainImageAcquireInfo*, uint32_t* index)
{
    if (index) *index = 0;
    SwapchainRec* sc = (SwapchainRec*)h;
    if (sc) sc->acquired = true;
    return XR_SUCCESS;
}

OVRSHIM_FN(shim_WaitSwapchainImage)(XrSwapchain, const XrSwapchainImageWaitInfo*)
{
    return XR_SUCCESS;
}

OVRSHIM_FN(shim_ReleaseSwapchainImage)(
    XrSwapchain h, const XrSwapchainImageReleaseInfo*)
{
    SwapchainRec* sc = (SwapchainRec*)h;
    if (sc) sc->acquired = false;
    return XR_SUCCESS;
}

// ---------------------------------------------------------------- frame loop
static vr::TrackedDevicePose_t g_renderPoses[vr::k_unMaxTrackedDeviceCount];

OVRSHIM_FN(shim_WaitFrame)(XrSession, const XrFrameWaitInfo*, XrFrameState* fs)
{
    if (!fs) return XR_ERROR_VALIDATION_FAILURE;
    if (!g_vr.ok) return XR_ERROR_SESSION_LOST;

    // The blocking pacing point: returns at "running start", ~2-3 ms before
    // vsync, with poses predicted to the upcoming photon time.
    const EVRCompositorError we = g_vr.comp->WaitGetPoses(
        (TrackedDevicePose_t*)g_renderPoses, vr::k_unMaxTrackedDeviceCount, nullptr, 0);
    {
        static EVRCompositorError lastWe = EVRCompositorError_VRCompositorError_None;
        if (we != lastWe)
        {
            lastWe = we;
            if (we != EVRCompositorError_VRCompositorError_None)
                SLOG("WaitGetPoses -> compositor error %d", (int)we);
        }
    }

    const vr::TrackedDevicePose_t& hp = g_renderPoses[vr::k_unTrackedDeviceIndex_Hmd];
    if (hp.bPoseIsValid)
    {
        M34 abs = M34_FromVr(hp.mDeviceToAbsoluteTracking);
        if (!g_st.haveOrigin)
        {
            // Latch LOCAL-space origin: first valid HMD pose, yaw + position.
            // HMD forward = -z column of the rotation. Origin = Ry(yaw) whose
            // forward (-s, 0, -c) matches the flattened HMD forward.
            float fx = -abs.m[0][2], fz = -abs.m[2][2];
            const float len = sqrtf(fx * fx + fz * fz);
            float yaw = 0.f;
            if (len > 1e-3f) yaw = atan2f(-fx / len, -fz / len);
            M34 o = M34_Identity();
            const float c = cosf(yaw), s = sinf(yaw);
            o.m[0][0] = c;  o.m[0][2] = s;
            o.m[2][0] = -s; o.m[2][2] = c;
            o.m[0][3] = abs.m[0][3];
            o.m[1][3] = abs.m[1][3];
            o.m[2][3] = abs.m[2][3];
            g_st.originInv = M34_InvRigid(o);
            g_st.haveOrigin = true;
            SLOG("LOCAL origin latched at (%.2f %.2f %.2f) yaw %.1f deg",
                 abs.m[0][3], abs.m[1][3], abs.m[2][3], yaw * 57.2958f);
        }
        g_st.hmd = M34_Mul(g_st.originInv, abs);
        g_st.hmdValid = true;
    }
    else
        g_st.hmdValid = false;

    const XrTime period = (XrTime)(1e9 / (double)g_st.displayHz);
    fs->predictedDisplayTime = NowXrTime() + period;
    fs->predictedDisplayPeriod = period;
    fs->shouldRender = XR_TRUE;
    g_st.lastPredictedTime = fs->predictedDisplayTime;
    return XR_SUCCESS;
}

OVRSHIM_FN(shim_BeginFrame)(XrSession, const XrFrameBeginInfo*)
{
    return XR_SUCCESS;
}

OVRSHIM_FN(shim_LocateViews)(
    XrSession, const XrViewLocateInfo* info, XrViewState* state,
    uint32_t capacity, uint32_t* count, XrView* views)
{
    if (!info || !state || !count) return XR_ERROR_VALIDATION_FAILURE;
    *count = 2;
    if (capacity == 0 || !views) return XR_SUCCESS;
    if (capacity < 2) return XR_ERROR_SIZE_INSUFFICIENT;

    if (!g_st.hmdValid || !g_st.haveOrigin)
    {
        state->viewStateFlags = 0;
        for (int e = 0; e < 2; ++e)
        {
            views[e].pose.orientation = { 0, 0, 0, 1 };
            views[e].pose.position = { 0, 0, 0 };
            views[e].fov = g_st.eyeFov[e];
        }
        return XR_SUCCESS;
    }

    for (int e = 0; e < 2; ++e)
    {
        const M34 eyePose = M34_Mul(g_st.hmd, g_st.eyeToHead[e]);
        float q[4], p[3];
        M34_ToQuatPos(eyePose, q, p);
        views[e].pose.orientation = { q[0], q[1], q[2], q[3] };
        views[e].pose.position = { p[0], p[1], p[2] };
        views[e].fov = g_st.eyeFov[e];
    }
    state->viewStateFlags = XR_VIEW_STATE_ORIENTATION_VALID_BIT |
                            XR_VIEW_STATE_POSITION_VALID_BIT |
                            XR_VIEW_STATE_ORIENTATION_TRACKED_BIT |
                            XR_VIEW_STATE_POSITION_TRACKED_BIT;
    return XR_SUCCESS;
}

OVRSHIM_FN(shim_EndFrame)(XrSession, const XrFrameEndInfo* fei)
{
    if (!fei) return XR_ERROR_VALIDATION_FAILURE;
    if (!g_vr.ok) return XR_ERROR_SESSION_LOST;

    ProjDrawView proj[2];
    bool haveProj = false;
    QuadDraw quads[kMaxQuadLayers];
    int quadCount = 0;
    static bool overflowTold = false;

    for (uint32_t i = 0; i < fei->layerCount; ++i)
    {
        const XrCompositionLayerBaseHeader* L = fei->layers[i];
        if (!L) continue;

        if (L->type == XR_TYPE_COMPOSITION_LAYER_PROJECTION)
        {
            const XrCompositionLayerProjection* pl = (const XrCompositionLayerProjection*)L;
            if (pl->viewCount != 2) continue;
            for (int e = 0; e < 2; ++e)
            {
                const XrCompositionLayerProjectionView& v = pl->views[e];
                SwapchainRec* sc = (SwapchainRec*)v.subImage.swapchain;
                if (!sc) { haveProj = false; break; }
                proj[e].srv = sc->srv;
                proj[e].pose[0] = v.pose.orientation.x;
                proj[e].pose[1] = v.pose.orientation.y;
                proj[e].pose[2] = v.pose.orientation.z;
                proj[e].pose[3] = v.pose.orientation.w;
                proj[e].pose[4] = v.pose.position.x;
                proj[e].pose[5] = v.pose.position.y;
                proj[e].pose[6] = v.pose.position.z;
                proj[e].tanL = tanf(v.fov.angleLeft);
                proj[e].tanR = tanf(v.fov.angleRight);
                proj[e].tanU = tanf(v.fov.angleUp);
                proj[e].tanD = tanf(v.fov.angleDown);
                haveProj = true;
            }
        }
        else if (L->type == XR_TYPE_COMPOSITION_LAYER_QUAD)
        {
            if (quadCount >= kMaxQuadLayers)
            {
                if (!overflowTold)
                {
                    overflowTold = true;
                    SLOG("!!! quad layer budget exceeded (%u submitted, cap %d) - "
                         "extra quads are dropped", fei->layerCount, kMaxQuadLayers);
                }
                continue;
            }
            const XrCompositionLayerQuad* ql = (const XrCompositionLayerQuad*)L;
            SwapchainRec* sc = (SwapchainRec*)ql->subImage.swapchain;
            if (!sc) continue;
            SpaceRec* sp = (SpaceRec*)ql->space;
            QuadDraw& q = quads[quadCount++];
            q.srv = sc->srv;
            q.pose[0] = ql->pose.orientation.x;
            q.pose[1] = ql->pose.orientation.y;
            q.pose[2] = ql->pose.orientation.z;
            q.pose[3] = ql->pose.orientation.w;
            q.pose[4] = ql->pose.position.x;
            q.pose[5] = ql->pose.position.y;
            q.pose[6] = ql->pose.position.z;
            q.sx = ql->size.width;
            q.sy = ql->size.height;
            q.viewSpace = (sp && sp->kind == SPACE_REF_VIEW) ? 1 : 0;
            // The mod's cinema quad ships NO blend flags (opaque); dots and
            // HUD ship SOURCE_ALPHA (| UNPREMULTIPLIED). Honor them exactly.
            if (ql->layerFlags & XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT)
                q.blend = (ql->layerFlags & XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT) ? 1 : 2;
            else
                q.blend = 0;
        }
    }

    Render_CompositeAndSubmit(haveProj ? proj : nullptr, quads, quadCount);
    ++g_st.frameIndex;
    return XR_SUCCESS;
}

// ---------------------------------------------------------------- paths
OVRSHIM_FN(shim_StringToPath)(XrInstance, const char* str, XrPath* out)
{
    if (!str || !out) return XR_ERROR_VALIDATION_FAILURE;
    if (!g_paths) g_paths = new std::vector<std::string>();
    for (size_t i = 0; i < g_paths->size(); ++i)
        if ((*g_paths)[i] == str) { *out = (XrPath)(i + 1); return XR_SUCCESS; }
    g_paths->push_back(str);
    *out = (XrPath)g_paths->size();
    return XR_SUCCESS;
}

const char* PathToString(XrPath p)
{
    if (!g_paths || p == XR_NULL_PATH || p > g_paths->size()) return "";
    return (*g_paths)[(size_t)p - 1].c_str();
}
