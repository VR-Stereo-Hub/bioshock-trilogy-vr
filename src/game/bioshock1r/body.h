#pragma once
// M7.5 body-follows-head yaw transfer: rotate the invisible body/pawn facing
// under an UNCHANGED camera, so "forward" is where the player looks.
//
// THE DEFECT (user, session 16 part 4): the body facing only rotates with the
// right stick. With the head physically turned, left-stick "forward" walks
// along the OLD facing. One root, three symptoms - the movement mapping, the
// weapon-laser desync that grows with hand-vs-facing angle, and the rig being
// CULLED past ~90 deg (its composition frame is the body facing).
//
// THE INVARIANT THIS MODULE EXISTS TO PRESERVE - read before touching it.
// The camera and BOTH halves of the XR-controller-to-world mapping
// (frame_context.h) are functions of the same composite:
//
//     camera yaw = gameYaw + (headYaw - recenterYaw)
//     hand rot   = (gameYaw - recenterYaw) + controller yaw
//     hand pos   = base + R(gameYaw - recenterYaw) * xr_offset * worldScale
//
// So transferring T into the body (gameYaw += T) while advancing the recenter
// reference by exactly the same T leaves the final camera AND the composite
// mapping bit-identical - only the body/head-look SPLIT of the yaw relabels.
// In integer rotator units that is a theorem, not a tolerance:
//
//     rot->yaw' = (gameYaw + T) + (residual - T) = gameYaw + residual
//
// The hand following the head again is the defect sessions 12-16 killed; the
// user's explicit non-regression requirement is that it never comes back. That
// is why on_calcview RETURNS the amount actually committed and camera.cpp adds
// exactly that to the recenter reference - the two can never drift apart
// because they are the same integer.
//
// What DOES legitimately change: the AHands actor's own rotation (the
// renderer's composition frame), because the engine places the rig from the
// view rotation. That is the point - it is what un-culls the rig past 90 deg
// and shrinks the camera-vs-actor split the bones render lock corrects for.

#include "core/hooks/pattern_scan.h"

#include <cstdint>

namespace bvr::b1r::body {

void init(const bvr::pattern_scan::ProcessImage& image);

// Called ONCE per rendered frame from the very end of the CalcView detour -
// after the FrameContext publish, so this frame's aim/hands consumers see the
// (driveYawOffset, recenterYaw) pair that describes the body the engine
// actually has right now. The write is state for the NEXT frame.
//
//   pc             the PlayerController the hook fired on
//   viewActor      *view_actor out-param (gameplay/cutscene guard)
//   gameYawUnits   the engine's own body yaw this frame (rot->yaw pre-drive)
//   residualUnits  head-look yaw not yet absorbed, wrapped to (-32768, 32767]
//   vrDriving      an HMD (or simhead) pose drove the camera this frame
//
// Returns the yaw units COMMITTED to the body. The caller must add exactly
// this to its recenter reference - no more, no less.
int32_t on_calcview(void* pc, void* viewActor, int32_t gameYawUnits,
                    int32_t residualUnits, bool vrDriving);

// Reset the transfer state machine (PC pointer change, world change, recenter).
void on_reset(const char* why);

// Seam commands: on | off | status | rate <perSec> | deadzone <deg> |
// max <degPerSec> | field pc|pawn|both | probe on|off | poke <deg>
void handle_command(const char* args);

// Overlay section (render thread only).
void draw_debug_ui();

bool enabled();

// STRICT gameplay-view predicate: the view actor is the player's ShockPlayer
// (vtable match), with NO `viewActor == pc` escape hatch - so the main-menu
// attract scene and cutscenes read false. This is the body-write guard, and
// since session 19 also the gate for the stick-pitch kill and the source of
// the "[b1r] view state" harness log signal.
bool is_gameplay_view(void* viewActor);

// Tuned values the VR preset persists to vrpreset.ini.
float rate_per_sec();
float deadzone_deg();
void set_tuning(float ratePerSec, float deadzoneDeg);

} // namespace bvr::b1r::body
