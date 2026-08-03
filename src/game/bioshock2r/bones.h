#pragma once
// BS2 bone drive MECHANISM: locate the AHands rig's SkeletonInstance, capture
// its reference pose, and rigidly place a configurable bone cluster at a
// game-space target every frame. The POLICY (which controller, what trims)
// lives in hands.cpp; the mechanism/policy split is what lets the actor-mode
// fallback ship if this lane ever stalls on another build.
//
// BS1's bones.cpp is the SHAPE reference (rigid cluster about an anchor,
// composed against the ACTOR transform, reapply on the stereo second pass,
// release() as the deliberate hand-back). Two deliberate omissions:
// - NO render-lock domain (lock/lockgain/lockdgain/lockpull): session 21's
//   user-accepted verdict - the lock's own correction CAUSED the +-90 deg
//   laser-vs-gun drift. BS2 composes at true geometry, full stop.
// - No barrel_ref_axis yet (session 40, with the bone-name map).
//
// Derivation for every offset: patterns.h "the AHands rig" + ENGINE_NOTES
// session 39. The cluster is RUNTIME-CONFIGURABLE (`vrbones cluster`)
// because the per-hand bone split is not yet derived - default drives the
// whole 64-bone rig from one controller; session 40 splits it by name.

#include "game/bioshock2r/frame_context.h"

#include <cstdint>

namespace bvr::b2r::bones {

// Rigidly place the cluster at the target pose (game space). Resolves and
// revalidates the rig lazily (one-shot heap scan, dormant after misses,
// re-armed on view-state change). Game thread, CalcView tail only.
// False = rig not available this frame.
bool drive(const FrameContext& ctx, const GamePose& target);

// Repaint the last drive's write - the stereo second pass replays CalcView
// and the engine may re-evaluate the skeleton over it. Cheap memcpy, no-op
// when nothing is cached.
void reapply();

// Stop driving: restores the captured reference pose once and drops the
// cache, so the engine's own animation owns the rig again.
void release(const char* why);

// World/view changed under us - drop every cached pointer and the reference.
void on_world_change(const char* why);

// `vrbones <args>`: status | cluster <lo> <hi> <anchor> | refcap | release.
bool handle_command(const char* args);

// Telemetry for vrhands status: the last written anchor location (UU) - the
// VERIFICATION 2.8 ground truth ("last write loc tracks the sweep").
bool last_write(float* x, float* y, float* z, uint64_t* ageMs);

} // namespace bvr::b2r::bones
