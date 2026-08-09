#pragma once
// s50: the attach-update seam for BioShock Infinite - built as the FX-origin
// fix candidate, kept as an INSTRUMENT + edge-cover after the flat measurement
// falsified the theory it was built on.
//
// THE THEORY (falsified): attached FX get their transforms from the per-tick
// attachment update reading engine-authored SpaceBases, so repainting the
// composed atoms right before the walk would put the whole FX family on the
// driven hand. THE MEASUREMENT (the dirty-count instrument in the detour):
// SpaceBases already holds OUR composed atoms at virtually every attach
// update - the render-side drive covers this lane - and the frozen FX family
// (vigor charge plume, ready sparkle, muzzle flash, tracer) reads positions
// from a lane that never touches SpaceBases. Full falsification ladder:
// ENGINE_NOTES "s50: THE FX-ORIGIN HUNT".
//
// WHAT SHIPS: the hook stays installed as (a) the dirty-count ordering
// instrument, and (b) edge cover - the engine's eval DOES restamp SpaceBases
// on rare ticks (measured 1-2 per ~10 min), and the pre-walk reapply turns
// those from one-frame authored-pose flashes into non-events. The derived
// seam itself (vtable slot 43, Attachments layout) is banked knowledge for
// the real FX-origin fix.

namespace bvr::bsi::fxorigin {

// Lazy install from the camera detour's 1 Hz throttled tick (fire.cpp lane).
bool wants_install();
bool try_install();

// `bsifx ...`. Returns false when the command is not ours.
bool handle_command(const char* cmd, const char* args);

// Overlay section (render thread: atomics only).
void draw_debug_ui();

} // namespace bvr::bsi::fxorigin
