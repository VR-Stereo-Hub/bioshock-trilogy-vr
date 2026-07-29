#pragma once
// BioShock 1 Remastered signature table. This header and patterns.cpp are the
// ONLY files allowed to contain raw addresses/offsets for this game (see
// ARCHITECTURE.md). Every entry is documented in docs/bioshock1/ENGINE_NOTES.md with
// its derivation method.

#include "core/hooks/heap_scan.h"
#include "core/hooks/pattern_scan.h"

#include <cstdint>

namespace bvr::b1r::patterns {

// Live horizontal FOV in degrees (float) inside APlayerController.
// Derivation: itsloopyo/bioshock-remastered-headtracking (MIT)
// FOV_LIVE_OFFSET; verified live in DR-4. Telemetry-only: the renderer never
// reads it (ENGINE_NOTES 2026-07-24).
inline constexpr uint32_t kFovLiveOffset = 0xE0;

// UShockUserSettings singleton - the remaster settings object that backs the
// [ShockGame.ShockUserSettings] ini section. Its HorizontalFOV int is what
// the renderer consumes EVERY FRAME, with no cap beyond the options UI's
// 75-130 range. Derivation (2026-07-24, ENGINE_NOTES symbol table): runtime
// value-scan narrowing through the command seam + poke/screenshot A/B to find
// the consumed copy + RTTI walk (vtable -> COL -> TypeDescriptor) for the
// class name. There is NO stable static pointer to the object (the .data
// slots the ptr-scan surfaced were coincidental range matches that a float
// later overwrote); the object itself is stable for the session, so we locate
// it by scanning the heap for its fixed-RVA vtable and caching the instance.
inline constexpr uint32_t kUserSettingsVtableRva = 0xDA3878;  // .?AVUShockUserSettings@@
inline constexpr uint32_t kUserSettingsHfovOffset = 0x8C;     // int32, degrees

// Plausibility window for the accept filter. The options UI clamps to 75-130,
// so anything outside that is debris rather than a settings object. The scanner
// this replaced accepted 40-170, which let a great deal more through - and the
// consumer writes to whatever is accepted (session 27).
inline constexpr int32_t kUserSettingsHfovMin = 75;
inline constexpr int32_t kUserSettingsHfovMax = 130;

// ---- object-scan policy (session 27) ----------------------------------------
// Wall clock one sliced sweep slice may spend on the game thread. 4 ms at ~90
// CalcView/s is a visible frame cost while a sweep is running and nothing at
// all once the object is bound; the alternative it replaced was a single 1-4 s
// blocking pass, which is what users reported as the game freezing.
inline constexpr uint32_t kScanSliceBudgetUs = 4000;
// Gap between search attempts once one has completed without finding anything.
inline constexpr uint64_t kScanRetryMs = 2000;
// Consecutive completed-but-empty searches before the scanner latches dormant.
// A rate limit alone rescans forever on a save where the object never appears.
inline constexpr int kScanMissesBeforeDormant = 3;

// Render command-queue functions (DR-5 / SequentialReentry). Derivation
// (ENGINE_NOTES "Scene-draw architecture"): draw-callstack RVA histogram from
// the frame inspector + byte-level prologue/RET walk via the hexdump seam
// (2026-07-24), CORRECTED live in session 5: the function session 4 called
// "frame root" (0x61D0F0) is a render-thread FLUSH/JOIN that never runs in
// steady-state gameplay (pass-through hook: 0 entries over 70 s of play).
// The real per-frame render entry is the DRAIN, entered exactly once per
// Present (573/s == presents/s live) from the render-thread PUMP LOOP at
// 0x61D1D0 (frameless `push esi` prologue - session 4's CC-55-8B-EC
// prologue walk overshot it into 0x61D0F0). All void __thiscall, zero stack
// args; the flush never reads its ECX.
inline constexpr uint32_t kRenderFlushRva = 0x61D0F0; // flush/join, NOT per-frame
inline constexpr uint32_t kDrainRva = 0x61CAE0;       // per-frame render entry
inline constexpr uint32_t kPumpLoopRva = 0x61D1D0;    // render-thread main loop
// Pump-loop internals (session-5 byte walk + live import resolution): it
// registers GetCurrentThreadId() into the global below, then loops
// WaitForSingleObject(INFINITE) -> drain -> SetEvent. [queue+0x58] != 0 is
// the pump-loop EXIT condition (not a per-frame latch).
inline constexpr uint32_t kRenderTidGlobalRva = 0x13784E4;
// Static render-manager global (loaded via A1 in the flush prologue):
// [mgr+4] = command-queue object (queue vtable RVA 0xE0DFD0, mgr vtable
// 0xE1CF14, both live session 5); the flush stamps [mgr+0x58] = 1.
inline constexpr uint32_t kRenderMgrGlobalRva = 0x1356590;
inline constexpr uint32_t kQueueDrainGuardOffset = 0x58; // pump exit flag
// Expected first bytes (build-identity check before patching a prologue).
inline constexpr uint8_t kRenderFlushPrologue[5] = {0x55, 0x8B, 0xEC, 0x51, 0xA1};
inline constexpr uint8_t kDrainPrologue[5] = {0x55, 0x8B, 0xEC, 0x6A, 0xFF};

// Game-thread frame SUBMIT/KICK - the SequentialReentry seam (session 5;
// ENGINE_NOTES "Scene-draw architecture"). Located by the reentry probe's
// SetEvent caller sampler (game-thread SetEvent ret RVA 0x585C68), entry by
// prologue walk. ret 0xC = 3 stack args: arg1 FVector* camLoc, arg3 a
// viewport/scene object; stores the camera into the submitted-frame globals
// and SetEvents the render pump. Double-calling THIS with a modified rot is
// the true second-render primitive (scenedraw.cpp SubmitDetour). Session-6
// disk-image byte walk confirmed: arg2 IS the FRotator* (3 dwords copied to
// the frame block +0x14), ECX is dead at entry (`push ecx` = stack alloc),
// literal `C2 0C 00` ret, SetEvent call site 0x585C62 / ret RVA 0x585C68.
inline constexpr uint32_t kFrameSubmitRva = 0x585AC0;
inline constexpr uint8_t kFrameSubmitPrologue[10] = {0x55, 0x8B, 0xEC, 0x51,
                                                     0x64, 0xA1, 0x2C, 0x00,
                                                     0x00, 0x00};

// Game-thread scene BUILD root - the function that builds the render command
// queue and (at its tail, site 0x4CDD85) calls the frame submit above.
// Derivation (2026-07-24 session 6): the live submit hook logged gameplay
// caller ret RVA 0x4CDD8A; disk-image disassembly (capstone) walked backwards
// to the enclosing entry - past a decoy SEH function at 0x4CCD20 that a
// CC-preceded 55-8B-EC scan finds first; the real boundary is the 11-byte CC
// padding run ending at 0x4CCE6F (the session-5 "frameless prologue" lesson
// again: this entry starts `push ebx`, not `push ebp`). Aligned-stack MSVC
// prologue (`push ebx; mov ebx,esp; sub esp,8; and esp,-0x10`), then an SEH
// frame; `ret 0x10` = 4 stack args ([ebx+8..0x14]); ECX = live this (stored
// to [ebp-0x80]; its +0x118/+0x11C ring positions gate the submit call);
// stack arg1 = object whose +0x48 the tail reads. EDX not read before the
// first write observed - fastcall passthrough detour covers either way.
// Discovered AFTER live evidence that double-calling the SUBMIT alone is
// absorbed (presents did not double, yawed camera never rendered): the view
// data is baked into the queue during THIS function, so SequentialReentry
// must re-enter here.
inline constexpr uint32_t kSceneBuildRva = 0x4CCE70;
// The STEADY-STATE GAMEPLAY caller's return RVA for the scene build (live
// hook telemetry, sessions 6-7). Loads/transitions call the build from other
// sites; pass 2 doubles ONLY gameplay-caller builds - doubling (or any 1t
// inline-forcing) during a level load is the 2026-07-24 19:54 loader-thread
// crash (ENGINE_NOTES "1t load hazard").
inline constexpr uint32_t kSceneBuildGameplayRetRva = 0x850EF0;
inline constexpr uint8_t kSceneBuildPrologue[9] = {0x53, 0x8B, 0xDC, 0x83,
                                                   0xEC, 0x08, 0x83, 0xE4,
                                                   0xF0};

// Submitted-frame id pair (.data globals, block at [0x13AF7E8..] - see the
// submit row in ENGINE_NOTES). Two slots (double-buffered frames): dword at
// +0 and dword at +0x10, each holding a frame id whose HIGH BIT is the
// completion flag (live dump 2026-07-24: 0x8000043B / 0x8000043C in steady
// state - consecutive frames, both consumed). The submit head's `jg` checks
// are SIGNED, so a set high bit (negative) never triggers the engine's wait;
// the wait path's -1 compare is an init sentinel only. The double-render
// waits BEFORE its second build call until BOTH slots read negative (both
// buffers consumed, zero frames in flight): the engine's own event-based
// wait has a lost-wakeup race (two live hangs, 2026-07-24 session 6), and a
// bounded poll on these flags cannot lose a wakeup - and with the pipeline
// drained the racy engine waiter never engages at all.
inline constexpr uint32_t kFrameIdPairRva = 0x13AF7E8;
inline constexpr uint32_t kFrameIdSecondOffset = 0x10;

// Command-queue state on the scene-build's `this` (the queue object), live
// dump 2026-07-24 at idle: +0x118/+0x11C are ring POINTERS (unequal at idle
// - the submit call-site gate `[+0x118] != [+0x11C]` reads as "commands
// written this frame", not empty/full), while +0x128/+0x12C are twin
// produced/consumed COUNTERS (equal at idle - value 9/9 live). Session-6
// hang thread-dump showed the engine's ring full/empty event waits
// deadlocking under the double-render (game thread waiting at exe+0x61D38E
// inside the build vs render thread waiting inside the drain at +0x30), so
// the second build call waits for counter equality first - an idle queue
// keeps those racy waits from engaging.
inline constexpr uint32_t kQueueSegProdOffset = 0x128;
inline constexpr uint32_t kQueueSegConsOffset = 0x12C;

// Engine event objects (all instances of the vtable-0xE2D584 event class;
// HANDLE at obj+4) used by the stereo deadlock watchdog. The pump kick event
// pointer lives in the static global below (the submit SetEvents it at
// 0x585C62); the flush-point wait function (~0x61D340) waits on the queue's
// event at [queue+0x10], with a sibling event at [queue+0xC]
// (queue = [[kRenderMgrGlobalRva]]+4). Re-SetEvent-ing these is exactly the
// wakeup the deadlocked protocol lost; the watchdog verifies the vtable
// before touching a handle.
inline constexpr uint32_t kPumpKickEventPtrRva = 0x13566C4;
inline constexpr uint32_t kEventVtableRva = 0xE2D584;
inline constexpr uint32_t kQueueEventAOffset = 0xC;
inline constexpr uint32_t kQueueEventBOffset = 0x10;

// Render-thread OBJECT global, sibling of the pump kick event above: the
// scene build's tail gates its submit call on this being non-null
// (ENGINE_NOTES scene-draw table, session 6). Under -onethread BOTH globals
// stay NULL (the pump/queue thread is never created) - so "either non-null"
// is the live THREADED-renderer detector (session 7: used to refuse stereo
// on the deadlock/crash-prone threaded substrate, and for the mode tag in
// the reentry heartbeat).
inline constexpr uint32_t kRenderThreadObjRva = 0x13566CC;

// Drain-head layout (session-7 minidump forensics + disk disasm of the head
// at kDrainRva): after EnterCriticalSection([this+8]+4) the drain loads the
// submitted-frame context from [this+0xC] (drain+0x30) and immediately
// dereferences its +0x40 viewport member (drain+0x33; virtual call through
// [+0x40] at vtbl+0xEC follows). All three 2026-07-24 drain+0x33 crash dumps
// show the SAME state: faulting thread = the render PUMP (ret 0x61D21E),
// ESI = [this+0xC] = NULL, fault addr 0x40 - a pump woken with NO pending
// frame (watchdog kick or desynced-protocol stray wake; THREADED mode only).
// The guard: while doubling, a drain entered with the slot empty is skipped
// outright - with no frame there is nothing to consume.
inline constexpr uint32_t kQueueFrameCtxOffset = 0xC;

// Render-mode override: the flush-point function (entry 0x61D260, `ret 8`;
// full head disasm 2026-07-24) picks threaded vs single-threaded rendering
// per call through a decision chain whose FIRST check is this static global
// - nonzero forces the single-threaded path: the game thread calls the
// drain INLINE and the entire two-thread queue protocol (submit SetEvent,
// pump, flush waits - the whole deadlock class) is bypassed. Discovered
// while chasing the stereo double-render deadlock; poking it to 1 at
// runtime is the candidate stereo-safe mode.
// DEAD END (session 6): 500+ references engine-wide (GIsEditor-class);
// poking it crashed the next level load. Kept for the decision-chain doc.
inline constexpr uint32_t kForceNonThreadedRenderRva = 0x1375BD4;

// THE render-mode selector pair (session-7 full decode of the flush-point
// decision chain at 0x61D260; ENGINE_NOTES has the complete chain). The
// LAST check - reached when no veto fired - is
//   threaded = ([kNumHwThreadsRva] / [kThreadDivisorRva]) > 1
// i.e. "got more than one hardware thread?". Live values: 12 / 1. The pair
// is written once at startup (writers RVA ~0x756ED2 numerator, ~0x6F26E0
// region denominator) and consumed by SEVEN inlined copies of the same
// quotient test (RVAs 0x43BD90, 0x4D0E24, 0x58413B, 0x604641, 0x61D1AC,
// 0x61D33B = the flush-point, 0x7814F6) plus a cmp-1 at ~0x6F26EE - a
// tight, single-purpose family (unlike the 500-ref editor global above).
// Poking the numerator to 1 makes every subsequent flush take the INLINE
// single-threaded drain path: no pump hand-off, no INFINITE render-done
// wait, deadlock class structurally unreachable. The pump thread stays
// alive and gets kicked by the submit each frame; it wakes into an empty
// frame slot, which the drain-hook empty-slot guard turns into a logged
// skip (this is why `reentry 1tpoke on` arms the drain hook BEFORE poking).
// SUPERSEDED (session 8) by the flush-point hook (kFlushPointRva) as the
// default `reentry 1t`: hooking the flush and forcing its inline branch
// leaves THIS numerator untouched, so the pair's load-path consumers
// (0x4D0E24) see the true core count - the poke's load hazard is gone.
// This poke is retained only as the `reentry 1tpoke` fallback/diagnostic.
// Session-6 postscript: the `-onethread` launch arg this pair replaces is
// NOT PARSED by the remaster at all (no such string in the image) - the
// "onethread substrate" was a menu-time artifact; this poke is the real
// single-threaded switch.
// Alternative (documented, unused): the chain's check #2 reads
// [[sceneObj+0x3DC]]+0x4C (live: scene vtable 0xE1846C, client obj vtable
// 0xE127CC, +0x4C == 1) - a per-client "use render thread" bool; zeroing
// it flips only the render chain but lives in heap object memory.
inline constexpr uint32_t kNumHwThreadsRva = 0x11B69FC;
inline constexpr uint32_t kThreadDivisorRva = 0x11B7A00;

// The flush-point FUNCTION itself (session-7 decision-chain decode; every
// byte re-confirmed by a capstone disk walk 2026-07-24 session 8): entry
// 0x61D260, `ret 8` = 2 stack args (arg1 = scene object, arg2 = 16-dword
// view group), ECX DEAD at entry (the prologue's `8B 4D 0C` loads arg2 into
// it). Body: mgr = [kRenderMgrGlobalRva]; arg1 -> [mgr+0xC]; arg2's 16
// dwords -> [mgr+0x10..0x4C]; decision chain -> eax; [mgr+0x50] = eax;
// [mgr+0x54] = 1; eax==0 -> `call drain(ECX=mgr)` then STRAIGHT to the
// epilogue (no post-drain work - verified), else the queue hand-off (the
// 0x61D38E deadlock wait). The structural `reentry 1t`: MinHook this entry
// and force the inline branch in the detour - the hw-thread numerator is
// never touched, so its OTHER quotient-family consumers (load-path site
// 0x4D0E24 - the 19:54 loader crash) see the true core count, and the
// inline behavior is confined to exactly the per-frame scene flush.
inline constexpr uint32_t kFlushPointRva = 0x61D260;
inline constexpr uint8_t kFlushPointPrologue[7] = {0x55, 0x8B, 0xEC, 0x51,
                                                   0x8B, 0x4D, 0x0C};
// Render-manager fields the flush-point writes (mgr is also the drain's
// `this` - kMgrSceneSlotOffset IS kQueueFrameCtxOffset, the slot the drain
// guard null-checks).
inline constexpr uint32_t kMgrSceneSlotOffset = 0xC;    // arg1 stored here
inline constexpr uint32_t kMgrViewGroupOffset = 0x10;   // arg2 copy 0x10..0x4C
inline constexpr uint32_t kMgrViewGroupDwords = 16;
inline constexpr uint32_t kMgrThreadedFlagOffset = 0x50; // 0 = inline drain
inline constexpr uint32_t kMgrFlushSeenOffset = 0x54;    // stamped 1 per flush

// ---- Gamepad / UWindowsClient facts (session 9, from the XINPUT1_3
// import-thunk walk + RTTI; full derivation in ENGINE_NOTES "Gamepad
// architecture") -------------------------------------------------------------
// The remaster reads the pad ONLY inside UWindowsViewport::UpdateInput
// (RVA 0x853D20, `ret 8` = thiscall(BOOL reset, FLOAT dt)), which NOBODY
// calls in windowed mode - the game probes XInputGetState ~6x at boot
// (client init, RVA 0x8507BB) and never again; WM_DEVICECHANGE, ini
// UseJoystick/UseController and the pad-connected global are all ignored
// until UpdateInput runs. The adapter therefore drives UpdateInput itself
// once per present (input_drive.cpp) and flips UseController through the
// engine's own setter so game-level checks (`client+0xDC && connected
// global`) light up. UWindowsClient::Exec also handles a
// "ToggleUseController" console command that calls the same setter.
inline constexpr uint32_t kGameEnginePtrRva = 0x1375368;  // [.]->UGameEngine, +0x4C = Client
inline constexpr uint32_t kEngineClientOffset = 0x4C;
inline constexpr uint32_t kClientVtableRva = 0xE4DBE0;    // RTTI .?AVUWindowsClient@@
inline constexpr uint32_t kViewportVtableRva = 0xE4E448;  // RTTI .?AVUWindowsViewport@@
inline constexpr uint32_t kClientViewportsDataOffset = 0x44; // TArray<UViewport*> data
inline constexpr uint32_t kClientViewportsCountOffset = 0x4C;
inline constexpr uint32_t kClientUseControllerOffset = 0xDC; // BOOL, ini UseController is dead
// Shared vtable byte offset 0x118 (slot 70): on UWindowsClient it is
// SetUseController(BOOL) (RVA 0x8509B0 - writes +0xDC, updates the UI-prompt
// global, SaveConfig, notifies gameswf); on UWindowsViewport it is
// UpdateInput(BOOL reset, FLOAT dt) (RVA 0x853D20 - DI keyboard block
// self-skips when no DI device, then XInputGetState(0) every call: connected
// -> processes pad into engine input, else retries and re-stamps the
// connected global at kPadConnectedFlagRva).
inline constexpr uint32_t kVtblSlot70Offset = 0x118;
inline constexpr uint32_t kPadConnectedFlagRva = 0x11B7A10; // written by UpdateInput
// The game's IAT slots for its XINPUT1_3 ordinal imports (from the PE import
// directory). The STEAM OVERLAY code-hooks the proxy's export thunk (E9 jmp
// into gameoverlayrenderer, observed live 2026-07-25) and swallows GetState
// before the proxy body runs - so the bridge re-points the ord-2 IAT slot at
// its own wrapper (previous target kept as passthrough; Steam-served real
// pads still work). Ord-3 (SetState/vibration) is untouched until M7 haptics.
inline constexpr uint32_t kXInputGetStateIatRva = 0xBCF8E0;
inline constexpr uint32_t kXInputSetStateIatRva = 0xBCF8DC;
// Exec chain entries (UBOOL __thiscall Exec(const wchar_t* cmd,
// FOutputDevice& ar), ret 8; found via ParseCommand string clusters + RTTI):
// UWindowsViewport::Exec handles SETRES/TOGGLEFULLSCREEN/SETMOUSE/...;
// UWindowsClient::Exec handles ToggleUseController/ResetFlashMovies and
// forwards to its base chain (call 0x4E2790 at its tail). Exec is vtable
// slot 65 (+0x104) on UWindowsClient; the seam calls the entries directly.
inline constexpr uint32_t kViewportExecRva = 0x8525C0;
inline constexpr uint32_t kClientExecRva = 0x850DE0;
// UGameEngine::Exec (start via SEH-prologue walk from the SERVERTRAVEL /
// SAVEGAME / GETMAXTICKRATE ParseCommand cluster 0x4C5D75..0x4C6324; common
// epilogue 0x4C71C7, `ret 8`). Expects `this` to be the FExec SUBOBJECT at
// engine+0x40 (the RTTI COL at offset 0x40; its body does `lea eax,[this-0x40]`
// to reach the real object - calling it with the plain engine pointer faulted).
// Its unhandled-command tail forwards to the chain walker 0x3528C0
// (engine, cmd, Ar) which reaches the player/actor ScriptConsoleExec path.
inline constexpr uint32_t kEngineExecRva = 0x4C5970;
inline constexpr uint32_t kEngineExecThisOffset = 0x40; // FExec subobject
// UObject::Exec is vtable slot 65 == byte offset 0x104 (verified on
// UWindowsClient/UWindowsViewport/UGameEngine). The player-object entry that
// runs UnrealScript exec commands needs its exact signature reversed before it
// can be called safely - a naive slot-65 call unbalanced the stack (session 9).
// Parked M6 groundwork.
inline constexpr uint32_t kEngineVtableRva = 0xE0DFF4; // RTTI .?AVUGameEngine@@ (offset 0)
// FOutputDevice contract (from the Logf helper 0x6E7AC0): vtbl+0x10 is a
// 2-arg log-filter (return 0 = suppress -> the helper skips formatting and
// Serialize entirely); vtbl+0x4 is Serialize(text, event), 2 args. The seam's
// stub device returns 0 from every slot with callee-pop-8.

// ---- M6 fire-flow seams (session 10) ---------------------------------------
// THE aim seam. Both fire paths ask one function for "where does this shot
// start and where does it point", and both are C++ implementations reached
// through the class vtable / a direct call - NOT through the UnrealScript
// exec thunks (native callers bypass those, which is why hooking the thunks
// caught nothing). Derivation chain, all in ENGINE_NOTES "Fire flow / aim":
//   script Ability.UseAbility -> native InitiateDamage (virtual)
//     AWeapon::InitiateDamage      RVA 0x226050 -> calls [weaponVtbl+0x304]
//     UAttackAbility::InitiateDamage RVA 0x1BBD80 -> calls 0x1BC220 directly
//   ...each of which fills the out-params below, and THEN the engine applies
//   its own spread (AWeapon::ApplyAimError impl 0x226AA0), so substituting
//   here keeps per-weapon accuracy/spread intact.
//
// AWeapon::GetPerfectFireStart, __thiscall, ret 0xC:
//   (FVector* outA, FVector* outB, FVector* outC)
//   outA <- ownerPawn+0x1D8 (location-family field), outB <- [pawn+0x450]+0x1E4
//   (direction-family field, the one ApplyAimError then perturbs).
// UAttackAbility::GetPerfectFireStart, __thiscall, ret 0x10:
//   (void* instigator, FVector* outA, FVector* outB, void* outC)
//   same two sources, one slot over; the probe logs both so the labels are
//   confirmed live rather than assumed.
inline constexpr uint32_t kAttackAbilityVtableRva = 0xD7E9D4;   // .?AVUAttackAbility@@
inline constexpr uint32_t kWeaponFireStartVtblOffset = 0x304;    // AWeapon vtable slot
inline constexpr uint32_t kExpectedWeaponFireStartImplRva = 0x226840;
inline constexpr uint32_t kAbilityFireStartImplRva = 0x1BC220;   // direct call target
inline constexpr uint8_t kAbilityFireStartPrologue[6] = {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF8};
inline constexpr uint32_t kWeaponOwnerOffset = 0x454;      // AWeapon -> owning pawn
inline constexpr uint32_t kAbilityInstigatorOffset = 0xF0; // UAttackAbility -> instigator
// AActor field offsets used by the fire path (from the APawn::GetViewPoint /
// GetViewDirection implementations, vtable slots +0x360 / +0x35C):
//   +0x1D8 location-family FVector, +0x1E4 direction-family FVector,
//   +0x550 eye height (GetViewPoint = location + eyeHeight on Z).
inline constexpr uint32_t kActorLocOffset = 0x1D8;
inline constexpr uint32_t kActorViewDirOffset = 0x1E4;
// M7 session-11 actor-field probes, recorded honestly: a TRUE DrawScale has
// NOT been located on this build. +0x16C (default 0.0) HIDES the mesh at small
// values (0.5/0.8 both vanished the pistol - cull-distance-like semantics, and
// an early "0.8 shrank it" reading was a misread); +0x168 (default 1.0, right
// after the mesh pointer at +0x164) looked like DrawScale but poking 0.5
// changed nothing visible. Gun-size control is still an open item. Also
// learned here: the weapon's attach BASE pointer (-> the AHands actor) is at
// +0x450, adjacent to Owner at +0x454 - the classic UE2 Owner/Base pair - and
// the renderer draws attached actors from the attach matrix, ignoring their
// own Location/Rotation fields.
inline constexpr uint32_t kActorHideDistOffset = 0x16C; // small value = mesh hidden
inline constexpr uint32_t kActorBaseOffset = 0x450;     // attach parent (AActor*)
// The two InitiateDamage implementations that CALL the fire-start functions
// above (weapon = AWeapon vtable slot +0x2FC, ability = the direct call target
// of UAttackAbility::execInitiateDamage). Both `ret 8` - thiscall taking an
// FName by value. Hooked read-only in `vraim probe` as the "a shot happened,
// and it came from this family" signal, which is how the flat verification
// tells a gun shot from a plasmid cast from a Havok melee swing.
inline constexpr uint32_t kWeaponInitDamageVtblOffset = 0x2FC;
inline constexpr uint32_t kExpectedWeaponInitDamageRva = 0x226050;
inline constexpr uint32_t kAbilityInitDamageRva = 0x1BBD80;
inline constexpr uint8_t kAbilityInitDamagePrologue[6] = {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF8};

// ---- M6 aim natives (session 10) -------------------------------------------
// Resolved at runtime through the engine's own native-function lookup table
// (pattern_scan::find_native_function - registration string
// "int<Class>exec<Func>" -> its .data table entry -> impl pointer), so no
// address below is hardcoded; the RVAs are recorded only as the expected
// values for this build (2022-04-13) and as the cross-check the log prints.
// Full derivation + the fire flow in ENGINE_NOTES "Fire flow / aim".
//
// All four are UnrealScript native thunks:
//   void __thiscall execFoo(FFrame& Stack, void* Result)
// FFrame layout used by the probe: +4 Node (UStruct* = the calling script
// function), +8 Object (UObject* = the script object executing), +0xC Code.
inline constexpr uint32_t kExpectedWeaponFireStartRva = 0x225B20; // AWeapon::GetPerfectFireStart
inline constexpr uint32_t kExpectedWeaponAimErrorRva = 0x226A00;  // AWeapon::ApplyAimError
inline constexpr uint32_t kExpectedPawnViewPointRva = 0x3CB990;   // APawn::GetViewPoint
inline constexpr uint32_t kExpectedPawnViewDirRva = 0x3CB9D0;     // APawn::GetViewDirection
inline constexpr uint32_t kExpectedActorTraceRva = 0x547BD0;      // AActor::Trace
// APawn::GetViewPoint/GetViewDirection are thin exec wrappers around virtual
// slots on the pawn vtable (byte offsets, from the disassembly of the thunks):
// +0x360 returns the view point FVector*, +0x35C the view direction FVector*.
inline constexpr uint32_t kPawnViewPointVtblOffset = 0x360;
inline constexpr uint32_t kPawnViewDirVtblOffset = 0x35C;
// FFrame field offsets (UE2 FFrame : FOutputDevice).
inline constexpr uint32_t kFFrameNodeOffset = 0x4;
inline constexpr uint32_t kFFrameObjectOffset = 0x8;
inline constexpr uint32_t kFFrameCodeOffset = 0xC;
// Class vtables (MSVC RTTI walk on the disk image: TypeDescriptor
// `.?AV<name>@@` -> CompleteObjectLocator -> vtable). AShockPlayer is the
// player's own pawn, which is what CalcView reports as the view actor during
// normal gameplay - the cutscene guard is "view actor still has this vtable".
inline constexpr uint32_t kShockPlayerVtableRva = 0xD82BB8;   // .?AVAShockPlayer@@
inline constexpr uint32_t kPlayerWeaponVtableRva = 0xD8FF58;  // .?AVAPlayerWeapon@@
inline constexpr uint32_t kHandsVtableRva = 0xD8A28C;         // .?AVAHands@@ (M7)

// ---- M7-v2 skeleton internals (session 12, 2026-07-26) ----------------------
// Derivations in ENGINE_NOTES "Skeleton / bone internals": native impl walks
// (execGetBoneCoords 0x545090 -> inner 0x544FA0, execGetBoneLocation 0x5414D0,
// execAttachToBone -> AActor::AttachToBone 0x379EF0, SkeletonInstanceFreeze
// 0x542AD0, AActor::SetDrawScale 0x375830) plus a live hexdump/poke session.
//
// The evaluated skeleton lives in a per-actor `SkeletonInstance` (RTTI name,
// 0x9C bytes) at actor +0x3FC. Bones are Havok hkQsTransform (48 bytes:
// pos float3+pad, quat xyzw, scale float3+pad) in COMPONENT space - poking a
// bone's pos moved that mesh part on screen the same frame, and the equipped
// WEAPON rendered at the poked transform of the attach bone (engine-side
// attachment follows the array). Two arrays: A (+0x48/+0x4C) feeds the by-index
// path and the renderer; B (+0x54/+0x58) is the lazily-filled by-name path.
inline constexpr uint32_t kSkeletonInstanceVtableRva = 0xE19ACC; // .?AVSkeletonInstance@@
inline constexpr uint32_t kActorMeshInstOffset = 0x128;  // AActor -> USkeletalMeshInstance*
inline constexpr uint32_t kActorSkelInstOffset = 0x3FC;  // AActor -> SkeletonInstance*
inline constexpr uint32_t kSkelInstActorOffset = 0x04;   // backref to the actor
inline constexpr uint32_t kSkelInstFreezeOffset = 0x20;  // int; SkeletonInstanceFreeze = 1
inline constexpr uint32_t kSkelInstBonesOffset = 0x48;      // hkQsTransform* array A
inline constexpr uint32_t kSkelInstBoneCountOffset = 0x4C;  // int (AHands rig: 47)
inline constexpr uint32_t kSkelInstBonesBOffset = 0x54;     // hkQsTransform* array B
inline constexpr uint32_t kSkelInstBoneCountBOffset = 0x58;
inline constexpr uint32_t kSkelInstDirtyOffset = 0x88;   // evaluate-if-dirty flag (byte)
// Session 20: bone NAMES. SkeletonInstance +0x08 -> SharedSkeletonData, whose
// +0xAC map is FName -> bone index (lookup fn 0x5F6500, disassembled): a
// standard UE hash map - +0x00 pairs base (16-byte pairs: +0 chain next,
// +4 FName index, +8 FName number, +0xC value), +0x0C bucket array (int32,
// -1 = empty), +0x10 bucket count (power of two). Walking every bucket chain
// enumerates the whole table, which inverts to bone index -> name.
inline constexpr uint32_t kSkelInstSharedOffset = 0x08;
inline constexpr uint32_t kSharedBoneNameMapOffset = 0xAC;
inline constexpr uint32_t kNameMapPairsOffset = 0x00;
inline constexpr uint32_t kNameMapBucketsOffset = 0x0C;
inline constexpr uint32_t kNameMapBucketCountOffset = 0x10;
inline constexpr uint32_t kNameMapPairStride = 16;
// The attach bone FName is stored ON the attached actor (weapon +0xF0/+0xF4)
// by AActor::AttachToBone; attachment itself is bone-name + SetBase (actor
// vtable +0x1A0). Equip-time only - nothing re-asserts it per tick.
inline constexpr uint32_t kActorAttachBoneNameOffset = 0xF0;
// The REAL DrawScale (the +0x168/+0x16C probes were wrong fields): float at
// +0x2AC, written by AActor::SetDrawScale (0x375830) together with the dirty
// protocol below - a raw field poke without the protocol is invisible.
inline constexpr uint32_t kActorDrawScaleOffset = 0x2AC;
inline constexpr uint32_t kActorDirtyFlagsOffset = 0xD0;  // |= 0x10 on transform-ish change
inline constexpr uint32_t kActorRenderRevOffset = 0x3F4;  // ++ (UpdateRenderRevision)
inline constexpr uint32_t kActorDirtyByteOffset = 0x3E4;  // = 0
// AHands rig bone indices (47-bone skeleton, measured live at the wall save,
// pistol raised): right hand cluster is CONTIGUOUS - 27 wrist, 28-42 thumb +
// finger chains, 43 weapon-attach helper (poking its pos moved the whole gun),
// 44 muzzle-ish tip. Right sleeve (collapse targets): 24 clavicle, 25 upperarm,
// 26 elbow, 45/46 forearm twist helpers. Left arm hangs in 4-23 (wrist/finger
// split still to be measured with a plasmid equipped - see vrbones lcluster).
inline constexpr int kHandsRigBoneCount = 47;
inline constexpr int kBoneRClusterFirst = 27;
inline constexpr int kBoneRClusterLast = 44;
inline constexpr int kBoneWeaponAttach = 43;
inline constexpr int kBoneRSleeve[] = {24, 25, 26, 45, 46};
// Left mirror, measured with Electro Bolt raised (left arm forward): wrist 6,
// thumb 7-9, fingers 10-21 - contiguous cluster 6-21, anchored at the wrist
// (no attach/muzzle analog exists on the left). Sleeve mirror: 3 clavicle,
// 4 upperarm, 5 elbow, 22/23 forearm twist helpers.
inline constexpr int kBoneLClusterFirst = 6;
inline constexpr int kBoneLClusterLast = 21;
inline constexpr int kBoneLWrist = 6;
inline constexpr int kBoneLSleeve[] = {3, 4, 5, 22, 23};

// ---- Foreground (viewmodel) scene (session 13, 2026-07-26) ------------------
// The renderer draws the first-person rig as a separate FOREGROUND scene with
// its own projection, fixed at a 60-deg 4:3 spec regardless of the world FOV
// (frame-dump proof: vm draws carry tanH = tan(30)*4/3 = 0.7698 / tanV =
// tan(30)*3/4 = 0.4330 at world FOV 110 AND 137, while world draws track the
// option exactly), through a VIEW whose eye is depth-pushed ~32 UU behind the
// rig origin IN ACTOR SPACE and which carries the engine's hand sway. The
// composite is self-consistent at view center but displaces the rendered rig
// laterally as soon as the camera splits from the actor rotation - the M7
// "camera-coupled rig term" (10-15 deg at 30 deg of HMD head-yaw). Fix:
// bones.cpp captures the vm draws' component->clip transform from cb0 every
// frame (fingerprint below, frame_inspector::set_cb_watch) and solves the
// anchor target so the renderer's own math lands it world-correct.
// Dead end recorded: the pawn (AShockPlayer) carries a foreground-FOV property
// group at +0x550 (default 60.0) / +0x554 (36.0, zoom?) / +0x558 (lerped
// current; script names ForegroundFovAngle/DefaultForegroundFOV exist in
// Engine.U/ShockGame.U) - but holding +0x558 at another value per frame does
// NOT change the render (dump-proven): the renderer's constants are built
// elsewhere; the property is upstream state only.
inline constexpr uint32_t kPawnForegroundFovOffset = 0x558; // research; not a render lever
// THE REAL FOREGROUND FOV (session 15): a float on the PLAYERCONTROLLER,
// consumed by the renderer EVERY FRAME - poking it re-lenses the whole rig
// the same frame, and a dump shows every vm draw (576 + 832/1088 lighting
// tiers) joining the world's projection cluster. Found by fsweep of the live
// PC (60.0 next to a 75.0 default at +0x45C) + live poke A/B. The value is a
// 4:3 spec: matching the world lens means 2*atan(tan(worldFov/2)*3/4) - at
// option 117 that is 101.5, the exact number session 13 held at the WRONG
// address (the pawn's eye height). The session-13 "+0x558 non-lever" verdict
// stands; this is its correction.
inline constexpr uint32_t kPcForegroundFovOffset = 0x460;
// cb0 fingerprint: floats 12..18 of every foreground draw hold the constant
// screen-ray block (2*tanH, 0, -tanH, 0, 0, -2*tanV, tanV) of the fixed
// projection; the capture spans floats 36..59 (viewport block, then the
// component->clip rows x,y,z,w, then an aux row).
inline constexpr float kFgCbFingerprint[7] = {1.5396008f, 0.0f,        -0.7698004f, 0.0f,
                                              0.0f,       -0.8660254f, 0.4330127f};
inline constexpr uint32_t kFgCbFingerprintFirst = 12;
inline constexpr uint32_t kFgCbTransformFirst = 36;
inline constexpr uint32_t kFgCbTransformCount = 24;
// The 576-byte tier only: the 832/1088 lighting-pass tiers carry the SAME
// fingerprint with a DIFFERENT transform at these offsets (same row norms,
// zero w translation - a light/other-space variant that must never feed the
// render-lock solve).
inline constexpr uint32_t kFgCbBytes = 576;
// Structural model of the foreground view (dump-derived, session 13; the
// render-lock SOLVE uses this instead of live captures - with the bone drive
// active the engine switches mesh sections to a baked bone-in-matrix rigid
// path, so a captured matrix embeds our own write and solving against it
// feeds back). Static decomposition, verified against the h0 AND h30 dumps
// to 3 decimals: M = rows scaled (1/tanH, 1/tanV, 1, 1) of [R | -R*E], where
// R = (actor-inverse x camera) rotation in the rig's component frame plus the
// engine's idle hand sway (~+-3 deg, unmodeled residual), and E = the
// foreground eye. The eye RIDES THE CAMERA, translation included (session 13
// part 3: the rig parallaxes 1.35x under a pure camera offset - an
// actor-anchored eye cannot produce that; the 12 dumps all had
// camLoc == actorLoc so they measured only the pull-back). Effective eye in
// component space = camera position + kFgEyeComp rotated into the fg view's
// frame; kFgEyeComp itself is the pull-back behind the render camera:
inline constexpr float kFgEyeComp[3] = {-32.1f, -5.6f, -0.9f}; // mean of 12 dumps, +-0.5
// CAVEAT (session 14): the matrix-recovered eye is SECTION-FRAME-RELATIVE -
// each vm section recovers a different value (a=7704 vs a=14595 differ by
// ~15 UU), so kFgEyeComp is NOT the absolute pull-back. The render-lock
// model uses only its LATERAL components (statics the abs solve absorbs);
// the forward component it uses is the TRUE camera-to-fg-eye pull-back,
// calibrated from physical measurements (three independent baselines
// agree): camera-offset parallax 420 px -> w_gun 29.9 at df 17.4 -> P 12.5;
// size ratio 0.605 on distance doubling 17.4->37.4 -> P 13.2 (session 13's
// 0.62 -> 15.2); the user's perceived ~28 cm at hand ~35 cm -> P 12.3:
inline constexpr float kFgEyeFwdBehindCam = 13.0f; // UU; rendered depth = df + this
inline constexpr float kFgInvTanH = 1.2990381f; // 1/0.7698004
inline constexpr float kFgInvTanV = 2.3094011f; // 1/0.4330127
// The fg view rotation = R_delta composed with a small composition bias whose
// mean measured (+1.7 yaw, +1.1 pitch) deg across the dump set; the +-1.7 deg
// wobble around it is the hand sway itself (x2.12 lens gain on screen =
// vanilla's visible hand liveliness, the model's residual floor).
inline constexpr float kFgViewYawBiasDeg = 1.7f;
inline constexpr float kFgViewPitchBiasDeg = 1.1f;

// ---- The FOREGROUND SCENE NODE (session 21) --------------------------------
// The fg pass is a SECOND SCENE NODE, pure data through shared render
// machinery (dump stack-diff: fg and world draws share the whole skeletal
// draw layer; they differ only in which recorded scene command iterates the
// mesh and which section-transform provider bakes it). The scene build
// (kSceneBuildRva 0x4CCE70) allocates a 0x400-byte fg node per frame from
// the frame pool and constructs it at the RVA below with
// (parentScene, &parentScene.view@+0x118, x, vec3 PC+0x65C, triple PC+0x668,
// float PC+0x45C, float PC+0x460), then stores it at parentScene+0x1B0.
// Derivation: static capstone disasm of the build's [PC+0x460] float read
// (the only fg-fov consumer in the build region; found by disp-0x460 code
// search) at build+0xD71, and of the ctor + its finisher (0x56DD90 - view
// offset/rotation compose into node+0x150). The ctor's base-class call
// receives THE PARENT VIEW POINTER - the fg node's view starts as a copy of
// the world view; the fg divergence (actor orientation, eye pull) enters
// downstream. The vec3 at PC+0x65C and triple at PC+0x668 read zero live at
// rest (auxiliary offsets; 75/75/60 fov floats sit just before them at
// PC+0x648..0x650).
inline constexpr uint32_t kFgSceneNodeCtorRva = 0x56DC30;
inline constexpr uint8_t kFgSceneNodeCtorPrologue[5] = {0x55, 0x8B, 0xEC, 0x6A, 0xFF};
inline constexpr uint32_t kFgSceneNodeVtableRva = 0xE1846C; // written by the build after ctor
inline constexpr uint32_t kSceneViewOffset = 0x118;   // parent scene's view block
inline constexpr uint32_t kSceneFgNodeOffset = 0x1B0; // parentScene -> fg node (per frame)
inline constexpr uint32_t kScenePcOffset = 0x48;      // parentScene -> PlayerController
inline constexpr uint32_t kFgSceneNodeViewOffset = 0x150; // view matrix the finisher composes
inline constexpr uint32_t kFgSceneNodeFovAOffset = 0x3F0; // from PC+0x45C (default 75.0)
inline constexpr uint32_t kFgSceneNodeFovBOffset = 0x3F4; // from PC+0x460 (the vrfgfov field)
inline constexpr uint32_t kFgSceneNodeBytes = 0x400;

// Hands.CurrentHoldable - THE equipped-weapon pointer, read directly off the
// rig actor (session 21 part 2). Derived live: with hands=594733A0 and the
// equipped weapon=5FAB2F00 known, the weapon pointer occurs EXACTLY ONCE in
// hands+0..0x800, at +0x45C - right after the actor's Base/Owner block
// (hands+0x450 held the pawn = Hands.Base; the weapon's own +0x450 holds the
// hands actor: the attach chain weapon -> hands -> pawn is self-consistent).
// This replaces learned/cache/scan as the primary weapon resolution: the
// trigger-learned object PINNED the old resolver to the previously-fired
// weapon across wheel switches (an unequipped weapon stays vtable- and
// owner-valid), which is why per-weapon profiles only swapped on FIRE in the
// first headset run.
inline constexpr uint32_t kHandsCurrentHoldableOffset = 0x45C;

// ---- UObject identity (session 21) ------------------------------------------
// Derived live via the seam: the equipped weapon actor's +0x28 dword read
// 18009 -> 'Shotgun' through fname_text (+0x2C = the instance number), and
// +0x30 -> a heap object carrying the UClass vtable whose OWN +0x28 is the
// same index with number 0 (the canonical class name). Cross-checked on the
// AHands actor: +0x28 -> 'PlayerHands', class object at +0x30 agrees.
// kUClassVtableRva validates the +0x30 target before it is trusted.
inline constexpr uint32_t kUObjectNameIndexOffset = 0x28; // FName index (+0x2C = number)
inline constexpr uint32_t kUObjectClassOffset = 0x30;     // UClass*
inline constexpr uint32_t kUClassVtableRva = 0xE2F04C;

// ---- Name system (session 20) ----------------------------------------------
// Derived by capstone disassembly of the FName constructor the event scan
// already finds (thin SEH/lock wrapper at RVA 0x70D660 -> worker 0x70D3C0);
// full chain in ENGINE_NOTES "Session 20". GNames is a TArray<FNameEntry*>
// indexed by name index:
//   [kGNamesDataRva] = FNameEntry** Data, +4 = int32 Count, +8 = int32 Max
// FNameEntry: +0x0 int32 index (self), +0x4/+0x8 the 8-byte flags (the
// FindType==2 path ORs 0x4000000 into +0x4), +0xC FNameEntry* hash next,
// +0x10 UTF-16 text in place. An FName field is 8 bytes {index, number}.
// The 4096-bucket string->index hash lives at kFNameHashRva (bonus oracle).
inline constexpr uint32_t kGNamesDataRva = 0x13904EC;
inline constexpr uint32_t kFNameHashRva = 0x1370EC0;
inline constexpr uint32_t kFNameEntryHashNextOffset = 0xC;
inline constexpr uint32_t kFNameEntryTextOffset = 0x10;

// UTF-16 text of a name index, or null (unresolved base / out of range /
// unreadable entry - every dereference is validated, fail-soft). Game thread.
const wchar_t* fname_text(int32_t index);
// GNames.Count (0 if unavailable) - for status lines.
int32_t fname_count();

// Canonical class name of any live UObject, or null. Every step validated:
// obj readable -> obj+kUObjectClassOffset -> UClass vtable at kUClassVtableRva
// -> class+kUObjectNameIndexOffset -> fname_text. The per-weapon profile key
// (session 21). Game thread.
const wchar_t* object_class_name(const void* obj);

struct Symbols {
    // void __thiscall(APlayerController* this, AActor** viewActor,
    //                 FVector* camLoc, FRotator* camRot)
    void* eventPlayerCalcView = nullptr;

