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
  `[b2r] camera: loc=(...) rot=(...) headOff=(...) drive=0|1 (N calls/s)` - flat checks measure
  these numbers directly.
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

M3 command vocabulary: `recenter`, `offset x y z`, `worldscale v`, `headoff up fwd`,
`simhead ...`, `vrcam on|off`, `camlog on|off`, `vroverlay on|off`, `vrcine ...`. BS1's wider
vocabulary (memscan family, vrstereo, vraim, reentry, exec) is NOT ported yet.

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
8. Report distortion/fisheye: EXPECTED at M3 - there is no BS2 FOV readback yet, so the
   projection layer claims the headset FOV while the game renders its own (the documented next
   M10 step; note BS2 has a native FOV slider in Graphics Options).

Verify from the log, not from a flat mirror screenshot: `view state: GAMEPLAY (ShockPlayer
view)`, heartbeat `drive=1`, and moving `headOff` values are the acceptance signals.
