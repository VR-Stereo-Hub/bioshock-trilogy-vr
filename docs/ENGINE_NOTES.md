# Engine notes - reverse-engineering knowledge base

Single source of truth for everything we know about BioshockHD.exe internals. Every signature,
offset, and hook point used in code is documented here **with its derivation method**, so it can
be re-derived after a game patch. Never paste decompiled UnrealScript or game code here -
summaries and struct layouts only.

Game build reference: `BioshockHD.exe`, 21,214,720 bytes, linker timestamp 2022-04-13
(Steam buildid 8552765, depot 409711). Update this line when Steam ships a new build.

## Process / module layout

- 32-bit x86, PE32, sections `.text .rdata .data .rsrc .reloc`.
  **LAA flag: YES** (Characteristics 0x0122 - verified 2026-07-23 via `tools/check-laa.ps1`).
  4 GB address space available for stereo render targets.
- **D3D11 renderer confirmed live at runtime** (2026-07-23, first Present hook log):
  feature level 0xB000 (11_0), backbuffer DXGI format 28 (R8G8B8A8_UNORM), 2560×1440,
  `windowed=0` - the game defaults to **exclusive fullscreen** (relevant to DR-7: borderless
  test still pending). D3D9 fallback path exists in the binary but is not the default.
- Renderer single-threaded by default (`UseMultithreadedRendering=False` in Default.ini).
- **Boot sequence pauses while the window is unfocused** (observed 2026-07-23: launched in the
  background, the game rendered frames but never advanced past the intros - no PlayerController,
  no CalcView calls - until the window was foregrounded). Automated tests must foreground the
  window (`SetForegroundWindow`) before expecting gameplay-side hooks to fire.
- UI = Flash .swf via embedded gameswf (source path `...\d3ddrv\src\gameswf` in exe strings).
- Havok 2012.2.0 r1 static; FMOD Ex via fmodex.dll; Bink 2 via bink2w32.dll.

## Signature / symbol table

| Name | Pattern / method | Module | Build | Found by | Status | Date |
|---|---|---|---|---|---|---|
| `APlayerController::eventPlayerCalcView` | FName-chain: find wide string `"PlayerCalcView"` → its FName-init xref → `89 0D` (`MOV [imm32], ECX`) store of the FName index → walk xrefs back to MSVC prologue `CC CC CC 55 8B EC` | BioshockHD.exe | 2022-04-13 | ported from itsloopyo/bioshock-remastered-headtracking `src/memory.rs` (MIT); C++ port: `core/hooks/pattern_scan.cpp` used by `game/bioshock1r/patterns.cpp` | **RESOLVED live: RVA 0x1BE7A0** (scan: 1 wide-string match, 1 string xref, FName global +3 xrefs, exactly 1 candidate past the init-site filter). Exe loads rebased (ASLR observed, base 0x0FB20000) - RVA is the stable identifier; the live-memory scan is relocation-transparent. | 2026-07-23 |
| PlayerController FOV (live) | `PlayerController + 0xE0` (float, degrees) | BioshockHD.exe | 2022-04-13 | itsloopyo `FOV_LIVE_OFFSET`; `kFovLiveOffset` in `patterns.h` | **readable, but NOT consumed by the renderer.** Reads 100.0 by default. 2026-07-24 automated flat A/B (command-file seam + window screenshots): writes of 60/137/140 leave the rendered frame pixel-identical, in real gameplay AND the menu attract scene, under BOTH `HorizontalFOVLock` states. The earlier "override widens view" (DR-4) was observed only through the VR projection layer, where the fov CLAIM follows the written value - a claim-side artifact, retracted. The field also does not mirror the video-option FOV (option applied while field kept reading 100). In-headset swim calibration measured the true render at ~100 = the settings value. Treat this field as telemetry-only; the real control is the video option (see Config/ini facts). | 2026-07-24 |
| `UShockUserSettings*` global | static pointer at **RVA 0x136AFA0** (`.data`); the object's **`+0x8C` = HorizontalFOV (int32, degrees)** | BioshockHD.exe | 2022-04-13 | runtime value-scan narrowing via the command seam (`memscani 130` -> user changed the video option to 100 then 117 through the in-game UI -> `memrescani` collapsed 662 candidates to 4 stable copies -> per-copy poke + screenshot img-diff found the consumed one -> `memptr` found the static root -> RTTI walk `vtable -> COL -> TypeDescriptor` read `.?AVUShockUserSettings@@`) | **RESOLVED + CONSUMED PER FRAME.** The renderer reads this field live: poking it mid-game changes the render immediately, no options APPLY needed. **NO code clamp past the UI cap: 145 renders wider than 130** (monotonic img-diff 117->130->145). Integrity check for resolution: the object's vtable == exe base + **0xDA3878**. NOTE the field is an **int32** - an ini float would serialize as `130.000000`, the bare `130` gave the type away; float scans cannot see it. `UD3DRenderDevice11` (vtable RVA 0xE38E7C) keeps a passive copy at `+0x74` - poking that changes nothing; two further copies (one heap-rooted, one at `[0x136A370]+0xCC`) are also passive. | 2026-07-24 |

