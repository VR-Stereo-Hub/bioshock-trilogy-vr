#pragma once
// BioShock 1 live render resize through the engine's SetRes (ENGINE_NOTES 2026-10-11).
// Re-resolve the viewport/device for every call, pin both vtable and entry, byte-check
// the ABI, and confirm by the backbuffer. The engine saves the active mode's size.
// The old window route remains a command-only diagnostic.
#include "core/hooks/pattern_scan.h"
#include <cstdint>

namespace bvr::b1r::resize {
// Adapter startup; optional path is also used by the offline fixture. Overrides in
// [Bioshock1R] are decimal or 0x-prefixed RVAs; they never bypass the prologue check.
void init(const bvr::pattern_scan::ProcessImage& image, const wchar_t* iniPath = nullptr);
// Any thread: queue a live resize to w x h (client area = render size). Applied on the
// game thread by tick(). One request at a time; a second while one is in flight is dropped
// with a log line.
void request(uint32_t w, uint32_t h, const char* who);
// Game thread, once per frame (camera.cpp's per-frame work): apply a queued request and
// confirm it against the backbuffer.
void tick();
// True while a request is queued or awaiting confirmation.
bool busy();
// One line for the F10 panel.
const char* status();
// The last size confirmed live (0 x 0 = none this session).
void last_confirmed(uint32_t* w, uint32_t* h);
// `vrres live <w>x<h>` (camera.cpp routes it).
void handle_command(const char* args);

} // namespace bvr::b1r::resize
