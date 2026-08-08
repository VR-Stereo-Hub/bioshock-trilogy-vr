#pragma once
// BioShock Infinite's UE3 reflection instruments (I2 / DR-I1).
//
// Instruments only: this module consumes constants from patterns.h and declares
// none of its own. Nothing here hooks or writes; it resolves, reads and reports.
//
// EVERY READER IS COMMAND-DRIVEN, NEVER INIT-DRIVEN. Our DLL loads from the
// proxy's DllMain during the exe's import resolution, before the exe's CRT
// static initializers, so GNames is empty at adapter-init time. The native
// table is the opposite case - its pointers are link-time constants with
// relocations, so it is valid from the first instruction - but keeping both on
// the command seam costs nothing and removes a whole class of "why is this
// empty" confusion.

#include "core/hooks/pattern_scan.h"

namespace bvr::bsi::reflect {

// Caches the image for later command-driven scans. Never scans here.
void init(const bvr::pattern_scan::ProcessImage& image);

// `bsinative`, `bsinames`, `bsiname`, `bsireflect`, `bsivtable`.
// Returns false when the command is not ours.
bool handle_command(const char* cmd, const char* args);

// Dispatch a console command through ConsoleCommand-by-name (the proven
// bsiexec lane: setres works, session 37) from adapter code. All of
// cmd_exec's gates apply - build gate, GAME-THREAD interlock, live vtable
// RVA match - so calling this off the game thread refuses cleanly.
// Acceptance is the downstream EFFECT (the backbuffer, a file), never a
// return value; that is why this returns void.
void exec_console(const char* cmd);

// Dispatch a UFunction BY NAME on an explicit object from adapter code - the
// bsicallat lane (FindFunction +0x54 then ProcessEvent +0x7C, SEH-isolated)
// with all of its gates: build gate, game-thread interlock, GNames populated,
// vtable-slot RVA match on THE OBJECT. `parms` is the caller's zeroed block
// (args in, out-params/return back out); pass at least 64 bytes. Returns true
// only when the function resolved AND the dispatch returned without fault -
// which is NOT acceptance of any effect, only of delivery. Added s45b for the
// bone rig resolve (GetFirstPersonAttachment); instruments-only otherwise.
bool call_on_object(void* obj, const char* funcName, void* parms);

// Overlay section.
void draw_debug_ui();

} // namespace bvr::bsi::reflect
