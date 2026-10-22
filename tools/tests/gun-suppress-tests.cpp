// gun-suppress-tests.cpp - host tests for src/game/bioshock1r/gun_suppress.h, what the game may not do per
// gun. Run `.\tools\host-test.ps1 gun-suppress`.
//
// What has to hold:
//   records - the three guns the hands reload take X, adopt the game's reload, own the free hand and the
//             magazine's visibility, and layer no clip but a shot's; only the pistol and Tommy eject on X;
//             a gun with no program leaves everything to the game
//   motion  - the shotgun's shot keeps half its kick through the recoil and none past 0.45 s, rounds or
//             not; the pistol's and Tommy's keep all of it to 0.30, none at 0.45, and none once empty; a
//             clip that is not a shot keeps nothing on a hands-reloaded gun and everything on another
//   muted   - the shotgun mutes its pump and reload-end cues, the Tommy its reload cue, the pistol nothing
#include "game/bioshock1r/gun_suppress.h"

#include <cmath>
#include <cstdio>
#include <initializer_list>

using namespace bvr::b1r;
using namespace bvr::b1r::gun_suppress;
using K = gun_state::Kind;

static int g_checks = 0, g_fails = 0;
static void check(bool ok, const char* what, double got = 0, double want = 0) {
    ++g_checks;
    if (!ok) {
        ++g_fails;
        printf("  FAIL %s: got %.4f want %.4f\n", what, got, want);
    }
}

int main() {
    // ---- records ----
    for (K k : {K::Pistol, K::Tommy, K::Shotgun}) {
        const Record& r = record(k);
        check(r.kind == k, "the record is the gun's");
        check(r.reloadButton && r.adoptGameReload && r.freeHandClip && r.ammoModelVisible && !r.otherClipsMotion,
              "a hands-reloaded gun: X, the adopted reload, the free hand, the magazine, no idle motion");
    }
    check(record(K::Pistol).ejectOnButton && record(K::Tommy).ejectOnButton && !record(K::Shotgun).ejectOnButton,
          "X ejects on the pistol and the Tommy, not the shotgun");
    const Record& o = record(K::Other);
    check(!o.reloadButton && !o.ejectOnButton && !o.adoptGameReload && !o.freeHandClip && !o.ammoModelVisible && o.otherClipsMotion,
          "a gun with no program: the game's");
    // ---- motion ----
    {
        const Record& s = record(K::Shotgun);
        check(std::fabs(clip_motion_keep(s, true, 0.1f, true) - 0.5f) < 1e-6f, "shotgun shot, the recoil: half the kick");
        check(std::fabs(clip_motion_keep(s, true, 0.1f, false) - 0.5f) < 1e-6f, "shotgun shot, empty: still the recoil");
        check(std::fabs(clip_motion_keep(s, true, 0.375f, true) - 0.25f) < 1e-6f, "shotgun shot mid-fade: a quarter");
        check(clip_motion_keep(s, true, 0.5f, true) == 0.0f, "shotgun shot past 0.45: none (the one-handed pump)");
        check(clip_motion_keep(s, false, 0.5f, true) == 0.0f, "shotgun, any other clip: none");
        const Record& t = record(K::Tommy);
        check(clip_motion_keep(t, true, 0.2f, true) == 1.0f, "Tommy shot, the recoil: all of it");
        check(clip_motion_keep(t, true, 0.5f, true) == 0.0f, "Tommy shot past 0.45: none");
        check(clip_motion_keep(t, true, 0.1f, false) == 0.0f, "Tommy shot with no rounds: none (the dip as it runs dry)");
        check(clip_motion_keep(record(K::Pistol), false, 0.0f, true) == 0.0f, "pistol, a reload clip: none");
        check(clip_motion_keep(o, false, 0.0f, true) == 1.0f, "another gun, any clip: all of it");
        check(clip_motion_keep(o, true, 5.0f, false) == 1.0f, "another gun's shot, empty: all of it");
    }
    // ---- muted ----
    {
        check(muted(record(K::Shotgun), L"PumpShotgun") && muted(record(K::Shotgun), L"ShotgunReloadEnd"),
              "the shotgun mutes its pump and reload-end cues");
        check(!muted(record(K::Shotgun), L"FiredSound") && !muted(record(K::Shotgun), nullptr), "and nothing else");
        check(muted(record(K::Tommy), L"ReloadTommy") && !muted(record(K::Tommy), L"PumpShotgun"), "the Tommy mutes its reload cue");
        check(!muted(record(K::Pistol), L"ReloadTommy"), "the pistol mutes nothing");
        check(!muted(o, L"PumpShotgun"), "another gun mutes nothing");
    }

    printf("gun-suppress: %d checks, %d failed\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
