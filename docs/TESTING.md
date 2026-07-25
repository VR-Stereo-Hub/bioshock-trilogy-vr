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

## Synthetic gamepad (M5 `vrinput`) - flat verification

The synthetic pad has two producers (OpenXR actions when a headset session is
FOCUSED, and self-expiring seam test slots) composed over whatever the real
XInput chain reports. Commands: `vrinput on|off|status`,
`vrinput test stick l|r <x> <y> [holdMs]` (raw -32768..32767),
`vrinput test trig l|r <0..255> [holdMs]`,
`vrinput test press <A|B|X|Y|LB|RB|START|BACK|LS|RS|DU|DD|DL|DR> [holdMs]`,
`vrinput test clear`. Holds self-expire inside the DLL (the seam polls at
1 Hz); test slots only feed the game while `vrinput` is ON. The enable is
STICKY across boots (marker file `%LOCALAPPDATA%\BioshockVR\vrinput.on`)
because the game's one-shot boot probe runs before the first seam poll.
Mid-session `vrinput on` works (the drive re-arms UseController and the
UpdateInput pump immediately); the marker just also covers the boot probe.

Verified flat procedure (2026-07-25, no physical pad connected):
1. Game closed -> `.\tools\build.ps1 -Install` (BOTH DLLs are locked while
   the game runs). Clear a stale `command.txt` before a controlled boot.
2. Launch flat. Log must show `input: bridge registered with proxy seam`,
   and on the first `vrinput on` frame
   `input: game import slot ... hijacked` + `[b1r] input drive: armed`.
3. Menu nav: `game-cmd "vrinput test press DD 150"` + game-shot - the
   gameswf highlight moves (a 400 ms hold auto-repeats ~2 steps; use ~150 ms
   for single steps). `press A` activates (CONTINUE loads the save), `press
   B` backs out. The menu also flips to Xbox prompt icons (UseController).
4. Gameplay: `vrinput test stick r 24000 0 1500` - the camera-loc heartbeat
   yaw changes during the hold and stops when it expires.
   `test stick l 0 32000 2000` - loc moves (NOT inside the bathysphere or
   other scripted vehicles - movement is locked there by design).
5. Passthrough: `vrinput off`, inject a stick - heartbeat must stay frozen,
   `vrinput status` shows the packet counter frozen and the real result
   (1167 = no pad) flowing again. `vrinput on` re-arms cleanly.
6. `vrinput status` lane counters tell you WHERE input flows: `iat` is the
   game's per-tick reads (healthy: ~2x present rate while armed), `proxy`
   stays at the ~6 boot-probe calls (the Steam overlay eats that lane -
   ENGINE_NOTES "Gamepad architecture"), xi14/xi13 are diagnostic hooks on
   the system DLLs.

## Decoupled aim (M6 `vraim`) - flat verification

Commands (all through the seam, so they work with no headset):

- `vraim probe on|off` - install the fire seams in telemetry mode;
  `vraim dump <n>` gives each seam n detailed log lines
  (`this`/vtable/out-params).
- `vraim on|off` - arm substitution (right hand aims weapons, left hand aims
  plasmids); `vraim status` prints seam counters, both rays and the learned
  object->hand map.
- `vraim test l|r <yawDeg> <pitchDeg> [holdMs]` - synthetic hand aim as an
  OFFSET from the current view rotation, feeding the exact slot the XR grip
  pose will. `vraim test clear` drops it. Holds self-expire inside the DLL.
- `vraim origin on|off` - hand origin + direction (default) vs direction only.
- `vraim seam weapon|ability on|off` - per-family substitution.
- **Investigation tools** (these are how the fire flow was mapped, keep them):
  `vraim scan <Class> <Func> [n]` hooks ANY name-based native read-only via the
  engine's own native table; `vraim scanimpl <rvaHex> <stackArgs 1..3> [n]`
  hooks any C++ implementation (arg count MUST match the target's `ret <n>`);
  `vraim scanoff` disables both. No rebuild per candidate.

Firing the game from the harness - the gotchas that cost a session:

1. **The first trigger pull only SWITCHES HANDS.** `XENON_RT =
   SwitchAndFireWeapon`, `XENON_LT = SwitchAndFireAbility` (ENGINE_NOTES
   "UnrealScript findings"), so a single `vrinput test trig l 255 400` looks
   like nothing happened. Send two pulls ~2 s apart (the seam polls at 1 Hz):
   the second one fires. Right mouse = switch hands, left mouse = fire the
   active hand.
2. **The wrench never traces** (Havok collision phantom), so no aim seam fires
   for melee no matter how it connects - not a bug, and not a useful control.
   The only save in the tree spawns with wrench + Electro Bolt; testing the
   WEAPON seam needs a ranged weapon (external trainer loadout).
3. Aim at something within range. A cast into open air still runs the ability
   fire-start seam; damage-side natives (`CanHit`, `InitiateDamage` on the
   weapon) only run on a hit.
4. After a force-kill the game shows a **"revert Options?" dialog** on the next
   launch and its window title is `Message`, not `Bioshock`; answer No by
   `BM_CLICK`-ing the `&No` button (scratchpad `boot2.ps1` does this). The game
   window can also report a 0x0 rect for a few seconds after it appears -
   retry `game-shot.ps1` instead of treating that as an error.

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
- **VR stereo one-toggle (`vrstereo on|off`, session 8 - the streamlined
  flow)**: sequences structural 1t + VR camera mode + SequentialReentry stereo,
  reversing on off. Sticky across loads. Reachable three ways: the top-level
  seam command `vrstereo on`, `reentry vrstereo on`, and the overlay "VR stereo"
  checkbox. Verify with `VRSTEREO READY` in the log and heartbeat `mode=1T` +
  `2nd=<pairs>/s`. This is what the in-headset checklist uses; the individual
  `reentry ...` commands below stay for debugging.
- **Structural single-threaded render (`reentry 1t on`, session 8 - THE stereo
  substrate, LOAD-SAFE)**: MinHooks the flush-point (0x61D260) and forces its
  decoded INLINE branch in the detour (see ENGINE_NOTES "Structural 1t") so
  every scene flush drains on the game thread - no pump hand-off, no INFINITE
  render-done wait, the deadlock class structurally unreachable - WITHOUT
  touching the hw-thread global, so loaders see the true core count. The command
  arms the drain-hook empty-slot guard first, then installs the flush-point
  hook. Verify with the heartbeat: `mode=1T`, `forced=<n>/s` climbing, and
  beatTid == calcTid. `reentry 1t off` returns the flush-point to the engine's
  own decision (hook stays installed, passive). **Load-safe and menu-safe
  (session-8 soaks: save load, quit-to-menu, new game, and the bathysphere
  descent all clean with 1t + stereo armed) - no off/on dance needed.**
  SUPERSEDES both the session-6 `-onethread` arg (not parsed) and the session-7
  numerator poke.
- **Legacy poke (`reentry 1tpoke on|off`, session 7 - NOT load-safe)**: pokes
  the hw-thread count global to 1. Kept as a fallback/diagnostic only. The
  global has load-path consumers, so the poke crashes the loader across a save
  load / level transition (session-7 19:54 dump): `1tpoke on` refuses at the
  menu and warns to `1tpoke off` before any load. Prefer `reentry 1t` (the
  load-safe hook).
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
