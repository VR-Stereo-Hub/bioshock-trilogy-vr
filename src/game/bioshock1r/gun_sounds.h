#pragma once
// The guns' action sounds as a table, pure (2026-10-21, docs/bioshock1/HELD_GUN_AUDIT.md 2.6; step 6). Host-tested
// by tools\tests\gun-sounds-tests.cpp.
//
// A gun's action sounds are whole files the game cues from its clips (ENGINE_NOTES "The guns' sounds are whole
// actions"): the Tommy Gun's reload is one 2.8 s sound, the shotgun's reload end holds both pump strokes. A manual
// reload plays the clips on its own terms, so the mod mutes those cues (gun_suppress.h) and fires each PART at the
// moment the player does the action, through sound_engine::play_part (the times are the decoded `pcm` timeline of
// tools/fsb-probe.ps1). This table is the one place that says which action plays which part; the actions come from
// the reload machine's phase transitions (by phase KIND, not number) and the pump's strokes.

#include "game/bioshock1r/gun_state.h"
#include "game/bioshock1r/reload_rig.h"

namespace bvr::b1r::gun_sounds {

enum class Action : unsigned char { DrumOut, DrumIn, BoltBack, PumpBack, PumpForward, DryFire };

struct Row {
    gun_state::Kind kind;    // Other: any gun
    Action action;
    const char* event;       // the effect event fired (the game's own response plays the sound)
    const char* sound;       // the part's sound (FMOD's name; the game's are unnamed, so the first the event starts)
    unsigned fromMs, toMs;   // the part; toMs 0: the whole sound
    const char* fallback;    // an event to fire whole when the part cannot be cut (no FMOD hook); null: none
    bool onWeapon;           // fired on the weapon actor (else the hands)
};

// Tommy Gun (HandsReloadTommy -> weapons_tommy_reload_01, 2795 ms; `pcm` timeline): the drum's release and pull
// 50-500, the drum in 1275-1925, the bolt 2225-2700 (back ~2350, home ~2450).
// Shotgun strokes (HandsShotgunReloadEnd -> weapons_shotgun_reloadEndwav): BACK 540-680 (clack 600-660), FORWARD
// 740-900 (slam 840-860); each plays as its stroke ENDS, so each starts near its clack. Without the FMOD hook the
// back stroke plays the whole PumpShotgun as the game would.
inline constexpr Row kRows[] = {
    {gun_state::Kind::Tommy, Action::DrumOut, "ReloadTommy", "weapons_tommy_reload_01", 0, 560, nullptr, false},
    {gun_state::Kind::Tommy, Action::DrumIn, "ReloadTommy", "weapons_tommy_reload_01", 1275, 1925, nullptr, false},
    {gun_state::Kind::Tommy, Action::BoltBack, "ReloadTommy", "weapons_tommy_reload_01", 2225, 2700, nullptr, false},
    {gun_state::Kind::Shotgun, Action::PumpBack, "ShotgunReloadEnd", "weapons_shotgun_reloadEndwav", 580, 710, "PumpShotgun", false},
    {gun_state::Kind::Shotgun, Action::PumpForward, "ShotgunReloadEnd", "weapons_shotgun_reloadEndwav", 780, 1060, nullptr, false},
    {gun_state::Kind::Other, Action::DryFire, "DryFiring", nullptr, 0, 0, nullptr, true},
};

// The row for a gun's action: the gun's own, else the any-gun row, else null (no sound for it).
inline const Row* find(gun_state::Kind kind, Action action) {
    for (const Row& r : kRows)
        if (r.kind == kind && r.action == action) return &r;
    for (const Row& r : kRows)
        if (r.kind == gun_state::Kind::Other && r.action == action) return &r;
    return nullptr;
}

// The actions a reload transition raises, from the phase that ENDED (its kind): a pull of the magazine out
// (or the eject that drops it) -> DrumOut; an insert seated -> DrumIn; a slide (the bolt) racked -> BoltBack.
// Returns how many were written to `out` (at most 2).
inline int reload_actions(const reload_rig::Program* prog, int phaseWas, int phase, bool ejected, Action out[2]) {
    int n = 0;
    if (ejected) out[n++] = Action::DrumOut;
    if (!prog || phaseWas < 0 || phaseWas >= prog->n || phase <= phaseWas) return n;
    switch (prog->phases[phaseWas].kind) {
        case reload_rig::Kind::Pull:
            if (phaseWas == 0 && !ejected) out[n++] = Action::DrumOut; // the first pull: the magazine out
            break;
        case reload_rig::Kind::Insert: out[n++] = Action::DrumIn; break;
        case reload_rig::Kind::Slide: out[n++] = Action::BoltBack; break;
        default: break;
    }
    return n;
}

// The action a pump's update raises: the back stroke just ended, or the forward one.
inline int pump_actions(bool wasFwd, bool backDone, bool done, Action out[1]) {
    if (!wasFwd && backDone) {
        out[0] = Action::PumpBack;
        return 1;
    }
    if (wasFwd && done) {
        out[0] = Action::PumpForward;
        return 1;
    }
    return 0;
}

inline const char* name(Action a) {
    static const char* const n[] = {"drum out", "drum in", "bolt back", "pump back", "pump forward", "dry fire"};
    return n[static_cast<int>(a)];
}

} // namespace bvr::b1r::gun_sounds
