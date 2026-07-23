#pragma once

struct IDXGISwapChain;

namespace bvr::overlay {

// ImGui debug/config overlay drawn from the Present hook. Toggled with Insert.
// Initializes itself lazily on the first Present (that's when the game's
// device and window are known).
void on_present(IDXGISwapChain* swapchain);
void on_resize(); // drop backbuffer references before ResizeBuffers

} // namespace bvr::overlay
