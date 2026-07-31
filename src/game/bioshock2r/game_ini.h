#pragma once
// Read and write the GAME's own config, specifically the render resolution.
// Per-game on purpose, and NOT a shared module: the file names, their directory
// and the sections that actually govern the viewport are all BioShock-2 facts,
// derived fresh from the live install (session 32) rather than inherited from
// BS1. Only the SHAPE of bioshock1r/game_ini is reused.
//
// THE FINDING THAT MADE THIS NOT A PORT (session 32, measured over three
// relaunches). BS1's whole lane writes `[WinDrv.WindowsClient]`'s
// Windowed/FullscreenViewportX/Y. **BS2 IGNORES THOSE KEYS.** The file that
// governs is `Shared.ini`, section `[SharedOptions]`, keys `ViewportX`/
// `ViewportY` - a file BS1 does not have at all:
//
//   Bioshock2SP.ini WinDrv | Shared.ini SharedOptions | rendered
//   -----------------------|--------------------------|------------
//   2048x2048              | 1920x1080                | 1920x1080
//   2048x2048              | 2048x2048                | 2048x2048
//   1920x1080              | 2048x2048                | 2048x2048   <- decisive
//
// Shared.ini wins in every case, including when it is the ONLY file asking.
// This is exactly what the never-copy rule protects against: the BS1-shaped
// port wrote its four keys, re-read them, logged "verified", and the engine
// rendered 1920x1080 anyway. **A verified write is not an honoured one** - the
// only acceptance that means anything here is the backbuffer at first Present.
//
// Both files are still written. Shared.ini is authoritative and is what gets
// verified; the WinDrv pair is kept in SYNC rather than left stale, so that if
// Shared.ini is ever deleted or regenerated (config reset, Steam file verify)
// it cannot silently drag the resolution back to a value the user changed
// months ago. Disagreeing configs are a trap; agreeing ones cost nothing.
//
// WHY IT MATTERS. The eye render IS the game's backbuffer, so the game's
// resolution is the VR resolution, and a headset eye is roughly square while a
// 16:9 render leaves unfilled bands top and bottom. BS1 learned over sessions
// 27-28 that those bands are an ASPECT problem and that raising FOV compensates
// on the wrong axis: match the render aspect to the eye and a sane FOV fills it
// exactly. That is why this lane exists before any FOV or viewmodel tuning on
// BS2 - trims tuned at the wrong aspect bake the error in and stop being
// portable across resolutions.
//
// WHY NOT A LIVE RESOLUTION CHANGE. BS2 has no engine `Exec` seam at all (b1r
// has console_exec.cpp; b2r has nothing), so the live `SETRES` question cannot
// even be ASKED without first deriving one - and BS1's answer, measured, is
// that its viewport-Exec SETRES faults with a near-null dereference and no
// ResizeBuffers follows. Deferred deliberately, not overlooked. If it is ever
// wanted, the BS2-native route is the ProcessEvent-by-name seam reaching
// PlayerController.ConsoleCommand (an FString param block: ptr/count/max),
// NOT a port of BS1's faulting viewport Exec. A change here therefore takes
// effect on the NEXT launch.
//
// SAFETY - AND BS2 IS WORSE THAN BS1 HERE. FIVE sections of Bioshock2SP.ini
// carry identical viewport key names: the PC driver plus the Xbox 360, PS3,
// Xbox One and PS4 ones (verified at lines 470/509/541/573/605). BS1 has four
// (no PS3). A naive key replacement finds the Xenon section and appears to
// succeed, so every write is section-scoped. Shared.ini has one section and is
// scoped the same way for the same reason. Both files are ANSI with CRLF
// endings and are edited in place, preserving every other byte.

#include <cstdint>

namespace bvr::b2r::game_ini {

// Full path to the located Shared.ini (the file that governs), or an empty
// string. Cached after the first call; the search is logged once.
const wchar_t* path();

struct Viewport {
    // The GOVERNING pair, from Shared.ini [SharedOptions].
    uint32_t w = 0, h = 0;
    // Bioshock2SP.ini [WinDrv.WindowsClient], reported only. Ignored by the
    // engine on this game; kept in sync so a config regeneration cannot revert
    // the resolution behind the user's back.
    uint32_t windowedW = 0, windowedH = 0;
    uint32_t fullscreenW = 0, fullscreenH = 0;
    bool startupFullscreen = false;
    bool valid = false; // true when the governing pair was read
};

Viewport read_viewport();

// Set the render resolution to w x h. Writes Shared.ini's ViewportX/Y (the
// authoritative pair, verified by read-back) and then keeps Bioshock2SP.ini's
// windowed AND fullscreen pairs in sync (best effort, non-fatal - the engine
// does not read them). MenuViewportX/Y is deliberately left alone: it is the
// pre-game menu surface, not the render target.
//
// Each file is backed up once to <name>.bvr-bak-res before its first edit.
// Returns false and logs on any failure; never leaves a partially written
// file. Game thread, on explicit request only. Takes effect on the NEXT
// launch - see the header note on why there is no live path.
bool write_viewport(uint32_t w, uint32_t h);

// One log line: both files' geometry and whether the governing pair agrees
// with the live backbuffer. A disagreement after a relaunch is the signal that
// something else is winning - which is precisely how the WinDrv keys were
// caught doing nothing.
void log_status(uint32_t liveBackbufferW, uint32_t liveBackbufferH);

} // namespace bvr::b2r::game_ini
