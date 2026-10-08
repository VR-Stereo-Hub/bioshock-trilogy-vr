# BS1 arm IK v2: the Dishonored solver

Session 81 (2026-10-08), branch `claude/bs1-arm-ik-redo`. **Not yet run in a headset.**
Everything below was checked offline: host tests, a sweep on the real rig skinned through
its own weights in Blender, and a side-by-side audit against the Dishonored VR mod's
solver. No game was launched.

## Test it (one launch, one question)

1. Install as usual (`.\tools\build.ps1 -Install`) and play BS1 with a weapon out.
2. F10 > Hands + weapon (M7) > ARMS: **"ARM IK v2 (the Dishonored solver)"** is ticked by
   default.
3. **The question: does the forearm keep its shape and follow your wrist when you roll it,
   without the shoulder moving?** Hold the gun arm out, still, and roll the wrist slowly
   both ways as far as it goes. Past about 80 degrees the elbow should lift outward to take
   the rest.
4. Untick the box for the old solver and roll the same way. Tick it again before closing.
5. The log has `ARMIK2` lines twice a second per arm. They are always on.

Read an `ARMIK2` line like this:

| Part | Meaning |
|---|---|
| `reach x of y UU` | shoulder-to-wrist distance against the arm's full length |
| `CLAMPED, shoulder moved z` | the hand was out of reach, so the shoulder slid z UU |
| `roll / tracked / elbow swivel` | what the forearm carries, the wrist's measured roll, and the elbow lift |
| `continuing` / `fresh history` | whether last frame's elbow and roll were used |

## What it is

`src/game/bioshock1r/arm_ik.h` is pure: no engine reads, no globals. It is a port of the
Dishonored VR mod's full-arm IK (`src/game/dishonored/hands/arm_ik.h` and `pose_arm` in
`arm_rig.h`). That design is itself the BioShock left-hand fork's
([Owloeb/bioshock-trilogy-vr-lefthand](https://github.com/Owloeb/bioshock-trilogy-vr-lefthand),
MIT, revision `3b5b818`, `bones.cpp` `arm_ik`). So all three implementations of the
arm solver now share one algorithm. Dishonored's `docs/dishonored/ARM_IK.md` records the
derivation and its headset history.

| Behaviour | How |
|---|---|
| Reach | The shoulder stays at its nominal point. Only past 99.5% of full reach, or closer than 40% of it, does it slide along the shoulder-wrist line. The hand never leaves the controller. |
| Elbow | Law of cosines toward a body-relative pole: down, out by `elbow out`, a little back. The elbow is never allowed inside the arm's own side. Near a pole singularity, last frame's pole (kept in body axes) decides. |
| Bone orientations | Each segment's reference frame (direction plus bend-plane normal) is carried onto the solved one. The rig's bone-axis convention does not need to be known. |
| Wrist roll | Measured as the wrist's own turn against where the solved forearm carries the reference wrist, and unwrapped across frames. Past 250 degrees (no wrist rolls that far) it snaps back to the direct reading once that reading is inside 110 degrees, so it cannot stay wound. |
| Roll distribution | The forearm carries 70% of the roll at the elbow, ramping to 100% at the wrist. Past 80 degrees, up to 70 more go into lifting the elbow (a swivel about the shoulder-wrist line). |
| Scale | Bone lengths scale by hand scale times `arm length scale`. Thickness scales by hand scale. The length stretch lands on the limb's own +X axis (BS1's Biped rig). |

### The one adaptation

