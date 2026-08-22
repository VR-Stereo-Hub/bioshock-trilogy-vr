# BioShock 1: the port plan from the BRVR mod

Two independent native VR mods for BioShock Remastered are being consolidated into this
repo. This file is the backlog for that: what moves, in what order, at what risk, and
what deliberately does not move.

**The other tree.** `Bioshock-Remastered-VR` ("BRVR"), a separate BioShock 1 VR mod built
by @BioVRDev: MSBuild `.vcxproj`, a `dxgi.dll` proxy, a flat `BioshockVR.ini` beside the
exe, and an end-user installer. BioShock 1 gets one repo, one issue tracker and one
release; BRVR becomes a source of features rather than a parallel download.

**Baselines compared.** This repo at `5bc5999` (v0.8.2, 2026-08-14). BRVR at `9e56d58`.
Comparison written 2026-08-15, ported here 2026-08-21.

**Every row is a claim about source, not about either README**, both of which oversell in
places and undersell in others. Each cites a greppable anchor so it can be checked or
disproved in one command. The negatives are the falsifiable half: each was established by
a search returning nothing across the whole of the other tree, and the search used is
stated so it can be beaten.

**Before the first line of code moves**, see *Licensing* at the bottom. It is a real gate.

---

## What BRVR has that this repo does not

| System | Anchor in BRVR | Search that found nothing here |
|---|---|---|
| **Square-deadzone pre-compensation on the movement stick** | `PrecompStickDeadzone`, `BioshockVR/Input/InputHook.cpp` | `precomp`, `inverse deadzone`, `deadzone.*invert` |
| **Turn-axis response linearisation** | `turnAxisMax` / `turnAxisExp`, same file | only a linear `g_turnScale` here |
| **World FOV guard** | `ClampWorldFov`, `BioshockVR/Game/GameState.cpp` | `vita`, `respawn.*fov`, `clampfov` |
| **Head bob, landing dip and damage shake removed at source** | `disableHeadBob`, `CameraHook.cpp`, anchor `>>> HEADBOB: camera base = Pawn.Location + EyeHeight` | `headbob`, `walkbob`, `viewbob`, `landing dip`, `damage shake` |
| **Per-movie-name screen routing** | `AnchorMovies` / `FollowMovies` / `SceneMovies` / `PanelMovies`, `Hud/DrawHook.cpp` | `AnchorMovies`, `movie name`, `screen name` |
| **Scripted sequences landing where they intend** | `GameState_ScriptedSequence()` and the M7 window machinery, `GameState.cpp` | scripted handling here is "leave the engine alone" (`aim.cpp`) |
| Two-handed grip on the barrel | `CameraHook_TwoHandGripped` | `two.?hand`, `foregrip`, `fore.?end` return only Infinite's unrelated `TwoHandFallback_Weight` |
| Gameplay haptics | `g_aHapticL` / `g_aHapticR`, `InputHook.cpp` | see the correction below |
| Quest arrow placed in the world | `DriveQuestArrow`, `CameraHook.cpp` | `quest arrow`, `objective arrow` |
| Per-plasmid calibration keyed on acquisition order | `ActiveAbility` at `pawn+0x0F24` | profiles here key on a resolved name string, which is arguably better |
| End-user installer suite | `dist/Setup.bat`, `CollectLogs.bat`, `Uninstall.bat`, with backup and restore, headset questions, VirtualStore awareness and Epic build support | install here is "copy the DLLs by hand" |
| Explicit Index / Vive / WMR binding tables | `InputHook.cpp` | plus the field-reported Index grip finding below |

**Correction on haptics, because the blunt version would be wrong.** The *runtime*
plumbing here exists and works: `shim_ApplyHapticFeedback` in `ovrshim_input.cpp`
forwards to OpenVR's `TriggerHapticVibrationAction`, and `xrsim` declares the haptic
output paths. What is absent is any BS1 gameplay code that fires a pulse; `patterns.h`
says vibration is *"untouched until M7 haptics"*. That makes this an easier port than a
cold one, not a harder one.

