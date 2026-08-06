#include "game/bioshockinf/rig.h"

#include "core/hooks/pattern_scan.h"
#include "core/util/log.h"
#include "game/bioshockinf/camera.h"
#include "game/bioshockinf/patterns.h"
#include "game/bioshockinf/reflect.h"

#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstring>

namespace bvr::bsi::rig {
namespace {

std::atomic<int> g_writePoint{static_cast<int>(WritePoint::Off)};
std::atomic<bool> g_probe{true};  // compute and count, write NOTHING
std::atomic<bool> g_armed{false}; // ships DISABLED
std::atomic<bool> g_poisoned{false};
std::atomic<int> g_carrierHand{1}; // the RIGHT hand owns the single carrier
std::atomic<float> g_scale[2] = {{1.0f}, {1.0f}};
std::atomic<float> g_weaponScale{1.0f};
std::atomic<int> g_armsMode{1}; // follow, but the verb refuses - see set_arms_mode

// --- resolve / epoch --------------------------------------------------------
// Game-thread-only state. The pointer WALK is redone every frame (four
// dereferences and four is_memory_valid calls is nothing, and a stale actor
// pointer surviving a level load is how a mod becomes a crash); only the
// expensive identity VERDICT is cached, per epoch.
uint32_t g_epoch = 1;
void* g_lastFpa = nullptr;
void* g_lastPc = nullptr;
bool g_identityCached = false;
bool g_identityVerdict = false;
char g_fpaClass[64] = {};

// The saved engine-authored transform, captured on the first write of a drive
// episode. Capturing every frame would save our OWN write back and make
// release() a no-op.
bool g_haveSaved = false;
uint32_t g_savedEpoch = 0;
void* g_savedFpa = nullptr;
FVector g_savedLoc{};
FRotator g_savedRot{};

// Read-back oracle.
bool g_haveWrite[2] = {false, false};
FVector g_writeLoc[2]{};
uint64_t g_writeStamp[2] = {0, 0};

// Last computed-but-maybe-not-written target, for probe mode's log.
bool g_haveProbeTarget = false;
GamePose g_probeTarget{};

struct Refusals {
    uint32_t gate = 0, thread = 0, dead = 0, identity = 0, probeOff = 0, point = 0;
    uint32_t noCarrier = 0, fault = 0, poisoned = 0, disarmed = 0;
};
Refusals g_ref;
uint32_t g_writes = 0, g_reapplies = 0, g_hits[4] = {0, 0, 0, 0};
uint32_t g_consecutiveFaults = 0;

// --- the two-name-cache dispatch helper -------------------------------------
// CACHE 1, the FName INDEX, is kept FOR THE PROCESS: UE3 registers names and
// never compacts GNames, so an index that resolved once stays valid. One
// fname_find per name per process, on a COMMAND, never on a frame.
// CACHE 2, the UFunction POINTER, is kept PER EPOCH: FindFunction walks a
// class's function map and the result is only good for objects of that class.
struct NativeSlot {
    const char* name;
    int32_t nameIndex;
    void* fn;
    uint32_t epoch;
    uint32_t calls, misses, faults;
};

enum NativeId {
    kNSetDrawScale = 0,
    kNSetRotation,
    kNForceUpdateComponents,
    kNSetHidden,
    kNCount
};

NativeSlot g_nat[kNCount] = {
    {"SetDrawScale", -1, nullptr, 0, 0, 0, 0},
    {"SetRotation", -1, nullptr, 0, 0, 0, 0},
    {"ForceUpdateComponents", -1, nullptr, 0, 0, 0, 0},
    {"SetHidden", -1, nullptr, 0, 0, 0, 0},
};
bool g_namesWarm = false;

using FindFunctionFn = void*(__fastcall*)(void*, void*, int32_t, int32_t, int32_t);
using ProcessEventFn = void(__fastcall*)(void*, void*, void*, void*, void*);

uint32_t rva_of(const void* p) {
    return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(p) -
                                 static_cast<uintptr_t>(patterns::kExpectedImageBase));
}

// SEH-isolated, in its own frame with NO C++ objects (SEH plus unwinding is a
// hard compiler error). 0 = ok, 1 = FindFunction null, 2 = fault.
int dispatch_seh(void* obj, const uint8_t* const* vt, int32_t nameIndex, void** inoutFn,
                 void* parms, uint32_t* outExcept) {
    __try {
        void* fn = *inoutFn;
        if (!fn) {
            FindFunctionFn ff = reinterpret_cast<FindFunctionFn>(
                const_cast<uint8_t*>(vt[patterns::kFindFunctionVtableOffset / 4]));
            fn = ff(obj, nullptr, nameIndex, 0, 0);
            *inoutFn = fn;
        }
        if (!fn) return 1;
        ProcessEventFn pe = reinterpret_cast<ProcessEventFn>(
            const_cast<uint8_t*>(vt[patterns::kProcessEventVtableOffset / 4]));
        pe(obj, nullptr, fn, parms, nullptr);
        return 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *outExcept = GetExceptionCode();
        return 2;
    }
}

bool vtable_ok(void* obj) {
    if (!bvr::pattern_scan::is_memory_valid(obj, 4)) return false;
    const uint8_t* const* vt = *reinterpret_cast<const uint8_t* const* const*>(obj);
    if (!bvr::pattern_scan::is_memory_valid(vt, patterns::kProcessEventVtableOffset + 4))
        return false;
    if (rva_of(vt[patterns::kFindFunctionVtableOffset / 4]) != patterns::kFindFunctionRva)
        return false;
    const uint32_t pe = rva_of(vt[patterns::kProcessEventVtableOffset / 4]);
    return pe == patterns::kActorProcessEventRva || pe == patterns::kProcessEventRva;
}

// Never reaches fname_find. A per-frame path physically cannot pay the scan -
// that is a structural guarantee, not a discipline.
bool call_native(void* obj, NativeId id, void* parms) {
    NativeSlot& s = g_nat[id];
    if (!patterns::rva_trusted()) { ++g_ref.gate; return false; }
    if (GetCurrentThreadId() != camera::camera_tid()) { ++g_ref.thread; return false; }
    if (s.nameIndex < 0) { ++s.misses; return false; }
    if (!bvr::pattern_scan::is_memory_valid(obj, 4)) { ++g_ref.dead; return false; }
    if (s.epoch != g_epoch) { s.fn = nullptr; s.epoch = g_epoch; }
    const uint8_t* const* vt = *reinterpret_cast<const uint8_t* const* const*>(obj);
    uint32_t code = 0;
    const int rc = dispatch_seh(obj, vt, s.nameIndex, &s.fn, parms, &code);
    if (rc == 0) { ++s.calls; return true; }
    if (rc == 2) {
        ++s.faults;
        ++g_ref.fault;
        if (++g_consecutiveFaults >= 3) {
            g_poisoned.store(true, std::memory_order_relaxed);
            g_armed.store(false, std::memory_order_relaxed);
            BVR_LOG("[bsi] rig: POISONED after 3 consecutive faults (last 0x%08X in '%s') - "
                    "the write is disarmed. `bsihands reset` to clear, explicitly.",
                    code, s.name);
        }
    } else {
        ++s.misses;
    }
    return false;
}

void bump_epoch(const char* why) {
    ++g_epoch;
    g_identityCached = false;
    g_identityVerdict = false;
    for (auto& s : g_nat) { s.fn = nullptr; s.epoch = 0; }
    // Drop the saved transform WITHOUT restoring it: the object it referred to
    // is gone, and writing it into whatever now lives at that address would be
    // corruption rather than a restore.
    if (g_haveSaved)
        BVR_LOG("[bsi] rig: epoch %u (%s) - dropping the saved transform unrestored (its "
                "object is gone)",
                g_epoch, why);
    g_haveSaved = false;
    g_haveWrite[0] = g_haveWrite[1] = false;
}

bool read_ptr(const void* base, uint32_t off, void** out) {
    if (!bvr::pattern_scan::is_memory_valid(base, off + 4)) return false;
    void* p = *reinterpret_cast<void* const*>(static_cast<const uint8_t*>(base) + off);
    if (!bvr::pattern_scan::is_memory_valid(p, 4)) return false;
    *out = p;
    return true;
}

} // namespace

