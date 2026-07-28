#pragma once
// PlayerCalcView hook for BioShock 1 Remastered: per-frame camera telemetry,
// debug location/rotation offsets, wobble test, and the live FOV override.

#include "core/vr/openxr_runtime.h"

#include <cstdint>

namespace bvr::b1r::camera {

// MinHook hook on eventPlayerCalcView; enables itself. False = flat mode.
bool install(void* eventPlayerCalcView);

bool hook_live();

// True while the foreground lens match is armed AND writing (session 15):
// the rig renders through the WORLD lens, so the render-lock solve must use
// the world tan scales and k = 1.
bool fg_fov_match_active();

void set_fov_override(float hfovDeg); // <= 0 disables; game value restored

// Session 20 vrrec: the recenter reference + world scale are the xr->game
// mapping's hidden state. The recorder snapshots them into a recording's
// header and restores them on play; set_ also suppresses the pending
// auto-recenter so the first replayed frame cannot re-reference itself.
void get_recenter_state(bvr::vr::HeadPose* pose, int32_t* yawUnits, float* worldScale);
void set_recenter_state(const bvr::vr::HeadPose& pose, int32_t yawUnits, float worldScale);

// Full ImGui section: hook status, telemetry, and all debug controls.
// Called from the overlay through IGameAdapter::drawDebugUi().
void draw_debug_ui();

} // namespace bvr::b1r::camera
