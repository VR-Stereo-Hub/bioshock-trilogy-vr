#pragma once
// Per-gun geometry, pure (2026-10-21, docs/bioshock1/HELD_GUN_AUDIT.md section 2.3). What the two-handed
// grip, the left-hand mirror and the manual reload need to know about a gun - where the front hand holds
// it, which way is sideways, where its centre plane is - is a constant of the gun in its grip bone's
// frame (the hands rig's bone 43, the weapon's socket), measured once offline. Nothing here is latched
// from a frame of a clip at runtime: the audit traced five regressions to latches of that kind (the
// anchor re-latched from a substituted pose, a rest taken from an equip's last frame, an axis chosen on
// a reload frame). Host-tested by tools\tests\gun-geometry-tests.cpp.
//
// THE FORE-GRIP POINT is the free hand's FIST CENTRE (hand_grip.h: the point the controller's grip pose
// pins, rebuilt from the wrist and the index/middle/pinky bases) against bone 43 in the gun's idle clip,
// Unreal axes, rig units at the authored size. Measured in Blender on the hands rig (NEWPlayerHands-all.glb,
// tools\bsmesh) over each Fidget clip at 0, 0.5, 1, 2 and 3 s; every clip holds it within 0.3 units.
// The shotgun's checks against the running game: 2946 idle frames of six pose recordings on 2026-10-21
// put the live fist centre at 54.03 14.69 14.31 (spread 0.2) - the offline number to 0.1.
// The clip's point is the drawn gun's only at the authored size: two_hand_math.h on_drawn_gun maps it.
//
// THE SIDEWAYS AXIS is one of the weapon socket's axes (the socket record on bone 43 is the weapon's own
// frame in the bone's frame): every gun's is its Y (read at rest on every gun in every 2026-10-21 run;
// the wrench, with no socket record, uses the grip bone's Y). The centre plane the mirror flips about is
// the median of the weapon skeleton's rest bones along that axis (the pistol's centre line sits 1.4-2.2
// units off its grip root; a rigid holdable uses its mesh's render bounds instead, bones.cpp).

#include "arm_ik.h"

#include <algorithm>
#include <cmath>