bool resolve(Resolved* out) {
    Resolved r{};
    r.pc = camera::last_player_controller();
    if (!r.pc || !bvr::pattern_scan::is_memory_valid(r.pc, patterns::kPcPawnOffset + 4)) {
        if (g_lastFpa) bump_epoch("pc lost");
        g_lastFpa = nullptr;
        if (out) *out = r;
        return false;
    }
    if (!read_ptr(r.pc, patterns::kPcPawnOffset, &r.pawn) ||
        !read_ptr(r.pawn, patterns::kPawnAttachmentListOffset, &r.list) ||
        !read_ptr(r.list, patterns::kAttachListFpaSlotOffset, &r.fpa)) {
        if (g_lastFpa) bump_epoch("walk broke");
        g_lastFpa = nullptr;
        if (out) *out = r;
        return false;
    }
    if (r.fpa != g_lastFpa || r.pc != g_lastPc) {
        bump_epoch(r.fpa != g_lastFpa ? "carrier changed" : "pc changed");
        g_lastFpa = r.fpa;
        g_lastPc = r.pc;
    }

    // Identity, once per epoch: the class NAME plus the structural
    // corroboration. Either alone is a hint - the s45 trap was a class name
    // resolved off a raw pointer list. An object owned by AND based on the
    // player pawn, in the actor layout, carrying that class name, is the
    // carrier.
    if (!g_identityCached) {
        g_identityCached = true;
        g_identityVerdict = false;
        reflect::ensure_obj_name_offset();
        if (reflect::object_class_name(r.fpa, g_fpaClass, sizeof g_fpaClass) &&
            strcmp(g_fpaClass, patterns::kFirstPersonAttachmentClass) == 0 &&
            bvr::pattern_scan::is_memory_valid(r.fpa, 0x100)) {
            const uint8_t* a = static_cast<const uint8_t*>(r.fpa);
            void* owner = *reinterpret_cast<void* const*>(a + patterns::kActorOwnerOffset);
            void* base = *reinterpret_cast<void* const*>(a + patterns::kActorBaseOffset);
            void* level = *reinterpret_cast<void* const*>(a + patterns::kActorLevelOffset);
            g_identityVerdict = owner == r.pawn && base == r.pawn &&
                                bvr::pattern_scan::is_memory_valid(level, 4);
        }
        BVR_LOG("[bsi] rig: epoch %u carrier %p class '%s' identity %s", g_epoch, r.fpa,
                g_fpaClass[0] ? g_fpaClass : "?", g_identityVerdict ? "OK" : "REFUSED");
    }
    r.identityOk = g_identityVerdict;
    r.epoch = g_epoch;
    if (out) *out = r;
    return true;
}

