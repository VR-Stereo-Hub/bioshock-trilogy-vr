// ============================================================================
//  ovrshim_negotiate.cpp - the loader handshake + GIPA dispatch.
//
//  This is the piece that turns the donor's "fake openxr_loader.dll" into a
//  real OpenXR runtime: the statically linked Khronos loader finds this DLL
//  through a runtime manifest (XR_RUNTIME_JSON, written by the mod at fallback
//  time), calls the one exported symbol below, and routes every OpenXR call
//  through the returned xrGetInstanceProcAddr. A function this table lacks is
//  a per-call XR_ERROR_FUNCTION_UNSUPPORTED - never the donor's
//  mod-fails-to-load-with-error-127 trap.
//
//  Negotiate/GIPA shape modeled on src/tools/xrsim/xrsim_instance.cpp (this
//  repo). Entry point implementations: ovrshim_main.cpp / ovrshim_input.cpp,
//  adapted from BioVRDev's OpenXRShim with permission (THIRD_PARTY_NOTICES.md).
// ============================================================================
#include "ovrshim.h"

#define XR_NO_PROTOTYPES_LOADER
#include <openxr/openxr_loader_negotiation.h>

#include <cstring>

static XRAPI_ATTR XrResult XRAPI_CALL shim_GetInstanceProcAddr(
    XrInstance instance, const char* name, PFN_xrVoidFunction* fn)
{
    if (!name || !fn) return XR_ERROR_VALIDATION_FAILURE;
    *fn = nullptr;

    struct ProcEntry { const char* name; PFN_xrVoidFunction fn; };
    static const ProcEntry table[] = {
        { "xrGetInstanceProcAddr",       (PFN_xrVoidFunction)shim_GetInstanceProcAddr },
        { "xrEnumerateInstanceExtensionProperties", (PFN_xrVoidFunction)shim_EnumerateInstanceExtensionProperties },
        { "xrCreateInstance",            (PFN_xrVoidFunction)shim_CreateInstance },
        { "xrDestroyInstance",           (PFN_xrVoidFunction)shim_DestroyInstance },
        { "xrGetInstanceProperties",     (PFN_xrVoidFunction)shim_GetInstanceProperties },
        { "xrResultToString",            (PFN_xrVoidFunction)shim_ResultToString },
        { "xrGetSystem",                 (PFN_xrVoidFunction)shim_GetSystem },
        { "xrGetSystemProperties",       (PFN_xrVoidFunction)shim_GetSystemProperties },
        { "xrGetD3D11GraphicsRequirementsKHR", (PFN_xrVoidFunction)shim_GetD3D11GraphicsRequirementsKHR },
        { "xrEnumerateViewConfigurationViews", (PFN_xrVoidFunction)shim_EnumerateViewConfigurationViews },
        { "xrCreateSession",             (PFN_xrVoidFunction)shim_CreateSession },
        { "xrDestroySession",            (PFN_xrVoidFunction)shim_DestroySession },
        { "xrBeginSession",              (PFN_xrVoidFunction)shim_BeginSession },
        { "xrEndSession",                (PFN_xrVoidFunction)shim_EndSession },
        { "xrPollEvent",                 (PFN_xrVoidFunction)shim_PollEvent },
        { "xrCreateReferenceSpace",      (PFN_xrVoidFunction)shim_CreateReferenceSpace },
        { "xrDestroySpace",              (PFN_xrVoidFunction)shim_DestroySpace },
        { "xrEnumerateSwapchainFormats", (PFN_xrVoidFunction)shim_EnumerateSwapchainFormats },
        { "xrCreateSwapchain",           (PFN_xrVoidFunction)shim_CreateSwapchain },
        { "xrDestroySwapchain",          (PFN_xrVoidFunction)shim_DestroySwapchain },
        { "xrEnumerateSwapchainImages",  (PFN_xrVoidFunction)shim_EnumerateSwapchainImages },
        { "xrAcquireSwapchainImage",     (PFN_xrVoidFunction)shim_AcquireSwapchainImage },
        { "xrWaitSwapchainImage",        (PFN_xrVoidFunction)shim_WaitSwapchainImage },
        { "xrReleaseSwapchainImage",     (PFN_xrVoidFunction)shim_ReleaseSwapchainImage },
        { "xrWaitFrame",                 (PFN_xrVoidFunction)shim_WaitFrame },
        { "xrBeginFrame",                (PFN_xrVoidFunction)shim_BeginFrame },
        { "xrLocateViews",               (PFN_xrVoidFunction)shim_LocateViews },
        { "xrEndFrame",                  (PFN_xrVoidFunction)shim_EndFrame },
        { "xrStringToPath",              (PFN_xrVoidFunction)shim_StringToPath },
        { "xrCreateActionSet",           (PFN_xrVoidFunction)shim_CreateActionSet },
        { "xrCreateAction",              (PFN_xrVoidFunction)shim_CreateAction },
        { "xrSuggestInteractionProfileBindings", (PFN_xrVoidFunction)shim_SuggestInteractionProfileBindings },
        { "xrAttachSessionActionSets",   (PFN_xrVoidFunction)shim_AttachSessionActionSets },
        { "xrCreateActionSpace",         (PFN_xrVoidFunction)shim_CreateActionSpace },
        { "xrSyncActions",               (PFN_xrVoidFunction)shim_SyncActions },
        { "xrApplyHapticFeedback",       (PFN_xrVoidFunction)shim_ApplyHapticFeedback },
        { "xrGetActionStateBoolean",     (PFN_xrVoidFunction)shim_GetActionStateBoolean },
        { "xrGetActionStateFloat",       (PFN_xrVoidFunction)shim_GetActionStateFloat },
        { "xrGetActionStateVector2f",    (PFN_xrVoidFunction)shim_GetActionStateVector2f },
        { "xrGetActionStatePose",        (PFN_xrVoidFunction)shim_GetActionStatePose },
        { "xrLocateSpace",               (PFN_xrVoidFunction)shim_LocateSpace },
    };
    for (const ProcEntry& e : table)
        if (strcmp(e.name, name) == 0) { *fn = e.fn; return XR_SUCCESS; }

    // The loader probes its whole generated dispatch table at instance create,
    // so unknown names here are NORMAL (dozens of them, once). Only a name the
    // MOD then actually calls would matter - and the mod's surface is fully
    // covered above. Log once per name would be spam; stay quiet.
    (void)instance;
    return XR_ERROR_FUNCTION_UNSUPPORTED;
}

