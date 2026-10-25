// hand-loadout-tests.cpp - host tests for src/game/bioshock1r/hand_loadout.h, what each hand holds and which
// hand a trigger pull casts from (dual-wielded plasmids, the base of mix-and-match). Run
// `.\tools\host-test.ps1 hand-loadout`.
//
// What has to hold:
//   resolve - a plasmid goes to the left hand, a weapon to the right; dual puts the same plasmid in the right
//             hand too, and never a weapon
//   route   - no plasmid in either hand: the triggers pass unchanged; a plasmid in both hands: either trigger
//             drives the game's plasmid trigger and the weapon trigger stays quiet; a single plasmid hand leaves
//             the weapon hand's trigger on the weapon trigger
//   latch   - a RISING pull latches its hand, a held one does not latch again; the second hand's pull while the
//             first is down lets the plasmid trigger go for the gap so the game sees a press
//   take    - casts come from the hands that pulled, both together, consumed once; a stale pull is dropped; none
//             fresh falls back to the given hand; a latch on a hand that no longer holds a plasmid does not cast
#include "game/bioshock1r/hand_loadout.h"

#include <cstdio>

using namespace bvr::b1r::loadout;

static int g_checks = 0, g_fails = 0;
static void check(bool ok, const char* what, double got = 0, double want = 0) {
    ++g_checks;
    if (!ok) {
        ++g_fails;
        printf("  FAIL %s: got %.3f want %.3f\n", what, got, want);
    }
}

int main() {
    // resolve
    {
        Loadout a = resolve(true, "ElectricBolt", false, "", false);
        check(a.plasmid(0) && !a.plasmid(1) && !a.weapon(1), "a plasmid alone is the left hand's");
        Loadout b = resolve(true, "ElectricBolt", false, "", true);
        check(b.plasmid(0) && b.plasmid(1) && b.hand[0].same(b.hand[1]), "dual: the same plasmid in both hands");
        Loadout c = resolve(false, "", true, "Shotgun", true);
        check(c.weapon(1) && !c.plasmid(0) && !c.plasmid(1), "dual never duplicates a weapon");
        Loadout d = resolve(false, "", false, "", true);
        check(d.plasmid_hands() == 0 && !d.weapon(0) && !d.weapon(1), "empty hands hold nothing");
    }
    const Loadout dual = resolve(true, "Incineration", false, "", true);
    // Both hands' plasmids on the GAME's one input (no second hands actor): the router merges and latches.
    Loadout shared = dual;
    shared.hand[1].ownHands = false;
    // ownHands: the right hand's plasmid runs on its own hands actor - its trigger never reaches the game.
    {
        check(dual.hand[1].ownHands && !dual.hand[0].ownHands, "dual: the right hand's plasmid is its own");
        CastRouter r;
        const uint8_t right[2] = {0, 255}, left[2] = {255, 0};
        GameTriggers g = r.route(dual, right, 0);
        check(g.plasmid == 0 && g.weapon == 0 && g.own[1], "own hands: the right pull goes to its own hands only",
              g.plasmid, 0);
        g = r.route(dual, left, 10);
        check(g.plasmid == 255 && !g.own[1], "own hands: the left trigger is the game's plasmid trigger", g.plasmid, 255);
        CastPlan p = r.take(dual, 20, -1);
        check(!p.hand[1], "own hands: the game's cast is never the right hand's", p.count(), 1);
        // NEGATIVE CONTROL: the same pull on the shared loadout DOES feed the game
        CastRouter s;
        g = s.route(shared, right, 0);
        check(g.plasmid == 255, "control: shared, the right pull is the game's plasmid trigger", g.plasmid, 255);
    }
    const Loadout single = resolve(true, "Incineration", false, "", false);
    const Loadout gun = resolve(false, "", true, "Pistol", true);
    // route: pass-through
    {
        CastRouter r;
        const uint8_t t[2] = {200, 120};
        GameTriggers g = r.route(gun, t, 0);
        check(g.plasmid == 200 && g.weapon == 120, "no plasmid in hand: the triggers pass as composed", g.plasmid, 200);
        CastPlan p = r.take(gun, 10, -1);
        check(p.count() == 0, "no plasmid: nothing latched", p.count(), 0);
    }
    // route + take: both hands
    {
        CastRouter r;
        const uint8_t right[2] = {0, 255};
        GameTriggers g = r.route(shared, right, 100);
        check(g.plasmid == 255 && g.weapon == 0, "dual: the right trigger is the plasmid trigger", g.weapon, 0);
        g = r.route(shared, right, 110);
        CastPlan p = r.take(shared, 300, 0);
        check(p.hand[1] && !p.hand[0], "dual: a right pull casts from the right hand only");
        p = r.take(shared, 310, 0);
        check(p.hand[0] && !p.hand[1], "consumed: the next cast without a pull is the fallback hand");
        // left then right while the left is held: the right latches and the plasmid trigger gaps
        const uint8_t left[2] = {255, 0}, both[2] = {255, 255};
        r.route(shared, left, 400);
        g = r.route(shared, both, 410);
        check(g.plasmid == 0, "a second hand's pull lets the plasmid trigger go first", g.plasmid, 0);
        g = r.route(shared, both, 420);
        check(g.plasmid == 0, "for the whole gap", g.plasmid, 0);
        g = r.route(shared, both, 430);
        check(g.plasmid == 255, "then presses it again", g.plasmid, 255);
        p = r.take(shared, 500, 0);
        check(p.hand[0] && p.hand[1], "both pulls: the cast comes from both hands", p.count(), 2);
        // held: no second latch
        r.route(shared, both, 600);
        p = r.take(shared, 700, -1);
        check(p.count() == 0, "a held trigger does not latch again", p.count(), 0);
        // NEGATIVE CONTROL: the same sequence with the hand released in between DOES latch again
        const uint8_t none[2] = {0, 0};
        r.route(shared, none, 800);
        r.route(shared, right, 810);
        p = r.take(shared, 900, -1);
        check(p.hand[1], "control: released and pulled again latches", p.count(), 1);
    }
    // stale
    {
        CastRouter r;
        const uint8_t right[2] = {0, 255};
        r.route(shared, right, 0);
        CastPlan p = r.take(shared, CastRouter::kLatchMs + 1, 0);
        check(p.hand[0] && !p.hand[1], "a stale pull is dropped for the fallback");
    }
    // single plasmid: the right (weapon) hand keeps the weapon trigger
    {
        CastRouter r;
        const uint8_t t[2] = {30, 255};
        GameTriggers g = r.route(single, t, 0);
        check(g.plasmid == 30 && g.weapon == 0, "single: only the plasmid hand drives the plasmid trigger", g.plasmid, 30);
        CastPlan p = r.take(single, 10, 0);
        check(p.hand[0] && !p.hand[1], "single: the right pull is not a cast");
    }
    // a latch whose hand stopped holding a plasmid
    {
        CastRouter r;
        const uint8_t right[2] = {0, 255};
        r.route(shared, right, 0);
        CastPlan p = r.take(single, 10, -1);
        check(!p.hand[1], "the right hand let go of the plasmid: its latch does not cast", p.count(), 0);
    }
    printf("hand-loadout: %d checks, %d failed\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
