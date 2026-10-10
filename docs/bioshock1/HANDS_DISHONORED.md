# BS1 hands, wrists, arms and animation vs the Dishonored VR mod: a 1:1 port plan

Session 82 (2026-10-08), branch `claude/bs1-hands-dishonored` (off `claude/bs1-arm-ik-redo`).
Research and offline derivation only: no game was launched and no mod behaviour changed.

Prompted by the first headset run of arm IK v2: "the arms and hands kind of suck, the
shoulders can still rotate, Dishonored felt perfect." This page compares the two mods,
end to end, from the controller to the pixels. It explains the BS1 symptoms from the log of
that run, and lays out how to make BS1 behave like Dishonored rather than tune it.

Sources: Dishonored `src/game/dishonored/hands/mesh_split.cpp` (`MpWorldTarget`, `MpBuild`,
`MpAnchorPos`), `hand_frame.h`, `anim_state.cpp`, `anim_policy.h`, `animation_rules.h`,
`arm_ik.h` / `arm_rig.h` and `docs/dishonored/{ARM_IK,HAND_POSE,ANIM-HANDOFF-PLAN}.md`. BS1:
`bioshock1r/hands.cpp` (`on_calcview`), `bones.cpp` (`drive`, `drive_free_hand`,
`solve_arm[_v2]`), ENGINE_NOTES sessions 12, 67-68 and 81, ARCHITECTURE decision log
session 68, and `tools\ida\hd1_skelinst_update.py`.

## 1. The verdict in one paragraph

The two mods differ in **which frame the rig lives in**, not in their IK.

- **BS1 (mode 3, the shipped "BRVR shape")** moves the whole hands ACTOR to the held
  controller and replays a frozen, captured pose inside it. A machine of settle windows,
  adoption, pins and rest-restores decides which animation shows.
- **Dishonored** never moves the arms component. It leaves it on the game camera and
  multiplies each hand's live, animated bone matrices by one rigid correction that puts
  the animated wrist exactly on the controller, every frame. Fingers animate, wrists never
  drift, and the arm solves in a frame that does not spin.

Every BS1 symptom below follows from the actor-carrying frame. The fix is to adopt
Dishonored's frame. BS1 can do that in BONES, engine-side, through a post-evaluation hook
on the skeleton. That is the twin of BS2's shipped `wfix`, and it is cheaper than
Dishonored's draw-time copy.

## 2. The evidence from the run (2026-10-08 16:51-16:57, `bioshockvr.log`)

