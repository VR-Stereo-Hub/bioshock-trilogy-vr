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
| `AWeapon::GetPerfectFireStart` impl RVA 0x226840 (weapon vtable +0x304) and `UAttackAbility::GetPerfectFireStart` impl RVA 0x1BC220 | decoupled controller aim: the shot's origin (and, on the weapon path, the direction the engine then applies spread to) | **active, command-gated** (M6: `game/bioshock1r/aim.cpp`, `vraim on`; ability seam live-confirmed firing, origin substitution proven) |
| Damage-factory trace virtual (factory vtbl +0xEC, factory from 0x231E70) | the plasmid/trace DIRECTION - the one open piece of the fire flow | M6 next session (probe with `vraim scanimpl`) |
| `APawn::GetViewDirection` impl 0x3CBA10 / `AShockPlayer::GetViewPoint` impl 0x1E5E50 | candidate aim sources - RULED OUT live (never called during a shot) | investigated, not hooked |
| Console-command dispatcher (unknown - FName-chain on command strings) | execConsole one-liners | M5/M6 |
| XInputGetState (we ARE the proxy - but the STEAM OVERLAY code-hooks the export thunk and eats calls, so the real seam is the game's IAT slot RVA 0xBCF8E0 -> bridge wrapper) | synthetic gamepad | **active** (M5: core/input/xinput_bridge + game IAT hijack; see "Gamepad architecture") |
| UWindowsViewport::UpdateInput RVA 0x853D20 (driven, not hooked - nothing calls it in windowed mode) | engine consumes the synthetic pad | **active** (M5: game/bioshock1r/input_drive.cpp calls it per present) |
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
- `.debug` files in `ContentBaked\pc\System` are plaintext console scripts - a useful
  reference for the engine's command vocabulary (e.g. `stopmovie HUD`, `setres`, `STAT FPS`)
  that the console-command seam can target.

## OpenXR runtime facts (this machine)

- **Grip pose vs aim pose (M6, 2026-07-25).** The first in-headset aim test read
  low: the wrist had to be held below where the shot went. The ray was built from
  `/user/hand/*/input/grip/pose`, whose forward axis runs along the controller
  HANDLE, not along the pointing direction - on Touch that is tens of degrees
  apart. `/user/hand/*/input/aim/pose` is the runtime's own pointing ray and is
  what aiming should use; grip stays the right choice for placing a hand/weapon
  MODEL (M7). Both are located every frame now, and the aim path keeps pitch/yaw
  trim offsets on top for personal taste.

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

**1t LOAD HAZARD (session 7, 19:54 crash - do not arm 1t across a level load).**
With the numerator poked to 1 AND stereo armed at the MAIN MENU, clicking CONTINUE
crashed ~7 s into the save load: 0xC0000005 in ntdll (EnterCriticalSection, ECX=8 -
null/dead CS) on an engine LOADER thread (FThread trampoline 0x94DF2E; load-path
frames 0x4D01F6/0x53DE8B/0x3A09ED/0x488219/0x467DFF; ZERO bioshockvr frames on the
stack). Control experiment from the same evening: an 18:58 CONTINUE load with
stereo armed but the numerator UNTOUCHED survived - so the load fragility comes
from the hw-thread global's OTHER consumers (the quotient family includes
load-path site 0x4D0E24), same dead-end class as the GIsEditor poke but confined
to loads. In-map gameplay with the poke is soak-proven fine (BioShock levels are
discrete; no mid-map streaming transitions observed). Mitigations shipped: `1t on`
refuses while the pump globals are still null (menu = the next thing is a load),
its ON log warns to `1t off` before any save load / level transition, and pass 2
doubles ONLY builds arriving from the gameplay caller (kSceneBuildGameplayRetRva
0x850EF0) so no doubling can ever run inside a load path. STRUCTURAL FIX SHIPPED (session 8 -
see below); the poke is retained as `reentry 1tpoke` (NOT load-safe, kept
as a fallback/diagnostic only).

