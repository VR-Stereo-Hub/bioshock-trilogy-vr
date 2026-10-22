// hand-pose-tests.cpp - host tests for src/game/bioshock1r/hand_pose.h, what the free hand is drawn from.
// Run `.\tools\host-test.ps1 hand-pose`.
//
// What has to hold (HELD_GUN_MATRIX.md section 10's table, row by row):
//   held     - two hands on a gun with a rest pose: on the gun, the rest fingers, through a reload, a shot,
//              an empty idle, the lowering; a gun with no rest pose: the controller
//   carry    - a carried part with a designed grip: the controller, designed fingers; without one: the clip's
//   part     - the bolt lock: on the part, the clip's fingers; pulling: the controller, the clip's fingers
//   relaxed  - the hand holding nothing of a gun: relaxed fingers (idle, waiting, the pouch, the drop, the
//              seating, finishing); no gun or the hands lowered or a plasmid: the clip's
//   clip     - a clip that owns the hand beats everything
//   ease     - a source eases in over its in time and out over its out time, from where it was
#include "game/bioshock1r/hand_pose.h"

#include <cmath>
#include <cstdio>
#include <initializer_list>

using namespace bvr::b1r;
using namespace bvr::b1r::hand_pose;

static int g_checks = 0, g_fails = 0;
static void check(bool ok, const char* what, double got = 0, double want = 0) {
    ++g_checks;
    if (!ok) {
        ++g_fails;
        printf("  FAIL %s: got %.4f want %.4f\n", what, got, want);
    }
}

static Inputs base(gun_state::Kind k) {
    Inputs in;
    in.gun.kind = k;
    in.gun.handsReload = k != gun_state::Kind::Other;
    in.hasRestPose = k == gun_state::Kind::Tommy || k == gun_state::Kind::Shotgun;
    in.hasRelaxed = true;
    in.weaponInHand = true;
    return in;
}

int main() {
    using K = gun_state::Kind;
    using St = gun_state::Stage;
    // ---- held ----
    {
        Inputs in = base(K::Shotgun);
        in.held = true;
        Decision d = resolve_free(in);
        check(d.wrist == Wrist::OnGun && d.fingers == Fingers::Rest && d.rule == 2, "held: on the gun, rest fingers");
        in.gun.stage = St::Waiting;
        in.gun.handFree = true;
        d = resolve_free(in);
        check(d.wrist == Wrist::OnGun && d.fingers == Fingers::Rest, "held through a reload's wait: still on the gun");
        in.gun.stage = St::None;
        in.gun.pumpHolding = true;
        d = resolve_free(in);
        check(d.wrist == Wrist::OnGun, "held through a shot's pump wait: on the gun");
        in.gun.pumpHolding = false;
        in.gun.cocked = false;
        d = resolve_free(in);
        check(d.wrist == Wrist::OnGun, "held, uncocked: on the gun");
        Inputs p = base(K::Pistol);
        p.held = true;
        d = resolve_free(p);
        check(d.wrist == Wrist::Controller && d.rule != 2, "a gun with no rest pose: the controller (control)");
        in.held = true;
        in.gun.carryingPart = true;
        in.designedGrip = true;
        d = resolve_free(in);
        check(d.rule == 2, "held outranks a carried part (the hold is refused by the owner table instead)");
    }
    // ---- carry ----
    {
        Inputs in = base(K::Pistol);
        in.gun.stage = St::Carrying;
        in.gun.carryingPart = true;
        in.designedGrip = true;
        Decision d = resolve_free(in);
        check(d.wrist == Wrist::Controller && d.fingers == Fingers::Designed && d.rule == 3, "a carried cylinder: designed fingers");
        in.designedGrip = false;
        d = resolve_free(in);
        check(d.fingers == Fingers::Clip && d.rule == 5, "carried with no table (the Tommy's drum): the clip's hold");
    }
    // ---- part ----
    {
        Inputs in = base(K::Tommy);
        in.gun.stage = St::Waiting;
        in.partLocked = true;
        Decision d = resolve_free(in);
        check(d.wrist == Wrist::OnPart && d.fingers == Fingers::Clip && d.rule == 4, "the bolt: on the part, the clip's fingers");
        in.partLocked = false;
        in.dragging = true;
        d = resolve_free(in);
        check(d.wrist == Wrist::Controller && d.fingers == Fingers::Clip && d.rule == 5, "pulling the drum: the clip's hand on it");
    }
    // ---- relaxed ----
    {
        Inputs in = base(K::Tommy);
        Decision d = resolve_free(in);
        check(d.wrist == Wrist::Controller && d.fingers == Fingers::Relaxed && d.rule == 6, "idle one-handed: relaxed");
        for (St st : {St::Starting, St::Waiting, St::Dropping, St::Seating, St::Finishing}) {
            in.gun.stage = st;
            in.gun.handFree = st == St::Waiting;
            d = resolve_free(in);
            check(d.fingers == Fingers::Relaxed, "the hand holding nothing of the reload: relaxed");
        }
        in.gun.stage = St::None;
        in.hasRelaxed = false;
        d = resolve_free(in);
        check(d.fingers == Fingers::Clip && d.rule == 7, "no relaxed table: the clip's (control)");
        in.hasRelaxed = true;
        in.weaponInHand = false;
        d = resolve_free(in);
        check(d.fingers == Fingers::Clip && d.rule == 7, "a plasmid or nothing in hand: the clip's");
        Inputs o = base(K::Other);
        o.hasRestPose = false;
        d = resolve_free(o);
        check(d.fingers == Fingers::Relaxed, "a gun with no program, one-handed: relaxed too");
    }
    // ---- clip ----
    {
        Inputs in = base(K::Shotgun);
        in.held = true;
        in.clipOwnsHand = true;
        const Decision d = resolve_free(in);
        check(d.wrist == Wrist::Controller && d.fingers == Fingers::Clip && d.rule == 1, "a clip that owns the hand beats the hold");
    }
    // ---- ease ----
    {
        Ease e;
        float w = ease(e, true, 1000, 100.0f, 200.0f);
        check(w < 0.01f, "the first frame of a source: 0", w, 0);
        w = ease(e, true, 1050, 100.0f, 200.0f);
        check(std::fabs(w - 0.5f) < 0.01f, "half way in at half the in time (smootherstep's midpoint)", w, 0.5);
        w = ease(e, true, 1100, 100.0f, 200.0f);
        check(w > 0.999f, "in", w, 1);
        w = ease(e, true, 1500, 100.0f, 200.0f);
        check(w > 0.999f, "stays in", w, 1);
        w = ease(e, false, 1600, 100.0f, 200.0f);
        check(w > 0.999f, "the first frame out: still 1", w, 1);
        w = ease(e, false, 1700, 100.0f, 200.0f);
        check(std::fabs(w - 0.5f) < 0.01f, "half way out at half the out time", w, 0.5);
        w = ease(e, true, 1700, 100.0f, 200.0f);
        check(std::fabs(w - 0.5f) < 0.01f, "turned on again mid-way: continues from where it was", w, 0.5);
        w = ease(e, true, 1750, 100.0f, 200.0f);
        check(std::fabs(w - 0.75f) < 0.01f, "and eases up from there", w, 0.75);
        check(std::fabs(smootherstep(0.5f) - 0.5f) < 1e-6f && smootherstep(-1.0f) == 0.0f && smootherstep(2.0f) == 1.0f,
              "smootherstep clamps");
    }

    printf("hand-pose: %d checks, %d failed\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
