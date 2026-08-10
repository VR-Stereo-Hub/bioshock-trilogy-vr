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

// ---- APlayerController / camera object layout ------------------------------
// Decoded from GetPlayerViewPoint's four internal paths (session 34, offline).
// Session 36 promoted +0x240 from INFERRED to OBSERVED: 40 of 40 live path
// samples found the flag clear and this pointer non-null, i.e. every sample
// took path 2 and read the POV out of the camera object.
//
// These live here rather than in camera.cpp because of the standing rule that
// engine offsets exist in patterns.h and nowhere else.
inline constexpr uint32_t kPcCachedPovFlagOffset = 0x248;  // bit 0: use the cached POV
inline constexpr uint32_t kPcCameraOffset = 0x240;         // lazily-created camera object
inline constexpr uint32_t kPcCachedLocOffset = 0x24C;      // FVector, path 1's source
inline constexpr uint32_t kPcCachedRotOffset = 0x258;      // FRotator, path 1's source
inline constexpr uint32_t kPcViewTransformOffset = 0x430;  // 0x40 bytes -> a 4x4 SSE transform
// The controlled pawn. Session 42's field walk named it by CLASS, live: a
// `bsifields 1F0 8` on the latched PC reports `+0x1FC -> class XHuman` next to
// `+0x200 -> class XPlayerReplicationInfo`, and it re-derived identically after
// a relaunch and a fresh checkpoint load (different addresses, same offsets),
// which is what makes it an offset rather than a coincidence.
inline constexpr uint32_t kPcPawnOffset = 0x1FC;
inline constexpr uint32_t kCameraPovLocOffset = 0x3B8;     // FVector, path 2's source
inline constexpr uint32_t kCameraPovRotOffset = 0x3C4;     // FRotator, path 2's source
// AActor, from paths 3 and 4 reading identical offsets off two different
// objects - which is what made the reading credible rather than a guess.
inline constexpr uint32_t kActorLocationOffset = 0x44;
inline constexpr uint32_t kActorRotationOffset = 0x50;

// ---- The aim seam (session 44, I7) -----------------------------------------
// APawn::GetBaseAimRotation is VIRTUAL, and this is its vtable slot.
//
// DERIVATION (offline, from the exec thunk the s34 native-table census already
// recorded as `execGetBaseAimRotation` at 0x12BF30 with ZERO E8 callers - a
// thunk, never a hook target). Disassembling the thunk shows the whole shape in
// nine instructions: it steps the script frame past the 0x41 opcode, then
//     mov eax, [esi]            ; the object's vtable
//     mov edx, [eax + 0x2E8]    ; <== THIS SLOT
//     lea ecx, [esp+4] ; push   ; the hidden return buffer - ONE stack arg
//     call edx
//     movq xmm0, [eax] / mov eax, [eax+8]   ; copy 12 bytes = an FRotator
// So the C++ signature is `FRotator* __thiscall (FRotator* retBuf)`, one stack
// arg, hence **ret 4** for any probe (the RTC rule in this file's header), and
// the return value is a POINTER to the buffer, not the rotator itself.
//
// A static search for the dispatch pattern `8B 90 E8 02 00 00` finds 8 sites in
// .text and `8B 81 E8 02 00 00` another 8 - the caller census this slot has,
// which is small enough to read by hand. Note the displacement alone does NOT
// prove a vtable dispatch: it must be `load object's vtable` then `call` that
// slot, so classify each site before believing it.
//
// Being virtual is the useful part: the implementation is read off a LIVE pawn
// (`bsivtable 0x<pawn> 4 0x2E8`) rather than needing a static RVA at all, which
// is why no impl RVA is recorded here.
inline constexpr uint32_t kPawnGetBaseAimRotationVtblOffset = 0x2E8;
inline constexpr uint32_t kPawnGetBaseAimRotationRetImm = 4; // ret imm / 4 == 1 arg

