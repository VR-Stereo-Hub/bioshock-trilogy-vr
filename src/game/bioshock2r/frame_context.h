#pragma once
// The view transform the CalcView drive produced, published once per frame on
// the game thread.
//
// Two consumers place a controller in the world: the aim ray (aim.cpp) and the
// hand/weapon models (hands.cpp). They MUST agree - a gun drawn with one
// transform and a bullet fired with another is exactly the mismatch this
// milestone exists to remove - so the context and the XR-pose-to-game-space
// mapping live here, once, in the same spirit as shared/ue_math.h.
//
// DUPLICATED from game/bioshock1r/frame_context.h per the keep-the-mods-
// decoupled directive (user, session 34): BS1 is the headset-accepted
// baseline and must not be put at risk to serve BS2. The math is engine
// convention, identical across the two remasters (same build session), so the
// duplication is deliberate and cheap; consolidation waits for the polish
// milestone's healing session.
//
// ONE THING HERE IS NOT FREE TO DIVERGE: the trim compose. Core's laser
// re-derives its ray on the RENDER thread with bvr::xrmath::xr_local_trim_quat
// (openxr_runtime.cpp build_laser_layers), so the model/ray chains below must
// use the same algebra or the beam and the bullet disagree - BS1 session 20
// measured up to 28.21 deg divergence when the ray added rotator angles in
// game space instead.

#include "game/shared/ue_math.h"

namespace bvr::b2r {

struct FrameContext {
    bool vrDriving = false;   // HMD is driving the camera this frame
    float camX = 0.0f, camY = 0.0f, camZ = 0.0f;    // final camera loc, UU (incl. head offset)
    float baseX = 0.0f, baseY = 0.0f, baseZ = 0.0f; // camera loc BEFORE the head offset
    int32_t camPitch = 0, camYaw = 0, camRoll = 0;  // final camera rot, 65536 units/turn
    float driveYawOffsetRad = 0.0f; // yaw the head drive added on top of the game yaw
    float recenterYawRad = 0.0f;    // XR yaw at recenter
    float recenterPx = 0.0f, recenterPy = 0.0f, recenterPz = 0.0f; // XR meters at recenter
    float worldScale = 100.0f;      // UU per meter
    void* viewActor = nullptr;      // CalcView param-block view actor (cutscene guard)
    void* pc = nullptr;             // the PlayerController the dispatch fired on
};

struct GamePose {
    FVector loc;
    FRotator rot; // pitch/yaw/roll all filled; consumers zero roll if they want it gone
};

// Map an XR-space controller pose (meters + quaternion, as core reports it) into
// game space through EXACTLY the transform this frame's camera drive used: the
// same recenter pose, the same game yaw, the same world scale.
inline GamePose xr_pose_to_game(const FrameContext& ctx, const float pos[3],
                                const float quat[4]) {
    // The game's own yaw, i.e. the final camera yaw minus whatever the head
    // drive added on top of it this frame.
    float gameYawRad =
        static_cast<float>(ctx.camYaw) / kRotUnitsPerRadian - ctx.driveYawOffsetRad;

    UeAngles a = ue_angles_from_xr_quat(quat[0], quat[1], quat[2], quat[3]);

    GamePose out{};
    out.rot.yaw = static_cast<int32_t>(
        (gameYawRad + (a.yawRad - ctx.recenterYawRad)) * kRotUnitsPerRadian);
    out.rot.pitch = static_cast<int32_t>(a.pitchRad * kRotUnitsPerRadian);
    out.rot.roll = static_cast<int32_t>(a.rollRad * kRotUnitsPerRadian);

    // Position: recenter-relative XR offset -> UE axes -> into the recenter-local
    // frame -> out by the game yaw -> scaled onto the pre-head-offset camera.
    float dxr[3] = {pos[0] - ctx.recenterPx, pos[1] - ctx.recenterPy, pos[2] - ctx.recenterPz};
    float d[3];
    xr_to_ue(dxr, d);
    float c = cosf(-ctx.recenterYawRad), s = sinf(-ctx.recenterYawRad);
    float lx = d[0] * c - d[1] * s;
    float ly = d[0] * s + d[1] * c;
    float cg = cosf(gameYawRad), sg = sinf(gameYawRad);
    out.loc.x = ctx.baseX + (lx * cg - ly * sg) * ctx.worldScale;
    out.loc.y = ctx.baseY + (lx * sg + ly * cg) * ctx.worldScale;
    out.loc.z = ctx.baseZ + d[2] * ctx.worldScale;
    return out;
}

// The INVERSE of xr_pose_to_game's position half, for the aim dot.
//
// Why this exists: the fire seam substitutes a game-space ray (built on the
// game thread), while the laser RE-DERIVES its ray from the controller pose in
// XR space on the render thread. Those two are congruent - same trim algebra,
// same pose funnel - but NOT identical: different pose instant, different
// origin-offset basis. A dot placed the laser's way would inherit that gap and
// "dot == shot" would be an argument rather than a fact. Mapping the FINAL
// game-space ray point back into XR closes it by construction.
//
// The forward map's position half is affine and yaw-only, so this is its exact
// algebraic inverse - not an approximation:
//   forward: loc = base + Rot(gameYaw - recenterYaw) * xr_to_ue(pos - recenterP) * scale
//   inverse: pos = recenterP + ue_to_xr( Rot(recenterYaw - gameYaw) * (loc - base) / scale )
// Pass the SAME ctx the ray was built from or the two disagree by whatever the
// camera did in between.
inline void game_point_to_xr(const FrameContext& ctx, const FVector& loc, float out[3]) {
    float gameYawRad =
        static_cast<float>(ctx.camYaw) / kRotUnitsPerRadian - ctx.driveYawOffsetRad;
    float scale = ctx.worldScale != 0.0f ? ctx.worldScale : 100.0f;

    float ux = (loc.x - ctx.baseX) / scale;
    float uy = (loc.y - ctx.baseY) / scale;
    float uz = (loc.z - ctx.baseZ) / scale;

    // Undo the net yaw the forward map applied.
    float th = ctx.recenterYawRad - gameYawRad;
    float c = cosf(th), s = sinf(th);
    float dx = ux * c - uy * s;
    float dy = ux * s + uy * c;

    // ue_to_xr: the exact inverse of xr_to_ue (UE +X fwd -> XR -Z, +Y -> +X,
    // +Z -> +Y), then back out of the recenter-relative frame.
    out[0] = dy + ctx.recenterPx;
    out[1] = uz + ctx.recenterPy;
    out[2] = -dx + ctx.recenterPz;
}

// ---- The two trimmed pose->rot chains, as PURE functions --------------------
// Production (hands.cpp / aim.cpp) and the flat sync sweep call the SAME code,
// so the sweep measures the real thing. Both chains compose the trim as a
// quaternion in the controller's LOCAL frame (the only algebra that holds at
// every controller orientation, and the one core's laser uses); the ray then
// drops roll at the final rotator write.

// Model chain (hands.cpp): trim quat composed in the controller's local frame,
// then mapped. Holds at every controller orientation.
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
// (roll is innermost in xr_local_trim_quat, so it could not move the ray
// anyway), roll dropped only at the final rotator write - aim carries no roll;
// the camera owns roll.
inline GamePose ray_pose_from_xr(const FrameContext& ctx, const float pos[3],
                                 const float quat[4], float trimPitchDeg,
                                 float trimYawDeg) {
    GamePose out = model_pose_from_xr(ctx, pos, quat, trimPitchDeg, trimYawDeg, 0.0f);
    out.rot.roll = 0;
    return out;
}

} // namespace bvr::b2r
