# Testing - BioShock Infinite

Infinite-specific install/launch/harness notes. The general harness documentation (command seam
gotchas, img-diff noise floors, memory-discovery commands, crash triage, Quest 3 / Virtual Desktop
setup) lives in [../bioshock1/TESTING.md](../bioshock1/TESTING.md) and mostly transfers. This file
records what differs, plus the one rule that is unique to this project.

---

## RULE 0: never run Infinite while BioShock 2 is running

BioShock 2 development runs **in parallel** with this project (user directive, 2026-07-31
session 34). Only one game can own the headset at a time. Starting or driving Infinite while BS2
is live steals the headset mid-test and invalidates whoever was already using it.

**Before any test that launches or drives the game:**

```powershell
Get-Process Bioshock2HD -ErrorAction SilentlyContinue
```

If that returns anything, **wait or postpone the test**. Do not close the other game.

This is enforced, not just documented. `tools\lib\assert-no-conflict.ps1` is dot-sourced by the
scripts that drive a running game, and they abort with the offending process name and pid.

| script | guarded? | why |
|---|---|---|
| `game-cmd.ps1 -Game bsi` | **yes** | foregrounds and drives the game |
| `game-shot.ps1 -Game bsi` | **yes** | foregrounds the game to capture it |
| `game-click.ps1 -Game bsi` | **yes** | foregrounds and clicks into the game |
| `build.ps1`, `install.ps1`, `uninstall.ps1`, `package.ps1` | no | touch the disk, never the headset - these must keep working while BS2 runs |
| `tail-log.ps1` | no | read-only file tail |
| `check-laa.ps1`, `decode-framedump.ps1`, `img-diff.ps1` | no | offline |

Pass `-Force` to override, which is only ever correct for a desktop-only comparison with no
headset involved.

The guard is currently wired into the `-Game bsi` paths only. The check itself is general (it
refuses if *any* other known BioShock is running), so the BS1/BS2 scripts can adopt it later; it
is deliberately not wired into them yet so this branch cannot disturb a parallel BS2 session.

*Verified live 2026-07-31: with BS2 running (pid 24588), `Assert-NoConflictingGame -Game bsi`
refused and named the process. This was a genuine conflict, not a simulation.*

---

## Install / launch

```powershell
.\tools\build.ps1 -Game bsi -Install          # build + install to the Infinite folder
.\tools\install.ps1 -Game bsi [-Release]      # copy already-built DLLs
.\tools\uninstall.ps1 -Game bsi
.\tools\tail-log.ps1 -Game bsi                # follow the Infinite log
```