// ---- The fire-ORIGIN seam (session 46, I8) ---------------------------------
// AXPawn::XGetWeaponStartTraceLocation - where the weapon trace STARTS. The
// s45b headset findings 2+3 (hole above the dot; bullets leaving the screen
// center) are one defect: this function returns the CAMERA viewpoint while the
// aim dot's ray starts at the HAND - two parallel rays from different origins.
//
// DERIVATION (offline s46, the s44 thunk recipe re-run):
//   1. Native-table dump (2079 rows resolved by the recorded PE-walk recipe)
//      names `AXPawnexecXGetWeaponStartTraceLocation` thunk at 0x4F9430 and
//      `...FloatingLocation` thunk at 0x4F94B0.
//   2. The Location thunk evaluates ONE optional Weapon param and calls THIS
//      implementation directly: `call 0x5344A0` with (FVector* out, Weapon*)
//      pushed - `__thiscall`, 2 stack args, hence **ret 8** (C2 08 00 sits at
//      impl+0x61). It returns the retbuf pointer in eax; the copy-back is 12
//      bytes (a plain FVector).
//   3. The Floating variant's impl (0x53C500) CALLS this one, then appends a
//      4th dword read from pawn+0xBC - so 0x5344A0 is the single choke point
//      for both natives. E8 census: Floating has 13 callers (the C++ fire
//      sites), this inner one has 2 (its own thunk + the Floating wrapper).
//   4. THE BODY IS THE DIAGNOSIS: with a controller it calls
//      controller->vtbl[+0x210] (returns the camera object, called once to
//      null-test and once for the dispatch) and then calls THE EXACT
//      kGetPlayerViewPointRva impl (0x1E10C0) - the function the camera drive
//      already detours. The trace origin IS the (VR-driven) camera eye, read
//      from the disassembly, not inferred. The camera itself never calls
//      either native (it reads GetPlayerViewPoint directly), so a hook here
//      cannot feed back into the view - the s44 collision hazard is
//      structurally absent at this seam.
inline constexpr uint32_t kXGetWeaponStartTraceLocationImplRva = 0x5344A0;
inline constexpr uint8_t kXGetWeaponStartTraceLocationRetImm = 8; // 2 stack args
// First 13 bytes at the impl, pinned as the install gate (same discipline as
// kGetPlayerViewPointPrologue): sub esp,0x24; push esi; mov esi,ecx;
// cmp dword ptr [esi+0x218], 0  (the Controller null-test that opens the body).
inline constexpr uint8_t kXGetWeaponStartTraceLocationPrologue[] = {
    0x83, 0xEC, 0x24, 0x56, 0x8B, 0xF1, 0x83, 0xBE, 0x18, 0x02, 0x00, 0x00, 0x00};

// ---- The SubtleFidget native impl (derived offline s49) ---------------------
// AXFirstPersonAttachment::StartSubtleFidget's C++ IMPLEMENTATION - the single
// choke point every dispatch route reaches (ProcessEvent, the timer executor,
// CallFunction: all execute the UFunction's native thunk at 0x503750, whose
// body is `mov ecx,esi; call 0x51BA00`; C++ callers would call the impl
// directly - E8 census: the thunk is the ONLY direct caller).
//
// DERIVATION (offline s49, zero boots):
//   1. Census string `AXFirstPersonAttachmentexecStartSubtleFidget` at file
//      offset 0xD9793C -> VA 0x119853C; the 8-byte {nameVA, implVA} native-
//      table pair at VA 0x1338C30 names the exec thunk VA 0x903750.
//   2. Thunk disasm: parses one optional bytecode arg, then
//      `mov ecx,esi; call 0x91BA00; ret 8` -> impl VA 0x91BA00 = RVA 0x51BA00,
//      __thiscall, ZERO stack args (plain C3 ret at impl+0xCB).
//   3. THE BODY IS THE SCHEDULER (this is the s48 mystery resolved): gates on
//      [this+0x214]&1 (bDisableSubtleFidget - the s48b-derived bit!) and
//      [this+0x5C]&4, fetches the action player off the mesh component
//      ([this+0x218] -> call 0x7033C0), plays the anim action by name
//      (call 0x5D1520 with the FName pair at [this+0x274]/[+0x278]), then
//      RE-ARMS ITSELF: samples SubtleFidgetTimeRange (lea ecx,[this+0x26C];
//      call 0x3C4970 = random-in-range) and calls 0x249D60 = SetTimer-family
//      (93 callers) with the FName cached in the file-static globals
//      {VA 0x13FEC50 = index, 0x13FEC54 = number} - live-read {2172, 0} =
//      'StartSubtleFidget'. The equip/fire-reset arm site (call at VA
//      0x91B80A) uses the SAME globals and the SAME [this+0x26C] range.
//   4. WHY EVERY s48/s48b FALSIFICATION HAPPENED: the impl checks the
//      INSTANCE bool at +0x214 honestly - but when disabled it STILL re-arms
//      the timer (the disable early-out jumps to the re-arm tail), and the
//      s48b property starves could not reach the timer record already armed;
//      more importantly the clean-boot ProcessEvent filter saw events=1
//      because the timer executor does not route through the attachment's
//      vtable +0x7C slot on this build - the impl below is upstream of ALL of
//      that.
inline constexpr uint32_t kStartSubtleFidgetImplRva = 0x51BA00;
inline constexpr uint32_t kStartSubtleFidgetThunkRva = 0x503750; // record only
// First 10 bytes at the impl, pinned as the install gate: push esi;
// mov esi,ecx; test byte ptr [esi+0x214],1 - the bDisableSubtleFidget test
// doubles as an identity check (the +0x214 offset is the s48b-derived bit).
inline constexpr uint8_t kStartSubtleFidgetImplPrologue[] = {0x56, 0x8B, 0xF1, 0xF6,
                                                             0x86, 0x14, 0x02, 0x00,
                                                             0x00, 0x01};
