#pragma once
// The view-transform basis the camera drive used this frame, published once
// per pass-1 dispatch on the game thread - and the TWO pose->game chains that
// consume it (the aim ray in aim.cpp, the hand/weapon model in hands.cpp).
//
// They MUST agree: a gun drawn with one transform and a bullet fired with
// another is exactly the mismatch I8 exists to remove. So the context and the
// XR-pose-to-game mapping live here, once, as PURE functions - production and
// any flat sweep call the same code.
//
// DUPLICATED IN SHAPE from game/bioshock2r/frame_context.h per the
// keep-the-mods-decoupled directive (user, session 34). NO NUMBER TRANSFERS:
// this is UE3 build 6829 - the yaw basis below is Infinite's own s44-proven
// integer-rotator derivation (additive yaw on the engine's pre-drive yaw,
// pitch absolute), not the Vengeance float path.
//
// THE TRIM COMPOSE IS NOT FREE TO DIVERGE: core's laser re-derives its ray on
// the render thread with bvr::xrmath::xr_local_trim_quat (openxr_runtime.cpp
// build_laser_layers), so both chains below compose the trim as a quaternion
// in the controller's LOCAL frame - `pose (x) trim` - BEFORE angle
// extraction. BS1 session 20 measured up to 28.21 deg of ray-vs-barrel
// divergence from adding euler angles after the map instead; roll is
// INNERMOST in xr_local_trim_quat, which is why the aim trim is pitch/yaw
// only (a roll trim cannot move a ray) while the model trim carries roll.

#include <cmath>
#include <cstdint>

#include "core/util/xr_math.h"
#include "game/bioshockinf/inf_math.h"

namespace bvr::bsi {

// Published by camera.cpp's drive_view (pass 1 only; pass 2 replays and must
// never touch it). Consumers: aim.cpp (ray), hands.cpp (model).
struct FrameContext {
    bool valid = false;          // head drive drove THIS dispatch, recenter exists
    float engineLocX = 0.0f;     // engine camera loc BEFORE the head drive (out-params
    float engineLocY = 0.0f;     //   as the original returned them)
    float engineLocZ = 0.0f;
    int32_t gameYawUnits = 0;    // engine's own pre-drive yaw (the aim_basis yaw)
    int32_t recenterYawUnits = 0;
    float recenterPx = 0.0f, recenterPy = 0.0f, recenterPz = 0.0f; // XR meters
    float worldScale = 50.0f;    // UU per meter (live slider value)
};

struct GamePose {
    FVector loc;   // game-space UU
    FRotator rot;  // pitch/yaw/roll rotator units; consumers drop roll if unwanted
};

// Map an XR-space controller pose (meters + quat, as core reports it) into
// game space through EXACTLY the basis this frame's camera drive used: the
// same pre-drive engine yaw, the same recenter, the same world scale.
//
// Rotation: yaw is ADDITIVE on the engine's own yaw (hand yaw residual off
// the recenter), pitch and roll are absolute - drive_view's s39/s44-proven
// convention, integer wrap_rot so the invariant is a theorem, not a drift.
// Position: recenter-relative XR offset -> UE axes -> de-rotated by the
// recenter yaw -> re-rotated by the game yaw -> scaled onto the pre-drive
// engine camera loc (the head drive adds its own delta through the identical
// formula, so head and hand share one origin by construction).
inline GamePose xr_pose_to_game(const FrameContext& fc, const float pos[3],
                                const float quat[4]) {
    UeAngles a = ue_angles_from_xr_quat(quat[0], quat[1], quat[2], quat[3]);
    const int32_t handYawUnits = static_cast<int32_t>(lroundf(a.yawRad * kRotUnitsPerRadian));
    GamePose out{};
    out.rot.yaw = fc.gameYawUnits + wrap_rot(handYawUnits - fc.recenterYawUnits);
    out.rot.pitch = static_cast<int32_t>(lroundf(a.pitchRad * kRotUnitsPerRadian));
    out.rot.roll = static_cast<int32_t>(lroundf(a.rollRad * kRotUnitsPerRadian));

    const float recenterYawRad = static_cast<float>(fc.recenterYawUnits) / kRotUnitsPerRadian;
    const float gameYawRad = static_cast<float>(fc.gameYawUnits) / kRotUnitsPerRadian;
    const float dxr[3] = {pos[0] - fc.recenterPx, pos[1] - fc.recenterPy,
                          pos[2] - fc.recenterPz};
    float d[3];
    xr_to_ue(dxr, d);
    const float c = cosf(-recenterYawRad), s = sinf(-recenterYawRad);
    const float lx = d[0] * c - d[1] * s;
    const float ly = d[0] * s + d[1] * c;
    const float cg = cosf(gameYawRad), sg = sinf(gameYawRad);
    out.loc.x = fc.engineLocX + (lx * cg - ly * sg) * fc.worldScale;
    out.loc.y = fc.engineLocY + (lx * sg + ly * cg) * fc.worldScale;
    out.loc.z = fc.engineLocZ + d[2] * fc.worldScale;
    return out;
}

// Model chain (hands.cpp): trim composed as a quat in the controller's local
// frame, then mapped. Holds at every controller orientation, rolled included.
inline GamePose model_pose_from_xr(const FrameContext& fc, const float pos[3],
                                   const float quat[4], float trimPitchDeg,
                                   float trimYawDeg, float trimRollDeg) {
    float trim[4], q2[4];
    bvr::xrmath::xr_local_trim_quat(trimPitchDeg / kRadToDeg, trimYawDeg / kRadToDeg,
                                    trimRollDeg / kRadToDeg, trim);
    bvr::xrmath::quat_mul(quat, trim, q2);
    return xr_pose_to_game(fc, pos, q2);
}

// Ray chain (aim.cpp): the model's EXACT compose with the roll trim slot 0
// (roll is innermost, it could not move the ray anyway), roll dropped only at
// the final rotator write - aim carries no roll; the camera owns roll.
inline GamePose ray_pose_from_xr(const FrameContext& fc, const float pos[3],
                                 const float quat[4], float trimPitchDeg,
                                 float trimYawDeg) {
    GamePose out = model_pose_from_xr(fc, pos, quat, trimPitchDeg, trimYawDeg, 0.0f);
    out.rot.roll = 0;
    return out;
}

} // namespace bvr::bsi
