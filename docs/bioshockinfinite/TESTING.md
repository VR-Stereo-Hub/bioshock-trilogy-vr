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

A second surface exists in `XGame\Config\DefaultDesignerControlPresets.ini` (base64 `Data=`), which
lists `God Mode`, `Ghost`, `Prevent Death`, `GiveAmmo`, `Slomo 10/50/100/200/1000%`, `QuickSave`,
`QuickLoad` and the viewmodes.

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

Carried from BS1/BS2 and expected to apply here until proven otherwise:

- **The game pauses while unfocused**, so a command written to `command.txt` may sit undispatched.
  Follow every `game-cmd` with a `game-shot` (which holds focus ~2.5 s) when the poll does not
  land.
- **Stale `command.txt` re-applies at boot.** This bit BS1 three times, once producing a false
  negative when a `vrpreset save` re-ran at the menu and overwrote a tuned ini with defaults.
  **Clear `command.txt` before closing the game, not only before launching it.**
- **Never write `command.txt` with `Set-Content -Encoding utf8`** - PowerShell 5.1 adds a BOM which
  silently corrupts the first token. `game-cmd.ps1` uses `[System.IO.File]::WriteAllText`.
- **`img-diff.ps1` noise floors** (BS1-calibrated, re-calibrate for this game): standing-still
  gameplay ~0.4 mean, a real FOV change reads 4-7, and an animated menu moves ~3.8 on its own.
  **Do A/B in a loaded save, never at the menu.**
- **Loading the same save reproduces the same spawn viewpoint**, which is what makes A/B render
  comparisons valid.

Planned for I1 and worth knowing early: **the command poller moves into core and ticks from
Present**, not from an engine hook. On BS1 and BS2 it lives in the adapter and ticks off the camera
hook, so a skeleton adapter has no command surface at all until its first engine hook fires. On
Infinite it should work from the first launch.

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
