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

#include "core/input/xinput_bridge.h"
#include "core/util/log.h"
#include "core/vr/openxr_runtime.h"
#include "game/bioshock1r/bones.h"
#include "game/bioshock1r/hands.h"
#include "game/bioshock1r/ue_math.h"

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
// (2026-07-29 headset run: -7.5/+37.0, "better than before"). The
// right-hand TRIM stays 0 - the per-weapon profiles own the right hand;
// its POS offsets below carry the generic baseline.
std::atomic<float> g_pitchOffsetDeg[2] = {-7.5f, 0.0f};
std::atomic<float> g_yawOffsetDeg[2] = {37.0f, 0.0f};
// Per-hand aim-ray ORIGIN offsets in cm (session 18 part 2, user request):
// the model offsets move the MESH about its pivot, so a tuned model can sit
// right while the ray no longer runs along the barrel. These move the RAY
// itself - laser, fire origin, everything reading g_ray - to re-align with
// the controller and the tuned model. Applied along the FINAL (trimmed)
// ray's zero-roll basis at ray build, so the laser and the bullet cannot
// disagree; the model deliberately does not take them (it has its own
// sliders, and the two are tuned against each other).
std::atomic<float> g_posFwdCm[2] = {-2.8f, 0.0f};  // v0.3.0 user-calibration bake
std::atomic<float> g_posRightCm[2] = {0.6f, -2.8f};
std::atomic<float> g_posUpCm[2] = {0.5f, 7.5f};
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
    int32_t dumpLeft = 0;
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

// `fallback` is the hand this seam belongs to when there is no trigger
// evidence yet (weapons right, abilities left).
Hand hand_for_object(void* obj, Hand fallback) {
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
            BVR_LOG("[aim] learned RIGHT-hand (weapon) object %p", obj);
            return Hand::Right;
        }
        if (takeLeft) {
            g_objLeft = obj;
            if (g_objRight == obj) g_objRight = nullptr;
            g_learnEvents.fetch_add(1, std::memory_order_relaxed);
            BVR_LOG("[aim] learned LEFT-hand (plasmid) object %p", obj);
            return Hand::Left;
        }
    }
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
        if (substitute(g_weaponFire, h, outs, 3))
            note = (h == Hand::Right) ? "SUB(R)" : "SUB(L)";
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
        if (substitute(g_abilityFire, h, outs, 3))
            note = (h == Hand::Right) ? "SUB(R)" : "SUB(L)";
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
    BVR_LOG("[aim] status: %s | seams weapon=%d ability=%d | handOrigin=%d probe=%d",
            g_enabled.load(std::memory_order_relaxed) ? "ON" : "off",
            g_subWeapon.load(std::memory_order_relaxed) ? 1 : 0,
            g_subAbility.load(std::memory_order_relaxed) ? 1 : 0,
            g_handOrigin.load(std::memory_order_relaxed) ? 1 : 0,
            g_probe.load(std::memory_order_relaxed) ? 1 : 0);
    Slot* all[] = {&g_weaponFire, &g_abilityFire};
    for (Slot* s : all) {
        BVR_LOG("[aim]   %-8s hook=%s calls=%u subs=%u skips=%u", s->name,
                s->enabled.load(std::memory_order_relaxed) ? "on " : "off",
                s->calls.load(std::memory_order_relaxed),
                s->subs.load(std::memory_order_relaxed),
                s->skips.load(std::memory_order_relaxed));
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

// Capture the live R-hand values into the active profile (no-op keyless).
void stash_active_profile() {
    if (g_weaponKey.empty()) return;
    WeaponProfile& p = g_weaponProfiles[g_weaponKey];
    p.trimPitch = g_pitchOffsetDeg[1].load(std::memory_order_relaxed);
    p.trimYaw = g_yawOffsetDeg[1].load(std::memory_order_relaxed);
    p.posFwd = g_posFwdCm[1].load(std::memory_order_relaxed);
    p.posRight = g_posRightCm[1].load(std::memory_order_relaxed);
    p.posUp = g_posUpCm[1].load(std::memory_order_relaxed);
}

void apply_weapon_key(const std::string& key, const char* why) {
    if (key == g_weaponKey) return;
    stash_active_profile();
    g_weaponKey = key;
    {
        std::lock_guard<std::mutex> lock(g_weaponKeyUiMutex);
        strcpy_s(g_weaponKeyUi, key.empty() ? "-" : key.c_str());
    }
    if (key.empty()) return;
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
                                              g_posUpCm[1].load(std::memory_order_relaxed)};
        g_weaponProfiles[key] = p;
        g_pitchOffsetDeg[1].store(p.trimPitch, std::memory_order_relaxed);
        g_yawOffsetDeg[1].store(p.trimYaw, std::memory_order_relaxed);
        g_posFwdCm[1].store(p.posFwd, std::memory_order_relaxed);
        g_posRightCm[1].store(p.posRight, std::memory_order_relaxed);
        g_posUpCm[1].store(p.posUp, std::memory_order_relaxed);
        BVR_LOG("[aim] weapon profile '%s' CREATED from the %s (%s): trim %.2f/%.2f pos "
                "%.1f/%.1f/%.1f",
                key.c_str(), g_presetBaselineValid ? "preset baseline" : "current R values",
                why, p.trimPitch, p.trimYaw, p.posFwd, p.posRight, p.posUp);
    } else {
        const WeaponProfile& p = it->second;
        g_pitchOffsetDeg[1].store(p.trimPitch, std::memory_order_relaxed);
        g_yawOffsetDeg[1].store(p.trimYaw, std::memory_order_relaxed);
        g_posFwdCm[1].store(p.posFwd, std::memory_order_relaxed);
        g_posRightCm[1].store(p.posRight, std::memory_order_relaxed);
        g_posUpCm[1].store(p.posUp, std::memory_order_relaxed);
        BVR_LOG("[aim] weapon profile '%s' applied: trim %.2f/%.2f pos %.1f/%.1f/%.1f (%s)",
                key.c_str(), p.trimPitch, p.trimYaw, p.posFwd, p.posRight, p.posUp, why);
    }
}

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
    static uint32_t nullResolves = 0;
    // Failure backoff for the SCAN fallback only (the rig read is a pointer
    // dereference and free): a world state with no acceptable weapon must
    // not full-heap-scan on a cadence (the session-18 hands-scan ~1 fps
    // lesson; the game intro has no weapon for minutes).
    uint32_t mask = nullResolves >= 3 ? 2047 : 15;
    if ((++throttle & mask) != 0) return;
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
        // Session 22: the SCAN fallback goes fully DORMANT after 3 straight
        // failures - the reduced cadence still meant a multi-second heap
        // walk every ~2000 frames FOREVER on saves with no resolvable
        // weapon (the early game; user-felt as "the game freezes every
        // couple of seconds" at the Gatherer's Garden). The cheap rig and
        // learned-actor reads above keep running at the throttled cadence
        // and re-arm the scanner the moment they see anything.
        if (!w && nullResolves < 3) w = hands::resolve_weapon_actor(ctx);
    }
    nullResolves = (haveRig || w) ? 0 : nullResolves + 1;
    if (w == g_weaponKeyActor) return;
    g_weaponKeyActor = w;
    const wchar_t* name = w ? patterns::object_class_name(w) : nullptr;
    if (w && !name)
        BVR_LOG("[aim] holdable %p has NO resolvable class name - profile key cleared, "
                "slider edits will touch no profile until it resolves",
                w);
    apply_weapon_key(name ? narrow_key(name) : std::string(), "weapon change");
}

