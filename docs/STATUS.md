# Project status

> Handoff file. Rewrite "Current state" and "Next steps" every session; append to the session log.

## Current state (2026-07-28, session 21 - RENDER SYNC: the fov audit came back clean, the fg scene decoded down to its ctor args, the fovA zoom-pull lever found, per-weapon profiles shipped - branch s21-render-sync)

**Branch `s21-render-sync` off main (post-PR-#5). Three flat-gated commits:
the FOV audit (negative result + permanent instruments), the fg scene-node
discovery (the `vrfgnode` instrument + the fovA lever, every new render
lever DEFAULT OFF - the shipping look is byte-identical to session 20), and
per-weapon aim profiles (shipping; engages only when a weapon resolves).
v0.3.0 is still NOT tagged - it waits for the in-headset verdict on the
checklist below and the user's explicit go.**

### 1. THE FOV AUDIT (plan item 1) - the fov-lie hypothesis is DEAD, and that is the finding

Instrumented the projection submission (per-eye claimed tangents + claim
source logged on change; the `fovaudit` seam command prints option vs
submitted vs option-derived side by side; `fovaudit pose on` arms a
tagged-vs-consumed yaw log for the headset) and built
`tools/decode-framedump.ps1` (recovers tangents from the cb0 screen-ray
block per depth-tested draw, clusters them, tags fg draws by the
lens-blind fg-bake stack RVAs). Measured under vrstereo on a clean boot
(`dumpframe full 2`): ONE tangent cluster per eye window - tanH=2.1445
tanV=1.2063 = exactly tan(130/2) at 16:9 - identical in both eyes, fg
draws included (the vrfgfov lens match verified from the cb side for the
first time), and no hidden second lens (every undecodable block is a
zero-filled non-perspective pass). The submission builds its claim from
the same option int at the same aspect, so rendered == submitted by
construction + measurement. BioVR's "remove the lie" discipline was
ALREADY our architecture. The +-90 drift is NOT a projection-tag mismatch;
the live suspects narrowed to the pose-tag domain (one new log line + the
poseaudit measure it on the next headset run) and the fg composition
(finding 2).

### 2. THE FG SCENE DECODED (plan item 2 - world-pass re-homing found a better lever)

The dump stack-diff + capstone work mapped the fg mechanism end to end
(constants in patterns.h "FOREGROUND SCENE NODE"; full story + negatives
in ENGINE_NOTES):

- The fg pass is a SECOND SCENE NODE (0x400 bytes, allocated per frame,
  ctor RVA 0x56DC30, stored at scene+0x1B0) flowing through the SAME
  render machinery as the world; the per-section transform providers are
  skinning strategies (656/178/240 live instances), NOT fg markers.
- The ctor receives the CAMERA POSE + TWO FOVS as plain arguments: vec3
  camera loc, rotator camera rot, fovA (PC+0x45C, engine-restamped 75.0
  every frame - why session 15's poke read inert), fovB (PC+0x460, the
  vrfgfov field).
- The camera inputs are ALREADY per-eye correct under SR stereo
  (pass-labeled capture: pass 1 receives the LEFT eye camera, pass 2 the
  RIGHT, matching apply_eye_offset to the digit). Two models raised and
  KILLED by measurement the same night: the stale-pre-drive camera and
  CROSSED EYES (which would have inverted the rig's disparity). The
  `vrfgnode sync` substitution built for the crossing is a measured
  flat-static no-op - kept default-OFF as an in-headset latency A/B only.
- **THE FIND: fovA/fovB is the fg ZOOM-PULL pair.** `vrfgnode fova match`
  (ctor-argument substitution - always wins, no byte patching) collapses
  the rig to TRUE world-lens geometry: screenshot diff 11.09 mean / 44.2%
  of channels changed vs the 2.9 ambient floor, world pixels untouched,
  exact round-trip on `fova off`, fire path clean under `fova match` +
  `vrbones lock off`. This is the fov-coupled eye-dolly/magnification the
  render lock has countered since session 13, now controllable at ONE
  seam.
- Negatives (ENGINE_NOTES): no script-side fg membership property exists
  (Actor + Hands property lists enumerated); pawn+0x724 = pawn->Hands and
  nulling it is FATAL (one crash, dump #9, minidump kept) - a proven
  non-lever; the 576-byte cb tier is not fg-exclusive.

**Candidate end-state for session 22 (needs the retune + the user's
eyes): `vrfgfov on` + `vrfgnode fova match` + `vrbones lock off` = the rig
rendered at true world geometry, retiring the whole counter-model domain
(render lock, lockgain/lockdgain/lockpull, kFgEye* constants).**

### 3. PER-WEAPON PROFILES (plan item 3) - SHIPPING

- **Identity**: `patterns::object_class_name()` - UObject +0x28 = own
  FName index, +0x30 = UClass (vtable-gated at RVA 0xE2F04C), derived
  live and cross-checked (weapon -> 'Shotgun'; the AHands actor ->
  'PlayerHands'). The profile key is the canonical class name.
- **The layer**: the R-hand trim + ray-origin offsets hot-swap per weapon
  class; the R atomics stay the single live truth (laser, fire ray, model
  publish all unchanged by construction); stash-on-switch,
  seed-from-current on first sight; persisted to weapons.ini;
  `vrpreset save` chains it; and the preset's value load RE-APPLIES the
  active profile at its tail (a flat-caught ordering clobber, fixed and
  gated). Commands: `vraim weapon | wsave | wkey sim <name> | wkey real`;
  the aim overlay shows the active key.
- **Resolution**: the weapon actor resolves PRE-FIRE now - the anchored
  scan accepts STRUCTURALLY first (candidate Base +0x450 == the AHands
  actor; attachment, not proximity) with the 120 UU proximity fallback,
  plus null-resolve backoff (the game intro has no weapon for minutes and
  must not full-heap-scan at 2 s cadence - live-reproduced).
- **Flat gates, all exact**: sim-key round-trip restores stashed values
  to the digit both directions; weapons.ini write/load round-trips (10
  values / 2 weapons); the composed chain proven on a clean boot (ini
  loaded at init -> the equipped shotgun keyed 'Shotgun' pre-fire ->
  profile values applied over the preset baseline -> re-applied after the
  preset chain); fire seam subs 2/2 with the profile live; heartbeat
  clean; dumps stable. The REAL weapon-switch swap cannot be driven flat
  (`exec NextWeapon` FAULTS - standing trap): it is the headline of the
  in-headset checklist.
- **The calibration flow**: our laser IS the fire ray - equip, fire at a
  wall, nudge the R trim/ray-offset sliders until the beam sits on the
  bullet hole, next weapon; profiles capture automatically; one "Save
  preset values" press persists everything.

### Harness notes (new traps, all field-hit this session)

- **boot.ps1 roulette**: after an abandoned run the MAIN MENU focus sits
  on NEW GAME, and the A-press loop starts a new game (the 1960 plane
  intro; this NG+ profile shows "IMPORTING NEW GAME PLUS DATA"). No save
  damage - the intro does not autosave before the lighthouse and the .bsb
  set was verified untouched - but flat runs MUST screenshot-verify the
  landing state. Deterministic recovery: main menu -> dpad-DOWN as an
  80 ms TAP (250 ms holds auto-repeat) to LOAD GAME -> A -> A on the
  newest entry. Mouse clicks do NOT register on the MAIN menu (they do on
  gameswf pause menus).
- The weapon-profile update gates on gameplay view AND backs off after 3
  null resolves - do not loosen either guard.
- A weapons.ini written by flat tests is a TEST ARTIFACT and was deleted
  at session end (the session-17 "ini overrides code defaults" trap,
  weapons edition): profiles apply OVER the preset's R values by design,
  so a stale test ini would clobber live tuning. The user's real profiles
  seed from their tuned R values on first play.
- Crash-dump baseline moved 8 -> 9 this session (the pawn+0x724 discovery
  crash, deliberately taken and logged); every later boot held 9.

### Session 21 part 2 (same day) - THE HEADSET FEEDBACK ROUND

**The user's verdicts:** (a) **`vrbones lock off` is "exactly what I was
looking for - the aim is in tune with the model... perfect"** - the +-90
laser-vs-gun drift ROOT CAUSE was the render lock's own correction. LOCK
IS NOW DEFAULT OFF (`vrbones lock abs` = the A/B back). (b) `vrfgnode
fova match` made THE WORLD move with head motion (rig "decent") -
in-headset NEGATIVE, parked default-off; the fovA arg evidently feeds a
world-coupled consumer beyond the rig bake (ENGINE_NOTES). (c) Per-weapon
profiles "didn't change per weapon" - TWO log-proven bugs, both fixed +
flat-gated the same night: the resolver's learned-object preference
pinned the key to the previously FIRED weapon across wheel switches (now:
Hands.CurrentHoldable at hands+0x45C read directly off the rig - instant,
scanless, pre-fire swap detection), and the first profile seeded from
pre-preset ZEROS then re-applied over the user's baseline (now: the
resolver idles until the preset baseline is captured; new profiles seed
from that baseline, not from the outgoing weapon's values). (d) NEW ASK,
next session's headline: DECOUPLE world scale from model scale - a world
slider and a separate hand/model scale slider (worldScale currently
scales both; the session-16 negatives - bone .s attach blowup, DrawScale
inert on the fg path - are the known walls; the fovA true-scale geometry
may be the opening if its world coupling gets explained). (e) FPS/freeze
audit: every stall in the run's log coincides with a FOCUSED->VISIBLE
window (VD overlay/headset off) - NOT mod work; zero mid-play scans, zero
blocked waits, clean heartbeat during play. Flat gates for the fixes:
baseline captured (0/-7.9/7.5), 'Shotgun' created FROM the baseline,
actor+class resolve pre-fire via the rig read, sim-swap round-trip exact
(4.00/2.00 restored; Pistol seeded from baseline), fire subs 2/2, dumps
9->9, no stray weapons.ini.

**Re-test in headset (short):** (1) tune the pistol's laser at a wall,
wheel-switch to the shotgun - its own tuning must be there IMMEDIATELY
(no fire needed), switch back - pistol tuning returns; the log echoes
`weapon profile '<name>' applied` per switch. (2) "Save preset values"
once happy; reload + PRESET 1: all weapon tunings return. (3) Lock-off is
now the default - confirm the aim-model sync feels like your `lock off`
moment with no commands. (4) Optional: `fovaudit pose on` for 30 s (the
submission closeout from the session-21 checklist still stands).

### Session 21 part 3 (same day) - run 2: profiles nearly perfect, the MG/GL gap

**The user's run 2:** per-weapon profiles "almost perfect... pretty good" -
Shotgun/Pistol/ChemicalThrower/Crossbow keyed and swapped per wheel switch
(log-proven, instant, no fire needed). But **MachineGun and
GrenadeLauncher never keyed** (their tuning edits polluted whichever
profile was still active): those two carry a DIFFERENT NATIVE VTABLE than
kPlayerWeaponVtableRva, so the vtable-gated holdable read rejected them
and the stale-cache fallback pinned the old key. FIXED the same night:
`hands::current_holdable()` returns the rig's holdable CLASS-AGNOSTICALLY
(the profile layer keys purely on object_class_name, which validates via
the UClass vtable and resolves ANY class; an unresolvable class now CLEARS
the key so edits can never touch another weapon's profile - logged).
Flat-gated: 4 profiles load, Shotgun keys + applies, preset-save chains
weapons.ini (mtime + 20 lines verified), fire subs 2/2, dumps stable. The
MG/GL live-switch proof needs the wheel - headset item.

**Also done on the user's ask:** (a) their four GOOD profiles from run 2
were rescued to weapons.ini via `vraim wsave` before the game closed (the
run had not pressed save) - Crossbow/others may carry MG/GL pollution, the
user re-checks per weapon; (b) the user's FIXED LEFT-HAND calibration (aim
trim +4.4/+30.0 deg, ray offset right +4.6 / up +0.7 cm, model offsets all
zero) is written into vrpreset.ini AND baked as CODE DEFAULTS (aim.cpp) -
new installs now start on their calibration; the rest of their preset
(worldScale 100, ipd 63, gfov 130, deadzone 23) already matched the
shipped defaults. The R-hand/profile default bake waits for their next
tuning pass (their call). weapons.ini is now a LIVE USER FILE - never
delete it in harness cleanups.

**Headset re-test (run 3, short):** (1) equip the MACHINE GUN - the log
must echo `weapon profile 'MachineGun' CREATED ...` the moment it equips;
tune it; same for the GRENADE LAUNCHER; (2) re-check Crossbow (and any
weapon tuned right before/after the MG/GL attempts in run 2) for polluted
values - retune where needed; (3) switch across all six - each keeps its
own; (4) "Save preset values" once at the end; (5) plasmid hand: confirm
the left laser sits right out of the box (its calibration is now the
default).

### Session 21 part 4 (same day) - RUN 3 PASSED, v0.3.0 SHIPS

**Run 3: "Alright perfect, looks pretty good and everything is added" -
MachineGun + GrenadeLauncher keyed and held their own profiles (the
class-agnostic fix proven live), all eight holdables calibrated, the left
hand re-tuned (trim -6.8/+30.0, offsets -2.8/0.6/0.5), deadzone set 0 by
the user (lock-off removed the percept the 23-deg band existed for).**
Their live values were saved via the seam (`vrpreset save` -> all three
inis) and BAKED AS CODE DEFAULTS on their explicit ask: aim L/R defaults,
deadzone 0 (body.cpp), and all 8 weapon profiles seeded at init
(weapons.ini overrides key by key). Aim-trim sliders widened to +-90 deg
(the user's L yaw sat pinned at the old +-30 cap). VIRGIN-INSTALL GATE:
with all three inis set aside, a clean boot + preset reproduced the full
calibration from code alone (L trim/offsets, R baseline, deadzone 0, 8
profiles, Shotgun keyed + applied) - a fresh install needs no tuning;
inis restored after, fire subs 2/2, dumps stable. README rewritten (per
weapon profiles + the bundled-preset section incl. how existing users
adopt or keep their own). v0.3.0: PR #6 merged, tagged, released with
the preset inis in the zip - the user's go on record this part.

## Previous state (2026-07-28, session 20 - THE AIM-SYNC SESSION: one trim algebra, vrrec record+replay, FName/GNames, the muzzle ray, the idle-sway kill - ALL SIX STAGES FLAT-GREEN on branch s20-aim-sync)

**Branch `s20-aim-sync`, MERGED to main as PR #5 (2026-07-28, the user's
explicit call after the part-2 results below - everything default-armed is
sound; the one failed feature ships default-OFF). v0.3.0 is NOT tagged yet -
it waits for the session-21 work. The session-19 plan's stage designs
(4.1-4.6) were followed as written - nothing re-derived - with two design
corrections forced by measurement (the SET-seam sway premise dissolved; the
fopen_s share-mode trap resurfaced 19 sessions after session 1 fixed the
same bug in the logger).**

### Session 20 part 2 (same day) - IN-HEADSET RESULTS, the BioVR analysis, and the re-plan

**The user's headset run, the ground truth:** (a) the algebra unification is
correct but a NO-OP for their weapon hand (R trims are 0/0 - expected once
understood; the fix matters for the L hand's 17.7 deg trim and any future
trim). The REAL standing defect: the laser sits LATERALLY OFF the rendered
gun with the offset FLIPPING SIGN with hand yaw (aim 90 deg right of facing
-> beam LEFT of the weapon; 90 deg left -> beam RIGHT), and the beam does
not originate at the barrel on most weapons. (b) `vraim muzzle` FAILED: the
pistol beam went up-and-right "by a lot" - the bone-43->44 axis is NOT the
barrel (d0 read ~31 deg up / ~18 deg right); the flat gate had only proven
internal consistency, never axis-vs-rendered-barrel ground truth. It ships
default OFF; RETIRE the derivation (keep `vrbones skel`). (c) muzzle mode
showed the SAME lateral drift as normal mode -> the drift is UPSTREAM of
all ray math, in the render/composition domain. (d) swaykill (default ON)
drew no complaint - provisionally good.

**The BioVR analysis (BioVRDev/Bioshock-Remastered-VR, released ~2026-07-13,
analyzed 2026-07-28; NO LICENSE = all rights reserved - CONCEPTS AND
MEASUREMENTS ONLY, never code; full findings in RESEARCH.md):** their scale
numbers are IDENTICAL to ours (1 UU = 1 cm, half-IPD ~3.2 cm camera offset,
head pos x100) - their better feel comes from two DISCIPLINES: (1)
FOV-EXACT SUBMISSION (projection layer tagged with a symmetric frustum
built from exactly the game's locked render FOV, never the runtime's
asymmetric per-eye fov; their measured mismatch symptom "yaw warped, pitch
stayed clean" MATCHES our +-90 sign-flipping drift), and (2) CYCLOPEAN HAND
ANCHORING (hands at one world position for both eyes - per-eye-camera
placement cancels disparity, "zero parallax means very far away", reads
HUGE = our sessions 11-16 size percept). Their aiming polish = dot==shot
from one value + per-weapon 3-vec3 profiles keyed by real weapon class +
an exact "fire at a wall, nudge the dot onto the hole" calibration flow.
They did NOT solve the rendered-gun-vs-dot drift (same structural split as
ours), have NO sway kill (ours works), and credit this repo for the
reticle disable, the arm-hide bone indices, and the HUD capture.

**The re-plan (user's call): the WORLD-PASS RE-HOMING is the new big swing**
- move the EXISTING AHands rig (+ attached weapon) out of the foreground
pass into the WORLD pass (find the pass-membership switch with dumpframe's
per-pass draw classification; flip it). If it works the entire drift domain
dies at once (fg lens split, eye dolly, render lock all unnecessary) with
FX/equip anchoring intact. Do NOT spawn a separate puppet actor - FX and
anim-notifies stay on the original rig. Follow-up idea (user's): anchor the
VR frame to the pawn's STATIC eye point (Pawn.Location + EyeHeight, the
+0x550-area field) instead of the animated camera, so walk-bob never leaks
into camera or hands - the BioVR-proven lever.

**1. The root cause is MEASURED, then KILLED (stages 1-2, the headline).**
`vraim synccheck` sweeps 21 axis-angle controller orientations (roll
included) through BOTH pose->rot chains as PURE functions
(frame_context.h - production and the test share the code). Pre-fix
baseline with a canonical 10/10 trim fed identically to both chains: 0.00
deg at identity/pure-yaw poses, ~4-12 on pitch, 10.70/19.85/**28.21 deg at
45/90/180 roll** - roll is where the two algebras differed most, exactly
where eye-tuning never looked. Post-unification (ray + laser adopt the
model's `q_ctrl (x) q_trim` compose via the promoted core/util/xr_math.h;
`ray_pose_from_xr` = `model_pose_from_xr` + roll drop; the laser's origin
basis now angle-built zero-roll, degeneracy bail gone; legacy `aligntrim`
DELETED): canonical max **0.03 deg** (the int-rotator floor) at every pose,
and the live-L divergence reads a CONSTANT 17.70 deg at every orientation =
pure trim-value difference, orientation-independent - the definition of one
algebra. Tuned trims carry over by construction. Fire test exact, laser
echo unchanged, dumps 8->8.

**2. vrrec record+replay (stage 3) - frame-for-frame EXACT.** One tap in
CalcView's tail records what the game consumed (head, the four
funnel poses, the published pad, predictedDisplayTime); replay feeds a
dedicated head lane in the camera gate + sim slots ON the
input_get_hand_pose funnel (ray, model, laser all read one world) + the pad
publish, with recenter state + worldScale serialized in the header and
restored on play. Acceptance: a neutral-stick simhead sweep (7112 frames)
replayed with **712/712 marks bitwise identical** - |dloc| 0.000000 UU,
rot/cam delta 0 units, head pose exact, pad exact - including the swept
segments (20-deg head yaw replayed to the unit). Traps found and settled:
the seam's trailing newline read as a file name (errno 22); fopen_s opens
non-sharable (fresh recordings held by the indexer - _wfsopen _SH_DENYNO);
`vrbody`'s probe moves gameYaw/recenterYaw between record and play, so
comparisons run `vrbody off` or settled (documented in TESTING.md).
`vrrec hand` arms a static funnel pose - the flat record path.

**3. FName index->string (stage 4).** The event scan's discarded FName-ctor
address is now captured (RVA 0x70D660 -> worker 0x70D3C0); capstone
disassembly yielded GNames (TArray<FNameEntry*> Data at RVA 0x13904EC),
the 4096-bucket name hash (0x1370EC0), and the entry layout that matches
the package prior (+0 self-index, +4/+8 the 8-byte flags, +0xC chain,
+0x10 UTF-16 text). `patterns::fname_text()` validates every dereference +
the self-index. Gate: index 0 -> 'None', 1 -> 'ByteProperty' (the canonical
table opening), and the weapon's attach-bone FName (weapon+0xF0) ->
**'Launcher'** (GNames count 54129).

**4. The muzzle ray (stage 5; `vraim muzzle on|off`, DEFAULT OFF pending
your verdict).** `vrbones skel weapon` dumps ANY actor's skeleton WITH
names (the SharedSkeletonData +0xAC FName->index map, layout from the
0x5F6500 lookup disasm, walked in reverse): the shotgun is 3 bones
(SG_Body/SG_Pump/SG_Shell, +X = barrel, NO muzzle bone) - so the ray
derives from the HANDS rig as the plan's on-file alternative: rendered
world barrel = q_target (x) d0 with d0 = normalize(bone44ref - bone43ref)
(the actor frame cancels - derivation in ENGINE_NOTES). d0 is per-weapon
BY CONSTRUCTION (the reference pose IS the per-weapon animation - the
flat-measured shotgun value: the barrel sits **~9-11 deg ABOVE the attach
forward**, the exact misalignment hand-trimmed by eye until now). Flat:
muzzle off ray rot (0, camYaw) -> on (+1971, +41) units = asin(d0.z) to
the unit; fire subs 3/3 with the muzzle ray live; the laser rides the same
d0 XR-side (model trim incl. ROLL - it moves an off-axis vector). Open
flat gap, honestly: a per-weapon d0 CHANGE was not demonstrated flat
(bumpers open the radial, which needs real stick timing; `exec NextWeapon`
FAULTS - SEH caught, negative result logged) - it is structural + on your
checklist.

**5. The idle-sway kill (stage 6; `vrhands swaykill on|off`, DEFAULT ON).**
Measured FIRST: at idle the reference barrel direction breathes **+-1.2
deg** (8.4-11.0 deg band, the muzzle echo as instrument); anchor deltas
peak 3.01 UU / 4.6 deg. The SET-seam premise DISSOLVED on decompilation:
UpdateHandBobAnimationParameters is the WALK bob (channel 2, weight =
velocity/GroundSpeed = zero at standstill) - the idle breathing is the
authored base idle animation, no script property to zero. The kill lives
where the sway enters the VR rig: the drive's reference recapture now
FREEZES unless either wrist anchor moves past 6 UU / 12 deg (2x the
measured envelope) with a 600 ms settle window so post-animation freezes
hold the SETTLED pose. Flat: kill ON = barrel axis bitwise identical 8/8
over 32 s; OFF = wobble back instantly; two fire pulls passed the
threshold and re-froze 3/3 (settled 0.4 deg from the old snapshot). The
weapon's own skeleton (pump/cylinder) animates untouched.

**Promoted-build clean-boot smoke (the stage-6 build):** preset chain in
order, stereo heartbeat clean (mode=1T, presents = 2x builds, guardskips
0), fire test subs exact, crash dumps 8->8 across every boot of the
session, vrpreset.ini + hands.ini untouched (live values loaded and echoed
correctly all session). Off-hand tracking (the stretch) was NOT started -
it stays queued in M9, now unblocked (one algebra + per-weapon identity
both exist).

## Previous state (2026-07-28, session 19 - M8 COMPLETE, v0.2.0 PUBLISHED: HUD on a floating quad, VR bindings, inactive hand hidden, stick pitch killed)

**v0.2.0 IS PUBLISHED** (tag on main at PR #4's merge,
https://github.com/mohamad-balouza/bioshock-vr/releases/tag/v0.2.0) after the
user's TWO in-headset passes: the main run ("I tested and it looks amazing")
and the feedback-round re-verify ("everything passed"). Watch the repo issues
for early-adopter reports. Next session starts at the SESSION 20 PLAN below
(aim-sync algebra + the flat testing framework).

**Branch `s19-m8-completion` (from main at v0.1.0). Everything below is
flat-verified on clean boots with numeric acceptance; the in-headset checklist
below is the open gate, and v0.2.0 publishes only after it + the user's
explicit go. Per the user's call, item 4 of the session-19 plan (aim-sync
algebra unification + the synccheck/record-replay testing framework + FName +
muzzle probe + idle-sway kill) moves WHOLE to session 20 / v0.3.0 - its root
causes and designs are fully written down in the session-19 plan file and the
"SESSION 20 PLAN" block below.**

**1. Hide the inactive hand (`vrhands hideinactive`, DEFAULT ON).** The
inactive hand's whole cluster + sleeve collapse exactly like the driven
sleeve (zero scale, positions pinned at the driven target, cached and
replayed by `reapply()` for the stereo second eye) - EXCEPT the weapon-attach
bone 43, which hides by TRANSLATION to (0,0,-5000) component space: the
attach path inverse-decomposes chain scale (session 16), so zero scale would
be 1/0. On a hand switch the incoming cluster restores from g_ref BEFORE the
rigid write (which sets p/q but never .s). Flat: ghost-arm region diff 2.64
mean/6.9% changed ON-vs-OFF with the drive live, ON-vs-ON2 at the 0.75 noise
floor; both switch directions round-trip (shotgun back at full scale, no
attach blowup); Electro Bolt cast executed with the collapse live (EVE
consumed, FX shell riding the DRIVEN hand); fire test passed; dumps 8->8.

**2. Right-stick pitch is dead under VR gameplay (`vrinput pitchkill`,
DEFAULT ON).** camera.cpp computes the STRICT gameplay-view predicate
(body.cpp's, now public) every CalcView and publishes vrDrove && strict to
the input bridge; compose_over() zeroes the composed right-stick Y while the
gate holds (self-expiring - fails open). Menus/cutscenes keep pitch (strict
false there). Flat: gate ACTIVE (published 16 ms fresh), pitch frozen through
8 s of full-up stick at two different pitch values while YAW still spun;
pitchkill off freed the pitch instantly (853 -> the 18000 clamp); packet
numbers still bump. The wrench-melee question (does it now hit where the hand
points?) is measured on the headset checklist before any body-pitch drive
gets built.

**3. THE HUD IS ON A FLOATING QUAD (M8 "HUD usability" - the release
headline).** The frame-dump fingerprint was re-derived and CORRECTED
(ENGINE_NOTES session 19: the HUD is ~119 NON-INDEXED Draw calls per present
interval on the tonemap target with the scene DSV still BOUND - session 6 had
that inverted; the tonemap is the interval's only no-DSV draw; the world is
pure DrawIndexed; gameswf's batch flush 0x7B8EB5 is in every HUD stack).
`core/gfx/hud_capture.cpp` classifies per interval (scene-RT vote by
DSV-bound DrawIndexed, tonemap = first non-indexed draw sampling the vote
leader, everything non-indexed after it on that target = HUD) and substitutes
our RTV at draw time. gameswf's destination alpha is GARBAGE, so consumers
read an alpha-repaired copy (luminance-derived, `core/gfx/blit.cpp`,
premultiplied semantics). The processed capture feeds (a) a HEAD-LOCKED
XrCompositionLayerQuad during stereo gameplay (distance/width/height sliders,
persisted; +1 layer = 10 of the 16 runtimes must accept) and (b) a
post-eye-capture window composite at all three present exits, so the FLAT
window keeps its HUD while the compositor feed stays clean - which also
fixes the parked "HUD renders in both eyes" defect, and the PAUSE MENU lands
on the readable quad for free. Flat: classification 119/interval with
leaks=0 across every boot; `vrhud force on` removes the HUD from the frame
(4.5% region diff, no blackout) and the composite restores the window
(HUD-corner pixels back, slightly more vivid from the alpha repair); menus
never classify; pause menu redirects + composites; round-trips exact; dumps
8->8. `vrhud on|off|force on|force off|status`; the quad itself renders only
with a session - ITS visual verdict is the headset checklist's headline item.

**4. VR-standard bindings + stick-flick ammo switching (the controls
audit).** Ground truth pulled from the live User.ini XENON_* block
(ENGINE_NOTES): the game's pad layout is A=Use, B=Heal(MedHypo),
X=Reload/Hack/InjectEVE, Y=Jump - confirming the user's complaint exactly
(jump sat on Touch Y). The XR layer now re-routes the face buttons: Touch
A->jump, B->use, Y->heal, X->reload (VR convention per the HL2VR/VRChat/
Pavlov survey: jump belongs on A). DPAD_UP/DOWN were flat-PROVEN to cycle the
equipped weapon's ammo type (00 Buck -> Electric -> Exploding on the
shotgun), so right-stick Y FLICKS (freed by the pitch kill) pulse them:
rising edge past 0.65 pre-deadzone, re-arm inside 0.30, 300 ms cooldown,
suppressed while a grip is held (the radials read the stick). README +
ARCHITECTURE tables rewritten. The remap lives in the XR-session-only path,
so flat coverage is the dpad ground truth + boot smoke (clean); the button
walkthrough is on the headset checklist.

**5. Harness upgrades that paid for themselves the same session.**
`tools/boot.ps1` is now IN THE REPO and save-agnostic: it watches for the
new `[b1r] view state: GAMEPLAY` transition line (the strict predicate,
logged on change) instead of the old wall-save location - verified on five
boots this session, 3-15 presses each. `dumpframe [full] [n]` records n
CONSECUTIVE present windows (`_qN` files) - a command-armed dump always
opens on the same stereo-pair phase, which is exactly why the HUD hid from
single-window dumps for half a session. The frame inspector now also hooks
DrawAuto/Dispatch/CopySubresourceRegion/CopyResource/UpdateSubresource/
ExecuteCommandList (census + events; all zero for this game - no deferred
contexts, which the HUD hunt needed proven).

**Promoted-build clean-boot smoke (remap build):** preset chain in order,
stereo heartbeat clean (mode=1T, presents = 2x builds, guardskips 0), two
right-trigger pulls fired, HUD classifier live with redirects 0 while
ungated, dumps 8->8. The vrpreset.ini gained hudQuadDistM/WidthM/UpM keys
(defaults 1.30/1.25/-0.10 m) - the user's live ini keeps every old key.

### Session 19 part 2 - IN-HEADSET VERDICT + the feedback round (same day)

**THE HEADSET RUN PASSED: "I tested and it looks amazing... there's just some
small fixes."** No regression (item 1 perfect); HUD panel "very good...
amazing"; ghost hands "perfect... exactly as intended"; monitor perfect; and
THE WRENCH WORKS with the pitch kill alone - the HMD-pitch body drive is NOT
needed (dropped from session 20). Four fixes came back and ALL FOUR ARE DONE
FLAT the same night:

**1. Menus were transparent on the quad -> gameswf's blend states accumulate
GARBAGE COVERAGE in the alpha channel** (they blend alpha like color). Every
blend state seen on a redirected draw now gets a cached variant with the
alpha ops corrected to over-composite (ONE / INV_SRC_ALPHA - rgb ops
untouched), so true coverage accumulates and the luminance repair dropped to
a 0.35 safety floor. Flat: the pause menu's cityscape panels render SOLID
black with the yellow glow correct - the original art.

**2. The odometer digit strips sprawled unclipped (user's screenshot) ->
flash MASKS are STENCIL-based and the redirect bound no DSV.** The capture
RT now owns a D24S8 depth-stencil, bound with every substitution and cleared
per interval. Flat: the pause menu's money (0451) and ADAM (2580) counters
render CLIPPED to their windows - strips gone.

**3. Buttons revised by feel: Touch A goes BACK to use/loot/menu-confirm
(it is the game's confirm button - looting and menus were unintuitive
remapped), jump moves to B.** X reload / Y heal stay.

**4. Ammo select redesigned - three types, three directions: HOLD the
right-stick CLICK, then push UP / DOWN / LEFT to select that slot (the dpad
directions each SELECT a slot, they do not cycle - which is why up/down
flicks could only reach two). Turning is suppressed while the click is held;
a QUICK click-tap with no push still zooms** (the click's original owner -
the tap is just delayed past the 400 ms select window). Flick-without-click
is gone entirely, which also removes any accidental-flick worry.

**FPS question measured flat: the whole HUD capture costs ~4% of an
UNCAPPED flat frame (336 vs 349 presents/s = ~0.11 ms/frame) - under 1% of
a headset frame budget.** The perceived hit is most likely the zone or the
Debug build; `vrhud off` is the live in-headset A/B if it recurs. A
per-present resource-desc cache also removed most of the classifier's COM
churn this round. Queued to M9 by the user: the WRENCH SWING GESTURE
(velocity-triggered melee - they play-tested the timing manually and it
felt right).

**5. (part 3, same night) The weapon wheel was unselectable up/down - the
pitch kill was eating stick Y in the radial state too.** The kill now LIFTS
while a grip/bumper is held (the wheel reads stick Y), and because the
wheel's binding state keeps the look axis bound, camera.cpp snapshots the PC
pitch at bumper-down and writes it back at release - the wheel selects, and
wheel-time look drift cannot stick. Flat, exact: pitch frozen at 853 under
full-up stick with no bumper; RB held -> pitch moved to 4001 (and the
calls/s dropped = the wheel actually opened); release -> pitch restored to
EXACTLY 853.

**6. (part 3) ZOOM IS REMOVED in VR (user's call - nothing needs it and an
HMD FOV zoom is a comfort hazard).** RS-click never reaches the game; the
click is purely the ammo modifier now (the tap-vs-hold logic is gone with
it). The `vrinput test press RS` lane still injects raw XInput for harness
use.

**RE-VERIFY IN-HEADSET (quick, 6 items):** (1) pause menu + a vending
machine: opaque panels, counters clipped; (2) A loots/confirms menus, B
jumps; (3) ammo: hold right-stick click + up/down/left selects the right
slot each time, no selects while turning normally, clicking alone does
NOTHING (zoom gone); (4) WEAPON WHEEL: hold grip, select with the stick
incl. up/down, release - and the view pitch must be exactly where you left
it; (5) FPS feel vs `vrhud off` in the same spot; (6) nothing else moved.

## Previous state (2026-07-27, session 18 - M8 QUICK PHASE CODE-COMPLETE FLAT: both release blockers fixed, per-hand offsets, grip-switch fix, release zip staged)

**Branch `m8-release-quick-phase` (from main at PR #2's merge). Everything below
is flat-verified on clean boots with numeric acceptance; the in-headset run is
the open gate, and the GitHub release publishes only after it + the user's
explicit go.**

**1. Per-hand model offsets (the user's ask, shipped first).** The viewmodel's
six position/rotation offsets are now PER HAND (0 = left/plasmid, 1 =
right/weapon, same convention as `vraim cal`): `vrhands pos [l|r] <f> <r> <u>`
and `vrhands rot [l|r] <p> <y> <r>` - no side = BOTH hands, so the harness and
old scripts are untouched; the overlay grew a "Tuning hand: L / R" selector
above the existing six sliders (not twelve sliders); hands.ini persists
per-hand keys (`posFwdCmL/R`, ...) with the legacy suffix-less key loading to
both; and **`vrpreset save` now also writes hands.ini**, so the one in-headset
save button covers the model sliders. Flat proof, all exact: with the right
hand driving, `pos r 40 0 0` moved the written loc +40.0 UU along fwd, a LEFT
offset left it bit-identical at baseline; with `hand l` the left offset
engaged (+40.0); `rot r 0 30 0` moved the written yaw by exactly 5461 units;
both ini formats round-trip. The M7.5 invariant is untouched by construction -
the change only swaps WHICH atomic feeds the existing trim/offset math,
nothing in the camera/recenter path was edited.

**2. Headset-disconnect stall (M8 blocker a) - FIXED flat.** When the headset
idles, the session leaves FOCUSED and per-present xrWaitFrame blocks for
seconds (<1 fps flat). The new pace guard (openxr_runtime.cpp) skips the
blocking pacing once a session that HAS been FOCUSED leaves that state.
Bring-up is exempt - SYNCHRONIZED -> VISIBLE -> FOCUSED needs submitted frames
to advance, a naive not-FOCUSED gate would deadlock session start - and a 5 s
keepalive still paces one real frame in case VDXR wants frames before
re-granting FOCUSED (event recovery is primary: pump_events runs every present
regardless). `vrpace on|off|simidle on|off|status`; wait durations >1 s now
log with the session state. Flat acceptance via the simulated idle (state
forced VISIBLE, 1 s sleep in place of the blocked wait): guard ON held
378-396 presents/s with one keepalive hitch per 5 s; guard OFF reproduced the
field stall exactly (presents=1/s); re-enabling recovered to 416/s. Known
cosmetic: each keepalive block trips the reentry watchdog's detect-only line.

**3. Flat-screen mirror (M8 blocker b) - FIXED flat, and the session-17 clue
is EXPLAINED.** The window now always displays the LEFT eye: left-tagged
presents snapshot the backbuffer (read-only), right-tagged presents re-blit
the held image AFTER the right eye's XR capture, so the compositor feed is
untouched; the pin also runs on presents with no open XR frame (pace-guard
skips / no session - the game still presents alternating eyes there).
`vrmirror on|off|status`. Why all 12 session-17 shots were one phase: the
pair's presents have wildly unequal display time (L visible only while the R
frame builds, ~1-3 ms; R through the next blocking wait, ~8+ ms), so
DWM-sourced captures land on R with high probability - duty-cycle skew, not
absent alternation (ENGINE_NOTES session 18). Acceptance matches that model:
within-condition consecutive shots sit at the 0.31-0.73 floor for BOTH mirror
on and off, while the on-vs-off cross-diff is 13.6-13.9 (the near-field eye
offset at worldScale 100) - the pin flips which eye the window shows. Holds ==
blits exactly (74774 each); stereo heartbeat unaffected (presents = 2x builds,
guardskips 0).

**4. Grip-switch wrong-controller bug - ROOT-CAUSED AND FIXED flat (the
ROADMAP theory was right).** Grips compose to the bumpers, a bumper press
switches the raised hand with no trigger event, and `active_hand()`'s auto
latch learned only from triggers - so the model+laser stayed on the stale
controller until the next pull (= the predicted self-correction). The latch
now learns from the composed bumpers too (LB -> left/plasmid, RB ->
right/weapon; triggers checked second so a same-frame fire wins). Live flat
repro then fix-proof on a clean boot: two right pulls -> status `auto(R)`; LB
alone raised Electro Bolt and (pre-fix) left the latch RIGHT; post-fix LB
alone flips to `auto(L)`, RB back to `auto(R)`, no triggers. Fire direction
was never affected (seam attribution is structural). Status now prints the
latched hand: `hand=auto(L|R)`.

**5. Release prep (task 4) - staged, NOT published.** README rewritten
(install incl. itsloopyo conflict + VDXR setup, VR PRESET 1 flow, tuning incl.
per-hand offsets, mirror/pace defaults; overlay key verified F10). Release zip
built from the RelWithDebInfo build (both DLLs + README.txt) at the session-18
scratchpad as `bioshock-vr-v0.1.0.zip`. Tag + GitHub release wait for the
in-headset verdict and the user's explicit go.

**Promoted-build clean-boot smoke (grip-fix build):** fire test 59->53 for 6
firing pulls with an LB/RB plasmid round-trip in between, fresh decal, dumps
8->8 all session, vrstereo heartbeat clean (mode=1T, presents = 2x builds,
guardskips 0). Test-artifact inis (hands.ini with zeros, vrpreset.ini with
defaults) deleted after the run per the session-17 trap - code owns defaults.

### Session 18 part 2 (same day): aim-ray origin offsets + the crosshair kill

**6. Aim-ray ORIGIN offsets (user ask: "the aim is the thing that is
misaligned, not the model").** New per-hand `vraim pos [l|r] <fwd> <right>
<up>` in cm (no side = both), an overlay "Ray offset hand: L / R" selector
with three sliders under the existing trim sliders, persisted in vrpreset.ini
(aimPosL*/aimPosR*). Applied ONCE at ray build (aim.cpp) along the FINAL
(trimmed) ray's zero-roll basis, so the laser, the fire-origin substitution,
and everything else reading the ray move together by construction - the
model deliberately does NOT take them (its own sliders stay, and the two are
tuned against each other). The laser applies the identical cm offset XR-side
in meters with the same basis convention. Flat-proven exact under vrstereo:
`pos r 0 60 0` moved the R ray origin by (-0.7, +60.0, 0.0) UU with the L ray
bit-identical; `pos l 0 0 45` moved L +45.0 UU up; two fire-seam
substitutions carried the offset ray (subs=2); save -> zero -> preset apply
restored both hands' values from the ini.

**7. The flat crosshair is GONE by default (user ask), and the lever is a
one-line engine property.** `ShockPlayer.bReticleDisabled` makes the game's
own RenderReticle push "NoReticle" to the flash HUD every frame - found by
reading the SCRIPT SOURCE embedded in ShockGame.U (the reticle functions are
in the 40% UELib cannot decompile; the source text can, ENGINE_NOTES session
18 part 2). It is written through **the engine's own console SET handler via
the exec seam** - `set ShockPlayer bReticleDisabled True` -> HANDLED - which
is the session's biggest reusable find: ANY script property is now writable
by name, no offsets, no reflection (SET also writes the class default, so
load-crossing pawns inherit it). Shipped as `vrxhair on|off|status` +
overlay checkbox ("Flat-screen crosshair", VR camera section), DEFAULT
HIDDEN, re-asserted every 15 s, `crosshairVisible` persisted in vrpreset.ini.
Flat: clean boot hides it automatically (19 -> 0 bright pixels in the center
crop, screenshot-verified), `vrxhair on` restores those exact pixels, off
kills them again, dumps 8->8.

## Previous state (2026-07-27, session 17 - M7.5 SHIPS AND IS IN-HEADSET VERIFIED; DEADZONE 23 DEG BY USER CALIBRATION)

**IN-HEADSET VERDICT (same day): "This is perfect... the stick was working as
expected, the models didn't move when I moved my head... so just needed that
deadzone change and it was perfect."** The user tuned `vrbody deadzone` to
**23 deg** live and asked for it as the default; it now ships that way. All
three reported symptoms are addressed and the hard invariant survived the
headset: the models do not move with the head.

**Why the deadzone was the last piece.** The user's one remaining note was
"the gun moves with the camera a bit". Inside the 23 deg band the body does
not steer at all, so an ordinary glance leaves the viewmodel completely
world-locked; only a deliberate turn past the band carries the body along. And
because the transfer takes `residual - band`, beyond the band the body trails
the head by exactly 23 deg, so head and body never diverge further than that
no matter how far you turn. Flat-confirmed on the shipping default: `resid`
settles at exactly 23.00 deg, and the invariant is untouched by the band -
gameYaw and recenterYaw each moved 0.38398 rad (22 deg = 45 - 23) while
`camYaw` stayed 8308 and the hand's world yaw stayed 116, both bit-identical.

**Branch `m7.5-body-yaw-transfer`, ready to merge.** Everything below is
flat-verified and now headset-verified.

**Harness note worth keeping: a `vrpreset.ini` written by a smoke test will
silently OVERRIDE a later code default.** The session-16-era ini on this box
pinned `bodyDeadzoneDeg=0.0` and would have cancelled the new 23 deg default
on the next preset press. It contained nothing but shipped defaults, so it was
deleted and code owns the values again. Any future default change needs the
same check.

**The root cause the user found by playing is fixed. The body facing is the
PlayerController's `Rotation.Yaw` at `PC+0x1E8`** - found and proven live in
one boot, nine probe steps, all positive (ENGINE_NOTES session 17): it carries
a non-zero pitch where the pawn's is 0, an additive yaw write LANDS and HOLDS
across samples, the pawn follows for free (so a PC-only write is enough), the
engine's own turn COMPOSES on our value rather than snapping, and a synthetic
walk burst followed the written facing to **0.4 deg**. That last step proved
the whole milestone before a line of transfer code ran.

**THE HARD INVARIANT HELD EXACTLY - not "within tolerance".** The camera and
both halves of the controller-to-world mapping depend only on the composite
`gameYaw - recenterYaw`, so the transfer adds T to the body and exactly T to
the recenter reference. Measured: the composite read **1.27742 rad at every
head angle** (0/30/45/90/-45) in both states; at head 45 the hand's world pose
(`rot.yaw=13323`) and the camera (`camYaw=21516`) were **bit-identical** off vs
on while gameYaw and recenterYaw each moved +0.78540 rad; `[tlm] yawstep`
showed **max=0 units, nbig=0** through the arm transient at ~1650 frames/s.
The yaw path was converted to integer rotator units so the cancellation is
exact rather than approximate, and `body::on_calcview` returns the units
COMMITTED (never the units requested), so the two absorbed quantities are the
same integer and cannot drift apart.

**The feature, as numbers.** Walk direction with the transfer OFF: 116.49 and
116.67 deg at two head angles **90 deg apart** - it tracked the body (118.19)
and ignored the head, which is the reported defect measured. With it ON: at
head +45, walk 163.19 / body 163.19 / camera 163.19; at head -45, walk 73.20 /
body 73.19 / camera 73.19. Walk == body == camera to 0.01 deg, the pair
spanning 89.99 deg for a 90 deg head change.

**A second payoff nobody planned: the render lock's correction goes FLAT under
head-look.** Parked hand, +-30 deg head sweep - with the transfer off the
lateral correction swings **10.5 UU** (it scales with the camera-vs-actor
split); with it on it is **flat within 0.46 UU**, and it lands on `lat` 4.58
against the 4.57 that the calibrated, headset-verified zero-split
configuration uses. So the transfer restores the session-16 regime at *every*
head angle instead of only at head 0. Because the lock runs at gain 0.9, a
swinging correction leaves a swinging residual (the "gun drifts as you look
around" percept) while a constant one leaves a constant, trimmable offset.

**Be ready for this in the headset: the viewmodel WILL render differently with
`vrbody on` vs `off`, and that is the mechanism working.** At head 45 the
world region of the frame is pixel-identical between the two (mean abs diff
0.048 against a 0.3 floor - the camera really is unchanged) while the gun+arm
region reads 23.4, and looking at the crops the gun is not merely shifted but
**viewed from a different angle**. The renderer orients the foreground rig by
the ACTOR fields, so with the transfer on the rig is viewed from the camera's
own orientation instead of from a 45-deg-stale body facing. The lock corrects
position, never viewing angle - which is why it could never fully paper this
over.

**Shipped defaults: `vrbody` ON, armed by VR PRESET 1 (after the viewmodel,
before vrstereo). `vrbody off` is the one-command live A/B against the
session-16 build.** Knobs `vrbody rate` (0 = instant 1:1, the user's call) and
`vrbody deadzone` (0) persist to vrpreset.ini. Safety: a probe handshake
transfers 1.1 deg and verifies the body actually moved before scaling up,
undoing itself and hard-disabling after three failures (worst-case visible
error ~1.1 deg for one frame); a gameplay-view guard that deliberately drops
aim.cpp's menu escape hatch; PC-pointer and discontinuity resets; a
once-per-present gate.

**Clean-boot acceptance (promoted build, under vrstereo):** fire test 57->51
for 6 pulls with the transfer live, dumps 8->8, stereo heartbeat clean
(mode=1T, presents 336/s = 2x build 168/s, guardskips 0), preset echo chain in
order, ini round-trip, and a 20-step alternating +-90 deg soak that returned
camYaw to its exact starting value with the recenter back to 0.0000 deg.

**One thing did NOT get measured and is honestly open: the exact cull angle.**
The 0/30/60/80/90/100/120 deg sweep needs a template-free instrument - NCC
template chaining broke down across the composition changes (correlations
0.56-0.79 at every step, so every pixel number from it is void; recorded in
ENGINE_NOTES so it is not retried blind). The sweep did expose that the two
physical cases differ in sign: a head-only glance with the hand parked in the
world *increases* hand-vs-body, while the reported case (the user swivels, so
head and hand rotate together) drives it to ~0.

## Previous state (2026-07-27, session 16 part 3 - WORLDSCALE 100 BY USER CALIBRATION; VR PRESET 1 SHIPS)

**The size problem dissolved without touching the mesh: the user found that
worldScale 100 makes the viewmodel read PERFECT in-headset - size AND
distance ("exactly on my controller and with the right size") - because at
100 the drawn angular size finally agrees with the stereo distance (at 50
the 2x-oversized mesh read "too close"; familiar-size vs stereo conflict).
The world reads ~half size in trade; the user judged it acceptable.
worldScale now DEFAULTS 100.** The engine-lever scale hunt from part 2 is
retired; the proper world/viewmodel scale SPLIT (viewmodel's own stereo
basis via per-eye bone offsets) is parked in M9 with a design sketch.

**Shipped this part (all flat-smoke-tested on a clean boot):**
- **Head-anchor offset sliders** (up/fwd UU, "VR camera" section; defaults 0
  by the user's call in part 4 - tuned by eye, persisted via the preset) -
  the "head very wrong at 100" fix; applied inside the VR/simhead drive,
  image-verified flat.
- **Per-hand aim trims** (`vraim cal [l|r] <pitch> [yaw]` + four overlay
  sliders, R weapon / L plasmid) - the laser, fire ray, and viewmodel all
  read the same per-hand values.
- **VR PRESET 1** (overlay button + `vrpreset` command): one press arms the
  user's full configuration in a safe order - VR enable, camera mode, SR
  pair pacing, vrinput, game-FOV write, controller aim + AIM pose + hand
  origin + laser, viewmodel + align-aim, then vrstereo last. `vrpreset
  save` / "Save preset values" persists the tuned sliders (worldScale, head
  offsets, IPD, gameFov, per-hand trims) to vrpreset.ini; the preset loads
  them on apply. All echoes + ini round-trip + fire test verified flat
  (dumps 8, clean).

## Current state (2026-07-27, session 16 - OPTION 2 COMPLETE FLAT: MATCHED LENS SHIPS ON, FULL ACCEPTANCE AT k=1)

**IN-HEADSET VERDICT (same day, session 16 part 2): THE CORE WORKS. The
user: "now it's fully working, and it's not moving with the head/headset/
camera anymore!! Which is amazing progress!"** The model follows the
controller and head-look is decoupled - the defect that survived sessions
12-15 is gone in the headset. Remaining from the same run: (1) SCALE - "the
left hand is like as big as my head and the weapon is like as big as my
torso + head". Three levers were probed flat the same night and ALL THREE
ARE DEAD (ENGINE_NOTES session 16 part 2): cluster bone .s blows the
attached weapon up near-plane (the attach path inverts chain scale - even
with the attach helper excluded), and rig-actor DrawScale is geometry-inert
on the fg path (positions round-trip, meshes don't scale). Scale needs the
attach/fg render-path disasm or the vm_draw lane - THE top next-session
task; no knob ships. (2) Weapon-laser fine alignment via the existing
pos/trim sliders - after scale. (3) A laser-crossing anomaly at large
right-aim angles (laser drifts from the right side of the weapon to its
left side) - discriminating questions + live A/Bs issued to the user.

**Harness note that cost a false alarm: with the Virtual Desktop / Quest
menu OPEN the XR session drops FOCUSED -> VISIBLE and the runtime stops
delivering controller poses - the drive receives an identity pose and the
model parks dead-center.** It looks exactly like "the viewmodel is stuck
and ignores the controller". Close the overlay and it resumes. (Log
signature: `xr: session state VISIBLE`, hands target rot pinned to
(0, camera-yaw, 0).)

**The decision hour picked the good branch: the driven rigid path's pull at
the matched lens calibrated to +11.5 UU (not the vanilla path's 65 - the
driven path's eye offset is NOT fov-coupled), two independent instruments
agreeing within ~1 UU. The whole session-14 acceptance ladder then passed at
k=1 on a clean boot with the SHIPPING DEFAULTS: `vrfgfov` ON, `vrbones
lockpull` 12.8 (= 11.5 physical through the 0.9 depth gain).** The rig now
renders through the WORLD lens - correct internal perspective, full
arm+sweater composition - at world-correct size, depth, and parallax.
`vrfgfov off` restores the session-14 narrow-lens configuration for A/B.

**A real defect was found and fixed on the way (ENGINE_NOTES session 16):
the simhead sweep - the one instrument zero-split calibration can't cover -
showed the gun over-shifting the world under head-split by exactly
pull*sin(split)*gain on both axes. The renderer's eye offset does NOT swing
with the camera-vs-actor split: the matched path now rotates the pull by the
constant view bias only (identical at zero split; the unmatched path keeps
its verified qd rotation). Post-fix: gun world-glued within 2-17 px across
+-30 yaw / +-20 pitch.**

**Clean-boot acceptance numbers (shipping defaults, all under vrstereo)**:
far-range offset parallax -156 px vs -159 world-correct (0.98x); size ratio
0.475-0.489 vs 0.465 (1.02-1.05x); simhead sweep glued (above); fire test 6
pulls ammo 59->53 with a fresh wall decal, dumps 8->8; stereo heartbeat
clean throughout (1T, presents = 2x builds, guardskips 0). **The in-headset
verdict is the only open gate (checklist below).** One known edge for it:
a hand closer than ~23 cm real puts the corrected cluster behind the world
camera and the engine culls the rig (the session-15 constraint at the new
11.5 threshold; probe it in the checklist).

## Previous state (2026-07-27, session 15 - STRATEGY PIVOT: THE FG FOV FIELD FOUND; THE DOLLY IS THE LAST WALL)

**The user called the counter-modeling approach dead after three sessions and
pivoted to patching the foreground pipeline at its source. The pivot paid
half-out immediately: the fg pass's FOV is a live PLAYERCONTROLLER FIELD
(+0x460), consumed by the renderer every frame - one write re-lenses the
whole rig to the WORLD lens, dump-proven across every cb tier.** Shipped as
`vrfgfov on|off` (per-frame write of the world-equivalent 4:3 spec,
2*atan(tan(worldFov/2)*3/4) - 101.5 at option 117, the exact value session
13 held at the wrong address: pawn+0x558 is EYE HEIGHT, not fg fov). Through
the honest lens the rig shows correct internal perspective and the full
arm+sweater composition - the telephoto ceiling of every bone-space counter
is gone while it is on.

**What stopped the full completion tonight, both flat-proven (ENGINE_NOTES
session 15):** (1) the fg eye DOLLIES BACK by a fov-coupled amount (~13 UU
at the 60-deg lens = session 14's calibration; ~65 UU at the matched lens,
vanilla path) whose source field was not found (every stored representation
scanned; the promising candidates poked inert); (2) bones CANNOT counter the
dolly at the matched lens - the correction places the cluster behind the
world camera and the engine culls the whole rig; (3) the DRIVEN rigid path
renders a different pull than the vanilla path (the parked fist rendered
near/huge where vanilla math said 65-deep) - the drive-on pull at the
matched lens is UNCALIBRATED.

**Shipped defaults are SAFE: `vrfgfov` is OFF, and the default build is
regression-verified byte-identical in behavior to session 14's fully
acceptance-passed configuration** (tlm reproduces k=2.12 wNat=30.4 w*=36.8
depth=+6.43 under vrstereo; fire test clean; dumps 8). All the matched-lens
plumbing (live invTan scales, k=1 collapse, `vrbones lockpull`) sits behind
the toggle, plus new instruments: `fgstack` (cb writer callstacks - already
harvested: 0x7C3044 <- 0x789D92/DF7 <- 0x7661B3 <- 0x77DC1E <- 0x60ECCA <-
0x3DBF7C/0x3EDCBF), `fsweep` (live float-field sweep), `fginfo` (PC/pawn
pointers).

## Previous state (2026-07-27, session 14 - THE DEPTH FIX: SIZE, STEREO DEPTH, PARALLAX WORLD-CORRECT FLAT)

**Session 13's depth-geometry defect (the "hands are a HUD on my face, but a
bit 3d" percept) is fixed and flat-verified IN STEREO with numeric acceptance;
the in-headset verdict is the only open gate (checklist below).** The render
lock's third constraint is now **w\* = k \* trueDistance** (k = the world/fg
lens ratio, ~2.12 at FOV 117): apparent size, stereo disparity, and
translation parallax are all the same (1/w)\*k geometry, so the one constraint
made all three world-correct at once. Measured on the wall save, vrstereo on,
baselines replicated first on the same harness: camera-offset parallax 420 px
(1.23x world-correct) -> **355 px vs 341 world-correct (1.04x)**; gun size on
hand-distance doubling 0.605 -> **0.465-0.470 vs 0.465 exact**; fg depth band
clean up to wSolve ~142 (FOV 137 + hand extended - the clamp fallback never
fired); simhead +-30/+-20 sweep still glued (no rotation regression); fire
test 57->55 with a fresh decal, dumps 8->8.

**Two model corrections were forced by measurement on the way** (full chain in
ENGINE_NOTES "Foreground scene FOV", session-14 block): (1) the fg eye RIDES
THE CAMERA, translation included - dump-proven (`offset 0 30 0` moved the
matrix-recovered eye 29.7 UU); the model now composes the eye from the live
camera position. (2) The dump-recovered E is SECTION-FRAME-RELATIVE - useless
as an absolute pull-back; the TRUE pull-back (13 UU behind the camera,
`kFgEyeFwdBehindCam`) was calibrated from three independent physical baselines
that agree to ~1 UU. With the model's depth scale made real, session 13's
"rigid-rebake doubling" decomposed into (model-scale error 1.63) x (true
rebake ~1.1), so BOTH gains now default 0.9: `vrbones lockgain` (lateral) and
the new `vrbones lockdgain` (depth), live knobs + overlay sliders.

**Instrument findings this session** (recorded in ENGINE_NOTES): `camrot` does
not rotate the fg view while the drive is on (the rigid path orients by the
actor view) - simhead is the only valid flat head-look stand-in; window
captures are eye-phase-locked, so stereo disparity is unmeasurable from
screenshots (size + offset-parallax carry the same geometry and are the
acceptance instruments); with lock ABS live the gun sits pixel-frozen across
a shot series where lock-off wobbled +-16 px (the sway may be partially
absorbed by the per-frame re-pin now).

## Previous state (2026-07-26, session 13 - THE CAMERA-COUPLED RIG TERM: ROOT-CAUSED AND COUNTERED FLAT)

**Session 12's single blocker is identified end to end and countered; the flat
simhead sweep now holds the gun on the world within a few degrees through
+-30 deg of head yaw and +-20 of pitch (was 15-25 deg of coupling), and the
fire test + stereo smoke passed on the shipped build.** The renderer draws the
first-person rig as a separate FOREGROUND scene: fixed 60-deg 4:3 projection
(tanH 0.7698 at 16:9 - dump-proven constant while the world tracks the FOV
option exactly), a view whose eye is parked ~32 UU BEHIND the rig origin in
ACTOR space (the "translation-about-a-pivot" the user predicted), the engine's
hand sway on top, and - found the hard way - a rigid per-section path that
REBUILDS section matrices from the very bones our drive writes. Full chain in
ENGINE_NOTES "Foreground scene FOV"; every constant in patterns.h.

**The counter (bones.cpp "render lock", default ON)**: an analytic model of
that foreground transform, solved per frame as a 3x3 so the anchor renders at
the world-correct pixel, applied at gain 0.5 (the rigid-section rebake doubles
any bone move on screen - flat-measured). `vrbones lock abs|diff|off` +
`lockgain <f>`: abs (default) also drops the authored raised/too-close
composition onto the true controller spot; diff cancels only the head-split
term. Residual floor = the vanilla hand sway (~+-3.5 deg breathing, x2.1 lens
gain) - killing it at its source (UpdateHandValues bob params) is queued.
The pawn's ForegroundFovAngle property group (+0x550/554/558) was found and
proven a NON-lever (the renderer ignores per-frame writes to it; the engine
lerps it back - dead end recorded). frame_inspector gained a generic
Map/Unmap cb watch (kept as a diagnostic; captures cannot feed the solve -
feedback). Gun SIZE is unchanged (still narrow-lens large) - the DrawScale
lever stays queued.

**Flat evidence (all in the session scratchpad)**: 2-shot FOV discriminator
(world rescales with gfov 100->137, gun holds - the projection split), dump
matrix decode at h0/h30 (world-correct at center, 14-deg anchor error at 30
head-yaw), 12-dump eye-offset recovery E=(-32.1,-5.6,-0.9)+-0.5, gain sweep
(1.0 doubles, 0.5 lands), final g5 series: anchor within 2-4 deg of world-true
at simhead -30/0/+30 yaw and -20 pitch, fire test 57->54 ammo with fresh decal
and dumps 8->8, vrstereo on smoke + fire clean.

## Previous state (2026-07-26, session 12 - M7-v2 BONE DRIVE BUILT + FLAT-VERIFIED)

**The viewmodel now follows the controller at the BONE level, and every flat
check passed on the first build: the gun rotates about the GRIP on all three
axes (the actor-pinning lever arm is gone), the fire test passed with the
drive live, and the plasmid PARITY test passed - Electro Bolt's electricity
rides the driven hand.** `vrhands mode bones` is the new default; the actor is
no longer written (it stays engine-placed at the eye anchor - no culling risk,
sane FX anchoring). Awaiting the user's in-headset verdict (checklist below).

**FIRST HEADSET RUN + THE FIX (same day, session 12 part 2).** The user's
verdict: a metric ton better - the model rotates and moves correctly with the
controllers - but it still FOLLOWED THE HEAD and sat too close to the eyes.
Root cause found flat and fixed: **the renderer orients the first-person rig
by the RENDER CAMERA's rotation, not the actor's rotation field**. Flat those
are the same value, so every flat test passed while the HMD head-look was
exactly the split between them, leaking head rotation into the hand placement.
The composition now inverts the FINAL camera rotation from the frame context
(anchored at the actor's location field, which camera-position moves proved
the renderer does use). Proven with the new `camrot <p> <y> <r>` seam command
(render-camera-only rotation offset = the flat stand-in for head-look; it
splits camera from pawn exactly like a headset does): camera-only pitch moves
the world while the hand stays world-anchored, camera-only yaw keeps the hand
on its target instead of panning away. Fire test re-passed on the fixed build.
Lesson recorded the hard way: an earlier "discriminator" used a `rot` command
THAT DOES NOT EXIST - no echo, no effect, and the conclusion drawn from it was
noise. Always check the command echo before trusting an A/B. Second in-headset
verdict pending.

**How it works** (bones.cpp + ENGINE_NOTES "Skeleton / bone internals", all
offsets in patterns.h): the AHands actor carries a `SkeletonInstance` at +0x3FC
whose bone array is 47 component-space hkQsTransforms (pos+quat+scale, 48 B).
The engine re-evaluates it lazily behind a dirty flag; our write runs in the
CalcView detour after the engine tick, then clears the dirty flag so the
renderer keeps our pose. The RIGHT hand cluster (wrist 27, fingers 28-42,
weapon-attach bone 43 - the equipped gun renders from that entry, live-proven)
and the LEFT cluster (wrist 6, fingers 7-21) each move RIGIDLY to their
controller's pose: rotate the engine's reference pose about the anchor bone,
translate the anchor onto the target. No IK. Sleeve bones ({24,25,26,45,46} /
{3,4,5,22,23}) collapse to zero scale to hide the arms (overlay toggle,
default ON). The reference self-refreshes whenever the engine re-evaluates,
so engine animations ride along when present and nothing feeds back when not.

**Flat evidence (2026-07-26, wall save, all screenshots in the session
scratchpad):** simpose 0/0/0 put the gun+fist at the synthetic controller spot
pointing forward; yaw+30 / pitch-25 / roll+45 each rotated gun+hand about the
grip with the fist planted (roll rolls about the BARREL - the wrist-roll spec
item); fire test: 4 shots, subs=4, ammo 57->53, fresh decal well off the
head-crosshair, dumps 8->8, game alive; left hand: cluster measured with
Electro Bolt raised, drive + collapse verified, FX parity A/B (drive on = the
electric shell at the driven spot; off = snaps back to the engine pose), and a
live cast fired through the anim-notify chain (ability InitiateDamage called,
EVE consumed, wall scorch) with the drive running.

**Bonus finds recorded in ENGINE_NOTES/patterns.h:** the REAL DrawScale
(actor +0x2AC + the dirty protocol the session-11 pokes were missing -
untested live, the likely engine-side gun-size lever), the attach-bone FName
on the attached actor (+0xF0), SetBase = actor vtable +0x1A0, skeleton freeze
flag (+0x20, `vrbones freeze`), and the weapon attach being EQUIP-TIME only
(Hands.uc: OnEquippingStarted -> AttachToBone; nothing re-asserts per tick).

**Session-12 method note:** the ROADMAP's render-side steps 1-2 were never
needed - the native-table impl walks (GetBoneCoords -> the instance chain)
plus one hexdump/poke session reached the bones directly, before any code was
written. The render-side vm_draw design (Map/Unmap CB patching) stays fully
specified in the session-12 plan file as the fallback that never fired; the
enlarged `dumpframe full` capture (256 -> 1344 B) shipped anyway.

## Previous state (2026-07-26, session 11 close - M7 REPLANNED with the user)

**Two in-headset runs and a long design review retired the actor-pinning
approach. M7 restarts at the draw/bone level next session.** The shipped build
works mechanically and is stable, but it cannot reach the goal, and the user's
verdict after the second run was blunt and correct: "still wrong... the model is
going faster than the aim", and pulling the offsets "doesn't matter... instead
it creates other problems".

**What the user actually wants (their words, this is the spec):** the weapon and
the plasmid hand each move as ONE with their own controller, like a native VR
game. Right controller = weapon (hide the arm, hand optional). Left controller =
plasmid hand (keep the hand, arm can go). Wrist roll must match a real wrist.
Explicitly NOT wanted: bent arms, elbows, IK, two-handed grips. "I don't need it
to be perfect - I just want it in sync with the controller." And a hard
requirement: **no shipping a working weapon with a broken plasmid hand** - they
are one deliverable.

**Why actor pinning cannot get there** (three structural walls, all live-proven,
details in ENGINE_NOTES "Viewmodel / AHands"):
1. The actor's pivot is the EYE anchor with the mesh a metre out, so every
   rotation swings the gun on a lever. Correcting it with position offsets is
   impossible - the engine culls the rig once the origin passes behind the
   camera.
2. **Both arms are ONE skinned mesh on ONE actor**, so a single transform can
   never decouple left from right - which the user's spec requires.
3. The equipped weapon renders from its ATTACHMENT matrix and ignores its own
   actor fields, so the ideal pivot (the weapon actor sits exactly at the gun)
   is unreachable that way.

**What today's inspection tests established, and they are all encouraging:**
- The rig renders as a normal WORLD-SPACE object - the camera orbits it and it
  holds still, so nothing structural blocks inspecting it from any angle.
- **The geometry is complete on every side** - full right side, muzzle,
  cylinder, correctly gripping hand. The art will survive close VR inspection.
- Both arms are always in the mesh; BioShock 1 just shows one at a time.
- Dual-wield is a state-machine change, not a rendering one. **The user dropped
  it** - it moves to the BioShock 2 adapter (M10), which supports it natively.

**The load-bearing design principle for the rebuild - where you write decides
whether effects follow.** Engine-side writes (actor, bones) are read by the
engine, so attachments and particle effects are recomputed and follow for free.
Render-side writes (patching a matrix at draw time) are invisible to the engine,
so separately-drawn effects stay behind. That is why the plan prefers BONE
writes and reserves render-side patching for what has no engine handle (scale -
no DrawScale field was ever found - and the viewmodel's projection).

**The plan, four steps, in ROADMAP "M7-v2"**: find the viewmodel's draw calls
(the fingerprint is that we write the rig's transform ourselves) -> prove we can
skip a draw and substitute a matrix -> reach the bone matrices -> write the hand
bones from the controllers with the forearm bones collapsed. Because the user
dropped arm articulation, step 4 is a direct bone write with **no IK solver at
all**, which is what makes this tractable.

**THE test to run early, not late: do the plasmid's hand FX follow?** Unknown
whether the electricity rides the hand bone or is drawn independently. It
decides whether plasmids can use the cheap render-side path or must go
bone-level. Given the parity requirement, find out in step 1-2.

## Current state (2026-07-25, session 11 - evening, superseded by the replan above)

**The user tested in-headset: LASER = "awesome, keep it as is"; hands/weapons
FOLLOW both controllers; but the MODEL placement was unusable** - "a slight
pivot or rotation that completely breaks everything... the laser moves
perfectly in tune with the controller but the viewmodel goes crazy", plus the
gun reads much too big next to real hands. The evening session diagnosed all of
it (three compounding defects) and shipped fixes for the two fixable ones:

1. **The model used the GRIP pose while the laser/bullets use the AIM pose** -
   a constant ~40-60 deg tilt between barrel and beam. FIXED: the model aligns
   to the AIM ray by default (`vrhands pose aim|grip`), plus the same aim trim,
   so barrel, laser and bullet are one ray by construction.
2. **The rotation trim was euler-adds after conversion**, which only behaves at
   one controller orientation - at any other orientation a large trim IS the
   "pivot that breaks everything". FIXED: trim is now a quaternion composed in
   the controller's LOCAL frame (`ue_math.h` quat helpers), correct at every
   orientation.
3. **The mesh's gun hangs ~50 UU (a meter-plus) from the AHands origin** (the
   eye anchor - `Hands.UpdateLocation` decompiled, ENGINE_NOTES), so rotations
   swing it on that lever. PARTIALLY ADDRESSABLE ONLY: sliders now reach
   +/-120 cm, but the full pivot correction puts the actor origin behind the
   camera and the engine CULLS the whole rig by origin (live-proven). Forward
   pull is bounded by how far out the hand is held; where to sit in that
   trade-off is the user's in-headset call. Defaults stay 0/0/0.

**The user's "just use the gun model" idea was pursued and is the right
direction, but the cheap version is dead**: the weapon IS its own actor sitting
exactly at the visible gun (ideal pivot), yet the renderer draws ATTACHED
actors from the attach matrix and ignores their transform fields - full-rate
writes to the live pistol moved nothing. The real version of the idea is the
DETACH experiment: the weapon's Base pointer (-> AHands) was located at
`+0x450` (Owner/Base adjacent pair), so nulling/re-pointing it is the next
concrete step toward a free gun-only viewmodel. `vrhands mode gun` exists but
is inert and says so.

**Also still open: gun size.** The morning's "DrawScale at +0x16C" was wrong -
that field HIDES the mesh at small values (a possible future hide-the-hands
lever!), and the other candidate (+0x168) is visually inert. No confirmed
scale field yet; the `vrhands scale` command explains this instead of writing.

**Verification state**: the shipped configuration (hands mode, aim pose, quat
trim, zero defaults) is flat-verified end to end on a clean boot - `simpose`
series places and rotates the rig sanely through the REAL mapping path, and
the mandatory fire test passed with the drive live (4/4 substitutions, 0 new
dumps). The evening's exploratory boots were heavily poked and produced two
red-herring "bugs" (a stuck lowered pose and a poke-desynced attach state)
that cost real time - the clean-boot discipline in TESTING exists for a
reason.

## Current state (2026-07-25, session 11 - morning, superseded above)

**M7's mechanism WORKS and is flat-verified: the visible hands and weapon now
follow the controller, and there was no ordering fight to lose.** The
first-person viewmodel is one `AHands` actor whose Location/Rotation the engine
copies from the camera every tick; `game/bioshock1r/hands.cpp` finds that actor
by its class vtable and overwrites those two fields from the CalcView detour,
which runs AFTER the engine's own tick placement - so ours is simply the last
write of the frame. Screenshot proof at the wall save: a 60 UU push moved the
pistol from the lower right into the centre of the view, +30 deg of injected yaw
swung it out of frame to the right and -30 deg swung it to the left, and firing
still works with the write active (4 pulls, ammo decremented, decals where the
aim seam asked, no dumps).

**Everything M7 asked for is built; what remains is the user's eye.** Three
things are in and none of them have been in a headset yet:
1. **Hands pinned to the GRIP pose** (grip, not aim - grip is where the hand
   physically is, which is what a model wants; the aim pose stays with the fire
   ray). `vrhands on|off|probe|hand|pos|rot|save|reload|test|status`.
2. **Per-weapon offset tuning**: position offsets in cm in the grip's own frame
   plus rotation trim in degrees, as live overlay sliders, persisted to
   `%LOCALAPPDATA%\BioshockVR\hands.ini`. Keyed `default` for now (the save
   carries pistol + wrench, so one profile covers it) - real per-weapon keys
   need the live weapon's class name, which means resolving this build's UObject
   class/name offsets.
3. **The aim laser** (moved here from M6 by the user): a 64x64 swapchain holding
   a CPU-generated soft dot, submitted as up to 8 XR quad layers spaced
   geometrically along the aim ray, each billboarded at the head at constant
   angular size. Pure XR space, so it is per-eye correct with no game-space
   projection. It reads the aim pose and the SAME pitch/yaw trim the fire ray
   uses, so the beam and the bullet are one ray - which is exactly what makes it
   the calibration tool. `vraim laser on|off` + overlay sliders.

**Be honest about the laser's verification status: it CANNOT be checked flat.**
XR quad layers exist only inside the compositor - they never appear in a window
screenshot - and with no headset there is no session, so the laser swapchain is
never even created. Flat, all that was asserted is "arms without crashing and
the fire test still passes". The first person to see the laser will be the user.

**M6's last loose end is CLOSED, and the answer overturns a session-10 note.**
The plasmid IS steered by the rotator out-param, not by the hand-origin
substitution. Session 10 recorded that the ability path's fire-start rotator
read all-zero; under a real Electro Bolt cast it carries the camera's own
FRotator exactly like the weapon path (all three agree at `(144 116 0)`, which
is a real rotation - the player spawns facing near +X and level). Proven by the
A/B that separates the two mechanisms, camera stationary: origin substitution ON
with +20 deg injected left-hand yaw put the bolt's scorch right of the
crosshair; origin substitution OFF (so only the rotator is written) with -25 deg
injected put it LEFT. So the damage-factory virtual never needs touching, and
step 5 of last session's plan is dead. Still unverified: a PROJECTILE plasmid
(Incinerate) - only a trace plasmid was available.

**Known cosmetic caveat found while testing**, worth knowing before tuning: at
LARGE displacements part of the mesh stretches - the forearm/sleeve geometry
appears anchored near the view while the hand follows the actor, so a 60 UU push
produces a visibly stretched arm. Realistic offsets (the few centimetres the
tuning sliders cover) stay well inside the range where this is invisible, but if
the user wants the gun held far from the face it will show.

Also shipped: `frame_context.h` now owns the FrameContext and the
XR-pose-to-game-space mapping, shared by the aim ray and the hand model, for the
same reason `ue_math.h` exists - a gun drawn with one transform and a bullet
fired with another is the exact mismatch M6/M7 exist to remove. And the
UShockUserSettings heap scan is generalized into
`patterns::scan_for_vtable_object`, which is how the AHands actor is found.

Branch: **main** (pushed directly this session, by the user's call).

## Previous state (2026-07-25, session 10)

**M6 IS WORKING AND USER-VERIFIED IN-HEADSET (2026-07-25): "I tested it and
it's pretty good... the plasmids are working and it's based on the left hand
which is very good."** The right controller aims weapons, the left controller
aims plasmids, and the camera stays on the HMD. Flat proof first (wall test with
a pistol: injected hand aim put the bullet decals 12 deg right, 12 deg left, 10
deg down and 8/8 up-right of the crosshair **while the camera never moved**, and
`vraim off` put the next round back on the crosshair), then the user confirmed
both hands in the headset.

**M6 IS DONE (user's call, 2026-07-25).** After the aim-pose fix the user
re-tested: "now it's pretty good - and the default at 0.00 for now is pretty
good." Aim trim stays at 0/0. The RETICLE/LASER moves to M7 by their call ("we
can do the laser thing when we do the guns and hands... it's the same idea"),
and they expect to judge aim calibration much better once the gun is visible.

**The complaint from the first in-headset run, and how it was fixed:**
aiming needed the wrist held lower than it should. Cause: the ray came from the
OpenXR **grip** pose, whose forward axis runs along the handle, tens of degrees
below where the controller visually points. The build now uses the runtime's
**aim pose** (its own pointing ray) with pitch/yaw **trim sliders** in the
overlay for taste - unverified in-headset as of this writing, so it is the first
thing to check next session. The user also asked for a **visible laser from the
hand**, which is the M6 rung-2 reticle work (not started - design in Next steps).

**Surprise worth chasing:** the plasmid path works in-headset even though its
fire-start rotator out-param read ALL-ZERO in the flat probe. Most likely the
hand-origin substitution alone moves the bolt convincingly (the plasmid spawns
from the hand), possibly plus a non-zero rotator under real play. Confirm with a
probe before assuming the ability path is fully controlled.

What changed today, in the order it matters:

**1. The fire flow is no longer a mystery** (full map + every address in
ENGINE_NOTES "Fire flow / aim"). Attacks in this engine are ABILITIES: trigger
-> `Weapon.BeginFiring` -> the `Firing` state -> an ANIM NOTIFY -> the ability's
`UseAbility` -> native `InitiateDamage` -> **`GetPerfectFireStart`** -> a damage
factory that runs the trace. Guns/melee go through `AWeapon`, plasmids through
`UAttackAbility`; the wrench damages through a **Havok collision phantom** and
never traces at all (so melee aim is an M7 hands matter, not an aim vector).

**2. Two discoveries made the search fast, and both are reusable.** (a) The
engine ships a readable **symbol table for name-based natives** (registration
string `int<Class>exec<Func>` -> a 12-byte `.data` entry -> the impl pointer);
`pattern_scan::find_native_function` now resolves natives with no hardcoded
addresses, and dumping that table offline (1822 entries) is the first stop for
any future engine question - M7's `AHands` natives are already visible in it.
(b) **UnrealScript `exec` thunks are not hookable seams**: hooking all four aim
thunks caught ZERO calls while shooting, because native callers go straight to
the C++ implementation. The shipped seams hook implementations (weapon side via
the RTTI-derived vtable slot, ability side via a prologue-checked RVA).

**3. The aim module is shipped, gated, and half-proven.**
`game/bioshock1r/aim.cpp` + `vraim ...` commands: per-hand rays built in the
CalcView frame from the XR grip poses (now located at the frame's predicted
display time in `core/vr/openxr_input`), self-expiring `vraim test l|r <yaw>
<pitch>` synthetic aim for headset-free testing, ownership gates that use the
same instigator check the engine makes itself, and value-driven substitution so
the engine's own spread (`ApplyAimError`) still applies on top. Live-verified:
the ability seam fires on an Electro Bolt cast, the hand map learns from the
trigger ("learned LEFT-hand (plasmid) object"), and ORIGIN substitution lands
(`SUB(L)` with our numbers).

**4. The direction WAS there - hiding in plain sight as a denormal.** The
weapon's out-param B is an **FRotator**, whose rotation-unit int32s reinterpret
as float denormals and therefore print as `(0.000 0.000 0.000)`. Its live values
matched the camera rotation exactly once printed as ints (`B[rot]=(132 116 0)` vs
heartbeat `rot=(144 116 0)`). `aim.cpp` now classifies every out-param by value
(small int32s = rotator, unit floats = direction, thousands = position) and
writes the matching type - and the bullets follow. Ruled out along the way:
`APawn::GetViewDirection` and `AShockPlayer::GetViewPoint` (probed live, never
called during a shot).

**5. What is NOT fully pinned down - the PLASMID direction.** The ability path's
rotator out-param read all-zero in the flat probe, yet the left hand demonstrably
aims plasmids in the headset. Either the origin substitution is doing the visible
work, or that slot carries a rotation under conditions the flat test did not hit.
Probe it (log the ability slots during a real cast) before touching the damage
factory virtual at vtbl `+0xEC`. Also not started: the reticle/laser at the aim
ray (M6 rung 2).

**6. Tooling that will keep paying off.** Headless UELib decompiling
(`tools/uscript/dump.ps1`, package loads in <1 s), `vraim scan <Class> <Func>`
(hook any name-based native read-only) and `vraim scanimpl <rva> <args>` (hook
any C++ implementation) - so "does this function run when X happens" is a
command, not a rebuild. Harness gotcha worth remembering: the FIRST trigger pull
only switches hands (`SwitchAndFireWeapon`/`SwitchAndFireAbility`), so a single
synthetic pull looks like nothing happened.

Branch: **`m6-decoupled-aim`** (this phase is being reviewed by PR rather than
pushed to main).

## Current state (2026-07-25, session 9)

**M5 rung 1 - the synthetic-XInput lane - is BUILT, FLAT-VERIFIED, and
IN-HEADSET USER-VERIFIED (2026-07-25): "controllers are working perfectly as
expected."** With NO physical gamepad connected, the game is fully driven from
synthetic controller state: flat-proven first (synthetic dpad moved the menu
highlight, synthetic A loaded the save, sticks moved/turned the live camera,
`vrinput off` restored byte-identical passthrough), then the user put the
headset on and played with the Quest 3 Touch mapping - move/look/fire/plasmid/
grips/buttons all correct. Some rebinds wanted later (parked to M9 fine-tuning
by user choice). Aiming note the user flagged: FIRE follows the RIGHT STICK
while the crosshair tracks the head - expected under the current
head-additive-yaw camera drive; **M6 decoupled aim replaces it** with true
controller aim. The OpenXR action layer (one "gameplay" action set, 15
actions, Quest 3 Touch bindings) boots clean and feeds the same bridge.

**The lane is NOT the originally-planned "proxy post-hook only" shape - two
surprises forced a bigger build (all in ENGINE_NOTES "Gamepad architecture"):**
(1) the **Steam overlay code-hooks our proxy's export thunk** and swallows
XInputGetState before the proxy body runs, so composed state must enter via
the game's own IAT slot instead (bridge re-points ord-2 at a wrapper; Steam
chain kept as passthrough so real pads still work); (2) the remaster **only
reads the pad inside `UWindowsViewport::UpdateInput` (RVA 0x853D20), which
nothing calls in windowed mode** - the game probes once at boot and never
re-polls (ini `UseJoystick`/`UseController` are dead). So the adapter drives
UpdateInput itself once per present and flips the engine's own
`SetUseController`. Shipped modules: `core/input/xinput_bridge` (compose +
merge + game-IAT wrapper + self-expiring seam test slots), `core/vr/openxr_input`
(action set), `game/bioshock1r/input_drive` (the UpdateInput pump). A marker
file (`%LOCALAPPDATA%\BioshockVR\vrinput.on`) read at DLL attach covers the
boot probe so a sticky opt-in survives relaunches.

**Console-command seam (M6 groundwork).** A command-seam ladder (`exec`/
`execc`/`exece`) calls the engine's own Exec dispatchers directly with a stub
FOutputDevice - the in-game Tab console is compiled out of this Steam build and
key-bound commands are inert, so this is how the mod will eventually issue
engine commands (weapon/plasmid switch, HUD toggle - M6/M8). It reaches native
engine handlers today (e.g. `GETMAXTICKRATE` = HANDLED); the script-command
path needs the player-object Exec signature reversed first (a naive
vtable-slot-65 call unbalanced the stack, so it was removed). Dispatcher RVAs +
the FExec-subobject note are in ENGINE_NOTES. Test loadouts for the combat
check were supplied out-of-mod; nothing cheat-related ships in the mod.

## Current state (2026-07-24, session 8)

**M4 IS DONE (user call at the session-8 wrap: "M4 is done - it's good for now and
the transitions are good").** Full-rate SequentialReentry stereo, comfortable,
load-safe, one toggle. The combat-scene check is deliberately deferred: the user
will test combat once MOTION CONTROLLERS (M5) are in, with a test-loadout
capability queued in that session for exactly that. **Next milestone: M5.**

**The 1t LOAD HAZARD is CLOSED and stereo is now one sticky toggle - the last sharp
edge from session 7 is gone (all flat-verified).** Single-threading is STRUCTURAL:
`reentry 1t on` MinHooks the flush-point (0x61D260, every byte re-confirmed by a
capstone disk disasm) and reproduces its decoded INLINE branch in the detour (copy
args to the render manager, stamp mode, call the drain through its guarded target),
leaving the hw-thread numerator global UNTOUCHED so its load-path consumers see the
true core count. **Load-crossing soak PASSED** with `1t` + `stereo` both armed: an
in-game save load, a quit-to-main-menu teardown, a new-game load, AND the bathysphere
DESCENT into Rapture (a real multi-map streaming transition) - zero crashes, zero new
dumps, guardskips 0 throughout, stereo re-engaging on arrival. The session-7 poke
crashed a loader on the first of those; the hook survives all four. Because of that,
`1t on` (hook mode) no longer refuses at the menu, and the off-before-load warnings
now live only on the legacy `reentry 1tpoke` (the poke, kept as a fallback).

**Everything folds into `vrstereo on|off`** - a top-level seam command, `reentry
vrstereo ...`, and an overlay "VR stereo" checkbox - that sequences structural 1t +
VR camera mode + stereo (reversing on off) and is STICKY across loads. Flat-proven:
one `vrstereo on` at the MAIN MENU armed all three (`VRSTEREO READY`), a CONTINUE-load
carried straight into Rapture with stereo doubling live and NO re-arm, and `vrstereo
off` restored mode=MT / build==presents / drain back on the render thread. Perf in the
Rapture arrival scene (heavier indoor geometry): ~81 pairs/s = 162 presents/s sustained
(eye-offset img-diff 6.5 vs 0.28 phase-consistent floor); lighthouse spawn 225 pairs/s
- both clear M4's 72-pairs/s bar.

**Session 7 (still current where not superseded):** M4 rung 2 had its FIRST IN-HEADSET
TEST and PASSED - real per-eye parallax at full rate, depth correct, world scale good
("pretty good and working as intended"). Head-motion eye weirdness was fixed by
xr-frame-per-pair pacing and USER-VERIFIED ("a looot better... comfortable now"). A
SMALL head-motion bobbing is PARKED to M9 by the user's choice. HUD-in-both-eyes still
unobserved (spawns have no HUD) - open M9 item.

**The session-6 blocker dissolved under forensics.** The minidump work (hand-parsed
MiniDumpNormal parser + capstone drain-head disasm, scratchpad) proved the
drain+0x33 null-deref is the render PUMP thread entering the drain with the
submitted-frame slot `[this+0xC]` NULL (fault addr 0x40 = its +0x40 viewport
member; registers cross-check in all specimens) - and that ALL three evening
crashes were THREADED-mode processes. The bigger surprise: **`-onethread` is not
parsed by the remaster at all** (string absent from the image; the pump thread ran
with the arg on the command line; the pump globals session 6 hexdumped are zero at
the menu in EVERY mode - they are created at first world load). The "onethread
substrate" never existed.

**The real single-threaded switch was found and shipped.** Full decode of the
flush-point decision chain (0x61D260, ENGINE_NOTES): every veto selects the INLINE
drain; the ONLY route to the threaded pump hand-off is
`[kNumHwThreadsRva]/[kThreadDivisorRva] > 1` (live 12/1 - a tight 10-reference
pair, written once at startup, consumed by seven inlined copies of the same test).
`reentry 1t on` arms the drain empty-slot guard, then pokes the numerator to 1:
every scene flush drains INLINE on the game thread. Live-verified: heartbeat
`mode=1T`, beatTid == calcTid, drain caller ret 0x61D367 (the inline call site),
submit stops firing in mono, pump sleeps forever. Mono cost ~20% (413 vs 530
presents/s) - irrelevant against the 144/s VR needs.

**Stereo on that substrate, flat-verified end to end (all session 7):**
`reentry 1t on` + `reentry stereo on` = every build doubled L/R at 168-471 pairs/s
(scene-dependent; ~225 typical in the save spawn), presents EXACTLY 2x builds, all
on the game thread, guardskips 0, no waits that can deadlock. Eye-offset render
diff 2.03 mean vs 0.33 floor; consecutive captures phase-consistent (0.43). **5-min
stationary soak + ~6.5 min of synthetic PLAY (13 clean WASD/mouse cycles navigating
up the lighthouse stairs; the second pass was cut short by the session wrap, not by
any defect) - zero faults, zero new dumps, zero watchdog events** (previous best
under threaded stereo: 16 s-3.5 min to deadlock/crash). User's call 2026-07-24: no
further flat passes needed - "everything looks good and the game was running
smooth".

**Defenses shipped so the threaded trap cannot recur silently:** `reentry stereo
on` REFUSES a threaded substrate (`stereo force` for experiments);
`render_is_threaded()` mirrors the engine's own decision chain; heartbeat/status/
overlay carry a `mode=MT|1T` tag; the drain hook (auto-installed by `1t on` and
`stereo on`) skips any empty-slot drain - the crash state - with a `guardskips`
counter. Watchdog stays detect-only.

**Known expected imperfections for the first headset test** (not failures):
per-present xrWaitFrame pacing halves game tick under a headset (xr-frame-per-pair
queued as polish), HUD renders in both eyes (M9 tie-in), IPD/world-scale not yet
calibrated.

## Previous state (2026-07-24, session 6)

**DR-5 is DONE - the engine renders a second full frame per game tick under our control,
flat-verified end to end.** The session-5 submit hypothesis was half right: hooking the
submit (0x585AC0) worked perfectly (gameplay telemetry: exactly 1 submit per present,
single call site, loc/rot == CalcView's camera), but DOUBLE-calling it is ABSORBED -
thousands of doubled submits, zero faults, zero extra presents, the yawed camera never
rendered. The view data is baked into the command queue during the game-thread BUILD
(consistent with DR-3's per-draw VS b0 finding). Following the submit's live caller RVA
into the disk image (capstone, installed this session) found the real seam: the **scene
BUILD root at RVA 0x4CCE70** (aligned-stack `push ebx` prologue - the 55-8B-EC scan hits
a decoy SEH function at 0x4CCD20; the boundary is a CC-padding run). CalcView runs
exactly ONCE inside every build call (live: calcview-in == build/s == presents/s).

**Double-calling the BUILD is the SequentialReentry primitive, proven:** pulse = second
call does real work (~2 ms vs ~60 us pass-through), re-submits, lands an extra present
during the call, CalcView re-enters and takes the second-pass yaw. Continuous (`reentry
on`, yaw 30): build 225/s all doubled, submit == presents == 450/s (TWO engine-paced
presents per game frame), game tick halves gracefully, and captures show the world yawed
30 degrees (img-diff 7.8 mean vs 0.33 noise floor). `off` recovers instantly. Stability:
~3.5 min continuous clean, then ONE hang (~124k doubled frames; struck during a focus
cycle - kill + relaunch, TESTING warning updated; hardening folds into the per-eye work).
DR-5 ticked in ROADMAP (yaw 30 > the 2-deg bar; 10-min PLAY test deferred to the per-eye
session). All constants in patterns.h (kSceneBuildRva/kSceneBuildPrologue), full map +
derivations in ENGINE_NOTES "Scene-draw architecture".

**Probe tooling extended** (`reentry hook [build|submit|drain|flush]`, `reentry dump
<n>` per-call submit arg telemetry, `reentry arg3` call-site filter; submit doubles with
copied loc/rot args, build doubles with original args + CalcView second-pass yaw; the
build slot owns the double-call controls while enabled). Capstone-based scratchpad
disasm workflow replaced hand byte-walking (findings summarized in ENGINE_NOTES, dumps
never committed).

**Session 6 part 2 - SequentialReentry STEREO built end to end; blocked on ONE
reproducible engine deadlock.** `reentry stereo on` (M4 rung 2) is fully wired and
flat-verified mechanically: every build doubled L-then-R (pass 1 caches the driven
camera + applies -IPD/2; pass 2 replays the cached base + IPD/2 - one head sample per
pair), each nested submit pushes its eye tag through the new `vr::sr_push_eye` SPSC
ring, and Present-tail pops one tag per present, capturing into the existing AER eye
swapchains (mono/AER paths untouched; presents without tags flow as before). Flat
proof: eye-offset frame renders (img-diff 2.0 mean vs 0.33 floor) and consecutive
captures are phase-consistent (0.35 - same eye every time). Design rationale in the
ARCHITECTURE decision log.

**The blocker, run to ground across the whole evening**: continuous doubling
deadlocks the THREADED renderer's event protocol (five runs, 16 s - 3.5 min, always
the same thread-dump signature: game thread in an INFINITE "render done" wait at
exe+0x61D38E vs render thread waiting inside the drain). Everything tried and its
verdict, all live: start-state gating (frame-id gate, ring-counter gate) - runs full
rate, does not prevent the hang; watchdog event re-kicks - detection is reliable but
kicking a desynced protocol CRASHES the drain (now detect-only by default, `reentry
wdkick on` to re-arm); poking the flush-point's first mode check `[0x1375BD4]` - it
is a 500-reference GIsEditor-class global, not a render toggle, crashed the next
load (dead end, recorded). **The breakthrough: `-onethread`** (Steam launch arg,
rides the steam://run URL) boots the engine's NATIVE single-threaded renderer - no
pump, no queue thread, no events, deadlock class structurally gone, and FASTER than
threaded in the test scene (630-710 fps vs ~530). Stereo on that substrate ran clean
at 194 pairs/s for ~23k pairs, then hit the ONE remaining defect: a rare crash at
drain+0x33 (fault addr 0x40 - a null object's +0x40 field, the recurring session-5
signature). Minidumps preserved in `%LOCALAPPDATA%\BioshockVR\crash\`
(bvr_20260724_181619.dmp is the onethread-stereo specimen). Stereo stays
command-gated experimental; NOT headset-safe until the null-deref is fixed.

**Nothing reached headset-testable state this session** (the stereo pipeline is
mechanically proven flat but deadlock-blocked; AER remains the working in-headset
stereo).

## Previous state (2026-07-24, session 5)

**DR-5's question is answered; the double-render seam is FOUND but not yet double-called.**
The session-4 command-queue model was corrected by live hooks (all command-gated - default
runs stay unhooked): the renderer is TWO-threaded. The game thread builds the frame and
SUBMITS it (camera loc/rot stored to globals, SetEvent kick); a dedicated render thread
sits in a pump loop (0x61D1D0, entered once - which is why hooking session 4's "frame
root" 0x61D0F0 caught zero calls: that function is a flush/join). The DRAIN (0x61CAE0) is
the real per-frame render entry - drain/s == presents/s exactly (517-525 live, 40 s soak,
~1.6 ms/frame), sole caller the pump. CalcView runs entirely on the game thread (0 calls
inside the drain) - so render-side re-entry can never re-sample the camera, and a drain
double-call pulse faulted at drain+0x33 (SEH-caught, poison latch worked as designed) then
wedged the pump's event protocol (hang, killed). **The SequentialReentry seam is the
game-thread frame SUBMIT at RVA 0x585AC0**: ret 0xC, camera loc by pointer in arg1, rot
copied to the submitted-frame block, TryEnter/Leave CS buffer sync, SetEvent at +0x1A2
(found via the probe's process-wide SetEvent caller sampler; fires ~3.7x per present at
the menu - per-arg telemetry needed before double-calling). Full corrected map + all RVAs
in ENGINE_NOTES "Scene-draw architecture"; constants in patterns.h (kFrameSubmitRva).

**Probe tooling shipped** (`game/bioshock1r/scenedraw.{h,cpp}`, seam commands `reentry
...`): command-gated MinHook slots (drain/flush), SEH-guarded double-call with poison
latch, 1 Hz heartbeat (entries/s, presents/s, tids, durations, caller RVAs), SetEvent
caller sampler (`reentry kick`), one-shot game-thread stack scan (`reentry calcstack`).
Supporting core additions: `d3d11_hook::present_count()`, `frame_inspector::
draw_call_census()`. Two clean soaks passed; the only crash was the intentional
drain-pulse probe (caught + logged exactly as designed).

**Nothing reached headset-testable state this session** (flat probe work only).

## Previous state (2026-07-24, session 4)

**The FOV problem is fully closed, flat-verified end to end.** The live settings object
(`UShockUserSettings`) is located at runtime by scanning the heap for its fixed-RVA vtable
(no stable static pointer exists - ENGINE_NOTES), its int32 `HorizontalFOV` at +0x8C is
what the renderer consumes EVERY FRAME, and:
- **Auto-claim**: the CalcView detour reads it per frame and feeds the projection claim -
  the manual claimed-FOV slider is no longer required (kept as an override).
- **Write past the cap**: `gfov <deg>` (command/overlay slider) writes it per frame with
  save/restore; **flat-verified rendering at 137** (the options UI's 130 is UI-only, no
  code clamp; monotonic img-diff 117 -> 130 -> 145). "Force headset FOV" now writes this
  real control when VR-driving.
- **USER-VERIFIED IN-HEADSET (2026-07-24, same day)**: auto-claim solid with the manual
  slider untouched, and game-FOV write at 137 "very good" (lands on the ~137 headset
  target). Both checklist items passed on the first try.

**DR-3 done in-tree** (RenderDoc never needed): new `core/gfx/frame_inspector` hooks the
context vtable's draw/clear slots; `dumpframe full` writes a one-shot frame dump (RT descs,
VS b0 readback, callstack RVAs, auto-summary + lifetime call census). Findings in
ENGINE_NOTES "D3D11 frame map": HDR R11G11B10 main pass + D24S8, half-res effects pass,
shadow pair, view-proj matrix in VS b0 bytes 128-191 (fov-scaling cross-check EXACT), and -
the headline - **the renderer is a command queue** (executor 0x61C8E0 / drain 0x61CAE0 /
frame root 0x61D0F0, all byte-verified). SequentialReentry must therefore re-enter the
command BUILD, not the drain - that redefines the DR-5 probe (next session, frame root
first).

**New tooling this session** (TESTING.md has workflows): value scanner + poke/ptr/hexdump/
strscan commands behind the command seam (found the settings object via option-change
narrowing + poke A/B), `game-cmd.ps1` (focus-safe command writes), `img-diff.ps1`
(automated A/B verdicts). Debug-CRT gotcha recorded: sprintf_s asserts modally on overflow -
use _snprintf_s/_TRUNCATE for untrusted bytes (froze the game once).

**Known flake (unresolved, low-rate)**: one boot crashed 0xC0000005 at
`bioshockvr.dll+0x30BE5` during init (before the SEH guards landed on the vtable sweep;
has not recurred since). If it recurs, the crash filter now logs module+RVA + fault addr -
symbolize against the build PDB.

**User checklist - COMPLETED 2026-07-24 (both items passed, see Current state):**
1. ~~Auto-claim check~~ - solid with the manual slider untouched.
2. ~~FOV 137 in-headset~~ - "very good".
3. Still optional any session: Steam Link cross-check; 4:3 resolution experiment
   (see session-3 notes).

## Previous state (2026-07-23/24, session 3)

**M4 rung 1 (AlternateEye) code is in on top of the verified M3 drive.** `core/vr` now owns a
PAIR of backbuffer-sized swapchains (index 0 still serves quad + mono projection). With camera
mode on, the "AlternateEye stereo test (judders)" checkbox alternates which eye each game frame
renders: CalcView shifts the camera by `sign * IPD/2 * worldScale` along view-right
(`vr::current_eye_sign()`), Present-tail copies the backbuffer into that eye's swapchain and
stores that eye's located pose, and submission gives each eye its LATEST held image + stored
pose so the compositor reprojects the half-rate-stale eye (judder, not flicker; mono fallback
until both eyes hold an offset image). The sign is published AFTER submit and only flips when an
offset frame was captured, so the un-offset enable frame is never mislabeled (see ARCHITECTURE
decision log). New controls: IPD slider (55-75 mm, default 63), "Swap eyes" inverted-depth
diagnostic, `(AER eye L/R)` tag on the layer line, and a "head offset (UU)" telemetry readout
that turns the world-scale question into a number. Flat path re-verified live this session
(scan RVA 0x1BE7A0, hook + heartbeat, VDXR instance, quiet no-headset retry, clean exit).

**M4 rung 1 USER-VERIFIED (2026-07-24): "parallax and other stuff are very nice."** The
winning configuration: the remaster's **FOV video option at 130** (its max) + the overlay's
**manual claimed-FOV slider at 130** (matching), VR camera mode + AlternateEye on. The user's
verdict: "everything is very good" - solid geometry, real parallax, depth not inverted. M3 is
now fully ticked too (6DOF verified session 2, geometry verified today). The whole fov saga is
resolved and recorded in ENGINE_NOTES: the PC+0xE0 field is telemetry-only (renderer never
reads it - proven by automated A/B screenshot sweeps via the new command-file seam +
tools/game-shot.ps1), the video option is the only real control, and the claim must match it
by hand until the settings object is scanned. Ini FOV locks back at stock True.

**Open item from the verification**: the user suspects the **IPD slider may not be doing
anything perceptible** - deferred by their choice. Note for that investigation: at world scale
50 UU/m, the 55-75 mm range only moves each eye by ~0.5 UU total, and perceived depth scale is
driven by the worldScale-to-IPD ratio - so a worldScale miscalibration would mask the slider.
Verify with an exaggerated test value (e.g. temporarily widen the slider range) before
concluding it is broken.

**User checklist (optional, any session):**
1. Steam Link cross-check - set SteamVR as active OpenXR runtime and repeat the M2 checklist.
2. If the thin top/bottom black bands bother: try the highest 4:3 resolution at FOV 130
   (vertical coverage becomes complete at a sharpness cost).

## Previous state (2026-07-23, session 2)

**DR-4 fully retired and M2 user-verified on the Virtual Desktop path.** In-headset: big
head-tracked screen, gamma OK (sRGB pick correct), sliders work, clean flat fallback. In-game:
wobble, offsets, yaw, FOV override all visible; no stutter, crashes, or input weirdness. M3
landed the same session and its 6DOF drive was user-verified (rotation, lean, turn, recenter);
immersion/world-scale judgment deferred to M4 stereo. `core/vr/openxr_runtime.cpp` runs the
whole xr_hello32 flow in-process: instance at init, lazy session on the game device with a 5 s
retry (**connect Virtual Desktop mid-game and it comes up without restarting**), xrWaitFrame
pacing at Present head, backbuffer copy at Present tail. The game pauses its boot sequence
while unfocused (see ENGINE_NOTES) - foreground the window in automated tests. The FName-chain
scan (`core/hooks/pattern_scan.cpp`, itsloopyo MIT attribution) resolves `eventPlayerCalcView`
at **RVA 0x1BE7A0** (exe rebased under ASLR; RVA is the stable identifier). The detour fires
every frame incl. main menu, call rate can exceed fps (heartbeat 400-7800 calls/s, default ON
during bring-up). Adapter UI flows through the `IGameAdapter` seam.

## Previous state (2026-07-23, session 1)

M0 complete and user-verified (F10 overlay, D3D11 FL 11_0 confirmed, LAA yes). DR-1 fully
retired: xr_hello32 (32-bit) ran a complete OpenXR session on VDXR 1.0.10 with the Quest 3
(60 frames, RTX 4060 LUID match) - M2 is unblocked. DR-2 done. Repo public at
https://github.com/mohamad-balouza/bioshock-vr. Em dashes banned repo-wide.

## Next steps (re-rewritten 2026-07-28 after the part-2 headset feedback; the +-90 drift is CLOSED - it was the render lock, now default OFF. Part 3: the user demoted the scale work to polish - "not that important for the gameplay")

1. **Await the part-3 headset verdict** (profiles swapping per wheel
   switch, save/reload persistence, lock-off default feel) - fixes ship
   from whatever comes back; then merge PR #6. v0.3.0 tags on the user's
   explicit go after a green run.
2. **Then the gameplay track (M9)**: pawn-eye-point anchoring (the user's
   walk-bob decoupling idea - camera base from Pawn.Location + eye height
   behind a default-OFF toggle), off-hand tracking + two-handed weapons
   (both prerequisites shipped), wrench swing gesture, first-boot-restart
   fix.
3. **The submission-side closeout (cheap, headset-only, any run)**: one
   glance at the `xr: fovaudit submit` line (src must read `readback`,
   swap = the render resolution) + `fovaudit pose on` for 30 s of head
   motion. Largely academic now the drift closed as the lock, but one
   glance settles it forever.

**POLISH / POST-POLISH (user's call 2026-07-28 part 3 - explicitly not
gameplay-critical):**

- **Model/world scale decoupling**: two independent sliders - world scale
  (the existing worldScale) and a HAND/MODEL scale that does not touch
  the world. Known walls from session 16 (do not re-walk blind): cluster
  bone .s blows up the attached weapon (the attach path inverts chain
  scale); actor DrawScale is geometry-inert on the fg path. Candidate
  routes, in probe order: (a) the fovA world-coupling hunt below - if the
  world-consumer is found and masked, fovA becomes a clean per-rig zoom =
  the model-scale slider for free; (b) DrawScale on a fova-matched rig
  (inert was only proven on the OLD fg path); (c) the per-eye bone-offset
  stereo-basis split parked in M9 (the session-16 design sketch).
- **fovA world-motion mystery (feeds the above)**: flat-find what ELSE
  consumes the fg node's fovA (the user saw THE WORLD move with head
  under `fova match` while the rig stayed decent) - dump-diff a
  fova-on/off pair window by window; the consumer is whichever non-rig
  pass's transforms move. Until explained, fova stays default-off and out
  of headset runs.

3. **Wall-calibration pass (the user, in headset)**: per-weapon profiles
   are live - calibrate each weapon dot==shot with the
   laser-on-bullet-hole flow, per weapon, then one "Save preset values".

4. **Pawn-eye-point anchoring (the user's idea, unstarted)**: anchor the
   VR frame to Pawn.Location + EyeHeight instead of the animated camera
   base so walk-bob never leaks into camera or hands. Cleanest AFTER the
   fova-match regime lands (one composition rewrite, not two).

5. **Then**: off-hand tracking + two-handed weapons (M9, prerequisites
   shipped), wrench swing gesture, first-boot-restart fix. v0.3.0 tags
   when the session-20/21 work is headset-verified with the user's go.

2. **DONE 2026-07-27: v0.1.0 IS PUBLISHED** - tag on main at PR #3's merge,
   release zip (both RelWithDebInfo DLLs + README.txt) at
   https://github.com/mohamad-balouza/bioshock-vr/releases/tag/v0.1.0, known
   issues listed (HUD, per-weapon alignment, bindings, flat menus = the v0.2
   targets). The user play-verified the build in-headset through a long NG+
   session the same day. Watch the repo issues for early-adopter reports.
   Session 19 ends by cutting v0.2.0 the same way (after the user's go).
1. **Publish the first GitHub release AFTER the user's explicit go** (their
   rule: nothing public-facing without asking): tag (proposed v0.1.0) on the
   merged main, upload the zip (rebuild from the tagged commit: `build.ps1
   -Release`, then zip the two RelWithDebInfo DLLs + README.txt), release
   notes from the README's status blurb.
2. **Watch the pace-guard telemetry on the first real headset idle**: the >1 s
   xrWaitFrame log lines + `vrpace status` skips/keepalives show whether VDXR
   recovers via events alone (then the 5 s keepalive can lengthen) or needs
   the keepalive. If the headset-off window still stalls in the field, the
   fallback design is an async wait thread - not built because the sync gate
   passed flat.
3. **Then M8's HUD usability** (health/EVE on a floating quad during stereo
   gameplay + the Quest 3 keybind audit) - the remaining M8 items.

Carried-over backlog (numbering kept from session 17):

2. **The M7.5 leftover: the exact cull angle.** Park `vrhands simpose <yaw> 0 0
   120000` at 0/30/60/80/90/100/120 from the facing and find the cull-on/off
   boundary from both directions - but NOT with NCC template chaining, which
   failed outright in session 17 (correlations 0.56-0.79; every number void).
   Use a template-free instrument: the `[tlm] lock tgt=` NDC is ground truth for
   where the anchor should land, and a dark-pixel disc-width profile or a
   `vrhands off` reference-frame difference detects presence/absence without a
   template. Only worth doing if the user still sees the rig vanish in the
   headset with the transfer on - the mechanism that caused it is now removed.
3. **The retired scale hunt**: worldScale 100 solved the size percept; the
   engine-lever negatives stay documented (ENGINE_NOTES session 16 part 2)
   and the world/viewmodel scale SPLIT design is parked in M9. Do NOT
   reopen unless the user wants the world scale moved independently.
4. **If the headset run reports depth slightly off**: `vrbones lockdgain`
   is the live A/B (it scales the applied pull too - the 12.8 knob assumes
   0.9); if the resting spot reads wrong, `vrbones lock diff`. If the rig
   BLANKS when the hand comes near the face: expected inside ~23 cm (the
   behind-camera cull edge, ENGINE_NOTES session 16) - report the distance,
   a soft clamp on the applied pull near the face is the queued fix.
   (Weapon-laser fine alignment: the existing pos/trim sliders + "Save
   offsets" - after the scale lands.)
5. **Sway kill at source (UpdateHandValues bob params)**: possibly less urgent
   now - with lock ABS the flat shot series sat pixel-frozen (the per-frame
   re-pin may be absorbing the lateral sway); ask the user whether any
   breathing still shows before building this.
6. **Polish knobs the headset run may ask for**: per-hand anchor trims (the
   left wrist anchor may want a palm offset), collapse scope (clavicle in or
   out), and a smoothing option if raw controller jitter reads on the model.
7. Carried over, unchanged: a true dot at the IMPACT point (needs a per-frame
   engine line-check - start at the damage factory, factory fetched by 0x231E70
   in `UAttackAbility::InitiateDamage` 0x1BBD80, virtual at factory vtbl
   `+0xEC`); verify a PROJECTILE plasmid (Incinerate); hide the head-centred
   crosshair; per-weapon offset profiles keyed by weapon class name; and the
   20-second load-crossing check the user still owes.
8. **Dropped from M7 by the user's call**: dual-wield (BioShock 1 equips one
   hand at a time - it moves to the M10 BioShock 2 adapter, which supports it
   natively), elbow IK, arm bending, two-handed grips.
9. M5 rung 2 - menu mode (quad + controller laser -> virtual mouse, DR-6).
   Controller polish (deadzone/curve/menu timing) and rebinds stay parked in M9.
10. Still open from M3: cutscene cameras are head-driven. The aim path already
   guards on the view actor's vtable (AShockPlayer) - reuse that predicate for
   the camera when it is addressed.
11. If the init-crash flake (bioshockvr.dll+0x30BE5, one occurrence pre-SEH
    guards) recurs: the crash log prints module+RVA - symbolize against the PDB.
12. DR-7 borderless/windowed stability; DR-6 menu input path; optional Steam
    Link / SteamVR cross-check any time.
13. **Parked in M9** (user's call 2026-07-24): IPD slider verification, small
    head-motion bobbing, the vrstereo off/re-arm state bug, HUD-in-both-eyes.

### IN-HEADSET CHECKLIST - render sync + per-weapon profiles (session 21)

Setup as always: Quest 3 + Virtual Desktop (VDXR), launch from Steam,
CONTINUE = the NG+ Medical Pavilion anchor (TRAP: if the game boots into
the 1960 plane intro, the main-menu focus moved to NEW GAME - use main
menu -> LOAD GAME -> the newest entry), press **VR PRESET 1**. Shipping
defaults are byte-identical to session 20 except the per-weapon profile
layer; every new render lever is opt-in. Nothing in the camera/recenter/
body path changed.

1. **Non-regression sweep (60 s, first).** Park the hand, look around
   wide, stick-walk, turn past 90 both ways, two right-trigger pulls + an
   Electro Bolt, pause menu once. Anything session 20 did not do: stop
   and report.
2. **PER-WEAPON PROFILES (the shipping headline).** With the shotgun up,
   fire at a wall and nudge the R trim + ray-offset sliders until the
   laser sits exactly on the bullet holes. Switch weapons through the
   wheel - pistol, MG, crossbow - and tune two or three the same way.
   Switch back and forth: each weapon must keep ITS OWN tuning (the log
   echoes `weapon profile '<name>' applied` per switch - this live-switch
   swap is the one proof the flat harness cannot do). Then "Save preset
   values", quit to menu, reload, PRESET 1: every weapon's tuning must
   come back by itself.
3. **THE FOVA PREVIEW (opt-in, 2 minutes - session 22's decision data).**
   `vrfgnode fova match` then `vrbones lock off`. The gun/hands will read
   MUCH smaller and deeper - that is true world geometry (the flat A/B
   shows the composition collapse). Judge two things only: (a) at +-90
   hand yaw, does the rendered gun now sit closer to the laser (the drift
   you reported)? (b) does the world-size percept change? Do NOT re-tune
   anything in this mode; `vrfgnode fova off` + `vrbones lock abs`
   restores the shipping look exactly.
4. **The submission log glance.** After PRESET 1, find the one-line
   `xr: fovaudit submit ...` in the log: report `src=` and `swap=`
   (expected: `readback` and your render resolution). Optionally run
   `fovaudit pose on` for ~30 s of head motion and grab the `poseaudit`
   lines - they close the pose-tag suspect for the +-90 flip.
5. **Anything off**: `vraim wkey real` re-resolves the weapon profile;
   the R sliders always edit the ACTIVE weapon. If a symptom survives
   `vrfgnode off` + the default toggles, it predates this session.

### PREVIOUS CHECKLIST - aim sync + testing framework (session 20) - RAN SAME DAY, results in "Session 20 part 2" above (muzzle failed -> default OFF; drift persists -> session-21 plan; swaykill clean; PR #5 merged on the user's call)

Setup as always: Quest 3 + Virtual Desktop (VDXR), launch from Steam, CONTINUE
(the NG+ Medical Pavilion all-weapons anchor), press **VR PRESET 1**. Nothing
in the camera/recenter/body path changed - head decoupling and body-follow
must feel identical to v0.2.0; any difference there is a regression, say so
first. Live A/Bs: `vraim muzzle on|off`, `vrhands swaykill on|off`.

1. **Non-regression sweep (60 s, do first).** Park the hand, look around
   wide, stick-walk, turn past 90 both ways, two right-trigger pulls + an
   Electro Bolt, open the pause menu once. Anything session 19 did not do:
   stop and report.
2. **THE ALGEBRA FIX (the headline). Laser vs barrel at MANY orientations,
   ESPECIALLY WRIST ROLL.** Weapon up, laser on: roll your wrist +-90, tilt
   up/down/diagonal, point across your body - the laser must stay glued to
   the barrel line at EVERY orientation now (pre-fix it drifted up to ~28
   deg at rolled poses - the "agrees only where I tuned it" defect). Same
   check on the plasmid hand: whatever offset the L laser has vs the hand,
   it should now be CONSTANT at every orientation (if you want it tighter,
   `vraim cal l <pitch> <yaw>` now holds everywhere, not just at one pose).
3. **THE MUZZLE RAY** (arm `vraim muzzle on`): bullets + laser now leave
   along the RENDERED barrel. Judge vs `vraim muzzle off` (the trimmed
   controller ray you have today). Then SWITCH WEAPONS through the wheel -
   pistol, MG, shotgun, crossbow - each should auto-align its laser to its
   own barrel with ZERO manual trim (this is the per-weapon proof the flat
   harness could not do). Report per weapon which mode aims truer.
4. **THE SWAY KILL** (on by default): raise a weapon, stand still, watch
   the barrel/laser - the breathing should be GONE. `vrhands swaykill off`
   = the old sway for comparison. Then verify animations still LIVE: fire,
   reload, switch weapons, swing the wrench - all should play through
   normally and the hand must settle correctly afterwards (a hand frozen
   mid-pose after an animation = the settle window failed, report it).
5. **Quick controls regression** (adjacent code, unchanged paths): wheel
   select incl. up/down with pitch restore, ammo click-hold select, A/B/X/Y
   as v0.2.0.
6. **(optional but valuable, 60 s) Record a real run**: `vrrec start`, play
   normally - walk, look, fire, switch - then `vrrec stop`. That file lets
   me replay your exact run flat next session (first true flat reproduction
   of headset behavior). Nothing to judge; just do it once mid-session.
7. **The 4 GB-scan field watch**: if the model EVER stops following after a
   level transition, grab the log ("AHands scan" lines).
8. **Anything off**: `vraim muzzle off` / `vrhands swaykill off` first - if
   the symptom survives both, it predates this session.

### PREVIOUS CHECKLIST - M8 completion (session 19) - PASSED (v0.2.0 shipped on it)

Setup as always: Quest 3 + Virtual Desktop (VDXR), launch from Steam, load the
newest save (CONTINUE = the NG+ Medical Pavilion all-weapons anchor), press
**VR PRESET 1**. Nothing in the camera/recenter/body path changed - head
decoupling and body-follow must feel identical to v0.1.0; any difference there
is a regression, say so first. Live A/Bs: `vrhud off`, `vrhands hideinactive
off`, `vrinput pitchkill off`, `vrhud force off`.

1. **Non-regression sweep (60 s, do first).** Park the hand, look around wide,
   stick-walk, turn past 90 both ways, two right-trigger pulls + an Electro
   Bolt. Anything session 18 did not do: stop and report.
2. **THE HUD QUAD (the headline).** From the moment stereo is up in gameplay:
   health/EVE/ammo must float on a readable head-locked panel - no more
   eye-searing screen-edge double vision. Judge: readability at a glance,
   size/distance/height (three "HUD" sliders in the VR section tune it -
   "Save preset values" persists), and that it never blocks aiming. Check the
   ammo counter updates when you fire and switch types. Then open the PAUSE
   MENU: it should land on the same readable panel (bonus feature - tell me
   if navigating it feels fine). `vrhud off` = HUD back in the game frame.
3. **THE MONITOR while stereo runs**: the flat window must still show the
   HUD over the single-eye mirror (slightly more vivid bars than stock is
   expected - the alpha repair). During headset-off idle the window keeps
   running WITH its HUD.
4. **HIDE-INACTIVE.** With the weapon up: NO ghost left hand/arm anywhere
   (the sweater arm reaching for the shotgun forend is the defect that
   died). Switch to plasmid (grip or trigger): weapon AND right hand
   gone, plasmid hand normal with FX. Switch back: weapon returns at FULL
   size instantly (a tiny/invisible/blown-up weapon after a switch = the
   restore failed, report + `vrhands hideinactive off`). Cast Electro Bolt:
   FX on the visible hand, nothing floating in the air.
5. **STICK PITCH.** The right stick turns you but can NO LONGER pitch the
   view - pitch is your head alone. The viewmodel must no longer ride
   vertically when you push the stick. THE WRENCH QUESTION (answer this one
   carefully - it decides whether session 20 builds the body-pitch drive):
   swing the wrench at things above/below eye level while aiming with your
   HAND, head level - does it hit where the hand points now, or still where
   the body faces? `vrinput pitchkill off` = old behavior live.
6. **THE NEW BINDINGS walkthrough** (README table): A jumps, B uses/loots,
   X reloads (and hacks/injects EVE in context), Y pops a first-aid kit,
   grips still switch/radial, triggers still fire, stick clicks
   crouch/zoom, menu short/hold = pause/map. **AMMO FLICKS**: with a gun
   up, flick the right stick UP = next ammo type, DOWN = previous - check
   it never fires accidentally while turning (the flick needs a sharp
   full push; report if it triggers on normal turns), and that holding a
   grip + moving the stick still drives the radial without ammo flips.
7. **Expected noise, not bugs**: the HUD quad is head-locked (moves with
   you by design - say if you'd prefer it lazier/world-locked, that is a
   slider-able follow-up); HUD glass slightly more vivid; the VD overlay
   still parks the model dead-center (VISIBLE state, known); one
   `engine exec 'set ShockPlayer bReticleDisabled True' -> HANDLED` line
   every 15 s.
8. **The 4 GB-scan field watch**: if the model EVER stops following after a
   level transition, grab the log immediately ("AHands scan" lines).
9. **Anything off**: `vrhud off` / `vrhands hideinactive off` /
   `vrinput pitchkill off` first - if the symptom survives all three, it
   predates this session.

### PREVIOUS CHECKLIST - M8 quick phase (session 18) - PASSED (v0.1.0 shipped on it)

Setup as always: Quest 3 + Virtual Desktop (VDXR), launch from Steam, load the
newest save, press **VR PRESET 1**. Four changes to judge; `vrmirror off`,
`vrpace off`, and the L/R forms are the live A/Bs. Nothing in the camera or
recenter path changed, so head decoupling and body-follow should feel
IDENTICAL to session 17 - anything different there is a regression, say so
first.

1. **Non-regression sweep (do this first, 60 seconds).** Park the hand, look
   around wide - the model must hold its world spot exactly like session 17.
   Stick-walk where you look, turn past 90 both ways, two right-trigger pulls
   + an Electro Bolt. Any drag, drift, or stutter that session 17 did not
   have: stop and report.
2. **THE GRIP-SWITCH FIX (the bug you reported).** Switch hands with the GRIP
   several times, in both directions, WITHOUT pulling triggers in between.
   The incoming hand's model must appear on the CORRECT controller
   immediately - no wrong-controller model, no needing a trigger pull to fix
   it. Then mix in trigger switching to confirm nothing regressed there.
3. **PER-HAND MODEL OFFSETS (your ask).** Overlay -> "Hands + weapon (M7)":
   above the six sliders there is now "Tuning hand: L (plasmid) / R (weapon)".
   With the pistol up, tune R sliders - the plasmid hand must NOT move when
   you later raise it. Switch the selector to L, tune the plasmid hand - the
   pistol must stay where you put it. Press **"Save preset values"** (one
   button now saves these too), quit to menu, reload, PRESET 1 - both hands'
   tuning must come back.
4. **THE DESKTOP MIRROR (blocker b).** While playing in stereo, glance at the
   monitor (or better: have the phone record it / stream it). The window must
   show ONE steady eye - no L/R flicker, no shimmer. In the HEADSET nothing
   may change: stereo depth still correct both eyes (this is the one thing
   flat cannot verify - the re-blit must not have touched the compositor
   feed). `vrmirror off` = the old flickering window for comparison.
5. **THE DISCONNECT STALL (blocker a).** Mid-game, take the headset off and
   let it sleep (or close Virtual Desktop streaming). The flat window must
   KEEP RUNNING at full speed (the old behavior was <1 fps). Put the headset
   back on / reconnect VD: the game must come back into the headset cleanly
   within a few seconds. Then check `vrpace status` in the log and tell me
   the skips/keepalives numbers and any "xrWaitFrame blocked N ms" lines -
   they tell us whether VDXR needed the keepalive.
6. **THE RAY OFFSET SLIDERS (part 2, your ask).** In the aim section, "Ray
   offset hand: L / R" + three sliders (forward/right/up, cm). Tune the
   LASER onto the barrel of the tuned model: the beam and the bullets move
   together (one ray by construction), the model does not move. Check a shot
   actually lands on the laser after tuning - that is the invariant that
   matters. Then "Save preset values" and confirm it comes back after a
   reload + PRESET 1.
7. **THE CROSSHAIR (part 2, your ask).** The flat head-centred crosshair
   should simply be GONE from the moment the game is up - no preset needed,
   hidden is the default. The overlay checkbox "Flat-screen crosshair" (VR
   camera section) or `vrxhair on` brings it back live if you ever want it.
   Check it STAYS gone across a save load and a level transition.
8. **Expected noise, not bugs**: during headset-off idle the log prints a
   watchdog "deadlock state detected (log only)" line every ~5 s (the
   keepalive block trips it); the VD menu overlay still parks the model
   dead-center (VISIBLE state, poses stop - known since session 16); and the
   log prints one `engine exec 'set ShockPlayer bReticleDisabled True' ->
   HANDLED` line every 15 s (the crosshair re-assert).
9. **Anything off: `vrmirror off` / `vrpace off` first.** If the symptom
   survives both, it predates this session.

### PREVIOUS CHECKLIST - M7.5 body-follows-head (session 17) - PASSED

**Result: passed on the first run.** Head decoupling held ("the models didn't
move when I moved my head"), the stick worked as expected, and looking to a
specific spot left or right at a decent angle behaved. The only change asked
for was `vrbody deadzone` 0 -> **23 deg**, which is now the shipped default -
so the "Deadzone (deg)" slider below reads 23, not 0, on any later run.

Setup: Quest 3 + Virtual Desktop, launch from Steam, load the newest save,
press **VR PRESET 1** (it now arms the yaw transfer too, after the viewmodel
and before stereo). What changed: the invisible body/pawn facing now follows
your head every frame, so **forward is where you look**. The camera is
mathematically unchanged by this - you should not feel the transfer happen at
all, only its effects. `vrbody off` reverts to the session-16 behaviour live,
`vrbody on` brings it back: that is the A/B for every question below.

1. **HEAD DECOUPLING - the non-regression check, please do this FIRST.** Park
   your hand pointing at something and look around with your head, yaw AND
   pitch, through a wide range. **The gun must hold its world spot and must
   NOT follow your head.** This is the defect that took sessions 12-16 to kill
   and the one thing we cannot regress. Flat it is bit-identical (the hand's
   world pose and the camera came out byte-for-byte the same with the transfer
   on and off), but your eyes are the gate. If anything drags with your head,
   say so and stop - `vrbody off` is the immediate revert.
2. **The headline: walk where you look.** Physically turn your head, then push
   the left stick forward without touching the right stick. You should walk in
   the direction you are looking. Then A/B it: `vrbody off`, same test - you
   should get the old behaviour (walking along the stale facing). Does the new
   one feel right, or does it need smoothing? (`vrbody rate 3` eases the body
   in over ~1/3 s instead of snapping; `vrbody deadzone 20` makes small
   glances not steer. Both default 0 = instant, which is what you asked for -
   these are here in case instant feels twitchy in practice.)
3. **The disappearing rig.** This is the symptom that should be gone. Turn
   past 90 deg in both directions, aim off to the sides, do the things that
   used to make the whole rig vanish. Does it still vanish anywhere? If yes,
   roughly what angle and which direction - and does `vrbody off` make it
   worse? (Flat note: the mechanism that caused it is removed, but the exact
   cull boundary was NOT measured this session - the template instrument
   failed and I did not want to report numbers I could not trust.)
4. **Weapon-laser alignment at angle.** The complaint was that alignment
   degraded as the hand aimed away from the body facing. Aim well off to the
   side and check the barrel against the laser. Flat, the render lock's
   correction went from swinging 10.5 UU across a +-30 deg head sweep to flat
   within 0.5 UU, which should read as "the gun stops drifting as you look
   around" - does it?
5. **Expect the viewmodel to LOOK different between `vrbody on` and `off`.**
   Not a bug: the renderer views the rig from the actor's orientation, so with
   the transfer on it is viewed from your camera's orientation instead of a
   stale body facing. Flat, the world is pixel-identical between the two while
   the gun sits differently. Which one looks right to you?
6. **Turning still works normally.** Right stick turns camera and body
   together as before; it composes with the transfer rather than fighting it
   (flat-proven). Any stutter, fight, or drift while turning is signal.
7. **Fire + plasmid parity**: two right-trigger pulls at a wall, then left
   trigger Electro Bolt - electricity on the driven hand. (Flat: 6 pulls,
   ammo 57->51, no crash dumps.)
8. **Load crossing (20 seconds)**: Esc -> LOAD -> newest save -> YES, then
   fire both hands. The transfer resets its state machine on the new
   PlayerController and re-probes; you should not notice.
9. **Cutscene / bathysphere**: if you hit one, confirm the view behaves
   normally. The transfer is gated off whenever the view actor is not your
   pawn, but it has not been tested against a real cutscene.
10. **Anything that feels off, `vrbody off` first** - if that fixes it, it is
    mine; if not, it predates this session.

### PREVIOUS CHECKLIST - M7-v2 matched-lens run (session 16 build)

Setup unchanged: Quest 3 + Virtual Desktop, launch from Steam, load the newest
save, tick **VR stereo**, then **"Viewmodel follows the controller"**. What
changed since your last run: the rig now renders through the WORLD lens (the
real foreground-FOV field found in session 15, calibrated and defaulted ON in
session 16) - correct internal perspective, the full arm+sweater composition,
world-correct size/depth/parallax, and a head-look overshoot the flat sweep
caught is fixed. `vrfgfov off` flips back to the previous build's look live.

1. **The headline: composition + perspective.** Does the gun+hand now read
   like a native VR viewmodel (a normal object with correct proportions),
   not a telephoto cutout pasted into a wide-angle world?
2. **Depth + size.** Gun AT the hand, receding/approaching 1:1 as you push
   and pull the controller.
3. **Head-look decoupling.** Park the hand, look around with your head (yaw
   AND pitch) - the gun should hold its world spot exactly (this run
   specifically fixed an overshoot here; any residual trail is signal:
   `vrbones lockgain 0.75` vs `1.1` is the live A/B).
4. **Lean/translation.** Lean side to side and forward - gun holds its place
   like a real object, no sliding against the laser.
5. **The close-range edge (NEW, please probe).** Bring the controller slowly
   toward your face: the rig is EXPECTED to blank out somewhere inside
   ~25 cm (engine culls behind-camera geometry - known edge, fix queued if
   it bothers). Report roughly where it vanishes and that it comes back.
6. **Tracking recheck**: yaw/pitch/wrist-roll about the grip, glued to the
   laser.
7. **Fire + plasmid parity**: two right-trigger pulls at the wall; left
   trigger Electro Bolt - electricity on the driven hand.
8. **Depth knob if needed**: gun a touch near/far or big/small ->
   `vrbones lockdgain 0.75` vs `1.1` (default 0.9; it also scales the pull).
9. **The A/B, any time**: `vrfgfov off` = the session-14 narrow-lens look.
   Which do you prefer, and by how much?
10. **Resting placement + proportions, your calls**: if the resting SPOT
    reads wrong (not the depth), `vrbones lock diff` keeps the classic
    composition with the same depth fix. And AT the correct distance, does
    the model itself still read oversized next to your real hand? Only if
    yes does the DrawScale lever get built.
11. **Load-crossing (20 seconds, still owed)**: with everything armed, Esc ->
    LOAD -> newest save -> YES, fire both hands, confirm no crash dump.

Expected and not failures: crosshair still head-centred and disagreeing with
the laser; HUD in both eyes; the rig's idle/fire animations may look muted or
static while the drive holds the pose; `vrhands mode gun` prints "inert" by
design; `vrhands mode hands` is the retired actor-pinning kept only for A/B;
and with the VD/Quest MENU OPEN the model parks dead-center (the runtime
stops delivering poses in the VISIBLE session state) - close the overlay
and it resumes.

## Open questions / blockers

- ~~PC+0xE0 FOV recompute~~ - moot: the field is telemetry-only (2026-07-24, ENGINE_NOTES);
  the engine re-stamps it to 100 on scene/controller changes, which is harmless to us.
- CalcView call rate >> fps at the uncapped menu - determine whether extra calls are benign
  re-entries (same frame) or distinct view queries; affects where per-frame XR pose sampling
  should live (matters more for SequentialReentry).
- ~~Console availability~~ - RESOLVED (session 9): the in-game Tab console is
  compiled out of this Steam build (`-allowconsole` verified on the live command
  line, still dead) and key-bound commands are inert. The mod issues engine
  commands via the console-command seam (`exec` -> engine Exec dispatchers).
- Adapter VRAM logs as "3072 MB" - DXGI_ADAPTER_DESC.DedicatedVideoMemory is a 32-bit SIZE_T in
  our process, so values â‰¥4 GB truncate. Cosmetic; ignore.
- itsloopyo's headtracking mod also installs as `xinput1_3.dll` - mutually exclusive with ours
  (install.ps1 backs theirs up automatically).

## Session log (newest first)

### 2026-07-28 - Session 21 (render sync: fov audit clean, the fg scene decoded, the fovA lever, per-weapon profiles)

- FOV AUDIT (plan item 1): instrumented submission tangents (`fovaudit`,
  on-change submit log, poseaudit) + built tools/decode-framedump.ps1;
  measured rendered == submitted == option-derived to 1e-5, both eyes, fg
  draws included, no hidden lens. NEGATIVE RESULT: no fov lie exists -
  BioVR's discipline was already our architecture. ENGINE_NOTES entry.
- FG SCENE (plan item 2): decoded the fg pass to a second scene node
  (ctor 0x56DC30, camera-pose + fovA/fovB args, node at scene+0x1B0);
  proved the camera inputs already per-eye correct (killed the
  crossed-eye and stale-camera models by pass-labeled measurement);
  FOUND THE fovA/fovB ZOOM-PULL - `vrfgnode fova match` collapses the rig
  to true world geometry (diff 11.09/44.2% vs 2.9 floor, exact
  round-trip). Negatives logged: no script fg property; pawn+0x724 null =
  fatal (dump #9); providers = skinning strategies. All levers default
  OFF; the session-22 retune decides the new regime.
- PER-WEAPON PROFILES (plan item 3, SHIPPING): UObject identity derived
  live (+0x28 name / +0x30 class, 'Shotgun'/'PlayerHands' cross-check);
  R-hand trim+offset profiles keyed by class name, hot-swap + weapons.ini
  + preset-chain save + preset-tail re-apply; weapon resolves PRE-FIRE
  via the structural Base==AHands accept + null backoff. All flat gates
  exact; the live-switch swap is the checklist headline.
- Harness: boot.ps1 NEW-GAME roulette diagnosed (menu focus; recovery =
  LOAD GAME via 80 ms dpad taps; main menu ignores mouse clicks); crash
  baseline 8 -> 9 (the one deliberate discovery crash).
- NOT started: stage 4 (pawn-eye anchoring) - queued behind the retune.
- Parts 2-4 (same day, three headset runs + fixes between): the +-90
  drift CLOSED as the render lock itself (lock now default OFF); weapon
  resolver rebuilt twice on live evidence (learned-object pinning, then
  the MG/GL native-vtable gap -> class-agnostic Hands.CurrentHoldable at
  +0x45C); profile seeding race fixed (preset baseline); the user's full
  calibration saved + BAKED AS DEFAULTS (incl. deadzone 0 and 8 default
  weapon profiles; virgin-install gate green); trim sliders +-90;
  scale-decoupling demoted to polish (user's call); fovA in-headset
  negative (world moves - consumer unknown, parked); FPS audit: all
  stalls = VISIBLE windows, not mod work. v0.3.0 MERGED + TAGGED +
  RELEASED with the preset inis bundled.

### 2026-07-28 - Session 20 part 2 (headset results, PR #5 merged, the BioVR analysis, the re-plan)

- In-headset: the algebra fix is a no-op for the user's zero R trims
  (expected); the standing defect is the laser sitting laterally off the
  rendered gun with the sign flipping at +-90 hand yaw + origin off the
  barrel; `vraim muzzle` FAILED (bone-44 axis is not the barrel - retire
  the derivation, feature stays default-OFF); the same drift shows in both
  modes -> render/composition domain; swaykill drew no complaint.
- PR #5 merged to main on the user's call (nothing default-armed failed).
- Analyzed the new BioVRDev/Bioshock-Remastered-VR mod (NO license -
  concepts only; findings in RESEARCH.md): same scale numbers as ours;
  their feel comes from FOV-exact layer submission + cyclopean hand
  anchoring + per-weapon hand-tuned profiles with an exact dot==shot
  wall-calibration loop; no sway kill; same structural gun-vs-dot drift.
- Re-plan (user's call): FOV audit first, then the WORLD-PASS RE-HOMING of
  the existing rig (the new big swing), per-weapon profiles as
  fallback/polish, pawn-eye-point anchoring as follow-up.

### 2026-07-28 - Session 20 (the aim-sync session: all six stages flat-green)

- Worked the session-19 plan stages 4.1-4.6 as designed, each flat-gated:
  synccheck baseline (canonical algebra divergence 28.21 deg max, roll-
  dominant) -> trim-algebra unification (0.03 deg max = the int-rotator
  floor; quat helpers promoted to core/util/xr_math.h; aligntrim deleted)
  -> vrrec record+replay (712/712 marks bitwise exact incl. swept head
  segments) -> FName/GNames (ctor 0x70D660 captured, GNames 0x13904EC,
  'None'/'ByteProperty'/'Launcher' resolved) -> muzzle ray (vraim muzzle;
  shotgun barrel measured ~9-11 deg above the attach forward; laser rides
  the same d0) -> idle-sway kill (vrhands swaykill; +-1.2 deg measured,
  frozen bitwise 8/8, animations pass a 2x-envelope threshold with a 600 ms
  settle window).
- Design corrections forced by measurement: the SET-seam sway premise
  dissolved (the bob function is the velocity-weighted WALK bob; idle
  breathing is the authored idle anim - no property to zero), so the kill
  moved to the drive's reference recapture; fopen_s share-mode trap hit
  AGAIN (session 1 fixed the same bug in the logger) - recordings open via
  _wfsopen _SH_DENYNO.
- Negative results logged: `exec NextWeapon` faults (SEH caught); flat
  weapon switching through the radial remains a harness gap; vrbody state
  poisons record/replay comparisons (documented rule: vrbody off or
  settled).
- Stretch (off-hand tracking) not started - queued in M9, now unblocked.
- Branch s20-aim-sync, six commits, PR opened; v0.3.0 waits on the
  in-headset checklist + explicit go.

### 2026-07-28 - Session 19 close (v0.2.0 PUBLISHED)

- The user's re-verify passed everything ("Everything passed, go ahead").
  PR #4 merged to main, tag v0.2.0 on the merge, release zip (RelWithDebInfo
  DLLs + README.txt from the merged tree) + notes published on GitHub.
  M8 is COMPLETE. Left-handed mode captured in the post-v1 backlog;
  off-hand tracking + two-handed weapons queued in M9 behind the session-20
  aim work; wrench swing gesture in M9 polish.

### 2026-07-28 - Session 19 part 3 (wheel-select fix + zoom removed)

- The pitch kill was eating stick Y inside the radial state too - the weapon
  wheel could not select up/down. The kill now lifts while a grip/bumper is
  held, with a PC-pitch snapshot/restore around the hold (the radial state
  keeps the look axis bound, so wheel-time drift would otherwise stick).
  Flat-exact: 853 frozen no-bumper; 853 -> 4001 during the RB hold (calls/s
  drop = wheel open); restored to exactly 853 on release.
- Zoom removed in VR by the user's call: RS-click never reaches the game;
  the click is purely the ammo modifier. Test lane unaffected.

### 2026-07-28 - Session 19 part 2 (headset PASSED; the four feedback fixes flat the same night)

- The user's in-headset verdict: "looks amazing" - no regression, HUD panel
  and ghost-hand removal called out as perfect, monitor perfect, and the
  WRENCH passed with the pitch kill alone (the HMD-pitch body drive is
  dropped from session 20).
- Feedback fixes, all flat-green: menu transparency root-caused to gameswf
  blend states accumulating garbage alpha coverage -> cached alpha-corrected
  blend-state variants per redirected draw (lum repair down to a 0.35
  floor); the sprawling odometer strips root-caused to stencil-based flash
  MASKS with no DSV bound -> the capture RT owns a D24S8 now (pause-menu
  counters render clipped, panels opaque - both verified on the window
  composite); Touch A back to use/confirm with jump on B; ammo select
  redesigned as right-stick CLICK-HELD + up/down/left (three slots, three
  directions - the dpad SELECTS slots, it does not cycle), quick tap still
  zooms, bare flicks removed.
- FPS measured flat: the full capture costs ~4% of an uncapped flat frame
  (~0.11 ms) - the perceived dip is likely zone/Debug-build. Desc cache
  added against the classifier's COM churn. Wrench swing gesture queued to
  M9 (user's call). Docs + tables updated; re-verify list in Current state.

### 2026-07-28 - Session 19 (M8 COMPLETE flat: HUD quad, VR bindings + ammo flicks, hide-inactive, stick-pitch kill; item 4 deferred whole to session 20)

- Branch `s19-m8-completion` off main at v0.1.0. Worked the SESSION 19 PLAN
  items in the user's order; per their call, v0.2.0 ships after items 1-3
  (this session) and item 4 (aim-sync algebra + testing framework) moves
  whole to session 20 - designs written down in Next steps + the plan file.
- **Harness first**: tools/boot.ps1 rewritten save-agnostic around a new
  `[b1r] view state: GAMEPLAY` transition log (the strict ShockPlayer
  predicate, promoted public from body.cpp) - five clean boots this session,
  3-15 A-presses each. One early misread (the NG+ save's Medical Pavilion
  coords pattern-matched to what looked like a menu loc from the old log)
  cost 38 stray A-presses before the screenshot showed live gameplay - the
  detector had been right all along.
- **Hide-inactive (1a)**: inactive cluster+sleeve zero-scale collapse, weapon
  bone 43 hidden BY TRANSLATION (attach path inverse-decomposes scale),
  restore-from-ref before the incoming hand drives. Flat: ghost-arm diff
  2.64/6.9% vs 0.75 floor, both switch directions, FX parity cast live,
  fire test, dumps 8->8. One trap self-inflicted mid-run: the 120 s
  simpose cap expired silently and a whole plasmid-phase test ran on the
  ENGINE pose - caught by the frozen writes counter, redone armed.
- **Stick-pitch kill (1b)**: compose_over zeroes right-stick Y while
  camera.cpp publishes vrDrove && strict-gameplay. Pitch frozen through
  full-up holds while yaw spun; kill-off freed it instantly (853 -> 18000
  clamp); packet bumps intact.
- **THE HUD (2)**: the hunt falsified the session-6 fingerprint (dumps
  showed the HUD draws carry the scene DSV; the tonemap is the no-DSV one),
  after first proving the HUD wasn't in ANY single dump window (dumpframe N
  built - command-armed dumps always open on the same pair phase) and that
  no deferred contexts exist (six new census hooks, all zero). Classifier =
  scene-vote + tonemap detection + non-indexed-after; redirect at draw time
  via the original SetRT; leaks=0 every boot. gameswf's garbage destination
  alpha found the hard way (first composite drew nothing) -> luminance
  repair pass; premultiplied consumers. Window composite at all three
  present exits; head-locked quad + sliders + preset keys; pause menu rides
  the quad. vrhud on|off|force|status.
- **Controls (3)**: User.ini XENON_* ground truth (A=Use B=Heal X=Reload/
  Hack/EVE Y=Jump - the user's "jump is on Y" confirmed); DPAD_UP/DOWN
  flat-proven as AMMO CYCLE (shotgun 00->Electric->Exploding); Touch face
  buttons re-routed to VR convention + right-stick flick ammo switching
  (edge 0.65, re-arm 0.30, cooldown 300 ms, grip-suppressed). README +
  ARCHITECTURE tables rewritten.
- Commits: hideinactive, pitchkill, boot.ps1, dumpframe N + census hooks +
  hud classifier/redirect, quad + composite + alpha repair, bindings +
  flicks, docs. In-headset checklist written; PR + v0.2.0 wait on the user.

- **User report from the long NG+ play session: after a level transition the
  model stopped following, re-enabling did nothing, and the game froze
  briefly on a cycle. Root cause: `scan_for_vtable_object` walked only to
  0x7FFF0000 - the game is Large-Address-Aware, and after enough streaming
  the new level's AHands actor was allocated ABOVE 2 GB.** The log showed
  the smoking gun: "2 vtable match(es), chosen=00000000" every 32 s (the
  part-3 backoff working as designed - each futile scan cost ~4 s, the
  periodic freeze) while the rig visibly rendered engine-placed. Fixed:
  both heap walkers (patterns.cpp + value_scan.cpp) now walk to 0xFFFE0000
  (VirtualQuery simply fails past the top on non-LAA processes). Verified
  on the user's new all-weapons Medical Pavilion save: chosen=5A1862C0,
  bones 47, drive writing, fire smoke clean, dumps 8->8. NOTE: a fresh
  boot cannot reproduce the >2 GB heap state - the mechanism evidence is
  the log + the constant; the user's next long session is the field test.
- The user made THE all-weapons save (NG+ at the Medical Pavilion, full
  arsenal + plasmids + ammo types restored by NG+ at that story point) -
  the permanent test anchor going forward. CONTINUE loads it.
- boot.ps1's success detector is wall-save-specific and prints FAIL on any
  other save - cosmetic, fix next session.

### 2026-07-27 - Session 18 part 3 (trim decoupling + the user's tuning captured; loadout via Nexus save pending)

- **In-headset feedback from the user's first part-2 run**: the ray offsets
  work (they tuned R right-7.9/up+7.5 cm live), BUT re-trimming the aim
  rotation dragged the tuned MODEL with it - the session-11 "barrel follows
  the ray by construction" coupling, now wrong because the model has its own
  per-hand trim. FIXED: the aim calibration trim is no longer applied to the
  model (model = raw aim pose + model trim; ray = raw aim pose + aim trim +
  origin offset). `vrhands aligntrim on` restores the old coupling. Flat:
  with `cal r 0 20` armed the model rot stayed bit-identical (0 116 0);
  aligntrim on moved it exactly +20 deg (3756). NOTE for the headset: the
  user's L trim (17.7 deg yaw) was tuned WITH the coupling live - the left
  hand may need a re-tune under the fix.
- **The user's live tuning was captured to the inis** (they had not pressed
  save): vrpreset.ini now holds aimPosR right-7.9/up+7.5, aimTrimL
  0.2/17.7, aimPosL 0.2/-0.5/-0.7 - "make them in the preset" done; NOT
  baked as code defaults (per-weapon differences pending). The inis on this
  box are now LIVE USER DATA - do not delete them in cleanup anymore.
- **Test loadout: DONE.** The cheat path is closed - `giveall`
  (ShockCheatManager has GiveAll/FillPlasmids) fell off the client+engine
  exec chains and FAULTED the viewport chain (SEH caught; relaunched). The
  Tab console is compiled out (known). The Nexus NG+ save (nexusmods.com
  /bioshock/mods/77) was downloaded by the user, installed as
  `7_21_2024_21_54_13.bsb` (13.6 MB, mtime touched to newest) into
  Documents\BioshockHD\BioShock\SaveGames, and USER-VERIFIED: it loads at
  the Fontaine fight with ALL weapons and plasmids in the LB/RB wheels.
  Boot note: CONTINUE loaded the wall save anyway (it tracks last-played,
  not newest mtime) - pick the NG+ save via LOAD explicitly; the flat
  harness is unaffected. Two observations for the headset run: the shotgun
  (and likely others) is a TWO-HANDED hold mesh - a new composition class
  for the one-controller bone drive - and per-weapon aim variance is the
  open design question (per-weapon profiles keyed by class name would need
  the FName resolution work).
- Dumps 8->8 across part 3.

### 2026-07-27 - Session 18 part 2 (aim-ray origin offsets + the crosshair kill via the engine SET seam)

- Two user asks, both flat-green same day:
- **Aim-ray origin offsets**: per-hand `vraim pos [l|r] <f> <r> <u>` cm,
  "Ray offset hand" selector + 3 sliders, vrpreset persistence. Applied once
  at ray build along the trimmed ray's zero-roll basis; the laser applies the
  identical offset XR-side (same basis convention: right = ray x world-up),
  so beam + bullet + substitution move as one and the model stays put. Flat:
  +60.0 UU right / +45.0 UU up exact, per-hand isolated, subs=2 carried the
  offset, ini round-trip via save -> zero -> apply.
- **Crosshair hidden by default**: the lever is `ShockPlayer.bReticleDisabled`
  (game-side RenderReticle then pushes "NoReticle" to the flash HUD per
  frame), found by reading the script SOURCE TEXT embedded in ShockGame.U
  after UELib failed to decompile the reticle functions. Written via **the
  engine's console SET handler through the exec seam** (`exece set
  shockplayer breticledisabled true` -> HANDLED) - the reusable find: any
  script property is now settable BY NAME with no offset/bitmask, and SET
  writes the class default so it survives load crossings. Shipped
  `vrxhair on|off` + checkbox, default hidden, 15 s re-assert,
  `crosshairVisible` in vrpreset.ini. Flat: clean boot 0 bright center
  pixels (was 19), toggle restores/kills them exactly, dumps 8->8.
- Method note: package source-text extraction needs BOTH UTF-16 alignments
  (odd-phase strings are invisible to an even-aligned decode), and char
  indices in a decoded string are half the byte offset - one extraction ran
  at the wrong offset before the arithmetic was fixed.

### 2026-07-27 - Session 18 (M8 quick phase: both blockers + per-hand offsets + grip fix, all flat-green; release staged)

- Branch `m8-release-quick-phase` off main (post-PR-#2). Five commits: per-hand
  offsets, pace guard, mirror pin, grip-switch latch fix, docs/release.
- **Per-hand model offsets** (user ask): `vrhands pos|rot [l|r]` (no side =
  both), overlay "Tuning hand" selector, per-hand hands.ini keys + legacy
  fallback, `vrpreset save` also writes hands.ini now. Flat: each hand's
  offsets apply only while that hand drives - +40.0 UU exact when driving,
  baseline bit-identical when not; rot trim +5461 units exact for 30 deg;
  both ini formats round-trip.
- **Pace guard (blocker a)**: skip per-present xrWaitFrame once a
  previously-FOCUSED session leaves FOCUSED; bring-up exempt (the state
  machine needs frames to REACH focused); 5 s keepalive as recovery
  insurance; >1 s waits now logged with state. Flat via `vrpace simidle on`
  (state forced VISIBLE + 1 s sleep standing in for the blocked wait):
  guard ON 378-396 presents/s, guard OFF 1/s (the field stall reproduced),
  re-on recovers 416/s. 21345 skips / 15 keepalives across the sim, clean.
- **Mirror pin (blocker b)**: window always shows the LEFT eye - left
  presents snapshot the backbuffer, right presents re-blit AFTER the right
  eye's XR capture; also active with no open XR frame (that path is exactly
  the headset-off case). Flat: within-condition shots at the 0.31-0.73
  floor for BOTH mirror on and off; on-vs-off cross-diff 13.6-13.9 = the
  pin flips the displayed eye; holds == blits (74774 each); stereo
  heartbeat unchanged.
- **The session-17 same-phase clue EXPLAINED** (ENGINE_NOTES session 18):
  under pair pacing the L image is displayed only while the R frame builds
  (~1-3 ms) but R persists through the next blocking wait (~8+ ms), so
  DWM-sourced captures land on R with high probability. Duty-cycle skew,
  not absent alternation - the naive 50/50 bimodal model was wrong, and
  the acceptance numbers match the corrected model exactly.
- **Grip-switch bug root-caused and fixed** (ROADMAP theory confirmed):
  grips compose to bumpers; a bumper press switches the raised hand with no
  trigger event; the auto latch learned only from triggers. Live flat repro
  (LB raised Electro Bolt, latch stayed RIGHT), then fix (latch learns from
  composed bumpers, triggers win same-frame), then flat proof on a clean
  boot: auto(R) -> LB alone -> auto(L) -> RB alone -> auto(R). Status now
  prints the latch (`hand=auto(L|R)`).
- Promoted-build smoke: fire 59->53 for 6 firing pulls with an LB/RB
  round-trip between, dumps 8->8 all session, stereo clean throughout.
- Release staged, NOT published (user gate): README rewritten (install,
  itsloopyo conflict, VDXR setup, PRESET 1, tuning, mirror/pace), zip with
  both RelWithDebInfo DLLs at the session-18 scratchpad. Proposed tag
  v0.1.0 after the in-headset run.
- Harness notes: `2>&1` on a native exe in PowerShell 5.1 still wraps
  stderr as errors (cost one build re-run); test-artifact inis deleted
  after the run per the session-17 vrpreset trap.

### 2026-07-27 - Session 17 part 2 (IN-HEADSET: PASSED; deadzone 23 deg becomes the default)

- The user tested the session-17 build: **"This is perfect... the stick was
  working as expected, the models didn't move when I moved my head while
  looking forward and around that section, when I looked at a specific part to
  the right or left that needed a decent angle also worked as expected."** The
  hard invariant survived the headset - that is the gate M7.5 existed to pass.
- One tuning change, made live by the user and now the shipped default:
  **`vrbody deadzone` 0 -> 23 deg**. Their remaining note before it was "the
  gun moves with the camera a bit"; inside the band the body does not steer at
  all, so a glance leaves the viewmodel world-locked, and beyond the band the
  body trails the head by exactly the band width (head and body never diverge
  more than 23 deg).
- Flat re-verified on the shipping default: `resid` settles at exactly
  23.00 deg; gameYaw and recenterYaw each moved 0.38398 rad (45 - 23 = 22 deg)
  while camYaw stayed 8308 and the hand's world yaw stayed 116 - both
  bit-identical, so the deadzone does not weaken the invariant. Fire smoke
  under stereo clean, dumps 8->8, guardskips 0.
- **Trap found while shipping the default: a `vrpreset.ini` left by a smoke
  test silently OVERRIDES a later code default.** The ini on this box pinned
  `bodyDeadzoneDeg=0.0` and would have cancelled the new default on the next
  preset press. It held nothing but shipped defaults, so it was deleted. Check
  this whenever a default changes.

### 2026-07-27 - Session 17 (M7.5: the body-follows-head yaw transfer ships, invariant exact)

- Work done on branch **`m7.5-body-yaw-transfer`** at the user's request, to
  keep the verified session-16 behaviour on main until they sign off in the
  headset. Two commits; PR after their run.
- **The body facing was found in one boot: `PC+0x1E8`** (the
  PlayerController's `Rotation.Yaw` - AActor layout inherited by AController,
  same +0x1E4 offset the mod already read on AHands and the pawn). A
  read-only `vrbody probe` line plus a `vrbody poke <deg>` one-shot write
  answered all nine probe steps positively: non-zero pitch where the pawn's is
  0, the write lands and holds across 11 samples, the pawn follows for free,
  the engine's turn composes on our value, and a synthetic walk burst followed
  the written facing to 0.4 deg. The milestone was proven before any transfer
  code ran. This overturns the M6-era ARCHITECTURE note rejecting a
  pawn/controller rotation write.
- **The hard invariant held exactly.** Composite `gameYaw - recenterYaw` read
  1.27742 rad at every head angle in both states; at head 45 the hand's world
  pose and the camera were bit-identical off vs on; `[tlm] yawstep` max=0
  units / nbig=0 through the arm transient. The yaw path was converted to
  integer rotator units to make the cancellation exact, and `on_calcview`
  returns the units COMMITTED so the two absorbed quantities are one integer.
- **The feature measured**: transfer OFF, the walk heading changed 0.18 deg
  for a 90 deg head change; ON, walk == body == camera to 0.01 deg at +45 and
  -45, spanning 89.99 deg.
- **Unplanned second payoff**: with the transfer on, the render lock's lateral
  correction goes flat within 0.46 UU across a +-30 deg head sweep instead of
  swinging 10.5 UU, landing on the same 4.57 the calibrated zero-split
  configuration uses. The transfer restores the headset-verified session-16
  regime at every head angle rather than only at head 0.
- **A trap that would have faked a regression, caught before it did**:
  `hands.cpp` forced the simpose lane's recenter yaw to 0, making the parked
  synthetic hand body-locked while a real controller is recenter-locked - the
  gate would have shown the parked gun swinging a full 45 deg and read as the
  sessions-12-16 defect. Fixed; it was a no-op before the transfer existed.
- **Two honest negatives, both recorded in ENGINE_NOTES so they are not
  retried blind**: (1) NCC template chaining across the sweep failed outright
  (correlations 0.56-0.79 at every step) because the composition changes too
  much per 15 deg - every pixel number from it is void, and the exact cull
  angle is therefore still unmeasured. The `[tlm] lock` telemetry is the
  template-free instrument that answers the same question. (2) The cull is
  direction-dependent: a head-only glance with the hand parked in the world
  *increases* hand-vs-body, while the reported case (the user swivels, so head
  and hand move together) drives it to ~0. Both are real.
- Also learned: a walk burst that slides along collision geometry yields a
  plausible-looking heading that is pure geometry (one read 105.65 deg at 16%
  perpendicular deviation). Every burst is now gated on straightness <= 5% and
  a failure is VOID, not a result.
- Promoted to default ON and into VR PRESET 1 (after the viewmodel, before
  vrstereo); `vrbody off` is the live A/B. `bodyRate`/`bodyDeadzoneDeg` persist
  to vrpreset.ini, both 0 = instant 1:1 by the user's call.
- Clean-boot acceptance on the promoted build: fire test 57->51 for 6 pulls,
  dumps 8->8 throughout the whole session, stereo clean (mode=1T, presents =
  2x builds, guardskips 0), preset echo chain + ini round-trip, and a 20-step
  +-90 deg soak returning camYaw and the recenter to their exact start values.
- **Not started, and they are the next session's job**: the two M8 release
  blockers (headset-disconnect stall, flat-screen mirror). The session went
  entirely on M7.5 and its verification.

### 2026-07-27 - Session 16 part 4 (preset verified in-headset; the ROOT CAUSE of the edge desync found by the user)

- The user verified the part-3 build in-headset: "perfect, it works
  perfectly." Head-offset defaults set to 0 at their request (tuned by eye,
  persisted via the preset ini).
- THE UNIFYING OBSERVATION (user's): with the head physically turned, the
  LEFT STICK still walks along the OLD body facing - the body/pawn only
  rotates with the right stick. That one fact ties together the movement
  mapping, the viewmodel-laser desync growing with hand-vs-facing angle,
  AND the rig culling past ~90 deg (all keyed to the body facing).
  Session-17 focus: BODY-FOLLOWS-HEAD yaw transfer (rotate the body under
  an unchanged camera each frame) - fix spec in Next steps item 1.
- New bug report: switching hands via GRIP (instead of trigger) loads the
  incoming hand's model on the WRONG controller - consistent with the hand
  map learning attribution only from the trigger-keyed fire seams; a
  grip-initiated switch never crosses them. Queued in the M8 quick phase.
- **M7-v2 IS DONE - the user's call.** Tracking + head decoupling verified
  in-headset (part 2), size resolved by their worldScale-100 calibration
  (part 3 - the engine-lever scale hunt turned out to be unnecessary, not
  unfinished), effects proven on the driven hand back in session 12. The
  ROADMAP box is ticked; the body-facing coupling is split out as M7.5
  (a camera/locomotion defect, not a viewmodel one) = session 17.
- ROADMAP restructured (user's calls): M7-v2 ticked + M7.5 added (the
  session-17 body/head work with its non-regression invariant as a
  checklist item); selection wheels -> post-v1 (the current switching UI
  is good enough); M8 = release quick phase (first GitHub release + the
  grip bug) then HUD usability (health/EVE visible, keybind audit; the
  gameswf redirect moved up from M9); M9 = comfort + better overlay/config
  UI polish + release follow-up.
- THE NON-REGRESSION GATE (user's explicit requirement, added same day):
  the transfer must NOT bring back the-hand-follows-the-head. The
  controller->world mapping composes through the body yaw, so a naive
  transfer rotates the hand with the chased head - the exact sessions-12-16
  defect. Invariant: the final camera AND the composite controller-to-world
  mapping stay bit-identical; only the body/head-look SPLIT of the yaw
  relabels (the recenter reference absorbs the transferred amount). Proof:
  the parked-hand simhead sweep (gun glued to world) + the full acceptance
  ladder pass unchanged BEFORE the transfer ships.

### 2026-07-27 - Session 16 part 3 (worldScale 100, head offset, per-hand trims, VR PRESET 1)

- The user's own experiment settled the size problem: worldScale 100 makes
  the viewmodel read perfect in-headset (size + distance - angular size and
  stereo finally agree; at 50 the oversized mesh read "too close" by the
  familiar-size cue). Defaulted 100; world reads ~half size, accepted; the
  proper world/viewmodel SPLIT (viewmodel's own stereo basis via per-eye
  bone offsets - design sketched) parked in M9.
- Shipped + flat-smoke-tested: head-anchor offset sliders (up/fwd, default
  up -24 = stand-vs-crouch eye delta; image-verified through the simhead
  drive), per-hand aim trims (vraim cal [l|r] + 4 sliders, one-ray shared
  by laser/bullet/viewmodel), VR PRESET 1 (one button arms the full 13-
  toggle configuration in order, vrstereo last; vrpreset save persists the
  tuned slider values to vrpreset.ini and apply loads them - echo chain,
  ini round-trip, and fire test all verified on a clean boot, dumps 8).
- User reports from this part: the viewmodel edge desync + cull keyed to
  the character facing (root-caused in part 4 - the session-17/M7.5 focus);
  headset-disconnect stalls the flat window under 1 fps (pacing waits while
  the session idles); flat-screen mirror for streaming under stereo (the
  window alternates eyes). The latter two were promoted to M8 RELEASE
  BLOCKERS in part 4 - they affect every user, not just this desk.

### 2026-07-27 - Session 16 part 2 (IN-HEADSET: the core verdict is POSITIVE; the scale wall mapped)

- The user tested the session-16 build in-headset: **"now it's fully
  working, and it's not moving with the head/headset/camera anymore!!
  Which is amazing progress!"** - the model follows the controller with
  head-look decoupled. M7-v2's core mechanism has its first positive
  in-headset verdict.
- A "viewmodel stuck center, ignores the controller" false alarm was
  root-caused live: the VD/Quest menu drops the XR session FOCUSED ->
  VISIBLE and poses stop - identity pose parks the model center. Documented
  in the checklist + ENGINE_NOTES.
- Remaining user reports: model far over life size (hand ~ head, weapon ~
  torso+head); weapon-laser fine alignment (existing sliders, after scale);
  and a laser-crossing anomaly at large right-aim angles (questions + live
  A/Bs issued; investigation queued).
- The scale lever was hunted the same night - THREE flat-proven dead ends
  (cluster bone .s: attach path blows the weapon up from ANY chain scale;
  attach-bone exclusion: identical; rig DrawScale + position pre-divide:
  geometry-inert on the fg path). All reverted; `vrhands scale` prints an
  honest not-yet; the shipping drive is byte-identical in behavior to the
  acceptance build (regression [tlm] reproduced df=17.4 k=1.00 depth=-12.81,
  fire clean, dumps 8). Render-path disasm or vm_draw is session 17's
  opener (ENGINE_NOTES session 16 part 2).

### 2026-07-27 - Session 16 (the decision hour picked branch (a): drive-on pull +11.5, matched lens ships ON, full flat acceptance at k=1)

- The first-hour calibration (lock abs, lockpull 0, matched lens, wall save):
  offset-parallax solved pull +12.0, size-on-distance solved +10.8 - two
  independent instruments within ~1 UU. The driven path's pull is NOT
  fov-coupled (11.5 vs the vanilla path's 65; near the stock-lens 13).
  Branch (a) of the decision tree: no disassembly hunt needed.
- Residuals at lockpull 11.5 measured exactly the 0.9 dgain (no rebake
  amplification on this axis) -> default knob 12.8 lands 11.5 physical.
- The simhead sweep caught a real defect the zero-split A/Bs cannot see:
  the model rotated the eye pull with the camera-delta quat, over-shifting
  the gun by pull*sin(split)*gain (predicted 194/134 px, measured 194/137).
  Fix: matched path rotates the pull by the constant view bias only.
  Post-fix sweep: gun world-glued within 2-17 px over +-30 yaw / +-20 pitch.
- Clean-boot acceptance with shipping defaults (vrfgfov ON, lockpull 12.8),
  all under vrstereo: parallax -156 vs -159 px (0.98x), size 0.475-0.489 vs
  0.465, sweep glued, fire test ammo 59->53 + fresh decal, dumps 8->8,
  stereo heartbeat clean. Option 2 complete flat; headset checklist issued.
- Instrument lessons recorded in ENGINE_NOTES: close-range parallax
  templates are invalid (viewpoint change + the gun's own depth extent);
  big mixed templates false-match on scale (tight disc template + direct
  disc-width profile are the robust size instruments); the fist blob finds
  the statue under teal lighting; simhead recenters on its first pose (arm
  zero first); crosshair-aimed test bullets hide their decal behind the
  parked gun (use vraim test to land one in the open); the boot A-press
  loop can leave the MAP open - screenshot-verify before any series.
- Known edge shipped as-is (probe in headset): hand nearer ~23 cm real puts
  the corrected cluster behind the world camera -> engine culls the rig.

### 2026-07-27 - Session 15 (pivot to source-patching: the fg FOV field found live; the dolly wall mapped)

- User verdict opened the session: three sessions of counter-modeling with
  no felt progress - approach retired. Discussion settled on "patch the fg
  pipeline at its source" (one timeboxed shot) with the draw-replay lane as
  the committed fallback.
- Discovery chain, all instruments now in-tree: derived tan constants have
  NO stored form anywhere (computed per frame; the only scan hit was the
  engine's own trig lookup table) -> cb writer callstacks harvested via the
  new fgstack -> pivoted to property hunting on the live objects via the
  new fginfo/fsweep -> PC+0x460 poked -> the rig re-lensed the same frame.
  Dump: every vm draw across all cb tiers joined the world projection
  cluster. `vrfgfov` ships the write per frame.
- Session-13 errata closed: pawn+0x550/558 is EYE HEIGHT (60/36 stand/
  crouch) - session 11 had it right; the fg fov property lives on the
  PLAYERCONTROLLER; and 101.5 (the value session 13 held at the wrong
  address) is exactly the world-match spec at option 117.
- The wall, mapped precisely: the fg eye dollies back fov-coupled (13 UU at
  60 deg -> ~65 UU matched, vanilla path); no stored field found (22.0 pair
  and 75.0 neighbors poked inert); bones cannot counter it at the matched
  lens (cluster lands behind the world camera -> engine culls the rig -
  the session-14 counter only ever worked because the narrow lens pushed
  AWAY); and the driven rigid path renders a DIFFERENT pull than vanilla
  (parked fist rendered near/huge at lockpull 0) - drive-on calibration is
  the session-16 opener.
- Shipped safe: vrfgfov default OFF; regression smoke reproduced session
  14's exact tlm numbers under vrstereo (k=2.12 wNat=30.4 w*=36.8
  depth=+6.43), fire clean, dumps 8. The matched-lens lock plumbing
  (parametrized invTan, k=1 collapse, lockpull knob) is behind the toggle.

### 2026-07-27 - Session 14 (the depth fix: w* = k*d, flat-stereo acceptance PASSED)

- Implemented the session-13 spec (the depth constraint in render_lock_delta)
  plus the two corrections the measurements forced: the fg eye rides the
  CAMERA translation (dump-proven: `offset 0 30 0` moved the recovered eye
  29.7 UU), and the model's absolute pull-back is the physically-calibrated
  13 UU (`kFgEyeFwdBehindCam`) - the dump-recovered E turned out to be
  section-frame-relative (each vm section recovers a different eye) and its
  -32 forward was 1.63x the real scale.
- Baselines replicated BEFORE judging the fix (lock off, same harness:
  parallax 420 px, size ratio 0.605 - matching session 13's 425/0.62), then
  the acceptance measured: parallax 355 px vs 341 world-correct (1.04x, was
  1.23x); size 0.465-0.470 vs 0.465 exact; depth band clean at wSolve 142
  (FOV 137 + hand out 40 cm - clamp fallback never fired); simhead
  -30/0/+30 yaw and +-20 pitch sweep glued (no rotation regression); fire
  test 57->55 with a fresh decal, dumps 8->8, stereo 150+ pairs/s clean.
- Session 13's "rigid-rebake doubling" DECOMPOSED: the 1.79x gain-1.0
  overshoot = (model depth-scale error 1.63) x (true rebake ~1.1). Gains are
  now per-axis (`lockgain` lateral / new `lockdgain` depth), both default
  0.9 ~ 1/1.1, both on the overlay.
- Instrument findings recorded in ENGINE_NOTES: camrot does not rotate the
  fg view with the drive on (rigid path orients by the actor view) - simhead
  is the only valid flat head-look stand-in; window captures are
  eye-phase-locked (disparity unmeasurable from shots; size/parallax carry
  the same geometry); with lock ABS the shot series sat pixel-frozen where
  lock-off wobbled +-16 px (sway possibly part-absorbed by the re-pin).
- Harness cost of the session: three false trails from template matching
  (stale gun regions after the composition moved at the new gains;
  multimodal correlation surfaces). Settled by blob/crop-and-look absolute
  localization. Recorded as caveat (d) in the ENGINE_NOTES session-14 block.

### 2026-07-26 - Session 13 part 3 (the depth geometry confirmed flat, fix specified)

- User correction: the rig IS 3D in the headset - just pinned near the face
  and under-responsive to hand distance. That killed the zero-disparity
  variant and focused the depth-geometry one.
- Confirmed flat under stereo with two no-code A/Bs (details in Next steps):
  camera-offset parallax puts the rig's stereo depth at ~14 UU (~28 cm) vs
  the hand's true ~20 UU, with 1.35x over-response to camera translation;
  hand-distance doubling shrinks the gun 0.62x vs the correct 0.5x. The fg
  eye follows camera TRANSLATION (the session-13 model's actor-anchored E is
  wrong for translation - E rides the camera; the 12 mono dumps could not
  distinguish because camLoc == actorLoc in all of them).
- Fix specified for next session: one constraint change in the existing
  solve (fg depth w* = k * trueDistance instead of keep-natural) makes size,
  stereo depth, and parallax world-correct at once. Flat stereo acceptance
  numbers defined (gun parallax 425 -> ~310 px; size halves with distance).
- The user's offered in-headset protocol (periodic screenshots + telemetry
  while they move on a script) stays in reserve in case the flat acceptance
  and the headset ever disagree again.

### 2026-07-26 - Session 13 part 2 (headset verdict: no change felt - the suspect moves to stereo depth)

- **In-headset result of the render lock: none felt.** Head-look still
  counter-moves the gun, gun/laser same direction different speeds, gun huge
  and near the face, hand-distance motion attenuated to "very little
  increments"; the user's framing: "like the hands are part of the HUD on my
  face, but a bit 3d". User calls for deeper investigation before more
  changes, and STEREO-ONLY testing from now on.
- **Why the flat pass and the headset disagree, with data**: the overlay
  after their run read lock |delta| 1.7 UU (flat sweeps ran 6-11) - the
  position term the lock corrects is proportional to the hand's OFF-CENTER
  angle, and in the headset you look AT your hand. The flat harness measured
  exactly the term that vanishes in real use. Meanwhile mono screenshots
  cannot show disparity, perceived depth, or reprojection behavior at all -
  which is where every remaining symptom lives.
- **New hypothesis set (Next steps above)**: zero per-eye disparity (fg eye
  anchored in ACTOR space -> camera translation, and therefore the per-eye
  offset, never reaches it), compressed/pinned fg depth response, and the
  size lens - one geometric story matching every report since session 11.
  The 12 model dumps all had camLoc == actorLoc so they cannot answer the
  anchor question; the camera-offset discriminator (attempted, blocked by
  the game being paused) answers it with two screenshots at gameplay.
- No code changed in part 2 (the eye-phase capture experiment showed the
  1 Hz screenshot lane cannot resolve eye phase past the hand sway; a
  consecutive-present pair dumper is the right instrument, queued).

### 2026-07-26 - Session 13 (the camera-coupled rig term: root-caused flat, countered, fire-tested)

- **The 2-shot discriminator opened the case in ten minutes**: hand
  world-parked via bones, camera still, `gfov 100` vs `gfov 137` - the world
  rescaled, the gun HELD. The rig renders through its own projection. Frame
  dumps then gave the exact constants: every foreground draw carries a
  screen-ray block (2tanH, 0, -tanH | -2tanV, tanV) = tanH 0.7698 / tanV
  0.4330 at BOTH world FOVs - a hard 60-deg 4:3 spec (tanV = tan(30)*3/4),
  while world draws track the option exactly (0.7002 -> 0.3939).
- **The property hunt found ForegroundFovAngle and proved it a NON-lever**:
  BakedScripts name tables show Engine.Controller.ForegroundFovAngle /
  PlayerController.DefaultForegroundFOV / PlayerWeapon.
  ZoomedForegroundFOVAngle; the live group sits on the PAWN at +0x550
  (60.0 default) / +0x554 (36.0) / +0x558 (lerped current). A poke seemed to
  rescale the gun, but holding +0x558 at 101.5 per frame changed NOTHING in
  the render (dump-proven) - the engine lerps the property back and the
  renderer's constants are built elsewhere. Recorded in patterns.h so the
  alley stays closed.
- **Matrix decode nailed the full pipeline**: the vm draws' cb0 carries the
  rig transform; decoding it at simhead 0 vs 30 showed the foreground VIEW's
  eye parked at a FIXED point in ACTOR space ~32 UU behind the rig origin
  (E recovered from 12 dumps: (-32.1, -5.6, -0.9) +- 0.5) with orientation
  following the camera plus the hand sway (+-1.7 deg wobble around a +1.7/
  +1.1 deg bias). Self-consistent at view center; under head-split the rear
  pivot translates the rig - the user's "translation-about-a-pivot" call was
  exactly right.
- **Two capture-based fix attempts failed for a REASON worth keeping**: a
  generic Map/Unmap cb watch (new in frame_inspector, kept as a diagnostic)
  captures the live vm matrix - but with the drive writing bones the engine
  switches rig sections to a rigid path whose per-section matrices REBUILD
  from OUR driven bones. Solving against a captured matrix is a feedback
  loop (monster-fist fixed point, frame-alternating oscillation - both
  observed). Same rebake also means any bone correction lands on screen
  roughly TWICE.
- **The shipped counter**: bones.cpp "render lock" - the analytic foreground
  model solved as a 3x3 per frame (anchor onto the world-correct pixel,
  natural fg depth kept), applied at gain 0.5 for the rebake doubling.
  Verified flat: anchor within 2-4 deg of world-true at simhead -30/0/+30
  yaw and -20 pitch, mono; the 15-25 deg coupling is gone. Modes abs/diff +
  lockgain for in-headset taste. Fire test 57->54 with a fresh decal, dumps
  8->8; vrstereo on smoke + fire clean.
- **Harness note for the record**: the g5 acceptance series burned three
  builds' worth of false starts on capture variants - the boot loop
  (launch -> A-presses -> save check -> double trigger) is now fully
  scripted and each cycle costs ~4 min.

### 2026-07-26 - Session 12 part 2 (first headset run of the bone drive + the head-coupling fix)

- **User verdict on the first bone-drive headset run**: controller tracking
  and rotation correct ("a metric ton better"), but the model still followed
  the HEAD and sat too close to the eyes; gun therefore not yet 1:1 with the
  laser.
- **Root cause, isolated FLAT with a new tool**: `camrot <p> <y> <r>` rotates
  the render camera only (CalcView out-params + frame context, never the
  pawn) - the flat equivalent of head-look. It showed the renderer orients
  the first-person rig by the RENDER CAMERA rotation, not the actor rotation
  field the drive had been composing against; the two only differ under
  head-look, which is why every flat test had passed.
- **Fix**: bones.cpp composes the target against the frame context's final
  camera rotation, anchored at the actor's location field. Camera-pitch and
  camera-yaw A/Bs both land on the correct-frame prediction; fire test
  re-passed (subs=2, dumps 8->8).
- **Method lesson recorded**: the first "discriminator" run used a `rot`
  command that does not exist in the seam - it echoed nothing, did nothing,
  and its "result" was misread as evidence. Command echoes are now a
  mandatory check before trusting any A/B.
- **Second headset report ("still moves with my head, REVERSED") led to the
  real remaining defect: a STEREO EYE MISMATCH.** Under SR stereo the second
  CalcView pass runs the engine's view update (re-evaluating the skeleton
  over the pass-1 bone write) but skips the drive body by design - so the
  right eye baked the ENGINE pose while the left baked ours. Flat-proven on
  a clean boot (bone array read back the engine idle pose every frame while
  the drive wrote; the driven gun vanished from the stereo present). Mono
  never showed it: the drive runs on EVERY CalcView call there and the last
  write wins. Fix: drive() caches its writes, the second-pass branch calls
  `bones::reapply()` (100 ms freshness gate). Flat-stereo verified clean
  boot + stereo fire test passed. The "reversed" motion reads as binocular
  rivalry between the two mismatched guns; third headset run pending.
- Mono flat A/Bs on the fixed build also settled the frame questions
  properly: camera-position offset -> the hand parallaxes like a world
  object (position anchored right), camrot pitch/yaw -> world-anchored
  hand with the fc.cam composition (rotation frame right).
- **Part 3 - telemetry ended the composition-frame saga with data.** Third
  headset report: still coupled, REVERSED. Built `vrbones log` (5 Hz [tlm]
  samples of the whole chain: raw head + controller XR poses, recenter,
  camera, actor, world target, bone state); the user ran a scripted movement
  protocol (still / yaw / pitch / roll / lean / controller-only). Verdict
  from the data: the world TARGET is perfectly head-decoupled (2 deg wobble
  during an 80 deg head sweep) and the ACTOR rotation NEVER carries head-look
  (constant 1.4 deg through the sweep). So the renderer's rig frame is the
  ACTOR, the ORIGINAL composition was right all along, and the fc.cam "fix"
  was the reversal the user felt. Reverted to actor-frame composition (bone
  values now head-independent end to end); the reapply/eye-consistency fix
  stays - it was the REAL cause of the first run's "follows my head, close
  to my eyes" (one eye engine pose, one eye ours = rivalry). The fc.cam
  detour traced back to misreading one flat screenshot (g3: the hand HAD
  panned with the world = actor-frame evidence, read as its opposite).
  Flat-stereo re-verified + fire test passed; fourth headset run pending.
- **Part 4 - the bug is REPRODUCED FLAT, no headset needed (user's idea).**
  Fourth headset run unchanged, so a synthetic HMD lane was built: `simhead
  <yaw> <pitch> <roll>` drives the whole camera path (recenter, additive
  yaw, stereo passes) exactly like a headset, flat. Result: with the hand
  parked and the bone array PROVEN byte-identical between head 0 and head
  30 (telemetry), the rendered gun still over-pans the world by ~10-15 deg
  - in MONO too. So the RENDERER applies a camera-coupled term to the
  first-person rig that the actor fields do not carry; the memory-side
  chain (target, composition, eye consistency, layer poses) is fully
  exonerated - those fixes were real but sat downstream of the symptom.
  This also likely explains session 11's actor-pinning "model going faster
  than the aim". Eliminated: eye offsets (mono repro), pair pacing (no
  session flat), bone values (constant), actor fields (constant), the
  compositor (flat repro!). Also caught: the earlier "mono is clean"
  reading was an invalid test - the simpose hold's silent 120 s cap had
  expired the drive. Next: parametric simhead yaw/pitch sweeps to fit the
  term's functional form (linear-in-yaw? tan? which pivot?), then a
  principled fix at its source, or a calibrated counter-term composed into
  the drive using dyaw - all iterable flat now, no headset time needed.
- Two PC power cuts (electricity, unrelated) punctuated the session; no work
  lost either time (recon results and commits were already on disk/pushed).

### 2026-07-26 - Session 12 (M7-v2: the bone drive, built and flat-verified in one day)

- **Method pivot at planning (user's call: goal-first, any stable method)**:
  skipped the ROADMAP's render-side steps 1-2 and went straight for the bones.
  Two design-review passes fed the plan; the engine-side one scanned the disk
  image's native table offline and found the full bone API kept from UE2 plus
  Vengeance's LOW/HIGH dual skeleton, `SkeletonInstanceFreeze`, and
  `execSetDrawScale` (-> the REAL DrawScale at +0x2AC with a dirty protocol).
- **Offline impl walks reached the skeleton before any boot**: actor +0x3FC ->
  `SkeletonInstance` -> component-space hkQsTransform array (+0x48, 47 bones),
  evaluate-if-dirty flag, freeze flag. RTTI + factory walk confirmed the class.
- **One pokeaddr session proved everything**: bone writes render the same
  frame, freeze holds them, per-bone independence (component space), and the
  headline - poking the weapon-attach bone (43) moved THE ENTIRE GUN,
  undistorted: the attached weapon renders from this array. Engine-side lever
  found; ENGINE_NOTES "Skeleton / bone internals" has the full map.
- **bones.cpp shipped** (mode bones = new default; policy stays in hands.cpp):
  rigid cluster move about the anchor bone, dirty-flag clear after write,
  self-refreshing reference (animations ride along when the engine evaluates;
  no feedback when it does not), per-hand sleeve collapse via zero bone scale.
- **Flat ladder all green on the first build**: simpose park + yaw/pitch/ROLL
  series rotate about the GRIP (screenshots); fire test subs=4, ammo 57->53,
  off-crosshair decal, dumps 8->8; left cluster measured with Electro Bolt
  raised (wrist 6, fingers 7-21); **plasmid parity PROVEN** - FX at the driven
  hand with the drive on, snap back on off, live cast fires through the
  anim-notify chain with the drive running.
- Session interrupted mid-way by a power cut (PC-side, not the game); Steam
  needed a manual re-sign-in. No state lost - the recon evidence had already
  decided the design.
- Commits: bone drive + skeleton notes, enlarged dumpframe capture (b0 256 ->
  1344 B), left cluster + per-hand collapse. All pushed to main.
- Note for the record: the remote history was force-rewritten by the user
  (identity change to mohamad-balouza); local work was cherry-picked onto the
  new history before pushing.

### 2026-07-26 - Session 11 part 3 (second headset run + the M7 replan)

- **Second in-headset verdict: still wrong.** "The model is going faster than
  the aim" - rotating the hand right sent the gun right faster than the laser -
  and crucially "doesn't matter if I change the offset forward at all, instead
  it creates other problems and the other problem stays the same". That killed
  the lever-arm correction as a fix and, with it, the whole actor-pinning
  approach.
- **Inspection tests, all with a new trick worth keeping**: pin the rig with
  `vrhands simpose`, then fly the CAMERA around it with the `offset` command -
  the frame context publishes the pre-offset camera, so the rig holds still in
  the world and we get a free orbit rig. Established: the viewmodel renders in
  world space (camera orbits it, it stays put), **the geometry is complete on
  every side** (spun 0/90/180/270 - full right side, muzzle, cylinder, properly
  gripping hand, no deleted faces), both arms are always in the mesh, and a
  90 deg roll about the eye anchor throws both forearms horizontal while the
  gun and its gripping hand stay correct relative to each other.
- **Confirmed BioShock 1 shows ONE hand at a time** (own captures: Electro Bolt
  frames contain no gun). The user dropped dual-wield to the BioShock 2 adapter
  on hearing it.
- **The user's spec, now the M7 definition of done**: weapon one with the right
  controller, plasmid hand one with the left, wrist roll matching a real wrist,
  arms hidden if that helps, and no shipping a working weapon with a broken
  plasmid hand. Explicitly not wanted: bent arms, elbows, IK, two-handed grips.
- **Replanned M7 to the draw/bone level** (ROADMAP "M7-v2", rationale in the
  ARCHITECTURE decision log): find the viewmodel draws -> prove skip + matrix
  substitution -> reach the bone matrices -> write hand bones from the
  controllers with forearms collapsed. Dropping arm articulation removes the IK
  solver entirely, which is what makes it tractable.
- **The principle that decided the architecture**: engine-side writes (actor,
  bones) let attachments and effects follow for free; render-side matrix
  patches do not, so separately-drawn FX stay behind. Hence bones for anything
  with attachments, render-side only for scale and projection.
- No code changed in this part - design only. Tree is at the session-11
  evening commit; game restored and left at the save.

### 2026-07-25 - Session 11 part 2 (evening: first M7 headset run + the fixes)

- **User verdict on the morning build**: laser "awesome, keep as is"; both
  hands track their controllers; but the model placement "goes crazy" on
  controller rotation and no slider could fix it, and the gun is far too big.
  Mid-session the user proposed the right architectural idea themselves: stop
  fighting the flat-screen viewmodel, drive the gun MODEL directly.
- **Diagnosis, all confirmed live**: (1) model on GRIP pose vs laser on AIM
  pose = constant large tilt; (2) euler-add trim only valid at one controller
  orientation - the "pivot that breaks everything"; (3) mesh gun ~1.2 m from
  the AHands origin (the eye anchor, per the Hands.UpdateLocation decompile) =
  huge rotation lever, and the user's slider caps (30 cm) could not reach the
  correction (their saved attempt sat pinned at -30/-30/-36.5).
- **Shipped fixes**: model aligns to the AIM ray + aim trim (one ray for
  barrel, laser, bullet), local-frame quaternion trim (`ue_math.h` quat_mul /
  axis-angle / xr_local_trim_quat), offsets to +/-120 cm, `vrhands simpose`
  (synthetic XR pose through the REAL mapping path - the flat lane that found
  half of tonight's bugs), config gains mode/pose keys.
- **Dead ends recorded honestly**: driving the WEAPON actor (perfect pivot,
  sits exactly at the visible gun) does nothing - the renderer draws attached
  actors from the attach matrix; `vrhands mode gun` is inert-by-design until
  the DETACH experiment (weapon Base pointer -> AHands found at +0x450). Full
  pivot correction via offsets is impossible - the engine culls the rig once
  its origin passes behind the camera. The morning's "DrawScale +0x16C" claim
  was WRONG (it is a hide/cull-style field; +0x168 is visually inert; no scale
  field confirmed yet) - the scale command now says so instead of writing.
- **Two red herrings that ate the evening, now in ENGINE_NOTES/TESTING**: the
  lowered/equip pose (until the first trigger pull the pistol idles pulled-in
  and centered - looks exactly like a placement bug) and poked-state
  contamination (a boot that has taken live-field pokes stops being evidence -
  clean-boot before judging).
- **Final build flat-verified on a clean boot**: simpose series places and
  rotates the rig sanely, fire test with the drive live passed (4/4 subs, 0
  new dumps, crash count 8 all session). Pushed to main. Game left running at
  the save for the user's second headset run.
- Session hygiene note: the evening's first probes ran inside the user's OWN
  running session before that was noticed (their overlay tuning state made it
  obvious in hindsight) - a few pistol rounds and pokes happened in it, then
  it was cleanly replaced by fresh boots. No saves were written.

### 2026-07-25 - Session 11 (M7 hands + weapons + laser, on `main`)

- **The M6 loose end went first, while the game was already up, and it
  overturned a session-10 finding.** The plasmid is steered by the ROTATOR
  out-param, not by the hand-origin substitution. Under a real Electro Bolt cast
  the ability path's B carries the camera's own FRotator, exactly like the weapon
  path - session 10's "all-zero" reading was a one-off. The decisive A/B was to
  turn origin substitution OFF (so only the rotator is written) and flip the sign
  of the injected yaw: with +20 deg and origin on the bolt's scorch landed right
  of the crosshair, with -25 deg and origin off it landed left. That kills
  planned step 5 (chasing the damage factory at factory vtbl +0xEC) outright.
- **M7's central question answered in one probe: the viewmodel is ONE actor and
  its transform fields are the placement.** `vrhands probe` reports 3 vtable
  matches - one live `AHands` whose Location/Rotation are an exact per-tick copy
  of the camera (distToCam 0.0), plus two stack false positives full of
  `0xCCCCCCCC` debug fill, which distance-to-camera rejects cleanly.
- **The expected ordering fight never happened.** The plan budgeted for a
  hunt-the-placement-code fallback (AHands natives, disassembling the writers of
  actor+0x1D8, hooking the Tick slot). None was needed: the engine places the
  viewmodel during its own tick and the CalcView detour runs afterwards, so the
  plain field write from `hands::on_calcview` is simply the last one of the
  frame. Flat proof by screenshot - a 60 UU push moved the pistol from the lower
  right into the centre, +30/-30 deg of injected yaw swung it out of frame right
  and left.
- **`game/bioshock1r/hands.cpp` shipped** (`vrhands on|off|probe|hand|pos|rot|
  writerot|save|reload|test|testclear|status`): lazy vtable lookup with cache
  revalidation and a world-change clear, the same cutscene guard the aim ray
  uses, SEH-guarded writes that drop the cached pointer if one ever faults, and a
  `vrhands test <yaw> <pitch> [distUU]` synthetic lane so the write can be
  verified with no headset at all. Offsets are overlay sliders (position in cm in
  the grip's own frame, rotation trim in degrees) persisted to `hands.ini`.
- **The laser is built and is the one thing flat testing cannot touch.** Up to 8
  XR quad layers along the aim ray, geometrically spaced, each billboarded at the
  head at constant angular size, fed by a 64x64 CPU-generated soft dot with
  premultiplied alpha. It takes the aim pose and the SAME trim the fire ray uses,
  so the beam and the bullet are one ray by construction. Quad layers live only
  in the compositor and no session exists without a headset, so "arms without
  crashing" is the honest limit of what was proven.
- **Refactors that paid for themselves immediately**: `frame_context.h` now owns
  FrameContext and the XR-pose-to-game-space mapping (aim.cpp and hands.cpp share
  one transform - the whole point of M6/M7), and the UShockUserSettings heap scan
  generalized into `patterns::scan_for_vtable_object`, which is what finds
  AHands.
- **Flat verification, run three times (baseline, hands build, laser build)**:
  the mandatory fire test passed every time - game alive, 0 new crash dumps (the
  count stayed at its pre-existing 8 all session), 4/4 substitutions, decal off
  the crosshair where the injected aim asked. The final battery had aim + hands +
  laser armed together, then `vrstereo on` for a soak at ~170 pairs/s with
  guardskips 0.
- Harness note: `firetest.ps1`'s `LClick` (raw `mouse_event`) fired nothing this
  session - `calls=0`. The reliable path is the documented synthetic trigger,
  `vrinput test trig r 255 400`, remembering that the FIRST pull only switches
  hands. New scratchpad `trigfire.ps1` wraps it.
- Nothing user-facing has been in a headset yet; the checklist above is the
  handoff.

### 2026-07-25 - Session 10 (M6 part 1, branch `m6-decoupled-aim`)

- **Investigation first, and it paid off.** Parsed ShockGame.U's name table
  directly (UTF-16 names with a positive char count, 8-byte flags on this
  licensee build), which surfaced `LastTraceFireStart`, `ApplyAimError`,
  `GetPerfectFireStart`, `AutoAim`... Then found the engine's **native-function
  symbol table**: every name-based native has a `.data` entry
  `{ "int<Class>exec<Func>", impl, 0 }`. Enumerated all 1822 entries offline
  (scratchpad `natives.py`/`nativemap.py`) and shipped
  `pattern_scan::find_native_function` so the mod resolves natives with zero
  hardcoded addresses. All five session-10 lookups hit their documented RVAs on
  the first boot.
- **Headless decompiling now works**: UE Explorer's portable build ships
  `Eliot.UELib.dll`, driven from PowerShell by `tools/uscript/dump.ps1` (<1 s to
  load ShockGame.U; lists classes/functions/states, decompiles by name). Two
  quirks recorded in ENGINE_NOTES (name encoding, and ~40% of UFunctions failing
  to deserialize on this build - the rest are plenty).
- **The fire flow, mapped end to end** (ENGINE_NOTES "Fire flow / aim"):
  trigger -> `Weapon.BeginFiring` -> `Firing` state -> anim notify ->
  `AttackAbility.UseAbility` -> native `InitiateDamage` (AWeapon 0x226050 /
  UAttackAbility 0x1BBD80) -> `GetPerfectFireStart` (0x226840 / 0x1BC220) ->
  damage factory. Plus the AActor field layout (+0x1D8 Location, +0x1E4
  FRotator Rotation, +0x550 eye height) cross-checked against a live pawn
  hexdump, and the RTTI vtables for AShockPlayer/APlayerWeapon/UAttackAbility/
  AHands.
- **Dead end recorded honestly: the exec thunks.** The first build hooked all
  four aim `exec*` thunks; a full live session of swinging, casting and clicking
  produced ZERO calls, because native callers reach the C++ implementation
  directly. Rebuilt against the implementations (weapon side by reading the
  RTTI-derived vtable slot +0x304, ability side by a prologue-checked RVA) and
  the ability seam fired on the very next cast.
- **`game/bioshock1r/aim.cpp` shipped** (command-gated `vraim on|off|probe|dump|
  origin|seam|test|scan|scanimpl|scanoff|status`): per-hand rays built inside the
  CalcView frame from XR grip poses, `ue_math.h` extracted so the camera and the
  aim ray can never drift apart, ownership gates using the engine's own
  instigator check, value-driven out-param substitution (positions get the
  hand's origin, directions get the hand's direction, zeros untouched) so
  `ApplyAimError` still applies the weapon's spread on top, and self-expiring
  `vraim test l|r <yaw> <pitch>` synthetic aim for headset-free testing.
- **Core plumbing**: `core/vr/openxr_input` locates the (already existing)
  grip-pose action spaces at the frame's predicted display time - the same
  instant as the head pose - and `vr::get_hand_pose` exposes them with the same
  mutex/no-XR-stub shape as `get_head_pose`. `xinput_bridge` publishes the last
  composed triggers, which is what seeds hand attribution.
- **Live results**: ability seam fires on Electro Bolt (`this` vtable =
  UAttackAbility), hand map learns from the trigger ("learned LEFT-hand
  (plasmid) object"), ORIGIN substitution proven (`SUB(L)` with our values).
  `GetPerfectFireStart` turns out to fill POSITIONS only - so the trace
  DIRECTION lives one layer deeper, in the damage factory. `APawn::
  GetViewDirection` and `AShockPlayer::GetViewPoint` implementations were probed
  and are never called during a shot (ruled out).
- **Two harness lessons** (now in TESTING): the FIRST trigger pull only switches
  hands (`XENON_LT/RT = SwitchAndFireAbility/Weapon`), so single synthetic pulls
  look inert; and the wrench never traces (Havok collision phantom), so it is
  useless as a fire-path control - the weapon seam needs a ranged weapon, which
  the only available save does not have.
- New investigation tools that remove rebuild cycles: `vraim scan <Class>
  <Func>` (any name-based native, read-only) and `vraim scanimpl <rva>
  <stackArgs>` (any C++ implementation, one detour family per arity so the
  callee-pop stays correct).
- **(late, same session) THE WEAPON AIM IS DECOUPLED - flat-verified.** The user
  supplied a pistol via the trainer and saved the game with it (so future
  sessions can iterate solo). With the probe logging out-params as ints, B
  revealed itself as an **FRotator** matching the camera rotation - rotation
  units reinterpret as float denormals, which is why it had been printing as
  `(0.000 0.000 0.000)`. Substitution now classifies each out-param by value and
  writes the matching type. Wall test, camera stationary throughout: injected
  hand aim of +12 deg yaw, -12 deg yaw, -10 deg pitch and +8/+8 each put the
  bullet decals exactly where asked, and `vraim off` put the next round back on
  the crosshair. Seam counters: 9 calls / 9 substitutions / 0 skips.
  Left-hand plasmids are unchanged (their rotator out-param is all-zero - the
  ability direction comes from the damage factory, next session's target).
- Harness note: a `command.txt` written with PowerShell's
  `Set-Content -Encoding utf8` gets a **BOM**, which corrupts the first command
  token and is silently ignored by the poller. Use `tools/game-cmd.ps1` (it
  writes via `File::WriteAllText`, no BOM). Also: with `vrinput` off, synthetic
  pad presses are inert - drive menus with a real `VK_RETURN` on the highlighted
  item instead.
- **(late, same session) IN-HEADSET USER-VERIFIED: "it's pretty good... the
  plasmids are working and it's based on the left hand which is very good."**
  Both hands aim their own fire while the camera stays on the HMD - M6's core
  goal, confirmed by a human. Two follow-ups from that run: (a) calibration sat
  low, because the ray used the OpenXR GRIP pose (handle axis) - the build now
  uses the runtime's AIM pose plus pitch/yaw trim sliders, pending an in-headset
  check; (b) the user wants a visible laser from the hand, which is the rung-2
  reticle work (design in Next steps). Hands still do not track the controllers -
  expected, that is M7.
- **(late, same session) A crash the user hit, and the process lesson.** The
  aim-pose build crashed on the first shot in the headset. Root cause found from
  the dump in one pass (EBP walk + our-DLL disassembly, no PDB needed): an
  unbounded string replace in a patch script had injected the new overlay
  widgets into `substitute()` as well as `draw_debug_ui()` - the anchor line
  appeared in both - so firing called `ImGui::Checkbox` from the GAME thread with
  no current window (`GetCurrentWindow()` null deref, fault address 0xBE).
  Fixed by removing the stray block; the ImGui-only-in-draw_debug_ui rule and a
  mandatory pre-handoff FIRE TEST are now in TESTING. The rebuilt version is
  flat-verified: overlay opens clean, 6 shots with substitution active, 0 new
  dumps, decal lands off-crosshair where the injected aim asked.
- Session ends: game closed, DLLs installed, branch `m6-decoupled-aim` pushed,
  PR opened for review. Of ~10 boots, one crash - the ImGui-on-game-thread bug
  above, caught, diagnosed and fixed the same session.


### 2026-07-25 - Session 9

- **Synthetic-XInput lane BUILT + FLAT-VERIFIED (3 commits).** commit 1
  `core/input/xinput_bridge`: composes an XINPUT_GAMEPAD from an XR slot +
  self-expiring seam test slots, merges over the real pad (buttons OR /
  triggers max / larger-magnitude axis / bridge-owned packet counter),
  registers the proxy `@200` post-hook and a `vrinput on|off|status` +
  `vrinput test ...` command grammar. commit 2 `core/vr/openxr_input`: one
  "gameplay" action set (move/look/fire/plasmid sticks+triggers, grips w/
  hysteresis -> bumpers, A/B/X/Y, stick clicks, menu short/long START/BACK,
  grip poses for M6), oculus/touch_controller bindings + khr/simple fallback,
  attached per session, synced once per eye pair from Present-head, published
  to the bridge. Mapping table + decision log in ARCHITECTURE.
- **Two blockers discovered live and worked around (ENGINE_NOTES "Gamepad
  architecture").** (a) The game reads the pad ONLY in
  `UWindowsViewport::UpdateInput` (RVA 0x853D20, found via XINPUT1_3 import-
  thunk walk + RTTI), which NOTHING calls in windowed mode - the game probes
  GetState ~6x at boot and never re-polls (WM_DEVICECHANGE, ini
  UseJoystick/UseController, pad-connected global all inert until UpdateInput
  runs). (b) The **Steam overlay code-hooks our proxy's export thunk** and
  eats GetState before the proxy body/post-hook runs. Fix: `game/bioshock1r/
  input_drive` drives UpdateInput once per present + flips the engine's own
  `UWindowsClient::SetUseController`, and the bridge re-points the game's IAT
  ord-2 slot at a composing wrapper (Steam chain kept as passthrough). Sticky
  opt-in via a marker file read at DLL attach (covers the boot probe).
- **Flat proof (no physical pad):** menu highlight moved on synthetic dpad;
  synthetic A activated CONTINUE and loaded the save; right-stick yawed and
  left-stick moved the live camera (heartbeat deltas > 50 UU); `vrinput off`
  froze it and restored the real DEVICE_NOT_CONNECTED result + frozen packet.
  `vrinput status` lane counters: `iat` climbs at ~2x present rate while armed,
  `proxy` stuck at the 6 boot-probe calls (Steam eats that lane).
- **Console-command seam (M6 groundwork).** In-game Tab console is compiled out
  of this Steam build and key-bound commands are inert, so the mod calls the
  engine's own Exec dispatchers directly: `exec`/`execc`/`exece` ->
  UWindowsViewport / UWindowsClient / UGameEngine Exec with a stub
  FOutputDevice. Native engine handlers are HANDLED; the script-command path
  falls off the links reachable safely, and a player-object vtable-slot-65 call
  unbalanced the stack (CRT check-fail modal) so it was REMOVED. Dispatcher
  RVAs + the FExec-subobject `this` note (engine+0x40) in ENGINE_NOTES. 3rd
  commit: exec seam + crash-lane removal. (Test loadouts for the combat check
  were supplied out-of-mod; nothing cheat-related is part of the mod.)
- Harness/tooling: scratchpad capstone scripts for the import-thunk walk,
  RTTI vtable resolution, vtable-slot indexing, SEH-prologue function-start
  finder, and an SEH fault-address capture that pinned the engine-Exec
  `this`-offset bug (needs the FExec subobject at engine+0x40).
- **IN-HEADSET USER-VERIFIED (2026-07-25): "controllers are working perfectly
  as expected."** Full Quest 3 Touch mapping correct (move/look/fire/plasmid/
  grips/buttons/menu), combat feel good with a starter loadout. Rebinds wanted
  later (parked M9). Aiming is right-stick with a head-tracked crosshair as
  expected - M6 decoupled aim replaces it. Session ends: fixed DLL installed,
  all commits pushed.

**In-headset flow (session 9 - now the standard controller bring-up):**
1. Quest 3 on, Virtual Desktop connected (VDXR), Streamer running. Launch
   BioShock from Steam, load a save.
2. Tick **"VR input (motion controllers as gamepad)"** in the F10 overlay's
   Input section (or `.\tools\game-cmd.ps1 "vrinput on"`). Log shows
   `input drive: armed` and the on-screen prompts flip to Xbox icons. Sticky
   across boots (marker file re-arms it; `vrinput off` clears).
3. Tick **"VR stereo"** (or `vrstereo on`) for the full VR view. Headset on.
4. Mapping (documented in ARCHITECTURE "Controller mapping"): LEFT stick moves,
   RIGHT stick looks, RIGHT trigger fires weapon, LEFT trigger fires plasmid,
   grips cycle weapon (R) / plasmid (L), A/B/X/Y + stick-clicks pass through,
   LEFT menu short = START, hold = BACK.
5. EXPECTED (not failures): aim is head-driven with a stick-aimed weapon
   (decoupled aim is M6); HUD renders in both eyes; hands/weapon not yet
   visible (M7). Bail-out: `vrinput off` + `vrstereo off`, or close the game.

### 2026-07-24 - Session 8

- **(late addendum) IN-HEADSET USER VERIFICATION of the one-toggle flow -
  PASSED**: "Alright looks good... I just care that it's turning on with a
  simple toggle instead of doing commands. What we have now is good." One
  non-blocking bug found and parked to M9 at the user's request: in-game
  `vrstereo off` alone does not fully disengage (needs the top "VR enabled"
  checkbox off too), and after that, re-arming in-game only re-engages after a
  quit-to-menu round trip. Recorded in the M9 polish list with debugging leads.
- **(late addendum) Model-switch code audit**: re-verified the flush-point
  detour against the disasm, all format strings, and the calling conventions;
  found + fixed ONE bug (commit 3ce7f3b - the overlay checkbox's pending
  request could swallow a second toggle posted in the same frame), and
  live-verified the checkbox path end to end (was command-path-only before).
- **Step 0 - flush-point disk disasm (capstone, scratchpad).** Re-walked
  0x61D260..0x61D400 from the exe: prologue `55 8B EC 51 8B 4D 0C`, `ret 8`,
  and the INLINE branch confirmed exactly (arg1 -> [mgr+0xC]; arg2's 16 dwords
  -> [mgr+0x10..0x4C]; decision chain -> eax; [mgr+0x50]=eax, [mgr+0x54]=1;
  eax==0 -> `mov ecx,esi; call 0x61CAE0` then straight to the epilogue - no
  post-drain work). Matched the session-7 decode byte for byte.
- **Structural 1t SHIPPED (commit f27a0d0).** `FlushPointDetour` reproduces
  that inline block itself when `g_forceInline` is armed and mgr is non-null,
  calling the drain THROUGH its hooked target (guard + telemetry stay live),
  SEH-guarded (poison + auto-disarm on fault), falls through to the original
  when mgr is null (pre-world). `reentry 1t` repointed at it (no poke); legacy
  poke moved to `reentry 1tpoke`. render_is_threaded() honors the override;
  heartbeat gains `forced/s`, status/overlay gain `1t=hook|poke|off`. Constants
  in patterns.h (kFlushPointRva/prologue/kMgr* offsets), full derivation in
  ENGINE_NOTES "Structural 1t".
- **Flat verification - baseline + soak PASSED.** In gameplay: `1t on` (hook)
  -> mode=1T, drain caller RVA inside bioshockvr.dll (expected), presents
  continue, numerator still 12. `stereo on` -> 239 pairs/s = 478 presents/s all
  on the game thread, guardskips 0, eye-offset img-diff 1.96 vs 0.40 floor,
  phase-consistent 0.40; ~3-min stationary soak (200 heartbeats, 0 anomalies).
- **LOAD-CROSSING - THE HAZARD IS CLOSED.** With 1t + stereo armed end to end:
  (1) in-game save load via LOAD; (2) quit-to-main-menu teardown; (3) new-game
  load (Bink intro -> in-water intro); (4) the bathysphere DESCENT into Rapture
  (real multi-map streaming transition, loc crossed +75000 UU with 1t forced
  the whole way). Zero crashes, zero new dumps, guardskips 0, stereo re-engaged
  on arrival. The session-7 poke crashed a loader on step 1; the hook survives
  all four because the numerator global is untouched.
- **One-toggle `vrstereo on|off` SHIPPED (commit 1a821a0)** - top-level command,
  `reentry vrstereo`, and overlay "VR stereo" checkbox; sequences 1t -> camera
  mode -> stereo, reverses on off, sticky across loads. Overlay checkbox posts a
  request the game thread applies from note_calcview (outside hooked calls - MH
  installs must not run mid-build). New core setter `vr::set_camera_mode`. The
  1t menu refusal was DROPPED (load-proven; pre-world arming is inert).
  Flat-verified: `vrstereo on` at the MENU armed all three (`VRSTEREO READY`),
  a CONTINUE-load carried into Rapture with stereo doubling live and no re-arm,
  `vrstereo off` restored mode=MT / build==presents / drain on the render thread.
- **Perf profile:** Rapture arrival scene ~81 pairs/s = 162 presents/s sustained
  (79-83 typ, one dip to 62), eye-offset 6.5; lighthouse spawn 225 pairs/s. Both
  clear the 72-pairs/s (144 presents/s) M4 target. Combat scene still untested
  (needs a combat save).
- Harness notes: PowerShell `mouse_event` dx/dy must be signed `int` (negative
  turns threw UInt32 cast errors); reused game-hover.ps1 (real WM_MOUSEMOVE for
  gameswf highlight) + game-move.ps1 (relative turn + held key) in scratchpad.
  The user drove the game to the lighthouse/Rapture at points to save time.
- Session ends: game closed, command.txt cleared, DLLs current, all commits
  pushed. New streamlined in-headset checklist below.

**In-headset stereo checklist (session 8 - the ONE-TOGGLE flow; loads no
longer need any off/on dance):**
1. Quest 3 on, Virtual Desktop connected (VDXR runtime), Streamer running.
2. Launch BioShock Remastered flat from Steam (no launch args). If the
   "failed to properly shutdown... revert Options?" dialog appears, click No.
3. `.\tools\game-cmd.ps1 "vrstereo on"` (or tick "VR stereo" in the F10
   overlay's Reentry section). The log must say `VRSTEREO READY (1t=1
   stereo=1)`. You can do this at the MENU or in gameplay - either is fine now.
4. Load your save via CONTINUE (or LOAD). Stereo stays armed across the load and
   re-engages automatically in-game - no commands needed. (At the static menu
   there is no doubling; it starts once gameplay builds run.)
5. Headset on. Verify: real per-eye parallax at full rate, depth correct, world
   solid on head turns, comfort as in session 7 (pair pacing is on; toggle "SR
   pair pacing" in the overlay for a live A/B).
6. Loads are now safe with it armed - load another save or cross a level
   transition freely; stereo persists. EXPECTED (not failures): HUD in both eyes
   on a HUD-bearing spawn, IPD/world-scale not yet calibrated.
7. Bail-out any time: `.\tools\game-cmd.ps1 "vrstereo off"` (full recovery to
   flat threaded) or just kill the game - saves are safe. NOTE: command.txt
   re-applies at boot; the session cleared it, but if you send `vrstereo on`
   and then quit, clear it (or send `vrstereo off`) before the next launch so
   it does not re-arm at the menu.

### 2026-07-24 - Session 7

- **Minidump forensics closed the null-deref in one pass** (scratchpad mdparse.py -
  hand-parsed MiniDumpNormal: streams, x86 contexts, module list, EBP walk +
  ret-scan with module attribution; pdbsym.py - dbghelp symbolization of our DLL
  frames; disasm.py - capstone disk walks). All three 2026-07-24 evening dumps
  decoded: two drain+0x33 specimens fault on the render PUMP thread entering the
  drain with `[this+0xC]` (submitted-frame slot) NULL; the third is the recorded
  0x1375BD4-poke load crash at 0x741D7F. The "onethread" crash specimen had a live
  pump thread + game thread parked in the deadlock wait inside a hooked build +
  watchdog kick frames - a threaded-mode deadlock-then-kick crash, not an
  onethread defect.
- **Session-6's onethread substrate falsified live**: the arg rode the command
  line into the process (WMI-verified) while the drain ran on a hot pump thread
  (96 s CPU), and "onethread" is not in the exe's strings - never parsed. The
  session-6 hexdump verification was a menu-time artifact: the pump globals are
  zero before the first world load in EVERY mode (watched them flip 1T -> MT at
  the save load, same boot).
- **Fix #1 (commit bc1f575)**: drain empty-slot guard (skip `[this+0xC]==0`
  drains - the crash state), stereo substrate gate (`stereo on` refuses threaded;
  `force` overrides), `mode=MT|1T` heartbeat tag, forensic constants in
  patterns.h. The gate proved itself the same session: it refused the first
  post-load stereo attempt (mode had silently flipped to MT).
- **The real single-threaded switch (commit 503a695)**: full flush-point decision
  chain decode -> the hw-thread quotient pair (12/1, 10 refs total, 7 identical
  inlined tests) is the only path to the threaded hand-off. `reentry 1t on` =
  guard first, then poke numerator -> 1. Verified same boot: mode=1T,
  beatTid==calcTid, drain caller 0x61D367 (inline site), submits stop in mono,
  pump never kicked, guardskips 0. Presents 413/s mono (~20% under threaded).
- **Stereo flat verification on the inline substrate - ALL PASSED**: 225 pairs/s
  = 450 presents/s (range 141-532 across scenes), presents exactly 2x builds,
  eye-offset diff 2.03 vs 0.33 floor, phase-consistent captures (0.43), 5-min
  stationary soak (15/15 clean) + ~6.5 min synthetic PLAY in two passes (10 + 3
  clean cycles; WASD/mouse navigation, camera climbed the lighthouse stairs,
  21-35 mean img-diffs between cycles; pass 2 ended by the session wrap, not a
  defect - user waved off further passes) - zero faults, zero new dumps, zero
  watchdog detections. `stereo off` recovery instant (2nd=0, builds 1:1 with
  presents, still inline). Previous best on the threaded substrate was
  16 s-3.5 min to a hang or crash.
- Harness lesson recorded: a stale `command.txt` re-applies at boot (the poller
  saw session-6's "reentry stereo on" and armed everything at the menu) - clear
  it or overwrite before launching for controlled runs.
- **FIRST IN-HEADSET FULL-RATE STEREO TEST - PASSED** (user drove it): real
  per-eye parallax at full rate, depth correct, world scale good - "pretty good
  and working as intended, we can continue". Two follow-ups: (a) eyes feel weird
  on head movement; (b) HUD not seen (that spawn has no HUD).
- **xr-frame-per-pair pacing built for (a)** (commit e90765c): per-present
  xrWaitFrame located each eye of a pair at a different predicted time while both
  images came from one head sample -> motion-dependent reprojection shear + halved
  tick. Now a LEFT present holds the XR frame open and the RIGHT completes it: one
  waitFrame/locate/prediction per pair. Default on, overlay toggle for live A/B.
  Flat regression clean; comfort is the next headset check.
- **Load-path crash found + guarded (same commit)**: arming 1t (or doubling)
  across a save load crashed a loader thread (ntdll EnterCriticalSection null CS,
  load-path stack, no mod frames) - the hw-thread global + the doubled build have
  load-path consumers. Guards: `1t on` refuses at the menu (verified live: the
  refusal fired), warns to off-before-load, and pass 2 doubles ONLY gameplay-caller
  builds. Regression: menu refusal OK, in-gameplay arm clean, 3-min stereo soak
  clean, stereo-off then 1t-off restored threaded mode. The 19:54 dump is that
  crash (recorded, not committed).
- Session ends with the updated (order-corrected) headset checklist below, game
  closed, all commits pushed.

**In-headset stereo checklist (updated session 7 - ORDER MATTERS: load first,
THEN arm 1t):**
1. Quest 3 on, Virtual Desktop connected (VDXR runtime), Streamer running.
2. Launch BioShock Remastered flat from Steam (no launch args - `-onethread`
   does nothing; remove it if set).
3. **Load the save via CONTINUE FIRST** (before arming anything - a load with 1t
   active crashes the loader). Confirm the F10 overlay works.
4. In PowerShell (repo root): `.\tools\game-cmd.ps1 "reentry 1t on"` - overlay/log
   shows `mode=1T` (or `render 1T` in the overlay reentry line). If it says
   "1t refused: no world loaded yet", you are still at the menu - load first.
5. Enable "VR camera mode" in the overlay; confirm the layer line reads
   `projection`.
6. `.\tools\game-cmd.ps1 "reentry stereo on"` - the log must say
   "STEREO ON (single-threaded render)". Put the headset on.
7. Verify: real per-eye parallax at FULL rate, depth correct, world solid on
   head turns - and THIS TIME whether head movement feels right (pair pacing is
   on; toggle "SR pair pacing" in the overlay for a live A/B if it still feels
   off).
8. EXPECTED imperfections (not failures): HUD visible in both eyes (on a
   HUD-bearing spawn), IPD/world-scale not yet calibrated. Game tick should feel
   less halved than before pair pacing.
9. **Before loading another save / changing level**: `.\tools\game-cmd.ps1
   "reentry 1t off"` first (then you may `stereo off` too). Bail-out any time:
   `reentry stereo off` recovers instantly; worst case kill the game - saves are
   safe. NOTE: command.txt re-applies at boot - clear it or send the off commands
   before quitting so stereo/1t do not re-arm at the next menu.

### 2026-07-24 - Session 6

- **DR-5 CLOSED with the full arc in one session**: hook the submit -> prove the
  double-submit is absorbed (presents never double, yawed camera never renders - the
  session-5 seam hypothesis refuted by its own probe) -> follow the submit's live
  caller RVA into the disk image -> find the scene BUILD root 0x4CCE70 -> double-call
  it -> a complete second engine-paced frame per game tick with our camera on it,
  yaw-30 visible in flat captures. Every step flat-harness-verified.
- **Tooling upgrade that cracked it**: installed capstone (pip) + scratchpad PE/disasm
  scripts - the disk-image walk found the submit's true statics (arg2 IS the FRotator*,
  ECX dead, literal ret 0xC, SetEvent site/event object), the decoy 55-8B-EC function
  at 0x4CCD20, the CC-run boundary, and the build's aligned-stack prologue in minutes.
  Hand byte-walking is retired for anything bigger than a spot check.
- **scenedraw extended**: build + submit hook slots (fastcall-passthrough detours for
  `ret 0x10`/`ret 0xC` targets), `dump <n>` per-call arg telemetry with presents-delta,
  `arg3` filter, submit-nested-in-build counter, build-slot priority on the double-call
  controls. Two incremental code commits pushed before the docs wrap.
- **Live numbers (gameplay, save spawn)**: submit 1:1 with presents (single site
  0x4CDD8A; load path uses 0x4CC6C8; static menu: zero - session-5's "3.7x at menu"
  kick figure did not reproduce). Build pass-through ~60 us; doubled second call
  ~2 ms. Continuous doubling: 225 build/s -> 450 presents/s, tick halves, off recovers
  instantly. Noise floor 0.33-0.37; yawed-frame diff 7.7-7.9 mean.
- **Honest stability record**: ~3.5 min continuous doubling clean, then one hang
  (~124k doubled frames, during a game-shot focus cycle; kill + relaunch). Recorded in
  TESTING with hardening candidates queued into the per-eye design.
- **Curious + useful**: flat captures phase-lock to the SECOND present of each doubled
  pair - present order within a pair looks deterministic, which simplifies per-present
  eye attribution for the split.
- Session ends with the game closed (killed post-hang), DLLs current in the game
  folder, no headset items this session.
- **(part 2, same day) SequentialReentry STEREO wired end to end** on user go-ahead:
  `reentry stereo on` = doubled builds with L/R eye offsets (pass-1 cached-base +
  pass-2 replay - one head sample per pair), SPSC eye-tag ring game->render
  (`vr::sr_push_eye`), per-present eye capture into the AER swapchain pair,
  mono/AER paths untouched. Flat-verified: offset frame renders (diff 2.0 vs 0.33
  floor), captures phase-consistent (0.35). ARCHITECTURE decision-log entry written.
- **(part 3, user go-ahead "let's do it") The fix hunt, every step live-tested**:
  watchdog built (depth-gated stall detection + engine event re-kicks) - detection
  perfect, recovery kicks CRASH desynced state (demoted to detect-only, `wdkick`
  opt-in); flush-point head fully disassembled -> its first mode check `[0x1375BD4]`
  turned out to be a 500-ref GIsEditor-class global (poke = load crash, dead end
  recorded); then the win - **`-onethread` launch arg boots the native
  single-threaded renderer**: deadlock class structurally gone, FASTER than
  threaded (630-710 fps), stereo doubles cleanly on top (194 pairs/s)... until a
  rare drain+0x33 null-deref crash (~1 per 23k pairs, the recurring 0x40-fault
  signature). Minidump preserved - next session symbolizes it. Eye tags moved to
  the build detour (mode-agnostic ordering). Session truly wrapped here: game
  closed, 5 code commits + docs pushed.
- **(part 2) The deadlock hunt**: five continuous runs hung (16 s - 3.5 min).
  Built hangdump.py (outside-process Wow64 thread dump) - two specimens show the
  IDENTICAL signature: game thread in WaitForSingleObject(INFINITE) at exe+0x61D38E
  (render-done flag+event wait in the build) vs render thread waiting inside the
  drain. Decoded the frame-id completion-bit pair (0x13AF7E8/+0x10, high bit =
  consumed; a wait-for-minus-one first guess throttled to 48 fps and was corrected),
  the queue ring pointers (+0x118/+0x11C, unequal at idle) vs seg counters
  (+0x128/+0x12C, equal at idle), and the flush-point wait function itself
  (~0x61D340: flag-then-INFINITE-wait, same event class as the pump kick, plus a
  single-threaded inline-drain fallback). Start-state gating falsified twice at
  full doubled rate; the pass-2 gate (frame-ids + counters, bounded, skip-on-
  timeout) is kept as the graceful unfocused degrade. Three concrete deadlock fixes
  queued in Next steps; stereo stays command-gated experimental, not headset-safe.

### 2026-07-24 - Session 5

- **DR-5 probe ran its full arc in one session**: hook the presumed frame root ->
  discover it never fires -> re-aim at the drain -> map the real two-thread architecture
  -> refute render-side re-entry -> locate the game-thread submit seam. Every step
  flat-harness-verified; two clean 40 s+ soaks; the one crash was the intentional
  SEH-guarded drain pulse (poison latch worked; the subsequent event-protocol wedge and
  hang is recorded in TESTING.md as expected probe cost).
- **Corrected architecture (ENGINE_NOTES rewritten)**: game thread builds + submits
  (camera by pointer into globals, SetEvent kick at submit+0x1A2); render thread pump
  loop 0x61D1D0 (thread main, registers its tid in a global) drains once per Present
  (drain/s == presents/s exactly); 0x61D0F0 is a flush/join, not a frame function.
  Session-4's prologue walk overshot the pump's frameless `push esi` entry - lesson:
  the CC-55-8B-EC heuristic misses frameless functions.
- **New instruments that cracked it**: process-wide SetEvent caller sampler
  (`reentry kick on|off`) - one 8 s sample separated the game-thread kick (0x585C68)
  from a 3-worker job pool (0x583FDB); one-shot game-thread stack scan
  (`reentry calcstack`); per-boot import resolution via shared system-DLL bases
  (WaitForSingleObject/SetEvent/GetCurrentThreadId/ReleaseSemaphore/
  RtlTryEnter+LeaveCriticalSection all pinned to IAT slots).
- **Submit function byte-walked** (entry 0x585AC0, ret 0xC, camera loc/rot through
  args/globals, CS-guarded buffer swap) - constants + prologue bytes staged in
  patterns.h for next session's hook. Double-submit with a yaw delta = the remaining
  DR-5 experiment.
- Probe v1 (frame-root) and v2 (drain + kick/calcstack) both committed and pushed
  incrementally; core gained `present_count()` and `draw_call_census()` accessors.
- Harness notes: gameswf CONTINUE needed VK_RETURN (clicks only highlighted - recorded
  in TESTING.md); today's standing-still noise floor measured 0.21-0.40 mean.

### 2026-07-24 - Session 4

- **(end of session) IN-HEADSET VERIFICATION PASSED**: auto-claim solid with the manual
  slider untouched; gfov 137 "very good" in the headset. The session-4 FOV work is fully
  user-verified; session closed with DR-5's hook probe as the next opener.
- **FOV endgame closed.** Built the value-scanner seam (memscan/mempoke/memptr/hexdump/
  strscan + int variants) and img-diff.ps1; narrowed 662 int candidates to 4 by having the
  user change the FOV option through the game UI between rescans; poke + screenshot A/B
  found the consumed copy; RTTI walk named it `UShockUserSettings` (+0x8C int32). Two traps
  documented: the ini value is an INT (float scans blind), and the one .data "static root"
  was a coincidental range hit later overwritten by floats - resolution is now a heap scan
  for the fixed-RVA vtable (cached, revalidated per call, SEH-guarded after one boot crash
  from unguarded reads during heap churn).
- **Auto-claim + gfov landed**: CalcView reads the option per frame into the projection
  claim (manual slider demoted to override); `gfov` writes it per frame with save/restore;
  Force-headset-FOV now writes the real control. Flat A/B: 137 renders wider than 130
  (UI cap is UI-only), restore returns to noise floor. In-headset check = user checklist.
- **DR-3 via in-tree frame inspector** (new core/gfx module; RenderDoc never installed):
  context-vtable hooks on draw/clear/SetRT slots, one-shot lite/full dumps with RT descs,
  VS b0 readback, triple-source callstack RVAs, auto-summary + lifetime census. Frame map
  in ENGINE_NOTES: HDR main pass, half-res effects pass, shadow pair, view-proj in VS b0
  bytes 128-191 (m00 scaled EXACTLY as 1/tan(hfov/2) between 117/137 dumps - independent
  proof the option IS the rendered fov).
- **DR-5 groundwork - architecture finding**: byte-walk of the draw-stack functions shows a
  render COMMAND QUEUE: executor 0x61C8E0 (`void __thiscall`, type id at this+0xC), drain
  loop 0x61CAE0 (site 0x61CD0D), frame root 0x61D0F0 (site 0x61D21E; its call rel32
  byte-verified to the drain). Consequence: SequentialReentry must re-enter the command
  BUILD, not the drain. Hook probe deferred to next session (fresh session; this one had
  two unrelated PC power cuts - user's electricity, not the mod).
- Debug-CRT lesson recorded in TESTING: sprintf_s asserts with a MODAL on overflow (froze
  the game mid-scan); value_scan switched to _snprintf_s/_TRUNCATE. Crash filter upgraded
  to log module+RVA + fault address. tools/game-cmd.ps1 added (focus-safe seam writes -
  the poller only runs while the game window is focused).

### 2026-07-23/24 - Session 3

- **M4 rung 1 (AlternateEye) landed** per the session-2 design handoff: per-eye swapchain pair
  in `core/vr` (index 0 still serves quad/mono), held stale image + stored per-eye pose with
  compositor reprojection, eye sign published after submit with capture-gated flip (the sign
  doubles as image attribution - the un-offset enable frame stays mono instead of being
  mislabeled), `vr::current_eye_sign()` consumed by the CalcView drive for the half-IPD
  view-right shift. New overlay controls: AER checkbox (camera mode only), Swap-eyes
  inverted-depth diagnostic, `(AER eye L/R)` layer tag, IPD slider, head-offset UU telemetry.
  Also fixed in passing: the no-OpenXR stub block was missing `set_rendered_hfov` (latent link
  error).
- **Flat smoke test PASSED live** (game launched/closed via Steam under standing permission):
  log shape identical to session 2 - scan 1 candidate at RVA 0x1BE7A0, hook + heartbeat at
  menu, VDXR instance up, quiet no-headset retry, graceful exit. In-headset AER test = user's
  next step (procedure in TESTING.md, checklist in Current state).
- **First AER in-headset test (still 1024x768)**: AER mechanics verified - `layer: projection`,
  eye L/R tag tracks per frame, depth NOT inverted (1-frame sign attribution holds, swap-eyes
  not needed), no crash. BUT the M3 distortion persists (center-stretch relaxing toward the
  periphery on slow head turns) and blocks parallax/immersion/scale judgment; IPD ruled out as
  the cause. Diagnosis: claimed-vs-rendered fov mismatch that the self-echoing readback cannot
  correct. Landed **"Manual claimed FOV" calibration slider** (claims an arbitrary hfov in the
  projection layer; swim stops when claim == truly rendered fov -> the locked value measures
  the engine's real fov); layer line now shows target/readback/claimed. Calibration procedure
  written into TESTING.md; deriving + baking the engine fov mapping = next step.
- **(07-24) Swim calibration SUCCEEDED and found the cause**: at 1920x1080 the world locked
  solid at claimed ~100 with the field forced to 137, and stayed solid at field=100 unforced ->
  renderer caps hfov at ~100. Live ini: remaster settings pin `HorizontalFOV=100` under
  `HorizontalFOVLock=True` (+ `bHorizontalFOVLock=True`) -> ENGINE_NOTES updated (the PC+0xE0
  readback echoes writes instead of the rendered fov whenever the lock clamps). User also
  reported the expected honest black band at the bottom (uncovered headset fov below center) -
  grows as claimed fov shrinks; closes only when the engine truly renders wider. Landed:
  force-headset-fov **default OFF** (truthful auto-claim = solid world out of the box), manual
  claim slider kept per user request. **Flipped both PC FOV-lock ini flags to False** (backup
  `Bioshock.ini.bvr-bak-fovlock`) - unlock verdict is the user's next flat-screen test.
- **(07-24, later still) M4 RUNG 1 USER-VERIFIED.** With the video FOV option at 130 + manual
  claimed fov at 130: "everything is now perfect... the parallax and other stuff are very
  nice." M3 done-when ticked as well (6DOF verified session 2 + geometry verified today).
  Depth not inverted (swap-eyes never needed - the 1-frame sign attribution holds). Open
  follow-up parked by the user: whether the IPD slider has a perceptible effect (note in
  Current state: at 50 UU/m the 55-75 mm range is a ~0.5 UU change - verify with an
  exaggerated value before concluding). Session wrapped here.
- **(07-24, later) FOV endgame - field retired, real control found.** User reported the FOV
  override slider dead on flat with locks off. Built an automated harness in response: 1 Hz
  `command.txt` seam in the detour (`fov`/`offset`/`recenter`) + `tools/game-shot.ps1`
  (PrintWindow captures) + `tools/game-click.ps1` (synthetic menu clicks; navigated the menu
  and triggered a save load with them). Flat A/B sweeps: field writes 60-140 = pixel-identical
  frames in real gameplay AND the menu attract scene, under BOTH lock states -> PC+0xE0 is
  telemetry-only; DR-4's "override widens view" retracted (VR claim-side artifact - user
  clarified it was only ever observed in-headset). Locks restored True; direct ini
  `HorizontalFOV=137` also did nothing (out of range). **User found the real control: the
  remaster's FOV video option, range 75-130, visibly drives the render on apply.** Field does
  not mirror it -> manual claimed-FOV slider must match the option value for now;
  settings-object scan queued to automate the claim and attempt >130. Payoff run queued:
  option 130 + claim 130 + AER.

### 2026-07-23 - Session 2

- **M3 second in-headset test**: 6DOF drive solid; Force-headset-FOV visibly changed the
  image; user cannot judge immersion/world-scale without per-eye stereo -> M4 AlternateEye
  promoted to next session's goal (full design in Next steps). A started AER refactor was
  reverted cleanly (session wrap; HEAD == installed build). Session totals: 13 commits -
  DR-4 complete + user-verified, M2 complete + user-verified (VD path), M3 code landed with
  diagnostics, M1 down to DR-3/5/6/7.
- **M3 first in-headset test + fix round**: drive worked; distortion/no-depth/scale reports
  led to: projection-readiness gate (mixed quad+drive state now impossible), rendered-fov
  readback claims (fisheye self-correction), resize-swapchain-recreate fix, layer/fov
  diagnostics in overlay + log, Force-headset-FOV escape hatch.
- **M3 code landed (same session, after the M2 pass)**: core/vr locates head pose + per-eye
  views at Present-head (VIEW-space xrLocateSpace + xrLocateViews), computes the circumscribed
  headset FOV, and swaps quad -> projection layer in camera mode. camera.cpp converts XR
  meters/quat -> UU/FRotator (conventions documented in-file, roll sign pre-validated by the
  user's slider test), drives loc/rot/FOV in the detour, adds World scale + Recenter. Pose
  crosses threads under a mutex; pull-based access (see ARCHITECTURE decision log). Flat path
  re-verified live. In-headset M3 test = next session's first item.
- PowerShell 5.1 gotcha hit: double quotes inside git commit -m here-strings mangle argument
  passing - use a message file + git commit -F for multi-line messages with quotes.
- **User verification pass (end of session): M2 VD path + DR-4 both PASSED.** In-headset: big
  screen appeared, gamma OK, sliders work, clean flat fallback. In-game: wobble, offsets, yaw,
  FOV override all visible. No stutter/crash/input issues. M2 lacks only the optional Steam
  Link cross-check; roll-offset visual check noted as a small open item for M3.
- **M2 stretch landed after DR-4**: `core/vr/openxr_runtime.{h,cpp}` ports the xr_hello32 flow
  in-process (instance at init; lazy session on the game device with 5 s retry; event pump;
  quad swapchain sRGB-preferred; Present head = waitFrame/beginFrame, tail = CopyResource +
  endFrame; kill switch + screen sliders in the overlay). Verified live: instance on VDXR
  1.0.10 inside the game, quiet no-headset retry, DR-4 hook and game unaffected. In-headset
  test = user's next step (TESTING.md M2 procedure written).
- Found + recorded: game boot pauses while the window is unfocused (foreground it in tests).
- **DR-4 landed**: studied itsloopyo's `memory.rs`/`engine_hook.rs` (MIT), ported the FName-chain
  scan as generic `core/hooks/pattern_scan.{h,cpp}` (module capture, wide-string/imm32 sweeps
  via VirtualQuery region walk, E8 -> 89 0D global extraction, CC CC CC 55 8B EC prologue walk,
  200-byte init-site filter), consumed from the new `game/bioshock1r/patterns.cpp`.
- Created the `IGameAdapter` seam (minimal, grows per milestone) + `Bioshock1RAdapter`;
  framework wires `game::init_adapter()` between MinHook init and the D3D11 hooks; every
  failure path fail-soft.
- Hook detour (`__fastcall` dummy-EDX): original first, relaxed-atomic telemetry, offset/wobble/
  FOV-override application, one-shot first-fire log + 1 Hz heartbeat (default on). Overlay draws
  the adapter section through the seam.
- **Smoke-tested live twice** (game closed/relaunched via Steam with user's standing permission):
  scan resolved RVA 0x1BE7A0 both runs (rebased base 0x0FB20000 - ASLR active, scan
  relocation-transparent), hook fired at menu, heartbeat 400-7800 calls/s. Recorded in
  ENGINE_NOTES: fires at main menu, call rate >> fps, `AActor**` signature correction, FOV 100.0
  read live.
- 4 code commits + this docs commit pushed incrementally to main.

### 2026-07-23 - Session 1 (continued)

- User confirmed the overlay visually (screenshot: main menu, 500 fps). Toggle key changed
  Insert -> F10 (user's keyboard lacks Insert). Verified live by the user.
- Em-dash ban added (global user preference): they broke PowerShell 5.1 parsing (BOM-less
  UTF-8 read as ANSI) and mojibaked the log and ImGui text. Repo swept to ASCII hyphens.
- **DR-1 retired with a FULL PASS**: wired the OpenXR loader (static, CRT override needed -
  see ENGINE_NOTES), built xr_hello32 (32-bit), found VDXR ships a 32-bit runtime, and with
  the Quest 3 connected ran a complete session: Meta Quest 3 system, FL 11_0, RTX 4060 LUID
  match, 60 frames pumped. VDXR path proven; 64-bit companion fallback not needed.

### 2026-07-23 - Session 1

- Researched game installation (32-bit DX11 Vengeance/UE2.5, no DRM, gameswf Flash UI), engine
  family (BS2R same engine; Infinite UE3-6829), modding ecosystem, prior VR art (vorpX G3D works;
  itsloopyo headtracking hook proven; REFramework MIT reference), and the VR injection stack
  (OpenXR-first for VDXR+SteamVR). Full findings â†’ RESEARCH.md.
- Decided architecture (C++20/x86, xinput proxy â†’ bioshockvr.dll, core+adapter split, stereo
  ladder with SequentialReentry as primary bet) â†’ ARCHITECTURE.md decision log.
- Built: repo + docs suite, CMake (VS2022 `-A Win32`, submodules minhook/imgui/OpenXR-SDK pinned),
  xinput proxy (ordinals verified against the real SysWOW64 DLL with dumpbin - game imports @2/@3),
  mod DLL (deferred init, logger, minidump handler, MinHook, kiero-style Present/ResizeBuffers
  hooks, ImGui overlay), tools scripts.
- **In-game smoke test passed** on first run (full init chain + D3D11 device info in log).
  Found + fixed: logger file locking (fopen_s denies sharing â†’ switched to `_wfsopen` with
  `_SH_DENYNO`), non-ASCII mojibake in log lines, missing `-Install` passthrough in build.ps1.
- Verified: LAA=YES; D3D11 confirmed at runtime; user ini path confirmed after first launch.
- Repo created and pushed: https://github.com/mohamad-balouza/bioshock-vr (public, MIT).
