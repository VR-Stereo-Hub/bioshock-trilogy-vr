#pragma once
// BS2 aim module. Session 39 stage 1: the DISPATCH PROBE that settles whether
// the fire chain (BeginFiring / UseAbility / InitiateDamage /
// GetPerfectFireStart) is ProcessEvent-visible (-> by-name seam, BS2-native)
// or native-to-native all the way (-> BS1-style impl hooks with fresh RVAs).
// docs/bioshock2/ENGINE_NOTES.md "Fire flow / aim" has the derisk; the seam
// hook itself lands only after the probe's verdict.
//
// Two instruments, both armed by `vraim probe on` and read by
// `vraim probe dump`:
//  - fire-watch: per-name UFunction pointers learned through the existing
//    FindFunctionChecked detour (keyed on the Lane-A FName index globals,
//    patterns::resolve_fire_names), hit-counted in the ProcessEvent detour.
//    A positive is decisive; a negative is not (a name can reach
//    ProcessEvent as a script-linked UFunction* without any native global).
//  - census: EVERY ProcessEvent dispatch's function name index, deduped into
//    a fixed table and printed with GNames text - the ground truth that
//    catches names the fire-watch cannot see. Needs the UFunction name-field
//    offset, which is SELF-DERIVED at runtime: the learned PlayerCalcView
//    UFunction* is scanned for a dword equal to *fnameIndexGlobal (with a
//    zero number dword behind it) - zero layout constants assumed.

#include "core/hooks/pattern_scan.h"
#include "game/bioshock2r/patterns.h"

#include <atomic>
#include <cstdint>

namespace bvr::b2r::aim {

// Probe master switch. The camera detours read this INLINE (one relaxed load
// per event when disarmed) - ProcessEvent traffic is the whole script engine,
// and the fast path must stay tiny.
extern std::atomic<bool> g_probeArmed;

// Resolve the fire-chain name globals (fail-soft per name) and stash what the
// probe needs. Call after camera::install succeeds.
void init(const bvr::pattern_scan::ProcessImage& image, const patterns::Symbols& symbols);

// Called from camera.cpp's detours ONLY while g_probeArmed (game thread).
void probe_findfunc(uint32_t nameIndex, uint32_t nameNumber, void* fn);
void probe_process_event(void* fn);

// 1 Hz probe summary while armed; rides the camera poll lane's maintenance
// tick (game thread, outside hooked calls).
void poll_tick(uint64_t now);

// `vraim <args>` from the command seam. Returns true when consumed.
bool handle_command(const char* args);

} // namespace bvr::b2r::aim
