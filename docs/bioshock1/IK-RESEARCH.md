# Arm IK: what FRIK does, why, and what of it belongs here

Research pass over [`rollingrock/Fallout-4-VR-Body`](https://github.com/rollingrock/Fallout-4-VR-Body)
("FRIK"), read at `b7ad4e1` (2026-08-30). **No code was changed in this mod for
this document** - it is a reading of their architecture against ours, and a
ranked list of what is worth taking.

Everything below cites `src/skeleton/Skeleton.cpp` unless stated otherwise.
Our side is `src/game/bioshock1r/bones.cpp`, `solve_arm()`.

---

## 0. Licence: we can take the IDEAS, not the code

**FRIK is GPL-3.0. This repo is MIT** (`LICENSE`). Copying their source in
would force GPL terms onto the files it lands in, so nothing below is ported
as code: every item is reimplemented from the described behaviour, with a
comment naming FRIK as the source of the idea.

(Corrected 2026-10-07. The first draft of this section said the repo was
GPL-3.0 too and that their code could be ported literally. It was wrong:
`LICENSE` has been MIT since the first commit. No FRIK code was copied on the
strength of it.)

This is worth stating because the repo's hard rules only name UEVR
(all-rights-reserved, concepts only) and REFramework (MIT). FRIK is a third
case: concepts only, like UEVR, for a different reason.

---

## 1. The one difference everything else follows from

**FRIK animates a real, visible, third-person body. We animate a viewmodel.**

FRIK owns the player's actual skeleton - 25 named bones from `Root` through
`COM`, `SPINE1..2`, `Chest`, both collarbones, both arms, both legs, `Neck`,
`Head`. Its job is that the body looks right *to the player looking down* and
*to anyone else*. Legs walk, the spine pitches, the head counter-rotates on a
neck.

We own an arm and a hand hanging off a weapon rig, seen from one eye position,
with no torso and no legs. Most of FRIK's machinery has no counterpart here and
should not grow one.

The consequence for reading their code: **their solutions are shaped by having a
torso to intersect and a spine to hang off.** Where a decision of theirs is
motivated by the torso, it usually does not transfer. Where it is motivated by
*the arm's own geometry*, it almost always does.

---

## 2. Their pipeline, in order

`Skeleton::onFrameUpdate()` (line 180), and the order is load-bearing - each
stage does a full world-transform update so the next one reads correct world
positions:

| # | stage | what it establishes |
|---|---|---|
| 1 | `restoreNodesToDefault()` | every one of the 25 bones back to a **hardcoded** local transform |
| 2 | `getNeckYaw()` / `getNeckPitch()` | where the torso is facing, from HMD + both hands |
| 3 | `setupHead()` | head counter-rotated by the neck yaw so it still looks where the HMD looks |
| 4 | `setBodyUnderHMD()` | `_forwardDir` / `_sidewaysRDir` - **the body frame everything else uses** |
| 5 | `setBodyPosture()` | spine pitch, hip position under the neck |
| 6 | `setKneePos()`, `walk()`, `setSingleLeg()` | legs |
| 7 | `setArms(false)`, `setArms(true)` | arm IK, right then left |
| 8 | cull, hand pose, pipboy | presentation |

Ours is a subset of stage 7. There is no stage 4 equivalent that *derives*
anything - we take the body frame from the camera.

---

## 3. Decision by decision

### 3.1 The body frame comes from the HANDS, not from the game's yaw

`getNeckYaw()` (line 300) is the piece with no counterpart here, and it is the
most interesting thing in the file.

It takes the vector from the HMD to each controller, **sums them**, and treats
that sum as the direction the torso faces. Then:

- returns 0 if either hand is within 10 units of the HMD (degenerate)
- reduces the weight as either hand rises above the head (`0.05 * z`)
- reduces the weight when the hands cross over each other (`locLeft.x > locRight.x`)
- switches from `atan2(x, y)` to `atan2(x, z)` when the pitch difference exceeds
  80 degrees, so looking straight down does not blow up the yaw
- clamps the result to **±50 degrees**

`setBodyUnderHMD()` then builds the body frame as **the HMD's flattened forward
rotated by `0.7 * neckYaw`** (line 388), and `setupHead()` rotates the head back
by the full `neckYaw` so the view is unaffected.

**Why:** they have no other signal. The game's own body yaw is driven by
locomotion and is not where the player's torso is pointing. Where your hands are
is the only evidence available about your shoulders.

**Why it matters to us:** this architecture *structurally cannot have the bug
s74 just fixed.* Their body frame is derived from tracked hardware, so it cannot
rotate out from under the hands - the hands are its input. Ours is derived from
the game's yaw, and the whole s74 defect was that frame drifting relative to the
hands.

**Does it transfer?** Partly, and it is not urgent. We have no torso, so the only
consumer is the shoulder anchor. But the `0.7` factor and the ±50 clamp encode a
real idea worth stealing eventually: *the shoulders should sit somewhere between
where the head points and where the hands are*, never fully at either. Right now
ours are rigidly at the room frame, which is the correct end of the range but not
responsive.

See §5 for why this is ranked below the two arm-geometry items.

### 3.2 The shoulder SLIDES toward a reaching hand

Lines 1046-1058, and their own comment calls it "done in a very simple way":

```
shoulderToHand = handPos - upperArm.world.translate
adjustAmount   = clamp(|shoulderToHand| - armLength*0.5, 0, armLength*0.85) / (armLength*0.85)
shoulderOffset = norm(shoulderToHand) * (adjustAmount * armLength * 0.08)
```

So: while the hand is within half an arm's length, nothing happens. Beyond that,
the shoulder slides toward the hand, ramping to a maximum of **8% of an arm
length**. The clavicle is then rotated to point at the new shoulder position and
the subtree updated.

**Why:** a real shoulder girdle translates when you reach. Without it the arm
hits full extension early and locks, and every remaining centimetre of reach has
to come from somewhere that does not exist.

**Ours anchors the shoulder rigidly** and clamps `d` to `L1+L2`. The s74 log
measured `sh->hand` **past full reach on 19 of 44 samples** during ordinary head
turning. The frame fix removed the head-driven part of that; simply reaching
forward will still hit it.

**This is the highest-value item in the document.** It is ~6 lines, it needs no
new bone (we synthesise the shoulder point, so we add the offset to `sWorld`
before it is converted into component space), and it directly attacks a defect we
have measured.

### 3.3 Over-reach STRETCHES the bones rather than clamping

Lines 1096-1102:

```
if (hsLen > upperLen + forearmLen) {
    diff  = hsLen - upperLen - forearmLen
    ratio = forearmLen / (forearmLen + upperLen)
    forearmLen += ratio * diff + 0.1
    upperLen   += (1 - ratio) * diff + 0.1
}
```

The two segments absorb the shortfall in proportion to their length, so the
triangle always closes and the wrist always lands **on** the hand.

**Why:** a slightly-too-long arm reads as a stretch. A correct-length arm whose
wrist does not reach the hand reads as a *detached hand*, which is far worse.

**Ours clamps `d`.** That is the opposite trade, and s73 already recorded the
symptom: the hands looked stretched an unnatural distance from the wrists, like
a very long wrist. That was diagnosed as an
`L1/L2` scaling bug and fixed, but the clamp behaviour behind it is still there
and still produces the same visual whenever the hand is genuinely out of reach.

**Second-highest value**, and it pairs with 3.2: the shoulder slide buys ~8% of
reach, and the stretch covers whatever is left over.

### 3.4 Impossible arm positions: SEVEN mechanisms, not one

This is the part of their solver with the most thought in it, and it is much more
than the bail-out. There are seven distinct guards, layered, each catching a
different flavour of impossible - and **the same seven appear in the leg solver**
(`setSingleLeg`, lines 728-750), which is what marks them as a deliberate house
idiom rather than accumulated patches.

| # | guard | line | catches |
|---|---|---|---|
| 1 | `isFiniteTransform(handWorldTarget)` | 1024 | a NaN target before anything is touched |
| 2 | per-component `isnan`/`isinf` on `handPos` | 1038 | the same, after the transform is unpacked |
| 3 | `\|upperArm.world - handPos\| > 200.0` -> bail | 1039 | **controller tracking loss** - their stated reason |
| 4 | `hsLen = max(\|handToShoulder\|, 0.1f)` | 1091 | hand exactly at the shoulder: divide-by-zero |
| 5 | `hsLen > (upperLen+forearmLen) * 2.25f` -> bail | 1093 | target so far it is certainly wrong, not merely a reach |
| 6 | **proportional stretch** when `hsLen > upper+forearm` | 1096 | hand out of reach but plausible (see 3.3) |
| 7 | **equal-segment fallback** when the law of cosines returns NaN | 1178 | **hand too CLOSE to the shoulder** |

Numbers 6 and 7 are the interesting pair, because they are the two ends of the
same failure and they are solved in opposite ways.

**Too far (6)** is absorbed by lengthening both segments in proportion, so the
wrist always lands *on* the hand.

**Too close (7)** is the one worth stealing outright. The law of cosines has no
solution when `c < |a - b|` - the hand is nearer the shoulder than the difference
of the segment lengths - and `acosf` returns NaN. Their fix:

```
if (isnan(wristAngle) || isinf(wristAngle)) {
    forearmLen = upperLen = (originalUpperLen + originalForearmLen) / 2.0f * adjustedArmLength;
    wristAngle = acosf(...);   // recompute
}
```

Set both segments to their mean. With `a == b`, `|a - b| == 0`, so **the triangle
closes for every distance greater than zero** and the degenerate case cannot
recur. Total arm length is preserved, so the elbow lands somewhere sane rather
than the solve failing.

**What we do instead.** `solve_arm()` clamps the distance into the legal band:

```
const float dMin = fabsf(L1s - L2s) + 1e-3f;
const float dMax = L1s + L2s - 1e-3f;
if (d < dMin) d = dMin;
if (d > dMax) d = dMax;
```

That is numerically safe - it never produces a NaN - but it is a **lie about
where the hand is**, in both directions. Past `dMax` the wrist stops short of the
hand (the "super long wrist" read of s73). Inside `dMin` the wrist overshoots
*past* the hand, toward the shoulder, which is the pose you get bringing a hand
to your own chest or reloading near the body.

Their approach keeps the wrist on the hand in both cases and moves the error into
segment length, which is far less visible than a detached hand.

**Ours has no plausibility guard at all** - `solve_arm()` returns early on mode,
collapse and invalid reference, all internal state, but a garbage *target* is
solved as enthusiastically as a good one. We have no fallback animation for the
free arm, so a bail for us means "leave the arm where it was", which is still
much better than solving to a NaN.

`setArms()` (line 985) adds an eighth layer specific to their API: if an external
mod supplied the target and the solve bailed, it restores the arm to default and
re-solves to the tracked hand - because bailing *after* the collarbone has
already been rotated leaves the arm half-solved, and an external target can stay
bad indefinitely where a tracking dropout clears next frame. **The transferable
part is the observation, not the code: their solver mutates the clavicle before
it can still fail.** Ours writes nothing until the triangle is solved, so we do
not have that hazard - worth keeping that way.

### 3.5 The elbow is constrained by three measured conditions

Lines 1134-1170. This is the most elaborate part of their solver and the least
portable.

Two twist angles are read off the hand's own basis - `handBack.z` (where the
wrist points into the forearm) and `handSide.z` (a vector out the side of the
wrist) - and blended, weighted by how far down the hand is pointing:

