#pragma once
// I4 vrrec for BioShock Infinite - record and replay the per-frame head state
// (plus the hand-funnel poses and the published XR pad, recorded now for
// forward-compatibility with I7) so a headset run can be re-examined FLAT,
// frame for frame, through the exact production drive path.
//
// Adapted from bioshock1r/recorder.cpp (the decoupling directive: copy, never
// share), with ONE structural change: BS1 taps once per CalcView, which IS the
// game tick there. Infinite's camera seam (GetPlayerViewPoint) fires 1000-9600
// times a second - many times per frame - so both the record tap and the
// replay cursor advance are gated on a d3d11_hook::present_count() edge
// instead: one entry per rendered frame, and the cadences match at record and
// replay by construction. The edge detection lives in camera.cpp's drive tail
// (game thread), so this module still needs no locks of its own.
//
// Carried BS1 lessons, verbatim:
// - The RECENTER STATE + worldScale are serialized in the file header and
//   restored on play - the whole xr->game mapping routes through them.
// - `vrrec play` refuses while an XR session lives (two writers on the
//   funnel). Recording in-headset is fine - that is the point.
// - The command seam delivers the raw line INCLUDING its trailing newline;
//   skip all whitespace or `play` reads "\n" as a filename (errno 22).
// - Files are read with _wfsopen(_SH_DENYNO): the fopen_s family opens
//   non-sharable and a fresh recording is routinely still held by the search
//   indexer / AV scan.
// - `[rec] mark` every 10th frame prints the head quat and the driven camera
//   so a record log and a replay log are directly comparable.

#include "core/vr/openxr_runtime.h"
#include "game/bioshockinf/inf_math.h"

namespace bvr::bsi::recorder {

// True while a replay is consuming entries (the drive checks this BEFORE the
// sim/live lanes - a replayed frame must never recenter or read the live
// head).
bool playing();

// Fill `hp` with the current entry's recorded head pose. False when the
// current entry was recorded with the camera not driven (the replayed frame
// then leaves the camera to the game, faithfully). Only valid while
// playing().
bool replay_head(bvr::vr::HeadPose& hp);

// The once-per-frame tap, called from the drive tail on a present-count edge
// (camera.cpp owns the edge detection). `consumedHead` is the head pose the
// drive used this frame (zeroed when !vrDrove); `liveHead` distinguishes a
// real headset drive from the simhead lane; loc/rot are the FINAL camera
// handed back to the game, for the mark lines.
void on_tick(const bvr::vr::HeadPose& consumedHead, bool vrDrove, bool liveHead,
             const FVector& loc, const FRotator& rot);

// Seam command handler: args after the "vrrec" verb (game thread).
//   start        begin recording (header snapshots recenter+worldScale -
//                recenter BEFORE start, not during)
//   stop         finalize the file / abort a running replay
//   play [file]  replay the newest (or named) recording; refused while an XR
//                session lives
//   status
void handle_command(const char* args);

} // namespace bvr::bsi::recorder