// Recorded, not consumed: the by-name anim-action player the impl calls
// (12 E8 callers - the next choke rung if a second start lane ever shows),
// its getter off the component, the SetTimer-family entry, and the cached
// FName global pair. The global is NOT poked: the timer fire path resolves
// the name FindFunctionChecked-style (null deref on failure), so a poisoned
// name index is a crash, not a kill.
// s49b: the Morpheme REQUEST layer, decoded from the attachment's
// reset-to-ready function (RVA 0x51B6C0, __thiscall zero-arg, `ret` after
// add esp - the function containing the 0x51B80A SetTimer arm). That body:
// resolves FOUR request descriptors by FName (attachment props +0x27C/+0x284/
// +0x28C/+0x2C4, Number dwords following each) via 0x5D1A20 =
// "resolve-request-descriptor-by-name(nameIdx, nameNum, 1)" on the network
// (23 callers), caches the 16-byte descriptors at attachment +0x294..+0x2E3
// (+ a fifth via 0x5D1CF0 at +0x2F0), validity-tests one at +0x2F8
// (0x5CE120), POSTS it via kPostRequestRva, re-arms the fidget SetTimer,
// clears BIT 0x2 of [attachment+0x214] (a second state bit beside
// bDisableSubtleFidget's 0x1) and zeroes the float at +0x2EC.
//
// kPostRequestRva = THE choke for every request entering any XMorphemeNetwork:
// __thiscall(network, void* desc16, int pri), `ret 8`; reads [network+0x118]
// (the Morpheme runtime) and forwards to the inner post 0x5CED00. Exactly
// EIGHT E8 callers: 0x51B7E5 (the reset above), 0x54B62E, four at
// 0x6C15D6/0x6C1604/0x6C167A/0x6C16A7 (a multi-network state applier that
// posts cached descriptors from a big object's +0x1A60/+0x1A70 into
// [component+0x228] networks), and 0x7DA1EA/0x7DA257 (a state-change poster
// that gates on [obj+0x167C]&2 and zeroes floats at +0x1674/+0x1680 first).
// The ASCII "rqHandFidget" is NOT in the exe - request names are asset data,
// resolved by FName at runtime.
inline constexpr uint32_t kPostRequestRva = 0x5CEF00; // wrapper - record only
// s49b correction: 0x5CEF00 is only ONE of the post wrappers (a live fire
// posted NOTHING through it). The FUNNEL is the inner post 0x5CED00 - all
// five E8 callers converge there: 0x5CEEE6 (wrapper0), 0x5CEF38 (wrapper1 =
// kPostRequestRva's body), 0x5CEF88 (wrapper2 0x5CEF50, float arg, 40
// callers - the attachment's 0x51B8AC/0x51B9DE sites among them), 0x5D0407,
// and 0x5D1B2A (inside the by-name resolver). Shape: __cdecl(runtime =
// [network+0x118], desc*, params*), plain-ret (caller cleans, add esp 0xC at
// the wrapper), jump table on the descriptor TYPE byte [desc+0xC] (0..4),
// request-id WORD at [desc+8] (0xFFFF = invalid, early-out).
inline constexpr uint32_t kPostRequestInnerRva = 0x5CED00;
// The float wrapper (wrapper2): __thiscall(network)(desc16*, float), ret 8 -
// the form the game uses for float control params (40 callers). Consumed by
// the manual poster `bsifidget post`.
inline constexpr uint32_t kPostRequestFloatRva = 0x5CEF50;
inline constexpr uint8_t kPostRequestInnerPrologue[] = {0x55, 0x8B, 0xEC, 0x83,
                                                        0xE4, 0xF0, 0x8B, 0x4D,
                                                        0x08, 0x8B, 0x45, 0x10};
