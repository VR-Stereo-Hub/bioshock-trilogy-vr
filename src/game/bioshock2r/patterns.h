#pragma once
// BioShock 2 Remastered (Bioshock2HD.exe) signatures. This header and
// patterns.cpp are the ONLY files allowed to contain raw addresses/offsets for
// this game; every value is documented with its derivation method in
// docs/bioshock2/ENGINE_NOTES.md. BS2 was built 16 minutes before BS1 from the
// same engine tree, so SHAPES transfer - but never copy a number from
// bioshock1r/patterns.h: every RVA/offset is derived fresh against this exe.

#include "core/hooks/pattern_scan.h"

#include <cstdint>

namespace bvr::b2r::patterns {

// MSVC RTTI walk (TypeDescriptor ".?AVClass@@" -> CompleteObjectLocator ->
// vtable), run offline against Bioshock2HD.exe on 2026-07-29; the method was
// validated by reproducing all of BS1's known-good vtable RVAs exactly.
// CANDIDATE until runtime-verified: a live object's dword0 must equal
// imageBase + RVA. The camera module logs the observed view-actor vtable RVA
// every time it changes, so a wrong candidate names its own correction from
// any session log.
constexpr uint32_t kShockPlayerVtableRva = 0x11197C0;           // AShockPlayer
constexpr uint32_t kShockPlayerControllerVtableRva = 0x1117BF0; // AShockPlayerController

// --- the PlayerCalcView dispatch seam (session 24) --------------------------
// BS1 hooks the compiler-generated event thunk (eventPlayerCalcView, 29 static
// callers). On BS2 THE SAME THUNK EXISTS BUT HAS ZERO STATIC CALLERS - the
// 16-minutes-earlier build INLINED the event-dispatch glue at ~35 call sites.
// Each site builds the 0x1C param block {AActor* viewActor; FVector loc;
// FRotator rot}, loads the 8-byte FName {index, number} from the cached-index
// global (found by the FName-chain scan), calls
// UObject::FindFunctionChecked(FName, UBOOL) and then virtual
// ProcessEvent(UFunction*, void* parms, void* result) at VTABLE SLOT 3
// (byte offset 0xC). So the BS2 camera seam is: hook FindFunctionChecked to
// LEARN the PlayerCalcView UFunction pointer (no UObject layout assumptions),
// and hook ProcessEvent to mutate the param block after the original returns
// (the inlined caller copies the block out afterwards - dump-verified).
// Derivation 2026-07-29: offline capstone disasm of the inlined call sites
// (RVAs 0x4D6970/0x868759/0xA092CF and siblings), the controller vtable
// slot 3 jmp-stub chain, and both function heads; docs/bioshock2/
// ENGINE_NOTES.md has the full trace.

// UObject::ProcessEvent outer impl: `ret 0xC` = exactly 3 stack args
// (UFunction*, parms, result) - same shape as BS1's outer at RVA 0x375140
// (StateFrame gate at this+0x10C, script-disable globals, tail-jmp inner).
constexpr uint32_t kProcessEventRva = 0x37A7E0;
constexpr uint8_t kProcessEventPrologue[] = {0x55, 0x8B, 0xEC, 0x8B, 0x81,
                                             0x0C, 0x01, 0x00, 0x00};
// push ebp; mov ebp,esp; mov eax,[ecx+0x10C]

// UObject::FindFunctionChecked(FName name8, UBOOL global) - callee pops 12.
// Reached from the inlined sites via the incremental-link jmp stub at RVA
// 0x20365; constant is the REAL body the stub jumps to.
constexpr uint32_t kFindFuncCheckedRva = 0xB6BA30;
constexpr uint8_t kFindFuncCheckedPrologue[] = {0x55, 0x8B, 0xEC, 0x64, 0xA1,
                                                0x00, 0x00, 0x00, 0x00};
// push ebp; mov ebp,esp; mov eax,fs:[0]  (SEH frame)

// ProcessEvent's slot in every UObject vtable (byte offset). Runtime resolve
// follows the controller vtable slot through its E9 stub and cross-checks the
// target against kProcessEventRva - a wrong candidate vtable RVA or a build
// mismatch names itself in the log instead of hooking garbage.
constexpr uint32_t kProcessEventVtblByteOffset = 0xC;

// Further candidates from the same RTTI walk, recorded for the milestone's
// next steps (hands, skeleton, console exec). Nothing consumes them yet, so
// they stay comment-only until the consuming code lands:
//   APlayerWeapon      0x112CC78   AHands           0x1125478
//   SkeletonInstance   0x10D0FC0
//   UGameEngine        0x10BD7DC / 0x10BD9E8 (BS1's console_exec used the
//   second of the pair - expect the same here, but verify before calling).

// --- render-substrate discovery (session 26) --------------------------------
// The engine's ONLY static path to kernel32!SetEvent is the event-object
// signal ("Trigger") method below: `push [ecx+4]; call [IAT SetEvent]; ret` -
// a 10-byte __thiscall with the HANDLE at object+4 (the same event-object
// shape BS1 documented on its submit kick). It has ZERO static E8 callers
// (virtual dispatch), so the submit is derived LIVE: `reentry kick2` hooks
// this method and samples _ReturnAddress() = the engine-side call site.
// Session-25 recon CORRECTIONS (offline capstone census, 2026-07-29 s26):
// 0xB81020 (E9 stub 0xF01A) is SuspendThread/ResumeThread on a thread object,
// 0xCDB917 (stub 0xD9BD) is a crit-section once-init helper, the second FF15
// SetEvent site 0xCDB9BB is CRT encoded-pointer once-init machinery, and the
// FF25 import jmp-thunks (SetEvent's at 0xF3E494) have zero callers - none of
// these are render seams. Sync-primitive census + derivations in
// docs/bioshock2/ENGINE_NOTES.md. Recorded there, comment-only here until a
// consumer lands: ReleaseSemaphore FF15 cluster 0x5C98CE/0x5C9A1B/0x5C9B84/
// 0x5C9DB8 (job-system shape); WFSO(INFINITE, handlePair[i<2]) helper
// 0x7C3E40 over the global handle pair at RVA 0x1803A14 (pump-wait
// candidate); CreateSemaphoreW ctor 0xCF3EE0 (session-25 "pump candidate"
// label was wrong - it is a semaphore-object constructor).
constexpr uint32_t kEventTriggerRva = 0xB81050;
// Opcode bytes only: the FF15 operand embeds the ASLR-rebased IAT VA, so the
// last 4 bytes of the instruction differ from the disk image at runtime.
constexpr uint8_t kEventTriggerPrologue[] = {0xFF, 0x71, 0x04, 0xFF, 0x15};
// push dword ptr [ecx+4]; call dword ptr [imm32]

// --- UShockUserSettings: the FOV option (session 25) ------------------------
// Vtable RVA runtime-VERIFIED 2026-07-29: vtscan found a live heap object
// whose dword0 == base + RVA (the two other matches were stack slots holding
// the same pointer). HorizontalFOV offset derived FRESH - BS1's +0x8C does
// NOT transfer (it reads 3 here): located by ini-adjacency (MouseIconScale=10
// immediately precedes HorizontalFOV=100 in [ShockGame.ShockUserSettings] of
// Bioshock2SP.ini, mirrored in the object as +0x48=10, +0x4C=100) and proven
// by poke evidence (100->130 screenshot img-diff mean-abs 8.99 / 39.3%
// changed vs a 1.34 / 5.0% ambient floor after restore; the drill viewmodel
// re-lensed WITH the world = BS2's foreground follows the option natively;
// monotonic widening through 150 = no code clamp in the writable range).
// int32 DEGREES, renderer-consumed per frame, no options APPLY. Full recipe
// in docs/bioshock2/ENGINE_NOTES.md.
constexpr uint32_t kUserSettingsVtableRva = 0x11523D8;
constexpr uint32_t kUserSettingsHfovOffset = 0x4C;

// Live pointer to the HorizontalFOV int, or null while the settings object
// is not located. Cache + revalidate by vtable dword every call; a miss
// falls through to the heap scan (rate-limited, DORMANT after 3 straight
// misses - hfov_scan_rearm() clears that, called on view-state changes).
// Game thread only.
int32_t* hfov_option_ptr();
void hfov_scan_rearm(const char* why);

// --- heap scan for vtable-identified objects (session 25) -------------------
// BS2 shape of BS1's scanner (bioshock1r/patterns.cpp - duplicated per the
// duplicate-now seam policy): walk the FULL 4 GB committed private RW space
// (the game is LAA; actors allocate above 2 GB) for objects whose dword0 is
// imageBase + vtableRva. `accept` is called per match until it takes one;
// every call site must treat this as EXPENSIVE (multi-second) - one-shot use
// only, never on a cadence (BS1 session-22 lesson: a scan cadence reads as
// "the game freezes every couple of seconds").
using ObjectAccept = bool (*)(void* obj, void* user);
void* scan_for_vtable_object(uint32_t vtableRva, uint32_t needBytes, ObjectAccept accept,
                             void* user, const char* what, int* outMatches);

struct Symbols {
    // The dead-on-BS2 event thunk (RVA 0x395CC0 live) - resolved and logged
    // for the knowledge base, NEVER hooked: its signature differs from BS1's
    // (it builds its own param block from cached controller fields).
    void* eventPlayerCalcView = nullptr;
    // Cached FName index global for "PlayerCalcView" (8-byte FName: this
    // dword is the index, +4 the number). The FindFunctionChecked filter
    // compares against *this.
    const uint8_t* fnameIndexGlobal = nullptr;
    void* processEvent = nullptr;       // resolved via vtable slot + prologue gate
    void* findFuncChecked = nullptr;    // resolved via constant + prologue gate
};

// Runs the PlayerCalcView event scan (for the FName global) and resolves the
// dispatch seam above. Logs every stage; false = nothing usable resolved, the
// game runs flat.
bool resolve(const bvr::pattern_scan::ProcessImage& image, Symbols& out);

} // namespace bvr::b2r::patterns
