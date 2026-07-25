#pragma once
// M7 visible hands + weapons: the first-person viewmodel follows the
// CONTROLLER instead of the camera.
//
// The engine draws the player's hands and current weapon as one actor
// (`AHands`, a single mesh carrying the hands plus a short forearm stub) that
// it places relative to the view every tick. This module finds that live actor
// by its class vtable - the same heap scan that locates UShockUserSettings,
// since there is no static pointer to it - and rewrites its Location/Rotation
// each frame from the hand's GRIP pose.
//
// GRIP, not AIM: the grip pose is where the hand physically is, which is what a
// model wants. The aim pose stays with the fire ray (aim.cpp). Both are built
// from the same per-frame transform (frame_context.h) so the drawn gun and the
// fired bullet cannot disagree.
//
// Everything is command-gated (`vrhands ...`) and fail-soft: no actor found, no
// pose, a scripted camera, or the master switch off all leave the engine's own
// placement untouched.

#include "core/hooks/pattern_scan.h"
#include "game/bioshock1r/frame_context.h"

namespace bvr::b1r::hands {

// Resolve-time wiring. Nothing is scanned here - the actor does not exist until
// a world is loaded, so the lookup is lazy and re-validating.
void init(const bvr::pattern_scan::ProcessImage& image);

// Once per frame from the CalcView drive, on the game thread, after the camera
// is final. This is deliberately the LAST thing to touch the viewmodel in the
// frame: the engine places it during its own tick, so our write has to land
// afterwards to win.
void on_calcview(const FrameContext& ctx);

// Seam command handler: args after the "vrhands" verb (game thread).
//   on | off | status
//   probe [n]               log every AHands instance found, choose none
//   hand l|r|auto           which controller drives the model (auto = the hand
//                           whose trigger was last pulled; default)
//   pos <fwd> <right> <up>  model offset in cm, in the grip's own frame
//   rot <pitch> <yaw> <roll>  model rotation trim, degrees
//   save | reload           persist / re-read the offsets
//   test <dYaw> <dPitch> [holdMs]   synthetic offset from the camera, for
//                           headset-free verification that the write lands
void handle_command(const char* args);

// Overlay section (render thread).
void draw_debug_ui();

// True while the viewmodel is being driven.
bool active();

} // namespace bvr::b1r::hands
