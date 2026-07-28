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

**Session-11 evening addendum - the first in-headset run failed, and chasing it
rewrote much of the mechanism knowledge (each item live-proven):**

- **The engine's own placement, decompiled** (`Hands.UpdateLocation`, summary
  only): `NewRotation = PawnOwner.GetViewRotation(); offset = (widescreen ?
  PlayerViewOffsetWidescreen : PlayerViewOffset) >> NewRotation; offset.Z +=
  EyeHeight; SetLocation(Pawn.Location + offset + ViewLocationOffset());
  SetRotation(NewRotation)` - via the INDEXED natives (SetLocation = 267,
  SetRotation = 299), i.e. script-driven per tick. The AHands origin is the EYE
  ANCHOR, and the whole visible mesh hangs off it.
- **The eye->gun offset, measured live**: the equipped pistol's actor sits at
  ~(+49 fwd, +27 right, -20 down) UU from the camera, with its own slightly
  canted rotation. That is the LEVER ARM: rotating the AHands actor swings the
  visible gun on a ~1.2 m radius, which is exactly the "slight pivot that
  breaks everything" the headset test reported. (It also says the viewmodel is
  authored pushed-out and oversized - the flat-screen weapon-FOV trick - which
  is why the gun reads huge in VR.)
- **The weapon is its own actor ATTACHED to AHands, and the renderer draws
  attached actors from the attach matrix, IGNORING their Location/Rotation
  fields.** Full-frame-rate writes to the live pistol actor's transform moved
  nothing on screen. The attachment linkage: weapon `+0x450` = Base -> the
  AHands actor (adjacent to Owner at `+0x454`, the classic UE2 Owner/Base
  pair; another AHands backref sits at `+0xB0`). A detach experiment (null the
  Base) is the open path to a free-flying gun-only viewmodel.
- **Frustum culling kills the pivot-correction shortcut**: writing the AHands
  origin behind the camera (the placement that would put the mesh's GUN at the
  controller) makes the ENTIRE rig vanish - the engine culls by actor origin.
  Forward offset is therefore bounded by roughly the controller's distance
  from the face.
- **Actor-field corrections** (the session-11 morning "DrawScale +0x16C" claim
  was WRONG): `+0x16C` (default 0.0) is a hide/cull-style field - 0.5 AND 0.8
  both make the mesh vanish (the "0.8 shrank it" reading was a misread);
  `+0x168` (default 1.0, right after the mesh pointer at `+0x164`) looked like
  DrawScale but poking 0.5 changed nothing visible. A true DrawScale has NOT
  been located; gun-size control is open.
- **Harness gotcha: the lowered/equip pose.** After a save load, until the
  first trigger pull raises the weapon (`SwitchAndFire*`), the rig idles in a
  lowered pose with the pistol pulled in near the camera axis - which reads as
  a "giant centered gun" in screenshots and is easy to mistake for a bug. Pull
  the trigger twice before judging any viewmodel screenshot.

**Viewmodel inspection results (2026-07-25/26, camera-orbit + rig-spin tests).**
Method: pin the rig with `vrhands simpose` (fixed synthetic pose), then move the
CAMERA around it with the `offset` command. This works because the camera drive
publishes the pre-offset camera into the frame context, so the rig stays put in
the world while the camera flies - a free orbit rig for inspecting anything we
place. Findings:

- **The rig renders as a normal WORLD-SPACE object.** The camera orbits it and
  it holds still. It is not screen-pasted, so it can be inspected from any
  angle in principle - which means "cannot see the far side of the gun" is a
  reachability problem, not a rendering lock.
- **The geometry is COMPLETE on every side.** Spun through 0/90/180/270 the
  pistol shows a full right side, muzzle, cylinder, frame and a hand correctly
  wrapped on the grip. No deleted hidden faces, no hollow shell. Whatever
  viewmodel scheme we build, the art will hold up to close inspection.
- **Both arms are always present in the mesh** (two sleeves visible at every
  spin angle), even while only one is on screen.
- **BioShock 1 shows ONE hand at a time**: gun = right hand only, plasmid =
  left hand only, never both (own captures: Electro Bolt frames contain no gun
  at all). Dual-wield is a state-machine change, not a rendering one - and,
  usefully, it means a single matrix applied to the whole rig only ever has one
  visible arm to move.
- **Rigid roll is the visible failure mode**: rolling the rig 90 deg about the
  eye anchor throws BOTH forearms out horizontal. The gun and the hand gripping
  it stay correct RELATIVE to each other; everything from the wrist back breaks.
  Pivot choice is what governs how bad this looks.

**The load-bearing principle for the next approach - WHERE you write decides
whether attachments follow:**
- **Engine-side writes** (actor transform, bone matrices) are read by the
  engine itself, so attached objects and effects are recomputed from them and
  follow for free.
- **Render-side writes** (patching a world matrix in a constant buffer at draw
  time) are invisible to the engine, so anything drawn as a SEPARATE object -
  the plasmid's hand FX above all - stays behind at the old position.

This is the single biggest design input for M7's rebuild: prefer engine-side
(bone) writes for anything with attachments, and reserve render-side patching
for what has no engine-side handle (scale, projection).

## Skeleton / bone internals (M7-v2 session 12, 2026-07-26)

**The evaluated skeleton is directly writable, component-space, per-bone, and
the equipped weapon renders from it - all live-proven at the wall save.**
Derivation: native-table impl walks (capstone on the disk image; entry points
resolved exactly like the M6 aim natives) + one hexdump/poke session. All
constants in `patterns.h` under "M7-v2 skeleton internals".

**The native table has the full bone API** (`nativemap.py` scratchpad tool:
registration string -> 12-byte .data entry -> the `mov [entry+4], imm32`
static-init write). Kept from stock UE2: `AActor::execGetBoneCoords` 0x545090,
`execGetBoneLocation` 0x5414D0, `execGetBoneRotation` 0x541580,
`execAttachToBone` 0x541650, `execDetachFromBone` 0x541A20, `execSetBase`
0x545410, `execUpdateAttachmentLocations` 0x541A70, `execLinkMesh` 0x540C60.
Vengeance-specific: `execGetLow/HighBone{IndexFromName,NameFromIndex,Parent,
Descendants}` (dual LOW/HIGH skeleton sets - Havok Animation underneath:
hkaSkeleton/hkaSkeletonMapper RTTI present), `execSkeletonInstanceFreeze`
0x542AD0 / `Unfreeze` 0x542B00 / `IsFrozen` 0x542B30. REMOVED vs UT2004: no
SetBoneScale/SetBoneRotation/SetBoneDirection - direct array writes are the
only bone mechanism. Bonus: `execSetDrawScale` 0x5454B0 -> AActor::SetDrawScale
0x375830 (see DrawScale below).

**Object chain** (walks of exec impls; class names via MSVC RTTI):
- `actor +0x128` -> `USkeletalMeshInstance` (map at +0xE8 FName->script index
  via 0x1FA700, remap array at +0xF4 8-byte stride, validity object at +0x30).
- `instance +0x29C` -> `SharedSkeletonData` (vtable RVA 0xE1B8A8; the
  RefSkeleton equivalent; map at +0xAC = FName -> SkeletonInstance array index,
  lookup fn 0x5F6500).
- `actor +0x3FC` -> **`SkeletonInstance`** (vtable RVA 0xE19ACC, 0x9C bytes,
  factory 0x595750, ctor 0x595820): +0x04 actor backref, +0x08
  SharedSkeletonData, **+0x20 freeze flag** (int; Freeze native sets 1),
  **+0x48 bone array A ptr / +0x4C count** (by-index reads via virtual +0xB8;
  what the renderer + the weapon attachment consume - proven by poke), **+0x54
  array B ptr / +0x58 count** (the lazily-filled by-NAME path, virtual +0xBC;
  all zeros until someone asks by name), +0x80/+0x84 per-array
  time-of-last-evaluate floats (freeze only holds once > epsilon), **+0x88
  evaluate-if-dirty flag** (byte; gate virtual +0xA0, real evaluator virtual
  +0x9C), refresher for array B 0x598970 (thread-checked against the game/
  render tid globals 0x13784E0/0x13784E4).
- `actor +0x164` is NOT the UMesh - it is a wrapper synced from the instance
  (helper 0x371EF0: creates via 0x700500, links wrapper+0x44=instance,
  +0x48=actor).

**Bone format: Havok hkQsTransform, 48 bytes** - pos float3 + w (engine-owned
junk/aux - one live bone held 35.02 there; DO NOT write it), quat xyzw, scale
float3 + w. COMPONENT space (actor-local): the AHands rig live-dumped as a
coherent skeleton around the eye-anchor origin. Live pokes (freeze on):
- bone pos +30 UU on the wrist -> the hand mesh moved the same frame and the
  pose HELD (freeze blocks re-evaluation); fingers did NOT follow (per-bone
  independence - component space, no child recompute).
