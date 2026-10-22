#pragma once
// mirror_math.h - BS1's hands mirrored across the rig, for the gun in the left hand.
// PURE: no engine reads, no globals; tools\tests\mirror-math-tests.cpp runs it on the host.
//
// The hands rig carries both hands, bone for bone (patterns.h): right wrist 27 and fingers
// 28-42, left wrist 6 and fingers 7-21, so left bone 6+k pairs with right bone 27+k. When the
// gun goes to the LEFT controller (bvr::input::left_handed), each hand takes the MIRROR of
// the other's bone layout - the left hand holds the gun the way the right hand did, the right
// hand does what the left did - and each is placed on its own controller.
//
// Three facts make that cheap and exact, given a mirror-symmetric rig:
//   * the mirror Mx is measured from the rig's own BIND pose: the plane halfway between each
//     left bone's rest position and its right twin (bind = inverse(palette) * pose);
//   * the SKIN matrix of a mirrored bone is  P'_left = Mx * P_right * Mx  - the reflection
//     appears on both sides and cancels, so it is a rotation: the mesh is not turned inside
//     out and no cull state changes;
//   * the POSE of a mirrored bone (what the hand placement, the grip calibration and the arm
//     read) is  A'_left = Mx * A_right * Mb,  Mb = inverse(bind_right) * Mx * bind_left - the
//     bone-local fix-up that makes A' consistent with that skin matrix. Mb is constant (binds
//     are), so it is measured once.
// A bone with no twin (43, the right hand's grip the gun hangs from; 44 its tip) is mirrored
// as  Mx * A * F  with F the bone-local reflection across the axis most along the mirror's
// normal, so the attached gun is moved to the mirrored grip as a solid object, never flipped.
//
// Conventions: an Aff is x' = R x + t, R row-major 3x3 (R[r*3+c]). A PALETTE ENTRY is BS1's
// row-vector skin matrix (palette_math.h): rows 0-2 the images of the bind axes, row 3 the
// translation. A FRAME is a bone's (q, p, s) - rotation, position, per-axis scale.

#include "game/bioshock1r/arm_ik.h"

#include <cmath>

namespace bvr::b1r::mirror {

struct Aff {
    float R[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    float t[3] = {0, 0, 0};
};

inline Aff mul(const Aff& a, const Aff& b) { // a after b
    Aff o;
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c)
            o.R[r * 3 + c] = a.R[r * 3 + 0] * b.R[0 * 3 + c] + a.R[r * 3 + 1] * b.R[1 * 3 + c] +
                             a.R[r * 3 + 2] * b.R[2 * 3 + c];
        o.t[r] = a.R[r * 3 + 0] * b.t[0] + a.R[r * 3 + 1] * b.t[1] + a.R[r * 3 + 2] * b.t[2] + a.t[r];
    }
    return o;
}

inline float det(const float m[9]) {
    return m[0] * (m[4] * m[8] - m[5] * m[7]) - m[1] * (m[3] * m[8] - m[5] * m[6]) +
           m[2] * (m[3] * m[7] - m[4] * m[6]);
}

inline bool inverse(const Aff& a, Aff* out) {
    const float* m = a.R;
    const float d = det(m);
    if (!(std::fabs(d) > 1e-12f)) return false;
    const float id = 1.0f / d;
    float* o = out->R;
    o[0] = (m[4] * m[8] - m[5] * m[7]) * id;
    o[1] = (m[2] * m[7] - m[1] * m[8]) * id;
    o[2] = (m[1] * m[5] - m[2] * m[4]) * id;
    o[3] = (m[5] * m[6] - m[3] * m[8]) * id;
    o[4] = (m[0] * m[8] - m[2] * m[6]) * id;
    o[5] = (m[2] * m[3] - m[0] * m[5]) * id;
    o[6] = (m[3] * m[7] - m[4] * m[6]) * id;
    o[7] = (m[1] * m[6] - m[0] * m[7]) * id;
    o[8] = (m[0] * m[4] - m[1] * m[3]) * id;
    for (int r = 0; r < 3; ++r)
        out->t[r] = -(o[r * 3 + 0] * a.t[0] + o[r * 3 + 1] * a.t[1] + o[r * 3 + 2] * a.t[2]);
    return true;
}

