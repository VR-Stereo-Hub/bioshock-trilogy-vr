// Self-contained vector/quaternion/matrix math for bvr_xrsim32.
//
// Deliberately NOT core/util or game/shared/ue_math.h: including a mod header
// here would couple the simulated runtime to the tree it is supposed to test.
// The sim must be able to build and run with the mod sources absent.
//
// Conventions are OpenXR's: right-handed, +X right, +Y up, -Z forward, and
// quaternions stored (x, y, z, w). Angles at this layer are RADIANS; the command
// parser is what accepts degrees.

#pragma once

#include <cmath>

namespace xrsim {

struct Vec3 {
    float x, y, z;
};

struct Quat {
    float x, y, z, w;
};

struct Pose {
    Quat q;
    Vec3 p;
};

// Tangent-space field of view. OpenXR gives angles; the compositor wants
// tangents, so both forms exist and convert on demand.
struct Fov {
    float angleLeft, angleRight, angleUp, angleDown;
};

constexpr float kPi = 3.14159265358979323846f;

inline float deg2rad(float d) { return d * (kPi / 180.0f); }
inline float rad2deg(float r) { return r * (180.0f / kPi); }

inline Vec3 v3(float x, float y, float z) { return Vec3{x, y, z}; }
inline Vec3 v3_add(const Vec3& a, const Vec3& b) { return Vec3{a.x + b.x, a.y + b.y, a.z + b.z}; }
inline Vec3 v3_sub(const Vec3& a, const Vec3& b) { return Vec3{a.x - b.x, a.y - b.y, a.z - b.z}; }
inline Vec3 v3_scale(const Vec3& a, float s) { return Vec3{a.x * s, a.y * s, a.z * s}; }
inline float v3_dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline float v3_len(const Vec3& a) { return std::sqrt(v3_dot(a, a)); }

inline Vec3 v3_norm(const Vec3& a) {
    const float l = v3_len(a);
    return (l > 1e-8f) ? v3_scale(a, 1.0f / l) : Vec3{0.0f, 0.0f, -1.0f};
}

inline Vec3 v3_cross(const Vec3& a, const Vec3& b) {
    return Vec3{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

inline Vec3 v3_lerp(const Vec3& a, const Vec3& b, float t) {
    return Vec3{a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t};
}

inline Quat quat_identity() { return Quat{0.0f, 0.0f, 0.0f, 1.0f}; }

inline Quat quat_norm(const Quat& q) {
    const float l = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    if (l <= 1e-8f) return quat_identity();
    const float inv = 1.0f / l;
    return Quat{q.x * inv, q.y * inv, q.z * inv, q.w * inv};
}

inline Quat quat_mul(const Quat& a, const Quat& b) {
    return Quat{a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
                a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
                a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
                a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}

inline Quat quat_conj(const Quat& q) { return Quat{-q.x, -q.y, -q.z, q.w}; }

inline Vec3 quat_rotate(const Quat& q, const Vec3& v) {
    // v + 2w(qv x v) + 2(qv x (qv x v)) - the standard shuffle-free form.
    const Vec3 u{q.x, q.y, q.z};
    const Vec3 t = v3_scale(v3_cross(u, v), 2.0f);
    return v3_add(v3_add(v, v3_scale(t, q.w)), v3_cross(u, t));
}

// Yaw about +Y, then pitch about +X, then roll about -Z.
//
// This matches the mod's own convention so a `head rot 30 0 0` here reads the
// same way a `simhead 30 0 0` does over in camera.cpp: positive yaw turns left,
// positive pitch looks up.
inline Quat quat_from_ypr(float yawRad, float pitchRad, float rollRad) {
    const float cy = std::cos(yawRad * 0.5f), sy = std::sin(yawRad * 0.5f);
    const float cp = std::cos(pitchRad * 0.5f), sp = std::sin(pitchRad * 0.5f);
    const float cr = std::cos(rollRad * 0.5f), sr = std::sin(rollRad * 0.5f);

    const Quat qy{0.0f, sy, 0.0f, cy};
    const Quat qp{sp, 0.0f, 0.0f, cp};
    const Quat qr{0.0f, 0.0f, -sr, cr};
    return quat_mul(quat_mul(qy, qp), qr);
}

inline void quat_to_ypr(const Quat& q, float& yawRad, float& pitchRad, float& rollRad) {
    // Extract by rotating the basis rather than by a closed form, which keeps
    // the round trip exact enough for the determinism check (M8) and avoids the
    // usual gimbal sign traps near +/-90 pitch.
    const Vec3 fwd = quat_rotate(q, Vec3{0.0f, 0.0f, -1.0f});
    const Vec3 up = quat_rotate(q, Vec3{0.0f, 1.0f, 0.0f});
    yawRad = std::atan2(-fwd.x, -fwd.z);
    pitchRad = std::asin(fwd.y < -1.0f ? -1.0f : (fwd.y > 1.0f ? 1.0f : fwd.y));
    const Vec3 right = v3_norm(v3_cross(fwd, Vec3{0.0f, 1.0f, 0.0f}));
    const Vec3 trueUp = v3_cross(right, fwd);
    rollRad = std::atan2(v3_dot(up, right), v3_dot(up, trueUp));
}

inline Quat quat_slerp(const Quat& a, const Quat& bIn, float t) {
    Quat b = bIn;
    float d = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
    if (d < 0.0f) {
        b = Quat{-b.x, -b.y, -b.z, -b.w};
        d = -d;
    }
    if (d > 0.9995f) {
        return quat_norm(Quat{a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t,
                              a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t});
    }
    const float theta = std::acos(d);
    const float s = std::sin(theta);
    const float wa = std::sin((1.0f - t) * theta) / s;
    const float wb = std::sin(t * theta) / s;
    return quat_norm(Quat{a.x * wa + b.x * wb, a.y * wa + b.y * wb,
                          a.z * wa + b.z * wb, a.w * wa + b.w * wb});
}

inline Pose pose_identity() { return Pose{quat_identity(), Vec3{0.0f, 0.0f, 0.0f}}; }

// b expressed in a's frame, then lifted into a's parent: the usual "a then b".
inline Pose pose_mul(const Pose& a, const Pose& b) {
    Pose out;
    out.q = quat_norm(quat_mul(a.q, b.q));
    out.p = v3_add(a.p, quat_rotate(a.q, b.p));
    return out;
}

inline Pose pose_inverse(const Pose& a) {
    Pose out;
    out.q = quat_conj(a.q);
    out.p = v3_scale(quat_rotate(out.q, a.p), -1.0f);
    return out;
}

inline Pose pose_lerp(const Pose& a, const Pose& b, float t) {
    Pose out;
    out.q = quat_slerp(a.q, b.q, t);
    out.p = v3_lerp(a.p, b.p, t);
    return out;
}

// Smoothstep. The default easing for `head to`, because a linear ramp makes
// every motion test look like a teleport at the endpoints.
inline float ease_smooth(float t) {
    if (t <= 0.0f) return 0.0f;
    if (t >= 1.0f) return 1.0f;
    return t * t * (3.0f - 2.0f * t);
}

// --- conversions to/from the XR types --------------------------------------

inline XrPosef to_xr(const Pose& p) {
    XrPosef out;
    out.orientation.x = p.q.x;
    out.orientation.y = p.q.y;
    out.orientation.z = p.q.z;
    out.orientation.w = p.q.w;
    out.position.x = p.p.x;
    out.position.y = p.p.y;
    out.position.z = p.p.z;
    return out;
}

inline Pose from_xr(const XrPosef& p) {
    Pose out;
    out.q = Quat{p.orientation.x, p.orientation.y, p.orientation.z, p.orientation.w};
    out.p = Vec3{p.position.x, p.position.y, p.position.z};
    return out;
}

inline XrFovf to_xr(const Fov& f) {
    XrFovf out;
    out.angleLeft = f.angleLeft;
    out.angleRight = f.angleRight;
    out.angleUp = f.angleUp;
    out.angleDown = f.angleDown;
    return out;
}

inline Fov from_xr(const XrFovf& f) {
    return Fov{f.angleLeft, f.angleRight, f.angleUp, f.angleDown};
}

// --- 4x4 matrices (column vectors, m[row][col]) -----------------------------

struct Mat4 {
    float m[4][4];
};

inline Mat4 mat4_identity() {
    Mat4 out{};
    out.m[0][0] = out.m[1][1] = out.m[2][2] = out.m[3][3] = 1.0f;
    return out;
}

// World-to-eye for a pose given in world space.
inline Mat4 mat4_view_from_pose(const Pose& eye) {
    const Pose inv = pose_inverse(eye);
    const Vec3 r = quat_rotate(inv.q, Vec3{1.0f, 0.0f, 0.0f});
    const Vec3 u = quat_rotate(inv.q, Vec3{0.0f, 1.0f, 0.0f});
    const Vec3 f = quat_rotate(inv.q, Vec3{0.0f, 0.0f, 1.0f});
    Mat4 out = mat4_identity();
    out.m[0][0] = r.x; out.m[0][1] = u.x; out.m[0][2] = f.x;
    out.m[1][0] = r.y; out.m[1][1] = u.y; out.m[1][2] = f.y;
    out.m[2][0] = r.z; out.m[2][1] = u.z; out.m[2][2] = f.z;
    out.m[3][0] = inv.p.x; out.m[3][1] = inv.p.y; out.m[3][2] = inv.p.z;
    return out;
}

// Asymmetric off-centre projection built straight from the fov half-angles,
// which is what a headset actually has. Reverse-Z is not used: the sim composites
// in submission order without a depth buffer, per the OpenXR layer rules.
inline Mat4 mat4_projection_fov(const Fov& fov, float nearZ, float farZ) {
    const float tl = std::tan(fov.angleLeft);
    const float tr = std::tan(fov.angleRight);
    const float tu = std::tan(fov.angleUp);
    const float td = std::tan(fov.angleDown);
    const float w = tr - tl;
    const float h = tu - td;

    Mat4 out{};
    out.m[0][0] = 2.0f / w;
    out.m[1][1] = 2.0f / h;
    out.m[2][0] = (tr + tl) / w;
    out.m[2][1] = (tu + td) / h;
    out.m[2][2] = -farZ / (farZ - nearZ);
    out.m[2][3] = -1.0f;
    out.m[3][2] = -(farZ * nearZ) / (farZ - nearZ);
    return out;
}

inline Mat4 mat4_mul(const Mat4& a, const Mat4& b) {
    Mat4 out{};
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
            out.m[r][c] = a.m[r][0] * b.m[0][c] + a.m[r][1] * b.m[1][c] +
                          a.m[r][2] * b.m[2][c] + a.m[r][3] * b.m[3][c];
    return out;
}

} // namespace xrsim
