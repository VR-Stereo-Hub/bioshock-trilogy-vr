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
| Frame root RVA 0x61D0F0 (+ drain 0x61CAE0 fallback) | DR-5 reentry probe -> SequentialReentry stereo | **command-gated** (session 5: `game/bioshock1r/scenedraw.cpp`; exists only after a `reentry hook` seam command - default runs stay unhooked) |
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

**Scene-draw architecture (DR-5 groundwork) - the renderer is a COMMAND QUEUE:**
byte-level walk (hexdump seam, prologue + RET analysis, 2026-07-24) of the functions
behind the draw stacks:

| RVA | Role | Evidence |
|---|---|---|
| **0x61C8E0** | render-command **executor**: `void __thiscall`, zero stack args (plain `C3` ret at 0x61CAA4); reads a command type id at `this+0xC` (an observed `cmp ecx,6` dispatch), virtual-calls per type. Its dispatch call-site cluster (ret RVAs 0x61C931..0x61CA87) appears in **84/86** main-scene draw stacks and in menu stacks. | first-1024-bytes disasm-by-hand |
| **0x61CAE0** | command-queue **drain loop**; its `call 0x61C8E0` site returns to **0x61CD0D** (84/86 stacks) | prologue walk + stack histogram |
| **0x61D0F0** | **frame root**: class check (vtable VA cmp -> RVA 0xE2D584), `[obj+0x58]==0` guard, then `call 0x61CAE0` at 0x61D219 returning to **0x61D21E** (terminal frame of every stack, menu + gameplay) | raw bytes: `E8 C2 F8 FF FF` rel32 == exactly 0x61CAE0 |

**Implication for SequentialReentry:** double-calling the drain (or executor) re-renders
NOTHING - the queue is already consumed. The re-entry seam must be the command BUILD
(scene traversal / camera consumption) upstream of or inside 0x61D0F0 before the drain
call. Next probe: command-gated hook on 0x61D0F0 - count CalcView invocations inside it
(answers where view sampling happens), then locate the build-vs-drain boundary. Neither
function is hooked yet. Derivation recipe if the build changes: `dumpframe full`,
histogram stack RVAs over the biggest depth-tested RT's draws, prologue-walk the cluster.

**Frame-root convention + structure (session 5, live hexdump walk; constants in
`patterns.h`):**

- Prologue `55 8B EC 51 A1 90 65 6D 11` - standard, first patchable boundary at 9 bytes
  of complete position-independent instructions; hookable, and byte 0 was clean (no
  foreign hook). Build identity re-verified: the class-check `cmp eax, imm32` immediate ==
  base+0xE2D584, drain call `E8 C2 F8 FF FF` at +0x129 (0x61D219).
- **Zero stack args, two exits, both plain `C3`** (epilogue `5E 8B E5 5D C3` at +0x14D and
  +0x15D) -> the dummy-EDX `__fastcall(self, edx)` detour shape is exact. The next
  function (0x61D260) is unrelated (2 args, `ret 8`).
- **The root never reads ECX.** All state comes from a static render-manager global at
  RVA **0x1356590**: `mgr = [global]`, queue object = `[mgr+4]`; the prologue stamps
  `[mgr+0x58] = 1`; the drain call is guarded by `cmp [queue+0x58], 0` (skip-drain
  latch - the reentry probe's `latchclear` target).
- The drain call is bracketed by `push -1; push [obj+4]; call [IAT 0xBCF130]` pairs -
  resolved live to **kernel32!WaitForSingleObject** (INFINITE waits on event handles held
  by class-checked objects of the same vtable). Smells like cross-thread queue handoff
  (double-buffered build/drain); the probe's thread-id telemetry decides.
- Drain 0x61CAE0: SEH-frame prologue `55 8B EC 6A FF 68 ...` (`mov ebx, ecx` = __thiscall),
  zero stack args by call-site evidence (`mov ecx, esi; call` with no pushes and no stack
  fixup after). Hookable; used by the probe as a pass-through entry counter / crash
  fallback only.

**DR-5 probe tooling (session 5):** `game/bioshock1r/scenedraw.{h,cpp}`, all
command-gated via the seam (`reentry hook|unhook|on|off|pulse|yaw <deg>|latchclear
on|off|reset|status`). Detour telemetry at 1 Hz: root entries/s, presents-per-root,
CalcView-inside-root vs outside (tid-attributed), both original-call durations (a ~0 us
second call = the [queue+0x58] early-out), draw-census delta of the second call, distinct
caller RVAs, drain entries. Second original call is SEH-guarded with a poison latch;
CalcViewDetour runs only original+yaw-delta during the second pass.

**HUD fingerprint (partial):** menu frames are pure gameswf - only SetRT ping-pong
between T0-like LDR targets and NO depth-tested draws; in-game HUD draws land on the
LDR target after the scene passes with no DSV bound. Good enough to segregate scene vs
HUD for M9; exact shader/SRV fingerprint deferred until the HUD-capture milestone.

## UnrealScript findings

_(Summaries only - never paste decompiled code. Tooling: UE Explorer/UELib on
`Build\Final\BakedScripts\pc\*.u`, workspace in `tools/uscript/` (gitignored).)_