bool drive(int hand, const GamePose& target) {
    hand &= 1;
    // ONE carrier, and it is the right hand's. The left hand's target is still
    // computed and printed by status - never a silent zero.
    if (hand != g_carrierHand.load(std::memory_order_relaxed)) {
        ++g_ref.noCarrier;
        return false;
    }
    if (g_poisoned.load(std::memory_order_relaxed)) { ++g_ref.poisoned; return false; }

    Resolved r{};
    if (!resolve(&r) || !r.fpa) return false;
    if (!r.identityOk) { ++g_ref.identity; return false; }
    if (GetCurrentThreadId() != camera::camera_tid()) { ++g_ref.thread; return false; }

    g_probeTarget = target;
    g_haveProbeTarget = true;

    if (!g_armed.load(std::memory_order_relaxed)) { ++g_ref.disarmed; return false; }
    if (g_probe.load(std::memory_order_relaxed)) { ++g_ref.probeOff; return false; }

    uint8_t* a = static_cast<uint8_t*>(r.fpa);
    FVector back{};
    bool ok = false;
    __try {
        // Capture the engine's own transform ONCE per drive episode, before the
        // first write of that episode.
        if (!g_haveSaved) {
            g_savedLoc = *reinterpret_cast<const FVector*>(a + patterns::kActorLocationOffset);
            g_savedRot = *reinterpret_cast<const FRotator*>(a + patterns::kActorRotationOffset);
            g_savedFpa = r.fpa;
            g_savedEpoch = g_epoch;
            g_haveSaved = true;
        }
        // ABSOLUTE, both fields, every frame. Absolute makes the several pass-2
        // dispatches per doubled draw idempotent, the same property the camera's
        // own pass-2 replay relies on.
        *reinterpret_cast<FVector*>(a + patterns::kActorLocationOffset) = target.loc;
        *reinterpret_cast<FRotator*>(a + patterns::kActorRotationOffset) = target.rot;
        // READ BACK, not echo. An echo proves we computed a number; a read-back
        // proves the number is in the object. That is what makes last_write an
        // oracle rather than a mirror.
        back = *reinterpret_cast<const FVector*>(a + patterns::kActorLocationOffset);
        ok = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ++g_ref.fault;
        if (++g_consecutiveFaults >= 3) {
            g_poisoned.store(true, std::memory_order_relaxed);
            g_armed.store(false, std::memory_order_relaxed);
        }
        return false;
    }
    if (!ok) return false;
    g_consecutiveFaults = 0;
    g_writeLoc[hand] = back;
    g_writeStamp[hand] = GetTickCount64();
    g_haveWrite[hand] = true;
    ++g_writes;
    return true;
}

