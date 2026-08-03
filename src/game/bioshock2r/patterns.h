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
//   written by the PlayerCalcView dispatch (fn 0x4D1080, references the
//   documented FName index global 0x17D9A08). CalcView runs EXACTLY once
//   inside every Draw (live-verified calcIn == draws every beat - see
//   kViewportCamActorOffset below), so pass 2 replays CalcView per eye; an
//   earlier session-26 reading of this comment claimed the opposite and is
//   corrected here.
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

// Draw's camera source probe (telemetry; the live camera enters via the
// CalcView dispatch that runs EXACTLY once inside every Draw - live-verified
// calcIn == draws every beat, so the BS1 pass-2 replay design transfers).
constexpr uint32_t kViewportCamActorOffset = 0x48;
constexpr uint32_t kCamActorLocOffset = 0x1EC; // 3 floats
constexpr uint32_t kCamActorRotOffset = 0x1F8; // 3 int32 rotator units

// The ONE gameplay Draw caller (live census 2026-07-29: single ret RVA over
// every beat, count == presents). It is the little virtual-dispatch fn at
// 0xCD5D60 (`call [vtbl+0x118]`, ret 4), itself called from the viewport
// iterator at 0xCD2C40. Pass 2 doubles ONLY Draws arriving from here -
// deny-by-default: any other caller (load paths included) is skipped and
// counted, so doubling can never run inside a loader.
constexpr uint32_t kSceneBuildGameplayRetRva = 0xCD5D7B;

// Float index of the screen-ray helper inside the scene VS cb0 (session 32).
// BS1's is 12; this is BS2's OWN, derived fresh - the never-copy rule covers cb
// layouts exactly as it covers addresses. Same SHAPE as BS1's
// (2tanH, 0, -tanH, 0, 0, -2tanV, tanV), four floats later.
//
// DERIVATION: `dumpframe full` in gameplay at 1920x1080, then
// `tools/decode-framedump.ps1 -ScanLayout`, which validates the structural
// signature (the three zero slots plus both pair checks) at every offset of
// every captured block. It matched at exactly ONE offset - 16 - across 249
// blocks, yielding tanH=1.1918 (= tan(50), the option-100 horizontal) for the
// world cluster. The same scan over a BS1 dump finds only offset 12, so the
// test is specific, not loose. Cross-checked live: the core watch's
// self-correcting hunt independently reported "ray block decodes at float 16".
//
// CAVEAT worth keeping: this validates at 16:9. At a 2048x2048 backbuffer BS2
// letterboxes the scene into 2048x1421 and the block's two vertical encodings
// stop agreeing (-f[21]/2 = 0.9662 vs f[22] = 0.6704), so the structural check
// REJECTS it there. That is the engine's projection degenerating off 16:9, not
// a wrong offset - see ENGINE_NOTES "BS2 does not render non-16:9 cleanly".
constexpr int kRayBlockCb0FloatIndex = 16;

// ---- THE FIRST-PERSON RIG (the Big Daddy helmet), session 34 ----------------
// DrawIndexed index count of the helmet's PORTHOLE RING - the mesh that
// surrounds the view and takes most of it once the viewmodel lens is wide.
//
// Derived 2026-07-31 from the FOREGROUND cluster of a full frame dump
// (`dumpframe full` + `decode-framedump.ps1 -RayOffset 16 -FgBakeRvas @()
// -ShowDraws 20`), which lists nine meshes: 3810, 3477, 23766, 12366, 10512,
// 2778 (10512 and 2778 twice each) and 90. CONFIRMED THE ONLY WAY THAT WORKS -
// by making it disappear. `reentry rig skip <n>` + `rig hide` + a flat
// screenshot, one candidate at a time:
//
//   23766, 12366, 10512  removed nothing visible (occluded or depth-only)
//   all nine together    removed the porthole AND the drill - so the ring IS
//                        in this pass, which is what justified bisecting
//   {3810, 3477}         removed the porthole, KEPT the drill
//   3477 alone           porthole intact
//   3810 alone           porthole GONE, drill kept, world fills the frame  <--
//
// Size is NOT the clue: the ring is 3810 indices while three larger meshes in
// the same pass are invisible. Guessing "biggest mesh = the thing filling the
// screen" was wrong on the first try, and it is exactly the draw-count
// inference BS1's rule forbids.
//
// Why an index count at all: inside one pass nothing else separates two meshes.
// The helmet and the weapon share the lens, the render target and the callstack
// (session 33 retracted `0xAECACF` as a foreground signature for that reason).
// The count is a property of the mesh's geometry, so it is stable across
// frames, positions and FOV values.
//
// This is a BS2 number for the BS2 Delta rig. Derive fresh, never copy.
constexpr uint32_t kRigMeshIndexCounts[] = {3810};
constexpr uint32_t kRigMeshCount = sizeof(kRigMeshIndexCounts) / sizeof(kRigMeshIndexCounts[0]);

