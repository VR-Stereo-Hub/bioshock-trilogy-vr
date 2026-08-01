// bvr_xrsim32: action sets, actions, interaction profiles, and action state.
//
// The binding table below is the sim's contract with core/vr/openxr_input.cpp.
// It carries the whole oculus/touch_controller component list, not only the 19
// paths the mod binds today, so that a binding the mod ADDS later is validated
// rather than silently ignored - an unknown path returns PATH_UNSUPPORTED and
// logs the exact string, which is the sim catching a typo a real runtime would
// swallow.

#include "xrsim_internal.h"

#include <cstring>

namespace xrsim {
namespace {

std::mutex g_mutex;
SimActionSet g_sets[kMaxActionSets];
SimAction g_actions[kMaxActions];
bool g_attached = false;
XrPath g_profileTouch = XR_NULL_PATH;
XrPath g_profileSimple = XR_NULL_PATH;
bool g_useSimpleProfile = false;
uint64_t g_lastSyncFrame = 0;

struct BindingEntry {
    const char* path;
    VirtualControl control;  // VC_NONE = a real profile path we accept but do not drive
    int hand;
};

// The full oculus/touch_controller component set. Order does not matter; this is
// a lookup table, not a priority list.
const BindingEntry kTouch[] = {
    {"/user/hand/left/input/thumbstick", VC_STICK_L, 0},
    {"/user/hand/right/input/thumbstick", VC_STICK_R, 1},
    {"/user/hand/left/input/trigger/value", VC_TRIGGER_L, 0},
    {"/user/hand/right/input/trigger/value", VC_TRIGGER_R, 1},
    {"/user/hand/left/input/squeeze/value", VC_SQUEEZE_L, 0},
    {"/user/hand/right/input/squeeze/value", VC_SQUEEZE_R, 1},
    {"/user/hand/right/input/a/click", VC_BTN_A, 1},
    {"/user/hand/right/input/b/click", VC_BTN_B, 1},
    {"/user/hand/left/input/x/click", VC_BTN_X, 0},
    {"/user/hand/left/input/y/click", VC_BTN_Y, 0},
    {"/user/hand/left/input/menu/click", VC_MENU, 0},
    {"/user/hand/left/input/thumbstick/click", VC_CLICK_L, 0},
    {"/user/hand/right/input/thumbstick/click", VC_CLICK_R, 1},
    {"/user/hand/left/input/thumbrest/touch", VC_REST_L, 0},
    {"/user/hand/right/input/thumbrest/touch", VC_REST_R, 1},
    {"/user/hand/left/input/grip/pose", VC_POSE_GRIP_L, 0},
    {"/user/hand/right/input/grip/pose", VC_POSE_GRIP_R, 1},
    {"/user/hand/left/input/aim/pose", VC_POSE_AIM_L, 0},
    {"/user/hand/right/input/aim/pose", VC_POSE_AIM_R, 1},
    // Accepted, not driven. Present so the profile is honest about what a real
    // Touch controller exposes.
    {"/user/hand/left/input/thumbstick/x", VC_NONE, 0},
    {"/user/hand/left/input/thumbstick/y", VC_NONE, 0},
    {"/user/hand/right/input/thumbstick/x", VC_NONE, 1},
    {"/user/hand/right/input/thumbstick/y", VC_NONE, 1},
    {"/user/hand/left/input/thumbstick/touch", VC_NONE, 0},
    {"/user/hand/right/input/thumbstick/touch", VC_NONE, 1},
    {"/user/hand/left/input/trigger/touch", VC_NONE, 0},
    {"/user/hand/right/input/trigger/touch", VC_NONE, 1},
    {"/user/hand/right/input/a/touch", VC_NONE, 1},
    {"/user/hand/right/input/b/touch", VC_NONE, 1},
    {"/user/hand/left/input/x/touch", VC_NONE, 0},
    {"/user/hand/left/input/y/touch", VC_NONE, 0},
    {"/user/hand/left/input/system/click", VC_NONE, 0},
    {"/user/hand/left/output/haptic", VC_NONE, 0},
    {"/user/hand/right/output/haptic", VC_NONE, 1},
};

// khr/simple_controller: the fallback the mod suggests best-effort, so the
// "unknown runtime" path stays exercisable.
const BindingEntry kSimple[] = {
    {"/user/hand/left/input/select/click", VC_BTN_X, 0},
    {"/user/hand/right/input/select/click", VC_BTN_A, 1},
    {"/user/hand/left/input/menu/click", VC_MENU, 0},
    {"/user/hand/right/input/menu/click", VC_MENU, 1},
    {"/user/hand/left/input/grip/pose", VC_POSE_GRIP_L, 0},
    {"/user/hand/right/input/grip/pose", VC_POSE_GRIP_R, 1},
    {"/user/hand/left/input/aim/pose", VC_POSE_AIM_L, 0},
    {"/user/hand/right/input/aim/pose", VC_POSE_AIM_R, 1},
    {"/user/hand/left/output/haptic", VC_NONE, 0},
    {"/user/hand/right/output/haptic", VC_NONE, 1},
};

const BindingEntry* find_binding(const BindingEntry* table, size_t n, const char* path) {
    for (size_t i = 0; i < n; ++i)
        if (strcmp(table[i].path, path) == 0) return &table[i];
    return nullptr;
}

bool control_is_pose(VirtualControl c) {
    return c == VC_POSE_GRIP_L || c == VC_POSE_GRIP_R || c == VC_POSE_AIM_L || c == VC_POSE_AIM_R;
}

// Read a control out of the committed rig. One place, so xrGetActionState* is a
// table read and cannot disagree with what the capture JSON reports.
float control_float(const Rig& rig, VirtualControl c) {
    switch (c) {
    case VC_TRIGGER_L: return rig.trigger[0];
    case VC_TRIGGER_R: return rig.trigger[1];
    case VC_SQUEEZE_L: return rig.squeeze[0];
    case VC_SQUEEZE_R: return rig.squeeze[1];
    default: return 0.0f;
    }
}

bool control_bool(const Rig& rig, VirtualControl c) {
    switch (c) {
    case VC_BTN_A: return rig.btnA;
    case VC_BTN_B: return rig.btnB;
    case VC_BTN_X: return rig.btnX;
    case VC_BTN_Y: return rig.btnY;
    case VC_MENU: return rig.menu;
    case VC_CLICK_L: return rig.click[0];
    case VC_CLICK_R: return rig.click[1];
    case VC_REST_L: return rig.rest[0];
    case VC_REST_R: return rig.rest[1];
    default: return false;
    }
}

} // namespace

SimAction* action_get(XrAction h) {
    if (h == XR_NULL_HANDLE || handle_type(h) != HT_ACTION) return nullptr;
    const uint32_t i = handle_index(h);
    if (i >= kMaxActions) return nullptr;
    SimAction& a = g_actions[i];
    if (!a.used || a.gen != handle_gen(h)) return nullptr;
    return &a;
}

SimAction* action_get_by_index(uint32_t index) {
    if (index >= kMaxActions) return nullptr;
    SimAction& a = g_actions[index];
    return a.used ? &a : nullptr;
}

bool actions_attached() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_attached;
}

