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
factor needs render-path work.** *(SUPERSEDED IN PART, session 61 2026-08-14:
dead end 2's conclusion was confounded - that test still WROTE bone 43's `.s`
at its authored value. Excluding bones 43/44 from the scale-channel write
entirely leaves the attach path undisturbed and the cluster scales cleanly.
See "Session 61" below for the working lever; items 1 and 3 stand as
recorded.)*

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
B->A (use), Y->B (heal), X->X.

**The d-pad lane (revised 2026-08-22).** Held modifier + the selecting stick,
dominant axis past 0.5 pre-deadzone, suppressed while a grip is held (the
radials read the stick). Emission is **per direction**, because the two kinds of
binding sit on the same d-pad:

| Direction | Binding | Emission |
|---|---|---|
| UP / DOWN | ammo type, and it **CYCLES** | **PULSED** - re-arm inside 0.30, 300 ms cooldown. A held cycle does not settle, it spins |
| LEFT | nothing verified | pulsed |
| RIGHT | **hints** - and holding it ~0.5 s is how the **MAP** opens | **HELD** |

`PadMap::flickHoldBits` is the switch. See "The d-pad must be HELD" below for
why the map was unreachable before this, and for BRVR's independent finding of
the same thing.

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

### UObject identity: class-name keys for any live object (session 21)

**Derivation (live, seam-only, no disasm):** the equipped weapon actor's
header decodes as a classic UObject: +0x28 = the object's own FName index
(read 18009 -> 'Shotgun' via fname_text; +0x2C = the instance number), and
+0x30 = the UClass pointer - a heap object carrying its own vtable (RVA
0xE2F04C) whose +0x28 FName is the CANONICAL class name (number 0).
Cross-checked on the AHands actor: its class resolves to **'PlayerHands'**
(the ShockGame subclass - explains the all_classes 'Hands : Actor' entry as
the base). Production accessor `patterns::object_class_name()` validates
every dereference plus the UClass vtable before trusting the layout.

**Per-weapon profiles (vraim weapon|wsave|wkey, weapons.ini):** the R-hand
trim + ray-origin offsets hot-swap keyed by that class name; the R atomics
stay the single live truth (laser + fire ray + model publish read them
unchanged), stash-on-switch / seed-on-first-sight semantics, persisted as
`<Class>.<field>=<value>`. Flat-proven exact: sim-key round-trip restores
stashed values to the digit both directions ('Shotgun' 5.00/3.00 restored
after a 'Pistol' 9/8 detour), weapons.ini write/load round-trips (10 values,
2 weapons at boot), and the equipped shotgun keyed 'Shotgun' PRE-FIRE via
the resolving scan. IMPORTANT ordering semantic: a resolved profile applies
OVER the vrpreset.ini R values - by design (the profile is the per-weapon
truth; the preset seeds the first profile on a virgin weapons.ini).

**Weapon-actor resolution hardening:** the old proximity-only accept (120
UU from the expected gun spot) missed in live boot states (2 owner-matched
candidates, both rejected); the scan now accepts STRUCTURALLY first - the
candidate whose Base (+0x450) IS the AHands actor (attachment, the
session-20 equip-flow fact) - with proximity as fallback, and the profile
resolver backs off after 3 consecutive null resolves (the session-18
scan-cadence lesson; the game intro legitimately has NO weapon for minutes).

**Boot-harness hazard found on the way:** boot.ps1's press-mashing can land
on NEW GAME instead of CONTINUE after a force-kill dialog cycle - the run
lands in the 1960 plane intro ("IMPORTING NEW GAME PLUS DATA" on this NG+
profile). No save damage (the intro does not autosave before the lighthouse
transition; the .bsb set was verified untouched), but flat runs must
screenshot-verify the landing state before measuring.

### Session 21 part 2 - the headset feedback round (same day)

**THE +-90 DRIFT ROOT CAUSE CLOSES: it was the render lock itself.** The
user ran `vrbones lock off` (as part of the fovA preview) and reported
"exactly what I was looking for - the aim is in tune with the model...
perfect". The lock's counter-model (calibrated sessions 13-16 against the
old fg composition) miscorrects laterally at large hand yaws - the very
sign-flipping laser-vs-gun offset reported in session 20. Lock is now
DEFAULT OFF (`vrbones lock abs` = the live A/B back); the solve code stays
for reference until the composition work retires it.

**Hands.CurrentHoldable = hands+0x45C** (patterns.h has the derivation):
the equipped-weapon pointer read straight off the rig actor. It replaces
learned/cache/scan as the weapon resolver's primary source - the
trigger-learned object PINNED the resolver to the previously FIRED weapon
across wheel switches (unequipped weapons keep vtable + owner), which is
why profiles only swapped on fire in the first headset run (log-proven:
swaps at fire timestamps, minutes after the switches).

**Profile seeding race (log-proven, fixed):** the first resolve beat the
preset's value load by ~1 s, seeded the first profile from pre-preset
ZEROS, and the preset-tail re-apply then wrote the zeros over the user's
tuned baseline. Now: the resolver idles until a value source exists
(preset baseline captured or ini profiles loaded), new profiles seed from
the CAPTURED preset baseline (not from the outgoing weapon's values), all
flat-gated exact.

**fovA in-headset negative (parked, default off):** under `vrfgnode fova
match` the user reports THE WORLD moving with head motion (the rig
"decent") - the fovA argument evidently feeds more than the rig's section
bake (a world-coupled or full-screen consumer downstream of the fg node's
fovs). Do not re-arm in-headset until that consumer is identified; the
flat instrument findings stand.

**FPS/freeze audit of the headset run (log analysis):** every
sub-20-presents/s stall run coincides exactly with an XR session
FOCUSED->VISIBLE window (VD overlay open / headset set down), including
the long 19:37-19:38 window where the fova commands were typed; zero
mid-play heap scans (all 5 scans at boot), zero xrWaitFrame-blocked
lines, steady heartbeat during FOCUSED play. The freezes were not mod
work. Cosmetic follow-up queued: during some VISIBLE stretches the flat
window also stops presenting despite the pace guard (presents=0 while the
skip counter is idle) - the guard covers xrWaitFrame but something else
in the frame loop stalls; in-play impact none.

### Session 21 part 3 - the MachineGun/GrenadeLauncher keying gap

Run-2 log proof: Shotgun/Pistol/ChemicalThrower/Crossbow keyed and swapped
per wheel switch (the rig read works), but 'MachineGun' and
'GrenadeLauncher' NEVER keyed - those two carry a DIFFERENT NATIVE VTABLE
than kPlayerWeaponVtableRva, so the vtable-gated holdable path rejected
them and fell back to the stale cached weapon: the old key stayed active
and the user's MG/GL tuning edits landed in the previous weapon's profile.
Fix: `hands::current_holdable()` returns the raw Hands.CurrentHoldable
pointer class-agnostically (with a rig-known/unknown status so legacy
fallbacks only serve pre-rig states); the profile layer keys purely on
object_class_name (UClass-vtable-validated, works for ANY class) and
CLEARS the key when a holdable's class cannot resolve (edits then touch no
profile - logged). weapon_actor() keeps its PlayerWeapon-vtable gate for
legacy consumers. The MG/GL live-switch proof stays on the headset list
(cannot switch flat); everything else gated exact on a clean boot.

Also this round: the user's fixed LEFT-hand calibration (aim trim
+4.4/+30.0 deg, ray offset R+4.6/U+0.7 cm) written to vrpreset.ini and
baked as CODE DEFAULTS (their explicit ask); their four live profiles
rescued to weapons.ini via wsave before anything could drop them (the run
had not pressed save).

## Session 67 - the viewmodel drive ARCHITECTURE, and the Hands state machine

### The desync was never an offset

The BS1 viewmodel tracked the controller but swung on an oval as the wrist
rolled: zero error at 0 deg, worst at 180, closed again at 360. Two sessions of
offset, pose and trim tuning never touched it, and every stage measured clean -
because the fault was structural.

**The two mods drive the rig in opposite directions.** BRVR moves the ACTOR and
leaves the skeleton alone: the hand cluster is replayed verbatim (frozen), bone
43 takes position but not rotation, and the actor transform carries the whole
assembly to the controller, so the rig's internal pose is always exactly as
authored. Named by BRVR's own log rather than by reasoning:

```
>>> WEAPONHAND: freezing the RIGHT cluster, bones 27-44.
>>> WEAPONHAND: pose settled after 359 ms (rig went still) -- freezing from here.
>>> B43: attach rotation drift 1.29 deg (cluster frozen; this bone is still the engine's)
```

This tree did the inverse - actor left where `Hands.UpdateLocation` pins it (on
the camera, every frame), cluster 27-44 retargeted inside it, bone 43 included,
about a reference that is itself a snapshot of an animated pose. Source: BRVR
`Camera/CameraHook.cpp` `DriveHands` and `Hands/ArmHide.cpp`.

`vrhands mode brvr` (mode 3) is that architecture and is now the shipped
default: position from the GRIP pose, rotation from the AIM pose, grip offset
SUBTRACTED along the model axes, cluster frozen, bone 43 position-only.

### The grip offset IS the pivot - and that is why tuning it could never work

A body point renders at `gp.loc + R(theta) * (G - gripOffset)`, so the offset
decides which mesh point stays still. Using it to correct the gun's HEIGHT
displaces the pivot by the same amount, which becomes an orbit the moment the
wrist rotates. Measured this session: a 15 cm height correction bought an 8 inch
orbit (tester: +4 in at 0 deg, -12 in at 180 - midpoint -4, amplitude 8).

So placement was split onto its own knob, applied in the VIEW frame after the
model transform, where it cannot create a lever. **Two knobs, two jobs: grip =
where it pivots, view = where it sits.** Anyone tempted to fix a height problem
with the grip offset should read this paragraph first.

### The idle animation is a WEIGHTED RANDOM draw

`Holdable.uc`:

```uc
var config travel array<name>  IdlingHandsAnim;
var config travel array<float> IdlingHandsAnimWeight;
```

`GetIdlingHandsAnim()` returns a weighted-random entry. `Hands.uc`'s
`WeaponIdling` loops it, and skips the loop entirely when the name is `'None'`.
Every return to idle can therefore settle the rig somewhere different - which is
the "crosshair moves randomly between shots" report, and the randomness is the
game's rather than the mod's. BRVR's `IdleAnimMode=1` exists for exactly this;
its own config echo reads "(all entries -> entry[0], kills the wrench slap)".

Neighbouring `config travel` properties on `Holdable`, all settable by name
through the engine SET seam (no offsets needed, so they survive Epic and GOG):
`EquippingHandsAnim` (the cycle on equip), `AdditiveHandBobAnim`,
`UnEquippingHandsAnim`, `IdlingAnim`, `AttachBone`.

### Every plasmid has its own idle FIDGET, and that is what a late capture samples

`Hands.uc`'s `AbilityIdling` (:1556) starts the idle animation through
`PlayAnimationOnChannelInstantEaseIn(0, CurrentAbility.GetIdlingAnim(), 4)`, and
`Ability::GetIdlingAnim()` draws a WEIGHTED RANDOM entry from
`IdlingAnimationName[]` - the same shape as `Holdable::GetIdlingHandsAnim()` for
weapons. What each ability declares differs:

| ability | `IdlingAnimationName` | source |
|---|---|---|
| `ElectricBoltAbility` | `ElectrokineticBolt_Fidget` (100), `..._Accent_A` (10) | its own defaults |
| **`TelekinesisAbility`** | **none - inherits `Generic_Fidget`** | `Ability.uc:165` |
| (no EVE) | `NoEve_Fidget` via `AbilityGenericIdling` (:1606, ease rate 8) | `Hands.uc:1834` |

**Consequence for the viewmodel drive.** Any reference capture that samples the
rig AFTER `Idling` begins is sampling a frame of that fidget, and each plasmid's
fidget puts the hand somewhere different. A fixed post-equip window therefore
lands correctly for one plasmid and wrongly for another with nothing configurable
between them - measured 2026-08-27 as `Tele -> Weapon -> Tele` reliably wrong while
`Electro -> Weapon -> Electro` was reliably fine.

**The equip-END pose is the `Idling` EDGE**, the last frame before any fidget.
Capture there and stop adopting; sampling later samples deeper into the fidget.
Continuing to adopt through that window is also, exactly, what plays "one cycle of
the idle animation" into the rig on every equip - one mechanism, both defects.

A plasmid rig also never goes STILL: the fidget animates it continuously. Measured
2156 ms to reach 1.24 deg against a +-1.2 deg idle envelope, so any "wait for the
pose to settle" test crosses its threshold at random. Do not build one.

### Hands state machine - DERIVED, not hardcoded

`ShockGame.Hands` is a UnrealScript state machine; gameplay code keys off
`GetHands().GetStateName()`. UE2 keeps the current state in the object's
`FStateFrame`, and the offsets differ per build.

`hands_state.cpp` derives both by sweeping candidate pointers off the Hands
actor and accepting only the pair whose `UState` name resolves, through GNames,
to a state `Hands.uc` actually declares. A wrong offset cannot pass that test,
so no number is carried between builds or storefronts. Uses the UObject layout
already recorded here: name index `+0x28`, class `+0x30`.

The states, from `Hands.uc`: `HandsOffscreen` (auto), `WeaponEquipping`,
`WeaponIdling`, `WeaponFiring`, `PostWeaponFiring`, `WeaponReloading`,
`ProceduralWeaponReloading`, `WeaponUnEquipping`, the zoom quartet, the ability
equivalents, `InjectingEve`, `UsingGathererTool`, `ExorcisingGatherer`,
`PlayingScriptedHandAnimation`.

**Session 68 - where the states EXIT, which is the part the policy got wrong.**
`WeaponFiring` (Hands.uc:1184) runs `PlayWeaponFiringAnimations()` and then
`TransitionToNextStateInSequence()`; `PostWeaponFiring` (:1210) is a single
`PostFired()` call and another transition. Neither waits for the gun to come back
down, so **the state leaves the adopt mask at the TOP of the recoil, not at its
end.** s67's policy merely ceased adopting there, which left the reference at the
apex and froze the pistol until a weapon switch - and `Firing -> Reloading` is the
same exit, which is why the ammo-out freeze and the shotgun's first reload had the
same shape.

`WeaponIdling` (:1147) is the counterpart and the only state that means "the
animation is over": it re-reads the active holdable, plays the idling anim and
LOOPS (`goto J0xBE`), redrawing `GetIdlingHandsAnim()` each time round - which is
the weighted-random draw recorded above, and why the loop's pose cannot be trusted
as a reference more than once.

s68 uses both facts: capture ONE canonical rest during `WeaponIdling` per holdable
and ease back to it when an adopted state exits. Design rationale in
`ARCHITECTURE.md`, decision log 2026-08-27.

**Session 68c - `Idling` is where the idle animation STARTS.** Both `WeaponIdling`
(:1147) and `AbilityIdling` (:1556) begin the idle through an EASE-IN
(`PlayAnimationOnChannelFlatEaseIn` / `...InstantEaseIn`), so on the first `Idling`
frame the rig is still blending out of the equip pose. Anything captured there is
a partial blend. The ease RATE differs per path - `AbilityIdling` at 4,
`AbilityGenericIdling` at 8 - so a fixed capture instant lands correctly for one
plasmid and wrong for another with nothing configurable between them. Capture when
the pose STOPS MOVING, not on the state edge.

### ROLLCHECK's "drift" is not an angle while an animation is blending

`ROLLCHECK` reports `2*acos(|w|)` of the delta between our write and what is
there now, plus the delta's imaginary parts. During a plasmid animation it reads:

```
drifted 16.64 deg (local x +0.00 y +0.00 z -0.00)
```

**Those cannot both be true of a unit quaternion.** With x=y=z=0 a unit quat has
w=+-1 and the angle is zero. w = cos(8.32 deg) = 0.9895 with zero imaginary parts
means the delta is not unit - the rotation is UNCHANGED and only the MAGNITUDE
differs, by about 1%.

That is `PlayAnimationOnChannelInstantEaseIn` leaving a non-normalised blend
quaternion in the bank while it eases, which is ordinary for an nlerp. **The
engine is not overwriting our bone write during an animation.** Read the
imaginary parts before believing the angle; a large `drifted` with a zero
component split is a denormalisation, not a fight.

Recorded because it was read as a fight twice and produced a fix for a
non-problem - and then, worse, because the same fact was known and NOT applied
one commit later.

**Anything that consumes a bone quat must normalise it.** `conj()` is only the
inverse of a UNIT quaternion; for a non-unit one the inverse is `conj(q)/|q|^2`.
Build a correction out of `conj()` on a mid-blend quat and it comes out non-unit,
after which `qts_rotate()` scales every offset it touches by `|q|^2` and
`quat_mul()` compounds the error into every bone downstream. The rig is skinned
on the assumption these are rotations, so the visible result is stretched
geometry - spikes shooting out of the hand - rather than anything that reads as a
wrong angle. Normalise the correction, and normalise the per-bone product.

**And that rule binds the INSTRUMENTS, not just the drive path.** The drive path
was normalised in `825ced6`; the two always-on probes that read the same bank
were not, and were still lying afterwards:

- `quat_angle_deg()` took a raw dot product of two bank quats. That dot is
  `|a||b|cos(theta/2)`, so it errs in both directions: magnitudes above 1 inflate
  it, the `dot > 1` clamp fires, and it reports **0 deg while the bones genuinely
  differ**; magnitudes below 1 deflate it and it **invents an angle** out of a
  pure magnitude difference. The second case is the `ROLLCHECK` lie in another
  form.
- `ROLLCHECK` itself built `d = conj(qLastWritten) * cur` and read `d[3]` as
  `cos(theta/2)` without normalising `d` - which is precisely how it produced
  `drifted 16.64 deg (local x +0.00 y +0.00 z -0.00)`. Both readings cannot be
  true of a rotation, and the rotation was in fact unchanged.

The `quat_angle_deg()` case is the sharper one, because the `ANIMPIN` telemetry
**gates** on its result (`angOff > 2.0`): a lying angle decides whether the line
prints at all, and an instrument that chooses its own visibility cannot be
checked against its own silence. A test whose only readout is a gated log line is
worth nothing until the gate is trustworthy.

**The general lesson: when a fix establishes an invariant, grep for every
consumer of the thing the invariant is about - the diagnostics included.** The
probes are what the next session reasons from, so a lying probe outlives the bug
it was pointed at.

## Session 71: the free hand retargets where the held hand replays

Source: BRVR's `DriveFreeHand` (CameraHook.cpp:2537) -> `ArmHide_DriveFreeHand`
(ArmHide.cpp:1565) -> `WriteCluster`. Cited here because BS1 had no off-hand
concept at all; the other cluster was collapsed to zero scale and parked 5000
units below the actor.

**One WriteCluster, two shapes, and the difference is the whole design.**

| | held hand | free hand |
|---|---|---|
| cluster | replayed VERBATIM (fed its own wrist, so the delta is identity) | RETARGETED to a controller-derived target |
| carried by | the ACTOR | nothing - the actor is already carrying the held hand |

