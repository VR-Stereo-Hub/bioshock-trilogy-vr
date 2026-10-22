// gun-sounds-tests.cpp - host tests for src/game/bioshock1r/gun_sounds.h, the action-to-sound table.
// Run `.\tools\host-test.ps1 gun-sounds`.
//
// What has to hold:
//   table   - each Tommy and shotgun action has its part with the measured times; the dry fire is any gun's and
//             fires on the weapon; a pistol reload action has no row; the shotgun's back stroke has a fallback
//   reload  - the Tommy program's transitions raise drum out (the eject, or the first pull ending), drum in (the
//             insert ending), bolt back (the slide ending), and nothing else; a phase that does not advance raises
//             nothing; the eject is not doubled with the pull
//   pump    - the back stroke ending raises pump back, the forward stroke ending pump forward, nothing twice
#include "game/bioshock1r/gun_sounds.h"

#include <cstdio>
#include <cstring>

using namespace bvr::b1r;
using namespace bvr::b1r::gun_sounds;
using K = gun_state::Kind;

static int g_checks = 0, g_fails = 0;
static void check(bool ok, const char* what) {
    ++g_checks;
    if (!ok) {
        ++g_fails;
        printf("  FAIL %s\n", what);
    }
}

int main() {
    // ---- table ----
    {
        const Row* r = find(K::Tommy, Action::DrumOut);
        check(r && !strcmp(r->event, "ReloadTommy") && r->fromMs == 0 && r->toMs == 560 && !r->onWeapon, "Tommy drum out: the reload sound's first part");
        r = find(K::Tommy, Action::DrumIn);
        check(r && r->fromMs == 1275 && r->toMs == 1925, "Tommy drum in: 1275-1925");
        r = find(K::Tommy, Action::BoltBack);
        check(r && r->fromMs == 2225 && r->toMs == 2700, "Tommy bolt: 2225-2700");
        r = find(K::Shotgun, Action::PumpBack);
        check(r && !strcmp(r->event, "ShotgunReloadEnd") && r->fromMs == 580 && r->toMs == 710 && r->fallback &&
                  !strcmp(r->fallback, "PumpShotgun"),
              "shotgun pump back: the reload end's clack, PumpShotgun whole as the fallback");
        r = find(K::Shotgun, Action::PumpForward);
        check(r && r->fromMs == 780 && r->toMs == 1060 && !r->fallback, "shotgun pump forward: the slam, no fallback");
        r = find(K::Pistol, Action::DryFire);
        check(r && !strcmp(r->event, "DryFiring") && r->toMs == 0 && r->onWeapon, "dry fire: any gun, whole, on the weapon");
        check(find(K::Pistol, Action::DrumOut) == nullptr && find(K::Pistol, Action::PumpBack) == nullptr, "the pistol's reload has no sound rows yet");
        check(find(K::Shotgun, Action::DrumOut) == nullptr, "the shotgun has no drum");
    }
    // ---- reload ----
    {
        const reload_rig::Program* tommy = reload_rig::entry(L"MachineGun");
        Action out[2];
        check(tommy && reload_actions(tommy, 0, 1, false, out) == 1 && out[0] == Action::DrumOut, "the first pull ending: drum out");
        check(reload_actions(tommy, -1, 0, true, out) == 1 && out[0] == Action::DrumOut, "the eject: drum out");
        check(reload_actions(tommy, 0, 1, true, out) == 1, "the eject and the pull together: once");
        check(reload_actions(tommy, 3, 4, false, out) == 1 && out[0] == Action::DrumIn, "the insert ending: drum in");
        check(reload_actions(tommy, 3, 6, false, out) == 1 && out[0] == Action::DrumIn, "the insert ending straight to the finish (a round in): drum in");
        check(reload_actions(tommy, 4, 5, false, out) == 1 && out[0] == Action::BoltBack, "the bolt racked: bolt back");
        check(reload_actions(tommy, 1, 2, false, out) == 0, "the carry ending (let go): nothing");
        check(reload_actions(tommy, 2, 3, false, out) == 0, "the fetch: nothing (the drum in plays as it seats)");
        check(reload_actions(tommy, 5, 6, false, out) == 0, "the clip playing on: nothing");
        check(reload_actions(tommy, 3, 3, false, out) == 0, "no advance: nothing");
        check(reload_actions(tommy, 3, 2, false, out) == 0, "a step back (a drum let go short of the seat): nothing");
        check(reload_actions(nullptr, 0, 1, false, out) == 0, "no program: nothing");
        const reload_rig::Program* pistol = reload_rig::entry(L"Pistol");
        check(pistol && reload_actions(pistol, 0, 1, false, out) == 1 && find(K::Pistol, out[0]) == nullptr,
              "the pistol's fold raises an action the table has no row for: silent");
    }
    // ---- pump ----
    {
        Action out[1];
        check(pump_actions(false, true, false, out) == 1 && out[0] == Action::PumpBack, "the back stroke ending: pump back");
        check(pump_actions(true, true, true, out) == 1 && out[0] == Action::PumpForward, "the forward stroke ending: pump forward");
        check(pump_actions(true, true, false, out) == 0, "mid forward stroke: nothing");
        check(pump_actions(false, false, false, out) == 0, "mid back stroke: nothing");
        check(pump_actions(false, true, true, out) == 1 && out[0] == Action::PumpBack, "both in one frame: the back first (the forward follows next frame)");
    }

    printf("gun-sounds: %d checks, %d failed\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
