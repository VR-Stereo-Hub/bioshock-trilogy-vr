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
std::atomic<bool> g_subFireStart{true};
std::atomic<bool> g_subAimError{true};
std::atomic<bool> g_subViewPoint{false};
std::atomic<bool> g_subViewDir{true};
std::atomic<bool> g_handOrigin{true};   // hand origin + direction (user's choice)
std::atomic<bool> g_probe{false};       // telemetry mode
std::atomic<int32_t> g_dumpBudget{0};   // per-seam detailed log budget

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
    int resultType = 0; // ResultType, learned from the engine's own values
};
Slot g_fireStart{"firestart"};
Slot g_aimError{"aimerror"};
Slot g_viewPoint{"viewpoint"};
Slot g_viewDir{"viewdir"};
Slot g_trace{"trace"};

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

bool is_player_weapon(void* obj) {
    return has_vtable(obj, patterns::kPlayerWeaponVtableRva);
}

bool is_player_pawn(void* obj) {
    return has_vtable(obj, patterns::kShockPlayerVtableRva);
}

// ---- hand selection --------------------------------------------------------

bool trigger_held(bool right) {
    uint8_t lt = 0, rt = 0;
    bvr::input::last_composed_triggers(&lt, &rt);
    return (right ? rt : lt) >= 64; // ~25% pull, same ballpark as the game's own
}

