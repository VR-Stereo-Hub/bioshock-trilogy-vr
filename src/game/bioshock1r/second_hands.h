#pragma once
// 2026-10-25: A SECOND HANDS ACTOR for the right hand's own plasmid - two fully independent plasmids, one per hand
// (the owner's plan: mix-and-match, any plasmid or weapon in either hand). ENGINE_NOTES 2026-10-25 (night, later).
//
// The game's hands (FirstPersonHands.PlayerHands, ShockGame Hands) hold ONE ability with its own state machine
// (AbilityFiring, FinishAbilityFiringWithEve, AbilityIdling, ...), its own fire and idle animations, and the effect
// events that make every plasmid effect ('SelectedAbility' with a persistent context: the palm glow; 'UsedAbility':
// the cast; the anim notifies' cues) - all attached to THAT actor's own bones and sockets. The cast itself is the
// anim notify AnimNotify_UseAbility calling Hands(Owner).UseCurrentAbility(), so it is the cast of whichever hands
// actor plays the animation, charged to the pawn's EVE once (Ability.UseAbility). So a second hands actor of the
// same class, owned by the player, given the plasmid with SetActiveAbility and fired with UseActiveAbility /
// UseActiveAbilityRelease, is a whole second plasmid: its own cast, animation, effects, EVE use and refire.
//
// This module makes and drives it:
//   - spawned (the main hands' own class, owned by the player) while dual-wielded plasmids are on, a plasmid is in
//     hand and the game is in gameplay; destroyed the moment any of that ends (a menu, a cinematic, a load), so a
//     save never holds it;
//   - given the right hand's plasmid in an ability OBJECT OF ITS OWN (own_ability.h; right_choice): the F10
//     RightPlasmid family, else the left's class - never the player's object the game's hands hold, so the two hands'
//     effect flags and states never mix, the same plasmid included (Telekinesis: the player's object of the
//     Telekinesis class the left does not hold);
//   - fired from the right trigger (the router keeps that trigger off the game's own input);
//   - drawing the RIGHT arm itself (palette.cpp: its left hand mirrored onto its right arm, carried into its own
//     space), so the right hand wears its own plasmid's look; the game's mesh hides its right arm only while that
//     draw is happening;
//   - placed each frame so its left palm sits on the drawn right palm, so its effects - attached to its own left
//     hand - appear on the right hand; with no sound pose (a fresh spawn) it stands on the game's hands instead;
//   - removed by the engine's own DestroyActor (forced: Actor.Destroy refuses it), its effect taken away first.
// Self-checks, each logged: a plasmid effect still off after the hands settle is turned on; a LastAbility the right
// hand's equip wrote is put back. Known, not handled yet: its AbilityIdling reads and clears the controller's
// bHoldingFireAbility (the LEFT trigger's hold) on entering; at 0 EVE both hands may inject a hypo; Decoy keeps one
// target per player.

namespace bvr::b1r::second_hands {

// Game thread, once per frame (bones.cpp m4_frame): the main hands actor, and whether the second hands are wanted
// (dual-wielded plasmids on, a plasmid in hand, gameplay).
void tick(void* mainHands, bool wanted);

// The second hands actor, or null.
void* actor();
// A second hands actor whose Destroy did not take yet (2026-10-25): its draw stays collapsed until it is gone.
bool is_leftover(const void* owner);

// The right hand's plasmid (F10 RightPlasmid): 0 the left's, else a family (1 Incinerate! ... 11 Hypnotize Big
// Daddy, the row's order). The right hand takes the player's own class of that family (its highest owned level).
void set_right_plasmid(int family);
int right_plasmid();

// The right trigger as the player holds it (the XInput thread; dual plasmids only).
void set_right_trigger(bool down);

// Game thread, inside a cast (InitiateDamage on `ability`): is it the second hands' cast? Takes the armed cast.
bool on_stack(const void* ability);
// `ability` is the second hands' OWN ability object (not the one the game's hands hold): the right hand's.
bool is_right_ability(const void* ability);
// Any pointer anywhere above this frame on the stack (diagnostics: which hands actor a cast runs under).
bool ptr_on_stack(const void* p);

// The skin gather (palette.cpp, the render thread): the second hands' palette, captured.
void capture_skins(const float* engine, int count);
// The captured palette (47 row-vector 4x4s) when fresh, else null.
const float* skins();

} // namespace bvr::b1r::second_hands
