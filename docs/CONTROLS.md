# Controls and `BioshockVR.ini`

User-facing configuration: the controller map, and the comfort/scale keys that
have grown alongside it. **The ini is intended to be the source of truth**; the
F10 panel is intended to become a front-end onto it rather than a parallel store.

**Today it is not, and the direction of the override is the opposite of what you
would guess.** A saved `vrpreset.ini` loads *after* this file and wins — see
[The F10 / ini relationship](#the-f10--ini-relationship-and-why-it-is-not-finished)
at the bottom. The startup echo says so on every line it can.

Location: `%LOCALAPPDATA%\BioshockVR\BioshockVR.ini` for BioShock 1, and the
`bs2\` / `bsi\` subfolders for the other two — the games never share files, and
their face buttons genuinely mean different things.

The whole resolved table is echoed to `bioshockvr.log` at startup. **If a change
is not in that echo, it did not take.** A recognised key with an unrecognised
value is logged loudly; an unknown key is ignored silently, so check spelling
against the echo before concluding anything from what you see in the headset.

## Status: not everything listed is wired up yet

The ini is deliberately being written out **ahead of the implementation**, so the
target shape is visible and can be argued about before the code chases it. The
BRVR mod (`BioVRDev/Bioshock-Remastered-VR`) is the reference — its own
`BioshockVR.ini` is ~1,500 lines and this is the plan to reach parity.

**A key in this table marked *planned* does nothing today.** It is not a bug
report. Only the *live* rows have code behind them.

### Live

| Key | Values | Default (BS1) | Notes |
|---|---|---|---|
| `FaceLayout` | `passthrough`, `session19` | `passthrough` | `passthrough` = the game's own layout, nothing rearranged. `session19` = the older rearrangement (Touch B = jump, Touch Y = first aid); still what BS2 uses |
| `FaceA` `FaceB` `FaceX` `FaceY` | button name | from layout | Individual overrides on top of `FaceLayout` |
| `GripL` `GripR` | button name | `LB` / `RB` | |
| `StickClickL` `StickClickR` | button name | `LS` / `NONE` | BS1 eats R3: zoom is a comfort hazard in a headset and is unreachable by design |
| `FlickUp` `FlickDown` `FlickLeft` `FlickRight` | button name | `DUP` `DDOWN` `DLEFT` `NONE` | BS1 has three ammo types, not four |
| `ControllerDpadModifier` | `0`–`4` | `1` | 0 off · 1 right thumbrest · 2 R3 · 3 left grip · 4 left thumbrest. **Mode 5 (left hand near head) is not implemented** — it falls back to 1 with a log line |
| `ControllerDpadFlip` | `0`, `1` | `0` | 0 = right thumbrest + LEFT stick (walking stops while held) · 1 = left thumbrest + RIGHT stick (turning suspended instead) |
| `JumpOnR3` | `0`, `1` | `1` | Additive — the layout's own jump button still jumps. Yields when `ControllerDpadModifier=2` |

Button names: `A B X Y LB RB LS RS DUP DDOWN DLEFT DRIGHT START BACK NONE`.

### Gestures that are not in the ini

| Gesture | What it does |
|---|---|
| **Menu button, or X + Y** (left hand) | **Pause** (START). The X+Y chord exists because on many setups no menu button reaches the game at all - SteamVR claims the left one, the Meta runtime the right |
| **MODIFIER + menu button, or MODIFIER + X + Y** | **"What is this?"** (`ShowContextHelp`), and **holding it ~0.5 s opens the MAP** - `ShockPlayerController` gates the map behind `HintButtonHeld` / `HintHoldTime = 0.5 s`. The modifier is whatever `ControllerDpadModifier` is set to. With the modifier OFF, holding the menu button alone still reaches BACK as a fallback |
| **R3 + L3, tap** | Opens / closes the **F10 panel**. Point at it with the right controller, **right trigger clicks**, **right stick UP/DOWN scrolls**, and **right stick LEFT/RIGHT tweaks whatever slider you are pointing at** - relative to its current value, 1% of its range per step, so it never jumps to where you happen to be aiming (dominant axis, so scrolling never nudges a value on the way past). While it is up the right trigger and right stick are swallowed in-game (so a click cannot fire and a scroll cannot turn), and the crosshair, aim laser and aim dot are hidden. The **left stick still walks** |
| **R3 + L3, hold ~0.6 s** | **Recenter** (seated pose + view yaw). It takes the hold rather than the tap because it is rare and deliberate |
| **Modifier + selecting stick UP / DOWN** | Cycle the equipped weapon's ammo type. Pulsed, so one step per push - dominant axis only, diagonals ignored |
| **Modifier + selecting stick RIGHT, held ~0.5 s** | **The MAP.** Right is the game's hint button, and `ShockPlayerController` gates the map behind `HintButtonHeld` / `HintHoldTime = 0.5 s` - so this one direction is HELD rather than pulsed. A short push is the hint on its own |

### Live — comfort and scale

BRVR's key names, units and shipped defaults, so a config carries between the two
mods. **A saved `vrpreset.ini` overrides all three** (it stores them as
`handScaleL`/`handScaleR`, `wScale` and `headUpUu`), which is how an in-headset
F10 calibration is kept.

| Key | Range | Default (BS1) | Notes |
|---|---|---|---|
| `HandsScale` | 0.05–5.0 | `0.8` | Viewmodel hand size, 1.0 = authored. Independent of world scale by design — the rig can be the wrong size while the world is right. Also `vrhands scale both <f>` and the F10 "model scale" slider |
| `GunScale` | 0.05–5.0 | `0.8` | Weapon size, 1.0 = authored (and 1.0 releases the lane entirely). Uniform about the grip. Also `vrhands wscale <f>` and the F10 "WEAPON scale" slider |
| `CameraHeightOffset` | −100–100 | `9` | **In centimetres, + is up**, above the game's own eye point. The pawn's eye height was authored for a monitor and reads short in VR. Stored internally as UU; at the shipped world scale of 100 the two are the same number. Also the F10 "Head offset up (UU)" slider |

An out-of-range value is **refused and logged**, not clamped — a silent clamp
reads in the headset exactly like the key not being implemented.

### Planned — present in BRVR, not yet here

| Key | What it does in BRVR |
|---|---|
| `ControllerDeadzone` | Fraction of stick travel ignored around centre. BRVR ships `0.15` and pre-compensates for the game's *square* deadzone |
| `GripThreshold` / `GripHysteresis` | Grip press/release points. Index grips read high from a resting hand, which would otherwise leave LB/RB permanently held |
| `ControllerPitch` | Right-stick Y. BRVR ships `0` because its camera hook erases injected pitch ~8 ms later and it reads as a fight |
| `ControllerStickYToDpad` | Stick Y as d-pad directions |
| `ControllerPauseChord` | X+Y → START, because on many setups no menu button reaches the game at all — SteamVR claims the left one, the Meta runtime the right |
| `ControllerMode` | Whether a real XInput pad in slot 0 wins over VR input. BRVR's `0` was never a real merge — see its graveyard before building one |
| `ControllerLayout` | A separate axis from `FaceLayout`: 0 = literal Xbox, 1 = jump on right-A. **Deliberately not reused for `FaceLayout`** — same name, different meaning |
| Turning block | Snap turn and turn speed |
| Accessibility block | BRVR groups comfort settings together and leads with them |

### Not portable as-is

`ControllerDpadModifier=5` — the left-hand-near-head gesture. It needs
head-distance hysteresis (`DpadHeadDistance` / `DpadHeadHysteresis`) and the
grip pose rather than the aim pose. Deferred, not rejected: it is the only
modifier that costs no button, which matters on Index/Beyond/Varjo/Somnium where
the thumbrest is really a trackpad the thumb rests on.

## Hardware notes that decide the modifier

Carried over from BRVR, where they were learned from field reports:

- **Rift CV1 has no thumbrest sensor at all**, so modes 1 and 4 can never fire
  for it. Those users need `2`.
- **Index, Bigscreen Beyond, Varjo and Somnium** must also use `2`, for the
  opposite reason: their "thumbrest" is the trackpad, which is where a thumb
  naturally rests — and the modifier is *held*, so a resting thumb would
  suppress movement continuously.
- **Quest 2 and later** have a real thumbrest and should stay on the default.

## The F10 / ini relationship, and why it is not finished

`vrpreset.ini` is **rewritten wholesale** when the F10 panel saves, and drops
keys it does not recognise. That is why `xr.ini` (runtime selection) and
`BioshockVR.ini` (controls) are separate files rather than sections of it.

Until that writer becomes a read-modify-write that preserves unknown keys and
comments, every option added to a panel-written file is something the panel can
silently eat. **That is the blocker for "the ini is the source of truth and F10
is a nice way to use it"**, and it is worth fixing before the ini grows rather
than after.
