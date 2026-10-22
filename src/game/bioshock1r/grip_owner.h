#pragma once
// Who owns a squeeze of the free hand's grip, pure (2026-10-21, docs/bioshock1/HELD_GUN_MATRIX.md rule R4 and
// section 8 G1/G7). Host-tested by tools\tests\grip-owner-tests.cpp; dial_input.cpp's arbiter calls it on the
// grip's rising edge.
//
// Four grabs want the same press: the hand-over (the hands together), the two-handed grip (the fore-grip), the
// manual reload (the part it waits for, or the pouch), and the spare magazine (the pouch). Each says how near
// the hand is to its own target over its own radius (a REACH: <= 1 is in reach, -1 is not on offer in this
// state), and the nearest in reach owns the whole squeeze; nothing in reach and it is the game's (the plasmid
// wheel). The one priority over distance: on a two-handed gun the fore-grip wins a tie with the reload - the
// Tommy Gun's drum well sits 20 units behind and below its fore-grip, so a hand reaching under the fore-grip
// is inside both, and the tester's rule is that the gun can always be gripped.
//
// What this replaces: an ACTIVE reload reported reach 0, an absolute claim, whenever its machine said `claim` -
// so inside the drum's 12 cm the two-handed grip could never win (bug 1), and during the 0.7 s drop and the
// seating, which claim without wanting the hand, every squeeze was swallowed (G1). The reload now reports a
// distance in every stage like the others, and -1 where it wants nothing of the hand.

namespace bvr::b1r::grip_owner {

enum class Owner : unsigned char { Game, HandOver, TwoHand, Reload, Spare };

struct Reaches {
    float handOver = -1.0f;  // the controllers' distance over the grab radius
    float twoHand = -1.0f;   // the hand's distance from the fore-grip point over the grab radius (0 while held)
    float reload = -1.0f;    // the reload's: its part's distance over the radius, the pouch zone (0.5), or -1
    float spare = -1.0f;     // the spare magazine's pouch zone (0.25), or -1
};
struct Policy {
    bool twoHandedGun = false; // the fore-grip wins a tie with the reload
    float tieRadii = 0.25f;    // ...within this many radii (3 cm at 12)
};

inline bool in_reach(float r) { return r >= 0.0f && r <= 1.0f; }

inline Owner decide(const Reaches& r, const Policy& p) {
    Owner best = Owner::Game;
    float bestR = 1.0f;
    const float rs[4] = {r.handOver, r.twoHand, r.reload, r.spare};
    const Owner os[4] = {Owner::HandOver, Owner::TwoHand, Owner::Reload, Owner::Spare};
    for (int i = 0; i < 4; ++i)
        if (in_reach(rs[i]) && rs[i] <= bestR) {
            bestR = rs[i];
            best = os[i];
        }
    if (best == Owner::Reload && p.twoHandedGun && in_reach(r.twoHand) && r.twoHand - r.reload <= p.tieRadii)
        best = Owner::TwoHand;
    return best;
}

inline const char* name(Owner o) {
    static const char* const n[] = {"the game", "the hand-over", "the two-handed grip", "the reload", "the spare"};
    return n[static_cast<int>(o)];
}

} // namespace bvr::b1r::grip_owner
