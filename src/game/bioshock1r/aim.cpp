// M6 decoupled aim. Hook behavior (call the original, then adjust the writable
// out-param) follows the same shape as the CalcView camera hook, i.e.
// itsloopyo/bioshock-remastered-headtracking (MIT), src/engine_hook.rs.
//
// Seams, all UnrealScript native thunks resolved through the engine's own
// native lookup table (no hardcoded addresses - see patterns.cpp):
//   AWeapon::execGetPerfectFireStart  result FVector  = trace origin
//   AWeapon::execApplyAimError        result vec/rot  = trace direction
//   APawn::execGetViewPoint           result FVector  = eye position
//   APawn::execGetViewDirection       result FVector  = view direction
//   AActor::execTrace                 read-only telemetry (hit point)
// ENGINE_NOTES "Fire flow / aim" carries the derivation and the live findings.

#include "game/bioshock1r/aim.h"

#include "core/gfx/hud_capture.h"
#include "core/input/xinput_bridge.h"
#include "core/util/log.h"
#include "core/vr/openxr_runtime.h"
#include "game/bioshock1r/body.h"
#include "game/bioshock1r/bones.h"
#include "game/bioshock1r/hands.h"
#include "game/shared/ue_math.h"

#include <windows.h>
#include <MinHook.h>

#include <imgui.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <string>