**Structural 1t SHIPPED and the LOAD HAZARD CLOSED (session 8) - the
flush-point hook.** The flush-point (0x61D260) is now MinHooked; the detour
`FlushPointDetour` reproduces the byte-confirmed INLINE branch itself and the
hw-thread numerator is never touched, so the quotient family's load-path
consumers (0x4D0E24 etc.) always see the true core count. Full disk disasm
(capstone, 2026-07-24 session 8) confirmed the branch exactly:
- prologue `55 8B EC 51 8B 4D 0C` (push ebp; mov ebp,esp; push ecx; mov
  ecx,[ebp+0xC]=arg2), then `mov eax,[ebp+8]=arg1`, `mov esi,[0x1356590]`=mgr;
- `[mgr+0xC]=arg1`; sixteen `mov` pairs copy arg2's 16 dwords to
  `[mgr+0x10..0x4C]`;
- the decision chain writes eax (0=inline); `[mgr+0x50]=eax`, `[mgr+0x54]=1`;
- `test eax,eax; jne threaded`: inline path is `mov ecx,esi; call 0x61CAE0`
  (drain, ret site 0x61D367) then the epilogue `pop esi; mov esp,ebp; pop
  ebp; ret 8` - NOTHING after the drain, so forcing inline drops no work;
- the threaded path (eax!=0) is `mov esi,[esi+4]`=queue then the racy
  flag-then-INFINITE-wait at 0x61D371/0x61D38E (the deadlock site).
