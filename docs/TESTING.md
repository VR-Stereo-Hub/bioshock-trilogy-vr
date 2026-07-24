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
  thread and applies each line, logging what it applied. Camera commands: `fov <deg>`,
  `fov off`, `gfov <deg>` / `gfov off` (REAL game fov via the settings object - works past
  the options UI's 130 cap), `offset <x> <y> <z>`, `recenter`.
- **Write commands with** `.\tools\game-cmd.ps1 "<cmd>" ["<cmd2>" ...]` - it foregrounds the
  game first and retries past the poller's transient file lock. IMPORTANT: the poller runs
  inside the CalcView hook, which the engine pauses while the window is unfocused - after
  game-cmd, run a game-shot (it holds focus ~2.5 s) so the poll actually fires.
- **Stale command.txt re-applies at boot** (learned 2026-07-24 session 7: a leftover
  "reentry stereo on" armed hooks at the menu on a fresh launch). Delete
  `%LOCALAPPDATA%\BioshockVR\command.txt` (or overwrite it) before a launch that must
  start clean.
- **Window screenshots**: `.\tools\game-shot.ps1 -Out shot.png` captures the game WINDOW
  (PrintWindow, D3D content included), foregrounding it first - the game pauses its boot and
  its presenting while unfocused.
- **Pixel-diff A/B**: `.\tools\img-diff.ps1 -A a.png -B b.png` prints mean-abs-diff / max /
  pct-changed. Standing-still gameplay noise floor is ~0.4 mean; a real fov change reads 4-7.
  The menu attract scene moves constantly (mean ~3.8 across 5 s) - do A/B in a loaded save.
- **Memory discovery** (CheatEngine-style, all via the seam; results in the log):
  `memscan <f>` / `memscani <u>` full sweep, `memrescan`/`memrescani` narrow,
  `memlist [n]`, `memread <i>`, `mempoke <i>|<lo>-<hi> <v>` / `mempokei ...`, `memrestore`,
  `memptr <i> [maxDeltaHex]` (owning-pointer sweep), `pokeaddr`/`pokeaddri <hex> <v>`,
  `hexdump <hex> <len>`, `strscan <text>`, `membases`. The proven narrowing recipe: scan,
  have the value changed through the game's own UI, rescan, poke each survivor + screenshot
  + img-diff to find the consumed copy (see the HorizontalFOV row in ENGINE_NOTES).
- **Frame dumps (DR-3)**: `dumpframe` / `dumpframe full` (or the overlay buttons) writes
  `%LOCALAPPDATA%\BioshockVR\framedump_HHMMSS.txt` for the next full Present-to-Present
  frame: per-draw RTs/formats, viewports, VS b0 readback (full mode), callstack RVAs, and
  a draws-per-RT + stack-histogram summary. Never commit dumps (game-derived).
- **Reentry probe (DR-5)**: `reentry hook [build|submit|drain|flush]` / `reentry unhook`
  (MinHook the game-thread scene BUILD root (default - the SequentialReentry seam), the
  frame submit, the render-thread drain, or the flush - command-gated, default runs stay
  unhooked), `reentry on|off` / `reentry pulse` (double-call the hooked original every
  frame / once; SEH-guarded with a poison latch - `reentry reset` clears after a caught
  fault), `reentry yaw <deg>` (yaw delta for the second pass: the build slot applies it
  via CalcView's second-pass path, the submit slot on a copied rot arg),
  `reentry dump <n>` (per-call submit arg telemetry), `reentry arg3 <hex|off>`
  (double-submit call-site filter), `reentry status`, `reentry kick on|off` (process-wide
  SetEvent caller sampler), `reentry calcstack` (one-shot game-thread stack scan).
  1 Hz `[reentry]` heartbeat lines carry entries/s, presents/s, tids, call durations.
  WARNING learned 2026-07-24: a drain double-call faults (caught) but then WEDGES the
  render event protocol - the game hangs and needs a kill. Pulse only, expect to relaunch.
  WARNING 2026-07-24 session 6: continuous BUILD double-call (`reentry on` or
  `reentry stereo on`) DEADLOCKS the engine's command-queue event protocol
  stochastically - observed survival times 16 s to 3.5 min across five runs. Thread
  dumps (scratchpad hangdump.py, Wow64GetThreadContext from outside) show the same
  signature every time: game thread in WaitForSingleObject(INFINITE) at exe+0x61D38E
  (flag+event "render done" wait, called from build site 0x4CDCD7) while the render
  thread waits inside the drain at +0x30 - a lost-wakeup/event-theft deadlock that
  doubling provokes. Start-state gating does NOT fix it (frame-id gate, ring-counter
  gate both live-falsified - the race is inside the concurrent window). Pulses and
  short windows are safe-ish; kill + relaunch on hang; the game process is otherwise
  unharmed and saves are untouched. Fix directions in STATUS next steps.
- **Single-threaded render mode (`reentry 1t on`, session 7 - THE stereo
  substrate)**: pokes the engine's hardware-thread count global to 1 (see
  ENGINE_NOTES "Flush-point decision chain") so every scene flush drains INLINE on
  the game thread - no pump hand-off, no INFINITE render-done wait, the whole
  deadlock class structurally unreachable. The command arms the drain-hook
  empty-slot guard first (mandatory ordering - the guard eats any pump wake into
  consumed state, the old drain+0x33 crash). Verify with the heartbeat: `mode=1T`
  and beatTid == calcTid. `reentry 1t off` restores the original count. Costs
  ~20% mono fps (413/s vs 530/s in the save-spawn scene) - irrelevant next to the
  144 presents/s VR needs. SUPERSEDES the session-6 `-onethread` launch arg, which
  the remaster does not parse at all (menu-time verification artifact - the pump
  globals are always zero until the first world load).
- **Stereo substrate gate (session 7)**: `reentry stereo on` REFUSES to arm while
  the threaded renderer is live (deadlock + empty-wake crash substrate - all three
  2026-07-24 crash dumps). Run `reentry 1t on` first; `reentry stereo force`
  overrides for experiments only. Crash dumps land in
  `%LOCALAPPDATA%\BioshockVR\crash\` (never commit them; symbolize with the
  scratchpad minidump parser against the game + build PDB - method in STATUS
  session-7 log).
- **Watchdog**: `reentry stereo on` arms a detect-only deadlock watchdog (logs when
  the game thread is stuck >300 ms inside a hooked call with builds/presents frozen).
  `reentry wdkick on` additionally re-SetEvents engine events on detection - known to
  CRASH a desynced protocol (live 2026-07-24); experiments only.
- **Menu clicks**: `.\tools\game-click.ps1 -X <px> -Y <py>` clicks at window coordinates
  (read positions off a game-shot capture; gameswf menus accept the synthetic click).
  If a click highlights but does not activate a menu item (stale gameswf hover state),
  send Enter with `keybd_event` instead - the highlighted item activates on VK_RETURN
  (worked for CONTINUE both times it was needed, 2026-07-24).
- Loading the same save reproduces the same spawn viewpoint - good for A/B render comparisons.
- Caveat: the main-menu backdrop is a live level with a flying attract camera that LOOKS like
  gameplay (no HUD) - do not draw render conclusions there; its fov path differs (see
  ENGINE_NOTES).
- Debug-build gotcha: `sprintf_s` ASSERTS on buffer overflow with a MODAL dialog that freezes
  the game (hit 2026-07-24 formatting arbitrary heap floats). Use `_snprintf_s` + `_TRUNCATE`
  in any code that formats untrusted bytes.

## Quest 3 / Virtual Desktop setup

1. Quest 3: install Virtual Desktop (paid) from the Meta store; PC: Virtual Desktop Streamer.
2. Streamer → OpenXR runtime: **VDXR** (once DR-1 confirms 32-bit support; otherwise SteamVR).
3. Connect in VD, then launch the game on the desktop (VD's desktop view) - the mod's OpenXR
   session takes over the headset from M2 onward.
4. Steam Link alternative: SteamVR must be the active OpenXR runtime
   (SteamVR → Settings → OpenXR → "Set SteamVR as OpenXR runtime").

## RenderDoc workflow (FALLBACK only - DR-3 was done with the in-tree frame inspector)

- Only reach for RenderDoc if a question exceeds the in-tree `dumpframe` data (e.g. full
  shader disassembly). RenderDoc is NOT currently installed.
- RenderDoc x86 build (32-bit game!) → Launch Application → `BioshockHD.exe` with working dir
  `Build\Final` - verify our proxy still loads (log line) under RenderDoc.
- Captures go to `captures/` (gitignored - they contain game content; never commit).
- Deliverable of any capture session = updated "D3D11 frame map" section in ENGINE_NOTES.md.

## Crash triage

- Our minidump handler writes to `%LOCALAPPDATA%\BioshockVR\crash\` (once implemented - M0).
- The game's own crash handler uses dbghelp/MiniDumpWriteMap; check
  `%LOCALAPPDATA%\..\Roaming\BioshockHD\` and Windows WER (`%LOCALAPPDATA%\CrashDumps`).
- First suspects, in order: loader-lock violations in DllMain (we defer init - keep it that
  way), Present-hook reentrancy, MinHook targets moved by a game update (check the build line
  at the top of ENGINE_NOTES.md).
- The mod must always fail soft: if a scan/hook fails, log it and let the game run flat.