void actions_reset_session() {
    std::lock_guard<std::mutex> lock(g_mutex);
    // Action sets survive a session (they belong to the instance), so only the
    // attachment is cleared. The mod re-attaches the SAME set to a new session
    // after a teardown and re-bring-up, and that must be accepted.
    g_attached = false;
    for (auto& s : g_sets)
        if (s.used) s.attached = false;
}

void actions_set_profile(bool simple) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_useSimpleProfile = simple;
}

// ---------------------------------------------------------------------------
// Creation
// ---------------------------------------------------------------------------

static XrResult impl_CreateActionSet(XrInstance instance, const XrActionSetCreateInfo* info,
                                     XrActionSet* out) noexcept {
    if (!valid_instance(instance)) return XR_ERROR_HANDLE_INVALID;
    if (!info || !out) return XR_ERROR_VALIDATION_FAILURE;
    std::lock_guard<std::mutex> lock(g_mutex);
    for (uint32_t i = 0; i < kMaxActionSets; ++i) {
        if (g_sets[i].used) continue;
        const uint32_t gen = g_sets[i].gen + 1;
        g_sets[i] = SimActionSet{};
        g_sets[i].used = true;
        g_sets[i].gen = gen;
        strcpy_s(g_sets[i].name, info->actionSetName);
        *out = static_cast<XrActionSet>(make_handle(HT_ACTIONSET, i, gen));
        XRSIM_LOG("xrsim: action set '%s' created", info->actionSetName);
        return XR_SUCCESS;
    }
    return XR_ERROR_LIMIT_REACHED;
}

