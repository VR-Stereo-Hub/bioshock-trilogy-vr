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

#include "core/vr/openxr_input.h"

#include <imgui.h>

#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>
#include <vector>

namespace bvr::vr {
namespace {

XrInstance g_instance = XR_NULL_HANDLE;
char g_runtimeName[XR_MAX_RUNTIME_NAME_SIZE] = "none";
XrSystemId g_system = XR_NULL_SYSTEM_ID;
XrSession g_session = XR_NULL_HANDLE;
XrSpace g_space = XR_NULL_HANDLE;
XrSpace g_viewSpace = XR_NULL_HANDLE;
XrSessionState g_state = XR_SESSION_STATE_UNKNOWN;
std::atomic<bool> g_sessionBegun{false}; // read from the game thread via vr_camera_mode()
bool g_frameOpen = false;
XrFrameState g_frameState{XR_TYPE_FRAME_STATE};

// Two identical backbuffer-sized swapchains: index 0 serves the quad screen
// and mono projection (and the left eye under AlternateEye), index 1 exists
// only for the AlternateEye right eye. Both live and die together.
XrSwapchain g_swapchains[2] = {XR_NULL_HANDLE, XR_NULL_HANDLE};
std::vector<XrSwapchainImageD3D11KHR> g_images[2];
uint32_t g_swapW = 0, g_swapH = 0;

ID3D11Device* g_device = nullptr;          // game device, AddRef'd
ID3D11DeviceContext* g_context = nullptr;  // immediate context, AddRef'd

// Controls (overlay writes, render thread reads).
std::atomic<bool> g_enabled{true};        // kill switch: tears the session down
std::atomic<float> g_screenDistM{1.75f};  // quad distance in meters
std::atomic<float> g_screenWidthM{2.4f};  // quad width in meters
std::atomic<bool> g_cameraMode{false};    // M3: drive the game camera from the HMD

// Bring-up retry (render thread only).
uint64_t g_nextRetryMs = 0;
bool g_loggedNoHmd = false;
uint32_t g_framesSubmitted = 0;

// M3 pose plumbing. The head pose crosses to the game thread (mutex); the
// per-eye views are only used on the render thread for layer submission.
std::mutex g_poseMutex;
HeadPose g_headPose{};
bool g_poseValid = false;
XrView g_views[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
bool g_viewsValid = false;
std::atomic<float> g_hfovDeg{0.0f};      // circumscribed symmetric hfov, read cross-thread
std::atomic<float> g_renderedHfov{0.0f}; // fov the game actually rendered (adapter readback)
// Distortion calibration: the readback reads the same engine address we write,
// so under forcing it echoes our own value and cannot see how the RENDERER
// interprets it (vfov? 4:3-referenced? clamped downstream?). This manual
// override claims an arbitrary hfov instead; in-headset, the world stops
// warping on head rotation exactly when the claim matches what the engine
// truly rendered - the locked slider value measures the real fov.
std::atomic<bool> g_claimFovManual{false};
std::atomic<float> g_claimFovDeg{100.0f};
// True only when everything the projection layer needs is in place (views
// located, fov known, swapchain alive). The camera drive is gated on this so
// a head-driven camera can never appear on the flat quad screen.
std::atomic<bool> g_projectionReady{false};
int g_lastLayer = 0; // 0 none, 1 quad, 2 projection (render thread only)
std::atomic<bool> g_loggedFirstProjection{false};
std::atomic<bool> g_loggedFirstStereo{false};
uint64_t g_lastProjBlockedLogMs = 0;

// M4 rung 1: AlternateEye stereo (AER). The render thread owns everything here
// except g_aerEyeSign, which CalcView on the game thread reads through
// current_eye_sign() to pick the per-frame eye offset. The sign also encodes
// which offset is baked into the NEXT backbuffer we will see: it is published
// at Present-tail and consumed by the following game frame's CalcView.
std::atomic<bool> g_aerEnabled{false};   // overlay checkbox
std::atomic<bool> g_aerSwapEyes{false};  // diagnostic: negate the sign (inverted-depth test)
std::atomic<int> g_aerEyeSign{0};        // -1 left, +1 right, 0 = AER off
int g_currentEye = 0;                    // eye slot the next captured frame belongs to
XrPosef g_eyePose[2] = {};               // pose claimed for each eye's held image
bool g_eyeValid[2] = {false, false};     // eye slot holds a released image + pose

// M4 rung 2 (SequentialReentry): SPSC eye-tag ring, game thread pushes at
// engine submit, render thread pops at Present-tail (see header). Normal
// depth is <= 2 (one L/R pair in flight); deeper means a skew - the consumer
// clears it. Ring slots hold the eye sign; indices are monotonic.
constexpr uint32_t kSrRingSize = 8; // power of two
std::atomic<uint32_t> g_srHead{0};  // push cursor (game thread)
std::atomic<uint32_t> g_srTail{0};  // pop cursor (render thread)
std::atomic<int8_t> g_srRing[kSrRingSize] = {};
std::atomic<uint32_t> g_srPushed{0}, g_srPopped{0}, g_srDropped{0},
    g_srCleared{0};
std::atomic<bool> g_loggedFirstSr{false};

// M4 rung 2 polish (session 7, after the first in-headset stereo test):
// xr-frame-per-pair pacing. Per-present xrWaitFrame gave the two presents of
// one stereo pair SEPARATE predicted display times (~one compositor period
// apart), so the pair was submitted with poses located at two different
// times while both images were rendered from ONE head sample - under head
// motion the compositor reprojected the eyes inconsistently (user report:
// eyes feel weird on head movement), and the second blocking wait halved the
// game tick. With pair pacing, a LEFT-tagged present leaves the XR frame
// OPEN (captures its eye, no submit/end) and the RIGHT-tagged present
// completes it: one waitFrame, one locate, one consistent pose pair, one
// blocking wait per game tick. Kill switch in the overlay for live A/B.
std::atomic<bool> g_srPairPacing{true};
bool g_srPairOpen = false; // present thread only
std::atomic<uint32_t> g_srPairs{0}, g_srPairAborts{0};
std::atomic<bool> g_loggedFirstPair{false};

// Pop one tag; 0 = none pending (mono/AER frame).
int sr_pop_eye() {
    uint32_t tail = g_srTail.load(std::memory_order_relaxed);
    uint32_t head = g_srHead.load(std::memory_order_acquire);
    if (tail == head) return 0;
    if (head - tail > 2) {
        // More than a pair in flight: submit/present pairing skewed (mode
        // boundary). Drop everything and resync from mono.
        g_srTail.store(head, std::memory_order_relaxed);
        g_srCleared.fetch_add(1, std::memory_order_relaxed);
        BVR_LOG("xr: sr tag ring skewed (depth %u) - cleared", head - tail);
        return 0;
    }
    int sign = g_srRing[tail & (kSrRingSize - 1)].load(std::memory_order_relaxed);
    g_srTail.store(tail + 1, std::memory_order_release);
    g_srPopped.fetch_add(1, std::memory_order_relaxed);
    return sign;
}

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

void reset_aer() {
    g_eyeValid[0] = g_eyeValid[1] = false;
    g_currentEye = 0;
    g_aerEyeSign.store(0, std::memory_order_relaxed);
}

void destroy_swapchains() {
    for (int i = 0; i < 2; ++i) {
        if (g_swapchains[i] != XR_NULL_HANDLE) {
            xrDestroySwapchain(g_swapchains[i]);
            g_swapchains[i] = XR_NULL_HANDLE;
        }
        g_images[i].clear();
    }
    g_swapW = g_swapH = 0;
    reset_aer(); // the held eye images died with the swapchains
}

void teardown_session(const char* why) {
    BVR_LOG("xr: session teardown (%s)", why);
    input_on_session_teardown(); // action spaces are session children
    destroy_swapchains();
    {
        std::lock_guard<std::mutex> lock(g_poseMutex);
        g_poseValid = false;
    }
    g_viewsValid = false;
    g_projectionReady.store(false, std::memory_order_relaxed);
    g_hfovDeg.store(0.0f, std::memory_order_relaxed);
    if (g_viewSpace != XR_NULL_HANDLE) { xrDestroySpace(g_viewSpace); g_viewSpace = XR_NULL_HANDLE; }
    if (g_space != XR_NULL_HANDLE) { xrDestroySpace(g_space); g_space = XR_NULL_HANDLE; }
    if (g_session != XR_NULL_HANDLE) { xrDestroySession(g_session); g_session = XR_NULL_HANDLE; }
    if (g_context) { g_context->Release(); g_context = nullptr; }
    if (g_device) { g_device->Release(); g_device = nullptr; }
    g_sessionBegun = false;
    g_frameOpen = false;
    g_srPairOpen = false;
    g_system = XR_NULL_SYSTEM_ID;
    g_state = XR_SESSION_STATE_UNKNOWN;
    g_framesSubmitted = 0;
    g_nextRetryMs = GetTickCount64() + 5000; // cooldown before the next attempt
}

bool create_swapchains(IDXGISwapChain* swapchain) {
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
    uint32_t imageCount = 0;
    for (int i = 0; i < 2; ++i) {
        XrResult r = xrCreateSwapchain(g_session, &sci, &g_swapchains[i]);
        if (XR_FAILED(r)) {
            BVR_LOG("xr: xrCreateSwapchain failed: %s", res_str(r));
            g_swapchains[i] = XR_NULL_HANDLE;
            destroy_swapchains();
            return false;
        }
        xrEnumerateSwapchainImages(g_swapchains[i], 0, &imageCount, nullptr);
        g_images[i].assign(imageCount, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
        r = xrEnumerateSwapchainImages(
            g_swapchains[i], imageCount, &imageCount,
            reinterpret_cast<XrSwapchainImageBaseHeader*>(g_images[i].data()));
        if (XR_FAILED(r)) {
            BVR_LOG("xr: xrEnumerateSwapchainImages failed: %s", res_str(r));
            destroy_swapchains();
            return false;
        }
    }

    g_swapW = desc.BufferDesc.Width;
    g_swapH = desc.BufferDesc.Height;
    BVR_LOG("xr: swapchain pair %ux%u format %lld (%u images each)", g_swapW, g_swapH,
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
    rsci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW; // head pose via xrLocateSpace
    r = xrCreateReferenceSpace(g_session, &rsci, &g_viewSpace);
    if (XR_FAILED(r)) {
        BVR_LOG("xr: xrCreateReferenceSpace(VIEW) failed: %s", res_str(r));
        teardown_session("view space failed");
        return;
    }

    if (!create_swapchains(swapchain)) {
        teardown_session("swapchain failed");
        return;
    }

    // M5: grip action spaces + attach (once per session; attach failure only
    // costs controller input, never the display).
    input_on_session_created(g_session, g_space);

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

    input_create(g_instance); // M5: action set + touch bindings (fail-soft)
}

void on_present_begin(IDXGISwapChain* swapchain) {
    if (g_instance == XR_NULL_HANDLE) return;

    if (!g_enabled.load(std::memory_order_relaxed)) {
        if (g_session != XR_NULL_HANDLE) teardown_session("disabled in overlay");
        return;
    }

    // Pair pacing: the previous (LEFT-tagged) present left the XR frame open;
    // this present completes the pair at its tail. No second waitFrame, no
    // re-locate - the pair shares one prediction and one pose set.
    if (g_srPairOpen) return;

    if (g_session == XR_NULL_HANDLE) {
        if (GetTickCount64() < g_nextRetryMs) return;
        try_bring_up(swapchain);
        if (g_session == XR_NULL_HANDLE) return;
    }

    pump_events();
    if (g_session == XR_NULL_HANDLE || !g_sessionBegun) return;

    // A mid-session ResizeBuffers destroys the swapchains; recreate them at
    // the new backbuffer size (and recompute the fov, which depends on aspect).
    if (g_swapchains[0] == XR_NULL_HANDLE) {
        g_hfovDeg.store(0.0f, std::memory_order_relaxed);
        if (!create_swapchains(swapchain)) {
            g_projectionReady.store(false, std::memory_order_relaxed);
            return;
        }
    }

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

    // M3: locate the head pose + per-eye views for the predicted display time.
    // The head pose feeds the CalcView camera drive on the game thread; the
    // views feed projection-layer submission at Present-tail.
    XrSpaceLocation sl{XR_TYPE_SPACE_LOCATION};
    bool poseOk =
        XR_SUCCEEDED(xrLocateSpace(g_viewSpace, g_space, g_frameState.predictedDisplayTime, &sl)) &&
        (sl.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) &&
        (sl.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT);
    {
        std::lock_guard<std::mutex> lock(g_poseMutex);
        g_poseValid = poseOk;
        if (poseOk) {
            g_headPose = {sl.pose.position.x, sl.pose.position.y, sl.pose.position.z,
                          sl.pose.orientation.x, sl.pose.orientation.y,
                          sl.pose.orientation.z, sl.pose.orientation.w};
        }
    }

    XrViewLocateInfo vli{XR_TYPE_VIEW_LOCATE_INFO};
    vli.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    vli.displayTime = g_frameState.predictedDisplayTime;
    vli.space = g_space;
    XrViewState vs{XR_TYPE_VIEW_STATE};
    uint32_t viewCount = 0;
    g_views[0] = {XR_TYPE_VIEW};
    g_views[1] = {XR_TYPE_VIEW};
    g_viewsValid =
        XR_SUCCEEDED(xrLocateViews(g_session, &vli, &vs, 2, &viewCount, g_views)) &&
        viewCount == 2 && (vs.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT);

    // Circumscribed symmetric FOV for the game render, computed once per
    // session (needs the backbuffer aspect from the quad swapchain).
    if (g_viewsValid && g_hfovDeg.load(std::memory_order_relaxed) == 0.0f && g_swapW != 0) {
        float maxHalfH = 0.0f, maxHalfV = 0.0f;
        for (const XrView& v : g_views) {
            maxHalfH = fmaxf(maxHalfH, fmaxf(-v.fov.angleLeft, v.fov.angleRight));
            maxHalfV = fmaxf(maxHalfV, fmaxf(-v.fov.angleDown, v.fov.angleUp));
        }
        float aspect = static_cast<float>(g_swapW) / static_cast<float>(g_swapH);
        float halfH = fmaxf(maxHalfH, atanf(tanf(maxHalfV) * aspect));
        float deg = fminf(halfH * 2.0f * 57.29578f, 160.0f);
        g_hfovDeg.store(deg, std::memory_order_relaxed);
        BVR_LOG("xr: headset fov half-angles h=%.1f v=%.1f deg -> game hfov %.1f deg "
                "(aspect %.3f)",
                maxHalfH * 57.29578f, maxHalfV * 57.29578f, deg, aspect);
    }

    // M5: one action sync per XR frame (with pair pacing that is once per eye
    // pair == once per game tick). Composes and publishes the synthetic pad.
    input_sync(g_session, g_frameState.predictedDisplayTime);

    // Single readiness gate for projection mode (and, through vr_camera_mode,
    // for the camera drive): never let a head-driven camera show on the quad.
    bool ready = g_viewsValid && g_hfovDeg.load(std::memory_order_relaxed) > 0.0f &&
                 g_swapchains[0] != XR_NULL_HANDLE;
    g_projectionReady.store(ready, std::memory_order_relaxed);
    if (!ready && g_cameraMode.load(std::memory_order_relaxed)) {
        uint64_t now = GetTickCount64();
        if (now - g_lastProjBlockedLogMs > 5000) {
            g_lastProjBlockedLogMs = now;
            BVR_LOG("xr: camera mode requested but projection not ready "
                    "(views %d, hfov %.1f, swapchain %d)",
                    g_viewsValid ? 1 : 0, g_hfovDeg.load(std::memory_order_relaxed),
                    g_swapchains[0] != XR_NULL_HANDLE ? 1 : 0);
        }
    }
}

void on_present_end(IDXGISwapChain* swapchain) {
    if (!g_frameOpen) return;
    bool pairSecond = g_srPairOpen; // this present completes an open pair
    g_srPairOpen = false;
    g_frameOpen = false; // the pair-hold path below re-arms both

    XrCompositionLayerQuad quad{XR_TYPE_COMPOSITION_LAYER_QUAD};
    XrCompositionLayerProjection proj{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    XrCompositionLayerProjectionView projViews[2] = {
        {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW},
        {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}};
    const XrCompositionLayerBaseHeader* layers[1] = {};
    uint32_t layerCount = 0;

    // Claim the fov the game actually rendered with (adapter readback);
    // fall back to the circumscribed target before the first readback lands.
    // The manual calibration override beats both (see its declaration).
    float hfovDeg = g_renderedHfov.load(std::memory_order_relaxed);
    if (hfovDeg <= 0.0f) hfovDeg = g_hfovDeg.load(std::memory_order_relaxed);
    if (g_claimFovManual.load(std::memory_order_relaxed))
        hfovDeg = g_claimFovDeg.load(std::memory_order_relaxed);
    bool projectionMode = g_cameraMode.load(std::memory_order_relaxed) &&
                          g_projectionReady.load(std::memory_order_relaxed) && hfovDeg > 0.0f;

    // SequentialReentry (rung 2): one tag pop per Present, ALWAYS - the ring
    // must drain even in quad mode so a mode change cannot leave stale tags.
    // A tagged present carries a known eye (game thread pushed the sign at
    // this frame's engine submit); sign -1 = left = eye index 0, same
    // convention AER validated in-headset (depth not inverted).
    int srSign = sr_pop_eye();
    bool srFrame = projectionMode && srSign != 0;

    // Pair-pacing bookkeeping and the hold decision. A LEFT-tagged present
    // holds the frame open for its RIGHT sibling; anything unexpected on the
    // completing present (mode boundary, stereo toggled mid-pair) falls
    // through to the normal single-present submission and resyncs.
    if (pairSecond) {
        if (srSign == +1)
            g_srPairs.fetch_add(1, std::memory_order_relaxed);
        else
            g_srPairAborts.fetch_add(1, std::memory_order_relaxed);
    }
    bool pairHold = srFrame && srSign < 0 && !pairSecond &&
                    g_srPairPacing.load(std::memory_order_relaxed) &&
                    g_frameState.shouldRender && g_swapchains[0] != XR_NULL_HANDLE;

    // AlternateEye bookkeeping. imageSign is the eye offset baked into THIS
    // backbuffer: the sign was published at the tail of the previous Present
    // and consumed by the game frame that produced the current image. Only a
    // frame carrying the current eye's offset is captured into that eye's
    // swapchain; anything else (mono, or the un-offset frame right after
    // enabling) flows through index 0 like M3. SR frames bypass all of it.
    bool aerActive = projectionMode && g_aerEnabled.load(std::memory_order_relaxed);
    if (!aerActive && !srFrame) reset_aer();
    int imageSign = g_aerEyeSign.load(std::memory_order_relaxed);
    int eyeFlip = g_aerSwapEyes.load(std::memory_order_relaxed) ? -1 : 1;
    int currentEyeSign = (g_currentEye == 0 ? -1 : 1) * eyeFlip;
    bool eyeCaptured = false;

    if (g_frameState.shouldRender && g_swapchains[0] != XR_NULL_HANDLE) {
        ID3D11Texture2D* backbuffer = nullptr;
        if (SUCCEEDED(swapchain->GetBuffer(0, IID_PPV_ARGS(&backbuffer)))) {
            int srEye = srSign < 0 ? 0 : 1;
            int target = srFrame ? srEye
                         : (aerActive && imageSign == currentEyeSign) ? g_currentEye
                                                                      : 0;
            uint32_t index = 0;
            XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
            if (XR_SUCCEEDED(xrAcquireSwapchainImage(g_swapchains[target], &ai, &index))) {
                XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
                wi.timeout = XR_INFINITE_DURATION;
                if (XR_SUCCEEDED(xrWaitSwapchainImage(g_swapchains[target], &wi))) {
                    // Same size + same typeless family (guaranteed at creation),
                    // so a straight GPU copy carries the frame - overlay included.
                    g_context->CopyResource(g_images[target][index].texture, backbuffer);
                }
                XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
                xrReleaseSwapchainImage(g_swapchains[target], &ri);

                if (srFrame) {
                    g_eyePose[srEye] = g_views[srEye].pose;
                    g_eyeValid[srEye] = true;
                    if (!g_loggedFirstSr.exchange(true))
                        BVR_LOG("xr: first SequentialReentry eye frame captured "
                                "(eye %c)", srEye == 0 ? 'L' : 'R');
                } else if (aerActive && target == g_currentEye &&
                           imageSign == currentEyeSign) {
                    g_eyePose[g_currentEye] = g_views[g_currentEye].pose;
                    g_eyeValid[g_currentEye] = true;
                    eyeCaptured = true;
                }

                if (pairHold) {
                    // Left eye captured; submission happens when the RIGHT
                    // present completes this XR frame. Both eye poses come
                    // from this frame's single locate (g_views is untouched
                    // until the next waitFrame).
                    g_srPairOpen = true;
                    g_frameOpen = true;
                    if (!g_loggedFirstPair.exchange(true))
                        BVR_LOG("xr: pair pacing live (one waitFrame per eye "
                                "pair)");
                    backbuffer->Release();
                    return;
                }

                XrSwapchainSubImage sub{};
                sub.swapchain = g_swapchains[target];
                sub.imageRect = {{0, 0},
                                 {static_cast<int32_t>(g_swapW), static_cast<int32_t>(g_swapH)}};

                if (projectionMode) {
                    // fov = the symmetric fov the game rendered with (hfov
                    // written by the adapter, vfov via aspect).
                    float halfH = hfovDeg * 0.5f / 57.29578f;
                    float halfV = atanf(tanf(halfH) * static_cast<float>(g_swapH) /
                                        static_cast<float>(g_swapW));
                    // AER: each eye shows its swapchain's most recently
                    // released image with the pose stored at its capture (the
                    // compositor reprojects the stale eye). Until both eyes
                    // hold an offset image: M3 mono - fresh image to both eyes
                    // with the per-eye located poses. Converges in 2 frames.
                    bool stereo = (aerActive || srFrame) && g_eyeValid[0] &&
                                  g_eyeValid[1];
                    for (int eye = 0; eye < 2; ++eye) {
                        if (stereo) {
                            projViews[eye].pose = g_eyePose[eye];
                            projViews[eye].subImage = sub;
                            projViews[eye].subImage.swapchain = g_swapchains[eye];
                        } else {
                            projViews[eye].pose = g_views[eye].pose;
                            projViews[eye].subImage = sub;
                        }
                        projViews[eye].fov = {-halfH, halfH, halfV, -halfV};
                    }
                    proj.space = g_space;
                    proj.viewCount = 2;
                    proj.views = projViews;
                    layers[0] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&proj);
                    g_lastLayer = 2;
                    if (!g_loggedFirstProjection.exchange(true))
                        BVR_LOG("xr: first projection-layer frame (claimed hfov %.1f deg)",
                                hfovDeg);
                    if (stereo && !g_loggedFirstStereo.exchange(true))
                        BVR_LOG("xr: alternate-eye stereo live (both eyes hold offset images)");
                } else {
                    float width = g_screenWidthM.load(std::memory_order_relaxed);
                    quad.space = g_space;
                    quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
                    quad.subImage = sub;
                    quad.pose.orientation.w = 1.0f;
                    quad.pose.position = {0.0f, 0.0f,
                                          -g_screenDistM.load(std::memory_order_relaxed)};
                    quad.size = {width, width * static_cast<float>(g_swapH) /
                                            static_cast<float>(g_swapW)};
                    layers[0] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&quad);
                    g_lastLayer = 1;
                }
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
        BVR_LOG("xr: first frame submitted to the headset (%ux%u quad)", g_swapW, g_swapH);

    // AER sign publish, AFTER submit: eye flip only once an offset frame was
    // actually captured, so CalcView for the next game frame simulates exactly
    // the eye the next Present's copy will feed (design: STATUS.md session 2).
    if (aerActive) {
        if (eyeCaptured) g_currentEye ^= 1;
        g_aerEyeSign.store((g_currentEye == 0 ? -1 : 1) * eyeFlip,
                           std::memory_order_relaxed);
    }
}

void on_resize() {
    // Recreated at the new backbuffer size on the next frame.
    destroy_swapchains();
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

    bool camMode = g_cameraMode.load(std::memory_order_relaxed);
    if (ImGui::Checkbox("VR camera mode (6DOF head drive)", &camMode))
        g_cameraMode.store(camMode, std::memory_order_relaxed);
    if (camMode) {
        bool aer = g_aerEnabled.load(std::memory_order_relaxed);
        if (ImGui::Checkbox("AlternateEye stereo test (judders)", &aer))
            g_aerEnabled.store(aer, std::memory_order_relaxed);
        bool pair = g_srPairPacing.load(std::memory_order_relaxed);
        if (ImGui::Checkbox("SR pair pacing (one waitFrame per eye pair)", &pair))
            g_srPairPacing.store(pair, std::memory_order_relaxed);
        if (aer) {
            ImGui::SameLine();
            bool swap = g_aerSwapEyes.load(std::memory_order_relaxed);
            if (ImGui::Checkbox("Swap eyes (inverted-depth test)", &swap))
                g_aerSwapEyes.store(swap, std::memory_order_relaxed);
        }
        bool manualFov = g_claimFovManual.load(std::memory_order_relaxed);
        if (ImGui::Checkbox("Manual claimed FOV (distortion calibration)", &manualFov)) {
            g_claimFovManual.store(manualFov, std::memory_order_relaxed);
            if (manualFov) {
                // Snap the slider to the current effective claim as a start point.
                float cur = g_renderedHfov.load(std::memory_order_relaxed);
                if (cur <= 0.0f) cur = g_hfovDeg.load(std::memory_order_relaxed);
                if (cur > 0.0f) g_claimFovDeg.store(cur, std::memory_order_relaxed);
            }
        }
        if (manualFov) {
            float v = g_claimFovDeg.load(std::memory_order_relaxed);
            if (ImGui::SliderFloat("Claimed hfov (deg) - stop the swim", &v, 40.0f, 160.0f))
                g_claimFovDeg.store(v, std::memory_order_relaxed);
        }
    }
    const char* layerName = g_lastLayer == 2 ? "projection" : g_lastLayer == 1 ? "quad" : "none";
    int eyeSign = g_aerEyeSign.load(std::memory_order_relaxed);
    float readback = g_renderedHfov.load(std::memory_order_relaxed);
    float claimed = g_claimFovManual.load(std::memory_order_relaxed)
                        ? g_claimFovDeg.load(std::memory_order_relaxed)
                        : (readback > 0.0f ? readback
                                           : g_hfovDeg.load(std::memory_order_relaxed));
    ImGui::Text("layer: %s%s | target %.1f | readback %.1f | claimed %.1f",
                layerName, eyeSign == 0 ? "" : eyeSign < 0 ? " (AER eye L)" : " (AER eye R)",
                g_hfovDeg.load(std::memory_order_relaxed), readback, claimed);
    uint32_t srPushed = g_srPushed.load(std::memory_order_relaxed);
    if (srPushed)
        ImGui::Text("SR tags: pushed %u popped %u dropped %u cleared %u  eyes %d/%d"
                    "  pairs %u aborts %u",
                    srPushed, g_srPopped.load(std::memory_order_relaxed),
                    g_srDropped.load(std::memory_order_relaxed),
                    g_srCleared.load(std::memory_order_relaxed),
                    g_eyeValid[0] ? 1 : 0, g_eyeValid[1] ? 1 : 0,
                    g_srPairs.load(std::memory_order_relaxed),
                    g_srPairAborts.load(std::memory_order_relaxed));
    if (camMode && !g_projectionReady.load(std::memory_order_relaxed))
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f),
                           "projection NOT ready - drive is held off (see log)");

    input_draw_debug_ui(); // M5 action-layer status line

    if (!camMode) {
        float dist = g_screenDistM.load(std::memory_order_relaxed);
        if (ImGui::SliderFloat("Screen distance (m)", &dist, 0.5f, 5.0f))
            g_screenDistM.store(dist, std::memory_order_relaxed);
        float width = g_screenWidthM.load(std::memory_order_relaxed);
        if (ImGui::SliderFloat("Screen width (m)", &width, 0.5f, 6.0f))
            g_screenWidthM.store(width, std::memory_order_relaxed);
    }
}

bool get_head_pose(HeadPose& out) {
    std::lock_guard<std::mutex> lock(g_poseMutex);
    if (!g_poseValid) return false;
    out = g_headPose;
    return true;
}

bool get_hand_pose(int hand, bool aimPose, HeadPose& out) {
    float p[3], q[4];
    if (!input_get_hand_pose(hand, aimPose, p, q)) return false;
    out = {p[0], p[1], p[2], q[0], q[1], q[2], q[3]};
    return true;
}

bool vr_camera_mode() {
    return g_cameraMode.load(std::memory_order_relaxed) &&
           g_sessionBegun.load(std::memory_order_relaxed) &&
           g_projectionReady.load(std::memory_order_relaxed);
}

void set_camera_mode(bool on) {
    bool was = g_cameraMode.exchange(on, std::memory_order_relaxed);
    if (was != on)
        BVR_LOG("xr: camera mode %s (request; drive engages when the session "
                "and projection are ready)",
                on ? "ON" : "off");
}

float suggested_hfov_deg() {
    return g_hfovDeg.load(std::memory_order_relaxed);
}

void set_rendered_hfov(float hfovDeg) {
    g_renderedHfov.store(hfovDeg, std::memory_order_relaxed);
}

int current_eye_sign() {
    return g_aerEyeSign.load(std::memory_order_relaxed);
}

void sr_push_eye(int eyeSign) {
    uint32_t head = g_srHead.load(std::memory_order_relaxed);
    uint32_t tail = g_srTail.load(std::memory_order_acquire);
    if (head - tail >= kSrRingSize) {
        // No consumer (no XR session) or consumer stalled: drop, count it.
        g_srDropped.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    g_srRing[head & (kSrRingSize - 1)].store(static_cast<int8_t>(eyeSign),
                                             std::memory_order_relaxed);
    g_srHead.store(head + 1, std::memory_order_release);
    g_srPushed.fetch_add(1, std::memory_order_relaxed);
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
bool get_head_pose(HeadPose&) { return false; }
bool get_hand_pose(int, bool, HeadPose&) { return false; }
bool vr_camera_mode() { return false; }
void set_camera_mode(bool) {}
float suggested_hfov_deg() { return 0.0f; }
void set_rendered_hfov(float) {}
int current_eye_sign() { return 0; }
void sr_push_eye(int) {}

} // namespace bvr::vr

#endif
