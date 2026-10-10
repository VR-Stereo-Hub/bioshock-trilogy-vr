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
#include "game/bioshock1r/frame_context.h"

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

    // ---- s86f: the hand step rows' math (frame_context.h) ------------------------------
    {
        using namespace bvr::b1r;
        // xr_local_trim_quat <-> xr_local_trim_angles round trip.
        float worstTrim = 0.0f;
        for (int p = -80; p <= 80; p += 20)
            for (int y = -170; y <= 170; y += 34)
                for (int r = -170; r <= 170; r += 34) {
                    float q[4], p2, y2, r2;
                    xr_local_trim_quat(p / kRadToDeg, y / kRadToDeg, r / kRadToDeg, q);
                    xr_local_trim_angles(q, &p2, &y2, &r2);
                    float q2[4];
                    xr_local_trim_quat(p2, y2, r2, q2);
                    float dot = 0;
                    for (int i = 0; i < 4; ++i) dot += q[i] * q2[i];
                    worstTrim = fmaxf(worstTrim, 1.0f - fabsf(dot));
                }
        check(worstTrim < 1e-5f, "trim quat -> angles -> quat round trip", worstTrim, 0.0);
        // ue_angles_from_xr_quat <-> xr_quat_from_ue_angles round trip.
        float worstXr = 0.0f;
        for (int p = -80; p <= 80; p += 20)
            for (int y = -170; y <= 170; y += 34)
                for (int r = -170; r <= 170; r += 34) {
                    UeAngles a{};
                    a.pitchRad = p / kRadToDeg; a.yawRad = y / kRadToDeg; a.rollRad = r / kRadToDeg;
                    float q[4];
                    xr_quat_from_ue_angles(a, q);
                    const UeAngles b = ue_angles_from_xr_quat(q[0], q[1], q[2], q[3]);
                    worstXr = fmaxf(worstXr, fabsf(bvr::ue::wrap_rot(static_cast<int32_t>((b.yawRad - a.yawRad) * kRotUnitsPerRadian)) / kRotUnitsPerRadian));
                    worstXr = fmaxf(worstXr, fabsf(b.pitchRad - a.pitchRad));
                    worstXr = fmaxf(worstXr, fabsf(bvr::ue::wrap_rot(static_cast<int32_t>((b.rollRad - a.rollRad) * kRotUnitsPerRadian)) / kRotUnitsPerRadian));
                }
        check(worstXr < 2e-3f, "ue angles -> xr quat -> ue angles round trip", worstXr, 0.0);
        // A zero step changes nothing; a yaw step turns the hand by exactly that yaw; a pitch
        // step on a hand pointing along the view lifts it by exactly that pitch.
        FrameContext ctx{};
        ctx.camYaw = 12000; ctx.driveYawOffsetRad = 0.1f; ctx.recenterYawRad = 0.7f; ctx.worldScale = 100;
        float ctrl[4];
        xr_local_trim_quat(0.3f, -0.8f, 0.4f, ctrl);
        float p = -32, y = -4, r = -8;
        const float pos[3] = {0, 0, 0};
        const GamePose before = model_pose_from_xr(ctx, pos, ctrl, p, y, r);
        check(hand_rotation_step(ctx, ctrl, 0, 0.0f, &p, &y, &r), "zero step accepted", 1, 1);
        const GamePose same = model_pose_from_xr(ctx, pos, ctrl, p, y, r);
        check(abs(bvr::ue::wrap_rot(same.rot.yaw - before.rot.yaw)) < 40 && abs(same.rot.pitch - before.rot.pitch) < 40 &&
              abs(bvr::ue::wrap_rot(same.rot.roll - before.rot.roll)) < 40, "zero step leaves the hand", same.rot.yaw, before.rot.yaw);
        check(hand_rotation_step(ctx, ctrl, 0, 10.0f, &p, &y, &r), "yaw step accepted", 1, 1);
        const GamePose yawed = model_pose_from_xr(ctx, pos, ctrl, p, y, r);
        const float dyaw = bvr::ue::wrap_rot(yawed.rot.yaw - before.rot.yaw) / kRotUnitsPerDegree;
        check(fabsf(dyaw - 10.0f) < 0.2f && abs(yawed.rot.pitch - before.rot.pitch) < 60, "yaw right 10 turns the hand 10 right", dyaw, 10);
        // Pitch: put the hand along the view first (yaw = camYaw, level), then pitch up 10.
        {
            // Find a trim that makes the hand level along the view: step yaw by the difference.
            float dy = -bvr::ue::wrap_rot(yawed.rot.yaw - ctx.camYaw) / kRotUnitsPerDegree;
            hand_rotation_step(ctx, ctrl, 0, dy, &p, &y, &r);
            const GamePose level0 = model_pose_from_xr(ctx, pos, ctrl, p, y, r);
            hand_rotation_step(ctx, ctrl, 1, -level0.rot.pitch / kRotUnitsPerDegree, &p, &y, &r);
            const GamePose level = model_pose_from_xr(ctx, pos, ctrl, p, y, r);
            hand_rotation_step(ctx, ctrl, 1, 10.0f, &p, &y, &r);
            const GamePose up = model_pose_from_xr(ctx, pos, ctrl, p, y, r);
            const float dp = (up.rot.pitch - level.rot.pitch) / kRotUnitsPerDegree;
            check(fabsf(dp - 10.0f) < 0.3f, "pitch up 10 lifts a view-aligned hand 10", dp, 10);
            // Roll right 10 on that hand: roll changes by +10, pitch and yaw stay.
            hand_rotation_step(ctx, ctrl, 2, 10.0f, &p, &y, &r);
            const GamePose rolled = model_pose_from_xr(ctx, pos, ctrl, p, y, r);
            const float dr = bvr::ue::wrap_rot(rolled.rot.roll - up.rot.roll) / kRotUnitsPerDegree;
            check(fabsf(dr - 10.0f) < 0.3f && abs(rolled.rot.pitch - up.rot.pitch) < 60, "roll right 10 rolls a view-aligned hand 10", dr, 10);
        }
        // Position: a step "forward" in the view, on a hand turned 90 deg right of the view,
        // lands as "left" in the hand's own frame.
        {
            GamePose hand{};
            hand.rot = FRotator{0, ctx.camYaw + 16384, 0};
            float g[3] = {0, 0, 0};
            hand_position_step(ctx, hand, 1, 2.0f, g);
            check(fabsf(g[1] + 2.0f) < 1e-3f && fabsf(g[0]) < 1e-3f && fabsf(g[2]) < 1e-3f,
                  "view-forward step on a right-turned hand is hand-left", g[1], -2);
            hand_position_step(ctx, hand, 2, 1.0f, g);
            check(fabsf(g[2] - 1.0f) < 1e-3f, "view-up step is hand-up on a level hand", g[2], 1);
        }
    }

    printf("ue-math: %d checks over %d rotators, %d failed (worst axis err %.2e, conjugate %.3f)\n",
           g_checks, cases, g_fails, worst, worstConj);
    return g_fails ? 1 : 0;
}