inline arm_ik::Vec apply(const Aff& a, arm_ik::Vec p) {
    return {a.R[0] * p.x + a.R[1] * p.y + a.R[2] * p.z + a.t[0], a.R[3] * p.x + a.R[4] * p.y + a.R[5] * p.z + a.t[1],
            a.R[6] * p.x + a.R[7] * p.y + a.R[8] * p.z + a.t[2]};
}

// BS1's row-vector skin matrix <-> Aff. The w column (m[3], m[7], m[11], m[15]) is left as is.
inline Aff from_palette(const float* m) {
    Aff a;
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) a.R[r * 3 + c] = m[c * 4 + r];
    a.t[0] = m[12];
    a.t[1] = m[13];
    a.t[2] = m[14];
    return a;
}
inline void to_palette(const Aff& a, float* m) {
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) m[c * 4 + r] = a.R[r * 3 + c];
    m[12] = a.t[0];
    m[13] = a.t[1];
    m[14] = a.t[2];
}

// A frame (q, p, s) <-> Aff: x' = rotate(q, s * x) + p.
inline Aff from_frame(const float q[4], const float p[3], const float s[3]) {
    const arm_ik::Quat qq = arm_ik::normalized(arm_ik::quat(q));
    Aff a;
    const arm_ik::Vec e[3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    for (int c = 0; c < 3; ++c) {
        const arm_ik::Vec col = arm_ik::rotate(qq, e[c]) * s[c];
        a.R[0 * 3 + c] = col.x;
        a.R[1 * 3 + c] = col.y;
        a.R[2 * 3 + c] = col.z;
    }
    a.t[0] = p[0];
    a.t[1] = p[1];
    a.t[2] = p[2];
    return a;
}
// Back to a frame: the rotation of R with the scale divided out per column. R must be a
// rotation times a positive per-axis scale (det > 0) - the mirrored poses are, by construction.
inline bool to_frame(const Aff& a, float q[4], float p[3], float s[3]) {
    float m[9];
    for (int c = 0; c < 3; ++c) {
        const float len = std::sqrt(a.R[0 * 3 + c] * a.R[0 * 3 + c] + a.R[1 * 3 + c] * a.R[1 * 3 + c] +
                                    a.R[2 * 3 + c] * a.R[2 * 3 + c]);
        if (!(len > 1e-8f)) return false;
        s[c] = len;
        for (int r = 0; r < 3; ++r) m[r * 3 + c] = a.R[r * 3 + c] / len;
    }
    if (!(det(m) > 0.0f)) return false;
    const arm_ik::Quat qq = arm_ik::from_matrix(m);
    for (int k = 0; k < 4; ++k) q[k] = qq.v[k];
    p[0] = a.t[0];
    p[1] = a.t[1];
    p[2] = a.t[2];
    return true;
}

// The reflection across the plane through c with unit normal n.
inline Aff reflection(arm_ik::Vec n, arm_ik::Vec c) {
    Aff a;
    const float v[3] = {n.x, n.y, n.z};
    for (int r = 0; r < 3; ++r)
        for (int k = 0; k < 3; ++k) a.R[r * 3 + k] = (r == k ? 1.0f : 0.0f) - 2.0f * v[r] * v[k];
    const float d = 2.0f * arm_ik::dot(n, c);
    a.t[0] = d * n.x;
    a.t[1] = d * n.y;
    a.t[2] = d * n.z;
    return a;
}

// The pairs: the hands (left 6+k with right 27+k, k < 16 - wrist, thumb, four fingers) and the
// ARMS (clavicle, upper arm, elbow and the two forearm twists: left 3, 4, 5, 22, 23 with right
// 24, 25, 26, 45, 46). The arms are mirrored with the hands - headset 2026-10-19: with only the
// hands mirrored, each arm's IK measured its forearm from its own elbow to the OTHER side's
// mirrored wrist, and the arms stretched "super far".
constexpr int kHandPairs = 16;
constexpr int kPairs = kHandPairs + 5;
constexpr int kPairLeft[kPairs] = {6,  7,  8,  9,  10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 3,  4,  5,  22, 23};
constexpr int kPairRight[kPairs] = {27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 24, 25, 26, 45, 46};
constexpr int kSockets = 2;
constexpr int kSocketBone[kSockets] = {43, 44};

struct Basis {
    bool valid = false;
    Aff mx;                 // the rig's mirror, component space
    float n[3] = {};        // its unit normal
    Aff mbLeft[kPairs];     // A'[kPairLeft[k]]  = mx * A[kPairRight[k]] * mbLeft[k]
    Aff mbRight[kPairs];    // A'[kPairRight[k]] = mx * A[kPairLeft[k]]  * mbRight[k]
    // How symmetric the rig measured: the mean distance between a left bone's rest position
    // and its right twin's, mirrored, against the hands' size (both in rig units); the worst
    // determinant of the Mb's (a mirror-symmetric rig gives -1 exactly).
    float residual = 0.0f, span = 0.0f, worstDet = 0.0f;
};

// The basis from the rig's rest pose: bind[i] = inverse(palette[i]) * pose[i] for every bone
// of both hands (and the sockets). bind is indexed by skeleton bone; only the hands' are read.
// False (and valid = false) when the rig is not mirror-symmetric enough to trust.
inline bool measure(const Aff* bind, int count, Basis* out) {
    *out = Basis{};
    if (count <= kSocketBone[kSockets - 1]) return false;
    arm_ik::Vec sumD{0, 0, 0}, sumC{0, 0, 0};
    for (int k = 0; k < kPairs; ++k) {
        const arm_ik::Vec l = arm_ik::vec(bind[kPairLeft[k]].t), r = arm_ik::vec(bind[kPairRight[k]].t);
        sumD = sumD + (l - r);
        sumC = sumC + (l + r) * 0.5f;
    }
    const float len = arm_ik::length(sumD);
    if (!(len > 1e-6f)) return false;
    const arm_ik::Vec n = sumD * (1.0f / len);
    const arm_ik::Vec c = sumC * (1.0f / kPairs);
    out->mx = reflection(n, c);
    out->n[0] = n.x;
    out->n[1] = n.y;
    out->n[2] = n.z;
    float res = 0.0f, span = 0.0f, worst = 0.0f;
    for (int k = 0; k < kPairs; ++k) {
        const Aff& bl = bind[kPairLeft[k]];
        const Aff& br = bind[kPairRight[k]];
        res += arm_ik::length(apply(out->mx, arm_ik::vec(br.t)) - arm_ik::vec(bl.t));
        span += arm_ik::length(arm_ik::vec(br.t) - arm_ik::vec(bind[kPairRight[0]].t));
        Aff ir, il;
        if (!inverse(br, &ir) || !inverse(bl, &il)) return false;
        out->mbLeft[k] = mul(ir, mul(out->mx, bl));
        out->mbRight[k] = mul(il, mul(out->mx, br));
        const float d1 = det(out->mbLeft[k].R), d2 = det(out->mbRight[k].R);
        const float w = std::fabs(d1 + 1.0f) > std::fabs(d2 + 1.0f) ? d1 : d2;
        if (k == 0 || std::fabs(w + 1.0f) > std::fabs(worst + 1.0f)) worst = w;
    }
    out->residual = res / kPairs;
    out->span = span / kPairs;
    out->worstDet = worst;
    // Trust it only if the rig really is mirrored: the twins' rest positions agree within 15%
    // of the hand's size, and every Mb is a reflection (det -1, within 10%).
    out->valid = out->span > 1e-6f && out->residual < 0.15f * out->span && std::fabs(out->worstDet + 1.0f) < 0.1f;
    return out->valid;
}

// The mirrored pose of one paired bone: dst = mx * src * mb.
inline Aff mirrored_pose(const Basis& b, const Aff& src, const Aff& mb) { return mul(b.mx, mul(src, mb)); }

// A bone with no twin (the gun's grip 43 and its tip 44), mirrored as a SOLID object: mx * src *
// F, with F the reflection of the bone-local axis that lies most along the mirror's normal IN THIS
// POSE - that is the gun's sideways axis, so its barrel and its top are mirrored and only its
// sideways axis is kept, never its up. (Headset 2026-10-19: chosen from the REST pose instead, a
// different axis was the sideways one there, and the gun came out upside down.)
// The bone-local axis that lies most along the mirror's normal in this pose: the gun's sideways
// axis while it is held. LATCH it per weapon from a resting hold and pass it below - picked every
// frame, a reload that tilts the gun hands the choice to another axis and the gun turns upside
// down mid-reload (headset 2026-10-19).
inline int solid_axis(const Basis& b, const Aff& src) {
    const arm_ik::Vec n{b.n[0], b.n[1], b.n[2]};
    int axis = 0;
    float best = -1.0f;
    for (int a = 0; a < 3; ++a) {
        const arm_ik::Vec col{src.R[0 * 3 + a], src.R[1 * 3 + a], src.R[2 * 3 + a]};
        const float l = arm_ik::length(col);
        if (!(l > 1e-8f)) continue;
        const float al = std::fabs(arm_ik::dot(col * (1.0f / l), n));
        if (al > best) { best = al; axis = a; }
    }
    return axis;
}
inline Aff mirrored_solid(const Basis& b, const Aff& src, int axis) {
    Aff f;
    f.R[axis * 3 + axis] = -1.0f;
    return mul(b.mx, mul(src, f));
}
inline Aff mirrored_solid(const Basis& b, const Aff& src) { return mirrored_solid(b, src, solid_axis(b, src)); }

// A solid mirrored by flipping an ARBITRARY bone-local unit axis `u` (F = I - 2 u u^T): the gun's
// own sideways axis as its socket holds it on the bone. The gun hangs off the grip through the
// socket's rotation, so the plane to flip is the GUN's, not the bone's; flipping a bone axis the
// socket turns away from tilts the gun by twice the turn (headset 2026-10-19: "the gun angle
// isnt quite right"). With u a bone axis this is mirrored_solid(b, src, axis).
inline Aff mirrored_solid_u(const Basis& b, const Aff& src, const float u[3]) {
    Aff f;
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) f.R[r * 3 + c] = (r == c ? 1.0f : 0.0f) - 2.0f * u[r] * u[c];
    return mul(b.mx, mul(src, f));
}