inline constexpr uint32_t kResetSubtleFidgetRva = 0x51B6C0; // the engine's own
// reset-to-ready - recorded as the Plan-B pin (callable on the attachment when
// the +0x214 bit-0x2 flip marks an onset; posts the ready request through the
// engine's own path).
inline constexpr uint32_t kPlayAnimActionByNameRva = 0x5D1520;
// s49 second-instrument derivation: the "action player" the impl fetches IS
// the runtime XMorphemeNetwork - the getter at kGetAnimActionPlayerRva reads
// [component+0x228] (the s45b-mapped runtime network slot) and IsA-gates it.
// So 0x5D1520 is the network's play-anim-action-by-name entry: __thiscall,
// FIVE stack args (nameIndex, nameNumber, 0, float blend, 0), SEH-framed
// prologue, `ret 0x14` at +0x99. 12 E8 callers total - the by-name choke
// point for EVERY scripted anim action entering the FP network.
inline constexpr uint8_t kPlayAnimActionByNameRetImm = 0x14; // 5 stack args
inline constexpr uint8_t kPlayAnimActionByNamePrologue[] = {
    0x6A, 0xFF, 0x68, 0x58, 0xEB, 0x04, 0x01, 0x64, 0xA1, 0x00, 0x00, 0x00, 0x00};
inline constexpr uint32_t kGetAnimActionPlayerRva = 0x7033C0;
inline constexpr uint32_t kSetTimerFamilyRva = 0x249D60;
inline constexpr uint32_t kFidgetTimerFNameGlobalRva = 0xFFEC50;

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
// produces the appErrorf above). Consumed by `bsicall` (session 37): the slot
// is the locator, the RVA below is the interlock.
inline constexpr uint32_t kFindFunctionVtableOffset = 0x54;
// UObject::FindFunction implementation RVA, derived twice in agreement:
// (a) offline session 36 - FindFunctionChecked's body dispatches through
// [vtable+0x54], and the function immediately preceding FFC's 0xD1090 in .text
// starts at 0xD1030; (b) live session 36 - slot +0x54 of a latched
// APlayerController's vtable read 0xD1030. `bsicall` refuses to dispatch when
// the live slot disagrees with this, so a re-linked build cannot be called
// through a slot that now means something else.
inline constexpr uint32_t kFindFunctionRva = 0xD1030;

// BOTH are thiscall with 3 stack args, `ret 0xC`, confirmed as the only ret imm
// in each body (ProcessEvent 672 bytes, FindFunctionChecked 304 bytes, both
// int3-padded to the next function). 0xC/4 == 3 is the argument count any
// detour on either MUST declare, or the RTC dialog that writes no crash dump
// is the result.
inline constexpr uint8_t kProcessEventRetImm = 0x0C;
inline constexpr uint8_t kFindFunctionCheckedRetImm = 0x0C;

// ---- XInput IAT slot (derived offline session 34, consumed session 42) ------
// BioShockInfinite.exe imports XINPUT1_3.dll by ORDINAL: two entries, ord 1
// (caps) and ord 2 (XInputGetState), parsed from the PE import directory in
// the s34 recon (ENGINE_NOTES "PE identity"). The ord-2 IAT slot sits at RVA
// 0xCD4814. The s34 live hook census read the slot UNHOOKED - Steam's overlay
// E9-hooks the export THUNK instead, which is exactly why re-pointing this
// last hop composes synthetic pad state without fighting Steam (BS1/BS2's
// measured mechanism; see core/input/xinput_bridge.cpp:hijack_import_slot).
// input_drive.cpp verifies the slot's CURRENT target resolves into a loaded
// module before consuming it, and refuses the hijack otherwise.
inline constexpr uint32_t kXInputGetStateIatRva = 0xCD4814;

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