Hand hand_for_object(void* obj) {
    if (obj && obj == g_objRight) return Hand::Right;
    if (obj && obj == g_objLeft) return Hand::Left;

    bool r = trigger_held(true), l = trigger_held(false);
    if (obj && is_player_weapon(obj)) {
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
    // Unknown and no usable trigger evidence: the right hand is the weapon
    // hand, which is the safer default for a stray query.
    return Hand::Right;
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

// A 12-byte engine "direction" result is either an FVector (unit-ish floats)
// or an FRotator (65536-per-turn int32s), and which one depends on the script
// signature we never see. Classify from what the ORIGINAL produced: rotator
// components are small integers, whose float reinterpretation is a denormal,
// while float components are ordinary floats whose int reinterpretation is
// enormous. All-zero is ambiguous, so the verdict is cached per seam from the
// first confident call and only then does that seam substitute.
enum ResultType { kUnknown = 0, kVector = 1, kRotator = 2 };

ResultType classify(const float raw[3]) {
    int32_t v[3];
    memcpy(v, raw, sizeof v);
    bool allZero = (v[0] == 0 && v[1] == 0 && v[2] == 0);
    if (allZero) return kUnknown;
    for (int i = 0; i < 3; ++i) {
        int32_t a = v[i] < 0 ? -v[i] : v[i];
        if (a > (1 << 21)) return kVector; // 2 million rotation units is nonsense
    }
    return kRotator;
}

void write_direction(void* result, const FRotator& rot, ResultType type, int32_t rollIn) {
    if (type == kRotator) {
        int32_t v[3] = {rot.pitch, rot.yaw, rollIn};
        float packed[3];
        memcpy(packed, v, sizeof packed);
        write12(result, packed);
        return;
    }
    float dir[3];
    ue_rot_to_dir(rot, dir);
    write12(result, dir);
}

// ---- detours ---------------------------------------------------------------
// Every thunk is `void __thiscall execFoo(FFrame& Stack, void* Result)`, which
// is register/stack-identical to __fastcall with a dummy EDX slot.

void log_call(Slot& slot, void* self, void* stack, const float raw[3], const char* note) {
    if (slot.dumpLeft <= 0) return;
    --slot.dumpLeft;
    void* obj = nullptr;
    void* node = nullptr;
    if (stack) {
        read_ptr(static_cast<const uint8_t*>(stack) + patterns::kFFrameObjectOffset, &obj);
        read_ptr(static_cast<const uint8_t*>(stack) + patterns::kFFrameNodeOffset, &node);
    }
    int32_t asInt[3];
    memcpy(asInt, raw, sizeof asInt);
    BVR_LOG("[aim] %s this=%p vtbl=0x%X obj=%p node=%p result f=(%.2f %.2f %.2f) "
            "i=(%d %d %d) %s",
            slot.name, self, self ? to_rva(*static_cast<void**>(self)) : 0, obj, node,
            raw[0], raw[1], raw[2], asInt[0], asInt[1], asInt[2], note);
}

// Learn (and remember) what type this seam's 12-byte result is.
ResultType learn_type(Slot& slot, const float raw[3]) {
    ResultType t = classify(raw);
    if (t != kUnknown && slot.resultType == kUnknown) {
        slot.resultType = t;
        BVR_LOG("[aim] %s result type = %s", slot.name, t == kRotator ? "FRotator" : "FVector");
    }
    return static_cast<ResultType>(slot.resultType);
}

void __fastcall FireStartDetour(void* self, void* edx, void* stack, void* result) {
    reinterpret_cast<ExecFn>(g_fireStart.original)(self, edx, stack, result);
    g_fireStart.calls.fetch_add(1, std::memory_order_relaxed);

    float raw[3] = {0, 0, 0};
    if (!read12(result, raw)) return;

    const char* note = "";
    if (g_subFireStart.load(std::memory_order_relaxed) &&
        g_handOrigin.load(std::memory_order_relaxed) && is_player_weapon(self)) {
        Hand h = hand_for_object(self);
        FVector o{};
        FRotator r{};
        if (ray_for(h, &o, &r)) {
            float v[3] = {o.x, o.y, o.z};
            if (write12(result, v)) {
                g_fireStart.subs.fetch_add(1, std::memory_order_relaxed);
                note = (h == Hand::Right) ? "SUB(R)" : "SUB(L)";
                note_substitution(h, o, r);
            }
        } else {
            g_fireStart.skips.fetch_add(1, std::memory_order_relaxed);
        }
    }
    log_call(g_fireStart, self, stack, raw, note);
}

void __fastcall AimErrorDetour(void* self, void* edx, void* stack, void* result) {
    reinterpret_cast<ExecFn>(g_aimError.original)(self, edx, stack, result);
    g_aimError.calls.fetch_add(1, std::memory_order_relaxed);

    float raw[3] = {0, 0, 0};
    if (!read12(result, raw)) return;
    ResultType type = learn_type(g_aimError, raw);

    const char* note = "";
    if (g_subAimError.load(std::memory_order_relaxed) && type != kUnknown &&
        is_player_weapon(self)) {
        Hand h = hand_for_object(self);
        FVector o{};
        FRotator r{};
        if (ray_for(h, &o, &r)) {
            int32_t rollIn = 0;
            if (type == kRotator) {
                int32_t asInt[3];
                memcpy(asInt, raw, sizeof asInt);
                rollIn = asInt[2]; // keep the engine's roll
            }
            write_direction(result, r, type, rollIn);
            g_aimError.subs.fetch_add(1, std::memory_order_relaxed);
            note = (h == Hand::Right) ? "SUB(R)" : "SUB(L)";
            note_substitution(h, o, r);
        } else {
            g_aimError.skips.fetch_add(1, std::memory_order_relaxed);
        }
    }
    log_call(g_aimError, self, stack, raw, note);
}

void __fastcall ViewPointDetour(void* self, void* edx, void* stack, void* result) {
    reinterpret_cast<ExecFn>(g_viewPoint.original)(self, edx, stack, result);
    g_viewPoint.calls.fetch_add(1, std::memory_order_relaxed);

    float raw[3] = {0, 0, 0};
    if (!read12(result, raw)) return;

    const char* note = "";
    if (g_subViewPoint.load(std::memory_order_relaxed) &&
        g_handOrigin.load(std::memory_order_relaxed) && is_player_pawn(self)) {
        // The pawn's view point has no weapon object to key on; the hand that
        // is actually firing owns it (right hand when both or neither).
        Hand h = trigger_held(false) && !trigger_held(true) ? Hand::Left : Hand::Right;
        FVector o{};
        if (ray_for(h, &o, nullptr)) {
            float v[3] = {o.x, o.y, o.z};
            if (write12(result, v)) {
                g_viewPoint.subs.fetch_add(1, std::memory_order_relaxed);
                note = "SUB";
            }
        } else {
            g_viewPoint.skips.fetch_add(1, std::memory_order_relaxed);
        }
    }
    log_call(g_viewPoint, self, stack, raw, note);
}

void __fastcall ViewDirDetour(void* self, void* edx, void* stack, void* result) {
    reinterpret_cast<ExecFn>(g_viewDir.original)(self, edx, stack, result);
    g_viewDir.calls.fetch_add(1, std::memory_order_relaxed);

    float raw[3] = {0, 0, 0};
    if (!read12(result, raw)) return;
    ResultType type = learn_type(g_viewDir, raw);

    const char* note = "";
    if (g_subViewDir.load(std::memory_order_relaxed) && type != kUnknown &&
        is_player_pawn(self)) {
        Hand h = trigger_held(false) && !trigger_held(true) ? Hand::Left : Hand::Right;
        FVector o{};
        FRotator r{};
        if (ray_for(h, &o, &r)) {
            write_direction(result, r, type, 0);
            g_viewDir.subs.fetch_add(1, std::memory_order_relaxed);
            note = (h == Hand::Right) ? "SUB(R)" : "SUB(L)";
            note_substitution(h, o, r);
        } else {
            g_viewDir.skips.fetch_add(1, std::memory_order_relaxed);
        }
    }
    log_call(g_viewDir, self, stack, raw, note);
}

// Read-only: AActor::Trace is the hitscan primitive, so its result IS the
// impact point - the objective number the flat verification asserts on. Hot
// path (AI line-of-sight uses it too), so it only ever logs from a dump budget.
void __fastcall TraceDetour(void* self, void* edx, void* stack, void* result) {
    reinterpret_cast<ExecFn>(g_trace.original)(self, edx, stack, result);
    g_trace.calls.fetch_add(1, std::memory_order_relaxed);
    if (g_trace.dumpLeft <= 0) return;
    float raw[3] = {0, 0, 0};
    if (!read12(result, raw)) return;
    log_call(g_trace, self, stack, raw, "hitactor");
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
    any |= install_slot(g_fireStart, g_syms.execWeaponFireStart,
                        reinterpret_cast<void*>(&FireStartDetour));
    any |= install_slot(g_aimError, g_syms.execWeaponAimError,
                        reinterpret_cast<void*>(&AimErrorDetour));
    any |= install_slot(g_viewPoint, g_syms.execPawnViewPoint,
                        reinterpret_cast<void*>(&ViewPointDetour));
    any |= install_slot(g_viewDir, g_syms.execPawnViewDir,
                        reinterpret_cast<void*>(&ViewDirDetour));
    any |= install_slot(g_trace, g_syms.execActorTrace,
                        reinterpret_cast<void*>(&TraceDetour));
    return any;
}

void disable_all() {
    disable_slot(g_fireStart);
    disable_slot(g_aimError);
    disable_slot(g_viewPoint);
    disable_slot(g_viewDir);
    disable_slot(g_trace);
}

void set_dump(int32_t n) {
    g_dumpBudget.store(n, std::memory_order_relaxed);
    g_fireStart.dumpLeft = n;
    g_aimError.dumpLeft = n;
    g_viewPoint.dumpLeft = n;
    g_viewDir.dumpLeft = n;
    g_trace.dumpLeft = n;
}

Slot* slot_by_name(const char* name) {
    if (strcmp(name, "firestart") == 0) return &g_fireStart;
    if (strcmp(name, "aimerror") == 0) return &g_aimError;
    if (strcmp(name, "viewpoint") == 0) return &g_viewPoint;
    if (strcmp(name, "viewdir") == 0) return &g_viewDir;
    if (strcmp(name, "trace") == 0) return &g_trace;
    return nullptr;
}

std::atomic<bool>* sub_flag_by_name(const char* name) {
    if (strcmp(name, "firestart") == 0) return &g_subFireStart;
    if (strcmp(name, "aimerror") == 0) return &g_subAimError;
    if (strcmp(name, "viewpoint") == 0) return &g_subViewPoint;
    if (strcmp(name, "viewdir") == 0) return &g_subViewDir;
    return nullptr;
}

void log_status() {
    BVR_LOG("[aim] status: %s | seams fs=%d ae=%d vp=%d vd=%d | handOrigin=%d probe=%d",
            g_enabled.load(std::memory_order_relaxed) ? "ON" : "off",
            g_subFireStart.load(std::memory_order_relaxed),
            g_subAimError.load(std::memory_order_relaxed),
            g_subViewPoint.load(std::memory_order_relaxed),
            g_subViewDir.load(std::memory_order_relaxed),
            g_handOrigin.load(std::memory_order_relaxed) ? 1 : 0,
            g_probe.load(std::memory_order_relaxed) ? 1 : 0);
    Slot* all[] = {&g_fireStart, &g_aimError, &g_viewPoint, &g_viewDir, &g_trace};
    for (Slot* s : all) {
        BVR_LOG("[aim]   %-9s hook=%s calls=%u subs=%u skips=%u type=%s", s->name,
                s->enabled.load(std::memory_order_relaxed) ? "on " : "off",
                s->calls.load(std::memory_order_relaxed),
                s->subs.load(std::memory_order_relaxed),
                s->skips.load(std::memory_order_relaxed),
                s->resultType == kRotator ? "rot" : s->resultType == kVector ? "vec" : "?");
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
    g_syms = symbols;
    BVR_LOG("[aim] init: firestart=%p aimerror=%p viewpoint=%p viewdir=%p trace=%p",
            g_syms.execWeaponFireStart, g_syms.execWeaponAimError, g_syms.execPawnViewPoint,
            g_syms.execPawnViewDir, g_syms.execActorTrace);
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
        BVR_LOG("[aim] unknown command '%s' (on|off|probe|dump|origin|seam|test|status)", verb);
    }
}

bool hook_live() {
    return g_fireStart.enabled.load(std::memory_order_relaxed) ||
           g_aimError.enabled.load(std::memory_order_relaxed) ||
           g_viewPoint.enabled.load(std::memory_order_relaxed) ||
           g_viewDir.enabled.load(std::memory_order_relaxed);
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

    ImGui::Text("seam calls: fs %u/%u  ae %u/%u  vp %u/%u  vd %u/%u  (subs/calls)",
                g_fireStart.subs.load(std::memory_order_relaxed),
                g_fireStart.calls.load(std::memory_order_relaxed),
                g_aimError.subs.load(std::memory_order_relaxed),
                g_aimError.calls.load(std::memory_order_relaxed),
                g_viewPoint.subs.load(std::memory_order_relaxed),
                g_viewPoint.calls.load(std::memory_order_relaxed),
                g_viewDir.subs.load(std::memory_order_relaxed),
                g_viewDir.calls.load(std::memory_order_relaxed));
    ImGui::Text("last sub: %s origin (%.0f %.0f %.0f) yaw %d pitch %d",
                g_lastSubHand.load(std::memory_order_relaxed) ? "RIGHT" : "LEFT",
                g_lastSubX.load(std::memory_order_relaxed),
                g_lastSubY.load(std::memory_order_relaxed),
                g_lastSubZ.load(std::memory_order_relaxed),
                g_lastSubYaw.load(std::memory_order_relaxed),
                g_lastSubPitch.load(std::memory_order_relaxed));
}

} // namespace bvr::b1r::aim
