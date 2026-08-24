#ifdef BVR_WITH_OPENXR

#include "core/vr/openxr_input.h"

#include "core/input/swing.h"
#include "core/input/xinput_bridge.h"
#include "core/ui/overlay.h"
#include "core/util/log.h"
#include "core/vr/openxr_runtime.h"

#include <windows.h>
#include <Xinput.h> // button bit constants only

#include <cstdio>  // swprintf_s/sprintf_s for the BioshockVR.ini reader

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
// Both-stick-click chord: tap toggles the F10 panel, hold recentres. The gap
// between them is deliberate - a tap that overruns kChordTapMs does nothing at
// all rather than recentring by accident. BOTH ONLY APPLY WHERE
// chord_tap_opens_panel() is on; elsewhere the chord recentres on its rising
// edge as it always did, and neither constant is read.
constexpr uint64_t kChordTapMs = 350;
constexpr uint64_t kChordHoldMs = 600;
// Ammo-slot select: while the d-pad modifier is held, stick directions past
// flick_press_threshold() emit a dpad direction. Zoom is removed, so RS-click
// is free.
//
// DOMINANT-AXIS-ONLY is unconditional and comes from BRVR (InputHook.cpp): a
// thumbstick makes diagonals far too easy to hit when the intent is one
// direction, and the old first-match chain resolved a deliberate "up" as
// "left" depending on which comparison ran first. It is a correctness fix and
// nothing about it is game-specific.
//
// THE 0.65 -> 0.5 THRESHOLD IS NOT, so it moved to the bridge as a settable
// value defaulting to 0.65 - main's number - with BS1 asking for 0.5 from its
// adapter. How far a stick must move before it means something is feel, and
// feel is not portable to two games nobody has tested it on.
// PULSE PATH ONLY (PadMap::flickHold == false - Infinite). See the drive.
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
    // WHICH emitted bits are HELD rather than pulsed. Per DIRECTION, because
    // in this game the two kinds sit side by side on the same d-pad.
    //
    // HOLD is required for the hint button. ENGINE_NOTES' flat-verified pad
    // audit: "BACK=ShowContextHelp, DPAD_RIGHT=hints", and BRVR found (its
    // session 38) that ShockPlayerController gates the MAP SCREEN behind
    // HintButtonHeld with HintHoldTime = 0.5 s. A pulse cannot satisfy a
    // half-second hold, so the map is unreachable by construction without this.
    //
    // PULSE is required for ammo. The same audit: "DPAD_UP and DPAD_DOWN CYCLE
    // the equipped weapon's AMMO TYPE ... flat-proven: 00 Buck -> Electric Buck
    // -> Exploding Buck". A cycle that is held does not settle - it spins. The
    // first cut of this made the whole map hold-or-pulse and would have traded
    // an unreachable map for ammo that machine-guns; the distinction is not
    // per-game, it is per-direction.
    //
    // Infinite holds nothing: its fourth direction drives a nav cycle.
    uint16_t flickHoldBits;
    uint16_t flickUp, flickDown, flickLeft, flickRight;
    // s63: what JUMP is on this game's pad, for the R3-jump lane. BS1/BS2 bind
    // jump to XENON_Y; Infinite's audited retail map puts it on A.
    uint16_t jumpBit;
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
    XINPUT_GAMEPAD_DPAD_RIGHT, // hold the HINT button only - up/down cycle ammo
    XINPUT_GAMEPAD_DPAD_UP, XINPUT_GAMEPAD_DPAD_DOWN, XINPUT_GAMEPAD_DPAD_LEFT,
    // DPAD_RIGHT = hints (ENGINE_NOTES pad audit), and holding it ~0.5 s is
    // how the MAP opens. It shipped as 0 on the belief that BS1 needed only
    // three directions for its ammo types - which cost the map and the
    // context help, not a fourth ammo slot.
    XINPUT_GAMEPAD_DPAD_RIGHT,
    XINPUT_GAMEPAD_Y, // jump
};

// PASSTHROUGH. The game's own pad layout with nothing rearranged: A=Use,
// B=MedHypo, X=Reload/Hack/EVE, Y=Jump, exactly as User.ini XENON_* defines it
// and exactly what the BRVR mod documents as stock controller semantics. This
// is what a player who knows the flat game expects every button to do.
// Everything except the four faces is inherited from the session-19 table,
// including the eaten RS-click - that one is not a preference, the ammo
// modifier needs it. Selected with `profile = passthrough` in BioshockVR.ini.
constexpr PadMap kPadMapPassthrough = {
    "passthrough",
    XINPUT_GAMEPAD_A,            // Touch A -> use / interact / menu confirm
    XINPUT_GAMEPAD_B,            // Touch B -> first aid (med hypo)
    XINPUT_GAMEPAD_X,            // Touch X -> reload / hack / EVE inject
    XINPUT_GAMEPAD_Y,            // Touch Y -> jump
    XINPUT_GAMEPAD_LEFT_SHOULDER, XINPUT_GAMEPAD_RIGHT_SHOULDER,
    XINPUT_GAMEPAD_LEFT_THUMB, 0,
    true, true,
    XINPUT_GAMEPAD_DPAD_RIGHT, // same game, same reason as kPadMapBioshock1
    XINPUT_GAMEPAD_DPAD_UP, XINPUT_GAMEPAD_DPAD_DOWN, XINPUT_GAMEPAD_DPAD_LEFT,
    XINPUT_GAMEPAD_DPAD_RIGHT,
    XINPUT_GAMEPAD_Y, // jump
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
    0, // flickHoldBits: Infinite CYCLES on the fourth direction - a held bit
       // would scroll its nav continuously. Everything stays pulsed.
    XINPUT_GAMEPAD_DPAD_UP, XINPUT_GAMEPAD_DPAD_DOWN, XINPUT_GAMEPAD_DPAD_LEFT,
    XINPUT_GAMEPAD_DPAD_RIGHT,
};

// ---------------------------------------------------------------------------
// s63 USER-OVERRIDABLE PAD MAP - BioshockVR.ini
//
// Ported in spirit from the BRVR mod, whose Keybinds.h makes the same argument
// for keyboard hotkeys: a binding that can only be changed by rebuilding is not
// a binding, it is a decision imposed on every user. The tables below stay the
// DEFAULTS and nothing changes for anyone who does not write the file.
//
// Its own file, deliberately NOT vrpreset.ini: the F10 preset save rewrites
// that wholesale and would silently drop unknown keys - the same reason xr.ini
// is separate. Lives in the per-game data dir, so BS1, BS2 and Infinite each
// get their own control scheme, which they need anyway (their face buttons mean
// different things - see the two tables above).
//
//   [pad]
//   faceA = A        ; what the RIGHT controller's A button sends to the game
//   faceB = Y        ; BS1 default: Touch B -> game Y = jump
//   faceX = X
//   faceY = B        ; BS1 default: Touch Y -> game B = first aid
//   gripL = LB
//   gripR = RB
//   stickClickL = LS
//   stickClickR = NONE   ; BS1: eaten, it IS the ammo modifier
//
// Names: A B X Y LB RB LS RS DUP DDOWN DLEFT DRIGHT START BACK NONE.
// The whole resolved table is logged at init - BRVR's other lesson, that a
// binding you cannot see resolved is one you cannot debug.
struct PadOverride {
    bool loaded = false;
    PadMap map{};
};
PadOverride g_padOverride;

// s63 D-PAD SIDE (BioshockVR.ini [VR] ControllerDpadFlip).
//
//   right (default, unchanged) - LEFT thumbrest modifies, RIGHT stick selects.
//                                Turning is suppressed while selecting; walking
//                                continues. This is what shipped.
//   left                       - RIGHT thumbrest modifies, LEFT stick selects.
//                                Walking is suppressed while selecting; turning
//                                continues. A d-pad lives on the left of a pad,
//                                so for a lot of people this is the one that
//                                matches their hands (user ask, 2026-08-22).
//
// The modifier is ALWAYS the opposite hand from the stick, and that is not a
// preference: one thumb cannot rest on a thumbrest and push that same stick at
// the same time, so the modifier is necessarily cross-hand either way. Flipping
// the stick therefore has to flip the thumbrest with it - which is exactly the
// pairing BRVR's ControllerDpadFlip encodes.
std::atomic<bool> g_dpadLeft{false};

