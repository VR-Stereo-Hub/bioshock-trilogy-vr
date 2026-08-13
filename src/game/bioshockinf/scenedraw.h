#pragma once
// BioShock Infinite's SequentialReentry seam (I5 rung 3, session 40): a
// detour on the scene-build root - the UGameViewportClient::Draw analog at
// patterns::kSceneDrawRva, derived live by three agreeing routes (caller
// census / backtrace / vtable probe; see patterns.h and ENGINE_NOTES).
//
// With stereo DISARMED the detour is a read-only probe: it counts entries,
// latches the drawing thread, and censuses callers - the observation phase
// that must look right BEFORE any doubling. With stereo ARMED it re-invokes
// the original once per gameplay tick with the ORIGINAL arguments; the
// per-eye cameras enter through the GetPlayerViewPoint redispatch inside the
// doubled call (camera.cpp owns the pass-2 cached-base replay).
//
// DR-I5 says this game's substrate is threaded/ring-buffered (BS2's shape,
// not BS1's kick-and-wait), so there is deliberately NO single-threading
// machinery here. BS2's lesson still applies: "doubling works" is not
// "doubling is stable" - acceptance is a soak, and the pace tracer's
// draw-stage marker brackets the second call so a wedge names itself.
//
// The shape is BS1/BS2's scenedraw (copied and adapted per the decoupling
// directive), minus the 1t kit and the Vengeance stream hook.

#include <cstdint>

namespace bvr::bsi::scenedraw {

// Installs the detour (prologue- and vtable-slot-gated, fail-soft). Called
// lazily from the stereo arm path on the GAME thread - never from the
// overlay. No image parameter: everything verifies against patterns.
bool install();

bool hook_live();

// True while SR stereo is armed (the doubling runs per gameplay tick).
bool stereo_active();

// Arm/disarm the doubling. Arming installs the hook on first use and pushes
// the pass-1/pass-2 eye tags from then on. Returns false if the hook refused.
bool set_stereo(bool on);

// True when the calling thread is currently inside the RE-ENTERED second
// draw - the camera detour forks on this (pass-2 replay instead of the
// drive). Thread-id latch, BS2's shape.
bool second_pass_for_current_thread();

// Side-effect-free observer of the same latch (any thread).
bool in_second_draw();

// Monotonic id of the pass-2 attempt currently (or last) running - bumped
// once per doubled draw, BEFORE the re-entrant call. The camera counts one
// replay BURST per id, which is what makes "replay bursts == second draws"
// an exact acceptance gate even though this seam dispatches the camera
// several times per draw.
uint32_t second_pass_seq();

// `reentry <verb>`: status | reset | pulse [n] | stereo on|off.
bool handle_command(const char* args);

void draw_debug_ui();

} // namespace bvr::bsi::scenedraw
