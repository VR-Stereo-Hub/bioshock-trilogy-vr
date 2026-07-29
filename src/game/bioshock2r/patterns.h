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
// next steps (FOV readback, hands, skeleton, console exec). Nothing consumes
// them at M3, so they stay comment-only until the consuming code lands:
//   UShockUserSettings 0x11523D8   APlayerWeapon    0x112CC78
//   AHands             0x1125478   SkeletonInstance 0x10D0FC0
//   UGameEngine        0x10BD7DC / 0x10BD9E8 (BS1's console_exec used the
//   second of the pair - expect the same here, but verify before calling).

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
