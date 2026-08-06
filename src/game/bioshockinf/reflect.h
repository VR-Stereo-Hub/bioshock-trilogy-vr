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

// Class name of an arbitrary pointer IF it reads as a UObject; false and an
// empty string otherwise. The gates (readable object, readable class, name
// index in range, name text readable) are what makes walking raw engine fields
// safe. Promoted from reflect.cpp's anonymous namespace in session 46 so the
// rig can validate its target's identity before writing to it.
//
// THE S45 TRAP, and the reason this is never sufficient on its own: the
// heuristic will happily resolve a NON-object address that happens to point at
// plausible memory - `bsifields` named the raw object-pointer list head
// "XWeaponModelFirstPerson". A class name from a walk is a HINT until the
// layout corroborates it.
bool object_class_name(const void* obj, char* out, size_t outSize);

// Derive UObject::Name's offset off the latched PC's class object if it is not
// known yet. Cheap and idempotent once resolved; false = no PC yet, so
// object_class_name cannot work. Game thread.
bool ensure_obj_name_offset();

// Overlay section.
void draw_debug_ui();

} // namespace bvr::bsi::reflect
