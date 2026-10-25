// gun-geometry-tests.cpp - host tests for src/game/bioshock1r/gun_geometry.h and the generated
// grip_rest_poses.h: the per-gun constants the two-handed grip, the mirror and the reload read instead of
// latching frames. Run `.\tools\host-test.ps1 gun-geometry`.
//
// What has to hold:
//   table   - every two-handed class resolves to a row with a fore-grip; the one-handed ones to a row
//             without; an unknown class to none
//   mirror  - reflecting a point twice gives it back; a point on the plane stays; a direction along the
//             axis flips, one across it does not; the shotgun's fore-grip reflected about its measured
//             plane lands as far on the other side
//   axis    - the socket record's row, normalised; the bone's own axis without a record
//   plane   - the median of the rest bones along the axis, a root at the origin left out, scaled by the
//             DrawScales; no skeleton gives the grip's origin
//   rest    - every rest pose's palm centre is the rig's rigid palm; the fist centre rebuilt from its
//             finger bases (hand_grip.h) and carried to bone 43 by its wrist row lands on that gun's
//             fore-grip point - the two tables were measured from the same clip and must agree
#include "game/bioshock1r/gun_geometry.h"
#include "game/bioshock1r/grip_rest_poses.h"
#include "game/bioshock1r/hand_grip.h"

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
    namespace gg = gun_geometry;
    // ---- table ----
    {
        const wchar_t* two[] = {L"MachineGun", L"Shotgun", L"GrenadeLauncher", L"ChemicalThrower", L"Crossbow"};
        for (const wchar_t* c : two) {
            const gg::Gun* g = gg::find(c);
            check(g && g->twoHanded && arm_ik::length(g->foreGrip) > 10.0f, "a two-handed gun has a fore-grip");
            check(g && gg::rest_pose(c) && gg::rest_pose(c)->n == 15, "and a rest pose of 15 finger bones");
        }
        const gg::Gun* p = gg::find(L"Pistol");
        check(p && !p->twoHanded, "the pistol is one-handed");
        check(gg::rest_pose(L"Pistol") == nullptr, "and has no rest pose");
        check(gg::find(L"ResearchCamera") == nullptr, "an unknown class: no row");
        check(gg::find(nullptr) == nullptr, "no class: no row");
    }
    // ---- the stroke axis and the one-spot grip (2026-10-22 late) ----
    {
        const gg::Gun* sg = gg::find(L"Shotgun");
        const gg::Gun* xb = gg::find(L"Crossbow");
        check(sg && std::fabs(arm_ik::length(sg->strokeDir) - 1.0f) < 1e-3f && sg->strokeDir.z == -1.0f, "the shotgun's stroke axis is the barrel");
        check(xb && std::fabs(arm_ik::length(xb->strokeDir) - 1.0f) < 0.02f && xb->strokeDir.y < 0.0f && xb->strokeDir.z > 0.0f,
              "the crossbow's stroke axis is the lever's swing, down and back (the first stroke goes up and forward)");
        // 2026-10-24: the crossbow mirrors as every gun (the snapshot of its left-handed hold); no gun uses the rule.
        bool anySame = false;
        for (const gg::Gun& g : gg::kGuns) anySame = anySame || g.gripSameSpot;
        check(!anySame, "no gun keeps its free hand on the unmirrored spot");
    }
    // ---- the pump (2026-10-22) ----
    {
        const gg::Gun* s = gg::find(L"Shotgun");
        const float travel = arm_ik::length(s->pumpBack);
        check(std::fabs(travel - 18.0f) < 0.3f, "the shotgun's pump travel is SG_Pump's 18 units", travel, 18.0);
        check(dist(gg::grip_point(*s, 0.0f), s->foreGrip) < 1e-6f, "pump forward: the fore-grip");
        check(dist(gg::grip_point(*s, 1.0f), Vec{38.54f, 8.33f, 7.83f}) < 0.2f, "pump at the rear stop: the Fire clip's fist at 0.85 s");
        check(dist(gg::grip_point(*s, 0.5f), (s->foreGrip + Vec{38.54f, 8.33f, 7.83f}) * 0.5f) < 0.2f, "half way: half way");
        for (const wchar_t* c : {L"MachineGun", L"GrenadeLauncher", L"ChemicalThrower", L"Crossbow", L"Pistol"}) {
            const gg::Gun* g = gg::find(c);
            check(g && arm_ik::length(g->pumpBack) == 0.0f && dist(gg::grip_point(*g, 1.0f), g->foreGrip) < 1e-6f,
                  "a gun with no pump: the anchor never moves");
        }
    }
    // ---- mirror ----
    {
        // The shotgun's measured mirror: socket Y in the grip's frame, the plane 3.63 units toward -u.
        Vec u{-0.377f, 0.925f, -0.059f};
        u = u * (1.0f / arm_ik::length(u));
        const float plane = -3.63f;
        const Vec fg = gg::find(L"Shotgun")->foreGrip;
        const Vec m = gg::mirror_point(fg, u, plane);
        check(dist(gg::mirror_point(m, u, plane), fg) < 1e-4f, "reflected twice: the point itself");
        const float d0 = arm_ik::dot(fg, u) - plane, d1 = arm_ik::dot(m, u) - plane;
        check(std::fabs(d0 + d1) < 1e-4f, "as far on the other side of the plane", d1, -d0);
        check(std::fabs(arm_ik::length(m - u * (d1 + plane)) - arm_ik::length(fg - u * (d0 + plane))) < 1e-3f,
              "the same place across the plane");
        const Vec on = u * plane + Vec{0.925f, 0.377f, 0.0f} * 7.0f; // on the plane
        check(dist(gg::mirror_point(on, u, plane), on) < 1e-3f, "a point on the plane stays");
        check(dist(gg::mirror_dir(u, u), u * -1.0f) < 1e-6f, "the axis flips");
        const Vec across = arm_ik::across(Vec{1, 0, 0}, u);
        check(dist(gg::mirror_dir(across, u), across) < 1e-6f, "a direction across the axis stays");
        // The reflection the live code did by hand (bones.cpp, the per-hand anchor): the same formula.
        float rel[3] = {fg.x, fg.y, fg.z};
        const float d = rel[0] * u.x + rel[1] * u.y + rel[2] * u.z - plane;
        for (int c = 0; c < 3; ++c) rel[c] -= 2.0f * d * (c == 0 ? u.x : c == 1 ? u.y : u.z);
        check(dist(arm_ik::vec(rel), m) < 1e-5f, "matches the hand-written reflection it replaces");
    }
    // ---- axis ----
    {
        const float rec[9] = {0.0f, 0.0f, 2.0f, -0.682f, 1.88f, 0.0f, 1.0f, 0.0f, 0.0f};
        const Vec y = gg::side_axis(rec, 1);
        check(std::fabs(arm_ik::length(y) - 1.0f) < 1e-5f && y.x < 0.0f && y.y > 0.9f, "socket Y, normalised");
        check(dist(gg::side_axis(nullptr, 1), Vec{0, 1, 0}) < 1e-6f, "no record: the bone's Y");
        check(dist(gg::side_axis(nullptr, 2), Vec{0, 0, 1}) < 1e-6f, "no record: the bone's Z");
        const float zero[9] = {};
        check(dist(gg::side_axis(zero, 1), Vec{0, 1, 0}) < 1e-6f, "a degenerate record: the bone's axis");
    }
    // ---- plane ----
    {
        // The pistol's shape: a root at the grip, the centre-line bones 1.4-2.2 off it along Y.
        const float pistol[] = {0, 0, 0, 10, 1.4f, 0, 20, 2.2f, 1, 15, 1.8f, 2, 12, 2.0f, 3};
        check(std::fabs(gg::centre_plane(pistol, 5, 1, 1.0f, 1.0f) - 1.9f) < 1e-5f, "the median of the four, root out",
              gg::centre_plane(pistol, 5, 1, 1.0f, 1.0f), 1.9);
        check(std::fabs(gg::centre_plane(pistol, 5, 1, 0.8f, 0.77f) - 1.9f * 0.8f / 0.77f) < 1e-5f,
              "scaled by the weapon's DrawScale over the hands'");
        // The shotgun's: a root AWAY from the grip (SG_Body) counts.
        const float shotgun[] = {10.1f, -3.7f, -1.7f, 30, -3.5f, 0, 40, -3.6f, 0};
        check(std::fabs(gg::centre_plane(shotgun, 3, 1, 1.0f, 1.0f) + 3.6f) < 1e-5f, "a root off the grip is the gun");
        const float even[] = {0, 0, 0, 1, 2, 0, 1, 4, 0};
        check(std::fabs(gg::centre_plane(even, 3, 1, 1.0f, 1.0f) - 3.0f) < 1e-5f, "an even count: the middle two's mean");
        check(gg::centre_plane(nullptr, 0, 1, 1.0f, 1.0f) == 0.0f, "no skeleton: the grip's origin");
        check(gg::centre_plane(pistol, 1, 1, 1.0f, 1.0f) == 0.0f, "only a root at the origin: the grip's origin");
    }
    // ---- rest ----
    {
        for (const gg::RestPose& r : gg::kRestPoses) {
            check(dist(arm_ik::vec(r.palm), Vec{9.215f, 0.119f, 0.313f}) < 0.01f, "the palm centre is the rig's rigid palm");
            // The fist centre from the rest pose's finger bases, in the wrist's frame (the wrist at the origin).
            Vec i1{}, m1{}, p1{};
            bool have = 0;
            for (int k = 0; k < r.n; ++k) {
                if (r.bones[k].bone == 10) i1 = arm_ik::vec(r.bones[k].p), have = true;
                if (r.bones[k].bone == 13) m1 = arm_ik::vec(r.bones[k].p);
                if (r.bones[k].bone == 19) p1 = arm_ik::vec(r.bones[k].p);
            }
            hand_grip::Frame fr;
            check(have && hand_grip::head_frame(Vec{0, 0, 0}, i1, m1, p1, -1.0f, &fr), "a head frame from the rest fingers");
            const Vec fistLocal = hand_grip::point(fr, hand_grip::kFistCentre);
            // Carried to bone 43 by the wrist row, it is the fore-grip point of the same gun.
            const arm_ik::Quat wq = arm_ik::normalized(arm_ik::quat(r.wristQ));
            const Vec fist43 = arm_ik::vec(r.wristP) + arm_ik::rotate(wq, fistLocal);
            const gg::Gun* g = gg::find(r.cls);
            check(g && dist(fist43, g->foreGrip) < 0.3f, "the rest pose's fist centre is the fore-grip point",
                  g ? dist(fist43, g->foreGrip) : -1.0, 0);
            // A negative control: the wrist itself is not the fore-grip.
            check(g && dist(arm_ik::vec(r.wristP), g->foreGrip) > 5.0f, "the wrist is not the fore-grip (control)");
            const float qn = std::sqrt(r.wristQ[0] * r.wristQ[0] + r.wristQ[1] * r.wristQ[1] + r.wristQ[2] * r.wristQ[2] +
                                       r.wristQ[3] * r.wristQ[3]);
            check(std::fabs(qn - 1.0f) < 1e-3f, "a unit wrist quaternion", qn, 1.0);
        }
    }

    printf("gun-geometry: %d checks, %d failed\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