// s63 D-PAD MODIFIER SOURCE (BioshockVR.ini [pad] dpadModifier).
// Ported from BRVR's ControllerDpadModifier, minus its mode 5 (left hand near
// head), which needs head-distance hysteresis and is deferred.
//
// Numbering is BRVR's, so a config carries across unchanged:
//   0 off · 1 right thumbrest · 2 R3 · 3 left grip · 4 left thumbrest
//   5 (left hand near head) is NOT implemented here yet.
//
// Legacy is internal and deliberately NOT settable from the ini - BRVR has no
// such mode and the user does not want one. It is only the pre-s63 heuristic
// (left thumbrest, with R3 standing in until a real thumbrest touch is seen),
// kept as the default so BioShock 2 and Infinite, which have never been tested
// with anything else, are untouched. BioShock 1 picks a real mode in its
// adapter.
//
// BRVR's hardware note carries over: a Rift reports no thumbrest at all, so
// those users need 2. Index/Beyond/Varjo/Somnium need 2 for the opposite
// reason - their thumbrest IS the trackpad, where a thumb naturally rests, and
// the modifier is held.
enum class DpadMod { Legacy = -1, Off = 0, RightRest = 1, R3 = 2, LeftGrip = 3, LeftRest = 4 };
std::atomic<int> g_dpadMod{static_cast<int>(DpadMod::Legacy)};

// s63 R3 -> JUMP, from BRVR. The game binds R3 to Zoom, which is a comfort
// hazard in a headset and is already removed here, so the click is free. It is
// ADDITIVE - the layout's own jump button still jumps - and it yields when R3 is
// carrying the d-pad modifier instead, because otherwise every ammo select
// would also jump.
std::atomic<bool> g_jumpOnR3{true};

// One settled census, scheduled when the controllers first go live. 0 = done.
uint64_t g_settleCensusAtMs = 0;

// s63 MOVEMENT-STICK DEADZONE PRE-COMPENSATION, ported from the BRVR mod.
//
// THE BUG. The game applies its stick deadzone PER AXIS (0.225, from its own
// User.ini bindings), not radially. Anything that ROTATES the movement vector
// moves magnitude between the two axes, so the game's per-axis threshold then
// bends the direction - by up to ~11 degrees - and snaps to a pure sidestep
// once the forward component drops under the band.
//
// WHY IT SHOWS UP HERE. body.cpp's moveDirInstant (default ON) publishes the
// not-yet-transferred body-yaw error every CalcView and rotates the movement
// stick by it, so the walk direction stays instant while the body catches up
// under the slew cap. That rotation is exactly the trigger: the error is only
// non-zero WHILE TURNING, and the stick is only pushed WHILE WALKING - which is
// why the fault appears when doing both at once and never when standing still.
// Reported as smooth turning that repeatedly snaps a few degrees.
//
// THE FIX. Split direction from magnitude, and pre-expand each axis so that
// after the game subtracts its per-axis band what survives is proportional to
// the direction actually asked for. The magnitude is what the player asked for
// and must survive untouched; only the direction is being corrupted.
//
// NOT BY EDITING User.ini, which is BRVR's hard-won note: those are binding
// lines carrying several bindings each (XENON_LTHUMB_XAXIS also holds
// `Axis xLean DeadZone=0.4`), the file has multiple binding sections, and the
// game rewrites it at exit. String surgery there risks breaking the controls
// outright, for a value that can simply be inverted here.
//
// Core default is OFF so BS2 and Infinite are untouched; BS1 opts in.
std::atomic<bool> g_stickPrecomp{false};
std::atomic<float> g_gameStickDeadzone{0.225f};

void precomp_stick_deadzone(float& x, float& y) {
    if (!g_stickPrecomp.load(std::memory_order_relaxed)) return;
    const float d = g_gameStickDeadzone.load(std::memory_order_relaxed);
    if (d <= 0.0f || d >= 0.95f) return;
    const float mag = sqrtf(x * x + y * y);
    if (mag < 1e-4f) return; // centred; leave it alone
    const float ux = x / mag, uy = y / mag;
    const float m = (mag > 1.0f) ? 1.0f : mag;

    // s63 DEVIATION FROM BRVR, and revert it first if this feels wrong.
    //
    // The straight formula adds the whole band `d` the instant an axis leaves
    // zero, so at an axis crossing that axis STEPS from 0 to +-0.225 in one
    // frame - and lands exactly ON the game's threshold, where float rounding
    // can dither between "inside the deadzone" and "just past it" from frame to
    // frame. Walking near-straight while turning sweeps an axis through zero
    // continuously, which is exactly when the residual jitter was reported.
    //
    // Ramping the band in over the first few percent of deflection removes the
    // step without touching the direction anywhere it matters: past kRamp the
    // result is bit-identical to BRVR's.
    constexpr float kRamp = 0.06f;
    auto axis = [&](float u) {
        const float a = fabsf(u);
        if (a < 1e-4f) return 0.0f;
        const float t = (a >= kRamp) ? 1.0f : (a / kRamp);
        return (u < 0.0f ? -1.0f : 1.0f) * (a * m * (1.0f - d) + d * t);
    };
    x = axis(ux);
    y = axis(uy);
}

// s63 TURN RESPONSE, ported from the BRVR mod.
//
// THE BUG. The game's own turn rate is nearly VERTICAL at the top of the stick.
// Measured there: 0.98 -> ~105 deg/s, 0.99 -> ~140, 1.00 -> ~200. A 2%
// difference in how hard the stick is pushed DOUBLES the turn speed, which is
// why turning feels steady while held and different every time it is re-pushed.
//
// FRAME-RATE DEPENDENCE IS FALSIFIED, and was BRVR's first hypothesis too: 40
// samples across 142-239 CalcView calls/s show no correlation. Do not re-derive
// this.
//
// THE FIX. Cap what the game is ever sent so the cliff is unreachable, and the
// same push always turns at the same rate. 0.95 gives roughly 90-110 deg/s.
// That trades top speed for repeatability, which is why it is tunable - and why
// BRVR pairs it with raising the game's OWN sensitivity slider (GameTurnSpeed),
// since a cap alone just makes turning slow.
//
// Exp shapes the rest of the range: 1.0 linear, above 1.0 finer near centre.
//
// Core defaults are the no-op pair (1.0 / 1.0) so BS2 and Infinite are
// untouched; BioShock 1 takes BRVR's 0.95 with the rest of its defaults.
std::atomic<float> g_turnAxisMax{1.0f};
std::atomic<float> g_turnAxisExp{1.0f};

// Applied to the TURN axis only - the right stick's X. Sign-preserving.
float shape_turn_axis(float v) {
    const float mx = g_turnAxisMax.load(std::memory_order_relaxed);
    const float ex = g_turnAxisExp.load(std::memory_order_relaxed);
    if (mx >= 0.999f && ex <= 1.001f && ex >= 0.999f) return v; // exact no-op
    const float sign = v < 0.0f ? -1.0f : 1.0f;
    float a = fabsf(v);
    if (a > 1.0f) a = 1.0f;
    if (ex > 1.001f || ex < 0.999f) a = powf(a, ex);
    return sign * a * mx;
}

