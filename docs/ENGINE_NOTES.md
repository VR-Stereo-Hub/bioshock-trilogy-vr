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
  required). It struck during a SetForegroundWindow cycle - focus transitions pause
  presenting and may race the doubled event protocol (unproven; alternatively a rare
  stochastic lost wakeup). Production per-eye pacing must harden this: candidates are
  waiting on the render-done event between the paired builds, or gating the second
  build on the submit head's frame-consumed state instead of racing it.
- Phase note: PrintWindow captures consistently caught the SECOND (yawed) present of
  each pair - the pair's present order appears deterministic, which is promising for
  per-present eye attribution in the per-eye split.

**DR-5 probe tooling (sessions 5-6):** `game/bioshock1r/scenedraw.{h,cpp}`, all
command-gated via the seam (`reentry hook [build|submit|drain|flush]|unhook|on|off|
pulse|yaw <deg>|dump <n>|arg3 <hex|off>|latchclear on|off|reset|status|kick on|off|
calcstack`; default hook target = build). 1 Hz heartbeat: build/submit/drain/flush
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
