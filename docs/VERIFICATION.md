# Verification catalog - what can be checked, and by which tool

Read the decision table first. It maps **what you want to verify** to a tool, a
command, and how to read the result numerically.

Three tiers, and the difference matters:

1. **Numeric** - a number in a file decides pass or fail. No human, no judgement.
   Most per-change work lives here.
2. **Flat visual** - a picture an agent can compare against another picture with
   `img-diff.ps1`. Still no human, but the oracle is a threshold, not an opinion.
3. **Headset-only** - a person wearing the Quest 3 forms an opinion. Comfort,
   judder, world scale. The simulator does not touch this tier and never will.

The simulator moved a large amount of work from tier 3 to tiers 1 and 2. It did
not abolish tier 3. Asking the user to put the headset on for something the
simulator could have answered wastes a test session; claiming a simulator pass
settles a tier-3 question is worse.

---

## 1. The decision table

| I want to verify... | Tool | Command | How to read the result |
|---|---|---|---|
| The simulator itself works | `xrsim-selftest.ps1` | `.\tools\xrsim-selftest.ps1` | exit 0 = healthy. **Exit 1 = the SIM is broken; do not go looking in the mod** |
| The mod opens an XR session at all | `xrsim-launch.ps1` | `.\tools\xrsim-launch.ps1 -Game bs1` | Returns `Runtime = bvr-xrsim`, `SessionState`, `Frame`. Any other runtime name throws |
| The game reaches gameplay unattended | `boot.ps1` | `powershell -ExecutionPolicy Bypass -File .\tools\boot.ps1 -Attach` | exit 0 + `GAMEPLAY REACHED`; detector is `view state: GAMEPLAY` in the mod log |
| Frames are actually being submitted | `xrsim-state.ps1` | `.\tools\xrsim-state.ps1 -For "frame+60"` | Completes inside ~1 s at 90 Hz. `layersLastFrame >= 1` |
| Stereo produces two different eyes | `xrsim-shot` + `img-diff` | `$s = .\tools\xrsim-shot.ps1 -Out a`<br>`.\tools\img-diff.ps1 -A $s.Left -B $s.Right` | `$s.ProjViews -eq 2`; mean-abs-diff well above the ~0.4 noise floor. Near 0 = both eyes are the same image |
| The eye separation is real | capture JSON | `$s.EyeSeparationM` | Must equal the configured IPD (0.063 by default) to ~1e-5 |
| The claimed FOV matches the eye | capture JSON | `$s.ClaimRatioH` | 1.0 = the game's claim matches what the eye sees. Session 28's warp was 1.84. Anything off 1.0 is magnification in the headset |
| Head look drives the camera | `xrsim-cmd` + `img-diff` | `.\tools\xrsim-run.ps1 -Path .\tools\xrsim\headlook.xrs` | `img-diff` between captures rises monotonically with yaw and sits well above 0.4 |
| **The aim laser is on screen** | `xrsim-shot.ps1` | `.\tools\xrsim-run.ps1 -Path .\tools\xrsim\laser.xrs` | `$s.QuadLayers` jumps by the dot count. **No other tool in this repo can answer this** - quad layers never appear in a window screenshot |
| **The HUD quad is on screen** | `xrsim-shot` + `img-diff -Bands` | shot with and without `vrhud`, then `img-diff -A off_left.png -B on_left.png -Bands 16` | A `quad` layer in `space: view`; the CHANGED rows/cols bound its position |
| A quad layer, not the world, is what changed | two captures | capture, toggle the layer, capture again | `img-diff` between them isolates exactly the quad contribution |
| A controller button reaches the game | `xrsim-cmd.ps1` | `.\tools\xrsim-cmd.ps1 "btn a press 150"` | The mod's `vrinput status` counters, or the in-game effect via `img-diff` |
| A thumbstick moves the player | `xrsim-cmd.ps1` | `.\tools\xrsim-cmd.ps1 "stick l 0 1"` ... `"stick l center"` | before/after captures, `img-diff` well above 0.4 |
| The unfocused-pacing regression (session 33) | `xrsim-run.ps1` | `.\tools\xrsim-run.ps1 -Path .\tools\xrsim\unfocused-pacing.xrs` | Mod log gets `session state VISIBLE` then `SUBMISSION IDLE`, then `FOCUSED again`. Frame rate must not collapse |
| A headset going idle mid-session | `xrsim-cmd.ps1` | `.\tools\xrsim-cmd.ps1 "idle on 5000"` | With `vrpace thread on` the flat window keeps presenting; with `thread off` it stalls. Two log lines prove the session-28 fix |
| Connecting the headset mid-game | `xrsim-cmd.ps1` | `"hazard nosystem on"` ... `"hazard nosystem off"` | Mod logs `no headset connected ... will keep retrying quietly`, then a full bring-up with no restart |
| Teardown and clean re-bring-up | `xrsim-cmd.ps1` | `.\tools\xrsim-cmd.ps1 "hazard waitfail 1"` | Mod logs the failure, `session teardown`, then a fresh session after its 5 s cooldown |
| A flat (non-VR) render change | `game-shot` + `img-diff` | `.\tools\game-shot.ps1 -Out a.png` ... `.\tools\img-diff.ps1 -A a.png -B b.png` | Standing-still noise ~0.4; a real FOV change reads 4-7. Do it in a loaded save, never at the menu |
| Which screen region an effect covers | `img-diff` | `.\tools\img-diff.ps1 -A a.png -B b.png -Grid 16` | ASCII coverage map + `bbox=`. Flat top/bottom rows with every column changed = letterbox |
| Projection tangents / lens clusters | `dumpframe` + `decode-framedump` | `.\tools\game-cmd.ps1 "dumpframe full"` then `.\tools\decode-framedump.ps1 -Path <file>` | ONE cluster = the foreground lens matches the world; TWO = it does not |
| An input pipeline is deterministic | `vrrec` | `game-cmd "vrrec start"` ... `"vrrec play"` | `[rec] REC/PLAY mark` lines compare number for number |
| An engine value's address | `memscan` family | `game-cmd "memscani 100"` ... | Results in the mod log; narrow with rescan, then poke + shot + `img-diff` |
| **Comfort, judder, latency, lens warp** | **HUMAN, in the headset** | - | The sim models no distortion, no timewarp, no real display cadence |
| **World scale, IPD, "does the weapon swim"** | **HUMAN, in the headset**, via the F10 overlay | - | Anything judged by eye must be an overlay control, built before the test is asked for |

