// two-hand-tests.cpp - host tests for src/game/bioshock1r/two_hand_math.h, the BS1 two-handed
// grip and the gunshot kick. Run `.\tools\host-test.ps1 two-hand`.
//
// What has to hold:
//   grip  - only a RISING grip inside the grab radius takes hold; a grip already down when the
//           hand arrives does not; the grip coming up lets go; held, sliding along the gun past
//           the release radius lets go, inside it does not; not applicable (-1) lets go;
//           eligible is true inside the radius and for the whole hold, and never outside unheld
//   align - takes `from` onto `to` exactly, for ordinary, opposite and identical directions
//   aim   - the authored front lands on the line to the real front; the rear palm is the pivot;
//           a span under the minimum is identity
//   kick  - a wrist jump past the threshold while firing kicks; not while not firing; entering
//           the firing state kicks; a rattle is rate-limited to the pulse length
#include "game/bioshock1r/two_hand_math.h"

#include <cmath>
#include <cstdio>

using namespace bvr::b1r;
using arm_ik::Vec;

static int g_checks = 0, g_fails = 0;
static void check(bool ok, const char* what, double got = 0, double want = 0) {
    ++g_checks;
    if (!ok) {
        ++g_fails;
        printf("  FAIL %s: got %.6f want %.6f\n", what, got, want);
    }
}
static float dist(Vec a, Vec b) { return arm_ik::length(a - b); }

int main() {
    const two_hand::GripConfig c;
    // ---- grip ----
    {
        two_hand::Grip g;
        two_hand::GripOutput o = g.update({30.0f, -1.0f, false}, c);
        check(!o.eligible && !o.held, "far: not eligible, not held");
        o = g.update({30.0f, -1.0f, true}, c);
        check(!o.held && !o.grabbed, "a squeeze far away does not grab");
        o = g.update({10.0f, -1.0f, true}, c);
        check(!o.held && o.eligible, "arriving with the grip ALREADY down: eligible, not grabbed");
        o = g.update({10.0f, -1.0f, false}, c);
        o = g.update({10.0f, -1.0f, true}, c);
        check(o.held && o.grabbed && o.eligible, "a rising grip inside the radius grabs");
        o = g.update({40.0f, 15.0f, true}, c);
        check(o.held && o.eligible, "held: the distance to the point no longer matters, only the slide");
        o = g.update({40.0f, 25.0f, true}, c);
        check(!o.held && o.letGo && o.slidOff, "held: sliding past the release radius lets go");
        o = g.update({10.0f, -1.0f, true}, c);
        check(!o.held, "still squeezing after sliding off: no re-grab without a new press");
        o = g.update({10.0f, -1.0f, false}, c);
        o = g.update({10.0f, -1.0f, true}, c);
        check(o.held, "a new press grabs again");
        o = g.update({10.0f, 0.0f, false}, c);
        check(!o.held && o.letGo && !o.slidOff, "the grip coming up lets go");
        o = g.update({10.0f, -1.0f, false}, c);
        o = g.update({10.0f, -1.0f, true}, c);
        o = g.update({-1.0f, -1.0f, true}, c);
        check(!o.held && !o.eligible, "not applicable (gun lowered, hand untracked) lets go");
        // negative control: the same squeeze at 13 cm (outside 12) must NOT grab
        two_hand::Grip h;
        h.update({13.0f, -1.0f, false}, c);
        o = h.update({13.0f, -1.0f, true}, c);
        check(!o.held && !o.eligible, "13 cm is outside the 12 cm radius");
    }
    // ---- align ----
    {
        const Vec pairs[][2] = {{{1, 0, 0}, {0, 1, 0}}, {{1, 2, 3}, {-3, 1, 0.5f}}, {{0, 0, 1}, {0, 0, -1}},
                                {{2, 0, 0}, {5, 0, 0}}, {{1, 0, 0}, {-1, 0, 0}}};
        for (const auto& p : pairs) {
            const Vec a = p[0] * (1.0f / arm_ik::length(p[0])), b = p[1] * (1.0f / arm_ik::length(p[1]));
            const Vec got = arm_ik::rotate(two_hand::align(p[0], p[1]), a);
            check(dist(got, b) < 1e-4f, "align takes from onto to", dist(got, b), 0);
        }
        // negative control: a rotation that is not the alignment fails the same check
        const Vec wrong = arm_ik::rotate(two_hand::align({1, 0, 0}, {0, 0, 1}), {1, 0, 0});
        check(dist(wrong, {0, 1, 0}) > 0.5f, "the check can fail (control)");
    }
    // ---- aim ----
    {
        const Vec rear{10, 0, 0}, authored{40, 0, 5}, real{30, 20, 10};
        const arm_ik::Quat q = two_hand::aim_turn(rear, authored, real, 5.0f);
        const Vec moved = rear + arm_ik::rotate(q, authored - rear);
        const Vec dir = (real - rear) * (1.0f / arm_ik::length(real - rear));
        const Vec on = rear + dir * arm_ik::length(authored - rear);
        check(dist(moved, on) < 1e-3f, "the authored front lands on the line to the real front", dist(moved, on), 0);
        check(dist(rear + arm_ik::rotate(q, rear - rear), rear) < 1e-6f, "the rear palm stays put");
        const arm_ik::Quat id = two_hand::aim_turn(rear, authored, rear + Vec{2, 0, 0}, 5.0f);
        check(std::fabs(id.v[3]) > 0.999999f, "hands together (< min span): identity", id.v[3], 1);
    }
    // ---- kick ----
    {
        const two_hand::KickConfig k;
        two_hand::Kick kick;
        check(!kick.update(false, 0, k), "no event, no kick");
        check(kick.update(true, 100, k), "a shot kicks");
        check(!kick.update(true, 130, k), "the same shot's second event (seam + state) folds into one kick");
        check(kick.update(true, 200, k), "the next shot of a 10/s automatic kicks again");
        int kicks = 0;
        for (uint64_t t = 1000; t < 3500; t += 100) kicks += kick.update(true, t, k) ? 1 : 0;
        check(kicks == 25, "a 2.5 s burst at 10 shots/s: one kick per shot", kicks, 25);
        // a swing whose firing state begins once: events only at its start
        check(kick.update(true, 5000, k) && !kick.update(false, 5016, k) && !kick.update(false, 5300, k),
              "one swing, one kick");
    }

    printf("two-hand: %d checks, %d failed\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
