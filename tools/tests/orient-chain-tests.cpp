// orient-chain-tests.cpp - the controller's ORIENTATION through the hand drive's chain, against
// the quaternion itself. Run `.\tools\host-test.ps1 orient-chain`.
//
// The drive turns the controller quaternion (with its trim) into yaw / pitch / roll
// (ue_angles_from_xr_quat), stores them as integer FRotator units, and rebuilds the hand's axes
// with ue_rot_basis. Whatever that round trip loses, the drawn hand inherits as an orientation
// error against the controller - and it would depend on WHERE the controller points, which is
// the s88 report ("90 deg one way is fine, the other way desyncs").
//
// The oracle is the quaternion rotating the XR axes directly (forward -Z, right +X, up +Y),
// taken to UE axes. Every check compares the chain's three axes to it, in degrees.
#include "game/bioshock1r/frame_context.h"

#include <cstdio>

using namespace bvr::b1r;

static int g_checks = 0, g_fails = 0;
static unsigned g_seed = 777;
static float rnd(float lo, float hi) {
    g_seed = g_seed * 1664525u + 1013904223u;
    return lo + (hi - lo) * ((g_seed >> 8) & 0xFFFFFF) / float(0xFFFFFF);
}
static float angle_deg(const float a[3], const float b[3]) {
    const float d = a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
    const float na = sqrtf(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]), nb = sqrtf(b[0] * b[0] + b[1] * b[1] + b[2] * b[2]);
    float c = d / (na * nb);
    c = c > 1 ? 1 : c < -1 ? -1 : c;
    return acosf(c) * 57.29578f;
}
// The chain's rebuilt axes for an XR quaternion (recenter 0, game yaw 0).
static void chain_axes(const float q[4], float f[3], float r[3], float u[3]) {
    FrameContext ctx{};
    const float pos[3] = {0, 0, 0};
    const GamePose gp = xr_pose_to_game(ctx, pos, q);
    ue_rot_basis(gp.rot, f, r, u);
}
// The oracle: the XR axes rotated by q, in UE axes.
static void direct_axes(const float q[4], float f[3], float r[3], float u[3]) {
    const float xf[3] = {0, 0, -1}, xr[3] = {1, 0, 0}, xu[3] = {0, 1, 0};
    float a[3];
    quat_rotate(q[0], q[1], q[2], q[3], xf, a);
    xr_to_ue(a, f);
    quat_rotate(q[0], q[1], q[2], q[3], xr, a);
    xr_to_ue(a, r);
    quat_rotate(q[0], q[1], q[2], q[3], xu, a);
    xr_to_ue(a, u);
}
static void quat_from_ypr_xr(float yawDeg, float pitchDeg, float rollDeg, float q[4]) {
    // the sim's convention for "hand r grip pose x y z yaw pitch roll": yaw about +Y (left +),
    // then pitch about +X, then roll about -Z - built with the same trim helper the drive uses
    xr_local_trim_quat(pitchDeg / 57.29578f, -yawDeg / 57.29578f, rollDeg / 57.29578f, q);
}

int main() {
    float worst = 0, worstYaw = 0, worstPitch = 0, worstRoll = 0;
    int bad = 0;
    // 1. a grid over the whole sphere: yaw -180..180, pitch -89..89, roll -180..180
    for (int yi = -180; yi <= 180; yi += 15)
        for (int pi = -85; pi <= 85; pi += 5)
            for (int ri = -180; ri <= 180; ri += 15) {
                float q[4];
                quat_from_ypr_xr(float(yi), float(pi), float(ri), q);
                float cf[3], cr[3], cu[3], df[3], dr[3], du[3];
                chain_axes(q, cf, cr, cu);
                direct_axes(q, df, dr, du);
                const float e = fmaxf(angle_deg(cf, df), fmaxf(angle_deg(cr, dr), angle_deg(cu, du)));
                ++g_checks;
                if (e > 0.05f) {
                    ++bad;
                    if (e > worst) {
                        worst = e;
                        worstYaw = float(yi);
                        worstPitch = float(pi);
                        worstRoll = float(ri);
                    }
                }
            }
    printf("  grid: %d of %d orientations off by > 0.05 deg; worst %.3f deg at yaw %.0f pitch %.0f roll %.0f\n", bad,
           g_checks, worst, worstYaw, worstPitch, worstRoll);
    if (bad) ++g_fails;
    // 2. random orientations, and the trims the drive actually uses (right -32/-4/-8, left -30/31/-206)
    const float trims[2][3] = {{-32, -4, -8}, {-30, 31, -206}};
    for (int h = 0; h < 2; ++h) {
        float tw = 0, twY = 0, twP = 0, twR = 0;
        int tb = 0;
        for (int i = 0; i < 20000; ++i) {
            float qc[4], trim[4], q[4];
            const float y = rnd(-180, 180), p = rnd(-89, 89), r = rnd(-180, 180);
            quat_from_ypr_xr(y, p, r, qc);
            xr_local_trim_quat(trims[h][0] / 57.29578f, trims[h][1] / 57.29578f, trims[h][2] / 57.29578f, trim);
            quat_mul(qc, trim, q);
            float cf[3], cr[3], cu[3], df[3], dr[3], du[3];
            chain_axes(q, cf, cr, cu);
            direct_axes(q, df, dr, du);
            const float e = fmaxf(angle_deg(cf, df), fmaxf(angle_deg(cr, dr), angle_deg(cu, du)));
            ++g_checks;
            if (e > 0.05f) {
                ++tb;
                if (e > tw) {
                    tw = e;
                    twY = y;
                    twP = p;
                    twR = r;
                }
            }
        }
        printf("  %s trim: %d of 20000 controller orientations off by > 0.05 deg; worst %.3f deg at controller yaw %.0f pitch %.0f roll %.0f\n",
               h ? "left" : "right", tb, tw, twY, twP, twR);
        if (tb) ++g_fails;
    }
    printf("orient-chain: %d checks, %d failing groups\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