```
interpTwist = clamp((handBack.z + 0.866) * 1.155, 0.45, 0.8)
twistAngle  = twistAngle + interpTwist * (twistAngle2 - twistAngle)
```

Then a **limit range** whose ends move with three measurements:

| measurement | how | effect |
|---|---|---|
| hand behind the body | signed distance to a plane through the shoulder along `forwardDir` | raises the minimum elbow angle |
| hand crossing the chest | plane rotated 135 deg from forward | raises the minimum, and sharply lowers the maximum |
| hand lifted above chest height | `chest.world.translate.z` vs `handPos.z` | lowers the maximum (elbows point down as hands rise) |

`twistMinAngle` runs -85..-35 deg and `twistMaxAngle` 55 deg down to as little as
-35, and the blended twist is mapped into that range.

**Why:** elbows have anatomical limits and the failures are extremely visible on
a third-person body - elbow through the ribs, elbow inverted, elbow above the
shoulder.

**Ours** blends an authored bend direction against a body-frame down/outward pole
on the `elbow follows wrist` slider, with `elbow out` for the outward component.
That is a much simpler model, arrived at by tester feel across s72-s73.

**Verdict: take the shape, not the code.** The three conditions are the right
three, and "hand crossed over the chest" is the one our pole model has no answer
for at all. But their constants are tuned against a Fallout skeleton and a
visible torso, their own source carries a commented-out `fixWonkiness1/2` block
that says this was hard to tune even for them, and we have no ribcage for the
elbow to pass through. **Medium value, high effort, and it should follow 3.2/3.3
rather than precede them.**

