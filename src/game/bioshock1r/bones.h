#pragma once
// M7-v2 bone drive: place the viewmodel's HAND CLUSTER at the controller by
// writing the engine's own evaluated skeleton, so everything the engine
// derives from bones - the attached weapon's render transform above all -
// follows for free (the where-you-write principle, ENGINE_NOTES).
//
// Mechanism (all offsets in patterns.h, derivations in ENGINE_NOTES):
// the AHands actor carries a `SkeletonInstance` at +0x3FC whose bone array is
// component-space hkQsTransforms. The engine re-evaluates it lazily behind a
// dirty flag; our write lands from the CalcView detour - after the engine's
// tick, before the render build - and was live-proven to be what that frame
// renders. The whole hand cluster (wrist + fingers + weapon-attach bone) is
// moved RIGIDLY: rotate the reference pose about the anchor bone's reference
// point, then translate the anchor point onto the target. No IK, by design -
// the user dropped arm articulation, which is exactly what makes this shape
// of drive sufficient.
//
// This module owns the skeleton MECHANISM only. Pose POLICY (which XR pose,
// trims, offsets, enable state, hand choice) stays in hands.cpp, which calls
// drive() with the finished game-space target - the same GamePose the actor
// pinning used, so `vrhands test`/`simpose` flat lanes exercise this path
// unchanged.

#include "core/hooks/pattern_scan.h"
#include "game/bioshock1r/frame_context.h"

