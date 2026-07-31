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

} // namespace bvr::bsi::patterns
