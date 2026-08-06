#pragma once
// The view transform the GetPlayerViewPoint drive produced, published once per
// dispatch on the game thread.
//
// Two consumers place a controller in the world: the aim ray (aim.cpp) and the
// hand/weapon model (hands.cpp). They MUST agree - a gun drawn with one
// transform and a bullet fired with another is exactly the mismatch I8 exists
// to remove - so the context and the XR-pose-to-game-space mapping live here,
// once.
//
// DUPLICATED from game/bioshock2r/frame_context.h per the keep-the-mods-
// decoupled directive: BS1 and BS2 are headset-accepted baselines and must not
// be put at risk to serve Infinite. This is a DIFFERENT ENGINE (UE3 build
// 6829), so the shapes are adapted, not inherited, and two of them depart
// deliberately - see below.
//
// ONE THING HERE IS NOT FREE TO DIVERGE: the trim compose. Core's laser
// re-derives its ray on the RENDER thread with bvr::xrmath::xr_local_trim_quat
// (openxr_runtime.cpp build_laser_layers), so the model/ray chains below must
// use the same algebra or the beam and the bullet disagree - BioShock 1
// session 20 measured up to 28.21 deg of divergence when the ray added rotator
// angles in game space instead.
//
// ---- DEPARTURE 1: YAW IS CARRIED AS AN EXACT INTEGER ------------------------
// BS2 carries the yaw basis as float radians and reconstructs it. Here both
// yaws are the int32 rotator units the camera drive already holds, and they are
// only ever ADDED to other integers.
//
// The reason is a specific banked number. publish_dot inverts the ray with
// wrap_rot(ray.yaw - gameYawUnits + recenterYawUnits). Substituting
// ray.yaw = gameYawUnits + wrap_rot(handYawUnits - recenterYawUnits), the
// gameYawUnits term cancels EXACTLY in int32 - no rounding, however large the
// game yaw is - and wrap_rot preserves the low 16 bits, so the inverse yields
// the hand's own yaw and `aimRayMaxDevDeg` reads a literal 0.0000. Route that
// term through float radians (a 24-bit mantissa over a value up to 32768, then
// truncated) and the residue is a rotator unit or two: the dot sits ~0.005-0.01
// deg off the controller forward and the headline baseline figure stops being
// zero. One rotator unit is 0.0055 deg.
//
// ---- DEPARTURE 2: PUBLISHED VIA A SEQLOCK, NOT PASSED -----------------------
// On BS2 the CalcView tail calls both consumers directly. Here the ray is built
// in the AIM hook (APawn::GetBaseAimRotation, which fires on the engine's fire
// path) while the context is produced in the CAMERA hook, and the rig write can
// sit in a third place again - so the context is published once and read
// wherever, with the writer never blocking. See frame_context.cpp.

#include "game/bioshockinf/inf_math.h"

#include <cstdint>

