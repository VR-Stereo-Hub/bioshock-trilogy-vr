#pragma once
// DR-5 / SequentialReentry probe: command-gated MinHook detours on the
// game-thread frame SUBMIT (the seam - default target) plus the render-side
// drain/flush instruments - see ENGINE_NOTES "Scene-draw architecture".
// NOTHING is hooked by default: the hooks exist only after an explicit
// "reentry hook" seam command, so default runs stay byte-identical to an
// unhooked game. Escalation ladder (each rung gated on the previous one's
// telemetry): pass-through soak with per-call arg dumps -> one-shot pulse
// double-submit (copied loc/rot args, yaw delta on the rot copy) ->
// continuous double-submit (a yaw-shifted final image proves the engine
// renders a second full frame per game tick - the SequentialReentry
// primitive).

#include "core/hooks/pattern_scan.h"

#include <cstdint>

namespace bvr::b1r::scenedraw {

// Stash the module image (base + size for RVA math). Creates no hooks.
void init(const bvr::pattern_scan::ProcessImage& image);

// Full "reentry ..." command grammar (catalog in camera.cpp's seam comment).
// Game thread only - runs from the command poller inside CalcViewDetour.
void handle_command(const char* args);

// CalcViewDetour head check: true when the current thread is executing the
// SECOND (re-entry) call of any hooked target. The caller must then run ONLY
// the original and add *yawDegOut to rot->yaw - none of its normal body
// (which would eat recenter requests and re-run per-frame state machines
// twice). For the submit seam the yaw already rides on the copied rot arg,
// so a hit here also tells us the engine re-enters CalcView inside the
// submit (watch for double-applied yaw if it ever fires).
bool second_pass_for_current_thread(float* yawDegOut);

// CalcViewDetour telemetry tap: attributes the call as inside/outside the
// hooked call on this thread. Cheap (two relaxed atomics).
void note_calcview();

// True while any reentry hook is created AND enabled.
bool hook_live();

// M4 rung 2: true while SequentialReentry stereo is on (build hook live, not
// poisoned). CalcViewDetour then renders pass 1 as the LEFT eye (cache the
// driven camera, apply -IPD/2) and pass 2 as the RIGHT (replay the cached
// base, +IPD/2) instead of the probe's yaw delta.
bool stereo_active();

// One-toggle "VR stereo" request (session 8): posts the on/off intent; the
// game thread applies 1t + camera mode + stereo outside any hooked call.
// Safe from any thread (the overlay checkbox draws on the render thread).
void request_vrstereo(bool on);

// Session 21 discovery instrument: "vrfgnode on|off|dump" - read-only hook on
// the engine's per-frame FOREGROUND SCENE NODE ctor (patterns.h "FOREGROUND
// SCENE NODE"); dump logs the parent-view/node snapshots taken at ctor tail
// and at frame submit. Game thread only.
void handle_fgnode_command(const char* args);

// Read-only telemetry section for the overlay (control is commands-only).
void draw_debug_ui();

} // namespace bvr::b1r::scenedraw
