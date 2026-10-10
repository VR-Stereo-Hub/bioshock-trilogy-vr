// arm-ik-tests.cpp - host tests for src/game/bioshock1r/arm_ik.h, the BS1 arm solver.
// Run with `.\tools\host-test.ps1 arm-ik`. Pure math: no game, no engine memory.
//
// The solve checks are the Dishonored VR mod's (tools/arm-ik-tests.cpp there), on this
// header. The pose checks are new: BS1 writes component-space bones, so what has to
// hold is that the written bones meet the shoulder and the wrist at the right lengths,
// that a wrist roll is followed continuously through 360 degrees, that the forearm
// bones share the roll on the ramp, and that the whole thing is frame-covariant (it
// cannot depend on which way the actor - which spins with the controller - faces).
#include "game/bioshock1r/arm_ik.h"

#include <cstdio>
#include <limits>

using namespace bvr::b1r::arm_ik;

static int g_checks = 0, g_fails = 0;
static void check(bool ok, const char* what, double got = 0, double want = 0) {
    ++g_checks;
    if (!ok) {
        ++g_fails;
        printf("  FAIL %s: got %.6f want %.6f\n", what, got, want);
    }
}
static bool near(Vec a, Vec b, float eps = 0.002f) { return length(a - b) < eps; }
static float qangle(const Quat& a, const Quat& b) { // the angle between two rotations
    const Quat d = mul(a, conj(b));
    return 2 * acosf(std::min(1.0f, fabsf(d.v[3])));
}
static void set(Bone& b, Vec p, Quat q) {
    put(p, b.p);
    for (int i = 0; i < 4; ++i) b.q[i] = q.v[i];
}

// A Biped-shaped right arm, like BS1's (+X along each limb bone): upper arm 43.3,
// forearm 30.0, elbow bent 40 degrees, the twist helpers at the elbow and halfway.
static Ref make_ref() {
    Ref r;
    const Vec S{0, 20, 0};
    const Quat qu = axis_angle({0, 0, 1}, -0.3f);
    const Vec E = S + rotate(qu, {43.3f, 0, 0});
    const Quat qf = mul(axis_angle({0, 1, 0}, 0.7f), qu);
    const Vec W = E + rotate(qf, {30.0f, 0, 0});
    set(r.clavicle, S + Vec{-18, -5, 3}, axis_angle({0, 0, 1}, 1.2f));
    set(r.upper, S, qu);
    set(r.fore, E, qf);
    set(r.twist[0], E, qf);
    set(r.twist[1], E + rotate(qf, {15.0f, 0, 0}), qf);
    put(W, r.wristP);
    const Quat qw = mul(axis_angle({1, 0, 0}, 0.2f), qf);
    for (int i = 0; i < 4; ++i) r.wristQ[i] = qw.v[i];
    return r;
}
static Input make_in(const Ref& r, Vec wrist, Quat wristQ) {
    Input in;
    in.shoulder = vec(r.upper.p);
    in.wrist = wrist;
    for (int i = 0; i < 4; ++i) in.wristQ[i] = wristQ.v[i];
    in.pole = {-0.3f, 0.6f, -1};
    in.outward = {0, 1, 0};
    return in;
}
// The forearm's swing with no roll, and its axis: what a wrist with no roll of its own
// is carried by. pose() measures the roll against exactly this.
static Quat seg_of(const Ref& r, const Input& in, Vec& axis) {
    Swing s;
    swing(r, in, in.pole, s);
    axis = s.axis;
    return s.fore;
}
static float seg_err(const Output& o, float a, float b, Vec wrist) {
    return std::max(fabsf(length(o.joints.elbow - o.joints.shoulder) - a),
                    fabsf(length(wrist - o.joints.elbow) - b));
}

