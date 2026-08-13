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

// s47: class name of an arbitrary pointer IF it reads as a genuine UObject
// (UClass-fixpoint gated - the s45b hardening against raw structs that walk
// as fake objects). Best-effort derives the UObject::Name offset first, so a
// call before a PlayerController exists returns false cleanly. Game thread.
// `out` needs >= 64 bytes (the fname_text buffer contract). Added for the I8
// per-weapon profile scaffold; instruments-only otherwise.
bool class_name_of(const void* obj, char* out, size_t outSize);

// s48: the OBJECT's own FName index (not its class's) - the cheap identity for
// hot paths: one gated 4-byte read once the UObject::Name offset has derived,
// -1 before that or on any gate failure. The fidget filter compares this
// against a pre-resolved index instead of doing text work per event.
int32_t object_name_index(const void* obj);

// s48: the derived UObject::Name byte offset itself (-1 until derived). For
// hot paths that must read a name index per event without per-read gates -
// the caller owns the safety argument (e.g. the engine was about to use the
// same pointer itself).
int32_t uobject_name_offset();

// s48b: silent UProperty-chain search (the bsiprop walk without the prints).
// Walks obj's class chain (Children/Next/Super links anchored on the derived
// Name offset - the s48 layout derivation, ENGINE_NOTES) for a BoolProperty
// named propName and hands back ITS OWN recorded byte offset and bitmask -
// self-deriving every boot, so a game patch moves the property and this moves
// with it (or refuses) instead of writing through a stale constant. Game
// thread; one-shot cost ~hundreds of gated reads - never on a cadence.
bool find_bool_property_bit(const void* obj, const char* propName, uint32_t* outByteOff,
                            uint32_t* outMask);

// s48b: generic form - the property's byte offset, gated on its class naming
// expectClass (nullptr = any). Same walk, same one-shot discipline.
bool find_property_offset(const void* obj, const char* propName, const char* expectClass,
                          uint32_t* outByteOff);

// Dispatch a UFunction BY NAME on an explicit object from adapter code - the
// bsicallat lane (FindFunction +0x54 then ProcessEvent +0x7C, SEH-isolated)
// with all of its gates: build gate, game-thread interlock, GNames populated,
// vtable-slot RVA match on THE OBJECT. `parms` is the caller's zeroed block
// (args in, out-params/return back out); pass at least 64 bytes. Returns true
// only when the function resolved AND the dispatch returned without fault -
// which is NOT acceptance of any effect, only of delivery. Added s45b for the
// bone rig resolve (GetFirstPersonAttachment); instruments-only otherwise.
bool call_on_object(void* obj, const char* funcName, void* parms);

// s52: the by-INDEX variant for cadenced callers. fname_find is a linear
// scan of the whole pool (~70k entries, hundreds of ms) - running it per
// poll stuttered the entire game at 2-3 Hz (the "fname_find never on a
// cadence" rule, re-proven). Resolve once with find_function_index (retry
// until >= 0 - GNames fills late), cache the index for the process
// lifetime (indices are stable per boot), then dispatch by index.
bool call_on_object_by_index(void* obj, int32_t fnameIndex, void* parms);
int32_t find_function_index(const char* funcName);

// s52: DynamicLoadObject by full path from adapter code (the bsiload lane as a
// callable - the arsenal's rung 1). All of bsiload's gates and its trailing-
// whitespace trim apply; returns the loaded object or null. Game thread.
void* load_object(const char* path);

// Overlay section.
void draw_debug_ui();

} // namespace bvr::bsi::reflect