- bone pos +30 UU on the weapon-attach helper -> **THE ENTIRE GUN rendered at
  the new spot, undistorted** - the attached weapon's render transform is
  derived from this array. The engine-side lever M7-v2 needed.

**The AHands rig (47 bones, indices measured at the wall save, pistol raised):**
right hand cluster is CONTIGUOUS 27-44 (27 wrist, 28-30 thumb, 31-42 finger
chains, **43 weapon-attach helper** - same quat as the wrist, the gun rides it -
44 muzzle-ish tip at x=+71). Right sleeve: 24 clavicle, 25 upperarm, 26 elbow,
45/46 forearm twist helpers. The lowered LEFT arm occupies 4-23 (wrist/finger
split still to be measured with a plasmid equipped). Bones 0-2 near origin
(root/head), 3 left-of-center.

**Attachments** (AActor::AttachToBone 0x379EF0): stores the bone FName ON the
attached actor at **+0xF0/+0xF4**, then calls the attached actor's `SetBase`
(vtable +0x1A0, args newBase + floor vec3{0,0,1} + bool) and notifies the
instance (0x3FA1E0). Script side (`Hands.uc` summaries): the weapon attach is
EQUIP-TIME ONLY - `OnEquippingStarted(holdable)` -> `AttachToBone(holdable,
holdable.AttachBone)` (AttachBone = per-weapon name property); the per-tick
path (`UpdateHandValues`) only runs UpdateLocation + hand-bob parameters, so
nothing re-asserts attachment against a per-frame bone write.

**DrawScale, finally**: float at **actor +0x2AC**, written by
AActor::SetDrawScale (0x375830) together with a dirty protocol - `[actor+0xD0]
|= 0x10`, `[actor+0x3F4]++` (the render-revision counter that
execUpdateRenderRevision 0x37A370 bumps), `[actor+0x3E4] = 0` - plus
level-hash re-registration when actor flag +0x304 bit 0 is set. The session-11
"+0x168/+0x16C" probes were the wrong fields, and a raw field poke without the
revision bump is invisible anyway. Untested live so far: the AHands actor read
0.0 there.

**Write protocol used by the bone drive** (`bones.cpp`): write from the
CalcView detour (after the engine tick), then CLEAR the dirty flag so a
render-side evaluate-if-dirty cannot rebuild the pose the same frame; detect
engine re-evaluation by comparing the anchor bone against the last write
(changed = fresh engine pose -> recapture the reference); on disable set dirty
so the engine restores itself. Freeze (+0x20) is kept as a diagnostic lever,
not used by the drive - it also stops the rig's own animations, and whether it
starves the anim notifies that drive firing is UNTESTED.

## Foreground scene FOV (session 13, 2026-07-26 - the camera-coupled rig term)

**The renderer draws the first-person rig as a separate FOREGROUND scene with a
fixed projection, and that is the whole "camera-coupled rig term" of session
12 part 4.** Evidence chain, all reproducible flat:

- **2-shot discriminator**: hand world-parked via bones (`vrhands simpose`),
  camera untouched, `gfov 100` vs `gfov 137`: the world rescales, the rendered
  gun HOLDS its screen position/size. The rig is not projected with the world's
  projection.
- **Frame-dump proof** (`dumpframe full` at gfov 110 vs 137, draws classified
  by which cb0 floats respond): world draws carry proj scale m00 = 1/tan(w/2)
  exactly (0.7002 -> 0.3939) plus a screen-ray helper block at floats 12-14 of
  the form (2*tanH, 0, -tanH); the VIEWMODEL draws (the first draws of the main
  pass - index counts 7704/14595/26178 at the wall save, repeated per light
  pass) carry the same structures with CONSTANT values tanH 0.7698 / tanV
  0.4330 at BOTH world FOVs. Those decode exactly as a **60-deg 4:3 spec**:
  tanV = tan(30)*3/4, tanH = tanV*16/9 = tan(30)*4/3. Implied 16:9 horizontal
  FOV 75.2 deg.
- **Geometry of the defect**: the rig is placed by the real view transform
  (world-correct orientation) but projected through the narrow lens, so a
  world-fixed hand at view angle theta renders at atan(k*tan(theta)), k =
  tan(worldFov/2)/0.7698 (~2.1 at option 117). Camera == rig anchor in flat
  play, so theta ~ 0 and nothing shows; under HMD head-look theta = the head
  offset and the position over-pans 10-20 deg. Also explains the oversized gun
  (narrow FOV renders larger) and "FOV slider does not rescale the hands".
- **Script-side names** (BakedScripts name tables): `Engine.Controller.
  ForegroundFovAngle`, `Engine.PlayerController.DefaultForegroundFOV`,
  `ShockGame.PlayerWeapon.ZoomedForegroundFOVAngle`, FadeFOV actions with
  Start/StopForegroundFOV.
- **Dead end, recorded so nobody re-walks it**: the view PAWN (AShockPlayer)
  carries a foreground-FOV property group at +0x550 (60.0 default) / +0x554
  (36.0, zoom spec?) / **+0x558** (lerped current - the engine steers it back
  to 60 within ~2-3 s of a poke). A one-shot poke of +0x558 appeared to
  rescale the gun, but HOLDING it at another value per frame (write in the
  CalcView detour, 1700 Hz) changed NOTHING in the render - dump-proven: the
  vm draws kept tanH 0.7698 while +0x558 held 101.5. The renderer's
  foreground constants are built elsewhere; the pawn property is upstream
  script state only. (The world option int, by contrast, IS consumed per
  frame.)
- **The foreground VIEW is not the world view either** (matrix decode of the
  vm draws' cb0 across a simhead series): the rig transform maps COMPONENT
  space directly to clip; its eye sits ~32 UU behind the rig origin at a
  roughly FIXED point in ACTOR space while its ORIENTATION follows the render
  camera, and the whole thing carries the engine's idle hand sway (~+-3 deg,
  time-varying). Composite effect: self-consistent at view center (anchor
  renders within 0.5 deg of world-correct at head 0), but under camera-vs-
  actor split the rear-pivot lever translates the rig laterally: ~14 deg of
  anchor error at 30 deg head-yaw, plus a standing ~10 deg vertical lift at
  rest (the authored "raised toward the eye" composition).
- **The fix that works (bones.cpp "render lock", session 13 final)**: an
  ANALYTIC model of the foreground transform - rows x/y/w of [R | -R*E]
  scaled by (1/tanH_fg, 1/tanV_fg, 1) - solved as a 3x3 so the anchor lands
  on the world-correct pixel (natural fg depth kept, so size/perspective are
  untouched). R = (actor-inverse x camera) rotation in the rig's component
  frame composed with a constant composition bias (+1.7 yaw / +1.1 pitch
  deg); E = (-32.1, -5.6, -0.9) component UU (mean of 12 dump-recovered
  values, spread +-0.5). The correction is applied at **GAIN 0.5**: with the
  drive writing bones every frame the engine switches rig sections to a
  rigid path whose per-section matrices REBUILD from the driven bones, so a
  bone move lands on screen roughly TWICE - flat-measured (gain 1.0 turned a
  14-deg cancel into ~25 deg of effect; 0.5 holds the anchor within 2-4 deg
  of world-true through +-30 yaw / +-20 pitch simhead sweeps). Two modes:
  `vrbones lock abs` (default - anchor to the TRUE world position, which
  also drops the authored raised/too-close composition onto the real
  controller spot) and `lock diff` (cancel only the head-split term, keep
  the authored composition); `lockgain` tunes the feedback compensation.
- **Residual floor**: the hand sway wobbles the fg view +-1.7 deg and the
  narrow lens amplifies screen error by k = tan(worldFov/2)/0.7698 (~2.1), so
  the rig breathes ~+-3.5 deg - the same liveliness vanilla shows. To go
  lower, kill the sway at its source (the UpdateHandValues bob parameters) -
  queued, not done.
- **Capture dead-ends, recorded so nobody re-walks them**: frame_inspector
  gained a generic cb watch (Map/Unmap hook - CPU-side, free per-frame
  capture of any fingerprinted constant buffer), and it works mechanically -
  but captured vm matrices CANNOT feed the solve: (a) with the drive live the
  per-section matrices embed the very bones we write (solving against them is
  a feedback loop - it settles at a wrong fixed point or oscillates); (b) the
  832/1088-byte cb tiers and the 480x270 postprocess pass carry the SAME
  f12 fingerprint with foreign transform content. The watch stays in core as
  a diagnostic instrument.

### Session 14 (2026-07-27) - the depth constraint: size, stereo depth, parallax world-correct

