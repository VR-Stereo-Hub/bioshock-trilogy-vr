# Engine notes - BioShock 2 Remastered (Bioshock2HD.exe)

Single source of truth for everything we know about **Bioshock2HD.exe** internals.
Sibling of [../bioshock1/ENGINE_NOTES.md](../bioshock1/ENGINE_NOTES.md) (BS1), which also documents
the general derivation recipes in full; this file records the BS2-specific values and the places
where BS2 *differs* - and it already differs at the very first hook.

Rules (same as BS1):
- Every address/offset used by code lives ONLY in `src/game/bioshock2r/patterns.h/.cpp`, and every
  one is documented here with its derivation method.
- NEVER copy a number from the BS1 notes or patterns - same engine tree, different link. Shapes
  transfer; values never do.
- No game-derived content in the repo: findings are summarized here, disassembly stays out.

**And one rule that is the OPPOSITE of copying BS1 (user directive, session 24): BS2 is not
bound by BS1's METHODS either.** BS1's fg/viewmodel FOV counter-modeling, weapon scaling
compensation, and aim workarounds were forced by BS1-specific limitations. BS2 demonstrably
differs where it counts (native FOV slider in its options UI, native dual-wield, inlined event
dispatch that gave us ProcessEvent-by-name hooking). For every BS1 subsystem being brought
over: first check what BS2 does natively, and prefer the cleaner path when BS2 affords one -
only port BS1's compensation machinery once BS2 has proven it needs it.

## PE identity (verified 2026-07-29, session 24)

| field | BioShock 1 | BioShock 2 |
|---|---|---|
| machine | x86 (0x14C) | x86 (0x14C) |
| LARGE_ADDRESS_AWARE | yes | yes |
| ImageBase | 0x10900000 | 0x10900000 (identical) |
| SizeOfImage | 0x1677000 | 0x1FEA000 (bigger) |
| PE TimeDateStamp | 2022-04-13 16:16:54 UTC | 2022-04-13 16:00:37 UTC |
| imports xinput1_3.dll / d3d11.dll | yes / yes | yes / yes |

BS2 was built 16 minutes BEFORE BS1, same engine, same build session. The `xinput1_3.dll`
proxy injection vector works unchanged (verified live session 24: `build:`/`env:` lines in
`%LOCALAPPDATA%\BioshockVR\bs2\bioshockvr.log` on first launch). Steam appid 409720. Game path:
`D:\SteamLibrary\steamapps\common\BioShock 2 Remastered\Build\Final\Bioshock2HD.exe` (D: drive,
unlike BS1 on K:). Observed live base 0x10040000 (rebased - ASLR active, RVAs are the stable
identifiers, exactly as on BS1).

An important build difference with consequences everywhere: **BS2's link went through an
incremental-link style jmp-stub table** (RVAs ~0x0D000-0x40000: five-byte `E9` stubs). Vtable
slots and many call sites point at stubs, not bodies - every derivation must follow one `E9` hop
(`follow_jmp_stub` in patterns.cpp). BS1 has no such indirection. Additionally BS2's optimizer
**inlined code BS1 kept out-of-line** - see the CalcView seam below.

## The CalcView seam (THE session-24 finding)

**`eventPlayerCalcView` exists in BS2 and is DEAD CODE.** The FName-chain scan resolves it
cleanly (RVA 0x395CC0, correct thunk shape, 0x1C param block), the hook installs - and it never
fires, menu or gameplay. Offline byte-scan: BS1's thunk has **29 static E8 callers; BS2's has
ZERO.** The build inlined the event-dispatch glue at every call site (~35 sites reference the
cached FName index global vs BS1's 3).

Each inlined site does, verbatim from capstone disasm of sites at RVAs 0x4D6970 / 0x868759 /
0xA092CF / 0x34D139:

1. build the 0x1C param block on the stack: `{ AActor* viewActor; FVector loc; FRotator rot }`
2. load the 8-byte FName `{ index, number }` from the cached-index global (dwords at
   RVA 0x17D9A08 / 0x17D9A0C) - **BS2's FName constant is 8 bytes** where BS1 passed 4
3. `push 0` (UBOOL global) + the FName -> call `UObject::FindFunctionChecked` (callee pops 12)
   through the jmp stub at RVA 0x20365 -> real body at **RVA 0xB6BA30**
4. push the returned `UFunction*` -> virtual call through **vtable slot 3 (byte +0xC)** =
   `ProcessEvent(UFunction*, void* parms, void* result)` - `ret 0xC`, three stack args
5. read the param block back and consume loc/rot

So the working BS2 seam (shipped in `bioshock2r/camera.cpp`) is:

- **hook `FindFunctionChecked` (RVA 0xB6BA30)** - when `nameIndex == *(RVA 0x17D9A08)`, cache the
  returned `UFunction*`. Zero UObject-layout assumptions; the camera sites re-resolve the name
  every dispatch, so the cache is fresh from the first frame.
- **hook `ProcessEvent` (RVA 0x37A7E0)** - after calling the original, if `fn` matches the cached
  pointer, mutate `parms` (the caller reads the block back afterwards). Equivalent to BS1's
  out-param writes.
- ProcessEvent is resolved at runtime by reading the AShockPlayerController vtable slot +0xC and
  following its stub; the result must equal base+0x37A7E0 and match the documented prologue, or
  the adapter refuses to hook (build-identity gate).

| symbol | RVA | derivation |
|---|---|---|
| `eventPlayerCalcView` (dead) | 0x395CC0 | FName-chain scan, resolved live; kept for the record only |
| PlayerCalcView FName index global | 0x17D9A08 (+0x17D9A0C number) | FName-chain scan (1 wide string, 1 string xref, ctor RVA 0x19C04, 37 global xrefs) |
| `UObject::FindFunctionChecked` | 0xB6BA30 | inlined call sites -> stub 0x20365 -> body; prologue `55 8B EC 64 A1 00 00 00 00` (SEH frame) |
| `UObject::ProcessEvent` (outer) | 0x37A7E0 | controller vtbl slot 3 stub 0x15FBE -> body; prologue `55 8B EC 8B 81 0C 01 00 00`; `ret 0xC` |
| ProcessEvent vtable slot | byte +0xC (slot 3) | read off the inlined call sites (`call [edx+0xC]`) |

Cross-check: BS1's outer ProcessEvent sits at RVA 0x375140 with the IDENTICAL shape (StateFrame
gate at `this+0xF8` there vs `this+0x10C` here, script-disable globals, tail-jmp to the inner
body, `ret 0xC`) - located via BS1's controller vtable slot 3. Same engine, shifted offsets:
never copy, always re-derive.