static XrResult impl_DestroyActionSet(XrActionSet handle) noexcept {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (handle == XR_NULL_HANDLE || handle_type(handle) != HT_ACTIONSET)
        return XR_ERROR_HANDLE_INVALID;
    const uint32_t i = handle_index(handle);
    if (i >= kMaxActionSets || !g_sets[i].used || g_sets[i].gen != handle_gen(handle))
        return XR_ERROR_HANDLE_INVALID;
    g_sets[i].used = false;
    for (auto& a : g_actions)
        if (a.used && a.setIndex == i) a.used = false;
    return XR_SUCCESS;
}

static XrResult impl_CreateAction(XrActionSet setHandle, const XrActionCreateInfo* info,
                                  XrAction* out) noexcept {
    if (!info || !out) return XR_ERROR_VALIDATION_FAILURE;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (setHandle == XR_NULL_HANDLE || handle_type(setHandle) != HT_ACTIONSET)
        return XR_ERROR_HANDLE_INVALID;
    const uint32_t si = handle_index(setHandle);
    if (si >= kMaxActionSets || !g_sets[si].used || g_sets[si].gen != handle_gen(setHandle))
        return XR_ERROR_HANDLE_INVALID;
    if (g_sets[si].attached) return XR_ERROR_ACTIONSETS_ALREADY_ATTACHED;

    for (uint32_t i = 0; i < kMaxActions; ++i) {
        if (g_actions[i].used) continue;
        const uint32_t gen = g_actions[i].gen + 1;
        g_actions[i] = SimAction{};
        g_actions[i].used = true;
        g_actions[i].gen = gen;
        g_actions[i].setIndex = si;
        g_actions[i].type = info->actionType;
        strcpy_s(g_actions[i].name, info->actionName);
        *out = static_cast<XrAction>(make_handle(HT_ACTION, i, gen));
        return XR_SUCCESS;
    }
    return XR_ERROR_LIMIT_REACHED;
}

static XrResult impl_DestroyAction(XrAction handle) noexcept {
    std::lock_guard<std::mutex> lock(g_mutex);
    SimAction* a = action_get(handle);
    if (!a) return XR_ERROR_HANDLE_INVALID;
    a->used = false;
    return XR_SUCCESS;
}

// ---------------------------------------------------------------------------
// Bindings
// ---------------------------------------------------------------------------

static XrResult impl_SuggestInteractionProfileBindings(
    XrInstance instance, const XrInteractionProfileSuggestedBinding* info) noexcept {
    if (!valid_instance(instance)) return XR_ERROR_HANDLE_INVALID;
    if (!info) return XR_ERROR_VALIDATION_FAILURE;

    const char* profile = path_str(info->interactionProfile);
    const bool isTouch = profile && strcmp(profile, "/interaction_profiles/oculus/touch_controller") == 0;
    const bool isSimple = profile && strcmp(profile, "/interaction_profiles/khr/simple_controller") == 0;
    if (!isTouch && !isSimple) {
        XRSIM_LOG("xrsim: interaction profile '%s' is not supported by this runtime",
                  profile ? profile : "(null)");
        return XR_ERROR_PATH_UNSUPPORTED;
    }

    const BindingEntry* table = isTouch ? kTouch : kSimple;
    const size_t n = isTouch ? (sizeof(kTouch) / sizeof(kTouch[0]))
                             : (sizeof(kSimple) / sizeof(kSimple[0]));

    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_attached) return XR_ERROR_ACTIONSETS_ALREADY_ATTACHED;

    uint32_t bound = 0;
    for (uint32_t i = 0; i < info->countSuggestedBindings; ++i) {
        const XrActionSuggestedBinding& b = info->suggestedBindings[i];
        const char* p = path_str(b.binding);
        const BindingEntry* e = find_binding(table, n, p ? p : "");
        if (!e) {
            XRSIM_LOG("xrsim: binding path '%s' is not on the %s profile - REJECTED",
                      p ? p : "(null)", isTouch ? "touch" : "simple");
            return XR_ERROR_PATH_UNSUPPORTED;
        }
        SimAction* a = action_get(b.action);
        if (!a) return XR_ERROR_HANDLE_INVALID;

        // The touch profile is the one the mod actually runs on; a simple-profile
        // suggestion must not overwrite a resolved touch binding.
        if (isTouch || a->control == VC_NONE) {
            a->control = e->control;
            a->hand = e->hand;
        }
        if (e->control != VC_NONE) ++bound;
    }

    XRSIM_LOG("xrsim: %u binding(s) suggested on the %s profile, %u driven",
              info->countSuggestedBindings, isTouch ? "touch" : "simple", bound);
    return XR_SUCCESS;
}

