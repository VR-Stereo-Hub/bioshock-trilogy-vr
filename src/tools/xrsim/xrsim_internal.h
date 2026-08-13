// bvr_xrsim32 internals: the global state singleton, the object tables, and the
// prototypes of every entry point that lives outside xrsim_instance.cpp.
//
// Split from xrsim_common.h so that header stays the small "what a reader needs
// first" surface while this one carries the machinery.

#pragma once

#include "xrsim_common.h"

#include <condition_variable>

namespace xrsim {

// One fixed system id. There is exactly one simulated headset.
constexpr XrSystemId kSystemId = 1;

// ---------------------------------------------------------------------------
// Object tables
// ---------------------------------------------------------------------------

struct SimSpace {
    bool used = false;
    uint32_t gen = 0;
    bool isAction = false;
    XrReferenceSpaceType refType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    Pose offset = pose_identity();
    uint32_t actionIndex = 0;   // action spaces only
    int hand = -1;              // resolved from the subaction path, -1 = none
};

struct SimSwapchain {
    bool used = false;
    uint32_t gen = 0;
    uint32_t width = 0, height = 0;
    int64_t format = 0;
    uint32_t imageCount = 0;
    ID3D11Texture2D* images[kMaxSwapchainImages] = {};
    bool acquired = false;
    uint32_t acquiredIndex = 0;
    uint32_t nextIndex = 0;
    uint32_t lastReleased = 0;
    bool everReleased = false;
    uint32_t acquiresThisFrame = 0;
    uint32_t releasedOnFrame = 0;
    uint32_t allZeroStreak = 0;
    bool allZero = false;
};

struct SimActionSet {
    bool used = false;
    uint32_t gen = 0;
    char name[XR_MAX_ACTION_SET_NAME_SIZE] = {};
    bool attached = false;
};

struct SimAction {
    bool used = false;
    uint32_t gen = 0;
    uint32_t setIndex = 0;
    XrActionType type = XR_ACTION_TYPE_BOOLEAN_INPUT;
    char name[XR_MAX_ACTION_NAME_SIZE] = {};
    VirtualControl control = VC_NONE;  // resolved at suggest-binding time
    int hand = -1;
};

// ---------------------------------------------------------------------------
// The frame snapshot
// ---------------------------------------------------------------------------
// Everything a frame was built from, published once inside xrWaitFrame and read
// unchanged by xrLocateSpace / xrLocateViews / xrSyncActions / xrEndFrame. One
// snapshot per frame is what makes a capture correspond to a single consistent
// rig rather than to whatever the control thread happened to be doing.
struct FrameSnapshot {
    uint64_t index = 0;
    XrTime displayTime = 0;
    XrDuration displayPeriod = 0;
    Rig rig{};
    Pose localOrigin = pose_identity();  // moved by `recenter`
    Pose headWorld = pose_identity();
    Pose gripWorld[2] = {pose_identity(), pose_identity()};
    Pose aimWorld[2] = {pose_identity(), pose_identity()};
    XrSessionState state = XR_SESSION_STATE_UNKNOWN;
};

// ---------------------------------------------------------------------------
// Globals
// ---------------------------------------------------------------------------

struct Globals {
    // Identity. Overridable by command so a log-parity test can make the sim
    // claim to be VirtualDesktopXR; the launcher's assertion uses the default.
    char runtimeName[XR_MAX_RUNTIME_NAME_SIZE] = "bvr-xrsim";
    char systemName[XR_MAX_SYSTEM_NAME_SIZE] = "Meta Quest 3";

    bool instanceAlive = false;
    uint32_t instanceGen = 0;
    bool d3d11Enabled = false;

    // Quest 3 native per-eye panel. Cosmetic for the mod, which sizes its
    // swapchains from the game backbuffer and never enumerates these.
    uint32_t recommendedWidth = 2064;
    uint32_t recommendedHeight = 2208;

    Hazards hazards{};
    Pacing pacing{PaceMode::Free, 90.0, 0, kStepStarveMsDefault, true, 0, kIdleMaxMsDefault};
    FocusPolicy focusPolicy = FocusPolicy::Vdxr;
    uint32_t focusFrames = 3;
    // Session 54, the VDXR park model. norender: while the session is not
    // FOCUSED, xrWaitFrame reports shouldRender=FALSE (measured: VDXR holds it
    // 0 for the whole VISIBLE episode). throttle: while not FOCUSED, xrEndFrame
    // blocks this many ms (measured: ~87 ms per call once VD's compositor is
    // busy elsewhere - the mechanism that paced the game to ~10 presents/s).
    bool focusNoRender = false;
    uint32_t focusThrottleMs = 0;

