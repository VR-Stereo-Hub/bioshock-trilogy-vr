#pragma once
// PlayerCalcView seam for BioShock 2 Remastered: per-frame camera telemetry,
// the command-file seam, the 6DOF HMD head drive (recenter, additive yaw,
// world scale, head-anchor offsets), and since session 25 the FOV readback
// (projection claim == rendered) plus the gated FOV write levers (default
// OFF). No stereo, no aim/hands - those land with their own milestones.
//
// Unlike BS1 (event-thunk hook), the seam here is a ProcessEvent hook
// filtered to the PlayerCalcView UFunction, learned via a FindFunctionChecked
// hook - BS2's build inlined the event dispatch at every call site, so the
// thunk is dead code (patterns.h has the derivation). The drive math mirrors
// game/bioshock1r/camera.cpp; every duplicated piece is recorded as a
// core/adapter seam leak in the ARCHITECTURE decision log.

#include "core/hooks/pattern_scan.h"
#include "game/bioshock2r/patterns.h"

namespace bvr::b2r::camera {

// Stash the image bounds for vtable-RVA identity checks (the gameplay-view
// predicate). Call before install().
void init_image(const bvr::pattern_scan::ProcessImage& image);

// MinHook hooks on ProcessEvent + FindFunctionChecked; enable themselves.
// False = flat mode.
bool install(const patterns::Symbols& symbols);

bool hook_live();

// IGameAdapter::setFov funnel: > 0 arms the manual game-FOV write at that
// value (strict gameplay only, save/restore-gated), <= 0 disarms it.
void set_fov_override(float hfovDeg);

// Full ImGui section: hook status, telemetry, and the M3 camera controls.
// Called from the overlay through IGameAdapter::drawDebugUi().
void draw_debug_ui();

} // namespace bvr::b2r::camera