The detour: when `g_forceInline` is armed and `mgr=[0x1356590]` is non-null,
it does exactly the inline block above (`[mgr+0xC]=scene`; copy 16 dwords;
`[mgr+0x50]=0`; `[mgr+0x54]=1`; call the drain THROUGH its hooked target
address so the empty-slot guard + telemetry stay in the path) and returns;
mgr null (pre-world) or a fault falls through to / disarms cleanly (SEH,
poison on fault). Constants: patterns.h `kFlushPointRva`,
`kFlushPointPrologue`, `kMgr{SceneSlot,ViewGroup,ThreadedFlag,FlushSeen}Offset`.
Expected + confirmed: the drain's heartbeat caller RVA reads inside
bioshockvr.dll (the detour's call site) instead of 0x61D367 - not a bug.

**Load-crossing soak - PASSED, hazard closed (session 8, flat).** With
`reentry 1t on` (hook) and `reentry stereo on` armed, ALL of these ran clean,
zero crashes / zero new dumps / guardskips 0 throughout: (1) in-game save
load via LOAD; (2) quit-to-main-menu teardown (a full level unload - camera
returns to the attract cam); (3) new-game load (Bink intro -> in-water
intro); (4) the bathysphere DESCENT into Rapture (a real multi-map streaming
transition - loc crossed +75000 UU with 1t forced the whole way); stereo
re-engaged on arrival. The session-7 poke crashed a loader thread on step 1;
the hook survives every transition because the numerator global is untouched.
Because of this, `reentry 1t on` (hook mode) no longer refuses at the menu -
pre-world arming is inert (the detour falls through while mgr is null) and
menu arming is proven. The menu refusal + off-before-load warnings now live
only on `reentry 1tpoke` (the legacy poke).

**Stereo performance envelope (session 8, focused flat):** lighthouse spawn
225 pairs/s; Welcome-to-Rapture arrival (heavier indoor geometry) ~81 pairs/s
= 162 presents/s sustained (79-83 typ, one transient dip to 62), eye-offset
img-diff 6.5 vs a 0.28 phase-consistent floor. Both scenes clear M4's 72
pairs/s (144 presents/s) target. A dedicated COMBAT profile is still open
(needs a combat save; queued).

**One-toggle "VR stereo" (session 8).** `vrstereo on|off` (top-level command,
`reentry vrstereo ...`, and the overlay "VR stereo" checkbox) sequences
structural 1t -> `vr::set_camera_mode(true)` -> stereo on, reversing on off;
sticky across loads (nothing disarms on a transition; the pass-2
gameplay-caller gate idles doubling through load-path builds and stereo
re-engages when gameplay builds resume). The overlay checkbox posts a
request that the game thread applies from `note_calcview`, OUTSIDE any hooked
call (MinHook installs must not run mid-build/mid-drain). Flat-verified: one
`vrstereo on` at the MAIN MENU armed all three (log: `VRSTEREO READY`), a
CONTINUE-load carried straight into Rapture with stereo doubling live and no
re-arm, and `vrstereo off` restored mode=MT / build==presents / drain back on
the render thread.

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

**xr-frame-per-pair pacing (session 7 polish, after the first in-headset stereo
test - flat-regression-clean, headset verdict pending).** First-test user report:
stereo correct and world scale good, but "eyes feel weird" on head movement. Root
cause: the SR pipeline runs the full xrWaitFrame/Begin/LocateViews/EndFrame cycle on
EVERY Present, so the two presents of one L/R pair got predictedDisplayTimes about
one compositor period apart and located their eye poses at those two different times,
while both eye images were rendered from ONE game-thread head sample - so the
compositor reprojected the pair with mismatched poses (motion-dependent shear), and
the second blocking wait halved the game tick. Fix (`core/vr/openxr_runtime.cpp`,
`g_srPairPacing`, default ON, overlay checkbox "SR pair pacing"): a LEFT-tagged
present captures its eye and RETURNS with the XR frame held open (`g_srPairOpen`,
skips submit/end and the next present's waitFrame/begin/locate); the RIGHT-tagged
present completes the same frame, submitting BOTH eyes with poses from the pair's
single locate. One waitFrame, one locate, one prediction per game tick - consistent
pose pair and the game tick is no longer double-waited. Robustness: the completing
present falls through to normal single-present submission if the pair is broken
(mode boundary, stereo toggled mid-pair); `g_srPairOpen` clears on teardown; the tag
ring's existing depth>2 resync still covers skew. Telemetry: overlay `pairs`/`aborts`
counters. Flat regression clean (231 pairs/s, no dumps, recovery clean); the actual
comfort improvement is an in-headset judgment (queued for the user).

**HUD fingerprint (partial):** menu frames are pure gameswf - only SetRT ping-pong
between T0-like LDR targets and NO depth-tested draws; in-game HUD draws land on the
LDR target after the scene passes with no DSV bound. Good enough to segregate scene vs
HUD for M9; exact shader/SRV fingerprint deferred until the HUD-capture milestone.

## Gamepad architecture (M5 session 9, 2026-07-25)

All derivations: XINPUT1_3 import-thunk walk of the disk image (capstone
scratchpad scripts), RTTI on the vtables, and live pokes/hexdumps through the
seam. RVAs against the preferred base 0x10900000 (subtract it from the static
VAs in the disasm; ASLR rebases at runtime).

**The remaster reads the pad in exactly one place**:
`UWindowsViewport::UpdateInput` (RVA **0x853D20**, vtable slot 70 = byte
offset +0x118 of the UWindowsViewport vtable RVA 0xE4E448, thiscall
`(BOOL reset, FLOAT dt)` / `ret 8`). Its body: WM mouse-capture block
(skipped when [vp+0x214]==0), DI keyboard block (self-skips when the global
DI keyboard device [RVA 0x1380DEC] is null or its viewport flag is off),
then the PAD BLOCK - `XInputGetState(0)` EVERY call (branch A processes
state while the connected global is set and re-stamps it on failure; branch
B at +0x854D01 is the reconnect probe that sets the global on success).
There is provably no path through the function that skips the pad block
(full branch-target scan of 0x853D20..0x8541C9).

**Nothing calls UpdateInput in windowed mode.** The game probes GetState ~6x
during client init (site RVA 0x8507BB refines `client+0xDC` after the
constructor defaults it to 1) and never again - no WM_DEVICECHANGE handling,
no re-probe interval (verified live over minutes, all flags forced). The
`[WinDrv.WindowsClient] UseJoystick/UseController` ini keys are dead (set
True, `client+0xDC` still boots 0). The pad path is fullscreen-era code the
remaster left orphaned for windowed - hence "no controller support in
windowed" behavior on stock installs.

**Key objects/globals** (constants in patterns.h):
- `[RVA 0x1375368]` -> UGameEngine; `+0x4C` -> UWindowsClient (vtable RVA
  0xE4DBE0, RTTI-confirmed). `client+0x44/+0x4C` = TArray<UViewport*>;
  first element is the live UWindowsViewport (vtable RVA 0xE4E448).
- `client+0xDC` = UseController (BOOL). Client vtable slot 70 (+0x118) =
  `SetUseController(BOOL)` (RVA 0x8509B0): writes +0xDC, updates the
  UI-prompt global byte [VA 0x11C80D50], SaveConfig, notifies gameswf
  (menu prompts flip to Xbox icons). Slot 71 = ToggleUseController; the
  UWindowsClient::Exec handler at RVA 0x850DE0 exposes it as the console
  command **"ToggleUseController"** (wide string RVA 0xE4DABC).
- `[RVA 0x11B7A10]` = pad-connected global, written only by UpdateInput's
  pad block; game code gates pad UI/behavior on `client+0xDC && global`.

**The Steam overlay eats XInput**: gameoverlayrenderer.dll code-hooks the
export thunk of whatever xinput1_3.dll is loaded - our PROXY included (live:
proxy export +0x2E5F starts with an E9 jmp into the overlay). Calls through
the game's IAT then die inside Steam Input (returns DEVICE_NOT_CONNECTED
with no physical pad) WITHOUT ever running the proxy body or its post-hook.
The proxy post-hook therefore only sees pre-overlay boot calls. Fix shipped:
the bridge re-points the game's IAT slot for XINPUT1_3 ordinal 2 (slot RVA
**0xBCF8E0**; ordinal 3/SetState is 0xBCF8DC, untouched until M7 haptics) at
its own wrapper - previous target kept as passthrough so Steam-served real
pads still work, composed synthetic state injected on the return path.
xinput1_4.dll in-process is the overlay's own load (absent at our init).