// ---- the lens (DR-I3, derived live session 36) -----------------------------
//
// Infinite does NOT carry BS1's and BS2's 7-float screen-ray helper
// (2tanH, 0, -tanH, 0, 0, -2tanV, tanV). It ships a 4x4, so core's
// hud::set_ray_block_offset / decode_ray_block CANNOT consume this and the
// adapter deliberately publishes nothing to them - a Vengeance-shaped decoder
// pointed at float 0 would either fail or, worse, false-positive.
//
// DERIVATION. `dumpframe cb` (frame_inspector mode 3) captured 1891 constant-
// buffer uploads in gameplay. Sweeping every offset of every block for a 4x4
// whose three columns are mutually orthogonal yields 139 candidates; filtering
// on `tanH/tanV == backbuffer aspect` leaves exactly ONE. That filter is
// load-bearing, not decoration: the top four candidates by block count are all
// degenerate tanH==tanV pairs, and the best of them had MORE support (108
// blocks) than the true answer (93). Plurality is not evidence here.
//
// CONFIRMED BY A FALSIFIABLE PREDICTION, which is what promotes it from
// candidate to fact. Moving the in-game FOV slider from minimum to maximum
// moved this block and nothing else about it:
//     slider min:  tanH 0.7674  tanV 0.4317  hFOV 75.01 deg  aspect 1.77762
//     slider max:  tanH 0.8770  tanV 0.4933  hFOV 82.50 deg  aspect 1.77782
// Both axes scaled by the SAME ratio (1.14282 vs 1.14269) with the aspect held
// at 16:9 to 0.002 % - the signature of a FOV change and of nothing else.
//
// tanH = |c3| / |c0| and tanV = |c3| / |c1| for the column triples of the
// row-major 4x4, so the object scale cancels. That is what makes this readable
// from a PER-OBJECT constant buffer, where nothing else is constant.
inline constexpr uint32_t kLensCbBytes = 80;      // the tier that carries it
inline constexpr uint32_t kLensFloatIndex = 0;    // float offset within it
inline constexpr bool kLensRowMajor = true;

// THE FOV LAW (session 37 aspect cross-check, ENGINE_NOTES "The FOV law"):
// the option is VERTICAL-referenced and tanH = tanV x aspect (Hor+). Slider
// min pins tanV at 0.4317 at BOTH tested aspects (16:9 and 4:3), slider max
// at 0.4933; vFOV 46.67..52.63 deg at any aspect. These are the frustum's own
// numbers (the ini's FOVAngle is decorative - see the warning below). I5's
// projection claim is 2*atan(tanV x aspect); the default assumes the slider
// at MINIMUM (the shipped default), correctable live via `bsifov tanv` and
// verifiable against a `dumpframe cb` decode.
inline constexpr float kTanVSliderMin = 0.4317f;
inline constexpr float kTanVSliderMax = 0.4933f;

// ---- the live FOV chain on the camera object (session 41, derived live) ----
//
// DERIVATION (poke/rescan at the attract, ENGINE_NOTES "LIVE RESULTS (session
// 41)"). With the in-game slider at max the frustum decodes tanH 0.8770 and
// the camera object [pc+0x240] carries 82.50f - and tan(82.5/2) = 0.8770
// EXACTLY, so the stored value is the horizontal FOV in degrees at the current
// aspect. Copies live at [cam+0x214] (followed by two 1.3333f floats - the
// UE3 ACamera DefaultFOV / DefaultAspectRatio shape) and [cam+0x3D0] (the
// cached POV: loc at +0x3B8, rot at +0x3C4, fov at +0x3D0 - the FTPOV
// layout). A full writable-memory scan for 82.5f found SIX holders and every
// single one snapped back to 82.5 within a tick of being poked (including
// these two), so the value is RECOMPUTED each tick from the option upstream:
// no memory address is the source, and the console `set` lane never writes
// XUserOptionsManager.FieldOfView at all (a scan for the written value found
// zero stable holders - recorded negative, do not re-try `set` here).
// THE LEVER therefore ENFORCES per camera-detour dispatch: writing both
// copies every GetPlayerViewPoint call outruns the once-per-tick refresh,
// and disarming self-restores - the engine's own recompute is the undo.
inline constexpr uint32_t kCameraDefaultFovOffset = 0x214; // f32 deg, [cam+...]
inline constexpr uint32_t kCameraPovFovOffset = 0x3D0;     // f32 deg, [cam+...]
// The camera degrees value is HORIZONTAL AT A FIXED 16:9 REFERENCE, not at
// the current aspect: the engine pins tanV = tan(deg/2) / (16/9) and derives
// tanH = tanV x actual aspect (Hor+ - the s37 law restated with its anchor).
// MEASURED at 1440x1440 with the lever enforcing 100 deg: both decoders read
// tanH = tanV = 0.6704 = tan(50 deg)/1.7778 exactly (predicting tan(50) =
// 1.19175 from a current-aspect reading was WRONG and the claim audit caught
// it at 43.7% off - session 41). At 16:9 the two readings coincide, which is
// why the 16:9-only measurements could not separate them.
inline constexpr float kFovRefAspect = 16.0f / 9.0f;

