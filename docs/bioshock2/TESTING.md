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

## Resolution (`vrres`, session 32; LIVE since session 37)

**BS2's lever is `Shared.ini`, not the file BS1 uses.** See ENGINE_NOTES section 1 - the
`[WinDrv.WindowsClient]` keys exist on BS2 and are ignored. **Since session 37 the apply is
LIVE**: the mod resizes the game window borderless to the exact client size, the engine follows
with its own ResizeBuffers, and the ini write is only the persistence for the next launch.

```
vrres                 # status: Shared.ini (governs) | Bioshock2SP.ini (synced) | live backbuffer
vrres list            # the preset table (same one the F10 picker shows)
vrres native          # 2064x2208 Quest 3 class; also flat/perf/sharp/max
vrres 2048x2048       # or `vrres 2048 2048`; any WxH, applies LIVE
vrres restore         # bring the window chrome back (client sized for the current backbuffer)
```

The F10 overlay's "RENDER RESOLUTION (applies live)" section is the same machinery: preset combo,
custom WxH, the auto-FOV preview for the selected size, Apply and Restore buttons.

Acceptance, end to end - **the only acceptance that counts is the backbuffer**:

1. `vrres native` -> expect `resolution: window client now 2064x2208 ... borderless` and
   `viewport set to 2064x2208 in Shared.ini [SharedOptions] (verified)`.
2. `fovaudit` -> `letterbox=1.0000 ... -> square pixels` at the new aspect, submitted ==
   option-derived tangents.
3. Relaunch. `first Present: backbuffer 2064x2208`, then after `vrstereo on` the self-heal line
   (`client ... < backbuffer ... re-applying the borderless fix`) - a chromed boot ALWAYS starts
   letterboxed on a desktop smaller than the render; the heal is the fix landing.
4. Safety check: `diff Shared.ini.bvr-bak-res Shared.ini` should show ONLY `ViewportX`/`ViewportY`,
   and the Bioshock2SP.ini diff ONLY the four keys in `[WinDrv.WindowsClient]` (lines 471-474).
   Any change inside `[XeDrv.XenonClient]` (509), `[PS3Drv.PS3Client]` (541),
   `[DurangoDrv.DurangoClient]` (573) or `[OrbisDrv.OrbisClient]` (605) is a section-scoping bug.

**[RETRACTED session 37]** ~~do not set a square resolution on BS2~~ - the square letterbox and
the "degenerate projection" were BOTH artifacts: the projection claim was a decoder bug (retracted
session 33), and the letterbox was the WINDOW's client clamping on the desktop, not an engine
behavior (ENGINE_NOTES session 37). Any aspect renders full-height with square pixels once the
client equals the backbuffer, which the live apply and the stereo self-heal guarantee. The
remaining true statement: pixels far from the eye's ~0.93 aspect fall outside the lenses, so
squarer-than-16:9 is now the RECOMMENDED direction, with `native` (2064x2208) the default choice.

## Lens / FOV measurement (session 32)

```
dumpframe full 2                                    # in GAMEPLAY - the menu has no world pass
tools\decode-framedump.ps1 -Path <dump> -ScanLayout -RayOffset 16 -FgBakeRvas @() `
    -OptionFov 100 -Aspect 1920x1080