**How M5 makes the pad work** (input_drive.cpp): on `vrinput on` the adapter
calls `SetUseController(TRUE)` through the engine's own setter and then
drives `viewport->UpdateInput(0, dt)` once per present from the CalcView
detour (game thread, SEH-guarded, self-throttled). UpdateInput's own pad
block then consumes the bridge-composed GetState: full menu navigation +
gameplay look/move from synthetic state, flat-verified 2026-07-25 (menu
highlight moved on dpad, CONTINUE activated by A, RS yawed the live camera,
passthrough byte-identical with vrinput off). Boot probes are covered by a
persisted marker (`%LOCALAPPDATA%\BioshockVR\vrinput.on`) read at DLL attach
so the client-init probe already sees a connected pad.

## Native function table (M6 session 10, 2026-07-25)

**The engine ships its own symbol table for every name-based native, and it is
trivially readable.** Each `native` UnrealScript function implemented in C++ is
registered through a 12-byte `.data` entry `{ const TCHAR* name; Native impl;
0 }`, where the name is the wide string **`"int<Class>exec<Function>"`** in
`.rdata` (e.g. `intAWeaponexecApplyAimError`) and the impl pointer is written by
static initialization (`C7 05 <entry+4> <imm32>`). 1822 such entries exist.

- **Runtime resolution** (shipped, `pattern_scan::find_native_function`): find
  the name string, find the dword that references it, read the next dword. No
  hardcoded address, no prologue scan, and it reads the same way on a patched
  build. Verified live: all five session-10 lookups resolved on the first try
  at their documented RVAs.
