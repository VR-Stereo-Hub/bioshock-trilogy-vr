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

// --- render substrate (session 26) -------------------------------------------
// Derived by the live kick/kick2 samplers + offline capstone walks; full
// derivation chains in docs/bioshock2/ENGINE_NOTES.md "Scene-draw
// architecture (session 26)". Headlines:
// - The scene build root is UGameEngine::Draw: vtable slot +0x118 of the
//   session-24 RTTI candidate 0x10BD7DC holds jmp stub 0x139D -> body
//   0x4EE8D0 (aligned-stack + SEH prologue, ret 0x10, this = the engine,
//   render-command ring cursors at this+0x118/+0x11C). ZERO static E8
//   callers - virtually dispatched; the gameplay caller ret RVA comes from
//   the live caller census (deny-by-default gate data for pass 2).
// - Draw reads its camera from the viewport's camera actor at the head:
//   [[arg1+0x48] + 0x1EC..0x1F4] = loc floats, [+0x1F8..0x200] = rot ints -
//   written earlier by the tick-side PlayerCalcView dispatch (fn 0x4D1080,
//   references the documented FName index global 0x17D9A08). CalcView does
//   NOT run inside Draw on BS2 (BS1 difference!) - a doubled Draw needs the
//   cached fields poked, not a CalcView replay.
// - 0x5C7C80 is NOT a frame submit despite wearing BS1's submit shape
//   (TLS frame-id spin-wait, camera globals, ret 0xC, tail event kick): its
//   `this` is the FContentStreamingManager global and the camera globals it
//   fills are the streaming view info. Hooked as telemetry only.
// - The two once-per-present SetEvent threads are the Flash (gameswf) and
//   FMOD lock-step runnables (FThreadLockStepRunnable::Run = 0x67DAD0,
//   shared vtable slot 1); the hw-thread quotient helper 0x67DAB0 gates the
//   Flash kick: threaded iff [0x149760C] / [0x1497798] > 1 (BS1's quotient
//   family, one out-of-line copy).
// Session-25 recon CORRECTIONS: 0xB81020 (stub 0xF01A) = thread
// Suspend/Resume; 0xCDB917 (stub 0xD9BD) = crit-section once-init helper;
// FF15 SetEvent site 0xCDB9BB = CRT once-init machinery; 0xCF3EE0 =
// CreateSemaphoreW object ctor (not a pump); 0x7C3E40 = WFSO(INFINITE)
// helper over the handle pair at 0x1803A14, callers 0x7A3FD6/0x7A4563
// (not the render queue).

// UGameEngine::Draw - THE SequentialReentry seam candidate.
constexpr uint32_t kGameEngineVtableRva = 0x10BD7DC; // RTTI walk s24, consumed s26
constexpr uint32_t kDrawVtblByteOffset = 0x118;      // slot holding stub -> Draw
constexpr uint32_t kSceneBuildRva = 0x4EE8D0;        // UGameEngine::Draw body
constexpr uint8_t kSceneBuildPrologue[] = {0x53, 0x8B, 0xDC, 0x83, 0xEC,
                                           0x08, 0x83, 0xE4, 0xF0};
// push ebx; mov ebx,esp; sub esp,8; and esp,-16 (aligned-stack MSVC frame,
// BS1's build prologue family; SEH frame follows)

// Draw's camera source (the pass-2 poke target).
constexpr uint32_t kViewportCamActorOffset = 0x48;
constexpr uint32_t kCamActorLocOffset = 0x1EC; // 3 floats
constexpr uint32_t kCamActorRotOffset = 0x1F8; // 3 int32 rotator units

// FContentStreamingManager view hand-off (telemetry hook only).
constexpr uint32_t kStreamViewRva = 0x5C7C80;
constexpr uint8_t kStreamViewPrologue[] = {0x55, 0x8B, 0xEC, 0x64, 0xA1,
                                           0x2C, 0x00, 0x00, 0x00};
// push ebp; mov ebp,esp; mov eax,fs:[0x2C]

// FEventWin::Trigger (vtable 0x11E4FAC slot 2): `push [ecx+4]; call
// [IAT SetEvent]; ret` - handle at object+4. The kick2 sampler hooks it.
// Opcode bytes only: the FF15 operand embeds the ASLR-rebased IAT VA.
constexpr uint32_t kEventTriggerRva = 0xB81050;
constexpr uint8_t kEventTriggerPrologue[] = {0xFF, 0x71, 0x04, 0xFF, 0x15};

// Globals (RVAs; live-verified via hexdump against the running game):
constexpr uint32_t kDrawCounterRva = 0x17D7D2C;     // inc at Draw head (~presents/s)
constexpr uint32_t kStreamMgrGlobalRva = 0x17F5D54; // FContentStreamingManager*
constexpr uint32_t kStreamCamLocRva = 0x17F5D7C;    // 3 floats, == live camera
constexpr uint32_t kStreamFrameIdARva = 0x17F5D8C;  // high bit = done (BS1 shape)
constexpr uint32_t kStreamCamRotRva = 0x17F5D90;    // 3 int32, == live camera
constexpr uint32_t kStreamFrameIdBRva = 0x17F5DA0;
constexpr uint32_t kHwThreadsRva = 0x149760C;       // quotient numerator
constexpr uint32_t kThreadDivisorRva = 0x1497798;   // quotient divisor
constexpr uint32_t kFlashTaskGlobalRva = 0x17F7794; // Flash lock-step task

// Static build-identity gate for the Draw hook: image[kGameEngineVtableRva +
// kDrawVtblByteOffset] must hold imageBase + a jmp stub that lands on
// kSceneBuildRva, whose bytes match kSceneBuildPrologue. Pure image reads -
// refuses the hook on any mismatch (a wrong candidate names itself).
bool verify_draw_chain(const bvr::pattern_scan::ProcessImage& image);

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
