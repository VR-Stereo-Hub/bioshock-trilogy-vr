#pragma once

#include <cstdint>

namespace bvr::d3d11_hook {

// Discovers the game's IDXGISwapChain vtable via a throwaway device/swapchain
// (kiero technique) and hooks Present + ResizeBuffers with MinHook. The game's
// own swapchain shares the vtable, so the hooks fire on its frames.
//
// If the game is running its D3D9 renderer instead, Present simply never
// fires — the log will show "installed" but no "first Present" line.
bool install();

// Lifetime Present count (telemetry: presents-per-frame-root ratios etc.).
uint64_t present_count();

// Thread id observed on the most recent Present (telemetry: single- vs
// multi-threaded render attribution - session 26, BS2 substrate work).
uint32_t last_present_tid();

} // namespace bvr::d3d11_hook
