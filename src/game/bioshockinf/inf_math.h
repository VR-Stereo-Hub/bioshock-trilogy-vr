#pragma once
// XR <-> Unreal math for BioShock Infinite (UE3 build 6829), LOCAL to this
// adapter on purpose. game/shared/ue_math.h documents itself as the
// Vengeance/UE2.5 family's convention, and this game's rules say that on UE3
// even shapes are suspect - so the helpers this adapter needs are DUPLICATED
// here (the decoupling directive: copy and adapt, never promote or share)
// and each convention below is stated as a falsifiable claim.
//
// CONVENTION STATUS (session 39): the UE3 frame is assumed to match the
// Vengeance one - forward +X, right +Y, up +Z; FRotator int32 x3, 65536 units
// per turn, SIGNED (session-36 motion test: one slow 360 swept -32392 to
// +32640), positive yaw toward +Y, positive pitch up, positive roll clockwise.
// The s39 flat battery falsifies each axis separately (simhead yaw/pitch/roll
// sweeps plus the sim's real `head orbit` path); if any axis disagrees, fix
// the mapping HERE and re-run the sweep, never patch at a call site.
//
// XR LOCAL space: right +X, up +Y, forward -Z, meters, right-handed.

#include "core/util/xr_math.h"

#include <cmath>
#include <cstdint>

namespace bvr::bsi {

using bvr::xrmath::quat_rotate;
using bvr::xrmath::xr_local_trim_quat;

// UE3 PODs. FRotator is 3x int32 in rotator units - NOT degrees, NOT floats.
// Reinterpreting one as a float gives a denormal that prints as 0.000 (the
// trap that cost BioShock 1 a long detour). Always %d, convert explicitly.
struct FVector {
    float x, y, z;
};
struct FRotator {
    int32_t pitch, yaw, roll;
};

constexpr float kPi = 3.14159265f;
constexpr float kRotUnitsPerDegree = 65536.0f / 360.0f;
constexpr float kRotUnitsPerRadian = 65536.0f / (2.0f * kPi);
constexpr float kRadToDeg = 57.29578f;
constexpr double kRotToDeg = 360.0 / 65536.0;

// Shortest-way-round rotator delta, wrapped to (-32768, 32767]. Rotator
// components are consumed modulo 65536, so this is the only correct way to
// subtract two of them: raw subtraction of yaw 100 from yaw 65500 reads as
// -65400 rather than +136. Integer arithmetic on purpose - exactness is what
// makes an additive-yaw invariant a theorem rather than a drift.
inline int32_t wrap_rot(int32_t delta) {
    return static_cast<int16_t>(delta & 0xFFFF);
}

inline void xr_to_ue(const float v[3], float out[3]) {
    out[0] = -v[2]; // XR -Z (forward) -> UE +X
    out[1] = v[0];  // XR +X (right)   -> UE +Y
    out[2] = v[1];  // XR +Y (up)      -> UE +Z
}

struct UeAngles {
    float yawRad, pitchRad, rollRad;
};

// Euler angles (UE frame) of an XR-space orientation quaternion: rotate the
// XR forward/up axes by the quat, map into the UE frame, then extract yaw and
// pitch from forward and roll from the up vector measured against a zero-roll
// reference frame (gimbal-guarded near straight up/down).
inline UeAngles ue_angles_from_xr_quat(float qx, float qy, float qz, float qw) {
    const float kFwd[3] = {0.0f, 0.0f, -1.0f};
    const float kUp[3] = {0.0f, 1.0f, 0.0f};
    float fxr[3], uxr[3], f[3], u[3];
    quat_rotate(qx, qy, qz, qw, kFwd, fxr);
    quat_rotate(qx, qy, qz, qw, kUp, uxr);
    xr_to_ue(fxr, f);
    xr_to_ue(uxr, u);

    UeAngles a{};
    a.yawRad = atan2f(f[1], f[0]);
    float len2d = sqrtf(f[0] * f[0] + f[1] * f[1]);
    a.pitchRad = atan2f(f[2], len2d);
    if (len2d > 0.001f) { // gimbal guard: keep roll 0 when looking straight up/down
        float rn[3] = {-f[1] / len2d, f[0] / len2d, 0.0f};
        float un[3] = {-f[2] * rn[1], f[2] * rn[0], f[0] * rn[1] - f[1] * rn[0]};
        a.rollRad = atan2f(u[0] * rn[0] + u[1] * rn[1],
                           u[0] * un[0] + u[1] * un[1] + u[2] * un[2]);
    }
    return a;
}

} // namespace bvr::bsi