// Render-thread sync pair (banked, unconsumed): the endframe fn 0x501EA0
// triggers FEventWin global [0x1A69294] once per present (kick2 site
// 0x5029BA) and reads its sibling [0x1A69298]; static readers of the pair
// include 0xB929F2 (render-thread loop candidate). Threaded-mode doubling
// DID prove unstable (the vrstereo freeze), but the 1t route is the flush
// point below (session 35), not this pair - kept for the record.

// --- THE RENDER FLUSH POINT (sessions 35-36) - the vrstereo freeze chain ----
// UGameEngine::Draw's tail makes exactly ONE static call to a render flush
// point - the structural twin of BS1's 0x61D260, veto for veto. The SHAPE
// transferred; every number below was derived fresh on this build with
// tools/disasm-rva.py (session 35) and re-verified against the exe
// 2026-08-02. Full derivation recipes: docs/bioshock2/ENGINE_NOTES.md
// "The render flush point".
//
// Draw tail: mov ecx,[base+kRenderMgrGlobalRva] at 0x4EF493, then a call
// (E8) at kFlushCallSiteRva -> link thunk kFlushThunkRva -> body
// kFlushPointRva. `ret 8` (2 stack args: scene, view group), ecx = mgr.
// Decision chain: seven vetoes that each select INLINE, then threaded iff
// [kHwThreadsRva] / [kThreadDivisorRva] > 1.
//   INLINE branch:   call thunk 0xE29B -> the drain (kDrainRva), then
//                    pop esi; pop ebp; ret 8 - NOTHING AFTER, the property
//                    that makes forcing this branch lossless (as on BS1).
//   THREADED branch: mov ecx,[mgr+4]; call thunk 0x1FBF9 -> kGateWaitRva,
//                    ret site kFlushThreadedRetRva. kGateWaitRva is
//                    `cmp [esi+8],0; jne skip; Wait(INFINITE)` - the latch-
//                    test-then-wait whose lost wakeup IS the vrstereo
//                    freeze (live-confirmed 2026-08-02: the wedged second
//                    draw's stack reads B8108F BB1963 69FD33 4EF4A6).
// NEVER poke kHwThreadsRva to force the inline branch: BS1's equivalent
// poke crashed a loader thread (other quotient consumers see a lie).
// Hooking the flush point instead is why BS1 survives load crossings.
constexpr uint32_t kFlushCallSiteRva = 0x4EF4A1; // E8 in Draw's tail, ret 0x4EF4A6
constexpr uint32_t kFlushThunkRva = 0x24A28;     // E9 link thunk
constexpr uint32_t kFlushPointRva = 0x69FC30;
constexpr uint8_t kFlushPointPrologue[] = {0x55, 0x8B, 0xEC, 0x8B, 0x55, 0x0C,
                                           0x8B, 0x45, 0x08, 0x56, 0x8B, 0xF1};