static XrResult impl_AttachSessionActionSets(XrSession session,
                                             const XrSessionActionSetsAttachInfo* info) noexcept {
    if (!session_valid(session)) return XR_ERROR_HANDLE_INVALID;
    if (!info) return XR_ERROR_VALIDATION_FAILURE;
    if (g.hazards.attachFail) return XR_ERROR_RUNTIME_FAILURE;

    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_attached) return XR_ERROR_ACTIONSETS_ALREADY_ATTACHED;
    for (uint32_t i = 0; i < info->countActionSets; ++i) {
        const XrActionSet h = info->actionSets[i];
        if (h == XR_NULL_HANDLE || handle_type(h) != HT_ACTIONSET) return XR_ERROR_HANDLE_INVALID;
        const uint32_t si = handle_index(h);
        if (si >= kMaxActionSets || !g_sets[si].used || g_sets[si].gen != handle_gen(h))
            return XR_ERROR_HANDLE_INVALID;
        g_sets[si].attached = true;
    }
    g_attached = true;

    // Emit the profile-changed event a real runtime sends once bindings resolve.
    XrEventDataBuffer buf{};
    auto* ev = reinterpret_cast<XrEventDataInteractionProfileChanged*>(&buf);
    ev->type = XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED;
    ev->session = session;
    queue_event(buf);

    XRSIM_LOG("xrsim: action set(s) attached to the session");
    return XR_SUCCESS;
}

static XrResult impl_GetCurrentInteractionProfile(XrSession session, XrPath,
                                                  XrInteractionProfileState* state) noexcept {
    if (!session_valid(session)) return XR_ERROR_HANDLE_INVALID;
    if (!state) return XR_ERROR_VALIDATION_FAILURE;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_attached) return XR_ERROR_ACTIONSET_NOT_ATTACHED;
    if (g_profileTouch == XR_NULL_PATH)
        g_profileTouch = path_intern("/interaction_profiles/oculus/touch_controller");
    if (g_profileSimple == XR_NULL_PATH)
        g_profileSimple = path_intern("/interaction_profiles/khr/simple_controller");
    state->interactionProfile = g_useSimpleProfile ? g_profileSimple : g_profileTouch;
    return XR_SUCCESS;
}

// ---------------------------------------------------------------------------
// Sync and state
// ---------------------------------------------------------------------------

static XrResult impl_SyncActions(XrSession session, const XrActionsSyncInfo*) noexcept {
    if (!session_valid(session)) return XR_ERROR_HANDLE_INVALID;
    if (!actions_attached()) return XR_ERROR_ACTIONSET_NOT_ATTACHED;
    if (!session_is_running()) return XR_ERROR_SESSION_NOT_RUNNING;

    g_lastSyncFrame = snapshot().index;

    // XR_SESSION_NOT_FOCUSED is a SUCCESS-class code (8). The mod checks for it
    // BEFORE its XR_FAILED test and treats it as success, publishing a zeroed
    // pad and invalidating its hand slots - that behaviour is exactly what the
    // `focus lose` hazard is meant to exercise, so returning it faithfully is
    // the whole point.
    if (current_session_state() != XR_SESSION_STATE_FOCUSED) return XR_SESSION_NOT_FOCUSED;
    return XR_SUCCESS;
}

