#pragma once
// hand_compose.h - BS1's hands placed the Dishonored way: ONE rigid correction per hand,
// applied to the hand's LIVE animated bones, so the wrist sits exactly on the controller
// and everything the animation does inside the hand survives. PURE: no engine reads, no
// globals; tools\tests\hand-compose-tests.cpp and tools\hand-compose-sweep.cpp run it on
// the host. docs/bioshock1/HANDS_DISHONORED.md is the design.
//
// Ported from the Dishonored VR mod: hf::delta_from_target and hf::compose_3x4
// (hands/hand_frame.h), MpBuild and MpAnchorPos (hands/mesh_split.cpp), and the hand-back
// blend - blend_transform, smootherstep, Handoff (anim_policy.h). Dishonored applies D to
// a copy of the skinning palette at draw time; BS1 applies it to the engine's evaluated
// component-space bones, which the renderer and the weapon attachment both read.
//
//   D    = Target * inverse(Source)          Source = the wrist as the engine animated it,
//                                            pivoted on the palm (a fixed point in the
//                                            wrist's frame), so D turns about the palm
//   B[i] = D * A[i]                          every bone of the hand: fingers, the weapon
//                                            attach (43) and its tip (44) included
//
// A weighted skin blend commutes with a common rigid transform, so the fingers' animation
// relative to the wrist is untouched (compose_commutes in the tests pins it).
#include "game/bioshock1r/arm_ik.h"

namespace bvr::b1r::hand_compose {

using arm_ik::Bone;
using arm_ik::Quat;
using arm_ik::Vec;

// A rigid transform: x -> q * x + t. Bones are carried by it as frames.
struct Rigid {
    Quat q;
    Vec t;
};
inline Vec apply(const Rigid& d, Vec p) { return arm_ik::rotate(d.q, p) + d.t; }
inline Rigid mul(const Rigid& a, const Rigid& b) { // a after b
    return {arm_ik::normalized(arm_ik::mul(a.q, b.q)), apply(a, b.t)};
}
inline Rigid inverse(const Rigid& a) {
    const Quat c = arm_ik::conj(a.q);
    return {c, arm_ik::rotate(c, a.t) * -1.0f};
}
inline Rigid frame_of(const Bone& b) {
    return {arm_ik::normalized(arm_ik::quat(b.q)), arm_ik::vec(b.p)};
}
// A bone carried by D: position and rotation; scale is the bone's own (D is rigid).
inline Bone carry(const Rigid& d, const Bone& b) {
    Bone o = b;
    arm_ik::put(apply(d, arm_ik::vec(b.p)), o.p);
    const Quat q = arm_ik::normalized(arm_ik::mul(d.q, arm_ik::normalized(arm_ik::quat(b.q))));
    for (int i = 0; i < 4; ++i) o.q[i] = q.v[i];
    return o;
}

// The palm: a fixed point in the wrist's own frame (Dishonored's VR-183 rigid anchor). The
// correction pivots here, so a wrist rotation turns the hand about the palm and can never
// orbit it - the lever s67 measured on BS1's actor-origin pivot cannot exist.
inline Vec palm_of(const Bone& wrist, Vec palmLocal) { return apply(frame_of(wrist), palmLocal); }

// D from a target frame for the palm. `target.q` is the orientation the wrist must take,
// `target.t` where the palm must be, both component space.
//   D.q = target.q * inverse(source.q)
//   D.t = target.t - D.q * sourcePalm
inline Rigid delta(const Rigid& target, const Bone& sourceWrist, Vec palmLocal) {
    Rigid d;
    d.q = arm_ik::normalized(arm_ik::mul(target.q, arm_ik::conj(arm_ik::normalized(arm_ik::quat(sourceWrist.q)))));
    d.t = target.t - arm_ik::rotate(d.q, palm_of(sourceWrist, palmLocal));
    return d;
}

// ---- the hand-back blend (Dishonored anim_policy.h) ------------------------------------
inline float smootherstep(float t) {
    t = t < 0 ? 0 : t > 1 ? 1 : t;
    return t * t * t * (t * (t * 6 - 15) + 10);
}
inline Quat slerp_identity(const Quat& q, float w) { // identity at 0, q at 1
    Quat a = arm_ik::normalized(q);
    if (a.v[3] < 0)
        for (float& c : a.v) c = -c;
    const float ang = 2.0f * acosf(std::min(1.0f, a.v[3]));
    const float s = sqrtf(std::max(0.0f, 1.0f - a.v[3] * a.v[3]));
    if (s < 1e-6f || ang < 1e-6f) return {};
    const Vec axis{a.v[0] / s, a.v[1] / s, a.v[2] / s};
    return arm_ik::axis_angle(axis, ang * w);
}
// weight 1 = the controller (D), 0 = the game (identity). The palm travels a STRAIGHT line
// between where the game puts it and where the controller does (Dishonored's SmoothBlend
// palm path), and the rotation turns about it.
inline Rigid blend(const Rigid& d, float weight, Vec palm) {
    if (weight >= 1) return d;
    if (weight <= 0) return {};
    Rigid o;
    o.q = slerp_identity(d.q, weight);
    const Vec moved = apply(d, palm);
    const Vec at = palm + (moved - palm) * weight;
    o.t = at - arm_ik::rotate(o.q, palm);
    return o;
}

// Who owns a hand, with Dishonored's release hysteresis and eased hand-back
// (anim_policy.h Handoff, SmoothBlend form): 250 ms release, 250 ms to the game, 350 ms
// back to the controller, a reversal mid-blend covering only the remaining distance.
struct Handoff {
    bool game = false, releasing = false;
    unsigned long long releaseAt = 0, blendAt = 0;
    float from = 1, target = 1;
    unsigned inMs = 250, outMs = 350, releaseMs = 250;
    unsigned span() const {
        const unsigned full = target < 0.5f ? inMs : outMs;
        const float part = fabsf(target - from);
        return (unsigned)(full * (part < 1 ? part : 1) + 0.5f);
    }
    float weight(unsigned long long now) const {
        const unsigned d = span();
        if (!d || now >= blendAt + d) return target;
        const float t = now > blendAt ? float(now - blendAt) / d : 0;
        return from + (target - from) * smootherstep(t);
    }
    // `owned`: a hand-back state is active for this hand right now.
    void update(bool owned, unsigned long long now) {
        if (owned) {
            game = true;
            releasing = false;
        } else if (game) {
            if (!releasing) {
                releasing = true;
                releaseAt = now;
            }
            if (now - releaseAt >= releaseMs) {
                game = false;
                releasing = false;
            }
        }
        const float next = game ? 0.0f : 1.0f;
        if (next != target) {
            from = weight(now);
            target = next;
            blendAt = now;
        }
    }
};

// ---- carrying mode 3's tuning across ---------------------------------------------------
// Mode 3 places the wrist at Actor(controller, offsets) * ref[wrist]. Expressed against
// the controller that is one fixed rigid transform - the grip the per-weapon profiles were
// tuned to - so Target = controller * grip reproduces mode 3's settled hand exactly, and
// keeps it there through every animation.
inline Rigid grip_from(const Rigid& controllerWorld, const Rigid& wristWorld) {
    return mul(inverse(controllerWorld), wristWorld);
}

} // namespace bvr::b1r::hand_compose
