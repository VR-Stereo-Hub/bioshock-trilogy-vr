// wheel-dial-tests.cpp - host tests for src/game/bioshock1r/wheel_mirror.h and wheel_dial.h, the
// BS1 weapon/plasmid wheels chosen by moving the hand. Run `.\tools\host-test.ps1 wheel-dial`.
//
// What has to hold:
//   mirror - items fill in pickup order and a re-add only updates; remove closes the list up;
//            a wheel opens only with 2+ items and not while the other is up; release closes it;
//            a new HUD movie resets; unknown methods change nothing
//   dial   - every slot's stick vector lands in the movie's own wedge for that slot (UpdateRadialAxis
//            ported), and survives the game's 0.225 axis deadzone
//          - for every stage (2..8 filled) and every hand direction, the dial picks the nearest
//            FILLED slot and never an empty one; equal shares (each filled slot owns 360/n deg)
//          - nothing is steered unless the game's wheel is up; a tap (no movement) sends nothing
//          - the centre and orientation freeze at the press: moving the head after opening
//            changes nothing; depth movement does not select
//          - inside the neutral radius the last pick holds (a weapon commits on release)
//          - hysteresis keeps a slot near a boundary
//          - tracking loss neutralises until release; a release resets
//          - the dial faces the opening eye position: "right" on the dial is the viewer's right
#include "game/bioshock1r/wheel_dial.h"
#include "game/bioshock1r/wheel_mirror.h"

#include <cmath>
#include <cstdio>

using namespace bvr::b1r;

static int g_checks = 0, g_fails = 0;
static void check(bool ok, const char* what, double got = 0, double want = 0) {
    ++g_checks;
    if (!ok) {
        ++g_fails;
        printf("  FAIL %s: got %.6f want %.6f\n", what, got, want);
    }
}

static float dz(float v) { // UE2 axis deadzone, rescaled (the game's xRadial DeadZone=0.225)
    const float d = 0.225f;
    const float a = std::fabs(v);
    return a <= d ? 0.0f : (v > 0 ? 1 : -1) * (a - d) / (1 - d);
}

static dial::Input base_input(int filled) {
    dial::Input in;
    in.gripDown = true;
    in.wheelOpen = true;
    in.filled = filled;
    in.handOk = in.headOk = true;
    in.head[0] = 0; in.head[1] = 1.6f; in.head[2] = 0;
    in.hand[0] = 0.25f; in.hand[1] = 1.1f; in.hand[2] = -0.4f;
    return in;
}

// Move the hand in the dial's own plane: angle (deg, CCW from the viewer's right) and distance.
static void move_in_plane(const dial::Dial& d, dial::Input& in, const float c[3], float angDeg, float m) {
    const float r0[3] = {1, 0, 0}, u0[3] = {0, 1, 0};
    float right[3], up[3];
    bvr::xrmath::quat_rotate(d.q[0], d.q[1], d.q[2], d.q[3], r0, right);
    bvr::xrmath::quat_rotate(d.q[0], d.q[1], d.q[2], d.q[3], u0, up);
    const float a = angDeg * 0.01745329f;
    for (int k = 0; k < 3; ++k) in.hand[k] = c[k] + (right[k] * std::cos(a) + up[k] * std::sin(a)) * m;
}

