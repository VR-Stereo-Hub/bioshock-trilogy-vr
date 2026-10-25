// mirror-math-tests.cpp - host tests for src/game/bioshock1r/mirror_math.h, BS1's hands
// mirrored across the rig when the gun is in the left hand. Run `.\tools\host-test.ps1 mirror-math`.
//
// A synthetic rig that is mirror-symmetric by construction - the left binds are the right ones
// reflected across a known plane and given a bone-local flip, as a mirrored rig is authored -
// posed with arbitrary rotations, and the module asked to mirror it. What has to hold:
//   - frames and skin matrices round-trip through Aff
//   - the measured mirror is the plane the rig was built with; the rig measures as symmetric
//   - the mirrored skin matrix is a rotation (det > 0): the mesh is not turned inside out
//   - the mirrored pose and the mirrored skin agree: pose' * inverse(bind) == skin'
//   - a left-hand vertex skinned by the mirrored matrix lands on the mirror image of its right
//     twin as the right hand is posed - the left hand shows the right hand's layout, mirrored
//   - the socket bones mirror to proper rotations; a NON-symmetric rig is refused (control)
#include "game/bioshock1r/mirror_math.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

using namespace bvr::b1r;
using mirror::Aff;
using arm_ik::Vec;

static int g_checks = 0, g_fails = 0;
static void check(bool ok, const char* what, double got = 0, double want = 0) {
    ++g_checks;
    if (!ok) {
        ++g_fails;
        printf("  FAIL %s: got %.6f want %.6f\n", what, got, want);
    }
}

static float rnd(unsigned& s) {
    s = s * 1664525u + 1013904223u;
    return static_cast<float>((s >> 8) & 0xFFFF) / 65535.0f * 2.0f - 1.0f;
}
static Aff random_frame(unsigned& s, float spread) {
    float q[4] = {rnd(s), rnd(s), rnd(s), rnd(s) + 1.5f};
    const float n = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    for (float& v : q) v /= n;
    const float p[3] = {rnd(s) * spread, rnd(s) * spread, rnd(s) * spread};
    const float sc[3] = {1, 1, 1};
    return mirror::from_frame(q, p, sc);
}
static float diff(const Aff& a, const Aff& b) {
    float d = 0;
    for (int i = 0; i < 9; ++i) d = std::fmax(d, std::fabs(a.R[i] - b.R[i]));
    for (int i = 0; i < 3; ++i) d = std::fmax(d, std::fabs(a.t[i] - b.t[i]));
    return d;
}
static Aff inv(const Aff& a) {
    Aff o;
    mirror::inverse(a, &o);
    return o;
}

