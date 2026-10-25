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
//   - given the main hands' plasmid (the shared per-class ability object; a different plasmid per hand is the
//     mix-and-match step, through ShockPlayer.GetAbilityFromClass);
//   - fired from the right trigger (the router keeps that trigger off the game's own input);
//   - its skin matrices captured and its own draw collapsed (palette.cpp): the main mesh's RIGHT hand shows its
//     LEFT hand's animation, mirrored (bones.cpp m4_pose / palette copy_left_skins);
//   - placed each frame so its left palm sits on the drawn right palm, so its effects - attached to its own left
//     hand - appear on the right hand.
// Known, not handled yet: its AbilityIdling reads and clears the controller's bHoldingFireAbility (the LEFT
// trigger's hold) on entering; at 0 EVE both hands may inject a hypo; Telekinesis and Decoy keep one target per
// player; the same plasmid in both hands shares one ability object (its AbilityEffectsTriggered flag).

namespace bvr::b1r::second_hands {

// Game thread, once per frame (bones.cpp m4_frame): the main hands actor, and whether the second hands are wanted
// (dual-wielded plasmids on, a plasmid in hand, gameplay).
void tick(void* mainHands, bool wanted);

// The second hands actor, or null.
void* actor();

// The right trigger as the player holds it (the XInput thread; dual plasmids only).
void set_right_trigger(bool down);

// Game thread, inside an engine call: is the second hands' script on this call's stack (its cast)?
bool on_stack();

// The skin gather (palette.cpp, the render thread): the second hands' palette, captured.
void capture_skins(const float* engine, int count);
// The captured palette (47 row-vector 4x4s) when fresh, else null.
const float* skins();

} // namespace bvr::b1r::second_hands
