#pragma once
// The D3D11 debug layer on the GAME's device, as a diagnostic the tester arms for the next
// launch (F10 > Diagnostics, or `d3ddebug on|break|off`). Off, this module does nothing.
//
// Why: a crash inside the NVIDIA driver on one of its worker threads (five headset runs on
// 2026-10-10 under the alternate frame warp, always nvwgf2um.dll+0x48153A, a read at 0x9370)
// names no caller - the dump holds driver frames only. The SDK layer validates every D3D11
// call on the game's device as it is made and names the invalid one (a view of a released
// resource, a resource of another device, a bad bind) with the reason, before the driver
// ever sees it. Armed as "break", an error also raises a debug break on the CALLING thread,
// which our crash filter turns into a dump with the mod's own frame on it.
//
// Mechanism: when the marker file <data_dir>\d3d11-debug-layer.txt exists at startup,
// D3D11CreateDevice and D3D11CreateDeviceAndSwapChain are hooked (ahead of the game's own
// device creation - the mod loads with xinput1_3 at process start) and the debug flag is
// added; a creation the layer refuses (SDK layers missing) is retried without it. Each
// present drains the device's info queue into the log, deduplicated by message id and
// capped, so the layer's cost in log volume is bounded. The layer itself costs frame time;
// it is for one diagnostic run, not for play.

struct IDXGISwapChain;

namespace bvr::d3d11_debug {

// Framework init, before the game creates its device. Reads the marker; hooks when armed.
void install();
// Present detour: drain the info queue (no-op unless the layer is live on this device).
void on_present(IDXGISwapChain* swapchain);
// The marker: 0 = off, 1 = log the layer's messages, 2 = log and break on errors.
int armed_for_next_launch();
bool arm(int mode);
// Whether the layer is on this run's game device.
bool live();
// One line for the F10 panel.
const char* status();

} // namespace bvr::d3d11_debug