int main() {
    unsigned seed = 12345;
    // Round trips.
    {
        const Aff a = random_frame(seed, 10.0f);
        float q[4], p[3], s[3];
        check(mirror::to_frame(a, q, p, s), "a rotation frame converts back");
        const Aff b = mirror::from_frame(q, p, s);
        check(diff(a, b) < 1e-5f, "frame round trip", diff(a, b), 0);
        float m[16] = {};
        mirror::to_palette(a, m);
        check(diff(mirror::from_palette(m), a) < 1e-6f, "palette round trip");
        // the palette is row-vector: row c is the image of axis c
        check(std::fabs(m[0 * 4 + 1] - a.R[1 * 3 + 0]) < 1e-6f, "palette row 0 is the image of x");
    }

    // A mirror-symmetric rig: the plane x = 3 (normal +x) in rig units.
    const Vec n0{1, 0, 0}, c0{3, 0, 0};
    const Aff mx0 = mirror::reflection(n0, c0);
    Aff local; // the bone-local flip a mirrored rig is authored with
    local.R[4] = -1.0f;
    constexpr int kCount = 47;
    Aff bind[kCount], pose[kCount], pal[kCount];
    for (int i = 0; i < kCount; ++i) bind[i] = random_frame(seed, 5.0f);
    for (int k = 0; k < mirror::kPairs; ++k) {
        Aff& r = bind[mirror::kPairRight[k]];
        r.t[0] = std::fabs(r.t[0]) - 8.0f; // the right side sits at x < 3, left of the plane
        bind[mirror::kPairLeft[k]] = mirror::mul(mx0, mirror::mul(r, local));
    }
    for (int i = 0; i < kCount; ++i) {
        pose[i] = random_frame(seed, 20.0f);
        pal[i] = mirror::mul(pose[i], inv(bind[i])); // skin = pose * inverse(bind)
    }
    // The module measures bind = inverse(skin) * pose itself.
    Aff measured[kCount];
    for (int i = 0; i < kCount; ++i) measured[i] = mirror::mul(inv(pal[i]), pose[i]);
    mirror::Basis b;
    check(mirror::measure(measured, kCount, &b), "a mirrored rig measures as symmetric");
    check(b.residual < 1e-3f, "the twins' rest positions mirror onto each other", b.residual, 0);
    check(std::fabs(b.worstDet + 1.0f) < 1e-3f, "every Mb is a reflection", b.worstDet, -1);
    check(diff(b.mx, mx0) < 1e-4f, "the measured mirror is the rig's plane", diff(b.mx, mx0), 0);

    int skinOk = 0, poseOk = 0, vertOk = 0, rotOk = 0;
    for (int k = 0; k < mirror::kPairs; ++k) {
        const int L = mirror::kPairLeft[k], R = mirror::kPairRight[k];
        const Aff skinL = mirror::mirrored_skin(b, pal[R]);
        const Aff poseL = mirror::mirrored_pose(b, pose[R], b.mbLeft[k]);
        const Aff skinR = mirror::mirrored_skin(b, pal[L]);
        const Aff poseR = mirror::mirrored_pose(b, pose[L], b.mbRight[k]);
        if (mirror::det(skinL.R) > 0.0f && mirror::det(skinR.R) > 0.0f && mirror::det(poseL.R) > 0.0f &&
            mirror::det(poseR.R) > 0.0f)
            ++rotOk;
        if (diff(mirror::mul(poseL, inv(bind[L])), skinL) < 1e-3f && diff(mirror::mul(poseR, inv(bind[R])), skinR) < 1e-3f)
            ++poseOk;
        // A left vertex is its right twin mirrored in bind space.
        const Vec vr{rnd(seed) * 3, rnd(seed) * 3, rnd(seed) * 3};
        const Vec vl = mirror::apply(mx0, vr);
        const Vec got = mirror::apply(skinL, vl);
        const Vec want = mirror::apply(mx0, mirror::apply(pal[R], vr));
        if (arm_ik::length(got - want) < 1e-3f) ++vertOk;
        float q[4], p[3], s[3];
        if (mirror::to_frame(poseL, q, p, s) && std::fabs(s[0] - 1) < 1e-3f && std::fabs(s[1] - 1) < 1e-3f) ++skinOk;
    }
    check(rotOk == mirror::kPairs, "mirrored skin and pose are rotations, not reflections", rotOk, mirror::kPairs);
    check(poseOk == mirror::kPairs, "pose' * inverse(bind) == skin' for every pair", poseOk, mirror::kPairs);
    check(vertOk == mirror::kPairs, "a left vertex lands on its right twin's mirror image", vertOk, mirror::kPairs);
    check(skinOk == mirror::kPairs, "the mirrored poses convert to frames of unit scale", skinOk, mirror::kPairs);
    for (int j = 0; j < mirror::kSockets; ++j) {
        const Aff s = mirror::mirrored_solid(b, pose[mirror::kSocketBone[j]]);
        check(mirror::det(s.R) > 0.0f, "a socket mirrors to a proper rotation (the gun is moved, not flipped)",
              mirror::det(s.R), 1);
        const Vec at = mirror::apply(mx0, mirror::apply(pose[mirror::kSocketBone[j]], Vec{0, 0, 0}));
        check(arm_ik::length(mirror::apply(s, Vec{0, 0, 0}) - at) < 1e-3f, "the socket sits at its mirror image");
        // Something hung off the socket (the gun at grip * offset) lands on its mirror image too.
        const float off[3] = {1.5f, -2.0f, 0.75f};
        const Aff so = mirror::mirrored_solid(b, pose[mirror::kSocketBone[j]],
                                              mirror::solid_axis(b, pose[mirror::kSocketBone[j]]), off);
        const Vec hung = mirror::apply(mx0, mirror::apply(pose[mirror::kSocketBone[j]], Vec{off[0], off[1], off[2]}));
        check(arm_ik::length(mirror::apply(so, Vec{off[0], off[1], off[2]}) - hung) < 1e-3f,
              "what hangs off a socket lands on its mirror image");
        check(mirror::det(so.R) > 0.0f && arm_ik::length(mirror::apply(s, Vec{off[0], off[1], off[2]}) - hung) > 0.1f,
              "control: without the offset it would not");
    }
    // The gun keeps its top up (headset 2026-10-19: it came out upside down). A gun held with its
    // side along the mirror's normal (+x here), its barrel forward (+y) and its top up (+z), in any
    // order of its local axes: mirrored, the barrel still points forward and the top still up.
    {
        const int perms[6][3] = {{0, 1, 2}, {0, 2, 1}, {1, 0, 2}, {1, 2, 0}, {2, 0, 1}, {2, 1, 0}};
        int upOk = 0;
        for (const auto& pm : perms) {
            Aff g; // local axis pm[0] -> barrel (+y), pm[1] -> side (+x), pm[2] -> top (+z)
            for (int i = 0; i < 9; ++i) g.R[i] = 0.0f;
            g.R[1 * 3 + pm[0]] = 1.0f;
            g.R[0 * 3 + pm[1]] = 1.0f;
            g.R[2 * 3 + pm[2]] = 1.0f;
            if (mirror::det(g.R) < 0) g.R[2 * 3 + pm[2]] = -1.0f; // keep it a rotation (top may read -z)
            // tilt it a little, as a hand holds it
            const float tq[4] = {0.05f, 0.08f, -0.04f, 0.99f}, tp[3] = {0, 0, 0}, ts[3] = {1, 1, 1};
            const Aff held = mirror::mul(mirror::from_frame(tq, tp, ts), g);
            const Aff m = mirror::mirrored_solid(b, held);
            const Vec barrel{m.R[0 * 3 + pm[0]], m.R[1 * 3 + pm[0]], m.R[2 * 3 + pm[0]]};
            const Vec top{m.R[0 * 3 + pm[2]], m.R[1 * 3 + pm[2]], m.R[2 * 3 + pm[2]]};
            const float topSign = g.R[2 * 3 + pm[2]];
            if (barrel.y > 0.9f && top.z * topSign > 0.9f) ++upOk;
        }
        check(upOk == 6, "the mirrored gun keeps its barrel forward and its top up, whatever its axes", upOk, 6);
    }
    // A latched axis survives a reload's tilt: the gun rolled 60 degrees about its barrel (its side
    // now tilts toward its top) stays upright with the axis latched at rest, where re-picking per
    // frame could take another axis.
    {
        Aff g; // barrel +y (local 0), side +x (local 1), top +z (local 2)
        for (int i = 0; i < 9; ++i) g.R[i] = 0.0f;
        g.R[1 * 3 + 0] = 1.0f;
        g.R[0 * 3 + 1] = 1.0f;
        g.R[2 * 3 + 2] = -1.0f; // det +1
        const int latched = mirror::solid_axis(b, g);
        check(latched == 1, "at rest the latched axis is the gun's side", latched, 1);
        const float s = std::sin(0.5236f * 1.0f), c = std::cos(0.5236f * 1.0f); // a 60 deg roll about the barrel
        const float rq[4] = {0.0f, s, 0.0f, c}, rp[3] = {0, 0, 0}, rs[3] = {1, 1, 1};
        const Aff tilted = mirror::mul(mirror::from_frame(rq, rp, rs), g);
        const Aff m = mirror::mirrored_solid(b, tilted, latched);
        // mirrored barrel = mirror of the barrel; mirrored top = mirror of the top
        const Vec barrel{m.R[0 * 3 + 0], m.R[1 * 3 + 0], m.R[2 * 3 + 0]};
        const Vec top{m.R[0 * 3 + 2], m.R[1 * 3 + 2], m.R[2 * 3 + 2]};
        const Vec wantB = mirror::apply(mirror::Aff{{-1, 0, 0, 0, 1, 0, 0, 0, 1}, {0, 0, 0}},
                                        Vec{tilted.R[0], tilted.R[3], tilted.R[6]});
        const Vec wantT = mirror::apply(mirror::Aff{{-1, 0, 0, 0, 1, 0, 0, 0, 1}, {0, 0, 0}},
                                        Vec{tilted.R[2], tilted.R[5], tilted.R[8]});
        check(arm_ik::dot(barrel, wantB) > 0.99f && arm_ik::dot(top, wantT) > 0.99f,
              "a tilted gun with the latched axis keeps its barrel and top mirrored", arm_ik::dot(top, wantT), 1);
    }

    // The flipped plane is the GUN's: a gun hung off its grip through a socket turned 20 degrees
    // about the barrel. Flipping the socket's sideways axis keeps the gun's barrel and top as exact
    // mirrors; flipping the grip bone's axis tilts the gun (the "angle isnt quite right" report).
    {
        Aff gripPose; // the grip bone, posed: barrel along its X (+y world), its Y along the mirror normal (+x world)
        for (int i = 0; i < 9; ++i) gripPose.R[i] = 0.0f;
        gripPose.R[1 * 3 + 0] = 1.0f;
        gripPose.R[0 * 3 + 1] = 1.0f;
        gripPose.R[2 * 3 + 2] = -1.0f;
        const float s = std::sin(0.1745f), c = std::cos(0.1745f); // the socket: 20 deg about the barrel (X)
        Aff sock;
        sock.R[0] = 1; sock.R[1] = 0; sock.R[2] = 0;
        sock.R[3] = 0; sock.R[4] = c; sock.R[5] = -s;
        sock.R[6] = 0; sock.R[7] = s; sock.R[8] = c;
        const Aff gun = mirror::mul(gripPose, sock); // the weapon's own frame, world
        const float u[3] = {sock.R[0 * 3 + 1], sock.R[1 * 3 + 1], sock.R[2 * 3 + 1]}; // the socket's Y, grip frame
        const Aff m = mirror::mul(mirror::mirrored_solid_u(b, gripPose, u), sock);
        const Aff bad = mirror::mul(mirror::mirrored_solid(b, gripPose, 1), sock);
        const Vec gTop = mirror::apply(mx0, Vec{gun.R[2], gun.R[5], gun.R[8]}) - mirror::apply(mx0, Vec{0, 0, 0});
        const Vec mTop{m.R[2], m.R[5], m.R[8]}, bTop{bad.R[2], bad.R[5], bad.R[8]};
        check(arm_ik::dot(mTop, gTop) > 0.999f, "flipping the socket's axis keeps the gun's top an exact mirror",
              arm_ik::dot(mTop, gTop), 1);
        check(arm_ik::dot(bTop, gTop) < 0.99f, "control: flipping the grip bone's axis tilts it", arm_ik::dot(bTop, gTop), 0);
        const float e1[3] = {0, 1, 0};
        check(diff(mirror::mirrored_solid_u(b, gripPose, e1), mirror::mirrored_solid(b, gripPose, 1)) < 1e-6f,
              "a bone axis through mirrored_solid_u is mirrored_solid");

        // 2026-10-25 (palette.cpp set_gun_reflection): the solid mirror pinned on the gun's centre plane, then the
        // gun reflected in ITS OWN space (its Y flipped about that plane), is exactly the mirror image of the
        // right-handed gun - whatever the plane.
        Aff grip = gripPose;
        grip.t[0] = 3.0f; grip.t[1] = 41.0f; grip.t[2] = -7.5f;
        for (float plane : {0.0f, -15.4f, 8.2f}) {
            const float off[3] = {u[0] * plane, u[1] * plane, u[2] * plane};
            const Aff solidGun = mirror::mul(mirror::mirrored_solid_u(b, grip, u, off), sock);
            Aff fw; // the gun's own Y flipped about y = plane (weapon space)
            fw.R[4] = -1.0f;
            fw.t[1] = 2.0f * plane;
            const Aff drawn = mirror::mul(solidGun, fw);
            const Aff want = mirror::mul(mirror::mul(b.mx, grip), sock);
            check(diff(drawn, want) < 1e-4f, "the solid mirror then the gun's own reflection is the true mirror image",
                  diff(drawn, want), 0);
        }
    }

    // Negative control: break the symmetry and the rig must be refused.
    for (int k = 0; k < mirror::kPairs; ++k) measured[mirror::kPairLeft[k]].t[1] += 6.0f * (k % 2 ? 1 : -1);
    mirror::Basis bad;
    check(!mirror::measure(measured, kCount, &bad) && !bad.valid, "a rig that is not mirror-symmetric is refused",
          bad.residual, 0);

    printf("mirror-math: %d checks, %d failed\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
