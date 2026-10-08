// xrsim-math-tests.cpp - host tests for src/tools/xrsim/xrsim_math.h, the simulated
// runtime's own math. Run with `.\tools\host-test.ps1 xrsim-math`.
//
// What it pins: mat4_view_from_pose maps a world point to the EYE's frame, in the
// row-vector convention the compositor multiplies with (model * view * proj, v * M).
// A point straight ahead of an eye at any yaw/pitch must land on the eye's -Z axis.
// The 2026-10-07 fix (from the Dishonored VR mod's copy) changed the basis from the
// inverse rotation's axes to the eye's own; the NEGATIVE control rebuilds the old
// basis and requires it to FAIL, so this suite can tell the two apart.
#include <openxr/openxr.h>

#include "tools/xrsim/xrsim_math.h"

#include <cmath>
#include <cstdio>

using namespace xrsim;

static Vec3 row_mul(const Vec3& v, const Mat4& M) {
    return Vec3{v.x * M.m[0][0] + v.y * M.m[1][0] + v.z * M.m[2][0] + M.m[3][0],
                v.x * M.m[0][1] + v.y * M.m[1][1] + v.z * M.m[2][1] + M.m[3][1],
                v.x * M.m[0][2] + v.y * M.m[1][2] + v.z * M.m[2][2] + M.m[3][2]};
}

// The pre-fix basis: the inverse pose's axes.
static Mat4 old_view(const Pose& eye) {
    const Pose inv = pose_inverse(eye);
    const Vec3 r = quat_rotate(inv.q, Vec3{1.0f, 0.0f, 0.0f});
    const Vec3 u = quat_rotate(inv.q, Vec3{0.0f, 1.0f, 0.0f});
    const Vec3 f = quat_rotate(inv.q, Vec3{0.0f, 0.0f, 1.0f});
    Mat4 out = mat4_identity();
    out.m[0][0] = r.x; out.m[0][1] = u.x; out.m[0][2] = f.x;
    out.m[1][0] = r.y; out.m[1][1] = u.y; out.m[1][2] = f.y;
    out.m[2][0] = r.z; out.m[2][1] = u.z; out.m[2][2] = f.z;
    out.m[3][0] = inv.p.x; out.m[3][1] = inv.p.y; out.m[3][2] = inv.p.z;
    return out;
}

int main() {
    float worstNew = 0.0f, worstOld = 0.0f;
    int cases = 0;
    for (int yaw = -180; yaw <= 180; yaw += 15)
        for (int pitch = -75; pitch <= 75; pitch += 15) {
            Pose eye;
            eye.q = quat_from_ypr(deg2rad(static_cast<float>(yaw)), deg2rad(static_cast<float>(pitch)), 0.0f);
            eye.p = Vec3{0.3f, 1.6f, -0.2f};
            // A world point 2 m straight ahead of this eye.
            const Vec3 ahead = v3_add(eye.p, quat_rotate(eye.q, Vec3{0.0f, 0.0f, -2.0f}));
            const Vec3 vNew = row_mul(ahead, mat4_view_from_pose(eye));
            const Vec3 vOld = row_mul(ahead, old_view(eye));
            const Vec3 want{0.0f, 0.0f, -2.0f};
            const float eNew = v3_len(v3_sub(vNew, want));
            const float eOld = v3_len(v3_sub(vOld, want));
            if (eNew > worstNew) worstNew = eNew;
            if (eOld > worstOld) worstOld = eOld;
            ++cases;
        }
    int fails = 0;
    if (!(worstNew < 1e-4f)) { printf("  FAIL view maps 'ahead' to -Z: worst error %.6f m\n", worstNew); ++fails; }
    if (!(worstOld > 0.5f)) {
        printf("  FAIL NEGATIVE CONTROL: the pre-fix basis should miss badly, worst only %.6f m\n", worstOld);
        ++fails;
    }
    printf("xrsim-math: %d eye poses, worst error %.2e m (pre-fix basis: %.3f m), %d failed\n",
           cases, worstNew, worstOld, fails);
    return fails ? 1 : 0;
}
