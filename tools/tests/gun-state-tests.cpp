// gun-state-tests.cpp - host tests for src/game/bioshock1r/gun_state.h, the held gun's one record and the
// rules read off it. Run `.\tools\host-test.ps1 gun-state`.
//
// What has to hold:
//   fire     - a loaded, cocked, idle gun fires; a reload in progress (any gun, any stage), no magazine, an
//              uncocked shotgun, or a shot's pump held for the hands refuses the pull - and every refused
//              pull clicks (the two are one rule)
//   two-hand - the shotgun grips at any time but while a shell is carried in a reload; the others grip when
//              no reload runs, at the reload's last pump, while the reload waits for a free hand, or when
//              the hold is already on
//   cocked   - only the shotgun, only a reload begun empty, leaves the gun uncocked
//   kind     - the three guns by class; anything else is Other
#include "game/bioshock1r/gun_state.h"

#include <cstdio>
#include <initializer_list>

using namespace bvr::b1r::gun_state;

static int g_checks = 0, g_fails = 0;
static void check(bool ok, const char* what) {
    ++g_checks;
    if (!ok) {
        ++g_fails;
        printf("  FAIL %s\n", what);
    }
}

int main() {
    // ---- fire ----
    {
        State s;
        s.kind = Kind::Tommy;
        s.rounds = 12;
        check(fire_allowed(s) && !dry_click_on_pull(s), "a loaded idle gun fires");
        s.rounds = 0;
        check(fire_allowed(s), "an empty idle gun: the game's own pull (it reloads or clicks itself)");
        for (Stage st : {Stage::Starting, Stage::Waiting, Stage::Carrying, Stage::Dropping, Stage::Seating, Stage::Pumping,
                         Stage::Finishing}) {
            s.stage = st;
            check(!fire_allowed(s) && dry_click_on_pull(s), "a reload in progress refuses the pull and clicks");
        }
        s.stage = Stage::Waiting;
        s.magazine = Magazine::In;
        check(dry_click_on_pull(s), "the dry Tommy waiting at its drum clicks (the audit's bug 2)");
        s.stage = Stage::None;
        s.magazine = Magazine::Out;
        check(!fire_allowed(s) && dry_click_on_pull(s), "no magazine: refused, clicks");
        s.magazine = Magazine::InHand;
        check(!fire_allowed(s), "a magazine on its way in: refused");
        s.magazine = Magazine::In;
        s.kind = Kind::Pistol;
        s.stage = Stage::Waiting;
        check(dry_click_on_pull(s), "the pistol's reload wait clicks too");
        s.stage = Stage::None;
        s.kind = Kind::Shotgun;
        s.cocked = false;
        check(!fire_allowed(s) && dry_click_on_pull(s), "an uncocked shotgun: refused, clicks");
        s.cocked = true;
        s.pumpHolding = true;
        check(!fire_allowed(s) && dry_click_on_pull(s), "a shot's pump waiting for the hands: refused, clicks");
        s.pumpHolding = false;
        check(fire_allowed(s), "pumped: fires again (control)");
        State o;
        check(fire_allowed(o), "the default record fires (a gun with no program)");
    }
    // ---- two-hand ----
    {
        State s;
        s.kind = Kind::Shotgun;
        check(two_hand_allowed(s), "shotgun idle: grips");
        s.stage = Stage::Waiting;
        check(two_hand_allowed(s), "shotgun reloading, no shell in hand: grips");
        s.carryingPart = true;
        check(!two_hand_allowed(s), "shotgun carrying a shell to the port: that hand is the shell's");
        s.stage = Stage::None;
        check(two_hand_allowed(s), "a carried flag with no reload does not refuse (control)");
        s.carryingPart = false;
        s.cocked = false;
        check(two_hand_allowed(s), "uncocked: grips (the hands cock it)");
        State t;
        t.kind = Kind::Tommy;
        check(two_hand_allowed(t), "Tommy idle: grips");
        t.stage = Stage::Carrying;
        t.carryingPart = true;
        check(!two_hand_allowed(t), "Tommy carrying a drum: no new hold");
        t.carryingPart = false;
        t.stage = Stage::Waiting;
        check(!two_hand_allowed(t), "Tommy reload waiting at a part the hand is on: no");
        t.handFree = true;
        check(two_hand_allowed(t), "the reload waits for a hand that holds nothing of it: grips");
        t.handFree = false;
        t.twoHanded = true;
        check(two_hand_allowed(t), "a hold already on survives a reload beginning");
        t.twoHanded = false;
        t.stage = Stage::Pumping;
        t.pumpWait = true;
        check(two_hand_allowed(t), "the reload's last pump: the grip is the pump");
    }
    // ---- cocked ----
    {
        check(reload_begins_uncocked(Kind::Shotgun, 0), "a shotgun reload begun empty: uncocked");
        check(!reload_begins_uncocked(Kind::Shotgun, 3), "with shells in: stays cocked");
        check(!reload_begins_uncocked(Kind::Shotgun, -1), "rounds unknown: stays cocked (never block a gun on a guess)");
        check(!reload_begins_uncocked(Kind::Tommy, 0), "a Tommy begun empty: no chamber to cock");
        check(!reload_begins_uncocked(Kind::Pistol, 0), "a pistol begun empty: no chamber to cock");
    }
    // ---- kind ----
    {
        check(kind_of(L"Pistol") == Kind::Pistol && kind_of(L"MachineGun") == Kind::Tommy && kind_of(L"Shotgun") == Kind::Shotgun,
              "the three guns by class");
        check(kind_of(L"Wrench") == Kind::Other && kind_of(nullptr) == Kind::Other && kind_of(L"Pistol2") == Kind::Other,
              "anything else is Other");
    }

    printf("gun-state: %d checks, %d failed\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