// v0.3.0 default profiles = the user's calibrated set (session 21 part 4,
// their ask: the saved preset becomes the defaults - dot==shot per weapon
// out of the box). Seeded BEFORE weapons.ini loads, so a user's own file
// always overrides, key by key.
void seed_default_profiles() {
    g_weaponProfiles["ChemicalThrower"] = {0.00f, -8.17f, 0.93f, -0.43f, -9.78f};
    g_weaponProfiles["Crossbow"] = {0.23f, -6.65f, 0.00f, -4.40f, 11.00f};
    g_weaponProfiles["GrenadeLauncher"] = {0.00f, 0.00f, 0.00f, -2.80f, 7.50f};
    g_weaponProfiles["MachineGun"] = {2.80f, -1.17f, 0.00f, -3.50f, 13.10f};
    g_weaponProfiles["Pistol"] = {-1.17f, -4.20f, -0.70f, -2.06f, 18.71f};
    g_weaponProfiles["ResearchCamera"] = {0.00f, 0.00f, 0.00f, -2.80f, 7.50f};
    g_weaponProfiles["Shotgun"] = {0.00f, 0.00f, 0.00f, -2.76f, 7.50f};
    g_weaponProfiles["Wrench"] = {0.00f, 0.00f, 0.00f, -2.80f, 7.50f};
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
        else continue;
        ++n;
    }
    fclose(f);
    if (n)
        BVR_LOG("[aim] %d weapon-profile value(s) loaded from weapons.ini (%u weapon(s))", n,
                static_cast<unsigned>(g_weaponProfiles.size()));
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
                        g_posUpCm[1].load(std::memory_order_relaxed)};
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
    g_pitchOffsetDeg[1].store(p.trimPitch, std::memory_order_relaxed);
    g_yawOffsetDeg[1].store(p.trimYaw, std::memory_order_relaxed);
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
    } else if (strcmp(verb, "origin") == 0) {
        bool on = strncmp(rest, "on", 2) == 0;
        g_handOrigin.store(on, std::memory_order_relaxed);
        BVR_LOG("[aim] hand origin %s (off = engine's own origin, direction only)",
                on ? "ON" : "off");
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

float trim_pitch_deg(int hand) {
    return g_pitchOffsetDeg[hand == 0 ? 0 : 1].load(std::memory_order_relaxed);
}

float trim_yaw_deg(int hand) {
    return g_yawOffsetDeg[hand == 0 ? 0 : 1].load(std::memory_order_relaxed);
}

bool laser_enabled() { return g_laser.load(std::memory_order_relaxed); }

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