So the free hand's bones are written as `inv(R_actor) * (worldTarget -
actorLoc)`, which divides the actor out exactly.

**That cancellation makes the free hand a far better INSTRUMENT than the held
hand.** The held hand goes wherever the actor points and therefore cannot reveal
an actor error - any mismatch just moves the whole rig, which reads as vague
desync. The free hand is computed against a SPECIFIC actor, so a mismatch shows
as an orbit about the actor position, with a magnitude of `|worldTarget -
actorLoc| x mismatch`: at arm's length (50-100 UU) even 10 deg is 10-17 UU of
visible swing. Rotation stays correct while position swings, because orientation
is written per bone and lands, while position is what gets rotated.

Two details from BRVR worth keeping:

- **Grip pose, not aim.** *"The aim pose is where a weapon would shoot; on most controllers it is tilted tens of degrees off the hand. A bare hand wants the pose that describes the hand."*
- **The actor transform is PASSED IN, never read.** `DriveFreeHand` takes `want` and `wx/wy/wz` as arguments, and its call site says why it runs last: *"Deliberately LAST, because it consumes `want` and wx/wy/wz - the actor rotation and location this function has just decided."*

**The free hand needs its OWN authored reference bank.** `g_ref` is refreshed
wholesale from the live bone array whenever the held hand adopts - and by then
that array contains what we wrote to the free hand on the previous frame.
Retargeting against it is retargeting against our own output. BRVR states the
same rule for its grab point: *"latch it only on frames the ENGINE owns the
cluster... CaptureClusterRef early-outs while driven and hands back OUR pose."*
This is the same feedback loop that made the s70h graded arm blend a silent
no-op, and it fails quietly rather than erroring.

**Falsified here, do not re-try:** using this frame's intended actor transform in
place of `last_actor_write()`'s frame-old one does NOT fix the free-hand orbit
(`2cdab5e`; symptom bit-identical afterwards). Note also that `last_actor_write()`
is the RIGHT answer for the arm solve (s70q) and the WRONG one for the free hand -
same accessor, opposite requirement, so "use the intended transform" is not a
general rule.

## Session 70: BRVR's viewmodel chain, traced end to end

Source: the BRVR mirror - `Hands/HandsProbe.cpp`, `Hands/ArmHide.cpp`,
`Camera/CameraHook.cpp`, `Core/Config.{h,cpp}`. Every number below is BRVR's own.

### The chain

**Offsets are ONE LIVE SET, swapped on switch.** `handsGrip[3]`, `handsRot[3]`
and `cursorRot[3]` are the live values and the drive path reads nothing else.
`UpdateWeaponGrip` saves the live set back to whichever table the outgoing item
came from and loads the incoming one - `gripBySlot[9]` for weapons (slot 8 = any
ability), `plasmidGrip[12]` for plasmids, indexed by `ResolvePlasmidId`. There is
no per-hand split; which controller drives is a separate decision,
`AbilityMode() ? HAND_LEFT : HAND_RIGHT`.

**The ACTOR carries everything.** `handsRot` composes onto the controller's aim
quat for the actor rotation; `handsGrip` is subtracted along the actor's own
forward/right/up basis for its location. The skeleton is never posed by offsets.
`WriteCluster` is fed the reference's OWN wrist, so the delta collapses to
identity and the cluster replays verbatim.

**Animation is a size threshold plus a hold window, and nothing else.**
`CaptureClusterRef`, when already driving: exact `memcmp` of the live cluster
against what we last wrote (bit-identical = our own pose, nothing happened);
otherwise measure the wrist's rotation delta; `deg >= HandAnimMinDeg (5)` stamps
`lastBig`; `playing = (now - lastBig) < HandAnimHoldMs (1200)`; while playing,
adopt the whole live cluster as the new reference.

**The hold window IS the settle mechanism.** Adoption keeps tracking for 1.2 s
past the animation's last big frame, so the reference lands on the SETTLED pose
by construction. Idle breathing (1-5 deg) never clears 5 deg, so idle is rejected
BY SIZE - no state machine is involved anywhere.

**Equip: release, wait for stillness, re-capture.** On a pose-key change BRVR
releases the cluster, lets the equip play, and captures once the rig settles:

| Guard | BRVR value | What it answers |
|---|---|---|
| `WeaponKeyDebounceMs` | 150 ms | The engine parks `CurrentHoldable` at NULL for a frame during fire/pump. MEASURED: the raw key fired **11 times in a 3-minute session with 2 real switches** |
| `kSettleStillUnits` | 0.05 UU/frame | Stillness is measured on the wrist's POSITION, not any angle |
| `kSettleStillMs` | 150 ms | How long that stillness must hold |
| `kSettleMinMs` | 350 ms | A FLOOR - many draw animations pause part-way, and quiet inside a pause is not the end |
| `WeaponSwitchSettleMs` | 600 ms | A CEILING, not a duration |

### A LOOPING IDLE NEVER GOES STILL - and the mean is the answer

BRVR: *"there is no single authored pose to latch and 'the last frame before we
take the cluster' is an arbitrary phase of the loop... accumulate while settling
and, if the rig never stills, latch the MEAN - the centre of the loop rather than
a point on its circumference."*

**s68 measured the identical fact** on a plasmid rig - 2156 ms to reach 1.24 deg
against a +-1.2 deg idle envelope - and concluded *"a stillness test cannot work
here"*, then abandoned the approach after nine builds. BRVR reached the same
observation and answered it with the mean instead of abandoning it. That fallback
is the piece all nine attempts were missing.

### Where this tree had diverged

1. **The per-state animation mask was the root of s67-s69.** It adopted only
   `Firing`/`PostFiring`; `Hands.uc` leaves `WeaponFiring` at the TOP of the
   recoil, so adoption was cut at the apex and the reference stuck there. The
   canonical rest, its eased restore, the anchor pin and the quaternion
   normalisation for that pin were all built to undo that one substitution. The
   threshold+hold mechanism it overrode was already present in this file.

2. **The freeze was anchored on bone 43, the WEAPON ATTACH.** BRVR's cluster spec
   is `{27, 44, wrist 27}` and it anchors on the wrist; 43 is precisely the one
   bone it leaves the engine still animating (*"cluster frozen; this bone is
   still the engine's"*, 1-5 deg idle drift, peaks of 41-135). Anchoring a frozen
   cluster on a moving bone writes every other bone relative to a moving point.
   The left cluster here already anchored on its wrist (6); s68 recorded the
   asymmetry as "not a defect".

   **The anchor is also the point the cluster is SCALED about** (`g_scale` =
   0.80), so moving it from 43 to 27 shifts the whole hand by `0.2 x (p43 -
   p27)`. The per-weapon placement offsets were tuned against the old anchor
   and will need a re-tune. Judge the anchor A/B on whether the hand STAYS
   PUT through an animation, not on where it sits.

3. **The adoption probe sampled bone 43 too**, so "has the pose changed?" was
   asked of a bone that moves on its own. This is what made s67 raise the adopt
   threshold from BRVR's 5 deg to 25 - the shotgun's idle "crossing 5" was bone
   43's own drift, not the hand. 25 deg is above some weapons' entire per-shot
   wrist movement, which is BRVR's recorded Tommy-gun failure and was reported
   here as well.

4. **No key debounce at all** - the `(holdable, ability)` pair was compared raw
   every frame.

### One deliberate deviation from BRVR

The crosshair is GLOBAL here, by the tester's direction. BRVR keys `cursorRot`
per slot AND per plasmid; s67 tried global in this tree and recorded that it does
not serve every gun. It is global **per hand**, because the seeded table puts the
weapons at 0.83/-9.20 and the plasmid at -11.00/37.00 - 46 deg of yaw apart,
which is two model frames rather than two opinions about one number.

### Open question the first headset run must answer

The settle block runs only when the engine RE-EVALUATED the bone array, and s68
measured that at roughly 1 frame in 19. A 600 ms ceiling may therefore hold only
a handful of samples, and BRVR's millisecond constants may not transfer. The
settle log prints the sample count for exactly this reason; if it reads 2 or 3,
the ceiling wants raising (F10, no rebuild).

### An adopted animation moves the anchor, and in FREEZE-ONLY that moves the hand

Mode 3 (BRVR's shape) sets `freeze_only`, which writes the cluster AS the
reference pose - `p[i] = pa + (g_ref[i].p - pa) * s`, with `pa = g_ref[anchor].p` -
and lets the ACTOR carry it to the controller.

That holds while the reference is frozen. Once an animation is ADOPTED, `g_ref`
tracks it, so `pa` moves and the entire cluster moves with it - while the actor
placement, computed from the target, knows nothing about it. **The hand walks off
the controller for the length of the animation**, which is what the plasmid firing
screenshots show, and why `animOn=0` made Telekinesis sit still: no adoption, no
drift.

The fix is a rigid transform of the cluster back onto the captured rest anchor -
**both position AND rotation**. `qFix = q_rest[anchor] * conj(q_ref[anchor])`
applied to every bone's quat, and to every bone's offset from the anchor, with the
anchor translated onto `g_rest[anchor].p`.

Position alone is only half of it, and the half it leaves out is the one that
shows. The actor's ROTATION is set from the controller on the assumption that the
anchor still carries its captured orientation; when the animation turns the
anchor, the whole hand turns with it and points somewhere else - reported as "the
position stays put now but the direction of it is still incorrect even though it
hits the same spot". The aim ray is computed separately, which is exactly why the
shot still lands correctly while the model points wrong: **a hand that aims wrong
while the bullet goes right is a viewmodel-frame problem, never an aim one.**

What survives the pin is everything INSIDE the cluster - finger curl, splay, the
shape of the animation. What is removed is the anchor's own motion, which is the
only part the actor cannot absorb.

This is a consequence of BRVR's architecture rather than a porting error - "the
actor carries the assembly" only holds if the assembly's anchor stays put.

### The engine's tick overwrites a bone write, but only while it is ANIMATING

`ROLLCHECK` measures whether our bone write survives to the next frame. The two
regimes are far apart, measured 2026-08-27 on an Electro Bolt:

```
idle    our bone write drifted  0.27 deg   engineEval=0   (write held)
firing  our bone write drifted 15.73 deg   engineEval=0   <-- OUR WRITE IS BEING CHANGED
firing  our bone write drifted 15.16 deg   engineEval=1   (local x -5.36 y -2.40 z +10.17)
```

At idle the engine barely re-evaluates the hands bone array (see the ~5%-of-frames
note above) so our write simply stands. **The moment an animation is adopted the
engine evaluates every frame, its tick lands AFTER ours, and it wins.** The
rendered pose is then the animation's, so a rig pinned to the controller walks off
it for the length of the animation - visible as the plasmid hand translating away
from the controller while firing.

This is BRVR's S59/S60 finding in a second place: BRVR measured the tick resetting
the hands ROTATOR and fixed it by writing again later in the frame, past the tick.
The same is required of the BONE cluster.

`hands::late_write()` (called from scenedraw's build detour, game thread, after
CalcView) is where that second write lives. **It must replay the cluster in mode 3
as well as mode 2.** Mode 3 is "BRVR shape, both halves" - it writes the ACTOR to
carry the rig and the CLUSTER to keep the rig rigid beneath it - so it needs the
bone replay exactly as mode 2 does. It only replayed the actor rotator until s69d,
which is why any adopted animation on the left hand walked off the controller.

Weapons hid it because their anchor is the weapon-attach bone (43), which the
engine's attachment path re-derives anyway.

### The left cluster's anchor is already correct

Bone names, dumped live: `6 = Bip01_L_Hand`, then `7-21` are nothing but finger
joints (`kBone_L_Thumb/Index/Middle/Ring/Pinky`, three each). Bone 6 IS the palm,
so `kBoneLWrist` is the right anchor and there is no better bone to move it to.
The right cluster's anchor at 43 is a different kind of thing - the point a WEAPON
hangs from - and the asymmetry between the two is not a defect. Ruled out
2026-08-27 as a cause of the plasmid animation walk; the cause was the late write
above.

### Plasmids need their OWN numbers - one shared set cannot work

Upgrade tiers are separate engine classes, so the ability class name has to be
folded before it can key anything: `ElectricBoltAbility`, `ElectricBoltTwoAbility`,
`ElectricBoltThreeAbility`, `ElectricBoltZeroAbility` are one plasmid, as are
`TelekinesisAbility` / `TelekinesisTwoAbility`. Strip the trailing `Ability` and
then a trailing `Zero`/`Two`/`Three` and eleven plasmids remain, which is what the
game ships.

**They are not interchangeable.** BRVR's shipped config, with `PerPlasmidTuning=1`,
carries rotations that differ by tens of degrees between them:

```
PlasmidGrip0=45.50,-14.90,-12.30   PlasmidRot0=-111.00,-64.00,22.00
PlasmidGrip1=49.50,-12.90,-12.30   PlasmidRot1=-35.00,-20.00,22.00
PlasmidGrip2=45.50,-10.90,-6.30    PlasmidRot2=-111.00,-16.00,22.00
```

The authored poses genuinely differ, so **no single grip/rotation serves two
plasmids** and no reference-capture instant can be found that makes one set work
for both. Nine attempts in session 68 searched for that instant; the search was
unsound, not merely unlucky. Per-plasmid values absorb the difference, which is
why BRVR never had the defect.

Per-plasmid values also require a **per-plasmid reference capture**: the identity
must be the pair `(CurrentHoldable, CurrentAbility)`, or the offsets sit on
whichever pose was captured last.

Identity resolves through the ordinary UObject path - `object_class_name()` on the
`CurrentAbility` INSTANCE - so no pawn scan and no new offsets are needed. (BRVR
does scan the pawn for `AvailableAbilities` and match `ActiveAbility` into it for
an index; that is its route, not a requirement.)

### Hands.CurrentAbility is where plasmids live - `+0x454`

`Hands.uc` declares them in separate slots:

```
var private travel ShockPawn PawnOwner;       // +0x450  kHandsBaseOffset
var private transient Ability CurrentAbility; // +0x454  kHandsCurrentAbilityOffset
var private transient Ability OldAbility;     // +0x458
var private Holdable CurrentHoldable;         // +0x45C  kHandsCurrentHoldableOffset
```

**`CurrentHoldable` is NULL while a plasmid is equipped** - the plasmid is in
`CurrentAbility`. Confirmed in the log: every plasmid switch prints bones'
`wscale rigid: released (holdable gone)`, whose test is literally `!hold`.

Consequences for anything keying off "what is in your hands":

- A plasmid looks like EMPTY HANDS to `CurrentHoldable` alone. That is what left
  the last weapon's whole profile applied - trims, placement, and the animation
  gate - while a plasmid was up.
- Every plasmid looks like EVERY OTHER plasmid, because they are all null there.
  Switching plasmid A for plasmid B is invisible, so B inherits A's captured
  reference pose and renders at A's position with no setting able to touch it.
- **Identity is the PAIR** `(CurrentHoldable, CurrentAbility)`. Watch both.

The offset was already derived and documented here (bracketed by `kHandsBaseOffset`
and `kHandsCurrentHoldableOffset`, four consecutive pointer fields with ours at
each end) and used only by `hands::armed()` for a cosmetic crosshair gate. The
viewmodel drive had never read it.

### Falsified this session, with the measurement that killed each

| Theory | Killed by |
|---|---|
| Aim-vs-grip pose choice for the model | headset A/B: no change. The aim pose is also a deliberate s11 fix so barrel, laser and bullet are one ray |
| Bone 43's rotation applied twice | writing position-only stops the gun rotating ENTIRELY. BRVR gets away with it only because its ACTOR carries the rotation |
| A rigid lever arm | three-axis offset tuning across two sessions never nulled it, and the required value changed between sessions |
| The game erasing our roll | BRVR measured 5-102 deg on its ACTOR rotator (its S59) and fixed it from Present (S60); this tree's BONE write holds to 0.13 deg drift, measured by `ROLLCHECK` |
| The foreground lens match not reaching the renderer | `FOVPROBE`: field holds 117.46 and the engine does not restamp it. The first write is 83.6 only because the backbuffer dims are not known yet - a real, minor, self-correcting bug |

`[hud] fov watch` reporting `1 lens(es)` / `FG tanH=0.000000` every run is a
BLIND INSTRUMENT, not evidence that the foreground pass is missing. It cost a
hypothesis; do not read it as a finding.

## Session 22 - scripted-camera scenes: the descent's real mechanism (measured flat)

The session-22 plan carried two hypotheses for the bathysphere descent
("no stereo + fisheye"): (a) scripted cameras bypass eventPlayerCalcView,
(b) our gfov-130 option write fisheyes a ~75-authored camera. **Both are
wrong for the descent** - measured on the crash-site save, vrstereo on,
clean boot, the ride replayed end to end:

- **CalcView keeps firing the whole ride** (heartbeat 150-1100 calls/s,
  camera loc walking the scripted track), and the view actor stays
  AShockPlayer - `[b1r] view state:` logs ZERO transitions across the
  descent, the pause menu, and the hack minigame (one boot-time
  menu/cutscene -> GAMEPLAY pair is the whole log). Strict-view and
  CalcView-staleness detectors both MISS this scene class.
- **The renderer consumes CalcView's camera even mid-scene**: the world
  pass's cb0 camera position matches the CalcView heartbeat loc to the UU
  (45542.2/-15444.8/-22293.4 vs 45543.1/-15447.8/-22293.4 at the same
  second). Eye offsets applied in CalcView DO reach the pixels - the
  "no stereo" percept is NOT missing disparity at the render level.
- **The scene renders its OWN FOV and ignores the option**: dump decode
  mid-ride shows ONE tangent cluster tanH=1.2800 tanV=0.7200 = 104.0 deg
  at 16:9 in BOTH eye windows while the option int reads 130 (fovaudit
  mid-ride: option=130, gfovWrite on). Control immediately after arrival:
  cluster 2.1445/1.2063 = exactly option 130 (the session-21
  rendered==option result holds for gameplay). The user's persisted
  option is itself 130, so "restore the saved option" can never fix a
  scene that does not read the option at all.
- **Root cause of both user percepts: the projection layer CLAIMS the
  option-derived FOV (130) over a 104-rendered image.** A 26-deg claim
  error warps geometry (the "fisheye") and mis-registers the two eyes'
  reprojection enough to break fusion (the "no stereo" percept) even
  though pixel-level disparity exists.

**The shipped detector (session 22): the live rendered-FOV watch**
(core/gfx/hud_capture.cpp): once per present interval the first
DSV-bound DrawIndexed's VS b0 head (80 bytes) is copied to a tiny staging
buffer (CopySubresourceRegion, async) and mapped on a LATER present with
DO_NOT_WAIT (zero stalls); tangents decode from the screen-ray block at
floats 12..18 (the session-21 layout decode-framedump.ps1 verifies
offline; both derivations must agree within 2% or the block is ignored).
`fov_mismatch()` = rendered-vs-option outside +-10%, 3-interval
hysteresis, transitions logged (`[hud] rendered-fov mismatch ON/off`) -
session-independent, so the descent is a fully flat-testable repro:
exactly one ON ~11 s after the lever pull (104.0 vs 130.0), live
`fovaudit live:` echoes 1.2799/0.7200 age 0 ms mid-ride, one off at
arrival. The cinematic quad fallback (openxr_runtime on_present_end)
keys on strict-false OR publish-staleness OR fov_mismatch; the first two
legs never fired during the descent and stay for menu-attract/true-bypass
scene classes.

Traps recorded: the descent ride creates a "Welcome to Rapture
(AutoSave)" at arrival that becomes the NEWEST save - CONTINUE stops
landing on the crash-site repro save after one replay (it drops to
second in the LOAD list). A save LOAD produced NO view-state transition
and NO fov-write OFF/ON churn (strict stayed true through the load
screen on this box).

### Session 22 part 2 - fullscreen-screen fingerprints and the per-kind routing

Dump ground truth for the three misrouted kinds (all `dumpframe full 2`,
clean boots, resource tables in the dumps):

- **Loading screen**: ~87 non-indexed gameswf draws (flush 0x7B8EB5 in
  every stack) straight onto the final LDR target, DSV bound but ZERO
  DrawIndexed in the interval - no world pass, no tonemap, nothing for
  the session-19 classifier to classify. Renders in-frame = full-FOV in
  the headset.
- **Hack minigame**: same family, measured 322 swf draws / 0 DrawIndexed
  (the intro board; the world pass is fully absent for the whole hack
  session even though the scene BUILD calls keep running - the build
  draws nothing). The user's "basically the same as the loading screen"
  was literally exact. The main menu is this family too (125 draws).
- **Alcohol-blur composite**: the SECOND non-indexed draw on the tonemap
  target (right after the tonemap), engine post path (no gameswf flush
  in its stack), sampling a BACKBUFFER-SIZED texture (1920x1080 RGBA8) -
  while every real HUD/flash draw samples 2048x2048 BC-compressed UI
  atlases or nothing. The pre-tonemap 480x270 blur pyramid runs on its
  own RTs and never touches the classifier.

Shipped discriminators (hud_capture.cpp): screen-only interval =
swf-draws >= 20 with no scene-vote leader (3-interval hysteresis,
`[hud] screen-only interval ON/off` transitions - flat instrument);
in-frame post effect = post-tonemap draw whose srv0 dimensions equal the
target's (srv desc cache, same 8-slot pattern as the RT desc cache).
Measured: postFx = 0 across normal gameplay (no false positives); the
pause menu contributes ~1/interval (its fullscreen dim layer sampling
the scene - correctly in-frame; the menu PANELS keep redirecting to the
quad). Loading screens fire exactly one ON/off pair per load (87 and
120-draw intervals measured on two different loads).

### Session 22 part 3 - the first-boot probe fix, virgin-install measured

compose_over now answers a FAILED slot-0 GetState with a neutral
CONNECTED pad (constant packet 1) even while vrinput is off. Virgin
boot (all inis + the old vrinput.on marker set aside): the main menu
shows NO pad glyphs before any pad input (prompts key on last-used
input, the session-9 model - the phantom pad alone does not flip them);
after `vrinput on` mid-session the IAT lane immediately polls at ~680
calls/s and a dpad press moves the UI (packet 1->3, the 2K-link prompt
flips to a pad glyph) - the restart is gone. The proxy-lane counter
STAYS at 6 by design (the Steam overlay swallows that lane post-boot;
it is not a poll-rate oracle). The marker machinery is deleted; the
orphan vrinput.on file on existing installs is inert.

### Session 22 round 2 - engine-cinematic letterbox (the plasmid FMV class)

The user's "Big Daddy plasmid FMV" report (black bars mid-view + camera
different from flat) is the ENGINE-CINEMATIC class, dump-decoded on their
prepared Gatherer's Garden save (the Electro Bolt injection sequence):

- The engine CLEARS the final target to opaque black (ClearRTV 0,0,0,1 -
  event immediately before the tonemap) and draws the tonemap as a
  vertically SHRUNKEN quad - full 1920x1080 viewport, shrunk GEOMETRY -
  leaving the top/bottom of the clear unpainted. The bars are therefore
  NOT draws (nothing to classify) and the band content is the FULL render
  anamorphically squeezed (the world pass renders normal full-frame at
  the option fov - the live watch read 130.0 exact, mismatch=0, through
  the whole sequence).
- CalcView keeps firing with strict GAMEPLAY and the AUTHORED camera
  choreography in loc/rot (heartbeat showed authored ROLL -7773..-8189
  during the wake-up shot). Pre-fix, the live head drive overwrote that
  choreography (the user's "camera was different than flat"), and the
  letterboxed frame projected across the whole claim (bars as floating
  bands).
- Sequence anatomy from the live transitions: letterbox ON (165/185 px
  of 1080) -> full-screen blackout (correctly rejected by the
  full-black guard: "top 1080") -> ON again (the balcony phase) -> ...
  The boot attract also letterboxes briefly (222/285 px) - caught and
  released cleanly.

Shipped: the LETTERBOX WATCH (hud_capture): three 1-px backbuffer columns
copied to a staging strip per present (async, DO_NOT_WAIT map - the
fov-watch pattern); a row is "bar" only if EXACTLY black (<=2/255) in all
three columns; >=4% height each side + rough symmetry + not-full-screen +
5-interval stability = letterbox, transitions logged. Consumers: the VR
runtime CROPS the submitted subImage (projection AND quad) to the band -
the compositor stretches it back to the claimed fov, which exactly
un-squeezes the anamorphic content (bars gone, geometry correct); the
camera adapter suspends the LIVE head drive while the letterbox holds
(authored camera plays, exactly the flat look - eye offsets still apply,
so the cinematic stays stereo).

Harness trap (session 22, marker retirement fallout): nothing pre-arms
vrinput at boot anymore, and test presses only compose while vrinput is
ON - boot.ps1 now sends `vrinput on` before its A-press loop; any manual
boot flow must do the same.

### Session 22 round 3 - imageRect crop NEGATIVE, the unsqueeze blit

The first letterbox fix cropped the projection layer's subImage imageRect
to the band. IN-HEADSET NEGATIVE (user run, VDXR): the bars stayed
visible - the runtime did not honor the sub-rect on the projection layer
(the drive suspension from the same flag worked, so detection was live;
the crop simply had no visual effect). Replaced with a runtime-agnostic
mechanism: while the letterbox holds, the eye capture stretches the image
band across the full swapchain image ITSELF (blit::stretch_band - a
vs_stretch UV-remap variant of the proven fullscreen-triangle blit, band
sourced from a backbuffer-desc scratch copy; falls back to the plain
CopyResource on any failure). One-shot log: `xr: letterbox unsqueeze
live (band A..B of H)`. Covers projection AND quad modes at once (both
consume the captured image). Do not retry imageRect crops on VDXR.

### Session 22 round 4 - flash-native cinematics, the sampler feedback trap, scanner dormancy

- The round-3 "texture-less fills = bars" fingerprint was WRONG (one dump
  line over-generalized): rendering just those fills in-frame with their
  raw flash blend states blacked out scene content (the user's "can't see
  both hands" regression). Superseded by the correct-by-construction rule:
  while the letterbox holds, the WHOLE flash layer renders in-frame with
  native state - the frame cannot differ from flat (verified: mid-sequence
  flat screenshot is vanilla; authored hands + bars composed by the game) -
  and the unsqueeze crops the bars for the headset. The HUD panel stays
  empty for the duration (subtitles render in-frame like flat).
- SAMPLER FEEDBACK TRAP: the letterbox watch originally sampled the
  backbuffer at the present-detour TAIL - AFTER our own window HUD
  composite painted panel content into the bar rows. With a FOCUSED
  session the detector flapped off constantly and most captures reached
  the eyes unstretched (the "bars still there" headset negatives).
  letterbox_sample now runs at the detour HEAD: pure game pixels, before
  overlay/mirror/composite. Rule: any backbuffer-content DETECTOR must
  sample before our own writers.
- WEAPON-RESOLVER DORMANCY: on saves with no resolvable weapon (early
  game, wrench-only) the scan fallback still ran a multi-second full heap
  walk every ~2048 frames FOREVER (backoff reduced cadence, never
  stopped) - flat-felt as "the game freezes every couple of seconds";
  log signature: [b1r] APlayerWeapon scan ... chosen=00000000 on a ~5 s
  cadence with matching camera-heartbeat gaps. The scanner now goes fully
  dormant after 3 straight failures; the cheap rig/learned reads keep
  running and re-arm it. Verified: exactly 3 scans post-load, then a
  1.000 s heartbeat metronome.

## Session 23 - crash-report forensics, build identification, thumbrest hardware

### Attributing an external crash report to a release (the method)

An external report ("v0.2.0 worked, newer ones crash at the menu or while loading")
arrived with a log and a minidump. The log banner said `bioshockvr 0.1.0 starting`,
which was useless: `BVR_VERSION` was a hand-edited `#define` that stayed "0.1.0" for
the v0.1.0, v0.2.0 AND v0.3.0 releases. Three independent methods identified the
build, and all three agreed on **v0.2.0**:

1. **PE TimeDateStamp of the shipped DLL.** The minidump's module list carries each
   module's `TimeDateStamp`. The reported `bioshockvr.dll` read 0x6A67E457 =
   2026-07-27 23:05:59 UTC, a byte-exact match for the v0.2.0 release asset. Per-release
   fingerprints (download the assets and read `e_lfanew + 8`):

   | Release | bytes | PE TimeDateStamp (UTC) | sha256 prefix |
   |---|---:|---|---|
   | v0.1.0 | 2,749,440 | 2026-07-27 19:55:13 | 8C071BAD24F7C872 |
   | v0.2.0 | 2,779,648 | 2026-07-27 23:05:59 | F96FC7A4D8A80E02 |
   | v0.3.0 | 2,830,848 | 2026-07-28 18:19:45 | D36EEDC8168A2EE0 |
   | v0.4.0 | 2,830,848 | 2026-07-29 00:26:50 | 82EF1971A1C46623 |

   Note v0.3.0 and v0.4.0 are the same SIZE - only hash or timestamp separates them.
2. **Banner exclusion.** "0.1.0" rules out v0.4.0 only (v0.4.0 is the first release whose
   banner is correct). Necessary but not sufficient.
3. **Feature fingerprint in the log.** `[b1r] APlayerWeapon scan:` is emitted
   unconditionally by every scan (`patterns.cpp`, the `%s scan:` line) and the resolver
   does not exist before v0.3.0. 25 s at the menu with ZERO such lines excludes v0.3.0
   and v0.4.0. Verified positively on this machine: a v0.4.0 menu boot logs exactly
   three of them before dormancy.

**Lesson, fixed in this session:** the version now comes from `project(... VERSION)` +
`git describe`, regenerated every build (`cmake/GenerateVersion.cmake`), and the mod logs
its own PE TimeDateStamp at startup. A report is now self-identifying.

### The reported crash (v0.2.0, external machine) - what the dump does and does not say

- `0xC0000005` with `ExceptionInformation[0] == 8`: a **DEP execute violation**. The CPU
  tried to FETCH an instruction from `0x2714FB00`, non-executable game heap. This is a
  different bug class from a null deref and the old crash log did not distinguish them.
- The memory captured around the faulting IP is engine data, not code: adjacent FName
  entries (`CountDownToHeartBeat`, `ResponsePending`) and an object whose vtable resolves
  by RTTI to **`FThreadLockStepExecution`**, holding pointers to two `FSynchronize`
  objects. So the target was a live engine sync object, reached as a function pointer.
- The faulting thread is **a game worker thread, not the game thread**. Frames:
  `BaseThreadInitThunk` -> runnable dispatch at `BioshockHD.exe+0x70CBDE` -> the
  `SetEvent`/`WaitForSingleObject` task loop at `BioshockHD.exe+0x7CCF40`, which
  virtual-calls queued `FSynchronize` tasks. **No bioshockvr.dll frame is on that stack**;
  all of the mod's CalcView and exec work runs on the game thread.
- Timing: the last CalcView heartbeat is 13 s before the fault, i.e. the game was mid
  level-load or transition - matching the reporter's "or while loading".
- The reported run had **no `[reentry]` lines at all**, so 1t/stereo were never armed.
  The session-23 watchdog bug below is therefore NOT this crash.
- **Ruled out: address-space exhaustion.** The report peaked at `PeakVirtualSize`
  1.88 GB, which reads alarming until you note the exe is LAA, so the ceiling is ~4 GB
  (confirmed live: `env: ... host LAA=yes (user address space 4095 MB)`). 46%, not a wall.
- **Not root-caused.** The dump was `MiniDumpNormal` - stacks and module list only, no
  heap and no data segments - so the contents of the smashed slot are unrecoverable.
  Fixed this session (see `crash.cpp`); a fresh report is required.

Environment of the report, for comparison with this dev machine: Win10 19045, 4 cores,
RTX 3070, Quest 2 over VirtualDesktopXR 1.0.10, backbuffer 3840x2160, with DisplayFusion's
`AppHook32` and the Steam overlay also injected. This machine: Win10 26200, 12 cores,
RTX 4060, 1920x1080. Resolution and core count are NOT reproduced locally.

### Thumbrest: what the hardware actually exposes

From `third_party/OpenXR-SDK/specification/registry/xr.xml` (authoritative, in-tree):

| Profile | thumbrest | trackpad |
|---|---|---|
| `oculus/touch_controller`, `meta/touch_plus_controller` (Quest 3) | `touch` | none |
| `meta/touch_pro_controller`, `facebook/touch_controller_pro` | `touch`, `force` | none |
| `htc/vive_controller` | none | `x`, `y`, `click`, `touch` |
| `valve/index_controller` | none | `x`, `y`, `touch`, `force` |

**The Quest 3 thumbrest is a single boolean.** No X, no Y - directional "flicks" on it are
impossible. Quest Pro adds analog `force`, which is pressure (1D), still not a direction.
The flick idea comes from TRACKPADS (Vive wand, Index), which do carry x/y. Any thumbrest
modifier is necessarily CROSS-HAND: a thumb cannot rest on the pad and push the adjacent
stick at the same time. UEVR uses it the same way, as a boolean modifier for DPad-shifting.

**Runtime support verified:** VDXR 1.0.10 reports it -
`xr-input: RIGHT thumbrest touch reported by the runtime` (2026-07-29, Quest 3).
Suggesting the two `/input/thumbrest/touch` bindings raised the action count 17 -> 19 with
no suggestion failure.

### Backbuffer aspect decides how much of the eye render is wasted

The XR eye swapchains are created at the game's backbuffer size, so the BACKBUFFER
ASPECT - a game video-settings choice, not a mod setting - determines the game hfov the
mod must request, and therefore how much of each rendered eye is inside the headset FOV.

The mod requests the hfov that covers the headset's vertical half-angle at the render
aspect: `tan(hfov/2) = tan(vHalf) * aspect`. Measured, Quest 2 half-angles h=49 v=50:

| backbuffer | aspect | requested hfov | horizontal actually visible |
|---|---:|---:|---:|
| 3840x2160 | 1.778 | 129.5 deg | `tan(49)/tan(64.7)` = **54%** |
| 2750x2850 | 0.965 | 98.0 deg | ~100% |

So 4K 16:9 renders 8.3 MPx of which ~4.5 MPx is discarded, while a near-square
2750x2850 renders 7.8 MPx that nearly all lands in the lens - fewer pixels, sharper
result, higher frame rate. Confirmed independently by an external tester who found
2700x2700 both sharper and faster than 4K before we had the explanation.

**Guidance for users: match the game resolution to the headset's per-eye aspect
(near square for Quest), do not just raise it to 4K.** The `xr: headset fov half-angles
... -> game hfov N deg (aspect A)` startup line reports A; the closer to 1.0, the better.

### Session 23 addendum - the second crash class (external, v0.4.0) and the resize churn

A second dump from the external machine is a DIFFERENT crash from the v0.2.0 one:
main thread, `eip/ecx/esi = 0xDEDEDEDE` (freed-memory poison), 22 stack frames of
`virtualdesktop-openxr-32.dll -> d3d11.dll` and no game or mod frame - a use-after-free
of a D3D11 object the OpenXR runtime still held. The dump's timestamp PREDATES the log
sent with it (the log is from the relaunch), so the crashing run's log is lost - exactly
the trap `bioshockvr.prev.log` now closes.

Evidence from the surviving logs of that machine:

- **Mid-session same-size ResizeBuffers is real**: 7 s after FOCUSED, 3840x2160 ->
  3840x2160, and `vr::on_resize()` destroyed and recreated all three XR swapchains
  inline for a no-op resize. Guarded now (same-size resizes keep the swapchains; the
  backbuffer-derived views still release unconditionally because DXGI fails the game's
  ResizeBuffers while references are held).
- **The `windowed` flag is not load-bearing**: one run reported `windowed=0` (DXGI
  `GetDesc().Windowed` at first Present) but the tester runs non-fullscreen, and both
  runs that produced dumps were `windowed=1`. Candidates for the flag on that one run:
  the game's own Fullscreen video setting, or DisplayFusion's window management
  (`AppHook32` is injected in that process). Fullscreen is NOT the discriminator.