- Game: `D:\SteamLibrary\steamapps\common\BioShock Infinite\Binaries\Win32\BioShockInfinite.exe`
  (D: drive, same library as BS2; BS1 is on K:). Note the path shape differs from both remasters:
  UE3 uses `Binaries\Win32\`, not `Build\Final\`.
- Steam appid **8870**. Launch via Steam or `Start-Process steam://rungameid/8870`.
- Data dir is **`%LOCALAPPDATA%\BioshockVR\bsi\`** (log, prev log, command.txt, crash dumps).
  BS1 keeps the flat `%LOCALAPPDATA%\BioshockVR\` layout and BS2 uses `bs2\`. The three games
  never share files, so an Infinite session can never clobber BS1's or BS2's calibration.
- The game's own config goes to `%USERPROFILE%\Documents\My Games\BioShock Infinite\XGame\Config\`
  (standard UE3). **This directory is created on first launch** and did not exist as of
  session 34.
- `boot.ps1` is BS1-only and will not work here. Infinite's menus are Scaleform, and its first-boot
  flow includes legal/EULA screens (`DefaultbHasSeenLegalDocument`,
  `DefaultTermsOfServiceVersion`) plus a promo announcement (`DefaultbHasSeenPromoAnnoucement`,
  which the shipped ini comments say is forced false every launch). **Drive the menu by hand, or
  ask the user** until a `boot-inf.ps1` is proven.

## Injection

Confirmed by import-table parse, session 34: the exe imports `XINPUT1_3.dll` by **ordinal 2 and
3**, exactly like BS1 and BS2, so `src/proxy/` works unchanged. The game IAT slot for ordinal 2 is
at RVA `0xCD4814`.

Expect the same Steam-overlay problem BS1 hit: the overlay code-hooks the proxy's export thunk and
eats `XInputGetState` before the proxy body runs. BS1's fix is a last-hop IAT re-point that keeps
the previous target as passthrough, and the slot address above is already known.

`d3d11.dll` and `dxgi.dll` are **not** imported (loaded dynamically), so do not assume d3d11 is
resident when `framework::init()` runs.

## Cheats and test loadouts

This is the big quality-of-life difference from the remasters, where the console is compiled out
and key-bound commands are inert.

Infinite's shipped `XGame\Config\DefaultInput.ini` contains a live `; --- Debug binds` block:

| key | command | use |
|---|---|---|
| `Delete` | `god` | survive calibration work |
| `End` | `preventdeath` | softer variant |
| `PageUp` | `ghost` | noclip - fly to a test spot |
| `PageDown` | `walk` | leave ghost |
| `Backslash` | `behindview` | third person, useful for viewmodel checks |
| `F9` | `shot` | native screenshot |
| `F1` / `F2` / `F3` | `viewmode wireframe` / `unlit` / `lit` | render instruments |
| `F7` / `F8` | `set D3DRenderDevice bUsePostProcessEffects False` / `True` | proves `set` works |

**All of those binds survived into the live `XInput.ini`** (verified session 34 after the first
launch), and the live file additionally carries **`[Engine.Console] ConsoleKey=Tilde` /
`TypeKey=Tab`**, which the shipped template does not. If the console opens, test loadouts stop
being a problem.

A second surface exists in `XGame\Config\DefaultDesignerControlPresets.ini` (base64 `Data=`), which
lists `God Mode`, `Ghost`, `Prevent Death`, `GiveAmmo`, `Slomo 10/50/100/200/1000%`, `QuickSave`,
`QuickLoad` and the viewmodes.

**Session 37: the console works WITHOUT any key or UI - `bsiexec <console cmd>`** runs the text
through `ConsoleCommand` resolved by name on the live player controller (proven by effect:
`bsiexec setres 2560x1440` resizes the backbuffer, logged by our ResizeBuffers hook). `bsicall
<Func> [float]` calls any UFunction on the controller by name. Both refuse unless the camera
hook owns the pump (`pump=game`), so they only run on the game thread. Acceptance stays a
measured effect, never the log line.

**CAUTION (session 37): do not leave the game UNATTENDED at the menu/attract.** Two unattended
boots froze there (also on an unmodified build - it is the game's own bug; watchdog stack photos
in `%LOCALAPPDATA%\BioshockVR\bsi\s37-*hang*.log`). Attended menus have never hung. If it
happens: the process stops responding, presents stop, the log goes quiet - `Stop-Process` is the
only exit, a wedged process cannot take WM_CLOSE.

### I0 live checklist (about 3 minutes, no mod installed, no headset needed)

Run this the next time Infinite can have the machine, i.e. **with `Bioshock2HD.exe` closed**. Load
any save; combat is not required for most of it.

1. **Console.** Press `~` (tilde), then `Tab`. Does a console open? If yes, type `god` and note
   whether it echoes something like "God mode on". *This one question decides how much of BS1's
   Exec-seam machinery we ever need to build.*
2. **`PageUp` (ghost).** You should detach and fly through geometry - unmistakable, and needs no
   combat. `PageDown` (walk) puts you back.
3. **`F3` then `F1` (viewmode lit / wireframe).** Wireframe is unmistakable. Put it back with `F3`.
   This also proves the `viewmode` family, which is a useful render instrument later.
4. **`F9` (shot).** Check for a new screenshot file; report where it landed.
5. **`Delete` (god).** Only verifiable once something can hurt you, so skip if there is no combat
   yet - or confirm via the console echo from step 1.
6. **`F7` then `F8`** (post-process off/on). A visible change confirms that `set <class> <prop>
   <value>` reaches a live object, which is the UE3 universal knob.

Report which of the six did something observable and which did nothing. **A bind that is wired but
inert is exactly as important to record as one that works** - BS1 spent eight sessions believing a
`set` was landing because the return value said `HANDLED`.

If the console works, the loadout question is solved by conjuring one rather than by playing to a
combat area. If it does not, the fallback is to play as far as the raffle (first Sky-Hook, then the
Machine Gun) and save there as the permanent test anchor.

`[Engine.Console]`'s autocomplete list additionally names `SaveCheckpoint`, `LoadCheckpoint`,
`ArchiveLastCheckpoint`, `DisplayLastCheckpointInfo` and `GIVELOCKPICKS`.

**None of this is confirmed live yet.** A shipped config entry is a claim, not an effect. I0
verifies each by observable behaviour and records the result, including anything inert. Per the
standing rule: **verify by effect, never by return value** - on BS1 a `set` logged `-> HANDLED`
every five minutes for eight sessions and was never doing anything. The technique that works is to
set the value **absurdly** rather than to the target, so a no-op is unmistakable.

Save files are fair game: change, remove and create them freely.

## Harness

`game-cmd.ps1 / game-shot.ps1 / game-click.ps1` all take `-Game bsi` (process
`BioShockInfinite`, `bsi\command.txt`, and the conflict guard).

**Since session 38 the sim harness does too**: `xrsim-launch.ps1 -Game bsi -Release` launches
Infinite directly against the simulated Quest 3 (Steam client must be running; the Remastered
revert-Options dialog probe is skipped), `launch-game.ps1 -Game bsi` goes through Steam (appid
8870), and `xrsim-run.ps1 -Game bsi` routes `@mod` lines to the bsi seam. Sim discipline for
this game: send `recenter` before quad-pose captures (VERIFICATION.md gotcha 13), foreground
the game before captures that must show gameplay (it auto-pauses unfocused, auto-resumes on
focus), and keep the menu attended.

**`bsivr on|off|status`** (session 38) is the scripted lever on core's VR master enable - the
same flag as the F10 overlay checkbox. `off` tears the XR session down, `on` re-brings-up from
Present (~250 ms to FOCUSED under the sim); `status` reports `vr::session_live()`, the
observable state, because the checkbox is a second writer. Note the seam does NOT split on `;`
- one command per `game-cmd` write.

Carried from BS1/BS2 and expected to apply here until proven otherwise:

- **The game pauses while unfocused**, so a command written to `command.txt` may sit undispatched.
  Follow every `game-cmd` with a `game-shot` (which holds focus ~2.5 s) when the poll does not
  land.
- **Stale `command.txt` does NOT re-apply at boot on Infinite (session 35).** This bit BS1 three
  times, once producing a false negative when a `vrpreset save` re-ran at the menu and overwrote a
  tuned ini with defaults. The core poller's first poll now adopts the file's write time and logs
  `skipping pre-existing command.txt (last written ...)` instead of running it - verified in both
  directions live. **Still clear `command.txt` before closing the game, not only before launching**:
  the habit costs nothing, keeps the file honest for a human reader, and BS1/BS2 keep the old
  behaviour until they fold into the core poller.
- **Never write `command.txt` with `Set-Content -Encoding utf8`** - PowerShell 5.1 adds a BOM which
  silently corrupts the first token. `game-cmd.ps1` uses `[System.IO.File]::WriteAllText`.
- **`img-diff.ps1` noise floors** (BS1-calibrated, re-calibrate for this game): standing-still
  gameplay ~0.4 mean, a real FOV change reads 4-7, and an animated menu moves ~3.8 on its own.
  **Do A/B in a loaded save, never at the menu.**
- **Loading the same save reproduces the same spawn viewpoint**, which is what makes A/B render
  comparisons valid.

**The command poller is core and ticks from Present on this game (landed I1, session 35).** On BS1
and BS2 it lives in the adapter and ticks off the camera hook, so a skeleton adapter has no command
surface until its first engine hook fires. Here it works from the first frame, with no engine hook
installed anywhere. Two things follow:

- Commands run on the **render thread**, so a long `memscan`/`fsweep` stalls presents rather than
  the game thread. `vrcmd` prints which pump is live. When the I2 camera hook lands it takes over
  the poll, one-way and permanently.
- The **core** vocabulary (`mem*`, `fsweep`, `dumpframe`, `vrinput`, `vrpace`, `vrmirror`, `vrcine`,
  `vroverlay`, `vrhud`, `vrcmd`) is available on Infinite immediately; adapter-specific commands are
  `bsi` and `buildgate off|on|status`.

## I1 smoke test (the acceptance, session 35 - repeat it after any core change)

```powershell
Get-Process Bioshock2HD -ErrorAction SilentlyContinue   # MUST be empty - if not, postpone
.\tools\build.ps1 -Game bsi -Install
Remove-Item "$env:LOCALAPPDATA\BioshockVR\bsi\command.txt" -ErrorAction SilentlyContinue
Start-Process "steam://rungameid/8870"
.\tools\tail-log.ps1 -Game bsi
```

1. **Init chain, in this order and with no gap:** `bioshockvr starting` -> `host exe: pe-timestamp
   0x627BE455 ...` -> `MinHook initialized` -> `[registry] host BioShockInfinite.exe ->
   bioshockinf adapter` -> `[bsi] host build VERIFIED` -> `[bsi] camera-seam probe (READ-ONLY...)`
   -> `[cmd] Present-thread command pump ARMED` -> `D3D11 swapchain hooks installed` ->
   `first Present: backbuffer 2560x1440 format 28` -> `overlay initialized`.
2. **F10 toggles the overlay.** Measured, not eyeballed: `game-shot` before and after, then
   `img-diff`. Session-35 numbers - **5.7 %** of channels changed against a **0.54 %** ambient
   floor (two shots with nothing between them).
3. **The command seam with no engine hook** - the whole point of the milestone:
   `game-cmd -Game bsi "vrcmd"` prints the pump, then `"vroverlay on"` / `"vroverlay off"` moves the
   screenshot (4.6 % vs the same 0.54 % floor). **The screenshot is the acceptance, not the log
   echo.**
4. `"dumpframe"` writes `framedump_HHMMSS_q0.txt` into the `bsi` data dir (482 events / 68
   resources in gameplay at 2560x1440).
5. `"nosuchcommand"` logs one unknown-command line - proves the dispatcher's fallthrough.
6. **Stale-file behaviour, both directions:** leave a command in `command.txt`, restart, and it must
   log `skipping pre-existing command.txt`; then write it again and it must dispatch.
7. **The game is otherwise normal**, and a menu quit logs
   `shutdown: DLL_PROCESS_DETACH (orderly process exit)`.

**One I1 line changes at I2**: step 3's `vrcmd` now prints `pump=game` rather than `pump=render`
once the camera hook has fired, because the hook takes over the poll. `pump=render` after gameplay
has started means the hook is NOT firing, which is a real result and not a harness fault.

## I2 battery (session 36 - the camera hook's first live run)

**Menu first, deliberately.** The process cannot reach gameplay without passing the menu, so a
gameplay-first plan tests the menu anyway, just without observing it. The two riskiest single events
in the milestone are the hook's **first fire** and the **command-pump handover**, and both happen at
whichever state comes first - doing them at the menu means the handover lands while nothing is
loading, and a wrong result costs a relaunch rather than a save. A UE3 main menu may have no
`APlayerController` at all; if so we learn that before loading anything.

```powershell
Get-Process Bioshock2HD -ErrorAction SilentlyContinue   # MUST be empty
.\tools\build.ps1 -Release -Install -Game bsi
Remove-Item "$env:LOCALAPPDATA\BioshockVR\bsi\command.txt" -ErrorAction SilentlyContinue
Start-Process "steam://rungameid/8870"
.\tools\tail-log.ps1 -Game bsi
```

**If a "Run-Time Check Failure #0 - ESP was not properly saved" dialog appears, press Abort. Never
force-kill** - that can leave the display mode unrestored. It writes no crash dump, so the log is
the only evidence; capture it before dismissing.

1. **At launch**, expect `[bsi] camera: READ-ONLY hook installed ... prologue and `ret 8` both
   verified` BEFORE `first Present`. A `prologue MISMATCH` or `no ret 8` line means the hook
   refused, which is the fail-soft path working, not a bug.
2. **At the main menu**, read the heartbeat burst. Record: does it fire at all; `calls/s`; the
   latched tid; the path census; whether `returned-minus-cached` is zero (raw copy) or not
   (TRANSFORMED); and the one-shot `[this+0x430] 4x4` line.
3. `game-cmd -Game bsi "vrcmd"` -> **`pump=game` is the handover acceptance**, not the log echo of
   the call. `pump=render` here is the menu answer being "the hook does not fire at the menu".
4. `bsicam status`, `bsicam paths`, `bsicam tid`, then `bsireflect selftest`.
   **The selftest is the gate on every reflection result**: it must report the four positive
   resolutions, the prefix/suffix/absent negatives all rejecting, a 2647-entry table walk,
   scan-and-walk agreement, and `GNames[0] == "None"`.
5. **The pump-lease positive control, at the menu, before any save is at risk:**
   `bsicam off` -> wait 4 s -> `vrcmd` must print **`pump=render(degraded)`** and the log must carry
   `RESUMING in DEGRADED mode`. Then `bsicam on` -> `vrcmd` must print `pump=game` and the log must
   carry `game thread resumed`.
6. **Controls on the instrument itself:** `bsicam heartbeat off` must stop the lines. `bsicam off`
   then two `bsicam status` five seconds apart must show an **unchanged call count** - an instrument
   that cannot be made to fail is not evidence.
7. Load a save; capture the calls/s spike across the transition.
8. **In gameplay**: 30 s standing still, then 30 s walking and turning. `loc` must move, and one
   full mouse turn must sweep `rot.yaw` through the full 16-bit range - that single motion falsifies
   both the field-ordering and the 65536-units-per-turn assumptions at once.
9. `bsivtable` - slot `+0x7C` on an `APlayerController` should hold **`0x19A150`**
   (`AActor::ProcessEvent`), NOT the `UObject` base at `0xCFE70`. Either is informative; neither is
   a failure. The `UClass` fixpoint in the selftest should now pass too.
10. Trigger a cutscene or a door transition and watch for `camera: RESUMED after N ms silent`.
11. **DR-I3 dumps** (three launches, same save and same spot each time, weapon drawn, NOT aiming
    down sights, somewhere with open sky and depth):
    - Launch 1, 2560x1440, FOV slider 0.0: `dumpframe cb 2`, then turn ~90 degrees without moving
      and `dumpframe cb 1` (the orientation control - a projection term is identical between the
      two, a view term is not).
    - Launch 2, FOV slider at maximum via the in-game Options: `dumpframe cb 2`.
    - Launch 3, `XEngine.ini` `ResX=1600` `ResY=1200`: **acceptance is the log line
      `first Present: backbuffer 1600x1200`**, which incidentally answers DR-I8. Then
      `dumpframe cb 2`. Back up both config files first and restore them after.
12. **Non-regression:** re-run the I1 smoke checklist above, play 2-3 minutes, confirm no new crash
    dump and an orderly `DLL_PROCESS_DETACH`. Clear `command.txt` before closing.
13. **Offline decode**, in this order: `-SelfTest` (prove the scanners still work), then
    `-ScanMatrix` (the likeliest shape for UE3), then `-ScanLayout`, then the diffs:
    `-Diff <L2> -DiffFovs 70,80.5 -BlockBytes 160` (then 640, then 1280), and
    `-Diff <L3> -DiffAspects 2560x1440,1600x1200`. **The identification is the cross-product**: an
    index that moves under FOV and is pinned under aspect is a horizontal projection term.
    Believe a `-ScanMatrix` row only when `tanH/tanV` matches the backbuffer aspect - plurality of
    blocks alone is NOT enough, because on a BS1 control dump a degenerate 0.5/1.0 pair outvoted
    the true answer 156 to 83.

**New at I2: commands run on the GAME thread.** Once the camera hook fires it owns the poll, so a
long `memscan` or `fsweep` now freezes the GAME rather than stalling presents. If the game thread
goes quiet for 3 s the Present pump resumes in degraded mode and refuses `mempoke*`/`pokeaddr*`/
`memrestore` until it returns.

## I4 battery (session 39 - the 6DoF drive, all flat, run at the attract)

The drive substitutes the detour's OUT-PARAMS only; `bsicam drive on|off` is the live-lane
lever (ships OFF), `simhead`/`vrrec replay` drive regardless. Every check below reads the
1 Hz `[bsi] drive:` heartbeat line (`bsicam heartbeat on` re-arms the 10-beat burst; ONE
command per `game-cmd` write; each `game-cmd` needs ~1 s for the poll before the next).

```powershell
Get-Process Bioshock2HD -ErrorAction SilentlyContinue   # MUST be empty
.\tools\xrsim-selftest.ps1 -Release
.\tools\build.ps1 -Release -Install -Game bsi
Remove-Item "$env:LOCALAPPDATA\BioshockVR\bsi\command.txt" -ErrorAction SilentlyContinue
.\tools\xrsim-launch.ps1 -Game bsi -Release
```

1. **Passthrough control** (drive off, default): `lane=off`, final rot == `engineRot`,
   `returned-minus-source d=(0.000 0.000 0.000)` still identical. Session-39 values held.
2. **simhead additive yaw**: `simhead off` -> `simhead 0 0 0` (arming from idle recenters
   onto the sim pose) -> `simhead 30 0 0` -> **final yaw - engineRot yaw = 5461 units =
   exactly 30.0 deg**, and engineRot keeps MOVING underneath (the attract camera) - a frozen
   engineRot beside a moving final rot is the BS1 pitch-freeze bug showing itself.
3. **Absolute pitch/roll**: `simhead 0 20 0` -> final pitch 3640 (20.0 deg), engine base
   untouched, `pitchErr` = head-minus-engine; `simhead 0 0 10` -> final roll 1820 (10.0 deg).
4. **Position triple** (after the recenter in step 2, XR meters): `simhead 0 0 0 0.5 0 0` ->
   `headOff` = 0.5 x scale UU rotated by the game yaw (at scale 50 / game yaw 65.1 deg:
   (-22.7, 10.5, 0.0)); `... 0 0.5 0` -> (0, 0, +25.0) world-up unrotated;
   `... 0 0 -0.5` -> +25 UU along the view yaw. Final loc = engine loc + headOff exactly.
5. **worldscale lever**: `worldscale 100` doubles `headOff`; `worldscale 50` restores.
6. **The real OpenXR path** (composes sim head -> xrLocateSpace -> get_head_pose -> drive):
   `bsicam drive on` -> `recenter` -> `xrsim-cmd "head rot 30 0 0"` -> `lane=live`, residual
   exactly 5461 units; `xrsim-cmd "head pos 0.25 1.6 0"` -> `headOff` = 12.5 UU rotated by
   the game yaw, exact.
7. **vrrec**: `vrrec start` -> `xrsim-cmd "head orbit 10 8000"` -> `vrrec stop` (~90
   entries/s, one per present edge) -> `bsivr off` -> `vrrec play` -> `[rec] PLAY` marks
   number-for-number identical to `REC` (head quat AND driven camera), `lane=replay` with
   `xr=none`, recenter+worldScale restored from the header -> `bsivr on` re-earns FOCUSED.
8. **Rendered pixels** (the engine consuming the substituted view, not our own read-back):
   `game-shot` twice for the ambient floor (s39: mean 0.58, 1.12 % changed), then
   `simhead 0 0 0` -> `simhead 60 0 0` -> `game-shot` again: s39 read **mean 6.12 / 23.4 %
   changed, 10x the floor**, and the shot visibly shows a rotated heading. Use the WINDOW
   for I4 pixel checks, not the compositor capture - see the recenter gotcha in
   VERIFICATION.md section 5.
9. **Perf + stability**: 90.0 fps sustained with the drive on (frame-counter delta across
   two `xrsim-cmd` status reads); `bsivr off/on` and the `bsicam off` lease control still
   pass; `vrpreset save` round-trips worldScale; clean WM_CLOSE exit.

## I4 in-headset checklist (VDXR, user drives - the Done-when)

Launch via Virtual Desktop as in I3. Load a Columbia save (never judge at the menu). All
levers are in the F10 overlay, "VR camera (I4)" section - never type commands in-headset.

1. **Non-regression sweep, 60 s, FIRST**: the I3 big screen as before - head-tracked quad,
   stable, no judder, F10 opens.
2. Tick **"Drive camera from HMD"**, then press **Recenter** looking straight ahead.
   The world view on the big screen now follows your head: turn (yaw), look up/down
   (pitch), TILT your head (roll - the screen horizon must tilt with you, this game's first
   roll ever).
3. **The corner lean**: stand at a doorway or corner, lean your body sideways around it
   without stepping. The view must translate - you can peek around the edge - and come back
   to exactly the same place (no drift after repeated leans).
4. **World scale**: lean toward a table/railing - does the world move the RIGHT amount?
   Tune the "World scale (UU per m)" slider by feel (default 50 = UE3 canonical; BS1/BS2
   wanted 100 on their engine). `vrpreset save` via desktop afterwards persists it.
5. Walk and turn with the stick while the drive is on - stick turning must still work
   (additive yaw), aim/shooting still functions (the gun aims where the ENGINE looks, which
   no longer matches your head pitch - that mismatch is expected and is I7's job).
6. Expected noise, not bugs: the HUD stays screen-locked on the quad; the weapon viewmodel
   rides the engine camera, not your head; pause/menus render on the quad unmoved; and the
   camera toggle in core's own VR overlay section does NOTHING on this game - that is the
   quad->projection flip, gated on a lens claim the adapter does not feed core until I5/I6
   (user pressed it in the s39 run and correctly saw no effect). Only the "VR camera (I4)"
   section is live. Anything off: untick the drive checkbox first - if the symptom
   survives, it predates this session (I3 baseline).

Run s39 verdicts (VDXR): lean tracked no drift; roll correct; world-scale feel deferred to
I5 (not judgeable on the mono screen). I4 CLOSED.

## I5 battery (session 40 - stereo, all flat, run at the attract)

Prep as for the I4 battery (BS2 conflict check, selftest, build, clear command.txt,
`xrsim-launch -Game bsi -Release`), then `xrsim-cmd "recenter"` (head at yaw 0 - gotcha 17)
and `xrsim-cmd "fov 54 55 54"` (symmetric eye, or the claimRatioH math needs the asymmetric
average). ONE command per game-cmd write throughout.

1. **Rung 1, the projection flip**: `vrstereo mono` -> log shows `VRSTEREO ON`, core's
   `camera mode ON` and `first projection-layer frame`; the fovaudit line must carry
   `src=readback` with `tanH = claimTanV x aspect` (slider-min claim at 16:9:
   0.4317 x 16/9 = 0.7675). Capture: `projectionViews=2`, `claimTanH 0.76747`,
   **claimRatioH 0.5576** (the s40 baseline - claimTanH / tan(54)), L and R
   **byte-identical** (img-diff 0.0 - the mono control). `vrstereo off` -> capture shows
   `LayerTypes={quad}` again.
2. **Rung 2, AlternateEye**: `vraer on` -> park the head (`simhead 0 0 0`) -> heartbeat
   `[bsi] stereo: inter-eye |d|` = ipd/1000 x worldScale exactly (63 mm at scale 50 =
   3.150 UU; residual only while the attract base moves between eye frames);
   `worldscale 100` doubles it; both `lastSign` values appear. `vraer off`.
3. **Rung 3, SequentialReentry**: `vrstereo on` -> `[bsi][reentry] hook installed on the
   viewport draw root`, `STEREO ARMED`, core logs `first SequentialReentry eye frame` and
   `pair pacing live`. The 5 s `[bsi][reentry] beat:` line is the acceptance instrument:
   **draws/s == 2nd/s, presents/s == 2 x draws/s, camReplays/s == 2nd/s** (s40: 90/90/180/90
   at the sim's 90 Hz ceiling), `call2` well under a millisecond, skips not accumulating
   during steady scenes. Inter-eye line exact as in rung 2. Capture pair L vs R img-diff
   NON-zero (s40: mean 0.42, 1.09 % - real parallax; the rung-1 identical pair is the
   control). Occasional `sr tag ring skewed - cleared` at attract scene/movie transitions is
   self-healing noise, not a failure; a CONTINUOUS skew stream means the doubling and the
   presents have gone one-sided (that signature = the wrong root; see ENGINE_NOTES s40).
4. **Soak**: 10+ minutes armed (attended - never leave the attract unattended). Zero
   watchdog fires, zero faults, zero poison; beats stay exact; `vrcmd` responsive.
5. **Fallbacks still clean**: `vrstereo off` (mono quad), `bsivr off/on` (~250 ms
   re-bring-up), `bsicam off` lease control, clean WM_CLOSE exit.
6. **vrrec determinism**: run replay-identity checks with stereo OFF - the eye offset rides
   the final camera the recorder taps, and AER phase vs the recording is not deterministic.

## I5 in-headset checklist (VDXR, user drives - the Done-when)

Launch via Virtual Desktop as in I3/I4. Load the Columbia save. All levers in the F10
overlay ("VR stereo (I5)" + "VR camera (I4)" sections) - never type commands in-headset.
**Desktop prep before putting the headset on**: verify the in-game FOV slider is at MINIMUM
(the claim assumes it; if you have moved it, `bsifov tanv` on the desktop first).

1. **Non-regression sweep, 60 s, FIRST**: I3/I4 behaviour with stereo off - head-tracked
   quad, drive checkbox, corner lean, F10 opens.
2. Tick **"VR stereo (projection layer)"** (or run the preset): the cinema screen is
   REPLACED by the world filling your view in true stereo. Judge: real depth (close one
   eye, then the other - nearby railings/objects shift; both eyes fused, no double
   vision), NO vertical disparity (no eye-strain "swimming"), no fisheye (straight lines
   stay straight - a wrong claim shows here immediately).
3. **THE CARRIED ITEM - world-scale tune**: lean toward a table/railing; does the world
   move the right amount? Does Columbia feel life-size, not miniature/giant? Tune "World
   scale (UU per m)" by feel (default 50 = UE3 canonical; do NOT expect BS1/BS2's 100),
   and the IPD slider if depth feels exaggerated/flat (default 63 mm). `vrpreset save`
   from the desktop afterwards persists worldScale + ipd + claim.
4. **Comfort/perf**: 72 fps at default render scale (no judder on head turns; VDXR
   overlay if in doubt), 30-minute session, no visual state corruption (flicker,
   stuck geometry, HUD ghosting).
5. **Stability**: one level transition and one quit-to-menu with stereo armed (the deny
   gate + silent/stall gates must keep loads un-doubled; `reentry status` on the desktop
   afterwards - foreign/stereo skips may count up, poisoned must stay "no").
6. Expected noise, not bugs: HUD is screen-locked mid-view (I9's lane), the weapon rides
   the engine camera not your head (I8), aim/head mismatch (I7), menus/loads drop to the
   quad briefly (the cine fallback working). Anything off: untick "VR stereo" first - if
   the symptom survives, it predates this session (I4 baseline).

## I6 battery (session 41 - lens/FOV/resolution/config, all flat, run at the attract)

Prep as for I5 (BS2 conflict check, selftest, build, clear command.txt, launch, `recenter`
at yaw 0, `fov 54 55 54`). NEW DISCIPLINE from s41: at attract movie transitions the pump
lags - send ONE command and confirm its dispatch line in the log before the next
(VERIFICATION gotcha 20), and never leave the attract unattended (the pre-existing freeze
hit twice this session).

1. **Lever**: `dumpframe cb` + `decode-framedump -ScanMatrix -BlockBytes 80` baseline
   (native, scene-dependent: slider-min 0.7673/0.4316 or max 0.8770/0.4933 - the attract
   cycles bases). `bsifov set 110` -> decode tanH 1.4281 (tan 55 exact) tanV 0.8033;
   `bsifov set 130` -> 2.1443/1.2063 (monotone, aspect held); `bsifov` shows the claim
   tracking (hfov == commanded, writes counting, faults 0); `bsifov off` -> native decode
   returns.
2. **Decoder**: before arming, `bsilens` must show samples=0 (the core tap's inertness
   observable). `bsilens on` -> rounds publish within seconds; majority tanV must match a
   same-scene dumpframe decode; the CLAIM MISMATCH line fires if the claim is stale (it
   caught 0.4317-vs-0.4933 at 14.3% on its first round in s41); `bsilens track on` heals
   the claim; with the lever armed the decoder follows within one round at 100% support
   and delta 0.0%. Refused rounds count up at menu/movie transitions - correct, not a
   failure.
3. **Resolution live**: `bsires squareperf` -> log shows setres dispatch, `ResizeBuffers
   1440x1440 hr=0`, XR swapchain rebuild queued, ini written (one-time .bvr-bak-res on
   first write). `bsires status` compares ini vs live.
4. **Resolution boot acceptance**: relaunch -> `first Present: backbuffer 1440x1440`.
   This line is THE acceptance; the ini read-back never is.
5. **The non-16:9 claim measurement (the ROADMAP done-when)**: at 1440x1440 with
   `bsifov set 100`, BOTH decoders read tanH = tanV = 0.6704 (= tan(50)/(16/9) - the
   16:9-referenced law, patterns.h kFovRefAspect) and the claim closes at delta 0.0%;
   stereo capture claimRatioH 0.48705 vs 0.4871 predicted. claimRatioH is
   CONFIG-DEPENDENT - recompute claimTanH/tan(54) per config, never reuse a banked value.
6. **Presets**: save a slot, scramble every value, load -> all applied (log counts);
   restart the game -> boot store applies, slot loads identically. A loaded preset's
   resolution is LATCHED into the picker (amber line), applied only by the Apply button.
   Legacy vrpreset.ini files (1-4 keys) still load.
7. **Battery 0 after any rebuild**: `vrstereo on` -> SR beat exact in an attract gameplay
   scene (draws==2nd, presents==2x, replays==2nd; menu/movie phases legitimately show
   30/30/30 with high raw draws and counting s-skips - wait for the gameplay scene),
   `reentry status` poisoned=no, clean exit.

## I6 in-headset checklist (VDXR, user drives - the Done-when; suggest Virtual Desktop at 72 Hz)

Launch via Virtual Desktop as before. Load the Columbia save. Everything judged by eye is
an F10 control - never type in-headset. Desktop prep: none needed (the claim now tracks
the lever, and the decoder audits it live - `bsilens on` from the desktop first if you
want the audit line running).

1. **Non-regression sweep, 60 s, FIRST**: stereo off - head-tracked quad, drive checkbox,
   corner lean, F10 opens.
2. **VR stereo on at defaults (lever off)**: the I5 baseline - true depth, no fisheye,
   the world through the window at true angular size. This is the BEFORE.
3. **THE VERDICT - fill the eye**: press **Load preset** in VR PRESET (ONE preset since
   the s41 feedback round; it applies EVERYTHING - stereo, drive, lever, scale, ipd and
   the resolution - and auto-loads at boot). The user's accepted values: lever 132 deg,
   worldScale 150, Quest 3 native 2064x2208. Judge: does the world fill the eye (no more
   window)? Do straight lines stay straight (a wrong claim = fisheye/warp on head
   rotation - the in-headset instrument for claim==render)? The FOV-lever slider is live
   for tuning; **Save preset** persists. *(Verdict 2026-08-05: GREEN at 132 - "no space
   warp, which is perfect".)*
4. **CARRIED - world-scale tune**: through the FILLED view, lean toward a railing - does
   Columbia feel life-size? Tune "World scale (UU per m)" (default 50) and IPD (63) by
   feel; Save current afterwards.
5. **CARRIED - judder verdict at VD 72 Hz**: the near-square render also lowers per-eye
   cost vs 2560x1440. Head turns smooth? (I5 measured 77-80 pairs/s under 90 Hz - at
   72 Hz the target is pairs >= refresh.)
6. **Preset round-trip across a restart**: Save preset with your tuned values, quit the
   game fully, relaunch - the boot auto-loads and auto-arms everything (verified flat:
   8/8 keys, stereo armed, first Present at the preset resolution, zero commands).
7. **CARRIED - 30-minute soak** with stereo armed, including one level transition and one
   quit-to-menu. Afterwards, desktop: `reentry status` -> poisoned must be "no".
8. Expected noise, not bugs: HUD screen-locked mid-view (I9), weapon rides the engine
   camera (I8), aim/head mismatch (I7), brief quad drops at loads (cine fallback), the
   HUD/menus render at the new aspect (they follow the backbuffer). Anything off: lever
   off first (bsifov checkbox), then stereo off - if the symptom survives, it predates
   this session.

## S42 judder verdict checklist (VDXR, Virtual Desktop at 72 Hz - the I6 close)

Boot as usual (the preset auto-arms everything, now including the new pair-rate sync -
it defaults ON with stereo). Load your save. Everything is F10; never type in-headset.

1. **Head turns, sync ON (the default)**: smooth or juddering? This is the verdict line.
2. **The A/B**: F10 -> VR section -> uncheck "Sync pair rate to headset refresh
   (judder A/B)" -> turn your head -> recheck it. Any difference? (If sync-off judders
   and sync-on is smooth, the beat hypothesis is CONFIRMED on VDXR; if both judder the
   same, it is falsified there and the trace tells us what is actually happening.)
3. Optional second experiment if judder persists: RENDER RESOLUTION -> `eye` preset
   (1600x1712) -> Apply, and judge head turns again (marginal-frame-time hypothesis -
   the render may sit right at the 13.9 ms budget at 2064x2208).
4. Nothing else changed this session for the headset: stereo, lever 132, world scale,
   preset all ride as accepted in s41.
5. Afterwards, desktop: keep `%LOCALAPPDATA%\BioshockVR\bsi\pacetrace.log` - the
   `TRACE pairs` lines under VDXR are the measurement half of the verdict (pairs/s vs
   72, sd, waitGate). Report the feel; the log carries the numbers.

## S43 stutter verdict checklist (VDXR, native 2064x2208 - the hitch-spike A/B)

The s43 flat hunt NAMED the dominant flat spike source: the engine's 30-second GC tick
(`TimeBetweenPurgingPendingKillObjects=30`) parks the game thread on a flush barrier
for 30-50+ ms - proven by intervention (interval -> 300: the 30 s spike grid vanished
in 3 min idle AND the matched wander protocol went 4-7 spikes -> 0), with the reversal
leg's grid-return closing the A-B-A. The candidate ships as
`TimeBetweenPurgingPendingKillObjects=300` in the GAME FOLDER's
`XGame\Config\DefaultEngine.ini` (line ~300; the boot-derived XEngine.ini picks it up -
verify `Documents\My Games\...\XGame\Config\XEngine.ini` line ~98 reads 300 after
boot). Backup: `DefaultEngine.ini.bvr-bak-s43` beside it; restore = copy back.

1. Boot as usual (preset auto-arms everything; the spike instrument + sampler ride
   with stereo automatically - nothing to press). Load your save.
2. **The verdict line: head turns OUTDOORS at native 2064x2208.** Smooth, or still
   hitching in bursts? Compare against the s42 memory (39-113 ms bursts every few
   seconds).
3. If you can: 60 s standing still in a quiet spot, then 60 s walking + turning
   through new streets. The old signature was bursts every few seconds while moving.
4. Optional across-boot A/B (each leg needs a restart, so only if you have patience):
   restore the backup (interval back to 30), reboot, same spot - the difference IS
   the lever.
5. Afterwards, desktop: keep `%LOCALAPPDATA%\BioshockVR\bsi\pacetrace.log`. The
   `TRACE pairs ... spikes=N` field counts hitch-class intervals per second, and
   every `TRACE spike`/`SPIKE-SAMPLE` line carries the attribution. Report the feel;
   the log carries the numbers.
6. Known open residuals, not regressions: a one-time settle burst ~60 s after a
   checkpoint load; traversal (streaming) spikes may remain outdoors - if the feel
   is better but not clean, the next flat rung (texture-pool sizing via
   `-ReadTexturePoolFromIni`, already researched) targets the remainder.

*(S43 outcome, user 2026-08-06: stutter resolved to the user's satisfaction by the
GC lever + the user's own quality settings + headset at 72 Hz - closed.)*

## S43b jumpy-camera checklist (the pose-attribution A/B) - VERDICT IN: lag 2, "extremely smooth"; now the boot default

The percept: head-coupled camera motion is bouncy/not-life-like, unlike BS1/BS2.
The suspect: the submitted eye poses are attributed to the wrong locate generation
for this threaded renderer (reprojection wobble - error scales with head SPEED).

1. Boot as usual, load your save (or judge at the menu - it renders the same lane).
2. F10 -> "VR stereo (I5)" -> **POSE ATTRIBUTION (jumpy-camera A/B, s43b)**: three
   radios - `fresh (0)`, `1 back (default)`, `2 back (threaded)`.
3. Turn your head steadily left-right at each setting for ~10 s. Judge: does the
   world stay nailed in place (real-life-like), or does it wobble/drag/bounce in
   proportion to how fast you turn?
4. Pick the smoothest and tell me WHICH ONE it was - that single answer names the
   render pipeline's depth and becomes the shipped default for Infinite.
5. If ALL THREE wobble the same: say so - that falsifies the attribution hypothesis
   and moves the instrument to the drive side (pose age at consume/engine smoothing);
   do not keep hunting settings.
6. The "head delta between generations" readout under the radios shows deg/pair at
   your current head speed - if you can, note roughly what it reads during a brisk
   turn (it is the predicted wobble amplitude of a one-generation error).

## S44 controls checklist (I7: the Touch layout, and the aim probe)

Controls now come up LIVE at boot (`inputOn`, a 9th preset key, default on) - you
should have a working controller the moment you are in the headset, without typing
anything. Everything below is an F10 control for the same reason.

**1. Non-regression sweep, 60 s, DO THIS FIRST.** Nothing this session touched
stereo, pacing or the camera, so this must look exactly like the s43b build you
signed off as "extremely smooth": turn your head steadily left-right, walk a few
steps, look up and down. **Smoothness must be UNCHANGED - pose lag 2, no new
judder.** If it is worse, stop and say so; F10 -> "VR stereo (I5)" -> pose
attribution radios are still there to bisect, and `bsiinput off` disables the whole
new lane.

**2. Every control on the Touch layout.** Each one was verified flat to produce the
right XInput bit; what you are judging is that the bit does the RIGHT THING in the
game. Work down the list and tell me any that misbehave:

| control | expected |
|---|---|
| A | jump (and Sky-Hook transfer where one is in range) |
| B | crouch - toggle, press again to stand |
| X | reload / hold to hack or use |
| Y | melee (hold ~0.15 s for the execution) |
| right trigger | fire |
| left trigger | vigor cast |
| left grip | next vigor |
| right grip | next weapon |
| left stick | move; also menu navigation |
| right stick | turn |
| left stick CLICK | sprint |
| **right stick CLICK** | **zoom / iron sights** - new here; BioShock 1 deliberately eats this button, Infinite forwards it |
| left thumbrest HELD + right stick flick | the DPad family (nav pulse, buyout hack, auto hack, quick-toggle cycle left/right). Touch has no DPad, so this is its stand-in |
| menu button, tap | pause menu |
| menu button, hold ~0.5 s | back |

Three of these the flat lane could NOT name individually, so they are the ones I most
need your eye on: **melee, next weapon** (the test save owns one gun) and **sprint**
(the checkpoint spot is walled in, so a flat speed test had no room to run).

**3. The pause menu** works from the controller (verified in the headset, s44) - the
flat lane's "blocker" was a harness focus artifact, not a real one.

**4. Pad map A/B**, if any button feels wrong: F10 -> "INPUT (I7)" has two radios,
`Infinite` (shipped) and `BioShock 1` (the control). Flipping to BioShock 1 should make
the faces obviously wrong - that is what tells us a complaint is about the MAP rather
than the binding underneath it.

**5. Aim - now SHIPPED ARMED, both hands.** F10 -> "AIM (I7)".

1. **Weapon hand.** Look one way, point the controller another, fire. Shots follow the
   CONTROLLER. You verified this in s44; re-confirm it survived the dual-hand change.
2. **Vigor hand.** Same test with the LEFT trigger. The aiming hand follows whichever
   trigger you pull, and the F10 panel shows `aiming hand RIGHT (weapon)` / `LEFT
   (vigor)` live, so you can watch it flip.
3. **Aim dots are ON by default, one per hand.** Each should sit exactly on the ray
   leaving its OWN controller. Flat measurement puts both at 0.0000 deg with the hands
   swung to opposite angles, so a dot that looks off its controller in the headset is a
   real finding worth reporting precisely.
4. **Dot distance** slider (0.5-15 m) if 3 m is not where you want it.
5. **Lasers are OFF by default**, one checkbox per hand. Turn them on for the full beam;
   two beams fill a lot of view, which is why they are not the default.
6. The calibration question: fire at a wall and say whether the hole lands ON the dot,
   near it, or somewhere else.

Expected noise, not bugs: `sr tag ring skewed - cleared` around movie and scene
transitions, and a ~1 s pause when a checkpoint loads.

Anything off, toggle the newest lever off first - `bsiaim off`, then the F10 pad-map
radio back to BioShock 1, then `bsiinput off`. If the symptom survives all three, it
predates this session.

## S45b hands checklist (I8: the viewmodel off the headset)

Everything below is judged in the headset; the flat lane already proved the numbers
(ground truth 150.0 UU/m exact, five-station rolled sweep at 0.0000 deg deviation both
hands, stick-Y bit-identical). All levers are F10 controls in "HANDS + MODEL (I8)".

1. **Non-regression FIRST (60 s).** Smoothness unchanged (pose lag 2), all Touch
   controls unchanged, aim still follows the controller, dots on the hands. Bisect
   lever: the "drive hands/weapon from the controllers" checkbox - off returns the
   engine viewmodel.
2. **Decoupling**: move the head with the controllers still - hands and pistol must
   PARK. Walk around them, look from above/side.
3. **Position/rotation/scale**: tune with the L/R radio + model trim/offset/scale
   sliders. The LEFT hand is expected to need a trim (the mirrored grip frame showed a
   constant offset flat); report the values that feel right - they persist via SAVE.
4. **Arms mode A/B** (radio): follow (default) vs hide (hand+gun only - flat-verified
   no skin web) vs game (engine arms - expected to read frozen/detached).
5. **Animations**: fire and reload - the animation must play INSIDE the driven hand.
   A/B: the "engine animations on driven hands" checkbox off = rigid pose.
6. **Model-vs-laser sync THROUGH ROLLED poses**: laser on (R), sweep the controller
   including full wrist rolls - divergence CONSTANT (a fixed offset = trim it away) is
   fine; divergence that GROWS with orientation is a real finding, report the pose.
7. **Carried from s44**: fire at a wall - does the hole land ON the dot? And if
   anything selectable/hackable is around: left thumbrest held + right-stick flick.

Expected noise: the sr-tag-ring lines at scene transitions, a ~1 s checkpoint-load
pause, and hands parked mid-air during load screens (the drive holds the last pose
while the world is gone - it re-adopts on the first live frame).

Anything off: the hands checkbox off first, then `bsiaim off`. If the symptom survives
both, it predates this session.

## S46 checklist (I8 part 2: the stance, the bullet origin, and the wrist)

Everything below is judged in the headset; the flat lane already proved the numbers
(stance glue A-B-A 0.33 -> 101.11 -> 0.06 deg; fire origin substituted at 77.4 UU
from the camera on both hands; three stations at 0.0000 deg deviation; wrist quat
exact to 20.00 deg on exactly the arm chain). All levers are live toggles.

1. **Non-regression FIRST (60 s).** Hands/gun still track the controllers with no
   drift, models and lasers in sync, sliders and scale still work, both hands aim
   independently, smoothness unchanged. Bisect: the "drive hands/weapon" checkbox.
2. **THE STANCE (headline)**: idle for ~3 minutes without firing. The weapon/hands
   must HOLD their forward pose - no off-forward drift-and-stick. Then verify the
   transients are ALIVE: fire (muzzle/recoil anim plays in the hand), reload, and
   wait for the occasional LEFT-hand vigor flourish. **If the flourish is gone or
   reads wrong, say so** - the glue passes its articulation through but cancels its
   whole-hand swing, and only you can judge whether what remains still reads as the
   flourish. Bisect: the "kill persistent stance" checkbox (bones block of the
   HANDS panel) / `bsibones glue off`.
3. **BULLETS FROM THE GUN**: fire while watching the muzzle - the shot should
   visibly leave the WEAPON, not the screen center. Bisect: `bsifire off`.
4. **HOLE ON THE DOT, two distances**: fire at a NEAR wall (~1-2 m) and a FAR wall
   (10+ m) - the hole must land ON the dot at both. (With `bsifire off` the near
   hole lands ~half a meter off the dot and the error shrinks with distance - that
   is the old parallax, useful as the A/B.)
5. **Wrist cap A/B** (arms = hide): flip the "wrist cap" radio between "pinch at
   grip" (0) and "pinch behind wrist" (2). Skip style 1 - flat capture shows a
   giant skin hood. Report which reads better.
6. **Left wrist angle**: after (2) holds, does the left hand-vs-forearm angle read
   natural now? If an angle survives, tune the per-hand "wrist pitch/yaw/roll"
   sliders (HANDS panel, L radio) and report the values that feel right - they are
   NOT persisted yet.
7. **Vigor cast**: cast with the left hand - the effect should originate from the
   left hand (the cast routes through the same origin seam, verified flat).

Expected noise: unchanged from s45b (sr-tag-ring lines at transitions, checkpoint
pause, hands parked during loads). The stance glue arms itself on your FIRST shot
(per boot); before that shot the LEFT hand WILL hold the stance - s47 measured
the boot/checkpoint-load pose to BE the stance (101.11 deg at the anchor, stable
from resolve), which is also why the glue cannot self-arm at boot: capturing the
resolve pose would pin the stance instead of ready. Fire once and it is gone.
Anything off: `bsibones glue off` first, then `bsifire off`, then the hands
checkbox - whichever clears it names the lever; if none do, it predates this
session.

## S47 additions (test together with the S46 list)

The s47 flat lane added no default-on behaviour: the reapply gate closed as
measured-no-defect (counters only), the world-scale ground truth re-verified
exact, and the per-weapon table is an empty scaffold. Two items for the headset:

1. **ANIMTRANS A/B (optional, default OFF).** After your first shot (the glue
   capture), tick "authored anchor travel (animtrans)" in the HANDS bones block
   (or `bsihands animtrans on`) and reload / fire. ON = the hands leave the
   controllers by the authored anim travel (measured: reload moves the right
   hand 14 cm, the LEFT hand 48 cm on its cross-over rack), with rotation still
   glued - judge whether that reads as "the anim came alive" or as "my hand
   detached". Known caveats: the hand slides without turning (rotation stays
   glued by design), and if the idle stance re-onsets while it is ON the left
   hand sits ~35 cm off the controller until the next shot. It is a pure A/B
   toggle; OFF returns to the pinned s46 behaviour and nothing is persisted.
2. **First-shot arming ergonomics.** Knowing the stance is now CERTAIN before
   the first shot each boot: does "fire once after loading" feel acceptable as
   the arming ritual, or should a future session hunt a pacifist-safe source?
   (Boot auto-capture is measured impossible from the resolve pose; the manual
   F10 capture button exists but captures the stance if pressed pre-shot.)

## S48 checklist (the verdict fixes; branch `si48-inf-verdict-fixes`)

State honestly: **the idle stance is NOT yet fixed in this build.** The s46 glue
was retired per your verdict (default off), and s48b then falsified FOUR
root-kill hypotheses with verified-held writes on the live save: the
ProcessEvent event (starts natively without it), the instance
bDisableSubtleFidget bool, the instance SubtleFidgetTimeRange (authored
{120, 240} s - exactly the observed 2-4 min re-onset - starved to 1e9), and
the archetype's copies of both. The consumer keeps its own private timing (the
anim-tree selection node) - that hunt opens the next session with the walker
toolchain already built. Expect the stance until then; fire once to clear it
whenever it appears. Everything else below is live.

1. **Non-regression FIRST (60 s)**: tracking, sync, sliders, scale, both-hand
   aim, hole-on-dot - all unchanged from what you verified.
2. **LOCOMOTION (the fix to judge)**: walk with the left stick while holding
   the weapon steady - the model must no longer shift/swim with movement
   (flat: 6.2 cm of wobble down to 1.1 cm). Bisect: `bsibones campin off`.
3. **Wrist bend sliders**: HANDS panel, per-hand "wrist bend P/Y/R" - they now
   tilt the HAND (and gun) about the grip while the forearm stays, i.e. an
   actual wrist bend instead of the arm sweep you rejected. Purely visual;
   aim is untouched. Report values that read natural (still unpersisted).
4. **Arms hide**: the style radio is gone; one mode with a "wrist cap depth"
   slider (arms=hide). Default 10 cm = the pinch you called decent; slide it
   and report where the stretch reads best (0 = the old collapse-at-grip).
5. **Fire origin**: unchanged and confirmed correct flat (the trace follows
   the controller). The "bullet from a fixed screen point" you saw is the
   visible tracer EFFECT - that spawn seam is next session's hunt, so expect
   it to look the same for now.
6. **Dynamic dot**: not possible yet on this build (the script trace surface
   is stripped); needs one more derivation session. The dot stays fixed-
   distance; the slider still works.

## S53 checklist (THE FP-RIG HIDE done right; branch `claude/bioshock-fp-rig-hide-fa6342`)

The hide gate ships ARMED: cutscene rig = **force-hide**, empty hands =
**whole limb bone-hidden** (grip + arm chain per side) outside holds. The
lever is BONE (HideBoneByName - the only lever that removes limb AND weapon
together; actor bHidden was measured INEFFECTIVE and comp SetHidden leaves
the weapon model floating). Everything is a control in F10 -> HANDS + MODEL:
the "hide rig" checkbox, the "cutscene rig" radio (force-hide / game-managed
/ cine off) and the "hide lever" radio (actor / owner / comp / bone). Escape
hatches: `bsihide auto off`, or the checkbox.

1. **FIRST MINUTE - the two round-4 falsifiers, rowboat save:** (a) the
   intro cutscene TEXT must render intact (the zero-scale attempt broke it);
   (b) the double/frozen controller hands must be GONE through the whole
   scene. Look around (left/right/down) mid-scene - the s53 sim measured the
   game itself hiding the rig (bHidden=1) through the no-hands phases, so
   anything you still see floating is a REAL finding, not our drive.
2. **The authored hand moments must SURVIVE**: the box handoff and any
   scripted reach must still show the game's own hand. If force-hide has
   eaten them (blank reach), flip the radio to "game-managed" and re-judge
   the same scene edge - that radio is the s53 A/B and either verdict is a
   result.
3. **If doubles persist in BOTH radio modes**: they are probably NOT the FP
   rig at all but the pawn's own body seen from the offset VR camera. One
   command while you look at them: `bsihide pawn 1` (hides the pawn mesh for
   the owner view; `bsihide pawn 0` restores). Report which lever killed
   them - this decides the next wiring.
4. **Empty-hand hide in gameplay**: with bare hands (start of game), both
   limbs should be GONE (no misrotated arms, no sprint-arms weirdness, no
   frozen hands). Draw a weapon - the weapon hand comes back driven; holster
   to bare - gone again. The skyhook-era left-only case: left may stay
   visible if only the rig-wide lever survived (accepted trade).
5. **Scene-end resume**: when the scripted scene closes, driven hands must
   come back within a beat (the release/re-arm edge), with no hidden-rig
   hangover (a rig stuck invisible = the watchdog failed - `bsihide status`
   and report the counters).

## S52 checklist (the input lane, THE CHEATED ARSENAL, the HUD quad, the cinematic gate; branch `si52-inf-input-arsenal-hud-cine`)

Four features this build, all flat-proven. Everything judged by eye has an F10
control (BSI section) - never alt-tab. Expected noise unchanged: award
dialogs replay per load; Enrage HOLD charges / RELEASE throws.

1. **Non-regression sweep FIRST (60 s)**: tracking, sync, both-hand aim,
   hole-on-dot, stance still dead (fire once, idle 3+ min), SHOULDERS still
   dead (fire the pistol, watch the left arm), flourish chord still fires
   (left thumbrest + A - and note the chord now pauses during cinematics).
2. **BODY FOLLOWS HEAD (the locomotion feel)**: look 90 deg left or right,
   push stick forward - you should walk WHERE YOU LOOK. A/B: F10 -> INPUT ->
   "BODY FOLLOWS HEAD" checkbox off = the old game-yaw walking. Judge
   comfort at partial angles (45 deg) and while strafing. KNOWN LIMIT: the
   pawn/body itself does not turn - past ~90 deg of head-yaw the weapon
   laser or rig may misbehave; report what you see, a body-yaw-sync lane is
   the known follow-up.
3. **RIGHT-STICK Y: the verdict.** Push the right stick up/down hard, watch
   the weapon model / muzzle vfx / aim: NOTHING should move (before this
   build the whole aim basis pitched). Also try it WHILE holding a grip
   (weapon/vigor cycle) - the kill must hold through the squeeze. Left/right
   on the same stick must still smooth-turn. Optional: F10 core input
   section has SNAP TURN (45 deg default) - flick right = turn right; snap,
   then walk - direction must compose with no double-turn.
4. **THE ARSENAL + PER-WEAPON TUNING (the calibration session).** In
   command.txt or console: `bsigive list` shows the carried slots;
   `bsigive <Archetype> [ammo]` grants + equips. Base roster:
   PistolFounder, MachineGunFounder, ShotgunFounder, CarbineFounder,
   HandCannonFounder, SniperRifleFounder, RPGFounder; vigors
   Plasmid_DevilsKiss, Plasmid_MurderOfCrowsFounder, Plasmid_BuckingBronco-
   Founder, Plasmid_UndertowFounder, Plasmid_VoltSwarmFounder, Plasmid_Charge
   (+ the owned Enrage). NOTE: giving a gun DROPS the currently carried
   replaced gun on the floor (the game's carry-2 rule) - pick it back up or
   re-give. THE TUNING WORKFLOW: hold a weapon, tune with the existing F10
   AIM (trim P/Y, origin F/R/U) and HANDS (trim/offset/scale) sliders -
   sliders address the hand HOLDING the weapon (right = guns, left = vigor)
   - then SWITCH weapons: the outgoing weapon's values AUTO-CAPTURE. F10 ->
   WEAPON PROFILES shows the live gun/vigor keys and has SAVE (writes
   weapons.ini). When the loadout feels right, SAVE THE GAME IN-GAME - that
   save is the calibration save. Values survive relaunch (weapons.ini loads
   at boot, applies on equip).
5. **THE HUD QUAD**: health/salts/shield/ammo/vigor now live on a floating
   panel (both eyes); the world image itself is HUD-free. Judge
   readability + placement: the panel size/distance sliders are in the core
   VR overlay section (set_hud_quad). Pause menu + upgrade/vending menus:
   open them - they should land readable on the panel and navigate with the
   pad. A/B: F10 -> HUD (I9) -> lane off = everything back in-world (flat
   style). If anything looks WRONG near the panel, `bsihud off` first.
6. **FULL-SCREEN EFFECTS**: take a hit (or RPG the floor) - the red hurt
   flash and the explosion must cover your WHOLE view, not just the panel.
   A/B: F10 -> HUD (I9) -> "Full-screen FX stay on the eye image" off puts
   the flash class on the panel instead. If a stray HUD element ever pins
   itself to your view (a leaked widget), report it and toggle the same
   checkbox - that is the discriminator.
7. **CINEMATICS - the regression + the radio.** Play into any Matinee scene
   (or use LOAD CHAPTER to reach one; you drive, per the navigation rule).
   Expect: your hand drive RELEASES (the game's authored hands and
   animations play, no fighting), the aim laser/dot disappear for the
   duration, and everything resumes clean at the end (stance/shoulders
   still good after). THE HEAD RADIO: F10 -> CINEMATICS (I9) - "Head look"
   (default, additive - the stereo-only-era behaviour you liked) vs "Fixed
   head" (authored camera untouched). Judge both in one scene.
8. **THE INTERACTIVE PROMPT (the raffle)**: if you reach the raffle (LOAD
   CHAPTER lane - ask first if save state is precious), the "throw the
   ball" prompt MUST accept your press even with a thumb resting on the
   left thumbrest (the chord pauses during cinematics). Positive control:
   in normal gameplay the same chord still fires the flourish.
9. **Two flat checks that moved to the headset** (the sim pad went dead in
   the last boots - harness, not the mod): (a) confirm the chord suspension
   A/B of item 8; (b) during a Matinee, confirm the gun does NOT track your
   controller (the aim substitution freeze).

Anything off: `bsibody off` (locomotion), `vrinput pitchkill off` (stick
pitch), `bsihud off` (panel), `bsicine force off`/`bsicine head look`
(cinematics), `bsibones fireglue anchor` (shoulders) - if a symptom survives
its lever, it predates this session.

### S52 round 2 (post-verdict fixes - re-judge these five)

1. **Snap turn direction**: flick right now snaps RIGHT (the drain sign was
   flipped back off your verdict).
2. **Cutscene start direction**: every cinematic hold now RECENTERS on your
   current head direction at its first frame - scenes should begin where
   you are looking, and gameplay resumes with that fresh baseline.
3. **Double hands in scripted cutscenes**: the detector now also reads
   APlayerController.bCinematicMode, which is what the first-person
   scripted scenes set (new-game intro). Expect ONLY the game's authored
   hands there now. `bsicine status` prints the bit if one still slips
   through - note WHICH scene.
4. **Bare hands (no weapon/vigor)**: they are their own profile entries now
   ("NoWeapon"/"NoVigor") - hold nothing, tune the HANDS trim/offset/scale
   sliders, and the values stick to the bare-hand state only. Scale near
   the floor = hidden arms, if the rotation never looks right.
5. **HUD size/position**: F10 -> HUD (I9) -> panel distance / width (this
   scales the icons) / height. `vrpreset save` makes it the default;
   `vrpreset saveas <name>` banks presets.
6. **THE ARSENAL BUTTONS**: F10 -> ARSENAL (I9 cheat) -> GIVE ALL (or
   per-weapon buttons) - grants land on the next game tick. NOTE: works
   from any save at/after the fair; the pre-raffle intro has no item
   assets to load from (measured), and nothing to tune there anyway.

## S51 checklist (the SHOULDERS kill, the FOV-edge discriminators + THE EDGE-TELEMETRY RUN; branch `si51-inf-shoulders-edge-fx`)

Two fixes/instrument sets this build. The fire-swing fix is flat-proven A-B-A;
the FOV-edge items are DISCRIMINATORS - none claims to fix the drift, together
they guarantee the next session starts from data. FX-origin: no behavior
change (three more lanes falsified flat; nothing to judge).

1. **Non-regression sweep FIRST (60 s)**: tracking, sync, both-hand aim,
   hole-on-dot, locomotion, stance still dead (fire once, idle 3+ min),
   flourish chord still fires (left thumbrest + A).
2. **THE SHOULDERS: fire the pistol and watch the LEFT ARM, not just the
   hand.** s50 pinned the hand while the arm flailed around it (133 deg
   measured); now the WHOLE hand renders its ready articulation rigidly on
   the controller through each shot. Expect: no arm/shoulder jump at all.
   A/B: `bsibones fireglue anchor` (or F10 -> BSI -> uncheck "full-hand")
   brings the s50 arm-flail back within one shot; `bsibones fireglue full`
   restores. Side effect to judge: the RIGHT hand's recoil articulation is
   now also frozen for 1.5 s per shot (the s50 residual wiggle is gone) -
   if the dead recoil reads WRONG, say so; `anchor` is the compromise mode.
3. **THE HAND-QUAD ONE-LOOK (the FOV-edge discriminator).** `bsicam handquad
   on` (or F10 -> "HAND REF QUAD") - a small red dot rides your right
   controller, drawn by the COMPOSITOR at the located grip pose (correct by
   construction). Hold the right hand at fixed depth, sweep LEFT of center
   (where the drift is worst), watch the dot vs the rendered hand model ONCE:
   - they SEPARATE (laterally or in depth) => the error is in the game-render/
     projection/submission lane - the hand's world position is right and the
     picture of it is wrong;
   - they move TOGETHER (both drift toward you) => the composed hand position
     itself bends off-center - the compose chain re-opens despite the s50
     inspection.
   Either answer kills half the remaining hypothesis space. `bsicam handquad
   off` when done.
4. **THE VIEW LOGGER (one command, 5 seconds).** Any time mid-session:
   `bsicam viewlog`. Ten frames of VDXR's located per-eye poses land in the
   log with derived lines - eyeSep (expect ~your IPD, lateral), **cant (the
   question - sim baseline is 0.0000)** and per-eye fov asymmetry. Nothing to
   look at in the headset; the log is the deliverable.
5. **THE EDGE-TELEMETRY RUN - the session's insurance policy (~2 min).**
   Leave every default ON (eyetag, fireglue, clamp). Then:
   a. `bsicam edgelog on` (console or command.txt - it confirms ARMED in the
      log; safe for minutes, zero per-frame I/O).
   b. Stand still in an open spot, head as still as you can, eyes free.
   c. HOLD the right hand at a fixed comfortable depth (~40 cm) and sweep it
      slowly LEFT -> center -> RIGHT and back, ~5 s per traverse, **4 full
      traverses** (~60-90 s). Exaggerate nothing; the drift you normally see
      is exactly the signal.
   d. `bsicam edgelog off` - it flushes `edgelog-<n>.tsv` into
      %LOCALAPPDATA%\BioshockVR\bsi\ and logs the path. Done - nothing to
      judge; the file lets the next session plot where the chain thinks the
      hand is at every stage and see which stage bends. OPTIONAL but
      valuable: repeat a-d once with `bsicam eyetag off`, then `eyetag on`.
6. **Flourish lead re-judgment**: the chord now leads by 200 ms (s50's ~2 s
   lead was rejected). If the gesture start reads clipped, tune live:
   `bsiflourish lead 400` (etc., 0-10000) until it reads right, and report
   the number that felt best.
7. Anything NEW-wrong near the weapon/vigor-hand models: `bsifx off` first
   (unchanged from s50), then `bsibones fireglue anchor`.

Expected noise: unchanged (award dialogs on checkpoint loads; Enrage HOLD
charges / RELEASE throws). The one deliberate feel change is item 2's frozen
right-hand recoil.

## S50 checklist (eye tags, THE FLOURISH BUTTON, the fire-swing kill; branch `si50-inf-fx-edge-flourish`)

Three levers this build, all default ON, all flat-proven, each with its own
in-headset A/B. The frozen-FX family (charge plume, ready sparkle, muzzle
flash, tracer) is UNCHANGED - the hunt was re-scoped by your call and its
instruments are banked for a later session.

1. **Non-regression sweep FIRST (60 s)**: tracking, sync, both-hand aim,
   hole-on-dot, locomotion, the stance still dead (fire once, idle 3+ min -
   the s49b kill is untouched). Anything off: the three new checkboxes/
   commands below first.
2. **THE FLOURISH BUTTON: touch the LEFT THUMBREST and press A.** Expect the
   full show-off gesture on the vigor hand ~2 s after the press (the graph
   blends into the lowered lane first), the pose returning to ready ~5 s
   later, NO 101-deg settle stick, and NO jump firing while your thumb is on
   the rest (A alone still jumps). `bsiflourish` from the console fires the
   same lane. Judge: does the gesture read right? Is the ~2 s lead
   acceptable, or should the window/lead be tuned (both are constants -
   kFlourishLeadMs/kFlourishTailMs)?
3. **THE FIRE-SWING: fire the pistol and WATCH YOUR LEFT HAND.** It should
   stay exactly on the controller through the shot (the 95.6 deg whip is
   killed). A/B: `bsibones fireglue off` brings the whip back within one
   shot; `on` restores. Also judge the RIGHT hand through shots - the glue
   window also damps the weapon-hand recoil visual for 1.5 s (measured 21 ->
   ~6 deg residual): better, worse, or tune?
4. **THE FOV-EDGE DRIFT: the eye-tag A/B.** Hold your right hand at a fixed
   depth and sweep it LEFT of view center, then RIGHT (the s49b symptom:
   left pulls closer, right pushes away). Then `bsicam eyetag off` (the old
   tags) and repeat. Judge: does the drift shrink/vanish with the new tags
   (default, `eyetag on`)? If the asymmetry reads IDENTICAL in both modes,
   say so - that exonerates the pose tags and the next suspects are
   runtime-side. (Flat could not reproduce this one - the sim's runtime has
   no cant; the fix is correct-by-construction, your eyes are the
   instrument.)
5. Watch for anything NEW-wrong near the weapon/vigor-hand models or their
   socket FX (`bsifx off` is the bisect for the attach-walker edge cover;
   measured a steady-state no-op).

Expected noise: unchanged. The award-dialog queue replays on every
checkpoint load (a few A presses); Enrage HOLD charges, RELEASE throws.

## S49b checklist (THE STANCE KILL; branch `si49-inf-stance-lens-tracer`)

**The stance is KILLED in this build, default ON.** Flat-proven A-B-A: with
the kill armed the left hand held ready for 7+ minutes (every unkilled leg
entered the stance within 2.5-4 min); disarming brought the stance back on
schedule; re-armed, it self-derives every boot and refuses on drift.

1. **Non-regression sweep FIRST (60 s)**: tracking, sync, both-hand aim,
   hole-on-dot, locomotion (campin), wrist bend - none of these paths were
   touched; flag anything off.
2. **THE HEADLINE: the stance.** Load, fire once (the boot pose still starts
   in the stance - the established ritual clears it; the kill then prevents
   every re-entry). Then play/idle normally for 5+ minutes without firing:
   the left hand must NEVER drift into the bent-wrist stance or the raised
   "alert" arm. The in-headset A/B: F10 -> BSI FIDGET -> "STANCE KILL
   ('Lowered' clamp)" checkbox, or `bsifidget req clamp off` (stance returns
   within 2.5-4 min) / `bsifidget req clamp auto` (kill re-arms).
3. **Judge the side effect: the weapon never "lowers".** The kill pins the
   Morpheme graph in the raised/ready subgraph - the exact pose the hands
   compose from. Watch for any place it reads wrong (walking longer
   stretches, ziplines/Sky-Hook, scripted moments). Cinematics park the
   hands anyway; report anything odd.
4. **40-deg alert-relax**: should also be gone while the kill is armed (its
   pose pair rides the same lowered subgraph). If any left-arm drift
   survives, note WHEN (it is a separate param - TwoHandFallback_Weight -
   and we can clamp that too with one command: `bsifidget req clamp 24 0`).

Expected noise: unchanged. Anything off: the STANCE KILL checkbox first -
if the symptom survives the kill being OFF, it predates this session.

## S49 checklist (stance instruments + the lens verdict; branch `si49-inf-stance-lens-tracer`)

State honestly: **the idle stance is STILL NOT fixed.** s49 falsified two more
root levers with live A/Bs (six total): blocking the StartSubtleFidget native
impl outright, and blocking the 'SubtleFidget' anim action by name at the
Morpheme network's play entry - the stance re-entered through both. The
residual is a Morpheme-internal transition (the network's own state machine);
that hunt has the vocabulary (rqHandFidget) and the tooling staked for next
session. Expect the stance until then; fire once to clear it whenever it
appears. What IS new and armed:

1. **Two probe hooks ride every boot** (default probe, log-only): the impl
   hook (`bsifidget impl probe|block|off`) and the action hook
   (`bsifidget act probe|block <idx>|off`). If you want to see the timer chain
   live: fire once, wait 2-4 min, and the log prints the impl call + the
   'SubtleFidget' action with its caller. `bsifidget act block 41347` refuses
   the action (flat-proven mechanical; does NOT stop the pose - recorded).
2. **The lens question is SETTLED for the projection-split mechanism**: in
   gameplay with the viewmodel rendering, the decoder read lens1 100% /
   lens2 0% over 281 rounds - no foreground frustum exists on this build. The
   FOV-edge model drift you reported is NOT a BS1-style lens split. Next
   session approaches it as a headset-side/compositor question - nothing to
   judge in this build yet.
3. **Non-regression sweep** (60 s): tracking, sync, both-hand aim, hole on
   dot, locomotion pin (campin), wrist bend - all untouched this session;
   flat battery was not re-run in full, so flag anything off.

Expected noise: unchanged from s48. Anything off: `bsifidget act off` and
`bsifidget impl off` first (they are the only new default-on machinery, both
log-only), then the s48 levers; if the symptom survives, it predates s49.

## Testing discipline

- **Stereo-only.** Never judge a lens, scale or depth question from a mono screenshot. Mono
  screenshots cannot show disparity, perceived depth or reprojection behaviour, and on BS1 the flat
  pass and the headset verdict disagreed for exactly this reason across sessions 12-13.
- **Quad layers are invisible to flat testing.** The laser, the aim dot and the HUD panel exist
  only in the compositor. A flat screenshot proving nothing about them is not evidence they are
  broken.
- **Window captures are eye-phase-locked** under pair pacing, so do not expect a flat screenshot to
  alternate eyes.
- **Run the flat fire test before handing any build to the user.** A build that compiles and boots
  can still crash on the first shot - it did on BS1. Assert the game is still running, no new dump
  in the crash dir, and the mod's own status line looks sane.
- **A full playthrough is the release gate**, including the DLC.
- Debug-CRT trap: `sprintf_s` asserts on buffer overflow with a **modal dialog that freezes the
  game**. Use `_snprintf_s` + `_TRUNCATE` in anything that formats untrusted bytes.
- RTC trap: a probe hook whose argument count does not equal `ret imm / 4` pops
  `Run-Time Check Failure #0` and **writes no crash dump**. Press **Abort** on the dialog; never
  force-kill, which can leave the display mode unrestored.

## In-headset checklists

Every session that changes headset-visible behaviour ends with a numbered checklist in
[../STATUS.md](../STATUS.md), in the established structure:

1. **Non-regression sweep, 60 s, do FIRST**
2. The headline feature of the session
3. Specific judgments, each with a named live A/B command so a report can be bisected without a
   rebuild

Ending with "expected noise, not bugs", and "anything off, toggle `<lever> off` first - if the
symptom survives, it predates this session."

## DLC

All three are installed and **in scope for tuning**: bring-up on the base campaign, but aim, scale,
HUD and viewmodel calibration must hold in Burial at Sea 1 and 2 and Clash in the Clouds. Burial at
Sea adds weapons and a Vigor the base game does not have, so per-weapon aim profiles must cover
them.

`DLC\DLCA` (5.8 GB), `DLC\DLCB` (7.5 GB), `DLC\DLCC` (11.8 GB) - map these to titles in I0.