// ---- the scene-build root (session 40, derived live) -----------------------
//
// The SequentialReentry seam: the UGameViewportClient::Draw analog. DERIVATION
// (three independent live routes agreeing, ENGINE_NOTES session 40):
//  1. Caller census at the GetPlayerViewPoint detour: return RVA 0x26B499
//     fires EXACTLY once per present (810 in 810 presents) on the game thread,
//     inside a loop that walks a linked list ([esi+0x208]) doing the
//     constant-time UClass-interval IsA check before sampling each player
//     controller's view - the per-frame scene camera sweep.
//  2. One-shot RtlCaptureStackBackTrace from that caller: dispatched from
//     return RVA 0x1FE05F, whose site is `mov ecx,[viewport+0x1C];
//     mov edx,[ecx]; mov edx,[edx+8]; call edx` bracketed by a stack canvas's
//     ctor (0x331110) / dtor (0x3339F0) - the engine's per-tick redraw.
//  3. One-shot live stack probe at that dispatch (`bsicam scenedraw`):
//     client [viewport+0x1C] -> vtable in .rdata at 0xDE6FC8 -> slot 2
//     (byte +0x8) = 0x6F1360, a `jmp 0x26A3E0` stub. Entry confirmed by
//     prologue disassembly; body spans the census call site 0x26B494.
//
// __thiscall, ecx = the viewport client, TWO stack args (viewport, canvas),
// `ret 8`. NOT the doubling root: doubling this function was TRIED first
// (session 40) and produced camera+scene doubling with NO second present -
// the tag ring skewed +1 per tick (self-healed at depth 3) because the
// present is kicked by the CALLER's tail, not by this call tree. Kept as
// derivation facts and for the install-time chain cross-check.
inline constexpr uint32_t kSceneDrawRva = 0x26A3E0;       // client Draw body
inline constexpr uint32_t kSceneDrawStubRva = 0x6F1360;   // vtable slot target (jmp stub)
inline constexpr uint32_t kSceneDrawVtableRva = 0xDE6FC8; // .rdata, slot 2 = +0x8
// The `call edx` return address of the client-Draw dispatch (inside the
// viewport draw root below) - the camera-side scene probe keys on it.
inline constexpr uint32_t kSceneDispatchRetRva = 0x1FE05F;
// The census fact, for reference: the GetPlayerViewPoint call site inside the
// scene draw's controller loop.
inline constexpr uint32_t kSceneGpvpCallSiteRetRva = 0x26B499;

// THE DOUBLING ROOT: the viewport draw - FViewport::Draw(bShouldPresent)'s
// analog, one level ABOVE the client draw. Its body is: stack canvas ctor
// (0x331110) -> client->Draw dispatch (ret 0x1FE05F) -> canvas dtor
// (0x3339F0) -> the present kick (call 0x1E50B0 with arg 1). Doubling THIS
// yields camera + scene + present per eye, which is what the SR tag ring
// requires (one present per pushed eye tag). Entry resolved by walking the
// live stack scrape one frame above the dispatch: ret 0x206309's E8 call
// targets 0x1FDE30 directly, and the forward stream from 0x1FDE30 reaches
// the dispatch at 0x1FE05D (capstone). __thiscall, ecx = the viewport, ONE
// stack arg, `ret 4` (at RVA 0x1FE0AB).
inline constexpr uint32_t kViewportDrawRva = 0x1FDE30;
inline constexpr uint8_t kViewportDrawPrologue[] = {0x6A, 0xFF, 0x68, 0x36, 0xCE, 0x02,
                                                    0x01, 0x64, 0xA1, 0x00, 0x00, 0x00, 0x00};
inline constexpr uint8_t kViewportDrawRetImm = 4;
// The gameplay caller's return RVA (pe-xref: 4 static callers - 0x118086,
// 0x1FFD5C, 0x2017CA, 0x206304; the live census names 0x206309, the return
// of 0x206304's call, as the per-tick dispatcher). Pass 2 is deny-by-default
// on this value, jointly with the camera-silent and present-stall gates
// (BS2's three-gate design).
inline constexpr uint32_t kViewportDrawGameplayRetRva = 0x206309;