### 3.6 They smooth a SCALAR, we smoothed a FRAME

Line 1143:

```
static std::array<float, 2> prevAngle = { 0, 0 };
twistAngle = prevAngle[i] + (twistAngle - prevAngle[i]) * 0.25f;
```

A flat 25%-per-frame exponential smooth on the elbow twist **angle**, per hand,
explicitly commented "to reduce elbow shake".

This is worth calling out as an architectural observation rather than a port
candidate. **s73 lost a session to smoothing the elbow in a frame that moved with
the held hand** (`1beab15`), and s74 lost most of another to a frame mismatch.
Smoothing a scalar angle cannot have either bug: an angle has no frame, so there
is no frame to get wrong.

**The general rule worth writing down: when a quantity can be smoothed either as
a scalar or as a position, smooth the scalar.** Our elbow smoothing is in the
body frame now and correct, but it is correct because we fixed the frame twice,
not because the representation made it hard to get wrong.

### 3.7 The reference pose is HARDCODED, not captured

`getSkeletonNodesDefaultTransforms()` (line 1338) is a literal table of 25 bone
transforms, and `restoreNodesToDefault()` writes all of them every frame before
anything else runs. Their comment gives the reason plainly: loading a save does
**not** reset the skeleton, and loading repeatedly makes it progressively worse.

**Ours captures** `g_freeArmRef` in a settle window. s72 hit exactly the failure
this design avoids - *"latching the reference mid-equip"*, which made Telekinesis
and Electro Bolt land in different places - and the fix was to gate the capture
on settling rather than to stop capturing.

**Does it transfer? No, and the reason is worth recording so nobody tries.** Their
skeleton is one rig, shipped with the game, identical every session. Ours is
per-weapon, and we do not ship the game's data, so there is nothing to hardcode.

What *does* transfer is the underlying principle, which our own notes already
state in a different form (`g_ref` is our own output one frame later, so anything
measured against it measures itself): **prefer a reference that cannot be
contaminated by your own output.** A cheap middle path is to keep the captured
bank but validate it against a stored fingerprint and re-capture on mismatch.
Low priority - the settle gate has held since s72.

### 3.8 Hand damping subtracts player velocity

`dampenHand()` (line 1294): slerp the world rotation toward the previous frame,
lerp the position - but first subtract the player's own movement:

```
dir      = _curentPosition - _lastPosition;
deltaPos = node->world.translate - prevFrame.translate - dir;
```

**Why:** without that term, damping fights locomotion. Walking forward would drag
the hand backward every frame, because "the hand moved" and "the player moved"
are indistinguishable to a naive smoother.

We hit the same class of problem from the other end in s72 - the head-bob fix
stopped the *view* bobbing and left the *gun* bobbing, because the two consumers
of the camera location silently diverged (`camera.cpp`, the `baseLoc` re-take).
Same lesson, different symptom.

**Worth auditing:** whether our anchor easing compensates for player velocity. If
it does not, the weapon lags while walking, which is the sort of defect that
reads as "feel" and never gets reported precisely.

### 3.9 There is no held-hand / free-hand fork

`setArms(false); setArms(true);` - both hands run the identical path through
`solveArmToHandWorldTarget()`. The only difference is where the target comes
from: for the weapon hand, the game's own `Update1StPersonArm` writes
`_rightHand->world` and they IK to whatever that produced.

