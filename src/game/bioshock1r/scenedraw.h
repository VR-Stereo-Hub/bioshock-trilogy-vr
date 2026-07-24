#pragma once
// DR-5 / SequentialReentry probe: command-gated MinHook detours on the
// renderer's frame root (and optionally the drain loop) - see ENGINE_NOTES
// "Scene-draw architecture". NOTHING is hooked by default: the hooks exist
// only after an explicit "reentry hook" seam command, so default runs stay
// byte-identical to an unhooked game. Escalation ladder (each rung gated on
// the previous one's telemetry): pass-through soak -> CalcView-inside-root
// count -> one-shot/continuous double-call with a yaw delta on the second
// pass (a yaw-shifted final image proves the root rebuilds the command
// queue - the SequentialReentry seam).

#include "core/hooks/pattern_scan.h"

#include <cstdint>

namespace bvr::b1r::scenedraw {

// Stash the module image (base + size for RVA math). Creates no hooks.
void init(const bvr::pattern_scan::ProcessImage& image);

// Full "reentry ..." command grammar (catalog in camera.cpp's seam comment).
// Game thread only - runs from the command poller inside CalcViewDetour.
void handle_command(const char* args);

// CalcViewDetour head check: true when the current thread is executing the
// SECOND (re-entry) frame-root call. The caller must then run ONLY the
// original and add *yawDegOut to rot->yaw - none of its normal body (which
// would eat recenter requests and re-run per-frame state machines twice).
bool second_pass_for_current_thread(float* yawDegOut);

// CalcViewDetour telemetry tap: attributes the call as inside/outside the
// frame-root call on this thread. Cheap (two relaxed atomics).
void note_calcview();

// True while any reentry hook is created AND enabled.
bool hook_live();

// Read-only telemetry section for the overlay (control is commands-only).
void draw_debug_ui();

} // namespace bvr::b1r::scenedraw
