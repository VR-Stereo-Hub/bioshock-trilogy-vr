#pragma once
// Generic in-process memory scanning: module capture, byte searches, and the
// FName-chain scan that locates UnrealScript event functions in UE2.5-era
// binaries from their event-name string. Game-agnostic - no game names or
// addresses here; per-game usage lives in game/<title>/patterns.cpp.
//
// FName-chain scan technique ported from
// itsloopyo/bioshock-remastered-headtracking (MIT license), src/memory.rs.

#include <cstdint>
#include <vector>

namespace bvr::pattern_scan {

// Live image of a loaded module: [base, base + size). Scans run against live
// memory, so loader relocations are already applied to every immediate we
// search for or extract - the whole chain is relocation-transparent.
struct ProcessImage {
    const uint8_t* base = nullptr;
    size_t size = 0;
};

// Main module of the current process via GetModuleInformation.
bool capture_main_module(ProcessImage& out);

// True if [addr, addr+size) is committed, readable memory within one region.
bool is_memory_valid(const void* addr, size_t size);

// All occurrences of `ascii` encoded as UTF-16LE inside the image.
std::vector<const uint8_t*> find_wide_string(const ProcessImage& img, const char* ascii);

// All occurrences of `ascii` as a raw byte string (no terminator) inside the
// image - for ANSI/UTF-8 string tables (e.g. ini key names).
std::vector<const uint8_t*> find_ascii_string(const ProcessImage& img, const char* ascii);

// All occurrences of the 32-bit little-endian value of `target` - i.e. every
// imm32/disp32 in the image that references that address.
std::vector<const uint8_t*> find_references(const ProcessImage& img, const void* target);

// From a code xref to an FName's init string: forward-scan (<=96 bytes) for
// the E8 CALL into the FName constructor, then past that 5-byte CALL for the
// 89 0D (mov [imm32], ecx) store that caches the returned name index. Returns
// the address of that global, or null. `ctorOut` (optional) receives the
// resolved CALL target - the FName constructor itself, the entry point of the
// engine's whole name system (session 20: disassembling it yields GNames).
const uint8_t* find_fname_index_global(const ProcessImage& img, const uint8_t* stringXref,
                                       const uint8_t** ctorOut = nullptr);

// Backward-scan (<=512 bytes) from an xref for the MSVC function prologue
// CC CC CC 55 8B EC. Returns the address of the 55 (push ebp), or null.
const uint8_t* find_function_start(const ProcessImage& img, const uint8_t* xref);

// Stage-by-stage diagnostics of find_event_function, for caller-side logging.
struct EventScanResult {
    size_t stringMatches = 0;            // wide-string occurrences of the name
    size_t stringXrefs = 0;              // code refs to those strings
    const uint8_t* fnameIndexGlobal = nullptr;
    const uint8_t* fnameCtor = nullptr;  // the FName constructor (session 20)
    size_t globalXrefs = 0;              // refs to the FName index global
    size_t candidates = 0;               // refs surviving the init-site filter
    void* function = nullptr;            // resolved event function entry point
};

// Full chain: wide string -> string xrefs -> FName index global -> global
// xrefs (skipping any within 200 bytes of the init site) -> containing
// function prologue. First candidate whose prologue lands in valid memory
// wins. Returns true and fills out.function on success.
bool find_event_function(const ProcessImage& img, const char* eventName, EventScanResult& out);

// Stage-by-stage diagnostics of find_native_function.
struct NativeScanResult {
    size_t stringMatches = 0;  // occurrences of the registration name
    size_t tableRefs = 0;      // dword references to it (candidate table entries)
    const void* tableEntry = nullptr;
    void* function = nullptr;  // resolved execFoo implementation
};

// UE2 native-function lookup: every `native` UnrealScript function implemented
// in C++ is registered through a 12-byte table entry
// `{ const TCHAR* name; Native impl; 0 }` whose name is the wide string
// "int<Class>exec<Function>" (e.g. "intAWeaponexecApplyAimError") in .rdata.
// The impl pointer is written into the entry by static initialization, so at
// runtime the entry IS the symbol table: find the name string, find the dword
// that references it, and read the next dword.
//
// Derived 2026-07-25 (M6) by dumping the registration strings out of the exe's
// .rdata and following their .data references (docs/bioshock1/ENGINE_NOTES.md "Native
// function table"). Nothing here is game-specific - it is how the engine
// registers name-based natives, so BioShock 2 will resolve the same way.
bool find_native_function(const ProcessImage& img, const char* className,
                          const char* funcName, NativeScanResult& out);

} // namespace bvr::pattern_scan