---

## 2. The simulated runtime (`bvr_xrsim32`)

### 2.1 What it is, and what it is not

`bvr_xrsim32.dll` is a purpose-built 32-bit OpenXR runtime that presents as a
Quest 3. It reports the same system name, layer limit, swapchain limit and
feature level the real device reported on this machine (recorded in
`docs/bioshock1/ENGINE_NOTES.md`), accepts the same session/swapchain/layer
calls VDXR does, and composites the submitted layers into per-eye PNGs the way a
compositor would.

It exists because BioShock is 32-bit and essentially nothing ships a 32-bit
OpenXR runtime - not Meta XR Simulator (x64, for Unity/Unreal editors), not
OpenXR-Simulator (x64), not `ox` (no D3D11). An OpenXR API layer cannot fill the
gap either: a layer needs a working runtime underneath, and with no headset VDXR
returns `XR_ERROR_FORM_FACTOR_UNAVAILABLE` so there is no session to intercept.

**It does NOT model:**

- lens distortion or chromatic aberration - "does the edge warp" is unanswerable
- timewarp, ASW or reprojection - judder and dropped-frame feel are unanswerable
- real display cadence or motion-to-photon latency - comfort is unanswerable
- VDXR's Wi-Fi encode and stream path, which dominates its real timing
- guardian, passthrough, hand tracking, haptics realism

What it proves is **geometry, content and protocol**: what was submitted, to
which layer, at what pose, with what FOV, and whether the two eyes differ.

