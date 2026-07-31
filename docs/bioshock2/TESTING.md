# Testing - BioShock 2 Remastered

BS2-specific install/launch/harness notes. The general harness documentation (command seam
gotchas, img-diff noise floors, memory-discovery commands, crash triage, Quest 3 / Virtual
Desktop setup) lives in [../bioshock1/TESTING.md](../bioshock1/TESTING.md) and mostly transfers;
this file records what differs.

## Install / launch

```powershell
.\tools\build.ps1 -Game bs2 -Install          # build + install to the BS2 folder
.\tools\install.ps1 -Game bs2 [-Release]      # copy already-built DLLs
.\tools\uninstall.ps1 -Game bs2
.\tools\tail-log.ps1 -Game bs2                # follow the BS2 log
```

- Game: `D:\SteamLibrary\steamapps\common\BioShock 2 Remastered\Build\Final\Bioshock2HD.exe`
  (D: drive; BS1 is on K:). Steam appid **409720**; launch via Steam or
  `Start-Process steam://rungameid/409720`.
- Data dir is **`%LOCALAPPDATA%\BioshockVR\bs2\`** (log, prev log, command.txt, crash dumps).
  BS1 keeps the flat `%LOCALAPPDATA%\BioshockVR\` layout - the two games never share files, so a
  BS2 session can never clobber BS1's calibration.
- `boot.ps1` is BS1-only. BS2's gameswf main menu largely IGNORES synthetic clicks and
  activation keys from the harness (arrow keys move the selection, Enter did not reliably
  activate in session 24) - drive the menu by hand, or ask the user. NEW GAME first-boot flow
  also inserts a Graphics Options confirm screen.

## Harness

`game-cmd.ps1 / game-shot.ps1 / game-click.ps1` all take `-Game bs2` (process `Bioshock2HD`,
bs2 command.txt). Same focus rule as BS1: the game pauses unfocused, so follow `game-cmd` with a
`game-shot` (holds focus ~2.5 s) when the poll does not land.

Differences from BS1:

- The command poller runs off the **ProcessEvent detour**, not the CalcView tail - so commands
  work at the main menu too (BS2's menu never runs PlayerCalcView).
- The 1 Hz heartbeat logs the **FINAL camera** (post drive + offsets):
  `[b2r] camera: loc=(...) rot=(...) fov=N headOff=(...) drive=0|1 (N calls/s)` - flat checks
  measure these numbers directly (`fov=` is the live HorizontalFOV option, 0 until the settings
  object is located).
- `simhead` takes an optional **position triple** (meters, XR space):
  `simhead <yaw> <pitch> <roll> [px py pz] [holdMs]` - proves the full 6DOF mapping flat, which
  BS1's rotation-only lane could not.

## M3 flat acceptance (all PASSED 2026-07-29, session 24 - log-measured)

| command | expected in heartbeat |
|---|---|
| `offset 0 0 50` | loc z exactly +50.0 UU |
| `simhead 30 0 0` (arming) | `vr camera recentered (yaw 30.0 deg)`, pitch/roll -> 0, drive=1 |
| `simhead 0 20 0` | pitch 3640 (absolute), yaw shifts by the -30 deg residual = -5461 units |
| `simhead 0 0 15` | roll 2730 |
| `simhead 0 0 0 0.10 0.20 -0.30` | headOff z +20.0, magnitude 37.4 UU at worldscale 100 |
| `worldscale 50` | headOff halves exactly |
| `worldscale 100` + `simhead off` + `offset 0 0 0` | camera returns to game values, drive=0 |

Command vocabulary: `recenter`, `offset x y z`, `worldscale v`, `headoff up fwd`,
`simhead ...`, `vrcam on|off`, `camlog on|off`, `vroverlay on|off`, `vrcine ...`; since
session 25 also the FOV levers (`vrfov on|off|status`, `gfov <deg>|off`, `fovaudit`) and the
full discovery family (`memscan(i)/memrescan(i)/memlist/memread/mempoke(i)/memrestore/memptr/
pokeaddr(i)/hexdump/fsweep/strscan/membases/dumpframe`, plus the b2r-first
`vtscan <hexRva> [needBytesHex]` candidate-vtable verifier - one-shot, walks the full 4 GB,
~3 s Debug in gameplay, EXPECTED to freeze the game for that long). Since session 26 also
the stereo family: `vrstereo on|off` (one toggle: camera mode + stereo - NO 1t rung on
this game) and `reentry vrstereo|stereo|pulse|on|off|yaw|reset|hook [draw|stream]|unhook|
dump <n>|kick on|off|kick2 on|off|calcstack|status`. Still unported from BS1: vraim/
vrhands/vrbones/exec.

## Stereo flat acceptance (session 26 - all PASSED, log-measured; stereo-only policy)

| step | expected |
|---|---|
| `reentry hook` in gameplay | `UGameEngine::Draw chain VERIFIED` + `draw hook ENABLED`; beat line shows `draws/s == presents/s`, `calc in == draws`, single caller `CD5D7B` |
| `reentry pulse 3` | 3x `second draw ok` (call2 ~5 ms vs ~0.7 ms pass-through); that beat shows `presents == draws + 3` |
| `reentry on` (yaw 30) | `2nd/s == draws/s`, `presents/s == 2x draws/s`, tick halves; `reentry off` recovers 1:1 instantly. Do NOT expect a yawed flat screenshot - PrintWindow catches the FIRST present of each pair on this game |
| `reentry stereo on` | beat `2nd/s == draws/s`, `presents == 2x`; 1 Hz `[b2r] sr eyes: ... |d|=6.30 UU (expect 6.30)` - the per-eye delta must equal ipd/1000 x worldScale |
| `reentry status` | `2ndHits == seconds`, `skips=0 foreign=0 poisoned=0` in steady gameplay |
| `vrstereo on` | `VRSTEREO READY` (camera mode + stereo sequence) |
| load crossing (user drives the menu) | with vrstereo armed: quit-to-menu / reload - zero faults, `foreign` may tick (loader draws skipped by the caller gate), stereo re-engages in gameplay without re-arm |

## In-headset stereo checklist (session 26 - the M10 depth acceptance)

1. Quest 3 + Virtual Desktop (VDXR), launch BS2, load a save.
2. `vrstereo on` (overlay "VR stereo" checkbox in the BS2 reentry section, or
   `.\tools\game-cmd.ps1 -Game bs2 "vrstereo on"`). Log must say `VRSTEREO READY`.
3. Depth check: nearby geometry (railings, the drill) must read at DIFFERENT depths
   than the far wall - real parallax, not a flat screen. If everything fuses but feels
   inside-out/eye-swapped, note it (core has a swap-eyes diagnostic).
4. Head motion comfort: look around + lean - the SR pair pacing (one xrWaitFrame per
   L/R pair) ships ON; report any "eyes feel weird" shear on head turns (BS1 session-7
   symptom; the fix is already active, this verifies it transfers).
5. World scale: NOW judge it (mono could not show scale) - tune the overlay
   "World scale" slider until the drill/hands/architecture read life-sized, note the
   number (BS1 calibrated 100).
6. IPD slider (overlay): verify it visibly changes eye separation.
7. Esc pause + resume: quad screen on pause, stereo re-engages on resume.
8. Quit to menu, CONTINUE back in: stereo must re-engage without re-arm, no crash.
9. Report: depth correct? eye swap? comfort on head motion? world-scale number?
   pause/load edges clean? fps feel (expect the tick to halve - ~100 pairs/s in the
   Adonis spawn flat).

## FOV flat acceptance (session 25 - log + img-diff measured)

| step | expected |
|---|---|
| boot to menu | `UShockUserSettings scan: N vtable match(es), chosen=<heap addr>` one-shot; heartbeat gains `fov=100` (the option value) |
| `fovaudit` | `option=<slider value>`, option-derived tanH == tan(option/2); with an XR session up, `src=readback` and submitted == option-derived; without one, `src=none swap=0x0` |
| user changes the slider in Graphics Options | heartbeat `fov=` and `fovaudit` follow, no rescan (cache revalidates) |
| `gfov 120` in gameplay | `game fov write ON (saved option N)`; heartbeat `fov=120`; visibly wider render |
| `gfov off` | `game fov write OFF (restored option N)`; options UI still shows the user's value afterwards |
| Esc to pause menu with a write armed | restore within ~0.5 s: the OFF edge (menu still CalcViews) or the stale-restore line (`calcview silent`) |
| `vrfov on` flat (no XR session) | no write (`suggested_hfov` is 0 without a session) - the status form says so |

## In-headset M3 checklist

1. Quest 3 + Virtual Desktop (VDXR), launch BS2, Continue into a save.
2. F10 overlay -> BS2 section -> "VR camera ON (enable + camera mode)"
   (or `.\tools\game-cmd.ps1 -Game bs2 "vrcam on"`).
3. Look around: yaw/pitch/roll track 1:1, no lag. MONO expected (no depth) - stereo is a later
   M10 rung.
4. Lean/crouch: full 6DOF translation.
5. Recenter from the overlay: horizon level, forward = game forward.
6. Tune world scale (BS1 shipped 100 UU/m) and head offset up/fwd.
7. Esc pause menu: drops to the flat big screen, returns to VR camera on resume.
8. Distortion/fisheye: since session 25 there is a live FOV readback, so this should be GONE
   with `vrfov` off (see the FOV checklist below). If fisheye is back, check `fovaudit` first.

Verify from the log, not from a flat mirror screenshot: `view state: GAMEPLAY (ShockPlayer
view)`, heartbeat `drive=1`, and moving `headOff` values are the acceptance signals.

## In-headset FOV checklist (session 25 acceptance: fisheye + world-drag gone)

1. Quest 3 + Virtual Desktop (VDXR), launch BS2, load a save, `vrcam on` (overlay or command).
2. `fovaudit` -> `src=readback`, submitted tanH == option-derived tanH (the honest claim).
3. With `vrfov` OFF (the default): look around and at straight edges - **fisheye GONE, no
   world-drag on head turns** (the world holds still). The image will not fill the headset's
   full FOV - that is CORRECT at the game's option FOV (Quest 3 wants ~104+, the option
   default is 100).
4. `vrfov on` (command or the "Force headset FOV" checkbox): log shows
   `game fov write ON (saved option N)`; the image now fills the headset FOV, still
   undistorted, still no world-drag.
5. Viewmodel check while `vrfov` is on: drill/weapon/hands look correctly proportioned (BS2's
   fg follows the world FOV natively - flat-verified session 25; this step is the in-headset
   confirmation. Only if the viewmodel breaks HERE does any BS1 fg machinery get considered).
6. Esc to pause: `game fov write OFF (restored option N)` in the log; resume re-arms the write.
7. Quit to the main menu, open Graphics Options: the FOV slider shows YOUR original value
   (save/restore leak check). While there, note the slider's min/max endpoints - they are
   still unrecorded in ENGINE_NOTES.
8. Report: fisheye gone? world-drag gone? viewmodel correct with vrfov on? slider endpoints?

## Resolution (`vrres`, session 32)

**BS2's lever is `Shared.ini`, not the file BS1 uses.** See ENGINE_NOTES section 1 - the
`[WinDrv.WindowsClient]` keys exist on BS2 and are ignored.

```
vrres                 # status: Shared.ini (governs) | Bioshock2SP.ini (synced) | live backbuffer
vrres 2048x2048       # or `vrres 2048 2048`; takes effect on the NEXT launch
```

Acceptance, end to end - **the only acceptance that counts is the backbuffer**, because the write
verifies its own read-back even when the engine ignores it:

1. `vrres 1600x1200` -> expect `viewport set to 1600x1200 in Shared.ini [SharedOptions] (verified)`
   and, on the first write, two `original backed up to ...bvr-bak-res` lines.
2. Relaunch. Grep the log for the startup line: `first Present: backbuffer 1600x1200`.
   **If it says something else, the write did not take effect regardless of what `vrres` logged.**
3. Safety check: `diff Shared.ini.bvr-bak-res Shared.ini` should show ONLY `ViewportX`/`ViewportY`,
   and the Bioshock2SP.ini diff ONLY the four keys in `[WinDrv.WindowsClient]` (lines 471-474).
   Any change inside `[XeDrv.XenonClient]` (509), `[PS3Drv.PS3Client]` (541),
   `[DurangoDrv.DurangoClient]` (573) or `[OrbisDrv.OrbisClient]` (605) is a section-scoping bug.

**WARNING - do not set a square resolution on BS2.** At 2048x2048 the scene renders into a
2048x1421 viewport with a black band across the bottom ~30%, and the projection degenerates
(horizontal collapses 100 -> 67.7 deg). This is a real BS2 difference from BS1, where square is the
RECOMMENDED VR setting. Until the aspect bisection in ENGINE_NOTES section 2 is done, stay at 16:9
and use the lane for sharpness (e.g. 2560x1440, 3840x2160).

## Lens / FOV measurement (session 32)

```
dumpframe full 2                                    # in GAMEPLAY - the menu has no world pass
tools\decode-framedump.ps1 -Path <dump> -ScanLayout -RayOffset 16 -FgBakeRvas @() `
    -OptionFov 100 -Aspect 1920x1080
```

