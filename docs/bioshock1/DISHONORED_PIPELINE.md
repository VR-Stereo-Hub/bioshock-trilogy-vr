# The Dishonored VR mod's hand pipeline, end to end, and what BS1 has of it

Session 86b (2026-10-08). Read from `C:\dev\Dishonored-VR` at its head that day:
`src/game/dishonored/hands/mesh_split.cpp`, `hand_frame.h`, `arm_rig.h`, `arm_ik.h`,
`arm_ik_draw.inc`, `weapon_attach.cpp`, `anim_policy.h`, `anim_state.{h,cpp}`,
`docs/dishonored/{VR-33-HANDS-AND-WEAPONS,ARM_HAND_SPLIT,ARM_IK,HAND_POSE,ANIM-HANDOFF-PLAN}.md`,
and the live `dishonored_vr.ini`. The tester's brief: "Dishonored did the hands, hand
animations, IK, and handing the animations back to the game for the whole arm for selected
animations. Research its entire pipeline. I am willing to do a radical overhaul for them to
function the same."

The short version: **BS1's one drive (mode 4) already is Dishonored's hand mechanism, done on
the skeleton instead of the draw.** The parts that are not there yet are the grip calibration,
the hand-back for game animations, and the game-arm share during a hand-back. Section 3 is
the plan, stage by stage.

## 1. Dishonored's pipeline, stage by stage

Dishonored cannot write its engine's bones, so everything happens at draw time on a copy of the
arm mesh and its bone palette. BS1 can write bones (the evaluator hook, s82), so each stage has
a skeleton-side twin. "Palette" below = the array of skin matrices the engine animated this
frame; "D" = one rigid transform.

### 1.1 The split (`mesh_split.cpp` Ms*, ARM_HAND_SPLIT.md)

The arms and hands are one triangle list. The mod copies the draw's vertex and index buffers
once, classifies every triangle by the bones that weight it into handA / handB / armA / armB,
cuts the ones that straddle a plane square to the forearm at `WristCutA/B = -4.9` mesh units
from the hand bone (clipping them, so the cut is a circle), and caps each stump with a disc
in the ring's dominant texel colour. It then draws the hand classes from its own index buffer
and suppresses the rest. Hands only, arms, or everything are modes.

**BS1 twin.** None needed for placement: BS1 moves the hand's bones, the engine skins the one
mesh. For "hands only", BS1 has no cut; the nearest thing is BRVR's `HideBone` rule - pin
the arm bones at the wrist with zero scale (s86b). A vertex weighted to both ForeTwist1 and the
hand then pulls toward the wrist point instead of flying off; the look is a pinch at the cuff,
not Dishonored's capped disc. A real cut would need a mesh copy, which BS1 has never done.

### 1.2 The palette and the source frame (`palette_capture.cpp`, `MpSourceFrame`)

Each hand's palette is captured from the ORIGINAL draw, and one slot per hand is frozen as the
"dominant" (wrist) slot. `MpSourceFrame` reads that slot's 3x3 each frame, splits off its uniform
scale and refuses anything sheared or mirrored: that is the animated wrist's orientation
`R_src` and its position `q_local` in component space. `WristReference` (VR-188) measures the
offset between the wrist slot and a "vote" slot once and keeps it across sleeve rebuilds, so a
level load cannot re-measure it at a different finger pose and turn the hand.

**BS1 twin:** `g_evCopy[wrist]` - the engine's evaluated wrist, position and quaternion, copied
in the evaluator hook the moment the engine finishes (s82 P1). Same thing, no decomposition
needed because hkQsTransforms are already p/q/s.

### 1.3 The target: `palm_target(O_C, G, trim)` (`hand_frame.h`)

```
O_C        = B * F * transpose(R_H) * R_C      the controller's orientation in draw space
d_cam      = k * (p_controller - p_head)       its position, k = WorldScaleUU * DriveGain
PalmTarget = [ O_C * G * Trim.r | d_cam + (O_C * G) * Trim.t ]
```

- `G` is the **grip calibration**, one rotation per hand (plus a parity sign, because the draw
  basis is a reflection on that game). It is SOLVED, not tuned: SHIFT+F7 snaps the hand to the
  game's own animated orientation at that instant, `G = O_C^T * R_src_world`, and saves it.