// The same, pinned at a point: `offset`, in the bone's own frame, lands exactly on its mirror image.
// For the gun that point is on the model's own centre plane - the pistol is not built about its
// grip, so mirroring about the grip alone shifted it sideways, into the back of the left hand
// (2026-10-19; bones.cpp m4_pose reads the plane from the weapon's skeleton).
inline Aff pinned(const Basis& b, const Aff& src, Aff m, const float offset[3]);
inline Aff mirrored_solid(const Basis& b, const Aff& src, int axis, const float offset[3]) {
    return pinned(b, src, mirrored_solid(b, src, axis), offset);
}
inline Aff mirrored_solid_u(const Basis& b, const Aff& src, const float u[3], const float offset[3]) {
    return pinned(b, src, mirrored_solid_u(b, src, u), offset);
}
// m (a mirrored solid of src) moved so src's point `offset` lands on its mirror image.
inline Aff pinned(const Basis& b, const Aff& src, Aff m, const float offset[3]) {
    const arm_ik::Vec o{offset[0], offset[1], offset[2]};
    const arm_ik::Vec want = apply(b.mx, apply(src, o));
    Aff r = m;
    r.t[0] = r.t[1] = r.t[2] = 0.0f;
    const arm_ik::Vec has = apply(r, o);
    m.t[0] = want.x - has.x;
    m.t[1] = want.y - has.y;
    m.t[2] = want.z - has.z;
    return m;
}
// The mirrored skin matrix of one paired bone: mx * src * mx (a rotation).
inline Aff mirrored_skin(const Basis& b, const Aff& src) { return mul(b.mx, mul(src, b.mx)); }

} // namespace bvr::b1r::mirror
