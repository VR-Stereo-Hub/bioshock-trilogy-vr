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
#include "game/bioshock1r/ue_math.h"

#include <windows.h>
#include <MinHook.h>

#include <imgui.h>

#include <atomic>
#include <cstdio>
#include <cstring>

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
    BVR_LOG("[aim] %s this=%p vtbl=0x%X A=(%.1f %.1f %.1f) B=(%.3f %.3f %.3f) "
            "C=(%.3f %.3f %.3f) %s",
            slot.name, self, to_rva(vtbl), a[0], a[1], a[2], b[0], b[1], b[2], c[0], c[1], c[2],
            note);
}

// The two out-params are a POSITION (thousands of Unreal units) and a unit
// DIRECTION, and which slot holds which differs between the weapon and the
// ability signature. Rather than trust the disassembly's labels, decide per
// call from the magnitudes - a direction is never longer than ~1.
bool looks_like_direction(const float v[3]) {
    float len2 = v[0] * v[0] + v[1] * v[1] + v[2] * v[2];
    return len2 < 4.0f;
}

bool is_zero(const float v[3]) {
    return v[0] == 0.0f && v[1] == 0.0f && v[2] == 0.0f;
}

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

    bool wrote = false;
    for (int i = 0; i < count; ++i) {
        if (!out[i]) continue;
        float cur[3];
        if (!read12(out[i], cur)) continue;
        if (is_zero(cur)) continue; // engine left this one alone; so do we
        if (looks_like_direction(cur)) {
            wrote |= write12(out[i], dir);
        } else if (handOrigin) {
            wrote |= write12(out[i], pos);
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

} // namespace

void init(const bvr::pattern_scan::ProcessImage& image, const patterns::Symbols& symbols) {
    g_imageBase = image.base;
    g_image = image;
    g_syms = symbols;
    BVR_LOG("[aim] init: weapon fire-start=%p ability fire-start=%p", g_syms.weaponFireStart,
            g_syms.abilityFireStart);
}

void on_calcview(const FrameContext& ctx) {
    uint64_t now = GetTickCount64();
    g_rayStampMs = now;

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

    float gameYawRad = static_cast<float>(ctx.camYaw) / kRotUnitsPerRadian - ctx.driveYawOffsetRad;

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
            continue;
        }

        // 2) The real thing: the hand's grip pose, mapped through EXACTLY the
        //    transform the camera drive used this frame.
        bvr::vr::HeadPose hp{};
        if (!ctx.vrDriving || !bvr::vr::get_hand_pose(i, hp)) continue;

        UeAngles a = ue_angles_from_xr_quat(hp.qx, hp.qy, hp.qz, hp.qw);
        out.rot.yaw = static_cast<int32_t>((gameYawRad + (a.yawRad - ctx.recenterYawRad)) *
                                          kRotUnitsPerRadian);
        out.rot.pitch = static_cast<int32_t>(a.pitchRad * kRotUnitsPerRadian);
        out.rot.roll = 0; // aim carries no roll; the camera owns roll

        float dxr[3] = {hp.px - ctx.recenterPx, hp.py - ctx.recenterPy, hp.pz - ctx.recenterPz};
        float d[3];
        xr_to_ue(dxr, d);
        float c = cosf(-ctx.recenterYawRad), s = sinf(-ctx.recenterYawRad);
        float lx = d[0] * c - d[1] * s;
        float ly = d[0] * s + d[1] * c;
        float cg = cosf(gameYawRad), sg = sinf(gameYawRad);
        out.origin.x = ctx.baseX + (lx * cg - ly * sg) * ctx.worldScale;
        out.origin.y = ctx.baseY + (lx * sg + ly * cg) * ctx.worldScale;
        out.origin.z = ctx.baseZ + d[2] * ctx.worldScale;
        out.valid = true;
    }
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
    } else if (strcmp(verb, "scanoff") == 0) {
        scan_off();
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
    } else if (strcmp(verb, "status") == 0) {
        log_status();
    } else {
        BVR_LOG("[aim] unknown command '%s' "
                "(on|off|probe|dump|origin|seam|test|scan|scanoff|status)", verb);
    }
}

bool hook_live() {
    return g_weaponFire.enabled.load(std::memory_order_relaxed) ||
           g_abilityFire.enabled.load(std::memory_order_relaxed);
}

bool active() {
    return g_enabled.load(std::memory_order_relaxed) && hook_live();
}

void draw_debug_ui() {
    if (!ImGui::CollapsingHeader("Decoupled aim (M6)", ImGuiTreeNodeFlags_DefaultOpen)) return;

    bool on = g_enabled.load(std::memory_order_relaxed);
    if (ImGui::Checkbox("Controller aim (right = weapon, left = plasmid)", &on)) {
        if (on) {
            if (install_all()) g_enabled.store(true, std::memory_order_relaxed);
        } else {
            g_enabled.store(false, std::memory_order_relaxed);
            if (!g_probe.load(std::memory_order_relaxed)) disable_all();
        }
    }
    bool handOrigin = g_handOrigin.load(std::memory_order_relaxed);
    if (ImGui::Checkbox("Ray starts at the hand (off = engine origin, direction only)",
                        &handOrigin))
        g_handOrigin.store(handOrigin, std::memory_order_relaxed);

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