**Ours forks structurally** - CARRIED vs SOLVED - and `STATUS.md` records that
full unification is not possible, because the held hand's solver input is the
frozen cluster's authored anchor, a constant in component space.

**That conclusion still stands and this is not evidence against it.** FRIK has no
fork because their engine hands them a *world-space transform* for the held hand;
ours hands us a component-space constant. The difference is in what the engine
provides, not in the algorithm. Recorded here only so the next reader does not
see `setArms(isLeft)` and conclude our fork was avoidable.

### 3.10 Arm length is ONE user-calibrated scalar

`Config.cpp:325` - `armLength`, default **36.74**, and
`adjustedArmLength = g_config.armLength / 36.74f` scales both bone lengths
(line 1043). The user adjusts it in the in-VR body config
(`BodyAdjustmentSubConfigMode.cpp:253`, 5-unit steps).

So a tall player calibrates once and every arm quantity - segment lengths, the
shoulder-slide ramp, the reach limits - scales together off the same number.

**We have no equivalent.** Confirmed against the F10 panel per the standing rule:
`bones.cpp` exposes `shoulder fwd/right/up (cm)`, `elbow out`, `elbow follows
wrist`, `elbow smoothing (ms)` - and nothing for arm length. `L1`/`L2` come from
the authored rig scaled by `s`.

**This is cheap and it is the lever that makes 3.2 and 3.3 rarer.** A player whose
real arms are longer than the authored rig's is permanently near full extension,
and no amount of shoulder-slide fixes a reach mismatch at the source.

### 3.11 Every hand modifier goes into the TARGET, then one solve

`WeaponHandRecoil::applyToHandWorldTarget()` (`src/skeleton/WeaponHandRecoil.cpp:152`)
is called in `setArms()` **before** `solveArmToHandWorldTarget()`. Visual recoil
is not applied to the bones afterwards - it is added to the hand's world target,
and the arm then solves to the recoiled hand like any other pose. Same for an
external mod's hand override: `ExternalAuthority::getHandWorldTransform()` also
produces a target, which then goes through the identical solve. `setArms()` says
so directly: *"only as the target handed to the same solver, so everything
downstream of the arm sees one consistent result."*

They also wrap the native engine kick in a `ScopedNativeKickNeutralizer` while
computing, so the game's own recoil cannot fight their controlled version.

And it fails safe - their doc comment: *"false only when the recoil could not be
applied safely, in which case the target is left unmodified and the hand solves
without recoil rather than to a garbage pose."*

**Ours takes the other route for recoil.** `solve_arm()`'s comment says the held
hand passes *"the eased anchor it just wrote (so the arm does not solve to a
wrist the hand has left during a recoil)"* - i.e. we deliberately solve the arm
to a **lagged** version of the hand so recoil does not whip the elbow.

That works, but it means during recoil the arm is solving to one place and the
hand is drawn at another - the two disagree for exactly as long as the easing
takes. Their model has no such window: one target, one solve, everything
consistent by construction.

**Worth considering, not urgent.** It is an architectural preference rather than
a bug, and our easing exists because it fixed something real. But *"put every
modifier into the target and solve once"* is the cleaner invariant, and it is the
sort of thing that is much cheaper to adopt before there are three modifiers than
after.

---

## 4. What NOT to take

- **The neck/spine/hip posture model** (`setBodyPosture`, `getBodyPitch`,
  `setKneePos`, `walk`, `setSingleLeg`) - roughly 300 lines serving a visible
  torso and legs. We have neither.
- **The hardcoded default-transform tables** - see 3.7. Per-weapon rigs, no
  shippable game data.
- **Their elbow constants verbatim** - tuned against a Fallout skeleton in
  Fallout units, with a visible ribcage as the thing being avoided. Take the
  three conditions, derive our own numbers.
- **`hideHands()`'s trick** of setting `_root->local.scale = 0.00001f` to hide the
  arms. We already have `g_collapse` / `g_collapseOff`, which are per-arm and do
  not smuggle a scale through the transform chain.

---

## 5. The plan, in phases

All of it is per-game BS1 work in `solve_arm()` and cannot reach `src/core/`.

**Where each item stands (2026-10-07):**

| item | state |
|---|---|
| 0 - capture | `tools\armcap.ps1` built; no capture recorded yet |
| 1a + 1c - stretch and shoulder reach offset | **Tried and REJECTED in the headset** (2026-08-31, `archive/s75-reach-fixes`). Reported: the left shoulder sat well forward of the right, and the whole arm stretched instead of the wrist while the shoulder did not move. Cause, from that commit: the authored arm (66.2 UU) is shorter than the player's, so the ramp never rested at zero and the stretch was always on. The branch was reset and only 2a kept |
| 1b, 1d - near-reach fallback, implausible-target guard | Also only on the archived line; not on this branch |
| 2a - arm-length scale | **Landed** (`890ae21`), default 1.0, not headset-tuned |
| 2b, 3, 4 | Not started |

The rejection is the reason 2a must come before 1a/1c if they are ever retried:
with the rig's arm shorter than the player's, both reach fixes fire on every frame.

Ordered as phases rather than a flat list, because the capture in phase 0 turns
three later items from guesses into measurements, and doing them in the wrong
order means tuning the same thing twice.

**"What you will see" is the important column.** Two of these are invisible in
normal play and only pay off when something goes wrong; that is not a reason to
skip them, but it is a reason not to expect a headset session to confirm them.

---

### Phase 0 - the capture (no code changes)