**The Index grip finding is worth carrying even if no code moves.** A field report on
2026-08-14 established that SteamVR's default Index binding maps grip to *pull*, which
reads high from a resting hand and eats the face buttons. No threshold value fixes it;
the binding has to be *force*. BRVR shipped two mitigations for it before learning that.

## What this repo has that BRVR does not

Recorded so the consolidation does not lose in one direction what it gains in the other,
and so nobody proposes porting these the wrong way.

| System | Anchor here |
|---|---|
| **Four cutscene signals beyond the bar draw** | `cinematic_hold()` is `bar_draw_active()` or `letterbox()`, the second a backbuffer pixel watch sampled at the head of Present (`letterbox_sample`, `hud_capture.cpp`). Combined in `openxr_runtime.cpp` with `is_gameplay_view()` (`body.cpp`, compares the view actor's **vtable** to `kShockPlayerVtableRva`), CalcView staleness, and a rendered-vs-claimed FOV mismatch, under hysteresis |
| **FName resolved to text** | `fname_text(int32_t index)` in `patterns.cpp`, walking `GNames` (`kGNamesDataRva`) to `FNameEntry` to a `wchar_t*` |
| ImGui in-headset tuning overlay | F10, every tunable live with save. BRVR uses numpad hotkeys and an ini |
| `xrsim`, a simulated OpenXR runtime | `tools/xrsim/` plus eight driver scripts. Verification without a headset |
| Crash minidump handler | `src/core/util/crash.cpp` |
| Hack minigame / loading / FMV screen routing | `screen_only()` |
| Auto-arm VR at launch | answers the game's one-shot startup gamepad check itself |
| Full-rate single-eye desktop mirror | BRVR's mirror runs at half rate, a known issue there |
| Two more games | BS2 and Infinite on a shared core |

**`fname_text()` retires a standing limitation in BRVR.** That tree reads raw
`{Index, Number}` pairs in `HandsProbe`, `ArmHide` and `GameState` and never resolves a
name: there is no `GNames` and no `FNameEntry` anywhere in it. That is why its
`ActiveAbility` had to be keyed on acquisition order and why bone identity was
established by measuring rather than by asking. Anything ported from BRVR that keys on an
index should be re-keyed on a resolved name on the way in.

**Not a gap in either direction: cutscene black bars.** Both builds remove them the same
way, by skipping the bar draw (`g_barActive` here, `HideCutsceneBars` /
`CutsceneBarVertices=29` there).

---

## Port list, ranked

By user-visible value per unit of integration risk. Tier 1 items are self-contained and
each is one headset run.

### Tier 1 - low risk

**1. `PrecompStickDeadzone`.** About thirty lines, pure function, no state. Drops into
`xinput_bridge.cpp` immediately after the `g_moveYawOffDeg` rotation block and before the
clamp.

**This repo has the bug today.** It rotates `out.lx`/`out.ly` so stick-forward tracks the
head, and the game then applies its stick deadzone *per axis*, from its own binding file:

```
XENON_LTHUMB_XAXIS=Axis xStrafe   Speedbase=1.0 DeadZone=0.225
XENON_LTHUMB_YAXIS=Axis xForward  Speedbase=1.0 DeadZone=0.225
```

Rotating moves magnitude between the two axes and the game shrinks each independently, so
the direction that comes out is not the direction that was sent. Modelling it as
`out = (|a| - d) / (1 - d)` reproduced the logged sent-versus-received pairs seven for
seven:

| sent | predicted | logged |
|---|---|---|
| -72.0 | -83.4 | -83.4 |
| -74.7 | -87.0 | -87.0 |
| +78.6 | +90.0 | +90.0 |
| +162.1 | +173.5 | +173.6 |
| -13.6 | -0.8 | -0.8 |

The saturation at 90 is the signature: once the forward component falls under 0.225 it is
zeroed outright and the walk collapses to pure strafe. The residual clusters at 11 degrees
and inverts with direction. The inverse to send instead is
`send_i = sign(u_i) * (|u_i| * m * (1 - d) + d)`, which recovers direction and magnitude
exactly and leaves a pure-forward push untouched.

The lesson attached to it, because it took three builds to find: **when a correction is
provably exact and the symptom survives, stop refining the correction and go and measure
what the other side actually received.**

**2. Turn-axis linearisation** (`turnAxisMax`, `turnAxisExp`). Replaces the linear scale
in the same file. The game's response curve is nearly vertical at the top of the stick,
which is the whole of "sometimes slow, sometimes fast".

**3. Head bob, dip and shake removal.** Self-contained in the camera hook. It ships
default-off in BRVR pending an EyeHeight check, so it should arrive here the same way,
with its own switch and off by default.

**4. Gameplay haptics.** The OpenXR action plumbing is already in `openxr_input.cpp` and
the shim already forwards pulses. What is missing is the BS1 code that fires them.

### Tier 2 - high value, needs design agreement first

**5. Per-movie-name screen routing.** The largest genuine feature gap and the one users
notice: the map, the manual, the upgrade machine, the Gene Bank and the whole
tonic/plasmid flow staying put in the room instead of riding the head. The HUD capture
here has no per-screen concept, so this is a port of a mechanism and not of a file.

Carry the cause with it, because it is not what anyone guessed: the trigger was **`paused`
silently anchoring the first page of every machine flow** while the second page unpauses,
not a movie name.

**6. Scripted-sequence landing.** Engine-side and offset-driven, so it ports independently
of the render architecture. Three rules travel with it, all bought expensively:

- **Never write `Controller.Rotation` while a sequence is moving the player.** Three
  balcony falls entered far right, straight on and far left all landed on the same spot
  with no write. With a heading substituted in, both straight-on runs landed badly wrong.
  The write itself is the damage.
- **Never let the window break mid-scene.** A per-frame "are you still in control"
  predicate over the HUD threw one landing 3.7 m.
- **Follow the camera alone, never the camera and the aim field.** They are not
  independent: the balcony's opening snap moves both by 41.03 deg/s, so following both
  applied it twice.

**7. World FOV guard.** Small, but read hazard 1 below before porting it.

**8. Two-handed grip.** Nearly finished in BRVR, one run from closed. Finish it there
first and port the settled version.

### Tier 3 - packaging, when the joint release is cut

**9. Installer suite**, Epic support, `CollectLogs.bat`, the startup config echo and the
VirtualStore handling. If this repo becomes the one place users go, this is what keeps the
support burden survivable. The config echo is already adopted here; the rest is not.

**10. Index / Vive / WMR binding tables**, and the Index grip *force* finding.

### Do not port

- **BRVR's camera-hook scan fix.** It fixes that project's own scanner; `patterns.cpp`
  here is a different one. The failure *shape* is worth knowing and the code is not: a
  byte-wise opcode search started inside an instruction's operand, the scan validated
  nothing, and nothing noticed the hook never fired. It presented as a
  1-in-256-per-launch silent total failure that looked like a broken runtime.
- **BRVR's pitch decoupling.** This repo uses a pitch servo, which is a falsified approach
  there (runaway feedback, froze the view and the hand). The servo works in this
  architecture. Do not relitigate it.

---

## The unbuilt features, against this architecture

| Feature | Verdict |
|---|---|
| **Cutscene detection by engine state** (`bHideHUD`, `HideMovie`, a reflection bridge) | **Retire the hunt.** BRVR spent three sessions and nine falsified approaches on it while `DrawHook_CutsceneBarsActive()` sat exported and unconsumed in its own tree, the same bar-draw signal `bar_draw_active()` uses here. The question was never whether the engine would answer |
| Holsters | congruent. Needs hand zones and a console equip command; `console_exec.cpp` is already the channel. Zones are new but small |
| Two-handed grip | congruent, and mostly built in BRVR |
| Hand poses and finger curl | congruent. `bones.cpp` already drives bone clusters against a frozen reference |
| Wrench swing hit detection at the tip | congruent and possibly cheaper here, since both `AWeapon::GetPerfectFireStart` and the ability variant are hooked |
| Gun-hand switching, left-handed mode | medium friction. "Right hand is weapons, left is plasmids" is assumed in more places here than in BRVR. It is an extension of left-handed mode plus weapon throwing; the hard parts are the weapon being its own actor and the left-hand models |
| Quick save/load on a chord | congruent and cheap. `console_exec.cpp` is the channel |
| Quest arrow | new work here, and BRVR's version is shelved with rotation unsolved anyway |
| Roomscale | needs `AActor::Move`. Neither build has it. Cost unchanged |
| Weapon throwing | unchanged. Far out on either architecture |

**One limit to write down rather than solve.** The cutscene detector is render-side on
both sides, so a scripted scene that draws no letterbox bars *and* keeps the player pawn
as the view actor is invisible to it. That is a known hole, not a reason to restart the
engine-state hunt.

**Nine falsified cutscene approaches, so nobody repeats them.** All were measured in BRVR
against this same game build: `myHUD.bHideHUD` (offsets correct, the DWORD never changed
once in 16 minutes, including while the HUD visibly appeared and disappeared, so suspect
`HideMovie` on the HUD instead); ViewActor divergence (never leaves the pawn); a
pitch-rate latch (latched during ordinary combat for four seconds); a pitch servo
(runaway); render-side unwind (made scripted scenes worse); cached view-target scans (no
signal); `LastPlayerInputContext` on the pawn (window correct, never locked, and the
*controller* copy is untried); console `get` (returns the class default object, not live
state); and an input-ignored detector (sound, but silent when a cutscene starts standing
still).

---

## Integration hazards

1. **The world FOV guard would blind the cutscene detector.** One of the four signals here
   is "the game renders a different FOV than it claims", measured on the bathysphere
   descent at 104 rendered against 130 claimed. `ClampWorldFov` snaps exactly that value.
   Ported naively it deletes the signal. Gate the guard on the gameplay verdict, or have
   the detector read pre-clamp.

2. **Different injection vectors.** `xinput1_3.dll` here, `dxgi.dll` in BRVR. This one
   collides with itsloopyo's head-tracking mod; that one collides with ReShade, DXVK and
   Special K. The consolidated mod probably wants both, chosen at install.

3. **Different build systems.** CMake here, MSBuild `.vcxproj` there. Ported files have to
   shed their `pch.h` assumptions. Both are `Release | Win32` only and must stay that way.

4. **Different config models.** `%LOCALAPPDATA%` presets plus the ImGui overlay here, a
   flat `BioshockVR.ini` beside the exe there. Every ported setting needs a home in this
   model, and anything ported should get an overlay row rather than only an ini key.

5. **Licensing. This is a gate, not a note.** This repo is MIT. **BRVR has no LICENSE file
   at all as of 2026-08-21**, which makes it all-rights-reserved rather than MIT by
   default. Contributing into an MIT repo relicenses the work as MIT. That has to be
   settled in writing by BRVR's author before the first line moves. It is one commit in
   that repo and it should land before the first port PR opens.

---

## Checking this document

Each "the other tree does not have X" row names the search that returned nothing. Re-run
it and any hit disproves the row. The searches were run case-insensitively across the
entire tree, including BS2 and Infinite, so a hit in `bioshock2r/` or `bioshockinf/` will
surface even though only BS1 is in scope.

The anchors are greppable symbols and banner text rather than line numbers, deliberately:
line numbers rot within a commit or two.