- **Pacing oscillation**: CalcView rate flips between ~90/s (headset-paced) and
  6000-7000/s (unpaced) in multi-second blocks on that machine - xrWaitFrame pacing is
  repeatedly lost and regained, matching the tester's "connecting sometimes takes over a
  minute". Unexplained; worth instrumenting if crashes persist after the resize guard.
- **The "v0.2.0 works" premise is dead**: their first dump IS a v0.2.0 crash (worker
  thread, FSynchronize dispatch, 25 s into the menu). All four releases crash on that
  machine; v0.2.0 got luckier runs. There may be no version regression at all.

Still open: defer swapchain destruction on REAL size changes to a safe point in the
frame loop (never between xrBeginFrame/xrEndFrame); the pacing oscillation.

## Session 27 - the object scanner was the freeze, and probably the crash

A third report from the same external machine (`bvr_20260729_163118.dmp` + log, v0.4.1)
crashed at the MAIN MENU 85 s in with `0xC0000374` (STATUS_HEAP_CORRUPTION) raised from
ntdll's heap-verification path: whole stack in ntdll, `fault addr 0`, tid 4072. A heap
fail-fast always reports the allocator operation that NOTICED, never the write that did
the damage, so there is no mod frame on that stack by construction. The log, though,
identifies the object scanner as the only mod machinery active in that window.

### The region filter admitted thread stacks, and the sweep matched its own argument

`region_scannable` accepted `MEM_COMMIT | MEM_PRIVATE | PAGE_READWRITE` under a comment
saying "the object lives on the heap". That filter excludes `MEM_IMAGE` and `MEM_MAPPED`;
it does NOT exclude stacks, TEBs or PEBs, which are all exactly
`MEM_PRIVATE | PAGE_READWRITE`. Every thread stack in the process was therefore swept.

The two false positives in the log prove the mechanism exactly:

```
[b1r] UShockUserSettings vtable match @ 00D3F1B0 HorizontalFOV=643373088
[b1r] UShockUserSettings vtable match @ 00D3F1F0 HorizontalFOV=1444230
crash: eip=76F86F73 esp=00D3F230 ebp=00D3F268
```

Both are 0x40 apart and sit 0x40 to 0x80 BELOW the crashing thread's esp. The old
`scan_region` took eight arguments, so `wantVtable` - the value being searched for - was
pushed onto the scanning thread's own stack on every call, and nested frames held a
second copy. **The sweep was finding its own argument spill.** The shape had been noted
three times before (STATUS sessions 18 and 22, bioshock2 ENGINE_NOTES) as a curiosity for
the per-callsite accept filter to reject, never as a safety problem. `value_scan.cpp` had
carried self-match hygiene since it was written; the vtable scanner never did.

### Two predicates were guarding three unguarded writes

The entire identity test was `dword0 == imageBase + 0xDA3878` plus `int32 at +0x8C` in
`[40,170]`. `is_memory_valid` checks page protection only, so it cannot distinguish a live
allocation from a freed block or a stack slot. And the consumer WRITES: three sites (the
CalcView FOV write, its restore, and the scenedraw stale-restore) poked an int32 at
`+0x8C` with no SEH and no ownership check.

The leading explanation for the `0xC0000374`, consistent with every detail including tid
4072 being the scanning thread: a freed UShockUserSettings-shaped block keeps its vtable
dword until the allocator reuses it, the cache revalidation kept calling it valid, and an
int32 written into it smashed free-list bookkeeping. Two related paths: a stack slot whose
`+0x8C` happened to fall in `[40,170]` would have been accepted and written every
CalcView, and `g_savedGameFov` captured from object A could be restored into object B
after a re-scan.

### The freeze was the same code, measured

One UShockUserSettings pass in that log spans 16:30:07.804 to 16:30:10.896 - over 3 s of
blocked game thread, inside `eventPlayerCalcView`. Across each APlayerWeapon pass the
camera heartbeat drops from ~90 to 16 calls/s. Cost per futile pass had already been
measured at 1-4 s (STATUS session 18).

Why it kept happening despite the session-22 fix:

- `hfov_option_ptr` had a 2000 ms rate limit and **no backoff and no dormancy at all**.
- The weapon resolver's dormancy was a caller-local `static uint32_t nullResolves` that
  **any single momentary success zeroed**. `weapon_valid()` is two chained vtable compares
  on memory the mod does not own, so a stack slot or a churning heap block satisfies it
  transiently, the counter resets, and the walks resume. That is the ~3 s cadence in the
  log.
- `aim.cpp`'s comment "no cutscene/menu heap scans" was false: its `g_gameplayView`
  deliberately counts the menu attract scene as gameplay, which is how full 4 GB walks
  came to run at the main menu - the exact window this tester dies in.

### What replaced it

`core/hooks/heap_scan.{h,cpp}`, game-agnostic:

- **Thread stacks, TEBs and PEBs excluded exactly.** Once per pass every thread is
  enumerated (`Thread32First` plus `NtQueryInformationThread` -> `TebBaseAddress` ->
  `NT_TIB.StackBase/StackLimit`), the whole stack ALLOCATION is excluded rather than just
  its committed part, and the current thread is added unconditionally. A
  guard-page-below probe is a second line for a thread that could not be opened, and
  threads we failed to query are counted and logged rather than silently swept.
- **The needle is never a live value.** The comparison is
  `(*p ^ mask) == (needle ^ mask)`, so the raw vtable address does not exist in the
  sweep's frame at all.
- **A live-heap fast path first.** `heap_blocks()` walks the BUSY blocks of every process
  heap under `HeapLock`, testing only blocks large enough to hold the object and probing a
  few aligned offsets to allow for an allocator header. Two wins: tens of milliseconds
  instead of seconds, and HeapWalk only reports ALLOCATED blocks, so a freed block cannot
  be handed to a caller that is about to write to it. `HeapUnlock` runs on the fault path
  too - a fault escaping with a heap lock held would wedge that heap for the life of the
  process.
- **The region sweep survives only as a sliced fallback**, for an engine that allocates
  from its own VirtualAlloc'd pools. 4 ms wall-clock budget per slice, resumable from a
  saved cursor, so no single call can stall a frame.
- **No logging or allocation inside the SEH guard.** MSVC under `/EHsc` does not run C++
  destructors during SEH unwinding, so a fault taken while the log mutex was held left it
  held for the life of the process - a silent-freeze class that was live in every accept
  filter that logged. Candidates, plus four diagnostic ints each, are recorded into a POD
  array and formatted after the guard returns.

Per-game policy in `patterns.cpp`:

- **UObject corroboration.** `object_class_name()` already walked
  `obj -> +0x30 UClass* -> UClass vtable identity -> +0x28 FName index -> GNames` with
  every step validated. Requiring a non-null result is a far stronger test than the vtable
  dword alone. Deliberately NOT compared against an expected name - the live instance
  could be a subclass, and rejecting it would silently disable FOV control - so the
  resolved name is logged instead, and tightening it later is one line backed by real data.
- **The FOV plausibility window narrowed to the options UI's own 75-130**, from 40-170.
- **A structural dormancy latch** for both scanners, backported from bioshock2r: a success
  clears the miss COUNT, only an explicit `hfov_scan_rearm()` or
  `aim::weapon_scan_rearm()` clears dormancy. Both are called from the one event that
  plausibly created what they were looking for - the gameplay-view transition in
  `CalcViewDetour`.
- **The weapon scan gate moved to the strict `body::is_gameplay_view`**, so it cannot run
  at the menu.
- **Fail closed on ambiguity**: more than one accepted candidate refuses to bind at all.

### Diagnostics that were actively misleading

`accept_weapon` returns `false` on every path and communicates through
`WeaponScanCtx::best`, which was never logged. So
`APlayerWeapon scan: 1 vtable match(es), chosen=00000000` was **unconditional and
structurally meaningless** - an external log could not say whether the resolver had
succeeded or failed. It now reports the real outcome (ATTACHED to the rig / nearest to the
gun spot / NONE), and the summary line carries the path taken, elapsed ms, slices,
heaps/blocks walked and the exclusion-span count.

### Also hardened here

`resolve_skel` bounded the bone count at 128 but never checked that `count * sizeof(Qts)`
bytes were actually readable behind the array pointer, so a plausible count paired with a
bad pointer was a multi-kilobyte write into whatever followed. The SEH guard on `write_n`
made that survivable, not correct; the span is now validated before the pair is trusted.

### Measured: where each object actually lives

From a boot-to-gameplay run with the new scanner (12-core machine, 1920x1080, no headset):

| object | found by | cost | matches |
|---|---|---|---|
| `UShockUserSettings` | **live-heap path**, block prefix | **62 ms**, 271k blocks over 10 heaps | 1, accepted, class `ShockUserSettings` (FName index 2137) |
| `APlayerWeapon` | region sweep only | 3773 ms of work over **888 slices** | 4, none accepted |

So the settings singleton IS a Win32 heap allocation near its block start, and the fast
path resolves it two orders of magnitude quicker than the old blocking walk. Engine ACTORS
are not found in a heap-block prefix - either they sit deeper in their block or they come
from a pool `HeapWalk` does not describe. The block probe prefix was widened from 64 to 256
bytes to give the first case a chance; if actor scans still fall through to the sweep, the
remaining option is enumerating `GObjObjects` (not yet derived), which would retire object
scanning altogether.

The important part is that the sweep's 3.8 s of work no longer arrives as a 3.8 s stall:
across all 888 slices the camera heartbeat held 1600-2100 calls/s with no dip.

### `SETRES` through the viewport Exec seam FAULTS - do not ship a live resolution change on it

Measured directly, in gameplay, with the seam that already handles `set ...` successfully:

```
[b1r] exec fault: eip=106B2353 (exe+0x4C2353) fault-addr=000099DA
[b1r] viewport exec FAULTED on 'setres 1400x1400w' (SEH caught ...)
```

A near-null dereference deep inside the engine's own Exec chain, and no `ResizeBuffers`
followed, so the resolution change never began. Notes:

- The stack stayed BALANCED (the new esp check did not fire), so this is not the
  FOutputDevice stub's argument-count hazard. `set ShockPlayer ...` through the same
  machinery works on every run, so the seam itself is sound - something specific to
  `SETRES` is unhappy, most likely wanting a real output device or a different `this`.
- `patterns.h` has advertised `UWindowsViewport::Exec` as handling `SETRES`/
  `TOGGLEFULLSCREEN` since it was derived. Nothing had ever actually called it. It is now
  measured, and the answer is no.
- Consequence for the resolution work: the persistence lane (writing the game's own
  `Bioshock.ini` viewport keys before launch) is the PRIMARY mechanism, not the fallback.
  A live in-session change stays unavailable until this fault is root-caused.
- This is also the first real exercise of the exec failure latch: the fault disabled the
  seam for the session with one clear line, instead of being retried every 15 s.

### RETRACTED - the "Hor+ with a fixed vertical" law below is NOT established

> **RESOLVED in session 28 - see "Session 28: two lenses, and the one the watch was reading"
> at the end of this file.** Both readings in the table below were real, fresh and correct
> measurements *of different lenses*: the frame carries a world lens AND a foreground lens, and
> off 16:9 they differ by `(16/9)*(h/w)`. `2.1445` is the world lens at 1:1 option 130 and
> `1.2063` is the foreground lens at the same moment. Staleness was never the explanation. The
> retraction was right to refuse the law; the session-27 un-retraction was wrong; the ORIGINAL
> assumption (option is a true horizontal) is what the world pass actually does.

**Read this before acting on the section that follows.** A third reading contradicts it. Same
backbuffer (2048x2048), same `option=130`, two different results:

| sample | rendered tanH | tanV | age |
|---|---:|---:|---|
| in gameplay, right after boot | 1.2063 | 1.2063 | 16 ms (fresh) |
| later, same session | **2.1445** | **2.1445** | 9016 ms (STALE) |

`2.1445 = tan(65)` exactly, which is what the ORIGINAL assumption predicts (option is a true
horizontal, vertical follows aspect). So the rendered projection is not a clean function of
(option, aspect) - something else varies between samples, and the 9 s stale age means the
second one may not be a gameplay world draw at all.

The law below was derived from a single pair of readings and does not survive the third. The
code changes it motivated (submission claim, suggested-option formula, and the fov-mismatch
verdict) were written, built, and then **reverted unbuilt-upon** rather than shipping a
plausible-but-unproven formula into the submission path - the mismatch verdict in particular
feeds `vr::cinematic_active()`, so getting it wrong drops normal gameplay onto the big-screen
quad.

What went wrong methodologically: every sample was taken through `fovaudit live`, whose value
can be stale, and the state at sample time (gameplay world draw vs menu vs scripted camera)
was not pinned. A correct measurement needs the sample age checked (<500 ms), the view state
confirmed as strict gameplay, and - crucially - a LIVE XR SESSION, because the submitted side
of the comparison does not exist flat (`swap=0x0`, `submitted tanH=0.000000`). Flat testing
cannot close this one.

Keep from below: the ini lane and the square-backbuffer results are independently verified and
stand. The pixel-efficiency arithmetic stands. The FOV law does not.

### UNPROVEN (see retraction above): the world lens is Hor+ with a fixed vertical

Two `fovaudit live` readings of the rendered projection, same `option=130`, different
backbuffer, taken through the ini lane below:

| backbuffer | aspect | rendered tanH | rendered tanV |
|---|---:|---:|---:|
| 1920x1080 | 1.7778 | 2.1445 | 1.2063 |
| 2048x2048 | 1.0000 | **1.2063** | **1.2063** |

**The vertical did not move. The horizontal collapsed.** So the law is the OPPOSITE of what
this project and BioVRDev both assumed:

```
tanV = tan(option/2) * 9/16     <- the option is a 16:9-REFERENCED horizontal, and what the
                                   engine actually derives from it is the VERTICAL
tanH = tanV * (live aspect)     <- horizontal follows the window. Hor+.
```

Both rows fit exactly: `tan(65) * 9/16 = 1.2063`, and `1.2063 * 1.7778 = 2.1445`. At 16:9 the
two conventions coincide (`tanH == tan(option/2)`), which is why this went unnoticed through
every session so far - every measurement had been taken at 16:9.

Cross-check against the other mod's independently measured table: BioVRDev record
`GameFovDegrees=100 -> 100.0h / 67.7v` at 16:9, and `tan(50)*9/16 = 0.6703 ->
2*atan(0.6703) = 68.4 deg` vertical, `0.6703*16/9 = tan(50)` horizontal. Their numbers are
this law, not the one their spec reasons with.

**Consequence 1 - our submitted claim is wrong at any non-16:9 aspect.** Submission takes
`hfovDeg` from the option readback and derives `halfV = atan(tan(halfH) * swapH/swapW)`
(openxr_runtime.cpp). At 16:9 that reproduces the render exactly. At 1:1 it claims 130x130
against a rendered 100.7x100.7 - a 1.78x horizontal stretch for the compositor to
reproject, i.e. the yaw-warp signature. The mod already detects it: that run logged
`mismatch=1`. The fix is to build the claim from the measured law
(`tanV = tan(option/2)*9/16`, `tanH = tanV*aspect`) instead of assuming the option is a true
horizontal.

**Consequence 2 - "render square and keep FOV 100" under-shoots badly.** At 1440x1440 with
option 100 the engine renders ~68x68 deg, not 100x100. That advice appears in BioVRDev's
SPEC and is contradicted by their own table.

**Consequence 3 - the pinned 130 is accidentally correct at square aspect.** Solving
`tan(option/2)*9/16*a = tan(H/2)` for `H=100, a=1.0` gives `option = 129.5`, which is what
VR PRESET 1 already writes (capped at the engine UI's 130). So at a square resolution the
shipped config renders ~100.7x100.7 - very nearly the ideal for a Quest-class eye. No FOV
policy change is needed there; the claim is the bug, not the render.

### The ini lane works, and the engine honours a square backbuffer

`%APPDATA%\BioshockHD\Bioshock\Bioshock.ini`, section `[WinDrv.WindowsClient]`. ANSI, CRLF.

**FOUR sections carry the same viewport key names** - the PC driver at line 429 plus
`[XeDrv.XenonClient]` (468), `[DurangoDrv.DurangoClient]` (500) and `[OrbisDrv.OrbisClient]`
(532). A key replacement that is not section-scoped writes a console driver section and looks
like it succeeded. Every write in `game_ini.cpp` is scoped to the PC section, writes BOTH the
windowed and fullscreen pairs (UE2 reads whichever mode it starts in), backs up once to
`.bvr-bak-res`, writes via temp + `ReplaceFileW`, and re-reads to verify.

Verified end to end: `vrres 2048x2048` -> `viewport set to 2048x2048 ... verified` -> relaunch
-> `first Present: backbuffer 2048x2048`. The engine accepts a non-display-mode square size in
windowed mode without complaint.

Two caveats. The write survived process exit in testing, but the harness hard-kills the
process, which skips the game's orderly `SaveConfig` - whether a clean quit through the menu
clobbers it is NOT yet tested. And `HorizontalFOV=130` currently sits in the shipped user ini,
which is the mod's own live FOV write having been saved by the game at some earlier exit.

A user datapoint that corroborates the whole thesis, independently: on a Dream Air they found
their runtime's own resolution slider did nothing (it cannot - nothing in the mod reads
`recommendedImageRect`), and editing this file to 7680x4320 made the image "super crisp". At
our 130 option and 16:9, only about 4120x4270 of that 7680x4320 falls inside a ~49x50 deg
eye: 17.6 MP useful out of 33.2 MP rendered, ~53% efficiency, matching the ~54% figure
already recorded above. The same sharpness is available near-square for ~18 MP. It also caps
how aggressively a derived target may clamp: 33 MP is a resolution a real user is happily
running, so any total-pixel ceiling must be generous and overridable.

## Session 28: two lenses, and the one the watch was reading

**This closes OPEN BUG 2 (yaw warping at non-16:9) and settles both lens laws.** Everything
below is measured, not derived: `dumpframe full` at 2750x2850 decoded by
`tools/decode-framedump.ps1` (which has always applied the structural zero-slot validation the
live watch did not), at two FOV options, on both SR eyes, plus a `vrfgfov on/off` A/B that
identifies the clusters by which one moves.

### 1. THE HEADLINE: a frame carries TWO perspective lenses, and off 16:9 they DIFFER

At 16:9 they coincide exactly, which is why every measurement from session 21 to session 27 saw
a single cluster and why every "law" derived from a single reading was a coin flip.

```
WORLD pass:      tanH = tan(option/2)          <- aspect-INDEPENDENT
                 tanV = tanH * (h/w)           <- vertical follows the window ("Vert+")

FOREGROUND pass: tanV = tan(fgFov/2) * 3/4     <- aspect-INDEPENDENT (the 4:3 spec)
                 tanH = tanV * (w/h)           <- horizontal follows the window ("Hor+")
```

The two conventions are exact opposites, and their ratio is `(16/9)*(h/w)` - 1.0 at 16:9,
1.7778 at 1:1, 1.8425 at 2750x2850.

Measured, 2750x2850 (aspect 0.964912), `decode-framedump.ps1` cluster output:

| option | vrfgfov | world cluster (draws) | fg cluster (draws) |
|---:|---|---|---|
| 100 | on | tanH **1.1918** tanV **1.2351** (154) | tanH 0.6468 tanV 0.6704 (24) |
| 100 | off | tanH **1.1918** tanV **1.2351** (138) | tanH 0.4178 tanV **0.4330** (24) |
| 130 | on | tanH **2.1445** tanV **2.2225** (163) | tanH 1.1640 tanV 1.2063 (24) |

Every number is exact: `tan(50) = 1.191754`, `1.191754 * 2850/2750 = 1.235090`,
`tan(65) = 2.144507`, `2.144507 * 2850/2750 = 2.222514`. The `vrfgfov off` row is the identifier
- **only the second cluster moved**, and it landed on `0.4330127`, the native foreground vertical
already hardcoded in `patterns.h`'s `kFgCbFingerprint[6]` (`tan(30)*3/4`). Both eye windows (q0
and q1) are identical, so this is not an eye-attribution artifact.

**So the world lens is what this project originally assumed, and what BioVRDev assume too.** The
submission formula in `openxr_runtime.cpp` (`halfV = atan(tan(halfH) * swapH/swapW)`) has been
correct at every aspect all along, and it was correct to revert all three session-27 rewrites.

### 2. THE BUG: the live watch was reading the foreground lens, and the claim followed it

`core/gfx/hud_capture.cpp` sampled the cb0 head of the **first** DSV-bound `DrawIndexed` of each
present interval whose VS b0 was >= 320 bytes. Three facts make that the viewmodel:

- the foreground draws are **the first draws of the main pass** (recorded since session 21);
- the foreground tier is **576 bytes** (`patterns.h` `kFgCbBytes`), which clears the 320 gate;
- the foreground block carries the screen-ray helper at the **same floats 12..18**, so the
  decode cannot tell it apart - and the two cross-checks the decode did run (`f[12]/2` against
  `-f[14]`, `-f[17]/2` against `f[18]`) are **intra-axis**: they confirm each triple is
  self-consistent and carry no information about which lens or which axis it belongs to. The
  three structural zero slots that *would* disambiguate (`f[13]`, `f[15]`, `f[16]`) were never
  tested, although the offline decoder has always tested them.

Consequences, in order:

1. `fov_mismatch()` compared the FOREGROUND tangent against `tan(option/2)`. Off 16:9 that ratio
   is 0.54, far outside the +-10% window, so **the verdict latched ON during normal gameplay**
   and stayed on - visible in the session-27 log as one `[hud] rendered-fov mismatch ON` line at
   03:50:56 and no `off` line for the rest of the session.
2. A latched `fovMm` with `g_cineStereo` (default true) routes the projection claim through the
   `fovMm && stereoCine` branch in `on_present_end`, which replaces the option-derived claim with
   **the live watch's tangent** - i.e. with the viewmodel frustum. That is the `src=live` on the
   session-27 submit line: `tanH=0.646840 tanV=0.670361 ... src=live swap=2750x2850`, over a
   world actually rendering `tanH=1.1918`. **A 1.842x under-claim of the submitted frustum.**
3. The compositor was therefore told the image spans +-32.9 deg horizontally when it spans
   +-50 deg. Everything reads magnified by 1.84x in tangent space, and every head rotation is
   mis-reprojected: a feature dead ahead at display time is displaced by
   `atan(k*tan(delta)) - delta`, several degrees at ordinary turn rates, snapping back the moment
   the head stops. **That is the warp.**

Why the observed signature is exactly this and nothing else:

- **FOV-slider independent** (the strongest constraint in the report): the error is
  `k = (16/9)*(h/w)`, which has no option term at all. Sweeping the slider changes the visible
  extent and never the distortion.
- **Aspect-gated**: at 16:9 the two lenses coincide, the verdict never latches, the claim stays
  `src=readback` and is correct. 1920x1080 is clean *by construction*.
- **Yaw-dominant, pitch clean**: the tangent error is uniform, but yaw excursions in play are far
  larger and faster than pitch, so the reprojection error is felt on yaw.
- **BioVRDev do not warp at the same 2750x2850**: they submit the option-derived claim and have
  no mismatch detector, so nothing ever substitutes the fg lens - and under the law above their
  claim is simply correct. The contradiction that made this look unexplainable was the clue.

Why the two session-27 eliminations, both honestly measured, could not catch it:

- **`src=live` was read as proof of correctness.** The reasoning was "live means derived from the
  measured rendered tangent, therefore it tracks the render by construction". The label was true;
  the lens it tracked was the wrong one. A source tag is not a correctness proof.
- **The pose audit is structurally blind.** It compares `projViews[0].pose.orientation` against
  `g_consumedHeadQuat`, and `g_consumedHeadQuat` is stamped inside `get_head_pose()` from the same
  `xrLocateSpace`/`xrLocateViews` generation the tag comes from. `delta 0.00 deg` was guaranteed
  by construction. It proves the locate-to-tag plumbing is intact and says nothing about whether
  the tag matches the pose the frame was RENDERED from. Recorded here so the next reader does not
  re-derive it: **that audit cannot eliminate the pose hypothesis, and the session-27 write-up
  over-claimed when it said pose latching was ruled out.**

### 3. Two suspects killed by measurement along the way

- **Head roll is NOT erased between tick and render.** BioVRDev record that this engine erases
  camera roll and ship a render-thread roll re-write; BS1R does not need one. `simhead 0 0 40`
  then `reentry dump`: the engine's own render submit receives
  `rot=(0,28303,7281)=(0.0,155.5,40.0)deg` - roll intact - and the screenshot shows the world
  rolled 40 deg. So the layer pose (which carries roll) matches the render, and roll is not a
  claim/render mismatch on this game.
- **Per-eye render orientation is provably identical.** The same dump shows both SR passes
  submitting `rot=(0,28303,0)` bit-identical, with locations 6.36 UU apart laterally
  (`ipd 63.4 mm` at worldScale 100 = 6.34 UU) and `dz = 0` at roll 0, becoming
  `dz = 4.1 UU` at roll 40 - the session-22 full-rotation right axis working as designed.

### 4. What shipped

- **`hud_capture.cpp` rewritten to vote, not to guess.** Up to 8 cb0 heads per interval into one
  640-byte staging buffer, sampled at a **stride** derived from the previous interval's distinct
  cb0 count so the samples span the whole pass (a first-8 sample is dominated by the foreground:
  measured 5/8 votes for the viewmodel at 2048x2048 before the stride went in). Clusters are
  voted; the winner is published as the world lens and the runner-up as the fg lens.
  `decode_ray_block` now enforces the offline decoder's structural zero slots, absolute
  0.001 pair agreement, and bounds on both axes. Two guards refuse a round rather than publish a
  marginal one: **majority** (a winner must exceed half the decoded samples) and **coverage** (a
  round whose stride was stale, so it only spanned the head of the pass, is discarded - this is
  the scene-change transient, and it fires exactly once entering gameplay).
- **The age gate moved into `fov_watch()`**, default 500 ms, `0` meaning "give it to me anyway
  and I will label it". Every printed line now says `FRESH` or
  `STALE - DO NOT CONCLUDE` in words. `fovaudit live` reports both lenses, the vote split, the
  stride, the sample count and the backbuffer aspect on one line, plus a `laws` line stating the
  expectation for the live aspect - no conclusion needs two log lines cross-referenced again.
- **The `fovaudit` option-derived column no longer falls back to a hardcoded 9/16** when there is
  no XR session. It uses the real backbuffer dims (`bvr::hud::backbuffer_dims`). Flat is where
  the measuring happens, and that fallback was wrong at every aspect but 16:9.
- **The claim substitution announces itself** once per session with the age and the reason, so an
  unexplained `src=live` can never hide a lens swap again.

Flat verification at 2048x2048 (the resolution the README recommends and the bug was reported
at): `WORLD tanH=1.191754 tanV=1.191754 (hfov 100.00 deg, 6/8 votes)`,
`FG tanH=0.670361 tanV=0.670361`, `lenses=2 mismatch=0 cineActive=0`, both ages 15 ms FRESH,
`ambiguous rounds 1` (the coverage guard doing its job at the menu->gameplay transition), and
**no `rendered-fov mismatch ON` line anywhere in the run** where session 27 had one within
seconds of reaching gameplay.

### 4b. The hands moving with the head was the SAME defect, seen from the other side