- **The fg eye RIDES THE CAMERA, translation included** - settled by dump:
  drive on, lock off, `offset 0 30 0` (render-camera-only translation) moved
  the matrix-recovered eye by (-0.3, +29.7, +0.9) - the full offset. The
  session-13 model's actor-anchored E could never produce the measured 1.35x
  parallax over-response; a camera-riding eye produces it exactly. The model
  now composes eye = camComp + rotate(qd, pull-back), camComp = qaInv *
  (ctx.cam - actorLoc).
- **The matrix-recovered eye is SECTION-FRAME-RELATIVE; its absolute value is
  NOT recoverable from captures.** Solving rows*e = -translation per vm draw
  gives a=7704 -> (-66.4, -19.6, 9.5) and a=14595 -> (-51.6, -20.6, 21.9)
  with the drive OFF, and (-32.6, -3.8, -1.7) for a=7704 with the drive ON -
  each is e_true minus that section's own frozen frame offset. Session 13's
  E (-32.1, -5.6, -0.9) is the a=7704 drive-on value, i.e. ts-contaminated;
  with it the model's natural depth ran ~1.63x the real one (49.5 vs ~30 at
  the parked pose). kFgEyeComp stays recorded for its LATERAL components
  only (statics the abs solve absorbs).
- **The TRUE pull-back, physically calibrated: kFgEyeFwdBehindCam = 13.0 UU
  (rendered fg depth = df + 13, df = target forward distance from the
  camera).** Three independent instruments agree: camera-offset parallax
  (lock off, 10 UU offset moved the gun 420 px through the 0.7698 lens ->
  w 29.9 at df 17.4 -> P 12.5); size ratio on hand-distance doubling (0.605
  at df 17.4->37.4 -> P 13.2); the user's perceived ~28 cm at a ~35 cm hand
  -> P 12.3.
- **The depth constraint (the session-14 fix, bones.cpp)**: the render-lock
  third solve equation is now w* = k*df, k = tan(worldFov/2)/tan(fg 30 deg)
  = tanH * kFgInvTanH (~2.12 at option 117, ~3.30 at 137), built on the
  physically-scaled eye. Apparent size, stereo disparity, and translation
  parallax are the same (1/w)*k geometry, so the one constraint fixes all
  three. The correction returns split lateral/depth (along the fg forward)
  with separate refusal caps (30 / 120 UU) and separate gains.
- **Session 13's "rebake doubling" DECOMPOSED**: the gain-1.0 overshoot
  (14-deg cancel -> ~25 deg effect, 1.79x) = model depth-scale error (1.63)
  x true rigid-path rebake (~1.1). With the model scale corrected, both axes
  run gain ~1/1.1: `vrbones lockgain 0.9` (lateral) + `lockdgain 0.9`
  (depth), both live knobs and overlay sliders.
- **Flat-stereo acceptance (vrstereo on throughout; baselines replicated
  first on the same harness)**: camera-offset parallax 420 px (1.23x world)
  -> 355 px vs 341 world-correct (1.04x); size-on-distance-doubling 0.605 ->
  0.465-0.470 vs 0.465 exact; fg depth band tolerates wSolve 142 (FOV 137 +
  hand 40 cm out - rig intact, the clamp fallback never fired); simhead
  +-30 yaw / +-20 pitch sweep stays glued (no regression vs g5); fire test
  57->55 with a fresh decal, dumps 8->8.
- **Instrument caveats, so nobody re-walks them**: (a) with the drive ON,
  `camrot` does NOT rotate the fg view (composite followed ~3 of 40 deg -
  the rigid path orients by the ACTOR/pawn view) while `simhead` DOES (~16
  of 20 deg pitch) - so camrot is not a head-look stand-in under the drive;
  simhead remains the valid one. (b) Window captures are eye-phase-locked
  (same stereo eye every shot - session 6's finding reconfirmed), so
  disparity cannot be measured from screenshots; size and offset-parallax
  carry the same (1/w)*k information. (c) With lock ABS live the gun sat
  pixel-identical across a shot series (the per-frame ndc re-pin), where
  lock-off series wobbled +-16 px - the vanilla sway may be partially
  absorbed by the lock now; not conclusively separated from a quiet sway
  phase. (d) Template-based pixel measures go multimodal when the gun's
  composition moves between builds - re-localize the gun (crop + look)
  before trusting any cross-build A/B.

### Session 15 (2026-07-27) - THE FOREGROUND FOV FIELD, and the dolly that guards it

**The strategy pivot (user's call): stop countering the fg pipeline from
outside - patch its inputs at the source.** Outcome: half of the pipeline
(the LENS) fell to a single field write; the other half (the fov-coupled eye
dolly) is located behaviorally but its source survives the night.

- **THE REAL FOREGROUND FOV: a float on the PLAYERCONTROLLER at +0x460
  (patterns.h kPcForegroundFovOffset), consumed by the renderer EVERY
  FRAME.** Found by float-sweeping the live PC (fsweep, new command) after
  every scan for the derived tan constants came up empty (they are computed
  per frame - zero disk hits, zero stable memory copies in any
  representation; the one 0.4330127 hit is the engine's trig lookup TABLE
  entry the computation itself samples). Live poke: the whole rig re-lenses
  THE SAME FRAME. Dump-proven: at the world-equivalent value every vm draw
  (576 tier AND the 832/1088 lighting tiers) joins the world's projection
  cluster - one field feeds all passes; the multi-builder risk never
  existed. The value is a 4:3 spec: world-match = 2*atan(tan(worldFov/2)*
  3/4) = 101.5 at option 117 - the exact number session 13 computed and
  held at the WRONG address (pawn+0x558, which is EYE HEIGHT - the
  session-11 identification was right and the session-13 property mapping
  wrong; the 60.0/36.0 there are stand/crouch eye heights).
- **Shipped as `vrfgfov on|off`** (camera.cpp): per-frame write of the
  world-equivalent spec from the live FOV option, post-tick so nothing
  fights it (a poke held for minutes; no lerp-back), with save/restore.
  Through it the rig renders with the world lens: correct internal
  perspective, the arm+sweater composition visible, size plausible - the
  telephoto-pasted-into-wide-angle ceiling of every bone-space counter is
  GONE while it is on.
- **The remaining defect: the fg eye DOLLIES BACK by a fov-coupled amount.**
  Measured by offset-parallax with the VANILLA rig (drive off): rendered
  depth = df + pull, pull ~13 UU at the 60-deg lens (matches session 14's
  calibration) and ~65 UU at the matched lens - a tan^2-like growth. The
  dolly preserves the authored framing under fov changes (that is WHY the
  lens field alone does not finish the job).
- **The dolly's source was NOT found tonight**: no stored 13/65/38.5-ish
  field on the PC; the promising 22.0 pair at PC+0x2FC/+0x300 and the 75.0
  at +0x45C poked inert (live + re-equip). Next instrument (built, unused):
  `fgstack` logs the cb writer's callstack - frames 0x7C3044 (uploader,
  from a shadow buffer at [obj+0x480]) <- 0x789D92/DF7 (commit) <-
  0x7661B3 <- 0x77DC1E <- 0x60ECCA <- per-section 0x3DBF7C/0x3EDCBF -
  disassembling the transform build upstream of the commit is the bounded
  session-16 route to the eye computation.
- **HARD CONSTRAINT, flat-proven: the dolly cannot be countered through
  BONES at the matched lens.** Pulling the anchor 65 UU toward the camera
  places the cluster behind the WORLD camera and the engine's visibility
  system culls the entire rig (blank hand). Session 14's counter only
  worked because the narrow lens demanded pushes AWAY from the camera.
  Corollary: the depth term must die at its source, or the lens must stay
  unmatched while bones push outward (the session-14 configuration).
- **Vanilla vs driven path render DIFFERENT pulls.** With the drive on at
  the matched lens the parked fist rendered huge (near), not 65-deep -
  the rigid rebake path's eye/translation behavior differs from the
  vanilla path's, and the session-15 lockpull calibration (65, vanilla)
  does not transfer. The drive-on pull at the matched lens is UNCALIBRATED;
  measuring it with the standard offset/size harness is session 16's first
  hour. Until then `vrfgfov` DEFAULTS OFF and the shipping behavior is
  byte-for-byte session 14's verified configuration (the matched-lens lock
  plumbing - live invTan scales, k=1 collapse, `vrbones lockpull` - is all
  in place behind the toggle).

### Session 16 (2026-07-27) - the drive-on pull calibrated SMALL; the matched lens ships ON

**The decision-hour measurement came out branch (a): the driven rigid path's
pull at the matched lens is +11.5 UU - not the vanilla path's 65.** Measured
at lock abs + lockpull 0 on the wall save (rendered depth = df + pull), by
two independent instruments that agreed within ~1 UU: offset-parallax (gun
shifted -202 px vs -343 world-correct at df 17.3 -> pull +12.0) and
size-on-distance (hand +20 UU: ratio 0.585 vs 0.465 -> pull +10.8). The
value sits near the stock-lens 13 (kFgEyeFwdBehindCam), far from the
vanilla-path 65: **the driven path's eye offset is NOT fov-coupled** - the
fov-coupled dolly session 15 measured belongs to the vanilla rig path only,
and it never applies to the rigid sections rebuilt from our driven bones.

