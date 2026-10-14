// hand-compose-tests.cpp - host tests for src/game/bioshock1r/hand_compose.h, BS1's hands
// placed the Dishonored way. Run with `.\tools\host-test.ps1 hand-compose`.
//
// What has to hold for the port to be Dishonored's:
//   - the wrist lands EXACTLY on the target orientation, and the palm on the target point,
//     whatever pose the animation left the wrist in
//   - every other bone keeps its pose RELATIVE TO THE WRIST (the fingers animate)
//   - the correction pivots on the palm: changing only the target's rotation moves the palm
//     nowhere (no orbit - the s67 lever)
//   - the blend is the identity at 0 and D at 1, the palm travels a straight line between,
//     and the Handoff state machine has Dishonored's timings
//   - a NEGATIVE control: composing with the inverse in the wrong order must fail the
//     wrist check
#include "game/bioshock1r/hand_compose.h"

#include <cstdio>

using namespace bvr::b1r;
using namespace bvr::b1r::hand_compose;
using arm_ik::axis_angle;
using arm_ik::vec;

static int g_checks = 0, g_fails = 0;
static void check(bool ok, const char* what, double got = 0, double want = 0) {
    ++g_checks;
    if (!ok) {
        ++g_fails;
        printf("  FAIL %s: got %.6f want %.6f\n", what, got, want);
    }
}
// The angle between two rotations. atan2 of the vector part, not acos of w: acos cannot
// resolve angles below ~7e-4 rad in single precision.
static float qdiff(const Quat& a, const Quat& b) {
    const Quat d = arm_ik::mul(a, arm_ik::conj(b));
    const float s = sqrtf(d.v[0] * d.v[0] + d.v[1] * d.v[1] + d.v[2] * d.v[2]);
    return 2 * atan2f(s, fabsf(d.v[3]));
}
static Bone bone(Vec p, Quat q) {
    Bone b;
    arm_ik::put(p, b.p);
    for (int i = 0; i < 4; ++i) b.q[i] = q.v[i];
    return b;
}
// The relative frame of b in a: inverse(a) * b.
static Rigid rel(const Bone& a, const Bone& b) { return mul(inverse(frame_of(a)), frame_of(b)); }

