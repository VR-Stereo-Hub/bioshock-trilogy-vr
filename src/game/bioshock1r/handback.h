#pragma once
// s88: the hand-back - BS1's twin of Dishonored's anim_state / anim_policy (ANIM-HANDOFF-PLAN,
// PHYSICAL_SWING "The animation belongs to the trigger, not the swing", DISHONORED_PIPELINE 1.8).
//
// The game owns a hand for selected animations and gives it back smoothly:
//   - a TRIGGER melee attack (the wrench's Firing state entered without the physical-swing
//     pulse) owns the weapon hand only - the swing clip plays on it, the other hand stays on
//     its controller. A PHYSICAL swing keeps the tracked hand: the player's arm IS the swing;
//   - a scripted hand animation (PlayingScriptedHandAnimation, the EVE hypo, gatherer tools -
//     BS1's takedowns) owns both hands;
//   - gun fire and reloads stay tracked unless their levers are on (Dishonored's defaults).
// Classification is the Hands actor's own script state (hands_state.h), the BS1 twin of
// Dishonored's FSM read. Timing is hand_compose::Handoff: release 250 ms after the state ends,
// 250 ms in, 350 ms out, smootherstep, a reversal covering only the remaining distance.
// Weight 1 = the controller, 0 = the game's clip.

#include <cstdint>

namespace bvr::b1r::handback {

// Game thread, once per drive frame: read the state, classify, advance both hands' blends.
void tick(const void* handsActor, int weaponHand, uint64_t nowMs);

// The current weight for a hand (1 = tracked, 0 = the game's animation).
float weight(int hand, uint64_t nowMs);

// True while any part of the hand-back is in effect for that hand (owned, releasing or
// blending) - the arms follow the same weight.
bool active(int hand, uint64_t nowMs);

// `vrhands handback ...`: status | melee on|off | swing on|off | scripted on|off |
// fire on|off | reload on|off | force l|r|both|off | blendms <in> <out> | release <ms>
void handle_command(const char* rest);

// One status line (the HANDS log and the command).
void log_status(uint64_t nowMs);

} // namespace bvr::b1r::handback