- **Knob vs gain**: the applied depth correction lands at exactly
  dgain*solve - residuals at lockpull 11.5 measured +1.1-1.4 UU on both
  instruments = the 0.9 gain, no rebake amplification on this axis. The
  default knob is therefore 11.5/0.9 = **12.8** (lands 11.5 physical); if
  lockdgain changes, the effective pull moves with it.
- **THE PULL FRAME - found by the simhead sweep, the one discriminator the
  zero-split calibration cannot cover.** With eTrue rotated by the
  camera-delta quat (qd), the gun over-shifted the world under head-split by
  exactly pull*sin(split)*gain on BOTH axes (yaw+30: 194 px predicted, 194
  measured; pitch-20: 134 predicted, 137 measured). **The renderer's eye
  offset does NOT swing with the camera-vs-actor split** - the matched path
  now rotates the pull by the constant view bias only (identical at zero
  split, where the A/Bs calibrated it); the unmatched session-14 path keeps
  the qd rotation it was verified with. Post-fix sweep: gun world-glued
  within 2-17 px across +-30 yaw / +-20 pitch (gun template corrs .88-.96).
- **Clean-boot acceptance with shipping defaults** (vrfgfov ON, lockpull
  12.8), all under vrstereo: far-range offset parallax -156 px vs -159
  world-correct (0.98x); size ratio 0.475 (tight-template) / 0.489 (direct
  disc-width profile) vs 0.465 (1.02-1.05x); sweep world-glued as above;
  fire test 6 pulls, ammo 59->53, fresh wall decal, dumps 8->8; stereo
  heartbeat clean throughout (mode=1T, presents = 2x builds, guardskips 0).
- **Instrument findings (each cost real time tonight)**:
  1. The offset-parallax template instrument is INVALID at close range: at
     rendered ~17 UU a 10 UU lateral offset is a ~30-deg viewpoint change
     and the gun spans ~10 UU of depth, so per-feature shifts range
     270-456 px and NCC returns feature-dependent garbage (three templates:
     -172/-252/-260). Run the parallax A/B at the far parked pose
     (vrhands pos 40 0 0, rendered ~37 UU), where two templates agreed.
  2. Big mixed templates (gun+fist+background) false-match on SCALE (0.345
     vs the true 0.475 from a tight disc-only template, cross-checked by a
     dark-pixel disc-width profile at 0.489).
  3. The skin-tone fist blob is defeated by the wall save's teal lighting
     and finds the golden statue instead - localize by crop-and-look.
  4. `simhead` RECENTERS onto its first pose: establish zero with
     `simhead 0 0 0` FIRST, or the first sweep step measures nothing.
  5. With no aim pose source (no headset and no `vraim test`), fired bullets
     follow the center crosshair - the impact hides BEHIND the parked gun.
     Decal evidence needs `vraim test r <yaw>` to land in the open.
  6. The wall-save boot's A-press loop can leave the MAP screen open
     (A = zoom on that screen); verify the screen state by screenshot
     before any series (press B to close).
