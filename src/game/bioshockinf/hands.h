#pragma once
// Infinite hands POLICY: which controller drives the rig, with what trims and
// offsets. Computes the model pose through the SAME frame context the aim ray
// uses (frame_context.h - the agreement is the whole point) and hands it to
// rig::drive. The AIM trims are deliberately NOT applied to the model: the
// model wears its own trims, the ray wears the aim trims, and they meet because
// both ride the same controller pose through the same algebra.
//
// Duplicate-and-adapt of game/bioshock2r/hands.cpp, with BS2's parallel
// 2-element arrays and NO per-hand struct. Every default is ZERO - BS2 bakes in
// its own in-headset calibration and copying those numbers would be exactly the
// cross-game transfer the project rules forbid.
//
// Each hand computes and releases INDEPENDENTLY via the wasDriving exchange
// edge, so a lost left controller can never hand the right hand back mid-aim.
// Only the carrier-owning hand is actually WRITTEN - see rig.h on the
// one-carrier constraint - but both targets are computed and both are printed.

#include "game/bioshockinf/frame_context.h"

#include <cstdint>

namespace bvr::bsi::hands {

// The three per-frame call-ins. Each returns immediately unless
// rig::write_point() names it, so three of them cost one relaxed load.
void on_draw_entry();
void on_draw_exit();
void on_camera_tail();
// Stereo second pass: repaint pass 1's write so the eyes cannot disagree.
void on_second_pass();

// `bsihands <args>`. Mechanism verbs forward to rig::handle_command.
bool handle_command(const char* args);

bool enabled();

// THE SHARED POSE SELECTOR, read by hands.cpp for the model AND by aim.cpp for
// the ray. It lives here rather than in each module precisely so the two can
// never sit on different poses - a gun drawn from the grip pose and a bullet
// fired from the aim pose was BioShock 1's first headset failure. Today both
// default to the aim pose, which is exactly why it must be wired now rather
// than after someone flips one of them.
bool use_aim_pose();
void set_use_aim_pose(bool on);

// Per-hand MODEL tuning (hand 0 = left, 1 = right) for the F10 panel and the
// vrpreset lane. Separate from aim::trim_* by design.
float trim_pitch(int hand);
float trim_yaw(int hand);
float trim_roll(int hand);
float off_fwd_cm(int hand);
float off_right_cm(int hand);
float off_up_cm(int hand);
void set_trim(int hand, float pitchDeg, float yawDeg, float rollDeg);
void set_offset(int hand, float fwdCm, float rightCm, float upCm);

void draw_debug_ui();

} // namespace bvr::bsi::hands
