# Testing

## Install & launch loop

1. `.\tools\build.ps1 -Install` (or `.\tools\install.ps1` for already-built DLLs) - copies
   `xinput1_3.dll` + `bioshockvr.dll` into
   `K:\SteamLibrary\steamapps\common\BioShock Remastered\Build\Final\`, backing up any existing
   `xinput1_3.dll` first.
2. Steam → BioShock Remastered → Properties → Launch options: `-allowconsole` (for the Tab console).
3. Launch through Steam (or `steam://rungameid/409710`).
4. In another terminal: `.\tools\tail-log.ps1` - follows
   `%LOCALAPPDATA%\BioshockVR\bioshockvr.log`.
5. `.\tools\uninstall.ps1` restores the game folder to stock.

## Per-milestone smoke checks

- **M0**: log contains the init banner + D3D device/swapchain info lines; F10 toggles the
  ImGui overlay in-game; game plays normally with the mod installed; no crash on exit.
- **M1**: each DR row in ROADMAP.md has a written answer in ENGINE_NOTES.md/STATUS.md.
  DR-4 check: camera visibly wobbles when the test toggle is on.
- **M2**: in-headset "big screen" with head tracking; desktop mirror still works.
  Procedure: connect the Quest 3 in Virtual Desktop (before or after launching the game - the
  mod retries every 5 s), then watch the log for `xr: session state READY` ->
  `xr: session running` -> `xr: first frame submitted`. In the headset: game screen on a fixed
  quad (look around it), F10 overlay visible on the screen, "Screen distance/width" sliders
  move/resize it. Expect game fps clamped to the headset refresh while the session runs; the
  "VR enabled" checkbox drops back to flat/uncapped.
- **M3**: lean around a corner; no drift after 10 min; roll matches head tilt.
- **FOV swim calibration (M3/M4 distortion)**: center-stretch that relaxes toward the periphery
  while turning the head = claimed-vs-rendered fov mismatch (the readback echoes our own write
  to PC+0xE0, so it cannot see renderer-side reinterpretation - vfov? 4:3-referenced? clamped
  downstream?). Procedure: set the game resolution to the one you play at FIRST (fov semantics
  can be aspect-dependent). Camera mode on, AlternateEye OFF, "Force headset FOV" ON. Enable
  "Manual claimed FOV", face a doorframe, rotate the head slowly and adjust "Claimed hfov"
  until objects stop growing/shrinking/warping as they cross from center to edge. Record the
  (readback, locked claim) pair from the layer line. Toggle "Force headset FOV" OFF and repeat
  for a second pair. The two pairs determine the engine fov mapping to bake into the adapter.
  Note whether any residual warp is vertical-only (would need a second claimed-vfov axis).
- **M4 rung 1 (AlternateEye)**: BEFORE anything else raise the game resolution above 1024x768
  (headset sharpness). Connect VD, enable "VR camera mode", confirm the overlay `layer:` line
  reads "projection". Enable "AlternateEye stereo test (judders)" - the layer line gains an
  `(AER eye L/R)` tag flickering per frame. Acceptance: wrench/railings/doorframes show REAL
  parallax (close one eye at a time: the two views differ). Half-refresh judder per eye is
  EXPECTED - this rung proves geometry, not comfort. If depth feels inside-out, toggle "Swap
  eyes (inverted-depth test)" and report which way felt right. Then calibrate: IPD slider
  (55-75 mm) until scale feels natural at arm's length, World scale until room-scale head
  movement matches the world (the "head offset" UU readout now shows the applied number live).
- **M4 (full)**: close-range parallax on the wrench; 30 min without visual corruption; fps ≥ 72
  at default renderScale.
- **M5**: full intro (plane crash → Rapture arrival) playable from the headset only.
- **M6**: aim decouple - look left, shoot right, impacts follow the controller.
- **M7/M8/M9**: hands track convincingly; wheels switch weapon+plasmid; HUD quad visible in stereo.

## Automated in-game testing (no human in the loop)

- **Command seam**: the mod polls `%LOCALAPPDATA%\BioshockVR\command.txt` at 1 Hz on the game
  thread and applies each line, logging what it applied. Commands: `fov <deg>`, `fov off`,
  `offset <x> <y> <z>`, `recenter`. Write the file, wait ~2 s, confirm via the log.
- **Window screenshots**: `.\tools\game-shot.ps1 -Out shot.png` captures the game WINDOW
  (PrintWindow, D3D content included), foregrounding it first - the game pauses its boot and
  its presenting while unfocused.
- **Menu clicks**: `.\tools\game-click.ps1 -X <px> -Y <py>` clicks at window coordinates
  (read positions off a game-shot capture; gameswf menus accept the synthetic click).
- Loading the same save reproduces the same spawn viewpoint - good for A/B render comparisons.
- Caveat: the main-menu backdrop is a live level with a flying attract camera that LOOKS like
  gameplay (no HUD) - do not draw render conclusions there; its fov path differs (see
  ENGINE_NOTES).

## Quest 3 / Virtual Desktop setup

1. Quest 3: install Virtual Desktop (paid) from the Meta store; PC: Virtual Desktop Streamer.
2. Streamer → OpenXR runtime: **VDXR** (once DR-1 confirms 32-bit support; otherwise SteamVR).
3. Connect in VD, then launch the game on the desktop (VD's desktop view) - the mod's OpenXR
   session takes over the headset from M2 onward.
4. Steam Link alternative: SteamVR must be the active OpenXR runtime
   (SteamVR → Settings → OpenXR → "Set SteamVR as OpenXR runtime").

## RenderDoc workflow (DR-3, M4)

- RenderDoc x86 build (32-bit game!) → Launch Application → `BioshockHD.exe` with working dir
  `Build\Final` - verify our proxy still loads (log line) under RenderDoc.
- Captures go to `captures/` (gitignored - they contain game content; never commit).
- Deliverable of any capture session = updated "RenderDoc frame map" section in ENGINE_NOTES.md.

## Crash triage

- Our minidump handler writes to `%LOCALAPPDATA%\BioshockVR\crash\` (once implemented - M0).
- The game's own crash handler uses dbghelp/MiniDumpWriteMap; check
  `%LOCALAPPDATA%\..\Roaming\BioshockHD\` and Windows WER (`%LOCALAPPDATA%\CrashDumps`).
- First suspects, in order: loader-lock violations in DllMain (we defer init - keep it that
  way), Present-hook reentrancy, MinHook targets moved by a game update (check the build line
  at the top of ENGINE_NOTES.md).
- The mod must always fail soft: if a scan/hook fails, log it and let the game run flat.
