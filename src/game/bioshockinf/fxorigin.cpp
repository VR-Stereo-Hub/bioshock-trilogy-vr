#include "game/bioshockinf/fxorigin.h"

#include "core/hooks/pattern_scan.h"
#include "core/util/log.h"
#include "core/vr/openxr_runtime.h"
#include "game/bioshockinf/aim.h"
#include "game/bioshockinf/bones.h"
#include "game/bioshockinf/camera.h"
#include "game/bioshockinf/frame_context.h"
#include "game/bioshockinf/inf_math.h"
#include "game/bioshockinf/patterns.h"
#include "game/bioshockinf/reflect.h"

#include <MinHook.h>
#include <imgui.h>
#include <intrin.h>
#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstring>

namespace bvr::bsi::fxorigin {
namespace {

// The seam's signature (patterns.h derivation): a `__thiscall` VIRTUAL with
// ZERO stack args - plain `ret`, hence zero declared args here (the RTC
// no-dump rule, inverted-form gate like the fidget impl hook).
using UpdateAttachmentsFn = void(__fastcall*)(void* self, void* edx);

UpdateAttachmentsFn g_original = nullptr;
void* g_target = nullptr;

// HONEST STATUS (s50 flat measurement): this hook is NOT the frozen-FX family
// fix. The dirty-count instrument below proved SpaceBases already holds the
// composed atoms at virtually every attach update (cleanTicks ~100%) - the
// render-side drive covers the attach lane on its own, and the FROZEN family
// (charge plume, ready sparkle, muzzle/tracer FX) reads its positions from a
// lane that never touches SpaceBases (every candidate falsified in
// ENGINE_NOTES "s50"). What the substitute DOES cover: the rare ticks where
// the engine's eval restamps SpaceBases between our writes (dirtyTicks 1-2
// per ~10 min) - without the reapply those are one-frame authored-pose
// flashes on everything the attach walker positions. Cheap, safe, measured.
std::atomic<bool> g_probe{true};       // install + observe (read-only)
std::atomic<bool> g_substitute{true};  // reapply composed atoms before the update
std::atomic<bool> g_installed{false};

// The updater runs for EVERY skeletal component on the map, every tick. The
// gate is self == the driven FP component; the split is telemetry.
std::atomic<uint32_t> g_calls{0};
std::atomic<uint32_t> g_playerCalls{0};
std::atomic<uint32_t> g_reapplies{0};
// s50 eval-ordering instrument: how many driven atoms the engine had
// overwritten since our previous write, sampled at each reapply. Nonzero =
// the anim eval restamps SpaceBases between our writes (the frozen-FX reader
// could be seeing those); zero = SpaceBases was still ours.
std::atomic<uint32_t> g_lastDirty{0};
std::atomic<uint32_t> g_dirtyTicks{0};
std::atomic<uint32_t> g_cleanTicks{0};

uint64_t g_lastLogMs = 0;

void __fastcall UpdateAttachmentsDetour(void* self, void* edx) {
    g_calls.fetch_add(1, std::memory_order_relaxed);
    if (self && self == bones::component()) {
        g_playerCalls.fetch_add(1, std::memory_order_relaxed);
        if (g_substitute.load(std::memory_order_relaxed)) {
            // Repaint OUR composed atoms over the engine's fresh anim eval so
            // the attachment transforms (and every later socket read this
            // tick) come from the driven pose. reapply() carries its own
            // rig-intact + freshness gates and no-ops when the drive is off.
            const int dirty = bones::reapply();
            g_lastDirty.store(static_cast<uint32_t>(dirty), std::memory_order_relaxed);
            (dirty ? g_dirtyTicks : g_cleanTicks).fetch_add(1, std::memory_order_relaxed);
            g_reapplies.fetch_add(1, std::memory_order_relaxed);
        }
        const uint64_t now = GetTickCount64();
        if (now - g_lastLogMs >= 5000) {
            g_lastLogMs = now;
            BVR_LOG("[bsi] fxorigin: attach update on the FP component - %s | calls=%u "
                    "player=%u reapplied=%u | pre-reapply overwritten atoms: last=%u "
                    "dirtyTicks=%u cleanTicks=%u",
                    g_substitute.load(std::memory_order_relaxed) ? "COMPOSED ATOMS REAPPLIED"
                                                                 : "probe (pass-through)",
                    g_calls.load(std::memory_order_relaxed),
                    g_playerCalls.load(std::memory_order_relaxed),
                    g_reapplies.load(std::memory_order_relaxed),
                    g_lastDirty.load(std::memory_order_relaxed),
                    g_dirtyTicks.load(std::memory_order_relaxed),
                    g_cleanTicks.load(std::memory_order_relaxed));
        }
    }
    g_original(self, edx);
}

// ---- THE EFFECT-UPDATE SEAM (the frozen-family fix candidate) ---------------
//
// patterns.h derivation: called once per live effect record per tick by the
// effect playback tick (rva 0x436490). this = the effect's component; the
// record's LOCATION buffer arrives by pointer, so rewriting it here - before
// the original consumes it - moves the effect. `ret 0x34` = THIRTEEN stack
// args (the RTC no-dump rule).
using EffectUpdateFn = uint32_t(__fastcall*)(void* self, void* edx, void* rec, FVector* loc,
                                             void* rot, uint32_t a4, uint32_t a5, uint32_t a6,
                                             uint32_t a7, uint32_t a8, uint32_t a9,
                                             uint32_t a10, uint32_t a11, uint32_t a12,
                                             uint32_t a13);

EffectUpdateFn g_fxOriginal = nullptr;
std::atomic<bool> g_fxProbe{false};       // install + log candidate records
std::atomic<bool> g_fxSubstitute{false};  // rewrite first-person record locations
std::atomic<bool> g_fxInstalled{false};

std::atomic<uint32_t> g_fxCalls{0};
std::atomic<uint32_t> g_fxFpCalls{0};   // owner == attachment or pawn
std::atomic<uint32_t> g_fxSubs{0};
std::atomic<int> g_fxDumpLeft{0};
uint64_t g_fxLastLogMs = 0;
// s51 fork (i): the runtime caller census (the static E8 census counts 194
// call sites - only the runtime one can say which fire during a charge).
// Same fixed-slot shape as camera.cpp's GetPlayerViewPoint census.
constexpr int kFxCallerSlots = 24;
struct FxCallerSlot {
    uint32_t rva;
    uint32_t count;
};
FxCallerSlot g_fxCallers[kFxCallerSlots] = {};
uint32_t g_fxCallerOverflow = 0;

void fx_note_caller(void* retAddr) {
    const uint32_t rva =
        static_cast<uint32_t>(reinterpret_cast<const uint8_t*>(retAddr) -
                              patterns::image_base());
    for (int i = 0; i < kFxCallerSlots; ++i) {
        if (g_fxCallers[i].rva == rva) {
            ++g_fxCallers[i].count;
            return;
        }
        if (g_fxCallers[i].rva == 0) {
            g_fxCallers[i].rva = rva;
            g_fxCallers[i].count = 1;
            return;
        }
    }
    ++g_fxCallerOverflow;
}

// The same displacement refusal the fire seam uses: a substitution that moves
// an effect absurdly far means a broken basis, not a long arm.
constexpr float kFxMaxDisplacementUu = 400.0f;

bool owner_is_first_person(void* comp, void** ownerOut) {
    if (!comp || !bvr::pattern_scan::is_memory_valid(comp, patterns::kSkelCompOwnerOffset + 4))
        return false;
    void* owner = *reinterpret_cast<void* const*>(static_cast<const uint8_t*>(comp) +
                                                  patterns::kSkelCompOwnerOffset);
    if (ownerOut) *ownerOut = owner;
    if (!owner) return false;
    if (owner == bones::attachment()) return true;
    void* pc = camera::last_player_controller();
    if (pc && bvr::pattern_scan::is_memory_valid(pc, patterns::kPcPawnOffset + 4)) {
        void* pawn = *reinterpret_cast<void* const*>(static_cast<const uint8_t*>(pc) +
                                                     patterns::kPcPawnOffset);
        if (pawn && owner == pawn) return true;
    }
    return false;
}

uint32_t __fastcall EffectUpdateDetour(void* self, void* edx, void* rec, FVector* loc,
                                       void* rot, uint32_t a4, uint32_t a5, uint32_t a6,
                                       uint32_t a7, uint32_t a8, uint32_t a9, uint32_t a10,
                                       uint32_t a11, uint32_t a12, uint32_t a13) {
    g_fxCalls.fetch_add(1, std::memory_order_relaxed);
    fx_note_caller(_ReturnAddress());
    void* owner = nullptr;
    const bool fp = owner_is_first_person(self, &owner);
    bool wrote = false;
    FVector engine{};
    int hand = 1;
    // s51 fork (i): the dump verb logs ALL records, not FP-gated - the frozen
    // family's writer was proven NOT to be an FP-owned record through this
    // seam (s50: ~2 calls/s, fp=0, plume at 90 Hz), so an FP-gated dump can
    // never show the interesting population. loc is NULLABLE outside the tick
    // path (measured: 310 census calls, zero with a location buffer).
    {
        int dump = g_fxDumpLeft.load(std::memory_order_relaxed);
        if (dump > 0) {
            g_fxDumpLeft.fetch_sub(1, std::memory_order_relaxed);
            char cls[64] = "?";
            reflect::class_name_of(self, cls, sizeof cls);
            const FrameContext& fc = camera::frame_context();
            float dcam = -1.0f;
            if (loc && fc.valid) {
                const float dx = loc->x - fc.writtenLocX;
                const float dy = loc->y - fc.writtenLocY;
                const float dz = loc->z - fc.writtenLocZ;
                dcam = sqrtf(dx * dx + dy * dy + dz * dz);
            }
            BVR_LOG("[bsi] fxupdate: rec %p comp=%p (%s) owner=%p fp=%d loc=%s(%.1f %.1f "
                    "%.1f) dCam=%.1f caller=0x%X",
                    rec, self, cls, owner, fp ? 1 : 0, loc ? "" : "NULL",
                    loc ? loc->x : 0.0f, loc ? loc->y : 0.0f, loc ? loc->z : 0.0f, dcam,
                    static_cast<uint32_t>(reinterpret_cast<const uint8_t*>(
                                              _ReturnAddress()) -
                                          patterns::image_base()));
        }
    }
    if (fp && loc) {
        g_fxFpCalls.fetch_add(1, std::memory_order_relaxed);
        engine = *loc;
        if (g_fxSubstitute.load(std::memory_order_relaxed)) {
            const FrameContext& fc = camera::frame_context();
            if (fc.valid) {
                // Hand attribution: the record's engine position sits at the
                // authored spot for ITS hand - left of the camera's right axis
                // for the vigor hand, right of it for the weapon. Sign of the
                // lateral component (in the pre-drive game-yaw basis) picks
                // the hand; the flat probe verifies.
                FRotator yawOnly{};
                yawOnly.yaw = fc.gameYawUnits;
                float fwd[3], right[3], up[3];
                ue_rot_basis(yawOnly, fwd, right, up);
                const float dx = engine.x - fc.writtenLocX;
                const float dy = engine.y - fc.writtenLocY;
                const float dz = engine.z - fc.writtenLocZ;
                const float lateral = dx * right[0] + dy * right[1] + dz * right[2];
                hand = lateral < 0.0f ? 0 : 1;
                bvr::vr::HeadPose hp{};
                if (bvr::vr::get_hand_pose(hand, /*aimPose=*/true, hp)) {
                    const float pos[3] = {hp.px, hp.py, hp.pz};
                    const float quat[4] = {hp.qx, hp.qy, hp.qz, hp.qw};
                    const GamePose gp = ray_pose_from_xr(fc, pos, quat, aim::trim_get(hand, 0),
                                                         aim::trim_get(hand, 1));
                    const float ddx = gp.loc.x - engine.x;
                    const float ddy = gp.loc.y - engine.y;
                    const float ddz = gp.loc.z - engine.z;
                    const float disp = sqrtf(ddx * ddx + ddy * ddy + ddz * ddz);
                    if (disp <= kFxMaxDisplacementUu) {
                        loc->x = gp.loc.x;
                        loc->y = gp.loc.y;
                        loc->z = gp.loc.z;
                        g_fxSubs.fetch_add(1, std::memory_order_relaxed);
                        wrote = true;
                    }
                }
            }
        }
        const uint64_t now = GetTickCount64();
        if (now - g_fxLastLogMs >= 2000) {
            g_fxLastLogMs = now;
            char cls[64] = "?";
            reflect::class_name_of(self, cls, sizeof cls);
            BVR_LOG("[bsi] fxupdate: FP record %p comp=%p (%s) owner=%p loc=(%.1f %.1f "
                    "%.1f)%s%c | calls=%u fp=%u subs=%u",
                    rec, self, cls, owner, engine.x, engine.y, engine.z,
                    wrote ? " -> HAND " : " ", wrote ? (hand ? 'R' : 'L') : ' ',
                    g_fxCalls.load(std::memory_order_relaxed),
                    g_fxFpCalls.load(std::memory_order_relaxed),
                    g_fxSubs.load(std::memory_order_relaxed));
        }
    }
    return g_fxOriginal(self, edx, rec, loc, rot, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13);
}

bool fx_wants_install() {
    return (g_fxProbe.load(std::memory_order_relaxed) ||
            g_fxSubstitute.load(std::memory_order_relaxed)) &&
           !g_fxInstalled.load(std::memory_order_relaxed);
}

bool fx_try_install() {
    if (g_fxInstalled.load(std::memory_order_relaxed)) return true;
    if (!patterns::rva_trusted()) {
        BVR_LOG("[bsi] fxupdate: REFUSED - build gate closed");
        return false;
    }
    uint8_t* impl = const_cast<uint8_t*>(patterns::image_base()) + patterns::kEffectUpdateRva;
    if (!bvr::pattern_scan::is_memory_valid(impl, 0x400)) {
        BVR_LOG("[bsi] fxupdate: REFUSED - impl rva 0x%X not readable",
                patterns::kEffectUpdateRva);
        return false;
    }
    if (memcmp(impl, patterns::kEffectUpdatePrologue,
               sizeof patterns::kEffectUpdatePrologue) != 0) {
        BVR_LOG("[bsi] fxupdate: REFUSED - prologue at rva 0x%X does not match the "
                "derivation (stale build?)",
                patterns::kEffectUpdateRva);
        return false;
    }
    bool sawRet = false;
    for (size_t i = 0; i + 2 < 0x400; ++i) {
        if (impl[i] == 0xC2 && impl[i + 1] == patterns::kEffectUpdateRetImm &&
            impl[i + 2] == 0x00) {
            sawRet = true;
            break;
        }
    }
    if (!sawRet) {
        BVR_LOG("[bsi] fxupdate: REFUSED - no `ret 0x%X` in the first 0x400 bytes at rva "
                "0x%X",
                patterns::kEffectUpdateRetImm, patterns::kEffectUpdateRva);
        return false;
    }
    if (MH_CreateHook(impl, reinterpret_cast<void*>(&EffectUpdateDetour),
                      reinterpret_cast<void**>(&g_fxOriginal)) != MH_OK) {
        BVR_LOG("[bsi] fxupdate: MH_CreateHook failed at rva 0x%X",
                patterns::kEffectUpdateRva);
        return false;
    }
    if (MH_EnableHook(impl) != MH_OK) {
        BVR_LOG("[bsi] fxupdate: MH_EnableHook failed at rva 0x%X",
                patterns::kEffectUpdateRva);
        return false;
    }
    g_fxInstalled.store(true, std::memory_order_relaxed);
    BVR_LOG("[bsi] fxupdate: effect-update seam hooked - rva 0x%X (per-record playback "
            "update, 13 args, location by pointer). Mode: %s.",
            patterns::kEffectUpdateRva,
            g_fxSubstitute.load(std::memory_order_relaxed) ? "SUBSTITUTING"
                                                           : "PROBE (read-only)");
    return true;
}

// ---- THE EFFECT PLAYBACK TICK (s51 fork ii) ---------------------------------
//
// Probe-only, default OFF. The one job: capture the live `this` (the tickee)
// so `bsifx t dump` can walk the record table (patterns.h shape) on the game
// thread, name the plume record by its camera-near location, and read the
// stamp pair against the globals. `ret 8` = TWO stack args (the RTC rule).
using EffectTickFn = void(__fastcall*)(void* self, void* edx, uint32_t a1, uint32_t a2);

EffectTickFn g_tickOriginal = nullptr;
std::atomic<bool> g_tickProbe{false};
std::atomic<bool> g_tickInstalled{false};
std::atomic<void*> g_tickee{nullptr};
std::atomic<uint32_t> g_tickCalls{0};

void __fastcall EffectTickDetour(void* self, void* edx, uint32_t a1, uint32_t a2) {
    g_tickCalls.fetch_add(1, std::memory_order_relaxed);
    g_tickee.store(self, std::memory_order_relaxed);
    g_tickOriginal(self, edx, a1, a2);
}

bool tick_wants_install() {
    return g_tickProbe.load(std::memory_order_relaxed) &&
           !g_tickInstalled.load(std::memory_order_relaxed);
}

bool tick_try_install() {
    if (g_tickInstalled.load(std::memory_order_relaxed)) return true;
    if (!patterns::rva_trusted()) {
        BVR_LOG("[bsi] fxtick: REFUSED - build gate closed");
        return false;
    }
    uint8_t* impl = const_cast<uint8_t*>(patterns::image_base()) + patterns::kEffectTickRva;
    if (!bvr::pattern_scan::is_memory_valid(impl, 0x400)) {
        BVR_LOG("[bsi] fxtick: REFUSED - rva 0x%X not readable", patterns::kEffectTickRva);
        return false;
    }
    if (memcmp(impl, patterns::kEffectTickPrologue,
               sizeof patterns::kEffectTickPrologue) != 0) {
        BVR_LOG("[bsi] fxtick: REFUSED - prologue at rva 0x%X does not match the "
                "derivation (stale build?)",
                patterns::kEffectTickRva);
        return false;
    }
    bool sawRet = false;
    for (size_t i = 0; i + 2 < 0x400; ++i) {
        if (impl[i] == 0xC2 && impl[i + 1] == patterns::kEffectTickRetImm &&
            impl[i + 2] == 0x00) {
            sawRet = true;
            break;
        }
    }
    if (!sawRet) {
        BVR_LOG("[bsi] fxtick: REFUSED - no `ret 0x%X` in the first 0x400 bytes at rva 0x%X",
                patterns::kEffectTickRetImm, patterns::kEffectTickRva);
        return false;
    }
    if (MH_CreateHook(impl, reinterpret_cast<void*>(&EffectTickDetour),
                      reinterpret_cast<void**>(&g_tickOriginal)) != MH_OK) {
        BVR_LOG("[bsi] fxtick: MH_CreateHook failed at rva 0x%X", patterns::kEffectTickRva);
        return false;
    }
    if (MH_EnableHook(impl) != MH_OK) {
        BVR_LOG("[bsi] fxtick: MH_EnableHook failed at rva 0x%X", patterns::kEffectTickRva);
        return false;
    }
    g_tickInstalled.store(true, std::memory_order_relaxed);
    BVR_LOG("[bsi] fxtick: effect playback tick hooked - rva 0x%X (probe-only: capture "
            "the tickee, count calls)",
            patterns::kEffectTickRva);
    return true;
}

// The table walk behind `bsifx t dump` - game thread only (the tickee is a
// live engine object; walking it while its own tick mutates it on the same
// thread is safe, from another thread is not).
void tick_dump_table() {
    if (bvr::bsi::camera::camera_tid() == 0 ||
        GetCurrentThreadId() != bvr::bsi::camera::camera_tid()) {
        BVR_LOG("[bsi] fxtick: dump REFUSED - not on the game thread");
        return;
    }
    const uint8_t* tickee = static_cast<const uint8_t*>(
        g_tickee.load(std::memory_order_relaxed));
    if (!tickee) {
        BVR_LOG("[bsi] fxtick: dump - no tickee captured yet (bsifx t probe first, and "
                "the tick must fire once)");
        return;
    }
    if (!bvr::pattern_scan::is_memory_valid(tickee, 0x44)) {
        BVR_LOG("[bsi] fxtick: dump - tickee %p unreadable (stale?)", tickee);
        return;
    }
    const uint8_t* data = *reinterpret_cast<const uint8_t* const*>(
        tickee + patterns::kEffectTickTableDataOffset);
    const int32_t count = *reinterpret_cast<const int32_t*>(
        tickee + patterns::kEffectTickTableCountOffset);
    const uint32_t* stampGlobals = reinterpret_cast<const uint32_t*>(
        patterns::image_base() + patterns::kEffectRecStampGlobalRva);
    uint32_t g0 = 0, g1 = 0;
    if (bvr::pattern_scan::is_memory_valid(stampGlobals, 8)) {
        g0 = stampGlobals[0];
        g1 = stampGlobals[1];
    }
    BVR_LOG("[bsi] fxtick: table dump - tickee=%p data=%p count=%d | stamp globals "
            "%u/%u | tick calls=%u",
            tickee, data, count, g0, g1, g_tickCalls.load(std::memory_order_relaxed));
    if (!data || count <= 0 || count > 4096 ||
        !bvr::pattern_scan::is_memory_valid(data, static_cast<size_t>(count) *
                                                      patterns::kEffectRecStride)) {
        BVR_LOG("[bsi] fxtick: table unreadable/empty - nothing to walk");
        return;
    }
    const FrameContext& fc = camera::frame_context();
    for (int i = 0; i < count; ++i) {
        const uint8_t* rec = data + static_cast<size_t>(i) * patterns::kEffectRecStride;
        const float* loc = reinterpret_cast<const float*>(rec + patterns::kEffectRecLocOffset);
        const int32_t* rot = reinterpret_cast<const int32_t*>(rec +
                                                              patterns::kEffectRecRotOffset);
        void* comp = *reinterpret_cast<void* const*>(rec + patterns::kEffectRecCompOffset);
        const uint32_t* stamp = reinterpret_cast<const uint32_t*>(
            rec + patterns::kEffectRecStampOffset);
        float dcam = -1.0f;
        if (fc.valid) {
            const float dx = loc[0] - fc.writtenLocX;
            const float dy = loc[1] - fc.writtenLocY;
            const float dz = loc[2] - fc.writtenLocZ;
            dcam = sqrtf(dx * dx + dy * dy + dz * dz);
        }
        char cls[64] = "-";
        if (comp) reflect::class_name_of(comp, cls, sizeof cls);
        BVR_LOG("[bsi] fxtick:   #%02d rec=%p loc=(%.1f %.1f %.1f) dCam=%.1f rot=(%d %d "
                "%d) comp=%p (%s) stamp=%u/%u%s",
                i, rec, loc[0], loc[1], loc[2], dcam, rot[0], rot[1], rot[2], comp, cls,
                stamp[0], stamp[1],
                (stamp[0] == g0 && stamp[1] == g1) ? "  [CURRENT]" : "");
    }
}

} // namespace

bool wants_install() {
    return ((g_probe.load(std::memory_order_relaxed) ||
             g_substitute.load(std::memory_order_relaxed)) &&
            !g_installed.load(std::memory_order_relaxed)) ||
           fx_wants_install() || tick_wants_install();
}

bool try_install() {
    if (fx_wants_install()) fx_try_install();
    if (tick_wants_install()) tick_try_install();
    if (g_installed.load(std::memory_order_relaxed)) return true;
    if (!patterns::rva_trusted()) {
        BVR_LOG("[bsi] fxorigin: REFUSED - build gate closed");
        return false;
    }
    uint8_t* impl = const_cast<uint8_t*>(patterns::image_base()) +
                    patterns::kSkelCompUpdateAttachmentsRva;
    if (!bvr::pattern_scan::is_memory_valid(impl, 0x440)) {
        BVR_LOG("[bsi] fxorigin: REFUSED - impl rva 0x%X not readable",
                patterns::kSkelCompUpdateAttachmentsRva);
        return false;
    }
    // Gate 1: the pinned prologue (kGetPlayerViewPointPrologue discipline).
    if (memcmp(impl, patterns::kSkelCompUpdateAttachmentsPrologue,
               sizeof patterns::kSkelCompUpdateAttachmentsPrologue) != 0) {
        BVR_LOG("[bsi] fxorigin: REFUSED - prologue at rva 0x%X does not match the "
                "derivation (stale build?)",
                patterns::kSkelCompUpdateAttachmentsRva);
        return false;
    }
    // Gate 2: the class identity. The RVA must be what the live
    // XSkeletalMeshComponent vtable carries in the derived slot - a stronger
    // claim than bytes alone, and it breaks loudly on a relinked build.
    const uint8_t* vtable = patterns::image_base() + patterns::kSkelCompVtableRva;
    if (!bvr::pattern_scan::is_memory_valid(vtable, patterns::kSkelCompUpdateAttachmentsVtableSlot * 4 + 4)) {
        BVR_LOG("[bsi] fxorigin: REFUSED - vtable rva 0x%X not readable",
                patterns::kSkelCompVtableRva);
        return false;
    }
    const void* slotTarget = reinterpret_cast<const void* const*>(
        vtable)[patterns::kSkelCompUpdateAttachmentsVtableSlot];
    if (slotTarget != impl) {
        BVR_LOG("[bsi] fxorigin: REFUSED - vtable slot %u holds %p, expected rva 0x%X",
                patterns::kSkelCompUpdateAttachmentsVtableSlot, slotTarget,
                patterns::kSkelCompUpdateAttachmentsRva);
        return false;
    }
    // Gate 3: the arity, inverted form (fidget impl precedent) - the epilogue
    // must be a PLAIN `ret` (8B E5 5D C3) with no `ret imm16` before it.
    bool sawPlainRet = false;
    for (size_t i = 0; i + 4 < 0x440; ++i) {
        if (impl[i] == 0xC2 && impl[i + 2] == 0x00) {
            BVR_LOG("[bsi] fxorigin: REFUSED - unexpected `ret %u` at +0x%X (arity drift)",
                    impl[i + 1], static_cast<unsigned>(i));
            return false;
        }
        if (impl[i] == 0x8B && impl[i + 1] == 0xE5 && impl[i + 2] == 0x5D &&
            impl[i + 3] == 0xC3) {
            sawPlainRet = true;
            break;
        }
    }
    if (!sawPlainRet) {
        BVR_LOG("[bsi] fxorigin: REFUSED - no plain-ret epilogue in 0x440 bytes at rva 0x%X",
                patterns::kSkelCompUpdateAttachmentsRva);
        return false;
    }
    if (MH_CreateHook(impl, reinterpret_cast<void*>(&UpdateAttachmentsDetour),
                      reinterpret_cast<void**>(&g_original)) != MH_OK) {
        BVR_LOG("[bsi] fxorigin: MH_CreateHook failed at rva 0x%X",
                patterns::kSkelCompUpdateAttachmentsRva);
        return false;
    }
    if (MH_EnableHook(impl) != MH_OK) {
        BVR_LOG("[bsi] fxorigin: MH_EnableHook failed at rva 0x%X",
                patterns::kSkelCompUpdateAttachmentsRva);
        return false;
    }
    g_target = impl;
    g_installed.store(true, std::memory_order_relaxed);
    BVR_LOG("[bsi] fxorigin: attach-update seam hooked - XSkeletalMeshComponent attachment "
            "updater rva 0x%X (vtable slot %u). Mode: %s. Instrument + edge cover: repaints "
            "the composed atoms before the walk so the rare eval restamps never reach the "
            "attachments (the frozen-FX family reads elsewhere - see ENGINE_NOTES s50).",
            patterns::kSkelCompUpdateAttachmentsRva,
            patterns::kSkelCompUpdateAttachmentsVtableSlot,
            g_substitute.load(std::memory_order_relaxed) ? "REAPPLYING" : "PROBE (read-only)");
    return true;
}

bool handle_command(const char* cmd, const char* args) {
    if (strcmp(cmd, "bsifx") != 0) return false;
    if (!args) args = "";
    while (*args == ' ') ++args;

    // The effect-update seam: `bsifx u probe|on|off|dump <n>|status`.
    // (command.txt lines carry the trailing newline - it terminates a token.)
    if (args[0] == 'u' && (args[1] == ' ' || args[1] == '\0' || args[1] == '\n' ||
                           args[1] == '\r')) {
        const char* sub = args[1] ? args + 2 : "";
        while (*sub == ' ') ++sub;
        if (strncmp(sub, "probe", 5) == 0) {
            g_fxProbe.store(true, std::memory_order_relaxed);
            BVR_LOG("[bsi] fxupdate: PROBE armed - installs on the next camera tick; "
                    "hold a vigor charge and read the FP record lines");
        } else if (strncmp(sub, "on", 2) == 0) {
            g_fxProbe.store(true, std::memory_order_relaxed);
            g_fxSubstitute.store(true, std::memory_order_relaxed);
            BVR_LOG("[bsi] fxupdate: SUBSTITUTION ARMED - first-person effect records "
                    "(owner = the FP attachment or the pawn) now update at the hand "
                    "(lateral-sign attribution, %.0f UU refusal)",
                    kFxMaxDisplacementUu);
        } else if (strncmp(sub, "off", 3) == 0) {
            g_fxSubstitute.store(false, std::memory_order_relaxed);
            BVR_LOG("[bsi] fxupdate: substitution off (records keep the engine's "
                    "camera-anchored positions)");
        } else if (strncmp(sub, "dump", 4) == 0) {
            int n = 12;
            sscanf_s(sub + 4, "%d", &n);
            if (n < 1) n = 1;
            if (n > 200) n = 200;
            g_fxDumpLeft.store(n, std::memory_order_relaxed);
            BVR_LOG("[bsi] fxupdate: dumping the next %d record updates (ALL records, "
                    "fp-tagged - s51)",
                    n);
        } else if (strncmp(sub, "callers", 7) == 0) {
            // s51 fork (i): the runtime census (194 static call sites; which
            // ones actually fire, and how often, during a charge).
            uint32_t total = 0;
            for (int i = 0; i < kFxCallerSlots && g_fxCallers[i].rva; ++i) {
                BVR_LOG("[bsi] fxupdate: caller 0x%06X x%u", g_fxCallers[i].rva,
                        g_fxCallers[i].count);
                total += g_fxCallers[i].count;
            }
            BVR_LOG("[bsi] fxupdate: callers total %u, overflowed slots %u (of %d)",
                    total, g_fxCallerOverflow, kFxCallerSlots);
        } else {
            BVR_LOG("[bsi] fxupdate: installed=%d probe=%d write=%d | calls=%u fp=%u "
                    "subs=%u | bsifx u probe|on|off|dump <n>|status",
                    g_fxInstalled.load(std::memory_order_relaxed) ? 1 : 0,
                    g_fxProbe.load(std::memory_order_relaxed) ? 1 : 0,
                    g_fxSubstitute.load(std::memory_order_relaxed) ? 1 : 0,
                    g_fxCalls.load(std::memory_order_relaxed),
                    g_fxFpCalls.load(std::memory_order_relaxed),
                    g_fxSubs.load(std::memory_order_relaxed));
        }
        return true;
    }

    // s51 fork (ii): the tick-table lane: `bsifx t probe|dump|status`.
    if (args[0] == 't' && (args[1] == ' ' || args[1] == '\0' || args[1] == '\n' ||
                           args[1] == '\r')) {
        const char* sub = args[1] ? args + 2 : "";
        while (*sub == ' ') ++sub;
        if (strncmp(sub, "probe", 5) == 0) {
            g_tickProbe.store(true, std::memory_order_relaxed);
            BVR_LOG("[bsi] fxtick: PROBE armed - installs on the next camera tick "
                    "(captures the tickee; `bsifx t dump` walks the record table)");
        } else if (strncmp(sub, "dump", 4) == 0) {
            tick_dump_table();
        } else {
            BVR_LOG("[bsi] fxtick: installed=%d probe=%d tickee=%p calls=%u | bsifx t "
                    "probe|dump|status",
                    g_tickInstalled.load(std::memory_order_relaxed) ? 1 : 0,
                    g_tickProbe.load(std::memory_order_relaxed) ? 1 : 0,
                    g_tickee.load(std::memory_order_relaxed),
                    g_tickCalls.load(std::memory_order_relaxed));
        }
        return true;
    }

    if (strncmp(args, "probe on", 8) == 0) {
        g_probe.store(true, std::memory_order_relaxed);
        BVR_LOG("[bsi] fxorigin: PROBE armed - installs on the next camera tick, read-only");
    } else if (strncmp(args, "probe off", 9) == 0) {
        g_probe.store(false, std::memory_order_relaxed);
        BVR_LOG("[bsi] fxorigin: probe off (an installed hook stays installed and passes "
                "through)");
    } else if (strncmp(args, "on", 2) == 0) {
        g_probe.store(true, std::memory_order_relaxed);
        g_substitute.store(true, std::memory_order_relaxed);
        BVR_LOG("[bsi] fxorigin: pre-walk reapply ARMED (edge cover for eval restamps; "
                "NOT the frozen-FX family fix)");
    } else if (strncmp(args, "off", 3) == 0) {
        g_substitute.store(false, std::memory_order_relaxed);
        BVR_LOG("[bsi] fxorigin: reapply off (the rare eval-restamp ticks reach the "
                "attachments again)");
    } else {
        BVR_LOG("[bsi] fxorigin: installed=%d probe=%d write=%d | calls=%u player=%u "
                "reapplied=%u | bsifx probe on|off | on|off | status",
                g_installed.load(std::memory_order_relaxed) ? 1 : 0,
                g_probe.load(std::memory_order_relaxed) ? 1 : 0,
                g_substitute.load(std::memory_order_relaxed) ? 1 : 0,
                g_calls.load(std::memory_order_relaxed),
                g_playerCalls.load(std::memory_order_relaxed),
                g_reapplies.load(std::memory_order_relaxed));
    }
    return true;
}

void draw_debug_ui() {
    if (!ImGui::CollapsingHeader("ATTACH UPDATE (s50) - instrument + eval-restamp cover"))
        return;
    bool sub = g_substitute.load(std::memory_order_relaxed);
    if (ImGui::Checkbox("Pre-walk reapply (covers rare eval restamps)", &sub)) {
        if (sub) g_probe.store(true, std::memory_order_relaxed);
        g_substitute.store(sub, std::memory_order_relaxed);
    }
    ImGui::Text("hook %s   calls %u   player %u   reapplied %u",
                g_installed.load(std::memory_order_relaxed) ? "LIVE" : "not installed",
                g_calls.load(std::memory_order_relaxed),
                g_playerCalls.load(std::memory_order_relaxed),
                g_reapplies.load(std::memory_order_relaxed));
    ImGui::TextDisabled("bsifx probe on|off | on|off | status");
}

} // namespace bvr::bsi::fxorigin
