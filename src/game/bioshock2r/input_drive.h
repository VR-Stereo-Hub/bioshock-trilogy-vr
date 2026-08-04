// Gamepad drive (BS2): makes the engine consume the synthetic XInput pad.
//
// BS2's pad pipeline lives entirely in UWindowsViewport::UpdateInput, which
// nothing calls per frame (ENGINE_NOTES "Session 40" - vtable-only dispatch,
// 2 boot GetState calls total): the game decides "no pad" once at boot and
// never re-polls. While the core bridge (core/input/xinput_bridge) is
// enabled, this module calls UpdateInput once per present on the game thread
// and flips the engine's own UseController state through
// UWindowsClient::SetUseController. UpdateInput's own reconnect branch then
// re-arms the pad-connected global against the bridge's composed state - the
// whole stock pad path (detection global, XENON_* input events, UI prompts)
// runs, and this module never writes engine globals directly.
//
// Duplicated from bioshock1r/input_drive (the decoupling directive): same
// shape, BS2 constants throughout, TWO vtable slot constants (client
// SetUseController vs viewport UpdateInput - BS1's shared 0x118 was a layout
// accident), and an engine-object identity check BS1's resolver never had.

#pragma once

#include <cstdint>

namespace bvr::b2r::input_drive {

// Game thread, called from the ProcessEvent lane every dispatch;
// self-throttles to once per present and no-ops while the bridge is
// disabled. Handles the enable/disable edges (SetUseController TRUE/FALSE)
// itself.
void on_frame(uint64_t nowMs);

// One status line for the adapter overlay section.
void draw_debug_ui();

// Session 42: `menukey on|off|force on|force off|status`. Pad A -> scancode
// Enter while a menu context holds (BS2's gameswf front-end activates on
// keyboard only). Inert while the drive is off (no fresh pad word).
void handle_menukey_command(const char* args);

// Resolve the live UWindowsClient / first UWindowsViewport (vtable-identity
// checked, SEH-safe reads; heap objects - never cache). False while the
// engine is not up. Game thread only.
bool resolve_engine_objects(void** client, void** viewport);

} // namespace bvr::b2r::input_drive
