// Synthetic gamepad bridge. The xinput1_3.dll proxy shim (which the game
// imports by ordinal) exposes a post-XInputGetState seam; this module
// registers with it and composes synthetic controller state - from the OpenXR
// action layer and/or seam-command test injection - over whatever a real pad
// (or no pad) reported. Real-pad passthrough is untouched while disabled.

#pragma once

#include <cstdint>

namespace bvr::input {

// Mirrors XINPUT_GAMEPAD (12 bytes) without leaking Xinput.h to callers.
struct Gamepad {
    uint16_t buttons = 0;
    uint8_t lt = 0, rt = 0;
    int16_t lx = 0, ly = 0, rx = 0, ry = 0;
};

// Resolve the proxy module and register the post-GetState hook. Fail-soft:
// logs and disables synthetic input if the seam is missing (e.g. the mod was
// loaded some other way). Call once from framework init.
void init();

// Master switch: synthetic state contributes only while enabled. Real-pad
// passthrough always works regardless.
void set_enabled(bool on);
bool enabled();

// XR action layer publishes its composed pad here once per frame (render
// thread). `active` false means "no data this frame" (no session / not
// focused); the slot also self-expires if publishing stops entirely.
void publish_xr_state(const Gamepad& pad, bool active);

// The game layer publishes "the VR camera is driving a REAL gameplay view"
// once per frame (CalcView; strict predicate - menus/cutscenes publish
// false). While true and pitchkill is on, the composed right-stick Y is
// zeroed: under VR the HMD owns pitch, and a stick-pitched body drags the
// viewmodel and the melee phantom with it (session 19). Self-expires if
// publishing stops, failing open to stock behavior.
void publish_vr_gameplay(bool on);

// Master toggle for the stick-pitch kill (default ON; `vrinput pitchkill`).
void set_pitch_kill(bool on);
bool pitch_kill();

// Radial stick deadzone (fraction 0..0.5) applied by the XR composer.
float stick_deadzone();

// Last composed trigger pair (0..255). The M6 aim path uses it to tell which
// hand a fire event belongs to: we compose this state ourselves, so "which
// trigger is the player pulling" is information the mod already owns.
void last_composed_triggers(uint8_t* lt, uint8_t* rt);

// Last composed bumper pair. The grips compose to the bumpers, and in this
// game a bumper press SWITCHES the raised hand (LB -> plasmid = left hand,
// RB -> weapon = right hand) without any trigger event - the M8 grip-switch
// fix latches hand attribution from these too.
void last_composed_bumpers(bool* lb, bool* rb);

// Install the bridge's composing XInputGetState wrapper into an import slot
// (e.g. the game module's IAT entry for xinput1_3 ordinal 2). The slot's
// previous target becomes the passthrough, so a hook chain already wrapping
// the import (the Steam overlay code-hooks the proxy's export thunk and
// swallows calls before our proxy body runs) keeps working underneath us
// while the composed state reaches the game on the last hop. Idempotent.
bool hijack_import_slot(void** slot);

// Seam command handler: args after the "vrinput" verb (game thread).
//   on | off | status
//   pitchkill on|off|status
//   test stick l|r <x> <y> [holdMs]   raw -32768..32767
//   test trig  l|r <0..255> [holdMs]
//   test press <A|B|X|Y|LB|RB|START|BACK|LS|RS|DU|DD|DL|DR> [holdMs]
//   test clear
void handle_command(const char* args);

// Overlay section (render thread).
void draw_debug_ui();

} // namespace bvr::input
