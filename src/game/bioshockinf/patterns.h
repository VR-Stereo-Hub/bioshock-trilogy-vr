#pragma once
// BioShock Infinite (BioShockInfinite.exe) signature table. This header and
// patterns.cpp are the ONLY files allowed to contain raw addresses/offsets for
// this game; every entry is documented in docs/bioshockinfinite/ENGINE_NOTES.md
// with its derivation method.
//
// Infinite is Unreal Engine 3 build 6829 with large Irrational replacements -
// NOT the Vengeance (UE2.5) tree the two remasters share. NEVER copy a number
// from bioshock1r/patterns.h or bioshock2r/patterns.h: different engine,
// different link, and here even the SHAPES are suspect (UE3 restructured
// UObject, FName, the actor attachment model and the skeleton representation).

#include "core/hooks/pattern_scan.h"

#include <cstdint>

namespace bvr::bsi::patterns {

// ---- host build identity (session 34, PE parse of the shipped exe) ----------
// Every RVA below was derived from ONE build of BioShockInfinite.exe:
// PE TimeDateStamp 0x627BE455 (2022-05-11 18:29:09 UTC), SizeOfImage
// 0x0124F000, 18,368,840 bytes on disk, sha256 C20A4252...14A0E77.
//
// The adapter is chosen by exe BASENAME and every storefront ships the same
// file name, so a differently-linked build is not rejected - it is ACCEPTED and
// then mis-addressed, which is how a mod corrupts a game rather than failing to
// work. Address-dependent features stand down on a mismatch; the log, overlay
// and command seam keep working, and the game stays fully playable.
inline constexpr uint32_t kHostTimeDateStamp = 0x627BE455;
inline constexpr uint32_t kHostSizeOfImage = 0x0124F000;
inline constexpr uint64_t kHostFileBytes = 18368840;
// Logged for the record only - module_id::matches does not test the checksum
// (BS1's exe carries 0, so the field is not universally usable).
inline constexpr uint32_t kHostCheckSum = 0x011590C3;

// ASLR is OFF on this exe (DllCharacteristics 0x8100, no DYNAMIC_BASE), unlike
// both remasters - it loads at a fixed base. That is a convenience, not a
// licence: everything below stays an RVA so a rebased build (or a storefront
// that turns ASLR back on) costs nothing.
inline constexpr uint32_t kExpectedImageBase = 0x00400000;

// ---- the camera seam (derived offline session 34, UNCONFIRMED live) --------
// APlayerController::GetPlayerViewPoint. Reached from the exec thunk, which is
// how it was FOUND and is never a hook target: a static E8 caller census gives
// the thunk 0 callers and the implementation 14 (13 of them native call sites).
// thiscall, 2 stack args (FVector* outLoc, FRotator* outRot), `ret 8` - and
// `ret imm / 4 == 2` is the argument count any probe hook must use, or the RTC
// dialog that writes no crash dump is the result.
//
// I1 consumes these READ-ONLY, to log the bytes actually living there. Hooking
// is I2 (DR-I2).
inline constexpr uint32_t kGetPlayerViewPointRva = 0x1E10C0;
inline constexpr uint32_t kGetPlayerViewPointThunkRva = 0x129280;

// The first 12 bytes living at kGetPlayerViewPointRva, read live in session 35
// and used as an install-time gate: if the running build does not have exactly
// these bytes here, the RVA means something else and the hook REFUSES rather
// than detouring whatever is there.
//   55              push ebp
//   8B EC           mov ebp, esp
//   83 E4 F0        and esp, -0x10        <- aligned stack, for the SSE transform
//   81 EC A4 00 00 00  sub esp, 0xA4
// Session 35 read 16 bytes; only the first 12 are used, because bytes 12-15
// (53 56 8B 75) begin `push ebx; push esi; mov esi,[ebp+disp]` whose
// displacement byte falls outside the window and is not worth pinning.
inline constexpr uint8_t kGetPlayerViewPointPrologue[] = {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0,
                                                          0x81, 0xEC, 0xA4, 0x00, 0x00, 0x00};

// `ret 8` - and 8/4 == 2 is the number of stack args any detour on this target
// MUST declare. Verified at install time by finding C2 08 00 in the body.
inline constexpr uint8_t kGetPlayerViewPointRetImm = 8;

// ---- UE3 reflection (derived offline session 36, DR-I1) --------------------
//
// Derivation, in full, because it is reusable and it is what made the frameless
// functions in this exe tractable:
//   1. Collect EVERY E8 call target in .text (38,638 of them). That set is the
//      complete list of known function entry points, so "the function
//      containing address X" is "greatest entry <= X" by binary search. No
//      `CC CC CC 55 8B EC` backward walk, hence no silent "returned the
//      PREVIOUS function" failure - which is exactly how that heuristic breaks
//      on this exe (the exec thunk at 0x129280 is frameless).
//   2. `Failed to find function` exists as a UTF-16 literal at .rdata 0xD07CC0
//      (appErrorf takes TCHAR). Its only code xref is 0xD1131, which step 1
//      resolves to the function at 0xD1090 = UObject::FindFunctionChecked.
//   3. That function has 426 E8 callers - the generated event stubs. In each,
//      the call that FOLLOWS is ProcessEvent. 407 of 420 decodable callers
//      agree on vtable offset +0x7C.
//   4. Sweeping .rdata/.data for vtable candidates (runs of >= 24 consecutive
//      dwords pointing into an executable section) and reading slot +0x7C from
//      each gives 1038 votes for 0xCFE70 and 175 for 0x19A150.
//
// TRAP worth recording: MSVC hoists the vtable pointer into a register, so the
// stub reads `mov edi,[esi]` ... `call FindFunctionChecked` ...
// `mov edx,[edi+0x7C]` / `call edx`. Searching for `call [reg+disp]` (FF /2)
// finds ZERO of the 426. A decoder must resolve `call reg` through the
// preceding load.
inline constexpr uint32_t kProcessEventRva = 0xCFE70;
inline constexpr uint32_t kActorProcessEventRva = 0x19A150;
inline constexpr uint32_t kFindFunctionCheckedRva = 0xD1090;

// UObject vtable byte offset of ProcessEvent. Recorded because a vtable read
// survives a rebuild that moves the function, while kProcessEventRva does not -
// the RVA is the cross-check, the slot is the durable locator.
inline constexpr uint32_t kProcessEventVtableOffset = 0x7C;
// UObject::FindFunction, read out of FindFunctionChecked's own body
// (`mov eax,[esi]; mov edx,[eax+0x54]; call edx`, then the null test that
// produces the appErrorf above). Recorded, not yet consumed.
inline constexpr uint32_t kFindFunctionVtableOffset = 0x54;

// BOTH are thiscall with 3 stack args, `ret 0xC`, confirmed as the only ret imm
// in each body (ProcessEvent 672 bytes, FindFunctionChecked 304 bytes, both
// int3-padded to the next function). 0xC/4 == 3 is the argument count any
// detour on either MUST declare, or the RTC dialog that writes no crash dump
// is the result.
inline constexpr uint8_t kProcessEventRetImm = 0x0C;
inline constexpr uint8_t kFindFunctionCheckedRetImm = 0x0C;

// ---- GNames / FName (derived offline session 34) ---------------------------
// GNames is a UE3 TArray<FNameEntry*> - the classic { Data, Num, Max } triple -
// traced from the hardcoded UnNames.h string run.
//
// TIMING, and it is load-bearing: our DLL is pulled in from the proxy's
// DllMain during the exe's import resolution, which runs BEFORE the exe's CRT
// static initializers and long before UE3 registers any name. At
// patterns::resolve() time this array is empty. Every reader below must be
// command- or hook-driven, never init-driven.
inline constexpr uint32_t kGNamesDataRva = 0xF9DFEC;
inline constexpr uint32_t kGNamesNumRva = 0xF9DFF0;
inline constexpr uint32_t kGNamesMaxRva = 0xF9DFF4;

// Name hash table: `mov [eax*4 + 0xF58BF8], ebp` after `and eax, 0xfff`.
// RECORDED, NOT CONSUMED - using it means calling the engine's own hash
// (0x80C70 ASCII / 0x80C10 wide), whereas a linear GNames walk is both safer
// and INDEPENDENT of the derivation above, which is what lets the two
// cross-check each other.
inline constexpr uint32_t kNameHashRva = 0xF58BF8;
inline constexpr uint32_t kNameHashBuckets = 4096;

// FNameEntry, decoded from the pool allocator and the registration function.
//
// THE DIFFERENCE FROM BS1 IS WHAT MAKES A NAIVE PORT RETURN GARBAGE: BS1's text
// at +0x10 is ALWAYS UTF-16. Here +0x8 packs (index << 1) | isWide, bit 0
// selects the encoding, and ASCII is the DEFAULT (the allocator copies 5 bytes
// for "None"). Two different hash functions are called on the flag, 0x80C70 vs
// 0x80C10, which is how the bit was identified.
inline constexpr uint32_t kFNameEntryIndexFlagsOffset = 0x8;
inline constexpr uint32_t kFNameEntryHashNextOffset = 0xC;
inline constexpr uint32_t kFNameEntryTextOffset = 0x10;
inline constexpr uint32_t kFNameEntryWideBit = 0x1;

// UE3's NAME_SIZE. Nothing legitimate is longer, and the cap is what stops a
// recycled or corrupt entry walking off the end of its page.
inline constexpr size_t kFNameMaxChars = 1024;
// Below this fname_text() refuses rather than truncating: a truncated name
// silently mis-keys a per-name filter, which is worse than no name at all.
inline constexpr size_t kFNameTextBufMin = 64;

// UObject::Class, from execIsA (`mov eax,[edi+0x20]`).
inline constexpr uint32_t kUObjectClassOffset = 0x20;

// False when the running exe is not the build the addresses came from. Anything
// consuming a raw RVA must check this first.
bool rva_trusted();

// Force the gate closed at RUNTIME, to exercise the stand-down path without
// building a sabotaged DLL that can be mistaken for a real one (BS1 session 27
// learned that the expensive way). `buildgate off|on|status` on the command
// seam.
void handle_buildgate_command(const char* args);

// What resolve() observed, for the overlay and the status command.
struct Symbols {
    const uint8_t* imageBase = nullptr;
    size_t imageSize = 0;
    bool buildVerified = false;
    // First bytes at kGetPlayerViewPointRva, read-only. Zeroed if the gate is
    // closed or the address is not readable.
    bool viewPointReadable = false;
    uint8_t viewPointBytes[16] = {};
};

// Runs the build gate and the read-only probes. Never hooks, never writes,
// never scans. Returns false when the host build is not the one every RVA came
// from; the adapter stays alive either way (fail soft).
bool resolve(const bvr::pattern_scan::ProcessImage& image, Symbols& out);

// The last resolve() result, for the overlay and `bsi` status.
const Symbols& symbols();

// Live image base, cached by resolve(). Null before resolve() has run. Every
// RVA above is turned into an address through this and nothing else.
const uint8_t* image_base();
size_t image_size();

// Turns an RVA into a live address, or null when the gate is closed, resolve()
// has not run, or the RVA is outside the image. This is the ONLY sanctioned way
// to consume a constant from this header.
const uint8_t* rva_to_address(uint32_t rva, size_t needBytes);

// ---- GNames readers (session 36) -------------------------------------------
//
// ALL OF THESE FAIL SOFT AND MUST NOT BE CALLED FROM init(). See the timing
// note beside kGNamesDataRva: GNames is empty at DllMain time, so a call that
// arrives too early returns false and says "not populated yet" rather than
// reading a half-built array.

// GNames.Num / GNames.Max, or 0 when unavailable.
int32_t fname_count();
int32_t fname_max();

// Copies GNames[index]'s text into `out` as NUL-terminated printable ASCII (a
// wide entry is transcoded, anything outside 0x20..0x7E becoming '?'), so the
// result is always safe to print with %s. Cannot return a bare pointer the way
// BS1's does: half this pool may be UTF-16, and a caller printing the wrong
// encoding gets mojibake instead of an error.
//
// Returns false - and writes an empty string - on ANY of: the build gate
// closed; GNames not yet populated; index outside [0, Num); the slot, entry or
// text unreadable; THE ENTRY'S OWN INDEX at (+0x8 >> 1) disagreeing with
// `index`; no terminator within kFNameMaxChars; or outSize < kFNameTextBufMin.
bool fname_text(int32_t index, char* out, size_t outSize);

// The encoding flag on its own, so a selftest can report a pool with zero wide
// entries as UNTESTED rather than silently assuming the wide path works.
bool fname_is_wide(int32_t index, bool& outWide);

// Linear, case-insensitive GNames lookup. Returns -1 on miss.
//
// ONE-SHOT ONLY - never on a cadence. BS1 session 22 established that a
// per-frame or per-second scan of an engine-sized table reads to the player as
// "the game freezes every couple of seconds". Deliberately NOT the engine's
// hash table at kNameHashRva: an independent instrument is the whole point,
// because it is what lets the two disagree.
int32_t fname_find(const char* text);

} // namespace bvr::bsi::patterns
