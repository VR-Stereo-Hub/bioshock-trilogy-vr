#ifdef BVR_WITH_OPENXR

#include "core/vr/openxr_input.h"

#include "core/input/swing.h"
#include "core/input/xinput_bridge.h"
#include "core/util/log.h"
#include "core/vr/openxr_runtime.h"

#include <windows.h>
#include <Xinput.h> // button bit constants only

#include <imgui.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace bvr::vr {
namespace {

// Grip squeeze -> bumper with hysteresis (press/release thresholds).
constexpr float kGripPress = 0.70f;
constexpr float kGripRelease = 0.55f;
// Left menu button: short press -> START, hold -> BACK (Touch has no second
// system-legal menu button). START is emitted as a short pulse on release so
// the game's per-tick edge detection cannot miss it.
constexpr uint64_t kMenuLongMs = 500;
constexpr uint64_t kStartPulseMs = 150;
// Ammo-slot select (session 19, headset-revised): while the right-stick
// CLICK is held, stick directions past kFlickPress select the ammo slot
// (dpad up/down/left pulses); re-arm inside +-kFlickRearm, cooldown against
// machine-gunning. Zoom is removed - RS-click never reaches the game.
constexpr float kFlickPress = 0.65f;
constexpr float kFlickRearm = 0.30f;
constexpr uint64_t kFlickPulseMs = 150;
constexpr uint64_t kFlickCooldownMs = 300;

// ---------------------------------------------------------------------------
// The per-game pad map (session 44, Infinite I7).
//
// A synthetic pad's only job is to land on the bindings the game already
// ships, so the XR-to-XInput table is a property of the GAME, not of the
// composer. Everything below was hardcoded BioShock 1 semantics; the tables
// make the game-specific decisions data, and the BioShock1 table reproduces
// the previous literals exactly - that is the whole inertness argument. The
// selector is one atomic in the bridge (bvr::input::pad_profile()), default
// Bioshock1, armed once by the game adapter.
//
// What is NOT in the table because it is game-neutral: stick deadzone and
// scaling, trigger scaling, the grip hysteresis thresholds, and the menu
// tap/hold split (short -> START, long -> BACK fits pause/back on both games).
// ---------------------------------------------------------------------------
struct PadMap {
    const char* name;
    // XInput bit each Touch face produces. 0 would mean "swallow it".
    uint16_t faceA, faceB, faceX, faceY;
    uint16_t gripL, gripR;             // squeeze past the hysteresis -> bumper
    uint16_t stickClickL, stickClickR; // 0 = deliberately NOT forwarded
    // The modifier + right-stick flick lane. Directions with a 0 bit are not
    // emitted at all, which is how BS1 keeps its three-way ammo select while
    // Infinite gets the fourth direction its dpad nav needs.
    bool flick;
    bool flickAmmoModPref; // honour `vrinput ammomod`; else thumbrest-only
    uint16_t flickUp, flickDown, flickLeft, flickRight;
};

// BioShock 1 and 2. Session 19's headset-revised audit, verbatim: the game's
// own pad layout (User.ini XENON_*) is A=Use, B=MedHypo, X=Reload/Hack/EVE,
// Y=Jump, and the user's verdict was that Touch A must STAY use/loot (it is
// also the menu confirm button) with jump on Touch B. RS-click is deliberately
// consumed: zoom was removed (a FOV zoom inside an HMD is a comfort hazard),
// so the click is purely the ammo modifier and never reaches the game. The
// three ammo types sit on dpad UP/DOWN/LEFT; there is no fourth.
constexpr PadMap kPadMapBioshock1 = {
    "bioshock1",
    XINPUT_GAMEPAD_A,            // Touch A -> use / interact / menu confirm
    XINPUT_GAMEPAD_Y,            // Touch B -> jump
    XINPUT_GAMEPAD_X,            // Touch X -> reload / hack / EVE inject
    XINPUT_GAMEPAD_B,            // Touch Y -> first aid (med hypo)
    XINPUT_GAMEPAD_LEFT_SHOULDER, XINPUT_GAMEPAD_RIGHT_SHOULDER,
    XINPUT_GAMEPAD_LEFT_THUMB, 0, // RS-click eaten: it IS the ammo modifier
    true, true,
    XINPUT_GAMEPAD_DPAD_UP, XINPUT_GAMEPAD_DPAD_DOWN, XINPUT_GAMEPAD_DPAD_LEFT, 0,
};

// BioShock Infinite (UE3). From the audited retail XboxTypeS_* binding set
// (ENGINE_NOTES "The audited retail pad map"): A = TBar transfer + Jump,
// B = TBar dodge/reverse + ToggleCrouch, X = ReloadOrHoldToHackOrUse,
// Y = TBar melee transfer + melee, LB = NextPlasmid, RB = NextWeapon,
// LS click = StartSprint, RS click = XToggleZoom. So the faces pass STRAIGHT
// THROUGH and RS-click must be FORWARDED - the opposite of BS1 on both counts.
//
// The dpad here carries nav and hack (XNavShowPulse/BuyoutHack,
// XMakeUnstableSelection/AutoHack, XNavQuickToggleCycleLeft/Right), and Touch
// has no dpad. Per the user's call (session 44) the flick lane is kept as the
// analogue - left thumbrest held, right stick flicked - and gains the fourth
// direction the cycle pair needs. RS-click can no longer double as the
// modifier, because here it is a real binding.
constexpr PadMap kPadMapInfinite = {
    "infinite",
    XINPUT_GAMEPAD_A,             // Touch A -> jump (+ TBar transfer)
    XINPUT_GAMEPAD_B,             // Touch B -> crouch (+ TBar dodge)
    XINPUT_GAMEPAD_X,             // Touch X -> reload / hold-to-hack / use
    XINPUT_GAMEPAD_Y,             // Touch Y -> melee (+ TBar melee transfer)
    XINPUT_GAMEPAD_LEFT_SHOULDER, XINPUT_GAMEPAD_RIGHT_SHOULDER,
    XINPUT_GAMEPAD_LEFT_THUMB,    // sprint
    XINPUT_GAMEPAD_RIGHT_THUMB,   // XToggleZoom - forwarded, unlike BS1
    true, false,                  // thumbrest-only modifier
    XINPUT_GAMEPAD_DPAD_UP, XINPUT_GAMEPAD_DPAD_DOWN, XINPUT_GAMEPAD_DPAD_LEFT,
    XINPUT_GAMEPAD_DPAD_RIGHT,
};

const PadMap& active_pad_map() {
    return bvr::input::pad_profile() == bvr::input::PadProfile::Infinite
               ? kPadMapInfinite
               : kPadMapBioshock1;
}

XrActionSet g_actionSet = XR_NULL_HANDLE;

XrAction g_move = XR_NULL_HANDLE;      // VECTOR2F left thumbstick
XrAction g_look = XR_NULL_HANDLE;      // VECTOR2F right thumbstick
XrAction g_fire = XR_NULL_HANDLE;      // FLOAT right trigger
XrAction g_plasmid = XR_NULL_HANDLE;   // FLOAT left trigger
XrAction g_gripR = XR_NULL_HANDLE;     // FLOAT right squeeze
XrAction g_gripL = XR_NULL_HANDLE;     // FLOAT left squeeze
XrAction g_btnA = XR_NULL_HANDLE;
XrAction g_btnB = XR_NULL_HANDLE;
XrAction g_btnX = XR_NULL_HANDLE;
XrAction g_btnY = XR_NULL_HANDLE;
XrAction g_stickClickL = XR_NULL_HANDLE;
XrAction g_stickClickR = XR_NULL_HANDLE;
XrAction g_menu = XR_NULL_HANDLE;
// Capacitive thumbrest pads (session 23). BOOLEAN touch, part of the core
// oculus/touch_controller profile - no extension needed. read_bool() returns
// false when a binding is inactive, so a runtime that does not expose them
// simply never fires the modifier. g_thumbrestSeen logs the first real touch,
// which is how we prove the runtime actually reports it.
XrAction g_thumbrestL = XR_NULL_HANDLE;
XrAction g_thumbrestR = XR_NULL_HANDLE;
bool g_thumbrestSeen[2] = {false, false};
XrAction g_poseL = XR_NULL_HANDLE;     // grip poses - hand position (M7 hands)
XrAction g_poseR = XR_NULL_HANDLE;
XrAction g_aimL = XR_NULL_HANDLE;      // AIM poses - where the controller POINTS.
XrAction g_aimR = XR_NULL_HANDLE;      // The runtime's own pointing ray; the grip
                                       // pose runs along the handle and reads
                                       // tens of degrees low as an aim vector.

XrSpace g_gripSpaceL = XR_NULL_HANDLE; // session children
XrSpace g_gripSpaceR = XR_NULL_HANDLE;
XrSpace g_aimSpaceL = XR_NULL_HANDLE;
XrSpace g_aimSpaceR = XR_NULL_HANDLE;
XrSpace g_baseSpace = XR_NULL_HANDLE;  // app space, owned by the runtime
bool g_attached = false;
bool g_created = false;
bool g_loggedAttachFail = false;

// Render-thread state for the composers.
bool g_gripLatchedL = false;
bool g_gripLatchedR = false;
uint64_t g_menuDownMs = 0;
uint64_t g_startPulseUntilMs = 0;
bool g_flickArmed = true;
uint16_t g_flickPulseBit = 0;
uint64_t g_flickPulseUntilMs = 0;
uint64_t g_flickCooldownMs = 0;
bool g_rsClickWasDown = false;

// M6 hand poses. Located on the render thread in input_sync; read from the
// GAME thread by the adapter's aim path, so publish through atomics-guarded
// snapshots (relaxed is fine: a group torn by one frame is invisible at
// 90 Hz, and the valid flag is what gates use of the numbers).
struct HandSlot {
    std::atomic<bool> valid{false};
    float px = 0.0f, py = 0.0f, pz = 0.0f;
    float qx = 0.0f, qy = 0.0f, qz = 0.0f, qw = 1.0f;
};
HandSlot g_hands[2]; // grip pose: 0 = left, 1 = right
HandSlot g_aims[2];  // aim pose, same indexing

// Session 20 vrrec: sim overlay ON the funnel. While armed, EVERY consumer of
// input_get_hand_pose (fire ray, viewmodel, laser) reads the injected poses
// instead of the runtime's - replay only works if all three see one
// consistent world. Written from the game thread (replay tick / drive
// command), read game+render side - the same atomics discipline as the live
// slots.
std::atomic<bool> g_simHandsActive{false};
HandSlot g_simHands[2];
HandSlot g_simAims[2];

// Telemetry for the overlay (render thread writes, overlay reads same thread).
std::atomic<uint32_t> g_syncOk{0};
std::atomic<uint32_t> g_syncNotFocused{0};
std::atomic<uint32_t> g_syncFailed{0};
std::atomic<bool> g_lastActive{false};

XrPath path(XrInstance inst, const char* s) {
    XrPath p = XR_NULL_PATH;
    xrStringToPath(inst, s, &p);
    return p;
}

bool make_action(const char* name, const char* localized, XrActionType type,
                 XrAction* out) {
    XrActionCreateInfo aci{XR_TYPE_ACTION_CREATE_INFO};
    aci.actionType = type;
    strcpy_s(aci.actionName, name);
    strcpy_s(aci.localizedActionName, localized);
    XrResult r = xrCreateAction(g_actionSet, &aci, out);
    if (XR_FAILED(r)) {
        BVR_LOG("xr-input: xrCreateAction(%s) failed (%d)", name,
                static_cast<int>(r));
        *out = XR_NULL_HANDLE;
        return false;
    }
    return true;
}

int16_t axis_to_thumb(float v) {
    if (v > 1.0f) v = 1.0f;
    if (v < -1.0f) v = -1.0f;
    return static_cast<int16_t>(v * 32767.0f);
}

// Radial deadzone, rescaled so the post-deadzone range covers full deflection.
void apply_deadzone(float& x, float& y) {
    float dz = bvr::input::stick_deadzone();
    float mag = sqrtf(x * x + y * y);
    if (mag <= dz) {
        x = y = 0.0f;
        return;
    }
    float scale = ((mag - dz) / (1.0f - dz)) / mag;
    x *= scale;
    y *= scale;
}

float read_float(XrSession session, XrAction action) {
    XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};
    gi.action = action;
    XrActionStateFloat st{XR_TYPE_ACTION_STATE_FLOAT};
    if (XR_FAILED(xrGetActionStateFloat(session, &gi, &st)) || !st.isActive)
        return 0.0f;
    return st.currentState;
}

