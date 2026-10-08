// ue-math-tests.cpp - host tests for src/game/shared/ue_math.h, the XR <-> UE2.5
// convention every BS1/BS2 camera, aim and bone path shares. Run with
// `.\tools\host-test.ps1 ue-math`. Pure math: no game, no engine memory.
//
// What it pins, and why each one matters:
//   - wrap_rot is the shortest way round, including across the 0/65535 seam
//     (the M7.5 yaw transfer's exactness rests on it)
//   - ue_rot_to_quat agrees with ue_rot_basis axis for axis, over a sweep that
//     includes roll and steep pitch (the quaternion the skeleton writes must be
//     the same rotation the aim ray's basis describes)
//   - ue_dir_to_rot inverts ue_rot_to_dir for pitch and yaw
//   - a NEGATIVE control: the conjugate quaternion must FAIL the basis check, so
//     a passing basis check is evidence and not a tautology
#include "game/shared/ue_math.h"

#include <cmath>
#include <cstdio>

using namespace bvr::ue;

static int g_checks = 0, g_fails = 0;

static void check(bool ok, const char* what, double got, double want) {
    ++g_checks;
    if (!ok) {
        ++g_fails;
        printf("  FAIL %s: got %.6f want %.6f\n", what, got, want);
    }
}

static float max_axis_error(const FRotator& r, bool conjugate) {
    float f[3], ri[3], u[3], q[4];
    ue_rot_basis(r, f, ri, u);
    ue_rot_to_quat(r, q);
    if (conjugate) {
        float c[4];
        quat_conj(q, c);
        q[0] = c[0]; q[1] = c[1]; q[2] = c[2]; q[3] = c[3];
    }
    const float axes[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    const float* want[3] = {f, ri, u};
    float worst = 0.0f;
    for (int a = 0; a < 3; ++a) {
        float got[3];
        quat_rotate(q[0], q[1], q[2], q[3], axes[a], got);
        for (int c = 0; c < 3; ++c) {
            const float e = fabsf(got[c] - want[a][c]);
            if (e > worst) worst = e;
        }
    }
    return worst;
}

int main() {
    // wrap_rot: shortest way round, both directions, across the seam.
    check(wrap_rot(100 - 65500) == 136, "wrap_rot(100 - 65500)", wrap_rot(100 - 65500), 136);
    check(wrap_rot(65500 - 100) == -136, "wrap_rot(65500 - 100)", wrap_rot(65500 - 100), -136);
    check(wrap_rot(32768) == -32768, "wrap_rot(32768) half-turn", wrap_rot(32768), -32768);
    check(wrap_rot(0) == 0, "wrap_rot(0)", wrap_rot(0), 0);

    // Quaternion vs basis over a sweep with roll and steep pitch.
    float worst = 0.0f, worstConj = 0.0f;
    int cases = 0;
    for (int p = -15000; p <= 15000; p += 2500)
        for (int y = 0; y < 65536; y += 4096)
            for (int r = -16384; r <= 16384; r += 8192) {
                const FRotator rot{p, y, r};
                const float e = max_axis_error(rot, false);
                if (e > worst) worst = e;
                const float ec = max_axis_error(rot, true);
                if (ec > worstConj) worstConj = ec;
                ++cases;
            }
    check(worst < 1e-4f, "quat vs basis, worst axis error", worst, 0.0);
    // Negative control: rotating by the INVERSE must disagree badly somewhere.
    check(worstConj > 0.5f, "NEGATIVE CONTROL: conjugate quat must fail the basis check", worstConj, 0.5);

    // dir -> rot -> dir round trip (roll is not representable in a direction).
    float worstRt = 0.0f;
    for (int p = -15000; p <= 15000; p += 1500)
        for (int y = 0; y < 65536; y += 2048) {
            float d[3], d2[3];
            ue_rot_to_dir(FRotator{p, y, 0}, d);
            ue_rot_to_dir(ue_dir_to_rot(d), d2);
            for (int c = 0; c < 3; ++c) {
                const float e = fabsf(d[c] - d2[c]);
                if (e > worstRt) worstRt = e;
            }
        }
    check(worstRt < 1e-3f, "dir -> rot -> dir round trip", worstRt, 0.0);

    printf("ue-math: %d checks over %d rotators, %d failed (worst axis err %.2e, conjugate %.3f)\n",
           g_checks, cases, g_fails, worst, worstConj);
    return g_fails ? 1 : 0;
}