namespace bvr::b1r::bones {

void init(const bvr::pattern_scan::ProcessImage& image);

// Move `hand`'s cluster (0 left, 1 right) so its anchor bone sits at `gp`.
// `handsActor` is the live AHands actor (validated by the caller). Game
// thread, once per frame from hands::on_calcview. Returns false if the
// skeleton could not be reached this frame (caller may fall back).
bool drive(const FrameContext& ctx, void* handsActor, const GamePose& gp, int hand);

// Re-write the values the last drive() produced. The stereo second pass runs
// the ENGINE's CalcView again (which re-evaluates the skeleton over our
// write) but deliberately skips the drive body - without this, the right eye
// bakes the engine pose while the left bakes ours (live-proven: under flat
// stereo the bone array read back the engine idle pose every frame despite
// the drive writing). Called from the CalcView detour's second-pass branch,
// after the original returns. Cheap (cached memcpy), safe when nothing was
// written this frame.
void reapply();

// The old actors died with the old world; drop every cached pointer.
void on_world_change();

// Session 29: hand the skeleton back to the engine, deliberately.
//
// Stopping the drive is NOT enough to restore an authored animation, and that
// asymmetry is the suspected cause of "the controllable rig hands instead of
// the cinematic ones". Three pieces of our state outlive the last drive() call:
// reapply() keeps repainting the cached pose for up to 100 ms AND keeps
// clearing the dirty flag while it does (so it actively suppresses the engine
// re-evaluation that would undo us); restore_hidden() is only ever called from
// inside drive(), so a collapsed inactive hand stays collapsed and the
// weapon-attach bone stays parked far below; and the frozen sway reference
// survives, so the first frame after the cutscene rebuilds from a pre-cutscene
// pose. release() undoes all three and sets the dirty flag so the engine takes
// the skeleton back on its next evaluation. Idempotent; game thread.
void release(const char* why);

// Snapshot for the session-29 cinematic-edge instrument: what our drive left
// behind at the moment a cutscene started or ended. Game thread.
void debug_state(int* hiddenHand, unsigned long long* cacheAgeMs, bool* refValid);

// Session 19: collapse the whole INACTIVE hand's cluster + sleeve while the
// other hand drives (default ON; `vrhands hideinactive on|off`). The
// weapon-attach bone hides by translation, never scale - the attach path
// inverse-decomposes chain scale (session 16). Restores from the reference
// on toggle-off and on hand switch, before the incoming hand is driven.
// s64: hide the WHOLE rig - arms, hands and weapon - for a scripted scene.
//
// DrawScale3D (+0x2B0), NOT the scalar DrawScale at +0x2AC. Session 63 measured
// that scalar as geometry-inert on the rig actor and s64 wasted a build on it;
// the per-axis vector is the one that works, measured in BRVR and re-derived
// here against the same actor. Never writes exact zero - the attach path
// inverse-decomposes chain scale (session 16).
//
// One-shot on each edge, NOT per frame: this is an actor field, so nothing
// re-evaluates over it the way the engine re-evaluates the bone array. The saved
// value is dropped WITHOUT restoring when the actor pointer changes - the old
// address may already belong to something else. Refuses to save an
// already-collapsed scale, which would otherwise make the hands permanently
// invisible. Idempotent; game thread.
void set_actor_hidden(void* handsActor, bool hidden);
bool actor_hidden();

// s64 arm hide, mechanism half: model-space motion of one ENGINE-OWNED wrist
// since the previous call, peak-held. Model space matters - the actor tracks the
// camera every frame, so a world-space measurement reads "moving" constantly.
//
// RETURNS FALSE WHEN IT CANNOT ANSWER (both clusters driven). The caller must
// treat that as "cannot say" and fail toward SHOWING the arms: the failure on
// record is arms hidden for a whole scene, and there is no matching one for arms
// shown for a frame. `outRaw` is the un-smoothed value, logged to calibrate the
// threshold. Game thread. Derivation in ENGINE_NOTES.
// Returns false until TWO samples exist - one reading is not a difference, and
// calling that "still" hid the arms on the first frame of the first scene.
// `outPos` is the sampled bone's own position, so a zero delta can be told apart
// from an array that is no longer being evaluated. That distinction has been
// used once already and the answer was: the array STAYS LIVE behind a hidden
// actor, so DrawScale3D is a safe hide.
// `outStale` is set when the bone still holds the collapse WE wrote, meaning the
// engine has not re-evaluated since and there is no new pose to difference. The
// caller must then leave the motion state untouched - it is not "no motion" and
// it is certainly not the 5000-unit spike that differencing it produces.
bool hand_motion(void* handsActor, float* outSmoothed, float* outRaw, float outPos[3],
                 bool* outStale);

// Ask the engine to re-evaluate the bone array this frame. Call once per frame
// for as long as anything is READING the array while the drive is stood down -
// release() sets the dirty byte once and then early-returns, so without this the
// render pass stops rebuilding and every array-derived signal silently freezes.
// See the banner in the .cpp for the measurement that made this necessary.
void keep_evaluating(void* handsActor);

// The evaluate-if-dirty byte as it reads now, or -1. Diagnostic.
int skeleton_dirty();

// Hide the rig's GEOMETRY while leaving the actor in the render set, so the
// engine keeps animating it and anything read from the bone array stays honest.
// Call every frame for as long as it should stay hidden - it is write-only and
// has no restore path, because the engine's own evaluation puts the authored
// pose back the moment this stops. end_collapse() re-flags the array on the way
// out. See the banner in the .cpp for the measurement behind it.
void collapse_rig(void* handsActor);
void end_collapse(void* handsActor);

// DIAGNOSTIC, read-only: how many of the 47 bones moved since the previous call,
// which one moved most, and by how much. Answers the question a single-bone
// signal cannot - "the array is frozen" and "this animation does not move the
// bone we sample" are indistinguishable from bone 27 alone and need opposite
// fixes. Returns false on the call that establishes the baseline.
bool array_motion(void* handsActor, int* outMoved, float* outMax, int* outBone);

// Which bone the call above is measuring, or -1 when both clusters are ours.
// For the log line - a run of exact zeros is only interpretable if you know
// whether it came from a bone we were writing.
int motion_bone();

void set_hide_inactive(bool on);
bool hide_inactive();

// Session 61: per-cluster viewmodel scale (1.0 = authored; `vrhands scale`).
// The cluster's anchor-relative translations shrink by s (scale about the
// anchor - the anchor write-loc is unchanged, which is the proof metric) and
// the hkQsTransform .s channel is written for the probe-mode-selected bones
// only, NEVER for the weapon-attach (43) or muzzle (44) bones - the s16
// "cluster scale blows the weapon up" test always wrote 43's .s; leaving the
// channel engine-owned entirely is the cell it never tested (BS2's bisection
// localised its identical blowup to the pivot's own scale channel). hand -1 =
// both. Probe modes: `vrbones scalemode <0..3>`.
void set_scale(int hand, float s);
float scale(int hand);

// Session 61: uniform weapon scale (`vrhands wscale <f>`; 1.0 = authored) -
// drives the equipped holdable's OWN SkeletonInstance: translations
// uniformly about the component origin (the grip), quats adopted per frame
// (weapon animations keep playing while scaled), scale channel pinned to the
// captured reference. At 1.0 the lane drops the skeleton entirely and
// restores the captured pose. Runs from hands::on_calcview via
// wskel_drive(); wskel_release() is the explicit hand-back (weapon switch
// and world change are handled internally).
void set_weapon_scale(float ws);
float weapon_scale();
void wskel_drive();
void wskel_release(const char* why);

// Session 20: freeze the drive's reference against the idle animation's
// breathing (default ON; `vrhands swaykill on|off`). Real animations pass an
// anchor-delta threshold and re-freeze when they settle. Measured baseline:
// +-1.2 deg of barrel-direction wobble at idle without it.
void set_sway_kill(bool on);
bool sway_kill();

// Seam commands (game thread): status | list [n] | skel [hands|weapon] |
// poke <idx> <dUU> |
// freeze on|off | collapse on|off | ref | anchor <idx> | lcluster <lo> <hi> <anchor> |
// log on|off (in-headset telemetry: head/controller/camera/actor/target/bone
// samples at ~5 Hz so a headset session can be diagnosed from the log)
void handle_command(const char* args);

// True while `vrbones log on` - camera.cpp and hands.cpp contribute their
// raw-pose lines to the same telemetry stream (each site throttles itself to
// ~5 Hz; the log timestamps correlate the lines of one sample).
bool telemetry_on();

// |render-lock position delta| applied last frame, UU. POSITION-only by
// construction (the lock never touches rotation) - `vraim synccheck` quotes it
// so the position story stays separate from the rotation-divergence gate.
float lock_delta_mag();

// Session 20 muzzle ray: the rendered barrel axis (hands-rig bones 43->44 in
// the current reference pose), expressed in the drive target's local frame -
// world barrel dir = target rotation (x) d0 (see the impl derivation). False
// until a reference pose exists. Game thread.
bool barrel_ref_axis(float d0[3]);

// Overlay section (render thread only).
void draw_debug_ui();

} // namespace bvr::b1r::bones