(Add one row per symbol as they land in `src/game/bioshock1r/patterns.cpp`.)

## Known structures & conventions

- **FRotator** = 3×i32 `{Pitch, Yaw, Roll}`, 65536 units per full turn. Roll is
  clockwise-positive. UE convention: forward = +X, right = +Y, up = +Z.
- **FVector** = 3×float, Unreal units. World scale unknown - assume ~50 UU/m until calibrated
  in M3 (config `worldScale`).
- `eventPlayerCalcView` hook signature (thiscall):
  `(APlayerController* this, AActor** view_actor, FVector* camera_location, FRotator* camera_rotation)`
  - view_actor is an out-param (pointer to actor pointer; corrected 2026-07-23 from `AActor*`
    against the Rust source). Fires before the view is built; location/rotation are writable.
  - **Fires at the main menu too** (menu scene has a live PlayerController; `*view_actor == this`
    there). Observed call rate can far exceed display fps at the uncapped menu (up to ~7800/s
    vs ~500 fps) - do not assume exactly one call per frame.
  - Detour convention: `__fastcall` with a dummy EDX slot (register/stack-identical to thiscall).
- RTTI class names observed in exe: `AVengeanceGameInfo`, `AShockPlayer`,
  `AShockPlayerController`, `APlayerController`, `APawn`, `AShockHUD`, `AHands`, `AWeapon`.

## Hook points (planned / active)

| Hook | Purpose | Status |
|---|---|---|
| `IDXGISwapChain::Present` (vtable, kiero-style dummy-device discovery) | frame boundary: XR pacing, overlay, mirror | skeleton in M0 |
| `IDXGISwapChain::ResizeBuffers` | RT cache invalidation | skeleton in M0 |
| `eventPlayerCalcView` | camera override (HMD pose, per-eye offsets) | **active** (DR-4: `game/bioshock1r/camera.cpp`, self-enabling MinHook) |
| Drain RVA 0x61CAE0 (+ flush 0x61D0F0) | DR-5 reentry probe telemetry | **command-gated** (session 5: `game/bioshock1r/scenedraw.cpp`; exists only after a `reentry hook` seam command - default runs stay unhooked). Drain re-entry refuted - see Scene-draw architecture. |
| Game-thread frame submit RVA 0x585AC0 | SequentialReentry stereo (double-submit with per-eye rot) | **next session's hook** - located via SetEvent caller sampler, byte-walked, unhooked |
| GetPlayerViewPoint-equivalent used by fire traces (unknown) | decoupled controller aim | M6 |
| Console-command dispatcher (unknown - FName-chain on command strings) | execConsole one-liners | M5/M6 |
| XInputGetState (we ARE the proxy - no hook needed) | synthetic gamepad | M0 shim |
| DINPUT8 / WM_* (DR-6 decides) | virtual mouse for gameswf menus | M5 |

## Config / ini facts

- `[Engine.Console]` `ConsoleKey=9` (Tab). Launch option `-allowconsole`. Mixed reports on
  newest builds - verify (see STATUS blockers).
