#pragma once
// PlayerCalcView hook for BioShock 1 Remastered: per-frame camera telemetry,
// debug location/rotation offsets, wobble test, and the live FOV override.

namespace bvr::b1r::camera {

// MinHook hook on eventPlayerCalcView; enables itself. False = flat mode.
bool install(void* eventPlayerCalcView);

bool hook_live();

void set_fov_override(float hfovDeg); // <= 0 disables; game value restored

// Full ImGui section: hook status, telemetry, and all debug controls.
// Called from the overlay through IGameAdapter::drawDebugUi().
void draw_debug_ui();

} // namespace bvr::b1r::camera
