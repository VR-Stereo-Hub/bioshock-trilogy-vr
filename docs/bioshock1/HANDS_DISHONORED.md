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
  off the controller in depth.
- **Engine-placed actor culling.** Mode 3 was limited by culling when the actor origin
  went behind the camera. Mode 4 keeps the origin on the camera, so it cannot.
- **Plasmid FX** attached to the actor rather than to a bone would stay at the camera.
  Check in P6; Dishonored's `fx_follow` is the fallback design.
