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
    // Filled by find_native_function_ex only; the UE2 entry point below leaves
    // them 0. These exist so a NEGATIVE result names its own stage - an
    // instrument that reports nothing but "not found" cannot be shown to have
    // failed correctly, and this project's rule is that such an instrument is
    // not evidence.
    size_t terminatorRejects = 0;  // occurrence whose own NUL was missing
    size_t neighbourRejects = 0;   // ref whose neighbouring entries were malformed
    size_t implRejects = 0;        // ref whose impl pointer was not code in the image
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

// ---- generalised native-table lookup (added session 36 for UE3) ------------
//
// Shape of one entry in an engine's name-based native registration table. Both
// UE2.5 and UE3 register every C++-implemented UnrealScript native through a
// static table of { const CHAR* name; Native impl; ... }, but the stride, the
// name encoding and the name SPELLING all differ between them. Nothing here is
// game-specific: this is how the engine registers name-based natives.
struct NativeTableShape {
    size_t entryStride = 12;                 // bytes between consecutive entries
    size_t implOffset = 4;                   // byte offset of the impl pointer
    bool wideNames = true;                   // true = UTF-16LE names, false = ASCII
    const char* nameFormat = "int%sexec%s";  // printf(className, funcName)
};

// UE2.5 / Vengeance (BioShock 1 + 2 Remastered): 12-byte
// { const TCHAR* name; Native impl; 0 }, names "int<Class>exec<Func>" UTF-16.
inline constexpr NativeTableShape kNativeTableUE2{12, 4, true, "int%sexec%s"};

// UE3 build 6829 (BioShock Infinite): 8-byte { const ANSICHAR* name; Native
// impl; }, names "<Class>exec<Func>" in ASCII with NO "int" prefix, 2647
// entries. Derived offline 2026-07-31 (session 34) by dumping the registration
// strings out of .rdata and following their .data references.
//
// The impl pointer is the exec THUNK, not the C++ implementation. Confirmed by
// static caller census (session 36): every thunk has 0 E8 callers. Use this to
// FIND an implementation - disassemble the thunk, take the last call before the
// epilogue - never as a hook target.
inline constexpr NativeTableShape kNativeTableUE3{8, 4, false, "%sexec%s"};

// Generalised native-function lookup: `shape` selects the table layout, so a
// new engine costs a NativeTableShape rather than a fork of the scan.
//
// ADDITIVE. find_native_function() above keeps its exact body and its exact UE2
// behaviour; nothing BioShock 1 or 2 calls changes.
//
// `verifyNeighbours` matters at an 8-byte stride and should stay on for UE3: a
// chance dword match somewhere in 18 MB of image is far likelier when entries
// are 8 bytes rather than 12. A table is contiguous, so a real entry always has
// a well-formed neighbour and a coincidence almost never does.
bool find_native_function_ex(const ProcessImage& img, const NativeTableShape& shape,
                             const char* className, const char* funcName,
                             NativeScanResult& out, bool verifyNeighbours = true);

// From any verified entry, walk both directions while entries stay well formed.
//
// WHAT THIS ACTUALLY RETURNS, established live session 36: engines that register
// natives this way emit ONE BLOCK PER CLASS, separated by `{0, 0}` sentinel
// entries - the classic `AutoRegisterNatives` layout. So the walk yields the
// seed's OWN CLASS block, not the whole registry. On BioShock Infinite,
// seeding from any `APlayerController` native returns exactly **46**, which is
// that class's native count from the offline per-class census; the image's 2647
// is the total across every class and is NOT a contiguous run.
//
// That makes this a real falsifiable check, just a per-class one: a count that
// disagrees with the class's known native count means the stride or the
// well-formedness test is wrong.
struct NativeTableBounds {
    const uint8_t* base = nullptr;
    size_t count = 0;
    size_t seedIndex = 0;
};
bool native_table_bounds(const ProcessImage& img, const NativeTableShape& shape,
                         const uint8_t* seedEntry, NativeTableBounds& out);

// Direct table walk: strcmp every entry name against "<Class>exec<Func>".
// O(count) with one compare each, versus two full-image sweeps for the scan
// above - and, more importantly, a SECOND INDEPENDENT INSTRUMENT answering the
// same question. The two must agree; when they do not, both are suspect.
//
// `table` must be the block for the CLASS being looked up (see above) - a block
// seeded from a different class cannot contain the name and will report a
// miss, which is a property of the layout and not a failure of either
// instrument. ASCII shapes only (wideNames == false).
bool find_native_in_table(const ProcessImage& img, const NativeTableShape& shape,
                          const NativeTableBounds& table, const char* className,
                          const char* funcName, NativeScanResult& out);

} // namespace bvr::pattern_scan
