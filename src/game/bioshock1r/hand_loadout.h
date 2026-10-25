#pragma once
// 2026-10-25: WHAT EACH HAND HOLDS, and which hand a trigger pull casts from - the base of the mix-and-match
// system (the owner's plan: any weapon or plasmid in either hand, the same or mixed, each hand with its own wheel).
// Pure logic, no engine reads: host suite tools\tests\hand-loadout-tests.cpp.
//
// The game itself has ONE hand item at a time: a holdable (drawn in the right hand) or an ability (the left hand,
// "plasmid mode"). This layer says what each PHYSICAL hand holds on top of that, and routes the triggers so every
// hand's pull is that hand's own:
//   - today's rules: the game's item in its own hand; with dual-wielded plasmids on and a plasmid in hand, the
//     right hand holds the SAME plasmid (resolve());
//   - the trigger router: a hand holding a plasmid feeds the game's plasmid trigger, and its pull is latched as a
//     cast from that hand (the engine's cast arrives after the trigger is let go, from an anim notify), so the cast
//     seam can run the cast once per hand that asked (take()).
// The mix-and-match work fills the slots from each hand's wheel instead of from these rules; the router and the
// per-hand cast need no change for a plasmid in either hand, and a weapon slot already routes to the game's
// weapon trigger.
#include <cstdint>
#include <cstring>

namespace bvr::b1r::loadout {

enum class Kind : uint8_t { Empty, Weapon, Plasmid };

struct Slot {
    Kind kind = Kind::Empty;
    char key[48] = {}; // the item: the aim profile key ("ElectricBolt", "Shotgun", ...), "" when unknown
    bool same(const Slot& o) const { return kind == o.kind && strcmp(key, o.key) == 0; }
};

struct Loadout {
    Slot hand[2]; // 0 the left hand, 1 the right hand (physical)
    bool plasmid(int h) const { return hand[h & 1].kind == Kind::Plasmid; }
    bool weapon(int h) const { return hand[h & 1].kind == Kind::Weapon; }
    int plasmid_hands() const { return (plasmid(0) ? 1 : 0) + (plasmid(1) ? 1 : 0); }
};

inline void set_key(Slot& s, const char* key) {
    size_t i = 0;
    for (; key && key[i] && i + 1 < sizeof s.key; ++i) s.key[i] = key[i];
    s.key[i] = 0;
}

// Today's rules. The game's own item goes to its own hand (a weapon in the right, a plasmid in the left); with
// `dualPlasmids` the right hand holds the same plasmid while a plasmid is in hand.
inline Loadout resolve(bool plasmidInHand, const char* plasmidKey, bool weaponInHand, const char* weaponKey,
                       bool dualPlasmids) {
    Loadout l;
    if (plasmidInHand) {
        l.hand[0].kind = Kind::Plasmid;
        set_key(l.hand[0], plasmidKey);
        if (dualPlasmids) l.hand[1] = l.hand[0];
    } else if (weaponInHand) {
        l.hand[1].kind = Kind::Weapon;
        set_key(l.hand[1], weaponKey);
    }
    return l;
}

// The game's two fire triggers as they should reach it.
struct GameTriggers {
    uint8_t plasmid = 0; // the plasmid (ability) trigger
    uint8_t weapon = 0;  // the weapon trigger
};

// Which hands one cast comes from.
struct CastPlan {
    bool hand[2] = {false, false};
    int count() const { return (hand[0] ? 1 : 0) + (hand[1] ? 1 : 0); }
};

// Routes the physical triggers and latches each hand's cast. Not thread-safe: the owner serialises route() (the
// XInput thread) and take() (the game thread).
struct CastRouter {
    static constexpr uint8_t kPull = 64;          // a trigger past this is pulled
    static constexpr uint64_t kLatchMs = 2000;    // a pull older than this asked for a cast that never came
    static constexpr int kGapPolls = 2;           // the plasmid trigger let go this long before a second hand's pull
    bool latched[2] = {false, false};
    uint64_t latchMs[2] = {0, 0};
    bool was[2] = {false, false};
    int gap = 0;

    // `trig` the physical triggers (0 left, 1 right). Only meaningful while some hand holds a plasmid; with none
    // the triggers pass as the game composed them and nothing is latched.
    GameTriggers route(const Loadout& l, const uint8_t trig[2], uint64_t nowMs) {
        GameTriggers out;
        if (l.plasmid_hands() == 0) {
            was[0] = was[1] = false;
            gap = 0;
            out.plasmid = trig[0];
            out.weapon = trig[1];
            return out;
        }
        uint8_t plasmidLevel = 0;
        for (int h = 0; h < 2; ++h) {
            const bool down = trig[h] > kPull;
            if (l.plasmid(h)) {
                if (down && !was[h]) {
                    latched[h] = true;
                    latchMs[h] = nowMs;
                    // another plasmid hand already holding the game's trigger down: let it go first, so the game
                    // sees a fresh press for this one
                    if (was[1 - h] && l.plasmid(1 - h)) gap = kGapPolls;
                }
                if (trig[h] > plasmidLevel) plasmidLevel = trig[h];
            } else if (l.weapon(h)) {
                if (trig[h] > out.weapon) out.weapon = trig[h];
            }
            was[h] = down;
        }
        if (gap > 0) {
            --gap;
            out.plasmid = 0;
        } else {
            out.plasmid = plasmidLevel;
        }
        return out;
    }

    // At the engine's cast: the hands whose pulls asked for it (consumed). None fresh: `fallbackHand` (the game's
    // own cast - a scripted one, or a pull this router never saw).
    CastPlan take(const Loadout& l, uint64_t nowMs, int fallbackHand) {
        CastPlan p;
        for (int h = 0; h < 2; ++h) {
            if (latched[h] && l.plasmid(h) && nowMs - latchMs[h] <= kLatchMs) p.hand[h] = true;
            latched[h] = false;
        }
        if (p.count() == 0 && fallbackHand >= 0) p.hand[fallbackHand & 1] = true;
        return p;
    }
};

} // namespace bvr::b1r::loadout
