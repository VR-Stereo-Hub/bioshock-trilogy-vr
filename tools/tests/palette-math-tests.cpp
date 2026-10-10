// palette-math-tests.cpp - host tests for src/game/bioshock1r/palette_math.h, the hands, arms and
// weapon on BS1's skin palette (Dishonored 1.2-1.5, 1.7, 1.8). Run `.\tools\host-test.ps1 palette-math`.
//
// What has to hold:
//   - a hand correction on a palette entry moves every skinned point exactly as the
//     correction moves the posed point (D * M, the palette form of the bone drive)
//   - the hand-back blend is the identity at 0 and the correction at 1, the palm on the straight
//     line between (Dishonored blend_transform_palm), the scale lerped; NEGATIVE control: a plain
//     translation lerp leaves the line
//   - the IK arm affine carries the game's bone to the solved one for every bind point, is the
//     identity at weight 0, and keeps non-uniform scale (the palette's freedom)
//   - the weapon's composition puts a weapon vertex where the hand correction puts the same
//     world point (Dishonored WaPublishCommon), and an actor transform round-trips
//   - the rigid palm (VR-183) equals the bind centroid carried by the wrist, independent of the
//     finger pose the palm bones are in
//   - the palette inverse round-trips
//   - the hand-back timeline: 250 ms in, 250 ms release, 350 ms out, a reversal mid-return with
//     no jump that covers only the remaining share, no step faster than the easing allows
#include "game/bioshock1r/hand_compose.h"
#include "game/bioshock1r/hand_grip.h"
#include "game/bioshock1r/palette_math.h"

#include <cstdio>
#include <cstdlib>

using namespace bvr::b1r;
using namespace bvr::b1r::palette_math;
using arm_ik::Quat;
using arm_ik::Vec;

static int g_checks = 0, g_fails = 0;
static void check(bool ok, const char* what, double got = 0, double want = 0) {
    ++g_checks;
    if (!ok) {
        ++g_fails;
        printf("  FAIL %s: got %.6f want %.6f\n", what, got, want);
    }
}
static unsigned g_seed = 12345;
static float rnd(float lo, float hi) {
    g_seed = g_seed * 1664525u + 1013904223u;
    return lo + (hi - lo) * ((g_seed >> 8) & 0xFFFFFF) / float(0xFFFFFF);
}
static Vec rvec(float r) { return {rnd(-r, r), rnd(-r, r), rnd(-r, r)}; }
static Quat rquat() {
    Vec a = rvec(1);
    const float n = arm_ik::length(a);
    if (n < 1e-3f) a = {1, 0, 0};
    else a = a * (1.0f / n);
    return arm_ik::axis_angle(a, rnd(-3.1f, 3.1f));
}
static float dist(Vec a, Vec b) { return arm_ik::length(a - b); }

// A row-vector palette entry for "rotate q, scale s (per axis, in the bone's own frame), then
// translate t" applied to a point already in the bone's frame - i.e. M = [R diag(s) | t], so a
// bind point x maps to R (s .* x) + t. Rows are the images of the axes.
static void make_entry(Quat q, Vec s, Vec t, float* m) {
    const Vec ex = arm_ik::rotate(q, Vec{s.x, 0, 0}), ey = arm_ik::rotate(q, Vec{0, s.y, 0}),
              ez = arm_ik::rotate(q, Vec{0, 0, s.z});
    const float v[16] = {ex.x, ex.y, ex.z, 0, ey.x, ey.y, ey.z, 0, ez.x, ez.y, ez.z, 0, t.x, t.y, t.z, 1};
    for (int i = 0; i < 16; ++i) m[i] = v[i];
}
static Vec at(const float* m, Vec x) {
    float o[3];
    const float in[3] = {x.x, x.y, x.z};
    entry_point(m, in, o);
    return {o[0], o[1], o[2]};
}
// M(bone) = [R(q) diag(s) | p] applied to a point.
static Vec bone_point(Vec p, Quat q, Vec s, Vec x) { return arm_ik::rotate(q, Vec{s.x * x.x, s.y * x.y, s.z * x.z}) + p; }

