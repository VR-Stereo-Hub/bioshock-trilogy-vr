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
// skip (this is why `reentry 1t on` arms the drain hook BEFORE poking).
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

struct Symbols {
    // void __thiscall(APlayerController* this, AActor** viewActor,
    //                 FVector* camLoc, FRotator* camRot)
    void* eventPlayerCalcView = nullptr;
};

// Runs all scans, logging each stage. False if anything failed to resolve.
bool resolve(const bvr::pattern_scan::ProcessImage& image, Symbols& out);

// The live HorizontalFOV field of the UShockUserSettings singleton, or null.
// Lazy by design: the object exists only after engine init, so this validates
// on every call (null global / dead memory / foreign vtable all return null)
// instead of caching at resolve() time. Game thread only.
int32_t* hfov_option_ptr();

} // namespace bvr::b1r::patterns
