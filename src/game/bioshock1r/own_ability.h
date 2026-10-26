#pragma once
// 2026-10-25: AN ABILITY OBJECT THAT BELONGS TO ONE HAND.
//
// The player keeps ONE Ability object per class (ShockPlayer.GetAbilityFromClass), and the plasmid's state lives on
// that object: AbilityEffectsTriggered (whether its hand effect is on), its pending fire, Telekinesis's target. Two
// hands holding the same class through the player's object share all of it - the 20:24 run's right hand had no hand
// effect (the flag was already set by the left hand, so the right hand's script never triggered its own), and a hand
// effect stayed on with a gun (one hand cleared the shared flag, so the other's clear was skipped).
//
// So a hand the mod drives holds an object of its own: made by the engine's own constructor, exactly as the script's
// `new` makes one (class defaults, so the config values - EVE cost, animations - are the class's). The cast still
// charges the player (Ability.UseAbility takes the instigator), the class is still the player's owned one, and
// nothing in the ability scripts reads the object's Outer. Nothing saved points at it: the hands holding it are gone
// before any save. Unreferenced, it goes with the engine's next garbage collection - so a pointer to one is kept only
// while a live hands actor holds it (second_hands.cpp checks the held object, never a cache).
//
// This is the per-hand half of mix-and-match: any plasmid, or the same plasmid, in either hand.

namespace bvr::b1r::own_ability {

// The game thread: a new Ability object of `abilityClass` (a UClass*) inside `outer` (the player), or null (the
// constructor not found on this build, or it failed). Logged.
void* make(void* abilityClass, void* outer);

// `ability` was made by make() and has not been forgotten.
bool is_own(const void* ability);

// Forget the objects made so far (they are the engine's to collect): a level change, or the hands that held them gone.
void forget_all();

} // namespace bvr::b1r::own_ability
