#pragma once
// What the game is not allowed to do while the mod drives a gun, one record per gun, pure (2026-10-21,
// docs/bioshock1/HELD_GUN_AUDIT.md 2.4; step 4). Host-tested by tools\tests\gun-suppress-tests.cpp.
//
// The facts were spread over a dozen flags: X stripped by `g_handsReload`, the trigger by the fire block, the
// game's own reload adopted by `in.adopt`, the gun hand's clip motion by `g_gunKeep` and `gun_motion_keep()`
// (one curve for the shotgun, another for the pistol and Tommy Gun, in two branches of tick), the free hand's
// clip pose by the hand-back's manual-reload case, the sound cues by `sound_engine::kMuted`, the magazine's
// visibility by four `ShowAmmoModel`/`HideAmmoModel` call sites. This is the one place that says it, per gun.

#include "game/bioshock1r/gun_state.h"

namespace bvr::b1r::gun_suppress {

// How much of a FIRE clip's own motion of the gun hand is layered on the controller: `kick` through the
// recoil, faded to 0 between fadeT0 and fadeT1 (clip seconds), none at all once the gun is empty when
// `onlyWithRounds`. The rest of a shotgun's Fire clip is the one-handed pump (the gun swung up to present
// it) and the Tommy Gun's firing state points the gun down as it runs dry: neither is the player's.
struct ClipMotion {
    float kick = 1.0f;
    float fadeT0 = 0.30f, fadeT1 = 0.45f;
    bool onlyWithRounds = false;
};

struct Record {
    gun_state::Kind kind;
    bool reloadButton;       // X never reaches the game (the reload is the hands')
    bool ejectOnButton;      // ...and X ejects the magazine instead (a program that fetches a new one)
    bool adoptGameReload;    // a reload the game begins itself (the gun ran dry) is held for the hands
    bool freeHandClip;       // the free hand never follows a reload clip: it is the hand pose engine's
    bool ammoModelVisible;   // the magazine's visibility is the mod's (the clip's notifies are overridden)
    bool otherClipsMotion;   // the gun hand follows clips other than a shot's (idle fidgets, reloads)
    ClipMotion fire;         // a shot's clip
    const wchar_t* const* mutedEvents; // effect events the mod fires itself (sound_engine), null-terminated
};

inline constexpr const wchar_t* kShotgunMuted[] = {L"PumpShotgun", L"ShotgunReloadEnd", nullptr};
inline constexpr const wchar_t* kTommyMuted[] = {L"ReloadTommy", nullptr};
inline constexpr const wchar_t* kNoneMuted[] = {nullptr};

inline constexpr Record kRecords[] = {
    // kind,                 X,    eject, adopt, freeHand, ammoVis, otherClips, fire{kick, fade0, fade1, onlyWithRounds}, muted
    {gun_state::Kind::Pistol,  true, true, true, true, true, false, {1.0f, 0.30f, 0.45f, true}, kNoneMuted},
    {gun_state::Kind::Tommy,   true, true, true, true, true, false, {1.0f, 0.30f, 0.45f, true}, kTommyMuted},
    // 2026-10-22: the launcher as the Tommy (its pack the magazine); no sound of its own is cut yet.
    {gun_state::Kind::Launcher, true, true, true, true, true, false, {1.0f, 0.30f, 0.45f, true}, kNoneMuted},
    // 2026-10-22 late: the crossbow; its box comes off by hand (no eject on X yet); no sound of its own is cut.
    {gun_state::Kind::Crossbow, true, false, true, true, true, false, {1.0f, 0.30f, 0.45f, true}, kNoneMuted},
    {gun_state::Kind::Shotgun, true, false, true, true, true, false, {0.5f, 0.30f, 0.45f, false}, kShotgunMuted},
    // Other: a gun with no program - the game keeps everything.
    {gun_state::Kind::Other,   false, false, false, false, false, true, {1.0f, 1e9f, 1e9f, false}, kNoneMuted},
};

inline const Record& record(gun_state::Kind k) {
    for (const Record& r : kRecords)
        if (r.kind == k) return r;
    return kRecords[5];
}

// The gun hand's clip motion to layer this frame, 0..1: a fire clip's by the record's curve (smootherstep
// from kick to 0 over fadeT0..fadeT1), any other clip's 1 or 0 by `otherClipsMotion`.
inline float clip_motion_keep(const Record& r, bool fireClip, float clipT, bool rounds) {
    if (!fireClip) return r.otherClipsMotion ? 1.0f : 0.0f;
    if (r.fire.onlyWithRounds && !rounds) return 0.0f;
    float u = (clipT - r.fire.fadeT0) / (r.fire.fadeT1 - r.fire.fadeT0);
    u = u < 0 ? 0 : u > 1 ? 1 : u;
    return r.fire.kick * (1.0f - u * u * (3.0f - 2.0f * u));
}

inline bool muted(const Record& r, const wchar_t* event) {
    auto same = [](const wchar_t* a, const wchar_t* b) {
        if (!a || !b) return false;
        while (*a && *b && *a == *b) ++a, ++b;
        return *a == *b;
    };
    for (const wchar_t* const* e = r.mutedEvents; e && *e; ++e)
        if (same(*e, event)) return true;
    return false;
}

} // namespace bvr::b1r::gun_suppress
