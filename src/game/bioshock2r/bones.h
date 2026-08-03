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

// Rigidly place one hand's cluster at the target pose (game space); hand 0 =
// left (plasmid), 1 = right (weapon). Resolves and revalidates the rig lazily
// (one-shot heap scan, dormant after misses, re-armed on view-state change).
// Game thread, CalcView tail only. False = rig not available this frame.
//
// Composition (session 40, the ~90 deg fix): the cluster's authored rotations
// are PRESERVED and rotated by the controller's rotation relative to the
// AHands actor - q_i = qtc * refQ_i, so a controller aiming where the view
// aims leaves the rig exactly where the engine drew it. The old form
// (delta = qtc * conj(refQ_anchor)) replaced the anchor's authored frame with
// the raw controller rotation and cost a constant ~81.6 deg on this rig.
bool drive(const FrameContext& ctx, const GamePose& target, int hand);

// Repaint the last drive's write - the stereo second pass replays CalcView
// and the engine may re-evaluate the skeleton over it. Cheap memcpy, no-op
// when nothing is cached.
void reapply();

// Stop driving one hand's cluster (hand < 0 = both): restores the captured
// reference pose over that cluster's bones and forgets its write, so the
// engine's own animation owns them again. A left-hand release must never
// disturb the right hand's live drive, hence the per-cluster restore.
void release(const char* why, int hand = -1);

// Per-cluster scale multiplier (1.0 = authored). Applied to the pose bank's
// scale channel AND to the anchor-relative translations, so the cluster
// scales about its anchor rather than only thinning the bones. Deliberately
// independent of worldscale (user requirement, session-40 first look).
void set_scale(int hand, float scale);
float scale_of(int hand);

// Scale the weapon-attach (pivot) bone's SCALE CHANNEL with the hand? Some
// weapon attachments inverse-decompose the bone scale (BS1 session-30 class -
// live on BS2: the rifle's ammo drum GROWS when the hand scales down). Off =
// the pivot still MOVES with the scaled hand but keeps its authored scale, so
// the weapon stays authored size. Atomic-only setter, F10-safe.
void set_scale_attach(bool on);
bool scale_attach();

// Arms mode (session 40 round 2): 0 = game (engine animates them - reads as
// FROZEN arms beside driven hands), 1 = follow (arm bones ride their hand's
// cluster rigidly), 2 = hide (collapsed to zero scale - hands + weapon only).
// Atomic-only setter; the game thread applies transitions inside drive(),
// restoring the arm bones from reference when leaving follow/hide (the scale
// channel is never restamped by animation, so a stale zero-scale would strand
// the arms invisible forever - BS1's session-29 lesson).
void set_arms_mode(int mode);
int arms_mode();

// Left-eye flicker fix (session 40 round 2): the engine's animation restamps
// the pose bank mid-draw on SOME frames, after pass 1's CalcView write -
// pass 2 is protected by reapply(), pass 1 was not. Called per ProcessEvent
// dispatch while inside a hooked draw; one 48-byte sentinel compare per
// driven hand, full repaint only when a restamp is actually seen.
void pe_repaint();

// World/view changed under us - drop every cached pointer and the reference.
void on_world_change(const char* why);

// `vrbones <args>`: status | cluster <lo> <hi> <anchor> | refcap | release.
bool handle_command(const char* args);

// Telemetry for vrhands status: one cluster's last written anchor location
// (UU) - the VERIFICATION 2.8 ground truth ("last write loc tracks the
// sweep"), now per hand so each cluster proves its own controller.
bool last_write(int hand, float* x, float* y, float* z, uint64_t* ageMs);

} // namespace bvr::b2r::bones
