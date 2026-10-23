#pragma once
// The held gun's state, one record (2026-10-21, docs/bioshock1/HELD_GUN_AUDIT.md 2.1), pure and host-tested
// by tools\tests\gun-state-tests.cpp. manual_reload.cpp assembles it once per tick from the game, the reload
// machine and the pump, and everything that used to keep its own copy - the trigger block, the dry click, the
// two-handed grip's permission, the Tommy's "no drum", the shotgun's "needs cocking" - is a function of it.
//
// THE RULE THAT REPLACES SIX FLAGS: the trigger reaches the game only while fire_allowed(); a pull the gun
// refuses clicks (the game's own DryFiring event, so the game never sees the pull and never clicks twice).
// The audit's bug 2 was a dry Tommy Gun waiting for the hand to pull its drum with nobody clicking: the game's
// click fires only outside its reload state, and the mod's covered only the magazine-out stages.

#include <cstdint>

namespace bvr::b1r::gun_state {

// Launcher (2026-10-22 evening, MANUAL_RELOAD_PLAN.md step 3): the grenade launcher, reloaded as the Tommy Gun -
// its grenade pack is the magazine, off the gun, a new one from the pouch and slapped on; the gun stays together.
enum class Kind : uint8_t { Other, Pistol, Tommy, Shotgun, Launcher };
// Where the gun's magazine is. In: in the gun (rounds or not). Out: dropped, or the gun was left without one.
// InHand: a new one rides the free hand on its way in.
enum class Magazine : uint8_t { In, Out, InHand };
// The manual reload's stage, as the machine reports it (reload_rig.h): None when no manual reload runs.
enum class Stage : uint8_t { None, Starting, Waiting, Carrying, Dropping, Seating, Pumping, Finishing };

struct State {
    Kind kind = Kind::Other;
    bool handsReload = false;   // the gun has a manual-reload program: X and the reload are the hands'
    // From the game (rate-limited reads; -1 unknown).
    int rounds = -1, magazineSize = -1, spareRounds = -1;
    bool switching = false;     // the pending ammunition differs from the current one
    // The magazine and the reload.
    Magazine magazine = Magazine::In;
    bool spareInHand = false;   // a spare magazine prop rides the free hand (the Tommy Gun)
    Stage stage = Stage::None;
    bool carryingPart = false;  // the free hand carries a part of the gun (a drum, a cylinder, a shell)
    bool handFree = false;      // the reload waits for a hand that holds nothing of it (it may grip two-handed)
    bool pumpWait = false;      // the reload's last pump: the two-handed grip works the clip
    // The shotgun's chamber and its pump between shots.
    bool cocked = true;         // false from a reload begun with the gun empty until a stroke completes
    bool pumpHolding = false;   // a shot's clip is held at its pump for the hands (or a stroke is in progress)
    // The hands.
    bool twoHanded = false;     // the free hand holds the fore-end
};

inline bool reload_active(const State& s) { return s.stage != Stage::None; }

// The trigger reaches the game. A manual reload in progress (any gun: the game would cut a shotgun reload
// short and fire the shell it just took; the others cannot fire mid-clip but their pull must not reach the
// game's auto-reload), no magazine, an uncocked shotgun, or a shot's pump waiting for the hands.
inline bool fire_allowed(const State& s) {
    return !reload_active(s) && s.magazine == Magazine::In && s.cocked && !s.pumpHolding;
}
// A pull the gun refuses clicks. (Every refused pull: the trigger is zeroed exactly when this is true, so the
// game never sees one of these and never clicks for it.)
inline bool dry_click_on_pull(const State& s) { return !fire_allowed(s); }

// The two-handed grip may hold now: whenever the free hand holds or works nothing of the gun - not while it
// carries a part (a drum, a cylinder, a shell), not while it pulls or slides one (a reload waiting at a part
// and taken hold of). Everything else - no reload, a reload waiting for the hand, the pouch, the drop, the
// seating, the clip playing on, the last pump (the grip IS the pump), an empty or uncocked gun - allows it,
// and a hold already on is never dropped by a reload beginning. (2026-10-21 23:53 sim run: the drop refused
// the hold and a squeeze at the fore-grip went to the game - HELD_GUN_MATRIX.md G1.)
inline bool two_hand_allowed(const State& s) {
    if (s.twoHanded) return true;
    if (!reload_active(s)) return true;
    return !s.carryingPart && !(s.stage == Stage::Waiting && !s.handFree);
}

// A reload beginning on this gun with these rounds leaves it uncocked (the shotgun loaded from empty: its
// last pump is worked by hand; cut short before it, the hands cock it dry).
inline bool reload_begins_uncocked(Kind k, int rounds) { return k == Kind::Shotgun && rounds == 0; }

// What the shotgun's pump is for right now (manual_reload.cpp's one ratchet): a shot's pump (the Fire clip waits
// at it), the reload's last pump, the dry cock of a gun left uncocked, a pump while the reload waits for a shell.
enum class PumpUse : uint8_t { None, Shot, ReloadEnd, Dry, Loop };
// 2026-10-22 (the 09:31 headset run: an empty gun's reload, started by the game, ended three times in four seconds
// by the aim's own drift read as a pump): what a stroke IS to the gun decides how much of one it takes. Required:
// the gun cannot fire until it is worked - a shot's pump (every shot, the last one too: the gun never pumps itself,
// the tester's rule), the reload's last pump, the dry cock; it stays the quick stroke. Optional: it does something
// but nothing needs it - a pump while the reload waits for a shell (it ends the reload, cocked); a deliberate
// stroke. (A first cut of this, 223bbe5, let an empty gun's shot pump itself and offered no stroke before the first
// shell was in: the 10:14 run read the one as the gun cocking by itself and the other as being refused a cock.
// Every pump is the player's; only its size changes.)
enum class PumpNeed : uint8_t { None, Required, Optional };
inline PumpNeed pump_need(PumpUse use) {
    switch (use) {
        case PumpUse::Shot:
        case PumpUse::ReloadEnd:
        case PumpUse::Dry: return PumpNeed::Required;
        case PumpUse::Loop: return PumpNeed::Optional;
        default: return PumpNeed::None;
    }
}

inline Kind kind_of(const wchar_t* cls) {
    auto same = [](const wchar_t* a, const wchar_t* b) {
        if (!a || !b) return false;
        while (*a && *b && *a == *b) ++a, ++b;
        return *a == *b;
    };
    if (same(cls, L"Pistol")) return Kind::Pistol;
    if (same(cls, L"MachineGun")) return Kind::Tommy;
    if (same(cls, L"Shotgun")) return Kind::Shotgun;
    if (same(cls, L"GrenadeLauncher")) return Kind::Launcher;
    return Kind::Other;
}

inline const char* stage_name(Stage s) {
    static const char* const n[] = {"none", "starting", "waiting", "carrying", "dropping", "seating", "pumping", "finishing"};
    return n[static_cast<int>(s)];
}

} // namespace bvr::b1r::gun_state
