#pragma once
// The gun sound engine (2026-10-21). A gun's action sounds - the shotgun's pump, its reload's end - are effect events
// (Hands.PumpShotgun, Hands.ShotgunReloadEnd, ...: EventResponse_SoundEffectsSubsystem objects named <class><event>),
// fired by cues inside the reload and fire clips as they play. A manual reload plays those clips on its own terms -
// a shot's clip runs through its pump cue before the player pumps - so the sounds came on their own ("it always plays
// the cocking sound after shooting"). The engine hooks the one native every effect event goes through
// (AActor::TriggerEffectEvent, patterns.h kActorTriggerEffectEventVtbl), MUTES a gun's action events while its manual
// reload is on, and PLAYS them itself at the moment the player does the action. Every effect event on the hands and
// the gun in hand is logged as it fires (`[sound] event ...`), so each gun's table is read off a run, not guessed.

namespace bvr::b1r::sound_engine {

// Game thread: hook the effect-event native through this actor's vtable (once; fail-soft, logged).
bool install(void* anyActor);
// Game thread, each tick: the actors whose events are this gun's (the Hands actor, the weapon), the events the mod
// fires itself for this gun (its suppression record's null-terminated list, gun_suppress.h; muted when a clip fires
// them), and whether its manual reload is on (the mute applies only then).
void set_context(void* hands, void* weapon, const wchar_t* const* mutedEvents, bool manualReloadOn);
// Game thread: fire `event` (an effect event name, e.g. "PumpShotgun") on `actor` as the game would - passes the mute.
bool play(void* actor, const char* event, const char* referenceTag = nullptr);
// Game thread: fire `event` on `actor`, but of its sound named `sound` (FMOD's name, e.g. weapons_shotgun_reloadEndwav)
// play only fromMs..toMs - one file is a whole action (both strokes of a pump), and each part goes with its own motion.
// The times are read off the sound with tools/fsb-probe.ps1. `lengthMs` is the take the times belong to (FMOD's
// length of it; 0 any): an event answering with another sound (an ammunition variant's own layer - the shotgun's
// electric buck answers ShotgunReloadEnd with an electric take inside the call and its base sound a moment LATER,
// through its second response) plays that one whole when `first` (the action's first part) and stops it otherwise
// (gun_sounds.h Row), and a sound of the take's length starting within 150 ms after is the base, cut to the part.
// False: no FMOD hook, or the event plays another sound (logged) - the caller falls back.
bool play_part(void* actor, const char* event, const char* sound, unsigned fromMs, unsigned toMs, unsigned lengthMs = 0,
               bool first = true);
// Game thread, each tick: stop each part at its end.
void tick();
// The weapon's current ammunition's class name - the ReferenceTag the game's own weapon events carry (DryFiring, ...).
bool ammo_tag(void* weapon, char* out, int n);

} // namespace bvr::b1r::sound_engine
