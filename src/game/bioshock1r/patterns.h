#pragma once
// BioShock 1 Remastered signature table. This header and patterns.cpp are the
// ONLY files allowed to contain raw addresses/offsets for this game (see
// ARCHITECTURE.md). Every entry is documented in docs/ENGINE_NOTES.md with
// its derivation method.

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
// on every call (null global / dead memory / foreign vtable all return null)
// instead of caching at resolve() time. Game thread only.
int32_t* hfov_option_ptr();

// Find a live object by its class vtable. There is no static pointer to most
// engine singletons, so we scan committed private memory for the fixed-RVA
// vtable dword - the technique that found UShockUserSettings, reused for the
// M7 AHands viewmodel.
//
// `accept` is called for every object whose first dword is that vtable and
// decides whether this instance is the one wanted: every UClass also has a
// default object carrying the same vtable, so a plausibility test on the
// object's own fields is mandatory. It runs inside the scan's SEH guard, may
// read up to `needBytes` from the object, and must not throw. The first
// accepted instance wins; `outMatches` reports how many vtable hits were seen
// (0 = the class is not instantiated yet, many = the filter is doing real work).
using ObjectAccept = bool (*)(void* obj, void* user);
void* scan_for_vtable_object(uint32_t vtableRva, uint32_t needBytes, ObjectAccept accept,
                             void* user, const char* what, int* outMatches);

} // namespace bvr::b1r::patterns
