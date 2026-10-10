#pragma once
// arm_ik.h - BS1's arm, solved: shoulder, elbow, forearm roll. PURE - no engine reads,
// no globals - so tools\tests\arm-ik-tests.cpp and the offline Blender sweep
// (tools\arm-ik-sweep.cpp) run the same code the game does.
//
// Ported from the Dishonored VR mod's full-arm IK (src/game/dishonored/hands/arm_ik.h and
// pose_arm in arm_rig.h, 2026-10-07), whose reach, pole and twist design is itself the
// BioShock left-hand fork's (github.com/Owloeb/bioshock-trilogy-vr-lefthand, MIT,
// revision 3b5b818, bones.cpp arm_ik). docs/bioshock1/ARM_IK.md says what was taken and
// what changed.
//
// THE ONE ADAPTATION. Dishonored poses its arm at draw time, as skin matrices over the
// original mesh. BS1 writes the engine's evaluated skeleton instead - per-bone
// COMPONENT-space hkQsTransforms (position, rotation, scale), which the engine skins
// with and the weapon attachment follows (BS1 ENGINE_NOTES, "Skeleton / bone
// internals"). So this returns component-space bone transforms: each bone's reference
// transform carried by its segment's solved rotation, which is the same thing as
// Dishonored's skin matrix times the reference bind.
//
// Every point and direction in here is in ONE frame (the rig's component space, in the
// rig's own units). The caller converts the shoulder, pole and outward hints into it,
// and carries the elbow history across frames in a frame that does NOT spin with the
// actor (the held hand's actor turns with the controller every frame).
#include "core/util/xr_math.h"

#include <algorithm>
#include <cmath>