int main() {
    const Vec palmLocal{8, 1.5f, -2};
    for (int i = 0; i < 300; ++i) {
        // An animated hand: a wrist somewhere, turned somehow, and three "fingers" off it.
        const Quat wq = axis_angle({cosf(i * 0.3f), sinf(i * 0.7f), 0.4f}, 0.01f * i);
        const Bone wrist = bone({10.0f + i * 0.1f, -20, 30 - i * 0.05f}, wq);
        Bone finger[3];
        for (int k = 0; k < 3; ++k)
            finger[k] = bone(arm_ik::vec(wrist.p) + arm_ik::rotate(wq, {11.0f + k, -3.0f + 3 * k, 0.5f}),
                             arm_ik::mul(wq, axis_angle({0, 1, 0}, 0.2f * k + 0.003f * i)));
        // The controller target.
        const Rigid target{axis_angle({0.2f, -1, 0.5f}, 1.1f + 0.002f * i), {-5, 40.0f + i * 0.01f, 12}};
        const Rigid D = delta(target, wrist, palmLocal);
        const Bone w2 = carry(D, wrist);
        check(qdiff(frame_of(w2).q, target.q) < 1e-4f, "wrist takes the target orientation",
              qdiff(frame_of(w2).q, target.q), 0);
        check(arm_ik::length(palm_of(w2, palmLocal) - target.t) < 1e-3f, "palm lands on the target point",
              arm_ik::length(palm_of(w2, palmLocal) - target.t), 0);
        for (int k = 0; k < 3; ++k) {
            const Bone f2 = carry(D, finger[k]);
            const Rigid r0 = rel(wrist, finger[k]), r1 = rel(w2, f2);
            check(arm_ik::length(r0.t - r1.t) < 1e-3f && qdiff(r0.q, r1.q) < 1e-4f,
                  "fingers keep their pose relative to the wrist", arm_ik::length(r0.t - r1.t), 0);
        }
        // No orbit: turn only the target's rotation, and the palm stays where it was put.
        const Rigid turned{arm_ik::mul(axis_angle({1, 0, 0}, 1.7f), target.q), target.t};
        const Bone w3 = carry(delta(turned, wrist, palmLocal), wrist);
        check(arm_ik::length(palm_of(w3, palmLocal) - target.t) < 1e-3f, "rotation pivots on the palm",
              arm_ik::length(palm_of(w3, palmLocal) - target.t), 0);
        // The blend.
        const Vec palm = palm_of(wrist, palmLocal);
        const Rigid b0 = blend(D, 0, palm), b1 = blend(D, 1, palm), bh = blend(D, 0.5f, palm);
        check(qdiff(b0.q, {}) < 1e-5f && arm_ik::length(b0.t) < 1e-5f, "blend 0 is the game's pose");
        check(qdiff(b1.q, D.q) < 1e-5f && arm_ik::length(b1.t - D.t) < 1e-5f, "blend 1 is the controller's");
        const Vec mid = apply(bh, palm), want = palm + (apply(D, palm) - palm) * 0.5f;
        check(arm_ik::length(mid - want) < 1e-3f, "the palm travels a straight line", arm_ik::length(mid - want), 0);
        check(fabsf(qdiff(bh.q, {}) - 0.5f * qdiff(D.q, {})) < 1e-3f, "half the blend is half the turn",
              qdiff(bh.q, {}), 0.5f * qdiff(D.q, {}));
        // NEGATIVE CONTROL: the inverse on the wrong side must miss.
        Rigid wrong;
        wrong.q = arm_ik::mul(arm_ik::conj(arm_ik::quat(wrist.q)), target.q);
        wrong.t = target.t - arm_ik::rotate(wrong.q, palm);
        if (qdiff(wq, {}) > 0.3f)
            check(qdiff(frame_of(carry(wrong, wrist)).q, target.q) > 1e-3f, "negative control: the wrong order misses");
    }
    {   // grip_from reproduces a placement: controller * grip lands where the wrist was.
        const Rigid controller{axis_angle({0, 0, 1}, 0.8f), {100, -30, 55}};
        const Rigid wrist{axis_angle({1, 1, 0}, -0.4f), {120, -10, 50}};
        const Rigid g = grip_from(controller, wrist);
        const Rigid back = mul(controller, g);
        check(qdiff(back.q, wrist.q) < 1e-5f && arm_ik::length(back.t - wrist.t) < 1e-3f, "grip_from round-trips");
        const Rigid moved{arm_ik::mul(axis_angle({0, 1, 0}, 0.5f), controller.q), controller.t + Vec{5, 5, 5}};
        const Rigid follows = mul(moved, g);
        check(arm_ik::length(follows.t - (apply(moved, arm_ik::rotate(arm_ik::conj(controller.q), wrist.t - controller.t)))) < 1e-3f,
              "the grip rides the controller rigidly");
    }
    {   // Handoff: Dishonored's timings.
        Handoff h;
        h.update(false, 1000);
        check(h.weight(1000) == 1, "starts on the controller");
        h.update(true, 2000);
        check(h.weight(2000) == 1 && h.weight(2125) < 1 && h.weight(2125) > 0, "hands back over 250 ms", h.weight(2125), 0.5);
        check(h.weight(2250) == 0, "the game owns it after 250 ms", h.weight(2250), 0);
        h.update(false, 3000);
        check(h.weight(3200) == 0, "release holds for 250 ms", h.weight(3200), 0);
        h.update(false, 3250);
        check(h.weight(3250 + 175) > 0.4f && h.weight(3250 + 175) < 0.6f, "returns over 350 ms", h.weight(3425), 0.5);
        check(h.weight(3250 + 350) == 1, "back on the controller", h.weight(3600), 1);
        Handoff r;
        r.update(true, 0);
        r.update(false, 125); // reversal mid-blend, before the release
        r.update(false, 375);
        check(r.weight(375) <= 0.0001f + r.weight(375) && r.target == 1, "a reversal re-targets the controller");
        check(fabsf(smootherstep(0.5f) - 0.5f) < 1e-6f && smootherstep(0) == 0 && smootherstep(1) == 1, "smootherstep");
    }
    {   // An animation at the gun (2026-10-14): the free hand carried by the gun hand.
        // The clip: a gun-hand palm and a free-hand palm with a magazine bone off it.
        const Vec gunPivot{20, 5, -3}, freePivot{34, -12, 6}, mag{38, -10, 9};
        const Placement gun{axis_angle({0.3f, 1, -0.2f}, 0.9f), {-50, 80, 20}, 1.06f};
        const Placement carried = carried_by(gun, gunPivot, freePivot);
        // Every free-hand bone lands where the GUN's placement would put it: the clip's
        // free hand against the gun, wherever the gun is held.
        check(arm_ik::length(place(carried, freePivot, mag) - place(gun, gunPivot, mag)) < 1e-3f,
              "a carried bone stands where the gun hand's placement puts it");
        check(arm_ik::length(place(carried, freePivot, freePivot) - place(gun, gunPivot, freePivot)) < 1e-3f,
              "the carried palm stands where the gun hand's placement puts it");
        // So the free hand's distance to the gun hand is the clip's, scaled by the size.
        const float clipDist = arm_ik::length(freePivot - gunPivot);
        const float drawnDist = arm_ik::length(place(carried, freePivot, freePivot) - gun.at);
        check(fabsf(drawnDist - clipDist * gun.s) < 1e-3f, "the clip's spacing survives the carry", drawnDist,
              clipDist * gun.s);
        // toward: exact at its ends, and the palm on the straight line between.
        const Placement own{axis_angle({1, 0, 0}, -0.4f), {10, 10, 10}, 1.0f};
        const Placement a = toward(own, carried, 0), b = toward(own, carried, 1), m = toward(own, carried, 0.5f);
        check(qdiff(a.q, own.q) < 1e-6f && arm_ik::length(a.at - own.at) < 1e-6f && a.s == own.s, "toward 0 is the own");
        check(qdiff(b.q, carried.q) < 1e-6f && arm_ik::length(b.at - carried.at) < 1e-6f && b.s == carried.s,
              "toward 1 is the carried");
        check(arm_ik::length(m.at - (own.at + carried.at) * 0.5f) < 1e-3f, "the palm goes the straight line");
        check(fabsf(qdiff(m.q, own.q) - qdiff(m.q, carried.q)) < 1e-3f, "the turn is halfway at 0.5",
              qdiff(m.q, own.q), qdiff(m.q, carried.q));
        // from_rest: with no motion since the rest it is the pinned placement; the clip's motion
        // since the rest moves the palm by that motion, carried onto the controller.
        const Bone rest = bone({12, -18, 30}, axis_angle({0.1f, 0.4f, 1}, 0.7f));
        const Rigid target{axis_angle({0.2f, -1, 0.5f}, 1.1f), {-5, 40, 12}};
        const float s = 1.06f;
        const Rigid pinned = delta(target, rest, palmLocal);
        const Placement still = from_rest(target, rest, palmLocal, palm_of(rest, palmLocal), s);
        check(qdiff(still.q, pinned.q) < 1e-6f && arm_ik::length(still.at - target.t) < 1e-4f,
              "from_rest at rest is the pinned hand");
        const Rigid clipMove{axis_angle({0, 0, 1}, 0.3f), {2, -1, 4}}; // the clip turns and lifts the hand
        const Bone moved = carry(clipMove, rest);
        const Vec livePalm = palm_of(moved, palmLocal);
        const Placement anim = from_rest(target, rest, palmLocal, livePalm, s);
        // The live palm lands where the rest palm's correction carries the clip's motion.
        check(arm_ik::length(anim.at - (target.t + arm_ik::rotate(pinned.q, (livePalm - palm_of(rest, palmLocal)) * s))) <
                  1e-3f,
              "from_rest moves the palm by the clip's motion since the rest");
        // And the wrist turns by the clip's turn relative to the controller: q * moved wrist.
        const Quat drawnWrist = arm_ik::mul(anim.q, arm_ik::normalized(arm_ik::quat(moved.q)));
        const Quat pinnedWrist = arm_ik::mul(pinned.q, arm_ik::normalized(arm_ik::quat(rest.q)));
        check(fabsf(qdiff(drawnWrist, pinnedWrist) - 0.3f) < 1e-3f, "the gun turns by the clip's turn",
              qdiff(drawnWrist, pinnedWrist), 0.3f);
        // NEGATIVE CONTROL: carrying with the free hand's own pivot in place of the gun's misses.
        const Placement wrongCarry{gun.q, place(gun, freePivot, freePivot), gun.s};
        check(arm_ik::length(place(wrongCarry, freePivot, mag) - place(gun, gunPivot, mag)) > 1.0f,
              "negative control: the wrong pivot misses");
    }
    printf("hand-compose: %d checks, %d failures\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
