#pragma once
// BS2 render-substrate discovery + (as the RVAs land) SequentialReentry.
// Session 26 ladder, mirroring bioshock1r/scenedraw.h's SHAPE: instruments
// first (this commit), pass-through hooks after the live derivation, then
// structural 1t and the double build. NOTHING is hooked by default - every
// instrument exists only after an explicit "reentry ..." seam command, so
// default runs stay byte-identical to an unhooked game.
//
// Discovery instruments (no engine RVAs consumed except the statically
// verified event Trigger method - patterns.h "render-substrate discovery"):
//   reentry kick on|off    process-wide kernel32!SetEvent caller sampler.
//                          BS2 twist vs BS1: the engine reaches SetEvent only
//                          through tiny FF15 wrapper methods, so the direct
//                          return RVA names the wrapper interior - each slot
//                          therefore also DEEP-captures up to 3 further
//                          call-preceded exe return RVAs from the sampling
//                          thread's stack (first insertion only).
//   reentry kick2 on|off   caller sampler hooked on the event-object Trigger
//                          method itself (kEventTriggerRva): its return
//                          address IS the engine-side virtual call site.
//   reentry calcstack      one-shot game-thread stack scan at the next
//                          CalcView dispatch.
//   reentry status         counters + hook states.

#include "core/hooks/pattern_scan.h"

namespace bvr::b2r::scenedraw {

// Stash the module image (base + size for RVA math). Creates no hooks.
void init(const bvr::pattern_scan::ProcessImage& image);

// The "reentry ..." grammar above. Game thread only (runs from the command
// poller in the ProcessEvent detour).
void handle_command(const char* args);

// calcview_tail telemetry tap: attributes the dispatch as inside/outside a
// hooked render call on this thread and services one-shot instrument
// requests. Cheap (two relaxed atomics) - safe at BS2's dispatch rates.
void note_calcview();

// True while the current thread is executing a depth-0 hooked render call
// (build/submit, once those hooks land). The command poller and the FOV
// stale-restore defer while this holds: total ProcessEvent traffic is high,
// so a deferred tick lands again within milliseconds - but a command that
// installs/disables hooks must never run mid-build.
bool inside_hooked_call();

// True while any reentry hook is created AND enabled.
bool hook_live();

// M4 rung 2 gate: true while SequentialReentry stereo is on. The camera
// module suppresses the AlternateEye sign while this holds (SR applies both
// eye offsets itself). Always false until the substrate commits land.
bool stereo_active();

// Read-only telemetry section for the overlay (control is commands-only).
void draw_debug_ui();

} // namespace bvr::b2r::scenedraw