uint16_t xinput_bit_by_name(const char* v, bool* ok) {
    struct { const char* name; uint16_t bit; } kNames[] = {
        {"A", XINPUT_GAMEPAD_A},         {"B", XINPUT_GAMEPAD_B},
        {"X", XINPUT_GAMEPAD_X},         {"Y", XINPUT_GAMEPAD_Y},
        {"LB", XINPUT_GAMEPAD_LEFT_SHOULDER},
        {"RB", XINPUT_GAMEPAD_RIGHT_SHOULDER},
        {"LS", XINPUT_GAMEPAD_LEFT_THUMB},
        {"RS", XINPUT_GAMEPAD_RIGHT_THUMB},
        {"DUP", XINPUT_GAMEPAD_DPAD_UP}, {"DDOWN", XINPUT_GAMEPAD_DPAD_DOWN},
        {"DLEFT", XINPUT_GAMEPAD_DPAD_LEFT}, {"DRIGHT", XINPUT_GAMEPAD_DPAD_RIGHT},
        {"START", XINPUT_GAMEPAD_START}, {"BACK", XINPUT_GAMEPAD_BACK},
        {"NONE", 0},
    };
    for (const auto& e : kNames)
        if (_stricmp(v, e.name) == 0) { *ok = true; return e.bit; }
    *ok = false;
    return 0;
}

// Reads one key, leaving the default in place when absent or unrecognised. An
// unrecognised value is LOUD - a typo that silently kept the default is exactly
// the failure this whole mechanism exists to remove.
void pad_key(const wchar_t* ini, const char* key, uint16_t* field) {
    char buf[32] = {};
    wchar_t wkey[32];
    swprintf_s(wkey, L"%hs", key);
    wchar_t wbuf[32] = {};
    GetPrivateProfileStringW(L"VR", wkey, L"", wbuf, 32, ini);
    if (!wbuf[0]) return;
    sprintf_s(buf, "%ls", wbuf);
    bool ok = false;
    const uint16_t bit = xinput_bit_by_name(buf, &ok);
    if (!ok) {
        BVR_LOG("xr-input: BioshockVR.ini [pad] %s = '%s' is not a button name - "
                "keeping the default. Names: A B X Y LB RB LS RS DUP DDOWN DLEFT "
                "DRIGHT START BACK NONE",
                key, buf);
        return;
    }
    *field = bit;
}

const char* xinput_bit_name(uint16_t bit) {
    bool ok = false;
    (void)ok;
    switch (bit) {
        case XINPUT_GAMEPAD_A: return "A";
        case XINPUT_GAMEPAD_B: return "B";
        case XINPUT_GAMEPAD_X: return "X";
        case XINPUT_GAMEPAD_Y: return "Y";
        case XINPUT_GAMEPAD_LEFT_SHOULDER: return "LB";
        case XINPUT_GAMEPAD_RIGHT_SHOULDER: return "RB";
        case XINPUT_GAMEPAD_LEFT_THUMB: return "LS";
        case XINPUT_GAMEPAD_RIGHT_THUMB: return "RS";
        case XINPUT_GAMEPAD_DPAD_UP: return "DUP";
        case XINPUT_GAMEPAD_DPAD_DOWN: return "DDOWN";
        case XINPUT_GAMEPAD_DPAD_LEFT: return "DLEFT";
        case XINPUT_GAMEPAD_DPAD_RIGHT: return "DRIGHT";
        case XINPUT_GAMEPAD_START: return "START";
        case XINPUT_GAMEPAD_BACK: return "BACK";
        default: return "NONE";
    }
}