namespace {

// Common preamble for the four state getters.
XrResult resolve(XrSession session, const XrActionStateGetInfo* info, SimAction** out) {
    if (!session_valid(session)) return XR_ERROR_HANDLE_INVALID;
    if (!info) return XR_ERROR_VALIDATION_FAILURE;
    if (!actions_attached()) return XR_ERROR_ACTIONSET_NOT_ATTACHED;
    SimAction* a = action_get(info->action);
    if (!a) return XR_ERROR_HANDLE_INVALID;
    *out = a;
    return XR_SUCCESS;
}

bool inputs_live() { return current_session_state() == XR_SESSION_STATE_FOCUSED; }

} // namespace

static XrResult impl_GetActionStateBoolean(XrSession session, const XrActionStateGetInfo* info,
                                           XrActionStateBoolean* state) noexcept {
    SimAction* a = nullptr;
    const XrResult r = resolve(session, info, &a);
    if (XR_FAILED(r)) return r;
    if (!state) return XR_ERROR_VALIDATION_FAILURE;
    if (a->type != XR_ACTION_TYPE_BOOLEAN_INPUT) return XR_ERROR_ACTION_TYPE_MISMATCH;

    const Rig& rig = committed_rig();
    const bool live = inputs_live() && a->control != VC_NONE;
    state->type = XR_TYPE_ACTION_STATE_BOOLEAN;
    state->isActive = live ? XR_TRUE : XR_FALSE;
    state->currentState = live && control_bool(rig, a->control) ? XR_TRUE : XR_FALSE;
    state->changedSinceLastSync = XR_FALSE;
    state->lastChangeTime = snapshot().displayTime;
    return XR_SUCCESS;
}

static XrResult impl_GetActionStateFloat(XrSession session, const XrActionStateGetInfo* info,
                                         XrActionStateFloat* state) noexcept {
    SimAction* a = nullptr;
    const XrResult r = resolve(session, info, &a);
    if (XR_FAILED(r)) return r;
    if (!state) return XR_ERROR_VALIDATION_FAILURE;
    if (a->type != XR_ACTION_TYPE_FLOAT_INPUT) return XR_ERROR_ACTION_TYPE_MISMATCH;

    const Rig& rig = committed_rig();
    const bool live = inputs_live() && a->control != VC_NONE;
    state->type = XR_TYPE_ACTION_STATE_FLOAT;
    state->isActive = live ? XR_TRUE : XR_FALSE;
    state->currentState = live ? control_float(rig, a->control) : 0.0f;
    state->changedSinceLastSync = XR_FALSE;
    state->lastChangeTime = snapshot().displayTime;
    return XR_SUCCESS;
}

static XrResult impl_GetActionStateVector2f(XrSession session, const XrActionStateGetInfo* info,
                                            XrActionStateVector2f* state) noexcept {
    SimAction* a = nullptr;
    const XrResult r = resolve(session, info, &a);
    if (XR_FAILED(r)) return r;
    if (!state) return XR_ERROR_VALIDATION_FAILURE;
    if (a->type != XR_ACTION_TYPE_VECTOR2F_INPUT) return XR_ERROR_ACTION_TYPE_MISMATCH;

    const Rig& rig = committed_rig();
    const bool live = inputs_live() && (a->control == VC_STICK_L || a->control == VC_STICK_R);
    const int hand = (a->control == VC_STICK_R) ? 1 : 0;
    state->type = XR_TYPE_ACTION_STATE_VECTOR2F;
    state->isActive = live ? XR_TRUE : XR_FALSE;
    // Raw, pre-deadzone. The mod applies its own radial deadzone and would
    // double-apply if the runtime helped.
    state->currentState.x = live ? rig.stick[hand][0] : 0.0f;
    state->currentState.y = live ? rig.stick[hand][1] : 0.0f;
    state->changedSinceLastSync = XR_FALSE;
    state->lastChangeTime = snapshot().displayTime;
    return XR_SUCCESS;
}