- **Behind-camera edge at the matched lens**: the corrected anchor sits at
  df - 11.5 UU from the camera, so a hand closer than ~11.5 UU (~23 cm real)
  puts the cluster behind the world camera and the engine culls the rig
  (session 15's constraint, now with an 11.5 threshold instead of 65). The
  wStar<4 refusal covers df<4; between ~4 and ~12 the rig may blank. Listed
  as an in-headset checklist probe.

### Session 16 part 2 (2026-07-27) - in-headset core verdict + the viewmodel-scale wall

**In-headset, same night: the core WORKS - the model follows the controller
with head-look fully decoupled (user: "fully working... amazing progress").**
Remaining user reports: the model reads far over life size (hand ~ head
size, weapon ~ torso+head), and a laser-vs-weapon lateral flip at large
right-aim angles (under investigation - see STATUS).

**VIEWMODEL SCALE: three levers probed flat the same night, all DEAD - the
factor needs render-path work.**

1. **Cluster bone .s (hkQsTransform scale field)**: the SKINNED geometry
   scales correctly (fist halved at .s 0.5), but the ATTACHED WEAPON blows
   up to near-plane huge - the attach path decomposes an INVERSE-scale term
   out of the wrist chain. Monotonic with s (0.8 -> moderately oversized gun,
   0.5 -> drum chambers filling the screen).
2. **Same, with the attach helper (bone 43) kept at authored scale**: the
   IDENTICAL blowup - the attach math reads the chain (wrist 27 et al), not
   the attach bone's own scale field. Any scale anywhere in the chain is
   fatal to the attached weapon.
3. **Rig-actor DrawScale (+0x2AC via the dirty protocol) + drive positions
   pre-divided by s**: no blowup, anchor stays pinned (bone positions
   round-trip through the scale - written p renders at s*p), but the
   GEOMETRY is inert: gun width 240 -> 234 px at s=0.5 (2%, not 50%) and the
   fingers compress to half-spacing at full size. The fg rig path consumes
   actor DrawScale for bone translations but NOT for skin/attached-mesh
   size.

Untested in isolation: DrawScale on the WEAPON actor (its effect was masked
by the chain blowup in the combined test) - it could at best scale the gun,
never the hand. Next-session routes: disassemble the attach-matrix build
(AActor::AttachToBone 0x379EF0 / the per-section fg bake at 0x3DBF7C -
find where chain scale is consumed and where a clean scale can be injected),
or take size control in the vm_draw replay lane where we own the matrices.
The `vrhands scale` command stays as an honest not-yet message; no scale
knob ships.

**Harness note (cost a false alarm): with the Virtual Desktop / Quest menu
open the XR session drops FOCUSED -> VISIBLE, the runtime stops delivering
controller poses, and the drive receives an IDENTITY pose - the model parks
dead-center as if stuck.** Log signature: `xr: session state VISIBLE` +
hands target rot pinned to (0, camera-yaw, 0). Close the overlay and it
resumes. Also noted: `xr: sr tag ring skewed (depth 8) - cleared` repeats
while the session is VISIBLE/idle with presents at 0 - harmless in that
state, recovers on resume.

## Body facing / control rotation (M7.5 session 17, 2026-07-27)

**THE PLAYERCONTROLLER'S ROTATION IS AT `PC + 0x1E4`, it is the view rotation
the camera comes from, an ADDITIVE YAW WRITE TO IT STICKS, and it reaches
locomotion.** Nine-step live probe on the wall save, one boot, all positive -
the `vrbody probe` telemetry line plus the `vrbody poke <deg>` one-shot write
(both in `body.cpp`). This closes the session-11 open question and overturns
the ARCHITECTURE decision-log entry that rejected writing the controller
rotation.

- **It is the AActor layout, inherited.** `AController` derives from `AActor`,
  so `kActorViewDirOffset` (0x1E4, already used on AHands and the pawn) is the
  controller's `Rotation` too. Live: `PC.rot=(144 116 0)` vs
  `pawn.rot=(0 116 0)` - **same yaw, and the PC carries a NON-ZERO PITCH while
  the pawn's is 0**. That is the discriminator: session 11's inference
  ("pawns keep pitch 0, so the pitched view rotation lives on the
  PlayerController") was right, and this is the field. Yaw is the second
  int32, i.e. `PC+0x1E8`.
- **An additive write LANDS and HOLDS.** `vrbody poke 40` wrote yaw
  116 -> 7398; the camera swung 40 deg and the value held across 11 telemetry
  samples over 5.5 s. Nothing recomputes it from another authority.
- **The pawn follows for free.** `pawn.yaw` tracked to 7398 on the next sample
  (the engine's own FaceRotation propagation), so a **PC-only write is
  sufficient** - no pawn write, and therefore no risk of desyncing a colliding
  actor's rotation. `vrbody field pawn|both` exists but is not needed.
- **The engine's own turn COMPOSES on our value.** A synthetic right-stick
  turn took 7398 -> 13324, continuing from the written value rather than
  snapping to a stick-derived absolute. The rotation update is incremental
  (`Rotation.Yaw += delta`), which is what makes an additive transfer safe
  alongside normal player turning.
- **It reaches locomotion.** With the body poked to yaw 13324 (73.19 deg), a
  synthetic left-stick-forward burst walked 780 UU along heading **72.79 deg**
  - a 0.4 deg match to the written facing, and ~73 deg away from the original
  one. So `PlayerWalking` composes movement against this rotation (directly or
  through the pawn copy).
- Pitch is deliberately never written: the engine applies a signed clamp to it
  and the pawn is kept at pitch 0 by design. Yaw only, masked `& 0xFFFF` (the
  engine keeps components in [0, 65535]; the live read of 55599 and 13324 are
  both in range).

**THE YAW-TRANSFER INVARIANT (why this cannot re-couple the hand).** The final
camera and BOTH halves of the XR-controller-to-world mapping are functions of
one composite:

```
camera yaw = gameYaw + (headYaw - recenterYaw)
hand rot   = (gameYaw - recenterYaw) + controller yaw
hand pos   = base + R(gameYaw - recenterYaw) * xr_to_ue(pos - recenterP) * scale
```

So adding T to the body while adding exactly T to the recenter reference
leaves the camera and the mapping unchanged - only the body/head-look SPLIT
relabels. Done in integer rotator units it is exact:
`rot->yaw' = (gameYaw + T) + (residual - T) = gameYaw + residual`. That is why
`camera.cpp` keeps the recenter yaw as `int32_t g_recenterYawUnits` (also
wrap_rot-bounded, so it cannot accumulate into ulps larger than a rotation
unit) and why `body::on_calcview` RETURNS the units actually committed rather
than the units requested - the two absorbed quantities are the same integer.

**Flat-measured, wall save, one boot (session 17):**

- Composite `gameYaw - recenterYaw` = **1.27742 rad at every head angle**
  (0/30/45/90/-45, transfer on and off) - five decimal places, unchanged.
- True A/B at head 45: transfer OFF vs ON, the hand's world pose from the
  `[tlm] xrmap` fixed-pose probe read `rot.yaw=13323` in **both** and
  `camYaw=21516` in **both**, while gameYaw and recenterYaw each moved
  +0.78540 rad. Position differed by 0.001 UU (float rounding; the gate was
  0.05).
- `[tlm] yawstep max=+0 units, nbig=0` across the arm transient and steady
  state at ~1650 frames/s - the camera does not move at all when the transfer
  arms, not merely "within tolerance".
- Walk direction, transfer **OFF**: 116.49 deg and 116.67 deg at two head
  angles **90 deg apart** - the walk tracked the body (118.19) and ignored the
  head. That is the reported defect as a number.
- Walk direction, transfer **ON**: at head +45, walk 163.19 / body 163.19 /
  camera 163.19; at head -45, walk 73.20 / body 73.19 / camera 73.19. Walk ==
  body == camera to 0.01 deg, and the pair spans 89.99 deg for a 90 deg head
  change. Both bursts >800 UU with 0.25% straightness.

**Instrument caveat recorded the hard way:** a walk burst that slides along
collision geometry yields a plausible-looking heading that is pure geometry
(one burst read 105.65 deg with 16% perpendicular deviation). Gate every burst
on straightness <= 5% of path length and treat a failure as VOID, not as a
result.

**THE RENDER LOCK'S CORRECTION GOES FLAT UNDER HEAD-LOOK - the transfer's
second, unplanned payoff.** Parked hand, `simhead` swept +-30 deg about a
45 deg base, `[tlm] lock` read at every step. The predicted world NDC (`tgt`)
and `df` came out **identical at every step in both conditions** (the hand is
world-parked and the camera is unchanged, exactly as the invariant requires),
and `targetYaw` held at 16500 throughout. What changed was the correction:

| head delta | predicted ndcX | `lat` OFF | `lat` ON | actorYaw OFF | actorYaw ON |
|---|---|---|---|---|---|
| -30 | +0.088 | 1.04 | 4.50 | 16500 | 19231 |
| -15 | -0.073 | 2.67 | 4.50 | 16500 | 21961 |
| 0   | -0.245 | 6.12 | 4.58 | 16500 | 24692 |
| +15 | -0.458 | 9.16 | 4.73 | 16500 | 27423 |
| +30 | -0.779 | 11.59 | 4.96 | 16500 | 30153 |

With the transfer OFF the lateral correction swings **10.5 UU** across the
sweep (it scales with the camera-vs-actor split, which is what the lock exists
to cancel); with it ON the correction is **flat within 0.46 UU** and actorYaw
tracks the camera in exact 2731-unit (15 deg) steps. `depth` behaves the same
way: a 6.1 UU swing becomes 1.0. Because the lock is applied at gain 0.9, a
swinging correction leaves a swinging residual - the "gun drifts as you look
around" percept - while a constant one leaves a constant, trimmable offset.

**And the ON value lands on the calibrated one: `lat` 4.58 at head 45 with the
transfer on vs 4.57 at head 0 with it off** - i.e. the transfer restores, at
every head angle, the exact zero-split configuration that session 16
calibrated and the user verified in the headset.

**The rendered viewmodel DOES move between the two states, and that is the
mechanism working, not a regression.** At head 45 the world region of the
frame is pixel-identical off vs on (mean abs diff 0.048, against a 0.3
same-condition floor) while the gun+arm region reads 23.4 - and crop-and-look
shows the gun is not merely shifted but **viewed from a different angle**. The
renderer orients the foreground rig by the ACTOR fields, so with the transfer
on the rig is viewed from the camera's own orientation instead of from a
45-deg-stale body facing. The lock corrects POSITION, never viewing angle,
which is why it could never fully paper this over. Removing the split at its
source is what the ENGINE_NOTES session-12 finding ("the user saw the gun move
REVERSED... actor-frame rendering composed against the camera frame") was
always pointing at.

**Instrument that failed, recorded so it is not retried blind:** tracking the
gun across the sweep with an NCC template - even re-localised every step and
re-cut from the previous step's match - produced correlations of 0.56-0.79 at
every step but the trivial self-match. The composition changes too much per
15 deg for template chaining to survive, so every pixel number from it is
VOID. The `[tlm] lock` telemetry above is the template-free instrument that
answers the same question, and it should be preferred.

**Nuance the flat sweep exposed - the cull is direction-dependent.** `simpose`
parks the synthetic hand in the RECENTER frame, i.e. it models "the head turns
but the hand stays put in the world". In that case the transfer *increases*
hand-vs-body (0 -> -45 deg at head 45, measured: actor yaw 21516 vs target yaw
13323). The reported symptom is the other case - the user physically swivels,
so head AND hand rotate together in the world while the in-game body does not
- and there the transfer drives hand-vs-body to ~0. Both are real; the
residual/cull sweep is the instrument that quantifies where the boundary sits
in each direction.

## Desktop present / mirror + hand attribution (M8 session 18, 2026-07-27)

**Why the session-17 screenshots were all the same eye phase - the desktop
present duty cycle is heavily skewed, not 50/50.** Under SequentialReentry
with pair pacing, the two presents of one stereo pair are separated only by
the time the game takes to BUILD the right-eye frame (~1-3 ms), while the
right-eye image then stays on the window through the whole next pair's
blocking xrWaitFrame (~8+ ms at 90 Hz, or the full inter-pair gap flat). A
DWM-sourced capture (PrintWindow/screenshot) samples the composed surface and
therefore lands on the SECOND eye of the pair with high probability - 12/12
same-phase shots is expected behavior, not evidence that alternation is gone.
The alternation is real (a 60 Hz recorder catches left-eye slices as flicker);
it is just not uniform. Session-8-era captures caught both phases because
pair pacing did not exist yet - the pair was split by a blocking wait.

**The mirror fix (M8b)** rides the same sr eye tags the capture uses: left
presents snapshot the backbuffer into a held texture (read-only), right
presents copy it back over the backbuffer AFTER the right eye's XR capture,
so the real Present always displays the left eye and the compositor feed is
untouched. It also runs on presents with no open XR frame (pace-guard skip,
session gone) since the game keeps presenting alternating eyes there. Flat
acceptance 2026-07-27: within-condition consecutive-shot img-diff 0.31-0.73
(the floor) for mirror on AND off - both phase-locked, as the duty cycle
predicts - while the cross-diff mirror-on vs mirror-off is 13.6-13.9 (the
full near-field eye offset at worldScale 100): the pin flips WHICH eye the
window shows. Counters: holds == blits exactly, one per pair.

**Grip-switch wrong-controller bug: root cause is attribution, not the game.**
The grips compose to the BUMPERS (LB/RB) in the M5 mapping, and a bumper
press switches the raised hand (LB -> plasmid, RB -> weapon) WITHOUT any
trigger event. `hands::active_hand()`'s auto latch learned only from the
composed triggers, so a grip switch left the latch (and with it the bone
drive and the laser) on the stale controller until the next trigger pull -
which also explains the reported self-correction after one shot. Fix: the
latch now learns from the composed bumpers too (bumpers checked first,
triggers second so a same-frame fire wins). Flat-proven on a clean boot:
after two right pulls the status reads auto(R); a single LB press flips it
to auto(L) with no trigger; RB flips it back. The fire-direction was never
wrong: the aim seams attribute weapon-vs-plasmid structurally (which C++
seam fired), so only the model/laser lagged.

**xrWaitFrame under an idle headset (VDXR observation, M8a).** When the
headset idles the session drops FOCUSED -> VISIBLE and per-present
xrWaitFrame starts blocking for seconds (flat window <1 fps, presents=0/s).
The pace guard skips the blocking wait once a session that HAS been FOCUSED
leaves that state; bring-up is exempt (SYNCHRONIZED -> VISIBLE -> FOCUSED
requires submitted frames to advance, so a naive not-FOCUSED gate would
deadlock session start), and a 5 s keepalive still paces one real frame in
case the runtime wants frames before re-granting FOCUSED. The keepalive's
block trips the reentry watchdog's detect-only log line - expected noise.
`xrWaitFrame` block durations now log when >1 s, so the first real headset
idle will tell us how VDXR actually behaves (event-driven recovery vs
keepalive-dependent).

## Reticle + the engine SET seam (M8 session 18 part 2, 2026-07-27)

**The flat crosshair has a first-class engine off-switch: `ShockPlayer.
bReticleDisabled`.** ShockHUD.RenderReticle's FIRST branch checks
`ShouldHideReticle()` and pushes the type string "NoReticle" to the flash HUD
movie (`CallMethodString("SetReticleInfo", ...)`) - the game itself then
hides the reticle every frame. `DisableReticle`/`EnableReticle`/
`ShouldHideReticle` are three-line script functions around that one bool
(read straight from the SOURCE TEXT embedded in ShockGame.U - see below);
nothing else in ShockGame calls them, so nothing fights a write. Verified
live both ways: set true -> the ornate ring at screen center vanishes
(19 -> 0 bright pixels in an 80x80 center crop), set false -> it returns.

**The bigger find: the engine's console `SET <class> <prop> <value>` handler
WORKS through the exec seam** - `exece set shockplayer breticledisabled true`
returned HANDLED and took effect. That means ANY script property is now
writable BY NAME with no offset, no bitmask, no reflection walking: the
engine resolves the property itself. SET also writes the class default, so
newly spawned instances (load crossings) inherit the value. This closes the
gap session 9 left ("script-command path needs the player-object Exec
signature") for the entire SET/GET family. Caveats: it writes ALL instances
of the class plus the default (fine for player-singleton classes); property
GETs still need `get` (untested).

**Package source-text extraction, the method.** UELib fails to deserialize
~40% of UFunctions on this licensee build (the reticle trio included), but
the packages embed the full UnrealScript SOURCE as UTF-16 TextBuffer objects.
Byte-scan the .u for UTF-16 identifiers - CHECK BOTH ALIGNMENTS (a name at an
odd byte offset is invisible to an even-aligned decode; the reticle
functions sat at odd phase) - then read the surrounding text. Faster and
more complete than decompiling when it works; `tools/uscript/` stays the
workspace, findings summarized here, never the source itself.

**Aim-ray origin offsets (session 18 part 2, user request).** The model
offsets move the MESH about its pivot, so a tuned model can sit visually
right while the aim ray no longer runs along the barrel. New per-hand
`vraim pos [l|r] <fwd> <right> <up>` (cm) moves the RAY: applied once at ray
build in aim.cpp along the FINAL (trimmed) ray's zero-roll basis, so the
laser, the fire-origin substitution, and every other g_ray consumer move
together by construction (the laser applies the same cm offset XR-side,
meters, same basis convention: right = ray x world-up). Flat-proven exact:
`pos r 0 60 0` moved the R ray origin (-0.7, +60.0, 0.0) UU (the 0.64 deg
yaw cosine leak on X), `pos l 0 0 45` moved L by +45.0 UU up, each hand
isolated, 2 fire-seam substitutions carried the offset ray, vrpreset
round-trip (save -> zero -> apply restores).

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

## Session 19 - the gameswf HUD pipeline, the pad layout, and hide-inactive constraints

### The in-game HUD fingerprint, corrected (supersedes the session-6 note)

Frame-dump ground truth (stereo gameplay, lite+full dumps, 2026-07-28): the whole
gameswf HUD is drawn EVERY present interval (both eye passes) as a contiguous run
of ~119 NON-INDEXED `Draw` calls on the TONEMAP TARGET (backbuffer-sized RGBA8,
RTV|SRV) **with the scene DSV still bound** (depth-testing off in state) - the
session-6 "no DSV bound" detail was inverted for gameplay. The tonemap itself is
the interval's only no-DSV draw on that target and samples the HDR scene RT
(R11G11B10F). The world renders exclusively via `DrawIndexed`. Every HUD draw's
stack carries the gameswf batch-flush at exe RVA **0x7B8EB5** (above the shared
draw helper 0x765C1C), with the recursive movie-clip walk 0x7ED6A1 deeper in.
DrawAuto/Dispatch/ExecuteCommandList are never used (census hooks added, all 0) -
no deferred contexts anywhere.

Classifier shipped in `core/gfx/hud_capture.cpp` (per present interval): the
scene RT is the resource hosting the most DSV-bound DrawIndexed calls (vote, >=32
wins); the first non-indexed draw on an LDR-1080 target sampling the vote leader
is the tonemap and marks that target; every later non-indexed draw on it is HUD
and gets our RTV substituted at draw time (through the ORIGINAL OMSetRenderTargets
so the classifier's own binding state stays honest). Flat: ~119 hudDraws per
interval, leaks=0 (nothing DrawIndexes the target post-tonemap), menus never arm
(no scene votes), the pause menu redirects too (world keeps rendering under it -
in-headset it lands on the readable quad).

### gameswf destination alpha is garbage - repair before blending

The capture RT (cleared to 0,0,0,0) receives gameswf output whose RGB is
premultiplied BY CONSTRUCTION (SrcAlpha*src + InvSrcAlpha*black), but the alpha
channel ends up unusable - a SOURCE_ALPHA consumer draws nothing. Shipped repair
(`core/gfx/blit.cpp` ps_process): alpha = max(stored, saturate(luminance*2.5)),
rgb untouched, blend OFF, into a second RT; every consumer (window composite,
XR quad) reads the processed copy with PREMULTIPLIED blending (ONE/INV_SRC_ALPHA;
quad submits WITHOUT the UNPREMULTIPLIED flag). Cosmetic cost: semi-transparent
HUD glass reads slightly more vivid.

### Frame dumps and the stereo pair phase

A dump armed from the command seam (game thread, CalcView poll) always opens on
the SAME phase of the stereo pair - single-window dumps can never see what the
other half draws (this hid the HUD for half a session). `dumpframe [full] [n]`
now records n consecutive present windows (files suffixed `_qN`).

### The gamepad action layout (User.ini XENON_*, flat-verified)

A=Use, B=UseHypoOfType MedHypo (heal), X=Hack|Reload|InjectBioAmmo (contextual),
Y=Jump, LB/RB hold=ability/weapon radial (sticks feed xRadial* while held),
LT/RT=SwitchAndFireAbility/Weapon, LS=Duck, RS=ZoomCycle, START=Pause,
BACK=ShowContextHelp, DPAD_RIGHT=hints. **DPAD_UP and DPAD_DOWN cycle the
equipped weapon's AMMO TYPE** (CallHudFunction DPadUp/DownPressed - flat-proven:
00 Buck -> Electric Buck -> Exploding Buck on the shotgun). The VR bindings
re-route the face buttons XR-side (openxr_input.cpp): Touch A->XInput Y (jump),
B->A (use), Y->B (heal), X->X; right-stick Y flicks pulse DPAD_UP/DOWN for ammo
(rising edge past 0.65 pre-deadzone, re-arm inside 0.30, 300 ms cooldown,
suppressed while a grip is held - the radials read the stick).

### Hide-inactive: the attach bone must never be scaled

`vrhands hideinactive` collapses the inactive hand's cluster+sleeve by zero
scale (positions pinned at the driven target), EXCEPT the weapon-attach bone 43:
the engine's attach path inverse-decomposes chain scale (session 16 - any wrist
chain scale blows the weapon up near-plane; zero would be 1/0), so the equipped
weapon hides by TRANSLATING bone 43 to (0,0,-5000) component space instead
(frustum-culled, scale untouched). Restore comes from g_ref BEFORE the incoming
hand is driven on a switch - the rigid write sets p/q but never .s, so a stale
zero scale would leave the hand invisible. An engine re-evaluation rewrites the
whole array scales included, so g_ref can never hold our zeros.

### The strict gameplay-view signal

`body::is_gameplay_view` (ShockPlayer vtable on the view actor, no viewActor==pc
escape hatch) is now computed every CalcView and published to the input bridge as
the stick-pitch-kill gate, and logged on transition as
`[b1r] view state: GAMEPLAY (ShockPlayer view)` / `menu/cutscene` - the harness's
generic "save is loaded" detector (tools/boot.ps1). The intro and main menu read
menu/cutscene (viewActor==pc there); the transition to GAMEPLAY fires exactly at
save load, any save, any level.

## Session 20 - the two trim algebras: measured divergence baseline (synccheck)

The session-18/19 root-cause claim ("the ray trims via rotator ADDS in game
space, the model trims via a QUAT COMPOSED in the controller's local frame -
they agree only at the tuning pose") is now MEASURED. Both pose->rot chains
were refactored into pure functions in `frame_context.h`
(`ray_pose_from_xr`, `model_pose_from_xr`) that production (aim.cpp,
hands.cpp) and the new `vraim synccheck` sweep share, so the sweep measures
the real shipping code. The sweep drives ~21 axis-angle controller
orientations (identity, +-45/90 per axis, 180 roll, mixed axes) through both
chains against the cached last FrameContext and prints the angle between the
ray direction and the model barrel direction.

**Baseline (2026-07-28, pre-unification build, clean boot, NG+ Medical
Pavilion, vrpreset armed):** with a canonical 10/10 trim fed IDENTICALLY to
both chains (so every degree of divergence is pure algebra difference):

- identity pose and all pure-YAW poses: 0.00 deg (rotations about world up
  commute with the yaw add - the algebras agree exactly there, which is why
  eye-tuning at a neutral pose always "worked")
- pitch poses: 4.14 deg at +45, 1.76 at -45, 11.58 at +90
- ROLL poses (about the view axis): 10.70 deg at 45, 19.85 at 90,
  **28.21 deg at 180 - the maximum of the whole sweep**
- mixed axes: 2.33-13.70 deg
- **MAX canonical divergence: 28.21 deg.** Roll is where the two algebras
  differ most - an XR-local-euler sweep (no rolled poses) would have
  under-reported the defect badly.

Live-trim sweep on the user's tuning (ray L +0.2/+17.7, R 0/0; model trims
all 0): liveR ~0.0 everywhere (zero trims = both chains degenerate to the
same map), liveL up to 17.70 deg (the L ray trim has no model-side
counterpart by construction of the tuning). Render-lock position delta
quoted separately at 0.00 UU (position-only by construction - the lock never
touches rotation).

Verification that the refactor is behavior-preserving: model drive live via
simpose (writes counting, loc/rot exact for sim 0/0/0), fire test through
the armed synthetic ray - substituted rot (1763, 20919) = camera (853,
19099) + trim (5, 10 deg) * 182.04 units/deg exactly, subs=2, ammo 47->43,
dumps 8->8.

**Post-unification (same day): the algebra gate collapsed 28.21 -> 0.03 deg.**
Ray + laser now run the model's exact compose (q_ctrl (x) q_trim via
xr_local_trim_quat; `ray_pose_from_xr` = `model_pose_from_xr` + roll drop in
frame_context.h; helpers promoted to core/util/xr_math.h so the laser -
core code - shares them). Canonical sweep: <= 0.03 deg at EVERY pose (the
int-rotator quantization floor, 1 unit = 0.0055 deg). The live-L sweep is
the confirmation from the other side: 17.70 deg at EVERY orientation
(constant = pure trim-value difference between the L ray trim and the L
model trim; pre-fix it varied 0.20-17.70 with orientation = algebra error).
The laser's origin basis now builds right from the ray's YAW angle (zero-roll
convention, defined at any pitch); the old d x worldUp cross degenerated near
vertical and silently dropped the right/up offset components. Legacy
`vrhands aligntrim` deleted. Fire test on the unified build: calls=2 subs=2
skips=0, substituted rot exact, dumps 8->8.

### The name system: GNames located, index->string live (session 20 stage 4)

The FName-chain event scan finds the FName constructor and used to throw it
away; it is now captured (`EventScanResult::fnameCtor`, logged at boot:
**RVA 0x70D660**). Capstone disassembly (scratchpad only - never committed):
0x70D660 is a thin SEH wrapper that enters a name-system critical section
(global at RVA 0x136CEB8) and calls the real worker at **RVA 0x70D3C0**. The
worker: wcslen, digit-suffix split (`Name_123` -> base + number - FName is
8 bytes {int32 index, int32 number}; the number stores at FName+4), a
case-insensitive hash AND 0xFFF into a **4096-bucket hash table at RVA
0x1370EC0** (chain via entry+0xC, wcsicmp against entry+0x10), and on the
FindType==2 path indexes **GNames.Data at RVA 0x13904EC**
(`TArray<FNameEntry*>`: Data, +4 Count, +8 Max) and ORs 0x4000000 into
entry+4.

**FNameEntry layout** (matches the package-file prior - UTF-16, 8-byte
flags): +0x0 the entry's own index (used as a self-check by the resolver),
+0x4/+0x8 the 8-byte flags, +0xC hash-chain next, +0x10 UTF-16 text in
place. Bonus find: a free-index STACK (Data/Count/Max ints at RVA
0x13904F8/FC/0x1390500) - new names reuse recycled indices.

`patterns::fname_text(index)` reads GNames with full validation (every
dereference is_memory_valid + the entry self-index check); exposed as
`vrhands fname <index>|weapon`. **Live gate passed**: index 0 -> 'None',
1 -> 'ByteProperty' (the canonical Unreal table opening), GNames count
54129, and the equipped weapon's attach-bone FName (weapon+0xF0) ->
**'Launcher'** (idx 18075) - the AHands rig's weapon-attach socket name
(bone 43 in index space).

### Weapon skeletons, bone names, and the muzzle ray (session 20 stage 5)

**Bone-name map**: SkeletonInstance +0x08 -> SharedSkeletonData; its +0xAC map
(lookup fn 0x5F6500, capstone-disassembled) is a standard UE hash map -
+0x00 pairs base (16-byte pairs {chain next, FName index, FName number,
value}), +0x0C int32 bucket array (-1 empty), +0x10 bucket count (power of
two). Walking every bucket chain enumerates FName->boneIndex, which inverted
+ fname_text gives index->name for ANY skeletal actor (`vrbones skel
[hands|weapon]`).

**The shotgun's own skeleton is 3 bones**: SG_Body (x=10.1), SG_Pump
(x=57.3), SG_Shell (x=25.0) - all identity-ish quats, so the weapon's
component +X is the barrel axis, and there is NO explicit muzzle bone. The
muzzle ray therefore derives from the HANDS rig (bones 43->44 of the
per-weapon reference pose), as the plan's on-file alternative anticipated.

**Muzzle-ray derivation** (vraim muzzle on|off, default OFF): bones::drive
writes every cluster quat as qtc (x) q_ref with qtc = inv(q_actor) (x)
q_target, so the rendered world direction of (bone44ref - bone43ref) is
q_target (x) d0 - the actor frame cancels, the head never enters
(actor-frame rendering, session 12 part 3). d0 = normalize(p44ref - p43ref)
is recomputed per frame from the live reference, so it is per-weapon (the
reference IS the per-weapon animation) and follows any authored sway.
**Flat-measured on the shotgun**: d0 = (0.98, ~0.01, 0.15-0.19) - the
rendered barrel sits ~9-11 deg ABOVE the attach frame's forward and visibly
wanders inside that band with the idle sway (the misalignment users
hand-trim today, now followed automatically). Muzzle off -> ray rot
(0, camYaw); on -> (+1971, +41) units = +10.8/+0.2 deg = asin(d0.z) exactly.
Fire test with the muzzle ray live: calls=3 subs=3 skips=0, dumps 8->8. The
laser rides the same d0 XR-side (LaserConfig.muzzle; model trim incl. ROLL -
roll moves an off-axis vector).

**Negative result**: `exec NextWeapon` through the viewport chain FAULTS
(eip exe+0x4C2353, SEH caught) - UE2 stock console weapon switching is not
wired on this build; flat weapon switching stays an open harness gap (the
bumpers open the session-19 radial, which needs real stick timing). The
per-weapon d0 change is structurally guaranteed but goes to the in-headset
checklist for the eyes-on proof.

### Idle sway: measured, root-cause identified, killed in the drive (session 20 stage 6)

**Measured flat first** (the muzzle ray's barrel-axis echo as the
instrument): at idle under the live drive the reference pose's barrel
direction oscillates 8.4-11.0 deg (z component 0.146-0.191) = **+-1.2 deg
amplitude**, yaw +-0.6 deg; 1 Hz anchor telemetry vs a frozen snapshot
peaks at **3.01 UU / 4.6 deg** on the wrist anchors.

**Negative result (the SET-seam premise dissolves)**:
`UpdateHandBobAnimationParameters` decompiled - it drives the WALK bob on
animation channel 2 with weight = velocity/GroundSpeed, i.e. ZERO at
standstill. `PlayHandBobAnimation` confirms channel 2 = the additive bob.
The idle breathing is the authored base idle ANIMATION - there is no script
property to zero, so nothing for the SET seam to set.

**The kill (default ON, `vrhands swaykill on|off`)**: the sway reaches the
VR rig through exactly one door - the drive's per-frame reference recapture
- so the reference FREEZES against it: a fresh engine pose is adopted only
when either wrist anchor moves past 6 UU / 12 deg (2x the measured idle
envelope; equip/reload/fire move tens of UU), plus a 600 ms settle window
past the last big frame so the eventual freeze holds the SETTLED pose, not
a mid-animation one. Flat acceptance: kill ON = barrel axis bitwise
IDENTICAL across 8 samples / 32 s; OFF = the wobble returns instantly; two
fire pulls passed the threshold (reference re-adopted, settled 0.4 deg from
the old snapshot) and re-froze 3/3. The weapon's own skeleton (pump,
cylinder) animates untouched - it is a different SkeletonInstance. Side
effect by construction: hand-cluster finger animation during reload freezes
too (the drive was already overwriting it); the weapon's own reload
animation still shows.

### The FOV audit: no lie exists between render and submission (session 21)

**Question** (prompted by BioVR's measured "yaw warped, pitch clean" mismatch
symptom matching our +-90 sign-flipping laser-vs-gun drift): does the FOV the
game RENDERS under vrstereo equal the FOV our projection layer is TAGGED
with? The readback that feeds the claim (camera.cpp) reads the same engine
address we write, so it echoes our own value - it had never been
ground-truthed against the renderer's actual output.

**Instrumentation added**: (1) `xr: fovaudit submit` log at the projection
submission site - the per-eye claimed tangents + source
(readback/fallback/manual) + swap dims, logged on change;
(2) `fovaudit` seam command - option int, gfov write state, submitted
tangents, option-derived expectation side by side; (3)
`tools/decode-framedump.ps1` - parses a `dumpframe full [n]` dump, recovers
tangents per draw from the cb0 screen-ray helper block (floats 12..18 =
`(2tanH, 0, -tanH, 0, 0, -2tanV, tanV)`), attributes each depth-tested draw
to its governing cb0 capture (exact: a block is written whenever the VS b0
buffer OBJECT changes), clusters by tangent pair, and tags per cluster how
many draws carry the per-section fg-bake RVAs (0x3DBF7C/0x3EDCBF) in their
stacks - the lens-independent fg marker (needed because the shipped vrfgfov
match makes fg tangents EQUAL world tangents by design, so tangent
clustering alone cannot separate the passes).

**Measurement** (clean boot, NG+ Medical Pavilion, VR PRESET 1, vrstereo
heartbeat clean, `dumpframe full 2` = both SR eyes): ONE perspective cluster
per window - tanH=2.1445 tanV=1.2063 (= 130-deg hfov at 16:9) on 298/300
depth-tested draws (q0/q1), identical across both eye windows, matching the
option-derived tan(130/2)=2.144507 / *9/16=1.206285 to dump print precision
(dH=dV=0.00001). The fg draws (fgBakeStacks=9, 576-byte b0 tier, first draws
of the pass) carry the SAME tangents - the fg lens match verified from the
cb side. Every undecodable block (119 draws/window) has ALL-ZERO screen-ray
slots = non-perspective passes (shadow/UI) - no hidden second lens. Render
viewport 1920x1080 (399 draws), so the 16:9 vertical derivation holds.

**Verdict - negative result, the fov-lie hypothesis for the +-90 drift is
DEAD flat**: the renderer does not clamp option 130, both stereo eyes render
the same symmetric frustum, and submission builds its claim from the same
option int at the same aspect (structural; `projViews[eye].fov` is symmetric
both eyes from one scalar - the runtime's asymmetric per-eye fov never
reaches submission, which is exactly BioVR's "remove the lie" discipline,
already our architecture). Remaining live suspects for the drift: (a)
session-only claim state - the new submit log line prints src + swap dims,
one-glance check on the next headset run (src must read `readback`, swap
must read the render resolution); (b) pose-tag attribution - `fovaudit pose
on` arms a tagged-vs-consumed yaw delta log (in-headset only; flat has no
session); (c) the structural foreground-vs-compositor split - the world-pass
re-homing experiment.

### The foreground pass decoded: a second scene node, and the fovA zoom-pull lever (session 21)

**Architecture (dump stack-diff + capstone, all constants in patterns.h
"FOREGROUND SCENE NODE"):** the fg pass is a SECOND SCENE NODE flowing
through the SAME render machinery as the world (fg and world skeletal draws
share the whole draw layer; a per-section transform-provider virtual at
vtbl+0x20 does the bake). The scene build (0x4CCE70) allocates a 0x400-byte
fg node from the frame pool every build and constructs it at 0x56DC30 with
MEASURED argument semantics: (parentScene, &scene.view@+0x118, x,
vec3 cameraLoc, rotator cameraRot, float fovA <- PC+0x45C, float fovB <-
PC+0x460), storing it at scene+0x1B0 (vtable 0xE1846C). The finisher
(0x56DD90) builds the node's view matrix at +0x150 (+ inverse at +0x190,
world-projection diag at +0x1D0) FROM THE LOC/ROT ARGS; the fovs land at
+0x3F0/+0x3F4. The ctor runs BEFORE CalcView inside each build (call sites
build+0xD59 vs +0xF1A).

**The fg camera inputs are ALREADY world-synced per eye** (pass-labeled
ctor-arg capture, `vrfgnode on` + `dump`): pass 1's node receives the LEFT
eye camera, pass 2's the RIGHT (values match apply_eye_offset's +-3.17 UU
at the live yaw exactly), rotation = the DRIVEN camera rotator. Two raised
hypotheses measured and KILLED the same night: (a) "fg view = stale
pre-drive camera" - simhead sweep showed the DRIVEN yaw/pitch in the args;
(b) "crossed eyes under SR" (each pass getting the sibling's camera - would
have inverted rig disparity) - the pass-labeled capture shows correct-eye
delivery. The `vrfgnode sync on` substitution (feeds the ctor camera.cpp's
per-eye stash) is therefore a measured flat-static NO-OP; kept default-OFF
as an in-headset latency A/B only.

