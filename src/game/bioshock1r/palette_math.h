#pragma once
// s88: the pure math of the palette route (palette.cpp, bones.cpp m4_compose), in one header so
// tools\tests\palette-math-tests.cpp tests the production code itself.
//
// Conventions. A PALETTE ENTRY is BS1's skin matrix: 16 floats, ROW-VECTOR - rows 0-2 the images
// of the bind-space axes, row 3 the translation, p' = x*R0 + y*R1 + z*R2 + R3 (patterns.h "THE
// SKIN PALETTE"). A SIM is a similarity x' = s * rotate(q, x) + t. An AFFINE is column form
// [R | t], row-major 3x4, x' = R x + t (R may carry scale or shear).

#include "game/bioshock1r/arm_ik.h"

#include <cmath>

namespace bvr::b1r::palette_math {

struct Sim {
    float q[4] = {0, 0, 0, 1};
    float s = 1.0f;
    float t[3] = {};
};

inline arm_ik::Quat nq(const float* q) { return arm_ik::normalized(arm_ik::quat(q)); }

// x' = s R x + t on a palette entry: rows 0-2 are directions, row 3 a point.
inline void apply_sim(const Sim& g, float* m) {
    const arm_ik::Quat q = nq(g.q);
    for (int r = 0; r < 3; ++r) {
        const arm_ik::Vec o = arm_ik::rotate(q, arm_ik::Vec{m[r * 4 + 0], m[r * 4 + 1], m[r * 4 + 2]}) * g.s;
        m[r * 4 + 0] = o.x;
        m[r * 4 + 1] = o.y;
        m[r * 4 + 2] = o.z;
    }
    const arm_ik::Vec o = arm_ik::rotate(q, arm_ik::Vec{m[12], m[13], m[14]}) * g.s;
    m[12] = o.x + g.t[0];
    m[13] = o.y + g.t[1];
    m[14] = o.z + g.t[2];
}
inline arm_ik::Vec sim_point(const Sim& g, arm_ik::Vec p) {
    const arm_ik::Vec o = arm_ik::rotate(nq(g.q), p) * g.s;
    return {o.x + g.t[0], o.y + g.t[1], o.z + g.t[2]};
}
inline Sim sim_mul(const Sim& a, const Sim& b) { // a after b
    Sim o;
    const arm_ik::Quat qa = nq(a.q);
    const arm_ik::Quat q = arm_ik::normalized(arm_ik::mul(qa, nq(b.q)));
    for (int c = 0; c < 4; ++c) o.q[c] = q.v[c];
    o.s = a.s * b.s;
    const arm_ik::Vec t = arm_ik::rotate(qa, arm_ik::vec(b.t)) * a.s;
    o.t[0] = t.x + a.t[0];
    o.t[1] = t.y + a.t[1];
    o.t[2] = t.z + a.t[2];
    return o;
}
inline Sim sim_inv(const Sim& a) {
    Sim o;
    const arm_ik::Quat c = arm_ik::conj(nq(a.q));
    for (int i = 0; i < 4; ++i) o.q[i] = c.v[i];
    o.s = a.s > 1e-6f ? 1.0f / a.s : 1.0f;
    const arm_ik::Vec t = arm_ik::rotate(c, arm_ik::vec(a.t)) * -o.s;
    o.t[0] = t.x;
    o.t[1] = t.y;
    o.t[2] = t.z;
    return o;
}
// L * S * inverse(L): a correction in L's local space, carried to the space L maps into
// (Dishonored WaPublishCommon: D_world = L_hand * D * inverse(L_hand)).
inline Sim sim_world(const Sim& L, const Sim& S) { return sim_mul(L, sim_mul(S, sim_inv(L))); }

// A hand's correction: x' = palmAt + s * rotate(q, x - pivot) - as a Sim.
inline Sim hand_sim(const float q[4], float s, const float pivot[3], const float palmAt[3]) {
    Sim g;
    for (int c = 0; c < 4; ++c) g.q[c] = q[c];
    g.s = s;
    const arm_ik::Vec pv = arm_ik::rotate(nq(q), arm_ik::vec(pivot)) * s;
    g.t[0] = palmAt[0] - pv.x;
    g.t[1] = palmAt[1] - pv.y;
    g.t[2] = palmAt[2] - pv.z;
    return g;
}

// The hand-back blend of a hand correction (Dishonored blend_transform_palm with the model
// scale): at w the rotation is slerped from identity, the scale lerped from 1, and the PALM
// moves on the straight line from its game position to the target. w = 1: the full correction;
// w = 0: identity, the game's own hand.
struct Blended {
    arm_ik::Quat q;
    float s = 1.0f;
    arm_ik::Vec palmAt;
};
inline arm_ik::Quat slerp_identity(const arm_ik::Quat& q, float w) {
    arm_ik::Quat a = arm_ik::normalized(q);
    if (a.v[3] < 0)
        for (float& c : a.v) c = -c;
    if (w >= 1.0f) return a;
    const float cw = a.v[3] > 1.0f ? 1.0f : a.v[3];
    const float ang = 2.0f * acosf(cw);
    const float sn = sqrtf(1.0f - cw * cw > 0.0f ? 1.0f - cw * cw : 0.0f);
    if (sn < 1e-6f || ang < 1e-6f || w <= 0.0f) return arm_ik::Quat{};
    const arm_ik::Vec axis{a.v[0] / sn, a.v[1] / sn, a.v[2] / sn};
    return arm_ik::axis_angle(axis, ang * w);
}
inline Blended blend_hand(const arm_ik::Quat& dq, float s, arm_ik::Vec palm, arm_ik::Vec target, float w) {
    Blended b;
    b.q = w >= 1.0f ? arm_ik::normalized(dq) : slerp_identity(dq, w);
    b.s = 1.0f + (s - 1.0f) * w;
    b.palmAt = palm + (target - palm) * w;
    return b;
}

// Every vertex weighted to this entry lands on one point (a hidden sleeve, hands only).
inline void collapse(const float at[3], float* m) {
    for (int r = 0; r < 3; ++r) m[r * 4 + 0] = m[r * 4 + 1] = m[r * 4 + 2] = 0.0f;
    m[12] = at[0];
    m[13] = at[1];
    m[14] = at[2];
}

// The column-form affine applied to a palette entry (rows 0-2 directions, row 3 a point).
inline void apply_affine(const float* T, float* m) {
    for (int r = 0; r < 4; ++r) {
        const float x = m[r * 4 + 0], y = m[r * 4 + 1], z = m[r * 4 + 2];
        for (int i = 0; i < 3; ++i) {
            float v = T[i * 4 + 0] * x + T[i * 4 + 1] * y + T[i * 4 + 2] * z;
            if (r == 3) v += T[i * 4 + 3];
            m[r * 4 + i] = v;
        }
    }
}

// The IK arm (Dishonored arm_ik_draw.inc) on a palette: the affine that carries the game's bone
// M(a) = [R(qa) diag(sa) | pa] to the solved one M(o) = [R(qo) diag(sa * so) | po], where `so` is
// the solver's own per-axis scale on top of the game's: T = M(o) * inverse(M(a)) =
// [R(qo) diag(so) R(qa)^T | po - L pa]. Lerped toward identity by w (Dishonored
// ArmIKGameArmInAnim): w = 0 leaves the game's arm exactly.
inline void arm_affine(const float pa[3], const float qa[4], const float po[3], const float qo[4], const float so[3],
                       float w, float T[12]) {
    const arm_ik::Quat a = nq(qa), o = nq(qo);
    float L[3][3];
    for (int c = 0; c < 3; ++c) {
        const arm_ik::Vec e{c == 0 ? 1.0f : 0.0f, c == 1 ? 1.0f : 0.0f, c == 2 ? 1.0f : 0.0f};
        const arm_ik::Vec la = arm_ik::rotate(arm_ik::conj(a), e);
        const arm_ik::Vec col = arm_ik::rotate(o, arm_ik::Vec{la.x * so[0], la.y * so[1], la.z * so[2]});
        L[0][c] = col.x;
        L[1][c] = col.y;
        L[2][c] = col.z;
    }
    for (int r = 0; r < 3; ++r) {
        const float t = po[r] - (L[r][0] * pa[0] + L[r][1] * pa[1] + L[r][2] * pa[2]);
        for (int c = 0; c < 3; ++c) {
            const float id = r == c ? 1.0f : 0.0f;
            T[r * 4 + c] = id + (L[r][c] - id) * w;
        }
        T[r * 4 + 3] = t * w;
    }
}

// A palette entry applied to a point, and its inverse (the 3x3 inverted).
inline void entry_point(const float* m, const float* x, float* out) {
    for (int i = 0; i < 3; ++i) out[i] = x[0] * m[0 * 4 + i] + x[1] * m[1 * 4 + i] + x[2] * m[2 * 4 + i] + m[12 + i];
}
inline bool entry_inverse_point(const float* m, const float* y, float* out) {
    const float a = m[0], b = m[4], c = m[8], d = m[1], e = m[5], f = m[9], g = m[2], h = m[6], k = m[10];
    const float det = a * (e * k - f * h) - b * (d * k - f * g) + c * (d * h - e * g);
    if (!(fabsf(det) > 1e-12f)) return false;
    const float q[3] = {y[0] - m[12], y[1] - m[13], y[2] - m[14]};
    const float inv = 1.0f / det;
    out[0] = ((e * k - f * h) * q[0] - (b * k - c * h) * q[1] + (b * f - c * e) * q[2]) * inv;
    out[1] = (-(d * k - f * g) * q[0] + (a * k - c * g) * q[1] - (a * f - c * d) * q[2]) * inv;
    out[2] = ((d * h - e * g) * q[0] - (a * h - b * g) * q[1] + (a * e - b * d) * q[2]) * inv;
    return true;
}

// VR-183: the palm anchor in the wrist bone's frame, from the palette. The palm bones' heads in
// bind space are P_j^-1(their posed heads); their mean, carried by the WRIST's entry alone, is the
// rigid palm, expressed in the wrist pose's frame (position wp, rotation wq).
inline bool rigid_palm(const float* const entries[5], const float heads[5][3], const float* wristEntry,
                       const float wp[3], const float wq[4], float out[3]) {
    float bind[3] = {0, 0, 0};
    for (int j = 0; j < 5; ++j) {
        float hb[3];
        if (!entry_inverse_point(entries[j], heads[j], hb)) return false;
        for (int i = 0; i < 3; ++i) bind[i] += hb[i] * 0.2f;
    }
    float now[3];
    entry_point(wristEntry, bind, now);
    const arm_ik::Vec local =
        arm_ik::rotate(arm_ik::conj(nq(wq)), arm_ik::Vec{now[0] - wp[0], now[1] - wp[1], now[2] - wp[2]});
    out[0] = local.x;
    out[1] = local.y;
    out[2] = local.z;
    return true;
}

} // namespace bvr::b1r::palette_math