- `Trim` is ONE rigid nudge per hand (and a second left trim while a power is held), stored in
  the calibrated palm frame (`TrimLT*/LR*`, `TrimRT*/RR*`): position in metres, rotation as
  extrinsic XYZ degrees. Live values: L +2.6/+1.2/+4.8 cm, +5.6/+7.5/-1.2 deg; R +3.6/+1.8/+1.5 cm,
  -36/+72/+17 deg.
- Editing (`MpTrimViewStep`, F10 Hands and the numpad): a press steps the hand right/up/forward
  **as the player sees it** - in the head's yaw frame - and that step is converted into the palm
  frame at the instant of the press, `Trim.t += (O_C G)^T * step`. The stored trim never changes
  kind, so a head turn afterwards moves nothing: the trim rotates with the palm.
- `ModelScale` 0.85 scales D about the target palm (`scale_about`), so the hand shrinks around
  the controller and anything held shrinks with it.

**There are no "pivot" values, because the pivot is built in:** the point D places at the
target is the **palm anchor** - `MpAnchorPos`, a 12-vertex patch in the middle of the hand
(taken beyond the wrist cut, VR-188), skinned by the current palette - never the wrist bone.
The hand turns about its palm, where the real hand holds the controller, so a wrist turn
cannot swing it. One translation, one rotation, per hand; the "grip" is the calibration G and
the per-hand offset is the trim.

**BS1 twin and the gap.** `m4_frame` builds `T = controller grip pose x rotation trim`, plus a
placement offset and a palm-frame offset. The rotation trim IS G*Trim.r, tuned by eye (s72y:
"the off hand's rotation already matches the controller untuned"). The two offsets are the
trim's translation, split in two frames - redundant, and until s86b one of them was in the
CAMERA frame, which is the head-turn drift the tester found. s86b puts it in the controller's
frame. What is missing: the **calibration press** (solve the rotation trim from the engine's
live wrist against the controller) and the **view-frame step editing**. Both are listed in
section 3.

### 1.4 The correction and the build (`delta_from_target`, `MpBuild`)

```
D = [ R_Lt * Target.r * R_src^T | tgt - D.r * q_local ]     one rigid transform
newPalette[i] = D * originalPalette[i]                       for every bone of that hand
```

Every hand bone gets the same D, so the weighted skin blend commutes with it and the finger
animation survives (VR-33 section 1, `compose_commutes`). The hand is composed every draw, both
eyes, from the eye's own draw context, so the two eyes cannot disagree.

**BS1 twin: exactly this, since s86c.** `hand_compose::delta(target, wrist, palmLocal)` is
`delta_from_target` with the palm anchor, and `m4_compose` writes `D * A[i]` for every
cluster bone (27-44 right, 6-21 left), scaled about the palm, after each evaluation and
again at the scene build (s83). The anchor is R_grip (43) on the right and L_Middle1 (13) on
the left (`kBoneRPalm` / `kBoneLPalm`). s83-s86b had passed the wrist as the palm - the
"huge pivot". Host-tested against real clips (3,279 checks; the sweep pins the palm to
0.00002 UU through a 180-degree wrist swing).

### 1.5 The weapons (`weapon_attach.cpp`, VR-33 W2/W3)

A held weapon is its own draw. The mod identifies it by matching its component transform to
the hand's, then composes the SAME D onto its palette (`WaPublishCommon` publishes the hand's D
after the model scale). The weapon can be placed before either hand has drawn because the target
needs only the pose, the basis, G and the trim.

**BS1 twin:** the weapon is attached to bone 43 of the hand cluster, which D carries; its own
skeleton lane (`wskel_drive`) sizes it. Equivalent, free.

### 1.6 Hand poses (`HAND_POSE.md`)

The empty right hand takes the left hand's finger pose, mirrored across the plane between the
wrists, relative to each wrist: `P'[f_R] = P[wrist_R] * X * inv(P[wrist_L]) * P[finger_L] * X`.
Measured bone pairing, refusal if one finger does not pair.

**BS1:** not needed so far; BS1's empty hands are the game's. Possible later.

### 1.7 Full-arm IK (`arm_ik.h`, `arm_rig.h`, `arm_ik_draw.inc`, ARM_IK.md)