// ---------------------------------------------------------------------------
// The one exported symbol (see ovrshim.def for why a .def pins the name).
// ---------------------------------------------------------------------------
extern "C" XRAPI_ATTR XrResult XRAPI_CALL
xrNegotiateLoaderRuntimeInterface(const XrNegotiateLoaderInfo* loaderInfo,
                                  XrNegotiateRuntimeRequest* runtimeRequest)
{
    if (!loaderInfo || !runtimeRequest) return XR_ERROR_INITIALIZATION_FAILED;
    if (loaderInfo->structType != XR_LOADER_INTERFACE_STRUCT_LOADER_INFO ||
        loaderInfo->structVersion != XR_LOADER_INFO_STRUCT_VERSION ||
        loaderInfo->structSize != sizeof(XrNegotiateLoaderInfo)) {
        SLOG("ovrshim: negotiate REJECTED - loader info struct mismatch");
        return XR_ERROR_INITIALIZATION_FAILED;
    }
    if (runtimeRequest->structType != XR_LOADER_INTERFACE_STRUCT_RUNTIME_REQUEST ||
        runtimeRequest->structVersion != XR_RUNTIME_INFO_STRUCT_VERSION ||
        runtimeRequest->structSize != sizeof(XrNegotiateRuntimeRequest)) {
        SLOG("ovrshim: negotiate REJECTED - runtime request struct mismatch");
        return XR_ERROR_INITIALIZATION_FAILED;
    }

    const uint32_t iface = XR_CURRENT_LOADER_RUNTIME_VERSION;
    const XrVersion api = XR_MAKE_VERSION(1, 0, 34);
    if (loaderInfo->minInterfaceVersion > iface || loaderInfo->maxInterfaceVersion < iface ||
        loaderInfo->minApiVersion > api || loaderInfo->maxApiVersion < api) {
        SLOG("ovrshim: negotiate REJECTED - version window does not include iface %u api 1.0.34",
             iface);
        return XR_ERROR_INITIALIZATION_FAILED;
    }

    runtimeRequest->runtimeInterfaceVersion = iface;
    runtimeRequest->runtimeApiVersion = api;
    runtimeRequest->getInstanceProcAddr = shim_GetInstanceProcAddr;

    SLOG("ovrshim: negotiate ok (loader iface %u..%u) -> runtime iface %u api 1.0.34",
         loaderInfo->minInterfaceVersion, loaderInfo->maxInterfaceVersion, iface);
    return XR_SUCCESS;
}