int main() {
    // ---- 1. the hand correction on a palette entry ----------------------------------------
    for (int i = 0; i < 200; ++i) {
        float m[16];
        make_entry(rquat(), {1, 1, 1}, rvec(50), m);
        const Quat dq = rquat();
        const float s = rnd(0.6f, 1.4f);
        const Vec pivot = rvec(40), palmAt = rvec(80);
        float qv[4], pv[3], tv[3];
        for (int c = 0; c < 4; ++c) qv[c] = dq.v[c];
        arm_ik::put(pivot, pv);
        arm_ik::put(palmAt, tv);
        const Sim g = hand_sim(qv, s, pv, tv);
        float m2[16];
        for (int k = 0; k < 16; ++k) m2[k] = m[k];
        apply_sim(g, m2);
        for (int j = 0; j < 4; ++j) {
            const Vec x = rvec(20);
            const Vec posed = at(m, x);
            const Vec want = palmAt + arm_ik::rotate(dq, posed - pivot) * s;
            const float e = dist(at(m2, x), want);
            check(e < 1e-3f, "hand correction moves a skinned point as it moves the posed point", e, 0);
        }
        // the pivot itself goes to palmAt
        const float e = dist(sim_point(g, pivot), palmAt);
        check(e < 1e-3f, "the palm (pivot) lands on the target", e, 0);
    }

    // ---- 2. the hand-back blend ----------------------------------------------------------
    for (int i = 0; i < 200; ++i) {
        const Quat dq = rquat();
        const float s = rnd(0.6f, 1.4f);
        const Vec palm = rvec(60), target = rvec(60);
        const Blended b0 = blend_hand(dq, s, palm, target, 0.0f);
        const Blended b1 = blend_hand(dq, s, palm, target, 1.0f);
        check(fabsf(b0.q.v[3]) > 0.99999f && fabsf(b0.s - 1) < 1e-6f && dist(b0.palmAt, palm) < 1e-4f,
              "weight 0 is the identity (the game's hand)", b0.q.v[3], 1);
        check(fabsf(fabsf(b1.q.v[0] * dq.v[0] + b1.q.v[1] * dq.v[1] + b1.q.v[2] * dq.v[2] + b1.q.v[3] * dq.v[3]) - 1) <
                      1e-5f &&
                  fabsf(b1.s - s) < 1e-6f && dist(b1.palmAt, target) < 1e-4f,
              "weight 1 is the full correction", b1.s, s);
        for (int k = 1; k < 10; ++k) {
            const float w = k / 10.0f;
            const Blended b = blend_hand(dq, s, palm, target, w);
            // the drawn palm IS palmAt (the pivot goes there): on the segment, at fraction w
            const Vec onLine = palm + (target - palm) * w;
            check(dist(b.palmAt, onLine) < 1e-4f, "the palm is on the straight line at fraction w", dist(b.palmAt, onLine), 0);
            check(fabsf(b.s - (1 + (s - 1) * w)) < 1e-5f, "the scale is lerped", b.s, 1 + (s - 1) * w);
            // rotation angle grows monotonically with w
            const float ang = 2 * acosf(fminf(1.0f, fabsf(b.q.v[3])));
            const Quat a1 = arm_ik::normalized(dq);
            const float full = 2 * acosf(fminf(1.0f, fabsf(a1.v[3])));
            check(fabsf(ang - full * w) < 2e-3f, "the rotation is slerped from identity", ang, full * w);
        }
    }
    {   // NEGATIVE control: interpolating the correction's TRANSLATION (the old way) leaves the line.
        const Quat dq = arm_ik::axis_angle({0, 0, 1}, 2.5f);
        const Vec palm{60, 0, 0}, target{-20, 10, 0};
        float worst = 0;
        for (int k = 1; k < 10; ++k) {
            const float w = k / 10.0f;
            // translation-lerp form: x' = w*(t_full) + slerp(R) x with t_full = target - R palm
            const Vec tFull = target - arm_ik::rotate(dq, palm);
            const Vec p = arm_ik::rotate(slerp_identity(dq, w), palm) + tFull * w;
            const Vec onLine = palm + (target - palm) * w;
            worst = fmaxf(worst, dist(p, onLine));
        }
        check(worst > 5.0f, "NEGATIVE: a translation lerp swings the palm off the line", worst, 5);
    }

    // ---- 3. the IK arm affine ----------------------------------------------------------------
    for (int i = 0; i < 200; ++i) {
        const Vec pa = rvec(60), po = rvec(60);
        const Quat qa = rquat(), qo = rquat();
        const Vec sa{rnd(0.8f, 1.2f), rnd(0.8f, 1.2f), rnd(0.8f, 1.2f)};
        const Vec so{rnd(0.5f, 1.5f), rnd(0.5f, 1.5f), rnd(0.5f, 1.5f)}; // non-uniform: the freedom
        float fa[3], fo[3], fqa[4], fqo[4], fso[3];
        arm_ik::put(pa, fa);
        arm_ik::put(po, fo);
        arm_ik::put(so, fso);
        for (int c = 0; c < 4; ++c) {
            fqa[c] = qa.v[c];
            fqo[c] = qo.v[c];
        }
        float T[12], T0[12];
        arm_affine(fa, fqa, fo, fqo, fso, 1.0f, T);
        arm_affine(fa, fqa, fo, fqo, fso, 0.0f, T0);
        // the game's palette entry for this bone: M(a) * IB, with some inverse bind IB = [Rb | tb]
        float m[16];
        const Quat qb = rquat();
        const Vec tb = rvec(30);
        // M(a) * IB maps bind x to M(a)(Rb x + tb)
        for (int j = 0; j < 4; ++j) {
            const Vec x = rvec(25);
            const Vec local = arm_ik::rotate(qb, x) + tb;
            const Vec game = bone_point(pa, qa, sa, local);
            const Vec solved = bone_point(po, qo, Vec{sa.x * so.x, sa.y * so.y, sa.z * so.z}, local);
            // the entry is affine; build it from the composition on axes
            const Vec o = bone_point(pa, qa, sa, tb);
            const Vec ex = bone_point(pa, qa, sa, arm_ik::rotate(qb, Vec{1, 0, 0}) + tb) - o;
            const Vec ey = bone_point(pa, qa, sa, arm_ik::rotate(qb, Vec{0, 1, 0}) + tb) - o;
            const Vec ez = bone_point(pa, qa, sa, arm_ik::rotate(qb, Vec{0, 0, 1}) + tb) - o;
            const float v[16] = {ex.x, ex.y, ex.z, 0, ey.x, ey.y, ey.z, 0, ez.x, ez.y, ez.z, 0, o.x, o.y, o.z, 1};
            for (int k = 0; k < 16; ++k) m[k] = v[k];
            check(dist(at(m, x), game) < 1e-2f, "the test's game entry is M(a) * IB", dist(at(m, x), game), 0);
            float m1[16], m0[16];
            for (int k = 0; k < 16; ++k) m1[k] = m0[k] = m[k];
            apply_affine(T, m1);
            apply_affine(T0, m0);
            check(dist(at(m1, x), solved) < 2e-2f, "the arm affine carries the game's bone to the solved one",
                  dist(at(m1, x), solved), 0);
            check(dist(at(m0, x), game) < 1e-3f, "weight 0 leaves the game's arm exactly", dist(at(m0, x), game), 0);
        }
    }

    // ---- 4. the weapon and actor transforms -------------------------------------------------
    for (int i = 0; i < 200; ++i) {
        Sim L, S, W;
        const Quat ql = rquat(), qs = rquat(), qw = rquat();
        for (int c = 0; c < 4; ++c) {
            L.q[c] = ql.v[c];
            S.q[c] = qs.v[c];
            W.q[c] = qw.v[c];
        }
        L.s = rnd(0.5f, 1.5f);
        S.s = rnd(0.6f, 1.4f);
        W.s = rnd(0.5f, 1.5f);
        arm_ik::put(rvec(5000), L.t);
        arm_ik::put(rvec(40), S.t);
        arm_ik::put(rvec(5000), W.t);
        const Sim Dw = sim_world(L, S);
        // a world point that is the hand-local point x moves to L(S(x))
        const Vec x = rvec(40);
        const Vec world = sim_point(L, x);
        const Vec want = sim_point(L, sim_point(S, x));
        const float scale = 1.0f + arm_ik::length(want) * 1e-6f;
        check(dist(sim_point(Dw, world), want) < 2e-2f * scale, "D_world = L S L^-1 moves world points as S moves local ones",
              dist(sim_point(Dw, world), want), 0);
        // the weapon: its local point y, at world W(y), must land on D_world(W(y))
        const Sim G = sim_mul(sim_inv(W), sim_mul(Dw, W));
        const Vec y = rvec(30);
        const Vec got = sim_point(W, sim_point(G, y));
        const Vec wantW = sim_point(Dw, sim_point(W, y));
        check(dist(got, wantW) < 2e-2f * scale, "the weapon's composition matches the hand's world correction",
              dist(got, wantW), 0);
        // round trip
        const Vec back = sim_point(sim_inv(L), sim_point(L, x));
        check(dist(back, x) < 1e-2f, "an actor transform round-trips", dist(back, x), 0);
    }

    // ---- 5. the rigid palm (VR-183) ----------------------------------------------------------
    for (int i = 0; i < 100; ++i) {
        // a wrist pose and five palm bones with their own inverse binds; then the fingers move
        const Quat wq = rquat();
        const Vec wp = rvec(60);
        Vec bindHeads[5];
        for (auto& h : bindHeads) h = rvec(10) + Vec{9, 0, 0};
        Vec mean{0, 0, 0};
        for (const auto& h : bindHeads) mean = mean + h * 0.2f;
        // wrist bind frame: identity at origin; the wrist's entry = pose_w * IB_w with IB_w = I
        float wm[16];
        make_entry(wq, {1, 1, 1}, wp, wm);
        float pm[5][16];
        float heads[5][3];
        const float* ents[5];
        for (int j = 0; j < 5; ++j) {
            // a finger-base bone: bind head h_j; its live pose has the head somewhere the finger
            // animation put it (NOT rigid with the wrist), with the entry consistent with that pose
            const Quat fq = rquat();
            const Vec fp = rvec(80);
            // entry_j = pose_j * IB_j where IB_j maps bind h_j to the bone origin: IB_j(x) = x - h_j
            // so entry_j(x) = fq (x - h_j) + fp, and the posed head is entry_j(h_j) = fp
            const Vec ex = arm_ik::rotate(fq, {1, 0, 0}), ey = arm_ik::rotate(fq, {0, 1, 0}), ez = arm_ik::rotate(fq, {0, 0, 1});
            const Vec t = fp - arm_ik::rotate(fq, bindHeads[j]);
            const float v[16] = {ex.x, ex.y, ex.z, 0, ey.x, ey.y, ey.z, 0, ez.x, ez.y, ez.z, 0, t.x, t.y, t.z, 1};
            for (int k = 0; k < 16; ++k) pm[j][k] = v[k];
            arm_ik::put(fp, heads[j]);
            ents[j] = pm[j];
        }
        float out[3], fwp[3], fwq[4];
        arm_ik::put(wp, fwp);
        for (int c = 0; c < 4; ++c) fwq[c] = wq.v[c];
        const bool ok = rigid_palm(ents, heads, wm, fwp, fwq, out);
        check(ok, "rigid palm solves");
        // expected: the bind centroid, carried by the wrist entry, in the wrist's frame = mean itself
        check(dist(Vec{out[0], out[1], out[2]}, mean) < 1e-2f, "the rigid palm is the bind centroid in the wrist frame, "
              "whatever the fingers do", dist(Vec{out[0], out[1], out[2]}, mean), 0);
    }

    // ---- 6. the palette inverse ----------------------------------------------------------------
    for (int i = 0; i < 200; ++i) {
        float m[16];
        make_entry(rquat(), {rnd(0.5f, 2), rnd(0.5f, 2), rnd(0.5f, 2)}, rvec(100), m);
        const Vec x = rvec(50);
        const Vec y = at(m, x);
        float in[3], back[3];
        arm_ik::put(y, in);
        check(entry_inverse_point(m, in, back) && dist(Vec{back[0], back[1], back[2]}, x) < 1e-3f,
              "the palette inverse round-trips", dist(Vec{back[0], back[1], back[2]}, x), 0);
    }
    {   // collapse: every point to one
        float m[16];
        make_entry(rquat(), {1, 1, 1}, rvec(50), m);
        const float at3[3] = {1, 2, 3};
        collapse(at3, m);
        check(dist(at(m, rvec(40)), Vec{1, 2, 3}) < 1e-6f, "a collapsed entry sends every point to one");
    }

    // ---- 7. the hand-back timeline (hand_compose::Handoff, as handback.cpp drives it) ----------
    {
        // owned 0..1000 ms, released at 1000, owned again at 1450 (inside the 350 ms return that
        // starts at 1250), released for good at 2500. Sampled every millisecond.
        hand_compose::Handoff h;
        float prev = 1.0f, worstStep = 0.0f, wAtRev = -1.0f, wJustAfterRev = -1.0f;
        float w250 = -1, w1249 = -1, wEnd = -1;
        unsigned long long reachedZeroAgain = 0;
        for (unsigned long long t = 0; t <= 4000; ++t) {
            const bool owned = t < 1000 || (t >= 1450 && t < 2500);
            h.update(owned, t);
            const float w = h.weight(t);
            if (t == 1449) wAtRev = w;
            if (t == 1450) wJustAfterRev = w;
            if (t == 250) w250 = w;
            if (t == 1249) w1249 = w;
            if (t >= 1450 && !reachedZeroAgain && w <= 0.0001f) reachedZeroAgain = t;
            if (t > 0) worstStep = fmaxf(worstStep, fabsf(w - prev));
            prev = w;
            if (t == 4000) wEnd = w;
        }
        check(w250 <= 0.0001f, "the hand reaches the game's clip 250 ms after the owned state starts", w250, 0);
        check(w1249 <= 0.0001f, "the release holds the clip for 250 ms after the state ends", w1249, 0);
        check(wAtRev > 0.05f && wAtRev < 0.95f, "the second attack lands mid-return (test setup)", wAtRev, 0.5);
        check(fabsf(wJustAfterRev - wAtRev) < 0.01f, "no jump at the reversal", wJustAfterRev, wAtRev);
        // the reversed blend covers only the remaining distance: 250 ms * w at the reversal
        const float span = static_cast<float>(reachedZeroAgain - 1450);
        check(reachedZeroAgain && fabsf(span - 250.0f * wAtRev) < 3.0f, "the reversal takes only the remaining share",
              span, 250.0f * wAtRev);
        // smootherstep's peak slope is 15/8 per blend span: 1.875/250 per ms on the way in
        check(worstStep < 1.875f / 250.0f + 1e-4f, "no step faster than the eased blend allows", worstStep,
              1.875f / 250.0f);
        check(fabsf(wEnd - 1.0f) < 1e-4f, "the hand is back on the controller at the end", wEnd, 1);
        // NEGATIVE control: restarting the blend from the game's end at the reversal would jump by
        // the weight already recovered.
        check(fabsf(0.0f - wAtRev) > 0.05f, "NEGATIVE: a restart from 0 at the reversal would jump", wAtRev, 0.05);
    }

    // ---- 8. the grip calibration and the fist pivot (hand_grip.h, s89) ---------------------------
    for (int i = 0; i < 300; ++i) {
        for (float side : {1.0f, -1.0f}) {
            const Vec a = arm_ik::rotate(rquat(), Vec{1, 0, 0});
            Vec n = arm_ik::rotate(rquat(), Vec{0, 1, 0});
            n = n - a * arm_ik::dot(n, a);
            if (arm_ik::length(n) < 0.2f) continue;
            const Quat C = hand_grip::calibration(a, n, side);
            const Vec ca = arm_ik::rotate(C, a * (1.0f / arm_ik::length(a)));
            const Vec cn = arm_ik::rotate(C, n * (1.0f / arm_ik::length(n)));
            check(dist(ca, Vec{1, 0, 0}) < 1e-3f, "the calibration puts the handle axis on the grip's forward", dist(ca, Vec{1, 0, 0}), 0);
            check(dist(cn, Vec{0, -side, 0}) < 1e-3f, "the palm normal on the grip's -X (right) / +X (left)",
                  dist(cn, Vec{0, -side, 0}), 0);
            // NEGATIVE control: the other hand's sign lands the palm normal on the wrong side
            const Quat Cw = hand_grip::calibration(a, n, -side);
            const Vec cw = arm_ik::rotate(Cw, n * (1.0f / arm_ik::length(n)));
            check(dist(cw, Vec{0, -side, 0}) > 1.9f, "NEGATIVE: the wrong side's calibration flips the palm", dist(cw, Vec{0, -side, 0}), 2);
        }
    }
    {   // mirrored hands give mirrored fist centres; the point scales with the hand
        const Vec w{0, 0, 0}, i1{12, 3, 0.5f}, m1{12.0986f, 0, 0}, p1{11, -3.5f, 0.2f};
        const auto mir = [](Vec v) { return Vec{v.x, v.y, -v.z}; };
        hand_grip::Frame fr, fl;
        check(hand_grip::head_frame(w, i1, m1, p1, 1.0f, &fr) && hand_grip::head_frame(w, mir(i1), mir(m1), mir(p1), -1.0f, &fl),
              "head frames build");
        const Vec pr = hand_grip::point(fr, hand_grip::kFistCentre), pl = hand_grip::point(fl, hand_grip::kFistCentre);
        check(dist(mir(pr), pl) < 1e-4f, "the left fist centre is the mirror of the right", dist(mir(pr), pl), 0);
        hand_grip::Frame f2;
        hand_grip::head_frame(w, i1 * 2.0f, m1 * 2.0f, p1 * 2.0f, 1.0f, &f2);
        check(dist(hand_grip::point(f2, hand_grip::kFistCentre), pr * 2.0f) < 1e-3f, "the fist point scales with the hand",
              dist(hand_grip::point(f2, hand_grip::kFistCentre), pr * 2.0f), 0);
    }

    printf("palette-math: %d checks, %d failures\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