// ---- The first-person rig (session 45b, I8 - derived LIVE) ------------------
//
// The chain, every link measured on a live TWN2 save (ENGINE_NOTES "I8 - the
// first-person rig, derived live"):
//   PC +0x1FC -> XHuman pawn
//   pawn.GetFirstPersonAttachment() [native, by-name dispatch] ->
//     XFirstPersonAttachment (an ACTOR: Level +0x14, owner XHuman +0x8C)
//   attachment +0x218 -> XSkeletalMeshComponent  <== THE VIEWMODEL RENDERER
// Proven by intervention, not inference: HideBoneByName(PlayerHandsChest) on
// that component removed BOTH hands AND the equipped pistol from the frame;
// UnHide restored them. Hiding only R_Grip removed the right hand AND the
// pistol while the forearm stayed - so the weapon rides the grip subtree and
// a grip-cluster drive carries the holdable for free.
inline constexpr uint32_t kFpAttachMeshCompOffset = 0x218;

// XSkeletalMeshComponent layout (typed-dump derivation, cross-checked against
// the engine's own natives):
//   +0x000 native vtable (identity gate below)
//   +0x014 Outer / +0x038 Owner  == the XFirstPersonAttachment
//   +0x060 LocalToWorld FMatrix, ROW-VECTOR convention (world = local * M);
//          rows 0-2 orthonormal rotation images, row 3 = world translation.
//          Verified: L2W(SpaceBases[L_Grip].trans) == GetBoneLocation(L_Grip)
//          to 0.1/0.4/0.0 UU on a frozen-coherent snapshot.
//   +0x21C SkeletalMesh   +0x224/+0x228/+0x244 XMorphemeNetwork   +0x274 PhysicsAsset
//   +0x290 SpaceBases  TArray<FBoneAtom> (COMPONENT space - the render feed;
//          the winner of the GetBoneLocation cross-check above)
//   +0x29C LocalAtoms  TArray<FBoneAtom> (parent space - loses the same check
//          by >100 UU)
//   +0x2DC RequiredBones-shaped TArray<BYTE> (identity map 0..42 live)
// FBoneAtom is 32 bytes: quat xyzw at +0x00, translation xyz at +0x10,
// UNIFORM SCALE (one float) at +0x1C. NOT the Vengeance 48-byte hkQsTransform.
inline constexpr uint32_t kSkelCompVtableRva = 0xDEA648;
inline constexpr uint32_t kSkelCompOuterOffset = 0x14;
inline constexpr uint32_t kSkelCompOwnerOffset = 0x38;
inline constexpr uint32_t kSkelCompLocalToWorldOffset = 0x60;
inline constexpr uint32_t kSkelCompSkelMeshOffset = 0x21C;
inline constexpr uint32_t kSkelCompSpaceBasesOffset = 0x290;
inline constexpr uint32_t kSkelCompLocalAtomsOffset = 0x29C;
inline constexpr uint32_t kBoneAtomStride = 0x20;
inline constexpr uint32_t kBoneAtomQuatOffset = 0x00;
inline constexpr uint32_t kBoneAtomTransOffset = 0x10;
inline constexpr uint32_t kBoneAtomScaleOffset = 0x1C;

// USkeletalMesh::RefSkeleton at +0x74: TArray<FMeshBone>, stride 0x50.
// Element: FName {Index +0x00, Number +0x04}, authored quat +0x10, authored
// pos +0x20, NumChildren +0x44, ParentIndex +0x48. Identified live: element 0
// named PlayerHandsChest (root, 4 children), element 1 L_Grip, element 22
// R_Grip - matching MatchRefBone/GetBoneName exactly. The FP arms rig has 43
// bones; grip subtrees are the hand clusters, PlayerHands[LR]arm* the arm
// chains. Names/indices are MESH facts (re-read per resolve, never baked).
inline constexpr uint32_t kSkelMeshRefSkeletonOffset = 0x74;
inline constexpr uint32_t kMeshBoneStride = 0x50;
inline constexpr uint32_t kMeshBoneNameOffset = 0x00;
inline constexpr uint32_t kMeshBoneParentOffset = 0x48;

// THE RESTAMP FACTS the drive design rests on (poke oracles, s45b):
//   - SpaceBases translations restamp within <2 s even with the world
//     auto-paused (Morpheme evaluates continuously) - a one-shot write cannot
//     survive; the drive writes EVERY pass-1 dispatch and repaints pass 2.
//   - SpaceBases SCALE restamps too (poked 0.3 -> back to 1.0 inside a
//     second). The BS2 rule "the engine never restamps scale" DOES NOT HOLD
//     here; adoption takes whole atoms and a stale zero-scale self-heals.
//   - LocalAtoms restamps identically; it is the anim source, not our target.