void reapply() {
    if (!g_haveSaved || !g_armed.load(std::memory_order_relaxed)) return;
    if (g_probe.load(std::memory_order_relaxed)) return;
    const int h = g_carrierHand.load(std::memory_order_relaxed);
    if (!g_haveWrite[h]) return;
    Resolved r{};
    if (!resolve(&r) || !r.fpa || !r.identityOk) return;
    uint8_t* a = static_cast<uint8_t*>(r.fpa);
    __try {
        *reinterpret_cast<FVector*>(a + patterns::kActorLocationOffset) = g_writeLoc[h];
        *reinterpret_cast<FRotator*>(a + patterns::kActorRotationOffset) = g_probeTarget.rot;
        ++g_reapplies;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ++g_ref.fault;
    }
}

void release(const char* why) {
    if (!g_haveSaved) return;
    Resolved r{};
    const bool intact =
        resolve(&r) && r.fpa == g_savedFpa && g_epoch == g_savedEpoch && r.identityOk;
    if (!intact) {
        BVR_LOG("[bsi] rig: release (%s) DECLINED to restore - identity is not intact "
                "(object or epoch changed). Forgetting the saved transform instead.",
                why);
        g_haveSaved = false;
        g_haveWrite[0] = g_haveWrite[1] = false;
        return;
    }
    uint8_t* a = static_cast<uint8_t*>(r.fpa);
    __try {
        *reinterpret_cast<FVector*>(a + patterns::kActorLocationOffset) = g_savedLoc;
        *reinterpret_cast<FRotator*>(a + patterns::kActorRotationOffset) = g_savedRot;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ++g_ref.fault;
    }
    BVR_LOG("[bsi] rig: released (%s) - the engine's own transform is back", why);
    g_haveSaved = false;
    g_haveWrite[0] = g_haveWrite[1] = false;
}

bool last_write(int hand, float* x, float* y, float* z, uint64_t* ageMs) {
    hand &= 1;
    if (!g_haveWrite[hand]) return false;
    if (x) *x = g_writeLoc[hand].x;
    if (y) *y = g_writeLoc[hand].y;
    if (z) *z = g_writeLoc[hand].z;
    if (ageMs) *ageMs = GetTickCount64() - g_writeStamp[hand];
    return true;
}

void on_world_change(const char* why) { bump_epoch(why); }

WritePoint write_point() {
    return static_cast<WritePoint>(g_writePoint.load(std::memory_order_relaxed));
}
void set_write_point(WritePoint p) {
    g_writePoint.store(static_cast<int>(p), std::memory_order_relaxed);
}
bool probe() { return g_probe.load(std::memory_order_relaxed); }
void set_probe(bool on) { g_probe.store(on, std::memory_order_relaxed); }
bool armed() { return g_armed.load(std::memory_order_relaxed); }
void set_armed(bool on) { g_armed.store(on, std::memory_order_relaxed); }

void set_scale(int h, float s) { g_scale[h & 1].store(s, std::memory_order_relaxed); }
float scale_of(int h) { return g_scale[h & 1].load(std::memory_order_relaxed); }
void set_weapon_scale(float s) { g_weaponScale.store(s, std::memory_order_relaxed); }
float weapon_scale() { return g_weaponScale.load(std::memory_order_relaxed); }