void pad_map_load_overrides(PadMap base) {
    // Adopt BRVR's shipped control defaults BEFORE the file is read, so the ini
    // still has the last word on every one of them.
    if (bvr::input::pad_brvr_defaults()) {
        g_dpadMod.store(static_cast<int>(DpadMod::RightRest), std::memory_order_relaxed);
        g_dpadLeft.store(true, std::memory_order_relaxed);  // ControllerDpadFlip=0
        g_jumpOnR3.store(true, std::memory_order_relaxed);
        g_turnAxisMax.store(0.95f, std::memory_order_relaxed);
        g_turnAxisExp.store(1.0f, std::memory_order_relaxed);
        g_stickPrecomp.store(true, std::memory_order_relaxed);
    }

    wchar_t ini[MAX_PATH];
    swprintf_s(ini, L"%s\\BioshockVR.ini", bvr::log::data_dir());
    if (GetFileAttributesW(ini) == INVALID_FILE_ATTRIBUTES) {
        // No file is the normal case. Still say what the defaults resolved to
        // when they are not the historical ones, so "why is my d-pad on the
        // left stick" has an answer in the log rather than only in a commit.
        if (bvr::input::pad_brvr_defaults())
            BVR_LOG("xr-input: no BioshockVR.ini - defaults: face buttons passthrough, "
                    "ControllerDpadModifier=1 (right thumbrest), ControllerDpadFlip=0 "
                    "(left stick selects), JumpOnR3=1");
        return;
    }

    // A named profile replaces the base wholesale; individual keys below then
    // override on top of whichever profile was chosen. So `profile =
    // passthrough` alone is a complete answer, and a profile plus one key is
    // still a complete answer.
    {
        wchar_t wprof[32] = {};
        GetPrivateProfileStringW(L"VR", L"FaceLayout", L"", wprof, 32, ini);
        if (wprof[0]) {
            char prof[32] = {};
            sprintf_s(prof, "%ls", wprof);
            if (_stricmp(prof, "passthrough") == 0) {
                base = kPadMapPassthrough;
            } else if (_stricmp(prof, "default") == 0 || _stricmp(prof, "session19") == 0) {
                // Explicitly the shipped table. Named so reverting is one word.
            } else {
                BVR_LOG("xr-input: BioshockVR.ini [pad] profile = '%s' is not a profile - "
                        "using the shipped default. Profiles: default (a.k.a. session19), "
                        "passthrough",
                        prof);
            }
        }
    }

    // BRVR key names and numbering, so a BioshockVR.ini carries across.
    {
        const int md = GetPrivateProfileIntW(L"VR", L"ControllerDpadModifier", -1, ini);
        if (md == 5) {
            BVR_LOG("xr-input: ControllerDpadModifier=5 (left hand near head) is not "
                    "implemented yet - falling back to 1 (right thumbrest)");
            g_dpadMod.store(static_cast<int>(DpadMod::RightRest), std::memory_order_relaxed);
        } else if (md >= 0 && md <= 4) {
            g_dpadMod.store(md, std::memory_order_relaxed);
        } else if (md != -1) {
            BVR_LOG("xr-input: ControllerDpadModifier=%d is out of range - keeping the "
                    "current setting. 0 off, 1 right thumbrest, 2 R3, 3 left grip, "
                    "4 left thumbrest",
                    md);
        }
    }
    {
        const int flip = GetPrivateProfileIntW(L"VR", L"ControllerDpadFlip", -1, ini);
        if (flip == 0) g_dpadLeft.store(true, std::memory_order_relaxed);   // LEFT stick
        else if (flip == 1) g_dpadLeft.store(false, std::memory_order_relaxed);
        else if (flip != -1)
            BVR_LOG("xr-input: ControllerDpadFlip=%d is not 0 or 1 - keeping the current "
                    "setting",
                    flip);
    }
    {
        // Floats through the string reader - GetPrivateProfileInt cannot do
        // decimals, and 0.95 is the whole point.
        wchar_t wv[24] = {};
        GetPrivateProfileStringW(L"VR", L"TurnAxisMax", L"", wv, 24, ini);
        if (wv[0]) {
            const float v = static_cast<float>(_wtof(wv));
            if (v > 0.05f && v <= 1.0f) g_turnAxisMax.store(v, std::memory_order_relaxed);
            else BVR_LOG("xr-input: TurnAxisMax must be between 0.05 and 1.0 - ignoring");
        }
        GetPrivateProfileStringW(L"VR", L"GameStickDeadzone", L"", wv, 24, ini);
        if (wv[0]) {
            const float v = static_cast<float>(_wtof(wv));
            if (v >= 0.0f && v < 0.95f) g_gameStickDeadzone.store(v, std::memory_order_relaxed);
            else BVR_LOG("xr-input: GameStickDeadzone must be 0.0 to 0.95 - ignoring");
        }
        GetPrivateProfileStringW(L"VR", L"TurnAxisExp", L"", wv, 24, ini);
        if (wv[0]) {
            const float v = static_cast<float>(_wtof(wv));
            if (v >= 0.5f && v <= 4.0f) g_turnAxisExp.store(v, std::memory_order_relaxed);
            else BVR_LOG("xr-input: TurnAxisExp must be between 0.5 and 4.0 - ignoring");
        }
    }
    g_stickPrecomp.store(
        GetPrivateProfileIntW(L"VR", L"StickPrecomp",
                              g_stickPrecomp.load(std::memory_order_relaxed) ? 1 : 0,
                              ini) != 0,
        std::memory_order_relaxed);
    g_jumpOnR3.store(
        GetPrivateProfileIntW(L"VR", L"JumpOnR3",
                              g_jumpOnR3.load(std::memory_order_relaxed) ? 1 : 0, ini) != 0,
        std::memory_order_relaxed);

    PadMap m = base;
    pad_key(ini, "FaceA", &m.faceA);
    pad_key(ini, "FaceB", &m.faceB);
    pad_key(ini, "FaceX", &m.faceX);
    pad_key(ini, "FaceY", &m.faceY);
    pad_key(ini, "GripL", &m.gripL);
    pad_key(ini, "GripR", &m.gripR);
    pad_key(ini, "StickClickL", &m.stickClickL);
    pad_key(ini, "StickClickR", &m.stickClickR);
    pad_key(ini, "FlickUp", &m.flickUp);
    pad_key(ini, "FlickDown", &m.flickDown);
    pad_key(ini, "FlickLeft", &m.flickLeft);
    pad_key(ini, "FlickRight", &m.flickRight);

    g_padOverride.map = m;
    g_padOverride.loaded = true;
    BVR_LOG("xr-input: BioshockVR.ini loaded - pad map '%s' resolved to "
            "A=%s B=%s X=%s Y=%s | gripL=%s gripR=%s | stickL=%s stickR=%s | "
            "flick U=%s D=%s L=%s R=%s",
            m.name, xinput_bit_name(m.faceA), xinput_bit_name(m.faceB),
            xinput_bit_name(m.faceX), xinput_bit_name(m.faceY),
            xinput_bit_name(m.gripL), xinput_bit_name(m.gripR),
            xinput_bit_name(m.stickClickL), xinput_bit_name(m.stickClickR),
            xinput_bit_name(m.flickUp), xinput_bit_name(m.flickDown),
            xinput_bit_name(m.flickLeft), xinput_bit_name(m.flickRight));
    BVR_LOG("xr-input: BioshockVR.ini d-pad side = %s (%s thumbrest modifies, %s stick "
            "selects; %s is suppressed while selecting)",
            g_dpadLeft.load(std::memory_order_relaxed) ? "LEFT" : "right",
            g_dpadLeft.load(std::memory_order_relaxed) ? "right" : "left",
            g_dpadLeft.load(std::memory_order_relaxed) ? "left" : "right",
            g_dpadLeft.load(std::memory_order_relaxed) ? "walking" : "turning");
    {
        const int md = g_dpadMod.load(std::memory_order_relaxed);
        const char* mn = md == static_cast<int>(DpadMod::Off)      ? "off"
                         : md == static_cast<int>(DpadMod::RightRest) ? "rightrest"
                         : md == static_cast<int>(DpadMod::R3)        ? "r3"
                         : md == static_cast<int>(DpadMod::LeftGrip)  ? "leftgrip"
                         : md == static_cast<int>(DpadMod::LeftRest)  ? "leftrest"
                                                                     : "legacy heuristic";
        BVR_LOG("xr-input: BioshockVR.ini d-pad modifier = %s | R3 jump = %s | "
                "TurnAxisMax %.2f exp %.2f",
                mn, g_jumpOnR3.load(std::memory_order_relaxed) ? "on" : "off",
                g_turnAxisMax.load(std::memory_order_relaxed),
                g_turnAxisExp.load(std::memory_order_relaxed));
        BVR_LOG("xr-input: StickPrecomp %s (game per-axis deadzone %.3f) - undoes the "
                "direction bend when the movement stick is rotated while turning",
                g_stickPrecomp.load(std::memory_order_relaxed) ? "on" : "off",
                g_gameStickDeadzone.load(std::memory_order_relaxed));
        // A thumbrest cannot modify the stick its own thumb has to push.
        const bool leftSel = g_dpadLeft.load(std::memory_order_relaxed);
        if ((leftSel && md == static_cast<int>(DpadMod::LeftRest)) ||
            (!leftSel && md == static_cast<int>(DpadMod::RightRest)))
            BVR_LOG("xr-input: WARNING - dpadModifier '%s' is on the SAME hand as the "
                    "selecting stick. One thumb cannot rest on the thumbrest and push "
                    "that stick at once; use r3 or leftgrip, or flip dpadSide.",
                    mn);
        if (md == static_cast<int>(DpadMod::R3) && g_jumpOnR3.load(std::memory_order_relaxed))
            BVR_LOG("xr-input: R3 is the d-pad modifier, so the R3 jump lane yields to it "
                    "- the layout's own jump button is unaffected");
    }
}

// The compiled base for the current game, before BioshockVR.ini is applied.
const PadMap& compiled_pad_map() {
    if (bvr::input::pad_profile() == bvr::input::PadProfile::Infinite) return kPadMapInfinite;
    return bvr::input::pad_passthrough_default() ? kPadMapPassthrough : kPadMapBioshock1;
}

const PadMap& active_pad_map() {
    if (g_padOverride.loaded) return g_padOverride.map;
    return compiled_pad_map();
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

// s50 (Infinite): the FLOURISH CHORD - left thumbrest touched + A pressed.
// Armed by the adapter only (default off - BS1/BS2 composers byte-identical).
// While the rest is touched, A is consumed (jump never fires under the
// chord) and each rising A edge bumps the counter the adapter polls on the
// game thread. Left thumbrest for the same reason the flick modifier uses
// it: the chord is necessarily cross-hand (A sits under the right thumb).
std::atomic<bool> g_flourishChordArmed{false};
// s52: cinematic-scoped suspension - while set, the chord neither consumes A
// nor counts edges, so an interactive prompt's confirm press reaches the
// game (the raffle lesson). Set/cleared by the adapter's cinematic gate.
std::atomic<bool> g_flourishChordSuspended{false};
std::atomic<uint32_t> g_flourishChordEdges{0};
bool g_chordAWasDown = false;
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

// s63: WHICH actions are live, by name. A dead stick and a centred stick are
// the same zeros to read_vec2(), so "the left stick did nothing while A worked"
// cannot be diagnosed from behaviour - isActive is the only thing that
// separates a binding the runtime never resolved from a genuinely idle input.
// Log-only; called once when the controllers first go live and again if any
// action's liveness later changes.
bool action_is_active(XrSession session, XrAction a, int type) {
    if (!a) return false;
    XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};
    gi.action = a;
    if (type == 0) {
        XrActionStateBoolean st{XR_TYPE_ACTION_STATE_BOOLEAN};
        return XR_SUCCEEDED(xrGetActionStateBoolean(session, &gi, &st)) && st.isActive;
    }
    if (type == 1) {
        XrActionStateFloat st{XR_TYPE_ACTION_STATE_FLOAT};
        return XR_SUCCEEDED(xrGetActionStateFloat(session, &gi, &st)) && st.isActive;
    }
    XrActionStateVector2f st{XR_TYPE_ACTION_STATE_VECTOR2F};
    return XR_SUCCEEDED(xrGetActionStateVector2f(session, &gi, &st)) && st.isActive;
}

