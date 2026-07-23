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
| PlayerController FOV (live) | `PlayerController + 0xE0` (float, degrees) | BioshockHD.exe | 2022-04-13 | itsloopyo `FOV_LIVE_OFFSET`; `kFovLiveOffset` in `patterns.h` | read verified live (100.0 at main menu); per-frame write wired, visual confirm pending | 2026-07-23 |

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
| Scene-draw entry (unknown - find via RenderDoc callstack, DR-3) | SequentialReentry stereo | M4 |
| GetPlayerViewPoint-equivalent used by fire traces (unknown) | decoupled controller aim | M6 |
| Console-command dispatcher (unknown - FName-chain on command strings) | execConsole one-liners | M5/M6 |
| XInputGetState (we ARE the proxy - no hook needed) | synthetic gamepad | M0 shim |
| DINPUT8 / WM_* (DR-6 decides) | virtual mouse for gameswf menus | M5 |

## Config / ini facts

- `[Engine.Console]` `ConsoleKey=9` (Tab). Launch option `-allowconsole`. Mixed reports on
  newest builds - verify (see STATUS blockers).
- `[Engine.RenderConfig]` `HorizontalFOVLock=True` - FOV control likely needs the live memory
  write (PC+0xE0) or `SetFOV` console command; there is no user FOV ini.
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

## RenderDoc frame map

_(DR-3 - to be filled: pass order, scene color/depth RTs + formats, gameswf HUD draw
fingerprint, view/proj constant-buffer slot + layout, scene-draw callstack.)_

## UnrealScript findings

_(Summaries only - never paste decompiled code. Tooling: UE Explorer/UELib on
`Build\Final\BakedScripts\pc\*.u`, workspace in `tools/uscript/` (gitignored).)_