void set_arms_mode(int mode) {
    g_armsMode.store(mode, std::memory_order_relaxed);
    // REFUSES, and says exactly what would unblock it. Session 46 R1 killed the
    // only two candidate components: neither XSkeletalMeshComponent on the pawn
    // is the first-person arms (hiding either changed nothing in first person),
    // so the component that carries the arms hangs off the FP ATTACHMENT and is
    // not yet named. HideBoneByName needs a specific component and a specific
    // bone list, and a guessed bone list is a guess.
    BVR_LOG("[bsi] rig arms: mode stored (%d) but NOT APPLIED - the first-person mesh "
            "component is not identified. R1 established it is NOT pawn+0x2E4 or "
            "pawn+0x72C (hiding either changes nothing in first person), so it hangs off "
            "the FP attachment and its component list still has to be walked.",
            mode);
}
int arms_mode() { return g_armsMode.load(std::memory_order_relaxed); }

bool warm_names() {
    if (g_namesWarm) return true;
    if (patterns::fname_count() <= 0) return false;
    const uint64_t t0 = GetTickCount64();
    int found = 0, missing = 0;
    for (auto& s : g_nat) {
        if (s.nameIndex >= 0) { ++found; continue; }
        s.nameIndex = patterns::fname_find(s.name);
        if (s.nameIndex >= 0) ++found;
        else {
            ++missing;
            BVR_LOG("[bsi] rig: '%s' is NOT in GNames - that native is unreachable on this "
                    "build and every verb needing it will refuse",
                    s.name);
        }
    }
    g_namesWarm = true;
    BVR_LOG("[bsi] rig: name warm-up %d found / %d missing in %llu ms. ONE-SHOT: GNames "
            "only grows, so these indices are valid for the process - this is the only "
            "linear scan this module will ever do, and no per-frame path can reach one.",
            found, missing, static_cast<unsigned long long>(GetTickCount64() - t0));
    return true;
}

void status_log() {
    Resolved r{};
    const bool ok = resolve(&r);
    const char* pt = write_point() == WritePoint::Off        ? "off"
                     : write_point() == WritePoint::DrawEntry  ? "drawentry"
                     : write_point() == WritePoint::DrawExit   ? "drawexit"
                                                               : "cameratail";
    BVR_LOG("[bsi] rig: armed=%d probe=%d point=%s poisoned=%d epoch=%u writes=%u "
            "reapplies=%u",
            armed() ? 1 : 0, probe() ? 1 : 0, pt, g_poisoned.load(std::memory_order_relaxed) ? 1 : 0,
            g_epoch, g_writes, g_reapplies);
    if (!ok || !r.fpa) {
        BVR_LOG("[bsi] rig: carrier UNRESOLVED (pc=%p pawn=%p list=%p) - no pawn yet, or a "
                "pawnless state. This is a wait, not a failure.",
                r.pc, r.pawn, r.list);
    } else {
        BVR_LOG("[bsi] rig: pc=%p pawn=%p list=%p fpa=%p class='%s' identity=%s", r.pc,
                r.pawn, r.list, r.fpa, g_fpaClass, r.identityOk ? "OK" : "REFUSED");
        if (bvr::pattern_scan::is_memory_valid(r.fpa, 0x60)) {
            const uint8_t* a = static_cast<const uint8_t*>(r.fpa);
            const FVector& L =
                *reinterpret_cast<const FVector*>(a + patterns::kActorLocationOffset);
            const FRotator& R =
                *reinterpret_cast<const FRotator*>(a + patterns::kActorRotationOffset);
            BVR_LOG("[bsi] rig: LIVE loc=(%.1f %.1f %.1f) rot=(%d %d %d) = (%.1f %.1f %.1f) deg",
                    L.x, L.y, L.z, R.pitch, R.yaw, R.roll,
                    static_cast<float>(R.pitch) / kRotUnitsPerDegree,
                    static_cast<float>(R.yaw) / kRotUnitsPerDegree,
                    static_cast<float>(R.roll) / kRotUnitsPerDegree);
        }
    }
    if (g_haveProbeTarget) {
        BVR_LOG("[bsi] rig: TARGET loc=(%.1f %.1f %.1f) rot=(%d %d %d) scale L=%.2f R=%.2f "
                "weapon=%.2f",
                g_probeTarget.loc.x, g_probeTarget.loc.y, g_probeTarget.loc.z,
                g_probeTarget.rot.pitch, g_probeTarget.rot.yaw, g_probeTarget.rot.roll,
                scale_of(0), scale_of(1), weapon_scale());
    }
    float x = 0, y = 0, z = 0;
    uint64_t age = 0;
    if (last_write(1, &x, &y, &z, &age))
        BVR_LOG("[bsi] rig: last write READ BACK loc=(%.1f %.1f %.1f) age %llu ms", x, y, z,
                static_cast<unsigned long long>(age));
    BVR_LOG("[bsi] rig: refusals gate=%u thread=%u dead=%u identity=%u disarmed=%u "
            "probe=%u point=%u noCarrier=%u fault=%u poisoned=%u",
            g_ref.gate, g_ref.thread, g_ref.dead, g_ref.identity, g_ref.disarmed,
            g_ref.probeOff, g_ref.point, g_ref.noCarrier, g_ref.fault, g_ref.poisoned);
    BVR_LOG("[bsi] rig: write-point hits drawentry=%u drawexit=%u cameratail=%u (compare "
            "against the present rate before arming any of them)",
            g_hits[1], g_hits[2], g_hits[3]);
}

