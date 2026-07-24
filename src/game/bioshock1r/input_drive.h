// Gamepad drive: makes the engine consume the synthetic XInput pad.
//
// The remaster's pad pipeline lives entirely in UWindowsViewport::UpdateInput,
// which nothing calls in windowed mode (ENGINE_NOTES "Gamepad architecture") -
// the game decides "no pad" once at boot and never re-polls. While the
// core bridge (core/input/xinput_bridge) is enabled, this module calls
// UpdateInput once per present on the game thread and flips the engine's own
// UseController state through UWindowsClient::SetUseController, so the whole
// stock pad path - detection global, input events, UI prompts - runs against
// the bridge's composed state.

#pragma once

#include <cstdint>

namespace bvr::b1r::input_drive {

// Game thread, called from the CalcView detour every call; self-throttles to
// once per present and no-ops while the bridge is disabled. Handles the
// enable/disable edges (SetUseController TRUE/FALSE) itself.
void on_frame(uint64_t nowMs);

// One status line for the adapter overlay section.
void draw_debug_ui();

} // namespace bvr::b1r::input_drive
