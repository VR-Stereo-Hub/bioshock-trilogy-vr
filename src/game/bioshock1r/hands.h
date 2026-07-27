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
//   mode bones|hands|gun    bones (default, M7-v2) drives the hand-cluster
//                           BONES so the attached weapon follows engine-side
//                           (see bones.h); hands pins the AHands actor
//                           (retired - eye-anchor pivot lever); gun is inert
//                           (the renderer ignores an attached weapon's actor
//                           fields)
//   pose aim|grip           aim = the ray the laser and bullets use (default),
//                           so the barrel agrees with them by construction
//   scale <f>               NOT WIRED YET - no confirmed DrawScale field on
//                           this build; gun size is an open item
//   probe [n]               log every AHands + player-weapon instance, choose none
//   hand l|r|auto           which controller drives the model (auto = the hand
//                           whose trigger was last pulled; default)
//   pos [l|r] <fwd> <right> <up>  model offset in cm, in the model's final
//                           frame; no side = BOTH hands (legacy form, what the
//                           harness and old scripts use)
//   rot [l|r] <pitch> <yaw> <roll>  mesh-alignment trim in degrees, composed in
//                           the controller's LOCAL frame (holds at any
//                           orientation); no side = BOTH hands
//   hideinactive on|off     collapse the whole INACTIVE hand's cluster while
//                           the other drives (default ON; the weapon-attach
//                           bone hides by translation - see bones.h)
//   save | reload           persist / re-read the offsets (per-hand keys in
//                           hands.ini; a legacy suffix-less key loads to both)
//   test <dYaw> <dPitch> [distUU] [holdMs]   camera-relative placement (proves
//                           the write lands; no pose math)
//   simpose <yaw> <pitch> <roll> [holdMs]    synthetic XR controller pose fed
//                           through the REAL mapping path (headset-free check
//                           of the whole transform chain)
//   testclear
void handle_command(const char* args);

// Which controller currently owns the viewmodel (0 left, 1 right): the hand
// whose trigger last fired, or the forced choice. Shared with the aim laser so
// the beam leaves the hand that is actually holding the weapon.
int active_hand();

// Persist the per-hand model offsets to hands.ini (same as `vrhands save`).
// Called by `vrpreset save` too, so the one in-headset save button covers the
// model sliders along with the preset's own values.
void save_offsets();

// Overlay section (render thread).
void draw_debug_ui();

// True while the viewmodel is being driven.
bool active();

} // namespace bvr::b1r::hands