static XrResult impl_GetActionStatePose(XrSession session, const XrActionStateGetInfo* info,
                                        XrActionStatePose* state) noexcept {
    SimAction* a = nullptr;
    const XrResult r = resolve(session, info, &a);
    if (XR_FAILED(r)) return r;
    if (!state) return XR_ERROR_VALIDATION_FAILURE;
    if (a->type != XR_ACTION_TYPE_POSE_INPUT) return XR_ERROR_ACTION_TYPE_MISMATCH;

    const Rig& rig = committed_rig();
    const int hand = (a->hand == 1) ? 1 : 0;
    const bool live = inputs_live() && control_is_pose(a->control) && rig.handValid[hand];
    state->type = XR_TYPE_ACTION_STATE_POSE;
    state->isActive = live ? XR_TRUE : XR_FALSE;
    return XR_SUCCESS;
}

// ---------------------------------------------------------------------------
// Shims
// ---------------------------------------------------------------------------

XRAPI_ATTR XrResult XRAPI_CALL xrsim_CreateActionSet(XrInstance i, const XrActionSetCreateInfo* ci,
                                                     XrActionSet* o) {
    XRSIM_ENTRY(impl_CreateActionSet(i, ci, o), "CreateActionSet")
}
XRAPI_ATTR XrResult XRAPI_CALL xrsim_DestroyActionSet(XrActionSet s) {
    XRSIM_ENTRY(impl_DestroyActionSet(s), "DestroyActionSet")
}
XRAPI_ATTR XrResult XRAPI_CALL xrsim_CreateAction(XrActionSet s, const XrActionCreateInfo* ci,
                                                  XrAction* o) {
    XRSIM_ENTRY(impl_CreateAction(s, ci, o), "CreateAction")
}
XRAPI_ATTR XrResult XRAPI_CALL xrsim_DestroyAction(XrAction a) {
    XRSIM_ENTRY(impl_DestroyAction(a), "DestroyAction")
}
XRAPI_ATTR XrResult XRAPI_CALL xrsim_SuggestInteractionProfileBindings(
    XrInstance i, const XrInteractionProfileSuggestedBinding* b) {
    XRSIM_ENTRY(impl_SuggestInteractionProfileBindings(i, b), "SuggestInteractionProfileBindings")
}
XRAPI_ATTR XrResult XRAPI_CALL xrsim_AttachSessionActionSets(
    XrSession s, const XrSessionActionSetsAttachInfo* i) {
    XRSIM_ENTRY(impl_AttachSessionActionSets(s, i), "AttachSessionActionSets")
}
XRAPI_ATTR XrResult XRAPI_CALL xrsim_GetCurrentInteractionProfile(XrSession s, XrPath p,
                                                                  XrInteractionProfileState* st) {
    XRSIM_ENTRY(impl_GetCurrentInteractionProfile(s, p, st), "GetCurrentInteractionProfile")
}
XRAPI_ATTR XrResult XRAPI_CALL xrsim_GetActionStateBoolean(XrSession s,
                                                           const XrActionStateGetInfo* i,
                                                           XrActionStateBoolean* st) {
    XRSIM_ENTRY(impl_GetActionStateBoolean(s, i, st), "GetActionStateBoolean")
}
XRAPI_ATTR XrResult XRAPI_CALL xrsim_GetActionStateFloat(XrSession s, const XrActionStateGetInfo* i,
                                                         XrActionStateFloat* st) {
    XRSIM_ENTRY(impl_GetActionStateFloat(s, i, st), "GetActionStateFloat")
}
XRAPI_ATTR XrResult XRAPI_CALL xrsim_GetActionStateVector2f(XrSession s,
                                                            const XrActionStateGetInfo* i,
                                                            XrActionStateVector2f* st) {
    XRSIM_ENTRY(impl_GetActionStateVector2f(s, i, st), "GetActionStateVector2f")
}
XRAPI_ATTR XrResult XRAPI_CALL xrsim_GetActionStatePose(XrSession s, const XrActionStateGetInfo* i,
                                                        XrActionStatePose* st) {
    XRSIM_ENTRY(impl_GetActionStatePose(s, i, st), "GetActionStatePose")
}
XRAPI_ATTR XrResult XRAPI_CALL xrsim_SyncActions(XrSession s, const XrActionsSyncInfo* i) {
    XRSIM_ENTRY(impl_SyncActions(s, i), "SyncActions")
}

} // namespace xrsim
