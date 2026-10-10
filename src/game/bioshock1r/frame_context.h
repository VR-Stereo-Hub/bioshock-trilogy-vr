#pragma once
// The view transform the CalcView drive produced, published once per frame on
// the game thread.
//
// Two consumers place a controller in the world: the M6 aim ray (aim.cpp) and
// the M7 hand viewmodel (hands.cpp). They MUST agree - a gun drawn with one
// transform and a bullet fired with another is exactly the mismatch this whole
// milestone exists to remove - so the context and the XR-pose-to-game-space
// mapping live here, once, in the same spirit as ue_math.h.

#include "game/shared/ue_math.h"

namespace bvr::b1r {

struct FrameContext {
    bool vrDriving = false;   // HMD is driving the camera this frame
    float camX = 0.0f, camY = 0.0f, camZ = 0.0f;   // final camera loc, UU (incl. head offset)
    float baseX = 0.0f, baseY = 0.0f, baseZ = 0.0f; // camera loc BEFORE the head offset
    // s90: the head-anchor offset the camera carries on top of base + the HMD's own offset - the
    // "Head offset up / fwd" sliders (`CameraHeightOffset`, 9 UU by default), world UU. Anything
    // placed relative to the PLAYER'S HEAD (the mode 4 hands) must add it, or it is drawn this far
    // off the eye: BRVR places hands head-relative, so its CameraHeightOffset "comes along for free".
    float anchorX = 0.0f, anchorY = 0.0f, anchorZ = 0.0f;
    int32_t camPitch = 0, camYaw = 0, camRoll = 0;  // final camera rot, 65536 units/turn
    float driveYawOffsetRad = 0.0f; // yaw the head drive added on top of the game yaw
    // s74c: the PHYSICAL head yaw, RAW off the HMD, before the body transfer
    // sees it. Nothing else here can stand in for it: camYaw is the head PLUS
    // the body, and driveYawOffsetRad is only the part the transfer has not
    // swallowed yet - which is ~0 in the shipping config. Without this a stick
    // turn and a real head turn are the same numbers.
    //
    // s74d: raw, and NOT recenter-relative. recenterYawRad below is advanced by
    // the body transfer every frame, so subtracting it here leaves a near
    // constant and any detector built on the difference goes silent. Take
    // differences of this over time instead; a fixed offset cancels in those.
    float headYawRad = 0.0f;
    float recenterYawRad = 0.0f;    // XR yaw at recenter
    // s85b: the head's XR yaw at the last ACTUAL recenter, never advanced. recenterYawRad
    // above starts there but the body transfer and snap turns advance it, so the net yaw
    // (game yaw - recenterYawRad) faces the room's XR yaw 0, not the player. Adding this
    // turns the net yaw into the way the player faced at recenter.
    float recenterHeadYawRad = 0.0f;
    float recenterPx = 0.0f, recenterPy = 0.0f, recenterPz = 0.0f; // XR meters at recenter
    float worldScale = 100.0f;      // UU per meter (session-16 user calibration)
    void* viewActor = nullptr;      // *view_actor out-param (cutscene guard)
    void* pc = nullptr;             // the PlayerController the hook fired on
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

// The INVERSE of xr_pose_to_game's position half (session 29, for the aim dot).
//
// Why this exists: the fire seam substitutes g_ray (game space, built on the
// game thread), while the laser RE-DERIVES its ray from the controller pose in
// XR space on the render thread. Those two are congruent - same trim algebra,
// same pose funnel - but NOT identical: different pose instant, different
// origin-offset basis. A dot placed the laser's way would inherit that gap and
// "dot == shot" would be an argument rather than a fact. Mapping the FINAL
// game-space ray point back into XR closes it by construction, so the dot is
// the exact point the bullet starts from, extended along the exact rotator the
// engine is handed.
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

// ---- The two trimmed pose->rot chains, as PURE functions (session 20) -------
// Production (hands.cpp / aim.cpp) and the `vraim synccheck` sweep call the
// SAME code, so the sweep measures the real thing. Since the session-20
// unification both chains compose the trim as a quaternion in the
// controller's LOCAL frame (the model's algebra - the only one that holds at
// every controller orientation); the ray then drops roll at the final
// rotator write. The pre-unification ray added rotator angles in game space
// after the map, which agreed with the model only at the tuning pose
// (measured: up to 28.21 deg divergence at rolled poses, ENGINE_NOTES
// session 20).

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

// ---- s86f: THE HAND STEP ROWS (Dishonored's MpTrimViewStep), pure --------------------
//
// F10 Hands has rows that move or turn a hand the way the player SEES it - left/right,
// down/up, back/forward in the head's yaw frame (up = world up); pitch/yaw/roll about the
// same axes - and each press is converted, at that instant, into the hand's own stored
// trim: a palm-frame translation (bones::off_hand_cm, applied along the trimmed hand's
// forward/right/up) and the controller-local rotation trim (pitch/yaw/roll degrees,
// xr_local_trim_quat's convention). Stored that way the trim rides with the hand and
// never with the head. Host-tested in ue-math-tests.

// The head's yaw frame: forward flattened, up = world up, right = fwd x up.
inline void view_step_basis(const FrameContext& ctx, float fwd[3], float right[3], float up[3]) {
    ue_rot_basis(FRotator{0, ctx.camYaw, 0}, fwd, right, up);
}

// A position step of `cm` along view axis (0 right, 1 forward, 2 up), into the palm-frame
// translation `gripCm[3]` (forward / right / up of the TRIMMED hand pose `hand`).
inline void hand_position_step(const FrameContext& ctx, const GamePose& hand, int axis, float cm,
                               float gripCm[3]) {
    float vf[3], vr[3], vu[3], tf[3], tr[3], tu[3];
    view_step_basis(ctx, vf, vr, vu);
    ue_rot_basis(hand.rot, tf, tr, tu);
    const float* a = axis == 0 ? vr : axis == 1 ? vf : vu;
    const float d[3] = {a[0] * cm, a[1] * cm, a[2] * cm};
    gripCm[0] += d[0] * tf[0] + d[1] * tf[1] + d[2] * tf[2];
    gripCm[1] += d[0] * tr[0] + d[1] * tr[1] + d[2] * tr[2];
    gripCm[2] += d[0] * tu[0] + d[1] * tu[1] + d[2] * tu[2];
}

// A UE-space quaternion (ue_rot_to_quat's convention) back to UE angles.
inline UeAngles ue_angles_from_ue_quat(const float q[4]) {
    const float kFwd[3] = {1, 0, 0}, kUp[3] = {0, 0, 1};
    float f[3], u[3];
    quat_rotate(q[0], q[1], q[2], q[3], kFwd, f);
    quat_rotate(q[0], q[1], q[2], q[3], kUp, u);
    UeAngles a{};
    a.yawRad = atan2f(f[1], f[0]);
    const float len2d = sqrtf(f[0] * f[0] + f[1] * f[1]);
    a.pitchRad = atan2f(f[2], len2d);
    if (len2d > 0.001f) {
        const float rn[3] = {-f[1] / len2d, f[0] / len2d, 0.0f};
        const float un[3] = {-f[2] * rn[1], f[2] * rn[0], f[0] * rn[1] - f[1] * rn[0]};
        a.rollRad = atan2f(u[0] * rn[0] + u[1] * rn[1], u[0] * un[0] + u[1] * un[1] + u[2] * un[2]);
    }
    return a;
}

// The quaternion (xyzw) whose rotation takes the local axes onto the columns x, y, z.
inline void quat_from_columns(const float x[3], const float y[3], const float z[3], float out[4]) {
    const float m00 = x[0], m01 = y[0], m02 = z[0], m10 = x[1], m11 = y[1], m12 = z[1], m20 = x[2],
                m21 = y[2], m22 = z[2];
    const float tr = m00 + m11 + m22;
    if (tr > 0.0f) {
        const float s = sqrtf(tr + 1.0f) * 2.0f;
        out[3] = 0.25f * s; out[0] = (m21 - m12) / s; out[1] = (m02 - m20) / s; out[2] = (m10 - m01) / s;
    } else if (m00 > m11 && m00 > m22) {
        const float s = sqrtf(1.0f + m00 - m11 - m22) * 2.0f;
        out[3] = (m21 - m12) / s; out[0] = 0.25f * s; out[1] = (m01 + m10) / s; out[2] = (m02 + m20) / s;
    } else if (m11 > m22) {
        const float s = sqrtf(1.0f + m11 - m00 - m22) * 2.0f;
        out[3] = (m02 - m20) / s; out[0] = (m01 + m10) / s; out[1] = 0.25f * s; out[2] = (m12 + m21) / s;
    } else {
        const float s = sqrtf(1.0f + m22 - m00 - m11) * 2.0f;
        out[3] = (m10 - m01) / s; out[0] = (m02 + m20) / s; out[1] = (m12 + m21) / s; out[2] = 0.25f * s;
    }
}

// The inverse of ue_angles_from_xr_quat: the XR quaternion whose forward (XR -Z) and up
// (XR +Y) map, through xr_to_ue, onto the UE basis of these angles.
inline void xr_quat_from_ue_angles(const UeAngles& a, float out[4]) {
    const FRotator r{static_cast<int32_t>(a.pitchRad * kRotUnitsPerRadian),
                     static_cast<int32_t>(a.yawRad * kRotUnitsPerRadian),
                     static_cast<int32_t>(a.rollRad * kRotUnitsPerRadian)};
    float f[3], ri[3], u[3];
    ue_rot_basis(r, f, ri, u);
    // ue_to_xr: UE +X -> XR -Z, UE +Y -> XR +X, UE +Z -> XR +Y.
    const float fx[3] = {f[1], f[2], -f[0]}, ux[3] = {u[1], u[2], -u[0]};
    // XR is right-handed with x right, y up, z back: right = fwd x up, z = -fwd.
    const float rx[3] = {fx[1] * ux[2] - fx[2] * ux[1], fx[2] * ux[0] - fx[0] * ux[2], fx[0] * ux[1] - fx[1] * ux[0]};
    const float zx[3] = {-fx[0], -fx[1], -fx[2]};
    quat_from_columns(rx, ux, zx, out);
}

// The inverse of xr_local_trim_quat: trim = Ry(-yaw) * Rx(pitch) * Rz(-roll), so a YXZ
// decomposition of its matrix gives the three angles back.
inline void xr_local_trim_angles(const float q[4], float* pitchRad, float* yawRad, float* rollRad) {
    const float ex[3] = {1, 0, 0}, ey[3] = {0, 1, 0}, ez[3] = {0, 0, 1};
    float cx[3], cy[3], cz[3];
    quat_rotate(q[0], q[1], q[2], q[3], ex, cx);
    quat_rotate(q[0], q[1], q[2], q[3], ey, cy);
    quat_rotate(q[0], q[1], q[2], q[3], ez, cz);
    // R[r][c]: columns are the images of the axes.
    const float R02 = cz[0], R22 = cz[2], R12 = cz[1], R10 = cx[1], R11 = cy[1];
    const float b = asinf(fmaxf(-1.0f, fminf(1.0f, -R12)));
    *pitchRad = b;
    *yawRad = -atan2f(R02, R22);
    *rollRad = -atan2f(R10, R11);
}

// A rotation step of `deg` about view axis (0 yaw: + turns right; 1 pitch: + nose up;
// 2 roll: + tips the top to the right), applied to a hand whose controller XR quaternion
// is `ctrlXr` and whose trim is (pitch, yaw, roll) degrees. Rewrites the trim so that
// model_pose_from_xr(ctx, ., ctrlXr, trim') is the stepped hand. False if the frame is
// degenerate (the hand pointing straight up or down).
inline bool hand_rotation_step(const FrameContext& ctx, const float ctrlXr[4], int axis, float deg,
                               float* pitchDeg, float* yawDeg, float* rollDeg) {
    float vf[3], vr[3], vu[3];
    view_step_basis(ctx, vf, vr, vu);
    // The hand as it is now, UE world.
    const float dummyPos[3] = {0, 0, 0};
    const GamePose now = model_pose_from_xr(ctx, dummyPos, ctrlXr, *pitchDeg, *yawDeg, *rollDeg);
    float H[4];
    ue_rot_to_quat(now.rot, H);
    // The world step. UE yaw + turns X toward Y: about +Z. Pitch + lifts X toward Z: about -Y
    // (here -right). Roll + tips up toward right: about -X (here -forward).
    float k[3];
    if (axis == 0) { k[0] = vu[0]; k[1] = vu[1]; k[2] = vu[2]; }
    else if (axis == 1) { k[0] = -vr[0]; k[1] = -vr[1]; k[2] = -vr[2]; }
    else { k[0] = -vf[0]; k[1] = -vf[1]; k[2] = -vf[2]; }
    float Rw[4], Hn[4];
    quat_axis_angle(k[0], k[1], k[2], deg / kRadToDeg, Rw);
    quat_mul(Rw, H, Hn);
    // Back through the pose map: UE angles -> the XR angles xr_pose_to_game would need.
    UeAngles a = ue_angles_from_ue_quat(Hn);
    const float gameYawRad = static_cast<float>(ctx.camYaw) / kRotUnitsPerRadian - ctx.driveYawOffsetRad;
    a.yawRad = a.yawRad - gameYawRad + ctx.recenterYawRad;
    float q2[4], cinv[4], trim[4];
    xr_quat_from_ue_angles(a, q2);
    quat_conj(ctrlXr, cinv);
    quat_mul(cinv, q2, trim); // q2 = ctrl (x) trim
    float p, y, r;
    xr_local_trim_angles(trim, &p, &y, &r);
    if (!std::isfinite(p) || !std::isfinite(y) || !std::isfinite(r)) return false;
    *pitchDeg = p * kRadToDeg;
    *yawDeg = y * kRadToDeg;
    *rollDeg = r * kRadToDeg;
    return true;
}

// Ray chain (aim.cpp): the model's EXACT compose with the roll trim slot 0
// (roll is innermost in xr_local_trim_quat, so it could not move the ray
// anyway), roll dropped only at the final rotator write - aim carries no
// roll; the camera owns roll. Tuned pitch/yaw trim values carry over from
// the old rotator-add algebra: the two agree exactly at the neutral pose.
inline GamePose ray_pose_from_xr(const FrameContext& ctx, const float pos[3],
                                 const float quat[4], float trimPitchDeg,
                                 float trimYawDeg) {
    GamePose out = model_pose_from_xr(ctx, pos, quat, trimPitchDeg, trimYawDeg, 0.0f);
    out.rot.roll = 0;
    return out;
}

} // namespace bvr::b1r