| | |
|---|---|
| **What** | `tools\armcap.ps1` - record head + hand + elbow through as many seated poses as possible (§7) |
| **You will see** | nothing in the game. This produces a CSV, not a behaviour |
| **Plan** | strap a controller to the elbow, hold the other in that hand, cover the pose list in the script header, drop a grip marker between groups. Several short runs beat one long one |
| **Then** | the fit gives arm segment lengths, shoulder position relative to the head, and the swivel data - feeding phases 2 and 3 |
| **Risk** | none to the mod. It is a standalone exe that never loads BioShock |
| **Watch for** | the tracked percentage. Under 60% and the run is mostly IMU drift - redo it rather than fit it |

---

### Phase 1 - the reach fixes (independent of the capture; can start any time)

These three are the ones with a measured defect behind them, they are all small,
and none of them needs the capture data to be correct.

**1a. Proportional over-reach stretch** (3.3)

| | |
|---|---|
| **You will see** | the wrist stops detaching from the hand when you reach out. This is the "super long wrist / stretched to an unnatural degree" percept from s73, at its source |
| **When** | any time you extend the arm, which the s74 log showed was 19 of 44 samples even standing still |
| **Plan** | replace the `d > dMax` clamp with FRIK's proportional lengthening: split the shortfall between `L1s` and `L2s` by their length ratio. ~6 lines |
| **Risk** | low. Below full reach it is a no-op, so anything it changes was already broken |
| **Verify** | headset. Hold the gun at full stretch and look at the wrist join |

**1b. Equal-segment fallback when the hand is too close** (3.4)

| | |
|---|---|
| **You will see** | the elbow stops snapping when you bring a hand in toward your own chest or shoulder |
| **When** | reloading, hand near the body, plasmid poses close in |
| **Plan** | on `d < dMin`, set both segments to their mean instead of clamping `d`. With `L1 == L2` the legal band starts at zero, so the case cannot recur |
| **Risk** | low, and it pairs with 1a - together they mean the wrist always lands *on* the hand at both extremes |
| **Note** | **nobody has looked at this end.** The `dMin` clamp currently pushes the wrist *past* the hand toward the shoulder |

**1c. Shoulder reach offset** (3.2)

| | |
|---|---|
| **You will see** | the arm stops locking dead straight early. Reaching forward feels like it has another few centimetres in it, because it does |
| **When** | any extended reach - which is also when 1a fires, so these two are felt together |
| **Plan** | before converting `sWorld` into component space, slide it toward the hand: `clamp(\|shoulderToHand\| - armLen*0.5, 0, armLen*0.85) / (armLen*0.85) * armLen * 0.08` along the normalised shoulder-to-hand direction |
| **Risk** | low. It ramps from zero at half-reach, so a bad constant degrades toward today's behaviour rather than breaking |
| **Verify** | headset, together with 1a - the pair is what makes full extension look right |

**1d. Implausible-target guard** (3.4)

| | |
|---|---|
| **You will see** | **nothing, normally.** It only shows itself when a controller loses tracking, and then it is the difference between the arm freezing and the arm flying into geometry |
| **Plan** | at the top of `solve_arm()`, bail on non-finite `W`, and on a shoulder-to-hand distance beyond a sane multiple of the arm. Bail means write nothing - our solver already writes nothing until the triangle is solved, so there is no half-solved hazard to clean up |
| **Risk** | low, and it is the cheapest insurance here |

---

### Phase 2 - calibration from the capture data

**2a. Arm-length scale** (3.10)

| | |
|---|---|
| **You will see** | the whole arm finally sized to *your* arm. If the authored rig is shorter than your real reach, you are permanently near full extension and phase 1 is papering over it |
| **Plan** | one scalar multiplying `L1`/`L2`, defaulted from the captured segment lengths, exposed as an F10 slider (checked: no such control exists today) |
| **Risk** | low, defaults to today's behaviour at 1.0 |

**2b. Shoulder position from the fit** (§7.3)

| | |
|---|---|
| **You will see** | the arm hanging from the right place. Hard to notice directly, very noticeable in how natural reaches feel across the whole space |
| **Plan** | sphere-fit the shoulder from elbow samples, replace the *defaults* of `shoulder fwd/right/up` - keep the sliders, since a fit from one body should not become a hard constant |
| **Note** | s72d already found the two sets were asymmetric from being tuned in isolation. This settles that argument with data rather than symmetry |

---

### Phase 3 - the measured elbow model

| | |
|---|---|
| **You will see** | the elbow sitting where a real elbow sits, across the whole pose space rather than just the poses the sliders were tuned at. Most visible where our current model has nothing to say: hand across the chest, hand behind the hip, hand up near the face |
| **Plan** | fit swivel angle against hand direction, reach fraction, wrist twist, height and midline offset. Must be smooth (discontinuities become elbow pops) and normalised by arm length. Ship it as the *default* behind the existing sliders |
| **Risk** | medium. It is a model of one person's arm, and the honest mitigation is keeping the sliders |
| **Supersedes** | FRIK's three hand-tuned constraint conditions (3.5) - no reason to port their tuned constants if we have measured ones |

---

### Phase 4 - deferred, and each for a stated reason

| item | why it waits |
|---|---|
| **Player-velocity compensation audit** (3.8) | may already be correct; it is an audit, not a change. Visible symptom if wrong: the gun lags while walking. Cheap to check whenever |
| **Every modifier into the target** (3.11) | architectural cleanup, not a bug. Visible only during recoil, and our easing exists because it fixed something real. Much cheaper to adopt before there are three modifiers than after |
| **Hand-corrected body frame** (§6) | closes the physical-torso-rotation blind spot, which **nobody has ever reported**. It would touch a frame that was signed off hours ago. Revisit only if a second frame defect appears |