- `-RayOffset 16` is BS2's (BS1's is 12, the default). `-FgBakeRvas @()` because the fg-bake RVAs
  are BS1-only.
- `-ScanLayout` brute-forces every offset and validates the structural signature - use it to
  re-derive after a game patch. **Derive at 16:9**: at square aspect the check fails for a real
  reason (the projection is degenerate there), which reads as "no layout found".
- `-Diff <other dump> -DiffFovs 100,130` compares two dumps taken at different FOV options and
  flags the floats that scale as `tan(fov/2)`. It assumes NOTHING about layout - the instrument to
  reach for when `-ScanLayout` finds nothing.
- `fovaudit` prints the live watch's verdict including `lenses=`. On BS2 expect **`lenses=2`** in
  gameplay: a world lens following the FOV option, and a fixed 60 deg one that does not.

## Frozen engine pitch (session 32) - IN-HEADSET CHECK STILL OWED

The fix (`publish_pitch_error` before the `rot->pitch` overwrite) is in, but its SIGN is unverified
because a meaningful `pitchErr` needs the HMD actually driving - flat, with `drive=0`, it is 0 by
construction.

1. In VR, gameplay, `camlog on`. The heartbeat now prints `enginePitch=<units> pitchErr=<deg>`.
2. Look up and down for ~30 s. **`enginePitch` must MOVE.** If it parks at one value while `rot`
   changes, the servo is not steering and the drill will aim where the engine last believed.
3. `vrinput pitchservo status` (now reachable on BS2 - it was not before this session).
4. If the view fights you or the error grows instead of shrinking, the sign is inverted:
   `vrinput pitchservo invert`.
5. Drill an enemy while looking DOWN - the BS1 symptom was hits landing on the floor.
6. Residual: BS1 settles at 4-8 deg because the proportional term falls under the game's own
   deadzone. **BS2's deadzone is its own - measure it, do not assume BS1's number.**