- A shipped reference rig (`dishonored_vr_arm_rig.bin`: named bone heads and the reference
  vertices/weights) is matched against the live draw, every vertex within 0.03 units, before
  the arm path is allowed at all.
- Body frame: `body.origin` is the draw camera corrected to the eye centre; `body.forward/right`
  from a filtered head yaw (`BodyYaw`: 25 deg deadzone, 1.5 s relaxation); head pitch and roll
  cancelled.
- Shoulders: `C = origin + fwd*F + right*R + up*U`, `S_l/r = C -/+ width/2 * R`. Live fit:
  -16 / 0 / -25 / 38.1 cm, `ArmLengthScale` 1.27, `ElbowOut` 0.6.
- Solve (`solve`): reach clamped to [max(|a-b|*1.05+margin, 0.4(a+b)), 0.995(a+b)]; the
  shoulder slides along the shoulder-wrist line when clamped; elbow by law of cosines toward
  a body-relative pole (down, out, a little back), never inside the arm's own side, history
  near a singularity.
- `pose_arm`: the IK endpoint is the wrist AS DRAWN (the final hand palette, after the hand-back
  blend); segment frames carried by `frame_delta`; wrist roll measured against the solved
  forearm, unwrapped, 70% at the elbow ramping to 100% at the wrist, beyond 80 deg an elbow
  swivel up to 70 deg.
- The whole original mesh is drawn with the arm regions' skin matrices from the IK and the
  hand regions' from the corrected palette. History (pole, twist) is per hand, in body axes,
  in an 8-entry per-pose-generation cache so both eyes and passes share it.

**BS1 twin:** `arm_ik.h` is a port of this solver (s81, audited to 0.03 deg), writing the five
sleeve bones instead of skin matrices; one solve per game frame, replayed per pass. s86 added
per-segment lengths because BS1's rig proportions differ (upper arm 1.44x the forearm vs
0.90x), measured in HANDS_DISHONORED.md s86 and verified by the audit (elbow gap 0.29 -> 0.002
arm-lengths). s85: the shoulder stays put and the arm stretches first (Dishonored lets the
shoulder slide at once). s86b: the origin is the head's pivot, not the eye.

### 1.8 The hand-back (`anim_state.cpp`, `anim_policy.h`, ANIM-HANDOFF-PLAN.md)

The game owns the hands for selected animations and gets them back smoothly:

- **Classification.** `anim::tick` reads the pawn's three FSMs (`m_pPlayerMasterFSM`,
  `m_pPlayerUpperFSM`, `m_pPlayerLeftArmFSM`) four times a second and matches the current state
  names against ini lists (`[Anim] HandBackMaster/Upper/...`): takedowns, chokes, mantles,
  cinematics, and - behind levers - sword swings (`HandAnimMelee`) and shots (`HandAnimFire`).
  Per hand: `handMask` bit 0 left, bit 1 right (a trigger sword attack owns the right hand only).
- **Timing** (`Handoff`): the game owns from the match; after the match ends a `releaseMs`
  hysteresis; then the weight ramps 1 -> 0 (to the game) over `HandBackBlendInMs` 250 and 0 -> 1
  (back to the controller) over `HandBackBlendOutMs` 350 with `smootherstep`; a reversal
  mid-blend covers only the remaining distance.
- **The blend** (`blend_transform` / `blend_transform_palm`): `D(weight)` slerps D's rotation
  from identity and interpolates its uniform scale and translation; with `SmoothBlend` the
  translation is solved so the PALM moves on a straight line between its game and controller
  positions (interpolating D's translation swung the palm around the mesh origin: median 8.9 uu,
  up to 98 uu off the path). At weight 0 the palette is the game's; the hands are drawn as the
  game animates them.
- **The arm during a hand-back** (`ArmIKGameArmInAnim`): the IK arm's skin matrices are
  lerped toward the game's own arm matrices by `1 - weight`, so at weight 0 the arm is exactly
  the game's clip. `ArmIKGameArmShoulder` re-seats the game's arm about its own wrist so its
  shoulder lands on the nominal IK shoulder (stretch 0.8..1.9, turn up to 45 deg), because the
  game draws its arm for a flat-screen camera and its shoulder sat out in front.
