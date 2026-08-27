#pragma once
// s67: WHAT THE HANDS ARE DOING, read from the engine's own state machine.
//
// `ShockGame.Hands` is a UnrealScript state machine and gameplay code keys off
// `GetHands().GetStateName()` constantly (see docs "02 player pawn" section 7).
// The states we care about are declared in Hands.uc:
//
//   WeaponEquipping -> WeaponIdling -> WeaponFiring / PostWeaponFiring
//                                   -> WeaponReloading / ProceduralWeaponReloading
//                                   -> WeaponUnEquipping
//
// This is the honest answer to "which animation is playing", and it replaces
// guessing from a movement threshold. The threshold approach cannot tell a
// reload from recoil - they are the same signal at different sizes - which is
// why the viewmodel adopted a reload's settled pose and left the crosshair
// pointing somewhere else until the next shot pulled it back.
//
// DERIVED, NEVER HARDCODED. UE2 keeps the current state in the object's
// FStateFrame, but the offsets differ per build and this project has already
// paid for hardcoded addresses once (BRVR's HANDOFF_12: everything pattern-found
// worked on Steam, Epic and GOG; everything hardcoded broke on two of the three,
// silently). locate() finds both offsets by walking candidate pointers and
// accepting only the one whose UState name resolves, through GNames, to a state
// that Hands.uc actually declares. A wrong offset cannot survive that test.

#include <cstdint>

namespace bvr::b1r::hands_state {

enum class State {
    Unknown = 0,
    Idling,      // WeaponIdling / WeaponZoomedIdling / AbilityIdling
    Equipping,   // WeaponEquipping / Ability*Equipping
    UnEquipping,
    Firing,      // WeaponFiring / WeaponZoomedFiring / AbilityFiring
    PostFiring,  // PostWeaponFiring / PostWeaponZoomedFiring
    Reloading,   // WeaponReloading / ProceduralWeaponReloading
    Zooming,     // WeaponZoomingIn / WeaponZoomingOut
    Scripted,    // PlayingScriptedHandAnimation / InjectingEve / gatherer tools
    Offscreen,   // HandsOffscreen (the auto state)
};

// One-shot derivation against a live Hands actor. Cheap after it succeeds;
// retries with backoff while it fails, and never runs per frame once locked.
// Returns true once both offsets are known.
bool locate(const void* handsActor);

// The live state, or Unknown when it cannot be read. Game thread.
State current(const void* handsActor);

// The raw state name, for logging - never null, "?" when unresolved.
const wchar_t* current_name(const void* handsActor);

// Human-readable for logs and the overlay.
const char* to_string(State s);

// True once locate() has succeeded.
bool located();

// s68b: is the thing in the hands a PLASMID (an ability) rather than a weapon?
//
// The State buckets above deliberately fold WeaponFiring and AbilityFiring into
// one value, because the animation policy does not care which it is. The HAND
// assignment very much does: in BioShock the plasmid is in the LEFT hand and the
// gun is in the right, and every offset, trim and ray in this mod is per hand.
//
// Hands.uc names it directly - the ability states are the ones declared Ability*,
// plus InjectingEve - so this is READ, not inferred. It is what active_hand() now
// uses instead of guessing from the last trigger pulled, which is what left a
// plasmid rendered with the WEAPON's offsets and its shot coming off a different
// ray than the crosshair was drawn from.
//
// current_is_ability() needs the actor and is game-thread only. last_ability() is
// the cached answer for callers that have neither (active_hand runs from more
// than one thread); it reports false once the cache is older than ~500 ms, so a
// stale value can never outlive the rig it came from.
bool current_is_ability(const void* handsActor);
bool last_ability();

// Status line for `vrhands state` and the F10 panel.
void log_status(const void* handsActor);

} // namespace bvr::b1r::hands_state
