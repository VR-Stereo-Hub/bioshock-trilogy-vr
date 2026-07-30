// Swing-to-attack: a physical wrench swing fires the melee attack.
//
// Session 30 fixed the wrench (the engine's own view pitch was frozen, so every
// swing went into the floor). What was left is HOW you swing: you still pull the
// right trigger. The user play-tested timing that trigger pull to a real arm
// swing and asked for the gesture, so this module watches the right controller
// and pulses RT when it crosses a speed threshold.
//
// What this is NOT: aiming. Melee damage is a Havok phantom
// (`Wrench.CreateCollisionPhantom`) aimed by the ENGINE's own view - measured in
// session 30, melee reaches neither fire-start seam, so no hook of ours can move
// it. A sideways swing while you look forward still hits forward. The gesture
// changes WHEN the attack happens, never WHERE it lands, exactly like the
// trigger it stands in for.
//
// Why this lives in core/input/ rather than in the OpenXR layer with the poses:
// core/vr/openxr_input.cpp compiles only under BVR_WITH_OPENXR and input_sync
// never runs flat, so a detector living there could not be exercised without a
// headset. Here it sits next to the bridge that owns the composed pad, runs on
// any composed frame, and `swing sim` can drive the whole decision path flat.
//
// Threading: everything is atomic and lock-free. rt_pulse() runs on the game's
// XInput poll (hundreds of calls per second, already inside the bridge's mutex)
// and must never take a lock of its own. The decision core has two possible
// callers - the render thread for live samples, a game thread for `sim` - but
// they are never live at once in practice (a sim is a flat test, live samples
// need an XR session), so relaxed atomics are enough and a torn decision could
// at worst cost one swing.

#pragma once

#include <cstdint>

namespace bvr::input::swing {

// Game thread, once per CalcView: "a swing right now would be legitimate" -
// strict gameplay view AND the wrench is the equipped holdable. Self-expiring
// like the other published gates, so a stopped publisher (world unload, drive
// off) disarms the gesture instead of latching it open.
void publish_gate(bool armed);

// Render thread, once per XR frame: the right GRIP pose position and the head
// position, both in meters in the app space, plus the frame's predicted display
// time in nanoseconds (pass <= 0 to fall back to the wall clock). Positions are
// read through the same funnel every other consumer uses, so the session-20
// recorder's sim overlay drives this too. `handValid` false re-seeds rather than
// producing a bogus delta across the gap.
void publish_sample(const float handPos3[3], const float headPos3[3], bool handValid,
                    bool headValid, int64_t displayTimeNs);

// True while the synthetic trigger pulse is open. Read by the bridge's
// composer; it merges as a trigger MAX, so a real trigger pull is unaffected.
bool rt_pulse(uint64_t nowMs);

// Drives an armed `swing sim` window. Called from the bridge's composed path;
// cheap (one relaxed load) while no sim is armed.
void sim_tick(uint64_t nowMs);

// Args after the "vrinput swing" verb (game thread):
//   on | off | status
//   threshold <m/s> | rearm <m/s> | cooldown <ms> | pulse <ms> | delay <ms>
//   rel on|off              head-relative velocity (default on): subtract the
//                           head's motion so physically turning your body with
//                           the wrench out is not read as a swing
//   log on|off              every fire, and every swing that was BLOCKED with
//                           the reason - silent below the threshold, and one
//                           line per SWING rather than per sample
//   sim <peak m/s> [humpMs] [reps]
//                           synthesize swings through the real decision path;
//                           the flat test seam. Each hump is followed by an
//                           equal gap at zero speed, so reps > 1 is what makes
//                           the cooldown observable flat
void handle_command(const char* rest);

// Overlay rows, drawn inside the bridge's Input section (render thread).
void draw_debug_ui();

// Persisted through vrpreset.ini (camera.cpp's save/load), so the values tuned
// in the headset survive a restart.
bool enabled();
void set_enabled(bool on);
float threshold_ms();
void set_threshold_ms(float v);
float rearm_ms();
void set_rearm_ms(float v);
uint32_t cooldown_ms();
void set_cooldown_ms(uint32_t v);
uint32_t pulse_ms();
void set_pulse_ms(uint32_t v);
uint32_t delay_ms();
void set_delay_ms(uint32_t v);
bool head_relative();
void set_head_relative(bool on);

} // namespace bvr::input::swing