// ---- THE FX-ORIGIN SEAM: the attachment updater (s50, I8) -------------------
//
// XSkeletalMeshComponent's per-tick ATTACHMENT UPDATER - the function that
// positions every attached child component (the vigor charge plume, the ready
// sparkle, pooled muzzle/tracer emitters) from SpaceBases. Derivation
// (ENGINE_NOTES "s50: THE FX-ORIGIN SEAM"):
//   1. Offline capstone sweep of .text for functions addressing BOTH
//      SpaceBases (+0x290 Data / +0x294 Num) AND LocalToWorld (+0x60):
//      102 candidates.
//   2. Intersected with the 125 .text entries of the class vtable
//      (kSkelCompVtableRva): TWO survive - 0x2A1B20 (slot 43) and 0x2A2130
//      (slot 115). Slot 115 takes a stack arg and MODIFIES the Attachments
//      array (attach/detach management); slot 43 is the pure per-tick walker.
//   3. 0x2A1B20 disassembly: loops the Attachments TArray at +0x1F0/+0x1F4
//      (stride 0x30: +0x00 child Component, +0x04/+0x08 BoneName FName,
//      +0x0C RelLoc, +0x18 RelRot, +0x24 RelScale), resolves the bone via the
//      mesh (+0x21C), bounds-checks vs SpaceBases Num, reads the atom at
//      Data + idx*0x20, builds the child transform. Plain `ret` (zero stack
//      args), epilogue 8B E5 5D C3.
// The flat mechanism proof (s50): drive OFF -> the charge flame sits exactly
// on the authored hand; drive ON -> the hand moves, the flame keeps the
// authored transform. The engine positions attachments from TICK-TIME
// SpaceBases; the render-side compose never reaches them.
inline constexpr uint32_t kSkelCompUpdateAttachmentsRva = 0x2A1B20;
inline constexpr uint32_t kSkelCompUpdateAttachmentsVtableSlot = 43;
inline constexpr uint32_t kSkelCompAttachmentsOffset = 0x1F0;
// push ebp; mov ebp,esp; and esp,-0x10; sub esp,0x114; (xorps...)
inline constexpr uint8_t kSkelCompUpdateAttachmentsPrologue[] = {
    0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0, 0x81, 0xEC, 0x14, 0x01, 0x00, 0x00, 0x0F};

// ---- THE EFFECT-UPDATE SEAM: per-record playback update (s50, I8) ----------
//
// The frozen-FX family's per-frame position feed. Derivation (ENGINE_NOTES
// "s50", part 2): XEffectPlaybackManagerTickHelper's tick-interface thunk
// (helper vtable slot 36: fld [esp+4]; this -= 0x28; jmp) leads to the real
// tick at rva 0x436490, which iterates an active-effect table (stride 0x74,
// [tickee+0x3C]/[tickee+0x40]) and calls THIS function once per live record:
//   this  = the effect's component            (record+0x68)
//   arg1  = the record                        (push esi)
//   arg2  = &record.location (FVector)        (lea +0x18)
//   arg3  = &record.rotation                  (lea +0x28)
//   args 4..13 = record fields +0x3C..+0x65 and a literal 0
// `ret 0x34` -> 13 stack args (the RTC rule). The location buffer arrives BY
// POINTER, so a detour can rewrite it before the original consumes it - the
// same call-original-substitute-the-buffer shape as fire.cpp, one level up.
inline constexpr uint32_t kEffectUpdateRva = 0x3EC4C0;
inline constexpr uint8_t kEffectUpdateRetImm = 0x34;
// push -1; push 0x103d6c0 (EH frame; absolute is stable - fixed image base
// behind the build gate); mov eax, fs:[0]
inline constexpr uint8_t kEffectUpdatePrologue[] = {
    0x6A, 0xFF, 0x68, 0xC0, 0xD6, 0x03, 0x01, 0x64, 0xA1, 0x00, 0x00, 0x00, 0x00};

// **A CONFIG VALUE IS A CLAIM, NOT A MEASUREMENT.** `XEngine.ini` says
// `FOVAngle=70` and `MaxUserFOVOffsetPercent=15`, which session 34 read as "the
// native slider spans roughly 70 to 80.5 degrees". The RENDERED frustum spans
// **75.01 to 82.50 degrees horizontal** at 16:9. Neither endpoint matches, and
// the tangent ratio across the slider is 1.1428 rather than the 1.2094 that
// 70-to-80.5 predicts. Do not build I5's FOV law on the ini numbers; derive it
// from the frustum, and at two aspects rather than one.

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