Reported immediately after the warp fix landed in-headset: the world stopped warping and **the
hand/gun model started moving with the headset** - the thing sessions 13-16 were spent fixing.
It is not a regression in the hands machinery. It is one claim serving two lenses.

**One projection layer carries ONE fov claim for the whole eye image, and the world and the
viewmodel were rendered through DIFFERENT frustums.** So only one of them can be geometrically
correct at a time:

| claim | world | viewmodel |
|---|---|---|
| fg lens `0.6704` (the pre-session-28 bug) | 1.78x wrong -> **warps** | correct |
| world lens `1.1918` (after the watch fix) | correct | 1.78x wrong -> **moves with the head** |
| lenses MATCHED, either claim | correct | correct |

The error is angular gain: a viewmodel feature at fg-angle `t` is displayed at
`atan(tan(t)*1.7778)`, so it swings 1.78x too far as the head turns - which reads exactly as the
gun sliding off the hand. Fixing the world did not break the hands; it moved a pre-existing 1.78x
error from the world onto the hands, and the only state where both are right is matched lenses.

**Second, coupled cause, in the bone solve.** `bones.cpp render_lock_delta` carried the comment
"with the lens match armed the rig renders through the WORLD lens ... the lens ratio k collapses
to 1". That was TRUE at 16:9 and false everywhere else: with the shipped `0.75` the real ratio at
a square backbuffer is 1.7778, so the depth constraint `wStar = k*df` and the head-split lateral
cancel were both mis-scaled - and the lateral cancel is precisely the term that stops the rig
sliding under head motion. `world_ndc` had the same hardcoded `9/16` for the WORLD lens, which is
independently wrong off 16:9. Both now read the live backbuffer aspect via
`bvr::hud::backbuffer_dims`, and with the matched fg write `k` genuinely collapses to 1 - the
assumption is earned rather than asserted.

**Flat gate, and it is the same instrument that found the original bug.** At 2048x2048,
`decode-framedump.ps1` went from two clusters to **ONE**:

```
before:  cluster tanH=1.1918 tanV=1.1918  draws=154  b0tiers[320:47 576:42 832:65]
         cluster tanH=0.6704 tanV=0.6704  draws=24   b0tiers[576:20 832:4]
after:   cluster tanH=1.1918 tanV=1.1918  draws=132  b0tiers[320:57 576:39 832:36]
```

The 576-byte foreground tier is now INSIDE the world cluster - the fg draws carry the world's
tangents. `fovaudit` agrees: `lenses=1`, `fg lens match ON (last written 115.6 deg, k=1.333333)`.
The engine accepted the wider write without clamping (115.6 deg at option 100 square; measured
`tan(57.8)*0.75 = 1.1918` == the world vertical, exact). `vrfgfov legacy on` puts the second
cluster back and `legacy off` removes it again, verified both directions - an instant in-headset
A/B.

**What flat CANNOT confirm**, stated so nobody assumes it was: the bone-solve half. With no XR
session there are no controller poses, so the hands drive never engages (`inst=0 writes=0
solves=0`) and `render_lock_delta` is never entered. The lens geometry it depends on is proven;
the solve itself needs the headset.

**IN-HEADSET: ACCEPTED, and the carry closed itself.** User verdict: "without changing anything
it's perfect, both the world and the gun/hand models". The open question was whether the
session-16 hand offsets in `vrpreset.ini` had absorbed part of the 1.78x lens error while being
tuned against it - **they had not.** No re-tune, no offset change, no need for the
`vrfgfov legacy on` comparison. The offsets were right all along and the fg lens was the only thing
wrong, which retroactively validates the sessions 13-16 calibration method: it was solving for the
rig, not papering over a projection error.

This also confirms the bone-solve half that flat could not reach. `render_lock_delta`'s head-split
lateral cancel is the term that stops the rig sliding under head motion, and it depends on
`k == 1`; the rig is now stable in-headset, so `k` really does collapse to 1 with the lenses
matched and the live-aspect model is right.

### 5. The foreground-lens aspect law, and the `0.75` it condemns

Stage 2's blocked measurement (b) is answered by the same dumps. `camera.cpp`'s foreground match
writes `fg = 2*atan(tan(worldHalf) * 0.75)`. Matching the world needs the fg VERTICAL to equal
the world vertical:

```
tan(fgHalf) * 3/4 = tan(option/2) * (h/w)
  =>  tan(fgHalf) = tan(option/2) * (4/3) * (h/w)
  =>  the constant is (4/3)*(h/w), which is 0.75 ONLY at 16:9
```

Checked against the measurement at 2750x2850 option 100: `tan(50)*1.3333*1.03636 = 1.64676`,
`fgFov = 117.4 deg`, `fg tanV = 1.64676*0.75 = 1.23507` == the world's `1.2351`, and
`fg tanH = 1.23507*0.96491 = 1.19175` == the world's `1.1918`. Exact.

So the shipped `0.75` under-lenses the viewmodel by `1.7778/aspect` - **1.78x at a square
backbuffer, which is what the README tells users to run**, and 1.84x at 2750x2850. That is the
"hands look huge" report, quantified. The fix reduces to `0.75` exactly at 16:9, so it cannot
invalidate the session-16 in-headset calibration. NOT shipped yet: it changes viewmodel scale
visibly and must not confound the warp re-test.

### Superseded: the earlier "still unmeasured" note on the world lens aspect law

At 1920x1080 the live watch reads `tanH=2.1445 tanV=1.2063`, i.e. `tanV/tanH = 0.5625 =
9/16` exactly, consistent with `tanH = tan(option/2)` and a vertical derived from the
render aspect. But 9/16 IS the render aspect here, so this run cannot distinguish
"vertical derived from aspect" from "vertical hardcoded to 9/16". Discriminating it needs a
non-16:9 backbuffer, and with `SETRES` dead that now requires an ini change plus a
relaunch. Until then the foreground-lens aspect correction is unproven and must not ship.

## Session 29: the cinematic bars are a gameswf DRAW - session 22's reading retracted

### 1. THE HEADLINE: `WidescreenBars` is a flash sprite painted OVER a full-frame image

Session 22 round 2 recorded that the engine "CLEARS the final target to opaque black and draws
the tonemap as a vertically SHRUNKEN quad", making the bars *unpainted clear* and the band
content *anamorphically squeezed*. **That is wrong.** The bars are a normal gameswf draw on top
of a full-frame tonemap, and nothing is squeezed. Two independent measurements, neither needing
a headset:

**(a) The Nexus mod is a one-byte SWF edit, and the byte says what the bars are.** The
"Fullscreen Cutscenes" mod ships a `HUDPC.swf` exactly **one byte larger** than stock
(12,322,709 vs 12,322,708). Byte-diffed against the stock file: the SWF header length field +1,
and a single edited tag at file offset ~0x0B750DE - a **`PlaceObject2` (tag code 26, body
length 25 -> 26)** that places **character id 292 at depth 256 under the name
`WidescreenBars`**. Stock MATRIX is translate-only (`HasScale=0, HasRotate=0`, 5 bytes); the
mod's is 6 bytes with **`HasScale=1` and `NScaleBits=0`, i.e. ScaleX = ScaleY = 0**, translate
preserved. It scales the sprite to nothing. A sprite is a draw.

Corroborating strings, all read-only from the shipped files: `HUDPC.swf` contains
`WidescreenBars` (4 hits) plus `ShowWidescreenBars` / `HideWidescreenBars`, and **both function
names are also in `ShockGame.U`'s name table**, adjacent to `UpdateRadialAxis` /
`CheckForRightRadialChange` - i.e. they are HUD-class UnrealScript functions driving the movie.
(Reaching those directly would need a ProcessEvent seam, which BS1 does not have - that is BS2's
design. Recorded as the alternative route, not taken.)

**(b) A framedump taken inside the letterbox shows the full-frame tonemap and the bar draw
after it.** `dumpframe full 2` fired automatically on the `[hud] letterbox ON` transition
during the Gatherer's Garden Electro Bolt sequence at 2048x2048 (bars measured 313 px top /
350 px bottom). Both captured intervals are identical in structure. On the final target `T0`:

```
00262 ClearRTV  rtv0=T0  color=(0.000 0.000 0.000 1.000)   <- opaque black clear
00263 Draw      a=6   rtv0=T0  vp=2048x2048  srv0=T2       <- tonemap, FULL viewport
00267 Draw      a=29  rtv0=T0  vp=2048x2048  srv0=T-1      <- textureless: the BARS
00268 Draw      a=5   rtv0=T0  vp=2048x2048  srv0=T44      <- textured flash quads
```

Events 267+ all carry the gameswf batch-flush RVA `0x7B8EB5` in their stack, so they are the
flash layer, issued AFTER the tonemap. Exactly one of them is textureless.

**Why session 22 got it wrong, and the lesson:** the frame dump captures no vertex buffers, so
"shrunken GEOMETRY" was never a measurement - it was an inference from seeing a black clear and
black bars with no draw identified in between. The tonemap's `cb0` is `[1.0, 0, 0, ...]` and
carries no vertical scale term at all, so cb0 could not have supported it either. Round 3's
"texture-less fills = bars" fingerprint, retracted in round 4, was **right about what the bars
are**; round 4's regression came from re-rendering those fills with raw flash blend states,
which is a different operation from not issuing them.

### 2. What this retires

- **`blit::stretch_band` / `vrcine unsqueeze` is DELETED, not defaulted off.** It stretched a
  measured band across the full image to undo a squeeze that does not exist, so on real content
  it could only ever have cropped picture and distorted the aspect. Three in-headset rounds
  failed to remove the bars with it, and the premise is why.
- The two confounds that made those rounds uninformative are worth recording, because both
  would have repeated: (i) all three ran at **1920x1080**, where the layer claim is
  `halfV = atan(tan(halfH)*9/16)` - at the measured 104 deg horizontal that is a **71.6 deg
  vertical claim inside a ~96 deg Quest 3 eye, i.e. ~12 deg of permanent black band top and
  bottom whether or not a cutscene is playing**. That is the same ambient banding the user was
  raising FOV to chase in session 28, and "bars stayed" may have been measuring it.
  (ii) The only evidence the stretch ever ran was `xr: letterbox unsqueeze live`, a
  **process-lifetime one-shot**; the boot attract also letterboxes (222/285 px, session 22), so
  that line could fire at boot and prove nothing about the cutscene. **Rule: a one-shot log can
  establish that a path ran once, never that it covered an episode.**

### 3. What replaced it

`hud::on_draw`'s verdict widened from `RTV* | nullptr` to `PassThrough | Redirect | Skip`, and
`DrawDetour` (frame_inspector.cpp) gained the only early-return in the mod that drops a draw.
Skipping is safe where redirecting is not: it changes no device state, so the gameswf batch's
own state machine is untouched and the next draw behaves exactly as it would have.

Discriminator, while a cinematic holds: **a gameswf draw on the HUD target with no texture bound
(`srv0` unresolvable)**. Every other flash element - subtitles, HUD art - samples a UI atlas.
Measured vertex count is 29, reported by `vrcine status` rather than hardcoded, since a
tessellated shape's count is not guaranteed stable across shots.

**The circularity this creates, and the fix.** Keying the cinematic gate on black pixels stops
working the moment suppression does: no bars painted -> `letterbox_sample` sees nothing -> the
branch is not entered -> the bars are painted again, flapping every other interval. So the
**bar draw itself is now the primary cinematic signal** (`hud::bar_draw_active()`), and
`hud::cinematic_hold()` = that OR the pixel watch. The pixel watch BOOTSTRAPS the hold (it needs
~6 presents of real bars: async staging map + 5-sample hysteresis), the draw signal SUSTAINS it,
and both go quiet one interval after the draw stops. The two are kept as **independent**
sources and reported separately by `vrcine status` - one reads draw calls, the other reads back
the backbuffer, so their agreement is evidence. With bars hidden, `barDraw=1 pixelWatch=0` is
the expected steady state, not a fault.

Every consumer that must hold for a whole cutscene now calls `hud::cinematic_hold()`, never
`hud::letterbox()` - with the bars suppressed there are no black pixels left to detect.

### 4. `bones::release()` - stopping the drive is not handing the skeleton back

Three pieces of drive state outlive the last `drive()` call, and all three were suspected in the
session-22 round-5 report ("the controllable rig hands instead of the authored cinematic ones"):

- `reapply()` keeps repainting the cached pose for up to 100 ms **and calls `set_dirty(0)` while
  doing it**, so it actively suppresses the engine re-evaluation that would restore the authored
  animation - roughly 6 frames into the cutscene.
- `restore_hidden()` is called from exactly one place, inside `drive()`. Stop calling `drive()`
  and the collapsed inactive hand stays collapsed, with the weapon-attach bone parked at
  `{0,0,-5000}`.
- The sleeve-collapse latch was a **function-static inside `drive()`** - a latch only `drive()`
  could see was a latch only `drive()` could clear, which is the shape of the bug itself. Hoisted
  to file scope.

`release()` restores both, clears the reapply cache, calls `set_dirty(1)` to hand the skeleton
back actively, and drops `g_refValid` so the next drive re-adopts a FRESH engine pose (the same
reasoning as `set_sway_kill(false)`) - otherwise the first post-cutscene frame rebuilds the rig,
and the barrel axis the laser and aim dot ride, from a pre-cutscene pose. **Order matters:** both
restores read `g_ref`, so `g_refValid` must be cleared last or `restore_hidden()` silently
becomes a no-op and the hand stays collapsed.

**Status: this diagnosis is from reading code and is still UNCONFIRMED.** The flat run measured
`vrDriving=0` at the cutscene edge, which does show the drives were already suspended - but with
no XR session `vrDriving` is false always, so it cannot distinguish "gated by the letterbox" from
"gated by no headset", and `hiddenHand=-1 refValid=0` merely means the bone drive never ran. The
`[b1r] cine edge` line exists to settle it in-headset.

### 5. The drives were already gated - by accident, not by contract

The roadmap item said the hands/aim/laser drives run ungated through cutscenes. They do not:
`driveHead` is false under a letterbox, so `vrDrove`/`ctx.vrDriving` is false, and all three
consumers bail on it (`hands.cpp` live lane, `aim.cpp` per-hand loop, the laser publish). That is
a side effect of the head gate, not a stated contract - and `authored+look` breaks it by design,
because it drives the head again. The gates are now EXPLICIT in both `hands.cpp` and `aim.cpp`.
The one line in `aim.cpp` covers three things at once, since all three read `g_gameplayView`: the
fire-seam substitution (via `ray_for`), the per-weapon profile heap scans, and the laser publish.

`authored+look` adds the head's rotation DELTA since the shot began, with **no positional term**.
It cannot reuse the gameplay path: `camera.cpp` writes `rot->pitch` and `rot->roll` ABSOLUTELY
from the head, which would erase the authored choreography (session 22 measured authored roll
walking -7773..-8189 through the wake-up shot). The reference is captured at the cinematic edge
and dropped on both edges, so every shot opens framed exactly as authored.

### 6. Cinematic FOV: the scene animates its own lens

During the Electro Bolt sequence the live watch reports **two lenses**, with the WORLD lens
SWEEPING while the foreground stays fixed:

```
WORLD tanH 1.215944 -> 1.272701  (hfov 101.13 -> 103.68 deg, 6-7/8 votes)
FG    tanH 1.191754               (100.00 deg, constant)
```

The scene dollies/zooms its own camera. The option is 100, so the sweep stays inside
`fov_mismatch()`'s +-10% band, the mismatch never latches, and `cinematic_active()` stayed 0
through the whole sequence - the session-28 claim-substitution branch correctly never fired.
Worth knowing for BS2: a cutscene is a place where the two lenses legitimately differ, so a
`lenses=2` report there is not automatically the session-28 defect.

### 7. The aim dot: dot == shot by shared DATA, not shared algebra

The laser re-derives its ray on the RENDER thread from the controller pose; the fire seam
substitutes `g_ray` built on the GAME thread in game space. Those agree by shared algebra
(`xr_math.h`) but differ in pose instant and origin-offset basis. The aim dot closes that gap by
converting the FINAL fire-seam ray point back into XR space with `game_point_to_xr` - the exact
inverse of `xr_pose_to_game`'s position half, which is affine and yaw-only:

```
forward: loc = base + Rot(gameYaw - recenterYaw) * xr_to_ue(pos - recenterP) * scale
inverse: pos = recenterP + ue_to_xr( Rot(recenterYaw - gameYaw) * (loc - base) / scale )
```

The dot is published only when `ray_for()` would succeed, so its presence proves the fire
substitution is live - the sight is also an instrument. The first publish logs the forward/inverse
round-trip error in UU and says in words whether it is exact, because a transform wrong by a yaw
term looks entirely plausible in the headset until you fire.

No trace exists anywhere in the mod (`kExpectedActorTraceRva = 0x547BD0` in patterns.h is
declared and never referenced), so the distance is a slider. BioVRDev's dot has the same shape
and the same limitation. A dot and a bullet hole only fuse in stereo at matching depth, so the
calibration flow is: set the distance to the wall, fire, nudge the trim onto the hole.

### 8. Harness notes

- The A-press loop in `boot.ps1` does **not** activate the main menu on this build, and
  `game-click.ps1` hovers the entry without activating it either. Menu navigation currently
  needs a human; in-game triggers (`vrinput test press A`) are unaffected.
- The revert-Options `#32770` "Message" dialog blocks the first Present entirely - the game
  reports alive and responding with `presents=0/s` until it is dismissed.
- `presents=0/s` with `calcview in=~2800/s` is the normal UNFOCUSED state, not a hang. Focused,
  the same scene reads `presents=443/s 2nd=222/s`.
- The Electro Bolt sequence is many short letterbox phases (313/350, 380/401, 313/298, 313/345
  px of 2048), not one - so an edge-triggered capture fires repeatedly through it.

### 9. The save-load hang: releasing bones from the wrong call site (session 29, in-headset)

A save load hung the game. `responding=False`, log dead. The last four lines name the defect
without ambiguity - all in the SAME millisecond, with the camera already at the NEW level's
coordinates:

```
19:13:33.618 [b1r] camera: loc=(-24927.1 4802.5 8275.3)   <- new world
19:13:33.618 [b1r] cine edge ENTER (barDraw=0 letterbox=0 cineQuad=1) | bones: hiddenHand=0
                    cacheAge=3281ms refValid=1
19:13:33.618 [bones] released to the engine (cinematic started): hidden hand 0 restored ...
19:13:33.618 [hands] world changed - actor caches cleared
19:13:33.618 [bones] world changed - skeleton cache cleared
```

`bones::release()` ran from the CAMERA-side cine-edge block, which sits ABOVE
`hands::on_calcview` - and `hands::on_calcview` is what detects a world change and calls
`bones::on_world_change()`. So the release wrote `restore_hidden()`'s ~1.8 KB (cluster + sleeve,
12+16+12 bytes per bone over ~47 bones) plus `set_dirty(1)` through the PREVIOUS level's
skeleton, which the engine had already freed and the loading level had already reused.

**Why it hung instead of crashing, and why that is the trap.** `write_n` is `__try/__except`
guarded, so a write to unmapped memory returns false harmlessly. That guard is worthless here:
the pages were still MAPPED, just owned by something else. SEH catches access violations, not
silent corruption - so the safety net made the failure quieter, not less likely.

**The rule was already written down.** `hands.cpp`'s world-change block says it verbatim: *"the
old actors died with the old world, and recycled heap addresses must never be written to. The
scale bookkeeping is dropped, not restored - restore would write into a stranger."* The bug was
not a missing insight; it was a new call site that bypassed the insight. **Any function that
writes engine memory must be called only from below the world-change check, or must interlock
against it itself.**

Fixed three ways, deliberately overlapping:

1. The camera-side edge block no longer writes bones at all - it only logs. `hands.cpp` releases
   at its gate, which runs AFTER the world-change check.
2. `release()` now refuses to write unless `g_skelInst && g_bones && g_boneCount > 0`, and in
   that case clears its own bookkeeping instead. `on_world_change()` nulls those, so the
   interlock holds from ANY call site, including ones not yet written.
3. `on_world_change()` also clears `g_wasCollapsed`/`g_collapsedHand`. Hoisting that latch out of
   `drive()` (so `release()` could reach it) silently made it outlive a world change; a stale
   `true` would have written a dead world's sleeve from a dead world's reference.

**Carry for BS2 and for any future drive:** the pattern "stop driving" and "hand state back" are
different operations with different safety requirements. Stopping is always safe. Handing back
writes, and writes need a live-world interlock.

## Session 30: the wrench reaches NO aim seam, and the post-FX rule was leaking the HUD

### 1. THE HEADLINE: melee runs neither fire-start seam. The aim substitution is innocent.

`docs/STATUS.md`'s leading hypothesis for the game-breaking wrench misses was that our origin
substitution moves the melee trace, because "the wrench IS an `AWeapon`". **Measured in-headset,
with the wrench actually equipped, and it is false.** The run (2026-07-30, live XR session, user
driving; wrench confirmed by `[aim] weapon profile 'Wrench' applied` at two separate swaps):

| seam | calls across the whole wrench period | classes seen |
|---|---|---|
| `AWeapon::GetPerfectFireStart` | **0** (the counter sat at 4 throughout - all four from a Shotgun test 15 minutes earlier) | `Shotgun` only |
| `UAttackAbility::GetPerfectFireStart` | 6 | `ElectricBoltThreeAbility` only |

Not one melee call at either seam. So the session-10 note in this file - *"the wrench does not
trace at all: `Wrench.CreateCollisionPhantom`, melee damage is a Havok phantom, so no aim seam
exists for it"* - is **CONFIRMED by direct measurement**, not merely asserted. It had never been
re-tested, and two other documents in the tree disagreed with it.

**Consequences that save real work:** `vraim seam weapon off` cannot affect the wrench.
`vraim origin off` cannot affect the wrench. Excluding melee from the origin substitution, which
was the planned fix, would have changed nothing. Anything that fixes the wrench has to act on
whatever positions the collision phantom, which is NOT a seam we hook today.

Remaining live candidates for the wrench, in the order they should be tested:

1. **The rig drive** (`vrhands off` A/B). The phantom must be positioned from something; if it
   rides the `AHands` actor or a rig bone, we rewrite that every frame. The A/B was armed but the
   session ended before a verdict - see section 7.
2. **Positional head/pawn decoupling.** We drive the CAMERA to the headset pose but transfer only
   YAW to the pawn (`body::on_calcview`); there is no positional transfer. Leaning in to hit
   something moves the view and the viewmodel but not the pawn, so a phantom spawned from the
   pawn transform would sit where the pawn is, not where the wrench is drawn. This fits every
   part of the report: worse in combat (more leaning and strafing), and present on the opening
   rocks (you lean in to smash them) with no combat at all.
3. **`SoftLockOnRadius 0`**, which we force at every world event. Cannot explain the rocks (they
   are not targets), so it is third. `vrlockon on|off|status` now exists for that A/B; note that
   `on` needs `vrpreset save` plus a RESTART, because a SET edit is memory-only and the stock
   radius is not readable back through the Exec seam.

### 2. Hand attribution in real play is ALWAYS the seam default, never learned

Every substituted plasmid cast in the run logged the same thing:

```
[aim] watch ability cls='ElectricBoltThreeAbility' this=50F59040 hand=L src=fallback lt=0 rt=0
      sub=1 origin=1 | engine=(-25069.7 4185.3 8275.3) ours=(-25064.0 4153.8 8241.0)
      d=46.9 UU (46.9 cm)
```

`lt=0 rt=0` **at the anim notify**, five casts out of five (`subsL=5 fallbacks=5 subsR=0`). The
notify that actually fires the ability arrives AFTER the player has released the trigger, so
`trigger_held()` (>= 64, ~25% pull) has nothing to see and `hand_for_object` returns the seam's
compile-time default. `g_objLeft` was never learned at all across the whole session.

The contrast is the discriminator: the Shotgun logs `rt=255 src=learned`, because a gun fires on
the trigger frame while a plasmid has a wind-up animation.

**This is not a live bug** - the ability seam's default is `Hand::Left` and plasmids are the left
hand, so the fallback lands correctly every time. It matters for two reasons. The object-learning
map is effectively dead code on that seam, and **anything else that ever arrives on the ability
seam would be attributed to the LEFT hand with the left trims (pitch -7.5, yaw +37.0 deg)**. That
was the mechanism behind the melee hypothesis, and it would have been real if melee had shown up
there. It did not.

### 3. Measured: we move the fire origin 40-47 cm

Every cast in the run, at `worldScale` 100 so UU == cm: `d = 46.9 / 42.5 / 44.0 / 40.2 / 40.3 UU`.
That is the distance between the engine's own fire start and the origin we write.

Worth keeping for any future short-range seam: at rifle range 45 cm of origin error is invisible,
which is why the substitution has looked correct since M6; at contact range it would be decisive.
This is exactly why the wrench theory was plausible, and why measuring first was the right call.

### 4. The post-FX size rule is DEGENERATE at a square render target - a shipped regression

`hud_capture.cpp`'s session-22 rule passed a post-tonemap draw in-frame when
`srv0 dims == target dims`. Its stated premise: post-FX samples a BACKBUFFER-SIZED texture while
gameswf samples 2048x2048 UI atlases. At the user's 2048x2048 square render **the premise
inverts** - the backbuffer IS 2048x2048, so the game's own UI atlases match the rule exactly.

Measured live at that resolution (`vrhud status` / `vrcine status`, session 30):

- `postFxRejected=1604161` against `postFx=2`. Two draws in the whole session were genuine
  post-FX; ~1.6 M were UI atlases the old rule would have passed in-frame - about **30 gameswf
  HUD draws per interval** (~9200/s at ~307 intervals/s).
- Restoring the old rule as a positive control (`vrcine postfx size`) gave
  `post-fx=83402/36140`: **43% of those draws were stranded onto the HUD panel by an earlier
  redirect and 57% reached the eye image.** So the old rule did not merely leak the HUD into the
  frame, it routed it **non-deterministically by draw order within the batch**.

Corroborated offline in `framedump_175024_q0` (2048x2048): the `T4` tonemap target and the `T6`
scene source are `bind=0x28` (RENDER_TARGET|SHADER_RESOURCE), while the atlases `T106` (fmt 71),
`T107` and `T113` (fmt 77) are `bind=0x8`, shader-resource only and BC-compressed. **The bind
flags are the resolution-independent discriminator**: a post-FX source is always something the
engine RENDERED, a UI atlas never is. That is now the test; `vrcine postfx size|rt` keeps the old
rule for a one-command A/B.

### 5. Where a PassThrough draw actually LANDS, and the theory it refuted

`PassThrough` is the absence of a routing instruction, and the redirect binds our RTV through the
ORIGINAL `OMSetRenderTargets` - so `hud::on_setrt` never sees it and `g_curRt` keeps naming the
game's target. The classifier can therefore believe a draw is in-frame while the device has the
HUD capture RT bound. Session 30 turned that prose contract (`hud_capture.h:22`) into counters.

**The instrument refuted the diagnosis it was built to test.** With the redirect armed, the effect
fills read `effect=127010/0` - passes/stranded. They are NOT stranded; they genuinely reach the
frame. Two checks make that trustworthy rather than a false negative:

- **The self-refutation clause fired and passed:** `[hud] stranded-pass PROOF: bound rtv0
  resource=92DB3E64, our capture RT=92DB3E64 - the substitution belief is CORRECT`. The flag is a
  faithful reading of the device, not a guess.
- **A positive control proved the counter can fire:** under `vrcine postfx size` the same run
  produced 36140 stranded post-FX passes.

