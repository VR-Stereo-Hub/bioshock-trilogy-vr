#pragma once
// s46: the fire-ORIGIN seam for BioShock Infinite (I8, headset findings 2+3).
//
// THE DEFECT, one root for two findings: the weapon trace STARTS at the camera
// viewpoint (AXPawn::XGetWeaponStartTraceLocation delegates to the exact
// GetPlayerViewPoint impl the camera drive detours - read from the
// disassembly, patterns.h "The fire-ORIGIN seam") while the aim dot's ray
// starts at the HAND. Two parallel rays from different origins never agree on
// a finite wall: the hole lands above the dot, and bullets visibly leave the
// screen center instead of the gun.
//
// THE SHAPE is BioShock 1's origin substitution (call the original, rewrite
// the out-param, ownership-gated), the derivation is this engine's own. The
// direction half is NOT here - it stays at the GetBaseAimRotation seam
// (aim.cpp), which the headset already verified; this lane substitutes the
// POSITION only, with the SAME hand, trim and origin-slider chain the dot
// uses, so hole and dot agree by construction rather than by tuning.
//
// SHIPS AS A PROBE FIRST. `bsifire probe on` (default) installs read-only
// telemetry - engine origin vs the hand origin, displacement, caller rate.
// The write is the separate, explicit `bsifire on`.

namespace bvr::bsi::fire {

// Lazy install from the camera detour's 1 Hz throttled tick (same lane as
// aim::try_install - needs the build gate and a readable impl, nothing live).
bool wants_install();
bool try_install();

// `bsifire ...`. Returns false when the command is not ours.
bool handle_command(const char* cmd, const char* args);

// Overlay section (render thread: atomics only).
void draw_debug_ui();

} // namespace bvr::bsi::fire