void note_hit(WritePoint p) {
    const int i = static_cast<int>(p);
    if (i >= 0 && i < 4) ++g_hits[i];
}

bool handle_command(const char* args) {
    if (strncmp(args, "point", 5) == 0) {
        const char* v = args + 5;
        while (*v == ' ') ++v;
        WritePoint p = WritePoint::Off;
        if (strncmp(v, "drawentry", 9) == 0) p = WritePoint::DrawEntry;
        else if (strncmp(v, "drawexit", 8) == 0) p = WritePoint::DrawExit;
        else if (strncmp(v, "cameratail", 10) == 0) p = WritePoint::CameraTail;
        set_write_point(p);
        BVR_LOG("[bsi] rig: write point = %s", v[0] ? v : "off");
        return true;
    }
    if (strncmp(args, "probe", 5) == 0) {
        set_probe(strstr(args, "off") == nullptr);
        BVR_LOG("[bsi] rig: probe %s (%s)", probe() ? "ON" : "off",
                probe() ? "computes and counts, writes NOTHING" : "the write is live");
        return true;
    }
    if (strncmp(args, "arm", 3) == 0) {
        const bool on = strstr(args, "off") == nullptr;
        set_armed(on);
        BVR_LOG("[bsi] rig: %s", on ? "ARMED (probe must also be off before a byte moves)"
                                    : "disarmed");
        if (!on) release("disarmed");
        return true;
    }
    if (strncmp(args, "reset", 5) == 0) {
        g_poisoned.store(false, std::memory_order_relaxed);
        g_consecutiveFaults = 0;
        bump_epoch("manual reset");
        BVR_LOG("[bsi] rig: poison cleared, epoch bumped");
        return true;
    }
    if (strncmp(args, "warm", 4) == 0) {
        warm_names();
        return true;
    }
    if (strncmp(args, "listdump", 8) == 0) {
        // Candidate (a) for the second carrier: walk the attachment list and
        // name every slot that reads as a UObject. Read-only, zero risk. Run it
        // on a save with a Vigor equipped - the s46 walk found ONE
        // XFirstPersonAttachment, but no Vigor was equipped at the time.
        Resolved r{};
        if (!resolve(&r) || !r.list) {
            BVR_LOG("[bsi] rig listdump: no list yet");
            return true;
        }
        reflect::ensure_obj_name_offset();
        BVR_LOG("[bsi] rig listdump: %p, 32 slots", r.list);
        for (uint32_t off = 0; off < 0x80; off += 4) {
            void* p = nullptr;
            if (!read_ptr(r.list, off, &p)) continue;
            char nm[64] = {};
            if (reflect::object_class_name(p, nm, sizeof nm) && nm[0])
                BVR_LOG("[bsi] rig listdump:   +0x%03X  %p  %s", off, p, nm);
        }
        return true;
    }
    return false;
}

} // namespace bvr::bsi::rig
