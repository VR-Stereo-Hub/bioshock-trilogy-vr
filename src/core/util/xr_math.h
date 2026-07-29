#pragma once
// Quaternion helpers on XR conventions (right +X, up +Y, forward -Z, meters,
// right-handed) - PURE math, no engine semantics, which is why this lives in
// core: the laser (core/vr) and the game adapters must compose trims with the
// SAME algebra or the beam and the barrel disagree everywhere but the tuning
// pose (session 20, the aim-sync unification). Promoted from what is now
// game/shared/ue_math.h, which re-exports these for its own callers.

#include <cmath>

namespace bvr::xrmath {

// Rotate v by the quaternion (xyzw).
inline void quat_rotate(float qx, float qy, float qz, float qw, const float v[3],
                        float out[3]) {
    float t[3] = {2.0f * (qy * v[2] - qz * v[1]), 2.0f * (qz * v[0] - qx * v[2]),
                  2.0f * (qx * v[1] - qy * v[0])};
    out[0] = v[0] + qw * t[0] + (qy * t[2] - qz * t[1]);
    out[1] = v[1] + qw * t[1] + (qz * t[0] - qx * t[2]);
    out[2] = v[2] + qw * t[2] + (qx * t[1] - qy * t[0]);
}

// Hamilton product a (x) b, xyzw order: rotating by the result applies b
// FIRST, then a. `pose (x) trim` therefore applies the trim in the POSE'S OWN
// local frame - which is what a mesh-alignment offset must be. Adding euler
// angles after conversion only behaves at one controller orientation; the M7
// in-headset test proved that the hard way (the "pivot that breaks
// everything"), and the session-20 synccheck baseline measured it at up to
// 28.21 deg of ray-vs-barrel divergence.
inline void quat_mul(const float a[4], const float b[4], float out[4]) {
    out[0] = a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1];
    out[1] = a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0];
    out[2] = a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3];
    out[3] = a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2];
}

inline void quat_axis_angle(float ax, float ay, float az, float rad, float out[4]) {
    float s = sinf(rad * 0.5f);
    out[0] = ax * s;
    out[1] = ay * s;
    out[2] = az * s;
    out[3] = cosf(rad * 0.5f);
}

inline void quat_conj(const float q[4], float out[4]) {
    out[0] = -q[0];
    out[1] = -q[1];
    out[2] = -q[2];
    out[3] = q[3];
}

// Local-frame trim quaternion from user-facing angles, XR axes, signs chosen
// to match the UE conventions the sliders claim: +pitch tilts up, +yaw turns
// right, +roll tilts clockwise (right). Applied as pose (x) trim. The roll
// axis is INNERMOST (qy * (qp * qr)), so a roll trim can never change where a
// ray points - which is why the aim trim stays pitch/yaw-only.
inline void xr_local_trim_quat(float pitchRad, float yawRad, float rollRad, float out[4]) {
    float qy[4], qp[4], qr[4], t[4];
    quat_axis_angle(0.0f, 1.0f, 0.0f, -yawRad, qy);  // +Y is up; -angle = right
    quat_axis_angle(1.0f, 0.0f, 0.0f, pitchRad, qp); // +X is right; + = up
    quat_axis_angle(0.0f, 0.0f, -1.0f, rollRad, qr); // -Z is forward; + = clockwise
    quat_mul(qp, qr, t);
    quat_mul(qy, t, out);
}

} // namespace bvr::xrmath