namespace bvr::bsi {

struct FrameContext {
    bool vrDriving = false;       // the HMD drove the camera on this dispatch
    bool haveRecenter = false;    // a recenter exists, so the residual has meaning
    int32_t gameYawUnits = 0;     // the ENGINE's own yaw, PRE-drive (rot->yaw at entry)
    int32_t recenterYawUnits = 0; // the yaw the recenter pinned
    // The drive_view ENTRY *loc: PRE-head-offset AND PRE-eye-offset. Anything
    // else and every hand sits half an IPD sideways and differs between eyes.
    float baseX = 0.0f, baseY = 0.0f, baseZ = 0.0f;
    float recenterPx = 0.0f, recenterPy = 0.0f, recenterPz = 0.0f; // XR m at recenter
    // UU per metre. 50 is UE3 canonical and this game's default - NOT BS1/BS2's
    // 100. Every fallback in this file uses 50 for that reason.
    float worldScale = 50.0f;
    void* pc = nullptr;   // the APlayerController the dispatch fired on
    uint64_t stamp = 0;   // GetTickCount64() at publish
};

struct GamePose {
    FVector loc;
    FRotator rot; // pitch/yaw/roll all filled; the ray zeroes roll at the write
};

// Map an XR-space controller pose (metres + quaternion, as core reports it)
// into game space through EXACTLY the transform this frame's camera drive used:
// the same recenter, the same game yaw, the same world scale.
inline GamePose xr_pose_to_game(const FrameContext& ctx, const float pos[3],
                                const float quat[4]) {
    const UeAngles a = ue_angles_from_xr_quat(quat[0], quat[1], quat[2], quat[3]);
    GamePose out{};

    // ---- ROTATION ----------------------------------------------------------
    // VERBATIM from what aim.cpp's controller_ray shipped and the user accepted
    // in the headset. The lroundf/truncate ASYMMETRY between yaw and pitch is
    // deliberate and must NOT be tidied here:
    //   yaw   -> lroundf. Yaw is the term that has to cancel exactly against
    //            publish_dot's inverse, so it becomes an integer ONCE and is
    //            thereafter only ever added to integers.
    //   pitch -> a bare cast, i.e. truncation toward zero. Not because
    //            truncation is better - it is not - but because that is what
    //            shipped and what the banked aimRayMaxDevDeg 0.0000 was measured
    //            on. Promoting it to lroundf would move the pitch by up to one
    //            rotator unit on every frame: a silent behaviour change inside a
    //            refactor whose entire claim is that it has none. If it is worth
    //            changing it is worth its own commit and its own A/B.
    //   roll  -> truncates for the same reason. The ray drops it; the model
    //            keeps it.
    const int32_t handYawUnits =
        static_cast<int32_t>(lroundf(a.yawRad * kRotUnitsPerRadian));
    const int32_t residual = wrap_rot(handYawUnits - ctx.recenterYawUnits);
    out.rot.pitch = static_cast<int32_t>(a.pitchRad * kRotUnitsPerRadian);
    // Rotator sum through unsigned, then back: the engine keeps rotators
    // normalized so this cannot overflow today, but a wrapped rotator is
    // well-defined and signed overflow is UB.
    out.rot.yaw = static_cast<int32_t>(static_cast<uint32_t>(ctx.gameYawUnits) +
                                       static_cast<uint32_t>(residual));
    out.rot.roll = static_cast<int32_t>(a.rollRad * kRotUnitsPerRadian);

    // ---- POSITION ----------------------------------------------------------
    // camera.cpp drive_view's head-offset math with `pos` substituted for the
    // head pose. BOTH yaws are derived FROM THE INTEGERS by exactly the
    // expressions drive_view uses, so feeding the head pose through here
    // reproduces the driven camera's pre-eye location - a free identity check.
    const float gameYawRad = static_cast<float>(ctx.gameYawUnits) / kRotUnitsPerRadian;
    const float recenterYawRad =
        static_cast<float>(ctx.recenterYawUnits) / kRotUnitsPerRadian;
    const float dxr[3] = {pos[0] - ctx.recenterPx, pos[1] - ctx.recenterPy,
                          pos[2] - ctx.recenterPz};
    float d[3];
    xr_to_ue(dxr, d);
    const float c = cosf(-recenterYawRad), s = sinf(-recenterYawRad);
    const float lx = d[0] * c - d[1] * s;
    const float ly = d[0] * s + d[1] * c;
    const float cg = cosf(gameYawRad), sg = sinf(gameYawRad);
    out.loc.x = ctx.baseX + (lx * cg - ly * sg) * ctx.worldScale;
    out.loc.y = ctx.baseY + (lx * sg + ly * cg) * ctx.worldScale;
    out.loc.z = ctx.baseZ + d[2] * ctx.worldScale;
    return out;
}

// The INVERSE of xr_pose_to_game's position half, for the aim dot's ORIGIN.
//
// Why it exists: the fire seam substitutes a game-space ray built on the game
// thread, while the laser RE-DERIVES its ray from the controller pose in XR
// space on the render thread. Mapping the FINAL game-space ray point back into
// XR makes "dot == shot" true by construction rather than by argument. Today's
// dot uses the raw controller position as its origin, which is correct only
// while the aim origin offset is zero.
//
// The forward map's position half is affine and yaw-only, so this is its exact
// algebraic inverse, not an approximation:
//   forward: loc = base + Rot(gameYaw - recenterYaw) * xr_to_ue(pos - recenterP) * scale
//   inverse: pos = recenterP + ue_to_xr( Rot(recenterYaw - gameYaw) * (loc - base) / scale )
// Pass the SAME ctx the ray was built from, or the two disagree by whatever the
// camera did in between.
inline void game_point_to_xr(const FrameContext& ctx, const FVector& loc, float out[3]) {
    const float gameYawRad = static_cast<float>(ctx.gameYawUnits) / kRotUnitsPerRadian;
    const float recenterYawRad =
        static_cast<float>(ctx.recenterYawUnits) / kRotUnitsPerRadian;
    const float scale = ctx.worldScale != 0.0f ? ctx.worldScale : 50.0f;

    const float ux = (loc.x - ctx.baseX) / scale;
    const float uy = (loc.y - ctx.baseY) / scale;
    const float uz = (loc.z - ctx.baseZ) / scale;

    const float th = recenterYawRad - gameYawRad; // undo the net yaw
    const float c = cosf(th), s = sinf(th);
    const float dx = ux * c - uy * s;
    const float dy = ux * s + uy * c;

    out[0] = dy + ctx.recenterPx;  // ue_to_xr: UE +Y (right) -> XR +X
    out[1] = uz + ctx.recenterPy;  //           UE +Z (up)    -> XR +Y
    out[2] = -dx + ctx.recenterPz; //           UE +X (fwd)   -> XR -Z
}

// ---- The two trimmed pose->rot chains, as PURE functions ---------------------
// Production (hands.cpp / aim.cpp) and any flat sweep call the SAME code, so a
// sweep measures the real thing. Both chains compose the trim as a quaternion in
// the controller's LOCAL frame - the only algebra that holds at every controller
// orientation, and the one core's laser uses - and the ray then drops roll at
// the final rotator write.

// Model chain (hands.cpp): trim quat composed in the controller's local frame,
// then mapped.
//
// WITH ALL TRIMS ZERO THIS IS A BIT-EXACT IDENTITY, and that is load-bearing
// (it is what lets the refactor claim it changed nothing). The proof:
//   1. 0.0f / kRadToDeg is exactly 0.0f, and sinf(+-0) = +-0, cosf(+-0) = 1.0f
//      exactly, so xr_local_trim_quat returns {z, z, z, 1.0f} where each z is a
//      signed zero and w is exactly 1.0f.
//   2. In quat_mul(quat, trim), every cross term has a zero factor and so is a
//      signed zero, and each lane keeps exactly one q_i * 1.0f term. Multiplying
//      a finite float by 1.0f is exact, and adding a zero of either sign to a
//      nonzero finite is exact. So q2 == quat component-wise, except possibly
//      the sign of a component that was already zero.
//   3. Sign-of-zero cannot survive to the rotator: atan2f's only +-0-sensitive
//      results are themselves +-0, and both lroundf(+-0.0f) and the truncating
//      cast yield 0.
// DELIBERATELY NOT short-circuited on "all trims zero". A skip branch would make
// the identity trivial AND would mean the shipped default configuration never
// executes the compose path, so the first user to type a trim would be the first
// to run it. The identity is left to the algebra and MEASURED live by
// `bsiaim selfcheck`.
inline GamePose model_pose_from_xr(const FrameContext& ctx, const float pos[3],
                                   const float quat[4], float trimPitchDeg,
                                   float trimYawDeg, float trimRollDeg) {
    float trim[4], q2[4];
    xr_local_trim_quat(trimPitchDeg / kRadToDeg, trimYawDeg / kRadToDeg,
                       trimRollDeg / kRadToDeg, trim);
    quat_mul(quat, trim, q2);
    return xr_pose_to_game(ctx, pos, q2);
}

// Ray chain (aim.cpp): the model's EXACT compose with the roll trim slot 0
// (roll is innermost in xr_local_trim_quat, so it could not steer the ray
// anyway), roll dropped only at the final rotator write - aim carries no roll,
// the camera owns roll.
inline GamePose ray_pose_from_xr(const FrameContext& ctx, const float pos[3],
                                 const float quat[4], float trimPitchDeg,
                                 float trimYawDeg) {
    GamePose out = model_pose_from_xr(ctx, pos, quat, trimPitchDeg, trimYawDeg, 0.0f);
    out.rot.roll = 0;
    return out;
}

// ---- Publication ------------------------------------------------------------
// Writer: the game thread, inside drive_view, once per camera dispatch (NOT on
// the present edge - the aim seam fires on the engine's fire path, which is not
// synchronized with presents, so a context one present old would be up to 11 ms
// stale on a moving hand).
namespace frame_context {

// Game thread only. A second writer is counted and refused, never accepted.
void publish(const FrameContext& c);

// Any thread. False = no coherent, driving, fresh context - the caller must
// REFUSE rather than substitute a guess. maxAgeMs 0 disables the age gate (the
// status readout wants to SHOW staleness, not hide it); the default 100 ms
// matches the SR pass-2 replay's own freshness gate in camera.cpp.
bool read(FrameContext* out, uint32_t maxAgeMs = 100);

void stats(uint32_t* publishes, uint32_t* retries, uint32_t* refusals,
           uint32_t* foreignWrites);

} // namespace frame_context

} // namespace bvr::bsi
