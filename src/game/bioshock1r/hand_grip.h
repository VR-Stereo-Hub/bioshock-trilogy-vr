#pragma once
// s89: WHERE THE HAND TURNS ABOUT, AND WHICH WAY IT FACES - the fist, matched to the controller's
// grip pose (OpenXR /input/grip/pose: origin at the centre of the grasped handle, -Z along the
// handle from the little finger to the thumb side, +X along the palm normal - into the palm on
// the right hand, away from it on the left).
//
// tools\blender\hand_pivot.py measured the BS1 hands rig in its settled wrench grip
// (Wrench__EquipWrench, last frame): each finger's curl circle (through its three joints and its
// tip), the handle axis through the index/middle/ring/pinky curl centres, the fist centre on it,
// and the palm normal. The drive had pinned the KNUCKLE centroid (the hand and the four finger
// bases) - 4.0 rig units from the fist centre, on the back of the hand toward the wrist - and
// oriented the hand by rotation trims tuned by eye; so a turn of the controller swung the drawn
// fist around the real one ("90 deg one way is fine, the other way desyncs"). Pinning the FIST
// CENTRE and aligning its handle axis and palm normal with the grip's makes the drawn fist turn
// about the real one.
//
// Every number is in a frame made from bone HEADS only - origin the wrist, x toward the middle
// finger base, y from the little finger base toward the index base, z = x cross y (times the side:
// +1 right, -1 left, so one set of numbers serves both mirrored hands) - so it is rebuilt here from
// the evaluated pose with no Blender axis convention in between, and scaled by the live
// wrist-to-middle-base distance against the measured one.

#include "game/bioshock1r/arm_ik.h"

#include <cmath>

namespace bvr::b1r::hand_grip {

using arm_ik::Quat;
using arm_ik::Vec;

// hand_pivot.py, right hand, Wrench__EquipWrench last frame (HANDS_DISHONORED.md s89).
inline constexpr float kWristToMiddleBase = 12.0986f; // the frame's unit at measurement
inline constexpr float kFistCentre[3] = {10.8726f, 0.4981f, -3.6547f};
inline constexpr float kHandleAxis[3] = {0.1453f, 0.9885f, -0.0421f}; // little finger -> index
inline constexpr float kPalmNormalOut[3] = {0.4091f, -0.0987f, -0.9072f}; // out of the palm

struct Frame {
    Vec o, x, y, z;
    float scale = 1.0f; // live wrist-to-middle-base over the measured one
};

inline bool head_frame(Vec wrist, Vec index1, Vec middle1, Vec pinky1, float side, Frame* f) {
    Vec x = middle1 - wrist;
    const float lx = arm_ik::length(x);
    if (!(lx > 1e-4f)) return false;
    x = x * (1.0f / lx);
    Vec y = index1 - pinky1;
    y = y - x * arm_ik::dot(y, x);
    const float ly = arm_ik::length(y);
    if (!(ly > 1e-4f)) return false;
    y = y * (1.0f / ly);
    f->o = wrist;
    f->x = x;
    f->y = y;
    f->z = arm_ik::cross(x, y) * side;
    f->scale = lx / kWristToMiddleBase;
    return true;
}
inline Vec point(const Frame& f, const float c[3]) {
    return f.o + (f.x * c[0] + f.y * c[1] + f.z * c[2]) * f.scale;
}
inline Vec dir(const Frame& f, const float c[3]) {
    const Vec d = f.x * c[0] + f.y * c[1] + f.z * c[2];
    return d * (1.0f / arm_ik::length(d));
}

// The rotation C that carries the WRIST bone's local frame to the controller's local frame in the
// drive's UE axes (x forward = grip -Z, y right = grip +X, z up = grip +Y), so that the hand's
// world rotation is  controller * C:  C maps the handle axis (wrist-local a) to forward (+x) and the
// outward palm normal (wrist-local n) to -y on the right hand (grip +X is INTO the right palm) and
// to +y on the left (grip +X is AWAY from the left palm).
inline Quat calibration(Vec aLocal, Vec nLocal, float side) {
    Vec a = aLocal * (1.0f / arm_ik::length(aLocal));
    Vec n = nLocal - a * arm_ik::dot(nLocal, a);
    n = n * (1.0f / arm_ik::length(n));
    const Vec b = arm_ik::cross(a, n);
    // source basis (columns): a, n, b ; target basis: ex, -side*ey, ex x (-side*ey) = -side*ez
    const Vec ta{1, 0, 0}, tn{0, -side, 0}, tb{0, 0, -side};
    // C = T * S^T, S = [a n b], T = [ta tn tb]
    float m[3][3];
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) {
            const float tr[3] = {r == 0 ? ta.x : r == 1 ? ta.y : ta.z, r == 0 ? tn.x : r == 1 ? tn.y : tn.z,
                                 r == 0 ? tb.x : r == 1 ? tb.y : tb.z};
            const float sc[3] = {c == 0 ? a.x : c == 1 ? a.y : a.z, c == 0 ? n.x : c == 1 ? n.y : n.z,
                                 c == 0 ? b.x : c == 1 ? b.y : b.z};
            m[r][c] = tr[0] * sc[0] + tr[1] * sc[1] + tr[2] * sc[2];
        }
    // matrix to quaternion (Shepperd)
    Quat q;
    const float t = m[0][0] + m[1][1] + m[2][2];
    if (t > 0) {
        const float s = sqrtf(t + 1.0f) * 2.0f;
        q.v[3] = 0.25f * s;
        q.v[0] = (m[2][1] - m[1][2]) / s;
        q.v[1] = (m[0][2] - m[2][0]) / s;
        q.v[2] = (m[1][0] - m[0][1]) / s;
    } else if (m[0][0] > m[1][1] && m[0][0] > m[2][2]) {
        const float s = sqrtf(1.0f + m[0][0] - m[1][1] - m[2][2]) * 2.0f;
        q.v[3] = (m[2][1] - m[1][2]) / s;
        q.v[0] = 0.25f * s;
        q.v[1] = (m[0][1] + m[1][0]) / s;
        q.v[2] = (m[0][2] + m[2][0]) / s;
    } else if (m[1][1] > m[2][2]) {
        const float s = sqrtf(1.0f + m[1][1] - m[0][0] - m[2][2]) * 2.0f;
        q.v[3] = (m[0][2] - m[2][0]) / s;
        q.v[0] = (m[0][1] + m[1][0]) / s;
        q.v[1] = 0.25f * s;
        q.v[2] = (m[1][2] + m[2][1]) / s;
    } else {
        const float s = sqrtf(1.0f + m[2][2] - m[0][0] - m[1][1]) * 2.0f;
        q.v[3] = (m[1][0] - m[0][1]) / s;
        q.v[0] = (m[0][2] + m[2][0]) / s;
        q.v[1] = (m[1][2] + m[2][1]) / s;
        q.v[2] = 0.25f * s;
    }
    return arm_ik::normalized(q);
}

} // namespace bvr::b1r::hand_grip