---

### If only one thing gets done

**Phase 1a + 1c together.** They are the two halves of "the arm runs out of
reach", they are both small, they both fail safe, and between them they are the
only items on this list with a number already measured against them - 19 of 44
samples past full reach, standing still.

---

## 6. Is the hand-derived body frame "more proper"? Not exactly

Worth working through properly, because the intuitive answer ("they derive it
from real hardware, so yes") is wrong.

**The shoulder position is not determined by the available data.** We have three
tracked things - HMD and two controllers - and a person can hold all three
perfectly still while rotating their torso. So every approach here is an
*estimator*, and the question is only which one is wrong in which direction.

Three families:

| model | shoulder yaw follows | used by |
|---|---|---|
| head-locked | HMD yaw | most cheap VR bodies |
| **room-locked** | fixed in the play space; moves only on artificial turns | **us, since s74** |
| **hand-derived** | HMD yaw rotated 0.7x toward the hands' bisector, clamped +-50 deg | **FRIK** |

Now the two cases that matter:

**Case A - turn your head, hands and body still.** (What the tester did last
night. The common case by a wide margin.)

- head-locked: **wrong**, shoulders swing the full head turn. This is the s74 bug.
- room-locked (ours): **right**. Shoulders do not move.
- FRIK: **partly wrong**. The hands stay put while the HMD turns, so the bisector
  falls behind the HMD by roughly the head-turn angle, `neckYaw` grows, and the
  body lands at HMD-forward rotated 0.7 back toward the hands - leaving about
  **30% of the head turn** in the shoulders, saturating once `neckYaw` hits its
  50 deg clamp.

**So for the case we just spent a session fixing, our model is strictly better
than FRIK's.** That was not what I expected going in.

**Case B - physically rotate your torso in the room** (swivel chair, stepping
around) while head and hands move with it.

- head-locked: right.
- room-locked (ours): **wrong**, and this is our blind spot. The body transfer
  follows the head, so `gameYaw` and `recenterYaw` both advance and the net yaw
  is unchanged - the shoulder anchor stays at its old room yaw. Turn 180 deg in
  place and the right shoulder anchor now sits where the *left* shoulder
  physically is, roughly 60 cm out.
- FRIK: **right**. The hands rotated with the torso, so the bisector tracks the
  HMD, `neckYaw` stays near zero, and the body follows.

**The honest conclusion: neither dominates - they fail on opposite cases.** What
FRIK genuinely has over us is not correctness but *shape*. Theirs is a **bounded
blend** - never fully right, never more than 50 deg wrong, degrading smoothly.
Ours is a **corner solution** - exactly right in case A and unboundedly wrong in
case B.

### The synthesis worth building instead of either

Neither model uses the strongest available signal, which is **time**.

A head glance is fast and reverts. A physical torso rotation is slower and
persists. So:

- keep the room-locked frame as the base, because it is already correct for
  case A and is signed off;
- compute FRIK's hand-bisector yaw as a *correction*, with their de-weighting
  (ignore when a hand is within 10 units of the HMD, fade as hands rise above the
  head, fade when the hands cross);
- drive the shoulder yaw toward that correction through a **long time constant** -
  order of a second, not a frame.

A glance never survives the filter, so case A stays exactly as correct as it is
now. A sustained physical turn does survive it, so case B stops being a blind
spot. And it fails safe: with the correction gain at zero it is bit-identical to
today's behaviour, which makes it A/B-able on one slider.

This is a better design than either mod has, and it is the one thing in this
document that is not a port. It is still ranked last (see §5) because case B has
never been reported - it should wait until someone actually notices it.

---

## 7. Measuring the real elbow: a capture protocol

The tester's idea - strap a controller to the elbow and record where it actually
goes - is sound, and it is aimed at exactly the right unknown. Worth writing down
properly, including what will go wrong.

### 7.1 Why it is the right target

Once the shoulder, the hand, and the two segment lengths are fixed, a two-bone
arm has **exactly one remaining degree of freedom**: the swivel of the elbow
around the shoulder-to-hand axis. Everything in FRIK's 40 lines of elbow
constraints (§3.5) - the two twist angles, the blend, the behind/crossing/lifted
limits - is one hand-tuned estimator of that single angle. Our `elbow out` and
`elbow follows wrist` sliders are a second, simpler estimator of the same angle.

**Neither has ever been measured against a real elbow.** Both were tuned until
they looked right. A capture would replace an estimator with data.

### 7.2 It works with two controllers, not three

A Quest 3 pairs exactly two controllers, so there is no third tracker - but none
is needed:

- **left controller strapped to the right elbow**
- **right controller held in the right hand**
- HMD as usual

That gives simultaneous head + hand + elbow ground truth for one arm. The off
hand is sacrificed for the session, which does not matter for a capture run.
(Which physical controller goes where is arbitrary - whichever straps more
securely.)

### 7.3 It calibrates the shoulder too, which may be the bigger prize

This is the part worth emphasising. With the elbow tracked and the upper-arm
length known, **the shoulder is solvable rather than guessable**: it lies on a
sphere of radius `L1` about the elbow, and across many poses it is the single
point that stays fixed relative to the head while satisfying that constraint for
every sample. That is an ordinary sphere-fit / least-squares problem.

The strapped controller's offset from the actual joint is unknown but *constant
in the controller's own frame*, so it is another few parameters in the same fit
rather than a source of error.

Our shoulder offsets are currently three hand-tuned F10 sliders
(`shoulder fwd/right/up (cm)`) that s72d already found were asymmetric because
each had been tuned in isolation. **A capture would replace all three with
measured values, and would also give the real arm length** - which is candidate
#3 in §5 and the lever that makes over-reach rare.

So the run yields three things, in descending order of certainty:

1. **arm segment lengths** - easiest, most robust, immediately useful
2. **shoulder position relative to the head** - a well-posed fit
3. **the swivel model** - the ambitious one, and the most fragile

### 7.4 The thing that will go wrong: occlusion

Quest 3 controllers are tracked inside-out by the headset cameras, with IMU
dead-reckoning when sight is lost. **An elbow-mounted controller is behind and
below the hand, often out of view, and the arm occludes it.** When tracking
drops, the pose does not vanish - it *drifts*, silently, and drifted samples look
exactly like real data.

This is the same trap as the s73 method note: a number that is present but wrong
is far more dangerous than one that is missing.

Mitigations, in order of importance:

- **Filter on `XR_SPACE_LOCATION_POSITION_TRACKED_BIT`, not just `_VALID_BIT`.**
  `openxr_input.cpp:730` currently checks only the VALID bits, which stay set
  while the runtime is extrapolating from IMU. TRACKED is what distinguishes a
  real observation from a guess. **The capture must record this flag per sample
  and discard anything not tracked** - and it is worth adding the distinction to
  the mod's own pose reads regardless of whether the capture happens.
- Keep the arm in front of the chest and in view; sweep slowly.
- Short sessions, many of them, rather than one long one - drift accumulates.
- Record a *stationary* reference pose at the start and end of each run and
  compare; if they disagree, the run drifted and is void. This is the same
  precondition discipline `offhand-swivel.xrs` uses.

### 7.5 Do it OUTSIDE the game

The capture needs no BioShock at all - it is pure OpenXR pose logging. **The
right host is `src/tools/xr_hello32/`** (207 lines, already a minimal standalone
OpenXR app in this repo). That means:

- no injection, no game, nothing to crash, no Rule 2 launch question
- it can be run any time, in any room, without a save or a weapon equipped
- the data is clean of every game-side transform - no `worldScale`, no
  `recenterYaw`, no DrawScale, none of the frames that have cost this project
  three sessions

Output should be a flat CSV - timestamp, HMD pose, both controller poses, both
tracked-flags - and the fitting done offline afterwards. Nothing about the model
needs to be decided before the data exists.

### 7.6 What the model should look like, and its honest limits

Fit the swivel angle against features that are cheap to compute at runtime -
essentially FRIK's, now measured instead of assumed: hand direction from the
shoulder (normalised), hand distance as a fraction of full reach, wrist twist,
hand height relative to the shoulder, and hand offset across the body midline.

Two hard requirements:

- **Smooth.** A lookup table with discontinuities becomes elbow pops. A low-order
  polynomial or a smooth fit over the reach hemisphere is the right shape.
- **Normalised by arm length**, or it is a model of one specific body.

And the limits, stated plainly: it is **one person's arm**, so it is a model of
the tester rather than of people. That is acceptable here - he is the only tester -
but it argues for keeping the fitted model behind the same sliders that exist
now, as a *default* rather than a replacement, so a different body can still
adjust.

---

## 8. The anatomical arm model, and why clamping alone cannot work

Written after s75's twist clamp failed in the headset, and after the tester made
the observation that settles the design: the player is bound by real controller
positions, so every pose a human can make is anatomically valid, and since it
can be done in real life there is a correct way to show it in game.

**That is decisive and it kills the framing s75 was built on.** The clamp assumed
a tracked controller could reach orientations a forearm cannot, and that the
excess had nowhere to go. But the controller is held in a real hand on a real
arm: every orientation reaching the solver has already been produced by an
anatomically valid pose. There is no impossible region. **The excess is not
error to be discarded - it is a real rotation belonging to a joint we were not
driving.**

That is why all three bounded formulations failed:

| approach | failure |
|---|---|
| accumulate unbounded (s73f) | a full turn winds permanently; never unwinds |
| store the clamped value (s75a) | latches at a *wrong branch* 165 deg off - measured |
| clamp the wrapped angle | hard ~170 deg snap at the far side |

All three try to fit a rotation the arm really made into one joint that cannot
hold it. Measured on the sim, 2026-08-31: a full 360 deg controller roll took the
accumulator from -11 to +349 and left it pinned at the limit.

### The three degrees of freedom a real arm uses

Hand roll is not one joint. It is three, and the research is consistent that a
solver must use all three:

| source | range | notes |
|---|---|---|
| **forearm** (radius over ulna) | ~85 sup / ~75 pron | the only true pronation joint |
| **humerus** (glenohumeral internal/external rotation) | ~90 | internal rotation accompanies pronation, external accompanies supination |
| **elbow swivel** (where the elbow sits around the shoulder-to-hand axis) | free | changes the forearm's frame, so it changes how much twist is *needed* at all |

Forearm alone gives 170-180 deg. **Combined with the shoulder the hand reaches
nearly 360.** The elbow swivel is the third and least obvious: rotating it
changes the forearm axis's roll reference, so a pose that would need 200 deg of
twist with the elbow down may need only 90 with the elbow lifted.

### What the VR solvers actually do

**VRIK** (Final IK; the solver behind *Dead and Buried*) is described by its own
author as "a collection of analytic and heuristic solvers rather than a purely
mathematical approach". The parts that matter here:

- The **elbow bend plane is guessed from three inputs**, mixed empirically: the
  hand's world position, **the hand's world ROTATION**, and the hand's position
  relative to the chest. Hand rotation feeding the bend plane is exactly the
  third DOF above - the elbow is *supposed* to move when you roll your wrist.
- **Shoulder rotation is derived from the ratio of shoulder-to-hand distance to
  arm length** - the same shape as FRIK's reach offset in section 3.2 - and is
  **clamped to a valid range**.
- `shoulderTwistWeight` twists the shoulders as the arms lift; `swivelOffset` is
  an explicit angular offset on the elbow bend direction.
- `wristToPalmAxis` / `palmToThumbAxis` define the hand bone's axis convention
  **explicitly**. Ours does not: s72z subtracts a hand-tuned "authored twist"
  constant to paper over the same problem, and the s75 log measured the residual
  error at a **median of -59 deg**, which is what made one roll direction hit the
  limit at 26 deg and the other at 144.

And the line that names our elbow behaviour as backwards, from RootMotion's own
write-up: *unrealistically large wrist rotations are corrected by rotating the
elbow in a direction which reduces the wrist rotation.* Our pole does the
opposite - `elbowFollowWrist` is a fixed blend that lets wrist roll **drive** the
elbow, rather than the elbow being solved to **relieve** the wrist.

### The model this argues for

Per arm, per frame, replacing the clamp:

1. **Fix the hand.** It is tracked; it is never adjusted. Everything else serves it.
2. **Solve the triangle** for the elbow, as now.
3. **Measure the total roll** the hand needs about the forearm axis, relative to
   a properly defined reference - not the current authored-constant fudge but an
   explicit axis convention, VRIK's `wristToPalmAxis` being the pattern.
4. **Let the elbow swivel absorb what it can.** Choose the bend plane to
   *minimise* the required roll rather than to follow it. This is the piece we
   have backwards today and the cheapest large win.
5. **Split the remainder anatomically:**
   - forearm takes up to its limit (~85), distributed 0 at the elbow to 1 at the
     wrist across the existing twist helpers - the elbow is a hinge and can hold
     no step, which s73g already established;
   - **the humerus takes the rest**, up to ~90, applied as upper-arm roll;
   - the clavicle takes none.
6. **Nothing is discarded.** forearm + humerus + swivel reproduce the hand
   exactly, so there is no seam, no latch and no wind-up - the three failures
   above are all consequences of having nowhere to put the excess, and now there
   is somewhere.

This also explains, rather than contradicts, the tester's long-standing report
that the shoulder rotates with the wrist. **It should** - that is humeral
rotation and it is anatomically correct. What was wrong was the amount and the
absence of any budget governing it.

### Cost, honestly

This replaces `g_armTwistShare`, the s75 clamp and the `elbowFollowWrist` blend
with one derivation. It is the largest single change in this document and it
needs the axis convention pinned down first - step 3 is a prerequisite for
steps 4-5, and the measured -59 deg offset says the convention is currently
wrong. The elbow-capture rig in section 7 would settle it directly, since it
measures where the elbow actually goes for a given hand orientation.

---

## 9. Sources

All line numbers against `rollingrock/Fallout-4-VR-Body` at `b7ad4e1`.

| topic | file |
|---|---|
| arm solver, shoulder offset, elbow constraints, twist | `src/skeleton/Skeleton.cpp:1022-1284` |
| per-frame order, reset-to-default | `src/skeleton/Skeleton.cpp:180-286` |
| body frame from hands | `src/skeleton/Skeleton.cpp:300-410` |
| hand damping | `src/skeleton/Skeleton.cpp:1294-1336` |
| default transform tables | `src/skeleton/Skeleton.cpp:1338-1427` |
| arm length config | `src/Config.cpp:325`, `src/config-mode/BodyAdjustmentSubConfigMode.cpp:253` |
| their own architecture summary | `CLAUDE.md:33-80` |

A read-only mirror is at `docs/frik-reference/`, excluded via
`.git/info/exclude` the same way `docs/brvr-reference/` is - local only, never
committed, and deliberately not in `.gitignore`.

### VR arm IK (section 8)

- [VRIK - Final IK documentation](http://www.root-motion.com/finalikdox/html/page16.html) -
  `shoulderRotationMode`, `shoulderTwistWeight`, `swivelOffset`,
  `bendGoalWeight`, `wristToPalmAxis`
- [Inverse Kinematics in Dead and Buried - RootMotion](http://root-motion.com/2016/06/inverse-kinematics-in-dead-and-buried/) -
  the three bend-plane inputs, and "rotate the elbow to REDUCE wrist rotation"
- [dabeschte/VRArmIK](https://github.com/dabeschte/VRArmIK) - shoulder
  estimation, distinct shoulder rotations at arm-stretch limits, dislocation
  detection
- [Parger et al., *Human upper-body inverse kinematics for increased embodiment
  in consumer-grade VR*, ACM VRST 2018](https://dl.acm.org/doi/10.1145/3281505.3281529)
- [Stolpe, *Inverse Kinematics for Arm Pose Reconstruction in VR*](https://kurser.math.su.se/pluginfile.php/105616/mod_folder/content/0/2021/2021_stolpe_erik.pdf)
- [Two Bone IK solver - Autodesk Maya](https://help.autodesk.com/cloudhelp/ENU/MayaCRE-CharacterAnimation/files/GUID-CEA46DDD-40C8-4F78-8928-DEBE1A94E430.htm) -
  bend plane and pole vector fundamentals