- **FOV control - RESOLVED (2026-07-24): the remaster's "FOV" video option is the only real
  control.** Range **75-130** in the options UI; user-verified that applying it visibly changes
  the flat render. It is stored as **`HorizontalFOV`** in the ini's remaster settings section
  (the one ending in `SettingVersion=2`, alongside mouse/subtitle options), i.e. rendered hfov
  == that option, which is why everything measured ~100 (the stored value). Editing the ini
  value directly to 137 (out of UI range) neither rendered nor propagated - use the in-game
  option. `[Engine.RenderConfig] HorizontalFOVLock=True;` (trailing semicolon is shipped -
  preserve) + `bHorizontalFOVLock=True` are back at their stock True after the 07-24 unlock
  experiment (False made the PC+0xE0 field fully inert without enabling anything; backup
  `Bioshock.ini.bvr-bak-fovlock` remains next to the ini). ~~VR consequence: claimed fov must be
  set to the option's value by hand (manual claimed-FOV slider) until we can read the live
  settings object.~~ **RESOLVED 2026-07-24 (session 4): the live settings object is found**
  (`UShockUserSettings`, static root RVA 0x136AFA0, int32 HorizontalFOV at +0x8C - see the
  symbol table). The renderer consumes it per frame and accepts values beyond the 130 UI cap.
  Extra facts from the discovery: `HorizontalFOV` is an int property (bare `130` in the ini);
  the string "HorizontalFOV" appears NOWHERE in the exe image (property names live in script
  packages/heap); the options MENU keeps its own transient copies that die on screen close
  (freed-heap fill 0xFEFEFEFE observed mid-scan).
- **Menu attract scene (2026-07-24)**: the main-menu backdrop is the live lighthouse level with
  a flying camera whose loc IS CalcView-driven (our offset command visibly moved it) but whose
  fov ignores the field. `bHasSaves` in the ini is stale/unreliable (read False while a
  loadable save existed). Save-spawn is deterministic - loading the same save reproduces the
  same viewpoint, good for A/B screenshot comparisons.