**ProcessEvent hook discipline:** every script event in the game passes through the detour.
Pre-filter work is two compares; the 1 Hz command-file poll ticks through a `& 0xFF` call
counter. Observed dispatch rates: ~200 calls/s of PlayerCalcView at the loading/menu-adjacent
scenes, ~850/s in gameplay, spikes to ~4500/s during level load. Total ProcessEvent traffic is
far higher - keep the fast path tiny.

## Class vtable RVAs (RTTI walk, runtime-verified)

Derivation: MSVC RTTI walk (find `.?AVClassName@@` TypeDescriptor -> the dword referencing its VA
is COL+12 -> the dword referencing the COL's VA is vtable-4), run offline 2026-07-29; the method
was validated by reproducing all of BS1's known-good vtable RVAs exactly.

| class | RVA | runtime verification (session 24) |
|---|---|---|
| `AShockPlayer` | 0x11197C0 | **VERIFIED** - view actor vtable in gameplay matches; drives `view state: GAMEPLAY` |
| `AShockPlayerController` | 0x1117BF0 | **VERIFIED** (indirectly) - its vtable slot 3 stub resolves to ProcessEvent, and the pre-save menu/load view actor logs it |
| `UShockUserSettings` | 0x11523D8 | **VERIFIED** (session 25) - vtscan found a live heap object with dword0 == base + RVA; consumed by the FOV readback/write (see the FOV section) |
| `APlayerWeapon` | 0x112CC78 | candidate - unconsumed |
| `AHands` | 0x1125478 | candidate - unconsumed |
| `SkeletonInstance` | 0x10D0FC0 | candidate - unconsumed |
| `UGameEngine` | 0x10BD7DC / 0x10BD9E8 | candidates - unconsumed (BS1's console_exec used the second of its pair; verify before calling) |

Runtime self-diagnosis: the camera module logs the observed view-actor vtable RVA on every
change, so a wrong candidate names its own correction from any session log. Observed so far:
0x106EE20 - the MENU/base controller class (BS2's main menu runs CalcView with
`viewActor == pc` of a NON-Shock controller class; identity not yet RTTI-resolved).

## Behavior differences from BS1 worth remembering

- **The main menu does not run PlayerCalcView at all** until a level/menu scene spins up a
  controller (first fire arrives seconds after launch with the 0x106EE20 controller). BS1's
  attract scene fires CalcView at up to ~7800/s from the start. Consequence: nothing driven from
  the CalcView tail works at the BS2 menu - which is why the b2r command poller ticks from the
  ProcessEvent detour instead.
- The graphics-options first-boot screen has a native **Field Of View slider (default 100)** -
  BS2 has an exposed FOV concept BS1 lacked. Session 25 confirmed it is the
  `UShockUserSettings.HorizontalFOV` int (offset below) - and, decisively: **BS2's foreground
  viewmodel follows the world FOV natively** (poking the option re-lensed the drill WITH the
  world, screenshot-verified). BS1's entire fg counter-model (fovA/fovB, kFgEyeComp, vrfgfov)
  exists because BS1's fg rig has a FIXED lens; BS2 does not have that defect, so none of that
  machinery ports. First applied case of the "BS2 is not bound by BS1's methods" directive.
- Flat 6DOF checks (session 24, log-measured via the final-camera heartbeat): `offset 0 0 50` ->
  z +50.0 exact; `simhead 0 20 0` -> pitch 3640; roll 15 -> 2730; yaw residual integer-exact
  (16450 -> 10989 = -5461 = -30.0 deg); sim position (0.10, 0.20, -0.30) m at worldscale 100 ->
  headOff (6.1, 31.0, 20.0) UU, |37.4|, halving exactly at worldscale 50.

## UShockUserSettings and the FOV option (session 25)

**`HorizontalFOV` = `UShockUserSettings + 0x4C`, int32, DEGREES.** BS1's +0x8C does NOT
transfer (it reads 3 here) - derived fresh, as the hard rule demands.

Derivation chain (2026-07-29, live save "Adonis Luxury Resort", option at 100):

1. **Vtable verify first**: `vtscan 11523D8` (the one-shot heap-scan probe, ~3 s Debug walk of
   the full 4 GB) -> 3 matches: 0x008FF29C / 0x008FF304 (stack slots 0x68 apart holding the
   pointer - the BS1-documented stack false-positive shape) and **0x0D368F80, a real heap
   object** whose dword0 == base + 0x11523D8. Candidate promoted to VERIFIED before anything
   trusted it.
2. **Object layout vs ini**: `hexdump` of the object against `[ShockGame.ShockUserSettings]` in
   `%APPDATA%\BioshockHD\Bioshock2\Bioshock2SP.ini`. Config properties visibly mirror the ini:
   the four volume 100s at +0x64..+0x70, Sensitivity=50 at +0x78, Brightness/Contrast/Gamma
   0.5/0.5/1.2f at +0x80..+0x88, MouseSensitivity=4.0f at +0xA8. The clincher:
   `MouseIconScale=10` immediately precedes `HorizontalFOV=100` in the ini, and the object has
   **+0x48=10 followed by +0x4C=100**. (The object spans ~0xE0 bytes; another UObject starts at
   +0xE0. +0x44 read 105 - some other field, do not confuse.)
3. **Consumed-copy proof (poke + img-diff)**: `pokeaddri <obj+0x4C> 130` -> screenshot
   mean-abs-diff **8.99, 39.3% pixels changed** vs baseline (BS1's session-4 consumed-copy
   evidence was 11.09/44.2%); `memrestore` -> **1.34 / 5.0%** = the ambient-animation floor.
   Renderer-consumed **per frame**, no options APPLY. The ini value is only read at boot -
   matching BS1's "editing the ini does not propagate" finding.
4. **No code clamp in range**: poke 150 -> monotonic widening (9.97/43.9% vs base, 6.91/34.0%
   vs the 130 shot). The UI slider's own range is still unrecorded (menu work needs the user);
   irrelevant for the write path since `suggested_hfov_deg` caps at 160 and Quest-class
   headsets ask ~104-115.
5. **Native fg verdict** (the policy gate): in the 100-vs-130 screenshot pair the drill
   viewmodel SHRANK with the wider lens exactly like the world - BS2's foreground renders
   through the world FOV. BS1's fixed-lens fg defect does not exist here.

Production locate (patterns.cpp `hfov_option_ptr`): no static pointer assumed (BS1 precedent -
its .data "roots" were coincidental); heap-scan for the vtable, cache the instance, revalidate
`dword0 == base + RVA` every call, 2000 ms rescan rate limit, **DORMANT after 3 straight
misses** (the b2r scanner bakes in the BS1 session-22 dormancy lesson from day one), re-armed
on view-state changes. Scan cost measured ~3 s (Debug, gameplay heap) - one-shot per session
in practice.

Consumers: `calcview_tail` readback -> `vr::set_rendered_hfov` (claim == rendered, menus
included; null object claims 0 = core's explicit fallback signal); the gated write block
(`vrfov` forced-headset / `gfov` manual, both DEFAULT OFF, strict-gameplay + save/restore,
stale-restore from the ProcessEvent detour because BS2 has no scenedraw hook); the 1 Hz
heartbeat's `fov=` field; `fovaudit`.

## Scene-draw architecture (session 26) - SequentialReentry WITHOUT single-threading

**The headline: BS2's SequentialReentry runs on the THREADED substrate.** BS1 needed
structural single-threading (the flush-point hook) because its game thread parks in a
racy kick-and-wait handshake per frame; BS2's Draw path has NO such handshake - the
game thread fills a cursor-based command ring and the per-tick sync runs once AFTER the
doubled call - so a second Draw simply enqueues a second scene + present command.
Flat-proven: pulse and continuous doubling, `presents/s == 2 x draws/s` exact, per-eye
camera delta IPD-exact, zero faults. None of BS1's 1t/flush-point/drain-guard machinery
ports. (Second applied case of the "BS2 is not bound by BS1's methods" directive, after
the session-25 fg verdict.)

### The derived render substrate (all RVAs live-verified 2026-07-29, session 26)

| symbol | RVA | role + derivation |
|---|---|---|
| `UGameEngine::Draw` | **0x4EE8D0** | THE SequentialReentry seam ("build"). Aligned-stack + SEH prologue `53 8B DC 83 EC 08 83 E4 F0` (BS1's build family), `ret 0x10`, ECX = engine this (stored [ebp-0x84]), arg1 = viewport -> edi (+0x48 read at tail). Render-command ring cursors at `this+0x118/+0x11C` (BS1's exact offsets). Derivation: live kick2 deep-chains -> offline capstone walk -> the fn sits at **engine vtable 0x10BD7DC slot +0x118** (stub 0x139D -> body; the session-24 RTTI candidate, now consumed) - the install path re-verifies that whole chain from the image every time. ZERO static E8 callers (virtual dispatch). Live: draws/s == presents/s == calcIn/s exactly; ~0.6-0.8 ms pass-through. |
| gameplay Draw caller | ret **0xCD5D7B** | the ONLY caller observed over every gameplay beat (count == presents): the 13-instr virtual-dispatch fn at 0xCD5D60 (`call [vtbl+0x118]`, ret 4), called from the viewport iterator 0xCD2C40 (IsWindow-gated, viewport type == 1). Pass 2's deny-by-default gate: double ONLY Draws from this ret. |
| `FContentStreamingManager` view hand-off | 0x5C7C80 | **NOT a frame submit** - it wears BS1's submit shape exactly (TLS frame-id spin-wait head, camera globals, `ret 0xC`, tail event kick) and that shape-match was session 25's mislabel. `this` = the streaming mgr global; called from the Draw tail (ret 0x4EF541, gated on mgr non-null + ring cursors unequal) and from the no-world frame fn (ret 0x4F9AB9 in fn 0x4F6E70 - the load/menu path). Prologue `55 8B EC 64 A1 2C 00 00 00`. Hooked as telemetry only (fires 1:1 with draws in gameplay). |
| `FEventWin::Trigger` | 0xB81050 | the event class's signal: `push [ecx+4]; call [IAT SetEvent]; ret` - handle at +4, vtable 0x11E4FAC slot 2 (slot 4 = Pulse, +0x14 = timed wait, +0x18 = wait INFINITE). The engine's ONLY static kernel32!SetEvent path; virtually dispatched everywhere. `reentry kick2` hooks it to sample engine-side call sites. |
| Flash/FMOD lock-step runnables | Run = 0x67DAD0 | `FThreadLockStepRunnable::Run` (vtable 0x10D53D4 slot 1; abstract work virtual at +0x14): `while (![this+0xC]) { [this+4]->wait(); vtbl[+0x14](); [this+4]->signal(); }`. Shared by `FFlashUpdateRunnable` (vtbl 0x10D53F8) and `FFMODUpdateThread` (vtbl 0x11FB574) - THESE are the two once-per-present SetEvent threads the kick sampler sees, NOT render threads. Flash kick fn 0x67DB30 (gate obj [0x17F7794], work at +0x10, dt at +0x14, wake via 0xBB1950); gate methods: 0xBB1400 wait, 0xBB1610 signal-done, 0xBB1420 execute-INLINE-if-no-thread. |
| hw-thread quotient | num **0x149760C** / div **0x1497798** | BS1's quotient family, ONE out-of-line copy at 0x67DAB0 (`return [num]/[div] > 1`), gating the Flash kick in the engine-tick fn (~0x500452 region, ret site 0x500546). Not consumed by the mod - kept for the record. |
| streaming camera globals | loc 0x17F5D7C, rot 0x17F5D90 | live-verified via hexdump: mirror the CalcView camera exactly (the streamer's view info). Frame-id pair 0x17F5D8C/0x17F5DA0 with BS1's high-bit-done convention (`0x800000BA` parked in steady state - backpressure slots, not per-frame counters); streaming mgr global 0x17F5D54; its FEventWin kick obj global 0x17F5D60. |
| render-thread sync pair | FEventWin globals 0x1A69294 / 0x1A69298 | the endframe fn 0x501EA0 Triggers [0x1A69294] once per present (kick2 site 0x5029BA); static reader 0xB929F2 = render-thread loop candidate. Presents run on a dedicated thread (presentTid != drawTid, live). BANKED, UNCONSUMED - only needed if threaded doubling ever proves unstable and a 1t fallback must be derived. |
| Draw camera-source probe | viewport+0x48 -> camActor +0x1EC loc / +0x1F8 rot | the Draw head copies these cached fields into its locals (telemetry probe in the detour). NOT the final view camera (loc differs from CalcView's output) - the live camera enters via the CalcView dispatch below. |

### Frame protocol (live-measured)

Game thread per tick: engine tick fn (~0x500452, SEH state machine) -> viewport iterator
0xCD2C40 -> dispatcher 0xCD5D60 -> **virtual `UGameEngine::Draw`** (fills the command
ring; **PlayerCalcView dispatches EXACTLY ONCE inside every Draw** - live: calcIn ==
draws every beat; the script-VM chain 0x4D3400 -> 0x4D06F0 -> 0x4D1080 carries the
inlined dispatch, 0x4D1080 references the FName index global 0x17D9A08) -> Draw tail:
streaming view hand-off + Flash kick-if-idle -> back in the tick fn: threaded Flash
kick + endframe 0x501EA0 (render event Trigger + FMOD kicks + QPC stats). A dedicated
render thread drains the ring and Presents (~200/s == draws/s); Flash + FMOD lock-step
threads each SetEvent once per present. PlayerCalcView also dispatches ~3x per frame
OUTSIDE Draw (other consumers).

### SequentialReentry results (flat gates, 2026-07-29)

- **Pulse** (3x): second Draw does real work (call2 4.5-5.0 ms vs 0.6-0.8 ms
  pass-through), presents == draws + seconds exactly in the same beat, zero faults,
  instant 1:1 recovery.
- **Continuous** (yaw 30): every gameplay Draw doubled - 107/107, presents 214/s == 2x,
  game tick halves (204 -> 107 draws/s), `reentry off` recovers instantly. 2nd-pass
  CalcView hits == seconds exactly (655/655): the doubled Draw re-dispatches CalcView
  once and the ProcessEvent-seam replay applies the pass-2 camera. (PrintWindow
  phase-determinism catches the FIRST present of each pair on this game - the yawed
  frame never appears in flat screenshots; the numeric gates carry the proof.)
- **Stereo**: `2nd/s == draws/s`, `presents/s == 2x draws/s` (99->198 ... 105->210),
  per-eye camera delta `|d| = 6.30 UU == ipd/1000 x worldScale` EXACT, L/R symmetric
  around the base along the full-rotation right axis, zero skips/faults/tag-ring
  resyncs. `vrstereo on` one-toggle (camera mode -> stereo, NO 1t rung) logs READY.
- Load safety: pass 2 is deny-by-default on the single gameplay caller ret 0xCD5D7B +
  `calcview_silent(400)` skip + present-stall skip; loaders/menus can never double.

### Frame inspector on BS2 (dumpframe decode-check, session 26)

`dumpframe full 2` under live stereo captured both halves of an SR pair - two
consecutive present windows, each a FULL depth-tested scene (~808/812 draws, ~630 cb0
blocks) - dump-level corroboration of the double render. KNOWN GAP: BS1's
`tools/decode-framedump.ps1` tangent recovery (cb0 floats 12..18 layout) decodes ZERO
screen-ray blocks on BS2's cb0 layout, so `hud::fov_watch` / `fovaudit`'s "rendered"
side stays `no decoded scene tangents` on this game. The BS2 cb0 view-proj layout needs
its own derivation pass when something consumes it (the fov readback claim is already
verified by other means; the script's $fgBakeRvas are also BS1-only - fgBakeStacks
reads 0 here, cosmetic).

### Session-25 recon corrections (banked so nobody re-trusts them)

- 0xB81020 (E9 stub 0xF01A) = thread-object Suspend/Resume (tail-jmps
  SuspendThread/ResumeThread by flag), not a SetEvent wrapper.
- 0xCDB917 (stub 0xD9BD) = crit-section once-init helper; the second FF15 SetEvent
  site 0xCDB9BB is CRT encoded-pointer once-init machinery.
- "Pump candidates": 0xCF3EE0 = CreateSemaphoreW object ctor; 0x7C3E40 = a
  WFSO(INFINITE) helper over the handle pair at 0x1803A14 with callers
  0x7A3FD6/0x7A4563 - neither is the render pump.
- The FF25 import jmp-thunks (SetEvent's at 0xF3E494) have zero callers (dead linker
  thunks) - an E8-to-thunk census finds nothing.

### Live-sampling instruments that produced all of this (b2r scenedraw.cpp)

`reentry kick` = process-wide kernel32!SetEvent sampler, BS1's shape + a BS2 extension:
on FIRST insertion of a (tid, ret-RVA) key the slot deep-captures up to 3 further
call-preceded exe return RVAs from the sampling thread's stack (the FF15 wrapper
methods mask the direct caller on this game); reports include the window's presents
delta so cadence reads as a ratio. `reentry kick2` = the same sampler hooked on
`FEventWin::Trigger` itself - its return address IS the engine-side virtual call site
(this one split the game thread's five per-frame kicks and cracked the architecture).
`reentry calcstack` = one-shot call-preceded stack scan at the next CalcView dispatch.
Menu-controller note: the 0x106EE20 view-actor vtable is plain **APlayerController**
(RTTI, session 26) - the menu scene runs the base class, closing the session-24 gap.

## Derivation recipes (shared with BS1 - short form)

- **FName-chain event scan** (core `pattern_scan::find_event_function`, game-agnostic): wide
  UTF-16 event name -> imm32 xref -> forward <=96 bytes to the `E8` FName-ctor call -> past it to
  the `89 0D` store = cached index global -> global xrefs (minus init site +-200) -> backward
  prologue walk `CC CC CC 55 8B EC`. On BS2 remember the chain's first PROLOGUE-VALID candidate
  is not necessarily a LIVE function - check callers before trusting a resolved thunk.
- **RTTI walk**: see the table intro above; ~40-line offline script, disposable (scratchpad).
- **Static caller census**: for any absolute-addressed function, scan the exe for `E8` opcodes
  whose rel32 lands on it. Zero callers on a function the engine "must" call per-frame = the
  dispatch is inlined or dynamic - look for the FName-global xrefs instead. (This is the check
  that cracked the dead-thunk mystery; do it BEFORE hooking, not after.)
- **Scan hygiene** (LAA: actors allocate above 2 GB, so any future heap scan walks the full 4 GB
  range; no scans on a cadence - BS1 needed backoff AND dormancy; prologue-gate every hook).

## Resolution, lens laws and the viewmodel: what BS1 hit, and how to check BS2 (banked session 28)

**Nothing here is a BS2 measurement.** It is the BS1 defect class written up as a checklist,
because all three of BS1's session-27/28 bugs came from ONE cause and BS2 is likely to have the
same shape. Per the standing policy (BS2 is not bound by BS1's methods): check whether the defect
EXISTS before porting any of the cure, and **derive every number fresh** - the never-copy rule
applies to lens constants exactly as it does to addresses.

### The BS1 cause, in one sentence

A frame carried TWO perspective lenses with OPPOSITE aspect conventions - the world pass fixed its
HORIZONTAL half-tangent (`tanH = tan(option/2)`, `tanV = tanH*h/w`) and the foreground/viewmodel
pass fixed its VERTICAL (`tanV = tan(fgFov/2)*3/4`, `tanH = tanV*w/h`) - and because they coincide
EXACTLY at 16:9, every measurement taken at 1920x1080 for six sessions saw one lens and could not
tell the two laws apart.

### The three symptoms it produced, all the same defect

1. **World warps on head turn at non-16:9.** The live fov watch sampled the FIRST decodable scene
   draw, the fg draws come first, so off 16:9 it reported the VIEWMODEL lens as the world lens. The
   mismatch verdict then latched ON during normal gameplay and the projection claim was substituted
   with the viewmodel frustum - a 1.84x under-claim, so the compositor mis-reprojected every
   rotation by `atan(k*tan(d)) - d`.
2. **Viewmodel moves with the head.** ONE projection layer carries ONE fov claim for the whole eye
   image, so while the two lenses differ only one of {world, viewmodel} can be geometrically
   correct. Fixing (1) moved the same 1.78x error from the world onto the hands. Only MATCHED
   lenses make both right.
3. **A FOV policy derived from the wrong law.** BS1's "129.5 circumscribing" preset value solved
   `tan(option/2)*9/16*aspect = tan(H/2)`, which is the law that turned out to be the FOREGROUND's.
   Under the real world law the option needs no aspect term at all.

### The checks for BS2, in order, each cheap

1. **Does BS2 even have two lenses?** Run `dumpframe full` at a NON-16:9 backbuffer and decode with
   `tools/decode-framedump.ps1` (it applies the structural zero-slot validation; the live watch is
   the thing that got this wrong on BS1). **One cluster = BS2 does not have the split and symptoms
   1-2 cannot occur.** There is real reason to expect this: session 25 measured that BS2's
   foreground follows the world FOV NATIVELY (poking the option re-lensed the drill viewmodel with
   the world), which is why none of BS1's fg counter-model was ported. If that holds off 16:9 too,
   BS2 is already in the state BS1 had to be fixed into.
2. **If there are two clusters**, identify which is the foreground before believing either: toggle
   whatever writes the fg lens (BS1 used `vrfgfov on/off`) and re-dump. The cluster that MOVES is
   the foreground. Do not infer it from draw counts or cb tier alone - on BS1 the 576/832 tiers were
   shared between both clusters.
3. **Derive BS2's world law from two backbuffers, not one.** Sweep the FOV option at 16:9 AND at a
   square-ish backbuffer. If `tanH` is unchanged by aspect the option is a true horizontal (BS1's
   world law); if `tanV` is unchanged, it is a 16:9-referenced horizontal. A single aspect cannot
   distinguish them - that is the trap that cost BS1 two sessions.
4. **Check the claim against the WORLD lens with a live session.** The submitted side does not exist
   flat (`swap=0x0`, `submitted tanH=0.000000`), so this needs the headset. And note the BS1 lesson:
   `src=live` on the audit line means "this came from the watch", NOT "this is correct". A source
   tag is not a correctness proof.
5. **If BS2 needs a fg match at all**, the aspect-general constant is `(4/3)*(h/w)`, NOT `0.75`.
   `0.75` is that expression evaluated at 16:9. BioVRDev reached the same generalization
   independently for BS1 (`2*atan(tan(fov/2)*(4/3)/aspect)`, RESEARCH.md) - but the `4/3` and `3/4`
   in it are BS1's foreground spec, measured from its cb0 fingerprint. **Measure BS2's own fg spec
   from its own dump before reusing those numbers.**
6. **Grep BS2 for hardcoded aspect constants** before shipping any non-16:9 support. On BS1 the
   coupled ones were the fg match constant, the bone solve's world model AND its fg model, and the
   audit's option-derived fallback - four places, all `9/16` or `0.75`, all silently correct at
   16:9. `bioshock2r/camera.cpp`'s `fovaudit` already carries the same `9/16` flat fallback.

### Infrastructure BS2 inherits for free (core, already shared)

`core/gfx/hud_capture.cpp`'s watch is game-agnostic and already fixed: it stride-samples up to 8
cb0 heads across the whole pass, clusters them, publishes the majority as the world lens and the
runner-up as the fg lens, enforces the structural zero-slot checks and a 500 ms age gate at the
source, and refuses a round that lacks a clear majority or failed to span its pass.
`bvr::hud::fov_watch_fg` / `fov_lens_count` / `backbuffer_dims` are available to b2r now.
**So on BS2, `fovaudit` reporting `lenses=2` off 16:9 is the alarm, and `lenses=1` is the
all-clear** - the diagnosis BS1 lacked is already in place before BS2 needs it.

### Resolution changes are dynamic on BS1 - keep it that way on BS2

Everything aspect-dependent reads the live backbuffer every frame rather than caching at init:
`backbuffer_dims` is published unconditionally at the head of the present detour (deliberately
BEFORE the letterbox watch's format whitelist and staging allocation, so lens correctness cannot
depend on an unrelated detector managing to allocate), the fg match constant is recomputed per
CalcView, the bone solve reads it per solve, and the XR claim derives from the rebuilt swapchain
dims. A user changing resolution mid-session or via the ini therefore needs no relaunch for the
lens math to follow. BS2's `SETRES` situation may differ; BS1's viewport-Exec `SETRES` FAULTS, so
its ini lane is primary.

### THE SETTLED POLICY that came out of all this (BS1, 2026-07-30) - the part to actually apply

The section above is the mechanism and the checks. This is the conclusion, and it is the single
most valuable thing to carry, because it retires a reflex rather than adding a feature.

**Set the ASPECT with the resolution lane, and leave the FOV option alone.**

The user had been raising FOV for sessions, and their own account of why says it best: *"I was
just changing the FOV since I wasn't able to put the screen in full view and have no black bars.
But now since I can change the resolution however I want then it's good."* **Those black bars were
an ASPECT problem and cranking the FOV was compensating on the wrong axis.** A headset eye is
roughly square; a 16:9 render claimed at 16:9 fills a wide short rectangle inside it and leaves
unfilled bands top and bottom, and the only way to cover them by widening FOV is to over-render
horizontally and throw the pixels away.

Match the render ASPECT to the eye instead - a square backbuffer - and the claimed frustum is the
right SHAPE, so a sane FOV fills the eye exactly. Under the corrected world law, option **100 at
2048x2048 renders 100x100 deg against a Quest 3 eye of roughly 100x96**, which is very nearly an
exact fill. **That works by arithmetic, not by luck**, and it is why the preset now writes no FOV
at all. `vrfov`/`gfov` survive as manual levers, default off.

So the resolution lane is not just a sharpness feature - **it is the mechanism that made the FOV
write unnecessary.** Carry the policy, not the numbers: derive BS2's own world law first (check 3
above), because if BS2's option is a 16:9-referenced horizontal rather than a true horizontal, the
same square backbuffer gives a different FOV answer.

**Corollary for the viewmodel/model trims.** Once the lenses are MATCHED, the per-hand model trims
and the bone solve's aspect terms are correct at every resolution rather than only at the one they
were tuned at - which is why BS1's hands stopped drifting with the head. If BS2 needs any trim
values, tune them AFTER the lens question is settled, or you bake the lens error into the trims
and they stop being portable across resolutions. `xrEnumerateViewConfigurationViews` is still
never called on either game; it is the missing input for a derived render target
(`recommendedImageRect`) and is why a runtime-side resolution slider does nothing for this mod.

## Carries from BS1 session 30 (2026-07-30) - one of these is a LIVE LATENT BUG in BS2

### 1. BS2 HAS THE FROZEN-PITCH BUG TODAY. Six lines fix it.

BS1's game-breaking wrench bug was that the ENGINE's own view pitch froze and melee aimed with it.
**BS2 reproduces the exact preconditions and lacks the fix.** Both halves are present:

- `src/game/bioshock2r/camera.cpp` writes the rotation with the SAME asymmetry as BS1:

```cpp
int32_t gameYawUnits = rot->yaw;
rot->pitch = a.pitchRad * kRotUnitsPerRadian;   // ABSOLUTE from the head
rot->yaw   = gameYawUnits + residualUnits;      // RELATIVE to the engine's own
```

  Yaw keeps the engine's value; pitch discards it unread.
- The same file calls `bvr::input::publish_vr_gameplay(...)`, which arms the **shared core** pitch
  kill in `xinput_bridge.cpp` - so BS2's composed right-stick Y is zeroed exactly as BS1's was.

Together those mean BS2's engine-side view pitch can never change and nothing ever reads it. It
parks at whatever value it last held, for the whole session.

**BS2 has a DRILL**, which is melee. Expect the same symptom: hits landing on the floor, worse
when looking down, fine against a wall approached level, guns unaffected.

**The fix is already in shared core** - the servo replaces the hard zero with a proportional term
whenever a `publish_pitch_error()` is fresh, and **fails open to `ry = 0` when nobody publishes**,
which is precisely why BS2 silently gets the old behaviour today. BS2 needs only the publisher,
mirroring BS1's block and placed the same way - immediately BEFORE the `rot->pitch` overwrite,
because one line later the error is identically zero:

```cpp
int32_t headPitchUnits = lroundf(a.pitchRad * kRotUnitsPerRadian);
int32_t errUnits = wrap_rot(headPitchUnits - rot->pitch);
bvr::input::publish_pitch_error(errUnits / kRotUnitsPerDegree);
```

This is NOT a case where "BS2 is not bound by BS1's methods" argues for a different design. It is
the same defect, in shared code, with a fix that writes no engine memory. Verify the sign
in-headset (`vrinput pitchservo invert` flips it) and check the residual, which on BS1 settles at
4-8 degrees because the proportional stick falls under the game's own deadzone near convergence -
BS2's deadzone may differ.

**How to confirm it before fixing:** the `camera:` heartbeat prints `rot` before the overwrite, so
it reports the engine's own belief. Turn your head up and down for thirty seconds. If the pitch
field never moves, the bug is live.

### 2. The gameswf classifier changes are SHARED, so BS2 inherits them untested

`src/core/gfx/hud_capture.cpp` is core, so both fixes below already apply to BS2 with no BS2-side
testing at all. Check them the first time BS2 renders a HUD in stereo:

- **Post-FX is now discriminated by BIND FLAGS, not by a size match.** The old rule passed a
  post-tonemap draw in-frame when `srv0 dims == target dims`, which is degenerate at a square
  render target - and the policy above makes a square backbuffer the RECOMMENDED configuration, so
  BS2 will hit this the moment it follows the resolution advice. Measured on BS1 at 2048x2048:
  `postFxRejected=1604161` against `postFx=2` genuine, ~30 HUD draws per interval leaking
  in-frame, 43% of them stranded onto the panel and 57% into the eye image - routed by draw ORDER,
  not by the classifier. Now requires `BIND_RENDER_TARGET` on the source. `vrcine postfx size|rt`
  restores the old rule for an A/B.
- **Full-screen effects default to the PANEL again**, and the reason generalises: the health and
  EVE bar COLOUR fills are textureless 5-vertex gameswf quads, identical to the "effect" fill by
  every test the classifier can apply, so routing effects in-frame sent the bar fills into the eye
  image and left the bars looking empty. **If BS2's HUD has bar-style fills, it has the same
  collision.** Watch `effectsInFrame` in `vrcine status`: a count advancing by a small fixed number
  every interval with nothing on screen is HUD, not an effect. On BS1 it was exactly 2. Two bars.

Carry the deeper point too: these draws are authored in gameswf **stage space**, so routing one
in-frame can never make it cover the eye - it makes it stage-sized inside it. Any BS2 attempt at
full-screen effects has to change GEOMETRY, not the render target.

### 3. `-> HANDLED` from an engine `Exec` proves nothing about whether a `set` landed

BS1's `console_exec` builds an `FOutputDevice` stub that returns 0 from the engine's log filter to
suppress output. A `set` naming a wrong class or property, or writing a class default the live
object never re-reads, therefore logs **identically** to one that works. This cost BS1 a whole
false belief: `set GamepadPlayerInput SoftLockOnRadius 0` had been logged as HANDLED every five
minutes since session 22 and was never doing anything - proven by setting the radius to 5000
instead of 0 and feeling no difference.

**If BS2 ever gains an engine-SET path, verify by EFFECT, not by the return value.** The technique
that worked: set the value absurdly rather than to the target, and see whether behaviour changes
at all. BS2's ProcessEvent-by-name seam may be a better route than `Exec` here - worth checking
before porting the Exec machinery at all.

### 4. Probe hooks: the arg count must equal `ret imm / 4`

Hooking an implementation with the wrong stack-arg count returns with a misaligned stack and pops
a `Run-Time Check Failure #0 - ESP was not properly saved` dialog. It writes **no crash dump** (RTC
is a Debug compiler check, not an SEH fault, so it bypasses the crash handler), and force-killing
the game while that modal dialog is up can leave the display mode unrestored - press Abort on the
dialog instead. Disassemble and read the first `ret` before hooking anything new; BS1's verified
table is in `docs/bioshock1/ENGINE_NOTES.md` session 30.

## Carries from BS1 session 31 (2026-07-31): swing-to-attack, and what BS2 gets free

The gesture ships on BS1 and is accepted in-headset ("I tested it and it's perfect"): a fast
right-hand motion composes a full RT pulse while the wrench is equipped, in addition to the
trigger. Threshold 3.6 m/s, re-arm 1.0, cooldown 300 ms, pulse 120 ms, delay 0.

### 1. Most of it is CORE, so BS2 inherits it by existing

`core/input/swing.{h,cpp}` is game-agnostic: the sample intake, the velocity differencing, the
threshold/hysteresis/cooldown, the pulse, the `vrinput swing` command surface and the overlay
block are all shared. `core/vr/openxr_input.cpp` publishes the sample, and
`core/input/xinput_bridge.cpp` composes the pulse. **None of that needs touching for BS2.**

**What the BS2 adapter has to supply is exactly one line**, next to its existing
`publish_vr_gameplay` call in `b2r/camera.cpp` (session 26 put that at the CalcView tail):

```cpp
bvr::input::swing::publish_gate(strictGameplay && <the melee weapon is equipped>);
```

The gate fails closed and expires after 500 ms, so **until that line exists BS2 simply has no
gesture** - nothing to disable, no risk, no half-state. That is the whole port.

### 2. The gate is the ONLY hard part, and BS1's answer probably does not port

BS1 gates on `aim::weapon_key_is("Wrench")`, which reuses the per-weapon profile key that
`update_weapon_profile` maintains off `Hands.CurrentHoldable`. **Do not assume BS2 has that
rig read** - the per-weapon profile machinery is BS1 aim work that BS2 has not needed.

Take the "BS2 is not bound by BS1's methods" directive here: BS2 has a **ProcessEvent-by-name
seam** (the session-24 finding), which is a far more natural place to learn the equipped
weapon than a heap read - a weapon-change or equip event by name gives the identity directly.
Look there before porting any of BS1's holdable resolver.

**Whatever source you use, the gate must be an IDENTITY test.** A swing composes RT, and RT
with a gun in hand is a shot. BS2 makes this sharper than BS1, not softer:

- **BS2 has native DUAL WIELD** - a plasmid in the left hand and a weapon in the right, at the
  same time. So "which hand swung" matters in a way it never did on BS1, and a left-hand
  gesture must not compose the right trigger.
- **BS2's melee is the DRILL, and the drill also has a sustained fire mode.** A pulse that is
  right for a discrete wrench swing may read as a stutter on a spin-up weapon. Measure what a
  drill attack actually wants before reusing 120 ms.
- BS2 has more melee-ish states than BS1's single wrench, so enumerate them before writing the
  predicate rather than assuming one class name covers it.

### 3. Two BS1 measurements that are probably NOT true on BS2 - re-derive, do not copy

- **A 120 ms synthetic RT pulse fires the weapon** (BS1, flat-measured: two pulses took the ammo
  6 -> 4), and the pulse reached the engine's fire path 8-11 ms later. BS2's input pipeline is a
  different link of the same engine tree; re-measure with `vrinput test trig r 255 <ms>` and the
  ammo counter before trusting any pulse width.
- **`XENON_RT = SwitchAndFireWeapon`** on BS1 means the first trigger pull can switch hands
  rather than fire. BS2's bindings are its own; check `User.ini` before assuming.

### 4. The threshold is per-PLAYER, not per-game

3.6 m/s is this user's swing on Quest 3 Touch. It is not a BS2 constant, and it is not a
constant for anyone else either. Ship the shared default, then read `vrinput swing status`'s
peak-speed-since-last-call over a few real swings and set the threshold under it.

### 5. The flat test seam ports for free, and one BS1 trick may not

`vrinput swing sim <peak> [humpMs] [reps]` is core, so BS2 gets the whole flat suite (gate,
cooldown, hysteresis, wheel suppression, off switch) with no headset. What may not port is
BS1's gate-forcing trick: `vraim wkey sim Wrench` sets the profile key the BS1 gate reads, which
is how the detector was verified flat with a pistol in hand. If BS2's gate reads something else,
give it its own forcing command - flat coverage of the gesture depends on being able to open the
gate without a human equipping a weapon (weapon switching still cannot be driven flat on either
game; the radial needs a person).