uint32_t action_liveness_mask(XrSession session) {
    uint32_t m = 0;
    if (action_is_active(session, g_move, 2)) m |= 1u << 0;
    if (action_is_active(session, g_look, 2)) m |= 1u << 1;
    if (action_is_active(session, g_fire, 1)) m |= 1u << 2;
    if (action_is_active(session, g_plasmid, 1)) m |= 1u << 3;
    if (action_is_active(session, g_gripL, 1)) m |= 1u << 4;
    if (action_is_active(session, g_gripR, 1)) m |= 1u << 5;
    if (action_is_active(session, g_btnA, 0)) m |= 1u << 6;
    if (action_is_active(session, g_btnB, 0)) m |= 1u << 7;
    if (action_is_active(session, g_btnX, 0)) m |= 1u << 8;
    if (action_is_active(session, g_btnY, 0)) m |= 1u << 9;
    if (action_is_active(session, g_stickClickL, 0)) m |= 1u << 10;
    if (action_is_active(session, g_stickClickR, 0)) m |= 1u << 11;
    return m;
}

void log_action_census(XrSession session, const char* why) {
    const uint32_t m = action_liveness_mask(session);
    BVR_LOG("xr-input: action census (%s): move=%d look=%d fire=%d plasmid=%d gripL=%d "
            "gripR=%d A=%d B=%d X=%d Y=%d stickL=%d stickR=%d",
            why, (m >> 0) & 1, (m >> 1) & 1, (m >> 2) & 1, (m >> 3) & 1, (m >> 4) & 1,
            (m >> 5) & 1, (m >> 6) & 1, (m >> 7) & 1, (m >> 8) & 1, (m >> 9) & 1,
            (m >> 10) & 1, (m >> 11) & 1);
    // MEASURED 2026-08-22: when this fires it is never one binding. Every
    // LEFT-hand action goes dead together (move, left trigger, left grip, X, Y,
    // left stick click) while every right-hand one stays live, which is a
    // controller with no interaction profile rather than a binding we got
    // wrong - our suggestions for those six paths could not fail as a set.
    const uint32_t kLeft = (1u << 0) | (1u << 3) | (1u << 4) | (1u << 8) | (1u << 9) | (1u << 10);
    const uint32_t kRight = (1u << 1) | (1u << 2) | (1u << 5) | (1u << 6) | (1u << 7) | (1u << 11);
    const bool leftDead = (m & kLeft) == 0;
    const bool rightDead = (m & kRight) == 0;
    if (leftDead && !rightDead)
        BVR_LOG("xr-input: THE WHOLE LEFT CONTROLLER IS UNBOUND (every left action "
                "inactive, right hand fine). Not a stick fault - the runtime has no "
                "interaction profile for that hand. Wake or re-pair the left controller; "
                "it usually means it was asleep when the session started.");
    else if (rightDead && !leftDead)
        BVR_LOG("xr-input: THE WHOLE RIGHT CONTROLLER IS UNBOUND (every right action "
                "inactive, left hand fine). Wake or re-pair the right controller.");
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

    // SteamVR-family profiles (s62). All best-effort like touch: a runtime that
    // rejects a profile only loses that hardware, never the session. Boolean
    // inputs (squeeze/click on Vive/WMR) bound to FLOAT actions are converted
    // to 0.0/1.0 by the runtime per spec 11.4 - the grip hysteresis
    // (0.70/0.55) latches at 1.0 and releases at 0.0, so no read path changes.
    // Unbound actions simply read inactive (read_float/read_bool return 0).

    // Valve Index. A/B on BOTH hands (left has no x/y paths), analog squeeze,
    // no menu button (system/click is reserved - never bind), no thumbrest:
    // the noThumbrestYet fallback keeps RS-click as the ammo modifier, and
    // menu goes to a firm left-trackpad press (boolean action on the float
    // force component - the runtime thresholds it).
    XrActionSuggestedBinding index[] = {
        {g_move, path(instance, "/user/hand/left/input/thumbstick")},
        {g_look, path(instance, "/user/hand/right/input/thumbstick")},
        {g_fire, path(instance, "/user/hand/right/input/trigger/value")},
        {g_plasmid, path(instance, "/user/hand/left/input/trigger/value")},
        {g_gripR, path(instance, "/user/hand/right/input/squeeze/value")},
        {g_gripL, path(instance, "/user/hand/left/input/squeeze/value")},
        {g_btnA, path(instance, "/user/hand/right/input/a/click")},
        {g_btnB, path(instance, "/user/hand/right/input/b/click")},
        {g_btnX, path(instance, "/user/hand/left/input/a/click")},
        {g_btnY, path(instance, "/user/hand/left/input/b/click")},
        {g_stickClickL, path(instance, "/user/hand/left/input/thumbstick/click")},
        {g_stickClickR, path(instance, "/user/hand/right/input/thumbstick/click")},
        {g_menu, path(instance, "/user/hand/left/input/trackpad/force")},
        {g_poseL, path(instance, "/user/hand/left/input/grip/pose")},
        {g_poseR, path(instance, "/user/hand/right/input/grip/pose")},
        {g_aimL, path(instance, "/user/hand/left/input/aim/pose")},
        {g_aimR, path(instance, "/user/hand/right/input/aim/pose")},
    };
    sb.interactionProfile = path(instance, "/interaction_profiles/valve/index_controller");
    sb.suggestedBindings = index;
    sb.countSuggestedBindings = static_cast<uint32_t>(sizeof(index) / sizeof(index[0]));
    r = xrSuggestInteractionProfileBindings(instance, &sb);
    if (XR_FAILED(r))
        BVR_LOG("xr-input: index_controller binding suggestion failed (%d)",
                static_cast<int>(r));

    // HTC Vive wands: trigger, squeeze CLICK only, menu on both hands, and a
    // trackpad standing in for both sticks. No face buttons exist, so jump/
    // heal/reload (btn_b/x/y) stay unbound natively - the SteamVR shim's
    // binding JSONs plus SteamVR's own binding UI are the full-coverage path.
    // stick_r = right trackpad click is eaten as the ammo modifier on BS1/BS2,
    // which suits a trackpad: press the pad, touch direction picks the slot.
    XrActionSuggestedBinding vive[] = {
        {g_move, path(instance, "/user/hand/left/input/trackpad")},
        {g_look, path(instance, "/user/hand/right/input/trackpad")},
        {g_fire, path(instance, "/user/hand/right/input/trigger/value")},
        {g_plasmid, path(instance, "/user/hand/left/input/trigger/value")},
        {g_gripR, path(instance, "/user/hand/right/input/squeeze/click")},
        {g_gripL, path(instance, "/user/hand/left/input/squeeze/click")},
        {g_btnA, path(instance, "/user/hand/right/input/menu/click")},
        {g_stickClickL, path(instance, "/user/hand/left/input/trackpad/click")},
        {g_stickClickR, path(instance, "/user/hand/right/input/trackpad/click")},
        {g_menu, path(instance, "/user/hand/left/input/menu/click")},
        {g_poseL, path(instance, "/user/hand/left/input/grip/pose")},
        {g_poseR, path(instance, "/user/hand/right/input/grip/pose")},
        {g_aimL, path(instance, "/user/hand/left/input/aim/pose")},
        {g_aimR, path(instance, "/user/hand/right/input/aim/pose")},
    };
    sb.interactionProfile = path(instance, "/interaction_profiles/htc/vive_controller");
    sb.suggestedBindings = vive;
    sb.countSuggestedBindings = static_cast<uint32_t>(sizeof(vive) / sizeof(vive[0]));
    r = xrSuggestInteractionProfileBindings(instance, &sb);
    if (XR_FAILED(r))
        BVR_LOG("xr-input: vive_controller binding suggestion failed (%d)",
                static_cast<int>(r));
    else
        BVR_LOG("xr-input: vive wands have no face buttons - jump/heal/reload "
                "unbound natively; use the SteamVR shim + binding UI for full "
                "wand support");

    // WMR motion controllers: thumbstick AND trackpad, squeeze click, menu on
    // both hands, no face buttons. Trackpad clicks stand in for A (use) and X;
    // B/Y stay unbound (same advisory as Vive). Never hardware-tested - the
    // SteamVR binding UI is the correction mechanism.
    XrActionSuggestedBinding wmr[] = {
        {g_move, path(instance, "/user/hand/left/input/thumbstick")},
        {g_look, path(instance, "/user/hand/right/input/thumbstick")},
        {g_fire, path(instance, "/user/hand/right/input/trigger/value")},
        {g_plasmid, path(instance, "/user/hand/left/input/trigger/value")},
        {g_gripR, path(instance, "/user/hand/right/input/squeeze/click")},
        {g_gripL, path(instance, "/user/hand/left/input/squeeze/click")},
        {g_btnA, path(instance, "/user/hand/right/input/trackpad/click")},
        {g_btnX, path(instance, "/user/hand/left/input/trackpad/click")},
        {g_stickClickL, path(instance, "/user/hand/left/input/thumbstick/click")},
        {g_stickClickR, path(instance, "/user/hand/right/input/thumbstick/click")},
        {g_menu, path(instance, "/user/hand/left/input/menu/click")},
        {g_poseL, path(instance, "/user/hand/left/input/grip/pose")},
        {g_poseR, path(instance, "/user/hand/right/input/grip/pose")},
        {g_aimL, path(instance, "/user/hand/left/input/aim/pose")},
        {g_aimR, path(instance, "/user/hand/right/input/aim/pose")},
    };
    sb.interactionProfile =
        path(instance, "/interaction_profiles/microsoft/motion_controller");
    sb.suggestedBindings = wmr;
    sb.countSuggestedBindings = static_cast<uint32_t>(sizeof(wmr) / sizeof(wmr[0]));
    r = xrSuggestInteractionProfileBindings(instance, &sb);
    if (XR_FAILED(r))
        BVR_LOG("xr-input: motion_controller binding suggestion failed (%d)",
                static_cast<int>(r));

    g_created = true;
    // s63: apply BioshockVR.ini on top of the compiled default for THIS game.
    // After the profile is known, so the override lands on the right base.
    pad_map_load_overrides(compiled_pad_map());

    BVR_LOG("xr-input: action set ready (%d actions; touch + simple + index + "
            "vive + wmr bindings suggested)", made);
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

    // s63: re-census on any CHANGE in which actions are live - that is how a
    // stick that dies mid-session, or comes back, gets a timestamp.
    {
        static uint32_t lastMask = 0xFFFFFFFFu;
        static uint64_t lastCensusMs = 0;
        const uint64_t nowMs = GetTickCount64();
        if (g_settleCensusAtMs && nowMs >= g_settleCensusAtMs) {
            g_settleCensusAtMs = 0;
            log_action_census(session, "settled");
        }
        if (nowMs - lastCensusMs >= 500) {
            lastCensusMs = nowMs;
            const uint32_t m = action_liveness_mask(session);
            if (lastMask != 0xFFFFFFFFu && m != lastMask) log_action_census(session, "changed");
            lastMask = m;
        }
    }

    // s63 BOOT INPUT DEAD WINDOW - measurement only, nothing here changes
    // behaviour for any game. xrSyncActions succeeding does NOT mean the
    // controllers are usable: until the runtime binds an interaction profile
    // every action reads isActive=false, so buttons and sticks do nothing while
    // the mod looks healthy. MEASURED 2026-08-22 on VDXR, same build, two boots:
    // the first profile bind landed 0.11 s after attach on one run and 11.14 s
    // on the next, which is the reported "thumbstick dead for several seconds"
    // and "A button did nothing for ten seconds". The runtime owns that timing,
    // so this records WHEN it ends rather than trying to force it.
    {
        static bool everActive = false;
        static uint64_t firstSyncMs = 0;
        if (!everActive) {
            const uint64_t nowMs = GetTickCount64();
            if (!firstSyncMs) firstSyncMs = nowMs;
            XrActionStateBoolean probe{XR_TYPE_ACTION_STATE_BOOLEAN};
            XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};
            // g_btnA is the exact action one report named, so probe it rather
            // than a proxy.
            gi.action = g_btnA;
            if (gi.action && XR_SUCCEEDED(xrGetActionStateBoolean(session, &gi, &probe)) &&
                probe.isActive) {
                everActive = true;
                BVR_LOG("xr-input: controllers LIVE - first active action %llu ms after the "
                        "first successful sync (before this, every button and stick reads "
                        "inactive however healthy the mod looks)",
                        static_cast<unsigned long long>(nowMs - firstSyncMs));
                log_action_census(session, "at first live");
                // The runtime's own profile-changed event lands AFTER this
                // (measured: 61 ms), so the first-live census is a snapshot
                // taken mid-binding. Take another once it has settled, and let
                // the change-watcher below carry it from there.
                g_settleCensusAtMs = GetTickCount64() + 1500;
            } else if (nowMs - firstSyncMs >= 2000) {
                static uint64_t lastWarnMs = 0;
                if (nowMs - lastWarnMs >= 2000) {
                    lastWarnMs = nowMs;
                    BVR_LOG("xr-input: no interaction profile bound yet - %llu ms of dead "
                            "controllers so far (runtime has not rebound; wake or move the "
                            "controllers)",
                            static_cast<unsigned long long>(nowMs - firstSyncMs));
                }
            }
        }
    }

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
    // AFTER our own deadzone, so ours is not re-expanded by the pre-comp.
    precomp_stick_deadzone(mx, my);
    lx = shape_turn_axis(lx); // turn only; Y is pitch and is left alone
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
    // s62c: LEFT X+Y PRESSED TOGETHER = the menu button. Under Steam Link the
    // physical menu press is sometimes swallowed by the Steam overlay before
    // it reaches the game (user report 2026-08-14; the donor project shipped
    // the same chord for the same reason on its shim path). Detected on the
    // raw booleans; the chord feeds the SAME tap/hold menu lane below (tap =
    // START pulse on release, hold = BACK), so pause semantics stay identical
    // to the real button. The suppression LATCHES until BOTH buttons release,
    // so the button released second cannot fire its game action on the way
    // out. The single-frame leak of the first-pressed button before the
    // second joins is accepted - same class as the recenter chord's
    // documented leak.
    const bool btnXDown = read_bool(session, g_btnX);
    const bool btnYDown = read_bool(session, g_btnY);
    const bool menuChord = btnXDown && btnYDown;
    static bool s_menuChordLatch = false;
    if (menuChord) {
        if (!s_menuChordLatch) {
            static bool s_chordTold = false;
            if (!s_chordTold) {
                s_chordTold = true;
                BVR_LOG("xr-input: left X+Y menu chord fired (pause without "
                        "the menu button - Steam Link overlay workaround)");
            }
        }
        s_menuChordLatch = true;
    } else if (!btnXDown && !btnYDown) {
        s_menuChordLatch = false;
    }
    if (!s_menuChordLatch) {
        if (btnXDown) pad.buttons |= map.faceX;
        if (btnYDown) pad.buttons |= map.faceY;
    }
    // Feedback session 2 (2026-08-13): BOTH-STICKS-CLICK = recenter chord.
    // Detected here on the RAW clicks - on BS1/BS2 the right click is eaten
    // as the ammo modifier and never reaches the composed pad, so the bridge
    // cannot see the chord. One edge per chord; re-arms only after BOTH
    // release. While the chord is held neither click bit reaches the game
    // (and the ammo-modifier read below is suppressed), so the recenter
    // cannot also sprint/zoom/select ammo. The single-frame leak of the
    // first-pressed click before the second joins is accepted - same class
    // as the documented radial-grip leak.
    const bool clickL = read_bool(session, g_stickClickL);
    const bool clickRraw = read_bool(session, g_stickClickR);
    const bool chordHeld = clickL && clickRraw;
    // 2026-08-22: the chord now carries TWO jobs, split by duration.
    //   TAP  (release under kChordTapMs)  -> toggle the F10 panel
    //   HOLD (past kChordHoldMs)          -> recenter, once, on the way past
    // Recenter keeps the chord because it is the one thing you must be able to
    // do when the view is already wrong; it takes the HOLD because it is rare
    // and deliberate, while opening the panel is neither. A hold that has
    // already recentered must NOT also toggle on release, hence s_chordFired.
    //
    // THE SPLIT IS BS1-ONLY UNTIL TESTED (VOID's review of PR 50). It changes
    // the chord for every game: recenter stops being instant and waits out
    // kChordHoldMs, and a short chord now opens a panel that BS2 and Infinite
    // cannot drive with a controller anyway (see overlay_pad_drive). With the
    // gate off this is main's machine verbatim - one recenter on the chord's
    // rising edge, re-armed only when both clicks are up.
    static uint64_t s_chordDownMs = 0;
    static bool s_chordFired = false; // this hold already recentred
    static bool s_chordArmed = true;  // legacy path only
    const uint64_t nowChord = GetTickCount64();
    if (!bvr::input::chord_tap_opens_panel()) {
        if (chordHeld && s_chordArmed) {
            s_chordArmed = false;
            bvr::input::queue_recenter_chord();
        } else if (!clickL && !clickRraw) {
            s_chordArmed = true;
        }
    } else if (chordHeld) {
        if (s_chordDownMs == 0) s_chordDownMs = nowChord;
        if (!s_chordFired && nowChord - s_chordDownMs >= kChordHoldMs) {
            s_chordFired = true;
            bvr::input::queue_recenter_chord();
        }
    } else if (s_chordDownMs != 0) {
        // Released. Both clicks are up by construction of chordHeld going
        // false only when at least one is - require BOTH up before re-arming,
        // so rolling off one click and back on is not a second gesture.
        if (!clickL && !clickRraw) {
            if (!s_chordFired && nowChord - s_chordDownMs < kChordTapMs)
                bvr::overlay::set_visible(!bvr::overlay::visible());
            s_chordDownMs = 0;
            s_chordFired = false;
        }
    }
    if (!chordHeld) {
        if (clickL) pad.buttons |= map.stickClickL;
        // s63, from BRVR: R3's precedence is MODIFIER > JUMP > passthrough.
        // Whichever wins, the other two must not also fire - otherwise every
        // ammo select would jump, or every jump would zoom.
        const int dpadModNow = g_dpadMod.load(std::memory_order_relaxed);
        // R3 only counts as the modifier when it IS the modifier. The legacy
        // heuristic can fall back to R3 at runtime, so it counts too - but a
        // real mode (including 1/3/4) frees the click, which is what makes the
        // jump lane reachable at all. This is the line that was swallowing it.
        const bool r3IsModifier =
            map.flick && (dpadModNow == static_cast<int>(DpadMod::R3) ||
                          (dpadModNow == static_cast<int>(DpadMod::Legacy) &&
                           map.flickAmmoModPref));
        // ...and it can only CLAIM the click on a game whose map actually has a
        // jump bit. Infinite's does not (its initializer stops at flickRight, so
        // jumpBit value-initialises to 0) and it forwards RS-click for
        // XToggleZoom instead. Without this guard the lane won the click, emitted
        // nothing, and then blocked the forward below - so s63 silently ate
        // Infinite's zoom on a global default it never opted into.
        const bool r3IsJump = !r3IsModifier && map.jumpBit &&
                              g_jumpOnR3.load(std::memory_order_relaxed);
        if (r3IsJump && clickRraw && map.jumpBit) pad.buttons |= map.jumpBit;
        // Forwarded only where the map says so, and only when the click is
        // carrying neither of the two jobs above.
        if (map.stickClickR && clickRraw && !r3IsModifier && !r3IsJump)
            pad.buttons |= map.stickClickR;
    }

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
    // Set by the flick block below; read by the menu lane after it.
    bool modHeldForMenu = false;
    if (map.flick) {
        // s63: which stick selects. Default right (shipped); `dpadSide = left`
        // moves it to the movement stick, where a d-pad normally lives.
        const bool dpadLeft = g_dpadLeft.load(std::memory_order_relaxed);
        float rawX = 0.0f, rawY = 0.0f;
        read_vec2(session, dpadLeft ? g_move : g_look, &rawX, &rawY);
        bool gripHeld = g_gripLatchedL || g_gripLatchedR;

        // Session 23: the modifier can also be the LEFT thumbrest. It has to be
        // the left one - the right thumb cannot rest on the right thumbrest and
        // push the right stick at the same time, so a thumbrest modifier is
        // necessarily cross-hand. Slot directions stay on the right stick, so
        // nobody's muscle memory changes.
        const bool rsClick = clickRraw && !chordHeld;
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
        // Auto's cross-hand default, before either branch below refines it.
        bool restMod = dpadLeft ? restR : restL;
        const int dpadMod = g_dpadMod.load(std::memory_order_relaxed);
        if (dpadMod != static_cast<int>(DpadMod::Legacy)) {
            // Explicit choice: no heuristic, no fallback. The user picked a
            // source and it is the only one that arms.
            switch (static_cast<DpadMod>(dpadMod)) {
                case DpadMod::Off:       clickMod = false; restMod = false; break;
                case DpadMod::RightRest: clickMod = false; restMod = restR; break;
                case DpadMod::LeftRest:  clickMod = false; restMod = restL; break;
                case DpadMod::R3:        clickMod = rsClick; restMod = false; break;
                case DpadMod::LeftGrip:  clickMod = false; restMod = g_gripLatchedL; break;
                default: break;
            }
        } else if (map.flickAmmoModPref) {
            const bvr::input::AmmoMod mode = bvr::input::ammo_mod();
            // Thumbrest is the default, but not every controller has one (Pico
            // has no thumbrest; some SteamVR setups do not report it) and
            // losing ammo select entirely would be a nasty surprise. So in
            // Thumbrest mode the stick click keeps working UNTIL a real
            // thumbrest touch is observed - after that the mapping is exactly
            // what was chosen.
            // Watch the hand that actually carries the modifier for this side.
            const bool noThumbrestYet = !g_thumbrestSeen[dpadLeft ? 1 : 0];
            clickMod = (mode != bvr::input::AmmoMod::Thumbrest || noThumbrestYet) && rsClick;
            // Cross-hand, always: the modifier thumbrest is on the opposite
            // hand from the selecting stick, because one thumb cannot rest and
            // push the same stick at once.
            restMod = mode != bvr::input::AmmoMod::Click && (dpadLeft ? restR : restL);
        }
        const bool modHeld = clickMod || restMod;
        // The menu lane below needs to know whether the modifier is down: it is
        // what turns the menu button from PAUSE into CONTEXT HELP. Published
        // here rather than recomputed, so the two can never disagree.
        modHeldForMenu = modHeld;

        if (modHeld && !g_rsClickWasDown) g_flickArmed = true;
        g_rsClickWasDown = modHeld;
        if (modHeld && !gripHeld) {
            // DOMINANT AXIS ONLY, BRVR's exact test. A thumbstick makes
            // diagonals far too easy to hit when one direction was meant, and
            // the previous first-match chain resolved a deliberate "up" as
            // "left" whenever the cross-axis happened to be over threshold too.
            // Directions the map leaves at 0 are never emitted - that is how
            // BS1 keeps a three-way select while Infinite gets its fourth.
            //
            // THE FOURTH DIRECTION AND THE HOLD ARE BS1-ONLY UNTIL TESTED.
            // kPadMapBioshock1 serves BioShock 1 AND BioShock 2 - PadProfile has
            // no Bioshock2 entry and no BS2 adapter selects one - so the table's
            // flickRight/flickHoldBits reach a game that has never been in a
            // headset with them. Before s63 both were absent. Read them through
            // the gate rather than editing the tables, which keeps the tables an
            // honest statement of what each game's pad MEANS and puts the
            // untested-ness in one place. (VOID's review of PR 50.)
            const bool fourth = bvr::input::flick_fourth_direction();
            const uint16_t mapRight = fourth ? map.flickRight : 0;
            const uint16_t holdBits = fourth ? map.flickHoldBits : 0;
            const float press = bvr::input::flick_press_threshold();
            const float ax = fabsf(rawX), ay = fabsf(rawY);
            uint16_t bit = 0;
            if (ay >= press && ay >= ax) bit = rawY > 0.0f ? map.flickUp : map.flickDown;
            else if (ax >= press && ax > ay)
                bit = rawX < 0.0f ? map.flickLeft : mapRight;

            // Turn/walk suppression. RS-click never reaches the game, so
            // killing the stick for its whole hold costs nothing there. A thumb
            // PARKED on a thumbrest is not a deliberate gesture though -
            // suppressing for as long as it rests would break normal play - so
            // in thumbrest mode only suppress while a direction is actually
            // resolved. Suppress the stick doing the SELECTING, so the gesture
            // does not also walk (left) or turn (right).
            if (clickMod || bit) {
                if (dpadLeft) {
                    pad.lx = 0;
                    pad.ly = 0;
                } else {
                    pad.rx = 0;
                    pad.ry = 0;
                }
            }

            if (bit && (bit & holdBits)) {
                // HELD - see PadMap::flickHoldBits. Set for as long as the
                // direction is, which is what lets ShockPlayerController satisfy
                // HintButtonHeld / HintHoldTime = 0.5 s and open the MAP.
                pad.buttons |= bit;
            } else if (g_flickArmed && now >= g_flickCooldownMs && bit) {
                // PULSED - one edge per return-to-centre, for maps whose
                // directions CYCLE rather than select.
                g_flickPulseBit = bit;
                g_flickPulseUntilMs = now + kFlickPulseMs;
                g_flickCooldownMs = now + kFlickCooldownMs;
                g_flickArmed = false;
            }
            if (rawX > -kFlickRearm && rawX < kFlickRearm && rawY > -kFlickRearm &&
                rawY < kFlickRearm)
                g_flickArmed = true;
        }
        if (now < g_flickPulseUntilMs) pad.buttons |= g_flickPulseBit;

        // s50 (Infinite): the flourish chord - see the state block. The edge
        // requires the rest ALREADY touched at the press, so a plain jump
        // with a thumb that later brushes the rest never fires it.
        if (g_flourishChordArmed.load(std::memory_order_relaxed) &&
            !g_flourishChordSuspended.load(std::memory_order_relaxed)) {
            const bool aDown = read_bool(session, g_btnA);
            if (restL) {
                pad.buttons &= ~map.faceA; // consume A while chorded
                if (aDown && !g_chordAWasDown)
                    g_flourishChordEdges.fetch_add(1, std::memory_order_relaxed);
            }
            g_chordAWasDown = aDown;
        }
    }

    // Left menu, or the X+Y chord that stands in for it. The MODIFIER decides
    // which of the two things this is - that is BRVR's shape, and it is what
    // reaches the map:
    //
    //     menu / X+Y             -> START (pause)
    //     MODIFIER + menu / X+Y  -> BACK  (ShowContextHelp, "WHAT IS THIS?")
    //
    // and BACK must be HELD, because ShockPlayerController gates the MAP SCREEN
    // behind HintButtonHeld with HintHoldTime = 0.5 s. BRVR: `if (s.menu) btn |=
    // (mod ? XI_BACK : XI_START);`.
    //
    // WHAT THIS REPLACES, and why the old shape could not work: BACK used to be
    // the LONG PRESS of the menu button, with no modifier involved. That put
    // context help behind the one input most likely not to exist - "on many
    // setups no menu button reaches the game at all: SteamVR claims the left
    // one for its dashboard, the Meta runtime the right" - and the X+Y chord,
    // which exists precisely for those setups, could then only ever produce
    // START. Hanging it off the modifier instead means the chord reaches it too.
    const bool menuRaw = read_bool(session, g_menu);
    const bool menuDown = menuRaw || menuChord;
    if (menuDown && modHeldForMenu && bvr::input::menu_modifier_context_help()) {
        // Held for as long as the gesture is, so the 0.5 s hint timer can run.
        pad.buttons |= XINPUT_GAMEPAD_BACK;
        g_menuDownMs = 0;         // never also count as a pause tap
        g_startPulseUntilMs = 0;  // and cancel one already in flight
    } else if (menuDown) {
        if (g_menuDownMs == 0) g_menuDownMs = now;
        // The long-press route to BACK is kept as a fallback for anyone whose
        // d-pad modifier is off (dpadModifier = 0), who would otherwise have no
        // way to reach context help at all.
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

// s50: the flourish-chord accessors live in bvr::input beside the other
// composed-state readers; the state itself is the XR composer's (same TU).
namespace bvr::input {

void arm_flourish_chord(bool on) {
    const bool was = bvr::vr::g_flourishChordArmed.exchange(on, std::memory_order_relaxed);
    if (was != on)
        BVR_LOG("xr-input: flourish chord %s (left thumbrest + A; A is consumed "
                "while the rest is touched)",
                on ? "ARMED" : "off");
}

uint32_t flourish_chord_edges() {
    return bvr::vr::g_flourishChordEdges.load(std::memory_order_relaxed);
}

void set_flourish_chord_suspended(bool on) {
    const bool was =
        bvr::vr::g_flourishChordSuspended.exchange(on, std::memory_order_relaxed);
    if (was != on)
        BVR_LOG("xr-input: flourish chord %s", on ? "SUSPENDED (A passes through)" : "resumed");
}

// s63 d-pad modifier / flip / R3-jump, reached from the F10 panel and the
// command seam. Same arrangement as the flourish chord above: the state stays
// in the composer's TU next to the per-frame reader, the accessors live in
// bvr::input beside the other composed-state readers.
int dpad_modifier() {
    return bvr::vr::g_dpadMod.load(std::memory_order_relaxed);
}

void set_dpad_modifier(int m) {
    // -1 (Legacy) is reachable deliberately: it is how a game with no tested
    // mode gets the pre-s63 heuristic back, and how an A/B against it is run.
    if (m < -1 || m > 4) return;
    const int was = bvr::vr::g_dpadMod.exchange(m, std::memory_order_relaxed);
    if (was == m) return;
    static const char* kNames[] = {"off", "right thumbrest", "R3", "left grip",
                                   "left thumbrest"};
    BVR_LOG("xr-input: d-pad modifier = %s", m < 0 ? "legacy heuristic" : kNames[m]);
    // The one combination that cannot work, said at the moment it is chosen
    // rather than only in the startup echo.
    const bool leftSel = bvr::vr::g_dpadLeft.load(std::memory_order_relaxed);
    if ((leftSel && m == 4) || (!leftSel && m == 1))
        BVR_LOG("xr-input: WARNING - that thumbrest is on the SAME hand as the selecting "
                "stick. One thumb cannot rest on it and push that stick at once - use R3 "
                "or the left grip, or flip the selecting stick.");
    if (m == 2 && bvr::vr::g_jumpOnR3.load(std::memory_order_relaxed))
        BVR_LOG("xr-input: R3 is the modifier now, so the R3 jump lane yields to it - the "
                "layout's own jump button is unaffected");
}

bool dpad_select_left() {
    return bvr::vr::g_dpadLeft.load(std::memory_order_relaxed);
}

void set_dpad_select_left(bool on) {
    const bool was = bvr::vr::g_dpadLeft.exchange(on, std::memory_order_relaxed);
    if (was == on) return;
    BVR_LOG("xr-input: d-pad side = %s (%s stick selects, %s is suppressed while selecting)",
            on ? "LEFT" : "right", on ? "left" : "right", on ? "walking" : "turning");
}

bool jump_on_r3() {
    return bvr::vr::g_jumpOnR3.load(std::memory_order_relaxed);
}

void set_jump_on_r3(bool on) {
    const bool was = bvr::vr::g_jumpOnR3.exchange(on, std::memory_order_relaxed);
    if (was != on)
        BVR_LOG("xr-input: R3 jump %s%s", on ? "ON (additive)" : "off",
                on && bvr::vr::g_dpadMod.load(std::memory_order_relaxed) == 2
                    ? " - but R3 is the d-pad modifier, so it yields"
                    : "");
}

} // namespace bvr::input

#endif // BVR_WITH_OPENXR
