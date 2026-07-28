#pragma once
// Session 20: vrrec - record and replay the mod's full per-frame input state
// (head pose, the four hand-funnel poses, the published XR pad) so a headset
// run can be re-examined FLAT, frame for frame, through the exact production
// code paths.
//
// Design (session-19 plan, design-check refined):
// - ONE tap in camera.cpp's CalcView tail, game thread, once per game tick
//   (the stereo second pass skips the drive body, so the tap can't double-
//   fire). It records what the game CONSUMED this frame - an
//   on_present_begin tap cannot record flat (no session, early bail) and
//   simpose injects downstream of the funnel.
// - Replay is frame-for-frame at CalcView cadence (determinism over
//   wall-clock): head via a dedicated replay lane in the camera gate (NOT
//   simHead - that lane is angles-only with deadline semantics), hands via
//   the sim overlay on the input_get_hand_pose funnel (all three consumers -
//   ray, model, laser - read one world), pad via publish_xr_state.
// - The RECENTER STATE + worldScale are serialized in the file header and
//   restored on play: the whole xr->game mapping routes through them, so
//   without this every comparison fails.
// - `vrrec play` refuses while an XR session lives (two writers on the
//   funnel). Recording in-headset is fine - that is the point.
// - `[rec] mark` every 10th frame prints head + the aim-R pose mapped
//   through the SAME pure functions production uses (frame_context.h) +
//   the pad, so record and replay logs are directly comparable.

#include "core/vr/openxr_runtime.h"
#include "game/bioshock1r/frame_context.h"

namespace bvr::b1r::recorder {

// True while a replay is consuming entries (the camera gate checks this
// BEFORE the sim/live lanes - a replayed frame must never recenter or read
// the live head).
bool playing();

// Fill `hp` with the current entry's recorded head pose. False when the
// current entry was recorded with the camera not driven (the replayed frame
// then leaves the camera to the game, faithfully). Only valid while
// playing().
bool replay_head(bvr::vr::HeadPose& hp);

// The once-per-tick tap, called from camera.cpp's CalcView tail right before
// aim/hands consume the frame: records (or replays) this frame's state.
// `consumedHead` is the head pose the camera drive used this frame (zeroed
// when !vrDrove); `liveHead` distinguishes a real headset drive from the
// simhead lane (recorded as the file's source tag).
void on_tick(const FrameContext& fc, const bvr::vr::HeadPose& consumedHead, bool vrDrove,
             bool liveHead);

// Seam command handler: args after the "vrrec" verb (game thread).
//   start             begin recording (header snapshots recenter+worldScale -
//                     recenter BEFORE start, not during)
//   stop              finalize the file / abort a running replay
//   play [file]       replay the newest (or named) recording; refused while
//                     an XR session lives
//   hand [p y r]      arm a static synthetic pose on ALL FOUR funnel slots
//                     (the flat record path: the model, ray and laser follow
//                     it); "vrrec hand off" clears
//   status
void handle_command(const char* args);

} // namespace bvr::b1r::recorder