namespace bvr::b1r::arm_ik {

struct Vec {
    float x = 0, y = 0, z = 0;
    Vec operator+(Vec b) const { return {x + b.x, y + b.y, z + b.z}; }
    Vec operator-(Vec b) const { return {x - b.x, y - b.y, z - b.z}; }
    Vec operator*(float k) const { return {x * k, y * k, z * k}; }
};
inline Vec vec(const float* p) { return {p[0], p[1], p[2]}; }
inline void put(Vec a, float* p) {
    p[0] = a.x;
    p[1] = a.y;
    p[2] = a.z;
}
inline float dot(Vec a, Vec b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec cross(Vec a, Vec b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
inline float length(Vec a) { return sqrtf(dot(a, a)); }
inline bool finite(Vec a) { return std::isfinite(a.x) && std::isfinite(a.y) && std::isfinite(a.z); }
inline bool unit(Vec& a) {
    const float n = length(a);
    if (!std::isfinite(n) || n < 1e-6f) return false;
    a = a * (1.0f / n);
    return true;
}
inline Vec across(Vec v, Vec axis) { return v - axis * dot(v, axis); }
inline Vec fallback(Vec axis) {
    Vec v = fabsf(axis.x) < 0.6f ? Vec{1, 0, 0} : Vec{0, 1, 0};
    v = across(v, axis);
    unit(v);
    return v;
}

// ---- quaternions (xyzw, Hamilton; mul(a, b) applies b first - xr_math's) ----
struct Quat {
    float v[4] = {0, 0, 0, 1};
};
inline Quat quat(const float* q) { return {{q[0], q[1], q[2], q[3]}}; }
inline Quat mul(const Quat& a, const Quat& b) {
    Quat o;
    bvr::xrmath::quat_mul(a.v, b.v, o.v);
    return o;
}
inline Quat conj(const Quat& q) { return {{-q.v[0], -q.v[1], -q.v[2], q.v[3]}}; }
inline Vec rotate(const Quat& q, Vec v) {
    float in[3], out[3];
    put(v, in);
    bvr::xrmath::quat_rotate(q.v[0], q.v[1], q.v[2], q.v[3], in, out);
    return vec(out);
}
inline Quat axis_angle(Vec a, float angle) {
    if (!unit(a)) return {};
    Quat o;
    bvr::xrmath::quat_axis_angle(a.x, a.y, a.z, angle, o.v);
    return o;
}
inline Quat normalized(Quat q) {
    const float n = sqrtf(q.v[0] * q.v[0] + q.v[1] * q.v[1] + q.v[2] * q.v[2] + q.v[3] * q.v[3]);
    if (!(n > 1e-8f)) return {};
    for (float& c : q.v) c /= n;
    return q;
}
// A rotation matrix (row-major, m[r*3+c]) as a quaternion.
inline Quat from_matrix(const float m[9]) {
    Quat q;
    const float tr = m[0] + m[4] + m[8];
    if (tr > 0) {
        const float s = sqrtf(tr + 1) * 2;
        q.v[3] = 0.25f * s;
        q.v[0] = (m[7] - m[5]) / s;
        q.v[1] = (m[2] - m[6]) / s;
        q.v[2] = (m[3] - m[1]) / s;
    } else if (m[0] > m[4] && m[0] > m[8]) {
        const float s = sqrtf(1 + m[0] - m[4] - m[8]) * 2;
        q.v[3] = (m[7] - m[5]) / s;
        q.v[0] = 0.25f * s;
        q.v[1] = (m[1] + m[3]) / s;
        q.v[2] = (m[2] + m[6]) / s;
    } else if (m[4] > m[8]) {
        const float s = sqrtf(1 + m[4] - m[0] - m[8]) * 2;
        q.v[3] = (m[2] - m[6]) / s;
        q.v[0] = (m[1] + m[3]) / s;
        q.v[1] = 0.25f * s;
        q.v[2] = (m[5] + m[7]) / s;
    } else {
        const float s = sqrtf(1 + m[8] - m[0] - m[4]) * 2;
        q.v[3] = (m[3] - m[1]) / s;
        q.v[0] = (m[2] + m[6]) / s;
        q.v[1] = (m[5] + m[7]) / s;
        q.v[2] = 0.25f * s;
    }
    return normalized(q);
}

// The rotation taking the frame (refDirection, refNormal) onto (direction, normal):
// direction = a segment, normal = the arm's bend-plane normal. Carrying a bone's
// reference rotation by this needs no knowledge of the rig's bone-axis convention.
inline Quat frame_delta(Vec refDirection, Vec refNormal, Vec direction, Vec normal) {
    auto frame = [](Vec d, Vec n, float m[9]) {
        unit(d);
        n = across(n, d);
        if (!unit(n)) n = fallback(d);
        const Vec side = cross(n, d);
        const float f[9] = {d.x, side.x, n.x, d.y, side.y, n.y, d.z, side.z, n.z};
        for (int i = 0; i < 9; ++i) m[i] = f[i];
    };
    float a[9], b[9], r[9];
    frame(refDirection, refNormal, a);
    frame(direction, normal, b);
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) {
            float s = 0;
            for (int k = 0; k < 3; ++k) s += b[i * 3 + k] * a[j * 3 + k]; // B * A^T
            r[i * 3 + j] = s;
        }
    return from_matrix(r);
}

constexpr float kPi = 3.14159265359f;
constexpr float kDeg = kPi / 180.0f;
inline float wrap(float a) { return std::remainder(a, 2.0f * kPi); }
inline float unwrap(float a, float previous) { return previous + wrap(a - previous); }

// The part of `delta` that turns about unit `axis`, as a signed angle in (-pi, pi].
inline float twist_angle(const Quat& delta, Vec axis) {
    const float v = delta.v[0] * axis.x + delta.v[1] * axis.y + delta.v[2] * axis.z;
    if (fabsf(v) + fabsf(delta.v[3]) < 1e-6f) return 0;
    return wrap(2.0f * atan2f(v, delta.v[3]));
}

// ---- the two-bone solve (Dishonored solve(), unchanged in substance) -------------
struct Solution {
    Vec shoulder, elbow, wrist, pole;
    float shoulderShift = 0; // how far reach moved the shoulder off its nominal point
    bool reachClamped = false;
};
// nominal: where the shoulder wants to be. pole: the elbow's preferred direction.
// outward: the arm's own side - the elbow is never allowed to point inside it.
// priorPole: last frame's pole, same frame; only weighs in near a pole singularity.
// Out of reach (too far, or pulled in past 40%), the SHOULDER slides along the
// shoulder-wrist line - the hand never leaves the controller.
inline bool solve(Vec nominal, Vec wrist, Vec pole, Vec outward, Vec priorPole, float upper,
                  float lower, float margin, Solution& out) {
    if (!finite(nominal) || !finite(wrist) || !finite(pole) || !finite(outward) ||
        !finite(priorPole) || !std::isfinite(upper) || !std::isfinite(lower) ||
        !std::isfinite(margin) || upper <= 1e-4f || lower <= 1e-4f || margin < 0)
        return false;
    Vec n = wrist - nominal;
    const float raw = length(n);
    if (!unit(n)) {
        n = pole;
        if (!unit(n)) n = {0, 0, -1};
    }
    const float high = (upper + lower) * 0.995f;
    const float low = std::max(fabsf(upper - lower) * 1.05f + margin, (upper + lower) * 0.40f);
    if (low >= high) return false;
    const float d = std::clamp(raw, low, high);
    const Vec s = wrist - n * d;
    Vec p = across(pole, n);
    if (length(p) < 0.25f * length(pole)) p = p + across(priorPole, n) * (0.3f * length(pole));
    if (!unit(p)) {
        p = across(outward, n);
        if (!unit(p)) p = fallback(n);
    }
    Vec o = across(outward, n);
    if (unit(o)) {
        const float v = dot(p, o);
        if (v < 0.25f) {
            p = p + o * (0.25f - v);
            unit(p);
        }
    }
    const float cosine = std::clamp((upper * upper + d * d - lower * lower) / (2 * upper * d), -1.f, 1.f);
    const Vec e = s + n * (upper * cosine) + p * (upper * sqrtf(std::max(0.f, 1 - cosine * cosine)));
    out = {s, e, wrist, p, length(s - nominal), raw != d};
    return finite(e) && finite(s);
}

// ---- the arm (Dishonored pose_arm, on component-space bones) ---------------------
struct Bone {
    float p[3] = {};
    float q[4] = {0, 0, 0, 1};
    float s[3] = {1, 1, 1}; // a MULTIPLIER on the bone's reference scale
};
// One consistent pose of the arm, component space: what the solve carries.
struct Ref {
    Bone clavicle, upper, fore, twist[2]; // twist[0] ForeTwist, twist[1] ForeTwist1
    float wristP[3] = {};
    float wristQ[4] = {0, 0, 0, 1};
};
struct Input {
    Vec shoulder;                   // nominal shoulder (the upper arm's head)
    Vec wrist;                      // where the wrist IS, as the hand drive wrote it
    float wristQ[4] = {0, 0, 0, 1}; // and its rotation - the IK follows the final hand
    Vec pole, outward, priorPole;
    float priorTwist = 0;
    bool fresh = false;             // priorPole / priorTwist are from the last frame
    float scale = 1;                // the hand's scale; the arm is drawn at the same size
    float lengthScale = 1;          // arm length vs the rig's, separate from scale
    // s86: each segment's own length, on top of lengthScale. BS1's rig has an upper arm
    // 1.44x its forearm; a body that fits (Dishonored's accepted fit, measured in
    // HANDS_DISHONORED.md) has 0.90x. One multiplier cannot make both segments right.
    float upperLength = 1;
    float foreLength = 1;
    float margin = 0.5f;            // the closest reach keeps this much off a folded arm
};
struct Output {
    Bone clavicle, upper, fore, twist[2];
    Solution joints;
    Vec basePole;           // the pole BEFORE the roll swivel: next frame's priorPole
    float trackedTwist = 0; // the wrist's roll before the swivel: next frame's priorTwist
    float roll = 0;         // what the forearm still carries after the swivel
    float swivel = 0;       // how far the elbow lifted to carry the excess roll
};

// A bone's scale multiplier for an arm stretched by `lengthScale` along `segment`
// (component space): exact when the bone's local axis is along the segment (BS1's
// Biped rig, +X along every limb bone), the nearest per-axis form otherwise.
inline void stretch(const Quat& refQ, Vec segment, float scale, float lengthScale, float out[3]) {
    Vec d = rotate(conj(refQ), segment);
    if (!unit(d)) d = {1, 0, 0};
    const float c[3] = {d.x, d.y, d.z};
    for (int i = 0; i < 3; ++i) out[i] = scale * (1 + (lengthScale - 1) * c[i] * c[i]);
}

// The arm's swing for one pole, before any roll: the solved joints, and the rotation
// each segment's reference frame takes onto the solved one. `axis` is the forearm.
struct Swing {
    Solution joints;
    Quat upper, fore;
    Vec axis;
};
inline bool swing(const Ref& ref, const Input& in, Vec pole, Swing& out) {
    const Vec s0 = vec(ref.upper.p), e0 = vec(ref.fore.p), w0 = vec(ref.wristP);
    const Vec ue = e0 - s0, ew = w0 - e0;
    Vec refNormal = cross(ue, ew);
    if (!unit(refNormal)) refNormal = fallback(ue);
    const float k = in.scale * in.lengthScale;
    if (!solve(in.shoulder, in.wrist, pole, in.outward, in.priorPole, length(ue) * k * in.upperLength,
               length(ew) * k * in.foreLength, in.margin, out.joints))
        return false;
    Vec normal = cross(out.joints.pole, in.wrist - out.joints.shoulder);
    if (!unit(normal)) return false;
    out.upper = frame_delta(ue, refNormal, out.joints.elbow - out.joints.shoulder, normal);
    out.fore = frame_delta(ew, refNormal, in.wrist - out.joints.elbow, normal);
    out.axis = in.wrist - out.joints.elbow;
    return unit(out.axis);
}

// ---- the body yaw (Dishonored arm_ik.h BodyYaw, verbatim in substance) -----------------
// The shoulders face where the BODY faces, which is the head's yaw with a 25-degree
// deadzone and a 1.5-second relaxation toward it: a glance moves nothing, a turn of the
// body carries the shoulders. Radians; `update` returns the body yaw for this frame.
struct BodyYaw {
    float yaw = 0;
    bool valid = false;
    void reset() { valid = false; }
    float update(float headYaw, float seconds) {
        if (!valid) {
            yaw = headYaw;
            valid = true;
            return yaw;
        }
        const float error = wrap(headYaw - yaw), limit = 25.0f * kDeg;
        yaw += error - std::clamp(error, -limit, limit);
        yaw += wrap(headYaw - yaw) * (1 - expf(-std::clamp(seconds, 0.0f, 0.1f) / 1.5f));
        yaw = wrap(yaw);
        return yaw;
    }
};

constexpr float kTrustDirect = 110 * kDeg; // past this the reading may have wrapped
constexpr float kTrackLimit = 250 * kDeg;  // no wrist rolls further than this
constexpr float kComfort = 80 * kDeg;      // past this the elbow lifts to take the roll
constexpr float kMaxSwivel = 70 * kDeg;
constexpr float kElbowShare = 0.7f;        // forearm roll at the elbow; 1.0 at the wrist

inline bool pose(const Ref& ref, const Input& in, Output& out) {
    out = Output{};
    if (!std::isfinite(in.scale) || in.scale < 0.05f || !std::isfinite(in.lengthScale) ||
        in.lengthScale < 0.5f || in.lengthScale > 2.0f || !finite(in.wrist) ||
        !std::isfinite(in.upperLength) || in.upperLength < 0.25f || in.upperLength > 2.0f ||
        !std::isfinite(in.foreLength) || in.foreLength < 0.25f || in.foreLength > 2.0f)
        return false;
    const Vec s0 = vec(ref.upper.p), e0 = vec(ref.fore.p);
    const Vec ue = e0 - s0, ew = vec(ref.wristP) - e0;
    const float k = in.scale * in.lengthScale * in.foreLength; // the forearm's own stretch
    const Quat wristNow = normalized(quat(in.wristQ)), wristRef = normalized(quat(ref.wristQ));

    Swing sw0;
    Solution& sol = sw0.joints;
    Quat& up = sw0.upper;
    Quat& fore = sw0.fore;
    Vec& axis = sw0.axis;
    auto attempt = [&](Vec p) { return swing(ref, in, p, sw0); };
    // The wrist's own roll about the forearm, relative to where the solved forearm
    // would carry the reference wrist.
    auto roll_now = [&]() {
        return twist_angle(mul(mul(wristNow, conj(wristRef)), conj(fore)), axis);
    };
    if (!attempt(in.pole)) return false;
    float roll = roll_now();
    if (in.fresh && fabsf(roll) > kTrustDirect) roll = unwrap(roll, in.priorTwist);
    roll = std::clamp(roll, -kTrackLimit, kTrackLimit);
    out.trackedTwist = roll;
    const Vec basePole = sol.pole;
    if (fabsf(roll) > kComfort) {
        const float swivel = std::clamp(roll - std::copysign(kComfort, roll), -kMaxSwivel, kMaxSwivel);
        const Vec sw = in.wrist - sol.shoulder;
        if (!attempt(rotate(axis_angle(sw, swivel), basePole))) return false;
        roll = unwrap(roll_now(), roll - swivel);
        out.swivel = swivel;
    }
    out.joints = sol;
    out.basePole = basePole;
    out.roll = roll;

    // Upper arm at the shoulder; the clavicle hangs off it by its reference offset.
    {
        const Quat q = mul(up, quat(ref.upper.q));
        for (int i = 0; i < 4; ++i) out.upper.q[i] = q.v[i];
        put(sol.shoulder, out.upper.p);
        stretch(quat(ref.upper.q), ue, in.scale, in.lengthScale * in.upperLength, out.upper.s);

        // (BS1's NEWPlayerHands weights no vertex to the clavicles, measured s86; this
        // is for form.)
        const Quat qc = mul(up, quat(ref.clavicle.q));
        for (int i = 0; i < 4; ++i) out.clavicle.q[i] = qc.v[i];
        put(sol.shoulder + rotate(up, (vec(ref.clavicle.p) - s0) * in.scale), out.clavicle.p);
        for (float& c : out.clavicle.s) c = in.scale;
    }
    // Forearm and its twist helpers: placed by their reference offset from the elbow,
    // stretched along the forearm, and rolled - 70% of the roll at the elbow, ramping
    // to all of it at the wrist. The authored weights blend the forearm bones across
    // most of the shaft; Dishonored measured a zero-roll forearm against a rolled
    // helper shrinking the shaft to 45% of its radius (ARM_IK.md, "Twist candidate").
    Vec ewn = ew;
    unit(ewn);
    auto place = [&](const Bone& r, Bone& o) {
        const Vec off = vec(r.p) - e0;
        const float along = dot(off, ewn);
        const Vec stretched = (off - ewn * along) * in.scale + ewn * (along * k);
        const float fraction = std::clamp(along / std::max(1e-6f, length(ew)), 0.f, 1.f);
        const Quat turn = axis_angle(axis, roll * (kElbowShare + (1 - kElbowShare) * fraction));
        const Quat q = mul(turn, mul(fore, quat(r.q)));
        for (int i = 0; i < 4; ++i) o.q[i] = q.v[i];
        put(sol.elbow + rotate(fore, stretched), o.p);
        stretch(quat(r.q), ew, in.scale, in.lengthScale * in.foreLength, o.s);
    };
    place(ref.fore, out.fore);
    place(ref.twist[0], out.twist[0]);
    place(ref.twist[1], out.twist[1]);
    return true;
}

// The wrist's position once the forearm is posed: where the solved forearm puts the
// reference wrist. Equals Input::wrist for every successful pose - the join check.
inline Vec posed_wrist(const Ref& ref, const Output& o) {
    const Quat fore = mul(quat(o.fore.q), conj(quat(ref.fore.q)));
    const Vec off = vec(ref.wristP) - vec(ref.fore.p);
    // The forearm bone's own roll turns about the axis through the wrist: it cannot
    // move the wrist, so undoing it is not needed for the position.
    Vec axis = o.joints.wrist - o.joints.elbow;
    const float k = length(axis) / std::max(1e-6f, length(off));
    return o.joints.elbow + rotate(fore, off) * k;
}

} // namespace bvr::b1r::arm_ik