    std::atomic<uint32_t> hapticPulses{0};
    std::atomic<uint32_t> stateWriteHz{20};

    // Capture request, consumed by xrEndFrame.
    std::atomic<uint32_t> captureCountdown{0};
    std::atomic<uint32_t> captureEvery{0};
    std::atomic<uint32_t> captureSeq{0};
    std::atomic<bool> captureLayers{true};
    std::atomic<bool> composeAlways{false};
    std::atomic<uint32_t> captureWidth{1032};
    std::atomic<uint32_t> captureHeight{1104};
    char captureTag[64] = {};

    // Command bookkeeping, surfaced in state.json as the ack channel.
    std::atomic<uint32_t> cmdSeq{0};
    char lastCmd[256] = {};
    char lastCmdError[256] = {};
    std::atomic<uint32_t> errors{0};
    char lastCapturePath[MAX_PATH] = {};
};

extern Globals g;

// ---------------------------------------------------------------------------
// Cross-module helpers
// ---------------------------------------------------------------------------

bool valid_instance(XrInstance h);
XrPath path_intern(const char* str);
const char* path_str(XrPath p);
uint32_t events_dropped();

void session_destroy_all();
SimSpace* space_get(XrSpace h);
SimSwapchain* swapchain_get(XrSwapchain h);
SimAction* action_get(XrAction h);
SimAction* action_get_by_index(uint32_t index);
bool session_valid(XrSession h);
XrSession current_session_handle();
void session_focus_lose(uint32_t holdMs);
bool session_should_render();       // false while norender is armed and not FOCUSED
uint64_t session_layered_frames();  // layer-carrying submissions (VdxrLayers policy)

// Swapchain census + image access, used by the compositor and state.json.
void swapchains_begin_frame_census();
void swapchains_for_each(void (*fn)(uint32_t, const SimSwapchain&, void*), void* user);
ID3D11Texture2D* swapchain_last_image(XrSwapchain handle, uint32_t* outW, uint32_t* outH);

// Resolve a space to a world pose at a given time, using the published snapshot.
bool space_pose(const SimSpace& space, const FrameSnapshot& snap, Pose& out, bool& tracked);

// The published snapshot (xrsim_frame.cpp).
const FrameSnapshot& snapshot();
void snapshot_copy(FrameSnapshot& out);
void snapshot_set_rig(const Rig& rig);
void recenter_local_space();
void actions_set_profile(bool simple);

// Frame gate counters, for state.json and for asserting the mod's 1:1 discipline.
struct FrameGate {
    std::atomic<uint64_t> waited{0};
    std::atomic<uint64_t> begun{0};
    std::atomic<uint64_t> ended{0};
    std::atomic<bool> open{false};
    std::atomic<uint32_t> discarded{0};
    std::atomic<uint32_t> outOfOrder{0};
};
extern FrameGate g_gate;

// The control channel (xrsim_control.cpp).
void control_start();
void control_stop();
void control_apply_pending();          // called inside xrWaitFrame, the commit point
void control_write_state();
void rig_staging_init();

// Actions (xrsim_actions.cpp).
void actions_reset_session();
bool actions_attached();

// Session/pacing interplay (xrsim_session.cpp).
void session_note_submitted_frame(bool layered);
void session_force_state(XrSessionState state);
void session_pump_state();

// Pacing control (xrsim_frame.cpp).
void pacing_grant(uint32_t frames);
void pacing_wake();                    // config changed - re-evaluate, no error
void pacing_abort();                   // the session is going away - unblock with an error
void pacing_reset_for_new_session();

// Compositor + capture (xrsim_compositor.cpp).
struct SimLayer {
    XrStructureType type = XR_TYPE_UNKNOWN;
    XrSpace space = XR_NULL_HANDLE;
    XrCompositionLayerFlags flags = 0;
    XrEyeVisibility eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    uint32_t viewCount = 0;
    XrCompositionLayerProjectionView views[2] = {};
    XrPosef pose{};
    XrExtent2Df size{};
    XrSwapchainSubImage sub{};
};

struct SimSubmission {
    uint64_t frameIndex = 0;
    XrTime displayTime = 0;
    XrEnvironmentBlendMode blend = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    uint32_t layerCount = 0;
    SimLayer layers[kMaxLayers];
    FrameSnapshot snap;
};

void compositor_init(ID3D11Device* device, ID3D11DeviceContext* context);
void compositor_shutdown();
void compositor_on_end_frame(const SimSubmission& sub, bool capture);
void compositor_note_layers(const SimSubmission& sub);
uint32_t compositor_last_layer_count();
uint32_t compositor_last_projection_views();

// ---------------------------------------------------------------------------
// Entry points implemented outside xrsim_instance.cpp
// ---------------------------------------------------------------------------

XRAPI_ATTR XrResult XRAPI_CALL xrsim_CreateSession(XrInstance, const XrSessionCreateInfo*, XrSession*);
XRAPI_ATTR XrResult XRAPI_CALL xrsim_DestroySession(XrSession);
XRAPI_ATTR XrResult XRAPI_CALL xrsim_BeginSession(XrSession, const XrSessionBeginInfo*);
XRAPI_ATTR XrResult XRAPI_CALL xrsim_EndSession(XrSession);
XRAPI_ATTR XrResult XRAPI_CALL xrsim_EnumerateReferenceSpaces(XrSession, uint32_t, uint32_t*,
                                                              XrReferenceSpaceType*);
XRAPI_ATTR XrResult XRAPI_CALL xrsim_CreateReferenceSpace(XrSession,
                                                          const XrReferenceSpaceCreateInfo*,
                                                          XrSpace*);
XRAPI_ATTR XrResult XRAPI_CALL xrsim_CreateActionSpace(XrSession, const XrActionSpaceCreateInfo*,
                                                       XrSpace*);
XRAPI_ATTR XrResult XRAPI_CALL xrsim_LocateSpace(XrSpace, XrSpace, XrTime, XrSpaceLocation*);
XRAPI_ATTR XrResult XRAPI_CALL xrsim_DestroySpace(XrSpace);

XRAPI_ATTR XrResult XRAPI_CALL xrsim_EnumerateSwapchainFormats(XrSession, uint32_t, uint32_t*,
                                                               int64_t*);
XRAPI_ATTR XrResult XRAPI_CALL xrsim_CreateSwapchain(XrSession, const XrSwapchainCreateInfo*,
                                                     XrSwapchain*);
XRAPI_ATTR XrResult XRAPI_CALL xrsim_DestroySwapchain(XrSwapchain);
XRAPI_ATTR XrResult XRAPI_CALL xrsim_EnumerateSwapchainImages(XrSwapchain, uint32_t, uint32_t*,
                                                              XrSwapchainImageBaseHeader*);
XRAPI_ATTR XrResult XRAPI_CALL xrsim_AcquireSwapchainImage(XrSwapchain,
                                                           const XrSwapchainImageAcquireInfo*,
                                                           uint32_t*);
XRAPI_ATTR XrResult XRAPI_CALL xrsim_WaitSwapchainImage(XrSwapchain,
                                                        const XrSwapchainImageWaitInfo*);
XRAPI_ATTR XrResult XRAPI_CALL xrsim_ReleaseSwapchainImage(XrSwapchain,
                                                           const XrSwapchainImageReleaseInfo*);

XRAPI_ATTR XrResult XRAPI_CALL xrsim_WaitFrame(XrSession, const XrFrameWaitInfo*, XrFrameState*);
XRAPI_ATTR XrResult XRAPI_CALL xrsim_BeginFrame(XrSession, const XrFrameBeginInfo*);
XRAPI_ATTR XrResult XRAPI_CALL xrsim_EndFrame(XrSession, const XrFrameEndInfo*);
XRAPI_ATTR XrResult XRAPI_CALL xrsim_LocateViews(XrSession, const XrViewLocateInfo*,
                                                 XrViewState*, uint32_t, uint32_t*, XrView*);

XRAPI_ATTR XrResult XRAPI_CALL xrsim_CreateActionSet(XrInstance, const XrActionSetCreateInfo*,
                                                     XrActionSet*);
XRAPI_ATTR XrResult XRAPI_CALL xrsim_DestroyActionSet(XrActionSet);
XRAPI_ATTR XrResult XRAPI_CALL xrsim_CreateAction(XrActionSet, const XrActionCreateInfo*, XrAction*);
XRAPI_ATTR XrResult XRAPI_CALL xrsim_DestroyAction(XrAction);
XRAPI_ATTR XrResult XRAPI_CALL xrsim_SuggestInteractionProfileBindings(
    XrInstance, const XrInteractionProfileSuggestedBinding*);
XRAPI_ATTR XrResult XRAPI_CALL xrsim_AttachSessionActionSets(XrSession,
                                                             const XrSessionActionSetsAttachInfo*);
XRAPI_ATTR XrResult XRAPI_CALL xrsim_GetCurrentInteractionProfile(XrSession, XrPath,
                                                                  XrInteractionProfileState*);
XRAPI_ATTR XrResult XRAPI_CALL xrsim_GetActionStateBoolean(XrSession, const XrActionStateGetInfo*,
                                                           XrActionStateBoolean*);
XRAPI_ATTR XrResult XRAPI_CALL xrsim_GetActionStateFloat(XrSession, const XrActionStateGetInfo*,
                                                         XrActionStateFloat*);
XRAPI_ATTR XrResult XRAPI_CALL xrsim_GetActionStateVector2f(XrSession, const XrActionStateGetInfo*,
                                                            XrActionStateVector2f*);
XRAPI_ATTR XrResult XRAPI_CALL xrsim_GetActionStatePose(XrSession, const XrActionStateGetInfo*,
                                                        XrActionStatePose*);
XRAPI_ATTR XrResult XRAPI_CALL xrsim_SyncActions(XrSession, const XrActionsSyncInfo*);

XRAPI_ATTR XrResult XRAPI_CALL xrsim_GetReferenceSpaceBoundsRect(XrSession, XrReferenceSpaceType,
                                                                 XrExtent2Df*);
XRAPI_ATTR XrResult XRAPI_CALL xrsim_RequestExitSession(XrSession);
XRAPI_ATTR XrResult XRAPI_CALL xrsim_EnumerateBoundSourcesForAction(
    XrSession, const XrBoundSourcesForActionEnumerateInfo*, uint32_t, uint32_t*, XrPath*);
XRAPI_ATTR XrResult XRAPI_CALL xrsim_GetInputSourceLocalizedName(
    XrSession, const XrInputSourceLocalizedNameGetInfo*, uint32_t, uint32_t*, char*);
XRAPI_ATTR XrResult XRAPI_CALL xrsim_ApplyHapticFeedback(XrSession, const XrHapticActionInfo*,
                                                         const XrHapticBaseHeader*);
XRAPI_ATTR XrResult XRAPI_CALL xrsim_StopHapticFeedback(XrSession, const XrHapticActionInfo*);
XRAPI_ATTR XrResult XRAPI_CALL xrsim_LocateSpaces(XrSession, const XrSpacesLocateInfo*,
                                                  XrSpaceLocations*);
XRAPI_ATTR XrResult XRAPI_CALL xrsim_SetDebugUtilsObjectNameEXT(XrInstance,
                                                                const XrDebugUtilsObjectNameInfoEXT*);
XRAPI_ATTR XrResult XRAPI_CALL xrsim_CreateDebugUtilsMessengerEXT(
    XrInstance, const XrDebugUtilsMessengerCreateInfoEXT*, XrDebugUtilsMessengerEXT*);
XRAPI_ATTR XrResult XRAPI_CALL xrsim_DestroyDebugUtilsMessengerEXT(XrDebugUtilsMessengerEXT);
XRAPI_ATTR XrResult XRAPI_CALL xrsim_SubmitDebugUtilsMessageEXT(
    XrInstance, XrDebugUtilsMessageSeverityFlagsEXT, XrDebugUtilsMessageTypeFlagsEXT,
    const XrDebugUtilsMessengerCallbackDataEXT*);
XRAPI_ATTR XrResult XRAPI_CALL xrsim_SessionBeginDebugUtilsLabelRegionEXT(
    XrSession, const XrDebugUtilsLabelEXT*);
XRAPI_ATTR XrResult XRAPI_CALL xrsim_SessionEndDebugUtilsLabelRegionEXT(XrSession);
XRAPI_ATTR XrResult XRAPI_CALL xrsim_SessionInsertDebugUtilsLabelEXT(XrSession,
                                                                     const XrDebugUtilsLabelEXT*);

XRAPI_ATTR XrResult XRAPI_CALL xrsim_GetInstanceProcAddr(XrInstance, const char*,
                                                         PFN_xrVoidFunction*);

} // namespace xrsim