So routing is not why full-screen effects do not cover the view. What remains is the fill's own
extent, or the projection layer's FOV claim (which is outside `hud_capture.cpp` entirely and which
flat testing cannot measure at all).

### 6. The effect is TWO textureless 5-vertex fills per interval, drawn EVERY interval

`effectsInFrame` advances at exactly 2 per present interval, continuously, in ordinary gameplay
with no effect visible. So the fill is a permanent gameswf element whose colour/alpha changes, not
a draw that appears when an effect starts. `note_textureless_count` logs each COUNT once, so the
second draw was invisible in the log and this was never apparent before. Confirms the
`framedump_175024_q0/q1` reading (events 00806 and 00807, both `a=5 srv0=T-1`) live.

`effectsOverBound=0` at the test location: nothing textureless above the 8-vertex bound occurred
there. The 1493-vertex textureless draw the census recorded earlier is elsewhere in the game, and
under the old residual rule ("textureless and not 29 verts") it was being rendered into the eye
image. The effect test is now a positive vertex-count bound.

### 7. `vraim scanimpl` arg count MUST equal the target's `ret imm / 4`, and getting it wrong is loud

Verified with capstone against the shipped exe (first `ret` in each implementation):

| implementation | RVA | `ret` imm | `scanimpl` args |
|---|---|---|---|
| `AWeapon::InitiateDamage` | `0x226050` | 8 | **2** |
| `UAttackAbility::InitiateDamage` | `0x1BBD80` | 8 | **2** |
| `AWeapon::GetPerfectFireStart` | `0x226840` | 0xC | 3 |
| `UAttackAbility::GetPerfectFireStart` | `0x1BC220` | 0x10 | 4 |

The last two match the shipped detours' signatures exactly, which is what validates the method.

Passing `1` for either `InitiateDamage` produced a **`Run-Time Check Failure #0 - The value of ESP
was not properly saved across a function call`** modal dialog: the detour returned with the stack
4 bytes off. Two things worth knowing about that failure mode:

- **It produces NO crash dump.** RTC is a Debug-build compiler check, not an SEH fault, so it
  never reaches the crash handler and `%LOCALAPPDATA%\BioshockVR\crash\` stays empty. In a Release
  build the same mistake would corrupt the stack silently instead - the same shape as the
  session-29 `write_n` lesson: a check that only exists in Debug is not a safety net.
- **Force-killing the game while that modal dialog is up leaves the display mode unrestored.** The
  desktop did not come back until the user reset it. Prefer Abort on the dialog over
  `Stop-Process -Force`.

### 8. Harness facts

- **Weapon switching still cannot be driven flat.** All four D-pad directions were tried
  (`vrinput test press DU|DD|DL|DR`); `vraim weapon` reported `active='Shotgun'` throughout. The
  radial needs a human, so any per-weapon measurement needs the user to equip first.
- `vrinput test trig r|l 255 400` DOES drive a shot, and two pulls ~2 s apart are needed (the
  first switches hands - `XENON_RT = SwitchAndFireWeapon`).
- **`vraim probe on` + `vraim off` is a genuinely read-only measurement mode** and it held for a
  whole live play session: hooks installed, `ray_for()` refusing, `subs=0 skips=N`. Note the trap
  it does NOT survive: **pressing VR PRESET 1 re-arms `aim on` and `origin on`**
  (`camera.cpp:862-864`), which is what turned substitution back on mid-run here. Any read-only
  measurement has to be re-armed after a preset press.

## Session 30 part 2: THE WRENCH IS FIXED - the engine's own view pitch was frozen

Part 1 above closed the aim-seam lane by measurement (melee reaches neither fire-start seam).
This is the actual cause, found and fixed in-headset the same session.

### 1. The mechanism: two things that were each individually reasonable

**(a) Pitch kill freezes the engine's pitch rather than setting it.** Session 19 zeroes the
composed right-stick Y while the VR camera drives a gameplay view, so the stick cannot fight the
HMD for pitch. Its own comment already named the stake: *"a stick-pitched body drags the viewmodel
with it and aims the wrench's melee phantom at the body pitch instead of the hand."* But zeroing
an INPUT does not set the value - it means the engine's own view pitch can never change again.

**(b) The camera write is asymmetric, and only one half was ever noticed.**
`camera.cpp` (session 30 line numbers):

```cpp
int32_t gameYawUnits = rot->yaw;
rot->pitch = a.pitchRad * kRotUnitsPerRadian;   // ABSOLUTE from the head
rot->roll  = a.rollRad  * kRotUnitsPerRadian;   // ABSOLUTE from the head
rot->yaw   = gameYawUnits + residualUnits;      // RELATIVE to the engine's own
```

Yaw keeps the engine's value and adds the head residual on top, so the engine's yaw stays real.
Pitch DISCARDS the engine's value. Nothing reads it, nothing corrects it, and the rendered view is
the head's either way - so a wrong value there is completely invisible.

Together: the engine's view pitch parks at whatever it last held and stays there for the session,
and anything the engine aims for ITSELF uses that stale number.

### 2. The measurement

The `[b1r] camera:` heartbeat prints `rot` BEFORE the write above, so it reports the engine's own
belief. In-headset, over fifty seconds while the user turned freely:

```
rot=(49350 22991 0) -> (49350 32820 0) -> (49350 42651 0) -> (49350 53290 0)
```

Yaw moving, **pitch pinned at 49350 = -88.9 degrees. Straight down.**

Corroborated by the user with `vrhands off`, which returns the viewmodel to engine placement and
therefore makes the engine's aim visible: *"this revealed that the hands were pointing downwards -
hitting the wall it was hitting straight, but when I entered a fight it was hitting downwards ...
I saw the hits hitting the floor when I looked down while hitting with the hammer in a fight."*
The occasional kill was catching a leg on the way down.

Every part of the original report follows:

- **Wall:** you approach it facing level, so a frozen-near-level pitch connects.
- **In a fight:** the frozen value was steeply down, so swings went into the floor.
- **The opening rocks other players reported, with no combat:** rocks are on the floor. You look
  down at them.
- **Guns unaffected:** we substitute the whole fire ray, pitch included, at a seam melee lacks.

### 3. Two hypotheses this killed, both of them ours

- **Soft lock-on.** The user's own leading theory, tested by setting the radius ABSURDLY high
  instead of to zero: `exece set GamepadPlayerInput SoftLockOnRadius 5000` produced no perceptible
  difference from 0. So that write never reaches the live object and the whole lock-on suppression
  has been a no-op. **Note for any future engine SET: `-> HANDLED` proves only that `Exec`
  recognised the command.** `console_exec`'s FOutputDevice stub returns 0 from the engine's log
  filter specifically to suppress output, so a `set` that names a wrong class or property, or that
  writes a class default the live object never re-reads, logs exactly the same line as one that
  works. The reticle SET happens to work; do not generalise from it.
- **Positional head/pawn decoupling** (part 1's candidate 2). Right neighbourhood, wrong axis. It
  is pitch, not position, and the mechanism is ours rather than the engine's.

### 4. The fix: servo the pitch through the game's own input

Instead of `out.ry = 0`, feed a proportional term that steers the engine's pitch toward the
head's. The game layer publishes `head pitch - engine pitch` once per CalcView **before** the
overwrite (one line later the error is identically zero, which is exactly why nobody saw this),
and `xinput_bridge` turns it into a stick value.

Why this shape rather than writing the rotation directly:

- **It writes no engine memory.** None of the session-29 world-change or recycled-heap hazards
  apply, because the value travels through the game's own input path.
- **It inherits the game's own pitch clamps** instead of needing its own.
- **It is invisible.** The rendered pitch is overwritten from the head regardless, so the head
  still owns what you see; only the engine's internal belief catches up.
- **It fails open.** A stale publisher (world unload, drive off, menu) reverts to `ry = 0`, the
  pre-session-30 behaviour, with no special case.

Guards: 1.5 deg deadzone so the stick stops chattering once converged (a permanently nonzero look
axis is the kind of thing a game reads as "the player is looking around"); gain capped at ~24%
deflection so it can never out-run a real player's look, and so a wrong SIGN saturates somewhere
recoverable rather than slamming to the clamp. `vrinput pitchservo on|off|invert|status`.

**In-headset verdict: "it's working and I was able to hit him consistently."** Measured after:
the engine pitch moved from -88.9 deg to **-6.6 deg**.

### 5. Known residual, measured - do not treat this as exact

`err=4.3 deg stick=3876` at steady state. The servo converges to within a few degrees and then
stalls, because near convergence the proportional stick value falls under the **game's own** stick
deadzone and stops moving anything. So expect a residual of roughly 4-8 degrees, not zero. That is
inside melee tolerance (confirmed by consistent hits) but it is not perfect.

Closing it needs a minimum stick magnitude that clears the game's deadzone, which risks a limit
cycle - overshoot, reverse, buzz. Measure the game's actual deadzone before adding one.

### 6. The health and EVE bar fills are not full-screen effects

Session 29 routed textureless gameswf fills in-frame. Confirmed in-headset that this also sends
the **health and EVE bar COLOUR fills** into the eye image while their frames stay on the panel,
so the bars read as empty. They are textureless 5-vertex gameswf quads - identical to the effect
fill by every test the classifier can apply. A/B'd both ways by the user: untick restores the
colour, re-tick removes it.

**The counter had been saying so since the change shipped and was misread.** `effectsInFrame`
advances by exactly 2 per interval, every interval, with nothing on screen. Two bars. Part 1 of
this session recorded that as "the fill is always drawn and usually transparent".

And the user's description of the effect - *"either the size of the HUD or the size of the old
resolution"* - identifies the deeper error. **These draws are authored in gameswf STAGE space.**
Routing one in-frame cannot make it cover the eye; it makes it stage-sized in the middle of the
view. So session 29's fix could never have worked, and the open "effects are not full-screen" item
needs different GEOMETRY, not a different render target. Default is back to the panel.

## Session 30 part 3: the classifier hardening's own fallout, and three wrong diagnoses

Part 1 hardened the post-FX rule; part 2 fixed the wrench and the bar colour. This is what the
hardening then broke, how it was chased, and what is still not understood. It is written at
length because the process failed repeatedly here and the failures are the useful part.

### 1. A floating screen in cutscenes, and a WORKAROUND rather than a fix

**Symptom** (user, in-headset): during the first-plasmid cinematic, once subtitles appear, a
screen floats in the middle of the view showing the same scene that is playing behind it, with
the subtitles on it. Enabling `vrcine subs frame` removed it (and made subtitles unreadable).
`vrcine postfx size` also removed it, with subtitles still fine.

**What shipped:** while `cinematic_hold()` is true, the post-FX rule falls back to SIZE-only; the
bind-flag rule returns the moment the scene releases. `vrcine postfx cine on|off`, default on.

**Why that scoping is defensible rather than arbitrary:** the bind-flag rule exists to keep
gameswf HUD art out of the eye image, which at a square render target is worth ~30 draws per
interval. A cutscene has essentially no HUD art, so the rule buys almost nothing there while
costing a visible defect. The gate is the bar draw, the most reliable edge in the mod - measured
firing at 00:06:32.211 and releasing at 00:08:22.216, with the exception logging inside that
window. Verified in-headset.

**It is still a workaround and the code says so.** We know the fallback removes the screen and we
know why the scoping is safe. We do NOT know which draw the user was seeing or why it lands on the
panel. **The missing measurement is a frame dump taken INSIDE the scene** - the one attempt landed
1.1 s early (dump at 23:59:49.704, `bar draw ON` at 23:59:50.798) and captured ordinary gameplay.

**Next instrument, and it is small:** auto-fire `frame_inspector::arm(2, 2)` on the
`bar_draw_active()` rising edge. Session 29 used exactly this and it is why that session got a
dump inside a letterbox. `arm()` has only two call sites today (the two adapters' `dumpframe`
commands), so this is additive.

### 2. NEVER take a reference to an engine D3D object from inside a detour

**This crashed a Release build on the loading screen, twice, with two DIFFERENT d3d11 faults**
(`d3d11+0x19B7EB` reading `[null+0xE5]` with `ecx=0`, then `d3d11+0x78F54` reading `[null+0x38]`
with `eax=0`), both immediately after `screen-only interval ON ... world pass absent`.

The cause was a four-line "safety" measure: the stranded-pass restore needs the game's RTV/DSV,
and the first version `AddRef`'d both on the reasoning that the game might release a view between
its `SetRT` and the draw. That reasoning was wrong twice over:

- **The game holds its own reference for its own binding**, for exactly as long as it is bound.
  There was nothing to guard against in the window the pointers are used - same thread, same
  gameswf batch, a few draw calls apart.
- **`hud_capture.cpp` already states the rule and had followed it everywhere else.** `g_curRt` is
  stored with `res->Release(); // identity only from here on`. Taking references to engine objects
  from a detour that runs ~18 million times a session, while a level load destroys and recreates
  render targets, is a far larger hazard than the one being guarded against.

Removing the refcounting entirely (raw identity pointers, nulled in `on_setrt` and
`release_resources`) fixed it. **Same shape as the session-29 `write_n` lesson: a safety measure
that creates a worse failure than the one it prevents.** Two of those in two sessions - when
adding a guard, ask what the guard itself can break.

### 3. The stranded-pass restore: shipped, measured, and NOT the cutscene fix

`PassThrough` is the absence of a routing instruction, so a pass issued after a redirect lands on
the capture RT because the redirect goes through the ORIGINAL `OMSetRenderTargets` and the game
does not rebind until the batch ends. `hud_capture.h` had stated that contract as prose since
session 19; it is now enforced. The classifier hands the game's binding back before a pass that
would otherwise be stranded, lazily (on the transition, ~28k times a session) rather than after
every redirect (~10M times).

Blend state deliberately is NOT restored: the alpha-corrected variant differs from the game's
state only in the alpha ops and the alpha write mask, so a passed-through draw renders identical
RGB. Verified by reading `fix_blend_alpha`, not assumed.

Measured working: `routes: tonemap=11010/0 post-fx=3798/0 unarmed=2340/0` - stranded zero in every
bucket with it on. It fixes a real defect. **It did not fix the cutscene screen**, which is how we
learned the cutscene draw is in the REJECTED population, not the stranded one.

### 4. Three wrong diagnoses in a row, and what actually caused each

Worth recording because the pattern is more instructive than any single error.

| # | Claim | Why it was wrong |
|---|---|---|
| 1 | The cutscene draw's source is backbuffer-sized but not a render target, so the bind test rejects it | Right in shape, never confirmed - no dump from inside the scene exists |
| 2 | It is a stranded pass; the restore will fix it | Read `post-fx=41124/28184` as covering the offending draw. That counter describes draws that PASSED the test; the offending one is in `postFxRejected`, a different population. **Two counters on one line, and the wrong one was read.** |
| 3 | The crash test isolates my change | The lever gated only the CONSUMER of the stored views. The `AddRef`/`Release` ran regardless, so the "controlled" build changed nothing relevant and cost two loads. **Before claiming one variable, check which code the variable actually reaches.** |

The one that generalises: **a counter is not evidence until you know which population it counts.**
The stranded/rejected split was designed into the instrument deliberately and then misread by its
own author within the hour.

### 5. What is verified and what is not, going into the release

VERIFIED IN-HEADSET: the wrench fix, the health/EVE bar colour, the alcohol blur unaffected by the
post-FX change, the cutscene screen gone with subtitles readable, no crash on load.

NOT VERIFIED: a full playthrough. Every fix in this session was accepted on a single check, and
two of tonight's three regressions were found by the user playing rather than by any check we ran.
The classifier changes touch every HUD frame in the game, so menus, the hack minigame, vending
machines, the Gatherer's Garden and Big Daddy FMVs are all untested against them.

## Session 31: what a synthetic trigger actually does, measured

Built for the wrench swing gesture (`core/input/swing.{h,cpp}`); all of it flat, no headset.

### 1. A 120 ms synthetic RT pulse FIRES the weapon - the "first pull only switches hands"
### caveat does not apply once the weapon hand is already active

Session 10 recorded `XENON_RT = SwitchAndFireWeapon`: the first trigger pull switches hands
("SwitchToWeapons") and only the second fires, and session 30's harness notes used
`vrinput test trig r 255 400` accordingly. Measured 2026-07-31 with the pistol equipped in
gameplay, hands already on weapons: **two consecutive `vrinput test trig r 255 120` commands
took the ammo counter 6 -> 4.** Two pulses, two shots, no swallowed first pull. So 120 ms is
enough to be seen by the game's per-tick edge detection (it sits between the 150 ms already
proven by the ammo flick and the START pulse), and the hand-switch caveat is about WHICH HAND
IS RAISED, not about the first pulse of a session.

### 2. The synthetic pad reaches the engine's own fire path, timed

Every `[swing] FIRE` line was followed **8-11 ms later** by `[aim] watch weapon ... rt=255` -
the mod's own fire-seam instrument seeing the engine run a real weapon fire. Five fires, five
matching seam events, no misses. That is the whole chain measured end to end: detector ->
pulse deadline -> `compose_synthetic` -> the game's XInput poll -> `UWindowsViewport::UpdateInput`
-> the engine's fire flow.

### 3. `vraim wkey sim <Class>` doubles as a gate-forcing seam for flat tests

Weapon switching still cannot be driven flat (session 30 part 1: all four D-pad directions
tried, the radial needs a human). But the swing gate reads the per-weapon PROFILE KEY, and
`vraim wkey sim Wrench` sets that key directly - so the gate can be forced open with a pistol
in hand and the pulse's effect observed on the ammo counter. `vraim wkey real` restores live
tracking. This makes every gate except "the wrench is really equipped" testable without a
human, and it is the reason the whole detector could be verified flat.

### 4. Harness trap, bitten again: a stale `command.txt` re-applies at boot

Documented since session 7, and it produced a false negative here. A `vrpreset save` left as
the last line of `command.txt` re-ran on the next launch - at the menu, before the preset load -
and overwrote the tuned ini with startup DEFAULTS. The persistence load path looked broken and
was not. **Clear `command.txt` before closing the game, not only before launching it**, whenever
the last command wrote a file.

## Session 61 (2026-08-14) - viewmodel scale SOLVED: the untested cell + the weapon's own skeleton

**BS1 now has working hand AND weapon scale sliders. Two lanes, both flat-proven
in the simulator with A-B-A round trips; no new engine offsets were needed.**

### 1. The s16 "chain scale is fatal" verdict was CONFOUNDED - the working cluster lever

The s16 dead-end test (part 2 above) scaled bones 27-42 and still WROTE bone
43's `.s` at its authored value. BS2's later live bisection (its ENGINE_NOTES
"the inverse-scaled ammo canister") localised the identical blowup to the
attach pivot's own scale CHANNEL being inverse-decomposed by the attachment
math - and BS1 never tested "scale the chain, never touch the pivot's channel".
That cell is the lever:

- **Recipe (bones.cpp drive loop): anchor-relative translations scale by s for
  EVERY cluster bone** (attach 43 + muzzle 44 MOVE with the shrunk hand; the
  right anchor IS bone 43, so the hand scales about the grip), **and the
  hkQsTransform `.s` channel is written only for bones 27-42 (right) / 6-21
  (left), NEVER for 43/44** - their channel stays engine-owned in every mode.
- **Flat proof (sim, vrstereo on, drive on, pistol equipped)**: at
  `vrhands scale r 0.5` the hand halves and the ATTACHED PISTOL IS UNCHANGED
  (img-diff strong cells localised to the hand region; weapon silhouette
  intact, zoomed crop crisp). Monotonic the other way at 2.0 (giant hand,
  authored pistol). Left cluster (plasmid hand) same result.
- **Anchor pinned**: `vrhands status` last write loc identical to 0.00 UU
  across 1.0/0.5/1.0 - scale is about the anchor, aim/laser untouched (the
  muzzle ray normalises bones 43-44, whose relative direction survives
  uniform position scaling).
- **A-B-A exact**: round-trip diff 0.21% channels changed vs a 0.75% ambient
  floor (animated light shafts), zero strong cells.
- **The engine does NOT restamp the scale channel** (counter in
  `vrbones status`: 0 across every run - BS2 behaviour, not Infinite's), so
  the drive PINS the scale rows of scale-written bones at reference
  recapture (never re-adopts its own write; adopting would compound
  refS * s^n) and hands the authored `.s` back explicitly on the off edge
  (scale->1.0, mode change, release, hide) - the sleeve-collapse lesson.
- Probe-mode bisection machinery stays in the tree (`vrbones scalemode 0..3`:
  cluster-sans-43/44 / fingers-only / wrist-only / translation-only) - mode 0
  is the ship mode; 1-3 were never needed since mode 0 passed outright.

### 2. Uniform weapon scale: drive the holdable's OWN SkeletonInstance (wskel lane)

BS2 session-41's shipped design ports cleanly because BS1 weapons have carried
their own skeleton all along (s20 - it is how the muzzle bone was found):

- **Resolution**: `hands::current_holdable()` (raw class-agnostic +0x45C read;
  vtable-gating pins a stale weapon across switches - the s21p3 class trap,
  re-confirmed live this session when `weapon_actor()` kept reporting the
  holstered pistol while the wrench was equipped) -> existing `resolve_skel()`
  at the SAME `+0x3FC` slot as the rig (pistol: 8 bones - R_Grip at the
  component origin, pistol_body, hammer, Trigger, Barrel, over, DrumParent,
  drum). No vtable scan needed, unlike BS2's +0x430 hunt.
- **Compose, per frame**: translations *= ws (uniform about the component
  origin = the grip), quats ADOPTED from the engine (32-byte compare per bone;
  drum/recoil animations keep playing while scaled - 1.7 adopts/drive
  measured), `.s` = captured reference * ws (pinned, never adopted). Dirty
  flag cleared after the write, replayed on the stereo second pass from
  `reapply()`.
- **At ws == 1.0 the lane drops entirely**: captured pose written back,
  dirty flag handed back, zero interference at the default. Same explicit
  restore on weapon switch, world change, and rig release (cinematics) -
  the engine never restamps scale, so "just stop writing" would leave the
  gun scaled forever.
- **Flat proof**: `vrhands wscale 0.5` halves the pistol uniformly about the
  grip - body, drum, hammer together, NO inverse-scaled part (BS2's ammo-
  canister trap does not exist in this lane by construction). Numeric A-B-A:
  the post-release bank re-dumps the pre-scale bone positions to the last
  digit. 1390 drives at 0.5 rendered exactly half - no compounding.
- **The WRENCH has no skeleton**: holdable +0x3FC is NULL (flat-proven by
  hexdump; no SkeletonInstance vtable nearby) - it is a rigid melee mesh.
  The lane negative-caches the failing holdable (1 s retry so a transient
  mid-equip failure self-heals) and logs the verdict once per holdable.
  Wrench renders authored size; its HAND still scales via lane 1. Expect the
  same for any future rigid holdable.
- **Weapon switching**: drop-and-rebind proven live (pistol -> wrench -> 
  pistol via `game-key` scancode 1/2 - note s30's "weapon switching cannot be
  driven flat" predates the s35 scancode key lane and no longer holds for
  number-key selection).

### 3. Surfaces

`vrhands scale [l|r|both] <f>`, `vrhands wscale <f>`, F10 sliders in the
hands section (model scale 0.2-4.0 per tuning hand + "scale both hands",
weapon scale 0.3-2.5), preset keys `handScaleL`/`handScaleR`/`wScale`
(save/load round-trip verified through a cold boot: 47 values, scales live
and wskel auto-bound at the loaded value). Ship defaults are 1.0 - the
calibration numbers are the user's to set in-headset (BS2's 0.771/0.760/0.770
are BS2-rig numbers and were deliberately not copied).

Routes NOT taken this session, for the record: DrawScale re-test on the new
fg path (route B - unnecessary, the cluster lever passed first) and the fovA
consumer hunt (route A - stays parked with its in-headset world-coupling
negative). `kActorDrawScaleOffset`/dirty-protocol constants remain declared,
still unreferenced.

## The Flash movie stack - WHICH interface screen is up, by name

**Ported from BRVR, 2026-08-21.** Consumed by `src/game/bioshock1r/screens.cpp`
and published into the cinematic verdict via `bvr::vr::publish_ui_pause`.

### Why a render-side signal cannot answer this

The pause menu and the machine flows draw OVER a live world, which defeats every
signal the verdict had. CalcView keeps firing, so it is not stale. The view actor
is still the player pawn, so it is still strict. The game renders and claims the
same fov. The frame is not pure gameswf, so `screen_only()` is false. The main
menu and loading screens moved to the anchored cinema quad while the pause menu
stayed on the head-locked HUD panel.

### The chain

The whole interface is a stack of named Flash movies. The engine exposes
`GetTopPlayingMovie`, which BRVR decoded by eye rather than calling (it is an
exec native taking an FFrame). Both getters are pure field walks:

```
GetFlashGUIController, ecx = LevelInfo:
    mov eax,[ecx+0xFC]      ; XLevel
    mov eax,[eax+0x5C]
    mov eax,[eax+0x4C]
    cmp [eax+0x48],0        ; TArray ArrayNum - the getter bails when zero
    mov eax,[eax+0x44]      ; TArray Data
    mov eax,[eax]           ; data[0]
    mov eax,[eax+0x7C]      ; the controller

GetTopPlayingMovie, ecx = FlashGUIController:
    mov edx,[ecx+0x15C]     ; count
    mov eax,[ecx+0x158]     ; array data
    mov ecx,[eax+edx*4-4]   ; data[count-1] == the TOP
```

The movie's filename is a **string** at `+0x40` on the movie object, so it needs
no name table. `+0x4C` also carries it but degrades to `"NoFileSpecified"` on
some entries; `+0x40` held on every sample.

**`AActor::Level` is derived at runtime, not assumed.** The controller and the
pawn must agree on the offset, both must point at the same object, and that
object's own copy must point at **itself**. Nothing but LevelInfo passes all
three. Measured `+0xF8` on this build, but the code never trusts that literal.

### Measured, first run of the probe, 2026-08-21

| Top movie | Meaning |
|---|---|
| `HUDRadial.swf` | ordinary gameplay |
| `PausePC.swf` | the pause menu |
| `craftingstation.swf` | a vending machine |
| `Fadeout.swf` | a loading transition |

Pause, vending and hacking screens were all confirmed anchoring in a headset the
same day.

### Fail-closed behaviour

- The LevelInfo self-reference is re-proved every frame, so a level change
  re-finds rather than depending on a reset hook nobody remembers to call.
- Array data is checked as a **buffer**, not an object. This is not pedantry: the
  object test demands a vtable whose first entry is executable, and a TArray
  buffer's first word is element 0, whose own first word is a vtable POINTER
  living in read-only data. Running the object test there rejects a correct
  pointer every time, and it cost BRVR a session. Fail closed is right; failing
  closed on the wrong predicate is not.
- The walk reports **which step** it stopped at. A six-deep chain decoded from
  instructions has six ways to be wrong and "did not resolve" distinguishes none
  of them. That cost BRVR a session too.
- An unreadable name leaves the previous verdict standing rather than dropping
  the panel out from under a screen the player is reading.
- If the chain never resolves, placement behaves exactly as it did before.

### FALSIFIED: LevelInfo::Pauser

The first attempt used `Level+0x668`, which BRVR recorded as the pause field and
its source called "the one reliable signal". **It read null through three real
pauses on this build** and is not used.

