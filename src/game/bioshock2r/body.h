#pragma once
// M7.5 body-follows-head yaw transfer, BS2 port (session 42 round 2, user
// report: stick-forward walks the OLD facing after a head turn, and snap turn
// rotates the camera but not the pawn).
//
// Duplicated from bioshock1r/body (the decoupling directive) with one BS2
// difference: the actor rotation-field offset is NOT a baked constant (BS1's
// kActorViewDirOffset must never be copied) - it is DERIVED LIVE by matching
// the engine's own pre-drive view rotation (pitch, yaw) against int32 pairs
// in the PlayerController across several frames until exactly one offset
// survives. The probe handshake then verifies a write to that offset actually
// steers the body before any transfer runs; three failed probes undo
// themselves and latch the module off. Fail-soft at every stage.
//
// THE INVARIANT (BS1's, verbatim - it is a theorem in integer rotator units):
//     camera yaw = gameYaw + (headYaw - recenterYaw)
// so transferring T into the body (gameYaw += T) while advancing the recenter
// reference by exactly the same T leaves the final camera AND the whole
// controller-to-world mapping bit-identical - only the body/head-look SPLIT
// of the yaw relabels. on_calcview RETURNS the units actually committed and
// camera.cpp adds exactly that integer to its recenter reference.

#include "core/hooks/pattern_scan.h"

#include <cstdint>

namespace bvr::b2r::body {

void init(const bvr::pattern_scan::ProcessImage& image);

// Called ONCE per rendered frame from the very end of the CalcView tail
// (after the FrameContext publish). Returns the yaw units COMMITTED to the
// body; the caller must add exactly this to its recenter reference.
//   pc            the PlayerController the hook fired on
//   viewActor     the view actor (strict-gameplay guard + pawn field)
//   gameYawUnits  the engine's own body yaw this frame (rot->yaw PRE-drive)
//   pitchUnits    the engine's own view pitch PRE-drive (derivation input)
//   residualUnits head-look yaw not yet absorbed, wrapped
//   vrDriving     an HMD (or simhead) pose drove the camera this frame
int32_t on_calcview(void* pc, void* viewActor, int32_t gameYawUnits,
                    int32_t pitchUnits, int32_t residualUnits, bool vrDriving);

// Reset the transfer state machine (PC change, world change, recenter).
void on_reset(const char* why);

// `vrbody on|off|status|rate <perSec>|deadzone <deg>|max <degPerSec>|
//  field pc|pawn|both|probe on|off|poke <deg>` (BS1 grammar).
void handle_command(const char* args);

// Overlay section (render thread only).
void draw_debug_ui();

bool enabled();

// Tuned values the VR preset persists.
float rate_per_sec();
float deadzone_deg();
void set_tuning(float ratePerSec, float deadzoneDeg);

} // namespace bvr::b2r::body
