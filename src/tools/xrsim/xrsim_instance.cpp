// bvr_xrsim32: loader negotiation, xrGetInstanceProcAddr, instance lifecycle,
// the path atom table, the event queue, and the Quest 3 system description.
//
// On the GIPA table: the loader populates 64 dispatch slots at instance creation
// and its trampolines call a slot without checking it for null, so every one of
// those names must resolve to a REAL, CORRECTLY TYPED function. On 32-bit the
// typing is not cosmetic: XRAPI_CALL is __stdcall, the callee pops the argument
// bytes, so one generic zero-argument thunk shared across differently-shaped
// entry points would corrupt the stack on every call. Hence a properly typed
// stub for each, even the ones nobody will ever call.

#include "xrsim_common.h"
#include "xrsim_internal.h"

#include <cstring>
#include <string>
#include <vector>

#include <openxr/openxr_reflection.h>

namespace xrsim {

Globals g;

namespace {

// --- path atoms ------------------------------------------------------------
std::mutex g_pathMutex;
std::vector<std::string> g_paths; // index 0 unused, so XR_NULL_PATH stays invalid

// --- event ring ------------------------------------------------------------
std::mutex g_eventMutex;
XrEventDataBuffer g_events[kMaxEvents];
uint32_t g_evHead = 0, g_evTail = 0;
uint32_t g_evDropped = 0;

} // namespace

// ---------------------------------------------------------------------------
// Events
// ---------------------------------------------------------------------------

void queue_event(const XrEventDataBuffer& buf) {
    std::lock_guard<std::mutex> lock(g_eventMutex);
    const uint32_t next = (g_evTail + 1) % kMaxEvents;
    if (next == g_evHead) {
        // Full. Drop the OLDEST: a stalled consumer must not cost us the newest
        // state change, which is the one that matters.
        g_evHead = (g_evHead + 1) % kMaxEvents;
        ++g_evDropped;
    }
    g_events[g_evTail] = buf;
    g_evTail = next;
}

void queue_session_state(XrSession session, XrSessionState state) {
    XrEventDataBuffer buf{};
    auto* ev = reinterpret_cast<XrEventDataSessionStateChanged*>(&buf);
    ev->type = XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED;
    ev->next = nullptr;
    ev->session = session;
    ev->state = state;
    ev->time = now_xr_time();
    queue_event(buf);
}

uint32_t events_dropped() {
    std::lock_guard<std::mutex> lock(g_eventMutex);
    return g_evDropped;
}

// ---------------------------------------------------------------------------
// Paths
// ---------------------------------------------------------------------------

XrPath path_intern(const char* str) {
    std::lock_guard<std::mutex> lock(g_pathMutex);
    if (g_paths.empty()) g_paths.emplace_back("<null>");
    for (size_t i = 1; i < g_paths.size(); ++i)
        if (g_paths[i] == str) return static_cast<XrPath>(i);
    if (g_paths.size() >= kMaxPaths) return XR_NULL_PATH;
    g_paths.emplace_back(str);
    return static_cast<XrPath>(g_paths.size() - 1);
}

const char* path_str(XrPath p) {
    std::lock_guard<std::mutex> lock(g_pathMutex);
    if (p == XR_NULL_PATH || p >= g_paths.size()) return "";
    return g_paths[static_cast<size_t>(p)].c_str();
}

// ---------------------------------------------------------------------------
// Handle validation
// ---------------------------------------------------------------------------

bool valid_instance(XrInstance h) {
    return h != XR_NULL_HANDLE && handle_type(h) == HT_INSTANCE && g.instanceAlive &&
           handle_gen(h) == g.instanceGen;
}

// ---------------------------------------------------------------------------
// Instance
// ---------------------------------------------------------------------------

static XrResult impl_EnumerateInstanceExtensionProperties(const char* layerName,
                                                          uint32_t capacity,
                                                          uint32_t* countOutput,
                                                          XrExtensionProperties* props) noexcept {
    log::init();
    if (layerName && layerName[0]) return XR_ERROR_API_LAYER_NOT_PRESENT;
    if (!countOutput) return XR_ERROR_VALIDATION_FAILURE;

    // One extension, which is exactly what the mod asks for. Advertising more
    // than we implement would be a lie a test tool has no business telling.
    static const char* kExts[] = {XR_KHR_D3D11_ENABLE_EXTENSION_NAME};
    const uint32_t total = static_cast<uint32_t>(sizeof(kExts) / sizeof(kExts[0]));

    *countOutput = total;
    if (capacity == 0) return XR_SUCCESS;
    if (capacity < total) return XR_ERROR_SIZE_INSUFFICIENT;
    if (!props) return XR_ERROR_VALIDATION_FAILURE;

    for (uint32_t i = 0; i < total; ++i) {
        props[i].type = XR_TYPE_EXTENSION_PROPERTIES;
        props[i].next = nullptr;
        strcpy_s(props[i].extensionName, kExts[i]);
        props[i].extensionVersion = 9;
    }
    return XR_SUCCESS;
}

static XrResult impl_EnumerateApiLayerProperties(uint32_t, uint32_t* countOutput,
                                                 XrApiLayerProperties*) noexcept {
    if (!countOutput) return XR_ERROR_VALIDATION_FAILURE;
    *countOutput = 0;
    return XR_SUCCESS;
}

static XrResult impl_CreateInstance(const XrInstanceCreateInfo* info,
                                    XrInstance* out) noexcept {
    log::init();
    if (!info || !out) return XR_ERROR_VALIDATION_FAILURE;
    if (g.instanceAlive) return XR_ERROR_LIMIT_REACHED;

    bool d3d11 = false;
    for (uint32_t i = 0; i < info->enabledExtensionCount; ++i) {
        if (strcmp(info->enabledExtensionNames[i], XR_KHR_D3D11_ENABLE_EXTENSION_NAME) == 0)
            d3d11 = true;
        else {
            XRSIM_LOG("xrsim: app asked for unsupported extension '%s'",
                      info->enabledExtensionNames[i]);
            return XR_ERROR_EXTENSION_NOT_PRESENT;
        }
    }

    ++g.instanceGen;
    g.instanceAlive = true;
    g.d3d11Enabled = d3d11;
    *out = static_cast<XrInstance>(make_handle(HT_INSTANCE, 0, g.instanceGen));

    XRSIM_LOG("xrsim: instance created for app '%s' (engine '%s', api %u.%u) - D3D11 %s",
              info->applicationInfo.applicationName, info->applicationInfo.engineName,
              XR_VERSION_MAJOR(info->applicationInfo.apiVersion),
              XR_VERSION_MINOR(info->applicationInfo.apiVersion),
              d3d11 ? "enabled" : "OFF");
    control_start();
    return XR_SUCCESS;
}

static XrResult impl_DestroyInstance(XrInstance instance) noexcept {
    if (!valid_instance(instance)) return XR_ERROR_HANDLE_INVALID;
    XRSIM_LOG("xrsim: instance destroyed");
    session_destroy_all();
    control_stop();
    g.instanceAlive = false;
    return XR_SUCCESS;
}

static XrResult impl_GetInstanceProperties(XrInstance instance,
                                           XrInstanceProperties* props) noexcept {
    if (!valid_instance(instance)) return XR_ERROR_HANDLE_INVALID;
    if (!props) return XR_ERROR_VALIDATION_FAILURE;
    props->runtimeVersion = XR_MAKE_VERSION(1, 0, 0);
    // The launcher asserts on this string. If XR_RUNTIME_JSON silently failed to
    // take, the mod's log says VirtualDesktopXR here instead and every downstream
    // result would have been measured against the wrong runtime.
    strcpy_s(props->runtimeName, g.runtimeName);
    return XR_SUCCESS;
}

static XrResult impl_PollEvent(XrInstance instance, XrEventDataBuffer* out) noexcept {
    if (!valid_instance(instance)) return XR_ERROR_HANDLE_INVALID;
    if (!out) return XR_ERROR_VALIDATION_FAILURE;

    // The state machine has to advance from here as well as from the frame path.
    // An app waits for READY before it calls xrBeginSession, and it only calls
    // xrWaitFrame once running - so if polling did not pump, IDLE would never
    // reach READY and the session would never start. (Found on the first M2 run.)
    session_pump_state();

    std::lock_guard<std::mutex> lock(g_eventMutex);
    if (g_evHead == g_evTail) return XR_EVENT_UNAVAILABLE;
    *out = g_events[g_evHead];
    g_evHead = (g_evHead + 1) % kMaxEvents;
    return XR_SUCCESS;
}

static XrResult impl_ResultToString(XrInstance instance, XrResult value,
                                    char buffer[XR_MAX_RESULT_STRING_SIZE]) noexcept {
    if (!valid_instance(instance)) return XR_ERROR_HANDLE_INVALID;
    if (!buffer) return XR_ERROR_VALIDATION_FAILURE;
    // Generated from the registry via openxr_reflection.h, so a submodule bump
    // keeps this current with no hand maintenance.
    switch (value) {
#define XRSIM_RESULT_CASE(NAME, VAL) \
    case VAL:                        \
        strcpy_s(buffer, XR_MAX_RESULT_STRING_SIZE, #NAME); \
        return XR_SUCCESS;
        XR_LIST_ENUM_XrResult(XRSIM_RESULT_CASE)
#undef XRSIM_RESULT_CASE
    default:
        break;
    }
    sprintf_s(buffer, XR_MAX_RESULT_STRING_SIZE, "XR_UNKNOWN_%s_%d",
              static_cast<int>(value) < 0 ? "FAILURE" : "SUCCESS", static_cast<int>(value));
    return XR_SUCCESS;
}

static XrResult impl_StructureTypeToString(XrInstance instance, XrStructureType value,
                                           char buffer[XR_MAX_STRUCTURE_NAME_SIZE]) noexcept {
    if (!valid_instance(instance)) return XR_ERROR_HANDLE_INVALID;
    if (!buffer) return XR_ERROR_VALIDATION_FAILURE;
    switch (value) {
#define XRSIM_STYPE_CASE(NAME, VAL) \
    case VAL:                       \
        strcpy_s(buffer, XR_MAX_STRUCTURE_NAME_SIZE, #NAME); \
        return XR_SUCCESS;
        XR_LIST_ENUM_XrStructureType(XRSIM_STYPE_CASE)
#undef XRSIM_STYPE_CASE
    default:
        break;
    }
    sprintf_s(buffer, XR_MAX_STRUCTURE_NAME_SIZE, "XR_UNKNOWN_STRUCTURE_TYPE_%d",
              static_cast<int>(value));
    return XR_SUCCESS;
}

// ---------------------------------------------------------------------------
// System
// ---------------------------------------------------------------------------

static XrResult impl_GetSystem(XrInstance instance, const XrSystemGetInfo* info,
                               XrSystemId* out) noexcept {
    if (!valid_instance(instance)) return XR_ERROR_HANDLE_INVALID;
    if (!info || !out) return XR_ERROR_VALIDATION_FAILURE;
    if (info->formFactor != XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY)
        return XR_ERROR_FORM_FACTOR_UNSUPPORTED;

    // `hazard nosystem on` reproduces the no-headset-yet path without hardware:
    // the mod logs once, arms a 5 s cooldown, and retries from the present hook,
    // which is how connecting Virtual Desktop mid-game brings VR up.
    if (g.hazards.noSystem) return XR_ERROR_FORM_FACTOR_UNAVAILABLE;

    *out = kSystemId;
    return XR_SUCCESS;
}

static XrResult impl_GetSystemProperties(XrInstance instance, XrSystemId systemId,
                                         XrSystemProperties* props) noexcept {
    if (!valid_instance(instance)) return XR_ERROR_HANDLE_INVALID;
    if (systemId != kSystemId) return XR_ERROR_SYSTEM_INVALID;
    if (!props) return XR_ERROR_VALIDATION_FAILURE;

    // Every number here was recorded from this machine's real Quest 3 on
    // 2026-07-23 (ENGINE_NOTES "OpenXR runtime facts"), so the mod cannot tell
    // the sim from the hardware by inspection.
    props->systemId = kSystemId;
    props->vendorId = 0x2833; // Oculus
    strcpy_s(props->systemName, g.systemName);
    props->graphicsProperties.maxLayerCount = 16;
    props->graphicsProperties.maxSwapchainImageWidth = 16384;
    props->graphicsProperties.maxSwapchainImageHeight = 16384;
    props->trackingProperties.orientationTracking = XR_TRUE;
    props->trackingProperties.positionTracking = XR_TRUE;
    return XR_SUCCESS;
}

static XrResult impl_EnumerateViewConfigurations(XrInstance instance, XrSystemId systemId,
                                                 uint32_t capacity, uint32_t* countOutput,
                                                 XrViewConfigurationType* types) noexcept {
    if (!valid_instance(instance)) return XR_ERROR_HANDLE_INVALID;
    if (systemId != kSystemId) return XR_ERROR_SYSTEM_INVALID;
    if (!countOutput) return XR_ERROR_VALIDATION_FAILURE;
    *countOutput = 1;
    if (capacity == 0) return XR_SUCCESS;
    if (capacity < 1 || !types) return XR_ERROR_SIZE_INSUFFICIENT;
    types[0] = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    return XR_SUCCESS;
}

static XrResult impl_GetViewConfigurationProperties(XrInstance instance, XrSystemId systemId,
                                                    XrViewConfigurationType type,
                                                    XrViewConfigurationProperties* props) noexcept {
    if (!valid_instance(instance)) return XR_ERROR_HANDLE_INVALID;
    if (systemId != kSystemId) return XR_ERROR_SYSTEM_INVALID;
    if (type != XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO)
        return XR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED;
    if (!props) return XR_ERROR_VALIDATION_FAILURE;
    props->viewConfigurationType = type;
    props->fovMutable = XR_FALSE;
    return XR_SUCCESS;
}

static XrResult impl_EnumerateViewConfigurationViews(XrInstance instance, XrSystemId systemId,
                                                     XrViewConfigurationType type,
                                                     uint32_t capacity, uint32_t* countOutput,
                                                     XrViewConfigurationView* views) noexcept {
    if (!valid_instance(instance)) return XR_ERROR_HANDLE_INVALID;
    if (systemId != kSystemId) return XR_ERROR_SYSTEM_INVALID;
    if (type != XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO)
        return XR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED;
    if (!countOutput) return XR_ERROR_VALIDATION_FAILURE;
    *countOutput = 2;
    if (capacity == 0) return XR_SUCCESS;
    if (capacity < 2 || !views) return XR_ERROR_SIZE_INSUFFICIENT;
    for (uint32_t i = 0; i < 2; ++i) {
        views[i].type = XR_TYPE_VIEW_CONFIGURATION_VIEW;
        views[i].next = nullptr;
        views[i].recommendedImageRectWidth = g.recommendedWidth;
        views[i].recommendedImageRectHeight = g.recommendedHeight;
        views[i].maxImageRectWidth = 16384;
        views[i].maxImageRectHeight = 16384;
        views[i].recommendedSwapchainSampleCount = 1;
        views[i].maxSwapchainSampleCount = 1;
    }
    return XR_SUCCESS;
}

static XrResult impl_EnumerateEnvironmentBlendModes(XrInstance instance, XrSystemId systemId,
                                                    XrViewConfigurationType type,
                                                    uint32_t capacity, uint32_t* countOutput,
                                                    XrEnvironmentBlendMode* modes) noexcept {
    if (!valid_instance(instance)) return XR_ERROR_HANDLE_INVALID;
    if (systemId != kSystemId) return XR_ERROR_SYSTEM_INVALID;
    if (type != XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO)
        return XR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED;
    if (!countOutput) return XR_ERROR_VALIDATION_FAILURE;
    *countOutput = 1;
    if (capacity == 0) return XR_SUCCESS;
    if (capacity < 1 || !modes) return XR_ERROR_SIZE_INSUFFICIENT;
    modes[0] = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    return XR_SUCCESS;
}

static XrResult impl_GetD3D11GraphicsRequirements(XrInstance instance, XrSystemId systemId,
                                                  XrGraphicsRequirementsD3D11KHR* reqs) noexcept {
    if (!valid_instance(instance)) return XR_ERROR_HANDLE_INVALID;
    if (systemId != kSystemId) return XR_ERROR_SYSTEM_INVALID;
    if (!reqs) return XR_ERROR_VALIDATION_FAILURE;

    reqs->minFeatureLevel = D3D_FEATURE_LEVEL_11_0; // 0xB000, as the real device reports

    // Report a REAL adapter LUID. xr_hello32 creates its device on the matched
    // adapter and falls back to D3D_DRIVER_TYPE_HARDWARE if nothing matches, so
    // a made-up LUID would quietly change what that probe is testing.
    reqs->adapterLuid = LUID{0, 0};
    IDXGIFactory1* factory = nullptr;
    if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) && factory) {
        IDXGIAdapter1* adapter = nullptr;
        SIZE_T best = 0;
        for (UINT i = 0; factory->EnumAdapters1(i, &adapter) == S_OK; ++i) {
            DXGI_ADAPTER_DESC1 desc{};
            adapter->GetDesc1(&desc);
            const bool software = (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;
            if (!software && desc.DedicatedVideoMemory >= best) {
                best = desc.DedicatedVideoMemory;
                reqs->adapterLuid = desc.AdapterLuid;
                XRSIM_LOG_ONCE("xrsim: reporting adapter '%ls' LUID %08lX-%08lX (%llu MB)",
                               desc.Description, desc.AdapterLuid.HighPart,
                               desc.AdapterLuid.LowPart,
                               static_cast<unsigned long long>(desc.DedicatedVideoMemory >> 20));
            }
            adapter->Release();
        }
        factory->Release();
    }
    return XR_SUCCESS;
}

// ---------------------------------------------------------------------------
// SEH shims - one per entry point, correctly typed
// ---------------------------------------------------------------------------

#define XRSIM_SHIM(name, sig, call)                              \
    XRAPI_ATTR XrResult XRAPI_CALL xrsim_##name sig {            \
        XRSIM_ENTRY(impl_##name call, #name)                     \
    }

XRSIM_SHIM(EnumerateInstanceExtensionProperties,
           (const char* l, uint32_t c, uint32_t* o, XrExtensionProperties* p), (l, c, o, p))
XRSIM_SHIM(EnumerateApiLayerProperties, (uint32_t c, uint32_t* o, XrApiLayerProperties* p), (c, o, p))
XRSIM_SHIM(CreateInstance, (const XrInstanceCreateInfo* i, XrInstance* o), (i, o))
XRSIM_SHIM(DestroyInstance, (XrInstance i), (i))
XRSIM_SHIM(GetInstanceProperties, (XrInstance i, XrInstanceProperties* p), (i, p))
XRSIM_SHIM(PollEvent, (XrInstance i, XrEventDataBuffer* e), (i, e))
XRSIM_SHIM(ResultToString, (XrInstance i, XrResult v, char b[XR_MAX_RESULT_STRING_SIZE]), (i, v, b))
XRSIM_SHIM(StructureTypeToString,
           (XrInstance i, XrStructureType v, char b[XR_MAX_STRUCTURE_NAME_SIZE]), (i, v, b))
XRSIM_SHIM(GetSystem, (XrInstance i, const XrSystemGetInfo* gi, XrSystemId* s), (i, gi, s))
XRSIM_SHIM(GetSystemProperties, (XrInstance i, XrSystemId s, XrSystemProperties* p), (i, s, p))
XRSIM_SHIM(EnumerateViewConfigurations,
           (XrInstance i, XrSystemId s, uint32_t c, uint32_t* o, XrViewConfigurationType* t),
           (i, s, c, o, t))
XRSIM_SHIM(GetViewConfigurationProperties,
           (XrInstance i, XrSystemId s, XrViewConfigurationType t, XrViewConfigurationProperties* p),
           (i, s, t, p))
XRSIM_SHIM(EnumerateViewConfigurationViews,
           (XrInstance i, XrSystemId s, XrViewConfigurationType t, uint32_t c, uint32_t* o,
            XrViewConfigurationView* v),
           (i, s, t, c, o, v))
XRSIM_SHIM(EnumerateEnvironmentBlendModes,
           (XrInstance i, XrSystemId s, XrViewConfigurationType t, uint32_t c, uint32_t* o,
            XrEnvironmentBlendMode* m),
           (i, s, t, c, o, m))
XRSIM_SHIM(GetD3D11GraphicsRequirements,
           (XrInstance i, XrSystemId s, XrGraphicsRequirementsD3D11KHR* r), (i, s, r))

// --- paths ------------------------------------------------------------------

static XrResult impl_StringToPath(XrInstance instance, const char* str, XrPath* out) noexcept {
    if (!valid_instance(instance)) return XR_ERROR_HANDLE_INVALID;
    if (!str || !out) return XR_ERROR_VALIDATION_FAILURE;
    if (str[0] != '/') return XR_ERROR_PATH_FORMAT_INVALID;
    const XrPath p = path_intern(str);
    if (p == XR_NULL_PATH) return XR_ERROR_PATH_COUNT_EXCEEDED;
    *out = p;
    return XR_SUCCESS;
}

static XrResult impl_PathToString(XrInstance instance, XrPath path, uint32_t capacity,
                                  uint32_t* countOutput, char* buffer) noexcept {
    if (!valid_instance(instance)) return XR_ERROR_HANDLE_INVALID;
    if (!countOutput) return XR_ERROR_VALIDATION_FAILURE;
    const char* s = path_str(path);
    if (!s || !s[0]) return XR_ERROR_PATH_INVALID;
    const uint32_t need = static_cast<uint32_t>(strlen(s)) + 1;
    *countOutput = need;
    if (capacity == 0) return XR_SUCCESS;
    if (capacity < need || !buffer) return XR_ERROR_SIZE_INSUFFICIENT;
    strcpy_s(buffer, capacity, s);
    return XR_SUCCESS;
}

XRSIM_SHIM(StringToPath, (XrInstance i, const char* s, XrPath* p), (i, s, p))
XRSIM_SHIM(PathToString,
           (XrInstance i, XrPath p, uint32_t c, uint32_t* o, char* b), (i, p, c, o, b))

// ---------------------------------------------------------------------------
// Typed "not supported" stubs
// ---------------------------------------------------------------------------
// These fill dispatch slots the loader populates but neither the mod nor
// xr_hello32 calls. They exist so the slot is never null AND the __stdcall
// argument count is right, which on 32-bit is the difference between a clean
// error return and a corrupted stack.

XRAPI_ATTR XrResult XRAPI_CALL xrsim_GetReferenceSpaceBoundsRect(XrSession, XrReferenceSpaceType,
                                                                 XrExtent2Df*) {
    return XR_SPACE_BOUNDS_UNAVAILABLE;
}
XRAPI_ATTR XrResult XRAPI_CALL xrsim_RequestExitSession(XrSession session) {
    queue_session_state(session, XR_SESSION_STATE_STOPPING);
    return XR_SUCCESS;
}
XRAPI_ATTR XrResult XRAPI_CALL xrsim_EnumerateBoundSourcesForAction(
    XrSession, const XrBoundSourcesForActionEnumerateInfo*, uint32_t, uint32_t* countOutput,
    XrPath*) {
    if (countOutput) *countOutput = 0;
    return XR_SUCCESS;
}
XRAPI_ATTR XrResult XRAPI_CALL xrsim_GetInputSourceLocalizedName(
    XrSession, const XrInputSourceLocalizedNameGetInfo*, uint32_t, uint32_t* countOutput,
    char* buffer) {
    if (countOutput) *countOutput = 1;
    if (buffer) buffer[0] = '\0';
    return XR_SUCCESS;
}
XRAPI_ATTR XrResult XRAPI_CALL xrsim_ApplyHapticFeedback(XrSession, const XrHapticActionInfo*,
                                                         const XrHapticBaseHeader*) {
    ++g.hapticPulses; // counted so a future haptics feature has an oracle already
    return XR_SUCCESS;
}
XRAPI_ATTR XrResult XRAPI_CALL xrsim_StopHapticFeedback(XrSession, const XrHapticActionInfo*) {
    return XR_SUCCESS;
}
XRAPI_ATTR XrResult XRAPI_CALL xrsim_LocateSpaces(XrSession, const XrSpacesLocateInfo*,
                                                  XrSpaceLocations*) {
    return XR_ERROR_FUNCTION_UNSUPPORTED; // an OpenXR 1.1 name in the 1.0 table
}
XRAPI_ATTR XrResult XRAPI_CALL xrsim_SetDebugUtilsObjectNameEXT(XrInstance,
                                                                const XrDebugUtilsObjectNameInfoEXT*) {
    return XR_SUCCESS;
}
XRAPI_ATTR XrResult XRAPI_CALL xrsim_CreateDebugUtilsMessengerEXT(
    XrInstance, const XrDebugUtilsMessengerCreateInfoEXT*, XrDebugUtilsMessengerEXT*) {
    return XR_ERROR_FUNCTION_UNSUPPORTED;
}
XRAPI_ATTR XrResult XRAPI_CALL xrsim_DestroyDebugUtilsMessengerEXT(XrDebugUtilsMessengerEXT) {
    return XR_ERROR_FUNCTION_UNSUPPORTED;
}
XRAPI_ATTR XrResult XRAPI_CALL xrsim_SubmitDebugUtilsMessageEXT(
    XrInstance, XrDebugUtilsMessageSeverityFlagsEXT, XrDebugUtilsMessageTypeFlagsEXT,
    const XrDebugUtilsMessengerCallbackDataEXT*) {
    return XR_SUCCESS;
}
XRAPI_ATTR XrResult XRAPI_CALL xrsim_SessionBeginDebugUtilsLabelRegionEXT(
    XrSession, const XrDebugUtilsLabelEXT*) {
    return XR_SUCCESS;
}
XRAPI_ATTR XrResult XRAPI_CALL xrsim_SessionEndDebugUtilsLabelRegionEXT(XrSession) {
    return XR_SUCCESS;
}
XRAPI_ATTR XrResult XRAPI_CALL xrsim_SessionInsertDebugUtilsLabelEXT(XrSession,
                                                                     const XrDebugUtilsLabelEXT*) {
    return XR_SUCCESS;
}

// ---------------------------------------------------------------------------
// xrGetInstanceProcAddr
// ---------------------------------------------------------------------------

XRAPI_ATTR XrResult XRAPI_CALL xrsim_GetInstanceProcAddr(XrInstance instance, const char* name,
                                                         PFN_xrVoidFunction* function) {
    if (!name || !function) return XR_ERROR_VALIDATION_FAILURE;
    *function = nullptr;

#define XRSIM_BIND(publicName, fn)                                     \
    if (strcmp(name, publicName) == 0) {                               \
        *function = reinterpret_cast<PFN_xrVoidFunction>(fn);          \
        return XR_SUCCESS;                                             \
    }

    // xrInitializeLoaderKHR is the one name where "success plus a non-null
    // pointer" is actively harmful: the loader would then CALL it and fail the
    // whole runtime load if it errored. It must be reported unsupported.
    if (strcmp(name, "xrInitializeLoaderKHR") == 0) return XR_ERROR_FUNCTION_UNSUPPORTED;

    XRSIM_BIND("xrGetInstanceProcAddr", xrsim_GetInstanceProcAddr)
    XRSIM_BIND("xrEnumerateApiLayerProperties", xrsim_EnumerateApiLayerProperties)
    XRSIM_BIND("xrEnumerateInstanceExtensionProperties", xrsim_EnumerateInstanceExtensionProperties)
    XRSIM_BIND("xrCreateInstance", xrsim_CreateInstance)
    XRSIM_BIND("xrDestroyInstance", xrsim_DestroyInstance)
    XRSIM_BIND("xrGetInstanceProperties", xrsim_GetInstanceProperties)
    XRSIM_BIND("xrPollEvent", xrsim_PollEvent)
    XRSIM_BIND("xrResultToString", xrsim_ResultToString)
    XRSIM_BIND("xrStructureTypeToString", xrsim_StructureTypeToString)
    XRSIM_BIND("xrGetSystem", xrsim_GetSystem)
    XRSIM_BIND("xrGetSystemProperties", xrsim_GetSystemProperties)
    XRSIM_BIND("xrEnumerateEnvironmentBlendModes", xrsim_EnumerateEnvironmentBlendModes)
    XRSIM_BIND("xrEnumerateViewConfigurations", xrsim_EnumerateViewConfigurations)
    XRSIM_BIND("xrGetViewConfigurationProperties", xrsim_GetViewConfigurationProperties)
    XRSIM_BIND("xrEnumerateViewConfigurationViews", xrsim_EnumerateViewConfigurationViews)
    XRSIM_BIND("xrGetD3D11GraphicsRequirementsKHR", xrsim_GetD3D11GraphicsRequirements)
    XRSIM_BIND("xrStringToPath", xrsim_StringToPath)
    XRSIM_BIND("xrPathToString", xrsim_PathToString)

    XRSIM_BIND("xrCreateSession", xrsim_CreateSession)
    XRSIM_BIND("xrDestroySession", xrsim_DestroySession)
    XRSIM_BIND("xrBeginSession", xrsim_BeginSession)
    XRSIM_BIND("xrEndSession", xrsim_EndSession)
    XRSIM_BIND("xrRequestExitSession", xrsim_RequestExitSession)
    XRSIM_BIND("xrEnumerateReferenceSpaces", xrsim_EnumerateReferenceSpaces)
    XRSIM_BIND("xrCreateReferenceSpace", xrsim_CreateReferenceSpace)
    XRSIM_BIND("xrGetReferenceSpaceBoundsRect", xrsim_GetReferenceSpaceBoundsRect)
    XRSIM_BIND("xrCreateActionSpace", xrsim_CreateActionSpace)
    XRSIM_BIND("xrLocateSpace", xrsim_LocateSpace)
    XRSIM_BIND("xrDestroySpace", xrsim_DestroySpace)
    XRSIM_BIND("xrLocateSpaces", xrsim_LocateSpaces)

    XRSIM_BIND("xrEnumerateSwapchainFormats", xrsim_EnumerateSwapchainFormats)
    XRSIM_BIND("xrCreateSwapchain", xrsim_CreateSwapchain)
    XRSIM_BIND("xrDestroySwapchain", xrsim_DestroySwapchain)
    XRSIM_BIND("xrEnumerateSwapchainImages", xrsim_EnumerateSwapchainImages)
    XRSIM_BIND("xrAcquireSwapchainImage", xrsim_AcquireSwapchainImage)
    XRSIM_BIND("xrWaitSwapchainImage", xrsim_WaitSwapchainImage)
    XRSIM_BIND("xrReleaseSwapchainImage", xrsim_ReleaseSwapchainImage)

    XRSIM_BIND("xrWaitFrame", xrsim_WaitFrame)
    XRSIM_BIND("xrBeginFrame", xrsim_BeginFrame)
    XRSIM_BIND("xrEndFrame", xrsim_EndFrame)
    XRSIM_BIND("xrLocateViews", xrsim_LocateViews)

    XRSIM_BIND("xrCreateActionSet", xrsim_CreateActionSet)
    XRSIM_BIND("xrDestroyActionSet", xrsim_DestroyActionSet)
    XRSIM_BIND("xrCreateAction", xrsim_CreateAction)
    XRSIM_BIND("xrDestroyAction", xrsim_DestroyAction)
    XRSIM_BIND("xrSuggestInteractionProfileBindings", xrsim_SuggestInteractionProfileBindings)
    XRSIM_BIND("xrAttachSessionActionSets", xrsim_AttachSessionActionSets)
    XRSIM_BIND("xrGetCurrentInteractionProfile", xrsim_GetCurrentInteractionProfile)
    XRSIM_BIND("xrGetActionStateBoolean", xrsim_GetActionStateBoolean)
    XRSIM_BIND("xrGetActionStateFloat", xrsim_GetActionStateFloat)
    XRSIM_BIND("xrGetActionStateVector2f", xrsim_GetActionStateVector2f)
    XRSIM_BIND("xrGetActionStatePose", xrsim_GetActionStatePose)
    XRSIM_BIND("xrSyncActions", xrsim_SyncActions)
    XRSIM_BIND("xrEnumerateBoundSourcesForAction", xrsim_EnumerateBoundSourcesForAction)
    XRSIM_BIND("xrGetInputSourceLocalizedName", xrsim_GetInputSourceLocalizedName)
    XRSIM_BIND("xrApplyHapticFeedback", xrsim_ApplyHapticFeedback)
    XRSIM_BIND("xrStopHapticFeedback", xrsim_StopHapticFeedback)

    XRSIM_BIND("xrSetDebugUtilsObjectNameEXT", xrsim_SetDebugUtilsObjectNameEXT)
    XRSIM_BIND("xrCreateDebugUtilsMessengerEXT", xrsim_CreateDebugUtilsMessengerEXT)
    XRSIM_BIND("xrDestroyDebugUtilsMessengerEXT", xrsim_DestroyDebugUtilsMessengerEXT)
    XRSIM_BIND("xrSubmitDebugUtilsMessageEXT", xrsim_SubmitDebugUtilsMessageEXT)
    XRSIM_BIND("xrSessionBeginDebugUtilsLabelRegionEXT", xrsim_SessionBeginDebugUtilsLabelRegionEXT)
    XRSIM_BIND("xrSessionEndDebugUtilsLabelRegionEXT", xrsim_SessionEndDebugUtilsLabelRegionEXT)
    XRSIM_BIND("xrSessionInsertDebugUtilsLabelEXT", xrsim_SessionInsertDebugUtilsLabelEXT)

#undef XRSIM_BIND

    // An unknown name is a version-drift signal worth seeing once: a future
    // OpenXR-SDK bump could add a dispatch slot this table does not cover.
    XRSIM_LOG_ONCE("xrsim: GIPA asked for an unknown entry point '%s' - reporting unsupported", name);
    (void)instance;
    return XR_ERROR_FUNCTION_UNSUPPORTED;
}

} // namespace xrsim

// ---------------------------------------------------------------------------
// The one exported symbol
// ---------------------------------------------------------------------------

extern "C" XRAPI_ATTR XrResult XRAPI_CALL
xrNegotiateLoaderRuntimeInterface(const XrNegotiateLoaderInfo* loaderInfo,
                                  XrNegotiateRuntimeRequest* runtimeRequest) {
    using namespace xrsim;
    log::init();

    if (!loaderInfo || !runtimeRequest) return XR_ERROR_INITIALIZATION_FAILED;
    if (loaderInfo->structType != XR_LOADER_INTERFACE_STRUCT_LOADER_INFO ||
        loaderInfo->structVersion != XR_LOADER_INFO_STRUCT_VERSION ||
        loaderInfo->structSize != sizeof(XrNegotiateLoaderInfo)) {
        XRSIM_LOG("xrsim: negotiate REJECTED - loader info struct mismatch");
        return XR_ERROR_INITIALIZATION_FAILED;
    }
    if (runtimeRequest->structType != XR_LOADER_INTERFACE_STRUCT_RUNTIME_REQUEST ||
        runtimeRequest->structVersion != XR_RUNTIME_INFO_STRUCT_VERSION ||
        runtimeRequest->structSize != sizeof(XrNegotiateRuntimeRequest)) {
        XRSIM_LOG("xrsim: negotiate REJECTED - runtime request struct mismatch");
        return XR_ERROR_INITIALIZATION_FAILED;
    }

    const uint32_t iface = XR_CURRENT_LOADER_RUNTIME_VERSION;
    const XrVersion api = XR_MAKE_VERSION(1, 0, 34);
    if (loaderInfo->minInterfaceVersion > iface || loaderInfo->maxInterfaceVersion < iface ||
        loaderInfo->minApiVersion > api || loaderInfo->maxApiVersion < api) {
        XRSIM_LOG("xrsim: negotiate REJECTED - version window does not include iface %u api 1.0.34",
                  iface);
        return XR_ERROR_INITIALIZATION_FAILED;
    }

    runtimeRequest->runtimeInterfaceVersion = iface;
    runtimeRequest->runtimeApiVersion = api;
    runtimeRequest->getInstanceProcAddr = xrsim_GetInstanceProcAddr;

    // Logging the loader's advertised window makes a future submodule bump
    // visible in one line instead of as a mysterious load failure.
    XRSIM_LOG("xrsim: negotiate ok (loader iface %u..%u, api %u.%u.%u..%u.%u.%u) -> runtime iface %u api 1.0.34",
              loaderInfo->minInterfaceVersion, loaderInfo->maxInterfaceVersion,
              XR_VERSION_MAJOR(loaderInfo->minApiVersion), XR_VERSION_MINOR(loaderInfo->minApiVersion),
              XR_VERSION_PATCH(loaderInfo->minApiVersion),
              XR_VERSION_MAJOR(loaderInfo->maxApiVersion), XR_VERSION_MINOR(loaderInfo->maxApiVersion),
              XR_VERSION_PATCH(loaderInfo->maxApiVersion), iface);
    return XR_SUCCESS;
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        // No work in DllMain beyond this: the loader lock is held, and the mod's
        // own crash history in this repo is mostly loader-lock violations.
        DisableThreadLibraryCalls(module);
    }
    return TRUE;
}