```

- `-RayOffset 16` is BS2's (BS1's is 12, the default). `-FgBakeRvas @()` because the fg-bake RVAs
  are BS1-only.
- `-ScanLayout` brute-forces every offset and validates the structural signature - use it to
  re-derive after a game patch. **Derive at a full-height render** (letterbox 1.0000 in fovaudit):
  a letterboxed viewport scales the vertical slope term and the session-33 decoder handles it, but
  a clean dump removes the variable entirely. (The old "derive at 16:9, square is degenerate"
  wording is superseded - session 37 showed the letterbox was the window clamp, not the aspect.)
- `-Diff <other dump> -DiffFovs 100,130` compares two dumps taken at different FOV options and
  flags the floats that scale as `tan(fov/2)`. It assumes NOTHING about layout - the instrument to
  reach for when `-ScanLayout` finds nothing.
- `fovaudit` prints the live watch's verdict including `lenses=`. On BS2 expect **`lenses=2`** in
  gameplay: a world lens following the FOV option, and a fixed 60 deg one that does not.

## THE VIEWMODEL LENS MATCH (`fgfov`, session 33) - IN-HEADSET CALL OWED

**What it does.** BS2 renders the world with the FOV option and the viewmodel with a lens pinned
at 60 deg. One projection layer carries one FOV claim, so the weapon is displayed with an angular
gain of `tan(option/2)/tan(30)` - 2.06x at option 100. That is the "the weapon moves with my head"
report. `fgfov on` writes the live world FOV into the viewmodel lens every frame, so both match.

**Default OFF** until this checklist passes. `fgfov off` is the A/B and restores instantly.

### Flat first (30 s, no headset)

```powershell
.\tools\game-batch.ps1 -Game bs2 -Delay 3 "fgfov on" "fovaudit" "dumpframe full"
```

`fovaudit live` should read **`lenses=1`**, and the dump decoded with
`decode-framedump.ps1 -RayOffset 16 -FgBakeRvas @()` should show **ONE cluster**. Sweep the option
(`gfov 80`, `gfov 130`) and it must STAY one cluster at every value - a match that only holds at one
option is a baked constant, which is the failure mode. `fgfov off` must bring back TWO clusters
(world + `tanH 0.5774`) and restore `60.000` exactly.

**`lenses=1` IS NOT PROOF - use the dump.** The live watch samples ~12 of 400-600 constant buffers
on a fixed stride, and the foreground pass is only ~17 of them, so whether it lands in a given
sample is luck. `lenses=2` is trustworthy (a second lens really was decoded); `lenses=1` is the
ordinary outcome of not sampling the fg pass. Session 33 tried twice to fix that and both attempts
failed - reserving the head of the pass (the fg run MOVES: blocks 2..12 in two captures, 369..377
and 456..463 in a third) and rotating the stride phase (correct in principle, measured at **10
fps** - copying from a different set of dynamic buffers each interval stalls the render thread).
The dump sees every block and has no sampling question at all. Every conclusion in session 33 came
from dumps.

### In-headset (the part flat cannot answer) - USE THE F10 OVERLAY, NOT COMMANDS

**Press F10.** The overlay renders into the game's backbuffer, which IS the eye image, so it is
visible and usable in the headset. The first section is **"VIEWMODEL LENS  <-- TEST THIS"**: a
match on/off checkbox, a manual FOV slider, a live readout of the world/viewmodel angles, and a
save button.

Do NOT drive an in-headset A/B through `command.txt`. Two reasons, both learned the hard way in
session 33: the user has to reach a keyboard, and **alt-tabbing drops the XR session
`FOCUSED -> VISIBLE`, which preceded a hard freeze by 17 s**. `game-batch.ps1` asserts foreground
by default for exactly the same reason the poll needs it flat - **pass `-NoFocus` during any
headset session**, or better, do not use it at all and use the overlay.

Anything the user must judge by eye belongs in `draw_debug_ui()`, built BEFORE the test is asked
for.

### What to look for

Flat CANNOT judge this and must not be used to. On a monitor the matched viewmodel looks enormous,
because a 100-deg render squeezed into a small window magnifies anything near the camera. In the
headset that same render is stretched across your whole view. **Only the headset can say.**

1. VR, gameplay, weapon out. FOV option at its normal 100. F10 for the overlay.
2. Match checkbox OFF - the CURRENT behaviour. Turn your head side to side and look up and down.
   Note how the weapon slides/swims relative to your hands, and how big it looks.
3. Match checkbox ON - turn your head the same way.
   - **The question that decides it: does the weapon stop swimming?** That is what the lens match
     is for and it is the only thing being claimed.
   - Second question, separate: is the SIZE right, or is the Big Daddy helmet too big now?
4. If the size is wrong but the swimming is fixed, tick "use a manual value" and drag the slider -
   try 85, then 75. The number that looks right IS the measurement, not a failure.
5. Compare against `gfov 60` + match OFF, which is the known-good reference from session 32
   ("the weapon looks correct now and doesn't move"). The match at option 100 should feel like
   that did, with a much wider world.

**Known open question.** The viewmodel rig's apparent SIZE is coupled to this FOV value, not only
its lens - widening the lens also moves the foreground eye, so the rig grows. Whether the matched
state is geometrically correct or needs a second compensation is exactly what step 3's second
question decides. Do NOT tune any per-weapon or per-hand offsets before it is answered: a trim
fitted against a wrong lens stops being portable, which is the mistake BS1 made.

### Only after the above passes

Re-judge **world scale** and **IPD** - both are sliders in the overlay's "VR camera (M3)" section,
so this is also a headset-only, no-typing job. (`worldscale 100` is the current value, accepted in
session 26 - but that predates knowing the viewmodel lens was wrong, so treat it as unconfirmed.)
Then hit the overlay's save button so the verdict survives a relaunch - BS2 had no persistence at all before session
33, which is why no previous verdict could be re-checked against the same numbers.

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

## Test loadouts: getting weapons and plasmids on BS2 (researched session 32, UNTESTED)

**The console is not usable on either game** (user, session 32 - same as BS1 despite
`-allowconsole` being documented). So the working mechanism is the one BS1 already uses: **bind a
cheat command to an unbound key in `User.ini`.** Everything below is prepared, not proven - nothing
here has been run on BS2 yet.

### The mechanism, from BS1's working precedent

BS1's `User.ini.bvr-bak-cheatkeys` diff is the ground truth - these lines are live and working on
BS1 today:

```
F7=stat fps
F11=givebioshockweapons
F12=testAddAvailablePlasmid ElectricBolt
```

Format is `<KEY>=<command>`, one per line, in a key-binding section. Press the key in game, the
command runs. A Steam community post confirms the same approach for both remasters and notes that
making `User.ini` read-only is NOT necessary.

### THE SECTION TRAP - the same class of bug as the viewport keys

BS2's `User.ini` carries the same key names in **at least seven sections**: `[Default]` (133),
`[RadialActive]` (477), `[RadialActive_TriggerSwap]` (760),
`[RadialActive_SouthpawTriggerSwap]` (1044), `[GathererChoice]` (1328), `[RescueVentChoice]`
(1629), and more. **Binding in the wrong one silently does nothing in normal gameplay** - exactly
how `Bioshock2SP.ini`'s five viewport sections would have swallowed a resolution write. The
gameplay binds are the ones in **`[Default]`**.

**Free keys on BS2 today: `F9` and `F12`** (both `F9=` and `F12=` are empty at lines 249 and 253),
plus `F23`/`F24` at 424-425. Note BS2's layout differs from BS1's: BS2 uses F7/F8 for
EquipAbility7/8 and puts QuickLoad/QuickSave on F10/F11, so **do not copy BS1's key choices**.

### Command ladder - try in this order, broadest first

Names collected from community sources; **none verified on this install.**

| command | expected effect |
|---|---|
| `GiveAll` | all obtainable weapons + 999 of every ammo type |
| `IGBigBucks` | money (sources disagree: 500 or 600 dollars) |
| `GiveWeapon Weapons.PlayerDrill` | one weapon; also `PlayerMachineGun`, `PlayerShotgun`, `PlayerSpeargun` |
| `GiveItem Plasmids.ElectroBoltBasicPlasmid` | one plasmid; `...MasterPlasmid` for level 3 |
| `GiveItem ShockGame.ActiveGeneticSlotUpgrade` | an extra plasmid slot |
| `exec fulleverything.debug` | everything: items, research, upgrades (the nuclear option) |
| `testAddAvailablePlasmid <name>` | BS1's plasmid command - may not exist on BS2, try it anyway |

### VERIFY BY EFFECT, NOT BY THE BIND EXISTING

This session's most expensive lesson applies directly: **a write that reports success proves
nothing.** A key bind that saves cleanly into `User.ini` is not evidence the command ran, and a
command name that is wrong for this game will fail exactly as silently as one that is right.

For each command, the acceptance is an observable change:

- weapon appears in the wheel / ammo counter moves -> `game-shot -Game bs2` before and after, and
  diff the two with `tools/img-diff.ps1`
- plasmid appears in the radial
- money changes on the HUD

If the effect cannot be observed, record the command as UNPROVEN. Do not build a test procedure on
a command that was never seen to do anything.

**Harness gap to close first:** `vrinput test press` composes XInput PAD buttons, not keyboard
keys, so nothing in the harness can press F9/F12 today. Either ask the user to press the key (they
are at the machine anyway, and this is a one-off per test session) or add a SendInput keyboard poke
to the harness. Ask before building the second.

### The authoritative source for item names is LOCAL, not the web

Both link targets that would carry a full item table are unreachable to tooling: the BioShock Wiki
console-commands page and the Scribd cheat sheet both return HTTP 402. Do not sink more time into
scraping them.

The real source is the game's own scripts: `BakedScripts/` in the BS2 install, via the existing
`tools/uscript/` decompile workspace (gitignored - and per the hard rule, **summarise findings
here, never paste game script into the repo**). That gives exact class names rather than
community-transcribed ones, which is what the `GiveItem`/`GiveWeapon` paths need.

### Fallback if no command lands: a prepared save

Exactly what BS1 fell back to. Saves are fair game (standing permission) and BS2's live in
`Documents\BioshockHD\BioShock2\SaveGames`. Make one save with everything unlocked in an open area
and reuse it as the test fixture - it also removes the per-session save-loading step that currently
needs a human.