That offset was never a measured result in BRVR either. Its detector shared one
dependency with two other features (`FindLevel`, which needs the pawn) and all
three were silent for a whole 16-minute session with their switches on. The
comment claiming reliability outlived the evidence for it.


**RE-TESTED PROPERLY 2026-08-24, and now falsified on evidence rather than on a
borrowed number.** The original test above used BRVR's `0x668` - a copied
offset, which the hard rules forbid for exactly this reason - so it could only
ever have shown that ONE slot was wrong. BRVR did not guess that number either;
it hunted it (`GameState.cpp HuntPauser`), and `0x668` is what its hunt returned
on its build.

The same hunt now exists here (`screens.cpp`, always-on, rides the panel edges)
and was run against a real pause in ordinary gameplay:

| Sweep | Filter | Result |
|---|---|---|
| `Level+0x0..0x1000` | slots going `null -> object` (the Pauser shape) | **nothing** |
| `Level+0x0..0x2000` | ANY slot that changes | 32 slots, **none of them a flag** |

Every one of those 32 is a `TArray` growing by one element - they arrive in
triples, `{Data, ArrayNum, ArrayMax}`, with the two counts stepping together:

```
Level+0x1560 changed (8ADC5F30 -> 8B34B0D0)
Level+0x1564 changed (00000015 -> 00000016)     ArrayNum
Level+0x1568 changed (00000015 -> 00000016)     ArrayMax
```

That is menu allocation churn, not a pause flag. **The pause state is not on
LevelInfo on this build at any shape, within 8 KB.** Do not try `Pauser` again
without first widening the sweep or pointing the hunt at a different object -
the PlayerController is the obvious next candidate and has not been swept.

**What is used instead: the composed pad.** The mod composes the gamepad, so it
knows when the PLAYER pressed menu; the game's own `PausePC` during a ride or a
scripted scene arrives with no press at all. `camera.cpp` latches on the press
and holds it until the panel closes, and that is what lets a real pause taken
mid-scene keep the anchored quad while the scene's own panel does not. It fails
towards today's behaviour: a pause opened by keyboard Escape misses the window
and behaves exactly as before rather than flattening a ride.

**Also falsified in BRVR, as sway fixes:** `AdditiveHandBobAnim` and
`WeaponBobDamping`, both observed inert. Do not revive either.

### Rejected alternative, on the record

BRVR first tried a **draw-signature heuristic**: in-game menus issue 20-31
non-indexed draws per frame against gameplay's 120+, because menus hide the HUD.
The gap looked clean at 4x with no overlap across seven captures. In practice it
**flipped twelve times in one spot during ordinary gameplay**.

## Session 63 (2026-08-22) - the rigid-holdable DrawScale lane, and what it inherits

**Status: HEADSET-CONFIRMED 2026-08-22 - the wrench visibly changed size.**
That settles a question open since session 16; see "the split", below.

### The gap

`bones.cpp`'s weapon-scale lane (`wskel_*`, session 61) drives the equipped
holdable's own `SkeletonInstance`. The **wrench has none** - actor `+0x3FC` is
null, it is a rigid melee mesh (flat-proven 2026-08-14) - so the lane logged
`holdable ... has no resolvable SkeletonInstance (+0x3FC) ... lane stays
unbound` and the wrench rendered at authored size no matter what `wScale` said.
BRVR never had the problem: it scales the weapon **actor** (`GunScale` ->
`DrawScale`), which needs no bones.

### The lane

Only where the skeleton lane cannot bind. A holdable WITH a skeleton keeps the
existing path; carrying both would compound.

- Field: `kActorDrawScaleOffset` +0x2AC, with the **dirty protocol** -
  `[actor+0xD0] |= 0x10`, `[actor+0x3F4]++`, `[actor+0x3E4] = 0`. All four
  constants were derived here in session 12 (AActor::SetDrawScale 0x375830
  disassembled, field poked live); nothing was carried across from BRVR.
  A raw poke without the revision bump is invisible - see the session-12 entry.
- `wScale` is a MULTIPLIER on the captured authored value, matching the sibling
  lane. BRVR writes its `GunScale` absolutely; for BS1 weapons the authored
  value is 1.0, so the two agree, and the captured value is logged so a weapon
  where it is not shows up rather than hides.
- **A field reading 0.0 is expected**, not garbage: session 12 recorded "the
  AHands actor read 0.0 there", which is Unreal's unset-means-one. It binds as
  a 1.0 multiplier while the raw 0.0 is kept for the restore.
- Re-asserts per frame (read-then-write, so zero writes when nothing moved it),
  because the engine restamps `DrawScale` on equip and a one-shot poke was
  BS2-proven not to render reliably.
- **Restores only when the actor is provably live** - on `wScale` returning to
  1.0, and on explicit release. A holdable CHANGE and a world change both
  forget it WITHOUT writing. That is BRVR's `ArmHide_Reset` rule and it exists
  for the same reason: by the time the change is noticed the old actor may be
  destroyed and its address reused, and an SEH guard does not save you from a
  VALID write into somebody else's object.

### The split: the RIG actor's DrawScale is inert, the WEAPON actor's is not

Session 16 measured `DrawScale` on the **RIG** actor and found the geometry
inert through the foreground path - **gun width 240 -> 234 px at s=0.5**, 2%
where 50% was asked - concluding that the fg rig path consumes actor DrawScale
for bone translations but NOT for skin/attached-mesh size. It named the
**WEAPON** actor as the case it could not isolate: "it could at best scale the
gun, never the hand."

**That guess was right, and the split is real.** The wrench visibly resized in a
headset on 2026-08-22. So: rig-actor DrawScale does not size geometry; weapon-
actor DrawScale does. The attach-matrix disassembly session 16 queued as the
fallback (`AActor::AttachToBone` 0x379EF0 / the fg bake at 0x3DBF7C) is **not
needed for weapon size** and can stay parked.

### Two things the first headset run corrected

**1. The wrench is authored at DrawScale 0.800, not 1.0.** Logged as
`wscale rigid: bound 7B912A20, authored DrawScale 0.800`. The lane's first cut
treated `wScale` as a MULTIPLIER on the authored value on the explicit
assumption that BS1 weapons are authored at 1.0 - so `wScale 0.80` rendered the
wrench at **0.64** and it read too small. It is now **absolute**, which is what
BRVR does (`*p = g_cfg.gunScale`) and which makes `GunScale=0.8` mean the same
size in both mods. The consequence to know: `wScale` is "fraction of authored"
on the skeleton lane and "the DrawScale itself" here. That asymmetry is BRVR's
too, and BRVR is the size that was accepted in a headset.

**2. A weapon SWITCH reached the restore path.** Logged as
`released 7B912A20 (holdable has a skeleton after all) - DrawScale restored to
0.800`: the wrench went out of hand, a skeletal weapon bound, and the lane
restored through the OUTGOING actor - exactly the stale-pointer case `ds_drop`'s
`restore` flag exists to refuse. That branch now checks the bound actor is still
`hands::current_holdable` before writing.

### How to verify a change here

The wrench must be EQUIPPED. `boot.ps1 -Attach` lands on the newest save, and
that save is the bathysphere intro ("Pick up the RADIO") with no weapon, so the
simulator cannot reach it without a later save or scripted navigation - which is
why this went to a headset. Once equipped, the log lines above are the evidence:
`bound <ptr>, authored DrawScale <x>` then `DrawScale <x> -> <ws>`, and a
hand-back line on unequip.

### Ini keys added the same session

`HandsScale`, `GunScale` (0.05-5.0) and `CameraHeightOffset` (cm, + up) in
`BioshockVR.ini`'s `[VR]` section, read in `Bioshock1RAdapter::init`. BRVR's key
names, units and defaults (0.8 / 0.8 / 9), so a config carries between the two
mods. **A saved `vrpreset.ini` loads later and overrides all three** - the
startup echo says so, because in a headset that is indistinguishable from the
ini being ignored.

## The d-pad must be HELD, not pulsed - and that is what gates the MAP

**Ported from BRVR 2026-08-22, after the modifier "did not work" in a headset.**

BRVR shipped a 120 ms pulse first and had to undo it. Its own note
(`BioshockVR/Input/InputHook.cpp`, its session 38):

> HELD, not pulsed. The first version emitted a 120ms pulse to avoid
> weapon-switch spam -- but weapons are on the RADIAL in this game, and the
> d-pad drives HUD functions. One of those is the hint button, and
> `ShockPlayerController` gates the MAP SCREEN behind `HintButtonHeld` with
> `HintHoldTime=0.5s`. A pulse made the map unreachable by construction.

This mod was in exactly that state until now: `kFlickPulseMs = 150`, so **the
map screen could not be opened at all** through the modifier, no matter how the
gesture was performed. Fixed by holding the bit for as long as the direction is
held (`PadMap::flickHold`).

**But holding is only safe on the direction that needs it, and the first cut of
this got that wrong.** It made hold-vs-pulse a per-GAME switch on the theory
that BS1's directions SELECT a slot. The pad audit above says otherwise with
better evidence - UP/DOWN **cycle** (`00 Buck -> Electric Buck -> Exploding
Buck`) - and a held cycle spins rather than settling. So the switch is
per-DIRECTION (`PadMap::flickHoldBits`): RIGHT (hints) is held, everything else
is pulsed, and Infinite holds nothing.

**THERE ARE TWO ROUTES TO THE MAP, and the one BRVR actually ships is the MENU
button, not the d-pad.** BRVR `InputHook.cpp`:
`if (s.menu) btn |= (mod ? XI_BACK : XI_START);` - the MODIFIER changes what the
menu button MEANS. Menu alone is START (pause); modifier + menu is BACK, which
is `ShowContextHelp` ("WHAT IS THIS?"), and holding it past `HintHoldTime` opens
the map. The same applies to the X+Y chord that stands in for the menu button on
setups where the runtime eats it.

This mod had BACK on the menu button's LONG PRESS instead, with no modifier
involved - which put context help behind the one input most likely not to reach
the game at all, and left the X+Y chord able to produce only START. Now
modifier-gated, with the long press kept as a fallback for `dpadModifier = 0`.

**The d-pad route, which is real but secondary: `flickRight` was 0.** BS1's map emitted only UP/DOWN/LEFT, on the belief that
three directions covered its three ammo types. But `DPAD_RIGHT = hints` - so
holding right emitted **nothing at all**, and no amount of hold could reach the
map. Both halves are needed: the bit has to be sent, and it has to be held.

Two smaller corrections landed with it, both BRVR's numbers: the select
threshold is **0.5** (was 0.65), and the direction is **dominant-axis only**
(`ay >= 0.5 && ay >= ax` -> up/down, else `ax >= 0.5 && ax > ay` -> left/right).
The previous first-match chain could resolve a deliberate "up" as "left"
whenever the cross axis also happened to be over threshold.

### What the modifier failure actually was

Worth recording because the config was never wrong. The log echoed
`d-pad modifier = rightrest` at startup and `RIGHT thumbrest touch reported by
the runtime` 100 s later, so both ends worked. What went wrong was **the
controllers repeatedly unbinding mid-session** - see STATUS. Before suspecting
the modifier again, check the census timestamps.

## Session 64 (2026-08-22) - scripted EVENTS: three offsets ported from BRVR, and what re-derives them

**Source: the BRVR mod** (`docs/brvr-reference/BioshockVR/Game/GameState.cpp`,
its M7-S1/S4/S5/S6 banners). Everything below is BRVR's measurement, cited so it
is not re-derived from scratch, plus the checks this tree adds because a
carried-over offset is a guess until this build agrees with it.

**These are not cutscene signals.** The cutscene detector here is `wantCine` in
`src/core/vr/openxr_runtime.cpp` and it is already the stronger of the two mods'
- BRVR's own `docs/CONSOLIDATION.md` records that BRVR shipped exactly one
cutscene signal (the letterbox bar draw, `DrawHook_CutsceneBarsActive()`) and
**never wired a caller to it**, while that same draw is `bar_draw_active()` here,
consumed, and combined with four signals BRVR has none of. Nothing in the
cutscene path was touched. What was missing here is the *scripted sequence*
question: the world renders, the HUD may be up, and the game is moving the player
through an authored moment.

### hands+0x594 bit 2 - CurrentlyExecutingScriptedHandAnimationSequence

`Hands.uc` lines 80-82 are three consecutive bools, and UE2 packs consecutive
bools into one DWORD:

| bit | field |
|---|---|
| 0 | `bFinishedStateAnimations` |
| 1 | `AbilityHasBeenReleased` |
| 2 | `CurrentlyExecutingScriptedHandAnimationSequence` |

BRVR computed the DWORD's address by walking the field list from its proven
`+0x494`/`+0x498` anchor. **The walk is the weak link, not the bit.**

**MEASURED, BRVR M7-S1, 2026-08-10:** the bit set 0.8 s after the tester marked a
scripted scene starting and cleared 0.75 s before they marked it ending - both
inside reaction time on the marker key - and fired **exactly once** in six
minutes covering weapon fire, plasmid fire, four gene-machine opens, a Little
Sister rescue and walking. Zero false positives.

**WHAT IT DOES NOT COVER, measured in the same run:** the Little Sister **rescue**
and the **EVE injection** both leave this bit clear. They are Hands *states*
(`ExorcisingGatherer`, `InjectingEve`), a different mechanism. **A build in which
a rescue fires this signal has a wrong offset - it has not improved on BRVR.**
That is the sharpest single test of the port.

**FALSIFIED, BRVR M7-S3 - do not gate anything on bit 0.** It is
`bFinishedStateAnimations` and it looked perfect, but gating the arms on it
produced *opposite* failures in two scenes: arms hidden for the whole Little
Sister crawl, arms stuck visible and frozen on the plasmid balcony. `state
PlayingScriptedHandAnimation` has an **empty body** and never touches the flag,
so during the crawl it kept the `true` left over from the last weapon state.

### pawn+0x464 bit 1 - Pawn.bCannotFall (the bathysphere)

`Pawn.uc` line 46. Pawn's own fields start at the `AActor` base `0x450`; lines
13..44 are exactly 32 bools - one full DWORD at `+0x460` - so the next three
start a fresh one:

| offset | contents |
|---|---|
| `+0x460` | lines 13..44, thirty-two bools, one DWORD exactly |
| `+0x464` bit 0 | `ShouldNotTakeDamageOnNextLanding` |
| `+0x464` bit 1 | `bCannotFall` |
| `+0x464` bit 2 | `bUseHavokRigidBodyCapsuleCollisions` |

**THE ORACLE.** `ShockPlayer` defaults bit 2 **true**, and
`ActionEnableBathysphereModeForPlayer` clears it in the same call that sets bit
1. So entering a ride must flip **bit 1 up and bit 2 down in the same write** -
two bits moving in opposite directions at once is not something a wrong offset
produces by chance. Every edge logs both bits and says whether the oracle held,
so the offset can be confirmed or refuted straight out of the log.

It exists so the rotation comfort settings can **leave a ride alone**: a
bathysphere is not a scripted animation, so without this signal a rotation freeze
applies there too and the camera stops following the sphere. BRVR shipped that
exact bug for as long as the signal was missing, and could not fix it by level
name because the mod does not know what map it is on.

### controller+0x9E0 - ShockPlayerController.bIsForcingPlayerMove

`ShockPlayerController` pushes `NullInput` and then calls `StartForcePlayerMove`,
which **interpolates** the player into position and heading *before* the scripted
animation begins. Through that whole window the hands flag is still false.

**It could not be computed.** Six interface-typed fields (`ICanBeUsed`,
`ICanBeFocused`, `ICanBeHacked`) plus a `TArray` sit between the class base and
the flag, and interface size in this fork is unknown - an unknown that compounds
six times. BRVR found it by **differential probe** (M7-S5) and correlated it
across three events whose durations differ by 24x - 1.0 s ("went straight in"),
0.24 s (instant), 5.75 s ("the slewing") - each matching an independent tester
report. The flag drops 0.09 s after the scripted animation begins.

**Shape check, every read.** A lone bool is exactly 0 or 1. Anything else means a
stale pointer or a wrong offset, which is not hypothetical: BRVR caught its own
bathysphere read doing precisely that.

### The held window - ONE SCENE, ONE WINDOW

A scene raises the two signals **in sequence**: the forced move walks you into
place, then the scripted animation plays. They normally overlap, so nothing
downstream sees a gap.

**The order is not guaranteed.** BRVR measured the Little Sister crawl, 2026-08-11:

```
22:46:10.556  FORCEDMOVE: --- forced move done ---
22:46:10.557  SCRIPTED: aim released back to the player   <- the collapse
22:46:10.561  SCRIPTED: *** SCRIPTED ANIMATION BEGAN ***  <- 5 ms later
```

One frame at 231 CalcView/s. Its camera hook released the aim in it, re-armed its
base from an aim field that happened to read exactly (0,0), and the field sat
**18.6 deg off the pawn for the whole 58-second scene**. So the pair is held:
rises instantly, falls only after the hold with neither signal set. Held in the
producer, not the consumer, because consumers layer different policies on top.

### What re-derives all three here, rather than trusting them

The rule is that an offset carried between builds is a guess until this build
agrees with it. This tree has something BRVR did not - **`fname_text()`, FName
index resolved to text via `GNames`** - so the anchor check is stronger here than
the one BRVR had to settle for ("does this field *shape* like a name"):

1. **Object identity.** The hands actor's own `UObject` name (`+0x28` ->
   `fname_text`) must read `PlayerHands`. patterns.h already records that
   cross-check.
2. **The walk.** All four FName slots the same field walk produces - `+0x498`
   `HandsOffscreenAnimationName`, `+0x4B8` `InjectingEveAnimationName`, `+0x4D8`
   `ExorcisingGathererAnimationName`, `+0x558` `CurrentScriptedAnimationName` -
   must resolve to non-null text. Four points along the walk's length.
3. **The pawn.** `Hands.Base` at `hands+0x450` must pass
   `body::is_gameplay_view()`, i.e. carry `kShockPlayerVtableRva`.
4. **The bool.** `ctl+0x9E0` must read exactly 0 or 1.

Any failure logs loudly and holds **every** signal false. Nothing is gated on a
value the check cannot stand behind.

**MEASURED AND NOT KEPT, BRVR M7-S2:** `+0x558` read `None` (index 0) for an
entire run *including* throughout a scripted sequence. Animation-level naming
does not come from that field, and the index-comparison idea it was going to
enable is unproven. It is an anchor slot here and nothing more.

### The three invariants that came with the port

All three were bought expensively in BRVR (`docs/brvr-reference/docs/INVARIANTS.md`
- *Locomotion and the aim field*) and none of them should be relearned:

1. **Never write `Controller.Rotation` while a sequence is moving the player.**
   Three balcony falls entered far right, straight on and far left and all landed
   on the same spot with no write. With a heading substituted in, both
   straight-on runs landed badly wrong. **The write itself is the damage.**
2. **Never let the window break mid-scene.** A per-frame "are you still in
   control" predicate over the HUD threw one landing 3.7 m. That is what the hold
   exists for.
3. **Follow the camera alone, never the camera and the aim field.** They are not
   independent - the balcony's opening snap moves both by 41.03 deg/s, so
   following both applied it twice and the view finished a whole snap past the
   authored heading. The measurement that originally justified following both was
   a deg/s average over 67 seconds, **and a rate cannot see a one-frame spike.**

### Where the game's rotation actually reaches the player here - measured against this tree

Worth writing down because it is not what the BRVR port would suggest, and it
changed what the comfort work had to be.

During **ordinary VR gameplay** `camera.cpp` overwrites `rot->pitch` and
`rot->roll` **absolutely** from the head, and writes yaw as `gameYaw +
headResidual`. So the game's screen shake, weapon kick and auto-pan **already
never reach the player in pitch or roll** - that fell out of the head drive years
of sessions ago and nobody wrote it down. Only yaw passes through.

The authored rotation reaches the player in exactly one case: **when the head
drive does not run at all**, and CalcView's rotator passes through untouched.
That is a scripted or cinematic camera taking the view (and, incidentally, a
menu - which is why the comfort policy also requires a scene to be active).

