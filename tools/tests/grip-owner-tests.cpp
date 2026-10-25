// grip-owner-tests.cpp - host tests for src/game/bioshock1r/grip_owner.h, who owns a squeeze of the free
// hand's grip. Run `.\tools\host-test.ps1 grip-owner`.
//
// What has to hold:
//   nearest - the grab nearest its target in reach owns the squeeze; nothing in reach is the game's; a grab not
//             on offer (-1) never wins however near the others are not
//   tie     - on a two-handed gun the fore-grip beats the reload when within a quarter radius of it, and not
//             beyond; on a one-handed gun distance decides
//   zones   - the pouch zones (0.5 reload, 0.25 spare) rank as distances: the spare beats the gun's own part
//             when the hand is at the pouch and the part is 2 radii off (the audit's 00:18 run)
#include "game/bioshock1r/grip_owner.h"

#include <cstdio>

using namespace bvr::b1r::grip_owner;

static int g_checks = 0, g_fails = 0;
static void check(bool ok, const char* what) {
    ++g_checks;
    if (!ok) {
        ++g_fails;
        printf("  FAIL %s\n", what);
    }
}

int main() {
    Policy one, two;
    two.twoHandedGun = true;
    // ---- nearest ----
    check(decide(Reaches{}, one) == Owner::Game, "nothing on offer: the game");
    check(decide(Reaches{2.0f, 3.0f, 1.5f, -1.0f}, two) == Owner::Game, "nothing in reach: the game");
    check(decide(Reaches{0.7f, -1.0f, -1.0f, -1.0f}, one) == Owner::HandOver, "hands together: the hand-over");
    check(decide(Reaches{0.7f, 0.4f, -1.0f, -1.0f}, two) == Owner::TwoHand, "nearer the fore-grip: the two-handed grip");
    check(decide(Reaches{0.3f, 0.4f, -1.0f, -1.0f}, two) == Owner::HandOver, "nearer the other hand: the hand-over");
    check(decide(Reaches{-1.0f, -1.0f, 0.9f, -1.0f}, one) == Owner::Reload, "at the part: the reload");
    check(decide(Reaches{-1.0f, -1.0f, 1.0f, -1.0f}, one) == Owner::Reload, "exactly one radius is in reach");
    check(decide(Reaches{-1.0f, -1.0f, 1.01f, -1.0f}, one) == Owner::Game, "just past it is not");
    check(decide(Reaches{-1.0f, 0.0f, 0.5f, -1.0f}, two) == Owner::TwoHand, "already holding (0): the hold keeps the squeeze");
    // ---- tie ----
    check(decide(Reaches{-1.0f, 0.6f, 0.4f, -1.0f}, two) == Owner::TwoHand, "fore-grip 0.2 radii behind the drum: the fore-grip wins the tie");
    check(decide(Reaches{-1.0f, 0.65f, 0.4f, -1.0f}, two) == Owner::TwoHand, "a quarter radius: still the fore-grip");
    check(decide(Reaches{-1.0f, 0.7f, 0.4f, -1.0f}, two) == Owner::Reload, "past a quarter radius: the drum");
    check(decide(Reaches{-1.0f, 0.6f, 0.4f, -1.0f}, one) == Owner::Reload, "a one-handed gun has no fore-grip rule (control)");
    check(decide(Reaches{-1.0f, 1.2f, 0.98f, -1.0f}, two) == Owner::Reload, "the fore-grip out of reach cannot win a tie");
    check(decide(Reaches{0.3f, 0.6f, 0.4f, -1.0f}, two) == Owner::HandOver, "the tie rule is between the fore-grip and the reload only");
    // ---- zones ----
    check(decide(Reaches{-1.0f, -1.0f, 2.0f, 0.25f}, two) == Owner::Spare, "at the pouch with the drum 2 radii off: the spare");
    check(decide(Reaches{-1.0f, -1.0f, 0.5f, 0.25f}, two) == Owner::Spare, "the spare zone outranks the reload's pouch zone");
    check(decide(Reaches{-1.0f, -1.0f, 0.5f, -1.0f}, two) == Owner::Reload, "the pouch zone alone: the reload");
    check(decide(Reaches{-1.0f, 0.2f, 0.5f, -1.0f}, two) == Owner::TwoHand, "nearer the fore-grip than the pouch zone: the grip");
    check(name(Owner::Reload)[0] == 't' && name(Owner::Game)[0] == 't', "names for the log");

    printf("grip-owner: %d checks, %d failed\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