- User config path **confirmed** (2026-07-23, generated on first launch):
  `%AppData%\Roaming\BioshockHD\Bioshock\` - `Bioshock.ini` (live engine ini, 25 KB),
  `User.ini` (bindings, 99 KB), `MEMORY\CurrentGame` (save data).
- `.debug` files in `ContentBaked\pc\System` are plaintext console scripts (useful command
  vocabulary: `testAddAvailablePlasmid ElectricBolt`, `toggleplayerinvisible`, `stopmovie HUD`,
  `setres`, `STAT FPS`).

## OpenXR runtime facts (this machine)

- 64-bit ActiveRuntime: `C:\Program Files\Virtual Desktop Streamer\OpenXR\virtualdesktop-openxr.json`
- 32-bit ActiveRuntime (WOW6432Node): `...\virtualdesktop-openxr-32.json` pointing at
  `virtualdesktop-openxr-32.dll` (verified PE machine 0x014C = x86).
- xr_hello32 (2026-07-23): runtime "VirtualDesktopXR" 1.0.10, 31 extensions,
  XR_KHR_D3D11_enable present, 32-bit xrCreateInstance OK.
- **FULL PASS with Quest 3 connected** (2026-07-23): system "Meta Quest 3" (max 16 layers,
  max swapchain 16384x16384), required min feature level 0xB000 (11_0 - same as the game),
  adapter LUID matched RTX 4060, 32-bit D3D11 session created and ran 60 frames.
- OpenXR loader build note: with `DYNAMIC_LOADER=OFF` upstream forces dynamic CRT; we override
  `MSVC_RUNTIME_LIBRARY` back to static in `third_party/CMakeLists.txt` (CRT mismatch otherwise).
- **In-process instance creation verified** (2026-07-23, M2): `core/vr/openxr_runtime.cpp`
  creates the XrInstance inside BioshockHD.exe on VDXR 1.0.10 (~15 ms on the init thread).
  With no headset connected, `xrGetSystem` returns FORM_FACTOR_UNAVAILABLE and the runtime
  retries on a 5 s cooldown from the Present hook - so connecting Virtual Desktop mid-game
  brings the session up without restarting the game.

## D3D11 frame map (DR-3 - in-tree frame inspector; RenderDoc fallback unused)

Captured 2026-07-24 with `core/gfx/frame_inspector` (one-shot full dumps via the
`dumpframe full` seam command; lighthouse save spawn, 1920x1080, option fov 117 and a
second dump with `gfov 137`). Dumps live in `%LOCALAPPDATA%\BioshockVR\framedump_*.txt`
(game-derived - never committed). All RVAs are for build 2022-04-13.

**Render targets (steady-state gameplay frame):**

| RT | Size | DXGI format | Role (evidence) |
|---|---|---|---|
| T2 | 1920x1080 | 26 R11G11B10_FLOAT (RTV+SRV) | **main HDR scene color** - 86 draws, all depth-tested |
| T1 | 1920x1080 | 45 D24_UNORM_S8_UINT (DSV) | **main depth/stencil** (T34 = its 44 R24X8 SRV view) |
| T3/T4 | 960x540 | 26 / 45 | half-res HDR pass + own depth - MOST draws (108): particles/effects/water |
| T0 | 1920x1080 | 28 R8G8B8A8_UNORM (RTV+SRV) | post-tonemap LDR; 71 draws incl. depth-tested forward bits |
| T31/T32 | 1024x1024 | 40 D32_FLOAT / 41 R32_FLOAT | shadow map depth + color |
| T37-T40 | 480x270 | 26 | quarter-res chains (bloom/downsample), no DSV |

Census sanity (lifetime counters in the dump header): DrawIndexed/Draw both fire at
hundreds-of-thousands scale; the game issues NO instanced draws at this spot.

**View-projection constant buffer:** VS **b0**, combined world/view-projection matrix at
**float offset 32-47 (bytes 128-191)** of the captured 256-byte window. Verified by fov
A/B: cells 36/40/44 scale EXACTLY by `tan(117/2)/tan(137/2) = 0.6428` between the two
dumps (proj m00 factor through a combined matrix; camera held still). This is the
independent ground truth that rendered hfov == the UShockUserSettings option value, and
the future patch point for asymmetric per-eye projections (post-v1 backlog).

**Scene-draw architecture (DR-5) - TWO-THREAD render command queue.** Session-4 model
(byte walk 2026-07-24) CORRECTED by session-5 live hooks: what session 4 called the
"frame root" (0x61D0F0) never runs in gameplay (pass-through hook: 0 entries / 70 s of
play); the per-frame render entry is the DRAIN. Full corrected map (all live-verified
2026-07-24 session 5, base that day 0x10380000):

| RVA | Role | Evidence |
|---|---|---|
| **0x61C8E0** | render-command **executor**: `void __thiscall`, zero stack args; command type id at `this+0xC`, virtual-calls per type. Call-site cluster in 84/86 draw stacks. | session-4 disasm-by-hand |
| **0x61CAE0** | **drain** - the per-frame render entry: entered EXACTLY once per Present (drain/s == presents/s == 517-525 live, 40 s soak), ~1.6 ms per call, on the render thread. SEH-frame prologue `55 8B EC 6A FF 68`, __thiscall, zero stack args. Sole caller: 0x61D21E (pump loop). | live pass-through hook heartbeat |
| **0x61D1D0** | render-thread **pump loop** (thread main): frameless `push esi` prologue (session 4's CC-55-8B-EC prologue walk overshot it), registers `GetCurrentThreadId()` into `[RVA 0x13784E4]` (live: held the render tid), then loops `WaitForSingleObject(INFINITE)` -> drain -> `SetEvent`. Entered ONCE - hooks on anything above the drain never fire per frame. | byte walk + import resolution + live tid global |
| **0x61D0F0** | render **flush/join** (not a frame function): stamps `[mgr+0x58]=1`, waits; `[queue+0x58] != 0` is the PUMP EXIT flag. 0 entries during menu + gameplay. | live hook, 0 entries |
| **0x585AC0** | **game-thread frame SUBMIT/KICK - hand-off only, NOT the reentry seam** (see session-6 refutation below). `ret 0xC` literal (`C2 0C 00`; 3 stack args; arg1 = FVector* camera loc, arg2 = FRotator* - CONFIRMED session 6: 3 dwords copied to block +0x14/+0x18/+0x1C, arg3 = viewport/scene object with +0x378/+0x3DC reads). ECX dead at entry (`push ecx` is stack alloc; first ECX use loads arg3). Head spin-waits on frame-number globals `[0x13AF7E8]`/`[0x13AF7F8]` vs a TLS frame id. Stores camera loc into `[0x13865B0..]` (3 floats) and the submitted-frame block `[0x13AF7E8..]` (frame/owner dword, float3 loc at +4, int3 rot at +0x14), TryEnter/LeaveCriticalSection on the two buffer objects held in globals `[0x13A5FBC]`/`[0x13A5FC0]` (CS at obj+4), then `SetEvent([[0x13566C4]]+4)` at 0x585C62 (event object vtable-checked against RVA 0xE2D584; handle at obj+4). Prologue `55 8B EC 51 64 A1 2C 00 00 00`. In GAMEPLAY it fires exactly 1:1 with presents from one call site (ret RVA 0x4CDD8A, inside the scene build); during save-LOAD from ret 0x4CC6C8 (a second, load-path build); at the static main menu it fires ZERO times (menu present path not via this submit). | disk-image capstone disasm + live submit hook (session 6) |
| **0x4CCE70** | **game-thread scene BUILD root - THE SequentialReentry seam.** Builds the render command queue (CalcView runs EXACTLY once inside every call - live: calcview-in == build/s == presents/s) and at its tail (site 0x4CDD85, gated on render-thread obj `[0x13566CC]` non-null and queue ring `this+0x118 != this+0x11C`) calls the submit. Aligned-stack MSVC prologue `53 8B DC 83 EC 08 83 E4 F0` (push ebx; mov ebx,esp; and esp,-16 - NOT 55 8B EC: the 55-8B-EC backwards scan lands on a DECOY SEH function at 0x4CCD20 ("MyCheckpointData" scope); the real boundary is the 11-byte CC run ending 0x4CCE6F), then an SEH frame. `ret 0x10` = 4 stack args; ECX = live `this` (stored to [ebp-0x80]; the queue-ring object), stack arg1 -> edi (+0x48 read at tail). Static call sites 0x4CA9A4 / 0x4D2F68; live gameplay caller ret RVA 0x850EF0. ~50-80 us pass-through. | live submit-hook caller RVA -> disk-image capstone walk -> live build hook (session 6) |
| 0x583FDB | job-system WORKER completion SetEvent (3 worker threads, ReleaseSemaphore wake + job virtual call). Not a stereo seam. | sampler + byte walk |

Thread topology (live): game thread runs CalcView (~2 calls/frame, 1030-1170/s) and the
submit; render thread (distinct tid every boot) runs pump -> drain -> Present
(presents/s == drain/s; Present is issued inside the drain). CalcView-inside-drain = 0
over the whole soak: **camera sampling is entirely upstream on the game thread.**
Unfocused, presenting stops but the pump spins ~3000/s.

**Drain re-entry REFUTED (the decisive DR-5 negative):** a second drain call from the
detour (SEH-guarded pulse) faults 0xC0000005 at **0x61CB13** (drain+0x33) - the queue is
consumed/swapped and re-draining dereferences dead state. The poison latch caught it and
the hook stayed pass-through, but the pump's event protocol wedged afterward (game hung
- kill it; this is why the double-call probe is a PULSE, not a soak, and why the drain
is not the seam). Session-5's "submit fires ~3.7x per present at the menu" kick-sample
figure did NOT reproduce in session 6: at the static main menu the hooked submit fires
ZERO times (the sampler likely caught a transient boot/attract state); in gameplay it is
exactly 1:1 with presents.

**Submit re-entry REFUTED, build re-entry PROVEN (2026-07-24 session 6, all
flat-verified).** Double-calling the SUBMIT with a yawed rot copy is ABSORBED: thousands
of doubled submits (pulse + continuous), zero faults, but presents never doubled and the
yawed camera never rendered (all continuous-window screenshots at the 0.33 noise floor).
The view data the renderer consumes is baked into the command queue during the BUILD -
the submit's camera-global stores do not feed the already-built queue (consistent with
DR-3: view-proj rides in per-draw VS b0). The caller's own locals are what the submit
receives (call site pushes `lea [ebp-0x6C]/[ebp-0x7C]`).

**Double-calling the BUILD (0x4CCE70) is the SequentialReentry primitive, proven
end-to-end:**
- Pulse: second call does real work (call2 ~1.9-2.3 ms vs ~50-80 us pass-through),
  re-submits (submitsD=1), an extra present lands DURING the second call (engine's own
  buffer sync paces it), and CalcView re-enters once (the CalcViewDetour second-pass
  applied the yaw). No fault, no wedge - unlike the drain, the build tolerates immediate
  re-entry (it spin-waits in the submit head until the render thread consumed frame 1).
- Continuous (`reentry on`, yaw 30): build 225/s, every call doubled, submit 450/s ==
  presents 450/s - EXACTLY two engine-paced presents per game frame; game tick halves
  (CalcView-out 1050 -> 460/s: real-time delta grows, UE2 delta-time absorbs it).
  Screenshots show the world yawed 30 deg (stairs swing left, off-screen burning wreck
  enters frame; img-diff mean 7.7-7.9 vs 0.33 noise floor). `reentry off` recovers to
  1:1 instantly. Stability: ~3.5 min continuous clean (no faults, no visual drift
  across captures), then ONE hang (~124k doubled frames in; game thread stopped, kill
  required); a second hang followed ~1 min into the first stereo run.

**`-onethread` is a NO-OP - session-6's "native single-threaded mode" was a
measurement artifact (session-7 correction, live-proven).** The string "onethread"
does not exist in the image in any casing or encoding - the remaster never parses
it. Session 6's verification hexdumped `[0x13566C4]`/`[0x13566CC]` at the MAIN MENU,
where they are ALWAYS zero: the pump kick event + render-thread object globals are
created at first WORLD LOAD, in every mode (live session 7: mode flips 1T -> MT at
the save load with the arg on the command line; the pump thread runs from boot -
96 s of CPU on the drain thread with `-onethread` present; the 18:16 crash dump has
the pump thread with the arg live). The fps difference (630-710 vs ~530) was scene/
timing coincidence. The REAL single-threaded switch is the hw-thread quotient poke
below (`reentry 1t on`).

**The "onethread stereo crash" was a THREADED-mode crash - session-7 minidump
forensics (all three 2026-07-24 evening dumps, hand-parsed MiniDumpNormal: exception
context + thread stacks + module list, cross-checked against a capstone disk disasm
of the drain head).** The findings, each register-verified:
- **Drain head decode (0x61CAE0..)**: after `EnterCriticalSection([this+8]+4)` the
  drain loads ESI = `[this+0xC]` (drain+0x30) - the SUBMITTED-FRAME CONTEXT slot -
  then dereferences `[ESI+0x40]` at drain+0x33 (the frame's viewport object; a
  virtual call through its vtable +0xEC follows, then +0x80..+0x98 viewport params
  are copied out). With the slot EMPTY (no submitted frame pending) ESI == 0 and the
  read faults at addr 0x40. That is the entire crash: **a drain entered with
  `[this+0xC]` NULL.**
- **Both drain+0x33 specimens (18:05:54 and 18:16:19) fault on the render PUMP
  thread** (shallow dedicated stack: BaseThreadInitThunk -> pump loop -> drain, ret
  0x61D21E, EBX=ECX=this, ESI=0). The 18:16 process ("the onethread stereo run") had
  a live pump thread, its game thread parked in the known deadlock wait INSIDE a
  hooked build (BuildDetour frames on its stack, stereo active), and the watchdog
  thread's stack carries kick_engine_event frames: the classic sequence is deadlock
  -> pump woken into consumed state (wdkick or the desynced protocol's stray kick)
  -> drain walks the empty slot -> crash.
- The 18:11:16 dump is the recorded `[0x1375BD4]`-poke dead end: game-thread fault
  at exe+0x741D7F in a load-path callstack (0x4CB40A/0x4CCB19/0x4D4821...), exactly
  as logged live.
- **Consequence: the drain null-deref is a threaded-mode empty-wake defect** (same
  state as session-5's forced re-drain pulse, which faulted at the same +0x33); no
  crash has ever been observed with the renderer actually inline.

**Flush-point decision chain FULLY DECODED (0x61D260, session 7) - and the real
single-threaded switch found.** Per flush call, after copying arg1 (the scene
object) to `[mgr+0xC]` and arg2's 16 dwords to `[mgr+0x10..0x4C]`, the chain picks
`threaded` (eax) as follows - every veto selects INLINE, only the final quotient can
select threaded:
1. `[0x1375BD4] != 0` -> inline (the 500-ref GIsEditor-class global; dead end).
2. `[[scene+0x3DC]]+0x4C == 0` -> inline (per-client "use render thread" bool; live:
   scene vtable RVA 0xE1846C, client object vtable RVA 0xE127CC, value 1 - the
   documented alternative poke, unused).
3. Scene-state vetoes: `+0x13C`/`+0x14C` pair, `+0x68`, `+0x64`, flag bit 0x20000 in
   `+0x114`, `+0x110 != 5` -> inline.
4. **`[0x11B69FC] / [0x11B7A00] > 1` -> threaded** (live: 12 / 1 - hardware threads
   vs divisor; patterns.h `kNumHwThreadsRva`/`kThreadDivisorRva`). The pair is
   written once at startup (~0x756ED2 / ~0x6F26E0) and consumed by SEVEN inlined
   copies of this exact test (0x43BD90, 0x4D0E24, 0x58413B, 0x604641, 0x61D1AC,
   0x61D33B = this flush, 0x7814F6) + a cmp-1 at ~0x6F26EE - a tight single-purpose
   family, nothing like the GIsEditor global.
Then `[mgr+0x50] = threaded`, `[mgr+0x54] = 1`; INLINE branch = `drain(ECX=mgr)`
directly on the game thread (drain call ret 0x61D367); THREADED branch = the
queue = `[mgr+4]` flag-then-INFINITE-wait protocol (the 0x61D38E deadlock wait
lives here). The pump-loop entry 0x61D1D0 is a VIRTUAL (mgr vtable 0xE1CF14 slot) -
no simple boot-time creation site to intercept.

**`reentry 1t on` - the live single-threading switch (session 7, flat-proven).**
Pokes `[kNumHwThreadsRva]` 12 -> 1 (saved/restored by `1t off`): every subsequent
flush takes the inline branch. Live effects, all observed: heartbeat `mode=1T`,
beatTid == calcTid (drain on the game thread), drain caller ret flips to 0x61D367,
presents continue (~413/s mono vs ~530 threaded, ~20% cost), and the SUBMIT stops
firing entirely in mono (the inline drain consumes the ring before the submit
call-site gate, so no camera hand-off, no pump kicks, pump sleeps forever). The
poke must follow the drain-guard install (the `1t` command does both, in order):
any pump wake after the flip finds `[mgr+0xC]` already consumed - exactly the
drain+0x33 state - and the guard skips it. STEREO ON TOP (flat, session 7):
225 pairs/s = 450 presents/s all on the game thread, submits fire nested-in-build,
guardskips 0, eye-offset img-diff 2.03 vs 0.33 floor, consecutive captures
phase-consistent (0.43), 5-min stationary soak + 10-min synthetic play soak clean
- faster AND structurally deadlock-free where threaded stereo survived 16 s-3.5 min.

**Session-7 fixes (scenedraw.cpp / patterns.h):** (1) `render_is_threaded()`
mirrors the chain's static config (pump infra exists AND editor global clear AND
quotient > 1) - `reentry stereo on` REFUSES a threaded substrate (`reentry stereo
force` overrides), heartbeat/status/overlay carry `mode=MT|1T`; (2) DrainDetour
(auto-installed by both `stereo on` and `1t on`) SKIPS any drain entered with
`[this+0xC] == 0` (`kQueueFrameCtxOffset`) - a null slot can never be drained, so
the skip is universally safe; skips get their own `guardskips` counter. In threaded
mode the null check races the producer (a skip can eat an auto-reset wake =
possible stall, strictly better than the crash); inline it is same-thread exact.

**Dead ends recorded (do not retry):** (1) `[0x1375BD4]`, the first check in the
flush-point's threaded-vs-inline chain, is NOT a render toggle - it has 500+ code
references engine-wide (a GIsEditor/commandlet-class mode global); poking it to 1
mid-run crashed the next level load at 0x741D7F. (2) Watchdog EVENT RE-KICKS on a
detected deadlock: detection (game thread depth>0 + builds/presents frozen 300 ms) is
reliable, but SetEvent-ing the engine's own events resumed threads into desynced
queue state and crashed the drain (threaded-mode live test) - the watchdog is now
detect-and-log by default (`reentry wdkick on` re-arms kicks for experiments only).

**Render-done wait decoded (the deadlock site, session 6 late).** The game-thread wait
where every hang strands is at 0x61D38E, inside a "kick render and wait" flush-point
function (~0x61D340 area, `ret 8`) called from build site 0x4CDCD7: it stamps
`[mgr+0x50]` = multithreaded?, `[mgr+0x54]` = 1, and EITHER calls the drain (0x61CAE0)
INLINE on the game thread (single-threaded render mode - a potential stereo escape
hatch: no cross-thread protocol at all) OR takes queue = `[mgr+4]` and does the
classic racy pair: `if ([queue+8] == 0) WaitForSingleObject([queue+0x10]+4, INFINITE)`
then clears the flag - the event object is the same vtable-0xE2D584 class as the
submit's kick event, `[queue+0xC]` holds a second event. INFINITE + auto-reset +
flag-check-then-wait = event theft under two frames per tick. All three observed
hangs: game thread parked exactly here while the render thread waits inside the drain
at +0x30. Fix candidates (next session): MinHook THIS function when stereo is on and
replace the wait with a bounded flag-repoll; or a watchdog thread that detects the
double-wait state and SetEvents `[queue+0x10]+4`; or force the single-threaded inline
path during stereo.

**Frame-pacing protocol decoded; start-state gating falsified (same day, later).** The
submitted-frame block holds TWO frame-id dwords - `[0x13AF7E8]` and `[0x13AF7F8]`
(+0x10), one per double-buffered frame slot - whose HIGH BIT is the completion flag
(live dump in steady state: `0x8000043B` / `0x8000043C`, consecutive frame numbers,
both consumed; loc floats at +4, rot ints at +0x14 of the same block mirror the live
camera exactly). The submit head's `jg` guards are SIGNED compares, so a set high bit
(negative value) never triggers the engine's wait; the wait path's `-1` compare is an
init sentinel only (a first-guess wait-for-minus-one poll never released and throttled
the game to its 20 ms timeout - 48 fps - before this was understood). The queue
object's `+0x118/+0x11C` are ring POINTERS (unequal even at idle - the submit
call-site gate reads as "commands written", not empty; polling equality also throttles
to 48 fps), while `+0x128/+0x12C` are twin seg counters (equal at idle). scenedraw's
pass-2 gate now polls frame-id bits + counter equality (bounded 20 ms, skip on
timeout) - full doubled throughput (~260 pairs/s = 520 presents/s) and a graceful
unfocused degrade, but it does NOT prevent the deadlock above: the race lives inside
the concurrent produce/consume window of the second frame, not in a dirty start
state (live-falsified twice). Constants: patterns.h `kFrameIdPairRva`,
`kFrameIdSecondOffset`, `kQueueSegProdOffset`, `kQueueSegConsOffset`.
- Phase note: PrintWindow captures consistently caught the SECOND (yawed) present of
  each pair - the pair's present order appears deterministic, which is promising for
  per-present eye attribution in the per-eye split.

**DR-5 probe tooling (sessions 5-6):** `game/bioshock1r/scenedraw.{h,cpp}`, all
command-gated via the seam (`reentry hook [build|submit|drain|flush]|stereo on|off|
unhook|on|off|pulse|yaw <deg>|dump <n>|arg3 <hex|off>|latchclear on|off|reset|status|
kick on|off|calcstack`; default hook target = build; `stereo on` = the M4 rung-2 L/R
double-render with eye-tagged capture, see ARCHITECTURE decision log). 1 Hz heartbeat: build/submit/drain/flush
entries/s, submit-nested-in-build count, presents/s, CalcView in/out (tid-attributed),
call durations, beat/calc tids, distinct caller RVAs. `dump <n>` logs per-submit-call
arg telemetry (loc/rot raw+degrees, arg3 ptr + vtable RVA, presents-delta). Second
original calls SEH-guarded (C++ throws pass through) with a poison latch; the submit
slot doubles with COPIED loc/rot args (yaw on the copy), the build slot passes original
args through and yaws via CalcViewDetour's second-pass path. `kick` = process-wide
SetEvent-caller sampler; `calcstack` = one-shot game-thread stack scan (script-VM frames
0x679067/0x67AF88 dominate; upstream candidates 0x491C86/0x7327DA/0x55A4A2).

**HUD fingerprint (partial):** menu frames are pure gameswf - only SetRT ping-pong
between T0-like LDR targets and NO depth-tested draws; in-game HUD draws land on the
LDR target after the scene passes with no DSV bound. Good enough to segregate scene vs
HUD for M9; exact shader/SRV fingerprint deferred until the HUD-capture milestone.

## UnrealScript findings

_(Summaries only - never paste decompiled code. Tooling: UE Explorer/UELib on
`Build\Final\BakedScripts\pc\*.u`, workspace in `tools/uscript/` (gitignored).)_