- `hands.ini`: `mode=3`, so the actor carries the rig.
- **`ACTORWATCH`, 312 lines in six minutes.** Between our write and the next frame the
  engine changed the hands actor's rotation by up to **roll +116, pitch -44, yaw +38
  degrees**; typical lines are 20-60 degrees. BRVR measured the same thing (its S59: the
  game erases the actor's roll by 5-102 degrees).
- **`ARMIK2`: 244,311 solves, 0 failures, roll tracked steadily.** The solver did its job.
- So "the shoulders rotate" is the rig's FRAME moving under a correctly solved arm. The arm
  is solved in component space through the actor transform we INTEND (s77). Whenever the
  renderer draws with the engine's rewritten rotation instead, the whole component space
  is turned by that difference. The shoulder sits 40-70 UU from the actor origin, so it
  swings on that lever. s77 measured the residual at a median 3.7 / max 20.6 UU on the held
  arm. `late_write()` restores our rotation, but its own comment (hands.cpp:134) records
  that it runs too early for the engine's next CalcView.
- The same rewrite moves the HAND, which is why BS1 grew ROLLCHECK, ACTORWATCH, the
  offset-roll toggle, the grip/view knob split and per-weapon trims. Each compensates one
  face of a frame that the engine, not the mod, finally decides.

## 3. Side by side

| | Dishonored (accepted in a headset) | BS1 today (mode 3) |
|---|---|---|
| **Where the arms mesh lives** | Arms component on the game camera, untouched by the mod | The hands ACTOR is written to the held controller every frame (`hands.cpp` `on_calcview`, `late_write`) |
| **What the mod writes** | A COPY of the game's skinning palette, at draw time (`MpBuild`: `P' = D * P` per bone) | The engine's evaluated bone array in place (component-space hkQsTransforms, `SkeletonInstance` +0x48), plus the actor transform |
| **The source pose** | This draw's live palette, every frame | `g_ref`: a snapshot of an animated pose, captured at the equip settle and "adopted" from live frames only under rules (sway threshold, hold window, rest-restore) |
| **The engine's evaluation** | Every frame, untouched | Suppressed: the drive clears the dirty byte, and the engine re-evaluates about **1 frame in 19** (ARCHITECTURE s68d) |
| **The hand's correction** | `D = Target * inverse(Source)`, ONE rigid transform per hand, pivoted on the palm (`hf::delta_from_target`) | Actor = controller pose minus a grip offset along the model axes (s67), the cluster replayed verbatim, optional anchor pins (s70d/n) |
| **Target orientation** | `O_C * G * trim`: controller orientation in camera space (`O_C = B * F * R_H^T * R_C`), a calibrated grip G (SHIFT+F7 snapshot, parity-safe) and a palm-frame trim | The aim pose's rotation written to the actor (roll erased by the engine), plus the profile's rotation trim |
| **Pivot** | The palm anchor, rigid on the wrist bone (VR-183), so rotation can never orbit the hand | The actor origin, 44-58 UU from the hand. The grip offset IS the pivot (s67: a 15 cm height fix bought an 8-inch orbit) |
| **Fingers** | Animate live (they ride inside D) | Frozen with the cluster unless an animation is adopted |
| **Wrist during reload, equip, idle, item use** | **Locked to the controller.** Only the fingers move | Carried by the adopted animation relative to the actor, so the hand leaves the controller during recoil and reload by design (BRVR: "recoil is supposed to be visible") |
| **When the game gets the hand back** | Only listed states, blended per hand (`anim::blend`, smootherstep 250 ms in / 350 ms out, 250 ms release): assassinate, choke, climb, stunned, dead, possess, minigame, mantle, cinematics, fatality, grab corpse, and the sword swing (right hand). Fire animations are NOT handed back by default | Implicitly, through adoption: big frames are adopted; settle windows hand the whole skeleton back during equips |
| **Weapon** | A separate draw, placed through the SAME D (`WaPublishCommon`) | Rides bone 43 (R_grip) in the cluster: engine-side attachment follows the bone array for free |
| **Hand FX** (Heart, powers) | `fx_follow.cpp` re-parents each effect through the correction | Engine-side bone writes: attachments follow (ENGINE_NOTES s12) |
| **Arms** | `pose_arm` to the FINAL wrist; the shoulder anchored off the head with a lagging body yaw | `solve_arm_v2` (the same solver) to the drawn wrist, but in a component space that spins with the controller |
| **Stereo** | Corrections decided once per Present; per-eye offset measured from LocalToWorld | One solve per game frame; a repaint cache (`g_cache`, `reapply`) because the engine can re-evaluate between passes |

## 4. Why each Dishonored choice matters, and its BS1 equivalent

### 4.1 One rigid D per hand, from the LIVE pose

`MpBuild` multiplies every bone matrix of a hand by the same `D`. A weighted skin blend
commutes with a common rigid transform, so the engine's animation inside the hand is
preserved exactly while D moves the result (`compose_commutes` pins that).

`D = Target * inverse(Source)`, where Source is the wrist bone's frame in THIS draw's
palette. So:

- the wrist lands exactly on the target, whatever the animation did to it;
- everything the animation does BELOW the wrist survives;
- the correction never compounds, because it is always applied to the engine's output and
  never to the mod's own.

**BS1 equivalent.** For each hand cluster (right 27-44, including R_grip 43 and
IKbindLhandDummy 44; left 6-21), with
`A` the engine's freshly evaluated bones (component space):

```
D      = T_target * inverse(A[wrist])             one rigid transform, component space
B[i]   = D * A[i]                                  every cluster bone, incl. 43 and 44
```

Bone writes are engine-side, so the weapon (bone 43), its muzzle and any attached plasmid
effect follow, with no separate weapon pass. BS1's own `rigid_cluster` / mode-2 retarget
already does `B = R * ref` about an anchor. What it lacks is a LIVE source, the
inverse-source term (it rotated the reference by the controller instead of mapping the
reference onto the controller), and the palm pivot.

### 4.2 The source must be fresh every frame: a post-evaluation hook

Dishonored gets a fresh palette free, because it works on a copy at draw time. BS1 writes
in place, so it needs the engine to evaluate every frame and the mod to compose
immediately after. **That is exactly what BS2's `wfix` does** (BS2 ENGINE_NOTES s74: a
naked ret-hook on the `SkeletonInstance` dirty-flagged update, filtered to the hands rig,
headset-confirmed).

**Derived for BS1 this session** (`tools\ida\hd1_skelinst_update.py`, offline):

| What | BS1 value |
|---|---|
| `SkeletonInstance` vtable | RVA `0xE19ACC` (`patterns.h`) |
| The evaluator | **vtable slot `+0x9C`, RVA `+0x597CF0`** (808 bytes) |
| Its guards | dirty byte `+0x88` (set means evaluate; cleared after), and **freeze `+0x20` with time `+0x80` > 0 skips evaluation entirely** |
| Callers (each dirty-guarded) | `+0x319F55` in `+0x319F00`, which then calls the mesh instance's `+0x12C` / `+0x128` virtuals (the per-draw skinning path); `+0x434B28` in `+0x434A70`; `+0x598050` in the `+0xA0` gate; `+0x6429AE` in `+0x6423F0` |

Hooking the slot function itself catches every caller, as BS2's `wfix` does. In the hook:
copy the evaluated pose (the "source palette"), compose, and write. The drive then stops
freezing and stops clearing the dirty byte: it SETS it each game tick, so the engine
re-evaluates once per frame. The pass-1 evaluation composes once, and pass 2 draws the
same bones. This replaces the settle, adopt, pin and rest-restore machinery outright:
nothing has to decide which animation to show, because the animation always plays and D
always places it.

### 4.3 The actor stays where the engine puts it

Dishonored reads the component's LocalToWorld from the draw and never writes it. In BS1,
mode 2 already leaves the actor engine-placed: `Hands.UpdateLocation` pins it to the
camera every frame. The target is converted into component space through the actor
transform READ IN THE HOOK, which is the one the renderer is about to use. That removes
the whole ACTORWATCH class: there is no written rotation for the engine to overwrite, and
no roll for it to erase. The shoulder, built in the body frame and converted through the
same transform, lands where it was asked.

The game's view bob and weapon sway move the actor. Because D is solved against the
actor as it is, they cancel out of the hand, exactly as in Dishonored.

### 4.4 The grip, and the end of the pivot problem

`Target = [O_C * G * trim.r | p_controller + (O_C * G) * trim.t]`. The pivot is the palm
by construction, so no knob can create a lever. That is the s67 lesson ("grip = where it
pivots, view = where it sits") made impossible to get wrong.

**Carrying BS1's tuning across.** BS1's per-weapon profiles encode a placement:

```
wrist_world(mode 3) = Actor(controller, posFwd/Right/Up, rot trim) * ref[wrist]
```

Evaluated at the profile's settled reference, this is `controller * G_eff` for a fixed
`G_eff`. So the Dishonored mode can take `Target = controller * G_eff` and reproduce mode
3's settled hand placement exactly at idle. It stays there through every animation. No
recalibration is needed; Dishonored's snapshot calibration (`grip_solve`) can come later
as a button.

### 4.5 Handing the hand back (`anim::blend`)

Default: both hands on the controllers ALWAYS (weight 1). For listed states, D blends to
identity per hand (rotation slerped, translation lerped, smootherstep). The hand then
renders exactly where the game animates it relative to its engine-placed actor, which is
the game's own viewmodel animation.

BS1 states (`Hands.uc`, ENGINE_NOTES s67) that map onto Dishonored's list:

| Dishonored | BS1 |
|---|---|
| cinematics | `wantCine` (core cutscene detector) |
| minigame | hacking (`screens.cpp` knows the screen) |
| scripted scene / choice | `PlayingScriptedHandAnimation`, `scripted.cpp` sequences |
| item actions on the body (Dishonored keeps them on the controller) | `InjectingEve`, `UsingGathererTool`, `ExorcisingGatherer`: candidates, default OFF like Dishonored's item actions |
| sword swing, right hand (`HandAnimMelee=1`) | wrench swing, right hand |
| fire (`HandAnimFire=0`) | `WeaponFiring` / `PostWeaponFiring`: off; the recoil plays in the fingers and the weapon's own skeleton |
| reload / equip (stay on the controller) | `WeaponReloading`, `WeaponEquipping`: stay on the controller; the weapon's skeleton and the fingers animate |

### 4.6 Arms

Already ported (arm IK v2). Once the actor is engine-placed, component space is the
camera's frame, not the controller's. The nominal shoulder is a fixed body-frame point;
the s74d net-yaw shoulder still applies. The two-arm reference bank no longer needs
capturing at a settle: the live source pose provides the arm's current reference every
frame, and its wrist rotation comes with it.

Dishonored's own behaviours that come along unchanged:

- the shoulder slides only out of reach;
- the elbow keeps its body side;
- the roll ramp and the swivel past 80 degrees.

### 4.7 What does NOT port, and why it is not needed

- Draw-time palette copy, shader reflection (`palette_capture.cpp`) and mesh splitting
  (`mesh_split.cpp`'s 5,600 lines). BS1 can write the bones the renderer and attachments
  read. Arm and hand separation is a bone question there, not a mesh one.
- The weapon draw matcher (`weapon_attach.cpp`). Bone 43 carries the weapon.
- `fx_follow.cpp`. Engine-side bones carry attachments.
- Per-Present eye classification. BS1 stereo is per pass and the hook fixes the pose
  before pass 1 draws.

## 5. Port plan

Each phase is one commit, testable on its own, and leaves the previous mode selectable.

| Phase | What | Verification |
|---|---|---|
| **P1** | Post-evaluation hook on `SkeletonInstance` slot `+0x9C` (BS1's `wfix` twin): naked ret-stub, filtered to the hands rig, game thread. Observer only: count evaluations per frame, copy the fresh pose to a source bank. `patterns.h` + ENGINE_NOTES get the RVAs. | Simulator: evaluations per frame with the dirty byte set each tick; the bank matches a direct read. No visible change |
| **P2** | `hand_compose.h` (pure): target in component space, `D = T * inv(A[wrist])`, the palm pivot, the blend to identity (Dishonored's `blend_transform` + smootherstep), and `G_eff` from a mode-3 profile. Host suite. | Host tests, including commutation (fingers preserved), exact wrist, pivot invariance, blend endpoints, and a negative control |
| **P3** | Offline proof on REAL BS1 animation: replay the exported clips (`bsmesh-export`: 130 on NEWPlayerHands) through the compose with a fixed controller. Check the wrist stays fixed, the fingers keep their motion, and bone 43 stays rigid on the wrist; render in Blender | `tools\hand-compose-sweep` + Blender bake: pistol reload, shotgun pump, wrench swing, an Electro cast |
| **P4** | `vrhands mode dishonored` (mode 4): actor engine-placed (mode 2's path), dirty set each tick, no freeze/settle/adopt/pin, compose in the hook and on the game thread from the source bank; arm IK v2 on the composed wrist; F10 radio + `hands.ini` | Simulator first: per-eye capture, the hand on the simulated controller through a reload, both eyes identical. Then the headset |
| **P5** | Hand-back policy: per-hand `Handoff` weight on the states in 4.5, F10 list like Dishonored's Animations tab | Headset: hack screen, Eve injection, a scripted sequence |
| **P6** | Off hand and plasmids through the same compose (left cluster 6-21); grip snapshot button (`grip_solve`) | Headset |
| **P7** | Retire mode 3's machinery once mode 4 is accepted: settle/adopt/pin/rest-restore, ACTORWATCH/ROLLCHECK, the twist-limit sliders | Healing session |

### Done this session: P2 and P3

**P2** - `src/game/bioshock1r/hand_compose.h`, pure, ported from Dishonored's
`delta_from_target` / `compose_3x4` / `blend_transform` / `smootherstep` / `Handoff`.
`.\tools\host-test.ps1 hand-compose`: **3,279 checks, 0 failures**. The suite covers:

- the wrist takes the target exactly and the palm lands on the target point;
- fingers keep their pose relative to the wrist;
- the palm pivot (turning the target moves the palm nowhere);
- blend endpoints and the straight palm path;
- Dishonored's Handoff timings;
- a negative control.

**P3** - `.\tools\hand-compose-sweep.ps1` replays REAL BS1 clips, exported from the game's
own Havok animation by `bs2gltf.py --clip-poses`, with the controller held still:

| Clip | Frames | How far the GAME moves the wrist | Composed wrist off the controller |
|---|---:|---|---|
| `Pistol__FastReloadPistol` | 55 | left 118 UU / 180 deg, right 30 UU / 70 deg | 0.00002 UU / 0 deg |
| `Wrench__Swing_A_Wrench` | 21 | left 38 UU / 128 deg, right 126 UU / 169 deg | same |
| `Shotgun__ReloadShotgun_LOOP` | 15 | left 45 UU / 140 deg, right 1 UU / 3 deg | same |
| `Default__ElectrokineticBolt_Fire` | 9 | left 23 UU / 75 deg | same |

Across all four clips:

- fingers, the weapon attach (43) and its tip (44) keep their pose in the wrist to
  0.00004 UU;
- the arms solve on every frame (0 failures, join 0.00002 UU);
- skinned through the original weights in Blender, the forearm keeps at least 93.8% of its
  radius composed, against 86.1% in the game's own pose;
- renders: `verification\hands-game-*` and `verification\hands-dishonored-*`.

The left-hand numbers are the point. BS1's reloads swing the OFF hand across the body by
over a metre's worth of arc. Mode 3 shows that and Dishonored's model removes it; the
fingers still re-grip.

### P1 done (session 82b) and P4 built (session 83)

**P1**, the evaluator hook as an observer (`vrbones evalprobe on` in `vrhands mode gun`,
one simulator run). With the dirty byte set each tick:

- the engine evaluates **once per game tick, always in the tick, never inside a render
  pass**;
- the hooked copy matched the live array in **all ~19,000 checks**.

Details in ENGINE_NOTES s82b.

**P4**, `vrhands mode dishonored` (mode 4, F10 Drive: DISHONORED), built and installed but
**not yet run**:

- The hands actor is never written: the engine keeps it on the camera.
- `m4_frame` (every CalcView) publishes both wrists' WORLD targets and the shoulders, then
  sets the dirty byte.
- `m4_compose` writes each hand cluster as `B = D * A`, with
  `D = Target * inverse(A[wrist])` (`hand_compose.h`). It runs from the evaluator hook
  right after the engine rebuilds the pose, and again at the scene build
  (`hands::late_write`) with the actor read at that moment.
- The arms are arm IK v2 to the composed wrists, with the reference taken from the same
  live pose.
- **Held hand target:** mode 3's actor (the same `loc` + aim rotation), times the wrist
  captured 300 ms into `Idling`, once per holdable. Every per-weapon profile carries over,
  and the hand then stays at its idle placement through reloads and shots. Before the
  first capture it follows mode 3's live placement.
- **Free hand target:** mode 3's free-hand target, unchanged (wrist-normalised since s71w):
  the heading-local composition, the view cm and the pos cm in the trimmed frame.
- Stands down in cutscenes and scripted scenes (`m4_release`, so the engine owns the hands
  there), and `reapply()` is gated off while mode 4 drives.
- Not done yet: the per-state hand-back (P5), `arms hide` (with hide selected, the engine's
  own arms show), and Dishonored's grip snapshot button.

### First headset run of mode 4, and the arm fixes it asked for (session 83b)

**Tester, 2026-10-08:** "I think it might have been better." The `MODE4` log showed it
healthy: ~25k composes, 0 stale.

- **Hands:** they stayed on the controllers with the right animations playing in both,
  and the plasmid hand was right.
- **Shoulders:** they still moved a little, but naturally. That is the reach slide.
- **Asked for:**
  - an arm-size setting, because the arms looked small (they were drawn at the 0.8 hand
    scale);
  - shoulders connected, so they move together and stay aligned;
  - a shoulder-width setting.

**Built (F10 Hands + weapon > ARMS > "DISHONORED mode arms", saved to hands.ini as
`m4*`):**

| Setting | What it does |
|---|---|
| `shoulder width` (`m4ShoulderWidthCm`, default 52) | The shoulders are now ONE bar, Dishonored's shared centre plus total width, the same whichever hand is held. The old per-hand shoulder triples are no longer read in mode 4. |
| `shoulders forward / up / right` (`m4ShoulderFwdCm` 3.8, `m4ShoulderUpCm` -19.8, `m4ShoulderRightCm` 0) | Place the bar's centre. The defaults reproduce the run's placement exactly. |
| `shoulders move together` (`m4ShouldersLinked`, default on) | Both arms solve; when reach slides one shoulder, the bar moves by that slide and both arms solve again. The shoulders stay level and aligned. Off restores Dishonored's independent slide. |
| `hands + arms size` (`m4Size`, default 1.00) | ONE size for the hands and arms together, a multiplier on the hand scale that both are drawn at, so they always match. The tester asked for exactly that ("the arms and hands should scale together"). 1.00 is as in the run. The weapon keeps its own size (`GunScale`). `arm length scale` still stretches only the arm's length on top. |

The `MODE4` line now also logs the width, the linked state, the last bar slide and the
size. Built and installed, not yet run; the bar slide has no host test yet.

The one question P4's first launch must answer: does the hand stay on the controller
through a full pistol reload, with the fingers animating, and do the shoulders stay put
while the wrist rolls?

### The shoulder-control audit (session 85)

**Tester, after the second mode-4 run (this time really in mode 4, 19:44-19:47):** "a lot
of the shoulder settings are confusing and don't work right", "several options do not
move in the right direction", and both arms should scale together without an option,
with a toggle for the weapon to scale with them, on by default.

**What the audit found:**

- **The sliders' axes were right, and the slide overrode them.** Forward, right and up are
  the body frame the hands are placed in (the net yaw, s74d). The `MODE4` lines of that
  run show `slid` up to 11.5 UU. The solver moves a shoulder whenever its hand is past the
  arm's reach, or closer than 40% of it, and s83b's linked bar then moved BOTH shoulders
  by the larger slide. So with an arm out:
  - widening the shoulders was pulled straight back toward the hand;
  - raising them swung them on a sphere about the wrist;
  - moving the left hand moved the right shoulder.
- **Ten controls on screen did nothing in mode 4:**
  - the per-hand `shoulder fwd / right / up` (s70i). They sat right under mode 4's own,
    and in mode 3 the free arm mirrors them, so their `right` works as a width;
  - `ARM IK v2` (mode 4 always solves with v2);
  - the two twist limits, `elbow follows wrist` and `elbow smoothing` (v1 only);
  - `model scale` and `scale both hands`. Mode 4 multiplied the PER-HAND scale by
    `m4Size`, so these were a second size control, and the two sides could differ.
- **Nothing sized the weapon with the hands.** It kept `GunScale` alone.

Staging's Rapture F10 menu (`F10_MENU.md`), merged into this branch after the audit,
already hides the ten in mode 4 (they are `LegacyHands` / `LegacySolver` there). What it
still carried from s83b was the `Shoulders move together` toggle.

**Changed** (the F10 menu entries are in `menu_model.cpp`, keyed for `menu-settings.ini`):

| Setting | Now |
|---|---|
| IK > Shoulder position: `Forward / backward`, `Right / left`, `Up / down`, `Shoulder width` (`ShoulderBar*`, `ShoulderWidth`; legacy `m4Shoulder*`) | Each moves both shoulders the way its name says, and they **stay there**: a hand past reach stretches its arm (the arm-length multiplier, raised just enough, up to x1.35) before a shoulder may move. Past that, only that arm's shoulder slides. The linked bar is gone, and so are `ShouldersLinked` and `m4ShouldersLinked`. A saved `ShouldersLinked` line is ignored as an unknown key. |
| Hands > Hand and weapon size: `Hands and arms size` (`HandsArmsSize`; legacy `m4Size`) | One value for both hands and both arms: the RIGHT hand's scale times this. Per-hand scales are a mode 2/3 lane, and `Hand size` stays hidden in mode 4. |
| Hands > Hand and weapon size: `Weapon scales with the hands` (`WeaponFollowsHands`; legacy `m4WeaponFollowsSize`), default on | New. The weapon is drawn at `Weapon size` x `Hands and arms size`. `Weapon size` stays its calibration against the hand. Mode 4 only. |
| Drive mode | A change writes the `mode=` line of `hands.ini` and nothing else, so DISHONORED survives a restart without copying menu overrides into the legacy file. |

The `MODE4` line logs the shoulder centre and width, the largest stretch and the largest
leftover slide since the last line, and the sizes. Built only. No headset or simulator
run yet; the five `bvr_f10_preview` cases pass (327 checks each).

**s85b, first run of the above:** "they move together, but the shoulders start
misaligned: the right one is further forward and maybe a little down, so they keep their
misalignment when moving together." Two causes, both fixed in mode 4 only:

1. **The bar faced the room, not the player.** solve_arm's net yaw (`camYaw - drive -
   recenterYaw`) is the hands' room mapping. It faces the room's XR yaw 0, because the
   body transfer and snap turns advance `recenterYaw` away from the recenter value. That
   run recentered at 18-23 deg, which turns a 42 cm bar about 20 deg: one shoulder
   about 7 cm ahead of the other.
   - Mode 4 now adds `FrameContext::recenterHeadYawRad`, the head's yaw at the last real
     recenter, read from `g_recenterPose` and never advanced.
   - A head turn still moves nothing (s74d). Stick and snap turns still carry the bar.
   - Mode 3's `solve_arm` has the same term and is unchanged. Its shoulders are rotated
     by the recenter yaw too: a candidate fix, not made here.
2. **The clavicle came from the animation.** `arm_ik::pose` hangs the clavicle off the
   upper arm by the reference offset and swings it with the arm. That is Dishonored's form,
   and it is right for a symmetric bind pose. BS1's reference is the LIVE pose, though, and
   the gun idle and the off-hand idle hold the two clavicles differently.
   - New `Input::bodyClavicle` (off by default, so mode 3 v2 is unchanged) places the
     clavicle on the body instead: at its own length inward from the shoulder along
     `-side * right`, carried there with no roll about body up.
   - Mode 4 turns it on. Host-tested (`arm-ik-tests`, 1448 checks).

The `MODE4` line now also prints the head's turn from the bar, the recenter yaw, and each
side's solved clavicle and upper-arm head in cm from the bar centre. Mirrored shoulders
read equal forward/up and opposite right.

(The body-fixed clavicle was removed again in s86: `NEWPlayerHands` weights no vertex
to either clavicle bone - measured from the glb, 0 of 2,771 - so it could never have
shown. The shoulder cap is the upper-arm bone.)

### s86: one drive, Dishonored's placement for both hands, and the fit measured

**Tester, after s85b:** "There shouldn't be a mode 3. There should just be one mode that
works correctly. Still off in scale, placement and arm length. Both arms aren't treated
identically: my right arm did not have as much reach as my left. Both arms should always
scale together and have the same reach and shoulder position. Use Blender to get the
exact same values and fit as Dishonored - it was perfectly aligned with my body,
including the forearm, bicep and total arm length."

#### What Dishonored does (its `arm_ik_draw.inc`, `arm_ik.h`, `mesh_split.cpp`, and the live ini)

| Thing | Dishonored | BS1 before s86 |
|---|---|---|
| Hand placement | `palm_target(O_C, G, trim)`: the wrist bone sits at the controller's grip pose, turned by a per-hand grip calibration and a per-hand palm-frame trim. One pipeline, both hands. | Held hand: the BRVR actor (per-weapon `posFwdCm` 58, `posRightCm` 18, `posUpCm` -17: the actor-origin-to-hand vector) times an idle wrist capture. Free hand: on its controller (s71w). **Two pipelines, two wrist-to-controller distances, so one arm ran out before the other.** |
| Shoulders | `body.origin` is the draw camera corrected to the eye centre; shoulders at forward -16, up -25, width 38.1 cm from it, mirrored; body yaw follows the head with a 25 deg deadzone | From `ctx.base`, the camera before the headset's translation and before the 9 cm head offset: leaning left the shoulders behind |
| Arm length | `ArmLengthScale` 1.27 on a rig whose upper arm is 0.90x its forearm | `arm length` on a rig whose upper arm is 1.44x its forearm |
| Hand size | `ModelScale` 0.85 on the hand palette; the arm segments scale with the wrist's skin scale | 0.904 per hand x `m4Size` 1.07-1.09 |
| Elbow | pole down + 0.6 out - 0.3 back | the same formula, `elbow out` 0.35 |

#### Measured: both rigs, in each game's cm (1 UU = 1 cm at world scale 100)

BS1 from `bs2gltf --rig-out NEWPlayerHands` (the Havok reference pose); Dishonored from
its shipped `dishonored_vr_arm_rig.bin` (`DVRIK002`, bone heads). Dishonored's drawn
size is rig x 0.85 (`ModelScale`) and its arm rig x 0.85 x 1.27.

| Length | BS1 rig | Dishonored rig | Dishonored as drawn (the fit) |
|---|---|---|---|
| wrist to the last knuckle of the middle finger | 20.21 | 19.65 | **16.7** |
| wrist to the middle fingertip | - | 21.81 | 18.5 |
| upper arm (upper-arm head to forearm head) | 43.30 | 23.47 | **25.3** |
| forearm (forearm head to wrist) | 29.98 | 25.95 | **28.0** |
| reach | 73.3 | 49.4 | 53.4 |
| upper / forearm | 1.44 | 0.90 | 0.90 |

So, on BS1's rig, **size 0.83** gives Dishonored's hand, and at that size the upper arm
needs **x0.71** and the forearm **x1.13**. One `arm length` slider could fit the reach or
the bicep or the forearm, never all three.

#### Changed

| | |
|---|---|
| **One drive.** | `g_mode` defaults to 4 and the `mode=` line of `hands.ini` is no longer read. The F10 "Hand implementation" choice has one entry. Modes 0-3 are reachable by `vrhands mode <n>` only, for a comparison, and their menu controls (`LegacyHands`) never show. The code of the retired drives stays for the healing session. |
| **Both hands on the grip pose.** | `hands.cpp` builds each hand's wrist pose the same way: the controller's grip pose turned by that hand's rotation trim (`offHandRot*`), and `m4_frame` adds that hand's view-frame placement (`offHandView*`) and palm-frame grip offset (`offHand*Cm`). The right hand keeps the numbers it already had as a free hand (view -6 / 4 / 8, rotation -32 / -4 / -8). The idle capture, mode 3's actor and the per-weapon profile are out of the hand path; the weapon rides the hand's bones. |
| **Per-segment arm length.** | `arm_ik::Input::upperLength` / `foreLength` (defaults 0.71 / 1.13 in the drive; 1 / 1 in the header, so mode 3's v2 is unchanged). Host-tested. |
| **One size.** | `size` (0.83) is absolute on the rig; the per-hand `handScaleL/R` pair no longer enters. Weapon follows = `GunScale` x size / 0.8. |
| **Shoulders from the eye.** | Centre at `ctx.cam` + (-16 fwd, 0 right, -25 up) cm, width 38.1 - Dishonored's numbers, as defaults. New keys (`ShouldersForward/Right/Up/Width`) so the old base-relative preferences do not carry over. |
| **Hands only.** | `Show arms` (Hands > Arms). Off collapses the ten sleeve bones in the composition. |
| **Menu.** | Hands: `Hands and arms size`, `Weapon scales with the hands`, per-hand `Hand position` / `Hand rotation` / `Grip pivot` (Left/Right/Both), `Show arms`. IK: the four shoulder sliders, `Elbows outward`, `Upper arm length`, `Forearm length`. |
| **Log.** | The `HANDS` line (was `MODE4`): size and the actor's DrawScale, the segment lengths, each arm's reach used (%), the largest stretch and slide, and both solved upper-arm heads in cm from the bar centre. |

#### Proof, offline (`.\tools\arm-ik-audit.ps1 -UpperLength 0.71 -ForearmLength 1.13`)

The s81 audit runs BS1's solver on BS1's rig and Dishonored's on its rig through the same
150 body-relative poses and said the elbow direction matches (0.03 deg) but "the elbow
BEND cannot match for the same reach: geometry, not solver". With the segments at
0.71 / 1.13:

| | 1.0 / 1.0 (s81) | 0.71 / 1.13 (s86) |
|---|---|---|
| elbow position gap, max (arm lengths) | 0.294 (~15 cm on a 53 cm arm) | **0.002** (~1 mm) |
| elbow position gap, mean | 0.171 | 0.001 |
| elbow direction, max | 0.03 deg | 0.04 deg |
| failures | 0 | 0 |

The 15 key poses are rendered side by side in Blender (`arm_ik_audit.py`, BS1 left,
Dishonored right) under `<model_workspace>\verification\arm-ik-audit-*.png`.

**Not verified:** anything in the headset. The fit numbers put BS1's hand and arm at
Dishonored's drawn lengths; whether the viewmodel path renders them at that size is the
first thing the next run answers (the `HANDS` line prints the actor's DrawScale, which
multiplies everything).

### s86b: the run, and three causes in it

**Tester:** hands only gives vertex explosions at the wrists; turning the head left and right
moves the hands (maybe just the right), which changes the arm's length; the Dishonored values
do not feel right; and a request to research Dishonored's whole pipeline for an overhaul.
The research is `DISHONORED_PIPELINE.md`. From the log (`actor DrawScale 0.80`, the saved
preferences) and the code:

1. **The hand drew at 0.66.** The hands actor renders at DrawScale 0.80, which multiplies
   everything written in component space; `size` 0.83 was a component scale, so the drawn hand
   was 0.83 x 0.80. `size` is now the DRAWN size (divided by the DrawScale read each frame),
   under a new key `HandAndArmSize` so the saved 1.09 (a component scale that drew at 0.87) does
   not carry over.
2. **The placement rotated with the head.** The per-hand placement (`offHandView*`: right
   -6 / 4 / 8 cm) was added along the CAMERA's axes every frame, so a head turn swung each hand
   on an arc as wide as its offset. Dishonored's trim is stored in the palm frame and only
   edited in the view frame. The placement now lives in the controller's own grip frame
   ("forward" is where the controller points), which turns with the hand and never with the
   head. The eye itself orbits the neck ~9 cm in a head turn and the shoulders hung off the
   eye, so the arm changed length by that much; the shoulder origin is now the head's pivot
   (eye - 9 cm along the head's forward, put back along the body's forward: identical to
   Dishonored's eye origin at a level head facing the body, and still under a head turn).
3. **Hands only sent the arm bones 50 m away.** The cuff vertices are blended between
   ForeTwist1 and the hand, and a blend between the hand and a point 50 m off is a spike.
   Now the arm bones are pinned AT the wrist with zero scale (BRVR's HideBone rule): a pinch
   at the cuff, no spike.

Built, installed, not run.

### s86c: the palm anchor, and the size in two parts

**Tester, after s86b:** "The arms were waaay too big and there is a huge pivot on the hands
when turning again. It took like 10 hours to solve the stupid hand pivot. ADAPT DISHONORED'S
SYSTEM 1:1." Both were places where s83-s86b was NOT 1:1:

1. **The pivot.** Dishonored's `delta_from_target` pins its PALM ANCHOR (`MpAnchorPos`: a
   12-vertex patch in the middle of the hand, beyond the wrist cut) to the controller plus
   the trim, and turns the hand about it. `hand_compose::delta(target, wrist, palmLocal)` is
   that function, and the s82 P3 sweep proved it on the real clips with R_grip (43) and
   L_Middle1 (13) as the palm. s83's integration passed a ZERO palm - the wrist bone itself -
   so every wrist turn swung the hand on the wrist-to-grip lever (~10 cm). Now
   `m4_compose` uses `kBoneRPalm` / `kBoneLPalm` (patterns.h) as the anchor, and scales the
   hand about it (Dishonored's `scale_about`), so sizing never slides the grip off the
   controller either. Re-run offline: `hand-compose-sweep.ps1` - through the pistol reload
   (the game swings the wrist 118 UU / 180 deg) the composed palm stays on the controller
   to 0.00002 UU; 44 Blender renders under `verification\hands-*.png`.
2. **The size.** The actor's DrawScale 0.80 multiplies the bone POSITIONS the engine renders
   (s72q) and not the skin (s16; and the three runs: s85 drew thickness 0.97, s86 0.83, s86b
   1.04, each equal to the component scale alone). s86b wrote positions AND the .s channel at
   size / k: lengths right, thickness 25% too big. Now positions are written at size / k and
   the .s channel at size, for the hand bones and the arm bones alike.

Built, installed, not run. **Still unmeasured:** that DrawScale rule is inferred from three
headset impressions and two old measurements; the simulator can settle it (one launch:
`hand r grip pose` at a fixed point, roll 0 / 90 / 180, per-eye captures - the grip must stay
put and the hand's span must read 0.83 of the rig's).

### s86d: the size rule was wrong, and the "too big" was a stale ini line

**Tester, after s86c:** "Now the hands are small and stretched like alien hands."

- **DrawScale scales the skin too.** s86c's split (positions at size / k, skin at size)
  spaced the fingers for 0.85 and drew each finger's mesh at 0.68 - exactly "small and
  stretched". Under the other rule the hand would have been a plain 0.85 hand. So the rule
  is the ordinary Unreal one: everything in component space renders times DrawScale, and
  the s86b uniform `size / k` was right. Reverted to it; `size` is the drawn size.
- **Then why were s86b's arms "waaay too big"?** The log: that run started at
  `size 1.070`, not 0.83 - `hands.ini` still carried `m4Size=1.070` from s83b, loaded
  before the menu's defaults, winning whenever the menu had no saved value. The tester
  dragged it to 0.85 mid-run. hands.ini's `m4*` keys are no longer read or written; the
  F10 menu owns these.
- **Girth, measured on both weighted meshes** (median radius at mid-segment, rig units,
  then drawn: BS1 x 0.83, Dishonored x 0.85): forearm 5.9 -> 4.9 cm vs 6.1 -> 5.2;
  upper arm 7.8 -> 6.4 vs 8.6 -> 7.3; palm 4.7 -> 3.9 vs 4.9 -> 4.2. At the matched hand
  size BS1's arms are 6-12% SLIMMER than Dishonored's, so there is no thickness to take
  off; no thickness lever was added.
- **No stretch (1:1).** s85 lengthened an arm up to 1.35x before its shoulder could move;
  Dishonored never lengthens an arm - the shoulder slides. Removed; the `HANDS` line keeps
  reporting the slide.

Built, installed, not run.

### s86e: the ini audit (asked for while the s86d build was tested)

Every file that reaches the one drive, in load order: `BioshockVR.ini` [VR] (HandsScale,
GunScale, CameraHeightOffset) -> `hands.ini` -> `weapons.ini` -> `vrpreset.ini` (overrides
the first) -> `menu-settings.ini` (overrides everything). Checked against the log's startup
echo of the s86d run.

| Finding | Effect | Done |
|---|---|---|
| `hands.ini elbowOut=0.350` loaded over the code default; the menu owns `ElbowOut` and had no saved value | the elbow pole sat at BS1's untested 0.35 while Dishonored's fit is 0.6 (ARM_IK.md flagged this as unverified in s81) | default 0.6; the key is no longer read (same rule as `m4*`) |
| The shoulder bar faced the room: net yaw + the recenter-time head yaw, a constant | a player who turns in the room keeps shoulders facing the recenter direction | Dishonored's `BodyYaw` (25 deg deadzone, 1.5 s relaxation) ported into `arm_ik.h`, host-tested; the bar follows the body |
| `hands.ini offHandView*` (R -6/4/8, L 4/-2/4) were tuned in the VIEW frame; since s86b they apply in the controller's frame | the same numbers mean something slightly different now; at a level, forward-pointing controller the frames coincide | nothing: retune in F10 > Hands > Hand position if they read off |
| `menu-settings.ini`: `ShouldersWidth 28`, `ShouldersForward -11.8`, `ShouldersRight 2.4` | set during the s86b/c runs while the size and pivot were wrong; Dishonored's are 38.1 / -16 / 0 | nothing: the tester's choice; the log line shows them |
| `menu-settings.ini`: `WeaponFollowsHands 0`, `ArmLength 0.94`, `PlaceForward 1 Wrench -4.8` | the weapon stays at `wScale` 0.868 whatever the size; the other two are legacy-only and inert | nothing |
| `weapons.ini`: ElectricBolt and Telekinesis carry `animOn=0` (the log calls it stale) | mode 2/3 only (`g_animAllowed`); the one drive never reads it | nothing |
| `vrpreset.ini` `gameFovDeg=100` vs the game's `HorizontalFOV=100` | agree | - |
| `[reentry] recovery FAILED - stereo auto-off` 11 s into the run | a false positive, re-armed 20 s later (`2nd=38/s` after); the first ~20 s of each run are mono | nothing; known |
| `FOVPROBE ... ENGINE IS RESTAMPING` on the first frames | the known self-correcting first-frame write (ENGINE_NOTES); holds at 117.46 after | nothing |
| `hands.ini` still gets rewritten by `save_vr_preset` and two console verbs | harmless now that the menu-owned keys are not read back | nothing |

Built, installed, not run.

### s86f: the pivot, explained, and Dishonored's step rows

**Tester, after s86e:** size right, shoulders alright, "still pivoting instead of just being
attached to my hand. Explain why BRVR didn't have this problem, Dishonored didn't, but this
one can't get rid of it." And: the hand-position controls move along the wrong axis; "I
told you to implement Dishonored's fine/normal/coarse buttons".

**Why the three differ.** All three drive the hand by one rigid transform; what differs is
WHICH POINT of the hand is pinned to the controller, because the hand turns about that
point:

| | pinned point | pivot |
|---|---|---|
| BRVR / mode 3 | the weapon's grip, through the per-weapon origin-to-hand vector (58 / 18 / -17 cm) tuned by eye until it was the point in the fist | none: the fist turns about itself |
| Dishonored | `MpAnchorPos`: the centroid of the hand mesh (a 12-vertex patch beyond the wrist cut) | none: the palm turns about itself |
| s83 mode 4 | mode 3's placement (the held hand) | none - the s83 run reported the hands on the controllers |
| s86-s86b | the WRIST bone (a zero palm passed to `hand_compose::delta`) | ~10 cm: the palm swings about the wrist |
| s86c-s86e | `R_grip` (bone 43), assumed to be in the fist; it is 4 rig units from the wrist, at the heel of the hand | ~6 cm, still |
| **s86f** | the palm's centre: the mean of the hand bone and the four finger bases (`kBone*PalmBones`), the bone-level twin of Dishonored's patch | none expected |

So the pivot was never the mechanism - `delta` pivots on whatever palm it is given - it was
the anchor I handed it, twice wrong.

**The step rows (`frame_context.h` hand_position_step / hand_rotation_step, pure).** F10
Hands > "Move the hand (as you see it)": Fine / Normal / Coarse (0.2 / 0.5 / 2 cm, 0.5 / 2 /
5 deg), then Move left/right, down/up, back/forward and Pitch, Yaw, Roll rows, held to
repeat, for the selected hand (Both = both). A press is queued to the game thread and
converted THERE, in the head's yaw frame of that instant (up = world up), into the stored
trim: the palm-frame translation (`off_hand_cm`, along the trimmed hand's forward / right /
up) and the controller-local rotation trim (pitch / yaw / roll, `xr_local_trim_quat`'s
convention, inverted by the new `xr_local_trim_angles`). Stored that way the trim rides
with the hand. The six stored values are saved as menu preferences and shown under
"Stored values (palm frame)" (Advanced), as Dishonored's Debug node does. The s86b
controller-frame placement sliders are gone and `offHandView*` is no longer applied.
Host-tested (ue-math): trim angles round trip to 1e-5; the XR-angle inverse to 2e-3 rad; a
zero step leaves the hand; yaw right 10 turns it 10 right; pitch up 10 lifts a view-aligned
hand 10; roll right 10 rolls it 10; a view-forward step on a hand turned 90 deg right
lands as hand-left.

Built, installed, not run.

### s87: the pivot measured and gone; the hands and the held weapon on the skin palette

Simulator only (2026-10-08/09), the Aug 03 11:01:57 PM save (Medical Pavilion, wrench up),
loaded through the menu by `tools\sim-load-save.ps1`, never Continue. The loop of
PIVOT_SIM_PROTOCOL.md, one change per row:

| # | change | worst palm-vs-grip | render | verdict |
|---|---|---|---|---|
| 1 | none (s86g build), the hands-less save Continue picked | 9.49 cm R, 8.49 cm L, CONSTANT at every orientation, the error vector turning with the hand | no hands in view | the oracle's "error grows with orientation" row in disguise: a fixed lever in the palm frame |
| 2 | same build, the 11:01 save | 9.49 cm, constant | the fist swings on that lever | the lever is exactly the stored grip trim: R 5.4 / 3.0 / 7.2 cm (= 9.49), L -6 / 6 / 0 (= 8.49) |
| 3 | `vrhands grip 0 0 0` (new verb, live) | **0.00 cm at all 11** | the fist holds one screen point at all 11 | PASS at bone level; the laser is not usable as the render reference (its dots start far down the ray and do not follow the sim's aim pose) |
| 4 | the fix in code: left default 0, `offHand*Cm` no longer read from hands.ini, the menu keys renamed `HandPalmTrim*` | 0.00 cm after a fresh boot | as row 3 | the trims load as zero on both hands |
| 5 | **the palette route** (`palette.cpp`, below), hands only, bones untouched | 0.00 cm (by construction) | the fist at the same screen point as row 3 at every orientation, the sleeves gone; the wrench left at the game's hand | the palette space is the pose's component space (space probe: inverse bind constant, drift 0.000 UU / 0.00 deg, mirror-symmetric L/R) |
| 6 | the held weapon Dishonored's way (1.5) | 0.00 cm, R and L sweeps | the wrench in the fist at every orientation | PASS |

**Why it pivoted.** s86f moved the anchor from the wrist to the palm's centre, but the stored
grip trims were wrist-era numbers: "where the wrist sits relative to your grip". Applied in
the palm frame on top of the new anchor they became a 9.5 cm lever from the controller to the
palm, which turns with every wrist turn: "synced forward, desync pointed elsewhere". No other
term was involved; zeroing them put the error at 0.00 at every orientation.

**The palette route (the tester's choice, kept regardless of the pivot).** BS1 skins on the
CPU (ENGINE_NOTES s87): a per-bone 4x4 array on the mesh instance, gathered per LOD by
`+0x3ED660` for the skinning job. `palette.cpp` hooks the gather and, for the instance whose
owner (`+0x48`) is the hands actor, hands it a copy with each hand's correction composed on -
`S * M` for every matrix of that hand's cluster, S being exactly mode 4's correction (palm on
the target, size about the palm), i.e. Dishonored's `D * M` with `scale_about`. The engine's
array, skeleton and bones are never written. Hands only collapses each sleeve bone's matrix to
the composed wrist point (no hierarchy: the palette's freedom). `vrhands palette on|off|status|
probe on|off`; default ON; off (or a refused hook) is the bone drive, unchanged.

**The weapon (Dishonored 1.5, `weapon_attach.cpp`).** The wrench is not CPU-skinned: it is a
`UStaticMesh` drawn with one transform per draw (the gather's census lists PlayerHands, bodies,
doors - no holdable). Its draw goes through `UStaticMesh` slot 5 (`+0x3DBCF0`), which hands the
renderer a per-draw record's LocalToWorld (+160) and WorldToLocal (+224), world space, owner
actor at +0. The hook composes the held hand's correction carried to world, `D_world = L_hand *
S * inverse(L_hand)` (Dishonored's `WaPublishCommon`), onto LocalToWorld (and its inverse onto
WorldToLocal) for that one draw. A skinned holdable, should one exist, takes the same D in its
own space through the gather (`inverse(L_weapon) * D_world * L_weapon`).

**Known gaps, by design of this step.** The IK arms are not on the palette yet ("hands first";
armsMode "solved" hides the sleeves while the palette is on). Anything else the engine attaches
to the hand bones (plasmid FX) still follows the GAME's hand, as Dishonored's did before its
`fx_follow`. The laser criterion of the protocol could not be applied (see row 3).



### s88: the animation lock, the hand-back, and the IK arm - all on the palette

Simulator only (2026-10-09), the 11:01 save. The tester's brief: the arms' animation is
suppressed and the hands' is not, the way Dishonored locks the hand; the RT swing and the
takedown-style animations hand the hand back to the game and return it; then the IK back, as
Dishonored does it. Everything is on the skin palette; the skeleton stays the game's.

**The hand animates, the arm's animation does not reach it** (Dishonored VR-183 and VR-184):
- *Rigid anchor.* The palm the hand is placed by is the bind pose's palm centre, measured once
  through the palette and carried by the hand bone alone (`palette_math::rigid_palm`). Measured:
  L (9.21, 0.12, +0.31) / R (9.21, 0.12, -0.31) in each wrist's frame - mirror-symmetric. The live
  palm centre (five palm bones) wanders up to 4.6 cm on the right while the wrench grip moves the
  fingers: that is what no longer moves the hand. `vrhands palette anchor on|off`.
- *Rigid wrist.* In the mod's copy of the hands' skinned vertices (swapped into the LOD for the
  gather call, like the palette), every vertex with at least half its weight on one hand has its
  forearm influences moved to that hand's bone: 142 of 3,469 vertices. The arm's animation and
  twist can no longer bend the cuff; the fingers still animate. A/B at a 100 deg roll: 0.25% of
  the image changes, all at the cuffs. `vrhands palette wrist on|off`.

**The hand-back** (`handback.cpp`, Dishonored anim_state / VR-220): the classifier is the Hands
script state (`hands_state`, which mode 4 had never located - fixed). A TRIGGER melee attack
(Firing entered without the physical swing's RT pulse) owns the weapon hand; a physical swing
keeps it tracked; `Scripted` states (EVE hypo, scripted hand animations, gatherer tools - BS1's
takedowns) own both; fire and reload are levers, off. 250 ms in, 250 ms release, 350 ms out,
smootherstep; the palm on the straight line; the arm lerped to the game's by `1 - weight`; the
weapon follows the blended hand. `vrhands handback status|melee|swing|scripted|fire|reload
on|off|force l|r|both|off|blendms|release`.

Measured (`tools\handback-test.ps1`, the TRACE lines):

| | |
|---|---|
| trigger attack | TRIGGER -> right hand only; the clip carries the game's palm up to 99.7 cm; drawn palm 0.00 cm from it at weight 0; back on the controller 1.28 s after the press |
| straight line | every sample: distance to the controller + distance to the game's palm = the separation, split by the weight (e.g. 0.569 -> 10.03 of 17.62 cm) |
| physical swing | SWING -> no hand-back, no trace |
| combo (2nd press at 400 ms) | one continuous hand-back through the 1.38 s combo |
| 2nd attack in the release | the hand never leaves the game; one return at the end |
| reversal mid-return | host-tested: no jump, takes only the remaining share (250 ms x weight) |
| largest weight step between 30 Hz samples | 0.342 - smootherstep's peak slope x the spacing; no jumps |
| forced both hands | both reach the game's clip; the frame is the game's own viewmodel |
| plasmid equip and cast | Ability states, no hand-back; Electro Bolt's idle sparks ride the tracked hands; the bolt itself leaves from the game's hand (the fx_follow gap) |

**The IK arm on the palette** (Dishonored `arm_ik_draw.inc`): the s81 solver to the composed (and
blended) wrist; each sleeve matrix becomes `T = M(solved) * inverse(M(game))`, set on its own
with no hierarchy, so the solver's per-axis scale passes straight through. `tools\arm-sweep.ps1`:
neutral 90/87% reach, full forward reach 145/142% (the shoulder slides, Dishonored's form),
chest 53/49%, side 93/86%, raised 105/101%, lowered 92/87%, across 93/93%; the captures (and
re-takes with the head turned to the out-of-view poses) show the forearms meeting the hands, the
elbows bending down and out, the crossed arms clear of each other. Head turned 60 deg with the
hands still: the arms stay put. `vrhands palette arms ik|hide|game`.

**Regression and stability.** Both pivot sweeps 0.00 cm on this build; bone-drive fallback
(`vrhands palette off`) unchanged; 150 s soak (`tools\hands-soak.ps1`: random poses and head
turns, 31 trigger presses, 119 attacks classified incl. the sim's teleport swings) - 28,418 hand
composes, 0 refusals, 0 faults. Host: `palette-math` 10,210 checks (the hand correction on a
palette entry, the blend and its palm line with a negative control, the arm affine with
non-uniform scale and its weight-0 identity, the weapon's world correction, the rigid palm, the
inverse, the hand-back timeline with a reversal).

**Audit fixes in the same change.** The rigid-wrist copy is an eight-entry cache, never freed
while a worker's skinning task may still read it; every LOD and vertex offset moved to
`patterns.h` with its derivation; `palette_math.h` holds the math the tests run.

**Open, by design of this step.** Plasmid FX and the bolt still leave from the game's hand
(Dishonored `fx_follow`); the sim cannot raise a real Scripted state (the force lever exercises
the same path); "the game's arms" mode stretches the game's forearm to the tracked hand (it is
not the default); a combo verdict is latched per attack entry, not per combo clip.



### s89: the pivot, found - the knuckles were pinned, the fist turns

**Report (headset, after s88):** "it doesn't desync turning my hand 90 degrees to the right, but
it does 90 degrees to the left"; weapons sit off in the hand; the shotgun points up.

**Ruled out first.** The controller's orientation chain (quaternion -> yaw/pitch/roll -> integer
rotator -> axes) is exact: `tools\tests\orient-chain-tests.cpp` checks 61,875 orientations
(a whole-sphere grid and 20,000 random ones under each hand's trim) - 0 off by more than 0.05
deg. Mode 4 already reads the GRIP pose (`aimPose` only steers retired mode 3). So the drawn
hand turned exactly as the controller did; what was wrong was the point it turned ABOUT, and
the axis it faced along.

**Measured in Blender** (`tools\blender\hand_pivot.py`, the rig in the settled wrench grip,
`Wrench__EquipWrench` last frame; renders `hand-pivot-*.png` in the model workspace): the curl
circle of each finger, the handle axis through the four curl centres, the fist centre on it,
and the palm normal. In the frame of the wrist and finger-base heads (rig units; wrist to the
middle base = 12.1):

| | position | from the fist centre |
|---|---|---|
| fist centre (the controller grip's origin belongs here) | 10.87 / 0.50 / -3.65 | - |
| the drive's anchor until s89 (hand + four finger bases) | 9.15 / -1.07 / -0.38 | 4.02 |
| R_grip, bone 43 | 3.80 / -0.76 / -2.21 | 7.33 (at the heel) |

The anchor sat on the knuckles, about 3 cm behind the real fist at drawn size; and the hand's
handle axis was oriented only by eye-tuned trims (right -32/-4/-8, left -30/31/-206, and the
headset retune of 09:27 - rotation -39.7/-7.8/+3.6 and a 10.7 cm position trim, which is that
lever being chased by hand). A fixed point 3 cm off the real one makes the drawn fist swing
toward the face turning one way and away turning the other: the asymmetric report.

**Moved there** (`hand_grip.h`, default ON): the drive pins the FIST CENTRE (rebuilt each frame
from the wrist and finger-base heads, rigid with the hand) to the grip origin, and turns the
wrist so the fist's handle axis lies on the grip's -Z and its palm normal on the grip's X
(Dishonored's calibration G, solved from the mesh instead of a key press). The rotation trims are
residual nudges, zero by default; the eye-tuned ones are dropped (keys renamed
`HandCalibTrim*` / `HandFistTrim*`, hands.ini `offHand*Deg` no longer read). Levers
`vrhands palette fist|calib on|off`.

**Simulator** (`pivot-sweep.ps1 -Wide -Laser`, 19 orientations incl. yaw +-90 / +-120, pitch
+-70, yaw 90 with roll 90, both hands): before, the wrench pointed straight UP with the controller
level (the "shotgun points up" shape); after, the wrench handle runs along the controller's axis
at every orientation - forward, left at yaw +90, right at yaw -90 - with the laser's dots
continuing past its head on the same line, the fist held in place, the left hand turning about
the same point. Host: `palette-math` 11,977 checks (calibration lands the handle on forward and
the palm on -X right / +X left, with a negative control; the left fist is the mirror of the
right; the point scales with the hand).

**Not done here:** per-weapon grips (the shotgun's authored hand pose still differs from the
wrench's - step 3 of the plan), the weapon socket (step 2).

**s89b - the headset run (Virtual Desktop):** "the hands start flipped 180 degrees facing my
shoulder". The geometric calibration assumes the OpenXR spec's grip axes; the sim's grip pose is
whatever the script sets, so it could not see that VDXR's axes differ. Calibration now OFF by
default; the orientation is the tester's tuned trims again (left -30/31/-206, right
-39.7/-7.8/3.6); the fist-centre pivot stays. Every HANDS interval logs a `GRIPCAL` line: the
stored trim beside the trim the calibration would set, so the next run measures the runtime's
grip axes instead of assuming them. Lesson: an axis convention is a runtime property, and only a
real runtime answers it.

### s90: no lens pull; the hands were placed 9 cm below the eye

s89c handed over the foreground lens pull as the leading suspect for the desync that survived the
pivot fix. Measured in the sim, it is not there: the hands draw's own cb0 says the fg pass renders
it from the camera exactly (ENGINE_NOTES s90), with the world's lens and rotation. The section-6
risk below is closed for the palette route.

What the same measurement found instead: the camera carries the head-offset sliders
(`CameraHeightOffset`, 9 UU up) and the hand target did not, so every hand sat 9 cm low relative to
the eye (grip - camera up -23.95 UU, true -15). That error is fixed in the WORLD frame; a palm-frame
trim rotates with the hand, so it cancels the error only at the orientation it was tuned at. The
mode 4 target now adds the slider vector (`FrameContext::anchor*`, `vrhands palette headanchor`,
default ON); BRVR's hands are head-relative and always had it. With trims zeroed the drawn fist
reads up -14.95 against the eye, the sim's true -15.

The s89c position trims (~14 cm per hand) were tuned against the old placement and should be
zeroed before re-tuning. Not changed: the aim ray and laser still start from the pawn's eye point.

## 6. Risks, recorded before they bite

- **The evaluator's convention.** Hex-Rays shows `sub_10E97CF0(int *a1@<ecx>, int a2@<ebp>)`.
  The `@<ebp>` is likely its SEH frame, but a C++ `__fastcall` detour is only safe if it
  is not. BS2's naked ret-stub is convention-agnostic and is the shape to copy (duplicated
  per the decoupling rule).
- **Evaluation inside a render pass.** BS2 measured its update inside pass 1 only. In
  BS1, caller `+0x319F00` looks like the same per-draw path. Compose there must not take
  locks the game thread holds.
- **The fg lens pull** (`vrfgfov`, +11.5 UU at the matched lens, ENGINE_NOTES s16). Mode 2
  applies `render_lock_delta` to the target; mode 4 must too, or the hand sits slightly
  off the controller in depth. **Closed s90:** measured zero on the palette route (the hands
  draw's cb0 eye is the camera, +-half IPD).
- **Engine-placed actor culling.** Mode 3 was limited by culling when the actor origin
  went behind the camera. Mode 4 keeps the origin on the camera, so it cannot.
- **Plasmid FX** attached to the actor rather than to a bone would stay at the camera.
  Check in P6; Dishonored's `fx_follow` is the fallback design.
