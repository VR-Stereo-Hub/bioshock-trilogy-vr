# BioShock 1 viewmodel rig: this repo vs the BRVR mod

Deep comparison of how the two mods place the first-person hands and the held
weapon, written 2026-08-21 while chasing a weapon bob that survives a working
skeleton freeze. Companion to `docs/bioshock1/PORT-PLAN.md`.

Sources: this tree at `5bc5999`, BRVR at `9e56d58`, plus BRVR's own
`docs/modules/hands.md`, `docs/ENGINE-MAP.md`, `docs/INVARIANTS.md` and
`.planning/features/skeletal-hand-drive.md`. Where a BRVR source comment and a
BRVR doc disagree, the doc is cited: a comment in that tree claimed
`Level.Pauser` was "the one reliable signal" and its own logs showed the
detector had never fired.

---

## 1. The weapon's own skeleton: neither mod drives its pose

This was the open question and it has a clean answer.

| | Weapon-skeleton access |
|---|---|
| **BRVR** | **None.** The only weapon-actor access in the tree is read-only: `WatchGunDistance` reads `CurrentHoldable` (`hands+0x45C`) then its `Location` (`+0x1D8`) for a diagnostic distance line. There is no write to the holdable, its `SkeletonInstance`, or its bones anywhere |
| **This repo** | **Uniform scale only.** `set_weapon_scale` / `wskel_drive` drives the equipped holdable's own `SkeletonInstance`, but only to scale translations about the grip. Its own contract says **"quats adopted per frame (weapon animations keep playing while scaled)"**, and at the default `wscale 1.0` **"the lane drops the skeleton entirely"** |

**So at default settings both mods leave the weapon's own skeleton fully
engine-animated.**

That is an elimination, not just a fact. BRVR has no weapon bob and does nothing
at all to the weapon skeleton, so **the walk bob cannot live there.** No
measurement is needed to rule it out, and the `wscale` lane is not worth
investigating for this symptom.

---

## 2. Where the two rigs actually diverge

Both mods agree on the rig map, independently derived, which is the strongest
confirmation either will get:

```
left cluster 6..21    left sleeve  {3,4,5,22,23}
right cluster 27..44  right sleeve {24,25,26,45,46}
weapon attach 43      muzzle-ish tip 44      47 bones
48-byte hkQsTransform   SkeletonInstance +0x3FC   bone array +0x48   count +0x4C
```

They diverge on **what owns the AHands actor**, and that is the whole of it.

| | BRVR | This repo |
|---|---|---|
| **AHands actor transform** | **PINNED.** `DriveHands` writes `Hands.Location` **and** `Hands.Rotation` from the grip pose every frame | **Engine-owned.** `mode hands` did this and is **retired**. The engine places the actor relative to the view every tick |
| **How bones are targeted** | In the pinned actor's frame | Composed against the **live** actor transform each frame: `ptc = qaInv·(grip − actorLoc)`, so an actor that moves cancels out |
| **Rotation write timing** | Twice. `DriveHands` on the game thread, **then `CameraHook_LateHandsWrite` re-applies the whole rotator from Present** | Once, on the game thread inside the CalcView path |
| **Reference freeze** | Unconditional while driving (`CaptureClusterRef` early-outs), re-captured on **held-weapon identity change** plus a settle window | Magnitude gate: adopt when the probe delta is under 6 UU / 12 deg (`swaykill`, default on) |
| **Bone 43 rotation** | Written as of Build V (`WeaponHandBone43Rot`), guarded on unit quaternion | Written for every cluster bone including 43 |
| **Bone 43 scale** | Never written. **Fatal** — the attach path inverse-decomposes and *divides* | Excluded from the `.s` lane for 43 and 44 |

### The line that matters most

BRVR's `CameraHook.cpp`, in `DriveHands`:

> *Roll restored. **The game tick erases it**, so `CameraHook_LateHandsWrite`
> re-applies the whole rotator from Present, after they are done.*

**The game tick overwrites writes made to the hands actor during CalcView.** BRVR
discovered this and answered it with a second, late write from the render thread.
This repo has no equivalent: every actor-level write it might make would land at
CalcView time and be erased before the frame is drawn.