**THE LEVER - the fovA/fovB pair IS the fg zoom-pull.** fovA (from
PC+0x45C, engine-restamped to 75.0 every frame - unpokeable as data; the
session-15 "75.0 poked inert" verdict explained) is the REFERENCE fov;
fovB (PC+0x460) the actual fg fov. Their ratio drives the rig's
magnification + eye pull-back (the fov-coupled dolly sessions 13-16
countered with the render lock + lockpull). `vrfgnode fova match`
(ctor-arg substitution fovA:=fovB - always wins, unlike the data poke):
the rig collapses to TRUE world geometry - screenshot mean-abs-diff 11.09
with 44.2% of channels changed vs a 2.9 ambient floor, world pixels
untouched, exact round-trip on `fova off`. Fire path alive under
`fova match` + `vrbones lock off` (3 weapon-hook calls, no crash); ctor
substitutions 3358+/0 misses; crash dumps stable across all fg-node work.
**Candidate end-state (retune next session, headset-judged): vrfgfov on
(projection matched) + fova match (zoom-pull neutralized) + render lock
OFF = the rig at true world-lens geometry; the whole counter-model domain
(lock, lockgain/lockdgain/lockpull, kFgEye* constants) retires.**

**Negative results, all live-measured this session:**
- NO script-side fg membership property exists: the full Actor property
  list (282 props) and Hands' own (70) contain nothing foreground/pass
  related - membership is native-only (fits the name-table finding: only
  FOV-related foreground names exist).
- Pawn+0x724 = the pawn->Hands reference (found by pointer-value hexdump
  scan; the ONLY holder in pawn+0..0x1C00, PC holds none in +0..0x800).
  Nulling it is FATAL within ~1 s (0xC0000005 at +0x6F5ED7, a hash-chain
  walk on a corrupted [obj+0x10] - secondary damage, not the direct
  consumer). NOT a lever. 26 code refs to +0x724 exist, all in game-logic
  regions (0x1CA-0x313xxx) - the scene build never reads it directly.
- The three section-transform provider classes (bake fns 0x3DBF10 /
  0x3EDC40 / 0x60C5B0; vtbl slot +0x20 at rvas 0xDF20B4/0xDF32D4/0xE1CAA0)
  are SKINNING STRATEGIES with 656/178/240 live instances - not fg
  markers. Per-draw fg identification = the fg-bake stack RVAs
  (0x3DBF7C/0x3EDCBF), which tools/decode-framedump.ps1 tags (lens-blind,
  works under the matched lens where tangent clustering cannot separate
  the passes).
- The 576-byte cb tier is NOT fg-exclusive (world draws use it too; 88
  draws at the tier, 9 fg).
