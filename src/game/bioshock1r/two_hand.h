#pragma once
// The two-handed grip and the gunshot kick (2026-10-19). The pure decisions are in
// two_hand_math.h (host-tested); this module carries them between the two threads that own the
// inputs: the GAME thread (bones.cpp's compose) knows where the drawn gun has the off hand, the
// XINPUT thread (dial_input.cpp's pad override) owns the grip, the bumper and the haptics.
//
// On the two-handed guns (the Tommy Gun, the shotgun, the grenade launcher, the chemical
// thrower, the crossbow) the game's own animation puts the off hand on the gun - BRVR's grab
// point, at no cost. Bring the hand without the gun within 12 cm of where the drawn gun has it
// and it buzzes as the hand-over grab does (60 ms at 0.35, every 50 ms); squeeze its grip and it
// takes hold: the off hand rides the gun where the game has it, and the gun turns about your
// rear hand to lie along both. That grip never reaches the game as a bumper, so reaching for the
// fore-end cannot raise the plasmid wheel. Let go of the grip, or slide the hand 20 cm along the
// gun, to let go. During a reload or any hand-back the game's clip owns the hands; a hold
// resumes after it.

#include "menu_model.h"

#include <cstdint>

namespace bvr::input {
struct PadOverride;
}

namespace bvr::b1r::two_hand {

// ---- game thread (bones.cpp compose) ----
// Whether a hold can apply this frame: on, and the gun held is a two-handed one (by its class,
// cached per holdable).
bool applicable(const void* holdable);
// This frame's numbers: the off hand's distance from where the drawn gun has it, and (held) how
// far it has slid along the gun; `paused` = the hands belong to a clip right now (a hold
// survives it, nothing new is grabbed). `towardCm` = from the hand to that point, fwd/right/up
// in the body's frame, for the probe line. publish_none() = no grab point this frame.
void publish(float distCm, float slideCm, bool paused, const float towardCm[3], uint64_t nowMs);
void publish_none(uint64_t nowMs);
// 0..1: how much the hold applies, eased in and out (smootherstep, 120 ms in, 150 ms out).
float hold_weight(uint64_t nowMs);
// The wrench: its swing never reaches a fire seam (simulator, 2026-10-19), so its kick is the
// hands' firing state beginning while it is held.
bool melee(const void* holdable);
// The gunshot kick: `shotEvent` = the player's fire seam ran or the hands' firing state began this
// evaluation (two_hand_math.h Kick). Pulses the gun hand's controller (and, while two hands are on
// the gun, the other one).
void shot(int gunPhys, int offPhys, bool shotEvent, uint64_t nowMs);

// ---- XInput thread (dial_input.cpp) ----
// The grip of the hand WITHOUT the gun (`gunPhys` the physical hand with it): grabs, lets go,
// withholds its bumper while it is the grip's, buzzes while it could grab. True when this frame
// the free hand's grip belongs to the two-handed grip (the hand-over grab then leaves it alone).
// `locked`: the grip arbiter gave this squeeze to another grab - the grip reads as up here.
bool pad(bvr::input::PadOverride* pad, int gunPhys, bool busy, bool* changed, bool locked);
// For the grip arbiter: the free hand's distance to the grab point over the grab radius (<= 1 in
// reach), 0 while holding, -1 no grab point.
float reach(bool busy);
bool held();
// The grip arbiter hands this squeeze on (a reload ended under it): the next pad() takes the grip
// already down as the grab's press, if the hand is in reach.
void take_held_grip();

// F10 (dial_input routes these): the grip on/off and its distance, the gunshot kick and its strength.
float menu_read(menu::Setting id); // NaN = not this module's
bool menu_write(menu::Setting id, float value);
void draw_status();

} // namespace bvr::b1r::two_hand