- **Offline enumeration** (scratchpad `natives.py` / `nativemap.py`): dumping
  the whole table gives a per-class native inventory - this is how the aim seam
  was found in minutes instead of by fire-path guesswork, and it is the fastest
  first stop for any future engine question (M7 hands: `AHands` has
  `CanExecuteAction`, `SetCurrentTransitionSequence`,
  `InterruptAnimNotifiesForAnimation`).
- **Caveat that cost time**: the linker pools wide strings by SUFFIX, so
  `AimError` is literally the tail of `ApplyAimError`. A substring match proves
  nothing; require the null terminator at the expected end (the shipped scan
  does).
- **`exec` thunks are NOT the seam.** They are the script->C++ entry only:
  `execFoo(FFrame& Stack, void* Result)` parses params off the bytecode and
  then calls the real C++ method (virtual or direct). Native callers skip them
  entirely - hooking all four aim thunks caught ZERO calls across a live
  session of shooting. Hook implementations, not thunks.
- FFrame layout used by the probe: `+4` Node (calling UStruct), `+8` Object,
  `+0xC` Code.

## Fire flow / aim (M6 session 10, 2026-07-25)

Chain, script -> native, derived from UELib decompiles of ShockGame.U (summaries
only) plus capstone walks of the implementations:

1. Trigger -> `Weapon.BeginFiring` -> `GotoState('Firing')`; the state plays the
   firing animation and an **anim notify** (`AnimNotify_UseAbility`) is what
   actually fires. Attacks are ABILITIES: `Ability -> AttackAbility ->
   TraceAttackAbility / ProjectileAttackAbility / MeleeAttackAbility`, and
   plasmids are the same machinery (`ElectricBoltAbility : TraceAttackAbility`).
   Player weapons are `Pistol/Wrench/... : PlayerWeapon : Weapon : Holdable`.
2. `AttackAbility.UseAbility` -> native `InitiateDamage(name)`:
   - `AWeapon::InitiateDamage` **RVA 0x226050** (weapon vtable slot +0x2FC)
   - `UAttackAbility::InitiateDamage` **RVA 0x1BBD80** (`ret 8`, FName by value)
3. Each InitiateDamage first asks for the shot's start:
   - `AWeapon::GetPerfectFireStart` **impl RVA 0x226840**, weapon vtable slot
     **+0x304**, `__thiscall(FVector* outA, FVector* outB, FVector* outC)`.
     outA <- ownerPawn+0x1D8 (Location), outB <- `[pawn+0x450]+0x1E4`.
   - `UAttackAbility::GetPerfectFireStart` **impl RVA 0x1BC220** (direct call
     target of its exec thunk), `__thiscall(void* instigator, FVector* outA,
     FVector* outB, FVector* outC)`, `ret 0x10`. Same two sources one slot over,
     plus a lean offset added to the position.
   - The weapon path then applies its own spread: `AWeapon::ApplyAimError` impl
     **RVA 0x226AA0** with a magnitude from **0x229DE0** - so substituting at
     GetPerfectFireStart keeps per-weapon accuracy intact.
4. Damage/trace is handed to a damage-factory object (`0x231E70` fetches it,
   then a virtual at factory vtbl+0xEC) - **this is where the trace direction is
   produced, and it is the one piece not yet pinned down** (see below).

**Live findings (probe, 2026-07-25):**
- **The weapon path's out-param B is an FRotator, and it IS the fire direction -
  CONFIRMED by substitution.** Live values `B[rot]=(132 116 0)` matched the
  camera's own rotation exactly (heartbeat `rot=(144 116 0)`), and writing our
  own pitch/yaw there moves the bullets: firing at a flat wall with an injected
  hand aim put the decals 12 deg right, 12 deg left, 10 deg down and 8/8 up-right
  of the crosshair while the CAMERA never moved, and `vraim off` put the next
  round back on the crosshair. That is M6's acceptance criterion, flat.
