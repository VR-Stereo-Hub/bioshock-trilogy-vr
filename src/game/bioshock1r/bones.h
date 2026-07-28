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

// Session 19: collapse the whole INACTIVE hand's cluster + sleeve while the
// other hand drives (default ON; `vrhands hideinactive on|off`). The
// weapon-attach bone hides by translation, never scale - the attach path
// inverse-decomposes chain scale (session 16). Restores from the reference
// on toggle-off and on hand switch, before the incoming hand is driven.
void set_hide_inactive(bool on);
bool hide_inactive();

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