int main() {
    // ---- solve() -----------------------------------------------------------
    Solution sol;
    const Vec pole{-0.3f, -1, 0.6f}, outward{0, 0, 1};
    check(solve({0, 0, 0}, {30, 0, 0}, pole, outward, {}, 25, 26, 0.5f, sol), "reachable solve");
    check(near(sol.shoulder, {}) && !sol.reachClamped, "reachable shoulder stays nominal");
    check(near(sol.wrist, {30, 0, 0}), "wrist stays exact");
    for (int i = 0; i < 500; ++i) {
        const Vec w{60 * cosf(i * 0.08f), 50 * sinf(i * 0.11f), 35 * cosf(i * 0.07f)};
        Solution a, b;
        const bool ok = solve({}, w, pole, outward, {}, 25, 26, 0.5f, a);
        check(ok && fabsf(length(a.elbow - a.shoulder) - 25) < 0.003f && fabsf(length(w - a.elbow) - 26) < 0.003f,
              "segment lengths survive reach correction", length(a.elbow - a.shoulder), 25);
        const Quat rot = axis_angle({1, 2, 3}, 0.91f);
        const Vec t{100, -37, 62};
        const bool ok2 = solve(t, rotate(rot, w) + t, rotate(rot, pole), rotate(rot, outward), {}, 25, 26, 0.5f, b);
        check(ok2 && near(b.elbow, rotate(rot, a.elbow) + t, 0.008f), "solve is frame-covariant",
              length(b.elbow - (rotate(rot, a.elbow) + t)), 0);
    }
    check(solve({}, {}, pole, outward, {}, 25, 26, 0.5f, sol) && finite(sol.elbow), "zero reach is deterministic");
    check(sol.shoulderShift > 0 && sol.reachClamped, "a close wrist moves only the shoulder");
    check(solve({}, {200, 0, 0}, pole, outward, {}, 25, 26, 0.5f, sol) && near(sol.wrist, {200, 0, 0}) &&
              sol.shoulderShift > 100,
          "a far wrist stays attached; the shoulder follows");
    check(solve({}, {0, -40, 0}, {0, -1, 0}, {1, 0, 0}, {}, 25, 26, 0.5f, sol) && finite(sol.elbow),
          "a pole along the arm stays finite");
    check(!solve({}, {}, pole, outward, {}, -1, 26, 0.5f, sol), "negative segment refused");
    check(!solve({}, {}, pole, outward, {}, 1, 90, 0.5f, sol), "inconsistent reach interval refused");
    check(!solve({}, {std::numeric_limits<float>::infinity(), 0, 0}, pole, outward, {}, 25, 26, 0.5f, sol),
          "infinity refused");
    {   // the elbow never points inside the arm's own side
        solve({}, {40, 0, 0}, {0, 0, -1}, {0, 0, 1}, {}, 25, 26, 0.5f, sol);
        check(dot(sol.pole, {0, 0, 1}) >= 0.25f - 1e-4f, "elbow kept on the outward side", dot(sol.pole, {0, 0, 1}), 0.25);
    }

    // ---- frame_delta / twist_angle -----------------------------------------
    {
        const Quat id = frame_delta({1, 0, 0}, {0, 0, 1}, {1, 0, 0}, {0, 0, 1});
        check(near(rotate(id, {2, 3, 4}), {2, 3, 4}), "reference frame yields identity");
        const Vec d{0.3f, -0.8f, 0.5f}, n{0.9f, 0.4f, 0.1f};
        const Quat q = frame_delta({1, 0, 0}, {0, 0, 1}, d, n);
        Vec du = d;
        unit(du);
        Vec nu = across(n, du);
        unit(nu);
        check(near(rotate(q, {1, 0, 0}), du) && near(rotate(q, {0, 0, 1}), nu), "frame_delta lands direction and normal");
        check(fabsf(twist_angle(axis_angle({0, 0, 1}, 0.7f), {0, 0, 1}) - 0.7f) < 1e-4f, "twist measures the axial part");
        const Quat mixed = mul(axis_angle({1, 0, 0}, 0.5f), axis_angle({0, 0, 1}, 0.7f));
        check(fabsf(twist_angle(mixed, {0, 0, 1}) - 0.7f) < 0.12f, "twist ignores most of a swing");
        check(fabsf(unwrap(-3.13f, 3.13f) - 3.1531853f) < 0.001f, "unwrap across pi");
    }

    // ---- pose() ------------------------------------------------------------
    const Ref ref = make_ref();
    const float A = length(vec(ref.fore.p) - vec(ref.upper.p)), B = length(vec(ref.wristP) - vec(ref.fore.p));
    const Quat refWrist = quat(ref.wristQ);
    {   // the reference itself: nothing moves, nothing rolls
        Input in = make_in(ref, vec(ref.wristP), refWrist);
        in.pole = vec(ref.fore.p) - (vec(ref.upper.p) + vec(ref.wristP)) * 0.5f;
        in.outward = in.pole;
        Output o;
        check(pose(ref, in, o), "reference pose solves");
        check(near(vec(o.fore.p), vec(ref.fore.p), 0.01f), "reference elbow reproduced", length(vec(o.fore.p) - vec(ref.fore.p)), 0);
        check(qangle(quat(o.upper.q), quat(ref.upper.q)) < 0.002f && qangle(quat(o.fore.q), quat(ref.fore.q)) < 0.002f,
              "reference rotations reproduced", qangle(quat(o.fore.q), quat(ref.fore.q)), 0);
        check(fabsf(o.roll) < 0.002f && o.swivel == 0, "reference wrist has no roll", o.roll, 0);
    }
    {   // a sweep of reachable wrists: lengths, the join, the twist helpers' spacing
        float worst = 0, worstJoin = 0;
        int failures = 0;
        for (int i = 0; i < 400; ++i) {
            const Vec w = vec(ref.upper.p) + Vec{55 * cosf(i * 0.13f), 20 + 30 * sinf(i * 0.07f), 25 * sinf(i * 0.19f)};
            const Quat wq = mul(axis_angle({cosf(i * 0.3f), 1, sinf(i * 0.2f)}, 0.02f * i), refWrist);
            Output o;
            if (!pose(ref, make_in(ref, w, wq), o)) {
                ++failures;
                continue;
            }
            worst = std::max(worst, seg_err(o, A, B, w));
            worstJoin = std::max(worstJoin, length(posed_wrist(ref, o) - w));
            const float helper = length(vec(o.twist[1].p) - vec(o.fore.p));
            check(fabsf(helper - 15.0f) < 0.01f, "mid-forearm helper keeps its offset", helper, 15);
        }
        check(failures == 0, "every reachable wrist poses", failures, 0);
        check(worst < 0.005f, "segment lengths exact", worst, 0);
        check(worstJoin < 0.005f, "forearm meets the wrist", worstJoin, 0);
    }
    {   // a full wrist roll, followed frame to frame
        const Vec w = vec(ref.wristP) + Vec{5, 4, -6};
        Vec axisRef = vec(ref.wristP) - vec(ref.fore.p);
        unit(axisRef);
        float prevTracked = 0, maxStep = 0, maxSwivel = 0;
        bool fresh = false, ok = true;
        Vec priorPole{};
        for (int deg = 0; deg <= 240; deg += 4) {
            Vec axis;
            const Quat seg = seg_of(ref, make_in(ref, w, refWrist), axis);
            const Quat wq = mul(mul(axis_angle(axis, deg * kDeg), seg), refWrist);
            Input in = make_in(ref, w, wq);
            in.fresh = fresh;
            in.priorTwist = prevTracked;
            in.priorPole = priorPole;
            Output o;
            if (!pose(ref, in, o)) {
                ok = false;
                break;
            }
            if (fresh) maxStep = std::max(maxStep, fabsf(o.trackedTwist - prevTracked));
            maxSwivel = std::max(maxSwivel, fabsf(o.swivel));
            if (deg == 60) check(fabsf(o.trackedTwist - 60 * kDeg) < 0.03f, "60 deg roll measured", o.trackedTwist / kDeg, 60);
            if (deg == 60) check(o.swivel == 0, "no swivel under the comfort limit", o.swivel, 0);
            if (deg == 200) check(fabsf(o.trackedTwist - 200 * kDeg) < 0.05f, "200 deg followed past the wrap", o.trackedTwist / kDeg, 200);
            prevTracked = o.trackedTwist;
            priorPole = o.basePole;
            fresh = true;
        }
        check(ok, "the roll sweep poses throughout");
        check(maxStep < 6 * kDeg, "the tracked roll is continuous", maxStep / kDeg, 4);
        check(maxSwivel > 30 * kDeg && maxSwivel <= kMaxSwivel + 1e-4f, "the elbow lifts past the comfort limit", maxSwivel / kDeg, 70);
        {   // without history the same 200 deg reads the short way round
            Vec axis;
            const Quat seg = seg_of(ref, make_in(ref, w, refWrist), axis);
            const Quat wq = mul(mul(axis_angle(axis, 200 * kDeg), seg), refWrist);
            Output o;
            pose(ref, make_in(ref, w, wq), o);
            check(fabsf(o.trackedTwist + 160 * kDeg) < 0.05f, "no history: shortest reading", o.trackedTwist / kDeg, -160);
        }
    }
    {   // the forearm bones share the roll on the ramp: 70% at the elbow, 85% halfway
        const Vec w = vec(ref.wristP) + Vec{3, -2, 4};
        Vec axis;
        Swing s0;
        swing(ref, make_in(ref, w, refWrist), make_in(ref, w, refWrist).pole, s0);
        const Quat seg = seg_of(ref, make_in(ref, w, refWrist), axis); // forearm, no roll
        const Quat wq = mul(mul(axis_angle(axis, 60 * kDeg), seg), refWrist);
        Output o;
        pose(ref, make_in(ref, w, wq), o);
        const float foreRoll = twist_angle(mul(mul(quat(o.fore.q), conj(quat(ref.fore.q))), conj(seg)), axis);
        const float midRoll = twist_angle(mul(mul(quat(o.twist[1].q), conj(quat(ref.twist[1].q))), conj(seg)), axis);
        check(fabsf(foreRoll - 0.7f * 60 * kDeg) < 0.01f, "forearm carries 70% at the elbow", foreRoll / kDeg, 42);
        check(fabsf(midRoll - 0.85f * 60 * kDeg) < 0.01f, "mid helper carries 85%", midRoll / kDeg, 51);
        check(qangle(mul(quat(o.upper.q), conj(quat(ref.upper.q))), s0.upper) < 0.002f,
              "the upper arm does not roll with the wrist");
    }
    {   // covariance: rotate and move the reference AND the targets - the arm follows
        // exactly. This is the property the spinning held actor needs.
        const Quat R = axis_angle({0.2f, -1, 0.4f}, 1.3f);
        const Vec T{-40, 12, 90};
        auto xb = [&](const Bone& b) {
            Bone o = b;
            put(rotate(R, vec(b.p)) + T, o.p);
            const Quat q = mul(R, quat(b.q));
            for (int i = 0; i < 4; ++i) o.q[i] = q.v[i];
            return o;
        };
        Ref r2 = ref;
        r2.clavicle = xb(ref.clavicle);
        r2.upper = xb(ref.upper);
        r2.fore = xb(ref.fore);
        r2.twist[0] = xb(ref.twist[0]);
        r2.twist[1] = xb(ref.twist[1]);
        put(rotate(R, vec(ref.wristP)) + T, r2.wristP);
        const Quat rw = mul(R, refWrist);
        for (int i = 0; i < 4; ++i) r2.wristQ[i] = rw.v[i];
        const Vec w = vec(ref.wristP) + Vec{-8, 6, 10};
        const Quat wq = mul(axis_angle({1, 1, 0}, 0.9f), refWrist);
        Input i1 = make_in(ref, w, wq), i2 = make_in(r2, rotate(R, w) + T, mul(R, wq));
        i2.pole = rotate(R, i1.pole);
        i2.outward = rotate(R, i1.outward);
        Output o1, o2;
        check(pose(ref, i1, o1) && pose(r2, i2, o2), "covariance poses");
        check(near(vec(o2.fore.p), rotate(R, vec(o1.fore.p)) + T, 0.01f), "elbow covariant");
        check(qangle(quat(o2.twist[1].q), mul(R, quat(o1.twist[1].q))) < 0.003f, "helper rotation covariant");
        check(fabsf(o1.roll - o2.roll) < 0.003f, "roll is frame-independent", o2.roll, o1.roll);
    }
    {   // length and scale: lengths follow, the stretch lands on the limb's own axis
        Input in = make_in(ref, vec(ref.wristP) + Vec{20, 0, 0}, refWrist);
        in.scale = 0.8f;
        in.lengthScale = 1.25f;
        Output o;
        check(pose(ref, in, o), "scaled arm poses");
        check(seg_err(o, A * 1.0f, B * 1.0f, in.wrist) < 0.005f, "0.8 x 1.25 = the rig's own length",
              seg_err(o, A, B, in.wrist), 0);
        check(fabsf(o.upper.s[0] - 1.0f) < 1e-4f && fabsf(o.upper.s[1] - 0.8f) < 1e-4f,
              "stretch along the bone's X, scale across it", o.upper.s[0], 1.0);
        in.lengthScale = 3;
        check(!pose(ref, in, o), "length scale out of range refused");
    }
    {   // s86: each segment's own length. The forearm helper keeps its fraction of the
        // forearm, and the wrist still joins.
        Input in = make_in(ref, vec(ref.upper.p) + Vec{10, 30, -40}, refWrist);
        in.scale = 0.83f;
        in.upperLength = 0.71f;
        in.foreLength = 1.13f;
        Output o;
        check(pose(ref, in, o), "per-segment lengths pose");
        check(seg_err(o, A * 0.83f * 0.71f, B * 0.83f * 1.13f, in.wrist) < 0.005f,
              "upper and forearm each at their own length", seg_err(o, A * 0.83f * 0.71f, B * 0.83f * 1.13f, in.wrist), 0);
        check(fabsf(o.upper.s[0] - 0.83f * 0.71f) < 1e-4f && fabsf(o.fore.s[0] - 0.83f * 1.13f) < 1e-4f,
              "each segment's stretch on its own X", o.upper.s[0], 0.83 * 0.71);
        const float helper = length(vec(o.twist[1].p) - vec(o.fore.p));
        check(fabsf(helper - 15.0f * 0.83f * 1.13f) < 0.01f, "helper keeps its fraction of the stretched forearm", helper,
              15.0 * 0.83 * 1.13);
        check(near(posed_wrist(ref, o), in.wrist, 0.01f), "forearm still meets the wrist", length(posed_wrist(ref, o) - in.wrist), 0);
        in.upperLength = 0.1f;
        check(!pose(ref, in, o), "segment length out of range refused");
    }
    {   // s86e: Dishonored's body yaw. A glance inside the 25-degree deadzone moves
        // nothing; a turn beyond it carries the body by the excess at once, then relaxes.
        BodyYaw by;
        check(fabsf(by.update(0.5f, 0) - 0.5f) < 1e-6f, "body yaw starts at the head's");
        check(fabsf(by.update(0.5f + 20 * kDeg, 0) - 0.5f) < 1e-6f, "a 20 deg glance moves nothing at once");
        const float big = by.update(0.5f + 40 * kDeg, 0);
        check(fabsf(big - (0.5f + 15 * kDeg)) < 1e-4f, "a 40 deg turn carries the excess 15 deg", big - 0.5f, 15 * kDeg);
        float y = big;
        for (int i = 0; i < 60; ++i) y = by.update(0.5f + 40 * kDeg, 0.1f);
        check(fabsf(y - (0.5f + 40 * kDeg)) < 2 * kDeg, "held, the body relaxes onto the head within 6 s", y - 0.5f, 40 * kDeg);
        by.reset();
        check(fabsf(by.update(3.0f, 0.1f) - 3.0f) < 1e-6f, "reset snaps to the head");
    }
    {   // NEGATIVE CONTROL: the length check must be able to fail. Move the solved
        // elbow by 1 UU and the same measure the sweep uses must see it.
        Output o;
        pose(ref, make_in(ref, vec(ref.wristP) + Vec{4, 4, 4}, refWrist), o);
        o.joints.elbow = o.joints.elbow + Vec{1, 0, 0};
        const float e = seg_err(o, A, B, vec(ref.wristP) + Vec{4, 4, 4});
        check(e > 0.3f, "negative control: a moved elbow fails the length check", e, 1);
    }

    printf("arm-ik: %d checks, %d failures\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