bool read_bool(XrSession session, XrAction action) {
    XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};
    gi.action = action;
    XrActionStateBoolean st{XR_TYPE_ACTION_STATE_BOOLEAN};
    if (XR_FAILED(xrGetActionStateBoolean(session, &gi, &st)) || !st.isActive)
        return false;
    return st.currentState == XR_TRUE;
}

// Locate one grip space against the app space at the frame's predicted display
// time - the same instant the head pose is located, so hand and head belong to
// the same moment and the aim ray cannot lag the camera.
void locate_hand(XrSession session, XrAction poseAction, XrSpace space, XrTime when,
                 HandSlot& slot) {
    if (space == XR_NULL_HANDLE || g_baseSpace == XR_NULL_HANDLE) {
        slot.valid.store(false, std::memory_order_relaxed);
        return;
    }
    // The action must be active (controller present + bound) before its space
    // is meaningful.
    XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};
    gi.action = poseAction;
    XrActionStatePose ps{XR_TYPE_ACTION_STATE_POSE};
    if (XR_FAILED(xrGetActionStatePose(session, &gi, &ps)) || !ps.isActive) {
        slot.valid.store(false, std::memory_order_relaxed);
        return;
    }
    XrSpaceLocation sl{XR_TYPE_SPACE_LOCATION};
    if (XR_FAILED(xrLocateSpace(space, g_baseSpace, when, &sl)) ||
        !(sl.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) ||
        !(sl.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)) {
        slot.valid.store(false, std::memory_order_relaxed);
        return;
    }
    slot.px = sl.pose.position.x;
    slot.py = sl.pose.position.y;
    slot.pz = sl.pose.position.z;
    slot.qx = sl.pose.orientation.x;
    slot.qy = sl.pose.orientation.y;
    slot.qz = sl.pose.orientation.z;
    slot.qw = sl.pose.orientation.w;
    slot.valid.store(true, std::memory_order_relaxed);
}

