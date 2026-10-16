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
//   - gun fire stays tracked unless its lever is on (Dishonored's default), but its RECOIL
//     plays (2026-10-14): mode 4 pins the gun hand's wrist to the controller every frame,
//     which removed the firing clip's kick along with the idle sway. During a gun's firing
//     states the gun hand's correction is measured from its pose just before the shot
//     instead (the same layering as "gun moves with the reload"), so the clip's kick moves
//     the gun on the controller and it settles back; the shot still leaves along the
//     barrel as it was before the kick (aim.cpp update_barrel holds it). The wrench and the
//     abilities are excluded (animOn, the state name);
//   - a reload plays AT THE GUN by default (2026-10-14, a sibling VR mod's "animation at the hand"):
//     the weapon hand stays on its controller and the whole clip is carried by that hand's
//     correction, so the other hand goes where the clip has it AGAINST THE GUN - it finds the
//     gun wherever you hold it - and comes back to its own controller after. It joins only
//     while the clip has it near the gun (the join distance), so a clip that keeps the free
//     hand low and away leaves it on its controller. Optionally the clip's own motion of the
//     gun hand (the gun tilting for the magazine) plays on top of the controller, measured
//     from the hand's pose just before the reload (BRVR's HandAnim=1 adoption).
//     "view" is the old reload hand-back: both hands go to the flat-screen clip.
// Classification is the Hands actor's own script state (hands_state.h), the BS1 twin of
// Dishonored's FSM read. Timing is hand_compose::Handoff: release 250 ms after the state ends,
// 250 ms in, 350 ms out, smootherstep, a reversal covering only the remaining distance.
// Weight 1 = the controller, 0 = the game's clip.

#include <cstdint>

namespace bvr::b1r::handback {

// Where an animation that needs the hands plays.
enum class Place { Off = 0, AtGun = 1, View = 2 };

// Game thread, once per drive frame: read the state, classify, advance both hands' blends.
void tick(const void* handsActor, int weaponHand, uint64_t nowMs);

// The current weight for a hand (1 = tracked, 0 = the game's animation in view).
float weight(int hand, uint64_t nowMs);

// The free hand while an animation plays at the gun: 0 = its own controller, 1 = carried by
// the weapon hand's correction (where the clip has it against the gun). Before the join gate.
float carry(int hand, uint64_t nowMs);

// The weapon hand while an animation plays at the gun, or a gun fires: how much of the clip's
// own motion of the hand, against its pose just before, plays on the controller (0 = pinned).
float gun_motion(int hand, uint64_t nowMs);

// How much of the carry applies when the clip has the free hand's palm `clipCm` from the
// weapon hand's: 1 within the join distance, 0 beyond it plus 20 cm, smootherstep between.
float join_gate(float clipCm);

// How much the hand is rigid with ITS OWN controller (the motion-vector consumers): the
// hand-back weight less any carry or gun motion.
float tracked(int hand, uint64_t nowMs);

// True while any part of the hand-back is in effect for that hand (owned, releasing,
// blending, carried or moving with the clip) - the arms follow the same weight.
bool active(int hand, uint64_t nowMs);

// F10 (menu_backend): the reload's place, the gun's share of the clip, the join distance.
int reload_place();
void set_reload_place(int place);
float gun_motion_amount();
void set_gun_motion_amount(float v);
float join_cm();
void set_join_cm(float cm);
// F10: the recoil's share of the firing clip's kick (0 = the gun stays pinned, as before).
float recoil_amount();
void set_recoil_amount(float v);

// `vrhands handback ...`: status | melee on|off | swing on|off | scripted on|off |
// fire on|off | recoil <0..1> | reload off|hand|view | gunmotion <0..1> | join <cm> |
// force l|r|both|off | blendms <in> <out> | release <ms>
void handle_command(const char* rest);

// One status line (the HANDS log and the command).
void log_status(uint64_t nowMs);

} // namespace bvr::b1r::handback
