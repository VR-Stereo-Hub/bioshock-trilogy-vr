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

// The last pad published above + its active flag - what the composer will
// merge this frame. The session-20 recorder taps it per CalcView so a replay
// can re-publish the exact pad stream.
void last_xr_pad(Gamepad* pad, bool* active);

// The game layer publishes "the VR camera is driving a REAL gameplay view"
// once per frame (CalcView; strict predicate - menus/cutscenes publish
// false). While true and pitchkill is on, the composed right-stick Y is
// zeroed: under VR the HMD owns pitch, and a stick-pitched body drags the
// viewmodel and the melee phantom with it (session 19). Self-expires if
// publishing stops, failing open to stock behavior.
void publish_vr_gameplay(bool on);

// Session 30. The pitch kill stops the stick fighting the HMD, but it also
// freezes the ENGINE's own view pitch forever - and camera.cpp writes the
// rendered pitch absolutely from the head, so nothing reads the engine's value
// either. Anything the engine aims for itself then uses a stale number; the
// wrench's melee phantom is one, and in-headset it was frozen at -89 degrees,
// putting every swing into the floor.
//
// The game layer publishes (head pitch - engine pitch) in degrees once per
// CalcView and the bridge feeds a proportional stick value instead of a hard
// zero, so the game steers its own pitch through its own input path. No engine
// memory is written. A stale publisher fails open to the old ry = 0 behaviour.
// `vrinput pitchservo on|off|invert|status`.
void publish_pitch_error(float headMinusEngineDeg);

// Master toggle for the stick-pitch kill (default ON; `vrinput pitchkill`).
void set_pitch_kill(bool on);
bool pitch_kill();

// s52 (Infinite I9): per-game policy for the bumper LIFT on the pitch kill.
// BS1/BS2 lift the kill while a grip/bumper is held because their radial
// wheels read stick Y for selection (session 19 part 2). Infinite has no
// radial states - its bumpers are momentary weapon/plasmid cycle taps - so
// the adapter opts out and the kill holds through a bumper press. Default
// true = the historical semantics; BS1 and BS2 never call the setter, so
// their composed pad is byte-identical.
void set_pitch_kill_lift_on_bumpers(bool lift);

// s52 (Infinite I9): head-relative locomotion. The game adapter publishes the
// head-vs-body yaw residual (degrees) once per view dispatch; while fresh and
// nonzero the composer rotates the MOVEMENT stick vector clockwise-from-above
// (+x right, +y forward) by this angle before the game sees it, so stick-
// forward walks along the head's facing rather than the game yaw's. The sign
// contract is the composer's rotation direction - the publisher owns mapping
// its residual sign onto it. Deliberately independent of the turn gate's
// bumper lift (a grip tap must not snap the walk direction mid-stride).
// Self-expires like the other publishes: a stopped publisher (menu, drive
// off, BS1/BS2 which never call it) composes byte-identical sticks.
void publish_move_yaw_offset(float deg);

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

// The full wButtons word the game last saw (composed or real pad). Additive,
// read-only; session 42 consumer is BS2's menukey lane (pad-A -> Enter).
void last_composed_buttons(uint16_t* buttons);

// s50 (Infinite): the FLOURISH CHORD - left thumbrest touched + A pressed.
// arm_flourish_chord(true) makes the XR composer consume A while the left
// thumbrest is touched and count rising A edges; flourish_chord_edges() is
// the monotonic counter the adapter polls on its game-thread tick. Default
// off - no other game's composed pad changes by a single bit.
void arm_flourish_chord(bool on);
uint32_t flourish_chord_edges();

// Session 22: the FINAL composed sticks the game consumed (post merge/
// pitchkill/turn controls) - the movement-wonkiness instrument reads them.
void last_composed_sticks(int16_t* lx, int16_t* ly, int16_t* rx, int16_t* ry);

// Session 22 turn controls (persisted via vrpreset.ini; overlay sliders).
// Smooth scale multiplies composed stick X under the vr-gameplay gate; snap
// mode consumes stick-X edges into discrete steps the camera adapter drains
// with take_snap_steps() and applies to the recenter composite (the body
// transfer carries the body - camera.cpp).
float turn_scale();
void set_turn_scale(float s);
bool snap_turn();
void set_snap_turn(bool on);
float snap_angle_deg();
void set_snap_angle_deg(float d);

// Ammo-slot select modifier (session 23). The shipped modifier is the RIGHT
// STICK CLICK, which testers find awkward to hold while pushing the same
// stick. The Touch thumbrest is the capacitive pad above the buttons; OpenXR
// exposes it as .../input/thumbrest/touch on oculus/touch_controller, so it can
// act as a no-button modifier. It must be the LEFT thumbrest, because your
// right thumb cannot rest on the right thumbrest and push the right stick at
// the same time. Default is Click - unchanged behaviour for existing users.
enum class AmmoMod { Click = 0, Thumbrest = 1, Both = 2 };
AmmoMod ammo_mod();
void set_ammo_mod(AmmoMod m);
int take_snap_steps(); // +right/-left steps queued since the last drain

// Session 44 (Infinite I7): WHICH pad map the XR composer builds.
//
// The composer's face-button re-route, its consumed RS-click and its
// synthesized dpad are BioShock 1 semantics, audited against that game's own
// XENON_* bindings. Infinite's audited retail map disagrees on four counts (it
// wants straight-through faces, RS-click forwarded for XToggleZoom, and a
// fourth dpad direction), and a synthetic pad's only job is to land on the
// bindings the game already ships - so the map has to be per game.
//
// DEFAULT Bioshock1 IN CORE, exactly the set_pose_lag contract: BS1 and BS2
// never call the setter and their composed pad is byte-identical. The Infinite
// adapter arms its profile once in init(). One atomic and one relaxed load per
// XR frame - a single scalar rather than a struct of fields so a live A/B can
// never compose a half-switched pad. The tables themselves live next to the
// composer in core/vr/openxr_input.cpp.
enum class PadProfile { Bioshock1 = 0, Infinite = 1 };
PadProfile pad_profile();
void set_pad_profile(PadProfile p);

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
//   turnscale <0.1..4> | snap on|off | snapangle <deg> | sticklog on|off
//   padlog on|off                     composed BUTTON word, edge-triggered
//   swing ...                         wrench swing-to-attack (core/input/swing.h)
//   test stick l|r <x> <y> [holdMs]   raw -32768..32767
//   test trig  l|r <0..255> [holdMs]
//   test press <A|B|X|Y|LB|RB|START|BACK|LS|RS|DU|DD|DL|DR> [holdMs]
//   test clear
void handle_command(const char* args);

// Overlay section (render thread).
void draw_debug_ui();

} // namespace bvr::input