Dishonored poses its arm at draw time, as skin matrices over a copy of the mesh. BS1
writes the engine's evaluated skeleton instead: per-bone component-space hkQsTransforms,
which the engine skins with and the weapon attachment follows (ENGINE_NOTES, "Skeleton /
bone internals"). So `pose()` returns component-space bones: each reference bone carried
by its segment's solved rotation. That is the same thing as Dishonored's skin matrix times
the reference bind. Arm bones carry no attachments, so BS1 loses nothing by writing them
engine-side.

### Integration (`bones.cpp`, `solve_arm_v2`)

`solve_arm()` still builds the inputs that s70-s77 got right, and v2 shares them:

- the shoulder in the hands' frame (s74d)
- the DrawScale division (s72q)
- the actor transform about to be written, not the live one (s77)
- the equip-settle reference bank (s74)

From there, v2 replaces the solve, the twist and the writes. Off falls through to the
s70i-s77 solver unchanged.

The IK ends on the wrist as drawn, position and rotation. For the free hand that is its
retarget target; for the held hand it is the live anchor bone. This is Dishonored's
rule: the endpoint is the final hand, after animation.

The settle capture now also stores the reference wrist's rotation (`g_freeArmW0Q`).

History (pole and roll) is per hand, in body axes, and fresh for 250 ms. It resets when
the reference is re-captured.

`hands.ini` key: `armIkV2=1`.

### What BS1's rig says about the twist

Skin weight per arm bone on `NEWPlayerHands` (each arm the same):

| Bone | Weight |
|---|---|
| `ForeTwist1` (mid-forearm) | 371 |
| `Hand` | 306 |
| `Forearm` | 238 |
| `UpperArm` | 188 |
| `Clavicle` | 0 |
| `ForeTwist` (at the elbow) | 0 |

So the clavicle's motion is invisible, and the forearm's shape is a blend of `Forearm`
(70% of the roll) and `ForeTwist1` (85%). The 15-point gap between them is what
Dishonored's ramp was designed for. Its zero-roll forearm against a rolled helper shrank
the shaft to 45%.

## Validation (all offline)

| Check | Tool | Result |
|---|---|---|
| Solver unit tests (Dishonored's solve checks plus pose-level ones) | `.\tools\host-test.ps1 arm-ik` | **1,442 checks, 0 failures**. Covers: segment lengths, wrist join, frame covariance, roll followed through 200 degrees, swivel past 80, 70/85% ramp, scaled arms, and a negative control. |
| Real rig, 270 frames: reach, crossed, raised, wide, behind, roll 0-270 and back, -170, short and long arms, hand scale, over-reach, pulled to the shoulder | `.\tools\arm-ik-sweep.ps1` | 0 solve failures. Segment error 1.3e-5 UU, wrist join 1.5e-5 UU. Largest per-frame twist step 17 degrees, the sweep's own step, outside the deliberate beyond-250 section. |
| The same frames skinned through the original weights in Blender | `tools\blender\arm_ik_bake.py` | Forearm shaft keeps **at least 88.8%** of its bind radius (Dishonored's floor is 85%; its accepted candidate measured 88.1%). At arm length 1.25 it keeps 90.8%. Triangle area 0.008-2.1x bind: one sliver below 1% in the roll-45 transition, the same class as Dishonored's frames 48/87. Nothing grows past 10x. |
| **BS1 vs Dishonored, same body-relative poses, each on its own game's rig** | `.\tools\arm-ik-audit.ps1` | 150 frames. Elbow direction around the shoulder-wrist line differs by **at most 0.03 degrees**. Shoulder slide, tracked roll and swivel are **identical**. 15 key poses rendered side by side. |

What the audit cannot make equal is the elbow BEND for the same reach. BS1's upper arm is
1.44x its forearm and Dishonored's is 0.91x, so the joints sit in different places by
geometry, not by solver. The resting palm direction also differs, because each rig's
authored wrist does; in game the controller sets it.

**Not covered by any of this:** the live pose, which has animation, the actor written
every frame and the viewmodel FOV path; stereo; comfort; and whether the 0.35 `elbow out`
default reads right with the new pole. Dishonored ships 0.6. All of these need the
headset.

## Not taken from Dishonored, and why

- **The draw-time mesh copy** (`arm_ik_draw.inc`, mesh-to-reference mapping). BS1 can
  write the bones themselves; see above.
- **The nominal-shoulder model** (shared centre, width, a body yaw with a 25-degree
  deadzone). BS1 keeps its s74d shoulder: it is in the frame the hands are placed in, and
  it was signed off in the headset. Both arms already share one mirrored shoulder (s72d).
- **The per-pose stereo history cache.** BS1 solves once per game frame on the game thread
  and replays the written bones per pass (`g_cache`), so the two eyes cannot disagree.
- **`ArmIKGameArmInAnim` / `ArmIKGameArmShoulder`** (handing the arm to a game animation).
  BS1 has no takedowns. `scripted.cpp` already tells a scripted sequence from play, so
  this is the candidate if a scripted sequence shows the IK arm fighting the game's.

## Tools added for this

- `bs2gltf.py --rig-out`: the Havok reference skeleton, composed into component space.
- `tools\arm-ik-sweep.cpp` + `.ps1`: the production solver over a pose set, on the real rig.
- `tools\blender\arm_ik_bake.py`: skins sweep frames through the original weights,
  measures shaft radius and triangle area, renders.
- `tools\arm-ik-audit.cpp` + `.ps1` + `tools\blender\arm_ik_audit.py`: BS1 vs Dishonored.
  It compiles Dishonored's own `arm_rig.h` from that repo, so a change there is audited
  as it stands.

Outputs (rig, sweep JSON, renders) are game-derived and stay in
`<model_workspace>\ik\` and `verification\`.
