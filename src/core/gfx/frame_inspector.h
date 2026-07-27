#pragma once
// One-shot D3D11 frame inspector (DR-3): hooks the immediate context's draw
// and clear vtable slots; when armed, records one full frame of draw events -
// render targets + formats, viewports, constant-buffer sizes/contents, SRV0,
// and return-address callstacks mapped to module+RVA - then writes a text
// dump next to the log and disarms. Costs one atomic load per draw call when
// idle. In-tree replacement for a RenderDoc frame map: the callstack RVAs are
// exactly what the SequentialReentry scene-draw hook needs.

#include <cstdint>

struct IDXGISwapChain;

namespace bvr::frame_inspector {

// Hook the immediate-context vtable slots (fail-soft per slot). `ctxVtable`
// is the vtable pointer harvested from the throwaway device's context.
bool install(void** ctxVtable);

// Arm a one-shot dump of the NEXT full Present-to-Present frame.
// mode 1 = lite (no constant-buffer readback), 2 = full.
// Arm a frame dump: mode 1 = lite, 2 = full (cb bytes). count > 1 records
// that many CONSECUTIVE present windows (files suffixed _qN) - required to see
// both halves of a stereo pair, since a game-thread arm always opens on the
// same pair phase.
void arm(int mode, int count = 1);

// Frame boundary, called at the head of the Present detour: finalizes and
// writes a recording frame, or begins a pending one.
void on_present(IDXGISwapChain* swapchain);

// Suppress capture of our own rendering (ImGui, VR copies) on this thread
// for the current scope. Used by the Present detour after on_present().
struct ScopedSuppress {
    ScopedSuppress();
    ~ScopedSuppress();
};

// Always-on constant-buffer watch (session 13): hooks Map/Unmap and, on every
// WRITE-mapped buffer unmap, matches `pattern` (patCount floats compared at
// float offset patFirst with tolerance) against the staged bytes; on a match
// the floats [capFirst, capFirst+capCount) are copied into a seqlocked slot.
// Cost per unmap on a miss is one float compare. Built for the BioShock
// foreground-scene transform capture (the vm draws' cb0 carries a constant
// tan-block fingerprint), but game-agnostic: the adapter supplies the floats.
// capCount is capped at 32.
// requiredBytes: exact buffer ByteWidth to accept (0 = any) - constant-buffer
// tiers reuse layouts, so the same fingerprint can select different content.
void set_cb_watch(const float* pattern, uint32_t patFirst, uint32_t patCount,
                  uint32_t capFirst, uint32_t capCount, uint32_t requiredBytes);

// Copy the latest watch capture into `out` (capCount floats). Returns false
// if nothing was captured yet. `ageMs` (optional) gets the capture age.
bool latest_cb_watch(float* out, uint32_t count, uint64_t* ageMs);

// Lifetime accepted-capture count (diagnostic: is the watch firing at all?).
uint32_t cb_watch_hits();

// Log the Unmap-time callstack (game-exe RVAs) for the next n fingerprint
// matches - the discovery instrument for whoever BUILDS the watched values.
void cb_watch_log_stacks(int n);

// Overlay section: dump buttons + last-dump status.
void draw_debug_ui();

// Lifetime draw-call census across all four draw slots. The reentry probe
// (DR-5) snapshots this around a re-entered frame-root call: a second-call
// delta comparable to a full frame's draws proves a real second scene render.
uint64_t draw_call_census();

} // namespace bvr::frame_inspector