    // M6 fire-flow seams; individually nullable (fail-soft - aim just reports
    // which ones resolved and stays off for the rest).
    void* weaponFireStart = nullptr;  // AWeapon::GetPerfectFireStart impl
    void* abilityFireStart = nullptr; // UAttackAbility::GetPerfectFireStart impl
    void* weaponInitDamage = nullptr;  // AWeapon::InitiateDamage impl (telemetry)
    void* abilityInitDamage = nullptr; // UAttackAbility::InitiateDamage impl (telemetry)
};

// Runs all scans, logging each stage. False if anything failed to resolve.
bool resolve(const bvr::pattern_scan::ProcessImage& image, Symbols& out);

// The M6 aim natives only (called by resolve; separate so a rescan is cheap).
// Fail-soft per symbol - each miss is logged and leaves its slot null.
void resolve_aim_natives(const bvr::pattern_scan::ProcessImage& image, Symbols& out);

// The live HorizontalFOV field of the UShockUserSettings singleton, or null.
// Lazy by design: the object exists only after engine init, so this validates
// on every call (null global / dead memory / foreign vtable / dead UObject
// class chain all return null) instead of caching at resolve() time. Goes
// dormant after repeated misses; hfov_scan_rearm() is the only way back.
// Game thread only.
int32_t* hfov_option_ptr();

// Clear the miss counter and wake a dormant settings scan. Call on any event
// that plausibly created the object: a view-state change, a pawn change, a
// level load. `why` is logged.
void hfov_scan_rearm(const char* why);

// True once the settings object is bound, for status lines.
bool settings_bound();

// ---- live-object search -----------------------------------------------------
// Find a live object by its class vtable. There is no static pointer to most
// engine singletons, so the object is located by its fixed-RVA vtable dword.
// The generic, stack-safe, budgeted machinery is core/hooks/heap_scan.h; this
// wraps it with the two-phase policy every callsite here wants.
//
// `accept` is called for every object whose first dword is that vtable and
// decides whether this instance is the one wanted: every UClass also has a
// default object carrying the same vtable, so a plausibility test on the
// object's own fields is mandatory. It runs inside an SEH guard, may read up to
// `needBytes` from the object, must not throw, and MUST NOT LOG OR ALLOCATE -
// MSVC does not run C++ destructors during SEH unwinding, so a fault taken
// while the log mutex is held would wedge logging for the life of the process.
// Hand diagnostics back through `probe` instead and format them afterwards.
using ObjectAccept = bvr::heap_scan::Accept;

// Caller-owned search state. One static per callsite; zero-initialize and keep.
struct ObjectScan {
    bvr::heap_scan::Sweep sweep;
    bool sweeping = false; // in the sliced fallback rather than the heap path
};

struct ScanResult {
    void* object = nullptr; // the accepted instance, or null
    int matches = 0;        // vtable hits
    int accepted = 0;       // accept() said yes; >1 means ambiguous, fail closed
    const char* path = "-"; // "heap" (fast path) or "sweep" (fallback)
};

// Advance one search. Returns true when a search COMPLETED this call and `out`
// is meaningful; false means a sliced sweep is still running, so call again
// next frame. Tries the live-heap-block fast path first and only falls back to
// the region sweep if the object is not in a Win32 heap block.
bool run_object_scan(ObjectScan& scan, uint32_t vtableRva, uint32_t needBytes,
                     ObjectAccept accept, void* user, ScanResult& out);

// Log a one-line summary of a completed search, and with `candidates` every
// recorded candidate and its probe values. Call after run_object_scan returns
// true - never from inside an accept filter.
void log_scan_result(const char* what, const ObjectScan& scan, const ScanResult& r,
                     bool candidates);

} // namespace bvr::b1r::patterns
