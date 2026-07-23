// Port of the proven xr_hello32 flow (DR-1) into the game process. The big
// differences from the standalone probe: the session binds the game's own
// ID3D11Device (grabbed from the hooked swapchain), bring-up is lazy and
// retried on a cooldown from the render thread, and each frame submits one
// quad layer containing a copy of the game backbuffer.

#include "core/vr/openxr_runtime.h"

#include "core/util/log.h"

#ifdef BVR_WITH_OPENXR

#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D11

#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <imgui.h>

#include <atomic>
#include <cstring>
#include <vector>

namespace bvr::vr {
namespace {

XrInstance g_instance = XR_NULL_HANDLE;
char g_runtimeName[XR_MAX_RUNTIME_NAME_SIZE] = "none";
XrSystemId g_system = XR_NULL_SYSTEM_ID;
XrSession g_session = XR_NULL_HANDLE;
XrSpace g_space = XR_NULL_HANDLE;
XrSessionState g_state = XR_SESSION_STATE_UNKNOWN;
bool g_sessionBegun = false;
bool g_frameOpen = false;
XrFrameState g_frameState{XR_TYPE_FRAME_STATE};

XrSwapchain g_quadSwapchain = XR_NULL_HANDLE;
std::vector<XrSwapchainImageD3D11KHR> g_quadImages;
uint32_t g_quadW = 0, g_quadH = 0;
int64_t g_quadFormat = 0;

ID3D11Device* g_device = nullptr;          // game device, AddRef'd
ID3D11DeviceContext* g_context = nullptr;  // immediate context, AddRef'd

// Controls (overlay writes, render thread reads).
std::atomic<bool> g_enabled{true};        // kill switch: tears the session down
std::atomic<float> g_screenDistM{1.75f};  // quad distance in meters
std::atomic<float> g_screenWidthM{2.4f};  // quad width in meters

// Bring-up retry (render thread only).
uint64_t g_nextRetryMs = 0;
bool g_loggedNoHmd = false;
uint32_t g_framesSubmitted = 0;

const char* res_str(XrResult r) {
    static char buf[XR_MAX_RESULT_STRING_SIZE];
    if (g_instance != XR_NULL_HANDLE && xrResultToString(g_instance, r, buf) == XR_SUCCESS)
        return buf;
    sprintf_s(buf, "XrResult(%d)", static_cast<int>(r));
    return buf;
}

const char* state_str(XrSessionState s) {
    switch (s) {
        case XR_SESSION_STATE_IDLE: return "IDLE";
        case XR_SESSION_STATE_READY: return "READY";
        case XR_SESSION_STATE_SYNCHRONIZED: return "SYNCHRONIZED";
        case XR_SESSION_STATE_VISIBLE: return "VISIBLE";
        case XR_SESSION_STATE_FOCUSED: return "FOCUSED";
        case XR_SESSION_STATE_STOPPING: return "STOPPING";
        case XR_SESSION_STATE_LOSS_PENDING: return "LOSS_PENDING";
        case XR_SESSION_STATE_EXITING: return "EXITING";
        default: return "UNKNOWN";
    }
}

void destroy_quad_swapchain() {
    if (g_quadSwapchain != XR_NULL_HANDLE) {
        xrDestroySwapchain(g_quadSwapchain);
        g_quadSwapchain = XR_NULL_HANDLE;
    }
    g_quadImages.clear();
    g_quadW = g_quadH = 0;
}

void teardown_session(const char* why) {
    BVR_LOG("xr: session teardown (%s)", why);
    destroy_quad_swapchain();
    if (g_space != XR_NULL_HANDLE) { xrDestroySpace(g_space); g_space = XR_NULL_HANDLE; }
    if (g_session != XR_NULL_HANDLE) { xrDestroySession(g_session); g_session = XR_NULL_HANDLE; }
    if (g_context) { g_context->Release(); g_context = nullptr; }
    if (g_device) { g_device->Release(); g_device = nullptr; }
    g_sessionBegun = false;
    g_frameOpen = false;
    g_system = XR_NULL_SYSTEM_ID;
    g_state = XR_SESSION_STATE_UNKNOWN;
    g_framesSubmitted = 0;
    g_nextRetryMs = GetTickCount64() + 5000; // cooldown before the next attempt
}

bool create_quad_swapchain(IDXGISwapChain* swapchain) {
    DXGI_SWAP_CHAIN_DESC desc{};
    if (FAILED(swapchain->GetDesc(&desc))) return false;

    // Pick a swapchain format CopyResource-compatible with the backbuffer
    // (same typeless family). Prefer the sRGB view so the compositor reads the
    // game's gamma-encoded output correctly.
    uint32_t formatCount = 0;
    xrEnumerateSwapchainFormats(g_session, 0, &formatCount, nullptr);
    std::vector<int64_t> formats(formatCount);
    xrEnumerateSwapchainFormats(g_session, formatCount, &formatCount, formats.data());
    int64_t pick = 0;
    for (int64_t want : {static_cast<int64_t>(DXGI_FORMAT_R8G8B8A8_UNORM_SRGB),
                         static_cast<int64_t>(DXGI_FORMAT_R8G8B8A8_UNORM)}) {
        for (int64_t f : formats)
            if (f == want) { pick = f; break; }
        if (pick) break;
    }
    if (!pick) {
        BVR_LOG("xr: no R8G8B8A8 swapchain format offered - cannot copy backbuffer");
        return false;
    }

    XrSwapchainCreateInfo sci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    sci.usageFlags = XR_SWAPCHAIN_USAGE_SAMPLED_BIT | XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
    sci.format = pick;
    sci.sampleCount = 1;
    sci.width = desc.BufferDesc.Width;
    sci.height = desc.BufferDesc.Height;
    sci.faceCount = 1;
    sci.arraySize = 1;
    sci.mipCount = 1;
    XrResult r = xrCreateSwapchain(g_session, &sci, &g_quadSwapchain);
    if (XR_FAILED(r)) {
        BVR_LOG("xr: xrCreateSwapchain failed: %s", res_str(r));
        g_quadSwapchain = XR_NULL_HANDLE;
        return false;
    }

    uint32_t imageCount = 0;
    xrEnumerateSwapchainImages(g_quadSwapchain, 0, &imageCount, nullptr);
    g_quadImages.assign(imageCount, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
    r = xrEnumerateSwapchainImages(
        g_quadSwapchain, imageCount, &imageCount,
        reinterpret_cast<XrSwapchainImageBaseHeader*>(g_quadImages.data()));
    if (XR_FAILED(r)) {
        BVR_LOG("xr: xrEnumerateSwapchainImages failed: %s", res_str(r));
        destroy_quad_swapchain();
        return false;
    }

    g_quadW = desc.BufferDesc.Width;
    g_quadH = desc.BufferDesc.Height;
    g_quadFormat = pick;
    BVR_LOG("xr: quad swapchain %ux%u format %lld (%u images)", g_quadW, g_quadH,
            static_cast<long long>(pick), imageCount);
    return true;
}

// One bring-up attempt: system -> requirements -> session on the game device
// -> LOCAL space. Any failure logs, tears down, and arms the retry cooldown.
void try_bring_up(IDXGISwapChain* swapchain) {
    XrSystemGetInfo sgi{XR_TYPE_SYSTEM_GET_INFO};
    sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    XrResult r = xrGetSystem(g_instance, &sgi, &g_system);
    if (r == XR_ERROR_FORM_FACTOR_UNAVAILABLE) {
        if (!g_loggedNoHmd) {
            BVR_LOG("xr: no headset connected (runtime '%s') - will keep retrying quietly",
                    g_runtimeName);
            g_loggedNoHmd = true;
        }
        g_nextRetryMs = GetTickCount64() + 5000;
        return;
    }
    if (XR_FAILED(r)) {
        BVR_LOG("xr: xrGetSystem failed: %s", res_str(r));
        g_nextRetryMs = GetTickCount64() + 5000;
        return;
    }
    g_loggedNoHmd = false;

    XrSystemProperties sp{XR_TYPE_SYSTEM_PROPERTIES};
    xrGetSystemProperties(g_instance, g_system, &sp);
    BVR_LOG("xr: system '%s' (max layers %u)", sp.systemName,
            sp.graphicsProperties.maxLayerCount);

    // Spec requires querying graphics requirements before xrCreateSession.
    PFN_xrGetD3D11GraphicsRequirementsKHR getReqs = nullptr;
    xrGetInstanceProcAddr(g_instance, "xrGetD3D11GraphicsRequirementsKHR",
                          reinterpret_cast<PFN_xrVoidFunction*>(&getReqs));
    XrGraphicsRequirementsD3D11KHR reqs{XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR};
    if (!getReqs || XR_FAILED(r = getReqs(g_instance, g_system, &reqs))) {
        BVR_LOG("xr: xrGetD3D11GraphicsRequirementsKHR failed: %s",
                getReqs ? res_str(r) : "proc not found");
        g_system = XR_NULL_SYSTEM_ID;
        g_nextRetryMs = GetTickCount64() + 5000;
        return;
    }

    if (FAILED(swapchain->GetDevice(IID_PPV_ARGS(&g_device)))) {
        BVR_LOG("xr: could not get game device from swapchain");
        teardown_session("no device");
        return;
    }
    g_device->GetImmediateContext(&g_context);
    BVR_LOG("xr: game device feature level 0x%X, runtime min 0x%X",
            g_device->GetFeatureLevel(), reqs.minFeatureLevel);

    XrGraphicsBindingD3D11KHR binding{XR_TYPE_GRAPHICS_BINDING_D3D11_KHR};
    binding.device = g_device;
    XrSessionCreateInfo sci{XR_TYPE_SESSION_CREATE_INFO};
    sci.next = &binding;
    sci.systemId = g_system;
    r = xrCreateSession(g_instance, &sci, &g_session);
    if (XR_FAILED(r)) {
        BVR_LOG("xr: xrCreateSession failed: %s", res_str(r));
        teardown_session("create failed");
        return;
    }

    XrReferenceSpaceCreateInfo rsci{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    rsci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    rsci.poseInReferenceSpace.orientation.w = 1.0f;
    r = xrCreateReferenceSpace(g_session, &rsci, &g_space);
    if (XR_FAILED(r)) {
        BVR_LOG("xr: xrCreateReferenceSpace failed: %s", res_str(r));
        teardown_session("space failed");
        return;
    }

    if (!create_quad_swapchain(swapchain)) {
        teardown_session("swapchain failed");
        return;
    }

    BVR_LOG("xr: session created on the game device - waiting for READY");
}

void pump_events() {
    XrEventDataBuffer ev{XR_TYPE_EVENT_DATA_BUFFER};
    while (xrPollEvent(g_instance, &ev) == XR_SUCCESS) {
        if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
            auto* sc = reinterpret_cast<XrEventDataSessionStateChanged*>(&ev);
            g_state = sc->state;
            BVR_LOG("xr: session state %s", state_str(g_state));
            switch (g_state) {
                case XR_SESSION_STATE_READY: {
                    XrSessionBeginInfo sbi{XR_TYPE_SESSION_BEGIN_INFO};
                    sbi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                    XrResult r = xrBeginSession(g_session, &sbi);
                    if (XR_FAILED(r)) {
                        BVR_LOG("xr: xrBeginSession failed: %s", res_str(r));
                        teardown_session("begin failed");
                    } else {
                        g_sessionBegun = true;
                        BVR_LOG("xr: session running - game is now paced by the headset");
                    }
                    break;
                }
                case XR_SESSION_STATE_STOPPING:
                    if (g_sessionBegun) {
                        xrEndSession(g_session);
                        g_sessionBegun = false;
                        BVR_LOG("xr: session stopped (headset idle?) - waiting for READY again");
                    }
                    break;
                case XR_SESSION_STATE_LOSS_PENDING:
                case XR_SESSION_STATE_EXITING:
                    teardown_session(state_str(g_state));
                    return; // g_session is gone; stop pumping
                default:
                    break;
            }
        } else if (ev.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) {
            teardown_session("instance loss pending");
            return;
        }
        ev = {XR_TYPE_EVENT_DATA_BUFFER};
    }
}

} // namespace

void init_instance() {
    uint32_t extCount = 0;
    XrResult r = xrEnumerateInstanceExtensionProperties(nullptr, 0, &extCount, nullptr);
    if (XR_FAILED(r)) {
        BVR_LOG("xr: no 32-bit OpenXR runtime reachable (%d) - VR disabled, game runs flat",
                static_cast<int>(r));
        return;
    }
    std::vector<XrExtensionProperties> exts(extCount, {XR_TYPE_EXTENSION_PROPERTIES});
    xrEnumerateInstanceExtensionProperties(nullptr, extCount, &extCount, exts.data());
    bool hasD3D11 = false;
    for (const auto& e : exts)
        if (strcmp(e.extensionName, XR_KHR_D3D11_ENABLE_EXTENSION_NAME) == 0) hasD3D11 = true;
    if (!hasD3D11) {
        BVR_LOG("xr: runtime lacks XR_KHR_D3D11_enable - VR disabled");
        return;
    }

    const char* enabled[] = {XR_KHR_D3D11_ENABLE_EXTENSION_NAME};
    XrInstanceCreateInfo ici{XR_TYPE_INSTANCE_CREATE_INFO};
    strcpy_s(ici.applicationInfo.applicationName, "bioshock-vr");
    ici.applicationInfo.applicationVersion = 1;
    strcpy_s(ici.applicationInfo.engineName, "bioshock-vr");
    ici.applicationInfo.apiVersion = XR_API_VERSION_1_0;
    ici.enabledExtensionCount = 1;
    ici.enabledExtensionNames = enabled;
    r = xrCreateInstance(&ici, &g_instance);
    if (XR_FAILED(r)) {
        BVR_LOG("xr: xrCreateInstance failed: %s - VR disabled", res_str(r));
        g_instance = XR_NULL_HANDLE;
        return;
    }

    XrInstanceProperties ip{XR_TYPE_INSTANCE_PROPERTIES};
    xrGetInstanceProperties(g_instance, &ip);
    strcpy_s(g_runtimeName, ip.runtimeName);
    BVR_LOG("xr: instance created on runtime '%s' %u.%u.%u", ip.runtimeName,
            XR_VERSION_MAJOR(ip.runtimeVersion), XR_VERSION_MINOR(ip.runtimeVersion),
            XR_VERSION_PATCH(ip.runtimeVersion));
}

void on_present_begin(IDXGISwapChain* swapchain) {
    if (g_instance == XR_NULL_HANDLE) return;

    if (!g_enabled.load(std::memory_order_relaxed)) {
        if (g_session != XR_NULL_HANDLE) teardown_session("disabled in overlay");
        return;
    }

    if (g_session == XR_NULL_HANDLE) {
        if (GetTickCount64() < g_nextRetryMs) return;
        try_bring_up(swapchain);
        if (g_session == XR_NULL_HANDLE) return;
    }

    pump_events();
    if (g_session == XR_NULL_HANDLE || !g_sessionBegun) return;

    XrFrameWaitInfo fwi{XR_TYPE_FRAME_WAIT_INFO};
    g_frameState = {XR_TYPE_FRAME_STATE};
    XrResult r = xrWaitFrame(g_session, &fwi, &g_frameState);
    if (XR_FAILED(r)) {
        BVR_LOG("xr: xrWaitFrame failed: %s", res_str(r));
        teardown_session("waitframe failed");
        return;
    }
    XrFrameBeginInfo fbi{XR_TYPE_FRAME_BEGIN_INFO};
    r = xrBeginFrame(g_session, &fbi);
    if (XR_FAILED(r)) {
        BVR_LOG("xr: xrBeginFrame failed: %s", res_str(r));
        teardown_session("beginframe failed");
        return;
    }
    g_frameOpen = true;
}

void on_present_end(IDXGISwapChain* swapchain) {
    if (!g_frameOpen) return;
    g_frameOpen = false;

    XrCompositionLayerQuad quad{XR_TYPE_COMPOSITION_LAYER_QUAD};
    const XrCompositionLayerBaseHeader* layers[] = {
        reinterpret_cast<const XrCompositionLayerBaseHeader*>(&quad)};
    uint32_t layerCount = 0;

    if (g_frameState.shouldRender && g_quadSwapchain != XR_NULL_HANDLE) {
        ID3D11Texture2D* backbuffer = nullptr;
        if (SUCCEEDED(swapchain->GetBuffer(0, IID_PPV_ARGS(&backbuffer)))) {
            uint32_t index = 0;
            XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
            if (XR_SUCCEEDED(xrAcquireSwapchainImage(g_quadSwapchain, &ai, &index))) {
                XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
                wi.timeout = XR_INFINITE_DURATION;
                if (XR_SUCCEEDED(xrWaitSwapchainImage(g_quadSwapchain, &wi))) {
                    // Same size + same typeless family (guaranteed at creation),
                    // so a straight GPU copy carries the frame - overlay included.
                    g_context->CopyResource(g_quadImages[index].texture, backbuffer);
                }
                XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
                xrReleaseSwapchainImage(g_quadSwapchain, &ri);

                float width = g_screenWidthM.load(std::memory_order_relaxed);
                quad.space = g_space;
                quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
                quad.subImage.swapchain = g_quadSwapchain;
                quad.subImage.imageRect = {{0, 0},
                                           {static_cast<int32_t>(g_quadW),
                                            static_cast<int32_t>(g_quadH)}};
                quad.pose.orientation.w = 1.0f;
                quad.pose.position = {0.0f, 0.0f,
                                      -g_screenDistM.load(std::memory_order_relaxed)};
                quad.size = {width, width * static_cast<float>(g_quadH) /
                                        static_cast<float>(g_quadW)};
                layerCount = 1;
            }
            backbuffer->Release();
        }
    }

    XrFrameEndInfo fei{XR_TYPE_FRAME_END_INFO};
    fei.displayTime = g_frameState.predictedDisplayTime;
    fei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    fei.layerCount = layerCount;
    fei.layers = layerCount ? layers : nullptr;
    XrResult r = xrEndFrame(g_session, &fei);
    if (XR_FAILED(r)) {
        BVR_LOG("xr: xrEndFrame failed: %s", res_str(r));
        teardown_session("endframe failed");
        return;
    }
    if (layerCount && ++g_framesSubmitted == 1)
        BVR_LOG("xr: first frame submitted to the headset (%ux%u quad)", g_quadW, g_quadH);
}

void on_resize() {
    // Recreated at the new backbuffer size on the next frame.
    destroy_quad_swapchain();
}

void draw_debug_ui() {
    if (g_instance == XR_NULL_HANDLE) {
        ImGui::Text("VR: no OpenXR runtime - flat mode");
        return;
    }
    if (g_session == XR_NULL_HANDLE) {
        ImGui::Text("VR: runtime '%s', no session (headset off?)", g_runtimeName);
    } else {
        ImGui::Text("VR: '%s' session %s, %u frames", g_runtimeName, state_str(g_state),
                    g_framesSubmitted);
    }

    bool enabled = g_enabled.load(std::memory_order_relaxed);
    if (ImGui::Checkbox("VR enabled (paces game to headset)", &enabled))
        g_enabled.store(enabled, std::memory_order_relaxed);

    float dist = g_screenDistM.load(std::memory_order_relaxed);
    if (ImGui::SliderFloat("Screen distance (m)", &dist, 0.5f, 5.0f))
        g_screenDistM.store(dist, std::memory_order_relaxed);
    float width = g_screenWidthM.load(std::memory_order_relaxed);
    if (ImGui::SliderFloat("Screen width (m)", &width, 0.5f, 6.0f))
        g_screenWidthM.store(width, std::memory_order_relaxed);
}

} // namespace bvr::vr

#else // !BVR_WITH_OPENXR

namespace bvr::vr {

void init_instance() {
    BVR_LOG("xr: built without OpenXR support - VR disabled");
}
void on_present_begin(IDXGISwapChain*) {}
void on_present_end(IDXGISwapChain*) {}
void on_resize() {}
void draw_debug_ui() {}

} // namespace bvr::vr

#endif
