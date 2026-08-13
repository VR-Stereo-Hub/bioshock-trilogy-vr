#pragma once
// I7 controls lane (session 42): the synthetic-pad path for BioShock Infinite.
//
// Unlike BS2 (whose engine needed SetUseController + a per-present UpdateInput
// pump + an A->Enter menu shim), Infinite ships native XInput support with its
// own binding chains - the game POLLS XInputGetState itself. The whole lane is
// therefore: re-point the game's XINPUT1_3 ord-2 IAT slot at the core bridge's
// wrapper (Steam's overlay E9-hooks the export thunk, so the proxy post-hook
// alone never sees the game's calls - BS1/BS2's measured mechanism), then let
// core's OpenXR action set publish pad state. The game's own DefaultInput.ini
// bindings do all routing, TBar chains included.
//
// `bsiinput on|off|padmap|status` + F10 controls. Fail-soft: the arm refuses
// unless the slot's current target resolves into a loaded module.

namespace bvr::bsi::input_drive {

// Select Infinite's XR-to-pad table in core (session 44). Called once from the
// adapter's init - core's default is BioShock 1's map, which is wrong here on
// four counts. Touches no engine memory; safe before the game is up.
void arm_pad_profile();

// Is the synthetic pad live? Read by the config lane so a preset can report
// and restore it.
bool enabled();

// Arm/disarm from the config registry (the `inputOn` key). Same path as the
// command verb; game thread.
void set_enabled_from_config(bool on);

bool handle_command(const char* cmd, const char* args);
void draw_debug_ui();

} // namespace bvr::bsi::input_drive
