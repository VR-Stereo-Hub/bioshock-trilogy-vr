#pragma once
// XR <-> Unreal (Vengeance/UE2.5) conventions for this adapter. ONE copy,
// shared by the camera drive (camera.cpp) and the aim ray (aim.cpp): if these
// ever disagree the crosshair and the bullet disagree, which is exactly the
// bug M6 exists to fix.
//
// XR LOCAL space: right +X, up +Y, forward -Z, meters, right-handed.
// UE2.5: forward +X, right +Y, up +Z; FRotator 65536 units per turn, positive
// yaw turns toward +Y (right), positive pitch looks up, positive roll tilts
// clockwise (right).

#include <cmath>
#include <cstdint>

namespace bvr::b1r {

struct FVector { float x, y, z; };             // Unreal units
struct FRotator { int32_t pitch, yaw, roll; }; // 65536 units per full turn

constexpr float kPi = 3.14159265f;
constexpr float kRotUnitsPerDegree = 65536.0f / 360.0f;
constexpr float kRotUnitsPerRadian = 65536.0f / (2.0f * kPi);
constexpr float kRadToDeg = 57.29578f;

inline void quat_rotate(float qx, float qy, float qz, float qw, const float v[3],
                        float out[3]) {
    float t[3] = {2.0f * (qy * v[2] - qz * v[1]), 2.0f * (qz * v[0] - qx * v[2]),
                  2.0f * (qx * v[1] - qy * v[0])};
    out[0] = v[0] + qw * t[0] + (qy * t[2] - qz * t[1]);
    out[1] = v[1] + qw * t[1] + (qz * t[0] - qx * t[2]);
    out[2] = v[2] + qw * t[2] + (qx * t[1] - qy * t[0]);
}

inline void xr_to_ue(const float v[3], float out[3]) {
    out[0] = -v[2]; // XR -Z (forward) -> UE +X
    out[1] = v[0];  // XR +X (right)   -> UE +Y
    out[2] = v[1];  // XR +Y (up)      -> UE +Z
}

struct UeAngles { float yawRad, pitchRad, rollRad; };

// Euler angles (UE frame) of an XR-space orientation quaternion.
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
        // Zero-roll frame from the forward vector, then measure the actual up
        // vector against it. rn = normalize(cross(worldUp, f)), un = cross(f, rn).
        float rn[3] = {-f[1] / len2d, f[0] / len2d, 0.0f};
        float un[3] = {-f[2] * rn[1], f[2] * rn[0], f[0] * rn[1] - f[1] * rn[0]};
        a.rollRad = atan2f(u[0] * rn[0] + u[1] * rn[1],
                           u[0] * un[0] + u[1] * un[1] + u[2] * un[2]);
    }
    return a;
}

// Unit forward vector of an FRotator (UE convention), for turning an aim
// rotation back into a direction vector.
inline void ue_rot_to_dir(const FRotator& r, float out[3]) {
    float yaw = static_cast<float>(r.yaw) / kRotUnitsPerRadian;
    float pitch = static_cast<float>(r.pitch) / kRotUnitsPerRadian;
    float cp = cosf(pitch);
    out[0] = cosf(yaw) * cp;
    out[1] = sinf(yaw) * cp;
    out[2] = sinf(pitch);
}

// Orthonormal basis of an FRotator (UE's own FRotationMatrix rows): X forward,
// Y right, Z up. Used to place a model-space offset - "2 cm forward, 1 cm up of
// the grip" - in world space without the offset swinging as the wrist rolls.
inline void ue_rot_basis(const FRotator& r, float fwd[3], float right[3], float up[3]) {
    float pitch = static_cast<float>(r.pitch) / kRotUnitsPerRadian;
    float yaw = static_cast<float>(r.yaw) / kRotUnitsPerRadian;
    float roll = static_cast<float>(r.roll) / kRotUnitsPerRadian;
    float cp = cosf(pitch), sp = sinf(pitch);
    float cy = cosf(yaw), sy = sinf(yaw);
    float cr = cosf(roll), sr = sinf(roll);

    fwd[0] = cp * cy;
    fwd[1] = cp * sy;
    fwd[2] = sp;

    right[0] = sr * sp * cy - cr * sy;
    right[1] = sr * sp * sy + cr * cy;
    right[2] = -sr * cp;

    up[0] = -(cr * sp * cy + sr * sy);
    up[1] = cy * sr - cr * sp * sy;
    up[2] = cr * cp;
}

// FRotator (pitch/yaw, roll 0) of a UE-space direction vector.
inline FRotator ue_dir_to_rot(const float d[3]) {
    FRotator r{};
    float len2d = sqrtf(d[0] * d[0] + d[1] * d[1]);
    r.yaw = static_cast<int32_t>(atan2f(d[1], d[0]) * kRotUnitsPerRadian);
    r.pitch = static_cast<int32_t>(atan2f(d[2], len2d) * kRotUnitsPerRadian);
    return r;
}

} // namespace bvr::b1r