int main() {
    // ---- the mirror -------------------------------------------------------------------------
    {
        wheel::Mirror m;
        check(!m.on_call("UpdateRadialAxis", nullptr), "an unrelated call changes nothing");
        m.on_call("AddWeapon", "Wrench");
        m.on_call("AddWeapon", "Pistol");
        m.on_call("AddWeapon", "Pistol"); // an update, not a new slot
        check(m.list[0].count == 2 && !std::strcmp(m.list[0].item[1], "Pistol"), "pickup order, re-add updates",
              m.list[0].count, 2);
        m.on_call("OpenAbilityMenu", nullptr);
        check(m.open == -1, "no plasmids: the plasmid wheel never opens", m.open, -1);
        m.on_call("OpenWeaponMenu", nullptr);
        check(m.open == wheel::kWeapon, "2 weapons: the weapon wheel opens", m.open, 0);
        m.on_call("AddAbility", "ElectricBoltAbility");
        m.on_call("AddAbility", "IncinerationAbility");
        m.on_call("OpenAbilityMenu", nullptr);
        check(m.open == wheel::kWeapon, "the other wheel cannot open over it", m.open, 0);
        m.on_call("LBReleased", nullptr);
        check(m.open == wheel::kWeapon, "the other release does not close it", m.open, 0);
        m.on_call("RBReleased", nullptr);
        check(m.open == -1, "its release closes it", m.open, -1);
        m.on_call("AddWeapon", "MachineGun");
        m.on_call("RemoveWeapon", "Pistol");
        check(m.list[0].count == 2 && !std::strcmp(m.list[0].item[1], "MachineGun"), "remove closes the list up");
        wheel::Mirror one;
        one.on_call("AddWeapon", "Wrench");
        one.on_call("OpenWeaponMenu", nullptr);
        check(one.open == -1, "1 weapon: the bumper equips it, no wheel", one.open, -1);
        for (int i = 0; i < 10; ++i) { char id[16]; snprintf(id, sizeof id, "W%d", i); one.on_call("AddWeapon", id); }
        check(one.list[0].count == 8, "never more than 8 slots", one.list[0].count, 8);
        one.on_call("InitializeMovie", nullptr);
        check(one.list[0].count == 0 && one.open == -1, "a new HUD movie resets");
        check(wheel::interesting("AddWeapon") && wheel::interesting("LBReleased") &&
              !wheel::interesting("UpdateRadialAxis") && !wheel::interesting("SetHealth"), "the hook's filter");
    }

    // ---- every slot vector lands in the movie's wedge, through the game's deadzone -------------
    for (int i = 0; i < dial::kSlots; ++i) {
        float x, y;
        dial::slot_vector(i, &x, &y);
        check(dial::movie_slot(x, y) == i, "slot vector in its wedge (raw)", dial::movie_slot(x, y), i);
        check(dial::movie_slot(dz(x), dz(y)) == i, "slot vector in its wedge (after the 0.225 deadzone)",
              dial::movie_slot(dz(x), dz(y)), i);
    }

    // ---- stages: nearest filled slot, equal shares, never an empty slot -----------------------
    dial::Config cfg;
    cfg.armMs = 0; // the arming delay has its own test below
    for (int filled = 2; filled <= dial::kSlots; ++filled) {
        int owned[dial::kSlots] = {};
        bool emptyPicked = false;
        for (int a = 0; a < 360; ++a) {
            const int s = dial::nearest_slot((float)a, filled, -1, 0.0f);
            if (s < 0 || s >= filled) emptyPicked = true;
            else ++owned[s];
        }
        check(!emptyPicked, "never an empty slot", filled, 0);
        // Equal shares hold for the stages that are symmetric (2 = up/down, 4 = the cross, 8 = all).
        if (filled == 2 || filled == 4 || filled == 8)
            for (int s = 0; s < filled; ++s)
                check(std::abs(owned[s] - 360 / filled) <= 1, "equal shares", owned[s], 360 / filled);
    }
    check(dial::nearest_slot(10.0f, 2, -1, 0) == 0, "2 items: right of centre goes to the nearer (up)",
          dial::nearest_slot(10.0f, 2, -1, 0), 0);
    check(dial::nearest_slot(-10.0f, 2, -1, 0) == 1, "2 items: below the axis goes down", dial::nearest_slot(-10.0f, 2, -1, 0), 1);
    check(dial::nearest_slot(69.0f, 8, 0, 6.0f) == 0, "hysteresis keeps the slot near a boundary",
          dial::nearest_slot(69.0f, 8, 0, 6.0f), 0);
    check(dial::nearest_slot(60.0f, 8, 0, 6.0f) == 5, "beyond hysteresis it changes", dial::nearest_slot(60.0f, 8, 0, 6.0f), 5);

    // ---- the dial in use ------------------------------------------------------------------------
    {
        dial::Dial d;
        dial::Input in = base_input(8);
        const float c[3] = {in.hand[0], in.hand[1], in.hand[2]};
        dial::Output o = d.update(cfg, in);
        check(o.steer && o.slot == -1 && o.x == 0 && o.y == 0, "a press with no movement sends neutral");
        for (int s = 0; s < 8; ++s) {
            move_in_plane(d, in, c, dial::slot_angle_deg(s), 0.03f);
            o = d.update(cfg, in);
            check(o.slot == s && dial::movie_slot(o.x, o.y) == s, "every direction picks its slot", o.slot, s);
        }
        // The head moves after the opening: nothing about the dial changes.
        in.head[0] += 0.4f; in.headQ[1] = 0.5f; in.headQ[3] = 0.866f;
        move_in_plane(d, in, c, dial::slot_angle_deg(3), 0.03f);
        o = d.update(cfg, in);
        check(o.slot == 3, "frozen orientation: head motion after opening changes nothing", o.slot, 3);
        // Back to the centre: the last pick holds.
        for (int k = 0; k < 3; ++k) in.hand[k] = c[k];
        o = d.update(cfg, in);
        check(o.slot == 3, "inside the neutral radius the pick holds", o.slot, 3);
        // Depth alone selects nothing new.
        dial::Dial d2;
        dial::Input j = base_input(8);
        const float c2[3] = {j.hand[0], j.hand[1], j.hand[2]};
        d2.update(cfg, j);
        const float z0[3] = {0, 0, 1};
        float n[3];
        bvr::xrmath::quat_rotate(d2.q[0], d2.q[1], d2.q[2], d2.q[3], z0, n);
        for (int k = 0; k < 3; ++k) j.hand[k] = c2[k] + n[k] * 0.10f;
        o = d2.update(cfg, j);
        check(o.slot == -1, "pushing toward the dial selects nothing", o.slot, -1);
        // Tracking loss: neutral until release, then a fresh press works.
        j.handOk = false;
        o = d2.update(cfg, j);
        check(o.steer && o.slot == -1 && o.x == 0, "tracking loss: neutral, sticks still held");
        j.handOk = true;
        move_in_plane(d2, j, c2, 90.0f, 0.03f);
        o = d2.update(cfg, j);
        check(o.slot == -1, "no re-seed mid-press after a loss", o.slot, -1);
        j.gripDown = false;
        o = d2.update(cfg, j);
        check(!o.steer && !d2.held, "release resets");
    }
    {   // the game's wheel is not up (0/1 items, or the other hand's wheel): hands off
        dial::Dial d;
        dial::Input in = base_input(1);
        dial::Output o = d.update(cfg, in);
        check(!o.steer, "1 item: the dial never steers");
        in = base_input(5);
        in.wheelOpen = false;
        o = d.update(cfg, in);
        check(!o.steer, "wheel not up: the dial never steers");
    }
    {   // facing: the viewer's right is the dial's right, wherever the hand is
        const float sides[3] = {-0.5f, 0.0f, 0.6f};
        for (float side : sides) {
            dial::Dial d;
            dial::Input in = base_input(8);
            in.hand[0] = side; in.hand[2] = -0.35f;
            d.update(cfg, in);
            const float r0[3] = {1, 0, 0}, z0[3] = {0, 0, 1};
            float right[3], n[3];
            bvr::xrmath::quat_rotate(d.q[0], d.q[1], d.q[2], d.q[3], r0, right);
            bvr::xrmath::quat_rotate(d.q[0], d.q[1], d.q[2], d.q[3], z0, n);
            float to[3] = {in.head[0] - in.hand[0], 0, in.head[2] - in.hand[2]};
            const float l = std::sqrt(to[0] * to[0] + to[2] * to[2]);
            check((n[0] * to[0] + n[2] * to[2]) / l > 0.999f, "the dial faces the opening eye position");
            // viewer looking at the dial: their right = forward x up with forward = -n, = (n.z, 0, -n.x)
            const float vr[3] = {n[2], 0, -n[0]};
            check(right[0] * vr[0] + right[2] * vr[2] > 0.999f && std::fabs(right[1]) < 1e-5f,
                  "the dial's right is the viewer's right, upright");
        }
    }
    {   // analog mode: the raw direction, the movie's own (unequal) wedges, empty slots refused
        dial::Config a;
        a.snap = false;
        a.armMs = 0;
        dial::Dial d;
        dial::Input in = base_input(3);
        const float c[3] = {in.hand[0], in.hand[1], in.hand[2]};
        d.update(a, in);
        move_in_plane(d, in, c, 45.0f, 0.03f);
        dial::Output o = d.update(a, in);
        check(o.slot == -1, "analog: a direction onto an empty slot selects nothing", o.slot, -1);
        move_in_plane(d, in, c, 180.0f, 0.03f);
        o = d.update(a, in);
        check(o.slot == 2, "analog: left picks slot 2", o.slot, 2);
    }

    {   // arming: neutral until the movie listens (its open animation), then the step to the slot
        dial::Config a;   // armMs 250 by default
        dial::Dial d;
        dial::Input in = base_input(4);
        in.nowMs = 1000;
        const float c[3] = {in.hand[0], in.hand[1], in.hand[2]};
        d.update(a, in);
        move_in_plane(d, in, c, 0.0f, 0.03f);  // right, picked early and held still
        in.nowMs = 1100;
        dial::Output o = d.update(a, in);
        check(o.steer && o.x == 0 && o.y == 0 && o.slot == -1, "before arming: neutral, sticks held");
        in.nowMs = 1260;
        o = d.update(a, in);
        check(o.slot == 3 && dial::movie_slot(o.x, o.y) == 3, "after arming: the held pick is sent", o.slot, 3);
        // the step from neutral to the slot is a wedge CROSSING for the movie
        check(dial::movie_slot(0, 0) == -1, "neutral is no wedge, so neutral -> slot crosses into it");
    }

    printf("wheel-dial: %d checks, %d failed\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