**A pacing bug that reproduces in the sim is real. One that does not may still
exist on VDXR**, whose focus-granting behaviour is modelled here, not
reproduced. `focus policy vdxr|permissive` exists so neither reading is baked in.

### 2.2 How it is selected

Per-process, via the `XR_RUNTIME_JSON` environment variable, which the OpenXR
loader checks **before** the registry. `xrsim-launch.ps1` sets it, starts
`BioshockHD.exe` directly, and restores the variable in a `finally`.

**The registry is never touched.** The machine's ActiveRuntime stays on VDXR, so
putting the Quest 3 on still works with no switching, and a normal
`launch-game.ps1` run is completely unaffected.

Two traps, both of which fail by **silently falling back to VDXR**:

1. **An elevated shell.** The loader reads `XR_RUNTIME_JSON` through a secure-env
   path that returns nothing for a high-integrity process. `xrsim-launch.ps1`
   refuses to run elevated.
2. **A bad manifest path, a BOM in the JSON, or a 64-bit DLL.**
   `xrsim-install.ps1` checks the DLL's PE machine is `0x014C` and writes an
   absolute `library_path` with no BOM.

`xrsim-launch.ps1` then asserts the runtime **name** in the mod log and throws on
anything that is not `bvr-xrsim`. **Never skip that check** - it is the only
thing standing between you and a whole session of measurements taken against the
wrong runtime.

### 2.3 The scripts

| Script | Job |
|---|---|
| `xrsim-install.ps1` | Write the manifest, create the control dir, clear stale files. Throws if the DLL is missing or not x86 |
| `xrsim-launch.ps1` | Preflight, launch the exe directly with the env var, wait for a live session, assert the runtime name |
| `xrsim-cmd.ps1` | Send control lines and WAIT for the applied-count acknowledgement. Throws on a rejected command |
| `xrsim-shot.ps1` | Capture per-eye PNGs + the JSON sidecar; returns a parsed object |
| `xrsim-state.ps1` | Read `state.json`, or wait on a `-For` predicate |
| `xrsim-run.ps1` | Run a `.xrs` sequence from `tools\xrsim\` |
| `xrsim-selftest.ps1` | Is the sim healthy? Run before blaming the mod |

Two existing scripts gained one flag each:

- `launch-game.ps1 -PreflightOnly` - run the guards without launching, so
  `xrsim-launch.ps1` reuses them rather than reimplementing them.
- `boot.ps1 -Attach` - **mandatory in sim mode.** Without it, `boot.ps1` calls
  `Start-Process steam://rungameid/409710`, Steam does not know about a directly
  launched process, and you get a SECOND `BioshockHD.exe` on the real runtime.

### 2.4 Control commands

Angles in **degrees**, positions in **metres**, OpenXR axes (+X right, +Y up,
-Z forward). `<h>` is `l` or `r`. A duration ending in `f` counts **frames**
instead of milliseconds, which is the deterministic form under `pace step`.

Every command applies at a frame boundary, and a whole batch applies atomically -
`head pose ... ; trigger r 1.0 ; step 1` lands as one instantaneous rig change.

**Head**

| Command | Effect |
|---|---|
| `head pos <x> <y> <z>` | absolute position |
| `head rot <yaw> <pitch> <roll>` | absolute orientation |
| `head pose <x> <y> <z> <yaw> <pitch> <roll>` | both |
| `head move <dx> <dy> <dz>` | relative, world axes |
| `head movelocal <dfwd> <dright> <dup>` | relative, head axes |
| `head turn <dyaw> <dpitch> <droll>` | relative rotation |
| `head to <x> <y> <z> <y> <p> <r> <ms>` | smooth move (slerp + lerp, smoothstep) |
| `head orbit <degPerSec> <ms>` | continuous yaw sweep - the head-coupling instrument |
| `head height <m>` | eye height only (default 1.6) |
| `head valid on\|off` | drop tracking; `xrLocateSpace` clears the VALID bits |
| `recenter` | re-origin LOCAL on the current head pose, and emit the event |