That is the single most important difference for anyone attacking a viewmodel
placement problem here, and it is not visible from either README.

---

## 3. What has been falsified for the walk bob, with evidence

Recorded so nobody re-walks these. All 2026-08-21, headset.

1. **Widening the sway gate while moving.** Premise was that the walk bob is
   velocity-weighted (true: `UpdateHandBobAnimationParameters` drives channel 2
   with weight = velocity/GroundSpeed) and would blow past a gate calibrated to a
   standing idle. **Measured and false.** Walking peaks were 1.0–1.6 UU /
   2–5.6 deg, indistinguishable from standing and far under the 6 UU / 12 deg
   gate. `adopt` was already false while walking: **the reference freeze holds,
   and the bob has never come through the skeleton path.** The gate change was a
   no-op and was reverted.

2. **Pinning the AHands actor's Z to a bob-free height** (pawn `Location +
   eyeHeight`, the same origin the camera fix uses), applied inside the CalcView
   bone path. **Result byte-identical.** Reverted. Note that the bone targets
   compensate for `actorLoc`, so pinning Z there changes nothing downstream by
   construction — and per §2 a CalcView-time actor write is erased by the game
   tick anyway. Either explanation alone accounts for the null result.

3. **Bone 43's rotation being unwritten.** BRVR's `ENGINE-MAP` records this as
   the cause of *"the hands appear to not be animated, but the gun sway still
   is"*, with idle drift of 1–5 deg peaking at 134.77 deg. **Not applicable
   here**: the drive loop writes `q` for every bone in cluster 27–44, bone 43
   included. Checked, not tested — the hole does not exist in this tree.

4. **The weapon's own skeleton.** Ruled out by §1.

---

## 4. What is left, ranked

The bob is not in the skeleton (1), not in the weapon's skeleton (4), and the
hands do not bob — so it is in the chain that composes the **weapon actor** from
the AHands actor. Two candidates remain and one measurement separates them.

**A. The actor's ROTATION bobs, not its location.** This fits the symptom better
than anything tried so far: a rotational bob about the actor origin moves the
hands barely at all (they sit near the pivot) and swings a long gun visibly at
the muzzle. "Hands fine, gun bobs" is what a pitch bob looks like. BRVR pins
rotation and has to re-apply it late *because the game tick erases it* — which is
exactly the write this repo never makes.

**B. The attach path composes the weapon from a pre-write snapshot.** The engine
resolves `AttachToBone`/`SetBase` during its own tick, so the weapon actor's
world transform may be computed from the actor transform and bone array *as they
were before* the CalcView bone write lands. The hands would still look right,
because they are drawn from the bones we wrote; the attached weapon would lag
into the bobbing pose.

**The measurement that separates them** is read-only and cheap: log, at 1 Hz
while walking, the AHands actor's `Rotation` (`+0x1E4`, as integers — a rotator
read as floats is a denormal) and the weapon actor's `Location` (`+0x1D8`) and
`Rotation`, all against the pawn eye point.

- Actor rotation oscillating at step frequency → **A**, and the fix is BRVR's:
  pin the rotation and re-apply it from Present, not from CalcView.
- Actor rotation steady but weapon actor location oscillating → **B**, and the
  fix is a late re-assert of bone 43 after the tick, not an actor write at all.
- Neither oscillating → the bob is downstream of both, in the foreground
  render pass, and this whole line of attack is wrong.

Do the measurement before the next attempt. Two theories were tried on reasoning
alone today and both produced byte-identical results.

---

## 5. Other differences worth knowing before porting viewmodel work

- **Re-capture keying.** BRVR re-captures its frozen reference on *what is in
  your hand* — the holdable's identity, explicitly not the weapon slot — after a
  settle window that lets the equip animation finish, and the window applies
  after **any** release. This repo infers the same thing from delta magnitude.
  The event-keyed form cannot be fooled by an animation whose amplitude happens
  to sit near the threshold; the magnitude form needs no weapon identity. This
  tree already has `weapon_key_is()` maintained from `Hands.CurrentHoldable`, so
  the event-keyed form is available if the gate ever proves unreliable.
- **The dirty byte has one owner.** `SkeletonInstance+0x88` is one byte for all
  47 bones. BRVR had six uncoordinated writers and the last one in the frame won,
  which produced a one-frame arm across the view on weapon switches. Its rule:
  any new per-bone writer must ask before setting the byte, never add a seventh
  independent writer. This repo's `set_dirty(0)` is a single site today; keep it
  that way.
- **A placement offset must not live in the actor's frame.** The actor is rotated
  by the weapon hand, so an offset expressed there swings as the *other* hand
  turns. It belongs in the frame it describes.
- **The sleeve must be re-pinned after the cluster write**, not before. BRVR
  pinned sleeve bones at the wrist the engine had just written, the cluster write
  then moved the wrist, and the forearm stayed behind as a spike out of the palm.

---

## 6. ANSWERED 2026-08-21: the AHands actor's LOCATION bobs

The probe in §4 ran and separated the candidates on its first session. Player
walking in circles in a small room, wrench then a gun equipped.

| Quantity | Standing peak-to-peak | Moving peak-to-peak |
|---|---|---|
| **actor Z above the pawn** | 0.15 – 0.96 UU | **5.3, 5.5, 6.0, 7.7, 8.1, 10.5, 17.2, 22.8 UU** |
| actor pitch | 0 – 3.4 deg (aim) | **0.46 deg max** |
| actor roll | 0.00 | 0.00 |
| weapon vs actor | 0.1 – 1.9 UU | *no sample* |
| weapon pitch / roll | 0.5 – 39 deg (equip) | *no sample* |

**Candidate A is falsified.** The actor's rotation is flat while moving: 0.46 deg
peak-to-peak at most, against 0.00 for roll. A rotational bob would have shown
here and did not.

**The actor's vertical position is the bob.** Ten to twenty-five times larger
moving than standing, on a gait cadence, up to 22.8 UU peak-to-peak.

So the Z-pin attempt (§3 item 2) had the right quantity and failed on **write
ordering**, not on the theory. A write made during CalcView is erased by the game
tick before the frame draws, which is precisely why BRVR has
`CameraHook_LateHandsWrite` re-applying from Present. This repo made the write in
the wrong place, not to the wrong field.

**Caveat on the measurement.** Actor Z is sampled relative to the pawn, so
terrain contributes in principle. Against that: standing spans stay under 1 UU,
the moving spans are gait-cadenced, and the room was small and flat. Treat the
absolute numbers as indicative and the standing-vs-moving ratio as the result.

**Candidate B is untested, not eliminated.** The weapon actor is only cached
after the first shot, and by the time one was cached the player had stopped, so
every `weapon vs actor` sample is a standing sample. It is largely moot - an
actor-location bob of this size explains the symptom on its own - but if pinning
the actor does not fully settle the gun, get a moving sample before doing
anything else.

### The fix this implies

Pin the AHands actor's Z **from Present, after the game tick**, mirroring
`CameraHook_LateHandsWrite`. Not from the CalcView path. Everything else about
the earlier attempt (bob-free target from `Pawn.Location + eyeHeight`, re-arm on
a large drift so stairs and lifts pass through) stands; only the write site
changes.

Verify with the same probe: the moving `actor Z` span should collapse to the
standing value. That is a numeric pass condition, not a judgement call.

---

## 7. Observed side effect: the wrench idle animations are gone

Reported in the headset 2026-08-21, on the build carrying the anchored-screens
and camera head-bob work. The wrench's idle fidget/slap no longer plays. **The
user considers this an improvement** - BRVR ships `IdleAnimMode=1` specifically
to suppress the wrench slap - so it is recorded as wanted behaviour, not a
regression to chase.

**It is unattributed.** Nothing in that build was aimed at idle animation, and
the plausible causes are not distinguished:

- `swaykill` was already default-on and freezes the driven rig's reference, so
  the idle fidget may simply never have played since session 20 and only now
  been noticed.
- The camera head-bob substitution changed the view origin, which could make a
  small residual fidget imperceptible without removing it.

Worth one cheap check before anyone relies on it: `vrhands swaykill off`, hold
the wrench, stand still. If the fidget returns, the freeze owns it and the
behaviour is stable. If it does not, something else is suppressing it and that
is worth knowing before a future change re-enables it by accident.