namespace bvr::b1r::gun_geometry {

using arm_ik::Vec;

struct Gun {
    const wchar_t* cls;
    bool twoHanded;   // the game's idle clip puts the off hand on the gun: it has a fore-grip
    Vec foreGrip;     // the two-handed anchor: the free fist centre against bone 43 (see above)
    int sideAxis;     // which socket axis (0 X, 1 Y, 2 Z) lies across the gun: the mirror flips it
    // 2026-10-22: where the fore-grip goes when the pump is worked - the free fist's travel from foreGrip to
    // the pump's rear stop, bone-43 frame (the shotgun's Fire clip, 0.75 -> 0.85 s: 17.97 units, SG_Pump's 18).
    // The anchor rides it by the pump's position (manual_reload::pump_back), so the hand follows the slide.
    Vec pumpBack;
    // 2026-10-22 (the crossbow in the left hand: the right hand's grip "off to the left of the weapon"): a gun
    // whose fore-grip is one place both hands take - the crossbow's stock, wrapped from above - mirrors about
    // the plane through the fore-grip itself, not the gun's centre line, so the other hand lands on the same
    // spot. A gun gripped from either side of a centred fore-end (the shotgun's pump) keeps the centre plane.
    bool mirrorAtGrip;
};

// Measured 2026-10-21 (the fist centre, left hand, against R_grip; clip times 0..3 s):
//   Shotgun  FidgetShotgun / EmptyFidgetShotgun  54.08 14.75 14.29 .. 53.81 14.40 14.28
//   TommyGun FidgetTommygun / EmptyFidgetTommygun 44.94 5.44 2.83 .. 45.01 5.57 3.07
//   GrenadeLauncher FidgetLauncher 51.33 20.49 15.12 .. 51.45 20.65 15.18
//   ChemicalThrower FidgetChem 12.47 -10.78 22.50 .. 12.52 -10.67 22.72
//   Crossbow FidgetCrossbow 26.90 8.55 -3.47 .. 27.00 8.71 -3.36
// The shotgun's pump travel (2026-10-22, the same fist in Shotgun__FireShotgun): 54.05 14.75 14.32 at 0.75 s,
// 38.54 8.38 7.85 at 0.85 s, back at 1.15 s.
inline constexpr Gun kGuns[] = {
    {L"MachineGun", true, {45.0f, 5.5f, 3.0f}, 1, {0.0f, 0.0f, 0.0f}, false},
    {L"Shotgun", true, {54.05f, 14.7f, 14.3f}, 1, {-15.51f, -6.37f, -6.47f}, false},
    {L"GrenadeLauncher", true, {51.4f, 20.6f, 15.2f}, 1, {0.0f, 0.0f, 0.0f}, false},
    {L"ChemicalThrower", true, {12.5f, -10.8f, 22.5f}, 1, {0.0f, 0.0f, 0.0f}, false},
    {L"Crossbow", true, {26.9f, 8.7f, -3.4f}, 1, {0.0f, 0.0f, 0.0f}, true},
    {L"Pistol", false, {0.0f, 0.0f, 0.0f}, 1, {0.0f, 0.0f, 0.0f}, false},
    {L"Wrench", false, {0.0f, 0.0f, 0.0f}, 1, {0.0f, 0.0f, 0.0f}, false},
};

// The two-handed anchor with the pump this far back (0 forward, 1 at the rear stop).
inline Vec grip_point(const Gun& g, float pumpBack) { return g.foreGrip + g.pumpBack * pumpBack; }

// The mirror plane's position along the sideways axis u: the gun's own centre line as measured (`centreLine`),
// or the fore-grip's own position along u for a gun that mirrors at its grip.
inline float mirror_plane(const Gun& g, Vec u, float centreLine) {
    return g.mirrorAtGrip ? arm_ik::dot(g.foreGrip, u) : centreLine;
}

// The free hand's rest pose on a two-handed gun (grip_rest_poses.h, generated): each left-hand bone
// against the wrist (engine bone 6), the wrist against bone 43, the palm centre in the wrist. The hold
// draws this whenever it holds, so a shot, an empty idle or a reload clip can never open the fingers or
// turn the wrist on the gun; a hand-over or a weapon switch cannot lose it (nothing is latched).
struct RestBone {
    int bone;    // engine bone index (7..21: the left thumb, index, middle, ring, pinky joints)
    float p[3];  // against the wrist, rig units
    float q[4];  // against the wrist, x y z w
};
struct RestPose {
    const wchar_t* cls;
    float wristP[3];  // the wrist (bone 6) against bone 43
    float wristQ[4];
    float palm[3];    // the palm centre in the wrist's frame (the five palm bones' mean)
    const RestBone* bones;
    int n;
};

inline bool same(const wchar_t* a, const wchar_t* b) {
    if (!a || !b) return false;
    while (*a && *b && *a == *b) ++a, ++b;
    return *a == *b;
}
// The gun's row by its class name, or null (a gun this table does not know: no fore-grip, Y sideways).
inline const Gun* find(const wchar_t* cls) {
    for (const Gun& g : kGuns)
        if (same(g.cls, cls)) return &g;
    return nullptr;
}

// ---- the mirror ---------------------------------------------------------------------------------
// The gun in the left hand is the right-hand pose mirrored with the gun's sideways axis u flipped about
// its centre plane (mirror_math.h mirrored_solid_u), so a point of the gun given in the grip's frame is
// the same point reflected about that plane: p - 2 (p.u - plane) u. A direction has no plane.
inline Vec mirror_point(Vec p, Vec u, float plane) { return p - u * (2.0f * (arm_ik::dot(p, u) - plane)); }
inline Vec mirror_dir(Vec d, Vec u) { return d - u * (2.0f * arm_ik::dot(d, u)); }

// The sideways axis as a unit vector in the grip's frame from the socket record (floats 3-11 are the
// socket's X, Y, Z axes in the bone's frame) - or the bone's own axis when there is no record.
inline Vec side_axis(const float* socketAxes /* 9 floats, rows X Y Z; null: none */, int axis) {
    const int a = axis < 0 ? 0 : axis > 2 ? 2 : axis;
    Vec u{a == 0 ? 1.0f : 0.0f, a == 1 ? 1.0f : 0.0f, a == 2 ? 1.0f : 0.0f};
    if (socketAxes) {
        const Vec v = arm_ik::vec(socketAxes + 3 * a);
        const float l = arm_ik::length(v);
        if (l > 1e-6f) u = v * (1.0f / l);
    }
    return u;
}

// The centre plane along the sideways axis, in the grip's units: the median of the weapon skeleton's
// rest bone positions' coordinate along that axis INDEX (the skeleton is in the weapon's own frame, the
// one the socket's axes give), a root sitting at the origin left out (it is the grip, not the gun), times
// the weapon's DrawScale over the hands'. n < 1 (no skeleton): 0, the grip's own origin.
inline float centre_plane(const float* restP /* n x 3 */, int n, int axis, float weaponDrawScale, float handsDrawScale) {
    if (!restP || n < 1) return 0.0f;
    const int a = axis < 0 ? 0 : axis > 2 ? 2 : axis;
    float ys[64];
    int m = 0;
    for (int i = 0; i < n && m < 64; ++i) {
        const float* p = restP + 3 * i;
        if (i == 0 && p[0] * p[0] + p[1] * p[1] + p[2] * p[2] < 0.25f) continue;
        ys[m++] = p[a];
    }
    if (m == 0) return 0.0f;
    std::sort(ys, ys + m);
    const float med = (m % 2) ? ys[m / 2] : 0.5f * (ys[m / 2 - 1] + ys[m / 2]);
    const float kw = weaponDrawScale > 0.01f ? weaponDrawScale : 1.0f;
    const float kh = handsDrawScale > 0.01f ? handsDrawScale : 1.0f;
    return med * kw / kh;
}

} // namespace bvr::b1r::gun_geometry