// push ebp; mov ebp,esp; mov edx,[ebp+0xC]; mov eax,[ebp+8]; push esi; mov esi,ecx
constexpr uint32_t kFlushThreadedRetRva = 0x69FD33; // ret site of the gate-wait call
constexpr uint32_t kGateWaitRva = 0xBB1950;         // latch-test-then-Wait(INFINITE)
constexpr uint32_t kEventWaitWrapperRva = 0xB8108F; // FEventWin Wait ret (vtbl +0x14)
// The render manager the flush point writes (mgr is also the drain's `this`;
// kMgrSceneSlotOffset is the slot the drain loads with no null check).
constexpr uint32_t kRenderMgrGlobalRva = 0x17DBF4C; // read into ecx at 0x4EF493
constexpr uint32_t kMgrSceneSlotOffset = 0x24;      // arg1 (scene) stored here
constexpr uint32_t kMgrViewGroupOffset = 0x28;      // arg2 copied to 0x28..0x5C
constexpr uint32_t kMgrViewGroupDwords = 14;
constexpr uint32_t kMgrThreadedFlagOffset = 0x60;   // threaded stamp (0 = inline)
constexpr uint32_t kMgrFlushSeenOffset = 0x64;      // flush-seen stamp (write 1)
// The drain - the inline branch's whole body. After its SEH frame it loads
// the scene from [this+kMgrSceneSlotOffset] and dereferences it with NO null
// check (BS1's drain+0x33 crash shape), which is what the 1t drain guard is
// for. The prologue constant stops before the absolute-VA scope-table push
// (`68 <VA>`) - those bytes relocate with the image base.
constexpr uint32_t kDrainRva = 0x69F3F0;
constexpr uint8_t kDrainPrologue[] = {0x55, 0x8B, 0xEC, 0x6A, 0xFF};

// Static build-identity gate for the flush hook, in verify_draw_chain's
// shape: the E8 at kFlushCallSiteRva must land on kFlushThunkRva, whose E9
// must land on kFlushPointRva, whose bytes must match kFlushPointPrologue.
// Pure image reads - a different build names itself instead of being hooked.
bool verify_flush_chain(const bvr::pattern_scan::ProcessImage& image);

// FContentStreamingManager view hand-off (telemetry hook only). NOTE: this
// is a DIFFERENT Draw-tail call from the flush point above - the streaming
// hand-off returns to 0x4EF541, the flush call to 0x4EF4A6. Two tail calls,
// two subsystems; do not conflate them (session 26 did).
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

// ---- The FOREGROUND (viewmodel) lens, session 33 ---------------------------
// A float in DEGREES on the PLAYERCONTROLLER - the `self` of the PlayerCalcView
// ProcessEvent dispatch, which b2r already holds every frame, so this needs no
// scan and no vtable revalidation of its own.
//
// BS1's equivalent is PC+0x460. THIS IS +0x694. Same engine family, same shape
// (a 75.0/60.0 pair), different link - derived fresh, as the hard rule demands.
//
// Derivation (2026-07-31, live save "Adonis Luxury Resort", option 100):
//  1. `pcinfo` swept the live PC and pawn for floats in [55,65]: 3 hits on the
//     PC (+0x488, +0x694, +0x69C), 3 on the pawn (+0x5CC, +0x850, +0xF08).
//  2. THE DISCRIMINATOR, in ONE dump: poke each candidate to a DIFFERENT
//     distinctive FOV (70/80/90/110/120/130) and see which value the fg
//     cluster's tangent lands on. It read tanH = 0.8391 = tan(40) = exactly
//     80.0 deg - the value written to +0x694 - on the same 17 blocks and the
//     same cb tiers as the 60-deg cluster it replaced. One capture, no
//     bisection, and immune to the "did the cluster vanish or did we stop
//     sampling it" ambiguity that a lens COUNT cannot resolve.
//  3. Confirmed as a lens rather than a coincidence by a full sweep with the
//     live match armed: ONE cluster at every option value tested (100 ->
//     1.1918, 130 -> 2.1445, 80 -> 0.8391) and TWO again the moment it is
//     disarmed, restoring 60.0 exactly.
//
// +0x690 IS THE WORLD LENS - DO NOT WRITE IT. Poking it to 125 took the WORLD
// pass to tanH 3.7320 (150 deg) while the fg stayed where it was put. The
// adjacent 75.0/60.0 pair looks exactly like BS1's fovA/fovB, and it is NOT:
// on BS2 the first member drives the world. A second 75.0/60.0 pair sits at
// +0x698/+0x69C and is inert (poking it to 90 changed nothing) - defaults.
// Unlike BS1's fovA, +0x690 is NOT restamped every frame; a poke sticks.
constexpr uint32_t kPcForegroundFovOffset = 0x694;
// Sanity band for the field before we write it: a plausible FOV in degrees.
// Cheap protection against a wrong offset on some other build writing into
// unrelated memory every frame - the check costs one compare.
constexpr float kFgFovMinDeg = 10.0f;
constexpr float kFgFovMaxDeg = 179.0f;

