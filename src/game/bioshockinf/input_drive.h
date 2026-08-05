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
// `bsiinput on|off|status` + an F10 checkbox. Fail-soft: the arm refuses
// unless the slot's current target resolves into a loaded module.

namespace bvr::bsi::input_drive {

bool handle_command(const char* cmd, const char* args);
void draw_debug_ui();

} // namespace bvr::bsi::input_drive