void invalidate_hand_slots() {
    for (int i = 0; i < 2; ++i) {
        g_hands[i].valid.store(false, std::memory_order_relaxed);
        g_aims[i].valid.store(false, std::memory_order_relaxed);
    }
}

bool read_vec2(XrSession session, XrAction action, float* x, float* y) {
    XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};
    gi.action = action;
    XrActionStateVector2f st{XR_TYPE_ACTION_STATE_VECTOR2F};
    if (XR_FAILED(xrGetActionStateVector2f(session, &gi, &st)) || !st.isActive) {
        *x = *y = 0.0f;
        return false;
    }
    *x = st.currentState.x;
    *y = st.currentState.y;
    return true;
}

} // namespace

void input_create(XrInstance instance) {
    XrActionSetCreateInfo asci{XR_TYPE_ACTION_SET_CREATE_INFO};
    strcpy_s(asci.actionSetName, "gameplay");
    strcpy_s(asci.localizedActionSetName, "Gameplay");
    XrResult r = xrCreateActionSet(instance, &asci, &g_actionSet);
    if (XR_FAILED(r)) {
        BVR_LOG("xr-input: xrCreateActionSet failed (%d) - controller input off",
                static_cast<int>(r));
        return;
    }

    int made = 0;
    made += make_action("move", "Move", XR_ACTION_TYPE_VECTOR2F_INPUT, &g_move);
    made += make_action("look", "Look", XR_ACTION_TYPE_VECTOR2F_INPUT, &g_look);
    made += make_action("fire", "Fire weapon", XR_ACTION_TYPE_FLOAT_INPUT, &g_fire);
    made += make_action("plasmid", "Fire plasmid", XR_ACTION_TYPE_FLOAT_INPUT, &g_plasmid);
    made += make_action("grip_r", "Right grip", XR_ACTION_TYPE_FLOAT_INPUT, &g_gripR);
    made += make_action("grip_l", "Left grip", XR_ACTION_TYPE_FLOAT_INPUT, &g_gripL);
    made += make_action("btn_a", "A", XR_ACTION_TYPE_BOOLEAN_INPUT, &g_btnA);
    made += make_action("btn_b", "B", XR_ACTION_TYPE_BOOLEAN_INPUT, &g_btnB);
    made += make_action("btn_x", "X", XR_ACTION_TYPE_BOOLEAN_INPUT, &g_btnX);
    made += make_action("btn_y", "Y", XR_ACTION_TYPE_BOOLEAN_INPUT, &g_btnY);
    made += make_action("stick_l", "Left stick click", XR_ACTION_TYPE_BOOLEAN_INPUT,
                        &g_stickClickL);
    made += make_action("stick_r", "Right stick click", XR_ACTION_TYPE_BOOLEAN_INPUT,
                        &g_stickClickR);
    made += make_action("menu", "Menu", XR_ACTION_TYPE_BOOLEAN_INPUT, &g_menu);
    made += make_action("thumbrest_l", "Left thumbrest touch", XR_ACTION_TYPE_BOOLEAN_INPUT,
                        &g_thumbrestL);
    made += make_action("thumbrest_r", "Right thumbrest touch", XR_ACTION_TYPE_BOOLEAN_INPUT,
                        &g_thumbrestR);
    made += make_action("pose_l", "Left grip pose", XR_ACTION_TYPE_POSE_INPUT, &g_poseL);
    made += make_action("pose_r", "Right grip pose", XR_ACTION_TYPE_POSE_INPUT, &g_poseR);
    made += make_action("aim_l", "Left aim pose", XR_ACTION_TYPE_POSE_INPUT, &g_aimL);
    made += make_action("aim_r", "Right aim pose", XR_ACTION_TYPE_POSE_INPUT, &g_aimR);

    // Quest 3 Touch. Never bind .../input/system/click - reserved by runtimes.
    XrActionSuggestedBinding touch[] = {
        {g_move, path(instance, "/user/hand/left/input/thumbstick")},
        {g_look, path(instance, "/user/hand/right/input/thumbstick")},
        {g_fire, path(instance, "/user/hand/right/input/trigger/value")},
        {g_plasmid, path(instance, "/user/hand/left/input/trigger/value")},
        {g_gripR, path(instance, "/user/hand/right/input/squeeze/value")},
        {g_gripL, path(instance, "/user/hand/left/input/squeeze/value")},
        {g_btnA, path(instance, "/user/hand/right/input/a/click")},
        {g_btnB, path(instance, "/user/hand/right/input/b/click")},
        {g_btnX, path(instance, "/user/hand/left/input/x/click")},
        {g_btnY, path(instance, "/user/hand/left/input/y/click")},
        {g_stickClickL, path(instance, "/user/hand/left/input/thumbstick/click")},
        {g_stickClickR, path(instance, "/user/hand/right/input/thumbstick/click")},
        {g_thumbrestL, path(instance, "/user/hand/left/input/thumbrest/touch")},
        {g_thumbrestR, path(instance, "/user/hand/right/input/thumbrest/touch")},
        {g_menu, path(instance, "/user/hand/left/input/menu/click")},
        {g_poseL, path(instance, "/user/hand/left/input/grip/pose")},
        {g_poseR, path(instance, "/user/hand/right/input/grip/pose")},
        {g_aimL, path(instance, "/user/hand/left/input/aim/pose")},
        {g_aimR, path(instance, "/user/hand/right/input/aim/pose")},
    };
    XrInteractionProfileSuggestedBinding sb{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
    sb.interactionProfile = path(instance, "/interaction_profiles/oculus/touch_controller");
    sb.suggestedBindings = touch;
    sb.countSuggestedBindings = static_cast<uint32_t>(sizeof(touch) / sizeof(touch[0]));
    r = xrSuggestInteractionProfileBindings(instance, &sb);
    if (XR_FAILED(r))
        BVR_LOG("xr-input: touch_controller binding suggestion failed (%d)",
                static_cast<int>(r));

    // Minimal khr/simple_controller fallback so unknown runtimes still give
    // menu-walk capability (select = A, menu = START).
    XrActionSuggestedBinding simple[] = {
        {g_btnA, path(instance, "/user/hand/right/input/select/click")},
        {g_menu, path(instance, "/user/hand/left/input/menu/click")},
        {g_poseL, path(instance, "/user/hand/left/input/grip/pose")},
        {g_poseR, path(instance, "/user/hand/right/input/grip/pose")},
        {g_aimL, path(instance, "/user/hand/left/input/aim/pose")},
        {g_aimR, path(instance, "/user/hand/right/input/aim/pose")},
    };
    sb.interactionProfile = path(instance, "/interaction_profiles/khr/simple_controller");
    sb.suggestedBindings = simple;
    sb.countSuggestedBindings = static_cast<uint32_t>(sizeof(simple) / sizeof(simple[0]));
    xrSuggestInteractionProfileBindings(instance, &sb); // best-effort

    g_created = true;
    BVR_LOG("xr-input: action set ready (%d actions, touch_controller bindings "
            "suggested)", made);
}

void input_on_session_created(XrSession session, XrSpace baseSpace) {
    if (!g_created) return;
    g_baseSpace = baseSpace; // M6: hand poses locate against the app space

    XrActionSpaceCreateInfo asci{XR_TYPE_ACTION_SPACE_CREATE_INFO};
    asci.poseInActionSpace.orientation.w = 1.0f;
    asci.action = g_poseL;
    if (XR_FAILED(xrCreateActionSpace(session, &asci, &g_gripSpaceL)))
        g_gripSpaceL = XR_NULL_HANDLE;
    asci.action = g_poseR;
    if (XR_FAILED(xrCreateActionSpace(session, &asci, &g_gripSpaceR)))
        g_gripSpaceR = XR_NULL_HANDLE;
    asci.action = g_aimL;
    if (XR_FAILED(xrCreateActionSpace(session, &asci, &g_aimSpaceL)))
        g_aimSpaceL = XR_NULL_HANDLE;
    asci.action = g_aimR;
    if (XR_FAILED(xrCreateActionSpace(session, &asci, &g_aimSpaceR)))
        g_aimSpaceR = XR_NULL_HANDLE;

    XrSessionActionSetsAttachInfo sai{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
    sai.countActionSets = 1;
    sai.actionSets = &g_actionSet;
    XrResult r = xrAttachSessionActionSets(session, &sai);
    if (XR_FAILED(r)) {
        if (!g_loggedAttachFail) {
            BVR_LOG("xr-input: xrAttachSessionActionSets failed (%d) - display "
                    "continues without controller input", static_cast<int>(r));
            g_loggedAttachFail = true;
        }
        return;
    }
    g_attached = true;
    g_loggedAttachFail = false;
    BVR_LOG("xr-input: action set attached to session");
}

void input_on_session_teardown() {
    if (g_gripSpaceL != XR_NULL_HANDLE) { xrDestroySpace(g_gripSpaceL); g_gripSpaceL = XR_NULL_HANDLE; }
    if (g_gripSpaceR != XR_NULL_HANDLE) { xrDestroySpace(g_gripSpaceR); g_gripSpaceR = XR_NULL_HANDLE; }
    if (g_aimSpaceL != XR_NULL_HANDLE) { xrDestroySpace(g_aimSpaceL); g_aimSpaceL = XR_NULL_HANDLE; }
    if (g_aimSpaceR != XR_NULL_HANDLE) { xrDestroySpace(g_aimSpaceR); g_aimSpaceR = XR_NULL_HANDLE; }
    g_baseSpace = XR_NULL_HANDLE;
    invalidate_hand_slots();
    g_attached = false;
    g_gripLatchedL = g_gripLatchedR = false;
    g_menuDownMs = 0;
    g_startPulseUntilMs = 0;
    bvr::input::publish_xr_state({}, false);
}

void input_sync(XrSession session, XrTime predictedDisplayTime) {
    if (!g_attached) return;

    XrActiveActionSet active{g_actionSet, XR_NULL_PATH};
    XrActionsSyncInfo si{XR_TYPE_ACTIONS_SYNC_INFO};
    si.countActiveActionSets = 1;
    si.activeActionSets = &active;
    XrResult r = xrSyncActions(session, &si);
    if (r == XR_SESSION_NOT_FOCUSED) {
        g_syncNotFocused.fetch_add(1, std::memory_order_relaxed);
        g_lastActive.store(false, std::memory_order_relaxed);
        invalidate_hand_slots();
        bvr::input::publish_xr_state({}, false);
        return;
    }
    if (XR_FAILED(r)) {
        // Never tear the session down from the input path - display first.
        g_syncFailed.fetch_add(1, std::memory_order_relaxed);
        g_lastActive.store(false, std::memory_order_relaxed);
        invalidate_hand_slots();
        bvr::input::publish_xr_state({}, false);
        return;
    }
    g_syncOk.fetch_add(1, std::memory_order_relaxed);

    // M6: hand grip poses for the aim ray (never fatal - a hand that fails to
    // locate just leaves its slot invalid and the aim falls back to the view).
    locate_hand(session, g_poseL, g_gripSpaceL, predictedDisplayTime, g_hands[0]);
    locate_hand(session, g_poseR, g_gripSpaceR, predictedDisplayTime, g_hands[1]);
    locate_hand(session, g_aimL, g_aimSpaceL, predictedDisplayTime, g_aims[0]);
    locate_hand(session, g_aimR, g_aimSpaceR, predictedDisplayTime, g_aims[1]);

    // Session 31 swing-to-attack: feed the right hand's motion to the detector.
    // Read through input_get_hand_pose rather than off g_hands[1] directly, so
    // the session-20 sim overlay (vrrec replay, `vrrec hand`) drives the gesture
    // exactly as it drives the ray, the viewmodel and the laser - one injected
    // world for every consumer. The GRIP pose is the hand itself; the aim pose
    // is where it points, which is not what a swing is made of.
    {
        float hand[3], handQ[4];
        const bool handOk = input_get_hand_pose(1, false, hand, handQ);
        bvr::vr::HeadPose head{};
        const bool headOk = bvr::vr::peek_head_pose(head);
        const float headPos[3] = {head.px, head.py, head.pz};
        bvr::input::swing::publish_sample(hand, headPos, handOk, headOk,
                                          static_cast<int64_t>(predictedDisplayTime));
    }

    bvr::input::Gamepad pad{};

    float mx = 0.0f, my = 0.0f, lx = 0.0f, ly = 0.0f;
    read_vec2(session, g_move, &mx, &my);
    read_vec2(session, g_look, &lx, &ly);
    apply_deadzone(mx, my);
    apply_deadzone(lx, ly);
    pad.lx = axis_to_thumb(mx);
    pad.ly = axis_to_thumb(my); // XR +y = stick forward = XInput +Y (up)
    pad.rx = axis_to_thumb(lx);
    pad.ry = axis_to_thumb(ly);

    float rt = read_float(session, g_fire);
    float lt = read_float(session, g_plasmid);
    pad.rt = static_cast<uint8_t>(rt * 255.0f + 0.5f);
    pad.lt = static_cast<uint8_t>(lt * 255.0f + 0.5f);

    float gl = read_float(session, g_gripL);
    float gr = read_float(session, g_gripR);
    g_gripLatchedL = g_gripLatchedL ? (gl >= kGripRelease) : (gl >= kGripPress);
    g_gripLatchedR = g_gripLatchedR ? (gr >= kGripRelease) : (gr >= kGripPress);

    // ONE map read per compose. A single relaxed load behind it, so a live A/B
    // can never build a half-switched pad.
    const PadMap& map = active_pad_map();

    if (g_gripLatchedL) pad.buttons |= map.gripL;
    if (g_gripLatchedR) pad.buttons |= map.gripR;

    // The face buttons, from the table. What each bit MEANS in the game is in
    // the table's own comment; the composer only lands the bit.
    if (read_bool(session, g_btnA)) pad.buttons |= map.faceA;
    if (read_bool(session, g_btnB)) pad.buttons |= map.faceB;
    if (read_bool(session, g_btnX)) pad.buttons |= map.faceX;
    if (read_bool(session, g_btnY)) pad.buttons |= map.faceY;
    if (read_bool(session, g_stickClickL)) pad.buttons |= map.stickClickL;
    // Forwarded only where the map says so: on BioShock 1 this bit is 0 and
    // the click is consumed below as the ammo modifier instead.
    if (map.stickClickR && read_bool(session, g_stickClickR))
        pad.buttons |= map.stickClickR;

    uint64_t now = GetTickCount64();

    // Ammo-slot select (session 19, revised twice by headset runs): the
    // three ammo types sit on dpad UP / DOWN / LEFT (each direction SELECTS
    // its slot - flat-proven CallHudFunction handlers). HOLD the right-stick
    // CLICK as a modifier - stick directions then select the slot (dpad
    // pulses) and turning is suppressed. ZOOM IS GONE by the user's call
    // (a FOV zoom inside an HMD is a comfort hazard and nothing requires
    // it), so the click is purely the ammo modifier and RS-click never
    // reaches the game. Direction reads the PRE-deadzone stick; the re-arm
    // band allows several selects in one hold; grips suppress it (the
    // radials read the stick).
    if (map.flick) {
        float rawX = 0.0f, rawY = 0.0f;
        read_vec2(session, g_look, &rawX, &rawY);
        bool gripHeld = g_gripLatchedL || g_gripLatchedR;

        // Session 23: the modifier can also be the LEFT thumbrest. It has to be
        // the left one - the right thumb cannot rest on the right thumbrest and
        // push the right stick at the same time, so a thumbrest modifier is
        // necessarily cross-hand. Slot directions stay on the right stick, so
        // nobody's muscle memory changes.
        const bool rsClick = read_bool(session, g_stickClickR);
        const bool restL = read_bool(session, g_thumbrestL);
        const bool restR = read_bool(session, g_thumbrestR);
        for (int i = 0; i < 2; ++i) {
            if ((i == 0 ? restL : restR) && !g_thumbrestSeen[i]) {
                g_thumbrestSeen[i] = true;
                BVR_LOG("xr-input: %s thumbrest touch reported by the runtime - "
                        "usable as the slot-select modifier%s",
                        i == 0 ? "LEFT" : "RIGHT",
                        map.flickAmmoModPref ? " ('vrinput ammomod thumbrest')"
                                             : " (it is the only modifier on this game - "
                                               "RS-click is a real binding here)");
            }
        }
        // Where RS-click is a real game binding it can never double as the
        // modifier, and the ammo-modifier preference (a BioShock 1 comfort
        // setting) does not apply. Thumbrest-only, unconditionally.
        bool clickMod = false;
        bool restMod = restL;
        if (map.flickAmmoModPref) {
            const bvr::input::AmmoMod mode = bvr::input::ammo_mod();
            // Thumbrest is the default, but not every controller has one (Pico
            // has no thumbrest; some SteamVR setups do not report it) and
            // losing ammo select entirely would be a nasty surprise. So in
            // Thumbrest mode the stick click keeps working UNTIL a real
            // thumbrest touch is observed - after that the mapping is exactly
            // what was chosen.
            const bool noThumbrestYet = !g_thumbrestSeen[0];
            clickMod = (mode != bvr::input::AmmoMod::Thumbrest || noThumbrestYet) && rsClick;
            restMod = mode != bvr::input::AmmoMod::Click && restL;
        }
        const bool modHeld = clickMod || restMod;

        if (modHeld && !g_rsClickWasDown) g_flickArmed = true;
        g_rsClickWasDown = modHeld;
        if (modHeld && !gripHeld) {
            // Turn suppression. RS-click never reaches the game, so killing turn
            // for its whole hold costs nothing. A thumb PARKED on the thumbrest
            // is not a deliberate gesture though - suppressing turn for as long
            // as it rests there would break turning during normal play - so in
            // thumbrest mode only suppress while the stick is actually pushed
            // past the select threshold.
            const bool pushed =
                fabsf(rawX) >= kFlickPress || fabsf(rawY) >= kFlickPress;
            if (clickMod || pushed) {
                pad.rx = 0;
                pad.ry = 0;
            }
            if (g_flickArmed && now >= g_flickCooldownMs) {
                // Directions the map leaves at 0 are simply never emitted -
                // that is how BioShock 1 keeps its three-way ammo select while
                // Infinite gets the fourth direction its nav cycle needs.
                uint16_t bit = 0;
                if (rawY >= kFlickPress) bit = map.flickUp;
                else if (rawY <= -kFlickPress) bit = map.flickDown;
                else if (rawX <= -kFlickPress) bit = map.flickLeft;
                else if (rawX >= kFlickPress) bit = map.flickRight;
                if (bit) {
                    g_flickPulseBit = bit;
                    g_flickPulseUntilMs = now + kFlickPulseMs;
                    g_flickCooldownMs = now + kFlickCooldownMs;
                    g_flickArmed = false;
                }
            }
            if (rawX > -kFlickRearm && rawX < kFlickRearm && rawY > -kFlickRearm &&
                rawY < kFlickRearm)
                g_flickArmed = true;
        }
        if (now < g_flickPulseUntilMs) pad.buttons |= g_flickPulseBit;
    }

    // Left menu: short press pulses START on release, holding it past the
    // threshold holds BACK until release.
    bool menuDown = read_bool(session, g_menu);
    if (menuDown) {
        if (g_menuDownMs == 0) g_menuDownMs = now;
        if (now - g_menuDownMs >= kMenuLongMs) pad.buttons |= XINPUT_GAMEPAD_BACK;
    } else {
        if (g_menuDownMs != 0 && now - g_menuDownMs < kMenuLongMs)
            g_startPulseUntilMs = now + kStartPulseMs;
        g_menuDownMs = 0;
    }
    if (now < g_startPulseUntilMs) pad.buttons |= XINPUT_GAMEPAD_START;

    g_lastActive.store(true, std::memory_order_relaxed);
    bvr::input::publish_xr_state(pad, true);
}

bool input_get_hand_pose(int hand, bool aimPose, float* pos3, float* quat4) {
    if (hand < 0 || hand > 1 || !pos3 || !quat4) return false;
    const bool sim = g_simHandsActive.load(std::memory_order_relaxed);
    const HandSlot& s = sim ? (aimPose ? g_simAims[hand] : g_simHands[hand])
                            : (aimPose ? g_aims[hand] : g_hands[hand]);
    if (!s.valid.load(std::memory_order_relaxed)) return false;
    pos3[0] = s.px; pos3[1] = s.py; pos3[2] = s.pz;
    quat4[0] = s.qx; quat4[1] = s.qy; quat4[2] = s.qz; quat4[3] = s.qw;
    return true;
}

void input_set_sim_hand(int hand, bool aimPose, bool valid, const float pos3[3],
                        const float quat4[4]) {
    if (hand < 0 || hand > 1) return;
    HandSlot& s = aimPose ? g_simAims[hand] : g_simHands[hand];
    if (pos3) { s.px = pos3[0]; s.py = pos3[1]; s.pz = pos3[2]; }
    if (quat4) { s.qx = quat4[0]; s.qy = quat4[1]; s.qz = quat4[2]; s.qw = quat4[3]; }
    s.valid.store(valid, std::memory_order_relaxed);
    // Arming ANY slot flips the whole funnel to sim - unset slots read
    // invalid, which is what a faithful replay of an untracked hand wants.
    g_simHandsActive.store(true, std::memory_order_relaxed);
}

void input_clear_sim_hands() {
    g_simHandsActive.store(false, std::memory_order_relaxed);
    for (int h = 0; h < 2; ++h) {
        g_simHands[h].valid.store(false, std::memory_order_relaxed);
        g_simAims[h].valid.store(false, std::memory_order_relaxed);
    }
}

void input_draw_debug_ui() {
    ImGui::Text("xr hands: grip L %s R %s | aim L %s R %s",
                g_hands[0].valid.load(std::memory_order_relaxed) ? "ok" : "-",
                g_hands[1].valid.load(std::memory_order_relaxed) ? "ok" : "-",
                g_aims[0].valid.load(std::memory_order_relaxed) ? "ok" : "-",
                g_aims[1].valid.load(std::memory_order_relaxed) ? "ok" : "-");
    ImGui::Text("xr actions: %s | sync ok %u nf %u fail %u | %s",
                g_attached ? "attached" : (g_created ? "created" : "off"),
                g_syncOk.load(std::memory_order_relaxed),
                g_syncNotFocused.load(std::memory_order_relaxed),
                g_syncFailed.load(std::memory_order_relaxed),
                g_lastActive.load(std::memory_order_relaxed) ? "ACTIVE" : "idle");
}

} // namespace bvr::vr

#endif // BVR_WITH_OPENXR
