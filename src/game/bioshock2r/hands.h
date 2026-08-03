#pragma once
// BS2 hands POLICY: which controller drives the rig, with what trims and
// offsets. Computes the model pose through the SAME frame context the aim
// ray uses (frame_context.h - the agreement is the whole point) and hands it
// to bones::drive. The aim trim is deliberately NOT applied to the model
// (BS1 shape: the model wears its own trims; the ray wears the aim trims;
// they meet because both ride the same controller pose).
//
// Session 40: BOTH hands drive, independently - the left cluster rides the
// left controller (BS2's plasmid hand) and the right cluster plus the weapon
// rides the right, each with its own trims, offset and scale, each releasing
// on its own controller's loss. BS2 is natively dual-wield; unlike BS1 there
// is no "inactive hand" to hide.

#include "game/bioshock2r/frame_context.h"

#include <cstdint>

namespace bvr::b2r::hands {

// Per-frame entry point, camera CalcView tail, AFTER aim::on_calcview
// (deliberately last, BS1 ordering). Game thread only.
void on_calcview(const FrameContext& ctx, bool strictGameplay);

// `vrhands <args>`: on|off | status | trim l|r <p> <y> <r> |
// offset l|r <f> <r> <u> | scale [l|r] <f> | pose aim|grip.
bool handle_command(const char* args);

bool enabled();

// Per-hand tuning accessors (hand 0 = left, 1 = right) for the F10 panel and
// the vrpreset lane.
float trim_pitch(int hand);
float trim_yaw(int hand);
float trim_roll(int hand);
float off_fwd_cm(int hand);
float off_right_cm(int hand);
float off_up_cm(int hand);
void set_trim(int hand, float pitchDeg, float yawDeg, float rollDeg);
void set_offset(int hand, float fwdCm, float rightCm, float upCm);

} // namespace bvr::b2r::hands