**Consequence:** BRVR's `FreezeGameplayRotation`, which exists to kill shake and
kick during play, would be **largely redundant here** - the axes it targets are
already gone. What is left of it is a yaw latch, which is the risky axis (it
feeds `body::on_calcview` and the pawn's facing) for a much smaller prize. It was
deliberately not ported. `ScriptedRotationFollow`'s job, by contrast, lands
cleanly and is what shipped.

## Session 64 part 2 (2026-08-22) - the offsets confirmed, and PausePC.swf

### All three offsets CONFIRMED in a headset run

The part-1 build was diagnostic and the run settled every question it asked.

**The anchor resolved four real, semantically correct names**, which is a
stronger result than "four slots resolved":

```
scripted: anchor ok - PlayerHands confirmed, 4/4 name slots resolve
          (+0x498 'HandsDown', +0x4B8 'Eve_ArmJab',
           +0x4D8 'GathererSave_Heal', +0x558 'None')
```

`HandsOffscreenAnimationName` reads `HandsDown`, `InjectingEveAnimationName`
reads `Eve_ArmJab`, `ExorcisingGathererAnimationName` reads `GathererSave_Heal`.
Those are the right *meanings*, not just well-formed names, so the field walk
behind `hands+0x594` is standing exactly where the derivation says.

**The bathysphere two-bit oracle held on BOTH edges:**

```
23:07:19.626  bathysphere ON   (pawn+0x464 = 00000002, bCannotFall=1 havokCapsule=0 - oracle HOLDS)
23:08:14.929  bathysphere off  (pawn+0x464 = 00000004, bCannotFall=0 havokCapsule=1 - oracle HOLDS)
```

Bits 1 and 2 moving in opposite directions in the same write, both times. That is
not something a wrong offset produces by chance.

**The held window earned itself on the first run:**

```
23:11:25.323  forced move BEGAN
23:11:26.415  --- forced move done ---
23:11:26.424  *** SCRIPTED ANIMATION BEGAN ***
23:11:26.424  window bridged a 16 ms gap between the two signals
```

Nine milliseconds of daylight between the two signals - the exact defect BRVR
measured on the Little Sister crawl, reproduced here immediately. Without the
hold, anything reading the pair would have seen the scene end and restart.

`forced move` also bracketed the bathysphere boarding at 1.22 s (23:07:18.393 to
23:07:19.617), consistent with BRVR's 1.0 s "went straight in" measurement.

### PausePC.swf is on the movie stack during rides and scripted scenes

**The finding that fixed two reported defects at once.** `PausePC.swf` is not
only the pause menu: the game keeps it on the playing-movie stack for the whole
of a bathysphere ride and every scripted scene, at `(2 playing)` rather than the
`(1 playing)` seen at a real pause.

`screens::panel_screen_up()` matched it, that fed `publish_ui_pause(true)`, and
`uiPaused` is an unconditional term in core's `wantCine`:

```
23:07:56.892  screens: top = "..\FlashMovies\PausePC.swf" (2 playing) -> PANEL
23:07:56.928  xr: cinematic quad ON (strict=1 stale=0 fovMismatch=1 screenOnly=0 uiPaused=1)
23:08:14.929  screens: top = "..\FlashMovies\HUDRadial.swf" -> gameplay
23:08:14.929  scripted: bathysphere off              <- the same millisecond
```

Two user-visible consequences, reported independently and diagnosed as one cause:

1. **The ride rendered on a head-locked quad** - "a square that is headlocked
   with black behind it" - instead of in stereo.
2. **The head was dead during scripted scenes.** `cinematic_active()` also gates
   the head drive in `CalcViewDetour`, so `driveHead` was false and looking
   around did nothing: "you stay looking wherever the game wants you to look,
   which feels very bad".

**Two candidates ruled out rather than assumed.** `g_cineStereo` already defaults
**true**, so the `fovMm && !stereoCine` term contributes nothing even though the
scripted camera genuinely renders 80 deg against a claimed 100. And `screen-only`
engaged 5 s *after* the quad (23:08:02 vs 23:07:56), so it is downstream of the
quad rather than a trigger. **`uiPaused` was the sole cause.**

The fix is one gate in the adapter: a panel screen is only a UI pause when no
scripted window is open and no bathysphere ride is running. At a real pause menu
neither signal is set, so that path is untouched; hack/loading/FMV screens arrive
through `screen_only()` and are untouched too.

### Writing the aim field during a forced move throws the landing

**Measured from a headset 2026-08-23, as a clean pair.** In a scripted event,
looking around with the head and right stick landed the player "way off" the
intended spot; holding head and stick completely still landed "in the exact right
spot". Same scene, same build, one variable.

`ctl+0x1E4` (`kActorViewDirOffset`, `Controller.Rotation`) is what a forced move
steers by, and it is the same field `body.cpp`'s transfer writes every frame to
keep the pawn's yaw under the player's head. So looking around re-aims the
interpolation the game is running.

**This is BRVR graveyard entry 16 reproduced**: *"A forced move steers by nothing
of ours - three balcony falls entered far right, straight on and far left all
landed on the same spot. The write itself is the damage."*

**Why it only appeared in s64.** Part 2's `PausePC.swf` fix made the head drive
live during scripted scenes, deliberately, so the player could look around
instead of being pinned. That made `vrDriving` true inside a scene for the first
time - which switched the body transfer on inside them too. One fix, two
consequences, and only the intended one was considered.

**The gate uses the HELD window, not the raw signal.** Entry 16's second half:
*"never let the window break mid-scene - a per-frame 'are you still in control'
predicate over the HUD did exactly that and threw one landing 3.7 m."*
`scripted_window()` bridged a real 16 ms gap on its first run here. The
bathysphere is included for the same reason with a different mover.

**No turn-axis kill goes with it.** Graveyard entry 12: zeroing `sThumbRX` freezes
`Controller.Rotation`, which forced-move sequences steer by, and the opening
bathysphere walked into the back wall. This repo has never zeroed it and must not
start.

#### What the probe proved on the way, and it matters architecturally

BRVR's `WalkDriftProbe`, ported as `vrbody walkprobe`, logged `pub -0.0` and
`camYaw == pawnYaw` on **all 25 samples** of a normal walking run. The
head-relative *stick rotation* lane is therefore inert in ordinary play: the body
transfer keeps the pawn glued to the camera, so the angle it would rotate by is
always zero.

**The two mods take opposite routes to the same feature.** BRVR redirects walking
by rotating the movement *stick* and never writes the aim field - `walk =
aimFieldYaw + stickAngle + R` - which leaves aim, the weapon trace and
forced-move sequences untouched by construction. This mod writes the field and
treats the stick rotation as a correction that measurement shows never fires.
**That difference is precisely why BS1 had a scripted-landing bug BRVR does not.**
Gating the write fixes the symptom; the architectural difference is worth
remembering before the next feature is built on the field write.

A consequence worth writing down so it is not re-derived: **`moveYawSign` is not
the walking-direction lever it looks like.** It scales a value that is almost
always zero. It was flipped in a headset and changed nothing, which is the
expected result, not evidence about the sign.

### The arms during a scene: MOTION answers what the flag cannot

**s64 round 7/8, 2026-08-23.** The arm hide's first predicate was
`scripted_window() && !scripted_anim()` - hide while the game is walking you into
position, show the moment an authored animation starts. It never fired once, and
the log shows it was **right not to**:

```
02:37:24.155  scripted: *** SCRIPTED ANIMATION BEGAN ***  (hands+0x594 = 00000006)
02:37:24.163  scripted: forced move BEGAN                 (ctl+0x9E0 = 1)
```

The animation flag was already true on the FIRST frame of the window, so
`!scripted_anim()` was false throughout and the arms stayed up. **The mechanism
was never the problem; the question was.** `hands+0x594` bit 2 answers *"is a
scripted sequence running"*, and a scene holds a pose for seconds in the middle
of one with the bit high the whole time. *"Do the hands have anything to do right
now"* is a different question and no flag on this actor answers it - BRVR reached
the same place from the other side and records `bFinishedStateAnimations` as
falsified for precisely this (see the M7-S3 note above).

**So measure the rig instead.** `bones::hand_motion()` differences one wrist's
component-space `hkQsTransform` against the previous CalcView:

```
raw = |dp| + (1 - |dot(q, qPrev)|) * 50
smoothed = max(raw, smoothed * 0.90)      // peak-hold with decay
```

- **Component space is what makes it honest.** The whole rig actor tracks the
  camera every frame, so a world-space measurement reads "moving" constantly.
  Only animation registers in the bone array's own frame.
- **The `* 50` is arbitrary by construction** and must not be read as a
  measurement. It only makes a small rotation comparable to a small translation.
  The threshold is meaningful *only* against it, which is why the raw and
  smoothed values are logged at 2 Hz inside a window rather than a constant being
  shipped and trusted.
- **The quaternion term is not optional.** A wrist can rotate in place without
  its position moving at all, so `|dp|` alone misses a whole class of animation.

**WHICH BONE, AND WHY IT CANNOT BE A CONSTANT.** A wrist *we* are writing reports
our own rigid transform - bit-for-bit identical every frame while the controller
is still, so the delta is not merely small, it is exactly zero. So sample the
wrist of a cluster the drive is **not** writing: bone 27 (right) when the right
cluster is free, else bone 6 (left), else **-1 meaning "cannot answer"**.

BRVR paid for this the hard way: 189 and 223 consecutive `raw 0.0000` samples in
two runs with the arms hidden for a whole scene, because its gate sampled bone 27
while a plasmid put the weapon in the left hand and the free-hand drive was
writing 27-44. **A run of exact zeros is only interpretable if you know which
bone produced it**, which is why the bone index is in our log line.

**-1 ROUTES TO ARMS VISIBLE.** The failure on record is arms hidden for a whole
scene; there is no matching record of arms shown for one frame. Fail toward the
one nobody has complained about.

**One local difference from BRVR, and it matters.** Its `HideBone` pins each
collapsed bone at *that cluster's own wrist*, so the wrist write is a no-op and a
hidden hand still carries the engine's pose on the one bone the gate reads. Ours
pins every collapsed bone at the **driven target** (`bones.cpp`, the
hide-inactive block), so a collapsed cluster is **not** honest here and counts as
written. Same rule, different reason - `g_clWritten[]` is set for both the driven
hand and the collapsed one.

#### Two of BRVR's constraints do not apply here, and the reasons are the point

1. **BRVR had to release its weapon cluster immediately before measuring**,
   because C1 let both clusters be driven inside a scripted window and there was
   then no engine-owned wrist. Here the scripted gate calls `bones::release()`
   (`hands.cpp`), which stands the *whole* drive down and hands the skeleton back
   - so on a scripted frame neither cluster is ours and bone 27 is always
   available. The blind guard is a backstop, not a load-bearing part.

2. **BRVR could not hide by bone while measuring by bone.** Its collapse cleared
   the skeleton's dirty byte, the engine stopped re-evaluating the whole array,
   the sampled bone froze, and a scene that started hidden stayed hidden forever
   while one that started animating stayed visible - a bistable latch.

#### FINAL, and it inverts what this section said for three rounds

**`DrawScale3D` IS NOT A SAFE HIDE HERE.** Measured 2026-08-23 with a read-only
whole-array probe, which is the only measurement in this arc that was never
confounded by the gate it was testing:

| | samples | bones moving |
|---|---|---|
| actor at `0.0001` | **293** | **0 of 47, every one** |
| actor at full scale | 183 | 21-47 on 116 of them |

Scaling the actor down takes it out of whatever the engine animates, so the whole
bone array freezes - not one bone, all of them. The motion gate reads that array
to decide when the arms come back, so hiding that way is a one-way door.
**BRVR's `ArmHide.h` says the opposite and is correct for BRVR; it does not hold
in this build, and three separate workarounds** (re-flagging the dirty byte every
frame, a timed re-check, discarding readings taken across the frozen gap) **all
failed, each in a new direction, because none addressed the freeze itself.**

**What works: collapse the BONES and leave the actor at full `DrawScale3D`.** The
actor stays in the render set, the engine keeps animating it, the array stays
live, and the signal is honest with no re-check machinery at all. Three things
are load-bearing and each cost a headset cycle to find:

1. **Write POSITION and SCALE, not scale alone.** A zero-scale bone still renders
   as a degenerate polygon *at its own position* - reported as "a strange looking
   small black polygon where both the right and left arm are supposed to be".
   Everything goes to a single point far below the actor. The session-19
   inactive-hand hide has always written both, for exactly this reason.
2. **Every bone, not just the clusters and sleeves.** Collapsing 44 of 47 leaves
   the root and spine at full scale, which is another way for geometry to survive.
3. **READING BEFORE WRITING IS NOT ENOUGH.** CalcView runs at 118-240/s against an
   animation tick well below that, so on many calls the bone still holds *our own
   collapse* - and differencing that produces a 5000-unit spike that reads as
   violent motion, trips the hold and pins the arms up permanently. The sampler
   must recognise its own write (bit-for-bit; we wrote the value) and report it as
   **stale**: not motion, not stillness, no new information. One real sample per
   engine tick is the correct rate anyway.

The weapon-attach bone still hides by translation and never by scale - session
16's divide-by-chain-scale is unchanged by any of this.

> **The generalised rule, and it is the reusable part:** *you cannot hide a thing
> from the renderer and keep measuring what the renderer drives.* BRVR wrote its
> version about the dirty byte; the real constraint is the render set, and actor
> scale is inside it while bone scale is not.

**The general shape, worth more than the feature:** a predicate can be a faithful
reading of a real engine flag and still answer the wrong question. Round 7's log
did not show a broken mechanism; it showed the mechanism working perfectly on a
question nobody wanted asked.

#### The `DrawScale3D` offset itself, kept after the constant was deleted

`kActorDrawScale3DOffset` was removed from `patterns.h` in the PR-51 review pass
(VOID, 2026-08-23): once the hide moved to the bones it had no reader anywhere in
`src/`, and its own comment still asserted the claim the table above falsified.
The derivation is preserved here because deriving it again is not free, and
because the neighbouring scalar is the trap:

| field | offset | what it is |
|---|---|---|
| `DrawScale` (scalar) | `+0x2AC` | written by `AActor::SetDrawScale` (`0x375830`). **Geometry-inert on the rig actor** - session 63: "rig-actor DrawScale does not size geometry; weapon-actor DrawScale does" |
| `DrawScale3D` (X/Y/Z floats) | `+0x2B0` | immediately after the scalar. The field BRVR hides arms with (`Hands/ArmHide.cpp`, `kDrawScale3DOff`) |

The layout is the ordinary Unreal shape - a scalar scale followed by a per-axis
vector - which is exactly why the two get confused; s64 wasted a build hiding the
arms with `+0x2AC` before finding that out. Either one needs the dirty protocol
(`kActorDirtyFlagsOffset |= 0x10`, `kActorRenderRevOffset++`,
`kActorDirtyByteOffset = 0`); a raw field poke without it is invisible.

**And if anything ever revives `+0x2B0`: NEVER WRITE EXACT ZERO.** The attach path
inverse-decomposes chain scale (session 16), the same division that makes bone 43
untouchable. That warning is about the write, not about the hide, and it survives
the falsification above intact.


### The viewmodel desync: it is the RENDER LOCK, and it was switched off

**BRVR HANDOFF_11 section 4.2 settles this, and supersedes HANDOFF_9 6.4.**
Chasing 6.4 - "write the weapon actor's own transform" - is a dead end that BRVR
already walked. HANDOFF_11 4.3(c):

> The rendered weapon follows a **skeletal attachment matrix**, not the weapon
> actor's top-level transform. That is why per-weapon grip offsets drift and why
> nine hand-tuned slots are needed at all. The prior-art notes state plainly that
> direct actor positioning was insufficient for this exact reason.

Measured here independently before reading it: the weapon actor's Location sits
10-16 UU off the bone we pivot about, does not rotate with that bone, and does
not track it under translation either. Same conclusion, arrived at the slow way.
**This repo already implements 4.3(c)** - bones.cpp's rigid cluster write IS
`newBonePos = targetAnchorPos + targetRot * (refBonePos - refAnchorPos)`.

**What 4.2 says the answer actually is:**

> Prior art does not use a formula. It computes a **render-lock correction** -
> lateral and depth corrections for the hand anchor derived from *captured
> renderer state*, failing soft to the uncorrected pose when that state isn't
> fresh. That is the answer. The foreground projection's effect on a world-space
> placement is a function of live renderer state, not of two FOV numbers. **You
> have to read it, not derive it.**

That is `render_lock_delta()` in bones.cpp, line for line: it reads the live
world tanH from `hfov_option_ptr()`, projects the target through the live camera
rotator to NDC, derives a lateral and a depth correction, and returns false -
failing soft - when the capture is not fresh.

**It defaults to OFF** (`g_renderLock{0}`), and a live run measured
`render lock: off |delta|=0.00 UU gain=0.90 dgain=0.90 solves=0 skips=0`.

**And its precondition only became true in session 28.** The function's own
comment says the correction assumes the lens ratio `k` collapses to 1 - which
was true only at 16:9. The shipped 0.75 match constant left the fg lens
1.7778/aspect narrower than the world everywhere else, so at this square
backbuffer `k` was really 1.78 while the code assumed 1, and the correction was
mis-scaled by that factor. Session 28 fixed the constant to `(4/3)*(H/W)`;
measured this session as `k=1.381818`, fg 117.5 deg, the two lenses agreeing on
the vertical to 0.071%. So the render lock's assumption holds NOW and did not
before - which is the likeliest reason it was left off.

**The test is a config change, not a build** (BRVR rule 6, "config bisection
beats commit bisection"): F10 -> bones -> `lock ABS (true position)`, or
`vrbones lock 1`. `vrbones lock diff` is the head-split-cancel-only variant, and
`vrbones lockgain <0..2>` / `lockdepthgain` tune it. Watch `solves` climb and
`skips` stay near zero in `vrbones status`.

**What this predicts.** If the render lock is the answer, the per-weapon grip
offset and model trim added earlier this branch become unnecessary rather than
merely untuned - which is the "weapons correct by default" outcome asked for,
and matches HANDOFF_11's account of why nine hand-tuned slots were ever needed.


#### TESTED 2026-08-25: all three lock modes desync identically - because the lock REFUSES ITS OWN ANSWER

Headset test, ABS / DIFF / off, all three the same. That reads as "the render
lock is not the fix" and it is not what happened. The log:

```
[bones] lock: refusing outsized delta (lat 30.0 depth -15.0)
```

`render_lock_delta` armed, projected, produced a correction, and then
`bones.cpp`'s own sanity guard threw it away:

```cpp
if (latMag > 30.0f || dDepth < -120.0f || dDepth > 120.0f)   // refuse
```

The lateral term hit the 30 UU ceiling, so `ptc` was never nudged and all three
modes collapsed to the uncorrected pose. **The A/B measured nothing.** (HANDOFF_7
rule 11: check whether the test was valid before believing the result.)

**The real question is now sharp: is a 30 cm lateral correction plausible?**

- If **yes**, the guard is simply too tight for this configuration and the fix is
  to widen it - but 30 cm is enormous for a viewmodel nudge, so this is the less
  likely branch.
- If **no**, the correction is mis-scaled and the guard is doing its job. That
  points back at `render_lock_delta`'s inputs: it takes world `tanH` from
  `hfov_option_ptr()` and projects through `ctx.cam*`. Session 28 fixed the
  lens-ratio assumption (`k` really is ~1 now, measured `k=1.381818`, fg 117.5,
  lenses agreeing to 0.071%), so the remaining suspects are the NDC projection in
  `world_ndc` and the depth term, not the lens.

**Next session starts here**, and it is a measurement, not a guess: log the raw
`dLat` / `dDepth` alongside the NDC the correction was derived from, hold the
controller still at a known pose, and see whether the 30 UU is a stable bias or
noise. A stable bias of that size with the lenses matched means the projection
model is wrong somewhere specific, and that is findable.

Do NOT widen the guard first. It would apply a correction nobody has shown to be
correct, on a path that already moves the whole cluster.

#### CORRECTION 2026-08-25 (s66): "lat 30.0" was never a measurement of the correction

Re-reading the s65 headset log rather than the one line quoted from it. The run
left **three** refusals, not one:

```
[14:43:55.383] [bones] lock: refusing outsized delta (lat 64.8 depth -4.1)
[14:44:10.335] [bones] lock: refusing outsized delta (lat 30.0 depth -16.6)
[14:44:21.346] [bones] lock: refusing outsized delta (lat 30.0 depth -15.0)
```

**That line only prints when `latMag > 30.0f`, so the sample is censored from
below at exactly the number we were treating as the finding.** "Is a 30 cm
lateral correction plausible?" is the wrong question - 30.0 is the guard's own
threshold showing through a filter that can never report anything smaller. The
one uncensored number in the set is the **64.8**, more than twice it.

The camera is stationary across all three (`loc=(-37382.0 518.1 7779.3)` in
every one), so what separates them is orientation: `rot=(64064 65158 0)` at
64.8 against `(1452 63974 0)` and `(1194 64160 0)` at the two 30.0 readings.
The two near-identical poses give near-identical `lat`, so the term is
deterministic and pose-stable - and roughly 10 deg of pitch more than doubles
it. **Strong pose dependence under a stationary camera is the shape of the
finding**, and neither the magnitude nor its cause can be read off a censored
log line.

Also true of that run, and worth knowing before repeating it: `[tlm] lock`
never fired once (`vrbones telemetry on` was never issued), and no `render
lock` command line appears at all - the lock was armed from the F10 radio,
which does not log. There is no other lock data in the run.

#### The s66 instrument: `vrbones lockprobe`, and the terms that decensor this

`render_lock_delta` now computes four diagnostic terms beside the existing
ones. All read-only; none of them feed the guard or the correction.

| Term | What it is | Why |
|---|---|---|
| `nat=(x y)` | where the fg model says `ptc` renders TODAY, in NDC | the model's own answer, to set against the world's |
| `dndc=(x y)` | `tgt - nat` | dimensionless, so it survives every depth and UU scale factor. Its two axes name the suspect: X -> `tanH` / the right basis, Y -> `live_inv_aspect` / `invTanVFg` |
| `latP` | the lateral correction solved at CONSTANT depth (`wNat`, not `w*`) | `lat` is the component of the FULL delta perpendicular to the fg forward, so it also carries the sideways shadow any depth move casts on an off-axis anchor - a point at NDC n sits `n*tan(fov/2)*w` off axis, so changing w by `depth` moves it sideways for free (~9 UU at the observed depth -15 and a half-screen anchor). `latP` is the honest lateral term; `lat` is not. `-1` means unsolvable, which is not `0` |
| `split` | camera yaw minus actor yaw, degrees | the head-split the lock exists to cancel. A correction that does not scale with this is not doing its job, whatever its magnitude |

`vrbones lockprobe on|off` runs the solve **for its numbers with the lock still
off** - the apply stays gated on the lock mode alone, so a probe can never move
a bone. That is what makes the s65 A/B repeatable without arming a correction
nobody has shown to be correct. It needs `vrbones telemetry on` for the lines
(the probe command says so when telemetry is off), and `vrbones status` reads
`+PROBE (measure only)` while it is armed.

**Read `k` first.** Under the session-28 lens match `k` collapses to 1 by
construction (`invTanHFg = 1/tanH`). If the log shows ~2.06 instead,
`fg_fov_match_active()` was false, the legacy 60-deg constants were used, and
every downstream number is mis-scaled for that reason alone - nothing else in
the run means anything until that reads ~1.00.

**Decision rule, written before the run:**

| Observation | Verdict |
|---|---|
| `latP` small (<=3 UU) and `dndc` ~ 0 | model and world agree laterally; `lat` is the depth move's shadow, and the guard is gating the wrong quantity |
| `latP` large and stable at a fixed pose | the model genuinely disagrees; mis-scaled, and `dndc`'s larger axis names the term |
| `latP` swings frame to frame at a fixed pose | unstable input (hand sway, per-eye vs mono `ctx.cam*`), not a scale error |
| `latP` does not scale with `split` | the lock is not cancelling what it exists to cancel - refuted regardless of magnitude |

Baseline to compare against: the session-21 `simhead` sweep recorded above
(`lat` 1.04-11.59 with the yaw transfer off, 4.50-4.96 with it on). The run
must be at the headset's own viewport - the s65 log reads `backbuffer 2750x2850
aspect 0.96491`, `WORLD tanH=1.191754 tanV=1.235090 (hfov 100.00 deg)` -
because `live_inv_aspect()` feeds both `world_ndc`'s `tanV` and `invTanVFg`,
and a 16:9 run would answer a different question.

**NOT RUN YET.** The instrument is built and installed; the sweep is the next
thing that happens.

### RETRACTION: the game's yaw DOES reach the camera during gameplay

Part 1 recorded that the head drive overwrites `rot->pitch` and `rot->roll`
absolutely, concluded that shake and the auto-pan therefore never reach the
player, and left BRVR's `FreezeGameplayRotation` deliberately unported as
"largely redundant here".

**That conclusion was wrong, and a headset run refuted it.** The measurement it
rested on was correct but incomplete: yaw is not overwritten.

```cpp
rot->yaw = gameYawUnits + residualUnits;   // camera.cpp
```

`gameYawUnits` is the engine's own value, so everything the game puts into yaw
arrives untouched. Reported 2026-08-22: *"I am getting screenshake with world
events and its making me look at groups of enemies that the game normally turns
you to."* Screenshake reads as horizontal because horizontal is the only axis
left.

**The lesson worth keeping:** "two of the three axes are discarded" is not "the
feature is redundant". The one surviving axis was the one the complaint was
about. A per-axis audit needs a per-axis conclusion.

The freeze now ships, absorbing the game's yaw *delta* into an offset rather than
clamping the value, so the view declines to be turned instead of snapping. It
excludes itself while the turn stick is off centre, during a scripted window, and
during a bathysphere ride - the last of which is only possible because part 1's
signal exists, and is the bug BRVR shipped for as long as its own signal was
missing.

**Known trade, accepted deliberately (user's call):** the filter is camera-only.
Aim and body still turn with the game, so the crosshair can sit off centre after
a large shake. The offset is bounded at 60 deg and says so in the log if it ever
reaches that, because an unbounded offset is an unbounded divergence between
where you are looking and where the pawn is facing.

### Turning yourself during a scripted scene

A scripted sequence pushes `NullInput`, so the game discards stick input and a
turn routed through the game does nothing. Ported from BRVR's `ScriptedManualYaw`
as a mod-side accumulator applied to the **camera only**.

It is never written into `Controller.Rotation`. That is BRVR's invariant 1 and it
was expensive: three balcony falls entered far right, straight on and far left
and all landed on the same spot with no write, while a substituted heading put
both straight-on runs badly wrong. **The write itself is the damage.**

### The both-edges rule, and a third door it came through

BRVR's rule is that a reference latched inside a scripted window must be dropped
on **both** edges, or the second scene of a session differences against a value
left over from the end of the first ("first almost perfect, second way off").

That applies here to the scripted-turn accumulator and the freeze reference, and
both are reset on the window's rising and falling edge. But there is a third gap
BRVR did not have: **the freeze filter only runs while the head drive runs.** A
menu, a cutscene or a scripted camera stops calling it entirely, so the yaw it
would difference against on the way back out is from before the scene - and
absorbing that delta would swallow the entire turn the scene just made. The
filter therefore re-arms whenever more than 250 ms has passed since its last
call, with dt as the witness that it was not being called.

## Session 64 part 3 (2026-08-23) - the bathysphere and Big Daddy runs, and two traps in the instrument

The two scenes s64 had never been run against were both verified in a headset,
and both passed. What is worth keeping is not the pass - it is the two ways the
instrument lied on the way there, because the instrument is now stripped and
cannot explain itself.

### The bathysphere: what actually carries the ride

The window covers the **boarding forced-move only** and then closes; the ride
itself is carried by `bathysphere()` (`pawn+0x464` bit 1), not by
`scripted_window()`. Measured:

```
16:15:05.517  body transfer STANDS DOWN        <- window opens (forced move)
16:15:06.877  bathysphere ON   (pawn+0x464 = 00000002, bCannotFall=1 havokCapsule=0 - oracle HOLDS)
16:15:07.014  last motion sample               <- window closes, 70 s of ride still to go
16:15:42.175  panel screen up during a bathysphere ride - NOT treating it as a UI pause
16:16:18.389  bathysphere off  (pawn+0x464 = 00000004, bCannotFall=0 havokCapsule=1 - oracle HOLDS)
16:16:18.568  body transfer resumes
```

The oracle held on both edges a second time, on a different ride from part 2's.

**This is why the consumers are `scripted_window() || bathysphere()` and not the
window alone** (`body.cpp:492`, `camera.cpp:2091`). The panel gate firing at
16:15:42 - 35 s after the window shut - is the whole design working. A future
session that sees "no window samples during the ride" and reaches for graveyard
entry 12 is reading a correct measurement and drawing the wrong conclusion.

The black square ~60 s into the descent is still there and is NOT this:

```
16:16:07.422  cinematic quad ON (strict=1 stale=1 fovMismatch=1 screenOnly=1 uiPaused=0)
```

`uiPaused=0` - the part-2 gate held. It is the `stale=1` term (CalcView stops
being called across the arrival and map transition), which is session 22's
finding and needs a render-side answer of its own.

### TRAP 1: a pause menu opened mid-scene makes the STALE watchdog cry wolf

The watchdog s64 added said, in words, "the gate is frozen; the hide mechanism is
the problem, not the threshold" - and it was written precisely so a later session
would trust it and not tune numbers. It fired once, and it was **wrong**:

```
16:18:51.888  screens: top = "..\FlashMovies\PausePC.swf" (2 playing) -> PANEL
16:18:53.909  [bones] motion has been STALE for 2015 ms - ...
16:18:55.116  screens: top = "..\FlashMovies\HUDRadial.swf" (1 playing) -> gameplay
```

"STALE for 2015 ms" back-dates the freeze to 16:18:51.894 - **6 ms after the
panel came up** - and it never recurred once the panel closed. A menu freezes the
game's own animation, so every bone read genuinely *is* our own collapse, which
is exactly the state the watchdog was built to detect. The two are
indistinguishable from the bone array alone; only the screen stack separates
them. Anything reviving this watchdog must stand it down while
`screens::panel_screen_up()`.

### TRAP 2: `0/47 bones moved while collapsed` is the HEALTHY state

The array probe was added to catch the collapse suppressing evaluation the way
`DrawScale3D` did. On the passing run, 323 of 352 collapsed probes read `0/47` -
92%, which reads as the failure returning. It is not.

The gate samples **before** writing the collapse, so two consecutive reads
normally both see the engine's authored pose and nothing has moved between them.
`0/47` is what a working hide looks like. The tell is the other bucket: every
`47/47` sample carried an identical `max 5009.3979`, and that constant is the
magnitude of our own collapse write - the probe straddling it, not the hands
moving. A raw count of moved bones cannot answer this question; only the
constant-vs-varying character of the maximum can.

### The scene that did exercise the hide

93 s, one engage, one release, no flapping and no one-way door:

```
16:17:38.046  hidden=0     <- scene opens, arms visible
16:17:43.050  hidden=1     <- hold expires, rig collapses
16:19:11.123  hidden=0     <- window closes, authored pose restored
```

That is the first time the bone-collapse hide has been exercised end-to-end
without freezing, which is what `DrawScale3D` could never do.

## Session 65 (2026-08-23) - the world FOV is overridden BELOW CalcView

### The bathysphere renders 80 deg while CalcView reports 100

The "black square" on a bathysphere was never the cinematic quad. Falsified
directly: `vrcine off` kills `g_cineActive` outright and the box survived it.

What actually happens, measured on the frame the player presses A:

```
19:48:40.950  scripted: forced move BEGAN  (ctl+0x9E0 = 1)
19:48:40.957  fov watch: WORLD tanH=0.839100 (hfov 80.00 deg, 12/12 votes)
19:48:40.966  rendered-fov mismatch ON (rendered 80.0 deg vs option 100.0)
19:48:40.969  xr: claim substituted from the live WORLD lens (hfov 80.00 deg)
```

**CalcView reports `fov=100.0` for the entire ride** - every sample, boarding to
landing. The narrowing therefore happens DOWNSTREAM of CalcView, which is why
nothing in the camera path could see it and why the fov watch is the only
instrument that catches it.

The mod's response was correct and was itself the visible defect: it re-claims
the OpenXR layer at the measured 80 deg, so the image fills 80 deg of a wider
headset - a correct, full-resolution picture sitting in a box with black around
it. Reported as "renders in a square, but the resolution is the same".

### The two fields, and they were already in this file

`PC+0x45C` is the world lens and `PC+0x648` its mirror. **Both were already
derived here for another purpose**: the foreground scene-node ctor is passed
`float PC+0x45C` (default 75.0) beside `PC+0x460`, and the note on `PC+0x65C`
records the "75/75/60 fov floats" at `PC+0x648..0x650`. Session 65 only needed
to WRITE them.

BRVR reached the same two fields independently (`ClampWorldFov`, its
`WorldFovOffset`/`WorldFovOffset2`) and measured the narrow side as 75.0 -> 60.0.
**The numbers do not transfer** - this build reads 100 -> 80 - but the offsets
agreeing with a derivation already in this tree is the corroboration that
matters.

### Why the guard is gated, and why the gate is not `bathysphere()`

The window is `forced_move() || bathysphere()`. The narrowing lands **1.2 s
before** the ride flag, on the forced-move frame, so gating on the ride alone
misses the start - which is precisely the moment the player sees the box appear.

Two things depend on the guard staying narrow:

1. **It would blind the cutscene detector.** One leg of `wantCine` fires when the
   game renders a different fov than it claims. Clamping globally deletes exactly
   that evidence - BRVR's own `CONSOLIDATION.md` names this as integration hazard
   number one for this port.
2. **Weapon zoom uses the same field.** `Hands::FadeFOV` drives it downward when
   you scope. A global floor would fight it; you cannot scope on a bathysphere,
   so the gate removes the conflict rather than special-casing it.

The restored value is SAMPLED, not hardcoded: whatever the lens read while no
scene owned the camera. That accounts for the user's own FOV option for free.

## Session 65 (2026-08-23) - the turn jitter, and the wall that looks like a pause menu

### The turn jitter: measured, NOT solved, and the attempt is reverted

Reported as: walking a circle is smooth while the turn stick is barely over, and
past an exact repeatable threshold it goes jittery. Predates every turn change
this repo has made. **Six headset runs. The anomaly is real and located; whether
it is what the player FEELS was never established, and the fix was reverted.**

What is measured and can be relied on:

- **It is not our input.** The composed pad the game actually consumed reads flat
  to three decimals (`composedRx -0.428..-0.428`) across whole windows in which
  the engine's yaw rate swings from 5 to 349 deg/s.
- **It is not the body transfer.** `resid` stayed at 0.00-0.02 deg throughout,
  the 180 deg/s slew cap never engaged, and the movement stick was never rotated
  (0 STRADDLE events in 570 samples).
- **It never stalls.** Zero frames where the yaw failed to advance, in any run.
- **The engine advances its own yaw unevenly**: about seven normal steps then one
  of ~2.5x, repeating every **125 ms (8 Hz)**. Confirmed across two different
  clocks and two frame rates - 8 counted frames at 64/s and 15 at 120/s are both
  125 ms - so it is time-periodic, not frame-counted.
- **Frame time amplifies it**, at a fixed stick: 1.98x at 8 ms/frame, 2.11x at 9,
  2.57x at 10, 3.67x at 12, **5.29x at 13**. So the lag spikes and the turn
  jitter are the same subject; fixing pacing would reduce the jitter.
- **There is a separate, real deadzone cliff**: mean turn rate jumps ~5x from
  3.5 to 16.7 deg/s across |rx| 0.20 -> 0.25, straddling the game's 0.225
  per-axis deadzone, and the response is nearly flat above it (16.7 at 0.25 to
  24.6 at 0.45). Worth fixing on its own merits.

**THREE PROBE REVISIONS WERE SPENT MEASURING THE INSTRUMENT.** Recorded because
the next person will reach for the same clock:

1. `GetTickCount64` has **15.625 ms** resolution and this path runs at ~245/s, so
   most calls read `dt == 0`. Worse, the sampler updated `prevYaw` before the
   `dt > 0` guard, so those frames' yaw was DISCARDED. The residue produced a
   convincing "double step every 8 frames" - which is exactly 8 x 15.625 ms.
   **Use QueryPerformanceCounter for anything sub-frame.**
2. A 20 Hz sampler against a ~120 Hz frame cadence aliases: `dt` alternated
   59/68 ms with the rate alternating against it, which is indistinguishable
   from a real oscillation. **Accumulate every call; throttle only the summary.**
3. A summary that resets its accumulators before reading them logs nothing. One
   whole run produced 38 windows and zero sequences.

**The attempted fix, and why it was abandoned rather than turned off.** A
view-only spike clipper: clamp the excess of a step over its local average out of
the yaw CalcView returns (through `yaw_adjust_units()`), and bleed it back over
the next few frames. Deliberately NOT a field write, which is what kept
graveyard entries 12, 13 and 16 out of scope entirely rather than merely handled.
It built and shipped behind an F10 toggle; the player still felt the jitter, and
the session ended before establishing whether the yaw anomaly is the percept at
all. **The turn probe could never have answered that** - it reads the engine's
yaw FIELD, and a view-only correction only changes CalcView's out-parameter, so
the probe is blind to it by construction.

**The open question, and it is the first thing to settle before any more work:**
is the jitter the VIEW rotating unevenly, or the DIRECTION OF TRAVEL stepping?
Everything above measures the view. Graveyard entry 14 is the movement-side
candidate and is untested for this symptom.

### Walking into a wall drops the view to the cinematic quad

**The bug**: walk into a wall and the world renders in an anchored square exactly
like the pause menu; the hand renders but cannot be moved; the game keeps
running. It toggles on and off as the player walks into and away from the wall,
and it persists if they quit while it is active. Several walls in one area.

**The cause is a hard-coded draw-count threshold**, `hud_capture.cpp`:

```
ID3D11Resource* scene_leader() {
    ...
    // A handful of DSV-bound draws is not a scene (the fg rig pass has ~14);
    // the world pass has hundreds. 32 is comfortably between.
    return (best && best->n >= 32) ? best->res : nullptr;
}
```

A view filled by one near wall draws too little geometry, the winning render
target falls under 32 DSV-bound draws, `scene_leader()` returns null, and the
screen-only verdict trips:

```
screenOnly = scene_leader() == nullptr && g_swfDrawsThisInterval >= 20
```

`screenOnly` is an unconditional term in core's `wantCine`, so the projection
layer drops to the M2 quad - the same path menus use, which is why it looks like
the pause menu and why the anchored placement is identical.

**Measured 2026-08-23, 19 transitions in one run:**

```
22:19:19.661 screen-only interval off (swf draws 145, world pass present)
22:19:19.710 screen-only interval ON  (swf draws 145, world pass absent)
22:19:19.723 xr: cinematic quad ON (strict=1 stale=0 fovMismatch=0 screenOnly=1 uiPaused=0)
```

**THE SWF HALF OF THE PREDICATE IS DEAD.** `swf draws 145` is identical on both
sides of every transition - ordinary gameplay is always far above the 20-draw
floor, so the guard that was meant to stop single stray draws tripping the
verdict contributes nothing. In practice `screenOnly == (scene_leader() == null)`.

**`strict=1` throughout**, which is the useful discriminator: the strict gameplay
verdict held the whole time the quad was up. A momentary world-pass loss while
the gameplay verdict is TRUE is not a loading screen, and gating on that looks
more promising than tuning 32 - a threshold has no correct value here, because
"how much geometry is on screen" is a property of the level, not of the mode.

The 3-interval hysteresis already present (`g_screenOnlyStreak >= 3`) is not
enough: a wall is not momentary, it lasts as long as the player faces it.

### NOT A BUG: the interact prompt aims with the HEAD, not the controller

Reported 2026-08-23 as a soft lock - the flying turret wedged in the first
Medical Pavilion door offered no hack prompt while vending machines, lootables
and money on the ground all did. It was aim, not a defect: the player was not
looking down far enough at it.

Worth writing down because the false alarm is a good one. The use/interact trace
is the ENGINE's, so it aims along the engine's view pitch - and in VR that value
is driven only indirectly, by the session-30 pitch servo steering it toward the
head through the game's own input path. **Pointing a controller at something
does nothing for the prompt; you have to physically look at it.** Objects with
generous use volumes (money, loot, a vending machine you walk into) hide this;
a small device low in a doorway does not.

If a prompt ever genuinely will not appear, the F10 input section's "Kill
right-stick pitch under VR" is the A/B: unchecked, the right stick's Y goes back
to the game as an ordinary look axis and can pitch the engine's view directly.
If the prompt appears that way and not otherwise, the servo has stalled - it
fails open to the plain kill, and a frozen engine pitch is invisible until
something the engine owns aims with it.

## Session 71 (2026-08-30) - THE OFF-HAND SWIVEL: the rig actor's DrawScale multiplies bone translations

**SOLVED, headset-confirmed.** The reported bug: *"Rotating the right hand causes
the left one to move as well. Turning the pistol left causes the left hand to
swivel towards the right hand... it doesnt apply rotation to the left hand, but
it just moves it positionally in a swivel depending on right hand rotation
horizontally."*

### The mechanism

`AHands` (the rig actor) ships with **DrawScale = 0.8000**, and the foreground
rig path **multiplies every bone TRANSLATION we write by it**. So a component-
space position `p` that we write renders at `k*p`, with `k = 0.8`.

The free hand's target is built to cancel the actor exactly:

```
ptc   = inv(A) * (want - actorLoc)          <- what we write
world = actorLoc + A * (k * ptc)            <- what the engine renders
      = k*want + (1-k)*actorLoc
```

With `k != 1` a share `(1-k)` of **actorLoc** leaks into the free hand's world
position - and `actorLoc` is the HELD hand's position plus its placement offsets
rotated into the held hand's own basis. Rotate that wrist and `actorLoc` swings
through an arc; 20% of that arc lands on the off hand. Measured this session: the
actor's location swung **103 UU in x and 96 UU in y** across a 344 deg sweep, so
the leak is ~20 UU - a plainly visible swivel.

Every part of the symptom falls out of that one term:

| Observation | Why |
|---|---|
| position moves, orientation clean | a scalar on translations cannot touch a quaternion |
| tracks the HELD hand's rotation | `actorLoc` carries the held hand's rotated offsets |
| magnitude grows with distance from the actor | the leak is `(1-k) * actorLoc`, and the cancellation error scales with `\|want - actorLoc\|` |
| **only the OFF hand, never the held one** | the held hand's anchor sits essentially ON the actor, so `\|want - actorLoc\| ~ 0` and the leak lands on the same point. The free hand is an arm's length away. **This asymmetry is the tell** - nothing else in the subsystem predicts "only the other hand moves" |

### The fix

`drive_free_hand()` divides its component-space target by the rig actor's
DrawScale before writing (`bones.cpp`, grep `FREESCALE`):

```cpp
float k = 1.0f;
if (ds_read(handsActor, &k) && k > 0.01f && fabsf(k - 1.0f) > 0.001f) {
    ptc[0] /= k; ptc[1] /= k; ptc[2] /= k;
}
```

Read live rather than hardcoded: 0.8 is what the shipped rig authors, not a
constant of the engine, and a rig that ever authored something else would
silently reintroduce the bug.

**BRVR had this from the start** and we had the measurement and did not use it.
`FreeHandModelPos()` in `Camera/CameraHook.cpp` divides by `handsScale` with the
reason in one line: *"DrawScale scales the whole mesh, skeleton included, so a
bone moved by N model units renders as N * scale centimetres."*

### Why it took a whole session, and the lesson

**Session 16 measured this exact behaviour and recorded it** (see "Rig-actor
DrawScale + drive positions pre-divided by s" above): *"bone positions round-trip
through the scale - written p renders at s\*p"*, and *"the fg rig path consumes
actor DrawScale for bone translations but NOT for skin/attached-mesh size."*

The headline that survived into the code was **"the rig actor's DrawScale does
not size geometry"** - true about mesh SIZE, and it buried the half that mattered.
The finding was never wrong; the summary of it was lossy, and the lossy version
is what every later session read. **When a measurement has two halves, the
summary must carry both, because the summary is what gets reused.**

### The probes that read clean, and why they all could

FREEPROBE, FREETARGET, FREEHOLD and ACTORWATCH all read clean or constant while
the bug was live, because **every one of them measures the input or component
space**. The multiply happens AFTER, in the engine's bone->world step, which
nothing had ever measured. A probe that cannot see the stage where the defect
lives will always report health.

Four instruments read zero in this session for four different reasons - zero by
algebra (measuring through the transform it had just cancelled), zero by timing
(a window that closes before the engine acts), zero by an unchecked read (a
failed read seeded from the value it was compared against), and zero by masking
(`reapply()` restoring the value before the comparison). **Before believing a
silent probe, ask what it would print if the defect were present** - and check
that the stage it samples is the stage the defect lives in.

### Corrections this session leaves behind

- **ACTORWATCH is not a defect meter.** With the head and both controllers held
  still it reads a CONSTANT (84 of 88 samples bit-identical at
  `pitch +42.0 yaw +24.7 roll +2.1`). It is the standing difference between the
  rotation we write (the controller's) and the one the engine writes (the view's),
  so it can never be zero while the mod deliberately writes a different rotation.
  Do not chase it to zero.
- **There is no actor race.** The actor holds our value at CalcView (0.1 deg) and
  through scenedraw (FREEPROBE, 115 samples, bit-identical). Cancelling the
  engine's actor instead of ours is a measured no-op.
- **The rig actor's DrawScale is NOT inert.** It does not size geometry; it does
  scale bone translations. Both halves are true and both matter.

### The off hand needed the SAME two-knob split as the weapon (2026-08-30)

Reported immediately after the DrawScale fix landed: *"they are decoupled, but now
the offhand is having the pivot problem the weapons had when I adjust the
position/rotation with the numpad."*

`g_offHandPosCm` is applied along the TARGET's own axes - `ptc += qtc * o`, i.e.
INSIDE the hand's own rotation - so in world the anchor sits at `want + T*o`. As
the free hand's own rotation `T` changes, the anchor orbits `want` at radius
`|o|`. **The knob is a pivot**, and tuning it to move the hand displaces the point
the hand turns about. That is the weapon's grip-offset defect exactly, inherited
by a hand that was written later:

> *"The grip offset IS the pivot - and that is why tuning it could never work...
> Using it to correct the gun's HEIGHT displaces the pivot by the same amount,
> which becomes an orbit the moment the wrist rotates."* (15 cm bought an 8 inch
> orbit.)

The weapon's answer was **two knobs, two jobs: grip = where it pivots, view =
where it sits**, with placement applied in the VIEW frame after the model
transform where it cannot create a lever. The off hand now has the same pair:

| knob | frame | job |
|---|---|---|
| `off_hand_cm` (`offHand*Cm*`) | the hand's own axes, inside the rotation | where it **pivots** |
| `off_hand_view_cm` (`offHandView*Cm*`) | the camera basis, on the WORLD target before the push into component space | where it **sits** |

The numpad's free-hand position mode drives the VIEW knob (help text: `FREE-HAND
PLACE`), because "move the hand" is what a tester means when they reach for it;
the pivot knob stays reachable through `hands.ini`. Roll is dropped from the
camera basis for the same reason the weapon drops it - the camera's own roll must
not tip placement.

**The general rule, now paid for twice:** an offset applied inside a rotation is a
pivot, not a placement. Any new knob that nudges a driven mesh has to say which of
the two it is, or it will be tuned as one and behave as the other.

## Session 72 (2026-08-30) - THE OFF HAND'S ARM: eight couplings, and the shape they share

**SOLVED, headset-confirmed: the off-hand arm is fully decoupled from the held
hand.** This is the long half of the off-hand work and it is worth reading before
touching either arm, because the same mistake recurs in eight different places
and one of them will be made again otherwise.

### The shape every one of them shares

> **A quantity that is not fully determined by the solve takes the frame it is
> written into, and that frame is the ACTOR - which is the held hand.**

The free hand's bones live in component space, which is relative to the `AHands`
actor, and this mod writes that actor from the HELD controller every frame. So
component space *spins and translates with the held hand*. Anything that is not
pinned to a world-anchored reference is therefore pinned to the other hand.

s70i had already written this down for the shoulder - *"the entire space the arm
lives in spins with your wrist... there is no bone write that can fix that"* -
and the lesson did not get carried to the other seven cases.

### The eight, in the order they were found

| # | symptom | cause |
|---|---|---|
| 1 | off-hand IK worked in some runs, not others | `g_armRefValid` was set ONLY in `drive()`'s settle window, indexed by the HELD hand. The free hand had an arm reference only if that hand had previously been the driven one - **equip-history dependent, not build dependent** |
| 2 | "the right hand drives the left arm's rotation" | when it did fire, it solved the free arm from `g_armRef`/`g_armW0` captured in the HELD role, and read `g_ref` (the held bank) for scale |
| 3 | hiding the driven arm silently killed the off arm's IK | `g_collapse` was one global doing two jobs for two hands: hiding the driven sleeve AND gating `solve_arm()` for both |
| 4 | bicep and elbow roll with the held hand | `quat_from_to` returns the MINIMAL rotation - **zero component about the axis** - so the arm's roll was never set by the solve. A free DOF takes the actor |
| 5 | forearm still rolls after 4 | the twist HELPERS (22/23) were built from the bare swing, so their axial DOF was free and the hand twist rode on top of it |
| 6 | elbow orbits the arm axis | the pole is a blend, and its `poleRig` half is `aSEn` - a capture-frame COMPONENT-space direction. At the shipped `ElbowFollowWrist=0.4` that was 40% of the pole spinning with the actor |
| 7 | shoulder shifts forward/back as the weapon pitches | the rig DrawScale (0.8) multiplies bone translations; the wrist target was divided by it and **the arm was not**, so the shoulder rendered at `0.8*sWorld + 0.2*actorLoc` - a fifth of the actor, on a 44 cm lever (`posFwdCmR`). Then, with S and W divided but the bone LENGTHS not, the triangle was closed with sides 1.25x short of its own hypotenuse and the elbow slid |
| 8 | "the two forearms are perfectly synced" | **`g_elbowPrev` held last frame's elbow in COMPONENT space.** Smoothing this frame's elbow against it while the actor had travelled in between dragged the elbow after the actor. The wrist is not smoothed, which is why it was immune - and a fixed wrist with a dragging elbow pivots the forearm |

### What each DOF is pinned to now (free arm)

| bone | roll pinned to |
|---|---|
| clavicle | authored; shoulder is head-relative (s70i) |
| upper arm | the pole |
| elbow / forearm | the pole |
| twist helper 22 | pole + 0.5 x hand twist |
| twist helper 23 | pole + 1.0 x hand twist |

The twist angle is measured about the forearm axis against the hand's
**actor-cancelled** orientation (`qtc`), which is what makes it independent of the
held hand rather than a correction applied afterwards.

### Fallout 4's FRIK named the fix

`rollingrock/Fallout-4-VR-Body`, `src/skeleton/Skeleton.cpp` (`setArms` /
`solveArmToHandWorldTarget`, algorithm credited to SkyrimVR's VRIK):

> *"For non-power armor, `forearm2` and `forearm3` receive opposing rotations
> based on computed wrist twist."* - the arm's roll is driven EXPLICITLY from the
> hand, never left free.
>
> *"Hand rotation is decoupled from arm solving."*
>
> *"Each arm solves independently via the `isLeft` parameter, with no cross-arm
> coupling in the solver logic."*

BS1 has the same bones: `patterns.h` records the sleeve as *"3 clavicle,
4 upperarm, 5 elbow, **22/23 forearm twist helpers**"*. This tree was writing
22/23 with the forearm's own swing and no wrist twist at all.

**Not yet ported, and worth having:** FRIK's shoulder reach offset
(`norm(shoulderToHand) * adjust * armLength * 0.08` - ours anchors the shoulder
rigidly) and its elbow constraints (law of cosines with hand-behind-body,
chest-crossing and arm-lift limits).

### MEASURED NEGATIVE: there is no script-level IK surface

`HavokSkeletalSystem.uc` is a **4-line stub** and the only IK class in the
decompiled tree is `AnimNotify_FootIK`. There is no skeletal controller to hook
from UnrealScript. **The bone array is the only lever** - do not go looking again.

### The two probes that did the work, and what skipping them cost

- **ARMHOLD** - reads the free arm's five bones back after the write. All five
  held to **0.0 UU / 0.1 deg across 41 samples** while the held hand was twisted
  every way, which killed the entire "something re-evaluates the arm" class in one
  run and sent the search back into the solve's own inputs, where the faults were.
- **FOREARM** - lifts the free elbow and wrist into WORLD. With the off hand held
  still: **wrist steady to 0.4 UU, elbow swinging 25 UU.** Every geometric input
  to the elbow was fixed, so the culprit had to be the only STATEFUL term - the
  smoothing. Found #8 in one run.

**Three whole-arm corrections were tried before the first probe was written and
all three made the coupling worse:** `inv(A_now) * A_capture`, its inverse, and
rebuilding the orientation in world. Three DIFFERENT shapes failing the same way
is not three wrong signs - it means the correction is aimed at the wrong thing.
Stop at the second and measure.

### Standing rules for this subsystem

1. **Anything derived per frame must be anchored in world or in the head frame,
   never left in component space.** That includes smoothed and remembered values -
   #8 was pure state, not geometry.
2. **Every rotational DOF must be explicitly pinned.** `quat_from_to` leaves the
   axial one free; a free DOF is a coupling waiting to be reported.
3. **The DrawScale divide applies to EVERYTHING written as a bone translation**,
   and to any length compared against one.
4. **Per-hand state must be per hand, and per-role state must be per role.** #1
   and #2 were one bank serving two roles.
5. **Measure before the third attempt.** Both probes above answered in a single
   run what several code changes could not.

## Session 73 (2026-08-30) - MAKING THE FREE ARM ROTATE: frames, gradients, and the hinge

Continues session 72, which decoupled the free arm's POSITION. This is the half
that made it rotate correctly, and it is signed off in the headset: *"it looks and
feels amazing"*.

### THE ONE-LINE SUMMARY

> **The held arm is CARRIED by the actor. The free arm is not, so everything the
> actor was doing for it implicitly has to be reconstructed explicitly - and each
> thing that was not reconstructed showed up as a coupling, a lock, or a pinch.**

Every fault below is one more thing the actor had been doing for free.

### THREE FRAMES, AND USING THE WRONG ONE COST THREE REGRESSIONS

This is the single most expensive lesson of the two sessions. "Not the actor" was
never the answer; there are THREE candidate frames and most of this subsystem
wants the third.

| frame | moves with the HELD hand | moves with the PLAYER | use it for |
|---|---|---|---|
| component (actor) | **yes** | yes | nothing that must stay put |
| world | no | **no** | nothing that must follow the player |
| **body** (`base[XYZ]` + body yaw) | no | yes | almost everything |

Measured failures, one per wrong choice:

- **elbow smoothing in COMPONENT space** -> the elbow dragged after the held hand;
  the forearm pivoted about a fixed wrist and the two forearms read as "perfectly
  synced" (s72u).
- **elbow smoothing in WORLD** -> running forward left the elbow behind while the
  shoulder and wrist travelled with the body, and the arm STRETCHED (s72w).
- **rig pole fixed in WORLD while its blend partner was body-relative** -> the
  elbow oscillated in lockstep with the right stick: *"it feels like right stick
  controls the placement"* (s72x).

s70i had already chosen the body frame for the shoulder and written down why. The
rule generalises: **anything derived per frame, including remembered and smoothed
values, belongs in the body frame unless there is a reason it does not.**

### A QUANTITY CANNOT BE MEASURED AGAINST SOMETHING THAT MOVES WITH IT

Two separate faults, same shape:

- `ElbowFollowWrist`'s follow half was the AUTHORED bend - a fixed direction - so
  it never followed a wrist at all. Rotating the off hand moved the hand and left
  the elbow, pinching the mesh between them. It now takes the free hand's own
  orientation (`qtc`), which is actor-cancelled (s73b).
- ...but the SAME vector was also the reference the segments' roll is pinned to,
  and the twist is the residual between that pinned forearm and the hand. Once
  the pole followed the wrist the residual collapsed and the forearm stopped
  rolling: *"the elbow pivots out, but it doesn't seem to rotate at all"*. The
  roll pin keeps a body-relative pole; the elbow position keeps the blended one.
  **Two jobs, two poles** (s73d).

### THE ELBOW IS A HINGE, SO IT CANNOT HOLD A TWIST STEP

The first twist gradient was 0.25 bicep / 0.50 forearm - a 0.25 STEP across the
elbow, up to 45 degrees of twist discontinuity at a joint with no twist axis. The
skin spanning it absorbs the whole step, which is the hourglass. Raising the
bicep alone would only have moved the pinch.

**Segments either side of a hinge must carry the same roll**, and the gradient
belongs inside the forearm, where the twist helpers exist because that is where a
real forearm pronates - radius over ulna, between elbow and wrist.

### UNWRAP BEFORE YOU SCALE

`atan2` returns `(-pi, pi]`, so the twist jumps 2pi at one point in the wrist's
travel. A full turn is INVISIBLE on a bone given the whole angle - which is why
the wrist end never flickered - but the gradient multiplies it by a FRACTION, and
a fraction of 2pi is not a full turn: the forearm snapped 180 degrees and the
bicep 90. ARMIK logged it flipping `+179.9 / -179.9`, which is how it was found.

**Any angle that will be scaled, blended or interpolated must be made continuous
first.** Carry it across frames by the shortest step and accumulate. That is
continuity, NOT smoothing - there is no lag - and the comment says so, because
the two look alike in a diff.

### EASE DERIVED JOINTS. NEVER EASE A JOINT THAT MUST MEET SOMETHING UNSMOOTHED

The twist was eased on the elbow's lane, copying FRIK. Wrong here: the twist
exists to make the forearm MEET THE HAND, and the hand tracks the controller
live. Easing one side of a junction guarantees a visible mismatch for the length
of every wrist movement - exactly when a wrist is looked at.

FRIK eases because its twist is an `asin` of a dot product, noisy near the poles.
Ours is an `atan2` of an already-stable orientation, so there is nothing to
denoise. The ELBOW keeps its easing: it is a derived joint with no counterpart to
match, which is the case easing is for.

### TWO BONES DO NOT SHARE AN AXIS CONVENTION

The twist was the raw angle between the forearm's local up and the hand's, and a
hand's local axes are not its forearm's. That difference baked in as a constant,
so the wrist never quite met the hand - small, identical on both hands, present
with NO trim dialled.

The helpers want the CHANGE in twist since the authored pose, so the same angle
is measured between the AUTHORED forearm and AUTHORED wrist and subtracted. It is
derived from capture data, so it is automatically right per weapon and per
plasmid with no constant to tune.

**And a trim must never be used to hide it.** A mirrored right-hand rotation
trim was seeded on the theory that the plasmid off hand needed the left's -206
roll; the tester reported the rotation already matched their controller untuned,
which meant the fault was downstream and the trim was masking it. Reverted. A
trim that compensates for a real fault is how the next session is lost - the same
way `ElbowFollowWrist` sat as a slider over a frame bug for three sessions.

### THE SCALE ARITHMETIC, WRITTEN OUT ONCE

A component-space vector `v` renders at `k*v`, where `k` is the rig actor's
DrawScale (0.8 on the shipped rig).

- A POINT that must land at a given world position is written `p/k`.
- A LENGTH compared against a distance between two such points is written
  **unchanged** - the authored segment renders at `k*|a|`, which in the divided
  space is `k*|a|/k = |a|`.

Dividing the lengths as well stretched every segment by 1/0.8, the forearm
overshot the hand, and the skin between the last helper and the wrist stretched
to cover it: *"like you have a super long wrist"*.

### THE SHARE IS 1.0 BECAUSE THAT IS WHAT THE HELD ARM DOES

The held arm gets no explicit twist at all - the twist block and `pinRoll` are
both `freeBank`-gated, and the held call site passes `handQ = nullptr`. Its whole
roll comes from the actor, which rotates fully with the held controller. So every
held-arm segment rotates 1:1 with that wrist: share = 1.0, no gradient.

`g_armTwistShare` therefore ships at 1.0 - a deliberate match, not a guess - with
`vrbones armtwist <0..1>` live for anyone who wants the bicep to roll less than
the wrist, which is what a real humerus does.

### STANDING RULES (extending session 72's five)

6. **Pick the frame deliberately: component, world, or body.** Body is usually
   right. Three regressions came from moving something out of a wrong frame into
   another wrong one.
7. **Never measure a quantity against a reference that moves with it.** If a pole
   both places a joint and defines the zero of an angle, it needs to be two poles.
8. **Segments either side of a hinge carry the same roll.** Gradients belong
   inside a segment, not across a joint that cannot twist.
9. **Unwrap any angle before scaling, blending or interpolating it.** A 2pi jump
   is invisible at weight 1.0 and a disaster at every other weight.
10. **Ease derived joints only.** Anything that must meet an unsmoothed thing must
    itself be unsmoothed.

### STILL OPEN: the two arms should be ONE implementation

`solve_arm()` is already shared - both hands call it - but its behaviour forks
internally on `freeBank` in six places: the twist gradient, the roll pins, the
smoothing frame, the reference bank, the shoulder mirror, and the DrawScale
divide. So the arms are one function and two behaviours, and every fix this
session had to be written for one of them.

That fork exists for a reason - the held arm is carried by the actor and the free
arm is not - but it is no longer a NECESSARY reason. Now that the free path
solves an arm explicitly and correctly from nothing but that hand's own
controller, it should reproduce the held arm too: the actor would still carry the
rig, but the bones get overwritten either way, so the result should be identical
while the code becomes one path.

**Do it as its own change with its own headset pass.** The held arm is the
signed-off, headset-accepted baseline, and unifying is exactly the kind of change
that trades a known-good half for elegance. The gate is: with `freeBank` forced
true for both hands, the held arm must be indistinguishable from today.
