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
// Game thread, each tick: the actors whose events are this gun's (the Hands actor, the weapon), its class name, and
// whether its manual reload is on (the mute table applies only then).
void set_context(void* hands, void* weapon, const wchar_t* weaponClass, bool manualReloadOn);
// Game thread: fire `event` (an effect event name, e.g. "PumpShotgun") on `actor` as the game would - passes the mute.
bool play(void* actor, const char* event);

} // namespace bvr::b1r::sound_engine