- **Cutscene arms.** A motion gate on the game's own arm bones (`arm_motion`: the second-
  largest spanning-tree edge of between-bone speeds, which excludes the mod's single rigid
  write) decides when a cutscene is animating the arms; still arms pointing behind the head
  are hidden by a fingerprinted pose (`CineHideStaticArms`). Both are experimental there.

**BS1 twin and the gap.** `hand_compose.h` already carries `blend` and `Handoff` (the pure half,
s82 P2). What BS1 does not have: the classifier (which scripted sequences own the hands; BS1's
`scripted.cpp` already tells a scripted sequence from play), the per-hand mask, wiring the
weight into `m4_compose` (`D(weight)` instead of `D`), and the game-arm share for the sleeve
bones (lerp the solved arm toward the engine's evaluated arm by `1 - weight`). BS1 has no
takedowns; its cases are plasmid casts, Eve hypos, gatherer tools, and scripted sequences.

## 2. What BS1's rig forces that Dishonored never met

| | Dishonored | BS1 |
|---|---|---|
| Rig proportions | upper 23.5 / forearm 26.0 (0.90x) | upper 43.3 / forearm 30.0 (1.44x): per-segment lengths (s86) |
| Actor scale | the palette's own scale, read each frame | the hands actor renders at DrawScale 0.80, which multiplies bone positions and not the skin: positions are written at `size / 0.8`, the .s channel at `size` (s86c) |
| Reference pose | a shipped bind pose, symmetric | the LIVE evaluated pose each frame; clavicles weight nothing on this mesh (s86) |
| Where the hands live | a mesh copy, drawn by the mod | the engine's skeleton, written after each evaluation |
| Weapon | a separate draw, matched and carried | bone 43 of the hand, carried for free |

## Status after s87/s88 (2026-10-09)

BS1 now runs Dishonored's pipeline on its OWN palette (BS1 skins on the CPU; ENGINE_NOTES s87):
1.2-1.4 the hands composed on the skin palette (`palette.cpp`, the gather hook), 1.5 the held
weapon (its static-mesh draw's LocalToWorld), VR-183 the rigid anchor, VR-184 the rigid wrist
(a vertex copy), 1.7 the IK arm as sleeve palette matrices, 1.8 the hand-back (`handback.cpp`:
trigger melee and scripted states; physical swings stay tracked). Not yet: 1.1's mesh cut (the
palette collapse and the rigid wrist stand in), 1.6 hand poses, `fx_follow`, the game-arm
shoulder re-seat. Measurements: HANDS_DISHONORED.md s87, s88.

## 3. The overhaul, in order

Each step is one headset question. The mechanism already matches; these are the missing
pieces, in the order they pay off.

1. **Head-independent placement (s86b, built).** The placement offset in the controller's
   frame; the shoulders from the head's pivot. Question: turn the head left and right with the
   hands still - does anything move?
2. **Drawn size (s86b, built).** `size` is the drawn size (DrawScale divided out), default 0.83.
   Question: is the hand the size of yours?
3. **Grip calibration press.** A menu action: solve the per-hand rotation trim from the engine's
   live wrist orientation against the controller at that instant (`trim = controller^-1 x
   wrist`, in `xr_local_trim_quat`'s convention), as SHIFT+F7 does. Needs the inverse of
   `xr_local_trim_quat` (host-testable). After it the rotation sliders are a nudge, not a search.
4. **View-frame step buttons.** Dishonored's F10 rows: "move left/right/up/down/back/forward",
   "turn pitch/yaw/roll", 0.2/0.5/2 cm per press, converted into the controller frame at the
   press. The sliders stay for reading and fine work.
5. **The hand-back (P5).** `scripted.cpp` + the holdable's state as the classifier; a per-hand
   mask; `Handoff` timing (250 ms release, 250 in, 350 out, smootherstep); `D(weight)` with the
   palm on a straight path in `m4_compose`; the sleeve bones lerped toward the engine's own arm
   by `1 - weight`. Question: cast a plasmid and reload - does the hand go to the game's clip
   and come back without a snap?
6. **Game-arm shoulder re-seat.** Only if step 5 shows the game's arm with its shoulder out in
   front (BS1's viewmodel arms are authored for a flat screen too).
7. **Mesh-level hands only.** Only if the pinch at the cuff is unacceptable: a mesh copy with a
   cut and a cap, which is the one Dishonored stage BS1 has no twin for.