// Live pointer to the HorizontalFOV int, or null while the settings object
// is not located. Cache + revalidate by vtable dword every call; a miss
// falls through to the heap scan (rate-limited, DORMANT after 3 straight
// misses - hfov_scan_rearm() clears that, called on view-state changes).
// Game thread only.
int32_t* hfov_option_ptr();
void hfov_scan_rearm(const char* why);

// --- the name system: GNames (session 39) -----------------------------------
// Derived offline 2026-08-03 by reproducing core's find_fname_index_global
// chain against the exe on disk (wide "PlayerCalcView" -> its single exec
// xref at 0x4DCABE -> FName-ctor stub 0x19C04 -> SEH-wrapper body 0xB813B0 ->
// worker stub 0x1B2A7 -> worker body 0xB81CE0) and capstone-walking the
// worker: digit-suffix split, case-insensitive hash AND 0xFFF into a
// 4096-bucket table at RVA 0x1A594D0 (chain via entry+0xC, wcsicmp against
// entry+0x10), and on the FindType==2 path an index into GNames.Data with
// 0x4000000 OR'd into entry+4. That is byte-for-byte BS1's session-20 SHAPE
// (its worker 0x70D3C0, GNames 0x13904EC) with every number fresh, as the
// hard rule demands. Full recipe: docs/bioshock2/ENGINE_NOTES.md session 39.
constexpr uint32_t kGNamesArrayRva = 0x1A614D0; // TArray<FNameEntry*>: Data, +4 Count, +8 Max
// FNameEntry layout (the worker's own accesses; fname_text() additionally
// runs the entry self-index check before trusting any of it):
constexpr uint32_t kFNameEntryIndexOffset = 0x0; // entry's own index (self-check)
constexpr uint32_t kFNameEntryChainOffset = 0xC; // hash-chain next
constexpr uint32_t kFNameEntryTextOffset = 0x10; // UTF-16 text in place

// GNames[index] -> ASCII text (lossy narrowing of the UTF-16). Every
// dereference is validated and the entry must carry its own index; false =
// anything failed. Game thread only.
bool fname_text(uint32_t index, char* out, size_t outCap);

// --- fire-chain FName index globals (session 39, Lane A) --------------------
// The same cached-index-global chain that resolves PlayerCalcView, run per
// fire-chain dispatch name. Offline census 2026-08-03 of the exe found real
// globals for BeginFiring (0x180B154), UseAbility (0x180C00C) and
// InitiateDamage (0x180B804) - all initialized by one boot-time batch
// registration function (the ctor-call run at ~0x976640 filling consecutive
// 8-byte FName globals from RVA 0x180B14C up). GetPerfectFireStart and
// ApplyAimError have wide strings but ZERO exec xrefs - no cached global, so
// no native by-name dispatch SITE exists for them (the live ProcessEvent
// probe is still the authority on visibility; a name can reach ProcessEvent
// through script-to-script dispatch without any native global).
// A name resolving null here is DATA, not an error - the probe uses whatever
// subset resolved.
struct FireNames {
    static constexpr int kMax = 8;
    const char* name[kMax] = {};
    const uint8_t* indexGlobal[kMax] = {}; // null = no cached-index global
    int count = 0;
};
void resolve_fire_names(const bvr::pattern_scan::ProcessImage& image, FireNames& out);

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
