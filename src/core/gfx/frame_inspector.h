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
void arm(int mode);

// Frame boundary, called at the head of the Present detour: finalizes and
// writes a recording frame, or begins a pending one.
void on_present(IDXGISwapChain* swapchain);

// Suppress capture of our own rendering (ImGui, VR copies) on this thread
// for the current scope. Used by the Present detour after on_present().
struct ScopedSuppress {
    ScopedSuppress();
    ~ScopedSuppress();
};

// Overlay section: dump buttons + last-dump status.
void draw_debug_ui();

} // namespace bvr::frame_inspector