**Hands**

| Command | Effect |
|---|---|
| `hand <h> grip pose <x> <y> <z> <y> <p> <r>` | absolute grip pose (also `pos` / `rot`) |
| `hand <h> aim pose ...` | absolute aim pose |
| `hand <h> point <yaw> <pitch>` | aim relative to where the head is looking |
| `hand <h> follow head on\|off` | park the hand at a head-local offset (default on) |
| `hand <h> offset <fwd> <right> <up>` | that offset |
| `hand <h> aimtrim <pitch> <yaw>` | grip-to-aim separation. Default -40 pitch, because on Touch the grip pose runs along the HANDLE and the aim pose along the pointing direction |
| `hand <h> valid on\|off` | untracked hand; the mod falls back to the view ray |
| `hands reset` | both back to default |

**Buttons and axes**

| Command | Effect |
|---|---|
| `btn a\|b\|x\|y\|menu down\|up\|press [ms\|Nf]` | face buttons; press defaults to 150 ms |
| `click <h> down\|up\|press [ms\|Nf]` | thumbstick click |
| `thumbrest <h> on\|off` | capacitive thumbrest touch |
| `trigger <h> <0..1>` / `trigger <h> pull [ms]` | analog trigger |
| `grip <h> <0..1>` / `grip <h> squeeze [ms]` | analog squeeze (drives the mod's 0.70/0.55 hysteresis) |
| `stick <h> <x> <y>` / `stick <h> center` | thumbstick, raw pre-deadzone |
| `input clear` | everything released |

**Pacing**

| Command | Effect |
|---|---|
| `pace free [hz]` | free-run, default 90 |
| `pace step` / `step [n]` | `xrWaitFrame` blocks until n frames are granted |
| `pace turbo` | never block |
| `step timeout <ms>` / `step onstarve advance\|hold` | starvation policy (default: advance) |
| `refresh <hz>` | display period and free-run rate |
| `idle on <ms>` / `idle off` / `idle max <ms>` | headset-idle stall, hard-capped at 20 s |

**Session state and hazards**

| Command | Effect |
|---|---|
| `state ready\|synchronized\|visible\|focused\|stopping\|exiting\|idle` | force a transition |
| `focus lose [ms]` / `focus regain` | the session-33 reproduction |
| `focus policy vdxr\|permissive` / `focus frames <n>` | whether regaining FOCUSED requires submitted frames |
| `hazard nosystem on\|off` | `xrGetSystem` returns FORM_FACTOR_UNAVAILABLE |
| `hazard waitfail\|beginfail\|endfail [n]` | next n calls return SESSION_LOST |
| `hazard swapchainfail\|attachfail on\|off` | creation / attach failures |
| `hazard clear` | all off |
| `instanceloss` | queue INSTANCE_LOSS_PENDING |

**Optics and capture**

| Command | Effect |
|---|---|
| `ipd <mm>` | default 63 |
| `fov <halfH> <halfV>` | symmetric-outer shorthand; `fov 55 48` gives the mod's log line `h=55.0 v=48.0` |
| `fov eye <h> <l> <r> <u> <d>` | full asymmetric, degrees |
| `fov quest3` | restore the defaults |
| `worldscale <s>` | scale head/hand translations |
| `shot [name]` | capture the next frame |
| `capture next <n>` / `every <n>` / `off` | capture scheduling |
| `capture size <w> <h>` | per-eye pixels (default 1032x1104) |
| `compose always\|oncapture` | whether to composite on non-capture frames (default oncapture, so idle cost is zero) |

**Misc**: `reset`, `status`, `log <text>` (a marker line, to correlate with the
mod log), `runtimename <s>`, `systemname <s>`, `profile touch|simple`.

### 2.5 `state.json`

Written to `%LOCALAPPDATA%\BioshockVR\xrsim\state.json`, atomically replaced, at
20 Hz free-running and every frame under `pace step`. Key fields:

| Field | Meaning |
|---|---|
| `runtime` | Must be `bvr-xrsim`. Anything else and you are on the real runtime |
| `frame`, `waitFrames`, `beginFrames`, `endFrames` | The frame gate. `waited - begun` and `begun - ended` expose the mod's 1:1 discipline |
| `framesDiscarded`, `endsOutOfOrder` | Protocol violations, counted rather than fatal |
| `sessionState`, `sessionRunning`, `actionsAttached` | Session status |
| `paceMode`, `refreshHz`, `stepsPending`, `idleBlockMs` | Pacing |
| `layersLastFrame`, `projectionViews` | The live layer census, updated every frame |
| `head`, `handL`, `handR`, `controls` | The committed rig |
| `cmdSeq`, `lastCmd`, `lastCmdError`, `errors` | The ack channel |
| `captureSeq`, `lastCapture` | Capture bookkeeping |

### 2.6 Capture output

Per capture, in `%LOCALAPPDATA%\BioshockVR\xrsim\capture\`:
`<name>_left.png`, `<name>_right.png`, `<name>_sbs.png`, `<name>.json`.

The JSON is the point. Beyond the poses and controls it carries:

- `layers[]` - every submitted layer with type, reference space, size, pose and
  premultiplied flag. This is how you tell a head-locked HUD (`space: view`) from
  a world-locked laser dot (`space: local`).
- `derived.eyeSeparationM` - assert this equals the IPD, and stereo is real.
- `derived.claimRatioH` - the game's claimed horizontal tangent over the eye's
  actual one. **1.0 means the claim is right.** Session 28's yaw warp was a 1.84x
  under-claim that took three sessions to infer; here it is one number.
- `stats.meanLumaL/R`, `stats.nonBlackPctL/R` - is anything actually rendered.

### 2.7 Scripted sequences

`tools\xrsim\*.xrs`. Directives: `@wait <ms>`, `@frames <n>`, `@shot <name>`,
`@mod <seam command>` (routes to the mod's own `command.txt`), and
`@assert <key> <op> <value>` against `state.json`.

Shipped: `smoke`, `stereo`, `headlook`, `laser`, `unfocused-pacing`.

---

## 3. The end-to-end agent workflow

```powershell
# 0. Is the SIM healthy? Answer this before blaming the mod.
.\tools\xrsim-selftest.ps1

# 1. Build and install.
.\tools\build.ps1 -Install -Game bs1

# 2. Launch against the simulator. Throws unless the runtime really is the sim.
$g = .\tools\xrsim-launch.ps1 -Game bs1

# 3. Reach gameplay. -Attach is mandatory: without it Steam starts a 2nd game.
powershell -ExecutionPolicy Bypass -File .\tools\boot.ps1 -Attach

# 4. Arm what you are testing, through the mod's own seam.
.\tools\game-cmd.ps1 "vrstereo on"
.\tools\xrsim-state.ps1 -For "frame+60"

# 5. Drive the rig and capture.
.\tools\xrsim-cmd.ps1 "reset" "head rot 0 0 0"
$a = .\tools\xrsim-shot.ps1 -Out "$env:TEMP\bvr\head_0"
.\tools\xrsim-cmd.ps1 "head rot 35 0 0"
$b = .\tools\xrsim-shot.ps1 -Out "$env:TEMP\bvr\head_35"

# 6. Assert with numbers, not eyes.
$a.ProjViews         -eq 2      # stereo projection submitted
$a.EyeSeparationM    -eq 0.063  # the eyes really are apart
$a.NonBlackPctL      -gt 50     # something was rendered
[math]::Abs($a.ClaimRatioH - 1.0) -lt 0.05
.\tools\img-diff.ps1 -A $a.Left -B $a.Right    # stereo: >> 0.4
.\tools\img-diff.ps1 -A $a.Left -B $b.Left     # head look moved the camera
```

---

## 4. Numeric thresholds

| Metric | Source | Expected | Fail |
|---|---|---|---|
| mean-abs-diff, same scene twice | `img-diff` | ~0.4 standing still | >2 with no change = the scene is moving; use a loaded save |
| mean-abs-diff, real FOV change | `img-diff` | 4-7 | <1 = the change did not land |
| mean-abs-diff, left vs right with stereo | `img-diff` | well above 0.4 | at the noise floor = both eyes are the same image |
| `nonBlackPctL` | capture JSON | >50 in gameplay | ~0 = black frame; check `sessionState` and `layersLastFrame` first |
| frames/s while FOCUSED | `state.frame` delta | near `refreshHz` | a collapse to ~10/s is the session-33 pacing bug |
| `layersLastFrame` | state.json | 1 projection + quads | 0 = nothing submitted; the mod is not in camera mode |
| `eyeSeparationM` | capture JSON | == configured IPD | 0 = mono submitted as stereo |
| `claimRatioH` | capture JSON | 1.0 +/- a few percent | far from 1.0 = the image is magnified in the headset |
| `errors`, `endsOutOfOrder` | state.json | 0 | any = read `lastCmdError` before trusting a capture |

---

## 5. Failure modes and gotchas

1. **The loader silently picked VDXR.** Elevated shell, bad manifest path, BOM,
   or a 64-bit DLL. Everything "works" but nothing renders and the session never
   focuses. `xrsim-launch.ps1` throws on any runtime name that is not
   `bvr-xrsim`. Never skip it.
2. **A second game instance.** `boot.ps1` without `-Attach` launches through
   Steam, which does not know about a directly launched process. Two
   `BioshockHD.exe` fighting over the GPU is the worst version of the standing
   one-game-at-a-time rule.
3. **Stale command files, twice over.** The mod's own `command.txt` re-applies at
   boot, and so would the sim's. `xrsim-install.ps1` clears both, and the sim
   discards a `command.txt` written before it started. A leftover
   `head rot 90 0 0` looks exactly like a camera bug.
4. **A capture before the session is live is black by construction.**
   `xrsim-shot.ps1` throws on the session state and on a non-advancing frame
   counter rather than saving a black PNG that later gets diffed against
   something real.
5. **Step mode does not freeze the game if you walk away.** `xrWaitFrame` blocks,
   but the mod's present thread abandons the wait after 200 ms and carries on,
   and the sim grants a frame after 30 s of starvation anyway. Step mode gates XR
   submission, not the game simulation - so it gives deterministic frame
   boundaries and poses, not a deterministic world. For repeatability, load the
   same save.
6. **Always `step off` at the end.** `xrsim-run.ps1` does it in a `finally`.
7. **Relative `library_path` in the manifest does not work for the game.** It
   resolves against the loading process's working directory, not the manifest's.
   `xrsim-install.ps1` always writes an absolute path.
8. **Focus is not the hazard here that it is in a real session.** The `-NoFocus`
   rule and "use the F10 overlay, not commands" exist because a real XR session
   drops FOCUSED on alt-tab. Under the sim, session state is a command, so
   foregrounding is harmless. **Do not generalise that back to headset sessions.**
9. **Quad layers still never appear in `game-shot.ps1`.** That is not a bug; the
   window is the projection source, not the compositor output. The difference
   between the two captures is itself a useful diagnostic.
10. **`refresh <hz>` is a real timestep change.** Raising it speeds up boots but
    can change engine-tick-dependent behaviour. Do not compare captures taken at
    different refresh rates.
11. **A direct launch still needs the Steam client running**, even though the exe
    has no DRM. If the process exits within 5 s, that is the first suspect.
12. **Captures are game-derived content.** They live under `%LOCALAPPDATA%` and
    are never committed - same rule as frame dumps and crash dumps.

---

## 6. What still needs a human in a headset

Comfort and judder. Lens-edge warp. Whether world scale and IPD feel right.
Whether the viewmodel swims. Anything the F10 overlay exists to judge - per
`docs/bioshock2/TESTING.md`, anything judged by eye must be an overlay control,
because alt-tabbing to type destabilises a real XR session.

The simulator moves the **evidence gathering** off the user. It does not move the
**verdict** on perceptual questions.
