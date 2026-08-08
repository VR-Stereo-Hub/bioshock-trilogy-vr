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

// I8 (s45b): per-hand aim trim (pitch/yaw ONLY - roll is innermost in the
// trim quat and cannot move a ray) and per-hand ray-origin offsets. Defaults
// 0 = byte-equivalent to the headset-verified s44 behaviour. The SAME trim is
// fed to core's laser (render side, same xr_local_trim_quat algebra) so beam
// and shot keep agreeing; the dot inherits it through the round trip of the
// WRITTEN rotator. Origin offsets move the dot/laser origin only - this
// game's fire seam substitutes rotation, not origin (GetBaseAimRotation), so
// there is no trace-origin write to shift; the headset hole-vs-dot verdict
// decides whether an origin seam is ever needed.
std::atomic<float> g_aimTrim[2][2] = {};   // [hand][0 pitch, 1 yaw] deg
std::atomic<float> g_aimPosCm[2][3] = {};  // [hand][fwd, right, up] cm

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
bool controller_ray(int hand, FRotator* out, bvr::vr::HeadPose* poseOut) {
    bvr::vr::HeadPose hp{};
    if (!bvr::vr::get_hand_pose(hand, /*aimPose=*/true, hp)) return false;
    const FrameContext& fc = camera::frame_context();
    if (!fc.valid) return false;

    // s45b: the shared PURE chain (frame_context.h) - the identical compose
    // the model drive uses, roll dropped at the rotator write. At zero trims
    // this is the s44-proven derivation (the only delta is lroundf on pitch,
    // sub-unit); with trims it composes them as a quat in the controller's
    // LOCAL frame, the one algebra that holds at every orientation.
    const float pos[3] = {hp.px, hp.py, hp.pz};
    const float quat[4] = {hp.qx, hp.qy, hp.qz, hp.qw};
    const GamePose gp =
        ray_pose_from_xr(fc, pos, quat, g_aimTrim[hand][0].load(std::memory_order_relaxed),
                         g_aimTrim[hand][1].load(std::memory_order_relaxed));
    *out = gp.rot;
    if (poseOut) *poseOut = hp;
    return true;
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
void publish_dot(int slot, int hand, const FRotator& ray, const bvr::vr::HeadPose& pose,
                 bool enabled) {
    bvr::vr::AimDotConfig dc{};
    dc.enabled = enabled;
    dc.sizeDeg = g_dotSizeDeg.load(std::memory_order_relaxed);
    int32_t gameYawUnits = 0, recenterYawUnits = 0;
    if (enabled && camera::aim_basis(&gameYawUnits, &recenterYawUnits)) {
        FRotator xrRot{};
        xrRot.pitch = ray.pitch;
        xrRot.yaw = wrap_rot(ray.yaw - gameYawUnits + recenterYawUnits);
        xrRot.roll = 0;
        float fwd[3], right[3], up[3];
        ue_rot_basis(xrRot, fwd, right, up);
        float dirXr[3], rightXr[3], upXr[3];
        ue_to_xr(fwd, dirXr);
        ue_to_xr(right, rightXr);
        ue_to_xr(up, upXr);
        const float d = g_dotDistM.load(std::memory_order_relaxed);
        // s45b: the ray-origin offsets, in the trimmed ray's own frame, cm ->
        // meters. Origin moves, direction does not - the dot stays the round
        // trip of the WRITTEN rotator.
        const float f = g_aimPosCm[hand][0].load(std::memory_order_relaxed) * 0.01f;
        const float rr = g_aimPosCm[hand][1].load(std::memory_order_relaxed) * 0.01f;
        const float u = g_aimPosCm[hand][2].load(std::memory_order_relaxed) * 0.01f;
        dc.posXr[0] = pose.px + dirXr[0] * d + dirXr[0] * f + rightXr[0] * rr + upXr[0] * u;
        dc.posXr[1] = pose.py + dirXr[1] * d + dirXr[1] * f + rightXr[1] * rr + upXr[1] * u;
        dc.posXr[2] = pose.pz + dirXr[2] * d + dirXr[2] * f + rightXr[2] * rr + upXr[2] * u;
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
    // s45b: the beam carries EXACTLY the ray's trim and origin offsets -
    // render-side it composes them with the same xr_local_trim_quat the game
    // side used, so beam and shot agree at every controller orientation by
    // shared algebra. Zero trims = the s44 behaviour, bit for bit.
    lc.pitchTrimDeg = g_aimTrim[hand][0].load(std::memory_order_relaxed);
    lc.yawTrimDeg = g_aimTrim[hand][1].load(std::memory_order_relaxed);
    lc.posFwdCm = g_aimPosCm[hand][0].load(std::memory_order_relaxed);
    lc.posRightCm = g_aimPosCm[hand][1].load(std::memory_order_relaxed);
    lc.posUpCm = g_aimPosCm[hand][2].load(std::memory_order_relaxed);
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
    bool have[2] = {false, false};
    for (int h = 0; h < 2; ++h)
        have[h] = controller_ray(h, &rays[h], &poses[h]);

    // Slot 0 is the RIGHT hand and slot 1 the left, matching BS2's convention
    // (slot 0 keeps BS1 parity there); a hand whose pose is missing publishes
    // disabled rather than stale.
    publish_dot(0, 1, rays[1], poses[1], have[1] && g_dot[1].load(std::memory_order_relaxed));
    publish_dot(1, 0, rays[0], poses[0], have[0] && g_dot[0].load(std::memory_order_relaxed));
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
    } else if (strncmp(args, "trim", 4) == 0) {
        char hs[4] = {};
        float p = 0, y = 0;
        if (sscanf_s(args + 4, " %3s %f %f", hs, 4u, &p, &y) == 3) {
            const int h = (*hs == 'l' || *hs == 'L') ? 0 : 1;
            g_aimTrim[h][0].store(p, std::memory_order_relaxed);
            g_aimTrim[h][1].store(y, std::memory_order_relaxed);
            BVR_LOG("[bsi] aim: trim %c = pitch %.2f yaw %.2f deg (no roll slot - roll "
                    "cannot move a ray)",
                    h ? 'R' : 'L', p, y);
        } else {
            BVR_LOG("[bsi] aim: usage - bsiaim trim l|r <pitchDeg> <yawDeg>");
        }
    } else if (strncmp(args, "origin", 6) == 0) {
        char hs[4] = {};
        float f = 0, r = 0, u = 0;
        if (sscanf_s(args + 6, " %3s %f %f %f", hs, 4u, &f, &r, &u) == 4) {
            const int h = (*hs == 'l' || *hs == 'L') ? 0 : 1;
            g_aimPosCm[h][0].store(f, std::memory_order_relaxed);
            g_aimPosCm[h][1].store(r, std::memory_order_relaxed);
            g_aimPosCm[h][2].store(u, std::memory_order_relaxed);
            BVR_LOG("[bsi] aim: origin %c = fwd %.1f right %.1f up %.1f cm (dot+laser "
                    "origin only - this game's fire seam is rotation-only)",
                    h ? 'R' : 'L', f, r, u);
        } else {
            BVR_LOG("[bsi] aim: usage - bsiaim origin l|r <fwd> <right> <up> (cm)");
        }
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

    // s45b: the aim CALIBRATION block - trim pitch/yaw (no roll: it cannot
    // move a ray) + ray-origin offsets, per hand behind one radio (BS1's
    // one-slider-set convention). The laser and dot carry these exactly.
    ImGui::Separator();
    static int aimHand = 1;
    ImGui::TextDisabled("Aim calibration (trim moves shot+dot+laser together)");
    ImGui::RadioButton("tune L##aim", &aimHand, 0);
    ImGui::SameLine();
    ImGui::RadioButton("tune R##aim", &aimHand, 1);
    float tv = g_aimTrim[aimHand][0].load(std::memory_order_relaxed);
    if (ImGui::SliderFloat("aim trim pitch (deg)", &tv, -30.0f, 30.0f))
        g_aimTrim[aimHand][0].store(tv, std::memory_order_relaxed);
    tv = g_aimTrim[aimHand][1].load(std::memory_order_relaxed);
    if (ImGui::SliderFloat("aim trim yaw (deg)", &tv, -30.0f, 30.0f))
        g_aimTrim[aimHand][1].store(tv, std::memory_order_relaxed);
    const char* axesP[3] = {"ray origin fwd (cm)", "ray origin right (cm)",
                            "ray origin up (cm)"};
    for (int a = 0; a < 3; ++a) {
        tv = g_aimPosCm[aimHand][a].load(std::memory_order_relaxed);
        if (ImGui::SliderFloat(axesP[a], &tv, -60.0f, 60.0f))
            g_aimPosCm[aimHand][a].store(tv, std::memory_order_relaxed);
    }

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

float trim_get(int hand, int axis) {
    return g_aimTrim[hand & 1][axis & 1].load(std::memory_order_relaxed);
}
void trim_set(int hand, int axis, float v) {
    g_aimTrim[hand & 1][axis & 1].store(v, std::memory_order_relaxed);
}
float origin_get(int hand, int axis) {
    return g_aimPosCm[hand & 1][axis % 3].load(std::memory_order_relaxed);
}
void origin_set(int hand, int axis, float v) {
    g_aimPosCm[hand & 1][axis % 3].store(v, std::memory_order_relaxed);
}

} // namespace bvr::bsi::aim
