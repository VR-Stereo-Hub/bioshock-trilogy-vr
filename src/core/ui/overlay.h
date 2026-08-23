#pragma once

struct IDXGISwapChain;

namespace bvr::overlay {

// ImGui debug/config overlay drawn from the Present hook. Toggled with F10.
// Initializes itself lazily on the first Present (that's when the game's
// device and window are known).
void on_present(IDXGISwapChain* swapchain);
void on_resize(); // drop backbuffer references before ResizeBuffers

// Session 22: programmatic visibility (any thread; applied on the next
// present). The `vroverlay on|off` seam command - the harness cannot press
// F10, and it doubles as the user's recovery if a keyboard state wedges.
void set_visible(bool on);

// Is the panel up? Two callers, both needing an answer from another thread:
// the R3+L3 chord toggles by inverting it, and the XR pad composer suppresses
// the right trigger while it is true so a click cannot also fire the weapon.
// Reads the applied state, not the pending request, so a toggle issued twice
// inside one present does not cancel itself out.
bool visible();

} // namespace bvr::overlay