namespace bvr::b1r::aim {
namespace {

// ---- state -----------------------------------------------------------------

const uint8_t* g_imageBase = nullptr;

// Master switch + per-seam substitution switches. The seam set is command
// controlled because which native actually decides a shot is a live question
// per weapon type (see the probe): flipping these needs no rebuild.
std::atomic<bool> g_enabled{false};
std::atomic<bool> g_subWeapon{true};  // right hand aims weapons
std::atomic<bool> g_subAbility{true}; // left hand aims plasmids
std::atomic<bool> g_handOrigin{true}; // hand origin + direction (user's choice)
std::atomic<bool> g_probe{false};     // telemetry mode
// Session 30 (the wrench-miss investigation). `vraim dump`/`probe` is BUDGET
// gated - a fixed number of lines - and it logs every call including every AI
// weapon and AI ability. In a firefight that budget is spent by splicer shots
// long before the one melee swing that matters, so the mod could not see its
// own bug in the exact condition the bug was reported in. The watch is RATE
// limited instead (one line per seam per interval), player-owned calls only,
// and it prints the two things the budget dump does not: what the engine's own
// fire start was against what we substituted, and HOW the hand was chosen.
std::atomic<bool> g_watch{true};
std::atomic<uint32_t> g_watchMinMs{200};
// The overlay runs on the RENDER thread and must never install a MinHook itself
// (same rule as scenedraw's vrstereo checkbox): it posts a request here and the
// game thread applies it from on_calcview, outside any hooked engine call.
std::atomic<int> g_pendingEnable{-1}; // -1 none, 0 off, 1 on
// Pose source + calibration. The runtime's AIM pose is the pointing ray and is
// the default; the offsets are there because "where it points" is still a matter
// of taste (grip angle, wrist posture) - the user's first in-headset run wanted
// the ray a little lower than the raw pose gave.
std::atomic<bool> g_useAimPose{true};
// Session 20 muzzle ray (default OFF until the in-headset verdict): the RIGHT
// hand's ray leaves along the RENDERED barrel - direction = the model's
// target rotation applied to the rig's reference barrel axis
// (bones::barrel_ref_axis) - instead of the trimmed controller forward.
// Per-weapon automatic (the reference pose is the per-weapon animation), no
// manual trim. The left/plasmid hand keeps the pointing ray: there is no
// barrel to follow.
std::atomic<bool> g_muzzleRay{false};
// Per-hand since session 16 part 3 (user request): each controller's wrist
// posture wants its own trim. 0 = left (plasmid), 1 = right (weapon).
// Defaults = the user's in-headset calibration (their explicit ask: the
// saved preset becomes the defaults). Left trim re-baked session 22
// (2026-07-29 headset run: -7.5/+37.0, "better than before"), and both
// hands re-baked session 61 (2026-08-14) from the calibration run that
// accepted hand/weapon scaling - the right hand now carries a small nonzero
// trim of its own (the per-weapon profiles still override it per weapon;
// this is the baseline they start from).
// s70: THE CROSSHAIR IS GLOBAL, so these two are the live values outright - the
// weapon profile no longer swaps them. The defaults are the seeded set's own
// numbers (plasmid -11.00/37.00, the weapons' common 0.83/-9.20) so a fresh
// install lands where the per-weapon table used to put it.
std::atomic<float> g_pitchOffsetDeg[2] = {-11.0f, 0.83f};
std::atomic<float> g_yawOffsetDeg[2] = {37.0f, -9.20f};
// Per-hand aim-ray ORIGIN offsets in cm (session 18 part 2, user request):
// the model offsets move the MESH about its pivot, so a tuned model can sit
// right while the ray no longer runs along the barrel. These move the RAY
// itself - laser, fire origin, everything reading g_ray - to re-align with
// the controller and the tuned model. Applied along the FINAL (trimmed)
// ray's zero-roll basis at ray build, so the laser and the bullet cannot
// disagree; the model deliberately does not take them (it has its own
// sliders, and the two are tuned against each other).
std::atomic<float> g_posFwdCm[2] = {-2.8f, 0.0f};  // v0.3.0 bake, s61 re-bake
std::atomic<float> g_posRightCm[2] = {0.6f, -2.1f};
std::atomic<float> g_posUpCm[2] = {0.5f, 12.9f};
// ---- Session 21: per-weapon profiles ----------------------------------------
// The RIGHT hand's trim + ray-origin offsets hot-swap per weapon, keyed by
// the equipped weapon's canonical class name ('Shotgun', 'Pistol', ... via
// patterns::object_class_name). The R atomics above stay THE live values
// everything reads (ray, laser, model publish, sliders); on a weapon change
// the outgoing values are stashed into the old key's profile and the new
// key's (seeded from the current values when first seen) loads in. The
// left/plasmid hand is untouched. weapons.ini persists the map (saved by
// `vrpreset save` and `vraim wsave`). Real weapon switching cannot be
// driven flat (exec NextWeapon FAULTS, session 20) - `vraim wkey sim
// <name>` forces a key for the flat swap proof; the live-switch proof is
// an in-headset checklist item. Game thread only (except the UI name copy).
struct WeaponProfile {
    float trimPitch, trimYaw, posFwd, posRight, posUp;
    // s65: the MODEL grip offset (hands.cpp), as opposed to the four above
    // which steer the aim RAY. Per weapon because the number is the model's
    // own origin-to-grip vector - see hands.h's banner. Zero means "as before":
    // a profile written by an older build has no grip keys, loads them as 0,
    // and the model sits exactly where it used to.
    float gripFwd, gripRight, gripUp;
    // s67: view-frame PLACEMENT, per weapon. Distinct from the grip offset
    // above: grip is the PIVOT, this is where the gun sits. A shotgun sits
    // differently in the hand from a pistol, so it needs its own.
    float viewFwd, viewRight, viewUp;
    // s67: may this weapon adopt the engine's animation? BRVR's HandAnimSlot.
    // 1 for guns (recoil and reload play); 0 for the wrench, whose swing
    // animation fights manual melee - the tester's words: "manual swinging with
    // the wrench feels amazing without the swing animation".
    float animOn;
    float modelPitch, modelYaw, modelRoll;
};
std::map<std::string, WeaponProfile> g_weaponProfiles;
std::string g_weaponKey;          // active profile key ("" = none)
void* g_weaponKeyActor = nullptr; // weapon pointer the key was resolved from
bool g_weaponKeySim = false;      // a sim key is armed (flat test seam)
uint32_t g_weaponSwaps = 0;
std::mutex g_weaponKeyUiMutex; // the overlay (render thread) reads the name
char g_weaponKeyUi[48] = "-";
// The preset-loaded R baseline. New profiles seed from THIS, not from
// whatever R values happen to be live (headset run 1: the first resolve
// beat the preset's value load by one second, so the first profile seeded
// from pre-preset ZEROS and the preset-tail re-apply then wrote those
// zeros over the user's tuned baseline). Until a baseline exists (or ini
// profiles were loaded), the resolver stays idle - the flat flow and the
// user's flow both press the preset before playing.
WeaponProfile g_presetBaseline{};
bool g_presetBaselineValid = false;
// M7 laser: the visible form of this same ray, published to core every frame.
// It lives here rather than with the hands so it cannot drift from the trim
// above - a laser that disagrees with the bullet is worse than no laser.
std::atomic<bool> g_laser{false};
std::atomic<int> g_laserDots{6};
std::atomic<float> g_laserNearM{0.30f}, g_laserFarM{6.0f}, g_laserSizeDeg{0.7f};
// Session 29 aim dot: ONE lever, default OFF. Unlike the laser it is not a
// beam reconstruction - it is the fire-seam ray's own point, pushed back into
// XR space (see AimDotConfig and frame_context.h's game_point_to_xr).
// The distance is a slider because nothing in this mod can trace: a dot and a
// wall only fuse in stereo at matching depth, so the calibration flow is "set
// the distance to the wall, fire, nudge the trim until the dot sits on the
// hole". BioVRDev's dot has the same shape and the same limitation.
std::atomic<bool> g_dot{false};
std::atomic<float> g_dotDistM{5.0f};
std::atomic<float> g_dotSizeDeg{0.5f};
constexpr float kIdentQuat[4] = {0.0f, 0.0f, 0.0f, 1.0f};
std::atomic<int32_t> g_dumpBudget{0}; // per-seam detailed log budget

// Per-frame aim rays, game thread only (built in on_calcview, read in the
// detours - all on the game thread, so no locking).
struct Ray {
    bool valid = false;
    bool synthetic = false; // came from a `vraim test` slot
    FVector origin{};
    FRotator rot{};
};
Ray g_ray[2]; // 0 = left (plasmid), 1 = right (weapon)
uint64_t g_rayStampMs = 0;
bool g_gameplayView = false;
// Copied out of the frame context each CalcView so the detours - which run far
// above g_lastCtx's definition - can turn Unreal units into centimetres for the
// watch line. A displacement is only meaningful next to the scale it lives in.
float g_worldScaleCache = 100.0f;

// Self-expiring synthetic aim, mirroring the xinput_bridge test slots: the
// command file is polled at 1 Hz, so a hold has to outlive its command inside
// the DLL. Yaw/pitch are OFFSETS from the current view rotation, which is what
// makes "aim 30 degrees right of where the camera looks" directly assertable.
struct TestAim {
    float yawDeg = 0.0f, pitchDeg = 0.0f;
    uint64_t deadline = 0;
};
TestAim g_test[2];

// Object -> hand mapping. BioShock dual-wields: the gun and the plasmid are
// two live weapon objects, and the natives are called on whichever is firing.
// Seed the map from the trigger the bridge itself is holding (we compose that
// state, so we know), then it is pure pointer identity - and it re-learns for
// free when the player switches weapon or plasmid.
void* g_objRight = nullptr;
void* g_objLeft = nullptr;
void* g_lastPc = nullptr; // PlayerController identity, to spot a world change
std::atomic<uint32_t> g_learnEvents{0};

struct Slot {
    const char* name;
    void* target = nullptr;
    void* original = nullptr;
    bool created = false;
    std::atomic<bool> enabled{false};
    std::atomic<uint32_t> calls{0};
    std::atomic<uint32_t> subs{0};
    std::atomic<uint32_t> skips{0}; // ran, but not ours to touch (AI, no ray)
    // Session 30 (the wrench-miss investigation): a cumulative record of WHICH
    // HAND each substitution used and how that hand was decided. A rate-limited
    // log line can be missed; a counter cannot, so after a real fight these
    // three numbers either confirm or refute "melee is being aimed with the
    // left controller" without needing the log at all.
    std::atomic<uint32_t> subsR{0};
    std::atomic<uint32_t> subsL{0};
    std::atomic<uint32_t> fallbacks{0}; // no trigger evidence: took the default
    int32_t dumpLeft = 0;
    uint64_t lastWatchMs = 0; // rate gate for the session-30 watch line
};
Slot g_weaponFire{"weapon"};  // AWeapon::GetPerfectFireStart impl
Slot g_abilityFire{"ability"}; // UAttackAbility::GetPerfectFireStart impl

// Last substituted values, for the overlay + the flat-test assertions.
std::atomic<float> g_lastSubX{0.0f}, g_lastSubY{0.0f}, g_lastSubZ{0.0f};
std::atomic<int32_t> g_lastSubYaw{0}, g_lastSubPitch{0};
std::atomic<uint32_t> g_lastSubHand{0};

using ExecFn = void(__fastcall*)(void* self, void* edx, void* stack, void* result);

uint32_t to_rva(const void* p) {
    if (!p || !g_imageBase) return 0;
    return static_cast<uint32_t>(static_cast<const uint8_t*>(p) - g_imageBase);
}

// ---- guarded memory helpers (no C++ objects in an SEH frame) ---------------

bool read12(const void* src, float* out3) {
    __try {
        memcpy(out3, src, 12);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool write12(void* dst, const float* in3) {
    __try {
        memcpy(dst, in3, 12);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool read_ptr(const void* src, void** out) {
    __try {
        *out = *static_cast<void* const*>(src);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// ---- ownership gates -------------------------------------------------------
// AI weapons inherit AWeapon and AI pawns inherit APawn, so these natives run
// for the whole level - substituting blindly would aim every splicer's gun with
// the player's controller. Class identity comes from the vtable (RTTI-derived
// RVAs in patterns.h).

bool has_vtable(void* obj, uint32_t wantRva) {
    if (!obj) return false;
    void* vtbl = nullptr;
    if (!read_ptr(obj, &vtbl)) return false;
    return to_rva(vtbl) == wantRva;
}

bool is_player_pawn(void* obj) {
    return has_vtable(obj, patterns::kShockPlayerVtableRva);
}

// A weapon's owning pawn sits at [weapon+0x454] (the fire-start implementation
// reads it), so "is this the player's gun" is one dereference plus the pawn
// vtable check - and an AI's weapon fails it.
bool owner_is_player_pawn(void* weapon) {
    if (!weapon) return false;
    void* owner = nullptr;
    if (!read_ptr(static_cast<const uint8_t*>(weapon) + patterns::kWeaponOwnerOffset, &owner))
        return false;
    return is_player_pawn(owner);
}

// ---- hand selection --------------------------------------------------------

bool trigger_held(bool right) {
    uint8_t lt = 0, rt = 0;
    bvr::input::last_composed_triggers(&lt, &rt);
    return (right ? rt : lt) >= 64; // ~25% pull, same ballpark as the game's own
}

// How a hand was decided. Session 30: without this the log cannot tell
// "learned Left" from "fell back to Left", and that distinction is the whole
// of the melee hypothesis - a swing whose anim notify fires after the player
// has already released the trigger has no evidence to learn from and takes the
// seam default, which for the ability seam is the LEFT hand and its +37 deg
// yaw trim.
enum class HandSrc { Learned, LearnedNow, Fallback };
HandSrc g_lastHandSrc = HandSrc::Fallback;
const char* hand_src_name(HandSrc s) {
    return s == HandSrc::Learned ? "learned"
         : s == HandSrc::LearnedNow ? "learned-now"
                                    : "fallback";
}

// `fallback` is the hand this seam belongs to when there is no trigger
// evidence yet (weapons right, abilities left).
Hand hand_for_object(void* obj, Hand fallback) {
    g_lastHandSrc = HandSrc::Learned;
    if (obj && obj == g_objRight) return Hand::Right;
    if (obj && obj == g_objLeft) return Hand::Left;

    bool r = trigger_held(true), l = trigger_held(false);
    if (obj) {
        // Exactly one trigger -> that hand owns this object. Both triggers with
        // one slot already known -> the other hand owns it by elimination.
        bool takeRight = false, takeLeft = false;
        if (r && !l) takeRight = true;
        else if (l && !r) takeLeft = true;
        else if (r && l) { takeRight = (g_objRight == nullptr && g_objLeft != nullptr);
                           takeLeft = (g_objLeft == nullptr && g_objRight != nullptr); }
        if (takeRight) {
            g_objRight = obj;
            if (g_objLeft == obj) g_objLeft = nullptr;
            g_learnEvents.fetch_add(1, std::memory_order_relaxed);
            g_lastHandSrc = HandSrc::LearnedNow;
            BVR_LOG("[aim] learned RIGHT-hand (weapon) object %p", obj);
            return Hand::Right;
        }
        if (takeLeft) {
            g_objLeft = obj;
            if (g_objRight == obj) g_objRight = nullptr;
            g_learnEvents.fetch_add(1, std::memory_order_relaxed);
            g_lastHandSrc = HandSrc::LearnedNow;
            BVR_LOG("[aim] learned LEFT-hand (plasmid) object %p", obj);
            return Hand::Left;
        }
    }
    g_lastHandSrc = HandSrc::Fallback;
    return fallback; // no trigger evidence yet
}

// ---- the ray ---------------------------------------------------------------

bool ray_for(Hand h, FVector* origin, FRotator* rot) {
    if (!g_enabled.load(std::memory_order_relaxed)) return false;
    if (!g_gameplayView) return false; // scripted camera: leave the engine alone
    const Ray& r = g_ray[static_cast<int>(h)];
    if (!r.valid) return false;
    // A ray older than a few frames means CalcView stopped running (load
    // screen, teardown) - never aim from a stale pose.
    if (GetTickCount64() - g_rayStampMs > 250) return false;
    if (origin) *origin = r.origin;
    if (rot) *rot = r.rot;
    return true;
}

void note_substitution(Hand h, const FVector& o, const FRotator& r) {
    g_lastSubX.store(o.x, std::memory_order_relaxed);
    g_lastSubY.store(o.y, std::memory_order_relaxed);
    g_lastSubZ.store(o.z, std::memory_order_relaxed);
    g_lastSubYaw.store(r.yaw, std::memory_order_relaxed);
    g_lastSubPitch.store(r.pitch, std::memory_order_relaxed);
    g_lastSubHand.store(h == Hand::Right ? 1u : 0u, std::memory_order_relaxed);
}

// What did the engine just put in this 12-byte out-param? Decide from the
// value, because the fire-start functions hand back a mix and the order differs
// between the weapon and the ability signature:
//   FRotator - three rotation-unit int32s (65536 per turn). Their FLOAT
//              reinterpretation is a denormal, which is why a rotator prints as
//              "0.000 0.000 0.000" - the trap that cost session 10 a detour.
//   FVector direction - ordinary floats, length ~1.
//   FVector position  - ordinary floats, thousands of Unreal units.
enum SlotKind { kUnused, kRotator, kDirection, kPosition };

SlotKind classify_slot(const float v[3]) {
    int32_t i[3];
    memcpy(i, v, sizeof i);
    if (i[0] == 0 && i[1] == 0 && i[2] == 0) return kUnused;
    bool allSmallInts = true;
    for (int k = 0; k < 3; ++k) {
        int32_t a = i[k] < 0 ? -i[k] : i[k];
        if (a > (1 << 21)) allSmallInts = false; // 2 million rot units is nonsense
    }
    if (allSmallInts) return kRotator;
    float len2 = v[0] * v[0] + v[1] * v[1] + v[2] * v[2];
    return len2 < 4.0f ? kDirection : kPosition;
}

const char* slot_kind_name(SlotKind k) {
    return k == kRotator ? "rot" : k == kDirection ? "dir" : k == kPosition ? "pos" : "-";
}

// ---- detours ---------------------------------------------------------------
// Both seams are __thiscall C++ methods, which is register/stack-identical to
// __fastcall with a dummy EDX slot. Substitution happens AFTER the original
// ran, so the engine's own numbers are what we log and what we fall back to.

void log_call(Slot& slot, void* self, const float* a, const float* b, const float* c,
              const char* note) {
    if (slot.dumpLeft <= 0) return;
    --slot.dumpLeft;
    void* vtbl = nullptr;
    read_ptr(self, &vtbl);
    int32_t bi[3];
    memcpy(bi, b, sizeof bi);
    BVR_LOG("[aim] %s this=%p vtbl=0x%X A[%s]=(%.1f %.1f %.1f) B[%s]=(%d %d %d) "
            "C[%s]=(%.1f %.1f %.1f) %s",
            slot.name, self, to_rva(vtbl), slot_kind_name(classify_slot(a)), a[0], a[1], a[2],
            slot_kind_name(classify_slot(b)), bi[0], bi[1], bi[2],
            slot_kind_name(classify_slot(c)), c[0], c[1], c[2], note);
}

// Session 30: which hand a substitution actually used, and whether the choice
// came from evidence or from the seam's default. Cumulative, so a whole fight
// collapses to three numbers in `vraim status`.
void note_hand(Slot& slot, Hand h) {
    if (h == Hand::Right)
        slot.subsR.fetch_add(1, std::memory_order_relaxed);
    else
        slot.subsL.fetch_add(1, std::memory_order_relaxed);
    if (g_lastHandSrc == HandSrc::Fallback)
        slot.fallbacks.fetch_add(1, std::memory_order_relaxed);
}

// Session 30: the rate-limited counterpart to log_call. Everything it prints
// is chosen to answer one of the four live hypotheses about the wrench:
//   cls/wkey   WHICH class made the call - this settles whether melee reaches
//              a seam at all, and which one. Three documents in this tree give
//              three different answers, so the log has to be the tiebreak.
//   hand/src   the hand and how it was decided. "L" with src=fallback on a
//              melee call is hypothesis H2 confirmed outright: the left trims
//              are pitch -7.5, yaw +37.0 deg, which at melee range is a miss.
//   lt/rt      the composed triggers AT the anim notify, which is the test of
//              "the player already let go before the swing landed".
//   engine vs ours + d
//              how far we moved the shot's start. For a bullet at range this
//              is invisible; for a short melee it is the whole question.
void watch_call(Slot& slot, void* self, const float* a, const float* b, const float* c,
                Hand h, bool subbed) {
    if (!g_watch.load(std::memory_order_relaxed)) return;
    uint64_t now = GetTickCount64();
    uint32_t minMs = g_watchMinMs.load(std::memory_order_relaxed);
    if (slot.lastWatchMs && now - slot.lastWatchMs < minMs) return;
    slot.lastWatchMs = now;

    // The engine's own fire start is whichever pre-substitution slot classified
    // as a POSITION (the index differs between the two signatures, which is why
    // substitute() is value-driven rather than index-driven).
    const float* eng = nullptr;
    const float* slots[3] = {a, b, c};
    for (const float* s : slots) {
        if (classify_slot(s) == kPosition) { eng = s; break; }
    }
    const Ray& r = g_ray[static_cast<int>(h)];
    float dUu = -1.0f;
    if (eng && r.valid) {
        float dx = r.origin.x - eng[0], dy = r.origin.y - eng[1], dz = r.origin.z - eng[2];
        dUu = sqrtf(dx * dx + dy * dy + dz * dz);
    }
    uint8_t lt = 0, rt = 0;
    bvr::input::last_composed_triggers(&lt, &rt);
    const wchar_t* cls = patterns::object_class_name(self);
    float uuPerCm = g_worldScaleCache / 100.0f;
    BVR_LOG("[aim] watch %s cls='%S' wkey='%s' this=%p hand=%c src=%s lt=%u rt=%u sub=%d "
            "origin=%d | engine=(%.1f %.1f %.1f) ours=(%.1f %.1f %.1f) d=%.1f UU (%.1f cm)",
            slot.name, cls ? cls : L"?", g_weaponKey.empty() ? "-" : g_weaponKey.c_str(), self,
            h == Hand::Right ? 'R' : 'L', hand_src_name(g_lastHandSrc), lt, rt, subbed ? 1 : 0,
            g_handOrigin.load(std::memory_order_relaxed) ? 1 : 0,
            eng ? eng[0] : 0.0f, eng ? eng[1] : 0.0f, eng ? eng[2] : 0.0f,
            r.valid ? r.origin.x : 0.0f, r.valid ? r.origin.y : 0.0f,
            r.valid ? r.origin.z : 0.0f, dUu, dUu >= 0.0f && uuPerCm > 0.0f ? dUu / uuPerCm
                                                                           : -1.0f);
}

// The two out-params are a POSITION (thousands of Unreal units) and a unit
// DIRECTION, and which slot holds which differs between the weapon and the
// ability signature. Rather than trust the disassembly's labels, decide per
// call from the magnitudes - a direction is never longer than ~1.
// Write our ray into the out-params: every slot the engine filled with a
// POSITION gets the hand's origin, every slot it filled with a DIRECTION gets
// the hand's direction. Which index is which differs between the two
// signatures (and an unused slot reads as zero), so this stays value-driven
// rather than trusting a fixed index. True if anything was written.
bool substitute(Slot& slot, Hand h, float* const* out, int count) {
    FVector o{};
    FRotator r{};
    if (!ray_for(h, &o, &r)) {
        slot.skips.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    float dir[3];
    ue_rot_to_dir(r, dir);
    float pos[3] = {o.x, o.y, o.z};
    bool handOrigin = g_handOrigin.load(std::memory_order_relaxed);

    // An FRotator carries the engine's roll through untouched: aim owns pitch
    // and yaw, the weapon's own roll is none of our business.
    bool wrote = false;
    for (int i = 0; i < count; ++i) {
        if (!out[i]) continue;
        float cur[3];
        if (!read12(out[i], cur)) continue;
        switch (classify_slot(cur)) {
            case kRotator: {
                int32_t curInt[3];
                memcpy(curInt, cur, sizeof curInt);
                int32_t rot[3] = {r.pitch, r.yaw, curInt[2]};
                float packed[3];
                memcpy(packed, rot, sizeof packed);
                wrote |= write12(out[i], packed);
                break;
            }
            case kDirection:
                wrote |= write12(out[i], dir);
                break;
            case kPosition:
                if (handOrigin) wrote |= write12(out[i], pos);
                break;
            case kUnused:
            default:
                break; // engine left it alone; so do we
        }
    }
    if (!wrote) return false;
    slot.subs.fetch_add(1, std::memory_order_relaxed);
    note_substitution(h, o, r);
    return true;
}

// AWeapon::GetPerfectFireStart(FVector* outA, FVector* outB, void* outC).
// `this` is the weapon; [this+0x454] is the pawn holding it, which is how a
// player shot is told from a splicer's.
void __fastcall WeaponFireDetour(void* self, void* edx, float* outA, float* outB, float* outC) {
    using WeaponFireFn = void(__fastcall*)(void*, void*, float*, float*, float*);
    reinterpret_cast<WeaponFireFn>(g_weaponFire.original)(self, edx, outA, outB, outC);
    g_weaponFire.calls.fetch_add(1, std::memory_order_relaxed);

    float a[3] = {0, 0, 0}, b[3] = {0, 0, 0}, c[3] = {0, 0, 0};
    read12(outA, a);
    read12(outB, b);
    read12(outC, c);

    const char* note = "";
    if (g_subWeapon.load(std::memory_order_relaxed) && owner_is_player_pawn(self)) {
        Hand h = hand_for_object(self, Hand::Right);
        float* outs[3] = {outA, outB, outC};
        bool subbed = substitute(g_weaponFire, h, outs, 3);
        if (subbed) {
            note = (h == Hand::Right) ? "SUB(R)" : "SUB(L)";
            note_hand(g_weaponFire, h);
        }
        watch_call(g_weaponFire, self, a, b, c, h, subbed);
    }
    log_call(g_weaponFire, self, a, b, c, note);
}

// UAttackAbility::GetPerfectFireStart(void* instigator, FVector* outA,
//                                    FVector* outB, void* outC).
// The instigator argument IS the ownership check the engine itself does here.
void __fastcall AbilityFireDetour(void* self, void* edx, void* instigator, float* outA,
                                  float* outB, float* outC) {
    using AbilityFireFn = void(__fastcall*)(void*, void*, void*, float*, float*, float*);
    reinterpret_cast<AbilityFireFn>(g_abilityFire.original)(self, edx, instigator, outA, outB,
                                                           outC);
    g_abilityFire.calls.fetch_add(1, std::memory_order_relaxed);

    float a[3] = {0, 0, 0}, b[3] = {0, 0, 0}, c[3] = {0, 0, 0};
    read12(outA, a);
    read12(outB, b);
    read12(outC, c);

    const char* note = "";
    if (g_subAbility.load(std::memory_order_relaxed) && is_player_pawn(instigator)) {
        // Plasmids are the left hand; the wrench's melee ability also lands
        // here, and the trigger-keyed map moves it back to the right hand the
        // first time it swings.
        Hand h = hand_for_object(self, Hand::Left);
        float* outs[3] = {outA, outB, outC};
        bool subbed = substitute(g_abilityFire, h, outs, 3);
        if (subbed) {
            note = (h == Hand::Right) ? "SUB(R)" : "SUB(L)";
            note_hand(g_abilityFire, h);
        }
        watch_call(g_abilityFire, self, a, b, c, h, subbed);
    }
    log_call(g_abilityFire, self, a, b, c, note);
}

// ---- generic native probe --------------------------------------------------
// `vraim scan <Class> <Func>` resolves ANY name-based native through the
// engine's lookup table and hooks it read-only. The whole fire flow is a
// question of which natives run when a trigger is pulled, and answering it
// from the command file beats a rebuild per guess. Eight slots is plenty for
// one investigation; template instantiation gives each its own detour address.

constexpr int kProbeSlots = 8;

struct ProbeSlot {
    char name[64] = {};
    void* target = nullptr;
    void* original = nullptr;
    bool created = false;
    std::atomic<bool> enabled{false};
    std::atomic<uint32_t> calls{0};
    int32_t dumpLeft = 0;
};
ProbeSlot g_probes[kProbeSlots];

void probe_note(int n, void* self, void* stack, void* result) {
    ProbeSlot& p = g_probes[n];
    p.calls.fetch_add(1, std::memory_order_relaxed);
    if (p.dumpLeft <= 0) return;
    --p.dumpLeft;
    float raw[3] = {0, 0, 0};
    read12(result, raw);
    int32_t asInt[3];
    memcpy(asInt, raw, sizeof asInt);
    void* obj = nullptr;
    void* node = nullptr;
    if (stack) {
        read_ptr(static_cast<const uint8_t*>(stack) + patterns::kFFrameObjectOffset, &obj);
        read_ptr(static_cast<const uint8_t*>(stack) + patterns::kFFrameNodeOffset, &node);
    }
    void* vtbl = nullptr;
    read_ptr(self, &vtbl);
    BVR_LOG("[aim] scan %s this=%p vtbl=0x%X obj=%p node=%p res f=(%.2f %.2f %.2f) "
            "i=(%d %d %d)", p.name, self, to_rva(vtbl), obj, node, raw[0], raw[1], raw[2],
            asInt[0], asInt[1], asInt[2]);
}

template <int N>
void __fastcall GenericProbeDetour(void* self, void* edx, void* stack, void* result) {
    reinterpret_cast<ExecFn>(g_probes[N].original)(self, edx, stack, result);
    probe_note(N, self, stack, result);
}

void* const kProbeDetours[kProbeSlots] = {
    reinterpret_cast<void*>(&GenericProbeDetour<0>),
    reinterpret_cast<void*>(&GenericProbeDetour<1>),
    reinterpret_cast<void*>(&GenericProbeDetour<2>),
    reinterpret_cast<void*>(&GenericProbeDetour<3>),
    reinterpret_cast<void*>(&GenericProbeDetour<4>),
    reinterpret_cast<void*>(&GenericProbeDetour<5>),
    reinterpret_cast<void*>(&GenericProbeDetour<6>),
    reinterpret_cast<void*>(&GenericProbeDetour<7>),
};

bvr::pattern_scan::ProcessImage g_image{};

// ---- arbitrary-implementation probe ----------------------------------------
// `vraim scanimpl <rva> <stackArgs>` hooks any C++ method read-only. The fire
// flow runs through virtuals and direct calls that no name lookup can reach, so
// answering "does THIS function run when a shot happens" has to be possible
// from the command file - a rebuild per candidate is how a session evaporates.
// The stack-arg count must match the target's `ret <n>`, hence one detour
// family per arity.

constexpr int kImplSlotsPerArity = 2;

struct ImplSlot {
    uint32_t rva = 0;
    void* target = nullptr;
    void* original = nullptr;
    bool created = false;
    std::atomic<bool> enabled{false};
    std::atomic<uint32_t> calls{0};
    int32_t dumpLeft = 0;
};
ImplSlot g_impl[3][kImplSlotsPerArity]; // [arity-1][slot]

void impl_note(ImplSlot& p, void* self, void* a1, void* a2, void* a3) {
    p.calls.fetch_add(1, std::memory_order_relaxed);
    if (p.dumpLeft <= 0) return;
    --p.dumpLeft;
    void* vtbl = nullptr;
    read_ptr(self, &vtbl);
    float v1[3] = {0, 0, 0};
    read12(a1, v1);
    BVR_LOG("[aim] impl 0x%X this=%p vtbl=0x%X a1=%p(%.2f %.2f %.2f) a2=%p a3=%p", p.rva, self,
            to_rva(vtbl), a1, v1[0], v1[1], v1[2], a2, a3);
}

template <int N>
void* __fastcall Impl1Detour(void* self, void* edx, void* a1) {
    using Fn = void*(__fastcall*)(void*, void*, void*);
    void* r = reinterpret_cast<Fn>(g_impl[0][N].original)(self, edx, a1);
    impl_note(g_impl[0][N], self, a1, nullptr, nullptr);
    return r;
}

template <int N>
void* __fastcall Impl2Detour(void* self, void* edx, void* a1, void* a2) {
    using Fn = void*(__fastcall*)(void*, void*, void*, void*);
    void* r = reinterpret_cast<Fn>(g_impl[1][N].original)(self, edx, a1, a2);
    impl_note(g_impl[1][N], self, a1, a2, nullptr);
    return r;
}

template <int N>
void* __fastcall Impl3Detour(void* self, void* edx, void* a1, void* a2, void* a3) {
    using Fn = void*(__fastcall*)(void*, void*, void*, void*, void*);
    void* r = reinterpret_cast<Fn>(g_impl[2][N].original)(self, edx, a1, a2, a3);
    impl_note(g_impl[2][N], self, a1, a2, a3);
    return r;
}

void* const kImplDetours[3][kImplSlotsPerArity] = {
    {reinterpret_cast<void*>(&Impl1Detour<0>), reinterpret_cast<void*>(&Impl1Detour<1>)},
    {reinterpret_cast<void*>(&Impl2Detour<0>), reinterpret_cast<void*>(&Impl2Detour<1>)},
    {reinterpret_cast<void*>(&Impl3Detour<0>), reinterpret_cast<void*>(&Impl3Detour<1>)},
};

void scan_impl(uint32_t rva, int args, int32_t dump) {
    if (args < 1 || args > 3) {
        BVR_LOG("[aim] scanimpl: stackArgs must be 1..3 (must match the target's ret size)");
        return;
    }
    if (!g_imageBase) return;
    void* target = const_cast<uint8_t*>(g_imageBase) + rva;
    for (int i = 0; i < kImplSlotsPerArity; ++i) {
        ImplSlot& p = g_impl[args - 1][i];
        if (p.target == target) {
            p.dumpLeft = dump;
            BVR_LOG("[aim] impl 0x%X re-armed (%d lines)", rva, dump);
            return;
        }
    }
    for (int i = 0; i < kImplSlotsPerArity; ++i) {
        ImplSlot& p = g_impl[args - 1][i];
        if (p.created) continue;
        p.rva = rva;
        p.target = target;
        p.dumpLeft = dump;
        MH_STATUS st = MH_CreateHook(target, kImplDetours[args - 1][i], &p.original);
        if (st != MH_OK) {
            BVR_LOG("[aim] scanimpl 0x%X: MH_CreateHook failed: %s", rva, MH_StatusToString(st));
            p.target = nullptr;
            return;
        }
        p.created = true;
        st = MH_EnableHook(target);
        if (st != MH_OK) {
            BVR_LOG("[aim] scanimpl 0x%X: MH_EnableHook failed: %s", rva, MH_StatusToString(st));
            return;
        }
        p.enabled.store(true, std::memory_order_relaxed);
        BVR_LOG("[aim] scanimpl 0x%X hooked (%d stack args, %d dump lines)", rva, args, dump);
        return;
    }
    BVR_LOG("[aim] scanimpl: no free %d-arg slot (`vraim scanoff` first)", args);
}

void impl_scan_off() {
    for (auto& arity : g_impl) {
        for (ImplSlot& p : arity) {
            if (!p.enabled.load(std::memory_order_relaxed)) continue;
            MH_DisableHook(p.target);
            p.enabled.store(false, std::memory_order_relaxed);
            BVR_LOG("[aim] impl 0x%X disabled (calls %u)", p.rva,
                    p.calls.load(std::memory_order_relaxed));
        }
    }
}

void scan_and_hook(const char* cls, const char* fn, int32_t dump) {
    bvr::pattern_scan::NativeScanResult scan{};
    if (!bvr::pattern_scan::find_native_function(g_image, cls, fn, scan)) {
        BVR_LOG("[aim] scan %s::%s NOT FOUND (%zu string match(es), %zu table ref(s))", cls, fn,
                scan.stringMatches, scan.tableRefs);
        return;
    }
    for (int i = 0; i < kProbeSlots; ++i) {
        ProbeSlot& p = g_probes[i];
        if (p.target == scan.function) { // re-arm an existing slot
            p.dumpLeft = dump;
            BVR_LOG("[aim] scan %s re-armed (%d lines)", p.name, dump);
            return;
        }
    }
    for (int i = 0; i < kProbeSlots; ++i) {
        ProbeSlot& p = g_probes[i];
        if (p.created) continue;
        _snprintf_s(p.name, sizeof p.name, _TRUNCATE, "%s::%s", cls, fn);
        p.target = scan.function;
        p.dumpLeft = dump;
        MH_STATUS st = MH_CreateHook(p.target, kProbeDetours[i], &p.original);
        if (st != MH_OK) {
            BVR_LOG("[aim] scan %s: MH_CreateHook failed: %s", p.name, MH_StatusToString(st));
            p.target = nullptr;
            p.name[0] = 0;
            return;
        }
        p.created = true;
        st = MH_EnableHook(p.target);
        if (st != MH_OK) {
            BVR_LOG("[aim] scan %s: MH_EnableHook failed: %s", p.name, MH_StatusToString(st));
            return;
        }
        p.enabled.store(true, std::memory_order_relaxed);
        BVR_LOG("[aim] scan %s hooked at %p (rva 0x%X), %d dump lines", p.name, p.target,
                to_rva(p.target), dump);
        return;
    }
    BVR_LOG("[aim] scan: all %d probe slots in use (`vraim scanoff` first)", kProbeSlots);
}

void scan_off() {
    for (ProbeSlot& p : g_probes) {
        if (!p.enabled.load(std::memory_order_relaxed)) continue;
        MH_DisableHook(p.target);
        p.enabled.store(false, std::memory_order_relaxed);
        BVR_LOG("[aim] scan %s disabled (calls %u)", p.name,
                p.calls.load(std::memory_order_relaxed));
    }
}

// ---- hook lifecycle --------------------------------------------------------

bool install_slot(Slot& slot, void* target, void* detour) {
    if (!target) {
        BVR_LOG("[aim] %s: native not resolved - seam unavailable", slot.name);
        return false;
    }
    if (slot.enabled.load(std::memory_order_relaxed)) return true;
    slot.target = target;
    if (!slot.created) {
        MH_STATUS st = MH_CreateHook(target, detour, &slot.original);
        if (st != MH_OK) {
            BVR_LOG("[aim] MH_CreateHook(%s) failed: %s", slot.name, MH_StatusToString(st));
            return false;
        }
        slot.created = true;
    }
    MH_STATUS st = MH_EnableHook(target);
    if (st != MH_OK) {
        BVR_LOG("[aim] MH_EnableHook(%s) failed: %s", slot.name, MH_StatusToString(st));
        return false;
    }
    slot.enabled.store(true, std::memory_order_relaxed);
    BVR_LOG("[aim] %s hook ENABLED (target %p, rva 0x%X)", slot.name, target, to_rva(target));
    return true;
}

void disable_slot(Slot& slot) {
    if (!slot.enabled.load(std::memory_order_relaxed)) return;
    // Disable, never remove: a thread could still be returning through the
    // trampoline (same rule as scenedraw).
    MH_STATUS st = MH_DisableHook(slot.target);
    slot.enabled.store(false, std::memory_order_relaxed);
    BVR_LOG("[aim] %s hook disabled (%s)", slot.name, MH_StatusToString(st));
}

patterns::Symbols g_syms{};

bool install_all() {
    bool any = false;
    any |= install_slot(g_weaponFire, g_syms.weaponFireStart,
                        reinterpret_cast<void*>(&WeaponFireDetour));
    any |= install_slot(g_abilityFire, g_syms.abilityFireStart,
                        reinterpret_cast<void*>(&AbilityFireDetour));
    return any;
}

void disable_all() {
    disable_slot(g_weaponFire);
    disable_slot(g_abilityFire);
}

void set_dump(int32_t n) {
    g_dumpBudget.store(n, std::memory_order_relaxed);
    g_weaponFire.dumpLeft = n;
    g_abilityFire.dumpLeft = n;
}

std::atomic<bool>* sub_flag_by_name(const char* name) {
    if (strcmp(name, "weapon") == 0) return &g_subWeapon;
    if (strcmp(name, "ability") == 0) return &g_subAbility;
    return nullptr;
}

void log_status() {
    BVR_LOG("[aim] pose=%s cal L pitch%+.1f yaw%+.1f | R pitch%+.1f yaw%+.1f deg",
            g_useAimPose.load(std::memory_order_relaxed) ? "aim" : "grip",
            g_pitchOffsetDeg[0].load(std::memory_order_relaxed),
            g_yawOffsetDeg[0].load(std::memory_order_relaxed),
            g_pitchOffsetDeg[1].load(std::memory_order_relaxed),
            g_yawOffsetDeg[1].load(std::memory_order_relaxed));
    BVR_LOG("[aim] ray origin offset L fwd%+.1f right%+.1f up%+.1f | R fwd%+.1f right%+.1f "
            "up%+.1f cm",
            g_posFwdCm[0].load(std::memory_order_relaxed),
            g_posRightCm[0].load(std::memory_order_relaxed),
            g_posUpCm[0].load(std::memory_order_relaxed),
            g_posFwdCm[1].load(std::memory_order_relaxed),
            g_posRightCm[1].load(std::memory_order_relaxed),
            g_posUpCm[1].load(std::memory_order_relaxed));
    BVR_LOG("[aim] status: %s | seams weapon=%d ability=%d | handOrigin=%d probe=%d watch=%d"
            "(%ums)",
            g_enabled.load(std::memory_order_relaxed) ? "ON" : "off",
            g_subWeapon.load(std::memory_order_relaxed) ? 1 : 0,
            g_subAbility.load(std::memory_order_relaxed) ? 1 : 0,
            g_handOrigin.load(std::memory_order_relaxed) ? 1 : 0,
            g_probe.load(std::memory_order_relaxed) ? 1 : 0,
            g_watch.load(std::memory_order_relaxed) ? 1 : 0,
            g_watchMinMs.load(std::memory_order_relaxed));
    Slot* all[] = {&g_weaponFire, &g_abilityFire};
    for (Slot* s : all) {
        // subsR/subsL/fallbacks are session 30. subsL dominating on the ability
        // seam after a melee-heavy fight is the wrench hypothesis confirmed;
        // subsR dominating refutes it, which is the point of shipping them.
        BVR_LOG("[aim]   %-8s hook=%s calls=%u subs=%u skips=%u | subsR=%u subsL=%u "
                "fallbacks=%u", s->name,
                s->enabled.load(std::memory_order_relaxed) ? "on " : "off",
                s->calls.load(std::memory_order_relaxed),
                s->subs.load(std::memory_order_relaxed),
                s->skips.load(std::memory_order_relaxed),
                s->subsR.load(std::memory_order_relaxed),
                s->subsL.load(std::memory_order_relaxed),
                s->fallbacks.load(std::memory_order_relaxed));
    }
    uint64_t now = GetTickCount64();
    for (int i = 0; i < 2; ++i) {
        const Ray& r = g_ray[i];
        BVR_LOG("[aim]   ray %s valid=%d synth=%d origin=(%.1f %.1f %.1f) rot=(%d %d) "
                "testHold=%dms", i ? "R" : "L", r.valid ? 1 : 0, r.synthetic ? 1 : 0,
                r.origin.x, r.origin.y, r.origin.z, r.rot.pitch, r.rot.yaw,
                g_test[i].deadline > now ? static_cast<int>(g_test[i].deadline - now) : 0);
    }
    BVR_LOG("[aim]   objects: right=%p left=%p (learn events %u), gameplayView=%d",
            g_objRight, g_objLeft, g_learnEvents.load(std::memory_order_relaxed),
            g_gameplayView ? 1 : 0);
}

// ---- per-weapon profile machinery (session 21) ------------------------------

// Class names are plain ASCII; anything else maps to '_' so a hostile name
// cannot break the ini line format.
std::string narrow_key(const wchar_t* w) {
    std::string s;
    for (int i = 0; w && w[i] && i < 47; ++i)
        s.push_back(w[i] >= 32 && w[i] < 127 && w[i] != '.' && w[i] != '=' ? static_cast<char>(w[i])
                                                                           : '_');
    return s;
}

// s69: fold an ability class name down to the plasmid it is.
//
// Upgrade tiers are separate engine classes - ElectricBoltAbility,
// ElectricBoltTwoAbility, ElectricBoltThreeAbility, ElectricBoltZeroAbility,
// TelekinesisAbility, TelekinesisTwoAbility - so keying on the raw name would
// hand an upgraded plasmid a fresh profile and drop the position the tester had
// tuned. Strip the trailing "Ability", then a trailing tier word, and all tiers
// of one plasmid share a profile. Yields ~11 keys, which is what BioShock ships.
std::string plasmid_key(std::string s) {
    auto chop = [&s](const char* suffix) {
        const size_t n = strlen(suffix);
        if (s.size() > n && s.compare(s.size() - n, n, suffix) == 0) {
            s.erase(s.size() - n);
            return true;
        }
        return false;
    };
    chop("Ability");
    // Only one tier word, and only after "Ability" is gone.
    chop("Three") || chop("Two") || chop("Zero");
    return s.empty() ? std::string("Plasmid") : s;
}

// s69b: which HAND does a profile own? Weapons the right, plasmids the LEFT.
//
// Measured, not reasoned. Three observations in one session pin it down:
// the numpad tuning hand 1 did not move a plasmid; retargeted to active_hand()
// it did; and tuning one plasmid then moved the other. That is only possible if
// plasmids render from hand 0, hand 0 carries ONE shared set of values, and the
// profile layer - which read and wrote hand 1 throughout - was inert for them.
// Per-plasmid profiles existed but nothing they stored was ever rendered.
bool is_plasmid_key(const std::string& k) {
    static const char* kPlasmids[] = {
        "ElectricBolt", "Telekinesis", "Incineration", "IcicleAssault", "InsectSwarm",
        "SecurityBeacon", "SpringBoardTrap", "SummonProtector", "AirBlast",
        "BerserkRage", "DecoyHuman", "Plasmid"};
    for (const char* n : kPlasmids)
        if (k == n) return true;
    return false;
}
int profile_hand(const std::string& key) { return is_plasmid_key(key) ? 0 : 1; }

// Capture the live values of the active profile's OWN hand (no-op keyless).
void stash_active_profile() {
    if (g_weaponKey.empty()) return;
    const int ph = profile_hand(g_weaponKey);
    WeaponProfile& p = g_weaponProfiles[g_weaponKey];
    // s70: trimPitch/trimYaw are NOT stashed - the crosshair is global now. The
    // fields remain in the struct and in weapons.ini so existing files still
    // parse, but nothing reads them back.
    p.posFwd = g_posFwdCm[ph].load(std::memory_order_relaxed);
    p.posRight = g_posRightCm[ph].load(std::memory_order_relaxed);
    p.posUp = g_posUpCm[ph].load(std::memory_order_relaxed);
    hands::model_offset_cm(ph, &p.gripFwd, &p.gripRight, &p.gripUp);
    hands::view_offset_cm(ph, &p.viewFwd, &p.viewRight, &p.viewUp);
    p.animOn = bones::anim_allowed() ? 1.0f : 0.0f;
    p.modelPitch = hands::model_trim_pitch_deg(ph);
    p.modelYaw = hands::model_trim_yaw_deg(ph);
    p.modelRoll = hands::model_trim_roll_deg(ph);
}

void apply_weapon_key(const std::string& key, const char* why) {
    if (key == g_weaponKey) return;
    stash_active_profile();
    g_weaponKey = key;
    {
        std::lock_guard<std::mutex> lock(g_weaponKeyUiMutex);
        strcpy_s(g_weaponKeyUi, key.empty() ? "-" : key.c_str());
    }
    const int ph = profile_hand(key);
    if (key.empty()) {
        // s68: a genuinely UNKNOWN holdable (the rig itself unreadable). This is
        // no longer the plasmid case - that resolves to "Plasmid" below - so it
        // is rare, and it is worth saying out loud rather than silently leaving
        // the previous weapon's whole profile applied, which is what it does.
        BVR_LOG("[aim] holdable unresolved - profile UNCHANGED, '%s' stays applied "
                "(%s). Slider edits will land in that profile.",
                g_weaponKeyUi, why);
        return;
    }
    ++g_weaponSwaps;
    auto it = g_weaponProfiles.find(key);
    if (it == g_weaponProfiles.end()) {
        // First sight: seed from the PRESET BASELINE (the user's generic
        // tuning), not from the outgoing weapon's values - switching from a
        // tuned shotgun to a never-seen pistol must not inherit
        // shotgun-specific trims.
        WeaponProfile p = g_presetBaselineValid
                              ? g_presetBaseline
                              : WeaponProfile{g_pitchOffsetDeg[1].load(std::memory_order_relaxed),
                                              g_yawOffsetDeg[1].load(std::memory_order_relaxed),
                                              g_posFwdCm[1].load(std::memory_order_relaxed),
                                              g_posRightCm[1].load(std::memory_order_relaxed),
                                              g_posUpCm[1].load(std::memory_order_relaxed),
                                              0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
        if (!g_presetBaselineValid) {
            hands::model_offset_cm(ph, &p.gripFwd, &p.gripRight, &p.gripUp);
            hands::view_offset_cm(ph, &p.viewFwd, &p.viewRight, &p.viewUp);
            p.animOn = 1.0f;
            p.modelPitch = hands::model_trim_pitch_deg(ph);
            p.modelYaw = hands::model_trim_yaw_deg(ph);
            p.modelRoll = hands::model_trim_roll_deg(ph);
        }
        g_weaponProfiles[key] = p;
        // s70: the crosshair is global - trim is deliberately not applied here.
        g_posFwdCm[ph].store(p.posFwd, std::memory_order_relaxed);
        g_posRightCm[ph].store(p.posRight, std::memory_order_relaxed);
        g_posUpCm[ph].store(p.posUp, std::memory_order_relaxed);
        hands::set_model_offset_cm(ph, p.gripFwd, p.gripRight, p.gripUp);
        hands::set_view_offset_cm(ph, p.viewFwd, p.viewRight, p.viewUp);
        bones::set_anim_allowed(p.animOn != 0.0f);
        hands::set_model_trim_deg(ph, p.modelPitch, p.modelYaw, p.modelRoll);
        BVR_LOG("[aim] weapon profile '%s' CREATED from the %s (%s): pos %.1f/%.1f/%.1f "
                "[crosshair is global, not per profile]",
                key.c_str(), g_presetBaselineValid ? "preset baseline" : "current R values",
                why, p.posFwd, p.posRight, p.posUp);
    } else {
        const WeaponProfile& p = it->second;
        // s70: the crosshair is global - trim is deliberately not applied here.
        g_posFwdCm[ph].store(p.posFwd, std::memory_order_relaxed);
        g_posRightCm[ph].store(p.posRight, std::memory_order_relaxed);
        g_posUpCm[ph].store(p.posUp, std::memory_order_relaxed);
        hands::set_model_offset_cm(ph, p.gripFwd, p.gripRight, p.gripUp);
        hands::set_view_offset_cm(ph, p.viewFwd, p.viewRight, p.viewUp);
        bones::set_anim_allowed(p.animOn != 0.0f);
        hands::set_model_trim_deg(ph, p.modelPitch, p.modelYaw, p.modelRoll);
        // s70b: the trim is NOT printed here any more, and that matters. It was
        // still reporting the profile's stored trimPitch/trimYaw as "applied"
        // after the crosshair went global and stopped applying them - an
        // instrument describing behaviour the code no longer has, which is the
        // exact failure this file's own s68 notes were written about.
        BVR_LOG("[aim] weapon profile '%s' applied: pos %.1f/%.1f/%.1f "
                "grip %.1f/%.1f/%.1f (%s) [crosshair is global, not per profile]",
                key.c_str(), p.posFwd, p.posRight, p.posUp,
                p.gripFwd, p.gripRight, p.gripUp, why);
    }
}

// Scan-fallback dormancy (session 27). A structural latch, deliberately NOT a
// counter that the next momentary success zeroes - see the long note at the use
// site for why that distinction is the whole fix.
int g_weaponScanMisses = 0;
bool g_weaponScanDormant = false;

// Per-frame (throttled) weapon-change watch. The weapon actor accessor is
// cached + vtable-revalidated in hands.cpp; the class name resolves only on
// a pointer change, so steady-state cost is one pointer compare.
void update_weapon_profile(const FrameContext& ctx) {
    if (g_weaponKeySim) return; // a forced key holds until 'wkey real'
    if (!g_gameplayView) return; // no cutscene/menu heap scans
    // Idle until a value source exists: either the preset baseline was
    // captured (the user pressed VR PRESET 1) or ini profiles loaded. This
    // closes the headset-run-1 race where the first resolve beat the
    // preset's value load and seeded the first profile from zeros.
    if (!g_presetBaselineValid && g_weaponProfiles.empty()) return;
    static uint32_t throttle = 0;
    // Cheap-read cadence. The scan fallback has its own dormancy latch below,
    // so this no longer needs to widen (it used to stretch to 2047 frames on
    // repeated misses, which merely spread the multi-second heap walks out
    // rather than stopping them).
    if ((++throttle & 15) != 0) return;
    // Primary: Hands.CurrentHoldable raw off the rig, CLASS-AGNOSTIC
    // (session 21 part 3: MachineGun/GrenadeLauncher carry a different
    // native vtable, so the vtable-gated path rejected them and the stale
    // cache pinned the OLD key - their edits landed in the previous
    // weapon's profile). When the rig read works, its pointer is the
    // identity, period: no fallback may resurrect a stale weapon. The
    // legacy paths only serve states where the rig is not resolved yet.
    void* w = nullptr;
    bool haveRig = hands::current_holdable(&w);
    if (!haveRig) {
        w = hands::weapon_actor();
        // The SCAN fallback goes fully DORMANT after repeated misses. Session
        // 22 established the need (a reduced cadence still meant a
        // multi-second heap walk every ~2000 frames FOREVER on saves with no
        // resolvable weapon - user-felt as "the game freezes every couple of
        // seconds" at the Gatherer's Garden), but shipped it as a caller-local
        // counter that ANY momentary success zeroed. weapon_valid() is two
        // chained vtable compares on memory we do not own, so a stack slot or
        // a churning heap block satisfies it transiently, the counter resets,
        // and the walks come back - which is exactly what the session-27
        // external crash log shows. The latch below is structural: a success
        // clears the miss COUNT, only an explicit re-arm clears dormancy.
        //
        // Also gated on the STRICT gameplay predicate, not aim's own
        // g_gameplayView: that one deliberately counts the menu attract scene
        // as gameplay (see the hatch in on_calcview), which is how full heap
        // walks came to run at the main menu despite the comment above saying
        // they did not.
        if (!w && !g_weaponScanDormant && body::is_gameplay_view(ctx.viewActor)) {
            w = hands::resolve_weapon_actor(ctx);
            // A null return can mean "searched, not there" OR "the sliced sweep
            // has not finished yet". Only the first is a miss; counting the
            // second latched the scanner dormant before it had ever completed a
            // pass (caught in the first smoke test of the sliced scanner).
            if (!w && !hands::weapon_scan_in_progress() &&
                ++g_weaponScanMisses >= patterns::kScanMissesBeforeDormant) {
                g_weaponScanDormant = true;
                BVR_LOG("[aim] weapon scan fallback DORMANT after %d miss(es) - the cheap rig "
                        "and learned-actor reads keep running; a view-state change re-arms it",
                        g_weaponScanMisses);
            }
        }
    }
    if (haveRig || w) g_weaponScanMisses = 0;

    // ---- s69 THE PLASMID KEY, PER PLASMID -----------------------------------
    // `Hands.CurrentHoldable` is NULL while a plasmid is equipped - abilities
    // live in their own slot (`CurrentAbility`, patterns.h +0x454). s68 turned
    // that null into ONE key, "Plasmid", shared by every plasmid.
    //
    // That cannot work, and the tester's own BRVR config is the proof. With
    // PerPlasmidTuning=1 it carries wildly different rotations per plasmid -
    // PlasmidRot0 = -111,-64,22 against PlasmidRot1 = -35,-20,22 - because the
    // plasmids' authored poses genuinely differ. Nine attempts to find the
    // "right" shared capture instant failed because there is no such instant:
    // no single set of numbers serves both. BRVR never had the defect because
    // per-plasmid grip/rot ABSORBS the difference.
    //
    // So each plasmid gets its own profile, keyed on its class name through the
    // same path weapons already use. BRVR resolves identity by scanning the pawn
    // for AvailableAbilities and matching ActiveAbility into it; that is not
    // needed here, because object_class_name() walks obj -> +0x30 UClass ->
    // validated vtable -> +0x28 FName -> GNames and names an Ability instance
    // exactly as it names a Shotgun. No pawn scan, no new offsets.
    void* abil = nullptr;
    const bool haveAbil = hands::current_ability(&abil) && abil != nullptr;

    // IDENTITY IS THE PAIR. Both plasmids leave the holdable null, so keying the
    // change-detector on the holdable alone makes every plasmid look like every
    // other one and the switch is never seen.
    void* const ident = w ? w : abil;
    if (ident == g_weaponKeyActor && !(haveRig && !ident && g_weaponKey.empty())) return;
    g_weaponKeyActor = ident;

    std::string key;
    const char* why = "weapon change";
    if (haveRig && !w && haveAbil) {
        const wchar_t* aname = patterns::object_class_name(abil);
        if (aname) {
            key = plasmid_key(narrow_key(aname));
            why = "plasmid equipped (Hands.CurrentAbility)";
        } else {
            // Degrade to s68's shared key rather than to nothing: a plasmid with
            // one set of numbers is worse than per-plasmid, and far better than
            // the outgoing weapon's profile staying applied.
            key = "Plasmid";
            why = "plasmid equipped, class name unresolved - shared fallback";
            static bool s_warned = false;
            if (!s_warned) {
                s_warned = true;
                BVR_LOG("[aim] plasmid %p has NO resolvable class name - falling back to "
                        "the shared 'Plasmid' profile. Per-plasmid tuning is OFF until "
                        "this resolves; report this line.",
                        abil);
            }
        }
    } else if (haveRig && !w) {
        why = "hands empty (no holdable, no ability)";
    } else {
        const wchar_t* name = w ? patterns::object_class_name(w) : nullptr;
        if (w && !name)
            BVR_LOG("[aim] holdable %p has NO resolvable class name - profile key cleared, "
                    "slider edits will touch no profile until it resolves",
                    w);
        if (name) key = narrow_key(name);
    }
    apply_weapon_key(key, why);
}

// DEFAULT PROFILES = the tester's calibrated set. Refreshed s67 after the
// viewmodel drive was rebuilt on BRVR's architecture (actor carries the rig,
// cluster frozen, bone 43 position-only), so every field below was measured
// against THAT drive and none of it transfers to the old bone retarget.
//
// Field order matches WeaponProfile: trimPitch, trimYaw (the AIM ray - dot and
// bullet come off it), posFwd/Right/Up (aim origin), animOn (may this weapon
// adopt engine animation - 0 on the wrench so manual swinging is not fought by
// a swing animation), viewFwd/Right/Up (PLACEMENT - where the gun sits; safe),
// gripFwd/Right/Up (PIVOT - what it turns about; BRVR's values, leave alone),
// modelPitch/Yaw/Roll.
//
// The wrench keeps BRVR's PIVOT but carries its own aim trim and placement,
// re-tuned after the s67 animOn bug was fixed (before the fix it was wearing
// the previous weapon's reference pose, so its earlier numbers measured that
// bug rather than the wrench). animOn=0: manual swinging, no swing animation.
//
// Seeded BEFORE weapons.ini loads, so a user's own file always overrides,
// key by key.
void seed_default_profiles() {
    // s68: DESIGNATED INITIALISERS, and that is the point of this rewrite.
    //
    // These rows used to be bare aggregate lists written in the order the comment
    // above gives (trim, pos, animOn, view, grip, model) while the STRUCT declares
    // grip BEFORE view and animOn AFTER both - so every field past posUp was
    // silently assigned to the wrong member. The Wrench came out with
    // animOn = -16.70: nonzero, so its animation gate read as ON, which is the one
    // thing it must not be. Every weapon's grip and placement were scrambled into
    // each other besides.
    //
    // Worse, a stale duplicate block underneath re-assigned all eight with FIVE
    // values each, and aggregate init zero-fills the rest - grip, view, animOn and
    // the model trims all went to zero, so a FRESH INSTALL had no recoil on any
    // weapon at all. Both bugs were invisible to anyone whose weapons.ini already
    // overrode every field by name, which is how they survived s67 and a headset
    // run: the tester's own file was hiding them.
    //
    // Named initialisers cannot drift out of order again. The values are the
    // tester's calibrated set, transcribed from their weapons.ini - which is what
    // the live tuning writes, and therefore the ground truth.
    //
    // Seeded BEFORE weapons.ini loads, so a user's own file always overrides,
    // key by key.
    //
    // "Plasmid" is s68 and is ONE key for EVERY plasmid - the tester's call,
    // "global, not per plasmid". See resolve_holdable_key() for why a plasmid
    // needs a key at all: the engine parks Hands.CurrentHoldable at NULL while
    // one is equipped, which used to leave the last WEAPON's profile applied.
    g_weaponProfiles["ChemicalThrower"] = {
        .trimPitch = 0.83f, .trimYaw = -9.20f,
        .posFwd = 0.93f, .posRight = -0.43f, .posUp = -9.78f,
        .gripFwd = 42.00f, .gripRight = 14.70f, .gripUp = 1.00f,
        .viewFwd = 0.00f, .viewRight = 0.00f, .viewUp = 15.00f,
        .animOn = 1.00f,
        .modelPitch = 0.00f, .modelYaw = 0.00f, .modelRoll = 0.00f};
    g_weaponProfiles["Crossbow"] = {
        .trimPitch = 0.83f, .trimYaw = -9.20f,
        .posFwd = 0.00f, .posRight = -4.40f, .posUp = 11.00f,
        .gripFwd = 44.00f, .gripRight = 14.70f, .gripUp = -21.00f,
        .viewFwd = 0.00f, .viewRight = 0.00f, .viewUp = 15.00f,
        .animOn = 1.00f,
        .modelPitch = 0.00f, .modelYaw = 0.00f, .modelRoll = 0.00f};
    g_weaponProfiles["GrenadeLauncher"] = {
        .trimPitch = 0.83f, .trimYaw = -9.20f,
        .posFwd = 0.00f, .posRight = -2.80f, .posUp = 7.50f,
        .gripFwd = 24.00f, .gripRight = 16.70f, .gripUp = -15.00f,
        .viewFwd = 0.00f, .viewRight = 0.00f, .viewUp = 15.00f,
        .animOn = 1.00f,
        .modelPitch = 0.00f, .modelYaw = 0.00f, .modelRoll = 0.00f};
    g_weaponProfiles["MachineGun"] = {
        .trimPitch = 1.83f, .trimYaw = -10.20f,
        .posFwd = 0.00f, .posRight = -3.50f, .posUp = 13.10f,
        .gripFwd = 52.00f, .gripRight = 17.00f, .gripUp = -14.70f,
        .viewFwd = 1.50f, .viewRight = 0.50f, .viewUp = 12.00f,
        .animOn = 1.00f,
        .modelPitch = -0.50f, .modelYaw = -9.00f, .modelRoll = -1.00f};
    g_weaponProfiles["Pistol"] = {
        .trimPitch = 2.83f, .trimYaw = -9.70f,
        .posFwd = -0.70f, .posRight = -2.06f, .posUp = 18.71f,
        .gripFwd = 44.00f, .gripRight = 16.70f, .gripUp = -15.40f,
        .viewFwd = 2.00f, .viewRight = 0.00f, .viewUp = 11.00f,
        .animOn = 1.00f,
        .modelPitch = 0.00f, .modelYaw = -8.00f, .modelRoll = 0.00f};
    g_weaponProfiles["ResearchCamera"] = {
        .trimPitch = 0.83f, .trimYaw = -9.20f,
        .posFwd = 0.00f, .posRight = -2.80f, .posUp = 7.50f,
        .gripFwd = 44.00f, .gripRight = 14.70f, .gripUp = -13.00f,
        .viewFwd = 0.00f, .viewRight = 0.00f, .viewUp = 15.00f,
        .animOn = 1.00f,
        .modelPitch = 0.00f, .modelYaw = 0.00f, .modelRoll = 0.00f};
    g_weaponProfiles["Shotgun"] = {
        .trimPitch = -2.17f, .trimYaw = -6.70f,
        .posFwd = 0.00f, .posRight = -2.76f, .posUp = 7.50f,
        .gripFwd = 16.00f, .gripRight = 11.80f, .gripUp = -11.80f,
        .viewFwd = 2.50f, .viewRight = 1.00f, .viewUp = 6.00f,
        .animOn = 1.00f,
        .modelPitch = -2.00f, .modelYaw = -8.00f, .modelRoll = 0.00f};
    g_weaponProfiles["Wrench"] = {
        .trimPitch = -6.67f, .trimYaw = -14.70f,
        .posFwd = 0.00f, .posRight = -2.80f, .posUp = 7.50f,
        .gripFwd = 58.00f, .gripRight = 18.30f, .gripUp = -16.70f,
        .viewFwd = -4.00f, .viewRight = -2.00f, .viewUp = 15.00f,
        .animOn = 0.00f,
        .modelPitch = -12.00f, .modelYaw = -16.00f, .modelRoll = -18.00f};
    // s69b PLASMIDS: ONE row for every plasmid, and it is a LEFT-HAND row.
    //
    // s70d: ALL ELEVEN START FROM THE TESTER'S OWN TUNED PLASMID ROW, which is
    // where they were before s70c and where testing put them back.
    //
    // BRVR's config does show the authored poses differ a lot between plasmids
    // (PlasmidRot0 -111,-64,22 against PlasmidRot1 -35,-20,22, and 2/4/5 near
    // -111,-14,22), so per-plasmid rotation is real and this table should
    // eventually carry eleven different rows. What it must NOT carry is BRVR's
    // numbers transcribed across: that was tried in s70c and moved the plasmids
    // to the wrong position, because the model trim rotation is the basis the
    // grip offset is applied along - the two are one calibration, not two.
    //
    // Derive them on this rig with the numpad tuner, one plasmid at a time.
    WeaponProfile kPlasmid{
        .trimPitch = -11.00f, .trimYaw = 37.00f,
        .posFwd = -2.80f, .posRight = 0.60f, .posUp = 0.50f,
        .gripFwd = 45.50f, .gripRight = -14.90f, .gripUp = -12.30f,
        .viewFwd = 0.00f, .viewRight = -4.00f, .viewUp = 13.00f,
        .animOn = 1.00f,
        .modelPitch = -59.00f, .modelYaw = -32.00f, .modelRoll = 22.00f};
    for (const char* k : {"ElectricBolt", "Telekinesis", "Incineration", "IcicleAssault",
                          "InsectSwarm", "SecurityBeacon", "SpringBoardTrap",
                          "SummonProtector", "AirBlast", "BerserkRage", "DecoyHuman",
                          "Plasmid"})
        g_weaponProfiles[k] = kPlasmid;

    // ---- s70d: THE BRVR ROTATIONS WERE TRIED AND REVERTED. Read this before
    // ---- transcribing them again.
    //
    // s70c set these from the tester's BRVR config - ElectroBolt to
    // PlasmidRot0 (-111,-64,22) and Telekinesis to PlasmidRot1 (-35,-20,22),
    // with PlasmidGrip1 for Telekinesis. Tested, and it was WORSE: the plasmids
    // moved to the wrong POSITION and the animation direction did not change at
    // all.
    //
    // TWO THINGS THAT COST NOTHING TO KNOW, both from that one run:
    //
    //   1. The model trim ROTATION MOVES THE PLASMID. Position was correct
    //      before and wrong after, and rotation was all that changed - so the
    //      grip offset is applied along the model's rotated basis and the two
    //      cannot be transcribed independently. BRVR's rot values are not
    //      portable without its grip values AND whatever else composes that
    //      basis; the matching roll of 22.00 was not enough to license the
    //      transfer, and CLAUDE.md's "never copy a number, derive it" applied
    //      here even though both mods are the same game.
    //
    //   2. A 48 DEG YAW CHANGE DID NOT MOVE THE ANIMATION DIRECTION. That is
    //      the useful half. If the direction were a product of the actor's
    //      rotation, 48 deg would have been unmissable. It is not the model
    //      trim, and it is not the drive hand, the freeze anchor or the
    //      adoption policy either - the symptom has now survived a change to
    //      every one of them. Look somewhere else entirely.
    //
    // So these are the tester's own tuned values, unchanged, and they stay that
    // way until something derives a replacement on this rig.
    g_weaponProfiles["ElectricBolt"].viewFwd = -2.00f;
    g_weaponProfiles["ElectricBolt"].viewRight = 2.00f;
    g_weaponProfiles["ElectricBolt"].viewUp = 11.00f;
    g_weaponProfiles["ElectricBolt"].modelPitch = -111.00f;
    g_weaponProfiles["ElectricBolt"].modelYaw = -16.00f;
    g_weaponProfiles["ElectricBolt"].modelRoll = 22.00f;

    // TELEKINESIS ANIMATES, and this one DID hold. s69b gated it off because its
    // grab animation threw the rig "super far away" - but that was the mode-3
    // late write never replaying the cluster (s69d), so ANY adopted left-hand
    // animation walked off. The animation was never the fault.
    //
    // SET EXPLICITLY, because a weapons.ini written while the gate was on still
    // carries animOn=0 and that file overrides these seeds. A stale 0 there was
    // the whole of "tele doesnt animate at all".
    g_weaponProfiles["Telekinesis"].animOn = 1.00f;
}

void weapons_ini_path(wchar_t* out, size_t count) {
    swprintf_s(out, count, L"%s\\weapons.ini", bvr::log::data_dir());
}

void load_weapon_profiles() {
    wchar_t path[MAX_PATH];
    weapons_ini_path(path, MAX_PATH);
    FILE* f = nullptr;
    if (_wfopen_s(&f, path, L"r") != 0 || !f) return; // no file yet is normal
    char line[256];
    int n = 0;
    while (fgets(line, sizeof line, f)) {
        char key[48] = {};
        char field[32] = {};
        float v = 0.0f;
        if (sscanf_s(line, "%47[^.].%31[^=]=%f", key, static_cast<unsigned>(sizeof key), field,
                     static_cast<unsigned>(sizeof field), &v) != 3)
            continue;
        WeaponProfile& p = g_weaponProfiles[key];
        if (strcmp(field, "trimPitch") == 0) p.trimPitch = v;
        else if (strcmp(field, "trimYaw") == 0) p.trimYaw = v;
        else if (strcmp(field, "posFwd") == 0) p.posFwd = v;
        else if (strcmp(field, "posRight") == 0) p.posRight = v;
        else if (strcmp(field, "posUp") == 0) p.posUp = v;
        // s65 grip keys. A file written before this existed simply has none of
        // them, they stay 0, and the model sits exactly where it used to.
        else if (strcmp(field, "animOn") == 0) p.animOn = v;
        else if (strcmp(field, "viewFwd") == 0) p.viewFwd = v;
        else if (strcmp(field, "viewRight") == 0) p.viewRight = v;
        else if (strcmp(field, "viewUp") == 0) p.viewUp = v;
        else if (strcmp(field, "gripFwd") == 0) p.gripFwd = v;
        else if (strcmp(field, "gripRight") == 0) p.gripRight = v;
        else if (strcmp(field, "gripUp") == 0) p.gripUp = v;
        else if (strcmp(field, "modelPitch") == 0) p.modelPitch = v;
        else if (strcmp(field, "modelYaw") == 0) p.modelYaw = v;
        else if (strcmp(field, "modelRoll") == 0) p.modelRoll = v;
        else continue;
        ++n;
    }
    fclose(f);
    if (n)
        BVR_LOG("[aim] %d weapon-profile value(s) loaded from weapons.ini (%u weapon(s))", n,
                static_cast<unsigned>(g_weaponProfiles.size()));

    // s70c: SAY OUT LOUD WHICH PROFILES HAVE ANIMATION GATED OFF.
    //
    // This file OVERRIDES the seeds, and animOn=0 is a gate that silently
    // disables a whole feature for one holdable. Telekinesis carried a stale 0
    // from s69b - written when its grab animation was blamed for throwing the
    // rig, a premise s69d then disproved - and it survived every later change
    // because nothing ever mentioned it. The tester reported it as "tele doesnt
    // animate at all" and the search went to thresholds and settle windows,
    // where the answer was one line in a config file.
    //
    // A gate nobody can see is a gate nobody can question, so it gets a line.
    for (const auto& kv : g_weaponProfiles)
        if (kv.second.animOn == 0.0f)
            BVR_LOG("[aim] NOTE: '%s' has engine animation GATED OFF (animOn=0). Its rig "
                    "will not move at all. Intended for the wrench; anything else here is "
                    "probably a stale value in weapons.ini.",
                    kv.first.c_str());
}

} // namespace

void init(const bvr::pattern_scan::ProcessImage& image, const patterns::Symbols& symbols) {
    g_imageBase = image.base;
    g_image = image;
    g_syms = symbols;
    seed_default_profiles();
    load_weapon_profiles(); // a user's weapons.ini overrides the seeds key by key
    BVR_LOG("[aim] init: weapon fire-start=%p ability fire-start=%p", g_syms.weaponFireStart,
            g_syms.abilityFireStart);
}

void note_preset_baseline() {
    // Called right after the preset's value load: the R atomics now hold
    // the user's generic (non-per-weapon) tuning. New profiles seed from
    // this; its existence also un-idles the profile resolver.
    g_presetBaseline = {g_pitchOffsetDeg[1].load(std::memory_order_relaxed),
                        g_yawOffsetDeg[1].load(std::memory_order_relaxed),
                        g_posFwdCm[1].load(std::memory_order_relaxed),
                        g_posRightCm[1].load(std::memory_order_relaxed),
                        g_posUpCm[1].load(std::memory_order_relaxed), 0.0f, 0.0f, 0.0f};
    hands::model_offset_cm(1, &g_presetBaseline.gripFwd, &g_presetBaseline.gripRight,
                           &g_presetBaseline.gripUp);
    hands::view_offset_cm(1, &g_presetBaseline.viewFwd, &g_presetBaseline.viewRight,
                          &g_presetBaseline.viewUp);
    g_presetBaseline.animOn = 1.0f;
    g_presetBaseline.modelPitch = hands::model_trim_pitch_deg(1);
    g_presetBaseline.modelYaw = hands::model_trim_yaw_deg(1);
    g_presetBaseline.modelRoll = hands::model_trim_roll_deg(1);
    g_presetBaselineValid = true;
    BVR_LOG("[aim] preset R baseline noted: trim %.2f/%.2f pos %.1f/%.1f/%.1f (seeds new "
            "weapon profiles)",
            g_presetBaseline.trimPitch, g_presetBaseline.trimYaw, g_presetBaseline.posFwd,
            g_presetBaseline.posRight, g_presetBaseline.posUp);
}

void reapply_weapon_profile() {
    // The VR preset's value load writes the vrpreset.ini R trims over
    // whatever profile was live (flat-caught: profile applied, preset chain
    // finished seconds later, R reverted). Re-apply WITHOUT stashing - the
    // preset values are a baseline, not a profile edit.
    if (g_weaponKey.empty()) return;
    auto it = g_weaponProfiles.find(g_weaponKey);
    if (it == g_weaponProfiles.end()) return;
    const WeaponProfile& p = it->second;
    // s70: trim is global and is not restored from a profile.
    g_posFwdCm[1].store(p.posFwd, std::memory_order_relaxed);
    g_posRightCm[1].store(p.posRight, std::memory_order_relaxed);
    g_posUpCm[1].store(p.posUp, std::memory_order_relaxed);
    BVR_LOG("[aim] weapon profile '%s' re-applied after preset load", g_weaponKey.c_str());
}

void save_weapon_profiles() {
    stash_active_profile(); // the live R values are the active profile's truth
    if (g_weaponProfiles.empty()) return;
    wchar_t path[MAX_PATH];
    weapons_ini_path(path, MAX_PATH);
    FILE* f = nullptr;
    if (_wfopen_s(&f, path, L"w") != 0 || !f) {
        BVR_LOG("[aim] could not write weapons.ini");
        return;
    }
    fprintf(f, "# BioShock VR - per-weapon R-hand aim profiles (session 21).\n");
    fprintf(f, "# <WeaponClassName>.<field>=<value>; trims in degrees, pos offsets in cm.\n");
    for (const auto& [key, p] : g_weaponProfiles) {
        fprintf(f, "%s.trimPitch=%.2f\n", key.c_str(), p.trimPitch);
        fprintf(f, "%s.trimYaw=%.2f\n", key.c_str(), p.trimYaw);
        fprintf(f, "%s.posFwd=%.2f\n", key.c_str(), p.posFwd);
        fprintf(f, "%s.posRight=%.2f\n", key.c_str(), p.posRight);
        fprintf(f, "%s.posUp=%.2f\n", key.c_str(), p.posUp);

        fprintf(f, "%s.animOn=%.0f\n", key.c_str(), p.animOn);
        fprintf(f, "%s.viewFwd=%.2f\n", key.c_str(), p.viewFwd);
        fprintf(f, "%s.viewRight=%.2f\n", key.c_str(), p.viewRight);
        fprintf(f, "%s.viewUp=%.2f\n", key.c_str(), p.viewUp);
        fprintf(f, "%s.gripFwd=%.2f\n", key.c_str(), p.gripFwd);

        fprintf(f, "%s.gripRight=%.2f\n", key.c_str(), p.gripRight);

        fprintf(f, "%s.gripUp=%.2f\n", key.c_str(), p.gripUp);
        fprintf(f, "%s.modelPitch=%.2f\n", key.c_str(), p.modelPitch);
        fprintf(f, "%s.modelYaw=%.2f\n", key.c_str(), p.modelYaw);
        fprintf(f, "%s.modelRoll=%.2f\n", key.c_str(), p.modelRoll);
    }
    fclose(f);
    BVR_LOG("[aim] %u weapon profile(s) saved to weapons.ini",
            static_cast<unsigned>(g_weaponProfiles.size()));
}

// The origin offset rides the FINAL (trimmed) ray frame - both lanes, the
// synthetic test lane included, which is what makes it flat-assertable via
// the ray origins in `vraim status`.
void apply_origin_offset(int i, Ray& out, float worldScale) {
    float of = g_posFwdCm[i].load(std::memory_order_relaxed);
    float orr = g_posRightCm[i].load(std::memory_order_relaxed);
    float ou = g_posUpCm[i].load(std::memory_order_relaxed);
    if (of == 0.0f && orr == 0.0f && ou == 0.0f) return;
    float fwd[3], right[3], up[3];
    ue_rot_basis(out.rot, fwd, right, up);
    float uuPerCm = worldScale / 100.0f;
    of *= uuPerCm;
    orr *= uuPerCm;
    ou *= uuPerCm;
    out.origin.x += fwd[0] * of + right[0] * orr + up[0] * ou;
    out.origin.y += fwd[1] * of + right[1] * orr + up[1] * ou;
    out.origin.z += fwd[2] * of + right[2] * orr + up[2] * ou;
}

// Last published FrameContext, cached for `vraim synccheck` (session 20): the
// sweep needs a real frame's transform to map through, outside the frame.
// Written and read on the game thread only (commands run there too).
static FrameContext g_lastCtx{};
static bool g_haveCtx = false;

void on_calcview(const FrameContext& ctx) {
    uint64_t now = GetTickCount64();
    g_rayStampMs = now;
    g_lastCtx = ctx;
    g_haveCtx = true;
    g_worldScaleCache = ctx.worldScale > 1.0f ? ctx.worldScale : 100.0f;

    // Apply an overlay request from THIS thread (see g_pendingEnable).
    int pending = g_pendingEnable.exchange(-1, std::memory_order_relaxed);
    if (pending == 1) {
        if (install_all()) {
            g_enabled.store(true, std::memory_order_relaxed);
            BVR_LOG("[aim] ON (overlay) - controller aim substitutes the view aim");
        } else {
            BVR_LOG("[aim] overlay enable REFUSED: no aim seam hooked");
        }
    } else if (pending == 0) {
        g_enabled.store(false, std::memory_order_relaxed);
        if (!g_probe.load(std::memory_order_relaxed)) disable_all();
        BVR_LOG("[aim] OFF (overlay) - engine aim restored");
    }

    // World change (save load, level transition): the PlayerController is
    // rebuilt, and every weapon/ability object we learned a hand for died with
    // the old world. Drop the map rather than risk a recycled heap address
    // being mistaken for the old weapon and aiming a plasmid with the right
    // hand. Comparing stale pointers is safe, but this makes it moot.
    if (ctx.pc != g_lastPc) {
        if (g_lastPc && (g_objRight || g_objLeft))
            BVR_LOG("[aim] world changed (pc %p -> %p) - hand map cleared", g_lastPc, ctx.pc);
        g_lastPc = ctx.pc;
        g_objRight = nullptr;
        g_objLeft = nullptr;
        g_weaponKeyActor = nullptr; // weapon actor died with the world - re-resolve
    }

    // Cutscene guard (the open M3 item): the view actor during normal play is
    // the player's own pawn - an AShockPlayer. A scripted camera swaps the
    // view target for some other actor, and then nothing about the player's
    // aim should be touched.
    g_gameplayView = false;
    if (ctx.viewActor) {
        void* vtbl = nullptr;
        if (read_ptr(ctx.viewActor, &vtbl))
            g_gameplayView = (to_rva(vtbl) == patterns::kShockPlayerVtableRva);
        if (!g_gameplayView && ctx.viewActor == ctx.pc)
            g_gameplayView = true; // menu attract scene views through the PC itself
    }

    // Session 29: made explicit here for the same reason as in hands.cpp. This
    // ONE line covers three things at once, because they all read
    // g_gameplayView: the fire-seam substitution (via ray_for), the per-weapon
    // profile heap scans, and the laser publish. The aim path fails safe by
    // construction - `out = {}` below leaves valid=false, so the engine's own
    // GetPerfectFireStart values survive untouched.
    if (g_gameplayView && bvr::hud::cinematic_hold() &&
        bvr::vr::cine_drive() != bvr::vr::CineDrive::Off)
        g_gameplayView = false;

    // Per-weapon profile hot-swap (session 21): the active R-hand trim/offsets
    // follow the equipped weapon's class (gameplay view only - see the guard).
    update_weapon_profile(ctx);

    for (int i = 0; i < 2; ++i) {
        Ray& out = g_ray[i];
        out = {};

        // 1) Synthetic injection wins (this is the no-headset test lane).
        if (now < g_test[i].deadline) {
            out.valid = true;
            out.synthetic = true;
            out.origin = {ctx.camX, ctx.camY, ctx.camZ};
            out.rot.yaw = ctx.camYaw +
                          static_cast<int32_t>(g_test[i].yawDeg * kRotUnitsPerDegree);
            out.rot.pitch = ctx.camPitch +
                            static_cast<int32_t>(g_test[i].pitchDeg * kRotUnitsPerDegree);
            out.rot.roll = 0;
            apply_origin_offset(i, out, ctx.worldScale);
            continue;
        }

        // 2) The real thing: the hand's pose, mapped through EXACTLY the
        //    transform the camera drive used this frame (frame_context.h -
        //    shared with the M7 hand viewmodel so the two cannot drift).
        bvr::vr::HeadPose hp{};
        bool useAim = g_useAimPose.load(std::memory_order_relaxed);
        if (!ctx.vrDriving || !bvr::vr::get_hand_pose(i, useAim, hp)) continue;

        const float pos[3] = {hp.px, hp.py, hp.pz};
        const float quat[4] = {hp.qx, hp.qy, hp.qz, hp.qw};
        // The trimmed chain is a pure function in frame_context.h, shared
        // with `vraim synccheck` (roll forced 0 there - the camera owns roll).
        GamePose gp = ray_pose_from_xr(ctx, pos, quat,
                                       g_pitchOffsetDeg[i].load(std::memory_order_relaxed),
                                       g_yawOffsetDeg[i].load(std::memory_order_relaxed));
        out.origin = gp.loc;
        out.rot = gp.rot;

        // Muzzle ray (session 20, right hand only): the bullet leaves along
        // the RENDERED barrel. The model's target rotation is recomputed here
        // through the same pure function hands.cpp uses this frame (same ctx,
        // same funnel pose, same trims - no staleness, no coupling), then
        // applied to the reference barrel axis. Falls back to the trimmed
        // controller ray whenever the rig has no reference yet.
        if (i == 1 && g_muzzleRay.load(std::memory_order_relaxed)) {
            float d0[3];
            if (bvr::b1r::bones::barrel_ref_axis(d0)) {
                GamePose mgp = model_pose_from_xr(ctx, pos, quat,
                                                  hands::model_trim_pitch_deg(1),
                                                  hands::model_trim_yaw_deg(1),
                                                  hands::model_trim_roll_deg(1));
                float qt[4], dirW[3];
                ue_rot_to_quat(mgp.rot, qt);
                quat_rotate(qt[0], qt[1], qt[2], qt[3], d0, dirW);
                out.rot = ue_dir_to_rot(dirW); // pitch/yaw; roll stays 0
            }
        }

        apply_origin_offset(i, out, ctx.worldScale);
        out.valid = true;
    }

    // Publish the laser for the render thread. It reads the aim pose itself at
    // submit time (later than this, so the dots are as fresh as the frame), and
    // takes the trim from here so the beam and the bullet are one ray.
    bvr::vr::LaserConfig lc{};
    lc.enabled = g_laser.load(std::memory_order_relaxed) && ctx.vrDriving && g_gameplayView;
    lc.hand = hands::active_hand();
    int lh = lc.hand == 0 ? 0 : 1;
    lc.pitchTrimDeg = g_pitchOffsetDeg[lh].load(std::memory_order_relaxed);
    lc.yawTrimDeg = g_yawOffsetDeg[lh].load(std::memory_order_relaxed);
    lc.posFwdCm = g_posFwdCm[lh].load(std::memory_order_relaxed);
    lc.posRightCm = g_posRightCm[lh].load(std::memory_order_relaxed);
    lc.posUpCm = g_posUpCm[lh].load(std::memory_order_relaxed);
    lc.dots = g_laserDots.load(std::memory_order_relaxed);
    lc.nearM = g_laserNearM.load(std::memory_order_relaxed);
    lc.farM = g_laserFarM.load(std::memory_order_relaxed);
    lc.sizeDeg = g_laserSizeDeg.load(std::memory_order_relaxed);
    // Muzzle ray (session 20): the beam must follow the bullet. d0 converts
    // UE (fwd,right,up) -> XR (right,up,-fwd): (y, z, -x).
    if (lc.hand == 1 && g_muzzleRay.load(std::memory_order_relaxed)) {
        float d0[3];
        if (bvr::b1r::bones::barrel_ref_axis(d0)) {
            lc.muzzle = true;
            lc.muzzleD0[0] = d0[1];
            lc.muzzleD0[1] = d0[2];
            lc.muzzleD0[2] = -d0[0];
            lc.modelPitchTrimDeg = hands::model_trim_pitch_deg(1);
            lc.modelYawTrimDeg = hands::model_trim_yaw_deg(1);
            lc.modelRollTrimDeg = hands::model_trim_roll_deg(1);
        }
    }
    bvr::vr::set_laser(lc);

    // Session 29: publish the aim DOT from the finished ray, not from a second
    // derivation of it. Gated on exactly what ray_for() checks, so the dot is
    // visible if and only if a shot fired right now would be substituted -
    // which makes the dot an instrument as well as a sight.
    bvr::vr::AimDotConfig dc{};
    dc.enabled = g_dot.load(std::memory_order_relaxed);
    dc.sizeDeg = g_dotSizeDeg.load(std::memory_order_relaxed);
    const Ray& dr = g_ray[lh];
    if (dc.enabled && g_enabled.load(std::memory_order_relaxed) && g_gameplayView && dr.valid) {
        float dir[3];
        ue_rot_to_dir(dr.rot, dir);
        float distUu = g_dotDistM.load(std::memory_order_relaxed) * ctx.worldScale;
        FVector at{dr.origin.x + dir[0] * distUu, dr.origin.y + dir[1] * distUu,
                   dr.origin.z + dir[2] * distUu};
        game_point_to_xr(ctx, at, dc.posXr);
        dc.valid = true;

        // The inverse is only worth trusting if it round-trips. Check it ONCE
        // against the forward map on real data and say so in words - a
        // transform that is wrong by a yaw term looks perfectly plausible in
        // the headset until you fire.
        static bool s_checked = false;
        if (!s_checked) {
            s_checked = true;
            GamePose back = xr_pose_to_game(ctx, dc.posXr, kIdentQuat);
            float ex = back.loc.x - at.x, ey = back.loc.y - at.y, ez = back.loc.z - at.z;
            float err = sqrtf(ex * ex + ey * ey + ez * ez);
            BVR_LOG("[aim] dot transform round-trip error %.4f UU (%.5f mm at worldScale "
                    "%.0f) - %s",
                    err, err * 10.0f / (ctx.worldScale / 100.0f), ctx.worldScale,
                    err < 0.05f ? "EXACT, the dot is the fire-seam point"
                                : "NOT EXACT - do not trust the dot as calibration");
        }
    }
    bvr::vr::set_aim_dot(dc);
}

// ---- synccheck (session 20): ray-vs-barrel divergence sweep -----------------
// Sweeps axis-angle controller orientations - INCLUDING rolled poses, where
// the two algebras differ most - through BOTH pure pose->rot chains
// (frame_context.h) against the cached last FrameContext, and prints the angle
// between the ray direction and the model barrel direction per pose. Two trim
// sets per pose: the LIVE trims (what the user's tuning experiences) and a
// canonical 10/10 trim fed IDENTICALLY to both chains, so any canonical
// divergence is pure algebra difference - the deterministic gate. Position is
// deliberately NOT part of the gate: the render lock shifts position only
// (bones.cpp - never rotation, quoted separately below) and the two
// origin-offset bases are tuned against each other by design.
static void run_synccheck() {
    if (!g_haveCtx) {
        BVR_LOG("[sync] no FrameContext cached yet - enter gameplay first");
        return;
    }
    const FrameContext ctx = g_lastCtx;
    const float pos[3] = {0.15f, -0.20f, -0.35f}; // the sim lane's hand spot

    struct AxisAngle { float x, y, z, deg; };
    static const AxisAngle kPoses[] = {
        {0, 0, 1, 0},                                       // identity
        {1, 0, 0, 45},  {1, 0, 0, -45}, {1, 0, 0, 90},      // pitch-ish (XR +X)
        {0, 1, 0, 45},  {0, 1, 0, -45}, {0, 1, 0, 90},      // yaw-ish (XR +Y)
        {0, 0, 1, 45},  {0, 0, 1, -45}, {0, 0, 1, 90},      // ROLL (view axis)
        {0, 0, 1, 180},
        {1, 1, 0, 60},  {1, 0, 1, 60},  {0, 1, 1, 60},      // mixed axes
        {1, -1, 0, 60}, {1, 0, -1, 60}, {0, 1, -1, 60},
        {1, 1, 1, 60},  {1, 1, 1, 120}, {1, -1, 1, 90}, {-1, 1, 1, 90},
    };
    constexpr int kPoseCount = static_cast<int>(sizeof kPoses / sizeof kPoses[0]);

    // Angle between the two chains' forward directions for one trim set.
    auto diverge = [&](const float q[4], float rayPitch, float rayYaw, float mPitch,
                       float mYaw, float mRoll) {
        GamePose rp = ray_pose_from_xr(ctx, pos, q, rayPitch, rayYaw);
        GamePose mp = model_pose_from_xr(ctx, pos, q, mPitch, mYaw, mRoll);
        float dr[3], dm[3];
        ue_rot_to_dir(rp.rot, dr);
        ue_rot_to_dir(mp.rot, dm);
        float dot = dr[0] * dm[0] + dr[1] * dm[1] + dr[2] * dm[2];
        if (dot > 1.0f) dot = 1.0f;
        if (dot < -1.0f) dot = -1.0f;
        return acosf(dot) * kRadToDeg;
    };

    const float rayP[2] = {g_pitchOffsetDeg[0].load(std::memory_order_relaxed),
                           g_pitchOffsetDeg[1].load(std::memory_order_relaxed)};
    const float rayY[2] = {g_yawOffsetDeg[0].load(std::memory_order_relaxed),
                           g_yawOffsetDeg[1].load(std::memory_order_relaxed)};
    BVR_LOG("[sync] sweep of %d poses | ray trim L(%+.1f,%+.1f) R(%+.1f,%+.1f) | "
            "model trim L(%+.1f,%+.1f,%+.1f) R(%+.1f,%+.1f,%+.1f) | canon 10/10 both chains",
            kPoseCount, rayP[0], rayY[0], rayP[1], rayY[1], hands::model_trim_pitch_deg(0),
            hands::model_trim_yaw_deg(0), hands::model_trim_roll_deg(0),
            hands::model_trim_pitch_deg(1), hands::model_trim_yaw_deg(1),
            hands::model_trim_roll_deg(1));

    float maxL = 0.0f, maxR = 0.0f, maxC = 0.0f;
    for (int p = 0; p < kPoseCount; ++p) {
        const AxisAngle& aa = kPoses[p];
        float len = sqrtf(aa.x * aa.x + aa.y * aa.y + aa.z * aa.z);
        float q[4];
        quat_axis_angle(aa.x / len, aa.y / len, aa.z / len, aa.deg / kRadToDeg, q);

        float dL = diverge(q, rayP[0], rayY[0], hands::model_trim_pitch_deg(0),
                           hands::model_trim_yaw_deg(0), hands::model_trim_roll_deg(0));
        float dR = diverge(q, rayP[1], rayY[1], hands::model_trim_pitch_deg(1),
                           hands::model_trim_yaw_deg(1), hands::model_trim_roll_deg(1));
        float dC = diverge(q, 10.0f, 10.0f, 10.0f, 10.0f, 0.0f);
        if (dL > maxL) maxL = dL;
        if (dR > maxR) maxR = dR;
        if (dC > maxC) maxC = dC;
        BVR_LOG("[sync] %2d axis(%+.0f,%+.0f,%+.0f) %+4.0f deg | liveL %6.2f liveR %6.2f "
                "canon %6.2f deg",
                p, aa.x, aa.y, aa.z, aa.deg, dL, dR, dC);
    }
    BVR_LOG("[sync] MAX divergence: liveL %.2f liveR %.2f CANON %.2f deg over %d poses "
            "(canon is the algebra gate: identical trims in, so nonzero = the two "
            "algebras disagree)",
            maxL, maxR, maxC, kPoseCount);
    BVR_LOG("[sync] position (separate story): render-lock delta last frame %.2f UU - "
            "position-only by construction, never rotation",
            bvr::b1r::bones::lock_delta_mag());
}

void handle_command(const char* args) {
    char verb[16] = {};
    int consumed = 0;
    if (sscanf_s(args, "%15s%n", verb, static_cast<unsigned>(sizeof verb), &consumed) != 1) {
        log_status();
        return;
    }
    const char* rest = args + consumed;
    while (*rest == ' ' || *rest == '\t') ++rest;

    if (strcmp(verb, "on") == 0) {
        if (!install_all()) {
            BVR_LOG("[aim] on REFUSED: no aim seam hooked (natives unresolved?)");
            return;
        }
        g_enabled.store(true, std::memory_order_relaxed);
        BVR_LOG("[aim] ON - controller aim substitutes the view aim");
        log_status();
    } else if (strcmp(verb, "off") == 0) {
        g_enabled.store(false, std::memory_order_relaxed);
        if (!g_probe.load(std::memory_order_relaxed)) disable_all();
        BVR_LOG("[aim] OFF - engine aim restored");
    } else if (strcmp(verb, "scan") == 0) {
        char cls[48] = {}, fn[48] = {};
        int dump = 8;
        int n = sscanf_s(rest, "%47s %47s %d", cls, static_cast<unsigned>(sizeof cls), fn,
                         static_cast<unsigned>(sizeof fn), &dump);
        if (n >= 2) scan_and_hook(cls, fn, dump > 0 ? dump : 8);
        else BVR_LOG("[aim] usage: vraim scan <Class> <Func> [dumpLines]");
    } else if (strcmp(verb, "scanimpl") == 0) {
        unsigned rva = 0;
        int args = 0, dump = 8;
        int n = sscanf_s(rest, "%x %d %d", &rva, &args, &dump);
        if (n >= 2) scan_impl(rva, args, dump > 0 ? dump : 8);
        else BVR_LOG("[aim] usage: vraim scanimpl <rvaHex> <stackArgs 1..3> [dumpLines]");
    } else if (strcmp(verb, "scanoff") == 0) {
        scan_off();
        impl_scan_off();
    } else if (strcmp(verb, "probe") == 0) {
        bool on = strncmp(rest, "on", 2) == 0;
        g_probe.store(on, std::memory_order_relaxed);
        if (on) {
            install_all();
            if (g_dumpBudget.load(std::memory_order_relaxed) <= 0) set_dump(24);
            BVR_LOG("[aim] probe ON (telemetry only; `vraim dump <n>` for more lines)");
        } else {
            if (!g_enabled.load(std::memory_order_relaxed)) disable_all();
            BVR_LOG("[aim] probe off");
        }
    } else if (strcmp(verb, "dump") == 0) {
        int n = 0;
        if (sscanf_s(rest, "%d", &n) == 1 && n >= 0) {
            set_dump(n);
            BVR_LOG("[aim] dump budget %d per seam", n);
        }
    } else if (strcmp(verb, "pose") == 0) {
        bool aim = strncmp(rest, "aim", 3) == 0;
        g_useAimPose.store(aim, std::memory_order_relaxed);
        BVR_LOG("[aim] pose source = %s", aim ? "AIM (pointing ray)" : "GRIP (handle axis)");
    } else if (strcmp(verb, "cal") == 0) {
        // "cal [l|r] <pitch> [yaw]" - no side = both hands (legacy behavior).
        int hand = -1;
        const char* nums = rest;
        if ((rest[0] == 'l' || rest[0] == 'r') && (rest[1] == ' ' || rest[1] == '\t')) {
            hand = rest[0] == 'r' ? 1 : 0;
            nums = rest + 1;
            while (*nums == ' ' || *nums == '\t') ++nums;
        }
        float pitch = 0.0f, yaw = 0.0f;
        if (sscanf_s(nums, "%f %f", &pitch, &yaw) >= 1) {
            for (int h = 0; h < 2; ++h) {
                if (hand >= 0 && h != hand) continue;
                g_pitchOffsetDeg[h].store(pitch, std::memory_order_relaxed);
                g_yawOffsetDeg[h].store(yaw, std::memory_order_relaxed);
            }
            BVR_LOG("[aim] calibration offset (%s): pitch %+.1f yaw %+.1f deg",
                    hand < 0 ? "both" : hand == 1 ? "right" : "left", pitch, yaw);
        } else {
            BVR_LOG("[aim] usage: vraim cal [l|r] <pitchDeg> [yawDeg]  (+pitch aims higher)");
        }
    } else if (strcmp(verb, "pos") == 0) {
        // "pos [l|r] <fwd> <right> <up>" (cm) - the aim-ray ORIGIN offset;
        // no side = both hands, mirroring cal.
        int hand = -1;
        const char* nums = rest;
        if ((rest[0] == 'l' || rest[0] == 'r') && (rest[1] == ' ' || rest[1] == '\t')) {
            hand = rest[0] == 'r' ? 1 : 0;
            nums = rest + 1;
            while (*nums == ' ' || *nums == '\t') ++nums;
        }
        float f = 0.0f, r = 0.0f, u = 0.0f;
        if (sscanf_s(nums, "%f %f %f", &f, &r, &u) == 3) {
            for (int h = 0; h < 2; ++h) {
                if (hand >= 0 && h != hand) continue;
                g_posFwdCm[h].store(f, std::memory_order_relaxed);
                g_posRightCm[h].store(r, std::memory_order_relaxed);
                g_posUpCm[h].store(u, std::memory_order_relaxed);
            }
            BVR_LOG("[aim] ray origin offset (%s): fwd%+.1f right%+.1f up%+.1f cm "
                    "(laser + fire origin move together)",
                    hand < 0 ? "both" : hand == 1 ? "right" : "left", f, r, u);
        } else {
            BVR_LOG("[aim] usage: vraim pos [l|r] <fwdCm> <rightCm> <upCm>");
        }
    } else if (strcmp(verb, "laser") == 0) {
        // "laser on|off" or "laser <dots> <nearM> <farM> <sizeDeg>"
        float nearM = 0.0f, farM = 0.0f, size = 0.0f;
        int dots = 0;
        if (sscanf_s(rest, "%d %f %f %f", &dots, &nearM, &farM, &size) == 4) {
            g_laserDots.store(dots, std::memory_order_relaxed);
            g_laserNearM.store(nearM, std::memory_order_relaxed);
            g_laserFarM.store(farM, std::memory_order_relaxed);
            g_laserSizeDeg.store(size, std::memory_order_relaxed);
            BVR_LOG("[aim] laser shape: %d dots, %.2f..%.2f m, %.2f deg", dots, nearM, farM,
                    size);
        } else {
            bool on = strncmp(rest, "on", 2) == 0;
            g_laser.store(on, std::memory_order_relaxed);
            BVR_LOG("[aim] laser %s (dots along the aim ray, XR quad layers)",
                    on ? "ON" : "off");
        }
    } else if (strcmp(verb, "dot") == 0) {
        // "dot on|off" | "dot dist <m>" | "dot size <deg>"
        if (strncmp(rest, "dist", 4) == 0) {
            float m = 0.0f;
            if (sscanf_s(rest + 4, "%f", &m) == 1 && m > 0.05f) {
                g_dotDistM.store(m, std::memory_order_relaxed);
                BVR_LOG("[aim] aim dot distance %.2f m (set this to the WALL's distance "
                        "before calibrating - a dot and a bullet hole only fuse in stereo "
                        "at matching depth)", m);
            } else {
                BVR_LOG("[aim] usage: vraim dot dist <meters>");
            }
        } else if (strncmp(rest, "size", 4) == 0) {
            float d = 0.0f;
            if (sscanf_s(rest + 4, "%f", &d) == 1 && d > 0.01f) {
                g_dotSizeDeg.store(d, std::memory_order_relaxed);
                BVR_LOG("[aim] aim dot size %.2f deg (angular, so it reads the same at any "
                        "distance)", d);
            } else {
                BVR_LOG("[aim] usage: vraim dot size <degrees>");
            }
        } else {
            bool on = strncmp(rest, "on", 2) == 0;
            g_dot.store(on, std::memory_order_relaxed);
            BVR_LOG("[aim] aim dot %s (%.2f m, %.2f deg) - placed from the FIRE-SEAM ray "
                    "point, so it shows only while a shot would actually be substituted",
                    on ? "ON" : "off", g_dotDistM.load(std::memory_order_relaxed),
                    g_dotSizeDeg.load(std::memory_order_relaxed));
        }
    } else if (strcmp(verb, "origin") == 0) {
        bool on = strncmp(rest, "on", 2) == 0;
        g_handOrigin.store(on, std::memory_order_relaxed);
        BVR_LOG("[aim] hand origin %s (off = engine's own origin, direction only)",
                on ? "ON" : "off");
    } else if (strcmp(verb, "watch") == 0) {
        // "watch on|off|<minMs>"
        unsigned ms = 0;
        if (sscanf_s(rest, "%u", &ms) == 1 && ms > 0) {
            g_watchMinMs.store(ms, std::memory_order_relaxed);
            g_watch.store(true, std::memory_order_relaxed);
            BVR_LOG("[aim] watch ON, at most one line per seam per %u ms", ms);
        } else {
            bool on = strncmp(rest, "on", 2) == 0;
            g_watch.store(on, std::memory_order_relaxed);
            BVR_LOG("[aim] watch %s (rate-limited per-substitution line: class, hand and how "
                    "it was chosen, triggers, engine origin vs ours). Unlike 'dump' this "
                    "survives a whole firefight - vraim watch on|off|<minMs>",
                    on ? "ON" : "off");
        }
    } else if (strcmp(verb, "seam") == 0) {
        char name[16] = {};
        char state[8] = {};
        if (sscanf_s(rest, "%15s %7s", name, static_cast<unsigned>(sizeof name), state,
                     static_cast<unsigned>(sizeof state)) == 2) {
            std::atomic<bool>* flag = sub_flag_by_name(name);
            if (!flag) {
                BVR_LOG("[aim] unknown seam '%s'", name);
                return;
            }
            bool on = strncmp(state, "on", 2) == 0;
            flag->store(on, std::memory_order_relaxed);
            BVR_LOG("[aim] seam %s substitution %s", name, on ? "ON" : "off");
        }
    } else if (strcmp(verb, "test") == 0) {
        char what[8] = {};
        int c2 = 0;
        if (sscanf_s(rest, "%7s%n", what, static_cast<unsigned>(sizeof what), &c2) != 1) return;
        const char* p = rest + c2;
        if (strcmp(what, "clear") == 0) {
            g_test[0] = {};
            g_test[1] = {};
            BVR_LOG("[aim] test aim cleared");
            return;
        }
        // "test l|r <yaw> <pitch> [holdMs]"
        float yaw = 0.0f, pitch = 0.0f;
        int hold = 0;
        int n = sscanf_s(p, "%f %f %d", &yaw, &pitch, &hold);
        if (n < 2) {
            BVR_LOG("[aim] usage: vraim test l|r <yawDeg> <pitchDeg> [holdMs]");
            return;
        }
        int idx = (what[0] == 'r' || what[0] == 'R') ? 1 : 0;
        if (hold <= 0) hold = 3000;
        if (hold > 60000) hold = 60000;
        g_test[idx].yawDeg = yaw;
        g_test[idx].pitchDeg = pitch;
        g_test[idx].deadline = GetTickCount64() + static_cast<uint64_t>(hold);
        BVR_LOG("[aim] test aim %s: yaw %+.1f pitch %+.1f for %d ms", idx ? "RIGHT" : "LEFT",
                yaw, pitch, hold);
    } else if (strcmp(verb, "muzzle") == 0) {
        bool on = strncmp(rest, "on", 2) == 0;
        g_muzzleRay.store(on, std::memory_order_relaxed);
        float d0[3];
        bool haveRef = bvr::b1r::bones::barrel_ref_axis(d0);
        BVR_LOG("[aim] muzzle ray %s - right-hand ray %s (barrel axis %s: %.3f %.3f %.3f)",
                on ? "ON" : "off",
                on ? "follows the RENDERED barrel (per-weapon, no manual trim)"
                   : "back to the trimmed controller forward",
                haveRef ? "from the live reference" : "UNAVAILABLE yet",
                haveRef ? d0[0] : 0.0f, haveRef ? d0[1] : 0.0f, haveRef ? d0[2] : 0.0f);
    } else if (strcmp(verb, "synccheck") == 0) {
        run_synccheck();
    } else if (strcmp(verb, "weapon") == 0) {
        // Per-weapon profile status (session 21).
        void* w = hands::weapon_actor();
        const wchar_t* cls = w ? patterns::object_class_name(w) : nullptr;
        BVR_LOG("[aim] weapon profile: active='%s'%s actor=%p class='%S' | %u profile(s), "
                "%u swap(s) | R trim %.2f/%.2f pos %.1f/%.1f/%.1f",
                g_weaponKey.empty() ? "-" : g_weaponKey.c_str(), g_weaponKeySim ? " (SIM)" : "",
                w, cls ? cls : L"-", static_cast<unsigned>(g_weaponProfiles.size()),
                g_weaponSwaps, g_pitchOffsetDeg[1].load(std::memory_order_relaxed),
                g_yawOffsetDeg[1].load(std::memory_order_relaxed),
                g_posFwdCm[1].load(std::memory_order_relaxed),
                g_posRightCm[1].load(std::memory_order_relaxed),
                g_posUpCm[1].load(std::memory_order_relaxed));
    } else if (strcmp(verb, "wsave") == 0) {
        save_weapon_profiles();
    } else if (strcmp(verb, "wkey") == 0) {
        // Flat test seam: force a profile key (real switching cannot be driven
        // flat - exec NextWeapon FAULTS). 'wkey real' resumes actor tracking.
        char name[48] = {};
        if (strncmp(rest, "real", 4) == 0) {
            g_weaponKeySim = false;
            g_weaponKeyActor = nullptr; // force re-resolve from the live actor
            BVR_LOG("[aim] wkey: back to live weapon tracking");
        } else if (sscanf_s(rest, "sim %47s", name, static_cast<unsigned>(sizeof name)) == 1) {
            g_weaponKeySim = true;
            apply_weapon_key(name, "wkey sim");
        } else {
            BVR_LOG("[aim] usage: vraim wkey sim <ClassName> | vraim wkey real");
        }
    } else if (strcmp(verb, "status") == 0) {
        log_status();
    } else {
        BVR_LOG("[aim] unknown command '%s' "
                "(on|off|probe|dump|origin|seam|test|synccheck|weapon|wsave|wkey|scan|"
                "scanimpl|scanoff|status)",
                verb);
    }
}

bool hook_live() {
    return g_weaponFire.enabled.load(std::memory_order_relaxed) ||
           g_abilityFire.enabled.load(std::memory_order_relaxed);
}

void* learned_weapon_object() {
    return g_objRight;
}

void aim_trim_deg(int hand, float* pitchDeg, float* yawDeg) {
    if (hand < 0 || hand > 1) return;
    if (pitchDeg) *pitchDeg = g_pitchOffsetDeg[hand].load(std::memory_order_relaxed);
    if (yawDeg) *yawDeg = g_yawOffsetDeg[hand].load(std::memory_order_relaxed);
}

void set_aim_trim_all(float pitchDeg, float yawDeg) {
    // PER WEAPON since s67. It was written to every profile at first, on the
    // hope that one crosshair would serve all eight; the tester could not get
    // them to agree, which is the same answer the grip offsets gave - each gun
    // is held differently, so each needs its own. Writing the LIVE value only
    // lets the existing profile stash/apply carry it per weapon, exactly like
    // grip and placement. (Name kept so the header contract does not churn.)
    g_pitchOffsetDeg[1].store(pitchDeg, std::memory_order_relaxed);
    g_yawOffsetDeg[1].store(yawDeg, std::memory_order_relaxed);
}

void weapon_key_name(char* out, size_t count) {
    if (!out || !count) return;
    std::lock_guard<std::mutex> lock(g_weaponKeyUiMutex);
    strncpy_s(out, count, g_weaponKeyUi, _TRUNCATE);
}

bool weapon_key_is(const char* name) {
    // The profile key IS the equipped holdable's class name, maintained by
    // update_weapon_profile off Hands.CurrentHoldable. Session 31 reuses it as
    // the swing gesture's "is this the wrench" test rather than resolving the
    // holdable a second time: one identity source, one place to be wrong.
    return name && g_weaponKey == name;
}

float trim_pitch_deg(int hand) {
    return g_pitchOffsetDeg[hand == 0 ? 0 : 1].load(std::memory_order_relaxed);
}

float trim_yaw_deg(int hand) {
    return g_yawOffsetDeg[hand == 0 ? 0 : 1].load(std::memory_order_relaxed);
}

bool laser_enabled() { return g_laser.load(std::memory_order_relaxed); }
bool dot_enabled() { return g_dot.load(std::memory_order_relaxed); }
float dot_dist_m() { return g_dotDistM.load(std::memory_order_relaxed); }
float dot_size_deg() { return g_dotSizeDeg.load(std::memory_order_relaxed); }

void weapon_scan_rearm(const char* why) {
    g_weaponScanMisses = 0;
    if (g_weaponScanDormant) {
        g_weaponScanDormant = false;
        BVR_LOG("[aim] weapon scan fallback re-armed (%s)", why);
    }
}

float pos_fwd_cm(int hand) {
    return g_posFwdCm[hand == 0 ? 0 : 1].load(std::memory_order_relaxed);
}

float pos_right_cm(int hand) {
    return g_posRightCm[hand == 0 ? 0 : 1].load(std::memory_order_relaxed);
}

float pos_up_cm(int hand) {
    return g_posUpCm[hand == 0 ? 0 : 1].load(std::memory_order_relaxed);
}

void set_pos_offset(int hand, float fwdCm, float rightCm, float upCm) {
    int h = hand == 0 ? 0 : 1;
    g_posFwdCm[h].store(fwdCm, std::memory_order_relaxed);
    g_posRightCm[h].store(rightCm, std::memory_order_relaxed);
    g_posUpCm[h].store(upCm, std::memory_order_relaxed);
}

void set_trim(int hand, float pitchDeg, float yawDeg) {
    int h = hand == 0 ? 0 : 1;
    g_pitchOffsetDeg[h].store(pitchDeg, std::memory_order_relaxed);
    g_yawOffsetDeg[h].store(yawDeg, std::memory_order_relaxed);
}

bool active() {
    return g_enabled.load(std::memory_order_relaxed) && hook_live();
}

void draw_debug_ui() {
    if (!ImGui::CollapsingHeader("Decoupled aim (M6)", ImGuiTreeNodeFlags_DefaultOpen)) return;

    bool on = g_enabled.load(std::memory_order_relaxed);
    if (ImGui::Checkbox("Controller aim (right = weapon, left = plasmid)", &on))
        g_pendingEnable.store(on ? 1 : 0, std::memory_order_relaxed);
    bool useAim = g_useAimPose.load(std::memory_order_relaxed);
    if (ImGui::Checkbox("Use the runtime AIM pose (off = grip pose)", &useAim))
        g_useAimPose.store(useAim, std::memory_order_relaxed);
    // Per-hand trims (session 16 part 3): R = weapon, L = plasmid.
    // Range +-90 since v0.3.0: the user's left-wrist posture pinned the old
    // +-30 cap (their saved L yaw sat at exactly 30.0 = the slider max).
    float rp = g_pitchOffsetDeg[1].load(std::memory_order_relaxed);
    if (ImGui::SliderFloat("R aim pitch trim (deg)", &rp, -90.0f, 90.0f))
        g_pitchOffsetDeg[1].store(rp, std::memory_order_relaxed);
    float ry = g_yawOffsetDeg[1].load(std::memory_order_relaxed);
    if (ImGui::SliderFloat("R aim yaw trim (deg)", &ry, -90.0f, 90.0f))
        g_yawOffsetDeg[1].store(ry, std::memory_order_relaxed);
    float lp = g_pitchOffsetDeg[0].load(std::memory_order_relaxed);
    if (ImGui::SliderFloat("L aim pitch trim (deg)", &lp, -90.0f, 90.0f))
        g_pitchOffsetDeg[0].store(lp, std::memory_order_relaxed);
    float ly = g_yawOffsetDeg[0].load(std::memory_order_relaxed);
    if (ImGui::SliderFloat("L aim yaw trim (deg)", &ly, -90.0f, 90.0f))
        g_yawOffsetDeg[0].store(ly, std::memory_order_relaxed);

    // Ray ORIGIN offsets (session 18 part 2): move the laser + fire origin to
    // line up with the controller and the tuned model. Selector like the
    // hands section - one set of sliders, per-hand values.
    static int posHand = 1;
    ImGui::Text("Ray offset hand:");
    ImGui::SameLine();
    ImGui::RadioButton("L##aimpos", &posHand, 0);
    ImGui::SameLine();
    ImGui::RadioButton("R##aimpos", &posHand, 1);
    float pf = g_posFwdCm[posHand].load(std::memory_order_relaxed);
    if (ImGui::SliderFloat("ray offset forward (cm)", &pf, -30.0f, 30.0f))
        g_posFwdCm[posHand].store(pf, std::memory_order_relaxed);
    float pr = g_posRightCm[posHand].load(std::memory_order_relaxed);
    if (ImGui::SliderFloat("ray offset right (cm)", &pr, -30.0f, 30.0f))
        g_posRightCm[posHand].store(pr, std::memory_order_relaxed);
    float pu = g_posUpCm[posHand].load(std::memory_order_relaxed);
    if (ImGui::SliderFloat("ray offset up (cm)", &pu, -30.0f, 30.0f))
        g_posUpCm[posHand].store(pu, std::memory_order_relaxed);

    bool handOrigin = g_handOrigin.load(std::memory_order_relaxed);
    if (ImGui::Checkbox("Ray starts at the hand (off = engine origin, direction only)",
                        &handOrigin))
        g_handOrigin.store(handOrigin, std::memory_order_relaxed);

    // Per-weapon profiles (session 21): the R sliders above edit the ACTIVE
    // weapon's profile - swap happens automatically on weapon change; "Save
    // preset values" persists weapons.ini too. Calibration flow: fire at a
    // wall, nudge the R trim/offset sliders until the beam sits on the
    // bullet hole, next weapon.
    {
        std::lock_guard<std::mutex> lock(g_weaponKeyUiMutex);
        ImGui::Text("weapon profile: %s", g_weaponKeyUi);
    }

    // The laser is this same ray made visible, so it lives in this section and
    // shares the trim sliders above - use it to judge the trim by eye.
    bool laser = g_laser.load(std::memory_order_relaxed);
    if (ImGui::Checkbox("Aim laser (dots along the ray)", &laser))
        g_laser.store(laser, std::memory_order_relaxed);
    int dots = g_laserDots.load(std::memory_order_relaxed);
    if (ImGui::SliderInt("laser dots", &dots, 1, 8))
        g_laserDots.store(dots, std::memory_order_relaxed);
    float farM = g_laserFarM.load(std::memory_order_relaxed);
    if (ImGui::SliderFloat("laser reach (m)", &farM, 1.0f, 20.0f))
        g_laserFarM.store(farM, std::memory_order_relaxed);
    float sizeDeg = g_laserSizeDeg.load(std::memory_order_relaxed);
    if (ImGui::SliderFloat("laser dot size (deg)", &sizeDeg, 0.1f, 3.0f))
        g_laserSizeDeg.store(sizeDeg, std::memory_order_relaxed);

    // Session 29: the aim dot. Separate from the laser on purpose - the laser
    // reconstructs the ray on the render thread, this is the fire-seam point.
    bool dot = g_dot.load(std::memory_order_relaxed);
    if (ImGui::Checkbox("Aim dot (on the fire-seam ray)", &dot))
        g_dot.store(dot, std::memory_order_relaxed);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("One quad at the exact point the bullet starts from, extended "
                          "along the exact rotator the engine is handed.\n"
                          "Set the distance to your calibration wall: a dot and a bullet "
                          "hole only fuse in stereo at matching depth.");
    float dotDist = g_dotDistM.load(std::memory_order_relaxed);
    if (ImGui::SliderFloat("aim dot distance (m)", &dotDist, 0.5f, 20.0f))
        g_dotDistM.store(dotDist, std::memory_order_relaxed);
    float dotSize = g_dotSizeDeg.load(std::memory_order_relaxed);
    if (ImGui::SliderFloat("aim dot size (deg)", &dotSize, 0.1f, 3.0f))
        g_dotSizeDeg.store(dotSize, std::memory_order_relaxed);

    ImGui::Text("fire seams (subs/calls): weapon %u/%u  plasmid %u/%u",
                g_weaponFire.subs.load(std::memory_order_relaxed),
                g_weaponFire.calls.load(std::memory_order_relaxed),
                g_abilityFire.subs.load(std::memory_order_relaxed),
                g_abilityFire.calls.load(std::memory_order_relaxed));
    ImGui::Text("last sub: %s origin (%.0f %.0f %.0f) yaw %d pitch %d",
                g_lastSubHand.load(std::memory_order_relaxed) ? "RIGHT" : "LEFT",
                g_lastSubX.load(std::memory_order_relaxed),
                g_lastSubY.load(std::memory_order_relaxed),
                g_lastSubZ.load(std::memory_order_relaxed),
                g_lastSubYaw.load(std::memory_order_relaxed),
                g_lastSubPitch.load(std::memory_order_relaxed));
}

} // namespace bvr::b1r::aim
