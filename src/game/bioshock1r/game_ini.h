#pragma once
// Read and write the GAME's own Bioshock.ini, specifically the render
// resolution. Per-game on purpose: the file name, its location and its section
// layout are all BioShock-1 facts.
//
// WHY THIS EXISTS RATHER THAN A LIVE RESOLUTION CHANGE. The engine's own
// `SETRES` console command, reachable through the viewport Exec seam this mod
// already uses successfully for `set ...`, FAULTS: a near-null dereference at
// exe+0x4C2353 with no ResizeBuffers following (measured, session 27 - see
// ENGINE_NOTES). So the only working control over the render resolution is the
// config the engine reads at startup, which means a change takes effect on the
// NEXT launch. The reference mod (BioVRDev) reached the same conclusion and
// ships a setup script for it.
//
// WHY IT MATTERS. The eye render is the game's backbuffer, so the game's
// resolution IS the VR resolution - and a headset panel is near square while
// 16:9 throws away roughly half the width at the FOV this mod asks for. A user
// on a Dream Air independently found this: their runtime's own resolution slider
// did nothing (it cannot - nothing reads it), and editing this file to 7680x4320
// made the image "super crisp" at about 53% pixel efficiency.
//
// SAFETY. Four separate sections of this file carry the same viewport key names
// - the PC driver plus the Xbox 360, Xbox One and PS4 ones. A naive key
// replacement writes a console driver section and appears to succeed, so every
// write here is scoped to `[WinDrv.WindowsClient]`. The file is ANSI with CRLF
// endings and the mod edits it in place, preserving every other byte including
// the trailing semicolons the shipped file has on some values.

#include <cstdint>

namespace bvr::b1r::game_ini {

// Full path to the located Bioshock.ini, or an empty string. Cached after the
// first call; the search is logged once.
const wchar_t* path();

struct Viewport {
    uint32_t windowedW = 0, windowedH = 0;
    uint32_t fullscreenW = 0, fullscreenH = 0;
    bool startupFullscreen = false;
    bool valid = false;
};

// Current [WinDrv.WindowsClient] geometry.
Viewport read_viewport();

// Write BOTH the windowed and fullscreen pairs to w x h. Both, because UE2 keeps
// two and reads whichever mode it starts in - writing only one is how "setting
// the resolution" silently stops working the moment the user toggles fullscreen.
// Backs the file up once to Bioshock.ini.bvr-bak-res (house convention), then
// re-reads and verifies. Returns false and logs on any failure; never leaves a
// partially written file. Game thread, on explicit request only.
bool write_viewport(uint32_t w, uint32_t h);

// One log line: located path, current geometry, and whether it agrees with the
// live backbuffer. A disagreement means the ini was not honoured - most likely
// the game rewrote it at exit from its own in-memory values.
void log_status(uint32_t liveBackbufferW, uint32_t liveBackbufferH);

} // namespace bvr::b1r::game_ini
