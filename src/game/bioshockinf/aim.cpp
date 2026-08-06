#include "game/bioshockinf/aim.h"

#include "core/hooks/pattern_scan.h"
#include "core/input/xinput_bridge.h"
#include "core/util/log.h"
#include "core/vr/openxr_runtime.h"
#include "game/bioshockinf/camera.h"
#include "game/bioshockinf/frame_context.h"
#include "game/bioshockinf/inf_math.h"
#include "game/bioshockinf/patterns.h"

#include <MinHook.h>
#include <imgui.h>
#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstring>

namespace bvr::bsi::aim {
namespace {

// The seam's signature. ONE stack arg (the hidden return buffer for the
// FRotator), which is `ret 4` in the body - and the arg count MUST equal
// ret imm / 4 or the result is a Run-Time Check Failure #0 dialog that writes
// no crash dump. The install below refuses unless it can SEE that `C2 04 00`.
using GetBaseAimRotationFn = FRotator*(__fastcall*)(void* self, void* edx, FRotator* out);

GetBaseAimRotationFn g_original = nullptr;
void* g_target = nullptr;

// Log addresses as RVAs, never as VAs: an RVA is what ENGINE_NOTES records and
// what the offline tools take.
uint32_t to_rva(const void* p) {
    const uint8_t* base = patterns::image_base();
    return (base && p) ? static_cast<uint32_t>(static_cast<const uint8_t*>(p) - base) : 0;
}

// HEADSET-VERIFIED (user, 2026-08-06): "aiming is not influenced by the head -
// the bullet kept going in the same direction as my controller". So the seam IS
// the fire path and the write SHIPS ARMED. The flat lane's null result was a
// false negative of the instrument (a window capture cannot show an impact in
// that scene); the probe/write split stays because it is how the next seam
// question gets answered without disturbing what it measures.
std::atomic<bool> g_probe{true};       // install + observe
std::atomic<bool> g_substitute{true};  // actually write the out-param
std::atomic<bool> g_installed{false};
std::atomic<uint32_t> g_calls{0};
std::atomic<uint32_t> g_subs{0};
std::atomic<int> g_dumpLeft{0};

// Telemetry, published for the heartbeat / F10 / the flat acceptance. All are
// written on the game thread and read anywhere, so plain atomics.
std::atomic<float> g_lastEngineYawDeg{0.0f};
std::atomic<float> g_lastEnginePitchDeg{0.0f};
std::atomic<float> g_lastRayYawDeg{0.0f};
std::atomic<float> g_lastRayPitchDeg{0.0f};
// THE NUMBER THIS LANE EXISTS TO MOVE: how far the engine's own aim is from
// where the controller points. Read it BEFORE arming the write - a large value
// with the write off is the defect, and it collapsing to ~0 with the write on
// is the proof.
std::atomic<float> g_lastDivergenceDeg{0.0f};

// Which hand the seam last carried (0 left / 1 right), latched from the
// triggers. Right is the sane default: the weapon hand is what fires first in
// every scene that has a weapon at all.
std::atomic<int> g_lastHand{1};

// The aim DOT and the LASER, per hand. Dots default ON (user's call, session
// 44: "the dot should be on by default for now"); the lasers default off - two
// full beams are a lot of the view to fill before anyone has asked for them,
// and they are one checkbox away.
std::atomic<bool> g_dot[2] = {{true}, {true}};   // [0] left, [1] right
std::atomic<bool> g_laser[2] = {{false}, {false}};
std::atomic<float> g_dotDistM{3.0f};
std::atomic<float> g_dotSizeDeg{0.5f};

// Per-hand AIM trims and ray-origin offsets, [0] left / [1] right. BS2's shape
// (aim.h trim_pitch/trim_yaw/set_trim, pos_fwd_cm/set_pos), numbers derived
// fresh: every default is ZERO. BS2 bakes in its own headset calibration and
// copying those figures would be exactly the cross-game number transfer the
// project rules forbid - Infinite starts at zero and bakes its own.
//
// The trims ride BOTH the game-side ray (ray_pose_from_xr) and core's
// LaserConfig, from these same atomics, so the beam and the bullet cannot carry
// different ones. NO roll trim: roll is innermost in xr_local_trim_quat, so it
// could not steer a ray even if it existed.
std::atomic<float> g_aimTrimPitch[2] = {{0.0f}, {0.0f}};
std::atomic<float> g_aimTrimYaw[2] = {{0.0f}, {0.0f}};
std::atomic<float> g_aimPosFwdCm[2] = {{0.0f}, {0.0f}};
std::atomic<float> g_aimPosRightCm[2] = {{0.0f}, {0.0f}};
std::atomic<float> g_aimPosUpCm[2] = {{0.0f}, {0.0f}};

bool any_trim_set() {
    for (int h = 0; h < 2; ++h)
        if (g_aimTrimPitch[h].load(std::memory_order_relaxed) != 0.0f ||
            g_aimTrimYaw[h].load(std::memory_order_relaxed) != 0.0f)
            return true;
    return false;
}

uint64_t g_lastLogMs = 0;

// The controller ray, in GAME rotation units, built on exactly the basis the
// view drive uses: yaw is the game's own yaw plus the residual measured off the
// recenter, pitch and roll absolute from the controller. Using the view's basis
// is the point - it is what makes "where I look" and "where I shoot" the same
// coordinate system rather than two parallel derivations that drift.
//
// hand: 0 left (vigor), 1 right (weapon). Also hands back the XR-space aim
// pose, because the laser and the dot both need the ray's ORIGIN and there is
// no point locating the hand twice in one frame.
// THE LEGACY FORMULA, kept VERBATIM as the shadow the refactor is measured
// against. It is what the user accepted in the headset and what the banked
// aimRayMaxDevDeg 0.0000 was measured on, so it stays the substituted source
// until a live selfcheck reads max|d| = 0 - at which point the follow-up commit
// flips the default and deletes this. That makes the refactor's risk a
// MEASUREMENT rather than a review.
bool ray_legacy(const bvr::vr::HeadPose& hp, FRotator* out) {
    int32_t gameYawUnits = 0, recenterYawUnits = 0;
    if (!camera::aim_basis(&gameYawUnits, &recenterYawUnits)) return false;

    const UeAngles a = ue_angles_from_xr_quat(hp.qx, hp.qy, hp.qz, hp.qw);
    const int32_t handYawUnits = static_cast<int32_t>(lroundf(a.yawRad * kRotUnitsPerRadian));
    const int32_t residual = wrap_rot(handYawUnits - recenterYawUnits);
    out->pitch = static_cast<int32_t>(a.pitchRad * kRotUnitsPerRadian);
    out->yaw = gameYawUnits + residual;
    // Roll is deliberately left at zero: an aim ROTATOR's roll does not steer a
    // trace, and a rolled controller must not tilt anything downstream that
    // reads this rotation for a basis.
    out->roll = 0;
    return true;
}

// --- selfcheck: prove the frame-context chain is INERT -----------------------
// The value of the refactor commit is that nothing moved, so it ships with the
// instrument that says so. Deltas are recorded as INTEGERS and never as floats:
// an FRotator's int32s reinterpret as denormals and print as 0.000 (the trap
// recorded twice already in this tree), and even a CORRECT
// (float)d / kRotUnitsPerDegree at %.1f prints a one-unit delta as 0.0 - which
// is exactly the failure this instrument exists to see. A rotator unit is
// 0.0055 deg. ZERO is the pass; 1 is a defect, not a rounding.
// DEFAULT: the frame-context chain, flipped from `legacy` in the same session
// that measured it. 1240 dispatches across 14 stations - three roll values, four
// yaws and a pitch pair - read max|dPitch| = max|dYaw| = max|dRoll| = 0 with the
// position round trip at 0.0000 mm, and aimRayMaxDevDegL/R stayed exactly
// 0.0000 with the flip live. `bsiaim source legacy` is the escape hatch, and
// ray_legacy stays as the shadow the selfcheck measures against - deleting it
// would delete the instrument that proves this.
std::atomic<bool> g_raySourceFrame{true};
std::atomic<int32_t> g_scRemaining{0};
int32_t g_scSamples = 0, g_scPerHand[2] = {0, 0}, g_scMismatches = 0;
int32_t g_scMaxDPitch = 0, g_scMaxDYaw = 0, g_scMaxDRoll = 0;
float g_scMaxRoundTripMm = 0.0f;
bool g_scFirstLatched = false;
int g_scFirstHand = 0;
FRotator g_scFirstLegacy{}, g_scFirstFresh{};

void selfcheck_sample(int hand, const FRotator& legacy, const FRotator& fresh,
                      const FrameContext& ctx, const bvr::vr::HeadPose& hp) {
    if (g_scRemaining.load(std::memory_order_relaxed) <= 0) return;
    g_scRemaining.fetch_sub(1, std::memory_order_relaxed);

    const int32_t dPitch = fresh.pitch - legacy.pitch; // RAW: pitch is absolute,
                                                       // a wrap would hide a
                                                       // 65536-unit error
    const int32_t dYaw = wrap_rot(fresh.yaw - legacy.yaw); // yaw IS modular
    const int32_t dRoll = fresh.roll - legacy.roll;
    const int32_t aP = dPitch < 0 ? -dPitch : dPitch;
    const int32_t aY = dYaw < 0 ? -dYaw : dYaw;
    const int32_t aR = dRoll < 0 ? -dRoll : dRoll;
    if (aP > g_scMaxDPitch) g_scMaxDPitch = aP;
    if (aY > g_scMaxDYaw) g_scMaxDYaw = aY;
    if (aR > g_scMaxDRoll) g_scMaxDRoll = aR;
    ++g_scSamples;
    g_scPerHand[hand & 1]++;
    if (aP || aY || aR) {
        ++g_scMismatches;
        if (!g_scFirstLatched) {
            g_scFirstLatched = true;
            g_scFirstHand = hand;
            g_scFirstLegacy = legacy;
            g_scFirstFresh = fresh;
            BVR_LOG("[bsi] aim selfcheck: MISMATCH #1 hand=%s legacy=(%d %d %d) "
                    "frame=(%d %d %d) d=(%d %d %d) units",
                    hand ? "R" : "L", legacy.pitch, legacy.yaw, legacy.roll, fresh.pitch,
                    fresh.yaw, fresh.roll, dPitch, dYaw, dRoll);
        }
    }

    // The POSITION round trip, on the same sample: game_point_to_xr must undo
    // xr_pose_to_game. Not bit-exact (a rotation and its transpose, a scale and
    // its reciprocal) - expect well under 0.01 mm. A sign error in ue_to_xr
    // shows up here as METRES.
    const float pos[3] = {hp.px, hp.py, hp.pz};
    const float quat[4] = {hp.qx, hp.qy, hp.qz, hp.qw};
    const GamePose gp = xr_pose_to_game(ctx, pos, quat);
    float back[3];
    game_point_to_xr(ctx, gp.loc, back);
    for (int i = 0; i < 3; ++i) {
        const float e = fabsf(back[i] - pos[i]) * 1000.0f;
        if (e > g_scMaxRoundTripMm) g_scMaxRoundTripMm = e;
    }

    if (g_scRemaining.load(std::memory_order_relaxed) == 0) {
        BVR_LOG("[bsi] aim selfcheck: %d samples (L %d / R %d) | max|dPitch|=%d "
                "max|dYaw|=%d max|dRoll|=%d units | mismatches=%d | "
                "roundTripMax=%.4f mm",
                g_scSamples, g_scPerHand[0], g_scPerHand[1], g_scMaxDPitch, g_scMaxDYaw,
                g_scMaxDRoll, g_scMismatches, g_scMaxRoundTripMm);
        BVR_LOG("[bsi] aim selfcheck: a rotator unit is 0.0055 deg. ZERO is the pass; "
                "1 is a defect, not a rounding. Source in use: %s",
                g_raySourceFrame.load(std::memory_order_relaxed) ? "frame" : "legacy");
        uint32_t pub = 0, ret = 0, ref = 0, foreign = 0;
        frame_context::stats(&pub, &ret, &ref, &foreign);
        BVR_LOG("[bsi] aim selfcheck: seqlock publishes=%u retries=%u refusals=%u "
                "foreignWrites=%u",
                pub, ret, ref, foreign);
    }
}

// hand: 0 left (vigor), 1 right (weapon). Hands back the full game-space ray -
// ROTATION and ORIGIN - plus the frame context it was built from, because the
// dot has to be placed through the SAME context or the two disagree by whatever
// the camera did in between.
//
// Computes BOTH chains every call while the shadow is armed, so the selfcheck
// measures the production path rather than a re-derivation of it.
bool controller_ray(int hand, FRotator* out, bvr::vr::HeadPose* poseOut, GamePose* gpOut,
                    FrameContext* ctxOut) {
    bvr::vr::HeadPose hp{};
    if (!bvr::vr::get_hand_pose(hand, /*aimPose=*/true, hp)) return false;

    const float trimP = g_aimTrimPitch[hand & 1].load(std::memory_order_relaxed);
    const float trimY = g_aimTrimYaw[hand & 1].load(std::memory_order_relaxed);

    FRotator legacy{};
    // The legacy shadow carries no trims by construction - it predates them - so
    // it is only a valid comparison while the trims are zero. selfcheck refuses
    // to start otherwise, and says why.
    const bool haveLegacy = ray_legacy(hp, &legacy);

    FrameContext ctx{};
    GamePose fresh{};
    bool haveFresh = false;
    if (frame_context::read(&ctx)) {
        const float pos[3] = {hp.px, hp.py, hp.pz};
        const float quat[4] = {hp.qx, hp.qy, hp.qz, hp.qw};
        fresh = ray_pose_from_xr(ctx, pos, quat, trimP, trimY);
        // Ray ORIGIN offset, along the FINAL TRIMMED basis - the basis of the
        // rotation that already carries the trims. In the untrimmed basis the
        // offset would swing as the trim is tuned and the two knobs would stop
        // being independent, so neither could be calibrated.
        //
        // cm -> UU by worldScale/100. The formula is general in worldScale,
        // which is why it survives Vengeance's 100 -> UE3's 50 without a second
        // number. Core's LaserConfig takes the SAME cm and does the same thing
        // render-side.
        const float oF = g_aimPosFwdCm[hand & 1].load(std::memory_order_relaxed);
        const float oR = g_aimPosRightCm[hand & 1].load(std::memory_order_relaxed);
        const float oU = g_aimPosUpCm[hand & 1].load(std::memory_order_relaxed);
        if (oF != 0.0f || oR != 0.0f || oU != 0.0f) {
            float fwd[3], right[3], up[3];
            ue_rot_basis(fresh.rot, fwd, right, up);
            const float k = ctx.worldScale / 100.0f;
            fresh.loc.x += (fwd[0] * oF + right[0] * oR + up[0] * oU) * k;
            fresh.loc.y += (fwd[1] * oF + right[1] * oR + up[1] * oU) * k;
            fresh.loc.z += (fwd[2] * oF + right[2] * oR + up[2] * oU) * k;
        }
        haveFresh = true;
    }

    if (haveLegacy && haveFresh) selfcheck_sample(hand, legacy, fresh.rot, ctx, hp);

    const bool useFrame = g_raySourceFrame.load(std::memory_order_relaxed);
    if (useFrame) {
        if (!haveFresh) return false;
        *out = fresh.rot;
    } else {
        if (!haveLegacy) return false;
        *out = legacy;
    }
    if (poseOut) *poseOut = hp;
    if (gpOut) *gpOut = fresh;
    if (ctxOut) *ctxOut = ctx;
    return haveFresh || !useFrame;
}

// WHICH HAND is aiming. The seam is pawn-level and hands back ONE rotation, so
// something has to decide whose ray it carries. We compose the pad ourselves,
// so "which trigger is the player pulling" is information the mod already owns
// - the same attribution BioShock 1 used, in shape. Latched rather than
// momentary: a shot's trace can run a frame or two after the trigger is
// released, and flipping the aim back mid-shot would throw it.
int aiming_hand() {
    uint8_t lt = 0, rt = 0;
    bvr::input::last_composed_triggers(&lt, &rt);
    constexpr uint8_t kPull = 64; // quarter pull, same gate the pad log uses
    if (rt >= kPull) g_lastHand.store(1, std::memory_order_relaxed);
    else if (lt >= kPull) g_lastHand.store(0, std::memory_order_relaxed);
    return g_lastHand.load(std::memory_order_relaxed);
}

// Publish the aim DOT for one hand: the substituted ray mapped BACK into XR
// space and placed a fixed distance along itself from the controller.
//
// The round trip is the point. The dot could trivially be drawn along the
// controller's own XR forward and would then always look perfect while proving
// nothing. Instead it takes the FRotator the seam actually wrote, undoes the
// game-yaw basis (xrYaw = rayYaw - gameYaw + recenterYaw), rebuilds a direction
// and converts it - so a basis error shows up as a dot that does NOT sit on the
// controller's forward, which is exactly the failure worth seeing.
// TWO different inverses, both needed. The ROTATION inverse undoes the game-yaw
// basis to recover the direction; the POSITION inverse (game_point_to_xr) maps
// the ray's game-space ORIGIN back into XR. Before the origin offset existed the
// second was not needed - the raw controller position WAS the origin - but a dot
// anchored at the controller while the ray leaves from an offset muzzle would
// make the dot lie by exactly that offset.
//
// Both run through the SAME ctx the ray was built from, passed in rather than
// re-read, so the dot cannot pick up a context the ray never saw.
void publish_dot(int slot, int hand, const GamePose& ray, const FrameContext& ctx,
                 bool enabled) {
    bvr::vr::AimDotConfig dc{};
    dc.enabled = enabled;
    dc.sizeDeg = g_dotSizeDeg.load(std::memory_order_relaxed);
    if (enabled) {
        FRotator xrRot{};
        xrRot.pitch = ray.rot.pitch;
        // The exact integer cancellation the frame context exists for: this
        // recovers the hand's own yaw with the game yaw AND the recenter both
        // gone, with no rounding at any magnitude of game yaw.
        xrRot.yaw = wrap_rot(ray.rot.yaw - ctx.gameYawUnits + ctx.recenterYawUnits);
        xrRot.roll = 0;
        float fwd[3], right[3], up[3];
        ue_rot_basis(xrRot, fwd, right, up);
        float dirXr[3];
        ue_to_xr(fwd, dirXr);
        float originXr[3];
        game_point_to_xr(ctx, ray.loc, originXr);
        const float d = g_dotDistM.load(std::memory_order_relaxed);
        dc.posXr[0] = originXr[0] + dirXr[0] * d;
        dc.posXr[1] = originXr[1] + dirXr[1] * d;
        dc.posXr[2] = originXr[2] + dirXr[2] * d;
        dc.valid = true;
    }
    bvr::vr::set_aim_dot_slot(slot, dc);
}

// Publish the laser for one hand. The beam is re-derived render-side from the
// controller pose, so it is fresher than the dot but a parallel computation;
// the two agreeing is itself the calibration (core's own note on the pair).
void publish_laser(int slot, int hand, bool enabled) {
    bvr::vr::LaserConfig lc{};
    lc.enabled = enabled;
    lc.hand = hand;
    lc.dots = 6;
    lc.nearM = 0.30f;
    lc.farM = 6.0f;
    lc.sizeDeg = 0.7f;
    // The trims and the origin offset come from the SAME atomics the game-side
    // ray reads, in the same units, so the beam and the bullet cannot carry
    // different ones. Core composes its trim with the same
    // bvr::xrmath::xr_local_trim_quat in the same order, which is why this works
    // without a core change - and why it must never be re-derived here.
    lc.pitchTrimDeg = g_aimTrimPitch[hand & 1].load(std::memory_order_relaxed);
    lc.yawTrimDeg = g_aimTrimYaw[hand & 1].load(std::memory_order_relaxed);
    lc.posFwdCm = g_aimPosFwdCm[hand & 1].load(std::memory_order_relaxed);
    lc.posRightCm = g_aimPosRightCm[hand & 1].load(std::memory_order_relaxed);
    lc.posUpCm = g_aimPosUpCm[hand & 1].load(std::memory_order_relaxed);
    // muzzle stays OFF: the FP attachment's barrel axis is not derived, so
    // muzzleD0 and the model*TrimDeg fields keep their defaults. Recorded so
    // nobody wires the muzzle ray half-way.
    bvr::vr::set_laser_slot(slot, lc);
}

// The seam. Original FIRST, always - this hook observes and adjusts, it never
// replaces. Everything here is POD and branch-only: no allocation, no locking,
// and logging is rate-limited and outside any guarded region.
FRotator* __fastcall AimDetour(void* self, void* edx, FRotator* outBuf) {
    FRotator* r = g_original(self, edx, outBuf);
    g_calls.fetch_add(1, std::memory_order_relaxed);
    if (!r) return r;

    // Snapshot what the ENGINE thinks before touching anything, so the probe
    // keeps measuring the original even while the write is armed.
    const FRotator engine = *r;

    // Both hands, every call. The seam only ever carries ONE of them (the hand
    // whose trigger is pulled), but the laser and the dot want both - the vigor
    // hand needs to show where it will cast even while the weapon hand is the
    // one aiming.
    const int hand = aiming_hand();
    FRotator rays[2]{};
    bvr::vr::HeadPose poses[2]{};
    GamePose gps[2]{};
    FrameContext ctxs[2]{};
    bool have[2] = {false, false};
    for (int h = 0; h < 2; ++h)
        have[h] = controller_ray(h, &rays[h], &poses[h], &gps[h], &ctxs[h]);

    // Slot 0 is the RIGHT hand and slot 1 the left, matching BS2's convention
    // (slot 0 keeps BS1 parity there); a hand whose pose is missing publishes
    // disabled rather than stale.
    publish_dot(0, 1, gps[1], ctxs[1], have[1] && g_dot[1].load(std::memory_order_relaxed));
    publish_dot(1, 0, gps[0], ctxs[0], have[0] && g_dot[0].load(std::memory_order_relaxed));
    publish_laser(0, 1, have[1] && g_laser[1].load(std::memory_order_relaxed));
    publish_laser(1, 0, have[0] && g_laser[0].load(std::memory_order_relaxed));

    const FRotator ray = rays[hand];
    const bool haveRay = have[hand];

    if (haveRay) {
        g_lastEngineYawDeg.store(static_cast<float>(engine.yaw) / kRotUnitsPerDegree,
                                 std::memory_order_relaxed);
        g_lastEnginePitchDeg.store(static_cast<float>(engine.pitch) / kRotUnitsPerDegree,
                                   std::memory_order_relaxed);
        g_lastRayYawDeg.store(static_cast<float>(ray.yaw) / kRotUnitsPerDegree,
                              std::memory_order_relaxed);
        g_lastRayPitchDeg.store(static_cast<float>(ray.pitch) / kRotUnitsPerDegree,
                                std::memory_order_relaxed);
        const float dYaw =
            static_cast<float>(wrap_rot(ray.yaw - engine.yaw)) / kRotUnitsPerDegree;
        const float dPitch =
            static_cast<float>(wrap_rot(ray.pitch - engine.pitch)) / kRotUnitsPerDegree;
        g_lastDivergenceDeg.store(sqrtf(dYaw * dYaw + dPitch * dPitch),
                                  std::memory_order_relaxed);

        if (g_substitute.load(std::memory_order_relaxed)) {
            *r = ray;
            g_subs.fetch_add(1, std::memory_order_relaxed);
        }
    }

    int dump = g_dumpLeft.load(std::memory_order_relaxed);
    const uint64_t now = GetTickCount64();
    if (dump > 0 || now - g_lastLogMs >= 1000) {
        if (dump > 0) g_dumpLeft.fetch_sub(1, std::memory_order_relaxed);
        else g_lastLogMs = now;
        // Rotator components as %d ALWAYS, with degrees alongside: an FRotator's
        // int32s reinterpret as denormal floats and print as 0.000, which cost
        // BioShock 1 a long detour.
        BVR_LOG("[bsi] aim: hand=%s engine=(%d %d %d)=(%.1f %.1f)deg ray=(%d %d %d)=(%.1f "
                "%.1f)deg divergence=%.1f deg | ray %s, write %s (%u calls, %u substituted)",
                hand ? "R" : "L", engine.pitch, engine.yaw, engine.roll,
                static_cast<float>(engine.pitch) / kRotUnitsPerDegree,
                static_cast<float>(engine.yaw) / kRotUnitsPerDegree, ray.pitch, ray.yaw,
                ray.roll, static_cast<float>(ray.pitch) / kRotUnitsPerDegree,
                static_cast<float>(ray.yaw) / kRotUnitsPerDegree,
                g_lastDivergenceDeg.load(std::memory_order_relaxed),
                haveRay ? "live" : "UNAVAILABLE (drive off, no recenter, or no hand pose)",
                g_substitute.load(std::memory_order_relaxed) ? "ARMED" : "off",
                g_calls.load(std::memory_order_relaxed),
                g_subs.load(std::memory_order_relaxed));
    }
    return r;
}

} // namespace

bool wants_install() {
    return (g_probe.load(std::memory_order_relaxed) ||
            g_substitute.load(std::memory_order_relaxed)) &&
           !g_installed.load(std::memory_order_relaxed);
}

bool try_install() {
    if (g_installed.load(std::memory_order_relaxed)) return true;
    if (!patterns::rva_trusted()) {
        BVR_LOG("[bsi] aim: REFUSED - build gate closed");
        return false;
    }
    // The implementation is a VIRTUAL, so it is read off the live pawn rather
    // than from a static RVA. The pawn comes from the latched PC's field the
    // grant lane already named, not from a scan.
    void* pc = camera::last_player_controller();
    if (!pc || !bvr::pattern_scan::is_memory_valid(pc, patterns::kPcPawnOffset + 4)) {
        BVR_LOG("[bsi] aim: no readable player controller yet - the camera hook must fire "
                "first");
        return false;
    }
    void* pawn = *reinterpret_cast<void* const*>(static_cast<const uint8_t*>(pc) +
                                                 patterns::kPcPawnOffset);
    if (!pawn || !bvr::pattern_scan::is_memory_valid(pawn, 4)) {
        BVR_LOG("[bsi] aim: no pawn at PC+0x%X yet (still loading, or a pawnless state) - "
                "this is a wait, not a failure",
                patterns::kPcPawnOffset);
        return false;
    }
    const uint8_t* const* vt = *reinterpret_cast<const uint8_t* const* const*>(pawn);
    const uint32_t slot = patterns::kPawnGetBaseAimRotationVtblOffset;
    if (!vt || !bvr::pattern_scan::is_memory_valid(vt, slot + sizeof(void*))) {
        BVR_LOG("[bsi] aim: REFUSED - pawn vtable %p not readable through +0x%X", (void*)vt,
                slot);
        return false;
    }
    void* impl = const_cast<uint8_t*>(vt[slot / sizeof(void*)]);
    if (!impl || !bvr::pattern_scan::is_memory_valid(impl, 0x40)) {
        BVR_LOG("[bsi] aim: REFUSED - vtable slot +0x%X is not readable code", slot);
        return false;
    }

    // THE ARITY GATE, and it is load-bearing. `ret imm / 4` must equal the
    // detour's stack-arg count; a mismatch pops Run-Time Check Failure #0 and
    // writes NO crash dump. Refuse rather than risk it - the same check the
    // camera hook makes, which is the stricter of the two patterns in the tree.
    const uint8_t* body = static_cast<const uint8_t*>(impl);
    bool sawRet = false;
    for (size_t i = 0; i + 2 < 0x200; ++i) {
        if (!bvr::pattern_scan::is_memory_valid(body + i, 3)) break;
        if (body[i] == 0xC2 && body[i + 1] == patterns::kPawnGetBaseAimRotationRetImm &&
            body[i + 2] == 0x00) {
            sawRet = true;
            break;
        }
    }
    if (!sawRet) {
        BVR_LOG("[bsi] aim: REFUSED - no `ret %u` found in the first 0x200 bytes at rva 0x%X. "
                "The arg count must equal ret imm/4 or the RTC dialog (which writes no dump) "
                "is the result; the derivation is stale, do not hook it.",
                patterns::kPawnGetBaseAimRotationRetImm, to_rva(impl));
        return false;
    }

    if (MH_CreateHook(impl, reinterpret_cast<void*>(&AimDetour),
                      reinterpret_cast<void**>(&g_original)) != MH_OK) {
        BVR_LOG("[bsi] aim: MH_CreateHook failed at rva 0x%X", to_rva(impl));
        return false;
    }
    if (MH_EnableHook(impl) != MH_OK) {
        BVR_LOG("[bsi] aim: MH_EnableHook failed at rva 0x%X", to_rva(impl));
        return false;
    }
    g_target = impl;
    g_installed.store(true, std::memory_order_relaxed);
    BVR_LOG("[bsi] aim: seam hooked - APawn::GetBaseAimRotation impl rva 0x%X, read off the "
            "LIVE pawn %p vtable slot +0x%X (a virtual, so no static RVA is needed). Mode: "
            "%s. This is where the fire direction comes from: with a controller the stock "
            "body delegates to the CONTROLLER's rotation, which is why the shot follows the "
            "BODY and not the head today.",
            to_rva(impl), pawn, slot,
            g_substitute.load(std::memory_order_relaxed) ? "SUBSTITUTING" : "PROBE (read-only)");
    return true;
}

bool handle_command(const char* cmd, const char* args) {
    if (strcmp(cmd, "bsiaim") != 0) return false;
    if (!args) args = "";
    while (*args == ' ') ++args;

    if (strncmp(args, "probe on", 8) == 0) {
        g_probe.store(true, std::memory_order_relaxed);
        BVR_LOG("[bsi] aim: PROBE armed - the seam installs on the next camera tick and only "
                "OBSERVES. It refuses to substitute, so this diagnostic cannot change what "
                "it measures. Read `divergence` - that is the angle between the engine's own "
                "aim and where the controller points.");
    } else if (strncmp(args, "probe off", 9) == 0) {
        g_probe.store(false, std::memory_order_relaxed);
        BVR_LOG("[bsi] aim: probe off (an installed hook stays installed and passes through - "
                "a thread may still be returning through the trampoline)");
    } else if (strncmp(args, "laser", 5) == 0 || strncmp(args, "dot", 3) == 0) {
        // "<laser|dot> [l|r|both] on|off" - default both hands.
        const bool isLaser = args[0] == 'l' && args[1] == 'a';
        const char* rest = args + (isLaser ? 5 : 3);
        while (*rest == ' ') ++rest;
        int lo = 0, hi = 1;
        if (*rest == 'l' && rest[1] != 'e') { hi = 0; ++rest; }
        else if (*rest == 'r') { lo = 1; ++rest; }
        else if (strncmp(rest, "both", 4) == 0) rest += 4;
        while (*rest == ' ') ++rest;
        const bool on = strncmp(rest, "on", 2) == 0;
        for (int h = lo; h <= hi; ++h)
            (isLaser ? g_laser : g_dot)[h].store(on, std::memory_order_relaxed);
        BVR_LOG("[bsi] aim: %s %s for %s | dots L=%d R=%d lasers L=%d R=%d",
                isLaser ? "laser" : "dot", on ? "ON" : "off",
                lo == hi ? (lo ? "the RIGHT hand" : "the LEFT hand") : "BOTH hands",
                g_dot[0].load(std::memory_order_relaxed) ? 1 : 0,
                g_dot[1].load(std::memory_order_relaxed) ? 1 : 0,
                g_laser[0].load(std::memory_order_relaxed) ? 1 : 0,
                g_laser[1].load(std::memory_order_relaxed) ? 1 : 0);
    } else if (strncmp(args, "dotdist", 7) == 0) {
        float v = 0.0f;
        if (sscanf_s(args + 7, "%f", &v) == 1 && v >= 0.3f && v <= 30.0f)
            g_dotDistM.store(v, std::memory_order_relaxed);
        BVR_LOG("[bsi] aim: dot distance %.2f m (bsiaim dotdist <0.3..30>)",
                g_dotDistM.load(std::memory_order_relaxed));
    } else if (strncmp(args, "dump", 4) == 0) {
        int n = 8;
        sscanf_s(args + 4, "%d", &n);
        if (n < 1) n = 1;
        if (n > 64) n = 64;
        g_dumpLeft.store(n, std::memory_order_relaxed);
        BVR_LOG("[bsi] aim: dumping the next %d calls in full", n);
    } else if (strncmp(args, "selfcheck", 9) == 0) {
        // REFUSE while a trim is set. The legacy shadow predates the trims and
        // carries none, so with one set the two paths SHOULD differ - reporting
        // that as a failure would be reporting a configuration.
        if (any_trim_set()) {
            BVR_LOG("[bsi] aim selfcheck: REFUSED - an aim trim is nonzero (L %.2f/%.2f "
                    "R %.2f/%.2f). The legacy shadow carries no trims, so the two paths "
                    "are SUPPOSED to differ. Zero the trims and re-run.",
                    g_aimTrimPitch[0].load(std::memory_order_relaxed),
                    g_aimTrimYaw[0].load(std::memory_order_relaxed),
                    g_aimTrimPitch[1].load(std::memory_order_relaxed),
                    g_aimTrimYaw[1].load(std::memory_order_relaxed));
            return true;
        }
        int n = 240; // ~12 s at the observed ~20 dispatches/s
        sscanf_s(args + 9, "%d", &n);
        if (n < 1) n = 1;
        if (n > 20000) n = 20000;
        g_scSamples = g_scMismatches = 0;
        g_scPerHand[0] = g_scPerHand[1] = 0;
        g_scMaxDPitch = g_scMaxDYaw = g_scMaxDRoll = 0;
        g_scMaxRoundTripMm = 0.0f;
        g_scFirstLatched = false;
        g_scRemaining.store(n, std::memory_order_relaxed);
        BVR_LOG("[bsi] aim selfcheck: shadowing the next %d dispatches - the legacy "
                "formula against the frame-context chain. Deltas are INTEGER rotator "
                "units; zero is the pass.",
                n);
    } else if (strncmp(args, "trim", 4) == 0) {
        // trim l|r <pitch> <yaw>
        char side[8] = {};
        float p = 0.0f, y = 0.0f;
        if (sscanf_s(args, "trim %7s %f %f", side, (unsigned)sizeof side, &p, &y) == 3) {
            const int h = (side[0] == 'l' || side[0] == 'L') ? 0 : 1;
            set_trim(h, p, y);
            BVR_LOG("[bsi] aim: trim %s pitch %.2f yaw %.2f deg (rides the ray AND the "
                    "laser, from one pair of atomics)",
                    h ? "r" : "l", p, y);
        } else {
            BVR_LOG("[bsi] aim: trim l|r <pitch> <yaw> (deg). No roll: roll is innermost "
                    "in the trim compose, so it cannot steer a ray.");
        }
    } else if (strncmp(args, "origin", 6) == 0) {
        // origin l|r <fwd> <right> <up>, in cm along the FINAL TRIMMED basis
        char side[8] = {};
        float f = 0.0f, r2 = 0.0f, u = 0.0f;
        if (sscanf_s(args, "origin %7s %f %f %f", side, (unsigned)sizeof side, &f, &r2,
                     &u) == 4) {
            const int h = (side[0] == 'l' || side[0] == 'L') ? 0 : 1;
            set_pos(h, f, r2, u);
            BVR_LOG("[bsi] aim: ray origin %s fwd %.1f right %.1f up %.1f cm - MOVES THE "
                    "BEAM AND THE DOT ONLY. The bullet still leaves the engine's own "
                    "start point until the GetWeaponStartTraceLocation seam lands (s47).",
                    h ? "r" : "l", f, r2, u);
        } else {
            BVR_LOG("[bsi] aim: origin l|r <fwd> <right> <up> (cm)");
        }
    } else if (strncmp(args, "source", 6) == 0) {
        const bool frame = strstr(args, "frame") != nullptr;
        g_raySourceFrame.store(frame, std::memory_order_relaxed);
        BVR_LOG("[bsi] aim: ray source = %s (%s)", frame ? "frame" : "legacy",
                frame ? "the frame-context chain - one algebra with the model"
                      : "the shipped formula, kept as the shadow until selfcheck reads 0");
    } else if (strncmp(args, "on", 2) == 0) {
        g_probe.store(true, std::memory_order_relaxed);
        g_substitute.store(true, std::memory_order_relaxed);
        BVR_LOG("[bsi] aim: SUBSTITUTION ARMED - the pawn's aim rotation now comes from the "
                "right controller. The engine's own rotation is never written; only this "
                "function's out-param is, so drive-off is a byte-identical passthrough.");
    } else if (strncmp(args, "off", 3) == 0) {
        g_substitute.store(false, std::memory_order_relaxed);
        BVR_LOG("[bsi] aim: substitution off - the engine's own aim rotation stands");
    } else {
        BVR_LOG("[bsi] aim: installed=%d probe=%d write=%d | calls=%u substituted=%u | "
                "engine=(%.1f yaw %.1f pitch) ray=(%.1f yaw %.1f pitch) divergence=%.1f deg "
                "| bsiaim probe on|off | on|off | dump <n> | status",
                g_installed.load(std::memory_order_relaxed) ? 1 : 0,
                g_probe.load(std::memory_order_relaxed) ? 1 : 0,
                g_substitute.load(std::memory_order_relaxed) ? 1 : 0,
                g_calls.load(std::memory_order_relaxed), g_subs.load(std::memory_order_relaxed),
                g_lastEngineYawDeg.load(std::memory_order_relaxed),
                g_lastEnginePitchDeg.load(std::memory_order_relaxed),
                g_lastRayYawDeg.load(std::memory_order_relaxed),
                g_lastRayPitchDeg.load(std::memory_order_relaxed),
                g_lastDivergenceDeg.load(std::memory_order_relaxed));
    }
    return true;
}

float trim_pitch(int h) { return g_aimTrimPitch[h & 1].load(std::memory_order_relaxed); }
float trim_yaw(int h) { return g_aimTrimYaw[h & 1].load(std::memory_order_relaxed); }

void set_trim(int h, float pitchDeg, float yawDeg) {
    h &= 1;
    g_aimTrimPitch[h].store(pitchDeg, std::memory_order_relaxed);
    g_aimTrimYaw[h].store(yawDeg, std::memory_order_relaxed);
}

float pos_fwd_cm(int h) { return g_aimPosFwdCm[h & 1].load(std::memory_order_relaxed); }
float pos_right_cm(int h) { return g_aimPosRightCm[h & 1].load(std::memory_order_relaxed); }
float pos_up_cm(int h) { return g_aimPosUpCm[h & 1].load(std::memory_order_relaxed); }

void set_pos(int h, float fwdCm, float rightCm, float upCm) {
    h &= 1;
    g_aimPosFwdCm[h].store(fwdCm, std::memory_order_relaxed);
    g_aimPosRightCm[h].store(rightCm, std::memory_order_relaxed);
    g_aimPosUpCm[h].store(upCm, std::memory_order_relaxed);
}

bool laser_hand(int h) { return g_laser[h & 1].load(std::memory_order_relaxed); }
void set_laser_hand(int h, bool on) { g_laser[h & 1].store(on, std::memory_order_relaxed); }
bool dot_hand(int h) { return g_dot[h & 1].load(std::memory_order_relaxed); }
void set_dot_hand(int h, bool on) { g_dot[h & 1].store(on, std::memory_order_relaxed); }
float dot_dist_m() { return g_dotDistM.load(std::memory_order_relaxed); }
void set_dot_dist_m(float m) { g_dotDistM.store(m, std::memory_order_relaxed); }

void draw_debug_ui() {
    if (!ImGui::CollapsingHeader("AIM (I7) - controller aims, head looks")) return;
    bool probe = g_probe.load(std::memory_order_relaxed);
    if (ImGui::Checkbox("Install the aim seam (probe, read-only)", &probe))
        g_probe.store(probe, std::memory_order_relaxed);
    bool sub = g_substitute.load(std::memory_order_relaxed);
    if (ImGui::Checkbox("Aim with the RIGHT CONTROLLER", &sub)) {
        if (sub) g_probe.store(true, std::memory_order_relaxed);
        g_substitute.store(sub, std::memory_order_relaxed);
    }
    ImGui::Text("hook %s   calls %u   substituted %u   aiming hand %s",
                g_installed.load(std::memory_order_relaxed) ? "LIVE" : "not installed",
                g_calls.load(std::memory_order_relaxed), g_subs.load(std::memory_order_relaxed),
                g_lastHand.load(std::memory_order_relaxed) ? "RIGHT (weapon)" : "LEFT (vigor)");

    // The overlays, per hand. Both are compositor quads, so they exist only in
    // the headset - a flat screenshot showing nothing here is not evidence they
    // are broken.
    ImGui::Separator();
    ImGui::TextDisabled("Aim dot (where the shot goes)");
    bool dotR = g_dot[1].load(std::memory_order_relaxed);
    if (ImGui::Checkbox("Dot: right hand (weapon)", &dotR))
        g_dot[1].store(dotR, std::memory_order_relaxed);
    bool dotL = g_dot[0].load(std::memory_order_relaxed);
    if (ImGui::Checkbox("Dot: left hand (vigor)", &dotL))
        g_dot[0].store(dotL, std::memory_order_relaxed);
    float dist = g_dotDistM.load(std::memory_order_relaxed);
    if (ImGui::SliderFloat("Dot distance (m)", &dist, 0.5f, 15.0f, "%.1f"))
        g_dotDistM.store(dist, std::memory_order_relaxed);

    ImGui::TextDisabled("Aim laser (the beam along the ray)");
    bool lasR = g_laser[1].load(std::memory_order_relaxed);
    if (ImGui::Checkbox("Laser: right hand (weapon)", &lasR))
        g_laser[1].store(lasR, std::memory_order_relaxed);
    bool lasL = g_laser[0].load(std::memory_order_relaxed);
    if (ImGui::Checkbox("Laser: left hand (vigor)", &lasL))
        g_laser[0].store(lasL, std::memory_order_relaxed);
    ImGui::Separator();
    // The number to watch in the headset: with the write off it is how wrong
    // the shot is; with it on it should sit at ~0.
    ImGui::Text("divergence %.1f deg  (engine yaw %.1f / ray yaw %.1f)",
                g_lastDivergenceDeg.load(std::memory_order_relaxed),
                g_lastEngineYawDeg.load(std::memory_order_relaxed),
                g_lastRayYawDeg.load(std::memory_order_relaxed));
    ImGui::TextDisabled("bsiaim probe on|off | on|off | dump <n> | status");
}

} // namespace bvr::bsi::aim