- **A rotator reads as three near-zero floats.** Rotation units are int32s whose
  float reinterpretation is a denormal, so an FRotator out-param prints as
  `(0.000 0.000 0.000)` - which cost this session a long detour chasing a
  "missing" direction. `aim.cpp` now classifies each out-param by value (small
  int32s = FRotator, unit-length floats = direction vector, thousands = position)
  and writes the matching type; the probe log tags each slot `[rot]/[dir]/[pos]`.
- `UAttackAbility::GetPerfectFireStart` FIRES on a plasmid cast (Electro Bolt),
  `this` vtable = `UAttackAbility` 0xD7E9D4, and its out-params read
  outA = the player's Location, outB = an ALL-ZERO rotator, outC = Location + a
  small lean offset. So the ability path gives a start but no usable rotation:
  the plasmid's direction still comes from somewhere else (see Open below).
- `APawn::GetViewDirection` impl (**0x3CBA10**, pawn vtable +0x35C) and
  `AShockPlayer::GetViewPoint` impl (**0x1E5E50**, +0x360) are NOT called during
  a cast (scanimpl, zero calls) - so the factory does not ask the pawn.
- The **wrench does not trace at all**: `Wrench.CreateCollisionPhantom` - melee
  damage is a Havok phantom, so no aim seam exists for it (M7's "wrench melee
  feels aimed" is a hands-rendering matter, not an aim-vector one).
- AI ownership is separable at every seam: the weapon's owning pawn is
  `[weapon+0x454]`, the ability's instigator is `[ability+0xF0]`, and the engine
  itself compares that instigator against the AShockPlayer vtable at 0x1BC2D0.

**AActor field layout (from the GetViewPoint/GetViewDirection implementations,
cross-checked against a live hexdump of the player pawn):**
- `+0x1D8` FVector **Location** (feet; live -7210.96 / 1106.66 / 2567.15)
- `+0x1E4` FRotator **Rotation** (live pitch 0 / yaw 55599 / roll 0 - the yaw
  matched the camera heartbeat exactly). `GetViewDirection` = `Rotation.Vector()`
  via the helper at **0x1BC870**; pawns keep pitch 0, so the pitched view
  rotation lives on the PlayerController, not the pawn.
- `+0x550` float **eye height** (`GetViewPoint` = Location + eyeHeight on Z)

**CLOSED (M7 session 11, 2026-07-25): the PLASMID direction is under our
control too, through the same rotator out-param.** Session 10's "all-zero
rotator" reading was a one-off, not the truth: under a real Electro Bolt cast
the ability path's out-param B carries the camera's own FRotator, exactly like
the weapon path (live: ability `B[rot]=(144 116 0)` == weapon `B[rot]=(144 116
0)` == camera heartbeat `rot=(144 116 0)` - the player spawns facing near +X and
level, so the small numbers are real rotation units, not a zeroed slot).

Proven by an A/B that separates the two candidate mechanisms, camera stationary
throughout: with hand-ORIGIN substitution ON and +20 deg injected left-hand yaw,
the bolt's scorch landed right of the crosshair; with origin substitution OFF
(so only the rotator is written) and -25 deg injected, it landed LEFT of the
crosshair. Direction follows the rotator, not the origin - so the damage-factory
virtual at factory vtbl `+0xEC` never needs to be touched. (A PROJECTILE plasmid
such as Incinerate is still unverified; only a trace plasmid was available.)

**Class vtables (MSVC RTTI walk: TypeDescriptor `.?AV<name>@@` ->
CompleteObjectLocator -> vtable; scratchpad `rtti.py`):** AShockPlayer
**0xD82BB8**, AShockPlayerController 0xD81C84, APlayerWeapon **0xD8FF58**,
AWeapon 0xD90268, UAttackAbility **0xD7E9D4**, AHands **0xD8A28C** (M7), APawn
0xD82824.

## Viewmodel / AHands (M7 session 11, 2026-07-25)

**The first-person hands + weapon are ONE actor whose Location/Rotation the
engine copies from the camera every tick, and writing those fields from the
CalcView detour wins.** That is the whole M7 mechanism; there was no ordering
fight to lose.

- **Finding the actor**: heap scan for the fixed-RVA `AHands` vtable
  (**0xD8A28C**), the same technique as the `UShockUserSettings` lookup and now
  a shared helper (`patterns::scan_for_vtable_object`). In a loaded world the
  scan reports **3 vtable matches: exactly ONE live actor**, plus two false
  positives in a stack region whose fields are all `0xCCCCCCCC` (MSVC debug
  fill - they print as loc `-107374176.0` and rot `-858993460`). The
  plausibility filter that separates them is distance to the camera: the live
  viewmodel is always within arm's reach of the view, the debris is ~1.9e8 UU
  away. One instance confirms the user's description - a single mesh carrying
  both hands plus a short forearm stub, so the visible arm moves rigidly with
  the weapon (articulated arms are the post-v1 IK item).
- **The fields are the placement.** The live actor reads
  `+0x1D8` Location == camera location and `+0x1E4` Rotation == camera rotation,
  exactly, every frame (live: loc `(-6729.6 2188.7 2627.3)` / rot `(144 116 0)`
  against an identical camera heartbeat, distance to camera 0.0 UU).
- **Our write lands, flat-verified by screenshot.** Pushing the actor 60 UU
  along the view moved the visible pistol from the lower right into the centre
  and enlarged it; injected yaw of +30 deg swung the gun out of frame to the
  right and -30 deg swung it to the left. The engine places the viewmodel during
  its own tick and the CalcView detour runs afterwards, so the last write of the
  frame is ours - no placement hook was needed.
- **Firing is unaffected.** With the write active, four synthetic trigger pulls
  fired normally (ammo decremented, bullet decals landed where the M6 aim seam
  asked), no faults, no dumps. The transform and the animation state are
  independent.
- **Caveat worth knowing when tuning**: at LARGE displacements part of the mesh
  stretches - the forearm/sleeve geometry appears anchored near the view while
  the hand follows the actor, so a 60 UU push produces a visibly stretched arm.
  Realistic offsets (a few centimetres, which is what the tuning sliders cover)
  stay well inside the range where this is invisible.

## UnrealScript findings

_(Summaries only - never paste decompiled code. Tooling: UE Explorer/UELib on
`Build\Final\BakedScripts\pc\*.u`, workspace in `tools/uscript/` (gitignored).)_

**Headless decompiling works and is fast** (session 10): the UE Explorer
portable build ships `Eliot.UELib.dll`, which UELib exposes to PowerShell -
`tools/uscript/dump.ps1` loads a package in <1 s and lists classes, functions
and states or decompiles one by name. Package: version 142 / licensee 56, build
"BioShock" (UELib auto-detects it). Two build quirks worth knowing: name-table
entries are UTF-16 with a POSITIVE char count and 8-byte flags, and UFunction
deserialization fails for ~40% of objects on this licensee build (the ones that
load are plenty - class/state bodies decompiled fine).

Findings so far: the attack/ability hierarchy and the trigger -> Firing state ->
anim-notify -> UseAbility -> InitiateDamage chain above; `Weapon.Firing` state
body is just animations plus `GotoState('None')`; `AttackAbility.UseAbility` is
two lines (`Damager = Instigator; InitiateDamage('None')`).

**Input bindings that matter for automated tests** (`User.ini`, session 10):
`XENON_RT = SwitchAndFireWeapon`, `XENON_LT = SwitchAndFireAbility` - the FIRST
trigger pull only switches hands ("SwitchToWeapons"/"SwitchToPlasmids"), the
second one fires. A single synthetic pull therefore looks like it did nothing.
`LeftMouse = Fire` (active hand), `RightMouse = SwitchWeaponsOrPlasmids`.
