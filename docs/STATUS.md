# Project status

> Handoff file. Rewrite "Current state" and "Next steps" every session; append to the session log.

## Active projects (two, running in parallel)

| Project | Branch | Handoff |
|---|---|---|
| **BS1 + BS2 (Vengeance/UE2.5)** | `main` and `sNN-...` | "Current state" below, ladder in [ROADMAP.md](ROADMAP.md) (M0-M10) |
| **BioShock Infinite (UE3)** | `bioshock-infinite` | "Infinite: current state after session 59" below, ladder in [bioshockinfinite/ROADMAP.md](bioshockinfinite/ROADMAP.md) (I0-I11) |

**Standing rule (2026-07-31, session 34):** never run BioShock Infinite while `Bioshock2HD.exe` is
running, and vice versa. Only one game can own the headset at a time. Building, installing,
packaging and tailing logs do not contend and must keep working while either game runs. Enforced
for `-Game bsi` by `tools/lib/assert-no-conflict.ps1`.

The Infinite "Current state" lives here and in its session-log entry rather than displacing the
section below, so the two projects' handoffs do not fight over the same lines while both are active.

### Infinite: current state after session 59 (THE OWN-RIG HOLD DISCRIMINATION - HEADSET ACCEPTED 2026-08-13 and merged; **v0.8.0 RELEASED same night** - Infinite's first public build; the tattoo-poster NON-HOLD beat deferred to the roadmap)

**v0.8.0 SHIPPED (2026-08-13, from `bioshock-infinite` @ e6e568f):**
https://github.com/mohamad-balouza/bioshock-vr/releases/tag/v0.8.0 - one zip,
three games (new `preset-bsi` folder; README gained the three-games table;
package.ps1 extended). BS1 + BS2 sanity-checked in the sim on the release
branch before tagging (BS1: alternate-eye stereo live, HUD quad, camera hook
healthy; BS2: stereo live, 3 quads, aim dots 2/2 - both branches contained:
origin/main and origin/bioshock-2 are ancestors of the release commit).
Release notes in docs/RELEASE_NOTES.md; Discord announcement text in
dist/discord-v0.8.0.md (untracked - regenerate from RELEASE_NOTES if lost).
Infinite is labeled EARLY ACCESS: tested range = game start through the early
city; Skyline/late chapters/DLC unswept. Known-issues list (user-supplied +
backlog) is in the notes: loading-area artifacts (the tower bells), model
jank, vigor-only hand position, aim/model calibration unfinished, the
tattoo-poster beat, reload glitch, FOV-edge drift, FX-origin family.

**HEADSET VERDICT (user, same night): "most of the things are perfect" -
vigor drink shows the authored hands (release ~166 ms after hold-open),
doors single-handed, executions intact, the long multi-phase hold played
all three gate branches live (hide -> show at the hand moment -> re-hide on
the game's bHidden park), the intro chain clean. ONE exception: the
tattoo-poster beat still hides the hand - the player CAN WALK during it, so
it is a NON-HOLD beat the gate never sees (deferred with full mechanism
notes to ROADMAP I9; likely the empty-hand hide eating the authored raise
after the beat's ForceUnequip). s59 is CLOSED and merged.**

**Session 59 (2026-08-13) closed the s58b missing-hands regression at the
mechanism level, flat. The s53 cine-hold rig hide is now PER-HOLD (cine
mode `auto`, the new default): hide-first at every hold-open (the doubles
protection stands), release for the rest of the hold when the game
demonstrably animates OUR rig. Evidence-first - the discriminator was
MEASURED before it was built (ENGINE_NOTES "s59"):**

1. **The instrument**: `bsihide probe on` - per-hold snapshots (bit edges
   per tick, who-identity at 750 ms cadence, articulation via new
   `bones::anchor_atoms`, open/close summaries). Two user flat legs (door,
   then the one banked Devil's Kiss first-drink) gave the paired trace.
2. **The falsifications**: bHidden reads 0 through BOTH classes (the s53
   rowboat tracking is scene-specific; only the death/respawn class stamps
   1); who-identity NEVER reads DIFFERENT (the spawned door rig is not
   reachable via GetFirstPersonAttachment; GObjObjects NOT FOUND on this
   build, find_instances is a multi-second sweep - no shippable second-rig
   detector exists); grip TRANSLATION is self-contaminated by our own
   grip-scale hide (~99-118 UU artifact, ~6 deg rotation).
3. **The discriminator**: the ROTATION channel. Door = 51-67 deg peak (its
   only own-rig motion is ForceUnequip); drink = 178-180 deg both hands
   from +15 ms. The gate shows the rig when either grip rotates >= 100 deg
   (config `cineShowDeg`, F10 slider) from its hold-open pose while
   bHidden==0; bHidden==1 (game parks the rig) hides immediately and
   clears the latch; any signal failure degrades to force = the shipped
   s53-s58 behavior. Auto can only regress into "always hide", never into
   doubles. The s57 melee-execution release keeps precedence.
4. **Ship shape**: `bsihide cine auto|always|game|off` + `cine deg <n>`,
   F10 "cutscene rig" radio (auto first) + threshold slider, config keys
   `cineRigMode`/`cineShowDeg`, enum APPEND-only (persisted values safe).
5. **Flat fence all green**: fake-hold hide branch; show branch (threshold
   20 crossing at 40 deg, latched, logged); per-hold re-arm; actor-lever
   degrade (it WRITES the discriminator bit - mutually exclusive with
   auto); force bit-identical; eye-check PASS all legs; vdeny seed 10 +
   head-use un-deny intact at boot.

**Caveats for the headset (TESTING "S59")**: ambient articulation drifts
(~40 deg / 25 s still) - a very long spawned-rig hold could creep past the
threshold and false-show (the raffle chain is the judge); the show latch
holds until hold-close; tattoo-poster and ball-77 are unmeasured beats (if
hidden, tune the slider down live). The headset checklist is TESTING "S59":
vigor drink shows hands, doors stay single-handed, one execution, the
raffle-save sweep, the always-hide radio reachable.

**Traps burned (s59)**: the first who_probe call pool-scans inside the
hold-open edge (~580 ms hitch, then cached); two game-cmd writes 1 s apart
clobber (the pump reads on its own cadence - one command per write, gap
them); death+respawn embeds its own door cinematic (a first-run trace
blended the classes - the clean replay separated them); the checkpoint
reload RESTORES an undrunk vigor bottle if no checkpoint fired post-drink
(the "one-shot" drink was re-runnable).

### Infinite: state after session 58 (superseded by s59 above) (HEAD-DIRECTED USE ACCEPTED IN THE HEADSET AND MERGED - 0x1E13DC cornered as THE USE consumer, policy default-on; merged to `bioshock-infinite`)

**HEADSET VERDICT (user, 2026-08-13): "everything worked as expected" -
the full TESTING "S58" checklist passed: head-directed targeting feel, the
F10 A/B both ways, the COMPLETE raffle chain with no stalls, doors and
kinetoscopes, and the sharpness sweep. s58 is CLOSED and merged.**

**Session 58 (2026-08-13) closed the deal-breaker at the mechanism level:
USE-target selection follows the HEAD. The s56 10-caller deny set stays
intact except that the head-use policy un-denies exactly 0x1E13DC (the
eyes-viewpoint VIRTUAL flavor - ENGINE_NOTES s56's prime suspect,
confirmed). Evidence-first, per the s58 ladder:**

1. **The oracle**: the GFx ButtonHint widget lane was a dead end (no
   IsShown on the CLIK hint classes; the container's entries TArray is
   sticky bookkeeping - it survives hide/walk-away; show/hide is pure
   ActionScript). The REAL oracle: **`XPlayerController.ButtonUseTarget`
   (InterfaceProperty, +0x176C this build, derived by name)** - the
   interaction system's live selected USE target. `bsihint` prints it;
   `bsihint watch on` edge-logs target changes. Whole vocabulary banked in
   ENGINE_NOTES s58 (PossibleUseObjects, LastUsedTime +0x424, ...).
2. **The sweep (user-parked at a vending machine, full VR sim)**: control =
   body-locked (armed with head 60 off; null with body 90 off + head
   compensating). Un-denying **0x1E13DC alone** INVERTS both legs (head
   rules); the other NINE candidates read identical to control - clean
   negatives, full table in ENGINE_NOTES. Cone half-angle 45-60 deg,
   applies to whichever view the consumer reads. Two mechanism findings:
   loot containers (XLootContainerStaticMeshActor) arm by PROXIMITY alone
   (facing irrelevant - they never discriminate); the sim `head rot +X`
   maps to MINUS X on the written view yaw (the compensated leg is
   body+X/head+X, not head-X - the wrong sign cost three legs).
3. **The fence (flat, all green)**: eye-check PASS all legs incl. pairing
   90/90 with the un-deny live in gameplay; 4-min soak - aim seam
   calls==substituted throughout (no s55-class freeze), replays healthy;
   fresh boot on the shipped build seeds 10 then logs `headuse: ON`,
   save loads to free play normally. The raffle-class chain is the
   HEADSET's leg (TESTING "S58" step 3) - judged low-risk because the s54
   stall was the FULL substitution under a sim head that never looks at
   targets, in the headset the head IS the pointer, and scripted holds
   suspend the drive (authored gates keep the authored view).
4. **Ship shape**: patterns.h `kInteractionUseViewCallerRva = 0x1E13DC`;
   `camera::set_head_use()` enforces the invariant (seed-then-del); config
   key `interactHeadUse` default ON; F10 -> HUD -> "HEAD-DIRECTED USE
   (s58)"; `bsicam vdeny del <hexRva>` added (16-slot set, runtime A/B, no
   reinstall). New read-only instrument module `hint.cpp` (`bsihint`).

**Traps burned (s58)**: the game PAUSES on focus loss and the pause MENU
does not close on refocus (fidget-silence + quads=1 = the tell; `game-key
Esc` resumes - NEVER Enter, RESTART CHECKPOINT is two slots down); the sim
idle hand pose parks the FP arm across the whole view (`hand l|r offset`
clears captures); this save's spawn is the Town Center fair plaza and its
load cycle closes in ~90 s with NO scripted-hold markers (the "~3 min
beat" was save-specific); Infinite has NO manual saves - user positioning
is per-boot standing state, not savable.

**NEXT SESSION (s59): THE MISSING-HANDS SCRIPTED-BEAT CLASS.** A
pre-existing regression (user: present BEFORE the s58 changes; likely from
the cine/model/hide rounds s52-s57, NOT the melee-execution fix which s57
already closed): in a SUBSET of scripted first-person beats the authored
hands never show. The user's observed split (2026-08-13):
- **Hands VISIBLE (correct)**: raffle beats generally (taking the ball
  from the basket shows the hand fully), the intro cutscene, door opens,
  melee executions (the s57 fix holds).
- **Hands HIDDEN (the regression class)**: (1) DRINKING A VIGOR;
  (2) the tattoo-poster beat in the city - flat raises the right hand to
  look at the AD mark, in VR no hand appears at all so it reads as plain
  talk; (3) raising the winning ball #77 after the raffle draw - the ONLY
  hidden-hand moment inside the otherwise-correct raffle chain.
**MECHANISM CONFIRMED same night (user A/B, headset - ENGINE_NOTES
"s58b")**: with `bsihide auto off` the vigor-drink vignette SHOWED the
authored hands AND the door cinematic showed DOUBLE hands - one lever flip
proved both classes. The drink opens a SCRIPTED hold (8.5 s in the log),
so the hiding leg is the s53 CINE-HOLD rig-wide hide (cineMode=force),
not the empty-hand policy. The two hold classes: SPAWNED-RIG (doors,
raffle, intro, executions - the game brings its own hands; ours must
hide) vs OWN-RIG (vigor drink, tattoo-poster raise, ball-77 - the game
animates the player's own rig; hiding it = no hands). s59 = per-hold
DISCRIMINATION: preferred - detect the spawned second rig live (the s53
`bsihide diff` fcomp-vs-ours machinery is the starting point) and hide
ours only then; fallback - detect authored articulation through OUR rig
during a hold (s51 travel/spread) and release the hide for that hold.
The drives already release during holds (s52), so an unhidden own-rig
beat plays correctly - user-watched. Trap: `bsigive` CANNOT trigger the
drink vignette (silent acquire/equip, no ceremony) - first-drink needs a
real bottle pickup, a user leg. Secondary backlog unchanged (TESTING
"S52" leftovers; reload glitch parked post-release).

### Infinite: state after session 57 (superseded by s58 above) (THE MODEL LANE LANDED FLAT - sprint glue, melee window, loadout buckets, crosshair hide, stump cuff; branch `claude/bioshock-model-lane-sprint-melee-7e33e4`, NOT merged, awaiting the headset checklist)

**Session 57 (2026-08-12) worked the five user-prioritized items; all five are
implemented, flat-proven at the mechanism level, and installed. Eye-check
PASS (all legs incl. leg 0 pairing 90/90) on the final build. Derivations in
ENGINE_NOTES "s57"; the numbered headset checklist is TESTING "S57" - the
LOOK verdicts (sprint feel, melee swing, execution hand, vigor-only tuning,
crosshair in combat, cuff) are the open items.**

1. **SPRINT KILL (bones.cpp sprint glue).** The s49b clamp shape was
   FALSIFIED first: a proven sprint posts NOTHING at the Morpheme message
   funnel and no by-name anim action - the state machine is driven inside
   the runtime. What sprint actually does: authored arm-pump articulation
   passes the compose exactly like the s51 fire swing (L cluster 103-124
   deg / 70.8 cm vs 0.00 walking). The kill reuses the proven machinery:
   the full-hand ready substitution held while the composed pad says sprint
   (LS edge + stick >= 0.5, exit < 0.35; capture-at-engage when the bank is
   missing; NOT ended on fire - over-hold is safe). Flat A-B-A: worst
   driven bone 124.38 -> 0.10 -> 124.38 deg. `bsibones sprintglue
   on|off|force on|off`, F10 "SPRINT KILL", key `sprintKill`, default ON.
2. **MELEE (melee.cpp).** Root cause measured: melee dispatches through the
   fire seam with weapon=NULL like gunshots - fireglue froze every swing
   and the +1.2 s capture banked MID-SWING poses (watched live), poisoning
   later shots; the execution's scripted hold force-hid the rig the game
   was animating (the s53 warning, verbatim). The fix: the Y-press edge
   (composed pad) classifies a dispatch as melee UNLESS a hold is already
   open (the raffle-QTE rule, byte-identical, proven by 7000+ holdSkips);
   a melee dispatch skips/cancels fireglue + captures and opens a 1500 ms
   window that releases the empty-hand hides; a hold opening INSIDE the
   window (the execution) releases the hide gate until it closes + 300 ms.
   Flat: capture counters held under a classified swing, incremented
   without. Modes off|glueskip(default)|release; `bsimelee`, F10 radio,
   key `meleeFixMode`. The execution needs a staggered enemy = user leg.
3. **LOADOUT BUCKETS (profiles.cpp).** Verified: keying was per-hand only -
   vigor-only SHARED the vigor archetype entry with vigor+gun (the leak the
   user felt) and "NoWeapon" with empty+empty. Now the loadout class forms
   the effective keys: gun present = raw names (existing entries
   untouched), vigor-only = `<Vigor>#solo` + `NoWeapon#solo`, empty+empty =
   the bare synthetic names. key_is_empty is prefix-matched so the hide
   gate still sees suffixed empties. Existing sliders/auto-capture work
   unchanged on the corrected keys.
4. **CROSSHAIR HIDE (xhair.cpp) - the hunt LANDED.** The s53 wall fell: the
   widget's Outer is the HUD screen instance XSinglePlayerGFxHUD; the
   widget's IsShown/IsCenterpointVisible bools live at +0x118 (masks
   0x1/0x2, live-verified). Policy default ON (`hudCrosshairHide`): sweep
   (the s54 enumerator as a callable) for live instances per level, clear
   both bits, 1 s watchdog - the game re-asserted 7x in one boot and lost
   every time. `bsixhair on|off|derive`, F10 in HUD (I9). Headset judges
   the visual (flat can't arbitrate the out-of-combat dot).
5. **STUMP CUFF (option A).** The armsMode-2 collapse writes epsilon scale
   (default 0.10, slider 0-0.30 next to cap depth) instead of zero - the
   capped-stub ring is visible in the flat captures where the pinch was.
   `bsibones stumpscale`; unpersisted pending the headset verdict.

**Traps burned:** the sim pad outage recurred (keyboard drive lane is the
fallback: W/S/A/D + LeftShift sprint + V melee; `force`/`swing` levers make
the glue/window testable pad-free); the save's load beat cycles
ForceUnequip for ~3 min and must NOT be keyed into (one Space mid-beat left
a boot disarmed inside a second vignette); the trailing-newline token trap
bit a THIRD time (`bsixhair on`); the spawn is nose-to-a-wall (W reads like
idle on img-diff - back up + strafe first, ~1900 UU corridor).

**HEADSET VERDICTS IN (same evening) + THE s57b ROUND SHIPPED:**
- **ACCEPTED**: melee swing + execution ("working perfectly"), stump cuff +
  depth ("decent for now"), sprint kill core.
- **FIXED same evening (s57b, re-judge)**: post-sprint SNAP (the glue now
  holds a 700 ms release tail through the sprint-exit anim blend;
  `sprintglue tail <ms>`); the reload-then-shoot GLITCH LOOP (the ready
  capture is quiet-gated - moving poses are refused per drive, ~2700
  refusals measured across one fire+reload, banks land clean after
  settling); the melee GUN-HAND lurch (right limb bone-hides for the first
  0.9 s of the swing, never the execution; `bsimelee hidegun`, F10, key
  `meleeHideGun`).
- **s57d/e REVERTED (2026-08-13, user verdict)**: the reload-then-shoot
  deep fixes (release fade + reload hold) were headset-falsified - the
  glitch stayed and sprint + melee-execution regressed; src is back to
  byte-identical s57c (the accepted build). The melee fixes and everything
  else stay. **USER RE-VERDICT after the revert: everything back to
  normal.** The reload glitch is MOVED TO THE POST-RELEASE BACKLOG
  (bioshockinfinite/ROADMAP.md; do-not-reuse record in ENGINE_NOTES
  "s57d/s57e REVERTED"). s57 is CLOSED and merged to `bioshock-infinite`.
- **DEFERRED - s58 PRIORITY 1: head-directed interaction.** Interactions
  aim with the BODY (right stick), not the head - the s56 partition as
  built (interaction consumers deliberately read the engine view; the VR
  view stalled the raffle). s58 plan: flat caller census near an
  interactable, find WHICH denied caller answers USE-target selection,
  un-deny only it, and fence with the raffle-class beats + eye-check (the
  s56 acceptance must survive). ENGINE_NOTES "s57b" carries the notes.

**NEXT SESSION (s58): THE HEAD-DIRECTED INTERACTION SPLIT - the user's
DEAL-BREAKER, the session's single focus.** Interactions must target where
the HEAD looks while every interactive cinematic / scripted view-cone gate
(the raffle class) keeps working. Evidence-first ladder: (1) build the flat
PROMPT ORACLE - the interaction prompt is a GFx widget (XClikButtonHint
instances were live in the s57 crosshair walk); its IsShown bits at a
derived offset are a log-readable "prompt visible" signal, which makes the
whole caller sweep automatable; (2) with the oracle live and the head yawed
off the body axis (sim `head rot`), sweep the 10-caller deny set - un-deny
ONE candidate at a time (`bsicam vdeny` is runtime-togglable, 16 slots) and
read which caller makes the prompt follow the HEAD; prime suspect first:
0x1E13DC (the eyes-viewpoint VIRTUAL flavor - ENGINE_NOTES s56 named it
the interaction path), NEVER touch 0x1E1367 (render smear) or 0x26B499;
(3) regression fence per candidate: the load-time scripted beat must still
complete + re-equip (the free flat canary every boot), `bsicam callers`
mid-any-stall, eye-check leg 0 per build; (4) if ONE caller serves both
USE-targeting and the beat gates (no view split can work), the fallback is
the TRACE-SEAM substitution - find the USE/interaction trace native (the
fire.cpp XGetWeaponStartTraceLocation shape) and substitute the head (or
hand-laser) ray directly, leaving the s56 partition untouched; (5) headset:
user positions at interactables + saves, judges head-vs-body targeting,
then re-runs the raffle-class chain (TESTING "S56"). Secondary if time:
TESTING "S52" leftovers (arsenal tuning + calibration save, HUD sliders,
subtitles).

### Infinite: state after session 56 (superseded by s57 above) (THE INTERACTION-VIEW FIX SHIPPED AND HEADSET-ACCEPTED - the raffle chain plays end to end under full VR, automatic at install; branch `claude/bioshock-interaction-view-fix-c1be42`, NOT merged)

**Session 56 (2026-08-12) finished the render/gameplay view partition and
shipped it: the deny set {0x1E13DC, 0x22587F, 0x5EA483, 0x5B2C8C, 0x59C87D,
0x244CF4, 0x52F301, 0x5344E8, 0x61C289, 0x5F9A94} is seeded automatically
at install behind the build gate (patterns.h `kViewConsumerDenyRvas`), zero
user levers, full 3D preserved. USER-ACCEPTED IN THE HEADSET: sharp on head
motion in every direction, and the whole raffle chain - activate, take-ball,
announcer within seconds, reveal, apprehension, skyhook QTE, control back -
played with no stalls.** Full derivation in ENGINE_NOTES "s56"; checklist in
TESTING "S56"; the s54 state below carries forward otherwise.

1. **The third gate**: mid-stall caller census (flat) found 0x5EA483 - a
   per-local-player "is the player looking at the target" view-cone helper
   in the scripted-sequence native cluster, absent from the s55 map;
   denying it unstuck the stalled reveal beat in the same log second.
2. **The 0x1E1367 correction (headset bisect, 4 live flips)**: the
   eyes-viewpoint wrapper's DIRECT flavor is a RENDER consumer - denying it
   causes an offset-dependent post-process smear (sharp only when the head
   aligns with the authored view) that NO flat instrument can see. It is
   never denied; the interaction path reads through the VIRTUAL flavor
   0x1E13DC. This corrects s55's attribution and closes its framing-shift
   observation.
3. **The eye check made honest (tools/eye-check.ps1)**: a negative control
   proved the s55 image legs are blind to the pairing break (mono reads
   in-band - the compositor presents identical eyes at two poses); leg 0
   (camReplays/s >= 80% of draws/s from a FRESH beat line) is now the
   pairing/mono gate, validated in both directions; interocular floor
   40 -> 30. vdeny widened to 16 slots + an allow-only DERIVATION mode.
4. **A-B-A**: `vdeny off` = sharp + interaction break returns (user-judged
   live); `on` = fixed. The lever needs no reinstall; restart re-seeds.

**HEADSET VERDICTS IN (user, post-s56 play session, 2026-08-12):**
- **CLOSED - scripted scenes clean**: the boat scene, door opens and the
  other cinematics all played correctly, NO double hands - the s53b hide
  defaults (owner+grips composite) and the s56 deny set are accepted
  together; the s53 rowboat re-check and the scripted-beat inheritance
  check are done.
- **FAIL - sprint stance**: sprinting changes the model position; very bad
  in VR. Wanted: sprint must not affect the model AT ALL (kill the trigger
  at the root - the s49b 'Lowered'-clamp shape is the template: probe the
  FP-network request while sprinting with `bsifidget req probe`, then clamp).
- **REGRESSION - melee**: melee attack animation is now weird, and the
  melee-execution mini-cutscene shows NO hand at all (tested vigor-only and
  vigor+weapon). Suspects: the s53 hide gate not releasing for the
  execution scene class (it may not register as a cine hold), and/or the
  bone drive fighting the attack anim (the s50/s51 fire-swing shape).
  Melee worked well before the cine/model rounds - corner by A/B at a melee
  with `bsihide`/`bsibones` levers.

**NEXT SESSION (s57), user-prioritized - the model lane:**
1. **Sprint stance kill**: derive the sprint request/anim on the FP network
   (fidget probe while sprinting), clamp at the root, default-on. VR sprint
   must leave the viewmodel untouched.
2. **Melee fix**: (a) attack anim - likely needs a melee window where the
   hand drive releases to the authored swing (fire-swing template);
   (b) execution mini-cutscene - the authored hand must SHOW (hide gate
   release for that scene class). A-B-A with levers, then headset.
3. **Per-loadout hand offsets**: hand pose offsets keyed by LOADOUT CLASS -
   vigor-only (currently rotated/positioned wrong), empty+empty, and the
   existing weapon(+vigor) profiles untouched. Saving must be bucket-local:
   a vigor-only tweak must not leak into the two-hand preset, and
   empty-hands tweaks stay in the empty bucket (verify the existing
   profiles.cpp keying - the equipped-identity poll may currently lump
   vigor-only with empty).
4. **The crosshair hunt**: `bsigfx scan XClikHUDCrosshair` / `scan HUDMovie`
   in the gameplay save, chase with bsiprop/bsifields, then the setb/cmd
   levers - disable the flat-screen crosshair.
5. **LAST PRIORITY - the wrist stump look (option A only).** The plain
   zero-scale hide EXISTS and is REJECTED by the user (the pinch lands
   inside the hand). Why: zero-scaled verts collapse toward the bone's
   CURRENT (authored) position, arbitrary relative to the driven hand. The
   fix to try: DRIVE THE COLLAPSE POINT - write a forearm atom that parks
   the bone just behind the driven wrist along the hand's arm axis with a
   small epsilon scale (5-15%) instead of zero, so the mixed-weight ring
   forms a symmetric capped stub/cuff aligned with the hand (the sleeve
   verts form the cap). Two F10 sliders (scale, back-offset), tuned in the
   headset. Same atom machinery as bones.cpp. The ceiling version (two-bone
   arm IK, option B) is MOVED TO THE POST-RELEASE BACKLOG
   (bioshockinfinite/ROADMAP.md, after I11) by user call 2026-08-12.

### Infinite: state after session 54 (superseded by s55 above) (THE RAFFLE WEDGE ROOT-CAUSED AND FIXED - the pace feed; branch `claude/bioshock-session-deadlock-root-28d444`, NOT merged)

**Session 54 (2026-08-11) root-caused the session-state deadlock from LAST
NIGHT'S OWN LOGS - no new instrumentation was needed - and shipped the fix,
flat-proven against a sim model built from the measured numbers.** Full
mechanism + derivation in ENGINE_NOTES "s54"; headset checklist in TESTING
"S54"; the s53 state below still describes the hide gate / GFx / verdict work,
all of which carries forward untouched.

1. **THE MECHANISM (measured, pacetrace.log + bioshockvr.log, boot
   02:32-03:52):** VDXR demotes FOCUSED -> VISIBLE holding `shouldRender=0`
   (trigger external, unconfirmed); our inline frame loop then submits
   ZERO-LAYER frames (every layer is gated on shouldRender), which VDXR (a)
   refuses to re-promote on - parked VISIBLE for minutes at 72 empty
   frames/s - and (b) throttles to **~87 ms per xrEndFrame, inline on the
   present thread**, pacing the game to ~10 presents/s (~5 fps): the
   announcer stalls, scripted timelines crawl - and this half needs no input
   lane, which is why the wedge predates it. Input-death is the other half
   (actions live only while FOCUSED). Both s53 recoveries happened WITHOUT
   teardown (external, VD-side); the VR toggle "works" because teardown
   removes the throttled calls instantly and bring-up paces freely with real
   frames. The old skip-guard suspect is EXONERATED (neutered by default;
   paceSkips 0 throughout; detach was off everywhere).
2. **THE FIX (core, opt-in per game - BSI arms it, BS1/BS2 take no new
   branch):** `set_pace_feed` = detach + keepalives that CARRY LAYERS. While
   not-FOCUSED-after-focus the present thread detaches (game free-runs) and
   the pace thread re-runs the frame cycle, re-submitting the last healthy
   projection/screen layer (no re-acquire needed - the compositor uses the
   most-recently-released image). Levers: `vrpace feed on|off`, F10 checkbox.
3. **FLAT-PROVEN (`tools/xrsim/vdxr-park.xrs`):** the sim grew the
   measured-VDXR model (`focus norender on`, `focus policy vdxr-layers`,
   `focus throttle 87`). Feed OFF = the park reproduced (10 s eligible, ~90
   empty frames, still VISIBLE); feed ON = self-healed (FOCUSED 2.2 s after
   the loss; ATTACHED reported 444 presents ran unpaced - the game free-ran
   the whole episode). Regressions green: unfocused-pacing 12/12, smoke 5/5.
4. **Open on the headset (TESTING "S54"):** VD's real re-promotion latency
   with layers flowing (dashboard open/close A-B-A), the doff/re-don case,
   and the raffle itself. Until those pass, the old wedge protocol (F10 ->
   VR enabled off/on) stays the fallback.

5. **BONUS - THE OBJECT-INSTANCE ENUMERATOR shipped and proven live
   (crosshair-hunt item 3, first lane; ENGINE_NOTES "s54 part 2").**
   `bsigfx scan <Name>` (FName-index sweep; one index finds class + package
   + instances - UE3 reuses the base index with a number) and `bsigfx scanc
   <hexClass>` (class-pointer sweep, name-gated after 82 fixpoint-passing
   fakes taught the lesson). Live validation: `scan HUD` found the UClass,
   the Package and the LIVE myHUD instance (pointer-identical to `bsigfx
   hud`); `scanc` found those plus **Default__HUD - the CDO the s53 "loads
   null 4 ways" wall could never reach**. ~780 MB in ~400 ms, one-shot
   game-thread hitch (never on a cadence). On this boot's save level
   `XClikHUDCrosshair` has NO instances (UClass only) - the hunt's next
   step is the same two commands in the user's gameplay-proper save.

**NEXT SESSION (s55): (1) the S54 headset A-B-A (dashboard, doff, THE
RAFFLE - the root fix's real-hardware verdict); (2) the remaining s53/s52
verdicts, unchanged from the s53 handoff below (rowboat re-check under the
s53b owner+grips defaults, cine-radio default decision, single-empty-hand,
then TESTING "S52": arsenal tuning + calibration save, HUD sliders, Matinee
gun-track, sprint arms, subtitles; preset keys land after verdicts); (3) the
crosshair hunt CONTINUES with the new enumerator in the gameplay save:
`bsigfx scan XClikHUDCrosshair` + `bsigfx scan HUDMovie`, chase hits with
bsiprop/bsifields to the owning screen, then the setb/cmd levers.**

### Infinite: state after session 53 (superseded by s54 above) (THE FP-RIG HIDE armed on the game's own bone lever; the s52 hide falsifications explained by measurement; the GFx screen model mapped; branch `claude/bioshock-fp-rig-hide-fa6342`, NOT merged)

**Session 53 (2026-08-11) attacked the round-4 revert head-on: derive the
game's OWN visibility levers, A/B them on the live view, and ship the hide as
policy on the winning lever.** Commits `6b83cfb` (probe surfaces) +
`308dc31` (gate armed). Derivations in ENGINE_NOTES "s53"; headset checklist
in TESTING "S53"; everything below is sim/flat-proven, awaiting the headset.

1. **THE LEVER VERDICTS (the session's spine).** All four candidate levers
   derived one-shot on the live rig (`bsihide derive`: bHidden actor+0x5C
   mask 4; HiddenGame/bOwnerNoSee/bOnlyOwnerSee all in comp+0xD4; SetHidden/
   SetOwnerNoSee/HideBoneByName/UnHideBoneByName/IsBoneHidden/MatchRefBone
   indices cached) and A/B'd on the live view in an armed chapter
   (LOAD CHAPTER -> Comstock Rooftops, pistol + Enrage): **actor bHidden is
   INEFFECTIVE** (bit written+read 1, hands and pistol keep rendering - the
   FP path ignores it; it merely TRACKS scene state, 1 in the rowboat's
   no-hands phases, 0 at authored moments/gameplay); **comp SetHidden hides
   the arms mesh but the weapon model keeps floating** (separate attached
   component); **bone HideBoneByName removes limb AND holdable together**
   (s45b re-proven) - the production default.
2. **THE GATE (hide.cpp, armed by default).** Policy consumes the untouched
   round-3 conditions: `cine::hold()` -> rig-wide hide (cine radio: force /
   game-managed / off, default FORCE per the user directive - the box-handoff
   risk is the first headset A/B); `hand_empty(h)` outside holds -> per-hand
   whole-limb bone hide (user call: no floating forearms). Edge-driven with a
   500 ms re-assert watchdog, per-boot derive with refused latch, instance-
   only writes, rig-drop/re-resolve safe (fresh rigs spawn visible), fault
   latch after 8 failed applies, F10 controls in HANDS + MODEL. Flat-proven
   live: empty-hand possession bone-hid both limbs, the arriving chapter
   loadout unhid them on the next profiles poll, zero reasserts, zero fights.
3. **THE DOUBLES' IDENTITY, narrowed.** `bsihide who` proved scripted scenes
   NEVER swap the attachment (stale-rig hypothesis dead), and the bHidden
   ineffectiveness explains round 4's "zero-scale hides nothing the user can
   see" the same way. Remaining suspect for headset doubles: the PAWN's own
   third-person body, visible only from the offset VR camera ("hands to the
   right and left of the character") - `bsihide pawn 0|1` (SetOwnerNoSee on
   pawn Mesh, derived per boot) is the one-command headset A/B.
4. **THE GFx LANE (bsigfx, crosshair kill rung 3 partial).** All machinery
   names live in the pool (HideableHUDWidgetNames 4835,
   NumReasonsToShowElement 36595, XSeqAct_HideHUDElement 10586, HUDMovie,
   FlashCommand, XClikHUDCrosshair 8654; SetVariableBool does NOT exist).
   `myHUD` (derived at PC+0x2B4) is a BARE `HUD`-class object even in
   gameplay - none of the machinery is on it. The screen MODEL mapped by
   walks: UI screens are GFxMoviePlayer-family UObjects (SwfMovie asset
   +0x34, PC +0x5C) owning CLIK widget UObjects; reachable so far:
   XGameViewportClient +0xF0 -> GFxInteraction; the HUD screen INSTANCE (the
   crosshair widget's owner) is the open hunt - needs an object-enumeration
   or GFx-advance-hook lane next session. `bsigfx` ships: hud/prop/cmd
   (FlashCommand)/element list/element +-N/setb/getb.
5. **Traps burned this session:** the s37 attract-movie freeze hit once
   (force-kill + relaunch protocol worked); game-shot captures RACE command
   dispatch (the game pauses unfocused - a "hidden" capture can show the
   pre-dispatch frame; read the log bits, not the pixels, for state);
   `xrsim head rot` arg order is (yaw?, pitch, roll) - the second arg pitches;
   an additive `head pos` during a scripted scene can put the camera inside
   geometry - reset pose + `bsicam drive off` restores the authored view.

**NEXT SESSION (s54), in the user's priority order: (1) ROOT-CAUSE THE
SESSION-STATE DEADLOCK (the raffle wedge - see the s53 addendum below; the
user explicitly rejected a watchdog band-aid: find the mechanism, fix the
root). Prime suspect from the code's own comments: the unfocused-pacing
skip-guard starves the compositor of frames while the session is VISIBLE,
and VDXR may need submitted frames to re-grant FOCUSED - bring-up paces
freely for exactly that reason (openxr_runtime.cpp ~2122 "the next bring-up
must pace freely again (bring-up needs frames)", the skip decisions at ~909/
~2293/~2388, and g_paceSkips). The user's proven manual release (VR enabled
off/on) resets g_everFocused, which is precisely the free-pacing path -
consistent. Reproduce by dropping focus (VD dashboard) mid-game, watch
paceSkips while VISIBLE, A/B the skip-guard, A-B-A the fix. (2) The remaining
headset verdicts (rowboat re-check under the s53b owner+grips defaults: intro
text, doubles, box-handoff hand, cine-radio default decision - currently
game-managed after the live flip; single-empty-hand case; then TESTING
"S52": arsenal tuning + calibration save, HUD sliders, Matinee gun-track,
sprint arms, subtitles). (3) The crosshair: the HUD-screen instance hunt
(ENGINE_NOTES s53 GFx screen model; myHUD is bare, CDO loads null - needs an
object-enumeration or GFx-advance-hook lane).**

### Infinite: state after session 52 (superseded by s53 above) (the I7 INPUT LANE landed - stick pitch killed + body follows head; THE CHEATED ARSENAL + per-weapon presets; the HUD ON A QUAD; the CINEMATIC GATE + head radio; branch `si52-inf-input-arsenal-hud-cine`, NOT merged)

**Session 52 (2026-08-10, evening) worked the four handoff items in strict
order; all four landed flat-proven and committed (882a420, 0e4a41c, a2aea90,
e8dc538). Checklist in TESTING "S52". Full derivations in ENGINE_NOTES "s52
part 1-4".**

**1. THE TWO INPUT FIXES (commit 882a420).** The I7 lane arrived: drive_view
now publishes `publish_vr_gameplay` + `publish_pitch_error` + the new
`publish_move_yaw_offset` every dispatch (all self-expiring; the I4
abstention comment retired). RIGHT-STICK Y IS DEAD: the camera-side engine
pitch is the stick-driven basis (clamps +/-89; the aim seam's engine value
FLATTENS pitch - wrong instrument), and with the kill on, a held full-up
stick composes as the servo value; the servo converges the manufactured +89
error back toward the head (sign correct un-inverted). Infinite opts out of
the BS1 bumper lift (`set_pitch_kill_lift_on_bumpers(false)`, new core seam)
- no ry leak with a grip held, measured. BODY FOLLOWS HEAD: the composer
rotates the movement stick by the published head residual - walk heading
matched the CAMERA facing to 0.5 deg under `head rot 90` while engine yaw
sat still; `bsibody off` control reverted to game yaw. SNAP TURN wired
(BS2's drain order); the drain sign is `+units` - the OPPOSITE of BS1
(stick-right DECREASES yaw units on this build; flick right measured +45
the wrong way on the copied sign, flipped, both directions re-proven).
Levers: `bsibody on|off|status`, F10 BODY FOLLOWS HEAD checkbox, core
pitchkill/snap controls now live via the gate; keys inputPitchKill/
inputBodyFollow/inputSnapTurn/inputSnapAngleDeg. Known limits (documented):
the pawn itself does not turn (no body.cpp counterpart - past ~90 deg
residual the rig/laser may desync); the pause menu keeps the camera seam
dispatching, so stick MENU nav is rotated while the head is off-center.

**2. THE CHEATED ARSENAL + PER-WEAPON PRESETS (commit 0e4a41c).** The
identity fact that re-pointed the s47 scaffold: every carried weapon and
vigor is literal `class XWeapon` - the durable key is the ARCHETYPE name
(UObject+0x24). `GetEquippedWeapon(int)` via ProcessEvent answers BOTH hands
(0 = gun, 1 = vigor; return at parms+4). THE GRANT RECIPE (every rung
measured; corrects s43 - AcquireWeapon wants the ARCHETYPE, not the CDO):
`DynamicLoadObject("PreCoalescedItemAssets.<Archetype>")` -> pawn
`AcquireWeapon(archetype)` (the carried list GREW 4->10) -> manager
`EquipWeapon(instance)` (pawn-side EquipWeapon is a no-op) -> instance
`AddAmmo(i:n)`. `bsigive <Archetype> [ammo]` automates it; `bsigive list`
names the carried slots; the whole base roster granted in one sim pass
(Sniper/RPG/Carbine/HandCannon/Shotgun + MurderOfCrows). bsigive on a gun
DROPS the replaced carried gun (the game's carry-2 rule) - by design.
PRESETS: profiles.cpp rework - per-archetype entries holding one hand's full
lever set (aim trim P/Y, ray origin F/R/U, model trim/offset/scale), ~1 Hz
identity poll, AUTO-CAPTURE on switch-away (hold weapon, tune sliders,
switch = saved), apply-on-equip, persisted to `bsi\weapons.ini`
(`<Archetype>.<lever>=v`), empty-entry path leaves levers untouched
(byte-identical default, proven in the logs). Round trip proven: tuned
sniper trim survived capture -> file -> re-equip OVERRIDE APPLIED. Verbs:
`bsiprofiles list|save|clear`, F10 WEAPON PROFILES section.

**3. THE HUD ON A QUAD (commit a2aea90; ENGINE_NOTES s52 part 3).** The DR-I7
positional rule implemented as core `gfx_hud.cpp` + adapter `hud.cpp`: the
eye blit = the ONLY full-screen depth-free `DrawIndexed a=6` into the
backbuffer (srv0 = full-res fmt-26 RT; the UI run SHARES its retRva, so
position is the only separator); every later backbuffer draw redirects into
a transparent capture RT that feeds the session-19 HUD quad via the new
`vr::set_hud_texture_provider` seam. The present-time eye capture is then
HUD-free with NO eye-source rerouting. Live: boundaries on 94% of frames,
1M+ draws redirected flap-free; per-eye captures show the eye image clean +
the panel carrying health/ammo/widgets in both eyes; the PAUSE MENU lands on
the quad readable; award dialogs too (the dark pane IS the dialog's dim
layer). FULL-SCREEN EFFECTS: the RPG explosion is SCENE-SPACE (fills the eye
image, nothing to route); the GFx hurt-flash class (tiny-count full-screen
draws, ~0.3/frame rest vs ~3.6/frame during a self-hit) PASSES THROUGH to
the eye image (default ON; census proof redirected == hudDraws -
bigPostDraws exactly). Movie/loading frames have no boundary -> classify
NOTHING (cinematics-safe by construction). The redirect engages only with a
live XR session - flat boots keep the window HUD. Verbs: `bsihud
on|off|status|redirect|fx`; F10 "HUD (I9)".

**4. THE CINEMATIC GATE (commit e8dc538; ENGINE_NOTES s52 part 4).** Bink
needs NO detector (camera-silence architecture: drives stop writing, the
stale-publish quad shows the movie - the pre-drive behaviour the user judged
perfect). The MATINEE detector: ~2 Hz `GetViewTarget` poll (GNames 17299),
class-name verdict (Matinee/CameraActor/Cinematic substring), hysteresis 2;
possession chain visible in the edge log. On a forced hold: EXACTLY one
bones::release per hand (via the hands tick's own edge - the s29 lesson),
re-arm clean, the flourish chord SUSPENDED (A passes to interactive prompts
- the raffle lesson; new core seam `set_flourish_chord_suspended`); HEAD
RADIO both modes proven under `head rot 30` (fixed = authored rot untouched,
look = additive compose; default look, key `cineHeadLook`, F10 radio).
aim/laser/fire substitutions carry the same one-line hold gates (hands-gate
shape); their runtime A/B rides the headset cinematic run. `bsicine
status|force|head look|fixed`.

**Traps and notes:** the SIM PAD OUTAGE (harness, not the mod): three
consecutive boots stopped calling XInputGetState entirely (bridge test-press
composes nothing because compose_over never runs); same binaries polled fine
in earlier boots; XUserOptions byte-identical. Cost this session: the chord
consumption A/B and the aim-subs-freeze check moved to the headset
checklist. Boot menu state still VARIES (two boots straight into the save,
two via MAIN GAME -> CONTINUE). The dumpframe-vs-flash timing chase (1 Hz
command poll) never caught a hurt-flash frame - the gfx_hud census is the
instrument that answered it instead.

**NEXT SESSION:** the user's headset verdicts on all four items (checklist
in TESTING "S52"); the calibration save (cycle the cheated arsenal, tune
each weapon, save in-game); then the remaining I9 surface per verdicts
(subtitles in stereo, upgrade/vending menus, the attract Bink visual
confirm, the raffle interactive prompt) and the parked polish items
(FOV-edge, FX-origin) per user priority.

### Infinite: state after session 51 (kept for the record; superseded by s52 above) (the SHOULDERS killed, the FOV-edge discriminators + edge telemetry shipped, the FX record lane exonerated; branch `si51-inf-shoulders-edge-fx`, merged into the s52 line)

**Session 51 (2026-08-10, overnight) worked the three s50-handoff items in
strict order; all three landed flat-proven and committed. Checklist in
TESTING "S51". A power loss mid-session cost nothing (all state on disk).**

**1. THE FIRE-SWING, ROUND 2 - fixed (commit cfa2964).** The new all-bones
instrument (`bsibones travel all [secs]` - per-bone peak table over the whole
bank, sorted worst-first) named the real mechanism in ONE shot: with the s50
anchor glue ON, the LEFT ANCHOR reads 0.10 deg but the rest of the left chain
swings 74-133 deg / up to 87.7 cm (Larm21 133.54) - the corr cancels only the
anchor's ABSOLUTE motion, and the fire anim's ANCHOR-RELATIVE arm articulation
passes through by design. The chest (the only undriven bone) measured 0.00 -
the s50 "engine-owned bones" theory is FALSIFIED. Fix: the +1.2 s ready
capture now banks the WHOLE hand's atoms; during the 1500 ms fire window the
compose substitutes them for the live source (corr collapses to identity) -
the hand renders its ready articulation rigidly on the controller. A-B-A:
FULL = left 0.00-0.16 deg / `anchor` mode = the exact 133.53/95.58 signature
back / FULL again clean; flourish still 8.43 img-diff; stance 4.5 min idle =
0 L-cluster movers. Levers: `bsibones fireglue on|off|full|anchor` + two F10
checkboxes. Known feel change to judge: the right hand's residual recoil
articulation is frozen too during the window.

**2. THE FOV-EDGE DISCRIMINATORS (commit a90322b; ENGINE_NOTES s51 part 2).**
Three instruments, none claiming the fix, all flat-verified on the sim:
- **Hand ref quad** (`bsicam handquad on|off|l|r`, F10): core parks a 1.5-deg
  compositor quad AT the located grip per present. Sim: quad == grip to 1e-4
  across three stations. In-headset one-look: quad-vs-hand SEPARATE = the
  projection/submission lane is guilty; TOGETHER = the composed hand position
  itself bends.
- **VDXR view logger** (`bsicam viewlog [n]`): bounded burst of located
  per-eye pose/fov + derived eyeSep/CANT/fov-asymmetry. Sim null: 0.0630 m
  lateral, cant 0.0000.
- **THE EDGE TELEMETRY LANE** (`bsicam edgelog on|off`, new edgelog.cpp):
  ~30 Hz in-memory ring -> 110-column TSV on `off` (located views, submitted
  tags + claim, consumed head pose, written camera + per-eye cameras, grip
  pose, composed model targets, component L2W, frame ids). Sim sweep null
  baseline banked: lateral chain EXACTLY linear at worldScale (rms 0.000),
  tags identity, eye pair exact. The headset run instructions are in the
  checklist - after the run the TSV alone lets the next session find which
  stage's numbers bend where the perceived depth bends.

**3. THE FX-ORIGIN FORKS - all three falsified (commit f7a232a; ENGINE_NOTES
s51 part 3).** The s50-decoded record lane is EXONERATED at runtime: the
effect playback tick 0x436490 NEVER RUNS (hooked probe-only, calls=0 through
a held charge), and the per-record update's entire live population is six
SkeletalMeshActor scene records with NULL location buffers (5 live callers of
194 static; 0x5EC393 dominant). No plume record exists in the lane, so the
planned one-poke experiment had no target - the honest outcome. SIX lanes now
falsified for the frozen family's writer. Banked instruments: `bsifx u dump`
(all-records), `bsifx u callers` (runtime census), `bsifx t probe|dump|status`
(tick + table walker). Defaults unchanged: bsifx ON, u/t OFF.

**Traps hit and documented:** the command.txt trailing-newline token trap
(recorder.h) bit TWICE (edgelog `on`, bsifx `u`/`t` lane detection) - fixed
and noted in ENGINE_NOTES; the travel sampler cadence is ~1350/s (camera
detour fires many times per frame), fine for peak detection but not a frame
counter.

**HEADSET VERDICTS on the s51 build (user, 2026-08-10, same night):**
1. **SHOULDERS: PERFECT** - no jump on shots OR plasmid throws. The
   full-hand substitution ships as the default; no tuning requested.
2. **FOV-EDGE DRIFT: DEFERRED by the user to a polish milestone** -
   "playable for now". The discriminator instruments (hand quad, viewlog,
   edgelog) stay shipped and OFF; run them when the item is picked back up.
3. **FLOURISH: ACCEPTED at lead 200 / tail 4500** - defaults stand.
4. FX-origin: user asked for a plain-terms explanation + a decision on
   whether to keep hunting (see the s51 close-out discussion).

**NEXT SESSION:** per the user's calls - FOV-edge and FX-origin both sit
behind the user's prioritization; the live FX leads remain (the 0x5EC393
caller loop, a render-side particle transform hunt) with all instruments
banked.

### Infinite: state after session 50 (kept for the record; superseded by s51 above) (three fixes shipped - eye tags, THE FLOURISH BUTTON, the fire-swing kill; FX-origin re-scoped by the user; branch `si50-inf-fx-edge-flourish`, NOT merged)

**Session 50 (2026-08-10, overnight) worked the s49b items in strict order.
Item 1 (FX-origin) hit the ask-when-blocked rule mid-session; the user
answered "re-scope, move on" - items 2, 3 and the mid-session item 4 then
ALL landed with flat proof. Everything below awaits the headset verdict
(checklist in TESTING "S50").**

**SHIPPED THIS SESSION (all default ON, each with its own A/B lever):**
1. **Rendered-pose eye tags** (item 2, the FOV-edge drift lever) - the
   projection layer now describes the parallel camera the game actually
   renders (located-pair midpoint +- ipd-slider/2) instead of the runtime's
   raw located per-eye poses. The compose chain itself was exonerated for
   the sign-flipping depth symptom; the pose-tag claim-vs-render violation
   is the one mechanism found that produces it (cant/IPD delta -> off-center
   disparity error, near-field visible). Identity on the sim (flat-proven);
   core change additive + opt-in, BS1/BS2 untouched. A/B: `bsicam eyetag
   on|off`.
2. **THE FLOURISH BUTTON** (item 3) - **left thumbrest (touched) + A**, or
   `bsiflourish`. Why it was lost: under the clamp a SubtleFidget play is a
   visual NO-OP (the response lives in the lowered subgraph). The shipped
   recipe holds 'Lowered' at 1.0 for a 6.3 s window (clamp-value override),
   fires the engine impl after a 1.8 s blend-in, then the kill resumes.
   Flat A-B-A: baseline -> 8.15 (full gesture) -> 0.5 (ready, no stick).
   A is consumed while the rest is touched (no jump under the chord); the
   SetTimer re-arm is measured benign. BS1/BS2 pads untouched.
3. **The fire-swing kill** (item 4, user mid-session: "shooting shouldn't
   affect the left hand") - flat-measured 95.58 deg of left-anchor rotation
   through one right-hand shot (idle 0.00); the engine's fire anim moves the
   authored left grip and the compose passed it through. Fix: the s46 glue
   correction, FIRE-SCOPED (1500 ms around each player shot) so the
   flourish/cast anims stay untouched. A-B-A: ON 0.00 / OFF 95.58 / ON +
   flourish still full-amplitude. A/B: `bsibones fireglue on|off`.

**ITEM 1 (FX-origin) - re-scoped by the user, banked for a later session:**

**What is PROVEN and banked (flat, this session):**
- The held vigor charge is the on-demand repro exactly as predicted. The FX
  split cleanly in two: **riders** (fingertip flames, weapon embers - socket
  FX on the child model components; they follow the driven hand ALREADY) and
  the **frozen family** (the charge plume, the vigor-ready sparkle, and per
  the headset the muzzle flash + tracer) which tracks the CAMERA anchor at
  the authored offset - with the drive released, the authored arm lands
  exactly inside the flame.
- **Every strong theory for the frozen family's position source was
  falsified by measurement** (ENGINE_NOTES "s50"): tick-time SpaceBases reads
  (the engine's eval almost never restamps SpaceBases while the drive runs -
  cleanTicks 12181/12183 on the new instrument), GetPlayerViewPoint consumers
  (caller census identical with/without charge), every reachable Attachments
  array (FP comp -> only the 3 child models; children/pawn comps/actor
  Components -> empty/no PSCs), the script-side XEmitterPool (empty). The
  effect playback tick was decoded to its per-record update (rva 0x3EC4C0,
  hooked probe-only as `bsifx u`) - and ALSO exonerated: ~2 calls/s, none
  first-person, while the plume updates at 90 Hz.
- **New engine knowledge:** the attachment walker (rva 0x2A1B20, vtable slot
  43) + FAttachment layout; the FP rig's child-model architecture (vigor hand
  at L_Grip, prop at PlayerHandsLarm22, weapon at R_Grip - why holdables ride
  for free); ParentAnimComponent redirect (+0x2F4/+0x2F8/+0x2FC); the vigor
  IS an XWeapon (slot 0 = Plasmid_EnrageFounder - the save's "devil face"
  vigor is ENRAGE; slot 1 = PistolFounder); the effect manager + tick-helper
  + record-table layout. All with derivations in ENGINE_NOTES.
- **Shipped code (all bioshockinf-local):** `bsifx` - the attach-walker hook
  (default ON: the dirty-count instrument + eval-restamp edge cover, honestly
  documented as NOT the family fix) and `bsifx u` - the effect-update probe
  (default OFF, next session's instrument). Both behind prologue + arity
  gates, plus a NEW vtable-slot identity gate on the walker.

**Session harness notes:** award dialogs REPLAY on every checkpoint load
(~4 modal `btn a press` dismissals before any input lands); Enrage HOLD =
charge, RELEASE = throw (a stray release burned the Blue Ribbon carpet and
drained the salts - `Restart Checkpoint` refills them); the game eats
trigger edges while unfocused-paused, so sim input needs the
foreground-then-edge pattern (focused-input.ps1 in the session scratchpad).

**The user's mid-session call: re-scope (option c).** The hunt resumes in a
later session from the banked forks: probe ALL records through the
effect-update seam (rva 0x3EC4C0, `bsifx u` - installed, probe default off)
and its other callers, instrument the decoded tick table (helper vtable
slot 36 -> rva 0x436490, stride-0x74 records), and the record stamp-pair
globals (0x135DC68/6C). The riders half of the family (weapon + vigor-hand
models and their socket FX) follows the driven hand ALREADY.

**HEADSET VERDICTS on the s50 build (user, 2026-08-10, same night):**
1. **FLOURISH: WORKS** - the chord fires the gesture. The ~2 s lead was
   rejected ("bad"); s50b response: the lead/tail are now LIVE-TUNABLE
   (`bsiflourish lead <ms>` / `tail <ms>`) and the default is **200 ms**
   (flat-proven full amplitude at 200; the s50 1800 was over-cautious).
   Re-judge; tune live if the start reads clipped.
2. **FIRE-SWING: STILL PRESENT** - "firing makes the arm/SHOULDERS jump and
   come back". The fire-glue provably zeros the DRIVEN ANCHORS' swing (the
   A-B-A stands), so what the user sees must live elsewhere: the prime
   suspect is the UNDRIVEN bones (chest/clavicle - "shoulders" - which the
   engine owns by design and the fire anim moves), and/or engine restamps
   beating the rewrite cadence render-side. NEXT: an all-bones peak
   instrument (travel currently samples only the two anchors), then decide
   whether to drive/correct the chest bones fire-scoped too.
3. **FOV-EDGE DRIFT: UNCHANGED** - worst on the LEFT (weapon pulls toward
   the camera "by a lot"). The rendered-pose eye tags did not move the
   symptom - consistent with VDXR already reporting parallel same-IPD views
   (the fix is identity there, exactly as on the sim). The pose-tag
   mechanism is now EXONERATED alongside the compose chain and the lens
   split. NEXT instruments: (a) log the actual VDXR located view poses/fovs
   in a headset session (one command + the log tells whether any per-eye
   cant/IPD delta even exists); (b) a HAND-ANCHORED reference quad (the aim
   dot machinery, parked at the grip pose) - in-headset, if the quad and the
   hand model separate laterally the error is in the projection/submission
   lane, if they move together the hand's world position itself moves: one
   look, two hypothesis families discriminated.

Also flat-found while probing: the muzzle SMOKE trail is another
camera-anchored frozen-family member (visible in a slow-pace capture).

**NEXT SESSION:** (1) the fire-swing's undriven-bones instrument + fix,
(2) the FOV-edge discriminator instruments above, (3) flourish lead verdict
(tuner shipped), then the FX-origin banked forks per the user's call.

### Infinite: state after session 49b (kept for the record; superseded by s50 above) (THE STANCE IS KILLED - the 'Lowered' clamp, A-B-A proven, ships default ON; branch `si49-inf-stance-lens-tracer`)

**Session 49b (2026-08-10, continuing s49 on the user's priority-1-only
directive) KILLED THE STANCE AT THE ROOT.** The mechanism, named end to end:
the 101-deg stance is the **lowered-idle settle inside the FP Morpheme
graph** - the game drives the control param **'Lowered' (id 2)** into the FP
network at 90 Hz; a fire posts 0.0 (raised) and it ramps back to 1.0 in ~7 s;
the graph then settles into the stance 150-240 s later with NO message
entering the network at the onset. **The kill: the funnel hook rewrites every
FP-network 'Lowered' post to 0.0** - the graph stays in the raised subgraph
(the ready pose) where no settle exists. A-B-A on the live save: clamp ON =
435 s idle, 0/43 bones (every unclamped leg entered within 150-240 s);
clamp OFF = the stance returned on schedule with the exact L_Grip 101.11
signature; the shipping auto-derive (descriptor at attachment+0x2CC,
name-verified, refuse-on-drift) armed itself 36 ms after the rig resolve and
held a 407 s green leg with the fire/aim battery clean. **Ships DEFAULT ON**;
`bsifidget req clamp off` is the in-headset bisect (F10: "STANCE KILL"
checkbox). The boot pose still needs the established fire-once ritual (the
clamp prevents re-entry, not exit from the pre-settled load state).

Also this session: the Morpheme message-lane fully mapped (the inner post
funnel 0x5CED00, typed control-param messages, the wrappers, the engine's
descriptor cache on the attachment), falsifications 7 (funnel silent at
onset) and 8 (TwoHandFallback_Weight - which A-B-A-proved to own the s48
"40-deg alert-relax" pose pair, on demand via the new `bsifidget post`),
and the manual param-poster experiment platform. Eight falsified levers now
documented in ENGINE_NOTES; the ladder ended at the real root.

**HEADSET VERDICTS on the s49b build (user, 2026-08-10):**

1. **THE STANCE KILL IS CONFIRMED - "everything looks awesome, the fix
   worked as I expected."** No regressions observed; both hands unaffected;
   the idle stance never enters; normal idle animation remains and reads
   right. The 40-deg alert-relax lane is also gone (the user confirms both
   hand drifts vanished together - consistent with both riding the lowered
   subgraph). Merged to `bioshock-infinite` per the user's call.
2. **FOV-edge drift, refined observation (the next lens-session's input):
   it is ASYMMETRIC by direction.** Moving the RIGHT hand LEFT of center:
   the hand model comes CLOSER to the face (very visible, feels less
   controlled); moving it RIGHT: the model reads pushed slightly AWAY.
   Near/far error that flips sign with horizontal off-axis direction - a
   translation-space or basis asymmetry, not a symmetric scale effect;
   projection split already eliminated (s49 one-lens verdict).

**Post-verdict additions (user, 2026-08-10, same night):** vigor CAST anims
confirmed normal under the clamp. Two new items from the headset: (a) the
occasional VIGOR FLOURISH (the 'SubtleFidget' show-off action) is lost with
the idle lane killed - user asks for it on a BUTTON (thumbrest + key; the
trigger function is known and callable - reflect::call_on_object(attachment,
"StartSubtleFidget") reproduced it all session; needs a raised-subgraph
visual test + must not re-arm anything behind the clamp); (b) **the vigor
CHARGE/READY effect freezes in place** while holding the vigor key instead
of following the hand (tested with the fire vigor) - same FX-origin family
as the tracer: engine-side FX positions are still camera/authored-anchored
while the model rides the driven bones. The hold-to-charge FX is a BETTER
flat test subject than the short-lived tracer streak (persistent,
reproducible on demand).

**NEXT SESSION**: (1) THE FX-ORIGIN SEAM - one origin for model, laser,
bullet, tracer, muzzle flash AND the vigor charge FX (recon banked in s49:
walk XWeaponModelFirstPerson, bsidiff RecentTracerParticles across a shot;
use the held fire-vigor charge as the persistent flat repro); (2) the
FOV-edge drift with the asymmetry observation above as the entry point
(check the hand-model compose chain for a view-dependent term - the sign
flip suggests an eye/head-relative offset entering the model transform, not
the lens); (3) the flourish-on-a-button feature (small, needs its visual
test). Later: wrist refinement, SingleLineCheck.

### Infinite: state after session 49 (kept for the record; superseded by s49b above) (StartSubtleFidget DECODED and falsified as the root, the Morpheme residual named, the gameplay lens verdict: ONE frustum; branch `si49-inf-stance-lens-tracer`)

**Session 49 (2026-08-09, late) worked the three s48 verdicts in priority
order. Zero core changes, all bioshockinf-local, nothing merged.**

1. **STANCE (priority 1): two more root levers built, both FALSIFIED live -
   six falsifications total - and the real mechanism cornered.** Offline
   derivation (exec census re-run, zero boots): StartSubtleFidget is a NATIVE
   function; its impl (0x51BA00, fully decoded in ENGINE_NOTES) IS the
   scheduler - plays anim action 'SubtleFidget' (41347) by name on the runtime
   XMorphemeNetwork (component+0x228) and re-arms its own SetTimer from
   SubtleFidgetTimeRange. Two MinHook choke points built with positive
   controls green end-to-end (`bsifidget impl ...` at the impl, `bsifidget
   act ...` at the network's play-by-name entry 0x5D1520 with caller-RVA
   attribution): **blocking the impl - stance re-entered with ZERO impl calls
   (falsification 5); blocking the action by name - two natural plays refused
   and the stance re-entered anyway (falsification 6).** The pose mover is a
   MORPHEME-INTERNAL transition (the network's own state machine; the FP
   request vocabulary has rqHandFidget staked). Next rungs: the Morpheme
   request/transition layer off the runtime network, or the "rqHandFidget"
   string-xref hunt. Both hooks ship as default-probe instruments.
2. **FOV-EDGE DRIFT (the lens question): the missing s41 check finally RAN,
   in gameplay, and the projection-split hypothesis is DEAD** - bsilens with
   the viewmodel rendering: 301 rounds, lens1 support 100% (tanH 1.1810 /
   tanV 1.2634, vertical-referenced law holds), **lens2 0%. No foreground
   frustum exists on this build**; BS1-style counter-modeling is
   measured-unnecessary. The pixel-level model-vs-dot station protocol was
   attempted (6 stations x 3 isolation captures) but the live scene's ambient
   motion (NPCs/flags/viewmodel sway) drowns window-grab isolation diffs -
   the symptom stays open as a headset-side/compositor question with the
   game-side mechanism eliminated.
3. **TRACER (recon): vocabulary confirmed live; the dispatch is pure C++ (no
   script natives); weapons reachable via pawn+0x314 -> XInventoryManager
   (+0x1FC melee, +0x200..+0x20C four XWeapon slots - corrects the s45b
   pawn+0xD8 note); TracerFX/TracerSocketNames/MuzzleSocketName are NOT on
   XWeapon's 950-field chain** - they live on the FP weapon model or an FX
   definition object. Next: identify the equipped slot, walk
   XWeaponModelFirstPerson, bsidiff RecentTracerParticles across a shot.
4. Tooling landed: `bsichase` pointer-chain walker, `bones::component()`,
   both fidget hooks; boot recipe hardened (click-driven menu navigation -
   the Enter-spam wedge is beaten by game-shot + game-click MAIN GAME ->
   CONTINUE). Traps recorded: `bsiaim dotdist` is eaten by the `dot` prefix
   branch (turns BOTH dots off); the PE vtable filter breaks bsicallat's
   occupant gate (`bsifidget off` first); a game-cmd write during load-time
   pump lag is eaten silently (re-send after resolve).

**NEXT SESSION, in priority order**: (1) the Morpheme-internal stance
transition - hunt the request/transition machinery off the runtime network
(raw bsichase descent) and/or the rqHandFidget poster via string xref; the
apply plumbing and both choke hooks are ready for whatever it names.
(2) The tracer feed: equipped-slot identification, the FP weapon model walk,
RecentTracerParticles bsidiff across a shot, then the seam choice.
(3) The FOV-edge symptom as a headset/compositor question (the game
projection is exonerated). Later: wrist refinement, SingleLineCheck.

### Infinite: state after session 48 (kept for the falsification record; superseded by s49 above) (VERDICT FIXES: locomotion pinned, wrist bend reworked, one hide mode - and the stance root hunt narrowed to the UBOOL; branch `si48-inf-verdict-fixes`)

**Session 48 (2026-08-09, same night) worked the six S46+S47 headset verdicts
(recorded verbatim in the s47 section below). Zero core changes, all
bioshockinf-local, nothing merged.** Landed and flat-proven:

1. **Locomotion swim FIXED (verdict 2)**: measured 9.26 UU of rigid model wobble
   at walk speed, mechanism named (one-frame phase skew between fc.engineLoc and
   the attachment L2W - the lag probe read the skew directly, 11.2 UU walking,
   0.00 stationary), fixed by composing the hand relative to the WRITTEN camera
   (camPin, default ON, `bsibones campin off` bisects). 9.26 -> 1.72 UU.
2. **Wrist sliders now bend the WRIST (verdict 3)**: the extra quat moved from
   the arm chain to the hand cluster - hand tilts about the grip, forearm
   stays. Flat diff: cluster-only rotation, zero arm entries.
3. **One arms-hide mode (verdict 5)**: style radio deleted; pinch-behind-wrist
   with an F10 depth slider (`bsihands capdepth`, default 10 cm, 0 = old mode).
4. **Fire origin (verdict 4)**: flat-proven the TRACE follows the controller
   (engine origin frozen, substituted origin moved 43.8 UU with a 30 cm hand
   move). The headset symptom is the visible TRACER FX spawn - vocabulary
   staked out in ENGINE_NOTES, the seam hunt is next session's item.
5. **Dynamic dot (verdict 6): infeasible via script on this build** - `Trace`
   is stripped from the name pool (verified). Machinery built and parked; it
   arms when a UWorld::SingleLineCheck C++ derivation lands.
6. **THE STANCE (verdict 1) - root found to be NATIVE, kill one step away.**
   The glue is retired (default off, user verdict). The ProcessEvent vtable
   filter was built, proven to block the StartSubtleFidget dispatch when it
   fires - and then the clean-boot A/B FALSIFIED it as the root: the stance
   re-entered in 8 min with ZERO dispatches (events=1, startSeen=0). The anim
   starts natively; the surviving root is the engine's own
   `bDisableSubtleFidget` UBOOL. The property-chain walker (bsiprop) and the
   bit lever (bsipropbit) are built and verified up to the super-chain walk;
   **finishing needs one booted save** - the last two flat boots wedged in
   menu states (see the trap note in ENGINE_NOTES; the save/menu state may
   have drifted - verify the save on next boot). ALSO found: a second idle
   lane - a uniform 40.00 deg post-fire alert-relax of the whole left arm
   within ~90 s, distinct from the 101.11 stance; recorded as the next
   suspect if a drift survives the UBOOL.

**s48b (same night, user granted the machine): the property-side hunt ran to
completion and narrowed further.** The UProperty layout was fully derived (the
first walk's links were falsified by name semantics and re-anchored - table in
ENGINE_NOTES) and the walker now covers full super chains (635 fields on the
attachment class, 2053 on XHuman). Measured on the live save, in order:
`bDisableSubtleFidget` (attachment+0x214, mask 0x1) SET -> stance returned;
`SubtleFidgetTimeRange` (attachment+0x26C) read **{120, 240} s - exactly the
measured 2-4 min re-onset window, the mechanism confirmed end to end** -
starved to {1e9, 1e9}, write verified held -> stance returned anyway (the live
scheduler reads neither instance property); the ObjectArchetype (the spawn
template, authored {120, 240}) starved too, bools set on both -> **stance
returned again (+5:20). Four property-side hypotheses falsified with held
writes** - the consumer keeps its own timing copy (the anim-tree node).
fidget.cpp's tick_apply (instance + archetype starve, self-derived offsets)
ships DEFAULT OFF as the ready plumbing for whichever object the anim-tree
hunt names; `bsifidget root off` restores authored values.

**HEADSET VERDICTS on the s48 build (user, 2026-08-09, late):**

1. **Locomotion fix CONFIRMED** - "great". camPin is headset-verified.
2. **Wrist sliders better** - refinement wanted later, not now.
3. **STANCE stays PRIORITY 1** - it makes testing everything else harder; the
   directive is unchanged: stop the TRIGGER, at the root.
4. **NEW: model/aim drift near the FOV EDGE** - the hand/gun model does not
   move uniformly with the controller; approaching the edge of the view it
   pulls closer to the headset and drifts off the aim. The user flags it as
   the BS1-shaped problem. NOTE: the "no fg lens on Infinite" conclusion
   (s41/s44) predates the current stereo lens levers - and the standing rule
   says a lens question is never settled by a mono check. Re-open as a LENS/
   PROJECTION question: measure IN STEREO with the hand commanded to edge
   stations, model-vs-dot screen positions compared (the sim's per-eye
   captures + img metrics), before any counter-modeling is even considered.
5. **The VISIBLE bullet origin still reads fixed in space** - it does not ride
   the controller. The TRACE is flat-proven to follow the hand (43.8 UU for a
   30 cm move, this build), so this is the tracer/muzzle-FX spawn seam -
   confirm with the staked GNames vocabulary and substitute or re-parent it.

**NEXT SESSION, in the user's priority order**: (1) the stance trigger kill -
the XFidgetAnimationSelection anim-tree node (it holds its own copy of the
{120, 240} timing, sampled at creation; find it off the component's anim tree,
starve/neutralize THERE - the walker toolchain and apply plumbing are ready);
(2) the FOV-edge model/aim drift - stereo edge-station measurement first;
(3) the tracer-FX spawn seam. Later: wrist slider refinement, SingleLineCheck
for the dynamic dot.

### Infinite: state after session 47 (I8 part 3 - kept for the verdict record; superseded by s48 above)

**Session 47 (2026-08-09) worked the I8 remainder that does NOT ride the pending
s46 headset verdicts. Nothing verdict-riding was touched (glue algebra, fire
substitution, wrist-cap styles, wrist sliders byte-identical), ZERO core changes,
nothing merged. Evidence first on every item; full numbers in the s47 session-log
entry and ENGINE_NOTES ("s47" sections).**

1. **Boot-time glue arming: CLOSED as impossible, by measurement.** The
   boot/checkpoint-load pose IS the 101.11 deg stance (measured at 23 s
   post-resolve, stable, and at 2 min never-fired) - a resolve-time qRef capture
   would pin the stance and invert the glue. First-shot arming stays; the
   pre-first-shot stance is now a certainty, not a maybe (TESTING note upgraded).
2. **The reapply-burst gate (carried s45b): CLOSED measured-no-defect.** New
   staleness counters in `bsibones` status: 33,255 replays across every reachable
   gap class, skippedStale=0, maxAge=63 ms. The edge-triggered release is the
   real protection; the 100 ms gate is untouched backstop.
3. **ANIMTRANS built from evidence, ships OFF and unpersisted.** New `bsibones
   travel` peak tracker measured the discarded authored anchor travel: reload
   R 14 cm / L 48 cm, fire R 3-5 cm. The lever passes it through (dp base = the
   ready anchor translation banked with qRef; anim mode + capture + 120 UU
   fallback). Driven A/B flat: off = pinned, on = authored travel to 1 UU with
   rotation still glued. `bsihands animtrans on|off` + F10 checkbox; no preset
   key (the precedent stands).
4. **World-scale groundwork (I8 open box)**: 1 m -> exactly 150.0 UU re-verified;
   the cm->UU audit table (ENGINE_NOTES) shows every adapter conversion on the
   live fc.worldScale; stale aim.cpp comments corrected. Calibration itself =
   headset, box stays open.
5. **Per-weapon profile SCAFFOLD** (I9 prep): class-name-keyed empty table +
   `reflect::class_name_of` + fire-seam latch + `bsiprofiles`. Measured: the
   seam's Weapon param is NULL on ordinary shots -> pawn-side source is I9 work.
6. **Battery green** on the final build (substituted first shot, both captures,
   0.0000 deg per-hand deviations, no new dumps, inis byte-identical).

**HEADSET VERDICTS IN (user, 2026-08-09, S46+S47 tested together) - six findings,
worked from s48 on branch `si48-inf-verdict-fixes`:**

1. **The stance glue is REJECTED as the approach.** It holds the wrist/grip but
   the SubtleFidget anim still plays on the rest of the model (arm rises, left
   wrist reads wrong). User directive: kill the stance at the ROOT - stop the
   code from ever triggering the anim (it fires on an idle timer) - instead of
   compose-side glue that "will break with different weapons and is currently
   breaking since it affects the whole model."
2. **Locomotion moves the model**: walking with the left stick shifts the
   hands/weapon - unacceptable in VR (affects aim). Stick motion must not
   disturb the model.
3. **The wrist sliders move the ARM, not the wrist** - they read as sweeping the
   whole arm rather than bending the wrist; rework.
4. **Fire origin: correct on a static screen, but NOT following the controller/
   model** - the visible bullet still leaves a fixed screen point instead of
   riding the weapon.
5. **Arms hide: remove the style choice.** Style 0 (collapse-at-grip) still bad
   (skin dives into the wrist); pinch-behind-wrist is decent but shows a
   stretch as if still connected - keep it as the only mode and fix the stretch
   if possible.
6. **Aim itself is FIXED - hole lands on the dot.** New request: a DYNAMIC dot
   (project to the actual hit point) because a fixed-distance dot is hard to
   read against far walls; assess feasibility honestly.

**NEXT after the headset verdicts**: per-weapon profile VALUES + the arsenal save
(I9 - the user produces the save), world-scale calibration in the headset, the
test-from-game-start pass, and whatever the verdicts re-scope.

### Infinite: current state after session 46 (I8 part 2: STANCE KILLED, BULLETS FROM THE GUN, wrist levers built - all flat-green; branch `si46b-inf-stance-origin`, headset verdict pending)

**Session 46 (2026-08-09) worked exactly the four open s45b headset findings.
Everything bioshockinf-local, ZERO core changes, all levers behind toggles, no merge.**

1. **FINDING 1 CLOSED FLAT - the persistent stance is the SubtleFidget lane, and the
   READY-POSE GLUE kills it.** Measured with the new `bsibones snap/diff` (drive off,
   raw bank): the stance is a discrete second pose of the LEFT hand - grip+palm rotate
   RIGIDLY 101.11 deg with finger curl on top, re-entering ~2.5 min after a shot and
   HOLDING; `bsicallat <attach> StartSubtleFidget` reproduces it on demand. Console
   `set`-by-name is DEAD on this retail build (even the `bHidden` positive control
   nulled under `bsidiff`), so the lever is compose-side: fold
   `corr = qRef x conj(src[anchor])` into the bones compose per hand - the anchor pins
   to controller x captured-ready, ALL relative articulation (fingers, reload, the
   vigor flourish's articulation) passes through by construction. qRef AUTO-CAPTURES
   1.2 s after every player shot (the fire seam signals it; the engine itself resets
   the stance on fire). **A-B-A flat: 0.33 deg drift through a stance onset with the
   glue ON -> uniform 101.11 deg the instant it goes OFF -> 0.06 deg back ON. Default
   ON.** `bsibones glue on|off|capture` + F10 checkbox + capture button.
2. **FINDINGS 2+3 CLOSED FLAT - the fire-ORIGIN seam.** Derivation (offline dump
   re-run + thunk disasm, full trail in ENGINE_NOTES): `AXPawn::
   XGetWeaponStartTraceLocation` impl 0x5344A0 - its body calls the camera's OWN
   GetPlayerViewPoint (the hooked kGetPlayerViewPointRva!), so the trace origin IS
   the camera eye - the diagnosed parallax root, read from the disassembly. The
   Floating variant (13 C++ callers) routes through the same impl = one choke point;
   the camera never calls it, so no feedback hazard. Probe measured live: ONE call
   per player shot, engine origin 77.4 UU (51.6 cm) from the hand - the exact
   headset hole-vs-dot magnitude. `bsifire on` substitutes xyz with the SAME
   ray_pose_from_xr origin the dot uses (latched hand shared via
   aim::last_aiming_hand, origin sliders cm->UU, 200 UU cap for melee/Sky-Hook
   short traces, player-pawn gate - NPCs call the same native). Both hands verified
   (vigor cast routes through the seam too, left origin mirrors right). **SHIPS
   ARMED**; `bsifire off` is the bisect lever. New fire.cpp/.h.
3. **FINDING 4 (wrist cap): three hide styles behind `bsihands hidestyle 0|1|2`** +
   F10 radio (arms=hide only). Flat captures: style 0 (current collapse-at-grip) is
   a clean taper; **style 1 (keep forearm twist) is BROKEN - a giant skin hood**
   (kept twist bones stretch skin against the collapsed upper arm; capture in the
   scratchpad); style 2 (collapse ~10 cm behind the wrist) gives a slightly longer
   clean stub. Headset A/B is styles 0 vs 2; style 1 stays for completeness only.
4. **FINDING 5 (left wrist): the per-hand ARM-RELATIVE wrist quat is BUILT and
   inert** - `bsihands wrist l|r <p y r>` + F10 sliders, an extra quat in the ARM
   chain compose only, about the grip (qtcArm = qtc x W in quat AND dp term).
   Smoke: 20 deg commanded -> EXACTLY the 4 left arm bones at 20.00 deg, cluster
   and right hand untouched. NOT persisted (defaults 0; a preset key waits until
   the headset says it earns its keep). Expectation: finding 1's glue fixes the
   wrist angle outright - the sliders are the fallback tuner.
5. **New instruments** (all game-thread gated): `bsibones snap <0-3> [written]` /
   `bsibones diff <a> <b>` (raw-bank or written-bank atom snapshots, sign-safe
   geodesic angles, rig-generation interlock), `bsidiff <hexaddr> <dwords>`
   (snapshot-compare, prints only CHANGED dwords - the UBOOL hunt instrument; the
   attachment's noise baseline is 2 dwords in 2 KB).
6. **Non-regression flat battery, all green on the shipping build**: three stations
   including full rolls - `aimRayMaxDevDegL/R = 0.0000 both hands` (per-hand fields;
   the legacy single field reads 86 deg with two dots, the documented artifact);
   scale 0.5 uniform on every right-hand bone; arms follow/hide/game transitions;
   per-hand release + re-enable; SR gates 90/90/180/90; single rig resolve, zero
   fails; first shot on the shipping build SUBSTITUTED with both ready poses
   captured; game alive, no new crash dumps (the three in CrashDumps predate the
   session: 00:15-00:46).
7. **Traps this session**: an EMPTY command.txt (zero bytes) crashes launch-game.ps1
   (`.Trim()` on null) - DELETE the file, never truncate it; the offline native-dump
   regex misses pooled-suffix exec names (2079 vs the s34 census 2647 - e.g. the
   Floating variant), so use the dump to FIND and `bsinative` to VERIFY; game-shot
   saves extensionless files (img-diff takes them as-is).

**HEADSET PENDING (next session opens with it): the S46 checklist in
docs/bioshockinfinite/TESTING.md.** Judgments owed: stance gone with transients
alive (fire/reload/VIGOR FLOURISH - flagged risk: if the flourish died, the glue ate
its rigid component; report and we re-scope), bullets from the gun, hole ON dot at a
near and a far wall, wrist-cap 0-vs-2, left-wrist feel, the standing non-regression
sweep. **vrpreset.ini and XUserOptions.ini verified byte-identical at session end;
nothing else touched.**

**NEXT after the headset verdict**: per-weapon aim/model profiles + the arsenal save,
animtrans, the reapply-burst gate refinement, world-scale calibration, and the
test-from-game-start pass (ROADMAP I8's two open boxes).

### Infinite: current state after session 45b (I8 FLAT HALF DONE - hands and weapon OFF the headset, calibration surface live; branch `si45b-inf-hands`, headset verdict pending)

**Session 45b (2026-08-08/09) is the REDO of I8's opening block** (the user discarded
the original s45/s46 results; branch `si45b-inf-hands` off the s44b tip - the old
`si45-inf-hands`/`si46-*` branches were left untouched and unread). Everything below
was derived fresh this session. **ZERO core/tools changes - the whole session is
bioshockinf-local**, so no BS1/BS2 inertness proof is owed and none could be needed.

1. **THE RIG, DERIVED BY INTERVENTION**: pawn `GetFirstPersonAttachment()` (native,
   by-name) -> XFirstPersonAttachment actor -> XSkeletalMeshComponent at +0x218 = THE
   viewmodel renderer: ONE 43-bone skeleton (PlayerHands*, L_Grip 1, R_Grip 22)
   carrying BOTH hands AND the weapon (HideBoneByName(PlayerHandsChest) removed
   everything; R_Grip alone removed hand+pistol, forearm stayed). SpaceBases +0x290
   (32-byte FBoneAtoms; the GetBoneLocation cross-check picked it over LocalAtoms to
   0.1 UU), LocalToWorld +0x60, RefSkeleton mesh+0x74 (NAME-FLAT - parents all 0,
   names carry the structure). All in patterns.h with derivations in ENGINE_NOTES.
2. **THE ORACLES THAT SHAPED THE DESIGN**: Morpheme restamps BOTH banks - trans, quat
   AND SCALE - at tick cadence even while auto-paused. So BS2's never-adopt-scale rule
   does not transfer (adoption takes whole atoms), release = stop writing (engine
   self-heals; BS1's freed-skeleton release hazard is deleted by design), and the
   drive writes per pass-1 camera dispatch with pass-2 verbatim reapply.
3. **THE DRIVE** (bones/hands/frame_context.cpp|.h, BS2's shape, zero numbers):
   FrameContext published once per dispatch by drive_view; the aim ray AND the model
   consume the same pure chains (trim as a quat in the controller's LOCAL frame, the
   laser's own algebra). Per-hand everything; cluster = grip+palm+digits by name; arms
   game/follow/hide (default follow, hide = collapse+zero-scale); adopt-then-compose
   with qtc = conj(qa) x qt; per-hand edge-triggered release.
4. **FLAT ACCEPTANCE, ALL EXACT**: ground truth 1.000 m -> 150.0 UU (worldScale 150),
   zero cross-axis; five stations INCLUDING 60/-90 deg ROLL -> written rotator matches
   commanded to THE UNIT, aimRayMaxDevDegL/R = 0.0000 at every station both hands;
   arms-hide leaves hand+gun with no skin web; scale 0.5 shrinks hand AND pistol
   uniformly about the grip (anchor loc unchanged to the digit - **the BS2
   inverse-scale drum trap did NOT reproduce; no separate weapon lane needed**);
   SR gates 90/90/180/90; zero faults.
5. **STICK-Y: MEASURED, NO DEFECT.** Full stick-up for 3 s - written model pose
   bit-identical, picture at noise floor. publish_vr_gameplay stays UNCALLED; the
   take_snap_steps landmine never arises.
6. **CALIBRATION SURFACE**: F10 "HANDS + MODEL (I8)" (L/R radio + one slider set:
   trim P/Y/R, offset F/R/U, scale; arms radio; anim checkbox) + AIM block gains
   per-hand trim P/Y and ray-origin F/R/U (trim rides ray+laser+dot together).
   **vrpreset registry 9 -> 36 keys** (user-approved batch; animTrans deliberately
   not persisted - not implemented). Seam verbs: bsihands, bsibones, bsiaim trim/origin.
7. **New instruments**: bsiarray (TArray walker), bsidump (typed triage), bsicallat
   parm ECHO (readable returns - it resolved the whole rig in one step), i:/n: arg
   shapes, and the UClass-fixpoint hardening of object_class_name (a raw cache struct
   at pawn+0x0D8 walked as a fake weapon-model object under the old gate).
8. **Traps recorded** (ENGINE_NOTES): fname_text's 64-byte buffer contract (silent
   empty strings cost a boot); the sim's `hand X aim pose` DEAD SLOT (aimWorld is
   always grip+aimtrim - drive stations via `grip pose` + `aimtrim 0 0`); modal
   dialogs freeze the world while the render keeps presenting (positive control
   before judging any intervention picture).

**HEADSET VERDICTS IN (user, 2026-08-09).** What works, verbatim scope: hand and gun
models move correctly with the controllers; models and lasers in sync, NO drift; the
sliders work for model and aim; hand scale is very good and scales the weapon
correctly; aiming with both hands works, hands independent. **The I8 core is
headset-confirmed.**

**The six findings, with their readings:**

1. **THE PERSISTENT STANCE ANIMATION (the headline next item)**: the weapon's idle
   stance HOLDS a non-forward pose until firing resets it. The user wants exactly the
   PERSISTENT stance suppressed while keeping every transient (reload, the occasional
   left-hand vigor flourish they explicitly like). Since the drive pins the grip
   POSITION, the stance can only enter through the ADOPTED anchor/cluster quats -
   candidate levers, in evidence order: (a) measure which bones the stance writes
   (bsibones vs a post-fire sample), (b) `bDisableFirstPersonAttachmentSubtleFidget`
   exists in the FName pool - a possible source-side kill, (c) pin the ANCHOR quat to
   a captured ready-pose reference while adopting everything else, (d) a time-domain
   high-pass (persistent deviation servoed out, transients pass). Do NOT re-tune the
   left wrist before this lands (user's own sequencing).
2. **Hole lands slightly ABOVE the dot** and 3. **bullets visibly leave the screen
   center, not the gun** - read as ONE root cause: the engine's trace ORIGIN is still
   the camera/eye while our dot ray starts at the hand; two parallel rays from
   different origins never agree on a finite wall. The fix is the fire-ORIGIN seam
   (BS1's origin-substitution shape, this engine's derivation - the s44 "instrument
   the trace result" rung is the front door; AXWeapon's native list is already dumped).
   Expect 2 to collapse when 3 lands; only then bake any residual as aim trim.
4. **Arms-hide wrist cap looks strange** - the wrist-boundary vertices pinch to the
   collapsed grip point. Cosmetic; candidates: keep the forearm twist bones (arm22/21)
   driven and hide only upper arm/parent, or collapse to a wrist-offset point.
5. **Left wrist angle reads unnatural** - BLOCKED behind finding 1 by design, and
   the user's correction (2026-08-09) is load-bearing: **a model trim CANNOT fix
   this.** In follow mode the arm rides the hand with the same composed rotation, so
   a trim rotates hand+arm as one rigid piece - the hand-vs-forearm RELATIVE angle is
   the ADOPTED ENGINE POSE (the held vigor stance), i.e. finding 1's mechanism. If a
   bad angle survives the stance fix, the lever is a new ARM-RELATIVE wrist
   adjustment: an extra quat in the ARM chain's compose only (about the grip), hand/
   aim/laser untouched - the cluster/arm split in bones.cpp already supports it.
6. **Anim toggle OFF misaligns the models** - recorded, NOT to be fixed (user keeps
   animations on; rigid mode uses the boot-time snapshot which embeds whatever stance
   was live at resolve).

**NEXT SESSION**: finding 1 (measure -> choose lever -> ship with a toggle), findings
2+3 (the origin seam, hole==dot as acceptance), finding 4 if cheap. Then per-weapon
profiles + the arsenal save, animtrans, the reapply-burst gate refinement.

### Infinite: HEADSET VERDICTS IN (s44b, same night) - I7 CONTROLS AND AIM ARE DONE

**The user tested the s44 build in the headset and returned three verdicts, two of which
overturn what flat testing had concluded:**

1. **Every button works as expected.** (Thumbrest+flick untested - nothing to select
   with yet in that save.)
2. **The pause menu works from the controller**, opening AND exiting. The flat lane's
   "blocker" was a HARNESS artifact: the menu parks the game thread and `xrsim-cmd`
   deliberately does not foreground, so the presses landed on an auto-paused window.
   Nothing to port from BS2. **Harness rule recorded**: never judge a MENU-context
   input question from the flat lane without asserting focus at the press AND after
   the menu opens - opening a menu is itself a focus event.
3. **AIM WORKS AND IS DECOUPLED FROM THE HEAD** - "I tried to look in different ways
   and aim in the same place and the bullet kept going in the same direction as my
   controller". So `APawn::GetBaseAimRotation` IS the fire path, and s44's flat null
   was a FALSE NEGATIVE of the instrument. Refusing to claim the effect on that
   evidence was right; the picture diff simply could not show it (the only thing that
   responds visibly to firing in that walled-in checkpoint is the viewmodel, which sits
   in the same screen region whatever the aim).

**Shipped in response (s44b):**

- **The aim write is ARMED at boot.**
- **Both hands, decoupled.** The seam is pawn-level and carries ONE rotation, so the
  hand is chosen by TRIGGER attribution, latched (right = weapon, left = vigor) - a
  trace can run a frame or two after the trigger releases, and flipping mid-shot would
  throw it. `aiming hand` is live in the F10 panel.
- **Aim dot and laser per hand, individually toggleable. Dots ON by default** (user's
  call), lasers off. Core's EXISTING two-slot API (added for BS2's dual wield) - **no
  core change was needed.**
- **The dot round-trips the ray deliberately**: it takes the FRotator the seam actually
  wrote, undoes the game-yaw basis and converts back to XR, so a basis error shows as a
  dot off the controller's forward rather than being hidden.

**Flat verification of s44b**: hands swung to OPPOSITE angles (left -45/+10, right
+45/-10) and **both `aimRayMaxDevDegL/R` read 0.0000** - each dot sits exactly on its
OWN hand's ray, proving the round-trip math and the per-hand attribution in one
measurement. Hand attribution flips L/R with the triggers. With both lasers on, 10 quad
layers submit, deviations 0.0000/0.0198. Zero faults. (Use the PER-HAND fields - the
legacy single-beam `aimRayMaxDevDeg` reads 7.59 with two beams, the documented
artifact.)

**NEXT (session 45)**: I8 - the weapon model and hands still ride the headset. That is
the next milestone and the user has been expecting it. `controller_ray` is already
built on the view's own basis, so the model drive should consume the SAME ray the aim
seam does rather than deriving a second one that drifts. Aim calibration (does the hole
land on the dot?) rides the next headset session.

### Infinite: current state after session 44 (I7 CONTROLS: the pad map lands and is proven both ways; aim seam DERIVED, its write UNPROVEN and off; branch `si44-inf-controls`)

**Session 44 (2026-08-06) ran the I7 controls block: the per-game pad map, the full
Touch layout, and the aim derivation. Five commits. Nothing merged.**

1. **THE INSTRUMENT FIRST (core, log-only)**: `vrinput padlog on` emits one line per
   composed BUTTON or trigger EDGE with the bits NAMED. Until now nothing could read
   the button word the game actually saw, so every claim about an XR-to-pad mapping
   had to be inferred from a game effect - save-dependent, timing-dependent, and for
   a melee swing or a weapon cycle on a one-weapon save not observable at all. Plus
   an adapter-local `loc.z` min/max/last WINDOW on the Infinite heartbeat: the 1 Hz
   beat is a point sample and a jump's whole arc fits between two of them.
2. **THE SEAM (core, additive, opt-in)**: `PadProfile` + two `constexpr PadMap`
   tables. One atomic, default `Bioshock1`, one relaxed load per compose - a single
   scalar rather than a struct of atomics so a live A/B can never compose a
   half-switched pad. Table-driven: faces, grips, both stick clicks (0 = "not
   forwarded", which is how BS1 keeps eating RS-click), whether the flick lane
   honours the ammomod preference, and the four flick directions (0 = "never
   emitted", which is how BS1 keeps its three-way select). Choice of seam and the
   REJECTED bsi-local duplicate: ARCHITECTURE decision log.
3. **BS1 INERTNESS, MEASURED PER CONTROL** (the banked `claimRatioH` is stereo
   geometry and cannot see an input regression, so it is the "nothing else moved"
   control, not the proof): faces still A/Y/X/B, RS-click still produces NOTHING,
   grips at 0.60/0.75/0.60/0.50 reproduce the 0.70-press / 0.55-release hysteresis
   exactly, menu tap/hold still START/BACK, flick still DU/DD/DL with **no DR ever**,
   turn suppression by an A-B pair (rx 32767 -> 0). claimRatioH 1.01769 == 1.018,
   zero faults, and **zero `pad profile` lines in BS1's log** - it never called the
   setter.
4. **INFINITE'S MAP, MEASURED THE SAME WAY**: faces straight through, **RS-click
   FORWARDED** (0x0080, for XToggleZoom - the headline inversion from BS1), grips ->
   LB/RB, triggers analog, menu tap/hold, thumbrest+flick -> DU/DD/DL and the **new
   DR**, stick polarity. Two A-B pairs rather than assumptions. Layer 2 in the save:
   **CROUCH and JUMP PROVEN by quantity** (crouch is a persistent ~110 UU toggle;
   jump is a 98.6 UU arc inside one beat). LB/RT/LT each produce a large reverting
   render change but the whole-frame diff **cannot say which binding fired**;
   NextWeapon and sprint are bit-level only WITH the reason (one gun; the checkpoint
   is walled in, proven by a no-click control).
5. **CONTROLS NOW ARM AT BOOT** - `inputOn`, a 9th preset key, default on. Without it
   a headset boot had no controller and no way to fix that, since every in-headset
   judgment must be an F10 control and reaching F10 needs a controller.
6. **THE AIM SEAM IS DERIVED**: `APawn::GetBaseAimRotation`, a VIRTUAL at pawn vtable
   **+0x2E8**, `FRotator* __thiscall (FRotator*)`, **`ret 4`**. From the exec thunk
   the s34 census had already recorded with zero E8 callers; because it is virtual
   the implementation is read off a LIVE pawn (rva 0x244CC0) and no impl RVA is
   recorded. Its body identifies it beyond doubt (delegates to the CONTROLLER's
   +0x2F4; otherwise Rotation at +0x50 with the stock UE3 `[pawn+0x235]<<8`
   RemoteViewPitch fixup). 32k calls, zero faults.
7. **AND THE SESSION'S OPENING ASSUMPTION IS FALSIFIED.** "The shot stays where the
   BODY faces" is **not true here**: with the hand parked and only the head moving,
   the engine's aim tracks the head degree for degree (+40 -> -130, -40 -> -50, back
   to 0 -> -90) while the ray holds still. The aim chain is downstream of the camera
   drive, so **shots already land where you LOOK**. The open half is aim following the
   CONTROLLER. This is exactly what the evidence-first gate exists for - a fix was
   about to be built for a defect that does not exist.
8. **THE WRITE EXECUTES BUT IS UNPROVEN, SO IT SHIPS OFF.** View fixed, two shots 70
   deg apart in commanded aim: post-shot frames **pixel-identical** (0.08%, zero
   covered cells) while pre-shot frames differ 2.6% from the viewmodel alone. The
   positive control could not be built - seeing an impact move needs the same view
   with different aim, and the only thing producing that is the substitution under
   test; turning the head instead moves the camera and swamps it at 60.9%. So the
   negative is real but **UNATTRIBUTED**.

**NEW BLOCKER (s44): the pause menu does not consume the synthetic pad.** START opens
it (55.8% of pixels) and no controller button closes it - keyboard Escape does. NOT
the bridge: the game keeps polling XInput through our wrapper at ~92/s the whole time
it is up (iat 153614 -> 154534 -> 155177), so the state arrives and the UI ignores it.
Leading candidate is BS2's shape (a synthetic pad never announces itself as the active
input device). BS2's scancode shim is the obvious port, after its own evidence rung.

**NEXT (session 45), in order**:
1. **Instrument the trace RESULT, not the picture** - a hit location readout. Every
   aim conclusion is blocked on it; do not touch a lever until it exists.
2. **Then probe the CONTROLLER's vtable +0x2F4**, which GetBaseAimRotation delegates
   to and which a weapon asking the controller directly would use instead - exactly
   the shape that produces the measured null. Hazard: the camera may read the same
   path, so check with the drive off before arming anything.
3. The pause-menu lane (its own evidence rung, then possibly BS2's scancode shim).
4. Then I8 - the weapon model still rides the headset. The user expected that in this
   block; it is the next milestone, and I7 left `controller_ray` built on the view's
   own basis so the model drive can consume the same ray rather than deriving a
   second one.

**Headset checklist for the user: TESTING.md "S44 controls checklist".** Non-regression
first (smoothness must be unchanged), then every Touch control, then the aim PROBE
(read `divergence`; the write stays off unless they choose to try it).

### Infinite: current state after session 43 (THE STUTTER HUNT: cause NAMED - the 30 s GC tick - fix candidate live, headset verdict pending; branch `si43-inf-stutter`)

**Session 43 (2026-08-06) ran the hunt the user ordered: research first, instrument
second, cause named by intervention third - no resolution/quality trade anywhere.**

1. **RESEARCH (rung 1a, committed before any change)**: `docs/RESEARCH.md` "Session 43"
   - three sourced sweeps (vanilla Infinite stutter fixes, VR-mod prior art with
   license flags, UE3 GC/streaming internals) ending in a 7-entry ranked experiment
   list. Load-bearing facts: PC texture pool is auto-calculated at boot (ini PoolSize
   only honored with `-ReadTexturePoolFromIni`; pool auto-calc is blind to our XR
   swapchains), this build ships GC every 30 s
   (`TimeBetweenPurgingPendingKillObjects=30`) and its own AddToWorld amortizer
   DISABLED, and no VR mod masks 40-120 ms stalls better than the compositor already
   does - the ecosystem consensus is fix-the-cause.
2. **THE SPIKE INSTRUMENT (core, opt-in, BS1-proofed)**: pair intervals > 2x period
   snapshot per-phase last/max tables + stage markers + the unattributed remainder to
   pacetrace.log (`TRACE spike`), `spikes=N` rides the 1 Hz TRACE pairs line, and a
   4 ms sampler stack-captures the stalled draw thread mid-stall (`SPIKE-SAMPLE`,
   watchdog machinery reused). Armed with Infinite stereo; `vrpace spike` is the seam.
3. **THE FLAT REPRO**: the pad lane WALKS in the TWN2 save (s42 scene-lock was that
   save's scripted state) - a 100 s turn-and-walk wander at native 2064x2208
   reproduced the headset signature (4-7 spikes, 29-350 ms, bursts, sd exploding) and
   EVERY spike carried 27-340 ms outside our code vs <= 12 ms inside - the SR
   lane/pacing/capture are exonerated.
4. **THE CAUSE, NAMED BY A-B-A** (`docs/bioshockinfinite/ENGINE_NOTES.md` s43 LIVE
   RESULTS): the 30-second spike grid is the engine's timed GC (game thread parked on
   an FEvent::Wait(100 ms) flush barrier, everyone else idle - Signature A; the
   traversal serialize-walk is Signature B). Intervention 30 -> 300 in the game
   folder's DefaultEngine.ini (the PROVEN propagation source of boot-derived
   XEngine.ini): idle grid GONE, matched wander 4-7 -> 0 spikes; reversal leg brought
   the periodic stalls back (cost varies with garbage - some idle ticks stay
   sub-threshold). **The candidate fix is LIVE: interval=300, backup
   `DefaultEngine.ini.bvr-bak-s43` beside it.**
5. **Rung 0 (grant demo): the combination is FALSIFIED, honestly recorded** -
   `bsiload` resolves class AND `Default__` CDO, CreateInventory(class) and
   AcquireWeapon(CDO) both dispatch and return, but neither registers in the
   NextPlasmid/NextWeapon cycles: the missing seam is the loadout/inventory-manager
   list registration (needs bsifields explicit-object generalization - next time the
   lane opens). LoadCheckpoint facts extended: loads can take 60-100 s (poll the pawn,
   do not conclude no-op), and the s43 boot-1 attract freeze hit once more
   (force-kill protocol worked).

**S43 same-night addendum (the headset verdict came in)**: the stutter is CLOSED by
the user's decision - the GC lever + the user's own quality-settings pass + headset
at 72 Hz ("we're golden"). The user's 10-min VDXR run was measured before that:
avg 67 pairs/s vs the 80 Hz display (sustained GPU over-budget at native x2 eyes +
encoder - the continuous-judder component the settings pass addressed) plus burst
stalls whose stacks matched the flat signatures. PCGamingWiki + the GameFAQs thread
were mined on request (notes in the session log; skip-intro-movies tweak and the
BaseEngine.ini third config layer banked for the harness).

**S43b - THE NEW FOCUS: the "jumpy camera"** (user: head-coupled motion is
bouncy/not-life-like, unlike BS1/BS2). Hypothesis, instrument and A/B are LIVE
(ENGINE_NOTES s43b): core's one-generation-back pose attribution
(`g_viewsContent`) assumes BS1's lockstep renderer; Infinite is threaded with
OneFrameThreadLag, so the content may be TWO generations back - a one-period pose
error scaling with head speed = reprojection wobble. `set_pose_lag(0|1|2)` (core,
default 1 = historical, BS1/BS2 untouched), `bsipose` seam, F10 radios + a
deg/pair error readout. **The user picks the smoothest of three radios in the
headset (S43b checklist in TESTING.md); that answer names the pipeline depth and
becomes Infinite's shipped default.** If all three wobble alike, the hypothesis is
falsified and the next instrument is drive-side (pose age at consume, engine
camera smoothing) - not more settings.

**S43b VERDICT IN (user, same night): pose lag 2 is "perfect - everything is
extremely smooth."** The jumpy camera is SOLVED and named: Infinite's threaded
one-frame-lag renderer presents content two locate generations old; the adapter now
ships `set_pose_lag(2)` at init (headset-verified; F10 radios + `bsipose` stay for
re-derivation). The user then authorized the merge: `si43-inf-stutter` ->
`bioshock-infinite`.

**NEXT (session 44) - CONTROLS + MOTION CONTROLS (the I7 block, BS2's proven shape)**:
the per-game pad map on the ENGINE_NOTES audit spec (additive opt-in seam or
bsi-local duplicate; sim per-control sweep as flat acceptance), then full
Touch-controller bindings through the live IAT lane, then the decoupled-aim
derivation (instrument first - the aim seam on this engine is underived; the
viewmodel shares the world lens, so no fg-FOV machinery exists or is needed).
Model-sync/sliders ride after aim. Only if the user reopens performance: the
texture-pool lane (researched, ranked, ready) with an outdoor save for flat
wandering.

### Infinite: current state after session 42 (superseded by session 43 above - kept for the derivation trail; I6 judder flat half DONE + sync armable; I7 OPEN - pad lane LIVE flat, exec surface truth mapped - branch `si42-inf-judder-bindings`)

**Session 42 (2026-08-05) closed I6's last flat item, opened I7's controls half, and
corrected the cheat-lane's foundational assumption.** Three commits.

1. **THE JUDDER, measured flat (I6)**: new core pair-cadence instrument (TRACE pairs line
   in pacetrace.log, 1 Hz: interval mean/sd/min/max + waitGate = present-thread ms/s
   blocked in the wait handoff; predictedDisplayPeriod published for the first time). The
   SIM's xrWaitFrame GATES strictly: pairs lock to refresh at both 72 and 90 Hz (waitGate
   540-620 ms/s, timeouts 0) - the s41 free-run-beat suspect is a PIPELINING-runtime
   behaviour the sim cannot reproduce; whether VDXR pipelines is answered by reading the
   TRACE pairs lines after the next headset run. The armable fix either way:
   **`vrpace sync` / F10 "Sync pair rate to headset refresh"** - delays only the
   pair-OPENING present to a one-period schedule, never the closing RIGHT present;
   self-collapses when the game is slower. Default OFF in core (set_pace_detach pattern),
   Infinite arms it with stereo (so the preset boots it ON). Measured sync-on at 72:
   pairs 72/s, sd 1.2 -> 1.0 ms, waitGate 615 -> 21 ms/s, SR beat exact 72/72/144/72.
   **BS1 inertness proof run on the build**: full sim lane, claimRatioH 1.01769 == the
   banked 1.018, no sync line in BS1's log, zero faults. **HEADSET VERDICT IN (2026-08-06,
   VD ran at 80 Hz)**: steady-state pacing LOCKED (pairs == refresh, sd 0.3-1 ms) - the
   beat hypothesis is falsified on VDXR too. The judder is recurring HITCH SPIKES
   (39-113 ms pair intervals in bursts every few seconds; sd explodes to 3.6-10.7 ms in
   exactly the bad seconds), worse outdoors at 2064x2208, better at the eye preset - a
   render-load/streaming matter, not pacing. Carried forward as its own hunt (streaming
   vs GC vs shaders vs encoder; hitches-on-head-turns points first at texture-mip
   streaming), plus the user's new "camera slightly jumpy" observation (instrument
   before theorizing). The 30-min soak stays DEFERRED by user decision. **NEXT SESSION
   (user decision, 2026-08-06): the judder/stutter hunt comes BEFORE the aim/motion
   work - performance first. SCOPE DIRECTIVE (user, same night): lowering render
   resolution in heavy areas is NOT an acceptable fix - the eye-preset observation was
   diagnostic evidence only; the hitches must be fixed at the user's chosen native
   resolution (streaming/GC/shader/scheduling class of fix, not a quality trade).** Also: the user banked a save WITH the pistol + two
   vigors, and the cheat-lane verification ran on it same-night (autonomous via
   LoadCheckpoint): `bsicall NextPlasmid` PROVEN BY EFFECT (vigor icon swap + cast hand
   raised + crosshair, 8.5% pixel diff), NextWeapon cycles the single gun (idle-sway
   diff only - a second weapon shows a real swap), AddInvulnerableFlag re-dispatched on
   the pawn (damage verification pends combat), and the s41 VIEWMODEL GUARD CLOSED FOR
   REAL: weapon + hands DRAWN, lens1 == lever exactly (tanV 1.2634, 100% of 211 valid
   samples, delta 0.0%), NO second lens - Infinite's viewmodel shares the world
   projection, no fg-FOV counter-modeling needed (ROADMAP I6 note carries the
   runner-up-threshold caveat).
2. **THE EXEC-SURFACE TRUTH (I7 cheat lane)**: script execs through ConsoleCommand are
   DEAD in retail (god/AllWeapons/behindview/viewmode - zero effect by pixel-identical
   screenshots in a gameplay save; C++ handlers setres/shot stay proven); the give-family
   names DO NOT EXIST (full 69.7k GNames dump); **XCheatManager is never instantiated**
   (PC+0x344 carries only CheatClass; EnableCheats not on the PC chain) - the whole
   CheatManager vocabulary is structurally absent, not gated. What WORKS, all proven live:
   `bsiexec LoadCheckpoint` **loads the newest save from the menu** (the autonomous
   save-entry lane; GNames 62,160 -> 69,719 + relocation + gameplay HUD, twice);
   `bsinames dump` (full pool -> gnames.txt, per-boot indices, text stable); `bsifields`
   (PC field map by class name: pawn XHuman +0x1FC, XCamera +0x240 confirming the s37
   inference, XPlayerInput +0x340; UObject::Name derived at +0x18); `bsicallat` (dispatch
   on any walked object - AddInvulnerableFlag / AddDefaultInventory / SetWeapon /
   CreateInventory dispatch on the pawn; **AcquireWeapon exists and wants a weapon object
   - the s43 grant seam**). The WEAPON-IN-HAND acceptance did not land: both user saves
   predate the first story weapon (grants are story-Kismet, defaults empty), so effects
   are state-confounded. **Session 43 needs one of**: the user plays to the raffle once
   and saves (recommended - makes grants unnecessary for aim work), or the archetype
   lookup + object-arg dispatch rung (scoped in ENGINE_NOTES).
3. **THE PAD LANE (I7 controls), LIVE FLAT**: `bsiinput on` verifies the s34 IAT slot
   (0xCD4814 - target read in XINPUT1_3.dll) and re-points it at core's composing wrapper.
   The game polls through it (iat 5642); **sim right-stick TURNED the camera** (yaw 65 ->
   145 -> -133 deg; screenshot diff 52% of pixels), sim A pressed (camera kick). NO
   UpdateInput pump / SetUseController - Infinite polls XInput itself; BS2's activation
   machinery correctly does not port. Movement was scene-locked in the probe save (loc
   frozen) - re-test walking in free roam before reading it as a defect. **The bindings
   audit is complete** (ENGINE_NOTES "audited retail pad map"): Infinite needs
   straight-through face buttons, RS-click passthrough (XToggleZoom), and NO synthesized
   dpad - core's BS1-semantics `input_sync` is wrong on exactly those counts; the
   per-game map seam (additive opt-in, absent = byte-identical) is session 43's first
   controls task, spec'd in the ROADMAP annotation.

**Hazards**: the pre-existing freeze hit once more (LoadCheckpoint dispatched into the
attract-movie window; force-kill, relaunch, then two clean loads with the
confirm-pump-first discipline). The bsi camera heartbeat rate-limits to ~10 lines per
`bsicam heartbeat on` - re-arm per read window (cost a probe round this session). FName
indices are PER-BOOT - re-dump gnames.txt each boot, only the text is stable.

**NEXT (session 43, the user's plan)**: decoupled aim + motion controls + model
sync/sliders. From this session: the judder verdict closes I6 (checklist ready); the
per-game pad map lands on the audit spec; the loadout lane picks its grant route (user
save vs archetype-lookup rung); the viewmodel-lens guard re-check needs a weapon in hand
(s42 gameplay check: lens1 tracks the lever at delta 0.0%, lens2 absent - but nothing
drew a viewmodel in the pre-weapon save).

### Infinite: current state after session 41 (superseded by session 42 above - kept for the derivation trail; I6 flat half CLOSED - lever, decoder, resolution, presets all measured - branch `si41-inf-lens-config`)

**The eye can now be filled, and every number that claims so was measured twice.** Session 41
landed the whole I6 flat stack in six commits:

1. **THE FOV LEVER** (`bsifov set <deg>`, F10 slider, preset-persisted). The named-property
   lane was tried first per the roadmap and is a recorded negative WITH the mechanism:
   `set XUserOptionsManager FieldOfView` dispatches but writes nothing (zero stable holders
   after a scan for the written value), and every FOV cache in the process (six 82.5f
   holders incl. `[cam+0x214]` DefaultFOV and `[cam+0x3D0]` POV fov) snaps back within a
   tick - the engine recomputes the chain from the option every tick. The lever therefore
   ENFORCES both camera fields per GetPlayerViewPoint dispatch; disarm self-restores via the
   engine's own recompute. Measured: 110 deg -> decode tanH 1.4281 (tan 55 exact) tanV
   0.8033; 130 -> 2.1443/1.2063; monotone, aspect held, 0 faults; tanV to 1.2+ proven =
   nearly 2.5x the native slider cap (0.4933).
2. **THE LAW'S ANCHOR, corrected by its own audit**: the camera degrees value is horizontal
   at a FIXED 16:9 REFERENCE - tanV = tan(deg/2)/(16/9) pinned, tanH = tanV x actual aspect.
   At 1440x1440 with the lever at 100 both decoders read 0.6704 = tan(50)/1.7778 EXACT while
   the first-cut claim (current-aspect) sat 43.7% off - the lens decoder flagged it on its
   first non-16:9 round. patterns.h `kFovRefAspect` carries the measurement. **The ROADMAP
   done-when "claimed projection matches the rendered frustum at a non-16:9 aspect, measured"
   is BANKED**: claim delta 0.0%, claimRatioH captured 0.48705 vs 0.4871 predicted.
3. **THE LIVE LENS DECODER** (`bsilens on|track|status`, F10 section): core grew an opt-in
   UpdateSubresource cb tap (raw 80-byte ring, disarmed cost one relaxed load, BS1/BS2
   byte-identical - proofs in the commit); the adapter decodes by the UE3 matrix law,
   aspect-gates structurally (the load-bearing filter), clusters by tanV, publishes only a
   >=60%-of->=16 majority, names the runner-up as a second lens, refuses rounds otherwise.
   It caught the stale slider-min claim (14.3% loud) AND the wrong claim law (43.7%) in its
   first hour - the audit instrument works. Track mode writes the claim on majority rounds;
   an armed lever always wins.
4. **RESOLUTION, both lanes end-to-end**: `bsires <mode|WxH>` / the RENDER RESOLUTION F10
   picker (flat/squareperf/eye/native/sharp + custom, ini-vs-live compare, aspect warning,
   headset-recommends annotation) applies live setres (backbuffer resized in 20 ms, XR
   rebuild survives) AND writes XUserOptions.ini `[XCore.XUserOptionsManager]` ResolutionX/Y
   (BS2's safety chain, adapter-local; XEngine.ini never touched). **The boot acceptance
   landed: `first Present: backbuffer 1440x1440` after the mod's own ini write.** The square
   render alone narrows tanH (claimRatioH 0.487 at 1:1/lever-100) - the modes pay only
   combined with the lever, now with numbers.
5. **`xrEnumerateViewConfigurationViews` at bring-up** (core, additive): recommendedImageRect
   stored + `recommended_eye_size()` getter; sim serves 2064x2208. Inertness proven by a
   FULL BS1 sim lane on the new build (boot -> gameplay -> vrstereo -> claimRatioH 1.01769 =
   the banked 1.018 baseline, zero faults) - named in the commit.
6. **CONFIG REGISTRY + NAMED PRESETS** (adapter-local BY DECISION - ARCHITECTURE log
   2026-08-05; core extraction deferred to the healing session): one KeyDesc table
   (worldScale, claimTanV, ipdMm, fovLeverDeg, resW, resH) serves vrpreset.ini
   (legacy-compatible, measured), `bsi\presets\<name>.ini`, `vrpreset
   save|saveas|load|list` and F10 slot buttons. **Preset round-trip across a full restart:
   6/6 keys both directions.** A loaded preset's resolution is LATCHED into the picker,
   never auto-applied (mid-headset resize hazard). A recommended **`eye` preset is banked**:
   lever 137 deg (tanV 1.428 ~ the 110-deg eye vertical) + 1600x1712.

**Session hazards, recorded**: the pre-existing unattended-attract freeze hit twice (zero mod
faults, force-kill + relaunch; same signature as s37's unmodified-build hit), and the
game-thread pump lags 30+ s at attract movie transitions - seam commands now go
one-at-a-time with dispatch confirmation (VERIFICATION gotchas 20-21, the second being
xrsim-shot littering the CWD with game-derived captures).

**HEADSET FEEDBACK (user, VDXR, 2026-08-05, same day): THE EYE IS FILLED.** Verbatim
verdicts and the fixes they drove:

- **"Everything through a box" until the lever hit 132 deg - then "pretty good with the
  stereo and no space warp, which is perfect."** The filled-eye verdict is GREEN at
  fovLeverDeg=132 (tanV 1.2634, hfov 99.5 at the 2064x2208 aspect), and no-warp confirms
  claim==render in the instrument that matters. VR stereo + VR camera "working very well";
  the resolution picker "also working very well" (the user runs Quest 3 native 2064x2208).
- **World scale tuned to ~150 UU/m** (not the UE3-canonical 50 the code guessed - the
  same 3x surprise BS1/BS2 produced; their calibrated value was 100).
- **The `eye`-preset Load button did nothing** - real bug: the F10 list's Load only
  handled slotN names and LOGGED for everything else. Fixed by the user's own redesign
  request: **ONE preset, Save + Load, like the other mods** - the slot/named UI is gone
  (named files stay as a desktop verb lane), and the preset now carries the WHOLE session
  shape: vrstereoOn + driveHmd joined the registry (8 keys), and a loaded preset's
  resolution now APPLIES instead of latching (user's call, overriding the s41 flat-half
  hazard design). Verified: one boot with zero commands auto-restores everything - 8/8
  keys, first Present 2064x2208, stereo auto-armed, SR beat exact 77/77/154/77, lever
  enforcing (217k writes 0 faults), claim tanV 1.2634.
- **JUDDER ON HEAD TURNS PERSISTS "even though the frames are good" - the remaining open
  item.** First suspect for next session: the pacing beat - 77-80 eye pairs/s against a
  72 Hz VD refresh is a ~5 Hz interference pattern; pairs synced/capped to the headset
  refresh is the experiment to run. The 30-minute soak + level transition also remains
  unreported.
- **Standing guard for I8 (user, this session): the FOV lever must not break the
  viewmodels (hands/guns)** - BS1/BS2 both bled sessions on fg-FOV counter-modeling. The
  lever writes ONLY the world camera's two FOV fields; the decoder's named second lens is
  the early-warning instrument (watch `bsilens` lens2 in gameplay saves); I8 tests
  whether Infinite's viewmodel lens is even coupled before porting ANY compensation.

**NEXT**: the judder investigation (pacing-beat hypothesis first) + the 30-minute soak
close I6; then I7 (controllers + decoupled aim) opens, with the I8 viewmodel-FOV guard on
record.

### Infinite: current state after session 40 (superseded by session 41 above - kept for the derivation trail; I5 CLOSED as re-scoped - stereo headset-verified on VDXR; world-scale tune, judder verdict and the long soak carried to I6 - branch `si40-inf-stereo`)

**HEADSET VERDICT (user, VDXR, 2026-08-05): "there's stereo 3d rendering and it's working
well" - I5 is CLOSED as re-scoped.** True geometric parallax confirmed in the headset;
nothing broke in their testing (short session by design); the log measured **77-80 eye
pairs/s (155-160 presents/s) at default render scale with every SR gate exact** - above
the 72 fps target. Three footnotes, all carried EXPLICITLY to I6's headset session:
(1) **the "looking through a window" percept is expected and correct** - the honest claim
renders the game's 75 x 47 deg frustum at true angular size inside a ~108 x 110 deg eye;
filling the eye is I6's whole purpose (a real FOV lever pushing tanV past the native
slider + a near-square render; resolution ALONE cannot help - the law is
vertical-referenced, so a square render just narrows the horizontal); (2) the world-scale
slider works live but the FEEL stays unjudgeable through the window - tune at I6; (3) a
slight judder on head motion - consistent with ~80 pairs/s under a 90 Hz headset refresh
(suggest Virtual Desktop at 72 Hz next session; I6's resolution work also lowers the
per-eye cost). The section below records the session-40 flat state that earned the
verdict.

### Infinite: session-40 flat state (the battery that preceded the verdict above)

**All three stereo rungs are live and flat-verified: mono projection, AlternateEye, and
SequentialReentry with per-eye presents and pair pacing.** The adapter now feeds core an
honest FOV claim from the I2 law (`hfov = 2*atan(tanV x aspect)`, tanV default = slider-min
0.4317, `bsifov tanv` lever, vrpreset-persisted) plus `publish_gameplay_view` per detour
call, so core's camera-mode toggle finally flips the quad to a projection layer on this
game. `vrstereo on` is the full one-toggle ladder (enable -> camera mode -> pair pacing ->
SR; `vrstereo mono` stops at mono projection, `vraer` is the AER backend); everything is in
the F10 "VR stereo (I5)" section for the headset. NO 1t machinery exists or is needed -
DR-I5's threaded/ring-buffered verdict held under real doubling.

**The session's derivation: the render-root chain, live** (ENGINE_NOTES s40). The SR
doubling root is the VIEWPORT draw `0x1FDE30` (FViewport::Draw analog: canvas -> client
draw -> present kick, thiscall + 1 arg, ret 4; gameplay caller ret `0x206309` of 4 static
callers). Derived by a caller census at the camera detour (ret `0x26B499` = exactly once
per present), a one-shot backtrace + raw stack scrape, and one-shot live vtable probes
(`bsicam scenedraw`/`vtprobe`: client `[viewport+0x1C]` -> vtable `0xDE6FC8` slot +0x8 ->
stub `0x6F1360` -> client draw `0x26A3E0`). **Recorded negative:** doubling the client draw
(the function that actually samples the camera) yields NO second present - the tag ring
skews +1/tick. The root must contain camera + scene + present; the viewport draw does.

**The flat battery (sim at the attract, all measured):** claim audit `src=readback`,
`tanH=0.767467` exact, **claimRatioH baseline 0.5576 derived fresh** (never BS1's 1.018);
projectionViews=2; mono pair byte-identical (the control) vs SR pair really differing
(mean 0.42 / 1.09 % channels = geometric parallax); AER + SR inter-eye |d| = ipd x scale
exact (3.150 UU at 63 mm/scale 50, doubles at 100); `draws/s=90 2nd/s=90 presents/s=180
camReplays/s=90` at the sim's ceiling with call2 80-215 us; deny gate observed refusing a
foreign caller (f=2, no double); 15-min armed soak - zero faults, zero watchdog, zero
poison, skips frozen, occasional self-healing tag-ring resyncs only at attract
scene/movie transitions; `vrstereo off` returns to the mono quad; clean exits. Runbook:
TESTING.md "I5 battery".

**NEXT SESSION = I6 (lens audit, FOV, resolution, config menu)** - now judged IN STEREO,
which is the entire point of the restructured ladder. The headset gave I6 its worklist:
fill the eye (FOV lever via the named property chain first - the native slider tops out at
52.6 deg vFOV, the eye wants ~110; then the near-square render, which only pays combined
with the lever because the law is vertical-referenced), re-judge world scale and judder
through the filled view (suggest VD at 72 Hz), run the 30-minute soak, and land
`xrEnumerateViewConfigurationViews` + the resolution picker + the config/preset menu per
the ROADMAP boxes. The claim machinery to extend is `bsifov`/`kTanVSliderMin` in
camera.cpp; `reentry status` after any load-heavy session must keep showing poisoned=no.

### Infinite: current state after session 39 (superseded by session 40 above - kept for the derivation trail; I4 CLOSED - flat battery green AND headset-verified on VDXR - branch `si39-inf-head-camera`)

**The 6DoF head camera drive is live and the entire flat harness proved it with numbers, no
headset involved.** The GetPlayerViewPoint detour tail gained `drive_view`: out-param
substitution ONLY (engine memory never written - drive-off is a byte-identical passthrough,
and the engine's own view kept moving under the drive on every heartbeat, which is the BS1
pitch-freeze bug made impossible by construction). Yaw additive, pitch/roll absolute,
position recenter-relative x worldScale (default 50 = UE3 canonical, NOT BS2's 100). Lane
order replay -> simhead -> live; the live lane gates adapter-locally and **never sets camera
mode** - core flips quad->projection on camera mode, and that rung is I5's (ARCHITECTURE
decision log 2026-08-05). All new code is Infinite-local (`inf_math.h`, `recorder.cpp/.h`,
camera/adapter edits + the source listing); zero core, zero shared, zero BS1/BS2 files.

**The flat battery, all measured at the attract (which runs real gameplay scenes):**
passthrough control (lane=off, final==engineRot, d=0); simhead yaw +30 -> final-minus-engine
= exactly 5461 units on three consecutive beats against a MOVING attract camera; pitch 20 ->
3640, roll 10 -> 1820; position triples on all three XR axes -> headOff exact (0.5 m = 25 UU
at scale 50, rotated by game yaw, world-up unrotated); worldscale 100 doubles it; the REAL
OpenXR path end-to-end (`head rot`/`head pos`/`head orbit` -> xrLocateSpace ->
get_head_pose) exact to the unit; vrrec round trip (1077 frames, PLAY marks
number-for-number identical to REC incl. the driven camera, lane=replay with xr=none,
recenter+worldScale restored from the header); rendered-pixels acceptance via WINDOW
img-diff (floor 0.58/1.12 % vs simhead-60 6.12/23.4 % - 10x, heading visibly rotated);
90.0 fps sustained with the drive on; vrpreset round trip; clean WM_CLOSE exit. Full runbook:
TESTING.md "I4 battery".

**One new sim trap found and recorded (VERIFICATION gotcha 17):** a sim `recenter` sent while
the head is yawed makes the next quad capture ~black (1 covered pixel), and even a yaw-0
recenter leaves the captured quad oddly off-centre - the capture's layer transform after a
reference-space change is suspect (healing lane). Use the game window for camera-drive pixel
checks.

**HEADSET VERDICT (user, VDXR, 2026-08-05): I4 is CLOSED.** The corner-lean tracked with
NO drift, head roll tilted the horizon correctly, and the head-driven camera showed on the
I3 big screen exactly as the MonoTracked rung intends. Two verdict footnotes: (1) the
world-scale FEEL could not be judged on the mono screen - the tune is DEFERRED to I5 where
stereo makes it a real judgment (the lever, F10 slider and vrpreset persistence are all in
place at default 50); (2) the user pressed core's VR-section camera toggle and saw nothing -
correct and by design: that toggle is the quad->projection flip, gated on a lens claim the
adapter does not feed core until I5/I6, and only the "VR camera (I4)" section is live this
milestone.

**NEXT SESSION = I5 (stereo)**: MonoTracked (done) -> AlternateEye (core supports it) ->
SequentialReentry; entry gate is the I2 FOV law (tanV 0.4317..0.4933, tanH = tanV x aspect);
DR-I5 says test threaded first, port no 1t machinery until a measured stall demands it.
Carry the world-scale tune into I5's headset checklist.

### Infinite: current state after session 38 (superseded by session 39 above - kept for the derivation trail; I3 DONE as re-scoped: VDXR headset-verified, three sim bugs fixed - branch `si38-inf-headset-bringup`)

**The whole I3 mono-big-screen stack runs on Infinite with ZERO core and ZERO adapter changes** -
the merged BS2 OpenXR runtime brought the session up on the game's own device first try
(backbuffer fmt 28 -> swapchain pair fmt 29, zero-copy; FOCUSED in ~600 ms), sustains 90.0 fps,
survives focus loss (keeps submitting, FOCUSED re-earned), tears down and re-brings-up cleanly
three independent ways (`bsivr off/on` ~250 ms, `hazard waitfail`, and live `bsiexec setres`
resizes BOTH directions with the queued swapchain rebuild + aspect-following quad). Captures
show the world-locked 2.4 m quad with correct stereo parallax and readable game pixels. Details
with numbers: ENGINE_NOTES "LIVE RESULTS (session 38)".

**The battery's real yield was three SIM bugs** (I3 is the first mono-quad capture consumer):
the `now_xr_time` int64 overflow (a ~30.7-min machine-uptime sawtooth that parked the pace
thread's `xrWaitFrame` forever ~25 min into gameplay - diagnosed from a minidump stack, fixed
with split arithmetic + a >1 s wait clamp so no clock anomaly can ever hang the host again),
sRGB-decoded captures (dark scenes crushed ~13x - composite target now `_SRGB`; **pixel-stat
baselines before s38 are not comparable**, BS1 re-baselined: stereo L/R diff 11.53, claimRatioH
1.018 unchanged), and sticky timed `focus lose`. Plus harness: `xrsim-launch`/`launch-game`/
`xrsim-run` take `-Game bsi`; new `bsivr on|off|status` adapter command; new sim traps
catalogued (VERIFICATION gotchas 13-16: floor LOCAL origin -> send `recenter` before quad
captures; persistent `idle on`; step-starve command latch; Infinite's auto-pause on focus loss).

**Shared-tool proofs run**: bs1/bs2 preflights green after the launcher edits; selftest green
after each sim fix; full BS1 lane on the fixed sim (boot -> smoke -> stereo) green with the
geometric baseline identical (1.018).

**HEADSET VERDICT (user, VDXR / Virtual Desktop, `VirtualDesktopXR` 1.0.10): "looks pretty
good, no crashes or freezes/hangs"** - head-tracked big screen live, F10 VR A/B confirmed in
the log (teardown -> re-bring-up on VDXR), alt-tab survived, two boots, clean exits. **I3 is
DONE as re-scoped by the user**: the SteamVR lane is deferred ("no Steam Link; test SteamVR
later, not needed for the first version") and the debt is carried explicitly on the I11 (release)
release checklist.

**Exit breadcrumb resolved to a benign property**: `DLL_PROCESS_DETACH` never logs when an XR
session is LIVE at exit - sim AND real VDXR, both games; sessionless exits log it. Prompt,
fault-free, dump-free closes either way; attribute the mechanism during the release (I11) soak only if
it ever hides a real teardown bug.

**NEXT SESSION = I4** (6DoF head camera + the flat harness: `simhead`, `vrrec`, servo-vs-write
discipline). The injection point is already settled - GetPlayerViewPoint returns a raw copy of
the camera POV (path 2, 100 % census), so I4 injects at the POV/out-params. Build `simhead`
FIRST; the sim's floor-LOCAL-origin trap (VERIFICATION gotcha 13) applies to every quad-pose
capture until the healing lane fixes the sim default.

### Infinite: current state after session 37 (superseded by session 38 above - kept for the derivation trail; branch `si37-inf-merge-and-derisk2`)

**I2 IS CLOSED - all eight de-risks recorded - and `main` (BS2 v0.7.0, 124 commits) is merged
into the Infinite line.** Details per DR in [bioshockinfinite/ROADMAP.md](bioshockinfinite/ROADMAP.md)
and the "LIVE RESULTS (session 37)" section of
[bioshockinfinite/ENGINE_NOTES.md](bioshockinfinite/ENGINE_NOTES.md). The branch is pushed;
fast-forwarding `bioshock-infinite` awaits the user's confirmation.

**The merge (step 0, before any testing):** 6 conflicted files, all resolved per the session-36
policy (BS2 wins behavioural, keep-both on additive). The 336-float-cap removals in
`decode-framedump.ps1` survived; `frame_inspector` mode 3 survived; `game-cmd`/`game-shot`
compose Infinite's bsi support with main's live-instance filter. **All four BS1/BS2 inertness
proofs re-run on the merged tree** (no `framework/command` include in either remaster adapter;
only bioshockinf arms the Present pump; both arm dumps as `full ? 2 : 1`; `git diff main` over
both adapter dirs empty) - cited in the merge commit. Acceptance ran clean: Release build,
decoder `-SelfTest` both scanners + `-ScanLayout` offset-12 regression on a banked BS1 dump,
**BS1 smoked on the simulator** (boot -> gameplay -> `vrstereo on` -> projectionViews=2,
eyeSep 0.063, claimRatio 1.018 - note stereo takes ~40 s to engage under the sim, longer than
stereo.xrs's 60-frame assert), **BS2 smoked on the simulator** (all scans resolve, XR session
FOCUSED, quad submits, `recenter` dispatched by the game-thread poller), Infinite full battery
below. The merge brings BS2's OpenXR runtime (+1271 lines), the xrsim simulated-Quest-3
toolchain, `docs/VERIFICATION.md`, hud_capture/crash/diag upgrades - **most of I3
pre-debugged**, untouched until I3 per the session brief.

**The battery, all measured live on the merged build:**

- **The TRANSFORM question is CLOSED**: the path-aware heartbeat in gameplay says path 2
  (100 % census), source `[cam+0x3B8]`, `returned-minus-source = (0,0,0)` every beat - a RAW
  COPY. **I4 injects at the camera POV / out-params; there is no downstream transform.**
- **DR-I4 CLOSED (negative)**: no engine stereo names in the live pool; `AllowNvidiaStereo3d`
  at index 4154 is the positive control and the only (driver-side) stereo surface.
- **DR-I5 recorded**: threads separate under BOTH `OneFrameThreadLag` positions; the lever is
  accepted, game healthy; substrate evidence says ring-buffered submission (90 M lifetime
  UpdateSubresource, no stalls at 9681 calls/s). The latency half needs I6's instrument, by design.
- **DR-I6 CLOSED**: `bsicall`/`bsiexec` (new, `src/game/bioshockinf/reflect.cpp`) dispatch by
  name via FindFunction(+0x54)+ProcessEvent(+0x7C) with full gates (build, thread identity,
  GNames, vtable-RVA interlock incl. new `kFindFunctionRva 0xD1030`), SEH-isolated.
  **`bsiexec setres` RESIZED THE BACKBUFFER live** - the strongest possible by-effect proof.
  `set FOVAngle` moved nothing, but FOVAngle is known-disconnected; recorded honestly.
- **DR-I7 CLOSED**: HUD fingerprint confirmed in a second scene/resolution - backbuffer-composite
  from 2 call sites, BC3 atlases. The tonemap target index VARIES (T9 then T8), so the eye-image
  rule is positional (srv0 of the last full-screen a=6 into the backbuffer) and needs no
  classifier - that texture is HUD-free by construction.
- **DR-I8 CLOSED (both levers)**: `XEngine.ini ResX/ResY` is a boot-derived copy - the real
  store is **`XUserOptions.ini ResolutionX/ResolutionY`** (honoured: `first Present: backbuffer
  1600x1200`); `setres` also works live. BS1's fault does not exist here.
- **The FOV law is VERTICAL-referenced** (aspect cross-check at 16:9 vs 4:3, slider max: tanV
  pinned at 0.4933, tanH = tanV x aspect to 5 digits). Slider = vFOV 46.67..52.63 deg. I5 derives
  everything from tanV; ini FOVAngle is decorative.

**Incidental but load-bearing:** an UNATTENDED menu/attract hangs the game (pre-existing - the
unmodified session-36 build hung identically, which exonerated the merge; watchdog stack
photographs banked in `%LOCALAPPDATA%\BioshockVR\bsi\s37-*-hang*.log`). Keep a driver at the
menu. Infinite exits CLEANLY via WM_CLOSE (orderly DLL_PROCESS_DETACH, twice) - better than
both remasters. Camera hook: 1.49 M calls this session, 0 foreign-tid dispatches ever.

**Still owed / known gaps:**

1. `frame_inspector` records `rtv0` only (4-RTV frames discarded) - worth fixing before I6;
   unchanged from session 36.
2. The FNameEntry UTF-16 branch remains UNTESTED (0 wide entries in the pool's first 4096).
3. `bsiexec`'s ReturnValue capture is unverified (possible signature drift on bWriteToLog);
   never rely on the returned string - verify by effect, which is the rule anyway.
4. BS1/BS2 flat sim smokes passed, but no headset regression ran this session - the core deltas
   are grep-proven inert and their game code is byte-identical to shipped v0.7.0.

**NEXT SESSION = I3, headset bring-up (mono big screen)** - and the merged BS2 OpenXR runtime
plus the xrsim toolchain make it a very different, much shorter job than BS2's was. Read
`docs/VERIFICATION.md` first.

### Infinite: current state after session 36 (superseded by session 37 above - kept for the derivation trail; branch `si36-inf-derisk`)

**DR-I1, DR-I2 and DR-I3 all PASS. Three of the eight de-risks are closed, and I2 is half done in
one session.** BioShock 2 held the machine for the first half, so the offline derivation came
first; the machine freed up and the whole live battery ran.

**THE HEADLINES, all measured live:**

- **The camera hook fires.** 9681 calls/s peak, ~3600/s idle gameplay, 4.07 M lifetime calls,
  **0 foreign-thread dispatches**. `vrcmd` reports `pump=game`, so the handover works.
- **Game and render threads are SEPARATE** (tid 13120 vs 1992). Free DR-I5 evidence.
- **The motion test passes**: one 360-degree turn swept yaw **-32392..+32640 = 99.2 % of a full
  16-bit range**, falsifying the field ordering and the 65536-units-per-turn assumption in one
  motion. **FRotator is SIGNED** - a naive unsigned read would be wrong for half the circle.
- **DR-I1 confirmed on a live object**: vtable slot `+0x7C` holds `AActor::ProcessEvent`
  (`0x19A150`), not the `UObject` base - the prediction only the base/override split made possible.
  GNames works (`Num=69718`, `[0]=="None"`), the UClass fixpoint passes.
- **THE LENS IS FOUND AND CONFIRMED.** Float 0 of the 80-byte constant buffer, row-major 4x4.
  Of 139 candidate 4x4s only one matched the backbuffer aspect. Promoted from candidate to fact by
  a falsifiable prediction: the FOV slider min-to-max moved **both axes by the same ratio**
  (1.14282 / 1.14269) with the aspect held to 0.002 %, 75.01 -> 82.50 degrees horizontal.

**AND THE INI LIED, which nearly poisoned I5.** Session 34 read `FOVAngle=70` +
`MaxUserFOVOffsetPercent=15` as "the slider spans 70 to 80.5 degrees". The rendered frustum spans
**75.01 to 82.50**, and the measured tangent ratio is **1.1428** against the **1.2094** that
70-to-80.5 predicts. Neither endpoint nor the ratio matches. **I5 must derive the FOV law from the
frustum, not the config** - third instance of "a verified value is not an honoured one".

**What landed:**

- **DR-I1, PASS.** `UObject::ProcessEvent` **`0xCFE70`** (vtable slot **`+0x7C`**, thiscall,
  3 stack args, `ret 0xC`), `AActor::ProcessEvent` **`0x19A150`**, `UObject::FindFunctionChecked`
  **`0xD1090`** (426 callers), `UObject::FindFunction` at vtable slot **`+0x54`**. Derived by
  three routes that agree, and **every census prediction written down before the run held**:
  FindFunctionChecked 426 E8 callers / 0 abs refs, ProcessEvent 1 / 2144. The base/override split
  is settled by structure rather than vote count - the runner-up tail-calls the mode.
- **`tools/pe-xref.ps1`**, the static caller census, is committed. Sessions 34 and earlier ran it
  from throwaway scripts, which is exactly why their numbers could not be reproduced.
- **A correction:** `ConsoleCommand 0x136070`, `AXPawn::SetWeapon 0x4F9ED0` and
  `AXWeapon::AddAmmo 0x5017D0` were recorded as "impl RVA". All three have **0 E8 callers** - they
  are exec thunks. The test loadout should go by name through `ProcessEvent` instead, which now
  exists and needs no further addresses.
- **DR-I2's camera hook is written**: read-only by construction, gated on the live 12-byte prologue
  AND on finding `ret 8` in the body before the detour is created, fail-soft on every refusal. It
  takes over the command poll on first fire.
- **DR-I3's frame map is done offline** from the banked lite dump: the full deferred pass order,
  the scene RTs, the tonemap target, and a free Scaleform half-answer for DR-I7.

**Two findings that changed the plan, both verified rather than assumed:**

1. **`GNames` is empty at adapter-init time.** Our DLL loads from the proxy's `DllMain` during the
   exe's import resolution, before the exe's CRT static initializers. Every GNames reader is
   therefore command-driven and fails soft with "not populated yet" rather than "not found".
2. **DR-I3's experiment had already been run and failed, with a statable scope.** Core's live FOV
   watch *was* sampling on Infinite - the banked dump shows 8 staging copies per frame from 3
   distinct buffers - and across **19,602 presents** it never adopted an offset. The negative is
   real and precisely bounded, and it has exactly three holes, all in our own code: the `>= 320`
   byte tier gate (and Infinite's deferred lighting pass uses the **160-byte** tier), the 1344-byte
   truncation, and VS-only. A plain `dumpframe full` would not have closed them either, because it
   gates on the buffer OBJECT changing while Infinite rewrites one object via `UpdateSubresource`
   (15.3 M lifetime calls). So the instrument was rebuilt before spending a live session on it.

**New instruments, all controls passing:**

- `dumpframe cb` (frame_inspector mode 3): captures every `UpdateSubresource` into a constant
  buffer at its real size, plus per-draw VS/PS constant-buffer identities.
- `decode-framedump.ps1 -ScanMatrix`: recovers `tanH = |c3|/|c0|` from a 4x4, **with the object
  scale cancelling**, which is what makes it work on a per-object constant buffer. Plus
  `-BlockBytes`, `-MinModeShare`, `-DiffAspects`, `-SelfTest`, and the removal of three hardcoded
  336-float limits (one of which silently truncated large buffers and dropped the rest).
- Controls: `-SelfTest` passes; `-ScanLayout` still reproduces BS1's known offset 12; `-ScanMatrix`
  independently finds the same BS1 lens (1.1917/1.2350 vs the ray block's 1.1918/1.2351) by a
  different decode; and **it recovers BS2's lens from a dump where `-ScanLayout` finds nothing** -
  the 2048x2048 square-aspect case that cost BS2 a session.
- Recorded honestly: `-ScanMatrix` produces false positives on degenerate matrices. On the BS1
  control a 0.5/1.0 pair scored 156 blocks and **outvoted the true answer's 83**, so plurality alone
  is not sufficient and the aspect cross-check is load-bearing.

**BS1 and BS2 are provably unaffected** (user directive). Their sources are byte-untouched;
`git diff` over `src/game/bioshock1r` and `bioshock2r` is empty. The three shared changes are inert
for them by construction, and it was checked rather than assumed: neither adapter includes
`core/framework/command.h` (so the pump lease cannot reach them); both compute `full ? 2 : 1` when
arming a frame dump (so mode 3 is unreachable); `pattern_scan`'s UE2 entry point keeps its exact
body. **The additive dump format was tested, not asserted**: the OLD decoder run against a
synthetic mode-3 dump gives a byte-identical answer to the original, while the new one additionally
reads a 640-float upload block the old 336 cap would have truncated.

**NEXT SESSION = I2 part 2**, the remaining de-risks: **DR-I5** (render substrate - and the
thread-split measurement above is already half of it), **DR-I6** (the `set` Exec seam), **DR-I7**
(Scaleform - and the frame map already hands over a large free head start), **DR-I8** (resolution,
which the aspect cross-check below settles as a side effect), and **DR-I4** (native stereo,
timeboxed, evidence already says no).

**Still owed / known gaps, in priority order:**

1. **The TRANSFORM question is still OPEN, but the instrument is FIXED and installed.** The
   heartbeat used to compare against `[this+0x24C]` (path 1's source) while every observed sample
   took path 2 reading `[cam+0x3B8]` - it compared the wrong field, so its "raw copy" verdict was
   worthless. It is now path-aware: `returned-minus-source` binds the comparison to whichever path
   actually ran, names the field it used, and **refuses to make a claim at all** on paths 3 and 4
   (whose source we decline to resolve from inside a detour). **This needs only a launch and one
   heartbeat to read.** It decides where an I4 HMD pose has to be injected, so do it first.
2. **The aspect cross-check** at a second backbuffer size (1600x1200), which also answers DR-I8 for
   free via `first Present: backbuffer WxH`. One relaunch.
3. **The FNameEntry UTF-16 branch is UNTESTED**, reported as such rather than as a pass - the pool
   has no wide entries in its first 4096.
4. `frame_inspector` records **`rtv0` only**, and every world `SetRT` on this game binds 4 RTVs, so
   three quarters of the deferred frame map is discarded. Worth fixing before I6.
5. A **4x4-shaped live lens decoder** behind `hud::fov_watch`. Core's `decode_ray_block` is
   Vengeance-shaped and cannot consume this game's lens, which is why the adapter publishes no
   `set_ray_block_offset` even though the offset is now known. I5 work.

### Infinite: current state after session 35 (superseded - kept for the derivation trail)

**I1 IS CLOSED - our code runs inside `BioShockInfinite.exe`.** Branch `bioshock-infinite`. The
adapter exists (`src/game/bioshockinf/`), the mod loads, logs, draws its overlay and takes commands,
and **not one engine hook is installed**. I0 stays closed; its recon is in `ENGINE_NOTES.md`.

What is now confirmed **live, from inside the process** (the whole session-34 table was outside-in
until today):

- **The `xinput1_3.dll` proxy works verbatim** - no proxy change was needed, and the Steam overlay
  does not interfere with injection (the proxy loads the mod from its own `DllMain`, long before
  anything calls `XInputGetState`; whether the overlay eats the *input* thunk is still an I7
  question).
- **The build fingerprint matches on all four fields** and **ImageBase really is `0x00400000`**.
- **D3D11, from the inside:** backbuffer **2560x1440 `R8G8B8A8_UNORM`, windowed**, feature level
  **11_0**, RTX 4060; frame inspector attached to the game's own context on **15/15** slots and
  wrote a real dump (482 events / 68 resources).
- **d3d11/dxgi being absent from Infinite's import table changes nothing**, and the reason is
  structural rather than lucky: `bioshockvr.dll` links `d3d11` itself, so our import table loads it
  before any of our code runs. Hooks in at T+0.4 s, first Present at T+8.5 s.
- **A free half-answer for DR-I8:** the live `XEngine.ini` says 2560x1440 and the backbuffer at
  first Present *is* 2560x1440, so the config resolution is honoured by the renderer - which is more
  than BS2 could say. It does **not** yet prove a *write* lands; that still needs a write, a
  relaunch, and the backbuffer as acceptance.
- **`CSERHelper.dll` displaces our exception filter here too**, exactly as on the remasters. Core's
  periodic re-arm caught it at the first Present, so it is load-bearing on this game as well.
- The camera seam was **probed read-only, never hooked**: `0x1E10C0` holds an aligned-stack MSVC
  prologue (consistent with the 4x4 SSE transform derived offline) and the exec thunk at `0x129280`
  is **frameless**, which independently confirms why the `CC CC CC 55 8B EC` prologue heuristic
  cannot find it. Hooking is I2.

**The command seam is core now, and Present-driven.** `core/framework/command` owns the poller and
the shared vocabulary (`mem*`, `fsweep`, `dumpframe`, `vrinput`, `vrpace`, `vrmirror`, `vrcine`,
`vroverlay`, `vrhud`, plus a new `vrcmd` status). Adapters get first refusal via
`IGameAdapter::handleCommand`. Three things worth carrying forward:

- **BS1 and BS2 are byte-untouched** (user directive, this session): the Present pump is opt-in, so
  they keep their own pollers and their own copies of the vocabulary. **Folding them in is a
  deferred consolidation task** - see "Deferred" below.
- While the Present pump owns the poller, **commands run on the render thread**. A long `memscan`
  stalls presents, not the game thread. I2's camera hook takes over the poll one-way and
  permanently.
- **A pre-existing `command.txt` is skipped at startup** rather than executed - the trap that bit
  BS1 three times. Found and fixed a subtlety live: prime on the first POLL, not the first sighting
  of a file, or a game that starts with no command file swallows the first real command (the first
  build did exactly that).

The session-34 recon that all of this rests on, unchanged:

Closed at the end of the session once BS2 freed the machine: **the renderer is D3D11, confirmed
live** (`nvwgf2um.dll`, the DX10/11 UMD, is loaded and the DX9 UMD is not - `d3d9.dll` alone proves
nothing). Two further live facts that change I1/I7: **`XINPUT1_3.dll` is loaded**, so the injection
vector is real at runtime, and **`GameOverlayRenderer.dll` is loaded**, so expect BS1's
Steam-overlay thunk problem and plan for the IAT-hijack lane (slot RVA `0xCD4814`) rather than
trusting the proxy seam alone. Reading a 32-bit process's modules needs **32-bit** PowerShell; a
64-bit host sees only the WOW64 shim and reports 6 modules instead of 123.

The harness was also verified against the live process: `game-shot -Game bsi` captures **real D3D
content** (not a black frame, which was not guaranteed), `game-cmd -Game bsi` writes a BOM-free
`command.txt`, and the conflict guard was exercised in **both** directions - refused with BS2 up,
allowed with it down.

Everything else derived so far is still offline and **unconfirmed live**. Confidence is stated per
row in `ENGINE_NOTES.md`; treat every RVA as a hypothesis until a hook fires on it.

**Next session (36) = I2, the de-risk battery** (DR-I1 through DR-I8 in the roadmap). The order that
buys the most:

1. **DR-I2, the camera seam.** Hook `GetPlayerViewPoint`'s IMPLEMENTATION (RVA `0x1E10C0`), never
   the thunk - 2 stack args, `ret 8`, and the arg count must equal `ret imm / 4` or the RTC dialog
   that writes no dump is the result. Confirm it fires in gameplay *and* at the menu, and measure
   the dispatch rate. **The moment it is live, call `bvr::command::poll_from_game_thread` from it**
   - that is the one-way handover that moves commands off the render thread.
2. **DR-I1, UE3 reflection.** `GNames` / `ProcessEvent` / `FindFunctionChecked`, plus an
   ASCII/8-byte-stride variant of `pattern_scan::find_native_function` for this game's 2647-entry
   native table. Run the static caller census BEFORE hooking anything.
3. **DR-I3, the frame map.** Infinite is deferred-rendered, so the BS1/BS2 forward fingerprints do
   not apply and the cb0 ray-block offset must be re-derived. `dumpframe` already works and one
   dump is banked in the `bsi` data dir.

**Blocked on the user / the headset:**

- **`Bioshock2HD.exe` must not be running.** Enforced for `-Game bsi` by
  `tools/lib/assert-no-conflict.ps1`; building and installing are deliberately unguarded. BS2 was
  running for the whole of session 34, which is why nothing live was attempted beyond the six-key
  check the user ran themselves. In session 35 the machine was free and the check was run before
  every launch.
- The user's save (`TWN`, Columbia town) has **no weapons or combat** yet, and that is
  **deliberate** (user directive, 2026-07-31). They will produce the combat/weapons save at **I9**,
  not earlier, because they want the viewmodel and scale work verified **from the start of the game
  as well as from a loaded-out save**. The opening hours have no weapon, then the Sky-Hook alone,
  then one gun - calibration that only holds with a full arsenal is not calibration. Do not treat
  the missing loadout as a blocker to be worked around; it is a test condition.

**Deferred, deliberately:** UELib/UE Explorer decompile workspace. It is most useful aimed at a
specific script question (the fire path for I8), not swept speculatively. `GObjObjects`, for the
reasons in the session log.

**Deferred consolidation task (session 35, user directive): fold BS1 and BS2 into the core command
seam.** Both adapters still carry their own `poll_command_file` and their own copy of the ~70 lines
of core-owned vocabulary that now live in `core/framework/command`. The migration is mechanical -
delete those branches, override `handleCommand`, and call `bvr::command::poll_from_game_thread`
where `poll_command_file` was called - but it was **deliberately not done this session** because a
parallel BS2 session was live in `bioshock2r/camera.cpp` and neither shipped game could be
smoke-tested from this branch. Do it in a healing/refactor pass with both games launchable. Two
details to carry: core's `vroverlay` uses BS2's argument reading (an explicit `off` is off, anything
else is on - BS1's bare `vroverlay` currently means off), and core primes the command file at
startup where BS1/BS2 still execute a stale one.

**Two things to carry into I1 that were bought expensively elsewhere:** hook implementations and
never thunks (offline census proves the thunks are dead here), and read the `FNameEntry` encoding
flag rather than assuming UTF-16 like BS1.

## Current state (2026-08-05, session 42 - THE PRESENTATION LANE, all reachable rungs flat-green - branch `claude/bioshock2-presentation-vr-2a7b3a`, merged to `bioshock-2`)

### The headline: the HUD rides a readable head-locked panel, and it took a core discovery

BS2's frame is a **backbuffer-composite pipeline**: gameswf draws land straight on a
RENDER_TARGET-only backbuffer and the tonemap is an INDEXED quad - no BS1 classifier
fingerprint ever fired (hudDraws=0 under armed stereo). One flag-gated core mode
(`set_backbuffer_composite`, adapter opt-in, default off = BS1 bit-identical) and the
whole presentation stack came alive: **hudDraws=redirects=12352, leaks=0, stranded=0
per reason**, the HUD panel submits as the 12th compositor layer (space=view) at the
preset pose, the flat window keeps its HUD via the composite, and the health/EVE bars
keep their colour (BS2's fills are textured - BS1's bar-fill collision does not
manifest). `vrhud force on|off` + full counters ported; hudQuadDistM/WidthM/UpM
persist (a hand-edited 1.55 survived a relaunch onto the live quad).

### What else landed (all flat-verified where the sim can reach)

1. **Screens route GENERICALLY** (user decision): loading AND the pause menu measured
   SCREEN-ONLY (world pass absent - unlike BS1's pause) -> head-locked quad; the
   title/menu attract stays a strict projection. Unmeasured kinds (vending, gene
   bank, hacking, map, FMV) ride the same generic route, and `vrcine dumparm`
   (one-shot frame dump on a classifier rising edge; **bars edge auto-armed at
   init**) harvests fingerprints during normal play.
2. **Cinematic gates fixed + consumed**: the camera's cine gate keyed on
   `letterbox()`, which is DEAD with bars hidden - now `cinematic_hold()`; `vrcine
   drive off|authored|authored+look` is consumed by camera/aim/hands/wskel
   (identity folds outside a cine - a full session of normal driving proves it);
   authored+look ported (head DELTAS only, residual 0, reference dropped on both
   edges); `bones::release()` gained the missing s29 memory-validity leg;
   `wskel_release` returns the scaled weapon to authored size in suspended shots.
3. **Pad-A (menukey)**: A -> scancode Enter while a menu context holds
   (calcview-silent / menu-shape vtable / screen-only, + foreground + stuck-key
   cap), default ON. **The title-continue accepts NATIVE pad A** - a synthetic A
   continued into the save. Gameplay negative proven (zero injects).
4. **The game crosshair is HIDDEN by default** (user ask, same session):
   `ShockPlayer.DisableReticle()` called through the engine's OWN
   FindFunctionChecked + ProcessEvent - the first PE-by-name CALL lane, the
   precedent for future BS2 engine-state writes. `vrxhair on|off` + F10 checkbox +
   `crosshairVisible` preset key; A/B screenshot-proven; fail-soft (one fault
   latches the lane off).
5. **Flicker diagnosis armed at boot**: per-site catch counters (PE pass-1/2,
   flush pass-1/2, hands vs wskel), write->catch latency maxima (the survivor
   discriminator), the drive-adopt cadence BASELINE, correlates, and a `[flick]
   min=N ...` line every minute. Proven flowing 4 minutes; ambient baseline: pe1
   ~1900/min hands + ~950/min wskel, dmax 16 ms, everything else 0. Any 12+ min
   play log now answers the ~10-min onset question.

### Two structural discoveries future sessions must not re-learn

- **The BS2 pause menu starves the entire PE-tail service lane**: every dispatch
  lands inside the hooked draw (3 CalcView/s, 205 ms draws), so seam commands
  never poll and the input pump - and therefore the PAD - is dead there
  (pre-existing, not a s42 regression; keyboard drives it). An unfocused-paused
  BS2 writes NOTHING and false-positives log-age wedge checks; `game-key Space`
  wakes a healthy game to ~370 CalcView/s instantly.
- **The title screen preloads the newest save under itself** - the camera
  heartbeat shows save coordinates while "PRESS A" is up; continue is instant.

### Regression guards: all intact

sr eyes 6.30 UU exact; wait2/s=0, guardskips 0; drive 90/s; write-locs live on both
hands; teardown clean with ZERO dumps across all four closes (the known host
exit-path fault absorbed each time). Core diff this session: the backbuffer-composite
flag + dumparm + `last_composed_buttons` - all additive, default-off/read-only,
no BS1 path changes behaviour.

### USER CHECKLIST (in-headset, the session-42 acceptance)

Load the save, headset on. Everything arms itself (HUD panel included).

1. **HUD panel**: health/EVE/ammo/plasmid + subtitles on a floating panel in front
   of you. Tune distance / width / height on the F10 sliders (VR section), press
   SAVE - it comes back after a relaunch. Bars must read FULL (coloured).
2. **Crosshair**: the game's own reticle is GONE by default. If you want it back:
   F10 "Game crosshair" checkbox (or `vrxhair on`).
3. **Screens during play**: pause menu and anything you reach (vending, gene bank,
   hacking, map) should land on a readable screen. If one misroutes, just note
   WHICH - the auto-armed dump harvests the evidence silently.
4. **Cutscenes** (any you reach): default = authored camera plays with stereo
   intact and your hands/weapon return at the end. F10 "During cutscenes" combo:
   try `authored+look` for head-look-on-rails and say which you prefer. If the
   drill/gun looks wrong DURING a shot, or a floating screen appears, say so.
5. **Pad A in menus**: A should activate at the title and on fullscreen screens.
   The PAUSE menu pad is dead (structural - use keyboard there for now); report
   whether the MAIN menu list activates on A.
6. **Flicker**: play 12+ minutes normally; the log records `[flick]` every minute.
   Note roughly WHEN you first see the flicker (minute-ish) - we correlate after.
7. Known: plasmid-hand tuning still global; no projectile-plasmid seam yet.

## Next steps (session 43)

1. **Read the session-42 headset verdicts** + the auto-harvested dumps: derive
   `patterns::kCineBarVerts` from the bars-edge dump (C10 - the one open M10.2
   code box), classify any misrouted screen kind from its dump.
2. **Flicker verdict from the [flick] readout**: pick the next fix from the
   playbook (ENGINE_NOTES s42 #7) - late-window catches vs cadence steps vs
   "not these banks at all".
3. **Main-menu pad-A verdict** decides whether menukey needs a 4th gate leg.
4. Plasmid item names (ContentBaked/pc string tables) -> ability-seam live check
   -> per-PLASMID left-hand profiles.
5. The pause-menu service-lane starvation + the alt-tab pacing wedge - one
   careful CORE-adjacent lane, together (same frame-loop neighbourhood).
6. Standing: BS1 regression testing deferred to the END of BS2 development. Core
   diff to re-check that day: laser/dot slots (s40), backbuffer-composite flag +
   dumparm + last_composed_buttons (s42) - all additive/default-off.

---

## Previous state (2026-08-04, session 41 - THE HOLDABLE LANE + THE ANIMATION-PRESERVING DRIVE, all flat-green - branch `claude/bioshock2-holdable-polish-7cbec4`, merged to `bioshock-2`)

### The headline: every round-2 defect has a shipped, flat-verified fix

Session 40's round-2 list is fully addressed in code and measured across two sim boots
at the user's save (vrstereo on throughout). What remains is the in-headset acceptance.

### 1. The animation-preserving drive (the flicker AND the missing animations, one root)

The rigid drive REPLACED driven bones every frame; engine restamps that survived were
the left-eye flicker, and bones where our writes always won had no animation at all.
Now the drive ADOPTS the engine's freshly-evaluated pose per bone (32-byte trans+quat
only, and ONLY when the bank stopped being our own last write - the feedback filter)
and composes the controller frame on top: `q = qtc * animQ`. Restamps became input.
The design review caught a real bug before it shipped: the engine never restamps
SCALE, so adopting all 48 bytes would compound `g_scale` geometrically - the scale
channel is pinned to the captured reference, structurally. The per-PE repaint is now
absorb-then-recompose (pass 1 only; pass 2 verbatim - both eyes render one frame).
`vrhands anim on|off` (default ON; off = the rigid session-40 drive, also the idle-sway
escape hatch), `animtrans 0..1` re-adds authored wrist travel (default 0, glued).

Flat: adoption live at ~7 absorbs/drive/hand; drill-region motion during a trigger
pulse that no rest pair shows; scale-flick captures decay to ambient with NO
pose-alternation outliers; write-loc 100 UU/m EXACT; per-hand decoupling exact;
anim-off composition bitwise-stable. INSTRUMENT CORRECTION banked: `vrbones axes`
'cur' RACES the restamp war (79 deg between same-pose samples) - the new race-free
`written q / anim q` line is the acceptance read.

### 2. The holdable lane - one derivation, two user asks (all constants fresh)

- **UObject identity**: name FName +0x28, UClass +0x30, UClass vtable RVA
  **0x11E71F8** (stable across PlayerHands/PlayerMachineGun/PlayerGrenadeLauncher);
  `patterns::object_class_name` with the full validation chain.
- **Hands.CurrentHoldable = hands+0x4B4**, derived TWICE (seam-anchored find: the
  fired weapon at exactly one slot, two weapons; switch-diff: the same slot swapping
  class objects). BS1's 0x45C did not transfer - the never-copy rule held.
- **The weapon's OWN SkeletonInstance = holdable+0x430** (vtable + owner backpointer,
  one hit; rivet gun 19 bones, grenade launcher 13).
- **Uniform weapon scale** (`vrhands wscale`, F10 slider, per-weapon profile field):
  drives the weapon's own pose bank - scale channels AND translations about the
  component origin. THE CANISTER REPRO INVERTED: at 0.5 the whole rivet gun halves,
  furnace/canister proportional (session 40: ballooned ~4x); at 2.0 uniform growth;
  1.0 = authored restored + fully hands-off. Weapon animations keep playing while
  scaled (adoption on the weapon bank too). `scaleweapon` (pivot-63) kept as
  fallback, default OFF.
- **Per-weapon profiles** (BS1 session-21 shape, all four seeding rules): RIGHT hand
  (aim trim/pos + model trim/offset/scale) + wScale per weapon, keyed by class name,
  auto-swap on the rig's live holdable, weapons.ini persistence. USER DECISION: the
  left/plasmid hand stays GLOBAL (per-plasmid keys are a future session). Flat:
  pre-fire keying ('PlayerRivetGun' applied the moment the rig resolved), edits stash
  on switch and restore on switch-back with NO leak into new profiles (they seed from
  the captured preset baseline), 2 profiles / 26 values round-tripped a relaunch.

### 3. Round-2 polish, all landed

- **Arms-hide web FIXED**: hidden arm bones now collapse ONTO the driven wrist
  (position = wrist target, zero scale) - the skinning blend degenerates to a point.
- **F10**: always-visible PRESET section at the TOP - "applies / saves ALL settings
  and values" - APPLY + SAVE in one obvious place; buried duplicates removed; new
  anim checkbox/slider, WEAPON scale slider, live "weapon profile: <key>" readout.
- **vrinput default-ON** (BS2-local call in camera::install): the pad drives at boot
  with no command - proven this session pre-command at 90/s. Core default untouched.
- **Per-hand aimRayMaxDevDegL/R** in the sim compositor (nearest-ray assignment):
  dual-beam acceptance reads **L 0.0000 / R 0.0000** at the save (legacy field kept
  for old baselines; a tuned aim trim reads as exactly the trim).
- **Preset**: 43 values round-trip (was 22) - animMode/animTrans/wScale/scaleWeapon
  added; boot order load -> baseline capture -> weapons.ini proven in-log.

### Regression guards: all intact (session-41 numbers)

11 compositor layers dual-beam; sr eyes 6.30 UU exact; wait2/s=0, guardskips 0; input
drive 58-92 UpdateInput/s; teardown 478/463 ms with ZERO dumps across both closes;
`vrhands offset` verb still whole-token. BS1 untouched (BS2-local code only; the one
sim-tool change is additive JSON fields).

### USER CHECKLIST (in-headset, the session-41 acceptance)

Load the save, headset on - everything arms itself (vrinput is ON at boot now; APPLY
PRESET is at the TOP of F10 with SAVE next to it).

1. **Weapon animations**: drill melee-hit should PLAY on the driven hand now; reload
   and grip animations too. Idle breathing/sway is BACK by design - if it bothers
   you, F10 "engine animations on driven hands" OFF is the old rigid feel; say which
   you prefer.
2. **The left-eye flicker**: flick the model scale slider (the old trigger) - the
   flicker should be gone. If any survives, say when it happens.
3. **Arms hide**: the stretchy web from the wrist should be gone (arms radio "hide").
   Follow stays the default.
4. **WEAPON scale (uniform)**: the new "WEAPON scale" slider shrinks the whole gun -
   ammo canister included, nothing should balloon. Tune per weapon; it saves into
   that weapon's profile.
5. **Per-weapon tuning**: aim/model sliders (R hand) now save PER WEAPON and swap
   automatically when you switch weapons - the panel shows "weapon profile:
   PlayerRivetGun" etc. Tune a couple of weapons, switch back and forth, press SAVE.
6. Known: plasmid-hand tuning is still global; pad A still does not activate menu
   items (Enter does); no projectile plasmid yet.

### ROUNDS 2-3 (same day): the user tested TWICE - near-total acceptance

Round-2 in-headset verdicts: **animations, sway, arms-hide/follow, uniform weapon
scale, per-weapon profiles, F10 - ALL ACCEPTED; nothing regressed.** The user
calibrated all 8 weapons + both hands and saved. Follow-ups shipped same day:

- **The calibration is BAKED AS CODE DEFAULTS** (their ask; BS1 s21 precedent) -
  atomics + `seed_default_profiles()` carry the tuned values, virgin-boot-proven
  (inis moved aside: 8 profiles seeded, rivet gun applied the exact numbers). Round 3
  re-baked after the offset pass (per-weapon wOff, handScaleR 0.760, shotgun 0.79).
- **NEW: per-weapon WEAPON OFFSET** (attach-pivot base; gun moves, fingers/wrist/aim
  stay) - round-3 verdict: "perfect". F10 sliders + `vrhands woffset` + profile
  fields + preset keys.
- **NEW: laser/dot per-hand F10 toggles** + preset keys (beam and dot independent).
- **THE FLICKER SURVIVES both repaint rungs** (PE-lane absorb + flush-point),
  reduced; new observation: possibly TIME-correlated (~10 min uptime), trigger
  unclear. This is now a DIAGNOSIS item, not a repaint-site hunt (ENGINE_NOTES s41
  r3 has the instrumentation plan).

### Next steps (as written end of session 41 - executed by session 42)

1. **THE HUD LANE** (the user's chosen focus): make the HUD work properly in VR -
   health/EVE/ammo readability and placement. Existing substrate: core `vrhud`
   (quad-layer HUD capture, `vrhud on|off|status`), the gameswf draw classifier,
   and the post-v1 backlog's wrist-anchored idea. Decide head-locked quad vs
   world/wrist anchoring, size/distance tuning in F10 (in-headset controls rule).
2. **Flicker DIAGNOSIS** (repro: ~10 min of play): per-site sentinel-catch counters
   per minute, log the pass phase of surviving restamps, correlate with uptime
   (GC/streaming/LOD cadence suspects). Only then pick the next fix.
3. **Plasmid item names** (ContentBaked/pc string tables) -> ability-seam live check
   -> per-PLASMID left-hand profiles.
4. Pad-A menu activation; the alt-tab pacing wedge (CORE-shared, own careful lane).
5. Standing: BS1 regression testing deferred to the END of BS2 development. Core
   diff to re-check that day: the additive laser/dot slot pair (s40) only.

---

## Previous state (2026-08-04, session 40 - BS2 PLAYS ON THE CONTROLLER; the hands are split and the ~90 deg misalignment is FIXED - branch `claude/bs2-controller-input-decoupling-d7cc14`)

### The headline: the pad drives BS2, both hands are their own, and the model sits where it should

All three of the user's top verdicts from the first look are addressed and measured flat
across three simulator boots at the user's save. **Every M10.1 code box is now ticked**; what
remains is the in-headset acceptance (the user's call) and one blocked check.

### 1. Controller-driven controls - the top ask, done

The premise had to be proven before anything was written: does BS2's binary still contain a
per-frame pad poller that nothing calls? **Yes.** `UWindowsViewport::UpdateInput` is at
viewport vtable slot 73 with ZERO callers, and the two boot GetState calls were the whole
story. So BS1's shape ports: pump UpdateInput once per present, flip the engine's own
`SetUseController` (client slot 73 - a coincidence, banked as two separate constants), hijack
the game's IAT slot. All RVAs derived fresh (GEngine ptr 0x1A638F0, IAT 0x1C0DBFC).

Flat-proven at the save: the drive runs at 63-92/s, `iat 2589` calls (the engine polling every
frame), **a stick walks the player ~400 UU**, **a synthetic trigger fires through the weapon
seam** (wep 0->1, subs 1), **the dpad navigates the main menu**, and the engine's own UI
switched to controller prompts (dpad glyph in the ammo tutorial, Y glyph on the 2K panel).
Session-39's open question about XInputGetCapabilities is answered: **the game never calls
it** - the sole cause of "no pad" was that nothing called UpdateInput. No core change needed,
which is fortunate: the candidate core fix would NOT have been inert for BS1.

### 2. THE ~90 DEG MISALIGNMENT: the composition was discarding the authored frame

The `vrbones axes` instrument - the mesh-orientation read `aimRayMaxDevDeg` never was - found
it in one reading. The rigid map was `delta = qtc * conj(refQ_anchor)`, which makes the
anchor's rotation become the controller's outright and throws the mesh's authored orientation
away. On this rig that orientation is **~81.6 deg** off the view frame. That discarded
rotation IS what the user saw.

Now it is `delta = qtc`: bones keep their authored rotations and the cluster turns by the
controller's rotation relative to the AHands actor (which carries the view rotation).
Measured: **mesh-vs-authored angle 0.21 deg at rest** (was ~81.6), and two 30 deg controller
steps rotate the mesh by 38.85 then 38.88 deg - agreeing to 0.03 deg. No baked constant is
needed at all, so nothing has to be re-derived when a weapon or animation changes.

### 3. Left/right decoupling, on real bone names

`vrbones names` auto-detected the map at **SharedSkeletonData+0xB4**, naming 64/64 bones.
Left = wrist 7 (`BD_HAND_BONE_L00`) + fingers 8..28 + pivot 62; right = wrist 36 + fingers
37..57 + pivot **63** (`RG_RightHandPivotTarget_BONE` - the weapon attach, proven by driving
that bone alone and watching the gun move). Each cluster tracks its own controller:
**35.0 UU on the moved hand, 0.0 on the other**, and 120.0 UU separation for 1.2 m apart -
100 UU/m exact.

### 4. Scale, bullet origin, dual lasers

- **`vrhands scale [l|r] <f>`**, decoupled from worldscale as the user required. It scales
  about the anchor: the anchor write-loc moved **0.00 UU** going 1.0 -> 2.0 while the model
  visibly changed.
- **Bullets leave the hand**: `aim origin (hand 1): ... displacement 61.9 UU`, same family as
  BS1's 40-47 UU, behind a 200 UU refusal clamp BS1 never had.
- **Both hands render a beam AND a dot** (user's call - BS2 is natively dual-wield): 11 of the
  16 compositor layers, each beam terminating at its own dot so there is one bright point per
  hand (the first look's "two dots"). The core change is strictly additive - new
  `set_laser_slot`/`set_aim_dot_slot` entry points no BS1 path calls, a SHARED dot budget so
  the layer arrays never grow.
- **F10 "HANDS + AIM (per hand)"** panel: tuning-hand radio, trim/offset/scale sliders, save
  button. In-headset controls, not commands - the standing rule.
- **14 new vrpreset keys**: a fresh boot loads **22 values** (was 8).

### What did NOT land, and why

- **Ability seam live check** - blocked on a projectile plasmid, not on the seam. Telekinesis
  provably does not traverse GetPerfectFireStart (two casts at a grabbable object, abi stayed
  0). `<X>BasicPlasmid` exists ONLY for Telekinesis in the exe; the other item class names
  live in the content packages and still have to be found.
- **Per-weapon aim presets** - deliberately deferred and split out of the sliders box. There
  is no tuned value source until the user calibrates a weapon in the headset, and seeding
  before one exists is exactly session-21's bug.
- **Pad menu ACTIVATION** - dpad navigation works, but the pad's A button does not trigger the
  highlighted item (keyboard Enter does). Worth chasing if pad-only menus matter.

### Two things a future session should not re-learn

- **`aimRayMaxDevDeg` assumes ONE laser.** With both beams live it reads 47-75 deg and varies,
  which looks exactly like an aim/model regression and is not one: single-beam at the same
  pose reads **0.0000**, the session-39 baseline. Run that acceptance single-beam until the
  metric grows a per-hand version. (VERIFICATION 2.8 records this.)
- **A latent bug, now fixed**: `vrhands offset ...` was swallowed by the `strncmp(args,"off",3)`
  verb check, so every offset command silently DISABLED the hands - which is why the preset
  kept saving zeros. Verb matching is whole-token now. Watch for prefix-matched command verbs
  where one verb is a prefix of another.

### Regression guards: all intact

Teardown across all three boots: **486 / 523 / 211 ms, zero new dumps** (session-38 baseline).
Session-37 baseline observed live (sr eyes 6.30 UU exact, guardskips 0, letterbox self-heal).
Session-39 coupling baseline re-measured at **0.0000** single-beam. Core diff is one additive
laser/dot slot pair that no BS1 path reaches.

### USER CHECKLIST (in-headset, the M10.1 acceptance)

Everything below is pre-armed; load the save, `vrstereo on`, headset on.

1. **Controller now plays the game** - sticks move and look, right trigger fires, left trigger
   casts, dpad works. Say if anything is mapped wrong or feels doubled.
2. **Model alignment** - the gun should sit in your hand the way the game drew it and turn
   with the controller. The ~90 deg offset should be GONE. Fine-tune per hand on the F10
   "HANDS + AIM (per hand)" panel (tuning-hand radio, then trim/offset sliders).
3. **Scale** - F10 "model SCALE" slider, per hand. It does not touch world scale.
4. **Both hands** - the left (plasmid) hand now rides the LEFT controller and has its own
   laser and dot. Check they feel independent.
5. **Bullets leave the hand**, not the head - most visible at contact range.
6. Press "Save these settings" on the F10 panel when it feels right; it survives a relaunch.
7. Known gaps: no shooting plasmid yet (item names not found), pad A does not activate menu
   items (use Enter/mouse in menus).

### ROUND 2 (same day): the user tested TWICE, and the second pass reshapes session 41

The first in-headset pass PASSED the core acceptance (decoupled hands/aim/lasers,
controller stack, thumbrest, 90-deg fix, origin). A same-day round-2 build addressed its
findings (per-hand aim sliders, arms follow/hide/game, weapon-scale toggle, PE-lane
repaint, full preset + one-button APPLY incl. controller arming), and the user tested
AGAIN. Round-2 verdicts (full analysis in ENGINE_NOTES "Round-2 in-headset verdicts"):

- **Flicker survives, and scale changes TRIGGER it** (persists after reverting the value).
- **Some weapon animations never play** (drill melee-hit; idles fine) - the rigid drive
  erases engine animation on driven bones. Same root as the flicker, opposite symptom.
  The session-41 fix is ONE mechanism: retarget engine animation deltas onto the driven
  frame instead of overwriting.
- **Arms hide stretches a web** from the wrist to a zero-scaled bone at its authored spot -
  collapse hidden bones ONTO the driven wrist.
- **Aim tuning must become per WEAPON per hand** (BS1 weapons.ini shape) and **weapon scale
  must be uniform-down** - both need the HOLDABLE lane (resolve the held weapon; its own
  SkeletonInstance is where uniform scale lives - the ammo-canister inverse-scale is
  PROVEN attach-path math, unreachable from the AHands bank).
- **F10 layout**: APPLY/SAVE buttons to a top always-visible section, labeled clearly.

Experiment verdicts banked on the way (ENGINE_NOTES): one-shot scale POKES do not render
on this rig state (drive-path writes do - attribution must go through single-bone
clusters); the ammo canister inversely rides pivot 63's scale; the alt-tab pacing wedge
reproduced twice with a fresh signature (SUBMISSION IDLE, frame not begun; restart-only
recovery).

### Next steps (as written end of session 40 - executed by session 41)

1. **The holdable lane** (unblocks TWO user asks): resolve the currently-held weapon
   object off the rig, then (a) its own SkeletonInstance -> uniform weapon scale-down,
   (b) per-weapon per-hand aim/model profiles with auto-swap (BS1 weapons.ini shape,
   session-21 seeding rules a-d).
2. **Animation-preserving drive**: compose the engine's per-frame animation delta
   (current engine pose vs captured reference) on top of the controller frame - fixes
   the missing melee animations AND the scale-triggered left-eye flicker at one root.
   Retire the PE repaint if the retarget makes it redundant.
3. **Arms-hide collapse-to-wrist**; **F10 top APPLY/SAVE section** ("saves ALL settings");
   vrinput default-ON once the user confirms the arm behavior.
4. **Plasmid item names** (ContentBaked packages - `<X>BasicPlasmid` is Telekinesis-only
   in the exe) -> ability-seam live check + "something that shoots".
5. Pad-A menu activation; per-hand aimRayMaxDevDeg; the alt-tab pacing wedge (CORE code -
   BS1-shared, needs its own careful session; repro is banked).
6. Standing: BS1 regression testing deferred to the END of BS2 development. Core diff to
   re-check that day: the additive `set_laser_slot`/`set_aim_dot_slot` pair only.

---

## Previous state (2026-08-03, session 39 - BS2 MOTION CONTROLS: aim decoupled, laser + dot live, the rig rides the controller, ALL FLAT-GREEN - branch `claude/bioshock2-motion-aiming-c7daed`)

### The headline: BS2 aims with the controller, and everything is in sync

The session-39 brief's Priority 1 is code-complete and flat-verified at the user's save,
under `vrstereo on`, across four unattended boots. The chain: XR hand pose -> b2r
frame context (the same transform the camera drive used) -> game-space ray -> the
GetPerfectFireStart impl seam, with the laser re-deriving render-side from the same
pose/trims and the aim dot published from the final fire point (round-trip error
0.0000 UU). The weapon/hand rig follows the right controller through a rigid 64-bone
drive with NO render-lock domain - session 21's lesson honored from day one.

### What shipped (five commits on the branch)

1. **The dispatch probe** (`vraim probe`): GNames derived fresh (RVA 0x1A614D0 +
   FNameEntry layout), Lane-A FName index globals for the fire-chain names, a
   FindFunctionChecked fire-watch + a full ProcessEvent name census with runtime
   self-derived UFunction name offset (+0x28). VERDICT: GetPerfectFireStart is
   native-to-native (0 PE hits over 6 real fires); InitiateDamage IS PE-visible 1:1
   with fires (banked as timing anchor); BeginFiring resolves via FFC but never
   crosses the outer PE.
2. **The impl seam**: weapon impl 0x89DCB0 (APlayerWeapon vtbl slot 221/+0x374 - SAME
   body on AWeapon and APlayerMeleeWeapon, one hook covers the drill) + ability impl
   0x81CE80 (non-virtual, found by a ret-0x10 + pawn-offsets .text sweep), both
   identity-gated, detours call the original then substitute out-param rotators.
   **Decoupled aim PROVEN**: camera provably static, +30 deg substituted yaw moved
   impacts from the crosshair band (0.1% changed) to the right band (12.4% changed) -
   M6's criterion on BS2. The drill (melee) never calls the seam on air swings -
   BS1's wrench precedent transfers.
3. **Hand rays + laser + aim dot** (`vraim handray/laser/dot/trim/origin/pose`):
   b2r `frame_context.h` (duplicated per the decoupling directive, xr_local_trim_quat
   algebra), rays built in the CalcView tail, seam substitutes the hand rotator 1:1
   (sim hand at -25 yaw -> delta 25.00 deg exact), 6 laser dots + dot as compositor
   quads under SR stereo.
4. **The bone drive** (`vrhands`, `vrbones`): AHands rig found live (SkeletonInstance
   at +0x430, two-factor identity), pose bank poke-PROVEN to render, rigid
   whole-rig cluster on the right controller, reference-pose capture (inherent sway
   kill), reapply on the SR second pass, release() hand-back, >500 UU sanity refusal.
5. **Cheats lane PROVEN**: `F9=GiveAll` in User.ini [Default] (backup kept) works by
   effect - 999 ammo + full arsenal; digit keys 1-8 switch weapons flat (no wheel, no
   `exec NextWeapon` trap). THE USER CAN GET ALL WEAPONS NOW - press F9 in gameplay.

### The acceptance (VERIFICATION 2.8, both subsystems armed)

Five-station controller sweep: **aimRayMaxDevDeg 0/0/0/0.02/0 - constant**;
`vrhands status` last-write loc tracks at EXACTLY 100 UU/m (0.25 m -> 25.0 UU exact -
the fresh world-scale self-consistency measurement); model-only diffs (lasers off)
show the drill region moving at every station with the head static. Teardown with
everything armed: 470 ms close, zero dumps - session-38 baseline intact. Session-37
baseline intact (sr eyes 6.30 UU exact, wait2/s=0, guardskips 0, fov law, letterbox
self-heal all observed live this session).

### What did NOT land (named session-40 slots)

- **The engine does not consume the synthetic pad** (P2 verdict, re-confirmed with the
  ini lever): BS2 polls XInputGetState twice at boot, pre-bridge, then never again.
  The `[WinDrv.WindowsClient]` UseJoystick/UseController=True flip alone is NOT
  enough. Session 40: port BS1's input_drive SHAPE (UpdateInput per present +
  SetUseController + IAT hijack) with fresh RVAs - the thumbrest ammo modifier,
  grip switch and dual-wield trigger mapping all wait on it (core already implements
  the behaviors; they are reachable via `vrinput` the moment the engine polls).
- Per-hand cluster split (left = plasmid hand): needs the bone-name map
  (SharedSkeletonData at skel+0x08, banked). Whole rig rides the right controller
  until then.
- Aim/hands preset persistence + F10 sliders: session 40's brief (aim sliders focus,
  per-weapon presets).
- The RMB/plasmid window cast nothing at the fresh save (no plasmid equipped?) - the
  ability seam is hooked and identity-verified but its live substitution path is
  untested; first plasmid-equipped session covers it.

### USER CHECKLIST (headset first look; everything pre-armed for you)

Wrap-up round (same day, user request): **all the aim/hands toggles now DEFAULT ON**
(handray, laser, dot, substitution, hands - inert until the HMD drives gameplay), and
**F12 = grant + equip Telekinesis** in one press (the dev's own benchmark recipe,
verified by effect: icon + name on the HUD). So:

1. Load the save, `vrstereo on`, headset on. That is the whole setup.
2. **F9** = all weapons + ammo; **1-8** switch weapons; **F12** = Telekinesis
   granted AND equipped (left hand). Space through the tutorial popups - they eat
   clicks while open.
3. Point the right controller off-view and FIRE (LMB or trigger... LMB for now):
   impacts follow the controller, laser on the impact, dot on the hit surface.
4. The weapon model rides the right controller - check it feels in tune with the
   laser (the +-90 deg drift class from BS1 session 21 must NOT appear; if anything
   swims, say so).
5. Cast Telekinesis with RMB at objects/enemies - does the CAST follow the LEFT
   controller? (The left-hand ray is live; the ability seam's live substitution is
   the one unverified link - your cast settles it.)
6. KNOWN: controllers do NOT drive movement/buttons yet (input_drive port is session
   40 P1) - kb/m for locomotion. The left-hand MESH also still rides the right
   controller (per-hand cluster split is session 40); only its AIM is its own.
7. Ride-alongs if time: pitch-servo sign (`vrinput pitchservo status` looking
   up/down), helmet key-3810 collateral in other maps.

### THE FIRST LOOK HAPPENED (same day) - user verdicts, and they reshape session 40

The user tested in the headset. **PASSED**: model view-locked (does not move with the
head), model moves with the controller, aim moves with the controller. **FINDINGS**:

1. **Aim vs model misaligned ~90+ deg** (tracks the controller, constant offset) -
   zero model trims shipped; the anchor bone's authored frame needs a BAKED correction
   + fine trims (BS1 needed the same calibration). The flat instruments never measured
   MESH orientation (aimRayMaxDevDeg = laser vs ray; write-loc = position) - which is
   exactly why this survived to the headset.
2. **Model scale badly off; user wants a scale lever DECOUPLED from world scale** -
   consistent with session 33's rig-size/fg-lens coupling (why the helmet is hidden).
   The skeleton's per-bone SCALE channel renders (poke-proven this session): a
   `vrhands scale` cluster multiplier is the clean fix, zero world-scale coupling.
3. **Bullet leaves the HEAD, lands where the laser points** - correct read: only the
   DIRECTION is substituted this session; enable the origin substitution
   (BS1-parity `vraim origin on`, outLoc/outEffect from the hand-ray origin).
4. **Two dots seen** - the laser's 6 m end dot + the fixed-3 m aim dot; the dot does
   not clamp to the hit surface yet. Needs the distance slider / surface semantics.
5. **Left hand**: mesh still rides the right controller (known), and there is NO left
   LASER (core renders one laser; needs a small additive core extension or an
   active-hand swap policy).
6. **Wants all/shooting plasmids** (Telekinesis equipped fine); ElectricBolt/
   Incinerate item names to verify by effect (`<Name>BasicPlasmid` convention).
7. **Wants controller-driven controls NEXT** - "to basically test everything from the
   controller without having to use the mouse".

### Session 40 (REORDERED per the user's first look)

1. **input_drive port** (fresh RVAs: UpdateInput-per-present + SetUseController +
   IAT hijack) -> controller locomotion/buttons/triggers; then the BS1-parity
   bindings (thumbrest ammo modifier, grip switch, dual-wield triggers - core
   behaviors ready and reachable via `vrinput`).
2. **Model alignment + scale**: bake the anchor-frame correction, `vrhands trim`
   fine-tune via F10 SLIDERS (in-headset controls, not commands), and the
   decoupled `vrhands scale` cluster-scale lever. Then the origin substitution
   (`vraim origin on`) so bullets leave the hand.
3. **Left hand**: bone-name map -> per-hand clusters (left mesh on left controller);
   left LASER (additive core extension or active-hand swap).
4. Aim-dot polish (distance slider / hit-surface clamp), plasmid pack
   (ElectricBoltBasicPlasmid etc., verify by effect), ability-seam live cast check.
5. Preset persistence for all new levers; per-weapon aim presets (session-21 seeding
   bugs = what not to repeat).

Standing: BS1 regression testing stays deferred to the END of BS2 development (user
decision 2026-08-02). Core diff this session: ZERO (everything rode existing public
APIs - the BS1-risk surface was never touched).

---

## Previous state (2026-08-03, session 38 - THE EXIT CRASH WAS THE GAME'S OWN; closes are now instant and dump-free - branch `s38-b2r-teardown-and-aim`)

### The teardown crash: root cause found, and it rewrites the brief

The session-38 brief's hypothesis (teardown races the doubled-draw/flush machinery) is
REFUTED by an unattended bisect. The close-time fault at `Bioshock2HD.exe+0x4FF0FE` (null
read in the engine's display-apply path, inside close-time window-message dispatch) fires:

- with stereo armed (echo-verified), passive (nothing armed), with NO XR session at all,
- and in the decider run **G4: with EVERY mod hook skipped** (new `BVR_SKIP` env bisect
  lever) - no MinHook detours, no D3D11 hooks, no adapter - confirmed first-chance by the
  new `BVR_VEH=1` vectored observer.

Vanilla (shim renamed away) "exits cleanly" only because nothing observes it: teardown
burns 0.1 s CPU over a 5-9 s exit while a chained filter (CSERHelper) eats the fault
invisibly. **BioShock 2 Remastered crashes on its own exit path on every close** - its
Steam-forum reputation is earned. The faulting site varies (+0x4FF0FE mostly, +0xC6C2C2,
+0xC312D2, a 0xDEDEDEDE freed-vtable jump on the gameplay-quit path) - a FAMILY of
close-time faults. The mod's real defect was AMPLIFYING it: one 58 MB dump per close
(eating the 3-dump session cap that exists for real crashes), an 86k-retry exception spin,
and a multi-second noisy exit. Full derivation: ENGINE_NOTES session 38.

### What shipped (commits `f8813b8`, `4071543`)

1. **Teardown-aware crash handling (core, additive)**: the overlay WndProc (already
   subclassing the game window) calls `crash::note_teardown()` on
   WM_CLOSE/WM_DESTROY/WM_ENDSESSION; after that, any fault gets ONE log line, NO
   minidump, and immediate `TerminateProcess(0)`. Close latency measured **0.1-0.3 s with
   zero dumps** - faster and quieter than the unmodded game. Pre-close behavior unchanged
   (BS1 inert: it does not fault at exit).
2. **BS2 adapter hygiene on the same signal** (atomic-read gates, no-op while alive):
   doubled draw bails, 1t stops forcing the inline branch (vanilla close path), vrstereo
   arming refuses, the option/fg FOV writes stop WANTING inside a live CalcView (the
   existing OFF-edge restores then run through engine-provided live pointers), the
   letterbox self-heal never touches a closing window.
3. **Drain-guard hardening**: freed-but-non-null scenes (first dword unreadable or pool
   poison 0xDEDEDEDE/0xDDDDDDDD) are skipped - the gameplay-quit dump's class.
4. **`tools/read-dump.py`**: minidump summarizer (at-fault context, module+RVA stack
   scan) on the pip `minidump` package - no cdb needed. Plus the `BVR_SKIP` /
   `BVR_VEH` diagnostic levers, kept.

### Acceptance (sim, unattended)

Three echo-verified `vrstereo on` closes + one 5-min armed menu soak (308 full-rate 1T
beats, `wait2/s=0`, `guardskips=0`, zero watchdogs), all closing in ~0.1 s with ZERO new
dumps; the log shows `teardown noted (WM_CLOSE)` -> one fault line -> clean exit. The
in-game quit from GAMEPLAY (the 0xDEDEDEDE path) is the user's half - checklist below.

### Aiming arc derisked (offline; ENGINE_NOTES "Fire flow / aim")

- **The seam family EXISTS with BS1's exact shape**: `GetPerfectFireStart(out loc,
  out Rotator rot, out effectLoc)` on `Weapon` AND `AttackAbility` (+ a new
  `ShockPlayer tester` param), plus the whole BeginFiring/AnimNotify_UseAbility/
  InitiateDamage chain. Decouple-from-view: viable in principle by BS1's proven
  substitute-the-out-params property; flat decal test is the proof gate.
- **It is NATIVE** (`execGetPerfectFireStart` wide string in the exe - this build stores
  script names UTF-16; ASCII sweeps miss everything). BS1's nativemap recipe does NOT
  transfer verbatim (scanned: zero real entries); the wide-name registration region has
  no per-string pointers - session-39 derives the boot walker.
- **The dispatch question** (ProcessEvent-visible = preferred by-name seam, vs
  native-to-native = BS1-style impl hooks) needs ONE live probe: learn the fire-chain
  UFunction pointers, count ProcessEvent hits while firing drill/gun/plasmid in the save.
- **Quad-layer gap list**: core's laser/aim-dot API is ready; the adapter needs XR
  hand-pose -> game-space conversion (head already crosses that boundary), a fresh BS2
  world-scale measurement, and the seam hook for the dot's hit point.

### Wrap-up round (same day, user asked for everything flat): BOTH user items CLOSED

- **In-game quit acceptance: PASS, flat.** Full pause-menu quit with save-before-exit,
  stereo armed, driven by scancode keys: save written, `teardown noted (WM_DESTROY)`
  ~3 s later (post-save - detection in time on this path), ZERO dumps, process gone
  within bound. Two more fixes fell out: the first teardown gate DEADLOCKED this path
  (stopping the forced-inline flush mid-worker-teardown = a handshake that never
  completes; reverted - forced-inline runs through close, the drain guard is the safety)
  and core gained a 15 s exit WATCHDOG in `note_teardown` (bounds fault AND deadlock
  exit shapes). ENGINE_NOTES "wrap-up round" has the full derivation + harness lessons
  (BS2 menus are raw-input-cursor - keyboard only; Space at the title continues into
  the newest save; zombie processes poison Get-Process/tasklist - tools fixed).
- **coupling-viewmodel: PASS at the save.** World moves at 4-5x the animation floor
  under 20 deg yaw / 0.5 m strafe while the drill's screen block stays AT floor -
  view-locked, correct, zero headset time.

### Known harness warts (queued, none block session 39)

- Sim closes can pop a "Debug Error! ... bvr_xrsim32.dll - abort() has been called"
  dialog at process exit (debug CRT in the SIM RUNTIME objecting to teardown with the
  XR instance still open - by design we never destroy it at exit). Dev-machine-only,
  fires after the save is written; click Abort. Fix when touched next: suppress the
  debug-CRT dialog in the sim DLL (`_set_abort_behavior`/`_CrtSetReportMode`) or
  tolerate a live instance at detach.
- The `.xrs` runner's `@shot <name>` naming is broken (all captures write `shot_*.png`);
  use `xrsim-shot.ps1 -Out` manually.
- Sim compositor PNGs read one sRGB step darker than the real frame - coverage reads
  fine, brightness judgments invalid.

### USER CHECKLIST - nothing required

Everything session 38 needed from you was closed flat. Only ride-alongs remain, IF
headset time happens anyway: pitch-servo sign (`vrinput pitchservo status` while looking
up/down; invert lever exists), helmet key-3810 collateral watch in other maps.

### Next steps (session 39) - MOTION CONTROLS, per the user's brief (2026-08-03)

The user's priorities, in order:

1. **Motion controls + aiming (the headline)**: laser + aim dot, and the WEAPON MODELS
   moving with the controllers with full freedom (rotation + translation), aim and model
   IN SYNC. Build order:
   a. Dispatch probe first (unattended-ready now: boot via Space-continue, cheats lane
      below for a ranged weapon): FName-index derivation for the fire-chain names ->
      log-only ProcessEvent fire-watch -> fire drill/gun/plasmid -> by-name seam if the
      chain is PE-visible, BS1-style impl hooks otherwise. Then decoupled-aim flat proof
      (BS1's decal method, fresh numbers).
   b. Hand-pose -> game-space conversion + world scale (fresh measurement), then
      `vr::set_laser`/`set_aim_dot` publishing (core API ready).
   c. Models-follow-controller: BS2-NATIVE method first. BS2's fg lens already matches
      law-exact (session 37, G self-identifying) - BS1's ENTIRE counter-model domain
      (render lock, lockgain/lockdgain/lockpull) existed to fight its lens mismatch and
      its lock's own correction CAUSED the +-90 deg laser-vs-gun drift (session 21 verdict:
      `vrbones lock off` = "aim in tune with the model... perfect"; the user remembers
      this as "switched from abs" - `lock abs` was the anchor-to-world mode). So: compose
      hands at true geometry, NO lock domain, and verify sync flat from day one -
      `coupling-hand.xrs` + `aimRayMaxDevDeg` CONSTANT across controller positions is
      the acceptance instrument (VERIFICATION 2.8; arm BOTH vrhands and vraim - the
      one-subsystem trap made a fake pass once).
   d. **Normal controller controls incl. thumbrest, BS1-parity**: BS1's shipped shape =
      LEFT `/input/thumbrest/touch` as the ammo-type modifier, weapon switch on grip,
      stick-click fallback until a real thumbrest touch is seen (session 23, user:
      "perfect"). Port the BEHAVIOR, derive BS2's bindings fresh (BS2 has native
      dual-wield - the left hand is the plasmid hand, not a mirror of BS1).
2. **Cheats (if session time remains; else session 40 P1)**: goal = user can GET ALL
   WEAPONS to test with. The lane is fully unattended now: bind the session-32 command
   ladder (`GiveAll` first) to F9/F12 in `User.ini` **[Default] section only** (the
   seven-section trap), press via `game-key`, VERIFY BY EFFECT (weapon wheel screenshot
   diff), exact item names from the BakedScripts dump if `GiveAll` fails. Fallback: a
   prepared everything-save as the test fixture.

### Session 40 (planned)

Cheats if not done; then BS1-parity SLIDERS for aim and models (aim sliders are the
focus - fix aim on the models), and PER-WEAPON AIM PRESETS that auto-swap with the
equipped weapon (BS1 shape: profiles keyed off the rig's current-holdable read, applied
over the preset baseline - session 21's two seeding bugs are the reference for what NOT
to repeat; derive BS2's equivalents fresh).

Standing: pacing-epic residue unchanged; BS1 regression testing stays deferred to the
END of BS2 development (user decision 2026-08-02).

---

## Previous state (2026-08-02/03, session 37 - THE LETTERBOX WAS THE WINDOW; RESOLUTION IS LIVE - merged to `bioshock-2`, ACCEPTED IN-HEADSET)

**The BS1-parity resolution picker ships, and it is better than BS1's: the apply is LIVE.** The
session-36 brief's three blocking unknowns all closed in one unattended screening pass (zero user
boots spent - BS2's menu background classifies as a strict-gameplay ShockPlayer view and renders
the full scene pipeline, so the campaign ran against it under xrsim).

### The finding that reshaped the feature

**The letterbox was never the engine - it was the WINDOW.** The engine sizes its scene viewport
to the window CLIENT area every frame while the backbuffer holds the ini size, and the game's
chromed window clamps on the desktop: on this 2560x1440 monitor the client tops out at **1421
rows** (outer height clamps to 1460, minus 39 rows of chrome). Sessions 32-33's "mystery ratio"
1.4413 = 2048/1421 is window arithmetic. Even 2560x1440 (16:9!) letterboxed (client 2560x1421,
lb=1.0141, anamorphic). A **borderless popup client sized exactly to the render** - beyond the
desktop where needed - renders full-height square pixels at EVERY aspect tried (1.778 / 1.6 /
0.9348 / 0.9321), and the engine follows a live client resize with its own ResizeBuffers. The
aspect bisection is DISSOLVED; resolution on BS2 needs no relaunch at all.

### What shipped (branch `s37-b2r-res-picker`, all in `bioshock2r/` + sim/docs)

1. **F10 "RENDER RESOLUTION (applies live)"**: preset combo (`flat` 1920x1080, `perf` 1648x1768,
   `native` 2064x2208 Quest 3 class, `sharp` 2480x2648, `max` 2888x3088), custom WxH
   (1024..8192), MPx readout, an auto-FOV preview for the SELECTED size (`auto_option_for_dims` -
   never the live-backbuffer inverse), aspect guidance against the eye's ~0.93, Apply + Restore
   buttons through the `g_resWritePending` pending seam (render thread posts, poll gate applies -
   and BS2's poll gate ticks at the menu, which BS1's CalcView consumer cannot).
2. **`vrres` grew the same table**: `vrres native|perf|sharp|max|flat`, `vrres list`,
   `vrres restore`, raw WxH - all through one `apply_resolution()`: borderless window resize
   FIRST (engine follows), Shared.ini persistence SECOND, deferred re-verify THIRD (the engine
   persists its live size into Shared.ini ON RESIZE one step behind - the mechanism behind every
   historic "my write did not stick" report; it does NOT rewrite at exit, measured).
3. **A stereo-gated self-heal**: every chromed boot starts letterbox-clamped; when stereo is
   armed and the client is smaller than the backbuffer, the borderless fix re-applies within ~1 s
   (verified live: boot at 2064x2208 -> client 2064x1421 -> `vrstereo on` -> healed to full
   2064x2208). Never fires flat, never under StartupFullscreen, holds off around an apply.
4. **The automatic FOV needed no new mechanism** - `vrfov` (default ON) already computes the
   option per CalcView from the headset half-angles via the inverse law. Session 37 verified it
   at the new aspects: at native the mod writes option 138 and the world renders **107.7 x 111.4
   deg against the 108 x 110 eye** - the eye-matched configuration. UI relabeled ("Automatic FOV
   (computed from your headset, never manual)"), option value shown, stale "squarer only narrows"
   copy replaced (FOV = coverage lever, resolution = sharpness lever). `gfov` stays the manual
   override.
5. **The sim eye is pinned to the measured VDXR values** (h=54 v=55; was the spec-guess 55/48,
   which flips the FOV circumscription branch - the session-34 open item). `fov` shorthand
   defaults follow. claimRatioH semantics corrected on the record: it is claim/EYE, ~1.0 only
   when render==eye; the per-config assertion is the law-derived expected value (measured to four
   decimals at native: 0.99521 symmetric, 1.16973 asymmetric-default).
6. **The world FOV law held at two more clean aspects** (tanV 1.427990 invariant through a live
   16:9 -> 1.6 change; 2064x2208 dump decodes law B PASS dH=0.00000). Stereo-on-1t survived every
   live resize: `wait2/s=0`, guardskips 0, zero faults, zero watchdogs.

### Same-evening addendum: the user's first native run, and the fg lens fix

The user ran `native` in the headset the same evening: **sharpness accepted** ("looks pretty
sharp so we're good"), the auto-FOV A/B behaves exactly per the law (off = pillarbox), **but the
viewmodel was moving against the head** - the exact defect the menu-scene dump flagged. In-save
three-probe measurement (drill drawn) settled it: the fg lens renders `tanV = tan(d/2) *
G(aspect)` with G(0.9348) = 0.99488 while G(16:9) = 9/16 exactly - writing the raw option at
native was a 1.24x viewmodel gain. **Shipped fix: the match self-identifies G live** from the
fov watch paired with its own last write, and writes `d = 2*atan(tan(option/2)*(9/16)/G)` -
identity at 16:9, converges in one sample, freezes when the lenses merge. Full derivation +
table in ENGINE_NOTES session-37 section 6. Build installed to the game folder; the user
launches tonight on their own schedule.

### ACCEPTED IN-HEADSET (user, 2026-08-03): "it's perfect"

The user's verdict, multiple resolutions tried: **"everything is perfect - the FOV is filling
the screen and the weapons/hand models are stable and glued."** The flat/sim half was verified
the same day: fresh boot, the G fix self-identified in one sample (`lens gain G 0.56250 ->
0.99488`), wrote 111.7 at option 138, and BOTH `dumpframe full` captures decode to ONE law-exact
cluster (dH=0.00000, square pixels, full height). Stability: the user's whole play session shows
ZERO watchdog lines and zero guardskips (the instrumented 5-min soak is thereby covered by a
longer real session; run one only if stability comes back into doubt). Session 37's feature -
live resolution picker + automatic FOV + the self-identifying viewmodel lens - is CLOSED end to
end.

### Teardown crash at exit - REPRODUCED UNATTENDED, FOUR dumps banked

Correction to the first read: the crash is NOT tied to the borderless/native window - the dump
folder shows it fired on the 2026-08-02 evening closes too (16:9-era, chrome restored). The
common factor across all four is a STEREO-ARMED close. Always AFTER the save (cosmetic: a
few-second exception loop, then exit):

| when | context | fault | dump |
|---|---|---|---|
| 08-02 19:43 | sim session close, 16:9-era | (log rotated - read the dump) | `crash\bvr_20260802_194326.dmp` |
| 08-02 19:51 | sim session close, 16:9-era | (log rotated - read the dump) | `crash\bvr_20260802_195143.dmp` |
| 08-02 21:44 | quit from GAMEPLAY, VDXR, native | DEP EXECUTE at 0xDEDEDEDE (freed poison), tid=game | `crash\bvr_20260802_214440.dmp` |
| 08-03 17:17 | quit from MENU scene, xrsim, native | null READ at `Bioshock2HD.exe+0x4FF0FE` | `crash\bvr_20260803_171738.dmp` |

`+0x4FF0FE` is game render code in the Draw/flush-chain neighborhood (Draw 0x4EE8D0, flush call
site 0x4EF4A1) - working hypothesis: teardown races the doubled-draw/flush machinery (the
drain's no-null-check shape is exactly this class; the 1t drain guard covers the FORCED path,
teardown may reach the drain another way). REPRO RECIPE (no headset needed): `xrsim-launch
-Game bs2` -> menu scene -> `vrstereo on` -> close the window. Candidate fix shape: a clean
disarm on teardown (WM_CLOSE/exit hook: park the second draw, restore the option/fg writes,
let 1t idle) BEFORE the engine frees the scene.

### Next steps (session 38)

1. **Teardown-crash fix**: disasm-rva 0x4FF0FE + read the four dumps (summarize in
   ENGINE_NOTES, never commit game-derived output), implement the clean-disarm-on-teardown,
   accept with the sim recipe (three closes, zero new dumps) plus one user quit.
2. **BS2 aiming arc begins** (the ROADMAP successor: aiming -> motion controls -> normal
   controls). Derisk first with BS2-native methods (ProcessEvent-by-name seam; BS1 is shape
   reference ONLY - no constants, no laws, no policies transfer): where does BS2 read the
   weapon's fire direction, can the aim decouple from the view like BS1's M6, and what do the
   laser/aim-dot quad layers need. The sim's `aimRayMaxDevDeg` + `coupling-hand.xrs` lane is
   the flat instrument.
   ALSO (user's point, 2026-08-03): run `coupling-viewmodel.xrs` on BS2 - the capture-based
   "does the viewmodel stay view-locked under a head sweep" check. It is the PICTURE-level
   complement to the session-37 lens-cluster instrument: cluster equality proves the LENS,
   not the POSE - a transform/attachment bug would pass the dumps and fail the pictures.
   Needs the save (a weapon in frame), a LIT area, and coverage/bbox reading, not headline
   diff numbers (the session-34 dark-scene trap; BS1 baselines in VERIFICATION 2.8).
3. Ride-alongs when headset time happens anyway: the pitch-servo sign check (`vrinput
   pitchservo status` while looking up/down), helmet key-3810 collateral watch in other maps.
4. Pacing-epic residue unchanged (keepalives with real layers). BS1 regression testing stays
   deferred to the END of BS2 development (user decision 2026-08-02).

---

## Previous state (2026-08-02, session 36 - THE BS2 STEREO FREEZE IS DEAD - merged to `bioshock-2`)

**`vrstereo on` ships real full-rate stereo - both eyes every frame, running on `reentry 1t` - and
it no longer freezes.** In-headset confirmed same day (VDXR/Quest 3, immersive, head-tracked,
user-verified stable). Nine commits on `s35-b2r-reentry-freeze`, merged to `bioshock-2`.

### What landed

1. **The soak lane is proven**: `tools/soak.ps1` boots unattended (`-Boot map Ghetto.bsm` reaches
   gameplay in ~2 min; `-Boot key`/steam with the user loading the save), waits for the camera
   heartbeat, arms, and turns "did it freeze?" into an exit code (0..9). A WATCHDOG episode the
   game RECOVERS from is a load, not the freeze - each gets a 30 s recovery watch (the wedge never
   advances its log again). The beat regex was timestamp-blind on day one; fixed.
2. **The flush chain is banked and verified**: constants + `verify_flush_chain()` in
   `bioshock2r/patterns.h/.cpp` (call site 0x4EF4A1 -> thunk 0x24A28 -> body 0x69FC30 + prologue,
   pure image reads). ENGINE_NOTES' session-26 "no submit handshake" section is REWRITTEN - that
   claim was the sole reason 1t was never ported. Live confirmation: the wedged stack reads
   `B8108F BB1963 69FD33 4EF4A6`, frame for frame the derived chain.
3. **The trigger question is measured shut**: `wait2/s == 2nd/s, set2/s == 0` at BOTH 1920x1080 and
   1280x720 - the second flush entered the `Wait(INFINITE)` on EVERY doubled frame since `97a229a`.
   The resolution/FOV work created no reachability; the 5-100 s onset was per-wait lost-wakeup
   probability. fgfov/vrfov A/Bs moot (wait2 saturated); the 4-commit bisect is superseded.
4. **`reentry 1t` for BS2** - BS1's session-8 cure, duplicated with fresh constants (never shared,
   never core): drain guard FIRST (null-scene skip; BS2 has BS1's drain+0x33 shape), flush-point
   force second, quotient never poked. ~15% draw cost (91 -> 78/s). `wait2/s == 0` by construction.
5. **Defaults**: `vrstereo on` = 1t -> camera mode -> stereo (BS1's ladder, full-rate SR).
   `reentry srdev on` = the repro escape (raw doubled draw WITHOUT 1t - wedges, dev only).
   `vraer` = AlternateEye. `vrstereo off` disarms everything symmetrically.
6. **Two real VDXR bugs found by the first headset attach since session 34** (the sim force-grants
   focus and structurally cannot see either):
   (a) a NEVER-focused session must keep its frame loop running - frames are how the runtime walks
   READY -> FOCUSED; `detach_skip_decision` gained the same bring-up exception `pace_should_skip`
   always had (core, but behind the pace-detach lever - BS1 untouched);
   (b) a DETACHED session is stranded FOREVER by VDXR - empty keepalive frames are never
   re-promoted (state parks at VISIBLE, shouldRender=0). BS2's detach default is now OFF: with the
   wait off-thread the unfocused frame loop is cheap (lastEnd ~1 ms measured live) and real
   submission is what lets VD re-grant FOCUSED by itself. `vrpace detach on` is the live A/B.

### Acceptance (all on the real game, in the user's save - per the user: test in the save, never the menu)

- **The decider**: 10-min flat soak of full-rate stereo on 1t - PASS (one recovered load episode).
  The unfixed build wedges in 5-35 s; measured 25 s (exit 3) on 7dce78c the same afternoon.
- **Load crossings, stereo armed and sticky throughout**: save load, quit-to-menu x2, new game,
  an idle-death respawn - guardskips 0, zero faults, zero new dumps (user-driven, log-verified).
- **Post-flip smoke** of plain `vrstereo on` - PASS.
- **In-headset**: immersive, head-tracked, full-rate stereo on 1t, stable. The user also confirmed
  the FOV-fill toggle behaves as designed (bars without it; full FOV at lower pixel density with
  it - the known P2 polish item; the counter-lever is `vrres`).

### Deferred by the user's call (run when stability is next in doubt; each needs the save loaded at boot)

```powershell
.\tools\soak.ps1 -Game bs2 -Minutes 10 -Arm ""                            # vanilla flat
.\tools\soak.ps1 -Game bs2 -Attach -Minutes 10 -Arm "vrcam on"
.\tools\soak.ps1 -Game bs2 -Attach -Minutes 10 -Arm "vrcam off; vraer on"
.\tools\soak.ps1 -Game bs2 -Attach -Minutes 10 -Arm "vraer off; vrstereo on"
.\tools\soak.ps1 -Game bs1 -Minutes 10 -Arm "vrstereo on"                 # BS1 regression
.\tools\xrsim-launch.ps1 -Game bs2                                        # sim never run vs BS2; selftest PASSED 2026-08-02
```

### Next steps (user-directed, 2026-08-02 end of session 36)

1. **SESSION 37 PRIORITY: BS1-parity resolution picker + automatic FOV (user-corrected brief,
   2026-08-02).** The user wants BS1's experience on BS2: a resolution picker offering
   headset-NATIVE (squarer) resolutions for the Quest 3 plus presets and custom, where choosing a
   squarer buffer ADAPTS to the headset - more rendered view, bars gone - with the FOV
   **calculated by the mod** (never manually overridden), and no world warp / no viewmodel
   movement against the head. On BS1 the engine does this itself (true-horizontal law: squarer
   buffer = taller FOV; see b1r `vrres`, camera.cpp:364 - the eye render IS the backbuffer). BS2's
   law is inverted (tanV fixed by the option against 16:9; tanH follows aspect), so the MOD must
   compute the option write from the headset FOV target + chosen aspect (option =
   2*atan(tanV_target*16/9); the claim already uses the law, so it stays warp-free by
   construction). Blocking unknowns, in order: (a) the ROADMAP "aspect bisection" - the squarest
   aspect BS2 renders full-height; the "degenerates off 16:9" claim was RETRACTED in session 33 (a
   decoder bug), and the REAL off-16:9 defect is anamorphic (the frustum takes the backbuffer
   aspect while the scene renders letterboxed, e.g. 2048x2048 -> 2048x1421 scene) - characterize
   whether the letterbox persists at all aspects and whether the claim needs an anamorphic
   correction; (b) verify `vrres` (Shared.ini) still applies end to end - the user doubts it;
   (c) the XR swapchain sizing (does it follow the backbuffer?). The user's current `vrfov`-only
   fill is the fallback, densified by supersampling, if the engine hard-letterboxes everything.
   Regression guards that must survive: the fgfov viewmodel lens match (weapon/hands glued to the
   view - session 33's accepted fix), no world warp (claimRatioH ~1.0), stereo-on-1t untouched.
2. **Helmet: HIDDEN BY DEFAULT since session 36** (user instruction; edge-of-FOV placement is not
   possible for a single mesh). UNVERIFIED caveat, first check next session: the index count 3810
   is a GLOBAL key - confirm no unrelated mesh vanishes in other maps, or tighten the key to the
   foreground pass.
3. Then per ROADMAP: aiming, motion controls, normal controls for BS2.
4. **BS1 regression testing is deferred to the END of BS2 development** (user decision 2026-08-02:
   work ships to the `bioshock-2` branch only, never main, so BS1 exposure begins only at the
   eventual promotion; the deferred soak matrix above is the recipe).
5. Pacing-epic residue (lower priority): keepalive frames that carry REAL layers so detach-on gets
   recovery; VD re-grants FOCUSED only when the app is foregrounded in the headset (double-tap) -
   "black void + VD environment" means backgrounded, not broken.

---

## Previous state (2026-08-01, session 35 - PAUSED MID-SESSION - branch `s35-b2r-reentry-freeze` off `bioshock-2`)

**The BS2 stereo freeze has a verified root cause. The fix is identified and not yet written.**
Two commits are pushed; nothing is half-applied in the working tree; the build is clean.

### THE FINDING (verified against the shipped exe with `tools/disasm-rva.py`, not inferred)

`UGameEngine::Draw`'s tail makes exactly one call to a **render flush point at RVA `0x69FC30`** - a
structural twin of BS1's `0x61D260`, veto for veto. Its threaded branch is the
`WaitForSingleObject(INFINITE)` session 34's watchdog caught. `maybe_second_draw` runs the whole Draw
a second time per tick, so that handshake runs twice per tick, and the second one's
flag-test-then-wait races the render worker's completion signal.

| role | BS1 (shipped) | BS2 (derived session 35) |
|---|---|---|
| flush point, `ret 8`, `ecx` = mgr | `0x61D260` | `0x69FC30`, prologue `55 8B EC 8B 55 0C 8B 45 08 56 8B F1` |
| only static caller | build | `0x4EF4A1` via thunk `0x24A28` (exactly one) |
| render-mgr global | `0x1356590` | `0x17DBF4C` (read at the call site into `ecx`) |
| scene slot | `mgr+0x0C` | `mgr+0x24` |
| view group | 16 dwords -> `mgr+0x10` | 14 dwords -> `mgr+0x28..0x5C` |
| threaded / flush-seen | `mgr+0x50` / `+0x54` | `mgr+0x60` / `+0x64` |
| INLINE branch | drain, nothing after | `call thunk 0xE29B -> 0x69F3F0`, then `ret 8` - **nothing after** |
| THREADED branch | flag-then-INFINITE-wait | `mov ecx,[mgr+4]; call thunk 0x1FBF9 -> 0xBB1950`, ret `0x69FD33` |
| drain | `0x61CAE0` | `0x69F3F0` (`[mgr+0x24]` then `lea edi,[esi+0x30]`, **no null check** = BS1's `drain+0x33`) |

**Nothing after the inline drain** is the property that makes BS1's session-8 cure lossless, and BS2
has it. So the fix is `reentry 1t` for BS2, ported with fresh constants - full-rate stereo, both eyes
every frame, exactly what BS1 ships. **Session 26's premise is refuted structurally**: its "the Draw
path has no submit handshake" is the only reason 1t was never ported, and Draw calls the flush point
directly.

**Mechanism vs trigger.** `0xBB1950` skips the wait entirely when the worker has already finished
(`cmp [esi+8],0; jne skip`). So whether the race is *reachable* is pure timing - which is exactly what
a resolution change moves. The user's recollection that the freeze began with the resolution/FOV work
is therefore **compatible** with this, not contradicted: the doubled draw made the race possible, the
resolution work plausibly made it reachable. Settle it with the A/B below, not with four builds.

### Landed this session (pushed)

- **`tools/soak.ps1`** - the acceptance instrument. Waits for gameplay (the 1 Hz camera heartbeat),
  arms one command, then fails with an exit code on: process death, `bioshockvr.log` ceasing to
  advance, new `WATCHDOG` lines, or new crash dumps. Exits **7 (inconclusive)** if `pacetrace.log`
  never appeared, because a WATCHDOG check with no tracer is vacuous.
- **`tools/game-key.ps1`** - the keyboard lane the harness never had, so a soak can pass the BS2 title
  screen unattended. Scancodes via `KEYEVENTF_SCANCODE`, not VK codes.
- **The stall watchdog could not fail an acceptance run, and now can.** Its trigger required an open
  draw stage, which only BS2's doubled draw ever opens - so a clean soak of vanilla/`vrcam`/`vraer`
  was guaranteed rather than evidence. And `watchdog_all_threads()` printed nothing every time via
  five silent exits. Both fixed; `nf == 0` filter dropped (it hid the far side of the deadlock).

### Next steps (in order; the plan is `~/.claude/plans/session-35-bioshock-2-jiggly-leaf.md`)

1. **Boot experiment** - try `Bioshock2HD.exe Ghetto.bsm` (maps in `ContentBaked/pc/Maps/`); if the
   remaster still routes through the front end, fall back to `soak.ps1 -Boot key`, then `-Attach`.
   Then a first baseline soak to prove the harness end to end.
2. **Backend selector** (`apply_vrstereo` -> AlternateEye by default, SequentialReentry behind a new
   `reentry srdev on`) so the shipped default cannot freeze while the real fix is built. **Temporary**
   - step 6 flips it back. Move `set_sr_pair_pacing(true)` out of `apply_vr_preset` into the SR branch.
3. **Timing A/B**: add a `waitTaken/s` counter to the `[reentry] beat` line (how often the second
   flush finds the latch already set vs has to wait), then soak 5 min each at baseline / lower
   `Shared.ini` resolution / `fgfov off` / `vrfov off`.
4. **Land the constants** in `bioshock2r/patterns.h` with a `verify_flush_chain()` in the shape of
   `verify_draw_chain`, and **rewrite** (not annotate) ENGINE_NOTES' session-26 "no submit handshake"
   section. Also fix the stale `patterns.h:87` CalcView comment, which contradicts `patterns.h:115`.
5. **`reentry 1t` for BS2**: drain guard on `[mgr+0x24] == 0` first, then `FlushPointDetour` forcing
   the inline branch. Never poke `0x149760C` - BS1's poke crashed a loader thread.
6. **Acceptance**: 10-min soak of every mode (vanilla, `vrcam`, `vraer`, and the decider
   `reentry srdev on; vrstereo on`), a BS1 regression soak, the load-crossing matrix, then the
   simulator. Then flip the default back to full-rate stereo.

**User's bar for this session**: every VR mode stable, BS1/Infinite untouched, and `vrstereo on` must
still be *real stereo* - removing the feature is a floor to avoid shipping a freeze, not the goal.

---

## Previous state (2026-08-01, session 34 - A SIMULATED QUEST 3: agents can test VR without the user - branch `s34-xrsim-simulated-headset`)

**M0-M9 pass, regression is green, and the branch is pushed. One open thread, below.**

`bvr_xrsim32.dll` is a purpose-built 32-bit OpenXR runtime that presents as a Quest 3, so an agent
can drive the head and controllers, step frames deterministically, and capture per-eye compositor
images - with no headset and with **zero lines of the mod changed** (M9: the mod DLLs hash
identically with the sim target off and on at one commit).

**Everything about how to USE it lives in `docs/VERIFICATION.md`** - the decision table, the command
grammar, the `state.json` and capture-JSON field lists, the measured baselines and the failure
modes. This section is the handoff only; do not grow it back into a manual.

### Why it matters

- Per-eye captures include the **XR quad layers** - aim laser, aim dot, HUD panel - which
  `docs/bioshock1/TESTING.md` has recorded since M8 as impossible to see in a window screenshot.
- `derived.claimRatioH` turns the whole claimed-FOV bug class into one number (1.0 = correct; BS1
  measures 0.98; session 28's yaw warp was 1.84 and took three sessions to infer).
- `derived.aimRayMaxDevDeg` answers "is the aim in sync with the weapon model" numerically.
- Session-state hazards (`focus lose`, `idle on`, `hazard ...`) make known bugs repeatable on a desk.

### Controller coupling: RESOLVED, and it works

The user's actual priority (stated 2026-08-01) is **visual and geometric coupling**, not input
fidelity: does the world warp under 6DOF, is the viewmodel glued to the view or to the world, does
the weapon model follow the controller, does the aim stay in sync with the model. Three sequences
cover those - `world-6dof.xrs`, `coupling-viewmodel.xrs`, `coupling-hand.xrs`.

Both halves of the hand test are now confirmed on BS1 (2026-08-02):

- **The aim ray tracks the controller.** Laser dot X follows grip X exactly
  (`0.172 -> 0.642 -> -0.293` as the controller sweeps `0.20 -> 0.55 -> -0.15`), and
  `aimRayMaxDevDeg` holds constant at every controller position - which is what "in sync" means.
- **The weapon model tracks the controller.** `vrhands status` shows `last write loc` following the
  controller at ~100 UU per metre: a 1.1 m sweep moved X by 110 UU, a 0.4 m drop moved Z by 40 UU.
  The captures confirm it visually - the sleeve sits lower-left at one extreme and at the right edge
  of frame at the other.

**The scare that got us there is the lesson, and it is recorded in VERIFICATION 2.8 trap 2:
`mean-abs-diff` is useless in a dark scene.** BS1's opening corridor has meanLuma ~3/255, so a dark
sleeve crossing the entire frame reads 0.25 - below the 0.4 standing-still floor. The headline
number said "nothing moved" while the model had crossed the screen. In dark scenes read the
coverage/bbox, `pct-channels-changed`, and the PNG; and cross-check against `vrhands status`, which
is lighting-independent ground truth.

### Two harness bugs found and fixed while chasing it

- **`@mod` raced the mod's poller.** The mod reads `command.txt` at 1 Hz on mtime change, so
  back-to-back `@mod` lines overwrote each other and `vraim on` silently never took - the sequence
  then "passed" while measuring the wrong thing. `@mod` now groups with `;` into one write and waits
  a poll period. Same trap `game-batch.ps1` already documents.
- **`hand <h> to` was declared but never implemented.** Smooth controller motion is required to see
  a model TRACK rather than teleport, so the coupling tests could not have worked without it.

### Next steps

1. Review and merge the branch.
2. Run `coupling-viewmodel.xrs` and `world-6dof.xrs` end to end. Both are written and the machinery
   under them is proven by `coupling-hand`, but neither has had its own acceptance pass yet. Pick a
   LIT area of the level for them, not the opening corridor - see the dark-scene trap above.
3. Pin the FOV defaults from a real headset run: the mod's own
   `xr: headset fov half-angles h=.. v=..` line gives the exact VDXR values, fed back via
   `fov eye l <l> <r> <u> <d>`. Until then FOV-derived captures are relative, not absolute.
4. Adopt the sim for BS2 and Infinite - the runtime is game-agnostic, the scripts take `-Game bs2`,
   each needs only its own acceptance run.
5. `boot.ps1`'s A-press loop does not reach gameplay from the main menu on this save (CONTINUE needs
   a click plus VK_RETURN). Pre-existing, and NOT caused by `-Attach`, which works correctly.
6. The markdown cleanup the user originally asked for is still deferred. Findings stand: STATUS.md
   has three `Next steps` sections (two stale, 1074 lines), a duplicate `Session log` heading, and a
   misfiled session-29 entry.

### What the simulator does NOT settle

It proves geometry, content and protocol. It models no lens distortion, no timewarp, no real display
cadence and none of VDXR's Wi-Fi encode path. The `unfocused-pacing` pass measures the mod against
the SIM's model of focus loss, not VDXR's. **A pacing bug that reproduces in the sim is real; one
that does not may still exist on hardware.** Comfort, judder and world scale remain the user's
verdict in the headset.

## Previous state (2026-07-31, session 33 - BS2: THE VIEWMODEL LENS IS FIXED AND ACCEPTED; VR PACING IS THE NEW BLOCKER - merged to main)

User priority 2 is DONE. Priority 1's aspect question is answered on paper. And a bug that makes
the game **unplayable in VR** was characterised, partly fixed, and is now the top of the queue.

### 0. THE FIX: BS2's foreground lens is `PlayerController + 0x694`

A float in DEGREES. BS1's is +0x460 - same engine family, same 75/60 shape, different link.
**Shipped DEFAULT ON**, with an F10 overlay checkbox as the A/B.

**In-headset verdict (user, same day): _"I tested the match viewmodel lens to the world and it
worked, the weapon was not moving anymore."_**

Flat acceptance, all from full frame dumps: ONE cluster at every option value (100 -> tanH 1.1918,
130 -> 2.1445, 80 -> 0.8391), TWO the moment it is disarmed, 60.0 restored exactly. Tracking the
option at every value is the property a baked constant could not have.

The derivation method is the transferable part: sweep the live PC and pawn for 60.0 floats, then
**poke each candidate to a DIFFERENT distinctive FOV and take ONE dump** - the fg cluster lands on
the value only one of them could have produced. One capture, no bisection, and immune to the
"did it merge or did we stop sampling it" ambiguity that produced six false positives first.

**`+0x690` is the WORLD lens - do not write it.** It looks exactly like BS1's fovA next to fovB and
it is not.

### 0b. THE NEW OPEN ITEM the user wants next: the rig eats the view

With the lens matched, the Big Daddy helmet takes much more of the screen and there are black bars
at the bottom. The user wants the visible image to fill the whole FOV. This is a DIFFERENT defect
from the swimming: the fg rig's apparent SIZE is coupled to the FOV value, not only to its lens -
widening the lens also moves the foreground eye, so the rig grows. Leads in Next steps.

### 1. THE BLOCKER: the game is paced by a headset that is not presenting

**Not a hang, not stereo, not the fov watch.** Measured flat with no headset:

```
xr: pace guard ON | wait off-thread | session SYNCHRONIZED everFocused=0
    | skips 0 lastWait 0 ms | handoffs 12747 timeouts 0
```

Nothing is blocked. The mod opens an OpenXR session whenever a runtime is present and the game is
then "paced by the headset". Not FOCUSED -> the runtime paces its not-visible cadence, about
**10 Hz** -> `draws/s 10`, `call2Us 99765` for the doubled stereo draw against session 26's
4.5-5.0 ms. In a headset that reads as a freeze. **Alt-tab reproduces it** (user-confirmed):
alt-tab drops the session out of FOCUSED. The user's words: *"the game is unplayable."*

Fixed here: **`vrcam off` now disables VR** (it only cleared the camera mode before, so nothing
could stop the pacing once a session ran), and the heartbeat carries `xr=<state>/neverFocused`.

NOT fixed, and it is priority 1 next session - see Next steps.

### 2. The world FOV law is SETTLED, and it is the OPPOSITE of BS1's

`tanV = tan(option/2) * 9/16` (aspect-INVARIANT), `tanH = tanV * bbW/bbH`. Verified at two aspects
x two FOV options entirely from dumps already on disk - no relaunch spent. BS1's law is the true
horizontal. The FOV LAW is now a third never-copy instance after the ini keys and the cb0 offset.
b2r's OpenXR claim uses the law rather than the raw option: identical at 16:9, correct elsewhere.

### 3. Session 32's "the projection degenerates off 16:9" is RETRACTED - it was a decoder bug

The ray block's vertical SLOPE carries a letterbox factor (RTheight/viewportHeight) that the
offset term does not; reading the pair as an equality rejected every letterboxed block. The ratio
was 1.4413 = 2048/1421 exactly in every block of both square dumps. Consequences: `-ScanLayout`
now finds offset 16 on the square dump (session 32 got nothing and read it as "a different layout
shape"), the live watch was blind at every aspect but 16:9, and the real non-16:9 defect is
**anamorphic** - the frustum takes the backbuffer aspect while the scene renders letterboxed.

Also retracted: `0xAECACF` is NOT the foreground's callstack signature. It and the world's
`0xAEC7B4` are two call sites of the same draw dispatch; 62 draws carry it against the fg's 19.

### 4. `lenses == 1` is not an acceptance criterion, and now says so

The fov watch samples ~12 of 400-600 buffers; the fg pass is ~17. Absence is the ordinary case.
Two attempts to fix the coverage are recorded as dead ends IN THE CODE: a head-slot reservation
(the fg pass moves between captures) and a rotating stride phase (correct, and 20x the frame time).
Acceptance is `dumpframe` + `decode-framedump.ps1`. `vrhud fovwatch off` disables the watch.

### 5. Tooling and harness

New: `tools/disasm-rva.py` (capstone; the one-liner three sessions retyped), `tools/game-batch.ps1`
(command sequences, no screenshots, `-NoFocus` for headset sessions), `tools/launch-game.ps1`
(refuses to launch over another BioShock, clears stale command.txt). `vrpreset` ported to BS2 - it
had NO persistence at all, so no in-headset verdict could be re-checked against the same numbers.
`vrpace`, `vrmirror`, `vrhud` dispatched on BS2 for the first time (same class as session 32's
`vrinput` gap). **F10 overlay: a "VIEWMODEL LENS" section with the toggle, a manual FOV slider, a
live readout and a save button** - anything judged by eye now lives there, because typing means
alt-tab and alt-tab is the pacing bug.

Harness lessons, all field-hit: a `game-shot` loop wedges BS2 under stereo (PrintWindow forces a
re-render); `Process.Responding` is not liveness in VR (the window pump is starved - it refused a
whole A/B on a healthy run); a guard that only prints is not a guard (the Infinite check reported
Infinite was running and launched anyway).

## Next steps

### 1. THE PACING BUG (top priority - the game is unplayable in VR without it)

A session that is running but not FOCUSED must not pace the game. Session 28 deliberately stopped
SKIPPING frames while unfocused - skipping made the alt-tab freeze permanent, because a runtime
will not re-grant FOCUSED to an app that submits nothing - and moved the wait off the present
thread so a block could not wedge the game. **That reasoning holds and must not be undone.** What
it missed: it treated "not blocked" as "not harmed", and the frame HANDOFF still paces the game
thread to the runtime's cadence.

The shape of the fix is therefore "keep submitting, stop WAITING": the game thread should never
block on the pace handoff while the session is not FOCUSED, even though the pace thread keeps
feeding the runtime so FOCUSED can be re-granted. Start at `pace_should_skip` and the handoff in
`src/core/vr/openxr_runtime.cpp` (`g_paceOffThread`, `handoffs`, `pace_thread_start`).

Acceptance: alt-tab away from a running VR session and back; the game must keep its frame rate
throughout (heartbeat `calls/s` unchanged, `xr=` shows the state change), and FOCUSED must return
by itself. Reproduces flat with a runtime present and no headset - `xr=SYNCHRONIZED/neverFocused`
in the heartbeat is the tell, and it is a 30-second check.

### 2. THE RIG EATS THE VIEW (the user's next felt problem)

"The helmet takes a lot of the screen and there are black bars at the bottom. I want the visual
space to be the whole screen/FOV."

Two separate things, do not conflate them:

- **Rig size.** The fg rig's apparent size is coupled to the FOV value, not only to its lens: at
  fg 60 the helmet is off-screen, at fg 100 it fills the view, while the world lens is unchanged
  in both. So the foreground EYE moves with the fov (BS1 measured the same coupling and called it
  the zoom-pull). Find what the fg view's eye offset is derived from - the pawn/PC floats near
  +0x694 are the obvious sweep, and `dumpframe` + the fg cluster's transform rows measure it
  directly. The manual slider (`fgfov <deg>` / the overlay) is the calibration lane: ask the user
  for the value where the helmet looks right, and see whether it is a constant or aspect-dependent.
- **Black bars at the bottom.** Check `[hud] letterbox ON (engine cinematic bars)` in the log
  first - that classifier fires on BS2 and there is a `vrcine bars hide|show` lever (BS1 uses it).
  If it is not that, it is the engine's own letterbox: BS2 renders 2048x2048 as 2048x1421, and at
  16:9 the viewport should fill - so bars at 16:9 are a different mechanism and want a `vp=`
  reading from a dump plus a screenshot with `-Bands`.

### 3. Owed, cheap, unchanged

- **Pitch servo sign** in-headset (`enginePitch=` must MOVE while looking up/down; `vrinput
  pitchservo invert`). Blocked only by the pacing bug making headset time expensive.
- **Aspect ladder, 2 rungs** (user's call). The world law no longer needs it; its remaining value
  is finding a squarer usable resolution. Each rung is a relaunch + a save load.
- **Re-judge world scale and IPD** now the lenses match - both are overlay sliders, so no typing.
  Worldscale 100 was accepted in session 26, which predates knowing the lens was wrong.

### 4. Do NOT do yet

No per-hand or per-weapon trims until item 2 is settled - a trim fitted against a rig whose size is
still moving is not portable. Same mistake BS1 made.

## Previous state (2026-07-31, session 32 - BS2: RESOLUTION LANE SHIPS, AND BS1'S SQUARE-BACKBUFFER POLICY IS DEAD ON BS2 - branch s32-b2r-resolution-and-lens)

All BS2 work. **Three BS1 assumptions died**, each caught by an acceptance test rather than by
reading code, and the second one changes the plan for BS2's VR configuration.

### 0. Step 0 - the frozen engine pitch is FIXED (sign check still owed in-headset)

Confirmed open by reading the code, not assumed: b2r armed the shared core pitch kill via
`publish_vr_gameplay` and never published a pitch error, so BS2's engine-side view pitch was frozen
and the DRILL aimed with it - BS1's session-30 wrench bug, same shared code. `publish_pitch_error`
now fires immediately before the `rot->pitch` overwrite.

Two corrections to the session brief, both verified in the tree:

- **The brief's confirmation method could not have worked.** It said the `[b2r] camera:` heartbeat
  prints `rot` before the overwrite; it does not - the heartbeat runs LAST by design and reports
  the FINAL camera, so its pitch is the head's, never the engine's. The heartbeat now carries
  `enginePitch=` (sampled pre-overwrite) and `pitchErr=`.
- **BS2 had no `vrinput` command at all.** `bvr::input::handle_command` is core and BS1 dispatches
  to it; b2r never did, so the synthetic gamepad, `pitchservo` AND the whole core swing-gesture
  flat suite were unreachable on BS2. One line fixed it - and it is a CLASS of bug worth auditing
  (core growing a feature does not give an adapter access to it).

**Still owed:** the servo's SIGN, which needs the headset - flat, with `drive=0`, `pitchErr` is 0
by construction. Checklist in `docs/bioshock2/TESTING.md`.

### 1. Step 1 - `vrres` ships on BS2, and the file is NOT the one BS1 uses

`vrres <w>x<h>` writes **`%APPDATA%\BioshockHD\Bioshock2\Shared.ini` `[SharedOptions]
ViewportX/Y`**. BS1's entire lane writes `[WinDrv.WindowsClient]`'s viewport keys - **BS2 has those
keys and IGNORES them.** Proven by changing one variable at a time across three relaunches; the
decisive row is SP-ini 1920x1080 + Shared 2048x2048 -> renders 2048x2048.

**The lesson, and it now has two instances so it is a standing rule:** the BS1-shaped port wrote
its four keys, re-read them, logged `verified`, and the engine rendered 1920x1080 anyway. **A
verified write is not an honoured one** - same failure class as trusting `-> HANDLED` from the Exec
seam. Acceptance must be a downstream EFFECT (the backbuffer at first Present), never the write's
own confirmation.

Verified end to end through the shipped path: write -> relaunch -> `first Present: backbuffer
2048x2048`; both files backed up once to `.bvr-bak-res`; diff shows ONLY the intended keys, with
all four decoy sections byte-unchanged. **BS2 has FIVE viewport-carrying sections, not BS1's four**
(it adds `[PS3Drv.PS3Client]`), so section-scoping matters more here, not less. Also answered, a
question BS1 left open: **a clean quit (`WM_CLOSE`) does not clobber the write** - byte-identical,
hash unchanged. Scope: menu-quit path only.

### 2. THE HEADLINE: BS2 does NOT render non-16:9. Do not port the square-backbuffer policy.

BS1's settled policy is that a square backbuffer matches the roughly-square headset eye, which is
what made its FOV write unnecessary. **On BS2 the premise fails.** At 2048x2048:

- the scene renders into a **2048x1421 viewport** - the bottom ~30% of the backbuffer is BLACK
  (screenshot-confirmed, HUD drawn over the band);
- the world horizontal **collapses from 100 to 67.7 deg**;
- the ray block's two vertical encodings stop agreeing - the frustum is no longer a consistent
  perspective at all.

At 1920x1080 all three are clean. So on BS2 today the resolution lane is a SHARPNESS lever at 16:9,
not an aspect-matching one. **Next session's first job** is to bisect which aspects BS2 will render
full-height; the squarest one that does is the best VR configuration available.

### 3. Step 2 - the lens verdict: TWO lenses, differing AT 16:9

Unlike BS1, whose split was two aspect CONVENTIONS that coincide exactly at 16:9 (so its symptoms
were aspect-gated), BS2 carries a MAGNITUDE difference that is present at 1920x1080:

| cluster | option 100 | option 130 | blocks | callstack head |
|---|---|---|---|---|
| **world** | tanH 1.1918 (100.0 deg) | tanH 2.1445 | 229 | `...AEC7B4...` |
| **second** | tanH **0.5774** (60.0 deg) | **0.5774 unchanged** | 19 | `...AECACF...` |

`0.5774 = tan(30)` to five decimals and it **ignores the FOV option** while the world lens tracks
it. One projection layer carries one fov claim, so whichever layer the claim does not match is
displayed at `k = 2.06x` angular gain at option 100 - rising to **3.99x at option 130**.

**This is the leading explanation for the user's stereo viewmodel report** ("wrong depth",
"moves/slides with the head", maybe "wrong size") and it fits where BS1's mechanism could not: it
is NOT aspect-gated, so it is present at 16:9, which is where the user tested. It also predicts
that raising FOV makes it worse.

**NOT proven: that the 60-deg cluster IS the viewmodel.** Evidence is circumstantial (distinct
pass, small draw count, FOV-independent). BS1's rule applies - identify it by making it MOVE, never
by draw counts. The test is one dump: holster/switch the weapon and see whether the 19-block
cluster changes.

### 4. The world FOV law is only PARTIALLY derived, deliberately

`tanH = tan(option/2)` measured exactly at 1920x1080 (and tracks the option 100 -> 130). But the
two candidate laws COINCIDE at 16:9 - that is the trap that cost BS1 two sessions - and BS2 gives
no clean second aspect because every non-16:9 render measured is degenerate. The decoder now prints
both laws and **both PASS at 16:9**, which is the tool reporting the ambiguity honestly rather than
manufacturing a verdict. Not settled; the section-2 bisection is what would settle it.

### 5. Tooling: both decoders parameterised, BS2's cb0 offset derived

BS2's ray block is at **cb0 float 16** (BS1's is 12) - same shape, four floats later. The offset is
now a per-game constant in `bioshock2r/patterns.h`, published to core via
`bvr::hud::set_ray_block_offset`. Core also widened its cb0 copy from **80 to 1344 bytes** (a block
past float 19 was previously not even COPIED, so no offset could have rescued it) and gained a hunt
that runs only after the configured offset fails and LOGS what it adopts - it found float 16 live,
independently of the offline derivation.

`tools/decode-framedump.ps1` gained `-RayOffset`, `-Aspect` (killing a hardcoded `9/16`),
`-FgBakeRvas`, `-ScanLayout` and `-Diff`. **`-Diff` is the instrument that cracked it**: comparing
two dumps at different FOV options assumes nothing about layout, which is what was needed after
`-ScanLayout` came back empty on the square dump. That empty result was itself a false lead - the
square-aspect degeneracy, not a different layout shape.

**BS1 no-regression PASSED**: `WORLD tanH=1.191754 tanV=1.191754` at 2048x2048, bit-identical to
the banked session-28 value, with the offset hunt never firing. The last hardcoded aspect constant
in b2r (`fovaudit`'s `9/16` flat fallback) is gone.

## Previous state (2026-07-31, session 31 - SWING-TO-ATTACK IS DONE AND ACCEPTED IN-HEADSET - branch s31-b1r-swing-to-attack)

**Release still held. v0.5.0 stays untagged.** Session 30's three items are unchanged (one fixed
and accepted, one fixed with the cause identified, one untested). This session adds one feature on
top of them and nothing else moved.

### 0. THE FEATURE: swinging the wrench now swings the wrench

The user's ask, straight from their own play-test: *"I tested it by pressing the trigger at the
right time and it felt amazing. So let's try to replicate it where it gets triggered when the user
swings the wrench."* This was already ROADMAP line 539, written from that same play-test.

A fast right-hand motion composes a full RT pulse while the wrench is equipped. **In addition to
the trigger, not instead of it** (user's call) - the trigger path is untouched, so there is nothing
to roll back if the gesture disappoints. Any fast motion counts (speed threshold only; no direction
or wind-up requirement) and it fires on the RISING EDGE of the threshold, so the game's own wind-up
animation plays while your arm is still travelling.

**Note what this is NOT: aiming.** Session 30 measured that melee reaches neither fire-start seam -
damage is a Havok phantom aimed by the engine's own view - so the gesture changes WHEN the attack
happens, never WHERE it lands. A sideways swing while you look forward still hits forward, exactly
as the trigger already did.

New module `src/core/input/swing.{h,cpp}`. It is in `core/input/`, NOT in the XR layer where the
poses live, and that is the design decision that mattered: `openxr_input.cpp` compiles only under
`BVR_WITH_OPENXR` and `input_sync` never runs flat, so a detector there could not have been tested
without a headset. Here the whole decision core is reachable from `vrinput swing sim`, and every
threshold, gate, latch and cooldown was verified flat before the feature ever saw a headset. The XR
layer keeps one line: publish the right grip pose + head pose per frame, read through
`input_get_hand_pose` so the session-20 recorder's sim overlay drives the gesture too.

### 0b. What flat actually proved (all measured 2026-07-31, no headset)

| check | result |
|---|---|
| **A 120 ms RT pulse fires the weapon** | two `test trig r 255 120` took the ammo 6 -> 4. The session-10 "first pull only switches hands" caveat is about which hand is RAISED, not the first pulse |
| **The pulse reaches the engine's fire path** | all 5 `[swing] FIRE` lines followed 8-11 ms later by `[aim] watch weapon ... rt=255` |
| **Gate closed with a gun equipped** | 3 simulated swings, `fires 0`, ammo unchanged. This is the safety property - RT with a gun in hand is a SHOT |
| **Cooldown** | cooldown 600 + 3 swings 400 ms apart -> FIRE, `BLOCKED ... cooldown`, FIRE |
| **Re-arm hysteresis** | one swing = one fire, never two, across every run |
| **Weapon wheel** | RB held across 2 swings -> both `BLOCKED ... a grip is held` |
| **Off switch** | `swing off` -> fires unchanged |
| **vrpreset.ini round-trip** | 7 keys saved; distinctive values came back after a relaunch |

Two defects were found and fixed by the flat run itself: the BLOCKED line logged once per SAMPLE
(106 lines for one simulated swing - now latched to one per swing), and the cooldown was untestable
until `sim` grew a repetition count (one hump crosses the threshold once, and the command seam
polls at 1 Hz, so no two commands can land inside a 300 ms cooldown).

### 0c. IN-HEADSET VERDICT: **"I tested it and it's perfect."**

The one thing flat could not answer is answered. The user's call on the tuning, applied as the
shipped defaults and written into their vrpreset.ini:

- **ON by default**, and **fire threshold 3.6 m/s** - replacing the 2.2 m/s guess that shipped to
  that run. 3.6 sits well clear of a walk, a body turn or a reach, which is why ordinary play does
  not produce stray swings.
- Everything else stayed where it was: re-arm 1.0 m/s, cooldown 300 ms, pulse 120 ms, **delay 0**.
  The rising-edge fire needed no delay - the game's own wind-up animation lands the hit where the
  arm is going, which was the whole bet.

Nothing further was asked for. `vrinput swing delay <ms>` remains if the contact point ever wants
moving, and `vrinput swing off` still disables it instantly.

The flat + in-headset procedure is in `docs/bioshock1/TESTING.md` under "Wrench swing-to-attack".

## Previous state (2026-07-30, session 30 - THE WRENCH IS FIXED AND ACCEPTED IN-HEADSET, the bar-colour regression is fixed, effects still open - branch s30-b1r-wrench-and-effects)

**Release still held. v0.5.0 stays untagged.** The game-breaking item is CLOSED. Of the three,
one is fixed and accepted, one is partly fixed with the real cause now identified, one is untested.

### 0. THE WRENCH: FIXED. The engine's own view pitch was frozen at -89 degrees.

**In-headset verdict: "it's working and I was able to hit him consistently."**

Two individually reasonable things met. **Pitch kill** (session 19) zeroes the composed
right-stick Y so the stick cannot fight the HMD - but zeroing an INPUT does not set a value, it
means the engine's own view pitch can never change again. And **the camera write is asymmetric**:
`rot->yaw` is written RELATIVE (the engine's own yaw plus a head residual, so it stays real) while
`rot->pitch` is written ABSOLUTE from the head, discarding the engine's value unread. Nothing
corrects it, nothing notices, and the rendered view is the head's either way.

So the engine's pitch parked at -88.9 degrees - straight down - and stayed there for fifty
seconds of the heartbeat while yaw moved freely. Melee aims with that number. The user confirmed
it visually with `vrhands off`, which returns the viewmodel to engine placement: *"this revealed
that the hands were pointing downwards ... I saw the hits hitting the floor."*

Every part of the report follows: walls hit (approached level), fights missed (frozen steeply
down), the opening rocks other players reported miss with no combat at all (rocks are on the
floor), and guns were fine because we substitute the whole fire ray at a seam melee lacks.

**Fixed by SERVOING instead of zeroing.** The game layer publishes `head pitch - engine pitch`
once per CalcView, before the overwrite, and the bridge feeds a proportional stick value in place
of the hard zero. The game steers its own pitch through its own input path: **no engine memory is
written**, so none of the session-29 world-change hazards apply; it inherits the game's own
clamps; it is invisible because the rendered pitch is the head's regardless; and a stale publisher
fails open to `ry = 0`, the old behaviour. `vrinput pitchservo on|off|invert|status`.

**Known residual, measured:** the engine pitch went from -88.9 to -6.6 deg, then stalls at
`err=4.3 deg stick=3876`, because near convergence the proportional value falls under the GAME's
own stick deadzone. Roughly 4-8 degrees, not zero. Inside melee tolerance (consistent hits) but
not perfect. Closing it needs a minimum stick magnitude clearing the game's deadzone, at the risk
of a limit cycle - measure the game's deadzone first.

### 0b. Two hypotheses died on the way, both of them ours

- **Soft lock-on** was the user's leading theory and their own test killed it: setting the radius
  ABSURDLY high (`exece set GamepadPlayerInput SoftLockOnRadius 5000`) felt no different from 0,
  so that write never reaches the live object. **Rule that comes out of it: `-> HANDLED` proves
  only that `Exec` recognised the command.** `console_exec`'s output-device stub suppresses all
  engine output, so a `set` naming a wrong class or property logs identically to one that works.
- **The aim substitution**, which was session 29's leading hypothesis - see section 1.

### 1. The aim-seam lane is CLOSED by measurement (this is what saved the session)

Session 29's leading hypothesis - the wrench is an `AWeapon`, our origin substitution moves its
short melee trace - is **refuted by direct measurement in-headset with the wrench equipped**:

| seam | calls across the whole wrench period | classes seen |
|---|---|---|
| `AWeapon::GetPerfectFireStart` | **0** (counter sat at 4 throughout, all four from a Shotgun test 15 min earlier) | `Shotgun` only |
| `UAttackAbility::GetPerfectFireStart` | 6 | `ElectricBoltThreeAbility` only |

So `vraim seam weapon off`, `vraim origin off` and the melee carve-out in `substitute()` that
was designed and about to be built **cannot affect the wrench at all**. `ENGINE_NOTES`'s
session-10 note - melee damage is a Havok phantom (`Wrench.CreateCollisionPhantom`), no aim seam
exists for it - is now CONFIRMED rather than asserted. Two other documents in the tree had
contradicted it; they are corrected.

Measurement mode that made this cheap and safe: **`vraim probe on` + `vraim off`** installs both
hooks with `ray_for()` refusing, so the diagnostic run cannot change the behaviour it measures.
It held through a whole live play session. One trap: **VR PRESET 1 re-arms `aim on` + `origin
on`** (`camera.cpp:862-864`), which is what turned substitution back on mid-run here.

The candidate list that followed from it is now resolved: candidate 2, "positional head/pawn
decoupling", was the right neighbourhood and the wrong axis - it is PITCH, and the mechanism is
ours rather than the engine's. Candidate 1 (`vrhands off`) turned out to be the diagnostic that
made the frozen pitch visible rather than a cause. Candidate 3 (lock-on) is dead, see 0b.

### 2. Two things measured for free on the way, both worth keeping

- **Hand attribution in real play is ALWAYS the seam default, never learned.** Five plasmid casts
  out of five: `hand=L src=fallback lt=0 rt=0`. The anim notify fires after the player releases
  the trigger, so `trigger_held()` never sees evidence. Not a live bug (Left is correct for
  plasmids) but the object-learning map is dead weight on that seam, and anything else arriving
  there would take the left trims - pitch -7.5, yaw **+37 deg**.
- **We displace the fire origin by 40-47 cm** on every substituted call (measured, `worldScale`
  100 so UU == cm). Invisible at range, decisive at contact range. That is exactly why the wrench
  theory was plausible.

### 3. EFFECTS: routing is clean, and a REAL shipped HUD regression was found instead

The instrument built to test the routing hypothesis **refuted it**: with the redirect armed the
effect fills read `effect=127010/0` (passes/stranded). They are never stranded and genuinely
reach the frame. Trustworthy because both checks passed - a one-shot device read
(`bound rtv0 resource=92DB3E64, our capture RT=92DB3E64 - CORRECT`) and a positive control
(`vrcine postfx size` made the same counter read 36140).

What the same run DID find, and it was shipping in v0.5.0: **the post-FX size rule is degenerate
at a square render target.** At 2048x2048 the backbuffer IS 2048x2048, so the game's own UI
atlases match `srv0 dims == target dims`. Measured `postFxRejected=1604161` against `postFx=2`
genuine - about **30 gameswf HUD draws per interval** taking the in-frame exit. Worse than a
leak: under the old rule 43% of them were stranded onto the panel and 57% reached the eye image,
so HUD elements were routed **non-deterministically by draw order**. Fixed structurally - a
post-FX source is something the engine RENDERED (`BIND_RENDER_TARGET`), a UI atlas never is -
which is stronger at every resolution, not just square ones.

### 3b. AND THE "EFFECT" DRAW WAS THE HEALTH AND EVE BARS - a shipped regression, now fixed

**In-headset verdict: "the health bar is filled and colored which is perfect."**

The user reported the health bar had lost its colour. It is the same draw. `effectsInFrame`
advances by **exactly 2 per interval, every interval, with nothing on screen** - a fact measured
earlier this same session and written up as "the fill is always drawn and usually transparent".
It is two bars. Health and EVE bar COLOUR fills are textureless 5-vertex gameswf quads, identical
to the effect fill by every test this classifier can apply, so session 29 had been sending the bar
fills into the eye image while their frames stayed on the panel. A/B'd both ways in-headset:
untick restores the colour, re-tick removes it.

**And this explains the effects item too, which is the important part.** The user's description of
the water tint - *"either the size of the HUD or the size of the old resolution"* - identifies the
real error: **these draws are authored in gameswf STAGE space.** Routing one in-frame cannot make
it cover the eye; it makes it stage-sized in the middle of the view. Session 29's fix could never
have worked. Covering the view needs different GEOMETRY, not a different render target.

Default is therefore back to the panel (pre-session-29 behaviour, v0.4.1 behaviour). Nothing
accepted in session 29 is affected: bars are matched by vertex count and skipped above this test,
subtitles are textured and never reach it, menus take the screen-only path, and the alcohol blur
is a textured engine post effect on the branch below. The alcohol blur was A/B'd in-headset
against the new post-FX rule and is unaffected.

**Still open: making effects actually cover the view.** Now correctly scoped - it is a geometry
problem, not a routing one. `img-diff.ps1 -Grid/-Bands` is built and self-tested for measuring
coverage but the screenshots were never taken. Also still unanswered, and it decides whether the
remaining cause is the fill or the projection claim: **in the headset, does the effect stop before
the scene picture stops, or do they end at the same edge?**

### 4. HANDS: untested this session

`vrbones status` now prints the drive residue on demand (`cacheAge`, `wasCollapsed`,
`collapsedHand`, `reapplies`, `cineHold`, `cineDrive`) instead of only at a cine edge, which is
what the regression check needs. The checklist itself was not run.

### 4b. THE CLASSIFIER HARDENING'S OWN FALLOUT - all found by the user PLAYING, not by our checks

Three regressions came out of part 1's post-FX change, in the order they were found. All are
fixed; one is fixed by a workaround rather than a diagnosis. Full write-up in ENGINE_NOTES
session 30 part 3.

1. **Health/EVE bar colour** - see 3b. Fixed, accepted.
2. **A floating screen in cutscenes** showing the scene plus subtitles. **Fixed by a WORKAROUND
   and the code says so:** while `cinematic_hold()` is true the post-FX rule falls back to
   size-only, and the bind-flag rule returns when the scene releases (`vrcine postfx cine
   on|off`, default on). Defensible scoping - the bind rule exists to keep HUD art out of the eye
   image and a cutscene has none - and verified in-headset with the exception logging inside the
   measured window (bar draw ON 00:06:32.211, exception 00:06:45.004, off 00:08:22.216). **But
   the root cause is NOT known.** The missing measurement is a frame dump taken INSIDE the scene;
   the one attempt landed 1.1 s early and captured ordinary gameplay. **Next instrument: auto-fire
   `frame_inspector::arm(2,2)` on the `bar_draw_active()` rising edge** - session 29 did exactly
   this, and `arm()` has only two call sites today so it is additive.
3. **A crash on load, twice, in Release.** Mine. The stranded-pass restore's first version
   `AddRef`'d the game's RTV/DSV; `hud_capture.cpp` already stated the rule it broke (`g_curRt` is
   stored "identity only from here on") and the game holds its own reference for as long as its
   own binding is live, so there was nothing to guard against. Taking references from a detour
   that runs ~18M times a session while a level load recreates targets is the larger hazard. Same
   shape as session 29's `write_n` lesson: **a guard that creates a worse failure than the one it
   prevents.** Two of those in two sessions.

The stranded-pass restore itself is shipped and measured working (`stranded=0` in every bucket),
but it did NOT fix the cutscene screen - that draw is in the REJECTED population, not the
stranded one. Reading the wrong half of that one status line is what cost the second wrong
diagnosis; a third came from a "controlled" test whose lever gated only the consumer of the
change, not the change. **A counter is not evidence until you know which population it counts.**

### 5. What broke, and the rule that comes out of it

`vraim scanimpl 226050 1` crashed the game with `Run-Time Check Failure #0 - ESP was not properly
saved`. **`scanimpl`'s arg count must equal the target's `ret imm / 4`**, and both
`InitiateDamage` implementations are `ret 8`, so the correct value is **2**. Verified for all
four fire-flow implementations by capstone; the table is in ENGINE_NOTES session 30. Two
after-effects worth knowing: RTC writes **no crash dump** (it is a Debug compiler check, not an
SEH fault), and force-killing the game while that modal dialog is up left the display mode
unrestored. Press Abort on the dialog rather than `Stop-Process -Force`.

## Previous state (2026-07-30, session 29 - STAGE 3 ACCEPTED IN-HEADSET, v0.5.0 PACKAGED, awaiting the user's final go - branch s29-b1r-cinematics-and-aim-dot)

**IN-HEADSET VERDICT (user, Quest 3 / VDXR): "bars are gone, hands and head are correct now and
the dot is perfect. Everything is very good."** Subtitles were reported wrong in the same run and
are now fixed and confirmed good.

**v0.5.0 is BUILT AS RELEASE AND PACKAGED** (`dist/bioshock-vr-v0.5.0.zip`, sha256
`1BAB7C5BEFA961CD135722099990024FA110CC6D4E7E30F2E7306240DA06838B`). **NOT merged to main and
NOT tagged - the user is testing first and will give the final go.** Do not merge or publish
without it.

**THE INSTALLED GAME DLL IS OLDER THAN THE FIX.** The user asked not to restart mid-test, so the
game is still running the 17:54 build, which contains the save-load hang in 0d-bis. The fix is
built and packaged but NOT installed. **First action next session: `tools/install.ps1 -Release`
and relaunch**, then re-test a save load, which is the one thing the fix changes and the one
thing not yet verified.

### 0a. THE BUG THAT ONLY THE HEADSET COULD SHOW: the HUD redirect was eating the bar draw

The first in-headset run of stage 3 came back "bars still present, hands still wrong" while every
flat test passed. One root cause for both, and it is a genuine trap worth remembering:

**With an XR session live, `hud::armed()` is true, so gameswf draws are REDIRECTED to the
offscreen HUD RT. The bars ARE a gameswf draw - so in-headset they never reach the backbuffer,
which is exactly the surface `letterbox_sample` reads.** The watch therefore never fired,
`cinematic_hold()` never became true, the suppression branch and every drive gate hanging off it
were never entered, and the bars rode the HUD panel into the headset. Flat, the redirect is
unarmed, the bars land in the backbuffer, and detection works every single time.

**Why it survived since session 22:** round 4 moved the sampler from the present TAIL to the HEAD
to kill the sampler-feedback trap. Correct fix - but the tail sample had been reading our own HUD
composite, which was the ONLY path by which redirected bars reached the sampled surface with a
session live. It silently broke in-headset detection at that moment, and every test of the
detector since has been flat. The user's "before, the scene was locking the head" dates the
regression exactly.

**Fixed by removing the dependency entirely:** the bar draw is now identified where it is issued,
BEFORE the redirect decision, by its measured fingerprint - a textureless gameswf draw on the HUD
target with the WidescreenBars vertex count (29). Textureless alone is NOT safe: the log
immediately proved it (`textureless gameswf draw, 5 verts (bars are 29) - NOT treated as bars`),
and treating all of them as bars is what blacked out the scene in session 22 round 4. The count
is retunable live (`vrcine bars verts <n>`) and every other textureless count is logged once, so
a wrong value shows up as data rather than a silent miss.

### 0b. The sticky bone state was REAL - measured, not argued

Flat could not test it (no XR session means no bone drive, so `hiddenHand=-1 refValid=0` always).
In-headset the cine-edge instrument caught it on the first cutscene frame:

```
[b1r] cine edge ENTER (barDraw=1 letterbox=0 cineQuad=0) | drives: vrDriving=0 strict=1
      aimArmed=1 | bones: hiddenHand=0 cacheAge=16ms refValid=1
[bones] released to the engine (cinematic started): hidden hand 0 restored, ...
```

`hiddenHand=0` = the left cluster collapsed, `cacheAge=16ms` = `reapply()` actively repainting,
`refValid=1` = the frozen reference held. Exactly the state predicted from reading the code, and
`release()` undid it on the same frame. Note also `barDraw=1 letterbox=0`: the two cinematic
sources disagreeing in precisely the way section 0a predicts.

### 0c. A second bug the instrument found, from the user toggling modes mid-scene

Switching drive mode to `off` mid-cutscene resumes the drive (collapsing the inactive hand), and
switching back gates the only code that can restore it - `restore_hidden()` lives inside
`drive()`, and `release()` only fired on the cinematic ENTER edge. Measured: the exit line read
`hiddenHand=0 cacheAge=32578ms`, and 32.5 s before that exit is exactly when the gate re-closed.
Fixed by releasing WHERE the suppression happens (the hands gate) rather than on an edge;
`release()` is idempotent so it self-limits to one real pass.

### 0d. Subtitles: the round-4 all-or-nothing rule had to go

Session 22 round 4 sent the WHOLE flash layer in-frame during a letterbox so the frame could not
differ from flat. That rule existed only because the bars could not be identified. With the bars
skipped precisely, sending the rest in-frame actively hurts: subtitles rendered into the eye
images are captured per eye, and under SequentialReentry the two eyes come from DIFFERENT game
frames, so two text states superimpose. User report: "impossible to read". They now default to
the head-locked panel (one image in both eyes) and the user confirmed them good. `vrcine subs
frame` / the overlay checkbox restores the old behaviour.

### 0d-bis. A SAVE-LOAD HANG, found and fixed in-headset - and it is the important lesson

A save load hung the game (`responding=False`, log dead). Root cause was mine, from earlier the
same session: `bones::release()` was being called from the CAMERA-side cine-edge block, which
sits ABOVE `hands::on_calcview` - and that is what detects a world change and calls
`bones::on_world_change()`. So the release wrote ~1.8 KB through the PREVIOUS level's skeleton,
already freed and already reused by the loading level.

**`write_n` is SEH-guarded, which is exactly why it hung instead of crashing**: the pages were
still mapped, just owned by something else, so the guard converted a fault into silent heap
corruption. A safety net that makes a failure quieter is not a safety net.

**The rule was already written in the tree** - `hands.cpp`'s world-change block says "recycled
heap addresses must never be written to ... restore would write into a stranger". This was a new
call site bypassing a documented invariant. Full write-up in ENGINE_NOTES session 29 section 9.

Fixed three overlapping ways (camera block no longer writes bones; `release()` interlocks on a
live skeleton from ANY call site; `on_world_change()` clears the hoisted sleeve latch). **The
in-headset-accepted cutscene behaviour is unaffected - only the call site moved.**

### 0e. Known characteristic, user-accepted: `authored+look` still pans with the director

The head delta is added ON TOP of the authored rotation, so when the scene pans, the view pans
too. User's call: *"everything was perfect except the head movement during authored + headlock
but I think it's fine and the user can choose for now."* The alternative - head fully owns
rotation, the scene owns only position - is a small change and is NOT built. Recorded as
available, not needed.

### 0. THE HEADLINE: the bars are a flash sprite, and that retracts session 22

The whole letterbox investigation was chasing the wrong mechanism for three in-headset rounds.
**The bars are a gameswf DRAW painted over a FULL-FRAME tonemap** - not unpainted clear behind a
vertically shrunken quad, which is what session 22 round 2 recorded. Two independent
measurements, and neither needed a headset:

- **The Nexus "Fullscreen Cutscenes" mod is a one-byte SWF edit.** Its `HUDPC.swf` is exactly
  one byte larger than stock (12,322,709 vs 12,322,708). Byte-diffed: the SWF header length
  field +1, and a single edited tag at ~0x0B750DE - a `PlaceObject2` (code 26, body 25 -> 26)
  placing **character 292 at depth 256 named `WidescreenBars`**, its matrix rewritten from
  translate-only to `HasScale=1, NScaleBits=0` (ScaleX = ScaleY = **0**). It scales the sprite
  to nothing. A sprite is a draw.
- **A framedump inside the letterbox shows it.** Auto-fired on the `letterbox ON` transition
  during the Electro Bolt sequence at 2048x2048 (bars 313/350 px). Both intervals identical:
  `ClearRTV (0,0,0,1)` -> tonemap `Draw a=6` over the **full 2048x2048 viewport** -> a
  **textureless `Draw a=29`** -> textured 5-vertex flash quads, all carrying the gameswf flush
  RVA `0x7B8EB5`. Exactly one textureless draw. That is the bars.

**Why session 22 got it wrong, and it is a reusable lesson:** the frame dump captures no vertex
buffers, so "shrunken GEOMETRY" was never a measurement - it was an inference from a black clear
plus black bars with no draw identified between them. The tonemap's `cb0` is `[1.0, 0, 0, ...]`
with no scale term, so cb0 could not have supported it either.

**Consequences.** `blit::stretch_band` / `vrcine unsqueeze` was undoing a squeeze that does not
exist; on real content it could only ever have cropped picture and distorted the aspect. It is
**deleted, not defaulted off**. Round 3's "texture-less fills = bars" fingerprint, retracted in
round 4, was right about what the bars are - round 4's regression came from re-rendering those
fills with raw flash blend states, a different operation from not issuing them.

### 0b. And the three failed in-headset rounds were confounded TWICE

Do not read them as evidence about the mechanism. (i) All three ran at **1920x1080**, where the
layer claim is `halfV = atan(tan(halfH)*9/16)` - at the measured 104 deg horizontal that is a
**71.6 deg vertical claim inside a ~96 deg Quest 3 eye, i.e. ~12 deg of permanent black band top
and bottom, cutscene or not**. That is the same ambient banding the user was raising FOV to chase
in session 28. (ii) The only evidence the stretch ever ran was `xr: letterbox unsqueeze live`, a
**process-lifetime one-shot** - and the boot attract letterboxes too, so it could fire at boot
and prove nothing. **Rule worth keeping: a one-shot log establishes that a path ran once, never
that it covered an episode.**

### 1. What shipped

- **Bar suppression.** `hud::on_draw`'s verdict widened from `RTV*|nullptr` to
  `PassThrough|Redirect|Skip`; `DrawDetour` gained the mod's only draw-dropping early return.
  Skipping is safe where redirecting is not - it changes no device state, so the gameswf batch's
  state machine is untouched. Discriminator while a cinematic holds: a gameswf draw on the HUD
  target with **no texture bound**. `vrcine bars hide|show`, default hide, overlay + ini.
- **The cinematic signal moved off pixels, and had to.** Suppressing the bars blinds the pixel
  watch that detects them - key the gate on black pixels and it flaps every other interval. The
  **bar draw is now the primary signal**; `hud::cinematic_hold()` = draw OR pixel watch. The
  watch bootstraps the hold (~6 presents: async map + 5-sample hysteresis), the draw sustains
  it. The two stay **independent** and `vrcine status` prints both, so agreement is evidence.
  Every consumer that must hold for a whole cutscene now calls `cinematic_hold()`, never
  `letterbox()`.
- **`vrcine drive off|authored|authored+look`** (default authored). `off` means no cinematic
  special-casing at all (the pre-session-22 behaviour, kept as a real A/B) rather than being a
  near-duplicate of `authored`.
- **`bones::release()`** on the cinematic entry edge, plus `[b1r] cine edge` telemetry.
- **The aim dot**, `vraim dot on|off` (default off) + distance/size sliders, persisted.

### 2. The roadmap item about the drives was wrong, and the correction matters

It said the hands/aim/laser drives run ungated through cutscenes. They do not: `driveHead` is
false under a letterbox, so `vrDriving` is false and all three consumers already bail on it.
**But that is a side effect of the head gate, not a contract** - and `authored+look` breaks it by
design, because it drives the head again. Hence explicit gates in `hands.cpp` and `aim.cpp`; the
one line in `aim.cpp` covers three things at once (fire-seam substitution via `ray_for`, the
per-weapon heap scans, and the laser publish).

So the real suspect for "controllable rig hands instead of authored" is **sticky state**:
`reapply()` repaints the cached pose for 100 ms *while clearing the dirty flag*, so it actively
suppresses the engine re-evaluation that would restore the authored animation; and
`restore_hidden()` only ever ran from inside `drive()`, so a collapsed inactive hand stays
collapsed. `bones::release()` undoes all of it. **This came from reading code, not measuring** -
see the honest limits in section 4.

### 3. `authored+look` = rotation delta, no positional term (user's call)

The head's rotation delta **since the shot began** is added on top of the authored rotation. It
cannot reuse the gameplay path: `camera.cpp` writes `rot->pitch`/`rot->roll` ABSOLUTELY from the
head, which would erase the authored choreography (session 22 measured authored roll walking
-7773..-8189 through the wake-up shot). The reference is captured at the cinematic edge and
dropped on both edges, so every shot opens framed exactly as authored. No positional offset at
all, so the camera can never be dollied into geometry, and the residual never reaches
`body::on_calcview` (which would silently rotate the pawn under the authored camera).

### 4. What is NOT confirmed - read this before trusting the above

- **The sticky-state diagnosis is unconfirmed.** The flat `cine edge` line read
  `vrDriving=0 ... hiddenHand=-1 cacheAge=0ms refValid=0` at the cutscene edge. That confirms the
  drives were suspended, but with **no XR session `vrDriving` is false always**, so it cannot
  distinguish "gated by the letterbox" from "gated by no headset" - and `hiddenHand=-1` merely
  means the bone drive never ran at all. Only a headset can test the release.
- **Bar suppression has not been seen working yet.** The code is in and the frame that proves the
  bars are a draw is captured, but the suppression run itself did not happen before the session
  paused. First thing to do next.
- **The aim dot has never been rendered.** No XR session flat means no quad layers at all.

### 5. Cinematic FOV, measured: the scene animates its own lens

During the sequence the WORLD lens **sweeps** `tanH 1.215944 -> 1.272701` (hfov 101.13 ->
103.68 deg) while the FG lens sits at `1.191754` (100.00 deg) - the scene dollies its own camera.
The option is 100, so it stays inside `fov_mismatch()`'s +-10% band, the mismatch never latches,
and `cineQuad=0` throughout: the session-28 claim-substitution branch correctly never fired.
Note for BS2: a cutscene is a place where two lenses legitimately differ, so `lenses=2` there is
not automatically the session-28 defect.

## Previous state (2026-07-30, session 28 - the warp is FIXED IN-HEADSET; the hands moving with the head was the same defect from the other side, now lens-matched - branch s27-b1r-stability-and-resolution)

**IN-HEADSET (user, Quest 3 / VDXR): the warping is FIXED.** Two things came back with it, and one
of them is the important one.

### 0. IN-HEADSET VERDICT: ACCEPTED - "without changing anything it's perfect, both the world and the gun/hand models"

**Both are geometrically correct at the same time, on the first try, with NO re-tune.** That last
part is a real finding, not just good news: the session-16 hand offsets in `vrpreset.ini` were
suspected of having absorbed some of the 1.78x lens error, and they had not. They were correct all
along and the fg lens was the only thing wrong - so the calibration work from sessions 13-16
stands unmodified, and the `vrfgfov legacy on` A/B was not needed.

### 1. "The hand and gun move when the headset moves" - the SAME defect, other side

Not a regression in the hands machinery. **One projection layer carries ONE fov claim for the whole
eye image, and the world and viewmodel were rendered through DIFFERENT frustums**, so only one
could be right at a time: the old bug claimed the fg lens (hands right, world warping); fixing the
world moved the same 1.78x error onto the hands. The only state where both are correct is MATCHED
lenses - which is the `(4/3)*(h/w)` fix that was measured last session and deliberately held back
so it could not confound the warp test. It is now required, and it landed.

Second, coupled cause: `bones.cpp` asserted that with the lens match armed "k collapses to 1".
True at 16:9 only - at a square backbuffer the real ratio was 1.7778, so the render lock's depth
constraint AND its head-split lateral cancel were mis-scaled, and that cancel is exactly the term
that stops the rig sliding under head motion. `world_ndc` had the same hardcoded `9/16` for the
world lens. Both now read the live aspect; `k` genuinely collapses to 1.

**Flat gate - the same instrument that found the original bug.** At 2048x2048 the dump went from
TWO tangent clusters to **ONE**, with the 576-byte foreground tier now inside the world cluster
(`tanH=1.1918 tanV=1.1918`, b0tiers `[320:57 576:39 832:36]`). `fovaudit` reports `lenses=1` and
`fg lens match ON (last written 115.6 deg, k=1.333333)`; the engine accepted the wider write
without clamping. `vrfgfov legacy on|off` restores/removes the split, verified both directions -
an instant in-headset A/B.

**Not confirmable flat, and not assumed:** the bone-solve half. No XR session means no controller
poses, so the hands drive never engages (`inst=0 writes=0 solves=0`) and `render_lock_delta` is
never entered. Its lens geometry is proven; the solve needs the headset.

**Carry:** the session-16 hand offsets in `vrpreset.ini` were tuned against the OLD 1.78x-narrow fg
lens at a square backbuffer, so some of those numbers may have been compensating for the lens
error. A re-tune may be wanted now the projection is honest.

Corroboration from the other repo, and it was already in our own notes: `docs/RESEARCH.md` records
BioVRDev's fg fov as `2*atan(tan(fov/2)*(4/3)/aspect)` - i.e. `(4/3)*(h/w)`, exactly this fix,
noted since session 20 and never acted on. Our `0.75` is that expression evaluated at 16:9.

### 1b. It is dynamic: the lens math follows any resolution, no relaunch needed for the math

Everything aspect-dependent reads the LIVE backbuffer every frame instead of caching at init:
`bvr::hud::backbuffer_dims` is published at the head of the present detour, the fg match constant
is recomputed per CalcView, `bones.cpp` reads it per solve, and the XR claim derives from the
rebuilt swapchain dims. One fragility was found and closed while checking this: the dims used to be
published only INSIDE the letterbox watch's RGBA8 format whitelist and staging-allocation success,
so a user on another backbuffer format would have silently fallen back to the 16:9 constants and got
the 1.78x viewmodel error back. Correct lens geometry must not depend on whether an unrelated
black-bar detector could allocate, so the dims read is now unconditional and first.

At 16:9 the corrected constant evaluates to exactly `0.75`, so 16:9 rendering is bit-identical to
everything that shipped before - which is what protects the sessions 13-16 calibration by
construction rather than by luck.

### 1c. Banked for BioShock 2 (user ask): `docs/bioshock2/ENGINE_NOTES.md`

A new section records the BS1 defect class as an ordered CHECKLIST for BS2 rather than a set of
constants to copy: does BS2 even have two lenses (session 25's native-fg finding says it may not),
how to identify which cluster is the foreground (toggle the fg write and see which one moves - not
by draw count or cb tier, both of which were shared on BS1), why two backbuffers are needed to
distinguish the two candidate world laws at all, that `src=live` is a provenance tag and not a
correctness proof, that the aspect-general fg constant is `(4/3)*(h/w)` but its `4/3` and `3/4` are
BS1's measured foreground spec and must be re-measured, and a grep list of the four places BS1 had
hardcoded aspect constants that were all silently correct at 16:9. The fixed watch is core and
game-agnostic, so on BS2 `fovaudit` reporting `lenses=2` off 16:9 is already the alarm - the
diagnosis BS1 lacked is in place before BS2 needs it.

### 2. Game FOV Write had to be turned off in-headset - and the corrected law says why

`apply_vr_preset` forced it ON, pushing option 130. That 130 came from the "129.5 circumscribing"
arithmetic, which solved `tan(option/2)*9/16*aspect = tan(H/2)` - **derived from the disproved
law**. Under the measured law `tanH = tan(option/2)` with no aspect term, so option 100 at a square
backbuffer already renders exactly 100x100 deg, and 130 over-widens by 30 deg (and drags the fg
lens to ~141 deg with it). The preset no longer arms the write; the global default was already
false, that line was the only thing turning it on. `gfov <deg>` still arms it manually.

### 3. Alt-tab: FIXED AND ACCEPTED IN-HEADSET ("the alt tab is working again")

Both of session 27's open bugs are now closed and confirmed on the real headset. Details of the
fix below; the thing to carry forward is that it took three attempts and only the one built on a
measurement worked - the first two (retiring the keepalive in session 26, the pair-hold ordering
earlier this session) were both derived from reading the code, and both were wrong about the cause
while being correct as hardening.

`xrWaitFrame` now runs on a dedicated pace thread. The present thread posts one request at a time
(keeping wait:begin at 1:1) and waits on the result with a deadline - 200 ms while FOCUSED so the
headset still paces the game, 20 ms otherwise so an unresponsive runtime cannot drag the flat window
down; on timeout it gives up and a later present consumes the same outstanding result.
`pace_should_skip` no longer skips merely for being unfocused, which is what lets FOCUSED be
re-granted. `teardown_session` refuses to destroy a session while a wait is in flight on it (a
use-after-free inside the runtime) and defers instead. `vrpace thread off` restores the exact
pre-session-28 inline behaviour; `vrpace status` reports handoffs and timeouts.

**Flat coverage is regression-only** - with no XR session flat the off-thread path never runs
(`handoffs 0`), so the fix itself needs the headset. Expect on alt-tab away and back:
`xr: FOCUSED again after N ms`, the headset image resuming by itself, and NO
`SUBMISSION IDLE` heartbeat persisting. This is the third attempt at this bug class and the first
one built on a measurement of it rather than a hypothesis.

**Why this design and not a smaller one:** the minimal fix (submit while unfocused, but skip when
waits get slow) needs a periodic probe wait, and an unbounded probe wait IS the session-26 keepalive
that hung. Moving the call off the present thread is the only shape that fixes alt-tab without
reinstating that hang class - and it retires the hang class permanently, because an unbounded block
can no longer reach the thread the game depends on.

### 3a. The measurement that named it (kept - it refuted my own first fix)

My pair-hold hypothesis was **wrong** - the log says `pairOpen=0`. The actual state:

```
xr: session state VISIBLE
xr: SUBMISSION IDLE (reason=pace guard: session not FOCUSED | state=VISIBLE everFocused=1
                     pairOpen=0 skips=5772 frames=5022)
```

and **no `FOCUSED` line ever follows** until `session teardown (disabled in overlay)`. So VDXR drops
the session to VISIBLE on alt-tab and never re-grants FOCUSED, because **we stop submitting frames
while unfocused and the runtime will not promote an app that submits nothing** - a circular wait.
This directly contradicts the session-26 comment that "recovery is event-driven, not something an
app earns by submitting frames": measured, on VDXR, it IS earned. Note also **zero
`xrWaitFrame blocked` lines in the entire session**, so the guard skipped 5000-9000 presents
without a single slow wait to justify it - it keys on session state when the thing it must avoid is
a slow wait. Fix shape (user deferred it: hands first): keep submitting while VISIBLE but run
`xrWaitFrame` on a dedicated thread so the present thread waits with a timeout and can never be
wedged - which also permanently retires the session-26 hang class. Nothing in `src/` reads Win32
focus, so alt-tab reaches us only through the runtime's own state.

## Previous state (2026-07-30, session 28 part 1 - OPEN BUG 2 ROOT-CAUSED: there are TWO lenses and the watch was reading the wrong one)

**The yaw warp is not a pose bug, not a formula bug, and not a stereo bug. The instrument was
reading the viewmodel's lens and the projection claim was following it.** Full derivation with
every number in `docs/bioshock1/ENGINE_NOTES.md` "Session 28".

**The mechanism, end to end.** A BS1R frame carries TWO perspective lenses that use OPPOSITE
aspect conventions: the world pass is `tanH = tan(option/2)` with `tanV = tanH*(h/w)`, the
foreground pass is `tanV = tan(fgFov/2)*3/4` with `tanH = tanV*(w/h)`. **They coincide exactly at
16:9 and diverge by `(16/9)*(h/w)` everywhere else** - 1.78x at 1:1, 1.84x at 2750x2850. The live
fov watch took the FIRST decodable draw of each interval, and the foreground draws are the first
draws of the main pass on a 576-byte tier that cleared its 320-byte gate, carrying the same
screen-ray block at the same floats. So off 16:9 the watch reported the VIEWMODEL lens as the
rendered world lens. That made `fov_mismatch()` latch permanently ON during normal gameplay,
which routed the projection claim through the `fovMm && stereoCine` branch and tagged the layer
with the viewmodel frustum: `tanH 0.6468` over a world rendering `tanH 1.1918`. **A 1.84x
under-claim, so the compositor mis-reprojects every head rotation by `atan(k*tan(d)) - d` -
several degrees at ordinary turn rates, snapping back when the head stops.**

That explains every constraint at once, including the two that made it look unexplainable:
FOV-slider independence (`k` has no option term), the 16:9 cleanliness (the lenses coincide
there), and **BioVRDev not warping at the same resolution** (they submit the option-derived claim
and have no mismatch detector - and under the measured law their claim is simply correct).

**Two session-27 eliminations were honest measurements that could not have caught it**, and both
are corrected in ENGINE_NOTES: `src=live` was read as proof the claim tracked the render (the
label was true, the lens was wrong - a source tag is not a correctness proof), and the pose audit
compares `projViews[0].pose` against `g_consumedHeadQuat`, which is stamped from the SAME locate
generation the tag comes from, so `delta 0.00 deg` was guaranteed by construction. **That audit
cannot eliminate the pose hypothesis** - it only proves the locate-to-tag plumbing is intact.

**Killed by measurement, no headset time needed:**

- **Head roll survives to the render.** `simhead 0 0 40` + `reentry dump`: the engine's own render
  submit gets `rot=(0,28303,7281)=(0.0,155.5,40.0)deg` and the screenshot shows the world rolled
  40 deg. BS1R does NOT need BioVRDev's render-thread roll re-write.
- **Both SR eyes render from a bit-identical rotation**, `rot=(0,28303,0)` on both passes, with
  locations 6.36 UU apart laterally (== ipd 63.4 mm at worldScale 100) and `dz` going 0 -> 4.1 UU
  when roll is applied. The per-eye camera rig is correct.
- The world-lens law is the ORIGINAL assumption. The session-27 un-retraction was wrong (it
  confirmed the fg lens three times); reverting all three formula rewrites was right.

**What shipped (flat-verified at 2048x2048, the resolution the bug was reported at):**

- `hud_capture.cpp` votes instead of guessing: up to 8 cb0 heads per interval at a **stride**
  derived from the previous interval's distinct-buffer count so the sample spans the whole pass
  (first-8 sampling gave the viewmodel 5/8 votes - measured), clustered, majority wins, runner-up
  published as the fg lens. Structural zero-slot validation ported from the offline decoder.
  Majority + coverage guards refuse a marginal round rather than publish it.
- The age gate moved INTO `fov_watch()` (default 500 ms); every line says `FRESH` or
  `STALE - DO NOT CONCLUDE` in words; `fovaudit` reports both lenses, the vote split, the stride
  and the live aspect on one line plus a `laws` line - no conclusion needs two lines again.
- `fovaudit`'s option-derived column no longer falls back to a hardcoded 9/16 with no session.
- The claim substitution logs itself once, with the reason.
- **Alt-tab freeze (OPEN BUG 1): the ordering bug is fixed.** `pump_events()` now runs ABOVE the
  pair-hold early return - it used to return first, so while a hold was set no XR events were
  polled at all, `g_state` could never update, and nothing below could re-arm; alt-tab stops the
  game presenting mid-pair, so a LEFT-tagged hold survived the whole unfocused window with no
  sibling coming, and the ONLY path that cleared it was the `!g_enabled` teardown - which is
  exactly why the VR toggle was the sole recovery. The hold is now aged (500 ms, ~125x the
  measured 1-4 ms build time) and force-aborts through the existing leaked-frame close; a leaked
  frame is closed before the pace guard can return; `g_unfocusedSinceMs` clears on STOPPING and
  `g_paceSkips` is per-episode (both suppressed the only two log lines that would have told a
  stuck user anything); and a 5 s `xr: SUBMISSION IDLE (reason=...)` heartbeat now names the exact
  guard that is firing.
- `vrstereo stereoonly on|off` + an overlay checkbox: drops ONLY the doubling and leaves 1t and
  the head drive alone. Verified flat (`2nd=153/s -> 0/s -> 163/s`, `1t=1` throughout). The
  one-toggle keeps its exact behaviour and label. Every previous "is it stereo?" A/B was
  confounded because the one-toggle also kills the head drive.
- `Bioshock.ini` restored to 2048x2048 via `vrres`, relaunch-verified.

**Stage 2 measurement (b) is answered as a by-product, and it condemns a shipped constant.** The
foreground match writes `fg = 2*atan(tan(worldHalf)*0.75)`; matching the world needs
`(4/3)*(h/w)`, which is `0.75` **only at 16:9**. So the shipped constant under-lenses the
viewmodel by `1.7778/aspect` - 1.78x at the square backbuffer the README recommends. That is the
"hands look huge" report, quantified, and the fix reduces to `0.75` at 16:9 so it cannot
invalidate the session-16 calibration. **Deliberately NOT shipped yet**: it changes viewmodel
scale visibly and must not confound the warp re-test.

**Also flagged:** VR PRESET 1's 129.5/130 FOV write was derived under the WRONG law
(`tan(option/2)*9/16*a = tan(H/2)`). Under the real world law, option 100 at a square backbuffer
renders exactly 100x100 deg, so 130 over-widens by 30 deg. A stage-2 policy fix, not a bug.

## Previous state (2026-07-30, session 27 - BS1 STABILITY: the object scanner was the freeze, and probably the crash - branch s27-b1r-stability-and-resolution)

**Stage 1 of four is in and flat-verified on the real game.** The work is driven by a third
crash report from the same external machine (v0.4.1, `0xC0000374` STATUS_HEAP_CORRUPTION at
the MAIN MENU 85 s in, whole stack in ntdll) plus widespread freeze reports. Full forensics
in `docs/bioshock1/ENGINE_NOTES.md` "Session 27".

**What was wrong.** `patterns::scan_for_vtable_object` swept the whole 4 GB address space at
a 4-byte stride, in one blocking call on the game thread, accepting any
`MEM_PRIVATE | PAGE_READWRITE` region - which is also the signature of a thread stack. Three
consequences, all in that log: stacks were swept and the sweep matched its OWN argument
spill (the two "vtable match" lines sit 0x40 below the crashing thread's esp); a candidate
that passed only two predicates then reached three unguarded writes, so a freed or
stack-resident address could be poked every frame; and one pass measured over 3 s of blocked
game thread, which is the freeze. The session-22 dormancy "fix" did not hold because it was
a caller-local counter that any momentary success zeroed, and because `aim.cpp`'s gameplay
predicate deliberately counts the menu attract scene as gameplay, so full walks ran at the
main menu - the exact window that tester dies in.

**What shipped, measured on a boot-to-gameplay run:**

- New `core/hooks/heap_scan` - thread stacks/TEBs excluded exactly via TEB stack bounds
  (plus a guard-page probe), the needle only ever compared XOR-masked, a live-heap-block
  fast path, and the region walk kept only as a 4 ms sliced fallback.
  **`UShockUserSettings` now resolves in 56-62 ms with exactly one match** (was 3+ s and
  three matches, two of them our own stack). Engine ACTORS are not in a heap-block prefix
  and still need the sweep: its ~3.8 s of work now spreads over 888 slices and **the camera
  heartbeat holds 1600-2100 calls/s across all of it with no dip.**
- UObject class-chain corroboration on every candidate and revalidation, the FOV window
  narrowed to the engine UI's 75-130, a structural dormancy latch only an explicit re-arm
  clears, the weapon scan gated on the strict gameplay view, and fail-closed on ambiguity.
- **Host build fingerprint gate.** Every storefront ships the same `BioshockHD.exe`, so an
  Epic/GOG/repatched build was accepted and then mis-addressed. Verified BOTH directions:
  correct build logs "host build VERIFIED"; a deliberately wrong expected timestamp prints
  the running-vs-expected numbers, refuses object scans once, keeps the pattern-derived
  CalcView hook (build-independent), and the game boots to gameplay fully playable.
- Engine-exec seam hardened: per-slot stub thunks that record being entered, an esp
  comparison across the call that repairs and latches the seam off on imbalance, and the
  re-assert moved off its 15 s timer to the gameplay transition plus a 5 min net
  (8 exec calls per boot -> 4).
- `crash::install()` first, and the swapchain hooks before the OpenXR instance, so a runtime
  that blocks on its streamer can no longer cost us the Present detour and with it
  `crash::rearm()`.
- Real-size `ResizeBuffers` now QUEUES the XR swapchain rebuild for a point in the frame
  loop with no XR frame open, closing the documented-open half of the session-23
  `0xDEDEDEDE` VDXR use-after-free.
- `XR_ERROR_RUNTIME_UNAVAILABLE` now explains itself: SteamVR has never shipped a 32-bit
  OpenXR runtime, so Index/Vive/Steam Link users land there every time.

**Two negative results that change the plan.** `SETRES` through the viewport Exec seam
FAULTS (`exe+0x4C2353`, near-null, no `ResizeBuffers` follows) even though `set ...` through
the same machinery works every run and the stack stays balanced - that seam has advertised
`SETRES` support since it was derived and had never been called. So a live in-session
resolution change is off the table until root-caused, and the game-ini lane becomes the
PRIMARY mechanism. And the world-lens aspect law is still unmeasured: at 1920x1080
`tanV/tanH = 9/16` exactly, but 9/16 IS the render aspect there, so this run cannot
distinguish "derived from aspect" from "hardcoded". The foreground-lens aspect fix is
therefore unproven and must not ship yet.

**Also fixed:** `-Game` was positional on `game-cmd.ps1`, so every bare
`game-cmd.ps1 "..."` failed validation and `boot.ps1`'s menu-press loop had been silently
doing nothing since the BS2 harness commit.

## Previous state (2026-07-29, session 26 - M10 STEREO ACCEPTED IN-HEADSET ("looks awesome"), plus a core HANG FIX found right after - branch s26-pace-hang-fix)

**IN-HEADSET VERDICT (user, Quest 3 / VDXR): stereo ACCEPTED - "looks awesome, very good
for everything".** Depth, comfort and scale all read right; the user did not need to move
the world-scale slider off the default 100. One known-deferred blemish, volunteered by the
user: **the viewmodel/hand models look weird and a bit wrong in stereo, same as BS1 did** -
explicitly parked for a later milestone (see next steps; the likely mechanism is noted
there and it is NOT the FOV apparatus, which s25 already cleared).

**THEN THE HANG.** Minutes after the acceptance run the user reported the game not
responding. Triage: NOT a crash - no dump, no fault, no poison. Process wedged at 0.11 s CPU
over 4 s with one running thread, log stopped dead one line after `xr: pace keepalive while
VISIBLE`. Root cause is CORE, affects both games, and predates the stereo work: the M8
unfocused-pace guard let one real paced frame through every 5 s as "insurance", and
`xrWaitFrame` takes no timeout - with the headset idle it never returned. On BS2 that call
runs on the dedicated present thread, so it back-pressured the game thread through the
render command ring and froze the process. **Fixed** (branch s26-pace-hang-fix): the
keepalive is retired - once FOCUSED has been held, unfocused presents always skip the
blocking wait; recovery was always event-driven (`pump_events` runs above the guard). Plus
a belt-and-braces close of any leaked open XR frame before waiting. Built and installed to
BOTH games; needs the user's headset-off/on re-test to confirm.

**BS2 SequentialReentry is flat-green, and the headline is what it does NOT need: none of
BS1's single-threading machinery.** The substrate was derived in one session (live
kick/kick2 samplers -> offline capstone walks -> `UGameEngine::Draw` = RVA 0x4EE8D0,
engine vtable 0x10BD7DC slot +0x118 - the session-24 RTTI candidate, now consumed), and
the policy gate returned its biggest win yet: BS2's Draw path has no kick-and-wait
handshake (BS1's deadlock class cannot form), so the double Draw runs on the THREADED
renderer as-is. Flat gates all passed: pulse (call2 ~5 ms real work, presents == draws +
pulses), continuous (2nd/s == draws/s, presents/s == 2x exact, off recovers instantly),
stereo (per-eye camera delta 6.30 UU == ipd/1000 x worldScale EXACT, 2nd-pass CalcView
replay 655/655, zero skips/faults), `vrstereo on` -> READY, ~5 min stereo soak clean.
Every lever DEFAULT OFF; pass 2 is deny-by-default on the single gameplay caller ret
0xCD5D7B. In-headset depth verdict + world-scale calibration = the user checklist in
docs/bioshock2/TESTING.md (world scale was blocked on stereo since session 24 - now
unblocked).

### 1. THE HEADLINE: threaded-substrate SequentialReentry (the policy's second big payout)

BS1 forces the renderer inline (structural 1t via the flush-point hook) because its
game thread parks in a racy per-frame event handshake. Session 26 checked whether BS2
even has that defect before porting the cure: it does not. `UGameEngine::Draw` fills a
cursor-based command ring; the render-thread sync runs once per tick AFTER Draw
returns; a doubled Draw enqueues a second scene + present command and the dedicated
render thread (presentTid != drawTid, live-attributed) just drains both. No flush-point
derivation, no drain guard, no hw-thread poke, no watchdog. The 1t-fallback entry
points (render sync FEventWin pair 0x1A69294/98) are banked UNCONSUMED in ENGINE_NOTES
in case long soaks ever disprove this.

### 2. The derivation (session 26; full chains in docs/bioshock2/ENGINE_NOTES.md "Scene-draw architecture")

- Live instruments first: `reentry kick` (BS1's SetEvent sampler + a BS2 deep-caller
  extension - the FF15 wrapper methods mask direct callers here) and `reentry kick2`
  (sampler ON `FEventWin::Trigger` 0xB81050 itself - its ret addr IS the virtual call
  site). kick2 split the game thread's five once-per-present kicks and cracked the
  protocol: they are Flash/FMOD lock-step and endframe signals, NOT render submits.
- Session-25 recon CORRECTED: the "SetEvent wrappers" were thread Suspend/Resume +
  CRT once-init; the "pump candidates" were a semaphore ctor + an unrelated wait
  helper; 0x5C7C80 wears BS1's submit shape but is the CONTENT-STREAMING view hand-off
  (RTTI: FContentStreamingManager). The never-copy rule extends to shapes: a
  shape-match on the wrong game is a mislabel waiting to happen.
- PlayerCalcView dispatches EXACTLY once inside every Draw (live: calcIn == draws every
  beat) - so BS1's pass-2 replay design transferred unchanged onto the ProcessEvent
  seam. camSrc probe + streaming camera globals live-verified via hexdump.

### 3. What landed in code (branch s26-m10-bs2-stereo, 3 commits)

- b2r `scenedraw.{h,cpp}` (new): kick/kick2/calcstack instruments, prologue-gated
  HookSlot infra with the vtable-chain build-identity gate (`verify_draw_chain`),
  pass-through Draw/stream hooks + beat telemetry, `maybe_second_draw` (poison latch,
  gameplay-caller gate, calcview-silent + present-stall skips, SEH-guarded second call),
  `vrstereo` one-toggle + overlay checkbox request lane.
- b2r camera.cpp: `apply_eye_offset` (full-rotation right axis), AER wiring + inter-eye
  delta log, SR pass-1 base cache + pass-2 `second_pass_replay` on the ProcessEvent
  fn-match branch, poll-gate hardening (commands NEVER execute mid-Draw - a BS2
  improvement over BS1), `vrstereo` command, `calcview_silent` export.
- core (one-liner): `d3d11_hook::last_present_tid()` - the single- vs multi-threaded
  render attribution that decided the whole design.
- Adapter advertises CAP_SCENE_REENTRY while reentry hooks are live.

### 4. Carries closed

- Menu controller vtable 0x106EE20 RTTI-identified: plain `APlayerController` (the
  menu scene runs the base class) - session-24 gap closed.
- Idle-gameplay crash 0x4FF0FE TRIAGED (offline disasm): a focus-poll path reads
  `[[0x1A638F0]+0x4C]+0x44` (viewport window object) and it was transiently NULL after
  ~11 min idle - then the code compares a virtual result against `GetFocus()`. A rare
  idle/focus race, one-off class, dump cap already ships; no fix attempted.
- FOV slider endpoints: still unrecorded (needs the user in Graphics Options - on
  their checklist).

## Next steps

### 0a. THE USER'S PRIORITY ORDER (directive, 2026-07-31, session 32 - supersedes ROADMAP ordering)

Verbatim intent: **core experience first, nice-to-haves last.** Swing-to-attack is explicitly
"a far away thing, it's a nice to have" - it drops to the bottom of the queue on BOTH games.

1. **Custom resolution** (BS2 lane ships; the aspect bisection is what is left)
2. **Correct scale, and the viewmodel not moving with the headset**
3. **Motion controls**: weapons, movement, and aim being right
4. **Normal controls**: weapon and plasmid switching
5. **HUD, effects, cinematics and menu elements**
6. Everything else core to the experience
7. ...then nice-to-haves (swing-to-attack lives here)

### 0b. ITEM 2 IS DIAGNOSED AND CONFIRMED IN-HEADSET (2026-07-31)

The two-lens magnitude mismatch (Current state 3) is **confirmed as the cause of the viewmodel
defect by an in-headset A/B that tested a QUANTITATIVE prediction**, not just a direction.

Predicted: the angular-gain error is `k = tan(option/2) / tan(30)`, so 2.06x at option 100 and
3.99x at option 130. Predicted consequence: the defect should get markedly worse at 130.
**User's verdict: "I just did the test and it very clearly got a lot worse at 130 FOV."**

This is much stronger evidence than the callstack/draw-count circumstantials, and it converts the
lens split from "leading hypothesis" to the working diagnosis.

**THE CLINCHER PASSED.** The same arithmetic predicted the error VANISHES at option 60, where
`k = tan(30)/tan(30) = 1.0` and the world lens exactly equals the fixed 60-deg lens. User, same
session: **"gfov 60 test done, the weapon looks correct now and doesn't move."**

Two predictions, at two different input values, both confirmed. **The diagnosis is closed.** A pass
that were not the viewmodel could not make the VIEWMODEL correct at exactly the option matching its
own tangent - so this also identifies the cluster, and the queued holster test is RETIRED as
unnecessary. It further retracts session 25's "BS2's foreground follows the world FOV natively"
(see the retraction in ENGINE_NOTES near line 126) - **BS2 does have a fixed foreground lens, just
like BS1.** The defect class ports; that does not license porting BS1's specific machinery.

**Immediate zero-code mitigation for the user:** keep the FOV option LOW (100 or below, and note
`gfov` defaults to 130 when enabled - leave it off). Lower option = smaller `k` = less viewmodel
error, until the real fix lands.

**The real fix, once the clincher passes:** make the two lenses MATCH, because one projection layer
carries one fov claim and only matched lenses make both world and viewmodel right (BS1's session-28
conclusion, and the one piece of BS1 reasoning that DOES transfer - the mechanism differs, the
consequence is identical). Direction: raise the 60-deg lens to the world's value rather than
dropping the world to 60. That needs BS2's second-lens FOV field, which is UNDERIVED. Per the
standing policy, check for a native BS2 route before porting BS1's fg-bake machinery.

### 0c. SESSION 33 OPENER (BS2) - written with the verdicts that shape it

Session 32 answered the lens question and killed the square-backbuffer plan. Steps 3-4 of the
session-32 brief were NOT reached; they are re-scoped below by what was measured.

**A. Bisect BS2's usable aspects (30-60 min, unblocks everything else).** BS2 renders 2048x2048 as
a letterboxed 2048x1421 with a degenerate projection, and 1920x1080 cleanly. The question is where
the boundary is. Each data point is `vrres <w>x<h>` -> relaunch -> load a save -> `dumpframe full`
-> check the `vp=` field and whether the ray block's vertical pair agrees. Suggested ladder:
2560x1440 (16:9 control, expect clean), 1920x1440 (4:3), 1920x1200 (16:10), 1920x1600. **The
squarest aspect that renders full-height with a consistent frustum is BS2's best VR
configuration**, and it doubles as the clean second aspect that settles the world FOV law (section
4 above). `[Engine.RenderConfig] HorizontalFOVLock=True` is an unexamined suspect for the
mechanism.

**B. ~~Prove what the 60-deg lens IS~~ - DONE, retired.** The two in-headset FOV A/Bs identified it
conclusively (see 0b). It is the viewmodel. No dump needed.

**B'. THE FIX: find BS2's foreground lens FOV and write the world's value into it.** This is now
priority 2 on the user's list and the diagnosis is closed, so it is the main event.

- **Direction: raise the fg lens to the world, do NOT drop the world to 60.** Matching at 60 works
  (proven) but a 60-deg world is unusable in VR. Matching at the world's value gives a wide world
  AND a correct viewmodel - the only state where both are right, because one projection layer
  carries one fov claim.
- **The field is UNDERIVED.** Leads, cheapest first:
  1. The value is exactly `tan(30)`, i.e. **60.0 degrees** - a round number, so likely a stored
     property or a class default rather than a computed one.
  2. **The fg draw callstacks are already captured**: `0xBE9E68,0xAECACF,0x646054` and
     `0xBE9E68,0xAECACF,0x6926FD` (session-32 dumps, 16:9). `0xAECACF` is the fg-specific frame -
     the world pass goes through `0xAEC7B4` instead. Disassemble around these offline (capstone is
     installed) to find where 60 enters the cb build. This is the highest-value lead and needs no
     game.
  3. b2r already has the full discovery command set (`memscan`/`memscani`/`mempoke`/`fsweep`/
     `hexdump`/`pokeaddr`) - scan for the float `60.0` or `0.57735` and poke, watching the
     viewmodel. Expect many candidates; use the option sweep as the discriminator (the right one
     does NOT change when the option does).
  4. Check for a NATIVE route first, per the standing policy: BS2 has an exposed FOV concept BS1
     lacked, so a viewmodel/weapon FOV property may exist in `BakedScripts` or be reachable
     through the ProcessEvent-by-name seam. Look before porting BS1's fg-bake machinery.
- **Acceptance:** `fovaudit` reports `lenses=1` in gameplay at 16:9 (the two clusters collapse),
  and in-headset the weapon is stable at option 100+ rather than only at 60.
- **Interim guidance for the user until it lands:** keep the FOV option LOW. The error is
  `k = tan(option/2)/tan(30)`, so lower is better; 60 is perfect but narrow. There is likely a
  tolerable middle (try 75-85) trading a little world width for a much steadier weapon.

**C. In-headset: the pitch servo's sign (30 s, owed).** Checklist in `docs/bioshock2/TESTING.md`.
`enginePitch=` in the heartbeat must MOVE while looking up and down; `vrinput pitchservo invert` if
the view fights you. Measure BS2's own stick deadzone rather than assuming BS1's 4-8 deg residual.

**D. Swing-to-attack on BS2 (its own session).** Still exactly one line
(`swing::publish_gate(...)`) plus the melee-equipped predicate, which is the hard part. Step 0's
`vrinput` dispatch removed the blocker that would have made the flat suite untestable. Hazards
unchanged and BS2-specific: native dual wield (a left-hand gesture must not compose the right
trigger), the drill's sustained fire mode (a 120 ms discrete pulse may read as a stutter), more
melee-ish states than BS1's single wrench. Re-measure the pulse width and threshold - 3.6 m/s is
per-PLAYER.

**E. Do not tune any BS2 viewmodel trims yet.** The reason is unchanged and now better evidenced:
trims tuned before the lens question is closed bake the lens error in and stop being portable.

### 0b. FIRST ACTIONS NEXT SESSION (BS1 - unchanged from session 31)

The game-breaking item is closed and accepted. What is left before the release is verification
breadth, not new investigation.

0. **A FULL PLAYTHROUGH IS THE GATE, and session 30 part 3 earned that the hard way.** Every fix
   there was accepted on a single check, and **two of the three regressions were found by the user
   playing, not by any check we ran**. The classifier changes touch every HUD frame in the game.
   Untested against them: menus, the hack minigame, vending machines, the Gatherer's Garden, Big
   Daddy FMVs, and every cutscene other than the first-plasmid one.
1. **Build the auto-fire frame dump** (see 4b) and get a capture from INSIDE a cutscene. The
   cutscene workaround is shipping without a diagnosis; this is what closes it.
2. **A proper soak on the wrench fix** - and the session-31 swing gesture rides along with it, since
   the gesture fires the same melee the servo aims, so one wrench soak exercises both. It was
   accepted on one enemy. The servo is a new input
   path that runs on every gameplay frame, so it wants a real play session: melee at different
   heights, on stairs, underwater, against a Big Daddy, and across a save load. Watch for the
   view feeling like it is fighting you (that would be the servo sign or gain) and for the
   `[b1r] camera:` heartbeat pitch tracking the head instead of freezing.
2. **Verify the classifier and bar fixes in-headset with breadth.** Both were accepted on a
   single check. Confirm: HUD art on the panel and not in the eye image, menus and the
   Gatherer's Garden readable, hack minigame unchanged, cutscene bars still gone, subtitles still
   readable, alcohol blur still full-frame (it was A/B'd and is fine, but re-check after a soak).
   `vrhud status` should show `stranded=0` and `postFxRejected` climbing with `postFx` low.
3. **The hands regression checklist** (section 3 below), which is the last untouched open item.
4. **Then the effects geometry**, now correctly scoped - see section 2.

### 1. THE WRENCH - CLOSED, with two lanes permanently shut behind it

Fixed by the pitch servo (Current state 0). Do not re-open either dead lane:

- **The aim seams.** Measured: melee reaches neither. No `vraim seam weapon off`, no
  `vraim origin off`, no melee carve-out in `substitute()`.
- **Lock-on.** The absurd-radius test showed 5000 feels identical to 0, so that write never
  reaches the live object. **`-> HANDLED` proves only that `Exec` recognised the command** - the
  output-device stub suppresses the error a failed `set` would print. If lock-on ever needs
  disabling for real, that is a fresh derivation, not a retry.

Open refinement, low priority: the servo's steady-state residual is 4-8 degrees because the
proportional stick falls under the game's own deadzone near convergence. Closing it needs a
minimum stick magnitude, which risks a limit cycle. Measure the game's deadzone first.

If a damage-side seam is ever wanted: `AWeapon::InitiateDamage` is RVA `0x226050` and
`UAttackAbility::InitiateDamage` is `0x1BBD80`, **both `ret 8`, so `vraim scanimpl <rva> 2`**.
Getting that number wrong is what crashed the game this session.

### 2. EFFECTS - it is a GEOMETRY problem, not a routing one

Two things are now excluded by measurement: routing (the stranded counters, with a device check
and a positive control) and "put it on a different render target" (session 29 tried exactly that
and it could not work). The remaining cause is the fill's own extent, and the user's report names
it: the effect is *"the size of the HUD or the size of the old resolution"*, i.e. the gameswf
STAGE rectangle.

So the fix has to change the geometry. Ranked:

1. **Patch the dynamic vertex buffer in flight.** Most likely to work. Scope it to
   `textureless && verts <= 8 && bbox is a proper sub-rect` so bars, subtitles, menus and HUD art
   are untouched by construction - and note the bar fills share that fingerprint, so the bbox test
   is doing real work and must come first.
2. **Draw our own full-screen quad** in the fill's colour. Blocked today: the colour is in a PIXEL
   shader constant buffer and the dump records only VS b0..b2.
3. **Edit the SWF.** Precedent exists (the Nexus mod is a one-byte scale edit) but it ships a
   modified game asset, which this project has avoided.

Measure before any of it. `img-diff.ps1 -Grid`/`-Bands` is built and self-tested: `vrhud force
off`, screenshot dry, hold the effect, screenshot wet, then `vrcine effects panel` and again;
`diff(dry,wet)` bbox is the coverage number and `diff(dry,panel)` gives the panel footprint in the
same image space. The flat window shows the whole square render stretched to the client area, so
band FRACTIONS map linearly to the frame and the measurement is valid. And ask the free question
first: does the effect stop before the picture stops, or do they end at the same edge? If they end
together the fill is fine and the defect is the projection claim, which is a different fix
entirely and one flat cannot measure.

If the bands come back ambiguous, the vertex-buffer capture is the next instrument: insert after
the srv0 block in `capture_draw_state` (`frame_inspector.cpp:400`), full mode and non-indexed
draws only, reusing the cb0 staging pattern but with `CopySubresourceRegion` and a byte-valued
`D3D11_BOX`, calling `g_origCopySubRes`/`g_origMap` so the hooked slots do not pollute the
census. Calibrate the decode against the 29-vertex BAR draw, whose on-screen extent the pixel
watch measured independently (313/350 px of 2048).

### 3. HANDS - the checklist, unrun

`vrbones status` now prints the residue on demand. Steps: gameplay, into a cutscene (expect
`[bones] released to the engine (hands gated for cinematic)` then `hiddenHand=-1`), out of it,
across a save load (`[bones] world changed` must precede any release - this is the session-29
hang path), `vrcine drive off` mid-cutscene and back, then `authored+look`. Plus the blind spot:
`vrhands off` mid-cutscene skips `bones::release()` entirely, because `hands.cpp:605` sits above
the cinematic gate.

### Previously: three open items, one of them game-breaking

Session 29 is MERGED TO MAIN. v0.5.0 is built and packaged but **deliberately untagged**: the
user's call after in-headset testing was *"Don't release yet till the above notes are done and
tested."* In priority order:

**1. THE WRENCH SOMETIMES DOES NOT HIT (game-breaking, user's ASAP).** Reported in-headset:
melee misses often *while fighting an enemy*, rarely outside combat. Other players have
reported it while breaking the rocks at the start where there is no combat, though the user has
not seen that case themselves. Cause unknown.

*Leading hypothesis, and it is ours to disprove first:* the aim substitution. `substitute()`
rewrites `GetPerfectFireStart`'s out-params for BOTH `AWeapon` and `UAttackAbility`, and the
wrench is an `AWeapon`. Melee is a SHORT trace, so an origin moved from the camera to the
controller (plus `vraim pos` offsets, up to several cm) can start the trace past or beside the
target in a way a bullet at range would never show. That would also explain "worse in combat":
close-quarters geometry is where a few cm of origin error decides a hit. **First measurement:
`vraim seam weapon off` (the per-seam substitution toggle already exists) and see whether the
misses stop.** If they do, the wrench needs excluding from the origin substitution - direction
only, or engine origin - keyed on the equipped weapon class, which `update_weapon_profile`
already resolves. The user also wonders about soft lock-on; note we force
`GamepadPlayerInput SoftLockOnRadius 0` at every world event (`console_exec`), so backing THAT
out is the second A/B and is one command.

**2. FULL-SCREEN EFFECTS ARE STILL NOT FULL-SCREEN.** Session 29 identified the effect draw
(textureless 5-vertex gameswf fill, framedump_175024) and routed it in-frame
(`effectsInFrame=1`). User verdict: better but **still not covering the whole view**. So the
draw we found is not the whole story - there is at least one more contributor, or the fill is
geometrically smaller than the viewport. Next step is measurement, not another guess: dump with
an effect held and read the fill's VERTEX BUFFER (the dump does not capture geometry today -
that is the gap), or compare a flat screenshot with the effect against one without to see how
much of the frame it actually covers. Must not regress cinematics, HUD elements, menus or
subtitles - all four share this classifier, and the subtitle routing in 0d is one flip away.

**3. HANDS CONSISTENCY PASS.** The user reports the hand behaviour is "not consistent either
way" and wants confirmation nothing was broken. Session 29 changed the gating and added
`bones::release()`, so this is a targeted regression check rather than an investigation: hands
during gameplay, across a cutscene, across a save load, and after switching drive modes.

### Previously: the user's final go

Stage 3 is accepted in-headset and v0.5.0 is packaged. The user is soaking the Release build
before giving the final thumbs up: *"Don't release and merge to main yet since I wanna test
stuff out first just in case - will give you the final thumbs up."* **Do not merge or publish
without it.**

When it comes: merge to main, `git tag v0.5.0`, publish `dist/bioshock-vr-v0.5.0.zip` with
`docs/RELEASE_NOTES.md` as the body. The zip is already built from this tree and its version,
DLL banner and tag are guaranteed consistent by `tools/package.ps1`.

Carried, not blocking: the harness debt below, and the `authored+look` alternative in 0e.

### Superseded by the in-headset run - the checklist that got us here

1. **Bars.** Load the Gatherer's Garden Electro Bolt save, take the plasmid. Expect the bars to
   flash briefly at the start (the pixel watch needs ~6 presents to arm the hold) and then go,
   with the picture underneath **intact** - not cropped, not stretched. A/B with
   `vrcine bars hide|show`, 30 s per verdict. `vrcine status` prints `barDraw` and `pixelWatch`
   side by side; with bars hidden, `barDraw=1 pixelWatch=0` is the DESIGN, not a fault.
   *Watch for over-suppression:* if a fade-to-black stops working, the textureless discriminator
   is catching a fade too, and the fix is to narrow it by vertex count (`vrcine status` reports
   the count it last skipped - 29 when measured).
2. **Drives.** `vrcine drive authored` (default) with the controllers AWAKE - this is the exact
   condition that produced the session-22 round-5 report. The authored hands should play. Then
   `authored+look`: the authored camera choreography must survive while the head can look around,
   and the pawn must not rotate under it. Then `off` as the A/B. The `[b1r] cine edge` line is
   what settles the sticky-state question flat measurement could not (see Current state 4).
3. **Aim dot.** `vraim dot on`, set `vraim dot dist <m>` to a wall's distance, fire, and check
   the dot sits on the hole. The dot should vanish exactly when substitution would refuse.
   Also confirm the `[aim] dot transform round-trip error` line says EXACT.

Also worth one pass while in there: **alt-tab mid-cutscene**. The session-28 pace thread has
never met a cinematic, and that intersection is the whole reason the release was held.

### Then, and only then: the release

Merge to main, bump `project(BioshockVR VERSION ...)` in `CMakeLists.txt:6` off 0.4.1 (single
source of truth; `bvr_version.h` is generated), build **Release** - everything in sessions 27-29
has been Debug - soak once because the pace thread is new and Debug timing is not Release timing,
then `tools/package.ps1`, tag, publish.

### Harness debt found this session (costs a human every run)

`boot.ps1`'s A-press loop does **not** activate the main menu on this build, and
`game-click.ps1` hovers the entry without activating it. Menu navigation currently needs a human,
which makes every replay a round trip. In-game triggers (`vrinput test press A`) are unaffected.
Worth fixing before the next investigation that needs repeated loads.

### Previously: BOTH session-27 open bugs are CLOSED and in-headset accepted.

Stage 1 (stability), the resolution lane, the yaw warp, the viewmodel/head coupling and the
alt-tab freeze are all done and confirmed on the real headset. Stage 2 is substantially complete -
both of its blocking measurements landed this session. What remains:

### 1. Stage 2 leftovers (small, and one is a policy decision, not a bug)

- **The VR PRESET FOV policy is CLOSED, by the user's call 2026-07-30, and the reasoning matters.**
  The preset no longer writes any FOV and should not. The user's account of WHY they had been
  raising it: *"I was just changing the FOV since I wasn't able to put the screen in full view and
  have no black bars. But now since I can change the resolution however I want then it's good."*
  **Those black bars were an ASPECT problem, not a FOV problem, and cranking the option was
  compensating for the wrong axis.** A headset eye is roughly square; a 16:9 render claimed at 16:9
  fills a wide short rectangle inside it and leaves unfilled bands top and bottom, and the only way
  to cover them by widening FOV is to over-render horizontally and throw the pixels away. Matching
  the render ASPECT to the eye instead - a square backbuffer - makes the claimed frustum the right
  shape, and then a sane FOV fills the eye exactly. Under the corrected world law option 100 at
  2048x2048 renders exactly 100x100 deg against a Quest 3 eye of roughly 100x96, which is very
  nearly an exact fill: the reason 100 + square works is arithmetic, not luck.
  So the standing policy is **set the aspect with the resolution lane, leave the FOV alone**, and
  the old "crank the option to 130" reflex is retired along with the law it came from.
  `vrfov`/`gfov` remain as manual levers, default off. This also means the resolution lane was not
  just a sharpness feature - it is the mechanism that made the FOV write unnecessary.
- **`xrEnumerateViewConfigurationViews` is still never called.** It is the missing input for both a
  correct FOV policy and a derived render target (`recommendedImageRect`), and its absence is why a
  runtime-side resolution slider does nothing for this mod (the Dream Air user datapoint).
- Overlay device-identity and hwnd-identity checks, and `input_drive`'s `g_armed` re-arm on a
  client/viewport rebuild - without the latter, motion controllers die silently after a live
  resolution change.
- `SETRES` through the viewport Exec seam still FAULTS (`exe+0x4C2353`); the ini lane is primary and
  works, so this is a diagnostic curiosity rather than a blocker.

### 2. Stage 3: cutscenes and the aim dot (the next real milestone)

Measure the black-bar mechanism FIRST - the Nexus "Fullscreen Cutscenes" mod is a `HUDPC.swf` edit
zeroing a sprite named `WidescreenBars`, which contradicts our own session-22 dump reading (engine
clears to black and draws a vertically shrunken tonemap quad). Install it, replay the Gatherer's
Garden Electro Bolt sequence, and see whether the bars go and whether the content is squeezed. Then
either suppress that one gameswf draw or fix the unsqueeze. Then `vrcine off|authored|authored+look`
with the hands, aim and laser drives gated on the letterbox exactly as the head drive already is
(closes ROADMAP's session-22 round-5 item - authored cinematic hands currently only survive when the
controllers are idle). Then a single toggleable aim dot on the verified aim ray.

### 3. Stage 4: SteamVR/OpenVR and the dead interaction profiles

SteamVR has never shipped a 32-bit OpenXR runtime, so Lighthouse and Steam Link users cannot start
VR at all (stage 1 now says so in the log). Real fix is a native OpenVR backend behind the
`bvr::vr` facade. Independently and much cheaper: `openxr_input.cpp` suggests bindings for only
`oculus/touch_controller` and `khr/simple_controller`, so on SteamVR with Index or Vive wands, or on
WMR, sticks/triggers/grips/face buttons are all dead even when a session does start. Add those
profiles - that is a contained win worth doing before the backend.

### 4. Release - HELD until stage 3 is finished (user's call 2026-07-30)

The user considered releasing at the end of session 28 and then explicitly deferred: *"let's hold
off on the release till we finish stage 3 that way I can make sure everything also works and not
weird for example in cinematics and the like."* The reasoning is sound and worth honouring rather
than re-litigating: session 28 changed the projection claim, the foreground lens and the frame
pacing, and **cinematics are the one place those three interact that has NOT been exercised** - the
cinematic path has its own claim substitution branch, its own letterbox gating of the head drive,
and its own quad fallback. Shipping before stage 3 would put an untested combination of the three
in front of an external tester.

So: stage 3 first, in-headset, then bump off 0.4.1 and build **Release** - everything tested in
sessions 27 and 28 has been Debug. Worth one soak on the Release build before packaging, because
the pace thread is new and Debug timing is not Release timing.

### 5. Carried diagnostics / follow-ups

- Root-cause the `SETRES` fault; an `offsets.ini` override so a non-Steam user can supply RVAs
  without a release; deriving `GObjObjects` to retire object scanning entirely; the config echo
  block, proxy breadcrumb and OpenXR API-layer enumeration from the diagnostics list.
- BS2 carries are unchanged (viewmodel-in-stereo diagnostic, input/motion controllers, FOV slider
  endpoints) - plus the new session-28 lens/resolution checklist now banked in
  `docs/bioshock2/ENGINE_NOTES.md`, which should be run BEFORE any BS2 non-16:9 support.
- v0.4.1 to the external tester; 2K-account lead; seated recenter designs.

### OPEN BUG 1 (CLOSED session 28, in-headset accepted): VR freezes permanently after alt-tab (user-reported, session 27)

> **Session 28: root-caused and fixed - `pace_should_skip` was NOT the culprit.** The real defect
> was ordering: `if (g_srPairOpen) return;` sat ABOVE `pump_events()`, so while an SR pair-hold was
> set no XR events were polled, `g_state` could never update, and no guard below could re-arm.
> Alt-tab stops the game presenting mid-pair, so a LEFT-tagged hold survived the whole unfocused
> window with no sibling coming - and the only path above it that cleared anything was the
> `!g_enabled` teardown, which is exactly why the VR toggle was the sole recovery (STATUS's own
> check 4 predicted the diff would "name the stuck variable outright", and it did). Fixed by
> moving the pump above the hold, aging the hold (500 ms) so a stale one force-aborts through the
> existing leaked-frame close, closing a leaked frame before the pace guard can return, clearing
> `g_unfocusedSinceMs` on STOPPING, making `g_paceSkips` per-episode, and adding the
> `xr: SUBMISSION IDLE (reason=...)` heartbeat. Note for the record: nothing in `src/` reads Win32
> focus (zero matches for `GetForegroundWindow`/`WM_ACTIVATE`/`GetFocus`), so an alt-tab reaches
> the guard only if the runtime itself drops FOCUSED. Still open, not fixed blind:
> `xrWaitSwapchainImage` uses `XR_INFINITE_DURATION` - the last unbounded block on the present
> thread.

Original write-up kept below for the record.



Alt-tab out of the game and the headset image freezes; the game keeps rendering flat
normally. Recovery requires toggling the VR enable off and back on. So Present keeps
running but XR submission stops and never resumes.

Prime suspect: `pace_should_skip` (openxr_runtime.cpp), from the M8 unfocused-pace guard as
hardened by `22ed1b0` - once `everFocused` latches, unfocused presents ALWAYS skip the
blocking `xrWaitFrame`, which returns early from `on_present_begin`, so nothing is submitted.
That part is deliberate and is what fixed the session-26 hard hang. The bug is that it never
RESUMES. Check in order:

1. What actually changes on a Windows alt-tab: does the XR session leave FOCUSED, or does it
   stay FOCUSED while only the Win32 window loses focus? `pump_events` runs above the guard,
   so state should update either way - confirm against the session-state log lines.
2. Whether anything the guard latches (`g_unfocusedSinceMs`, `g_everFocused`) fails to clear
   on return, or whether the guard consults Windows focus rather than XR state.
3. Whether a leaked open frame (`g_frameOpen` / `g_srPairOpen`) survives the skip: there is a
   leaked-frame close before the wait, but the pair-hold returns EARLIER than that.
4. Why the VR off/on toggle recovers. That path tears the session down, so diffing what it
   clears against what the resume path clears should name the stuck variable outright.

Also note the engine pauses CalcView while the window is unfocused (the harness scripts
depend on this), so the game thread may stop driving the camera - check whether that starves
something the submit path needs.

### OPEN BUG 2 (CLOSED session 28, in-headset accepted): yaw warping on head turn at non-16:9 resolutions

> **Session 28: the frame carries TWO lenses and the watch was reading the viewmodel's.** World
> pass is `tanH = tan(option/2)`, `tanV = tanH*(h/w)`; foreground pass is
> `tanV = tan(fgFov/2)*3/4`, `tanH = tanV*(w/h)`. Identical at 16:9, `(16/9)*(h/w)` apart
> elsewhere. The watch took the first decodable draw and the fg draws come first, so off 16:9
> `fov_mismatch()` latched ON in normal gameplay and the claim was substituted with the viewmodel
> frustum - a 1.84x under-claim, which is the warp. Fixed by stride-sampled majority voting plus
> the structural validation the offline decoder always had. See the "Current state" block above
> and ENGINE_NOTES "Session 28" for every number.
>
> Corrections to the analysis below, both important: the FOV hypothesis was eliminated on the
> strength of `src=live` (true label, wrong lens - a source tag is not a correctness proof), and
> **the pose audit is structurally incapable of detecting a render-vs-submit mismatch** because
> both sides come from the same locate generation, so "pose latching is NOT the cause" over-claims.
> Roll and per-eye render orientation ARE now genuinely eliminated, by dump measurement.

Original write-up kept below for the record.

Reported in-headset at 2048x2048. Turning the head makes the world warp; pitch is reportedly
clean. **Independent of the FOV slider** - sweeping the whole range changed only the visible
angular extent, never the distortion. That property is the most valuable constraint we have.

**What has been ruled out, and what is merely unproven:**

- The FOV formula was changed three times this session and reverted all three times. The
  cause was NOT the engine changing behaviour but the rendered-FOV watch being untrustworthy:
  at the same option and the same aspect it reported values differing by exactly 9/16 (1:1 at
  option 130 gave both 2.1445 and 1.2063; 1:1 at option 100 gave both 1.1918 and 0.6704).
  9/16 is the 16:9 vertical factor, so the watch is sometimes decoding the VERTICAL tangent
  into its horizontal slot. **Fix the instrument before touching projection math again**: the
  cb0 layout has a built-in cross-check (`2tanH, 0, -tanH` against `-2tanV, tanV`) that should
  make the two unambiguous and is evidently not being enforced. Several samples were also
  accepted despite printing `age>9000ms`, i.e. stale by the rule the same line prints.
- BioVRDev use the ORIGINAL formula (`halfV = atan(tan(halfH)*h/w)`) at a near-square
  2750x2850 and report no warping. That is strong evidence the formula shape was never the
  bug, and it should have been treated as a red flag against each derived "law" rather than as
  an anomaly.
- A stereo A/B was attempted and is CONFOUNDED: the overlay toggle is labelled
  "1t + camera mode + stereo" and disables the head-driven camera too. With the head not
  driving the camera, the compositor reprojects an unchanging image as you turn, which looks
  exactly like warping. Stereo is therefore neither confirmed nor eliminated. A clean test
  needs stereo off with camera mode still ON.

**ELIMINATED BY MEASUREMENT (end of session 27):**

- **FOV mismatch is NOT the cause.** At BioVRDev's exact config (2750x2850, ini FOV 100, both
  locks pinned) the submit line read `src=live`, meaning the claim is derived from the MEASURED
  rendered tangent and therefore tracks the render by construction - and the warping persisted
  anyway. Checking `src=` on that line hours earlier would have ruled FOV out before three
  formula rewrites. (That run also confirmed the law a third time and exactly:
  `tanV = 0.670361 = tan(50)*9/16`, `tanV/tanH = 1.0364 = 2850/2750`. So the law is real, the
  last revert was wrong, and it is ALSO irrelevant to this bug because the live path was
  already keeping claim and render aligned.)
- **Pose latching is NOT the cause.** `fovaudit pose on` during head sweeps reported
  `delta 0.00 deg` on every sample with real yaw motion (61 -> 1 -> -2 -> 11 deg). The
  comparison is not vacuous: it diffs `projViews[0].pose` against `g_consumedHeadQuat`, which
  the game-thread camera drive publishes independently. `g_viewsContent` already holds the
  previous locate generation deliberately, with a comment describing this exact artifact.

**NEXT AND MOST SPECIFIC: the two eyes may be tagged with different ORIENTATIONS.**

The pose audit only inspects `projViews[0]` - the left eye. Nothing checks eye 1. Under
SequentialReentry both eyes are rendered in ONE game tick from ONE head sample, differing only
by an IPD position offset, so both layers must carry the SAME orientation. But the stereo
branch tags each eye with `g_eyePose[eye]`, a pose stored at THAT eye's capture, and the two
captures happen on different presents. If those orientations differ, the compositor reprojects
the two eyes differently, the images disagree during a turn, and that reads exactly as the
world warping - independent of FOV, and consistent with the left eye's pose being provably
correct. Check: log `projViews[0].pose` vs `projViews[1].pose` orientation during a turn; a
non-zero orientation delta is the bug. Fix shape: tag both eyes with the single orientation the
tick was rendered from, keeping only the position offset per eye.

**Superseded hypothesis (kept for the record): the submitted POSE, not the submitted FOV.** A projection
layer is submitted with both a pose and an fov; if the pose handed to the compositor does not
match the pose the frame was actually rendered from, the compositor reprojects incorrectly and
the world swims as the head turns - and that error is completely independent of the FOV value,
which is exactly the observed signature. Look at `projViews[eye].pose` versus the pose the
camera drive used for that frame, including staleness across the pair-hold.

Secondary: the claim's source was observed as `src=live`, meaning it latched a measured
tangent instead of deriving from the option, and was still reporting the option-130 value
after the slider moved to 100. Stale regardless of which formula is correct.

**Session 27 stage plan** (branch `s27-b1r-stability-and-resolution`; stage 1 done):

1. **In-headset re-test of stage 1** (user, BS1). This is the release gate for a stability
   hotfix. Checklist: launch, VR PRESET 1, play a few minutes across a level load. Expect
   `host build VERIFIED`, one `UShockUserSettings scan via heap` line in tens of ms, a
   1.000 s camera heartbeat metronome with no gaps, exactly 4 `engine exec` lines per boot,
   and no `IMBALANCE` / `exec fault` / `crash:` lines. Also: change the game's video
   resolution once mid-session and confirm `XR swapchain rebuild QUEUED` followed by
   `performing the queued XR swapchain rebuild` and a clean recovery - that path only
   matters with a live session, so it cannot be verified flat.
2. **Stage 2, resolution / FOV / apparent scale.** Blocked on two measurements before any
   code: (a) the world-lens aspect law, which needs a non-16:9 backbuffer and therefore the
   ini lane plus a relaunch; (b) the FOREGROUND-lens aspect law, decoded from the fg draws'
   cb0 block at a non-16:9 resolution. (b) decides between two contradictory fixes: if the
   fg vertical half-tangent stays `tan(F/2)*3/4` and the horizontal follows the live aspect,
   then `camera.cpp:1202`'s hardcoded `0.75` is `(4/3)/(16/9)` and is wrong everywhere else,
   magnifying the viewmodel by `1.7778/aspect` (1.78x at a square backbuffer, which is what
   the README currently tells users to run) - and fixing it restores the session-16
   in-headset calibration rather than invalidating it. If instead the fg is
   aspect-independent, `0.75` is always right and "hands are huge" is something else. Do NOT
   ship the lens change before that dump. Then: derive the target from
   `xrEnumerateViewConfigurationViews` (never called today), write the game ini's four
   viewport keys plus the FOV locks with read-back verification, add `vrres` and overlay
   presets, and keep the 129.5 circumscribing claim as the 16:9 safety net.
   Extra hazards to close before any live resize lane: device-identity and hwnd-identity
   checks in the overlay, and `input_drive`'s `g_armed` re-arm on a client/viewport rebuild
   (without it motion controllers die silently after a resolution change).
3. **Stage 3, cutscenes and the aim dot.** Measure the black-bar mechanism first: the Nexus
   "Fullscreen Cutscenes" mod is a `HUDPC.swf` edit that zeroes a sprite named
   `WidescreenBars`, which contradicts our own session-22 dump reading (engine clears to
   black and draws a vertically shrunken tonemap quad). Install it, replay the Gatherer's
   Garden Electro Bolt sequence, and see whether the bars go and whether the content is
   squeezed. Then either suppress that one gameswf draw or fix the unsqueeze. Add `vrcine`
   modes off / authored / authored+look, gating the hands, aim and laser drives on the
   letterbox exactly as the head drive already is (closes ROADMAP's session-22 round-5
   item - the authored cinematic hands currently only survive when the controllers are
   idle). Add a single toggleable aim dot on the verified aim ray.
4. **Stage 4, SteamVR/OpenVR.** SteamVR has never shipped a 32-bit OpenXR runtime, so
   Lighthouse and Steam Link users cannot start VR at all; stage 1 now says so in the log.
   Real fix is a native OpenVR backend behind the `bvr::vr` facade. Independently,
   `openxr_input.cpp` suggests bindings for only `oculus/touch_controller` and
   `khr/simple_controller`, so on SteamVR with Index or Vive wands, or on WMR, sticks,
   triggers, grips and every face button are dead even when a session does start - add
   those profiles.
5. **Follow-ups opened by stage 1**: root-cause the `SETRES` fault; an `offsets.ini`
   override so a non-Steam user can supply their own RVAs without a release; deriving
   `GObjObjects` to retire object scanning entirely; the config echo block, proxy breadcrumb
   and OpenXR API-layer enumeration from the diagnostics list.

**Carried from session 26:**

1. **Re-test the hang fix** (user, both games): put the headset on, get FOCUSED, then
   take it off / let it idle for a minute, then put it back on. Expect one
   `xr: pacing SKIPPED while VISIBLE` line, the flat window to keep running, and
   `xr: FOCUSED again after N ms` on return - and NO freeze. Also worth one
   Alt-Tab-away-and-back cycle.
2. **BS2 viewmodel/hands in stereo** (the user's one reported blemish, "same as BS1").
   Bounded diagnostic FIRST, do not assume the BS1 rig: BS2's foreground almost
   certainly renders from a camera the SR pass-2 replay never touches (the Draw-head
   probe already shows the cached camera-actor fields at viewport+0x48 hold a
   DIFFERENT, static value than the live CalcView camera - `camSrc` in the beat line
   never moved while the real camera did). If the fg scene node takes its view from
   that path, both eyes get one camera and the weapon reads at the wrong depth -
   which is exactly what "weird and a bit wrong" looks like. BS1's answer was the fg
   scene-node ctor hook + per-eye camera substitution (`vrfgnode sync`); check
   whether BS2 needs the same before porting any of it.
3. **BS2 input / motion controllers** - the real playability unlock (M5-equivalent).
   Core's synthetic-XInput lane is game-agnostic, but BS1 also needed an engine-side
   drive (`input_drive.cpp`, UWindowsViewport::UpdateInput) because nothing calls it
   in windowed mode; whether BS2 needs the same is unprobed.
4. If stereo ever proves unstable under long play: the 1t fallback derivation entry
   points are banked in ENGINE_NOTES (render sync pair 0x1A69294/98, endframe fn
   0x501EA0, reader 0xB929F2).
5. BS2 combat-scene stereo perf profile (needs a combat save); BS2 load-crossing pass
   with stereo armed (the menu needs a human driver).
6. Record the BS2 FOV slider min/max endpoints when someone is in Graphics Options.
7. BS1 carries: v0.4.1 to the external tester, 2K-account lead, seated recenter
   designs, letterbox investigation.

## Previous state (2026-07-29, session 25 - M10: BS2 FOV DONE flat - readback claim == rendered, forced-headset-FOV write default OFF, discovery tooling ported, crash-dump loop fixed - branch s25-m10-bs2-fov)

**The BS2 FOV-claim mismatch is CLOSED - in-headset ACCEPTED (user, Quest 3 / VDXR).**
`UShockUserSettings.HorizontalFOV` derived fresh at **+0x4C** (BS1's +0x8C reads 3 on BS2 -
the never-copy rule earned its keep), readback wired into `vr::set_rendered_hfov` every
CalcView, `vrfov`/`gfov` write levers landed DEFAULT OFF, capabilities now 0x3. User verdict:
**fisheye GONE, world-drag GONE**; viewmodel unremarkable (nothing looked wrong = the
no-fg-defect flat finding holds in-headset); the Esc-pause save/restore edges were exercised
repeatedly in-headset (the 19:37 ON/OFF cycles in the session log ARE the user's pause tests
- each pause restored option 100, each resume re-armed). vrfov wrote 131 on this rig
(headset-derived, effectively BS1's 130); the manual gfov lever now defaults to 130 for BS1
parity. Two user observations correctly deferred: world scale cannot be judged or perceived
in mono (slider works - flat-proved - but perceived size needs stereo's IPD ratio; already
the session-24 call), and vrstereo/SR does not exist on BS2 yet (that IS the next
milestone).

### 1. THE HEADLINE: BS2's foreground follows the world FOV natively - BS1's fg apparatus stays unported

The session-24 policy gate ran FIRST and returned the best possible answer: poking the FOV
option re-lensed the drill viewmodel WITH the world (screenshot pair, ENGINE_NOTES). BS1's
entire foreground counter-model - fovA/fovB, kFgEyeComp, vrfgfov, the lock/lockpull domain -
exists to fight a fixed-lens fg rig that BS2 simply does not have. None of it ports. BS2's
FOV milestone is readback + gated write + nothing else.

### 2. The derivation (session 25, full chain in docs/bioshock2/ENGINE_NOTES.md)

- Vtable 0x11523D8 runtime-VERIFIED via the new `vtscan` probe before anything trusted it
  (3 matches: 2 stack slots + the real heap object).
- Offset by ini-adjacency (`MouseIconScale=10` immediately precedes `HorizontalFOV=100` in
  Bioshock2SP.ini's [ShockGame.ShockUserSettings]; the object mirrors it at +0x48=10,
  +0x4C=100) + poke proof: 100->130 img-diff 8.99 mean-abs / 39.3% changed vs a 1.34 / 5.0%
  restore floor. Renderer-consumed per frame, no APPLY, no code clamp through 150.
- Production locate: heap scan for the vtable (BS1 shape) + 3-miss DORMANCY from day one
  (the b2r scanner bakes in the session-22 lesson BS1's own settings scanner predates);
  re-armed on view-state changes. Measured ~0.4 s at menu, ~3 s in gameplay (Debug).

### 3. What landed in code (branch s25-m10-bs2-fov, 5 commits so far)

- b2r command seam: full memscan/pokeaddr/hexdump/fsweep/strscan/membases/dumpframe family
  (verbatim BS1 routes) + the b2r-first `vtscan <hexRva>` candidate-vtable verifier.
- b2r patterns: `scan_for_vtable_object` (full-4GB LAA walk, SEH-guarded) + `hfov_option_ptr`
  (cache + vtable revalidate + 2 s rate limit + 3-miss dormancy + `hfov_scan_rearm`).
- Readback: every CalcView claims the live option (menus included); missing object claims 0 =
  core's explicit fallback signal (bit-identical to pre-readback behavior). Heartbeat gained
  `fov=N`; `fovaudit` ported (simple form); overlay shows the option + Force-headset-FOV
  checkbox + manual game-FOV write controls.
- Write block: BS1 shape - `vrfov` (headset-suggested hfov) wants strictGameplay AND vrDrove;
  `gfov` (manual) wants strictGameplay; one-shot save/restore of the user's option; the
  CalcView-silent stale-restore ticks from the ProcessEvent detour (BS2 has no scenedraw
  hook). Adapter advertises CAP_FOV_WRITE; `setFov` routes to the manual lever.
- Core fix (only BS1-shared file touched): crash.cpp repeat-fault suppressor + 3-dumps-per-
  session cap. Motive below.

### 4. Flat gates all PASSED (log-measured)

Scan one-shot at menu boot, correct object chosen from 4 matches, heartbeat `fov=100`;
`fovaudit` option=100 with option-derived tanH=1.191754 == tan(50 deg) exact (src=none only
because no XR session was up - readback claim is stored); `gfov 120` at the MENU correctly
refuses to write (strictGameplay gate); `vrfov status` reports force=off suggested=0.0
writing=0. Gameplay-side write gates (ON/OFF edges, stale-restore) still need a loaded save -
queued for the user session below.

### 5. Incident: the session-24 build crash-looped and wrote 115 GB of dumps

The user's BS2 run crashed at 18:20 (null read, `Bioshock2HD.exe+0x4FF0FE`, after ~11 min
idle in gameplay, headset in VISIBLE-idle) - and Steam's CSERHelper filter CONTINUE_EXECUTIONs
the fault, so our chained filter re-dumped the SAME fault once per second for 40 minutes:
2,083 dumps, 115 GB. Cleaned to two exemplars (`bs2\crash\bvr_20260729_182015.dmp` +
`_185932.dmp`); crash.cpp now suppresses repeat reports at the same address and caps minidumps
at 3/session. The crash itself is UNTRIAGED - dump kept, RVA 0x4FF0FE, registers in the log
around 18:20:15.

### Next steps as of session 25 (superseded - the stereo item became session 26; live items carried into the list above)

**STANDING POLICY (user directive, session 24): BS2 is not bound by BS1's methods** - check
native first, test whether the BS1 problem exists, port only what is proven necessary. Full
entry in the ARCHITECTURE decision log + CLAUDE.md hard rules. Session 25's fg verdict is the
first applied case.

1. **BS2 M4 stereo** (the user's top ask - "jitter 3D" parallax already reads well in mono,
   and world-scale tuning is blocked on it): re-derive the render substrate. Session-25
   recon says BS1's STATIC submit-finding method is dead on BS2 (SetEvent is
   wrapped/virtually dispatched, zero static callers of the wrapper) - so start LIVE: port
   BS1's SetEvent caller sampler (`reentry kick` instrument), sample the game-thread submit
   caller, walk back to the entry offline. Pump-loop candidates 0x7C3E40 / 0xCF3EE0 banked
   in ENGINE_NOTES. `dumpframe full 2` decode-check on BS2 is a cheap first probe. Per the
   policy: check whether BS2 offers a less invasive doubling path before assuming
   SequentialReentry's exact BS1 shape.
2. Record the BS2 FOV slider min/max endpoints next time someone is in Graphics Options
   (ENGINE_NOTES gap, cosmetic).
3. **Triage the BS2 idle-gameplay crash** (dump kept, RVA 0x4FF0FE) - one-off or a class?
   BS1's session-23 offline-disasm triage recipe applies.
4. Menu-load view actor class (vtable RVA 0x106EE20) still not RTTI-identified.
5. BS1 carries: v0.4.1 to the external tester (crash capture), 2K-account lead, seated
   recenter designs, letterbox investigation.

## Previous state (session 23, superseded - CLEAN-MACHINE NEW-USER FLOW + CRASH CAPTURE: Steam's CSERHelper was eating our crash handler, load-crossing stereo drop fixed, thumbrest ammo modifier, v0.4.1 packaged - branch s23-crash-diagnostics)

**v0.4.1 IS RELEASED (2026-07-29):** PR #11 merged to main, tagged, published at
https://github.com/mohamad-balouza/bioshock-vr/releases/tag/v0.4.1 (zip 1,215,780
bytes, sha256 026EF542C1D68232..., DLL build id stamps exactly v0.4.1). Built for
the external tester: their next crash produces a rich dump despite Steam's
CSERHelper displacing our filter, and prev.log survives their relaunch reflex.

### 1. THE HEADLINE: Steam's crash reporter displaces our exception filter

An external tester's v0.4.0 crash produced **no dump and no log line at all** - the log
just stops mid-heartbeat. That is not a crash we missed; it is a crash we were never told
about. `SetUnhandledExceptionFilter` is global last-writer-wins and we install at DLL
attach, so anything that installs later silently owns the slot.

**Reproduced locally within 30 s of adding a re-arm check:**
`crash: our exception filter had been displaced by 708C2571 [CSERHelper.dll+0x12571]`.
`CSERHelper.dll` is Steam's Crash Error Reporting Helper, and it is in the tester's module
list too. It explains everything: their v0.2.0 crash DID produce a dump (it happened
before Steam took the slot), and their v0.4.0 crashes at ~94 s produce nothing.

Fix: `crash::rearm()` from the present loop (~every 10 s) takes the slot back and chains
to whoever displaced us, so Steam still gets its report. Plus a vectored handler for the
always-fatal codes that never reach a filter at all (heap corruption, stack overflow,
`__fastfail`), and a `DLL_PROCESS_DETACH` breadcrumb so an orderly exit is distinguishable
from a hard kill (verified absent on TerminateProcess; the orderly path is NOT yet
verified end to end).

### 2. The external report - ATTRIBUTED, still not root-caused

Tester's original artifacts were a **v0.2.0** run, confirmed later in their own words:
they tested v3 and v4, both crashed, then re-tested v2 and sent THAT log, not knowing the
log is truncated every launch. Three methods had already agreed (PE TimeDateStamp byte-matching
the v0.2.0 asset, the `0.1.0` banner, absence of `APlayerWeapon scan:`). Method + a
per-release fingerprint table are in ENGINE_NOTES session 23.

The v0.2.0 dump: DEP execute violation, CPU fetching from non-executable game heap in the
FName table beside an `FThreadLockStepExecution` object, on a GAME WORKER thread in the
engine's `FSynchronize` dispatch (`BioshockHD.exe+0x7CCF40`), no bioshockvr.dll frame,
13 s after the last CalcView. Address-space exhaustion RULED OUT (exe is LAA, 1.88 GB of
~4 GB). Root cause still unknown - `MiniDumpNormal` captured no heap.

Latest v0.4.0 capture (2750x2850, unchanged resolution): 94 s at the menu during the 2K
account connect, steady 90 calls/s, then the log ends. `fflush` is per-line so nothing was
lost - the process simply vanished. Consistent with the CSERHelper finding.

### 3. Clean-machine new-user flow - PASSED

Full wipe (both DLLs + `%LOCALAPPDATA%\BioshockVR\`, preset backed up + sha256 manifest
to `Documents\BioshockVR-backup-20260729-142724\`), vanilla baseline, then the PUBLISHED
v0.4.0 zip installed by its own README - two DLLs, no preset, shipped defaults. Clean boot,
7.3 min menu soak flat with no crash, then user play-tested in-headset. Preset restored and
hash-verified afterwards.

### 4. Load-crossing stereo drop - USER-REPORTED, REPRODUCED, FIXED

After VR PRESET 1 at the menu, loading a level dropped stereo to "VR but not 3D" until the
preset was pressed again. The reentry watchdog's deadlock signature is satisfied honestly
by a level load, so it auto-disabled stereo. Now it stands down while
`hud::screen_only()` is true, and an auto-off re-arms once the pipeline moves again with
depth 0 for 500 ms; a deliberate `vrstereo off` clears that. Verified flat: the crossing
that previously logged `deadlock state detected` at +1.0 s and `stereo auto-off` at +2.0 s
now logs neither, doubling still live. Coverage knowingly reduced: the menu also trips
screen_only, so detection is inert there.

### 5. Thumbrest ammo modifier - SHIPPED, user says "perfect"

`/input/thumbrest/touch` is core to `oculus/touch_controller`; VDXR 1.0.10 reports it.
It is ONE BIT - no x/y, so directional flicks are impossible (that idea comes from
Vive/Index TRACKPADS). Weapon switching stays on the grip. LEFT thumbrest is the modifier
with slot directions still on the RIGHT stick (a thumb cannot rest on a pad and push the
stick beside it). Default thumbrest; overlay combo + `vrinput ammomod`; stick click keeps
working until a real thumbrest touch is seen, so Pico-class controllers are safe.

### 6. Diagnostics, now that we know how expensive their absence is

Version generated from `project(... VERSION)` + `git describe` (the old `#define` shipped
"0.1.0" for three releases); startup logs PE TimeDateStamp + an `env:` line; dumps carry
heap-adjacent memory and data segs; crash log gets registers, a symbolized stack and
read/write/DEP classification; `bioshockvr.prev.log` survives one relaunch (the exact trap
that cost this session); `tools/package.ps1` + in-repo `release/preset/` make releases
reproducible and refuse to package a version mismatch.

### 7. For users: match resolution to the headset's per-eye aspect

The eye swapchain is sized from the game backbuffer, so 16:9 renders a wide strip the
headset discards - at 3840x2160 on a Quest 2 only ~54% of the width is inside the FOV. A
near-square 2750x2850 has FEWER pixels, is sharper and runs faster. Found independently by
the tester; math in ENGINE_NOTES, guidance now in the README.

### Next steps as of session 23 (superseded - live items carried into the session-24 list above)

0. **Late session-23 findings (after the handoff above was first written):** a second
   external dump is a DIFFERENT crash - main-thread use-after-free (0xDEDEDEDE poison)
   inside VDXR->d3d11 frame submission, no mod frame on the stack. Shipped a guard:
   same-size ResizeBuffers no longer destroys the XR swapchains (their machine issues
   one mid-session 7 s after FOCUSED). Also: the v0.2.0-works premise is dead (their
   first dump IS a v0.2.0 crash), fullscreen is not the discriminator (both dump-
   producing runs were windowed=1), and their CalcView rate oscillates 90/s <-> 7000/s
   (pacing lost/regained - unexplained). Full detail in ENGINE_NOTES session 23
   addendum. Still open: defer real-size-change swapchain teardown to a safe point in
   the frame loop; the pacing oscillation.

1. **Get v0.4.1 to the tester.** With the filter re-arm their next crash should finally
   produce a dump with heap + data segs, registers and a symbolized stack. Ask for
   `bioshockvr.log`, `bioshockvr.prev.log` and the `.dmp` BEFORE relaunching.
2. **Chase the 2K-account lead** - they report account connect is very slow on v0.3.0 and
   v0.4.0, and that is the window they die in. Worth checking what the mod does during the
   menu that could interact with the Steam/2K network path.
3. **Seated vs standing recenter** - deferred by the user; three designs in the ROADMAP
   post-v1 backlog.
4. Letterbox bars investigation (carried from session 22, still open).
5. Checklist sections B/C/E were never formally walked item by item.

### 1. CINEMATICS (plan item 1) - both on-file hypotheses DISPROVEN, the real fix shipped

Measured on the user's crash-site save (two full descent replays,
vrstereo on): **CalcView keeps firing the whole ride with a ShockPlayer
view actor** (zero `view state` transitions - strict/staleness detectors
never fire), **the renderer consumes our camera writes mid-scene** (world
cb0 camera == CalcView heartbeat loc to the UU), and **the scene renders
its OWN fov - 104.0 deg - while the option (and the projection claim)
says 130** (dump decode: one tangent cluster 1.2800/0.7200 both eyes;
control after arrival: exactly 2.1445/1.2063 = option 130). The 26-deg
claim-over-render mismatch IS the fisheye AND the broken fusion. Full
story + traps in ENGINE_NOTES session 22.

Shipped (commit 9a85856): a **live rendered-fov watch** in hud_capture
(per-present cb0 tangent decode off the first scene draw - async 80-byte
staging copy, DO_NOT_WAIT map, zero stalls; `[hud] rendered-fov mismatch
ON/off` transitions; `fovaudit live:` echo) and the **cinematic
fallback** in on_present_end: predicate = strict-false | publish-stale |
fov-mismatch | screen-only, 3-present hysteresis, `vrcine
on|off|mode|status` + counters. While active: projection drops to the M2
quad screen, eye offsets + live head drive suppress (renderer consumes
them mid-scene - dump-proven, so the quad would jitter otherwise), the
HUD quad/laser/HUD-gate all self-adjust per present. The FOV write is
strict-gated with a BuildDetour-side restore for true CalcView-silent
scenes; SR pass-2 gained a 100 ms base-age gate (stale-replay hazard).
**DEFAULT = `mode stereo` (the user's call): fov-mismatch scenes KEEP the
stereo projection and the claim follows the MEASURED fov - full stereo
cinematics with head-look; the overlay toggle "Cinematics as stereo
projection (off = big screen)" is the in-headset A/B.** Flat gates: one
mismatch ON at the lever pull (104.0 vs 130.0), one off at arrival, live
tangents == dump decode to 1e-4 in both states, zero false transitions
across pause/loads, heartbeat 2x clean, dumps 9->9.

### 2. FULLSCREEN SCREENS/EFFECTS (plan items 2+7) - per-kind routing, dump-grounded

Evidence (dumps on file, fingerprints in ENGINE_NOTES): loading screens
= ~87 pure-gameswf draws with ZERO DrawIndexed (no world pass, nothing
classifies - hence full-FOV in headset); the HACK minigame = same family
(322 draws, 0 DrawIndexed; the user's "same as the loading screen" was
exact; the main menu = 125); the alcohol-blur composite = the second
post-tonemap draw sampling a BACKBUFFER-SIZED texture from the engine
post path, vs HUD draws sampling 2048x2048 BC atlases.

Shipped (commit e4425e6): **screen-only interval detector** (swf draws
with no scene-vote leader, hysteresis + logged transitions) feeding the
cinematic fallback - hack/loading/menu/FMV-class screens go to the
READABLE quad screen regardless of the stereo/quad mode; **in-frame
post-effect skip** (post-tonemap draw with backbuffer-sized srv0 never
redirects - srv desc cache, `postFx` counter in vrhud status). Flat
gates: two save-loads produced exactly one screen-only ON/off pair each
(87/120-draw intervals); the live hack board fired the detector with the
user at the machine; postFx = 0 across normal gameplay (zero false
positives), ~1/interval on the pause menu = its fullscreen DIM layer
(correctly in-frame - the menu PANELS keep redirecting; flagged on the
in-headset list as a look-check). The Big Daddy FMV has no flat repro -
its routing verdict is on the headset checklist.

### 3. FIRST-BOOT RESTART KILLED (plan item 3) - virgin-install gate PASSED

compose_over answers a failed slot-0 GetState with a neutral CONNECTED
pad while vrinput is off; the vrinput.on marker machinery is deleted
(the orphan file is inert) and the README restart note is gone. Virgin
boot (all inis + marker set aside, restored byte-verified after): main
menu shows NO pad glyphs with the phantom pad idle (prompts key on
last-used input - the session-9 model held); `vrinput on` mid-session
put the IAT lane at ~680 polls/s instantly and a dpad press moved the UI
- no restart. TRAP for counter-readers: the proxy-lane getstate[0]
counter stays at 6 FOREVER by design (Steam overlay swallows that lane
post-boot) - it is not a poll oracle; the IAT counter is.

### 4. HEAD-ROLL EYE SEPARATION FIXED (plan item 4) - exact

apply_eye_offset now offsets along the FULL rotation's right axis
(ue_rot_basis); the AER path shares the one implementation. Flat gate
(`fovaudit eyes`, simhead roll sweep): eye delta (6.300, 0, 0) at roll
0 (bit-identical to the old formula by construction), (4.455, 0,
-4.454) at 45, (0.000, 0, -6.300) at 90 - the separation rotates
exactly with the head.

### 5. TURN CONTROLS SHIPPED (plan item 5) - exact

Smooth-turn scale on the composed stick X (pitchkill gate cluster -
gameplay only, lifts in radials) + snap turn (0.65/0.30 edge hysteresis
-> queued steps consumed at the camera's transfer point: recenter
composite shifts, the M7.5 transfer carries the body). Flat gates: +1
then -1 pulse moved camYaw 49152->57344->49152 = +-8192 units (+-45.00
deg) EXACT round-trip, ONE step per pulse even on a 2 s full deflection;
turnscale 2.0 turned test rx 8000 into composed 16000 exact. Knobs
turnScale/snapTurn/snapAngleDeg persisted in vrpreset.ini + overlay
("Smooth turn speed", "Snap turn", "Snap angle").

### 6. MOVEMENT-WONKINESS INSTRUMENT (plan item 6) - shipped, investigation OPEN

`vrinput sticklog on` logs the FINAL composed pad (post
merge/pitchkill/turn) at 10 Hz; `last_composed_sticks()` exists for the
recorder. The real capture wants a HEADSET walk (composed stick vs
`vrbody probe on` resid lines) - flat synthetic sticks cannot reproduce
controller noise or head-micro-steering. Queued on the checklist;
analysis next session. Do not tune blind.

### Session-22 harness notes (new traps)

- **The descent ride WRITES a "Welcome to Rapture (AutoSave)"** that
  becomes the newest save: after one replay, CONTINUE lands at the
  arrival dock, NOT the crash-site repro (it drops to second in LOAD).
- A save LOAD produces NO view-state transition and NO fov-write churn
  (strict stays true through load screens on this box); loading screens
  show up as screen-only ON/off pairs instead.
- Flat fire tests need a pose source: with simhead/aim-test lanes
  expired the fire seam counts skips (correct fail-soft), not subs -
  re-arm `simhead` + `vraim test r` before counting subs.
- The plane-intro trap refined: pressing START during the wallet scene
  SKIPS toward the crash - recover via the pause menu LOAD instead.
- The old vrinput.on marker no longer pre-arms anything (code deleted).

### Session 22 round 2 (same night) - THE HEADSET FEEDBACK ROUND

**The user's verdicts, run 1:** no regression; stereo cinematics "amazing"
(104-not-130 understood and accepted); alcohol blur "perfect"; hacking
"amazing, works as intended" after the same-night head-lock fix (the first
try was world-locked at the recenter facing - screen-only intervals now
ride the head-locked view space like the pause panel); head-roll fixed;
turn controls "perfect". Movement wonkiness ROOT-CAUSED BY THE USER: their
controller's up+right stick diagonal is dead (all other directions fine;
mod stick path audited clean - deadzone/clamp direction-agnostic); they
will cross-check the hardware with another tester/game. Remaining asks all
landed the same night:

1. **The "Big Daddy plasmid FMV" class = ENGINE-CINEMATIC LETTERBOX**,
   dump-decoded on their prepared Gatherer's Garden save: the engine
   clears the final target opaque-black and tonemaps the scene into a
   vertically SHRUNKEN quad - bars are unpainted clear (nothing to
   classify), content is anamorphically squeezed, world renders full
   130 (mismatch=0), CalcView stays strict-GAMEPLAY with the AUTHORED
   camera choreography. Shipped the LETTERBOX WATCH (3-column exact-black
   backbuffer sampling, async staging, 5-interval stability, full-black
   fade guard) + two consumers: the submitted subImage (projection AND
   quad) CROPS to the band - the compositor unsqueezes, bars gone,
   geometry correct - and the live head drive SUSPENDS while the
   letterbox holds (authored camera plays like flat; eye offsets stay =
   stereo cinematics). Flat gates: the injection sequence's phases
   tracked exactly (ON 165/185 px -> blackout REJECTED via the full-black
   guard -> ON -> final off), boot attract caught/released (222/285),
   90 s+ post-sequence gameplay soak with zero false positives, dumps 9.
2. **The user's new L-hand calibration rescued via `vrpreset save`**
   (they had not pressed save) and BAKED as code defaults: trim
   -7.5/+37.0 (offsets unchanged). Their turn prefs (scale 2.56, snap on,
   45 deg) persisted to their ini (not baked - shipped defaults stay
   1.0/off/45).
3. **Laser OFF by default + preset**: the preset no longer arms it; the
   persisted `laserOn` ini key (written by "Save preset values") applies
   the user's choice, so opting back in sticks. Virgin default = off.
4. **F10 overlay hardening** (user: overlay vanished while scrolling,
   dead until alt-tab): (a) backbuffer-identity watch recreates the RTV
   when the game swaps buffers without ResizeBuffers (the "invisible
   overlay, F10 toggles nothing visible" state); (b) the WndProc now
   CONSUMES mouse/keyboard messages while ImGui wants them (the game was
   fighting the overlay for the wheel/clicks); (c) new `vroverlay on|off`
   seam = remote/emergency toggle. Root cause unconfirmed (not flat-
   reproducible on demand) - the user re-checks scrolling next run.
5. **Harness trap from the marker retirement**: nothing pre-arms vrinput
   at boot and test presses only compose while it is ON - boot.ps1 now
   sends `vrinput on` before its A-press loop (it timed out otherwise).

**Still open before the release**: the user's in-headset re-verify of the
letterbox class (their prepared save; both cinematic modes), the overlay
scroll behavior, and their controller cross-check. After the release: the
CLEAN-MACHINE new-user flow session (user's ask - wipe, install from the
release zip, fix whatever breaks until out-of-box works).

### Session 22 round 5 (2026-07-29, pre-release) - LETTERBOX BARS PARKED OPEN, user's call

Three in-headset rounds could not remove the cinematic letterbox bars
(imageRect crop ignored by VDXR; the capture-side unsqueeze executed -
`unsqueeze live` logged - but the user still saw bars; mechanism
UNRESOLVED). The user called STOP: ship the release, tackle the bars next
cycle. Shipping posture: the unsqueeze is DEFAULT OFF (`vrcine unsqueeze
on` = the experiment lever); the letterbox DETECTOR, the authored-camera
drive suspension and the flash-native in-frame rendering stay live (the
parts the user verified; the flat frame is pixel-vanilla by construction).

**Two documented open items for the next release:**
1. **The bars themselves** - where do the user-visible bars actually come
   from with the unsqueeze provably drawing? Next tools: capture the eye
   swapchain content (not the backbuffer) during letterbox; check whether
   VD's own compositing letterboxes; A/B `vrhud off` + `vrcine unsqueeze
   on|off` in-headset with 30 s verdicts per toggle.
2. **Hands drive during cinematics** - the user sees the CONTROLLABLE rig
   hands instead of the authored cinematic hands whenever the controllers
   are awake (the run where authored hands showed = controllers idle in
   the lap; not a build regression). Fix: suspend the hands/aim/laser
   drives while the letterbox holds, exactly like the head drive.

Also fixed this round (user-found while flat-playing): the weapon
resolver's scan fallback froze the game for seconds every ~2000 frames
FOREVER on weaponless saves (early game) - now fully dormant after 3
failures, re-armed by the cheap rig reads (3 scans then a 1.000 s
heartbeat metronome, measured). This was the "game freezes every couple
of seconds" report and it is exactly the new-user first-hour path.

## Previous state (2026-07-28, session 21 - RENDER SYNC: the fov audit came back clean, the fg scene decoded down to its ctor args, the fovA zoom-pull lever found, per-weapon profiles shipped - branch s21-render-sync)

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

## Next steps (post-session-22; the release gate is the in-headset checklist below)

1. **THE IN-HEADSET RUN (the release gate).** Everything in the
   session-22 checklist below; headline verdicts: stereo cinematics vs
   big screen (the overlay toggle - user picks the default that stays),
   the hack/loading screens on the readable screen, the Big Daddy FMV
   routing (no flat repro existed), alcohol blur in-frame, head-roll
   stereo, snap/smooth turn feel. The next release cuts ONLY after the
   user's explicit go.
2. **Movement-wonkiness capture + analysis (instrument shipped, data
   missing).** On a real headset walk: `vrinput sticklog on` + `vrbody
   probe on` (+ optionally `vrrec start/stop`), walk + look for ~60 s,
   send the log. Then decide bodyRate smoothing vs stick curve vs
   hardware noise from the numbers - not blind.
3. **If a scene class surfaces that defeats all four cinematic legs**
   (strict/stale/fov-mismatch/screen-only): grab `dumpframe full 2` +
   `vrcine status` during it - the counters + fingerprints classify it.
4. Then (unchanged queue): pawn-eye anchoring, off-hand tracking +
   two-handed weapons, wrench gesture. The fovaudit-submit/poseaudit
   glance remains a free 30 s check on any headset run.

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

### IN-HEADSET CHECKLIST - cinematics + routing + polish (session 22)

Setup as always: Quest 3 + Virtual Desktop (VDXR), launch from Steam.
**SAVE TRAP: CONTINUE now lands on the "Welcome to Rapture (AutoSave)"
from the flat descent replays - your crash-site save is SECOND in LOAD,
Medical Pavilion third.** Press **VR PRESET 1**. Live A/Bs: the
"Cinematics as stereo projection" overlay checkbox (= `vrcine mode
stereo|quad`), `vrcine off`, `vrhud off`, `vrinput snap on|off`.

1. **Non-regression sweep (60 s, first).** Park the hand, look around,
   stick-walk, turn past 90 both ways, two right-trigger pulls + an
   Electro Bolt, pause menu once. NOTE the pause menu changed slightly
   BY DESIGN: its fullscreen DIM layer now stays in the world (per eye)
   while the panels stay on the readable quad - report if it reads
   worse than before.
2. **THE DESCENT (the headline).** Load "The Crash Site", pull the
   lever, ride down TWICE - once per cinematic mode:
   (a) DEFAULT stereo mode: the ride should be full stereo WITH
   head-look and NO fisheye (the claim now follows the measured 104);
   (b) untick "Cinematics as stereo projection": the ride plays on the
   big virtual screen instead (world-locked, sized by the old Screen
   distance/width sliders). PICK THE DEFAULT YOU WANT - one checkbox,
   tell me the verdict. The log echoes `xr: cinematic quad ON/off` and
   `[hud] rendered-fov mismatch ON/off` for me either way.
3. **THE HACK (your confirmed-broken case).** Hack the vending machine:
   the board must now sit on the READABLE screen (not sprayed across
   the FOV). Loading screens same. Main menu after quit-to-menu: also
   on the screen - confirm it is navigable by controller.
   *Headset round 1 (2026-07-29): worked but the screen was world-locked
   at the recenter facing - the user had to turn back to it. FIXED same
   night: screen-only intervals now ride the HEAD-LOCKED view space
   (like the pause panel), centered wherever you look; cinematic scenes
   keep the world-locked screen. Re-verify.*
4. **THE BIG DADDY FMV** (when you next pass one - no flat repro): it
   should land on the screen, not in the small HUD box. Report either
   way.
5. **Alcohol blur:** drink something - the blur must be IN the world
   (both eyes, on the camera), not on the HUD panel.
6. **HEAD ROLL:** tilt your head to a shoulder while looking at
   near-field geometry - stereo should stay comfortable/fused (pre-fix
   it went weird sideways).
7. **TURN CONTROLS:** feel the "Smooth turn speed" slider; then "Snap
   turn" on - discrete 45-deg steps, one per flick, hands/body carried
   exactly (no drift, no double-steps). Adjust the angle slider to
   taste; "Save preset values" persists all three.
8. **First-boot sanity (already virgin-gated flat):** controllers came
   alive with the preset on THIS boot with no restart - just confirm
   nothing about controller pickup felt different.
9. **Hands offset sliders (verify-only):** "Tuning hand: L / R" + six
   sliders exist and move the model per hand (shipped session 18).
10. **The wonkiness capture (2 min, for next session):** `vrinput
    sticklog on` + `vrbody probe on`, walk around normally ~60 s,
    `vrinput sticklog off`. That log is next session's analysis input.
11. **Anything off:** `vrcine off` kills the whole cinematic layer;
    `vrhud force off` + `vrhud on` resets the HUD path; if a symptom
    survives both, it predates this session.

### PREVIOUS CHECKLIST - render sync + per-weapon profiles (session 21)

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
  our process, so values Ã¢â€°Â¥4 GB truncate. Cosmetic; ignore.
- itsloopyo's headtracking mod also installs as `xinput1_3.dll` - mutually exclusive with ours
  (install.ps1 backs theirs up automatically).

## Session log (newest first)

### Session 58 (close-out, same day) - HEADSET ACCEPTED, merged to bioshock-infinite; s59 = the missing-hands scripted-beat class

User verdict on the full TESTING "S58" checklist: "everything worked as
expected" - head-directed targeting, the F10 A/B, the complete raffle
chain (no stalls), doors/kinetoscopes, sharpness sweep. Fast-forward
merged to `bioshock-infinite`. Next session's target banked with the
user's observations (see the current-state section): the missing-hands
scripted-beat class - vigor drink, the tattoo-poster hand raise, the
ball-77 raise; all "look at your own hand" vignettes, while full scripted
holds show hands correctly.

### Session 58 - 2026-08-13 - HEAD-DIRECTED USE: 0x1E13DC cornered by the ButtonUseTarget oracle, policy shipped flat

On branch `claude/bioshock-head-interaction-bebe6d` (off origin/bioshock-infinite
@ bb0df6a). The deal-breaker session, evidence-first per the s58 ladder; full
derivation in ENGINE_NOTES "s58", headset checklist TESTING "S58".

- **The oracle hunt**: the planned GFx ButtonHint widget lane FALSIFIED as a
  live signal (no IsShown on the hint classes; the container entries TArray
  is sticky through hide/walk-away - show/hide is ActionScript-only). The
  validated oracle: `XPlayerController.ButtonUseTarget` (InterfaceProperty
  +0x176C, by-name), the live facing-gated USE target. `bsihint` (new
  hint.cpp instrument) prints it + edge-logs changes in watch mode.
- **The sweep**: at a user-parked vending machine, body yaw via relative
  mouse + head yaw via xrsim, A/B/A per candidate with restore verified:
  un-denying **0x1E13DC alone** flips USE arming to the composed head view
  in BOTH directions; all nine other deny-set callers = control. Cone
  half-angle 45-60 deg. Findings: loot containers arm by proximity only;
  `head rot +X` = MINUS X on the written view yaw (sign trap, cost 3 legs).
- **The fence**: eye-check PASS (pairing 90/90) with the un-deny live;
  4-min soak clean (aim calls==substituted, no s55 freeze); shipped-build
  fresh boot seeds 10 -> `headuse: ON`, save to free play normal. The
  raffle chain re-verify is the headset's leg (judged low-risk: scripted
  holds suspend the drive; in the headset the head IS the pointer).
- **Ship**: `kInteractionUseViewCallerRva`, `camera::set_head_use()`
  (seed-then-del invariant), config `interactHeadUse` default ON, F10
  "HEAD-DIRECTED USE (s58)", `bsicam vdeny del` (new verb), hint.cpp.
- **Traps**: pause-on-focus-loss leaves a MENU refocus cannot close
  (Esc, never Enter - RESTART CHECKPOINT is two slots down); sim idle
  hands block the captures (`hand l|r offset`); this save = fair-plaza
  spawn, ~90 s load cycle, NO scripted-hold markers; no manual saves.

### Session 57b - 2026-08-12 (same evening) - headset verdicts in; three fixes shipped, interaction split deferred to s58

Melee swing + execution, stump cuff, sprint-kill core: ACCEPTED. Fixed off
the verdicts (all flat-verified + eye-check PASS): (1) the post-sprint SNAP
- a 700 ms glue release tail rides out the sprint-exit anim blend
(`sprintglue tail <ms>`); (2) the reload-then-shoot glitch loop - the ready
capture is QUIET-GATED (motion < 3 deg / 3 UU per 100 ms sample, retries
per drive; ~2700 refusals measured across one fire+reload, banks land only
on settled poses); (3) the melee gun-hand lurch - the right limb bone-hides
for the swing's first 0.9 s (never the execution; mid-swing capture shows
gun+hand gone, unhide edge clean). DEFERRED s58 priority 1: head-directed
interaction (the s56 partition needs a finer split - USE-targeting to the
VR view, scripted view-cone gates keep the engine view; caller census +
raffle-fence next session).

### Session 57 - 2026-08-12 (late night) - THE MODEL LANE: sprint falsified-then-glued, melee window, loadout buckets, the crosshair hunt LANDED, stump cuff

On branch `claude/bioshock-model-lane-sprint-melee-7e33e4` (off
`bioshock-infinite` @ c1db3d4). Full derivations in ENGINE_NOTES "s57";
headset checklist in TESTING "S57". All five user-prioritized items landed
flat-proven; eye-check PASS (leg 0 pairing 90/90) on the final build.

- **Sprint**: the s49b clamp shape falsified (nothing at the message funnel,
  no by-name action - the runtime drives the state internally); the sprint
  arm-pump measured as compose pass-through (L 103-124 deg / 70.8 cm vs 0.00
  walking) and killed with the fireglue-full substitution held on the
  input-derived sprint state. A-B-A 124.38 -> 0.10 -> 124.38 deg.
- **Melee**: weapon=NULL at the seam for melee too (param discriminator
  dead); the Y-edge classifies instead, holds-open dispatches exempt (the
  QTE rule); melee skips/cancels fireglue + the poison capture (watched
  banking a mid-swing pose live) and the execution hold releases the hide
  gate. Counter-proven A/B; the execution look needs the user at a
  staggered enemy.
- **Profiles**: per-hand keying confirmed as the vigor-only leak; loadout-
  class suffix `#solo` added (gun-present keys byte-identical; empty
  prefix-match keeps the hide gate honest).
- **Crosshair**: the owning screen reached (widget Outer =
  XSinglePlayerGFxHUD); IsShown/centerpoint bits at +0x118 cleared by a
  default-on policy with per-level re-derive + watchdog (game re-asserted
  7x, lost each time). `bsixhair` A/B lever.
- **Stump**: collapse epsilon 0.10 (slider) replaces zero scale - capped
  stub visible flat; tuning is the user's.
- Traps: pad outage again (keyboard lane + force/swing test levers), the
  load beat must play untouched (~3 min), token-newline trap third bite,
  nose-to-wall spawn.

### Session 56 - 2026-08-12 (night) - THE INTERACTION-VIEW FIX SHIPPED: third gate found flat, the smear pinned in the headset, deny set seeded at install, raffle chain accepted end to end

On branch `claude/bioshock-interaction-view-fix-c1be42` (tip-identical to the
s55 branch). Full record in ENGINE_NOTES "s56"; checklist in TESTING "S56":

- **Instrument audit first**: a negative control (drive fully off) passed
  every s55 eye-check leg while `camReplays/s` collapsed 90 -> 0 - the image
  legs are blind to the pairing break (mono reads in-band). Leg 0 (fresh-beat
  camReplays/s >= 80% of draws/s) added and validated in both directions;
  interocular floor 40 -> 30. vdeny widened (16 slots, allow-only derivation
  mode); allow-only{0x26B499} proved the pairing is fed by the scene caller
  alone.
- **First flat raffle playthrough**: the 11-deny trial advanced past s55's
  wall (take-ball accepted, reveal played) then stalled pre-throw; live
  census named a caller absent from the s55 map - 0x5EA483, the scripted-
  sequence "is the player looking at the target" cone gate; denying it
  unstuck the beat in the same log second. Chain then ran to the skyhook
  QTE (fire press accepted) and free play.
- **The headset bisect (user, VDXR, 4 live flips)**: the seeded 11-set
  produced an offset-dependent post-process smear no flat leg can see;
  bisect pinned it to 0x1E1367 alone - the eyes-viewpoint wrapper's DIRECT
  flavor is a RENDER consumer, never to be denied; 0x1E13DC (virtual
  flavor) carries the interaction path. s55's attribution corrected, its
  framing-shift observation explained.
- **Shipped**: patterns.h `kViewConsumerDenyRvas` (10 callers) +
  `kEyesViewDirectCallerRva` (never-deny marker), seeded in camera
  `install()` behind the build gate - automatic, zero levers. **Headset
  acceptance: sharp in all directions + the full raffle chain with no
  stalls, user-played.** Final sim eye-check green on the shipped build;
  A-B-A via `vdeny off/on` live.
- New traps recorded: menu-contaminated eye-check trials (quads=1 tell),
  xrsim-launch's stale-runtime misread after a VDXR boot.

### Session 55 - 2026-08-12 (night) - eye-check tooled, DENYLIST landed, the interaction consumer cornered live at the raffle lady

Executed the s54f plan on branch `claude/bioshock-interaction-view-denylist-5f89c7`
(worktree off 12140e0). Full technical record in ENGINE_NOTES "s55"; summary:

- **tools/eye-check.ps1** wraps the 5-leg stereo check; calibrated on the
  known-good build (interocular 46.8/73.3% at the fairground - the s54f
  53-56/77% numbers are scene-dependent, band [40,70] holds); both s55 runs
  PASS, including on the vdeny build with the set empty.
- **vdeny (camera.cpp)**: deny set gates only drive_view; empty = historical
  substitute-for-all; `bsicam vdeny` lever + F10 counters. Un-seeded by
  design until the acceptance ladder passes.
- **Derivation**: near/far census killed the proximity hypothesis (all nine
  in-scene callers fire per-draw everywhere); offline capstone named the
  mid-stall candidates - 0x1E1367 (eyes-viewpoint wrapper, direct flavor),
  0x22587F (view -> GLOBAL 0x13AB894 publisher), 0x203E73 (out-params
  DISCARDED - exonerated, and it resolves s54f's render-suspect question).
- **The live A/B (fresh boot, user walked to the lady, full VR)**:
  deny{1E1367} -> she raises the basket + interaction marker; deny{both} +
  pad X -> `ForceUnequip` + `cine: SCRIPTED hold OPEN` = the take-ball
  interaction ACCEPTED under full VR (a first across every era of the mod).
- **Aborted at the user's shutdown**: the announcer/reveal after take-ball
  did not fire within ~8 min (one confounding pre-reveal pad-A press was
  sent); run abandoned, not proven stuck. Boot-1 oddities to watch: the aim
  seam's `substituted` counter froze during deny churn (reload cleared it);
  deny{1E1367} visibly shifted the rendered framing.
- The first-boot "heavy stutter every 5-10 s" the user reported matched
  beat-log dips to 57-77 draws/s; it vanished on the second boot with the
  harness quiet - prime suspect is harness focus-steals during censusing,
  unresolved.

### Session 54f addendum - 2026-08-12 - the s54e view-consumer filter FALSIFIED IN THE HEADSET (stereo broken) and REVERTED; the s55 derivation plan

**s54e (commit af23824: VR pose to caller 0x26B499 only, everything else
authored) passed its flat raffle acceptance and then BROKE VR RENDERING in
the headset**: each eye a COMPLETELY different image, smearing on head
motion, no 3D. REVERTED (2b1b169), rebuilt, installed, and sim-validated
(eye-check numbers below). The s54d MECHANISM stands un-falsified - the
interaction system reads the substituted view, and giving it the authored
view mid-stall made the very next press register, twice. What failed is the
FIX SHAPE: "0x26B499 is THE render caller" was wrong or incomplete.

- **Why it broke (s55 must verify)**: the renderer does not consume the view
  (only) through 0x26B499. With only that site substituted, one stereo pass
  rendered from an UNSUBSTITUTED consumer (authored view) while the reentry
  pass replayed the VR-substituted SR base cached by 0x26B499's dispatch -
  a different world per eye, plus reprojection smear from layer tags that no
  longer described the rendered content. Prime suspects for the real render
  consumer(s): the present-rate caller 0x203E73 (parent 0x21E03E) and the
  cached-POV / view-transform paths (PC+0x24C/0x258/0x430) that other sites
  may refresh. The flat acceptance NEVER CHECKED THE PER-EYE IMAGES after
  the change - the raffle-throw oracle covered the interaction half only.
  The stereo-only-testing rule exists for exactly this and was violated.
- **The s55 approach - INVERT to a measured DENYLIST**: keep
  substitute-for-all as the baseline (stereo known-good, raffle known-bad)
  and derive the INTERACTION consumer's call site(s): caller-census DIFFS
  near/far from an interactable and during the raffle wait (the site that
  fires while a prompt is evaluated), `bsicam stack` backtraces to confirm,
  then give ONLY that site the authored view. After EVERY trial run the
  eye check below, THEN the raffle acceptance (prompt arms + pad press
  throws), then A-B-A. The end state must be AUTOMATIC (armed at install
  behind the build gate, zero user levers) and preserve full 3D - user
  directive; a targeted mechanism is fine only if it self-applies to every
  raffle-class prompt.
- **THE PER-TRIAL EYE CHECK (mandatory after ANY view-path change; sim, no
  headset needed)**: boot the save, `vrstereo on`, then:
  1. `xrsim-shot A` - projection layer present, EyeSeparationM ~0.063;
  2. `img-diff A_left A_right` - healthy baseline (this save, reverted
     build, 2026-08-12): mean ~53-56, pct-changed ~77% (composite includes
     the HUD/laser quads - not pure parallax); a per-eye-different WORLD
     reads far outside this band;
  3. `xrsim-cmd "head rot 25 0 0"`, settle ~3 s, `xrsim-shot B`;
  4. `img-diff A_left B_left` AND `A_right B_right` - BOTH large (healthy
     ~21-31 mean): the view moved in BOTH eyes;
  5. `img-diff B_left B_right` - back near the baseline band.
  Any leg out of band = the trial broke stereo: revert the trial first.
  (Wrap as tools/eye-check.ps1 first thing next session.)

### Session 54d addendum - 2026-08-11 (night) - THE RAFFLE WEDGE MECHANISM CORNERED FLAT: the VR VIEW breaks the interaction system

**The scene-stall reproduced FLAT against the sim on the user's save, and five
controlled runs cornered it.** The raffle sequence NEVER stalls - it waits at
the throw INTERACTION, whose prompt never arms and whose presses never
register while the VR view is active. Verbatim run matrix (config AT the
reveal / at press time -> result):

| run | pad | camera | stereo | result |
|---|---|---|---|---|
| 1 (flat) | on | on | on | broken; released by keys AFTER stripping cam+stereo+pad |
| 2 (user, flat) | off | off | off | **works immediately, prompt visible** |
| 3 (flat) | on | on | on | broken; pad A/B/X/Y/triggers/grip/click ALL dead; released with keys after `bsivr off` |
| 4 (flat) | on | on | OFF | broken (prompt absent on the flat screen too) - **stereo alone exonerated as necessary** |
| 5 (flat) | OFF | on | on | broken - **the pad exonerated as the sole cause**; then cam+stereo stripped MID-STALL -> E/F/Space released it WITHIN SECONDS (18:34:19) |

- **The mechanism**: the interaction/button-hint system (GNames vocabulary:
  `XClikButtonHint`/`XClikButtonHintsContainer`/`UsePromptButtonHints`/
  `ForceInvalidateButtonHints`; live instances confirmed mid-stall by `bsigfx
  scan`) evaluates off the ENGINE VIEW - the same view the camera drive
  substitutes with the VR pose. With the VR view active the throw
  interaction's aim/reach gate never passes: no prompt, no accepted press,
  on pad AND keyboard alike. Restore the authored view (`bsicam drive off` +
  `vrstereo off`) and the very next press completes the throw. Camera ALONE
  breaks it (run 4). A ~9-minute "re-arm" pattern seen in runs 1/3 was a
  COINCIDENCE of when the strips happened - run 5 falsified the timer.
- **Why "VR off fixes it" was always observed**: the master toggle strips the
  view substitution (and everything else) - the presses the user then made
  registered. vrstereo-off alone not releasing (headset, s53) fits too: the
  BSI one-toggle leaves the camera drive on - see run 4's mirror.
- **Why it "predates everything"**: the camera substitution is the oldest
  feature in the mod. Every era had it; every era wedged at the raffle.
- **THE FIX (s55, the real work): call-site discrimination on the camera
  seam** - gameplay/interaction consumers of GetPlayerViewPoint (the Use
  trace, the hint arming) must read the AUTHORED view; only the render path
  gets the VR pose. Same class of seam BS1/BS2 grew (their aim/fire seams);
  Infinite's camera hook already logs caller context - derive the
  interaction call site's RVA and filter. `ForceInvalidateButtonHints` +
  the ButtonHint instances are the probe surface for proving the arming
  live; the raffle save is the acceptance test (prompt appears + pad press
  throws, full VR on).
- **WORKAROUND until then (headset protocol)**: at any scripted interactive
  beat that won't offer its prompt: F10 -> camera OFF + stereo OFF, press
  the interact, then both back ON. (The old VR-toggle protocol also works -
  it is the same strip with extra teardown.)
- The doff/alt-tab crash of s54b remains SEPARATE and open (the s54c revert
  removed the feed that surfaced it; the park protocol stands).

### Session 54c addendum - 2026-08-11 (evening) - BOTH s54b SUSPECTS FALSIFIED; REVERTED BY USER DIRECTIVE; the suspect set narrows to the CAMERA+STEREO era

Second raffle replay on the s54b build (cine hide game-managed, feed armed
with the demote-edge close moved off-thread):

- **The raffle stalled IDENTICALLY with game-managed** - the hide gate is
  EXONERATED for the scene wedge. Default reverted to force (the user
  directive stands; it was never the blocker).
- **Alt-tab still broke the game with the feed armed** - the CloseOpen
  hardening was not sufficient (or not the mechanism). BY USER DIRECTIVE the
  feed is DISARMED (adapter no longer calls set_pace_feed); Infinite returns
  to the pre-s54 session handling: a VISIBLE episode throttles the game
  (~10 Hz) and may park until an external VD action or the F10 VR toggle.
  THE WEDGE PROTOCOL IS BACK: F10 -> VR enabled off, beat, on. The feed
  machinery stays in core, default off; `vrpace feed on` is a controlled-
  experiment lever only. The sim model + vdxr-park.xrs remain (they encode
  measured VDXR behaviour and the park regression).
- **The user's directive on the scene wedge (verbatim intent): it predates
  the input lane, arsenal, hide, aim - EVERYTHING except "normal VR + stereo
  rendering and related headset features". Do not touch model/controls
  machinery for it.** The s55 suspect set is therefore: the camera
  substitution (GetPlayerViewPoint detour + drive), the stereo reentry
  (second scene draw + camera replay per frame), XR frame pacing (the game
  ticking at headset cadence), and the xinput proxy shim itself (present
  since day zero). The cleanest first bisect at the raffle: `vrstereo off`
  BEFORE the scene, replay - if the sequence completes, split camera-only
  vs stereo-only next; if it still stalls, the shim/pacing lanes take over.
  A flat repro would be gold: the save sits right before the raffle; the
  walk to the trigger is short (~50 s) - candidate for a game-cmd/key
  scripted approach next session (or the user drives to the trigger flat).

### Session 54b addendum - 2026-08-11 (afternoon) - THE HEADSET VERDICT REFRAMES THE WEDGE: two bugs, not one

The user replayed the raffle with the pace feed armed. **The scene stalled
again - and this time the log proves it is NOT the session park**: the whole
wedge ran FOCUSED at 144 presents/s, input live (the user could open the
menu), sounds playing, head tracking fine. The game's own scripted
input-lock opened at 16:00:28 ("cine: SCRIPTED hold OPEN"), our gate
FORCE-HID the rig at the same edge, and the sequence stalled inside the
lock - announcer silent, the throw prompt never appeared. The park
root-caused this morning is real and stays fixed, but it was the AMPLIFIER
of the old hits (any VD demote turned into 5 fps + dead input), not the
raffle's own stall. The s53 "released by the VR toggle" reading also
weakens: the user now believes the FOCUSED flips in that log were their own
alt-tabs.

- **Scene-stall prime suspect: the s53 force-hide through the raffle hold**
  (s53 measured the game fighting it 15 reasserts in this exact scene; a
  raffle-class sequence appears to gate on its authored hand/ball moment).
  s54b flips the cine radio DEFAULT to game-managed (force stays as the
  A/B); the retry protocol is simply "replay the raffle on the new build".
  If it still stalls: bisect ladder = `bsifidget req clamp off` (the
  Lowered clamp posts into the FP network all through the scene), then
  `bsiaim drive off`, then `vrstereo off` - one lever per attempt.
- **The doff CRASH (new)**: removing the headset mid-wedge froze the game
  instantly - draw thread wedged in a win32u syscall, presents 0, Windows
  ghosted it (the user's "warning/error"), close -> WM_DESTROY -> the
  KNOWN exit-path fault. The one blocking XR call the present thread could
  make at that edge (the inline idle-close of a pair-open frame) moved to
  the pace thread (CloseOpen request kind). Whether that was the freeze's
  cause is UNPROVEN - the next doff is the A/B; if it still freezes, the
  suspect moves to VD/VDXR's own display handling and `vrpace feed off`
  is the isolation lever.
- vdxr-park.xrs 21/21 green on the s54b build; installed to the game.

### Session 54 (Infinite) - 2026-08-11 - THE RAFFLE WEDGE ROOT-CAUSED (log archaeology) AND FIXED (the pace feed)

Branch `claude/bioshock-session-deadlock-root-28d444` from d49dcff. The
user's directive was ROOT, not band-aid - and the root was already ON DISK:
last night's pacetrace.log + bioshockvr.log recorded both wedge episodes at
1 Hz. Full derivation in ENGINE_NOTES "s54"; checklist in TESTING "S54".

- **The mechanism, measured end to end**: VDXR demotes to VISIBLE holding
  shouldRender=0 -> our inline loop submits zero-layer frames (layers gated
  on shouldRender) -> VDXR refuses to re-promote on empty frames (parked
  VISIBLE for minutes at 72 empty frames/s) AND throttles their xrEndFrame
  to ~87 ms inline on the present thread -> the game runs at ~10 presents/s:
  announcer stalls, input effectively dead (plus actions are spec-dead while
  not FOCUSED). Predates the input lane because the throttle half needs no
  input lane. Both s53 recoveries were external (no teardown involved); the
  VR toggle works by removing the throttled calls + free-pacing bring-up.
  Skip-guard suspect exonerated (neutered by default, paceSkips 0).
- **The trap that nearly misled the read**: pacetrace.log appends across
  DAYS - two nights both have a "03:02:24". Segment by file offset, not
  timestamp (recorded in ENGINE_NOTES s54).
- **The fix**: core `set_pace_feed` (opt-in; BSI arms, BS1/BS2 no new
  branch, snapshot banking gated on the flag): while not-FOCUSED-after-focus
  the present thread detaches and the pace thread re-submits the last
  healthy layer set with fresh displayTime (request protocol grew
  wait/feed-cycle/feed-finish kinds; snapshot invalidated before swapchains
  die; 1 s backoff on a failed cycle). `vrpace feed on|off` + F10 checkbox.
- **The sim grew the measured-VDXR model** (focus norender / policy
  vdxr-layers / throttle <ms>, all reset-safe, defaults untouched) and
  `tools/xrsim/vdxr-park.xrs`: feed OFF = the park reproduced on a desk;
  feed ON = self-healed (FOCUSED 2.2 s after the loss, 444 presents ran
  unpaced through the episode). unfocused-pacing 12/12 and smoke 5/5 stay
  green; xrsim-selftest PASS (empty frames still promote under the default
  policy).
- **Item 3's first lane LANDED same-session**: the object-instance
  enumerator (`bsigfx scan` by FName index / `bsigfx scanc` by class
  pointer, name-gated after 82 fixpoint-passing fakes) proven live on a sim
  boot - found the HUD UClass + Package + the live myHUD instance +
  **Default__HUD, the CDO the s53 null-load wall never reached**. The save
  level this boot reaches has NO XClikHUDCrosshair instances (UClass only) -
  the hunt resumes with the same commands in the gameplay-proper save.
- Item 2 (s53/s52 headset verdicts) untouched - user-in-headset only;
  carried to s55 with the S54 A-B-A first.

### Session 53 (Infinite) - 2026-08-11 - THE FP-RIG HIDE done right: the lever A/B, the bone-lever gate, the GFx screen model

- **The mandate**: round 4's revert record (zero-scale falsified in-headset,
  release freezes hands) -> find the game's OWN hide lever, flip it live,
  wire it to the round-3 conditions. Branch `claude/bioshock-fp-rig-hide-fa6342`
  off `si52` tip 9e38f7a; commits `6b83cfb` (bsihide + bsigfx probe surfaces)
  and `308dc31` (gate armed, bone default).
- **DERIVED (one-shot, cached per boot)**: bHidden actor+0x5C mask 0x4;
  HiddenGame comp+0xD4 mask 0x20; bOwnerNoSee mask 0x40; bOnlyOwnerSee mask
  0x80 (and the FP component carries bOnlyOwnerSee=1 in gameplay - the rig is
  owner-only by construction); indices for SetHidden/SetOwnerNoSee/
  HideBoneByName/UnHideBoneByName/IsBoneHidden/MatchRefBone. SetHiddenGame
  does not exist; console `set` stays dead; all writes are instance bits or
  native dispatch.
- **MEASURED, and it reshaped the design** (rowboat save + Comstock Rooftops
  chapter, window A/B with a pistol equipped): (a) scripted scenes NEVER swap
  the attachment (`bsihide who` = SAME throughout - the stale-rig pivot is
  dead); (b) the game TRACKS scenes on bHidden (1 through no-hands phases,
  0 at the authored moment - timestamped flip caught live) **but bHidden does
  NOT hide the FP rig** - bit set+verified 1 with hands and pistol still
  rendering. The FP path ignores actor visibility - which also explains why
  round 4's rig-side manipulations changed nothing the user could see;
  (c) comp SetHidden hides the arms mesh but the WEAPON MODEL floats on
  (separate component); (d) bone HideBoneByName removes limb + holdable
  together - the only complete hide, now the production default.
- **SHIPPED (hide.cpp, armed)**: cine hold -> rig-wide bone hide behind a
  POLICY radio (force / game-managed / off; force default per the user
  directive - the authored box-handoff hand is the known risk, first headset
  A/B); empty hand outside holds -> per-hand whole-limb bone hide (user
  call). Edge-driven + 500 ms watchdog, refused-latch derive, rig-drop-safe,
  fault latch, F10 controls in HANDS + MODEL, `bsihide` command surface
  (status/derive/who/diff/actor/comp/owner/bone/hand/pawn/lever/cine/auto).
  Flat-proven live: possession with empty hands bone-hid both limbs, the
  chapter loadout's arrival unhid them, 0 reasserts / 0 failures.
- **The pawn-body suspect**: with the rig exonerated twice, the headset
  doubles ("hands to the right and left of the character") are likely the
  pawn's OWN third-person mesh, visible only from the offset VR camera.
  `bsihide pawn 0|1` (SetOwnerNoSee on pawn Mesh, derived at pawn+0x2E4 via
  the Mesh property) is the one-command headset A/B.
- **The GFx lane (crosshair kill, rung 3 PARTIAL)**: every machinery name
  lives (HideableHUDWidgetNames 4835, NumReasonsToShowElement 36595,
  XSeqAct_HideHUDElement 10586, XClikHUDCrosshair 8654, FlashCommand,
  HUDMovie; SetVariableBool/GetVariableBool do NOT exist on this build) but
  `myHUD` (PC+0x2B4) is a bare `HUD` object in EVERY level probed - none of
  the machinery is on it, and no HUDMovie property either. Walks mapped the
  screen model: XGameViewportClient(+0xF0)->GFxInteraction; UI screens are
  GFxMoviePlayer-family UObjects (SwfMovie +0x34, PC +0x5C) owning CLIK
  widget UObjects (XClikLabel's Outer = XModalTrainingTextScreen). The HUD
  screen INSTANCE is the open hunt - needs an object-enumeration or
  GFx-advance-hook lane. `bsigfx` shipped (hud/prop/cmd/element/setb/getb);
  the CDO-load shortcut failed (four path shapes, all null).
- **Traps**: the s37 attract freeze recurred once (force-kill + relaunch,
  protocol unchanged); game-shot captures RACE command dispatch (game pauses
  unfocused - trust the log's bit readbacks over pixels for state); xrsim
  `head rot` pitches on the SECOND arg; `head pos` offsets during scripted
  scenes can bury the camera in geometry (`bsicam drive off` restores the
  authored view for observation).
- **NEXT**: headset block (TESTING "S53" minute one = intro text + doubles;
  the cine-radio and pawn A/Bs), then the s52 verdict list, then the HUD
  screen-instance hunt for the crosshair + preset keys post-verdict.

### Session 53 addendum - THE RAFFLE DEADLOCK: an OLD session-state bug, released by a VR-enabled toggle

- **User report (live, mid-headset-block)**: at the raffle the game deadlocks -
  the announcer stops, no input begins the confrontation. **The user has seen
  this since the EARLY builds (camera + stereo only - it predates motion
  controls, the input lane and the s53 hide gate entirely).** This time
  `vrstereo off` did NOT release it (their remembered lever from the earlier
  hit); toggling **"VR enabled" off -> on (F10; full session teardown +
  re-bring-up) released it INSTANTLY** - announcer resumed, prompt appeared,
  and it stayed fixed after re-enabling.
- **The log during the stuck state**: continuous `xr: present phases (state
  VISIBLE shouldRender=0)` - the session was parked in VISIBLE, not FOCUSED.
  Per the OpenXR spec, action state (our entire controller bridge) is only
  live while FOCUSED - a session stuck VISIBLE means the game receives no
  synthesized input at all, and the game's own pause/focus handling can stall
  scripted timelines with it. Teardown + re-create re-earns FOCUSED, which is
  exactly why the user's toggle works.
- **Work item (core, next session)**: a session-state watchdog - if the
  session sits VISIBLE while the game window is foreground for more than a
  few seconds, log loudly (and consider an automatic re-bring-up, which the
  user has now proven safe mid-game). Until then the PROTOCOL on any
  "announcer stopped / nothing accepts input" wedge: F10 -> VR enabled off,
  wait a beat, back on.
- Note: the hide gate was initially suspected (its raffle-hold force-hide had
  15 watchdog reasserts - the game visibly fighting to show the authored
  ball hand) and was flipped to game-managed live as a first attempt; it was
  NOT the deadlock cause, but the 15-reassert fight stands as evidence that
  raffle-class scenes want the rig visible - the box-handoff A/B question.

### Session 52 (Infinite) - 2026-08-10 (evening) - the I7 input lane, THE CHEATED ARSENAL + presets, the HUD on a quad, the cinematic gate

Branch `si52-inf-input-arsenal-hud-cine` from `bioshock-infinite` f1b78a8; four
items in strict order, all flat-proven and committed; NOT merged (headset
verdict pending). Commits: 882a420 (input), 0e4a41c (arsenal+presets),
a2aea90 (HUD quad), e8dc538 (cinematics). ENGINE_NOTES s52 parts 1-4 carry
the derivations; TESTING "S52" carries the headset checklist.

- **Input**: publish_vr_gameplay/pitch_error/move_yaw_offset from drive_view;
  right-stick Y provably dead (camera-pitch instrument, +/-89 clamp,
  A/B/servo-convergence proven); bumper-lift opt-out (no ry leak); body
  follows head (walk heading == camera facing to 0.5 deg; off-control
  reverts); snap turn wired, drain sign FLIPPED vs BS1 (sim-derived).
- **Arsenal**: weapon identity = ARCHETYPE name (all weapons class XWeapon);
  GetEquippedWeapon(0/1) = both hands' getter; grant recipe load-archetype ->
  AcquireWeapon(archetype) -> manager EquipWeapon -> AddAmmo (s43 corrected:
  the CDO was the wrong shape, the archetype grants); `bsigive` + auto-
  capture per-weapon presets (weapons.ini), round trip proven.
- **HUD**: gfx_hud positional classifier (DR-I7 verbatim: the a=6 depth-free
  eye blit, then the UI run); redirect -> capture RT -> the session-19 quad
  via a new provider seam; eye image HUD-free, pause menu readable on the
  panel; scene-space effects untouched, GFx flash-class passes through
  (census-proven); movie frames classify nothing.
- **Cinematics**: Bink covered by the silence architecture (no code); Matinee
  detector = GetViewTarget poll; one release per hand on hold edges, chord
  suspension (A reaches interactive prompts), head radio look/fixed proven.
- **Harness**: the SIM PAD OUTAGE recorded (game stops polling XInput on
  some boots; not the mod; two flat checks moved to the headset list). inis
  restored byte-identical (never modified); weapons.ini test file removed;
  the granted arsenal was never saved in-game (sim-only state).
- **ROUND 4 (2026-08-11) - ATTEMPTED AND REVERTED (77e1eb5 -> revert
  256a244; user call: "revert, fix next session")**: releasing is NOT
  hiding - in the states that need hiding the game never animates the
  normal FP rig, so released bones FREEZE visibly at their last pose
  (headset-measured: "frozen in place, can't control them"). The attempt
  drove every cluster + arm bone to the kind-2 collapse (ZERO SCALE) per
  frame (`bones::drive_hidden` / `g_hideWhole`, reusing the s48 armsMode-2
  collapse widened to the whole hand). The SIM looked clean through the
  entire rowboat intro (no frozen rig, authored scene intact to the dock) -
  but the HEADSET verdict was: (a) the intro cutscene TEXT broke, and (b)
  the double hands were STILL there. LESSON: the flat lane is NOT a
  sufficient oracle for this feature - the sim never reproduced either
  symptom. NEXT-SESSION mechanism candidates, in order: find the game's
  OWN hide lever for the FP attachment (vanilla must hide it in scenes -
  hunt bHidden/HiddenGame/SetHiddenGame on XFirstPersonAttachment or its
  component, or the show/hide call sites around scripted-scene starts);
  component-level hide beats bone-level collapse. What ships on the branch
  is the ROUND 3 state below (release-based; frozen-hands-in-view is the
  KNOWN OPEN issue).
- **ROUND 3 (same night, second headset verdicts)**: snap turn + cine-start
  recenter CONFIRMED by the user; the DOUBLE HANDS persisted in every
  scripted FP hand scene (box handoff, doors - the view target stays the
  pawn and bCinematicMode does not cover them all). Fixes: (1) the cinema
  gate SPLIT into camera_hold (Matinee/cinematic-bit: recenter + head
  radio) vs SCRIPTED hold (hand/aim/fire release only, head keeps driving,
  NO recenter - a door grab must not snap the world). The scripted signal
  is the engine's own IsMoveInputIgnored/IsLookInputIgnored QUERY functions
  polled by cached index at 500 ms - after TWO falsified variants
  ("IgnoreMoveInput" resolves to a FUNCTION and a null-class property find
  matched it, reading garbage = permanent hold; the bIgnore* bool mirrors
  do not exist on this build). VERIFIED in the exact reported scene: the
  rowboat box handoff shows ONLY the authored hand, movement provably
  locked, head look free. (2) EMPTY-HAND RELEASE (default ON,
  `handsHideEmpty` key, F10 checkbox in WEAPON PROFILES): a hand holding
  nothing releases its drive - authored arms play (kills the bare-hand
  misrotation, the intro double hands, and the sprint-arms weirdness for
  the bare case in one stroke; per-key scale-to-floor remains the literal
  hide option). (3) CROSSHAIR hide DEFERRED with the reason measured: the
  crosshair is a Scaleform CLIK widget (XClikHUDCrosshair) - the HUD
  object's property chain carries NO crosshair state (full walk, zero
  rows), so hiding it needs a GFx-invoke lane (a future derivation; also
  the subtitle/HUD-tweak power tool). The sim pad outage persisted all
  night (harness); the profiles identity poll now proves the empty keys
  live (gun[R]='NoWeapon' vigor[L]='NoVigor' on the rowboat save).
- **ROUND 2 (same night, first headset verdicts)**: stutter fix CONFIRMED by
  the user; five fixes landed off the headset list: (1) SNAP TURN sign
  flipped back to BS1's drain - the sim smooth-turn derivation was ALIASED
  (1 Hz reads of a fast yaw wrap); the headset read "flick right snaps
  left"; the eye beats the sampler. (2) Cinematics RECENTER on the
  hold-open edge - a cutscene starts centered on the current head
  direction (camera::request_recenter, fired from cine's apply_hold).
  (3) The double-hands cutscenes are the scripted FIRST-PERSON ones (view
  target stays XHuman - the Matinee leg cannot see them); the detector
  grew the APlayerController.bCinematicMode leg (s48b property-bit walker,
  one-shot derive + per-poll bit read). (4) EMPTY HANDS became their own
  profile keys ("NoWeapon"/"NoVigor" via the tri-state identity poll) -
  tunable with the same sliders; scale-to-floor IS the hide-arms fallback.
  (5) HUD quad sliders in F10 HUD (I9) (distance/width/up; width scales
  the icons) + hudDistM/hudWidthM/hudUpM preset keys. PLUS the F10
  ARSENAL (I9 cheat) section: GIVE ALL + per-weapon buttons (render
  thread posts, arsenal::tick drains on the game thread; all grant
  function indices cached - the fname_find rule). `bsigive all` is the
  verb twin. KNOWN LIMIT (measured): grants need a level with item assets
  resident - the pre-raffle intro's PreCoalescedItemAssets is empty and
  the cooked CoalescedItems package refuses demand-load (bare + dotted
  probed); from the fair onward everything resolves. Flat smoke: hold
  recenter + cine fields + edges verified; the give-all code path is the
  same that granted 10 slots in the town save.
- **HOTFIX 2 (same night, user report "clear stuttering, unplayable")**: the
  s52 cadenced pollers (cine 2 Hz GetViewTarget, profiles 1 Hz
  GetEquippedWeapon x2) each ran `fname_find` - the deliberate WHOLE-POOL
  linear scan (~70k names) - per poll, freezing the game ~300 ms in a
  perfect 500 ms rhythm (edgelog dt series: 300/47/47/47/47/300...). The
  recorded "fname_find never on a cadence" rule, violated by its own
  author. Fix: `reflect::call_on_object_by_index` +
  `find_function_index` - resolve once per boot, dispatch by cached index
  (cine, profiles, and the dyndot Trace all converted). Verified: the same
  edgelog run reads a flat 46-47 ms, max 47, zero teeth.
- **HOTFIX (same night, user report)**: a FLAT launch (no headset) with the
  preset's sticky `vrstereoOn=1` ran the SR doubling + per-eye offsets with
  NO session - alternating two-view window at half rate ("completely
  broken... alternating between 2 screens"). NOT an s52 feature regression:
  the arm path never had a session gate; the user's preset arrived stereo-
  armed from the s51 headset night. Fix: `scenedraw::stereo_active()` now
  requires `bvr::vr::session_live()` (arm flag stays sticky; doubling, eye
  offsets and the sr eye tags all key off it; `reentry pulse` stays
  sessionless as the flat A/B instrument). Verified both ways: flat =
  single view, draws==presents, no inter-eye, no ring-skew spam; sim
  session = doubling + inter-eye 9.448 back exactly.

### Session 51 (Infinite) - 2026-08-10 (overnight) - the SHOULDERS killed (full-hand substitution), the FOV-edge discriminators + THE EDGE TELEMETRY, the FX record lane exonerated

Three items in strict order, all flat-proven and committed on
`si51-inf-shoulders-edge-fx` (from s50 tip 0a17918; a mains power loss
mid-session cost nothing). (1) FIRE-SWING ROUND 2: the new
`bsibones travel all` per-bone peak table named the mechanism in one shot -
the s50 corr pins the anchor (0.10 deg) while the fire anim's anchor-RELATIVE
articulation swings the rest of the left chain 74-133 deg / 87.7 cm; the
chest (the only undriven bone) read 0.00, falsifying the engine-owned-bones
theory outright. Fix: the ready capture banks the whole hand; the fire window
substitutes the banked atoms for the live source (corr collapses to
identity). A-B-A exact (full ~0 / anchor 133.53-95.58 / full ~0), flourish
8.43, stance 4.5-min idle clean. `bsibones fireglue on|off|full|anchor` +
F10. (2) FOV-EDGE: three discriminators - the hand-anchored compositor quad
(core builds it AT the located grip per present; sim: quad==grip to 1e-4),
the VDXR view logger (located per-eye pose/fov + eyeSep/cant/fov-asym,
bounded burst), and THE EDGE-TELEMETRY LANE (`bsicam edgelog`, ~30 Hz ring,
110-column TSV of the whole chain per sample; sim sweep null baseline banked:
lateral chain exactly linear at worldScale, rms 0.000). (3) FX-ORIGIN FORKS:
all three falsified live - the decoded tick 0x436490 NEVER RUNS (probe
calls=0 through a held charge), the update seam's whole population is six
SkeletalMeshActor scene records with NULL loc buffers (5 live callers of 194
static, 0x5EC393 dominant), the stamp-pair gate is inside the cold tick, and
the one-poke experiment therefore had no target. Six lanes down for the
frozen family. Command-seam trailing-newline trap hit twice, fixed, and
documented. Headset checklist: TESTING "S51" (the shoulders A/B, the
hand-quad one-look, viewlog, THE EDGE-TELEMETRY RUN, flourish lead-200).

### Session 50 (Infinite) - 2026-08-10 (overnight) - FX-origin hunt re-scoped; eye tags, THE FLOURISH BUTTON, and the fire-swing kill shipped

Second half (after the user's mid-session "re-scope" call on item 1): three
levers landed, each flat-proven with its own A/B. (2) RENDERED-POSE EYE TAGS
- the FOV-edge drift's one surviving mechanism: the projection layer tagged
the runtime's located per-eye poses over images rendered from OUR parallel
camera (base +- ipd-slider/2); the claim now matches the render (located
midpoint + nlerp orientation +- ipd/2). Identity on the sim (EyeSeparationM
0.063 both modes, per-eye diffs under the ambient floor); the compose chain
itself exonerated by inspection + the s48 lag probe; core change additive,
BSI-armed only; `bsicam eyetag on|off`. (3) THE FLOURISH BUTTON - measured
twice that a SubtleFidget play under the clamp is a visual no-op (the
response lives in the lowered subgraph; the natural timer chain fires all
session as no-ops - the flourish was un-answered, not un-scheduled). The
shipped recipe: a 6.3 s 'Lowered'=1.0 window (clamp-value override inside
the funnel rewrite), the engine impl called at +1.8 s via the trampoline
(SEH-isolated), the kill resumes on lapse. A-B-A img-diff 0.5 -> 8.15 ->
0.5. Chord: left thumbrest + A (core XR-composer addition, BSI-armed only,
A consumed under the chord), plus `bsiflourish`. (4) the mid-session user
report "shooting shouldn't affect the left hand": the travel instrument
measured a 95.58 deg left-anchor rotation through one right-hand shot
(idle 0.00) - the fire anim moves the authored left grip and the compose
passes whole-hand authored swings through. Fix: the retired s46 glue
correction, FIRE-SCOPED to 1500 ms around each player shot. A-B-A: 0.00 /
95.58 / 0.00 with the flourish still full-amplitude. First half: the
FX-origin hunt (part 1 of this log entry, below) - the frozen family
mapped, four position-source theories falsified by measurement, the
attach-walker + effect-update seams derived and probe-hooked, re-scoped by
the user. All levers to the headset checklist (TESTING "S50"); inis
restored byte-identical; nothing merged.

### Session 50 part 1 (Infinite) - 2026-08-10 (overnight) - the FX-origin hunt: frozen family mapped, position feed unfound, paused on a question

Branch `si50-inf-fx-edge-flourish` off a7ba268. Item 1 of the s49b handoff
only (strict order held; items 2/3 untouched; a 4th user item - firing
perturbs the left hand - queued mid-session). The held Enrage charge proved
out as the on-demand flat repro and split the FX in two: socket FX on the
child model components RIDE the driven hand already (the vigor hand model
attaches at L_Grip, the weapon at R_Grip - the attachment walker positions
them from SpaceBases, which the render-side drive keeps composed), while the
charge plume + ready sparkle (and per the headset, muzzle flash + tracer)
stay CAMERA-ANCHORED at the authored offset. Four falsification lanes ran
against the frozen family's position source: tick-time SpaceBases (the new
dirty-count instrument: the eval restamps SpaceBases only 2 ticks in 12183 -
our atoms stand, and the FX freeze anyway), GetPlayerViewPoint consumers
(caller census identical with/without charge), all reachable attach lanes
(child comps, pawn comps, actor Components, script XEmitterPool - empty),
and the decoded effect-playback tick's per-record update (rva 0x3EC4C0,
probe-hooked: ~2 calls/s, zero first-person - not the 90 Hz feed). Landed:
the attach-walker hook (rva 0x2A1B20, vtable slot 43 + a new vtable-slot
identity install gate) as instrument + eval-restamp edge cover, the
`bsifx u` effect-update probe (default off), and a page of new layouts in
ENGINE_NOTES (FAttachment, ParentAnimComponent redirect, the vigor-is-an-
XWeapon slot map, the effect manager + tick-helper + record table). Session
paused on the ask-when-blocked rule with three options for the user
(continue the banked forks / redirect / re-scope). Harness: award dialogs
replay on every checkpoint load; Enrage release = throw (burned the carpet,
drained salts; Restart Checkpoint refills); trigger edges need the
foreground-then-edge pattern. Inis restored byte-identical; nothing merged.

### Session 49b (Infinite) - 2026-08-10 - THE STANCE KILL: the 'Lowered' clamp, A-B-A proven, default ON

Same branch, continuing s49 under the user's "priority 1 only until fixed"
directive. Six more boots. The ladder ran: the request-post funnel derived
(wrappers -> inner post 0x5CED00, typed control-param messages, param FName
in the descriptor) and hooked with values logged - falsification 7 (nothing
enters the network at the onset). The engine's own reset-to-ready decoded -
its descriptor cache on the attachment (+0x294..+0x2F8) named five params;
Plan B (call the reset as a pin) falsified by DATA (it posts
ZipLine_IsBollard, not a pose). The manual poster (`bsifidget post`) built;
TwoHandFallback_Weight A-B-A'd as the 40-deg alert-relax pose pair
(falsification 8: the stance enters with the weight held). Then the live
value log caught the FIRE flipping 'Lowered' 1->0 and the ramp back - the
mechanism: the stance is the Morpheme lowered-idle settle. THE KILL: clamp
every FP-network 'Lowered' post to 0.0 (the game's own 90 Hz driver is the
carrier). A-B-A: ON = 435 s idle, 0/43 bones; OFF = stance back on schedule
(L_Grip 101.11); shipping auto-derive (name-verified descriptor, refuse on
drift) self-armed at +36 ms and held 407 s green with the battery clean.
Ships DEFAULT ON; `bsifidget req clamp off` bisects; boot pose still needs
the fire-once ritual. Traps: bsibones snap has slots 0-3 only; the eaten-
write pump trap recurs. Inis restored byte-identical; nothing merged.

### Session 49 (Infinite) - 2026-08-09 - StartSubtleFidget decoded and falsified, the Morpheme residual, one-lens verdict

Branch `si49-inf-stance-lens-tracer` off 4d46a73. Three boots (one menu wedge,
beaten with the new click-driven recipe). The stance hunt: the offline exec
census re-derivation revealed StartSubtleFidget is NATIVE; its impl (0x51BA00)
decoded end-to-end (the self-re-arming SetTimer scheduler, the by-name
'SubtleFidget' action play into the runtime Morpheme network at comp+0x228, the
cached FName globals). Two MinHook choke points landed with green positive
controls (impl hook; network play-by-name hook with caller-RVA attribution) -
and BOTH falsified as the root with live A/Bs: the stance re-entered with zero
impl calls (falsification 5) and with the action blocked by name
(falsification 6). Residual named: a Morpheme-internal transition
(rqHandFidget vocabulary staked). The lens: bsilens finally ran IN GAMEPLAY -
lens1 100%, lens2 0% over 281 rounds; no viewmodel frustum exists; the
edge-drift symptom is not a projection split (pixel station protocol attempted,
drowned by ambient scene motion - captures banked). Tracer recon: weapons
reachable via pawn+0x314 inventory manager; Tracer*/Muzzle* NOT on XWeapon's
950 fields - the FP-model walk is next. Tooling: bsichase, bones::component(),
bsifidget impl/act surfaces. Traps recorded: bsiaim dotdist eaten by the dot
prefix; the PE filter breaks bsicallat's gate; load-time pump lag eats
game-cmd writes. Inis restored byte-identical; nothing merged.

### Session 48 (Infinite) - 2026-08-09 - the verdict fixes: locomotion pinned, wrist/hide reworked, the stance proven native

Branch `si48-inf-verdict-fixes` off the s47 tip (ca1d438), same night as the
S46+S47 headset verdicts (all six recorded in STATUS). Zero core changes, no
merge. The evidence trail, in order:

**The stance (verdict 1)**: glue retired on the user's directive. Built the
ProcessEvent vtable-slot filter on the attachment (slot +0x7C occupant verified
= AActor::ProcessEvent; int-compare against GNames 2172; self-gated block) as
the root kill - probe observed the dispatch passing, filter observed it
blocked. Then the CLEAN boot falsified the premise: stance fully re-entered in
8 min with events=1/startSeen=0/blocked=0 - **the anim starts natively**; the
dispatch is a notification. Filter demoted to observer (default probe). Built
bsiprop (UProperty chain walker - Children/Next/Super link offsets DERIVED
live: +0x38/+0xC on this build's UClass; leaf class carries only 4 fields, so
supers matter) and bsipropbit (masked-bit read/write) to reach the surviving
root, bDisableSubtleFidget; the finishing walk needs a booted save - the last
two boots wedged in drifted menu states (trap recorded). Bonus finding: a
SECOND idle lane, a rigid uniform 40.00 deg alert-relax of the whole left arm
~90 s post-fire - every prior "ready" reference was actually the alert pose.

**Locomotion (verdict 2)**: reproduced flat (travel window: 9.26 UU rigid
wobble both anchors at walk speed vs 0.81 stationary), mechanism measured with
the new lag probe (c0 = R^-1(writtenCam - L2Wt): 0.00 exact stationary - the
attachment origin IS the written camera - spanning 11.2 UU walking), fixed by
camPin (dp base = fc.writtenLoc, the camera written THIS dispatch, so
engineLoc cancels; new FrameContext field). 9.26 -> 1.72 UU, default ON.

**Wrist (verdict 3)**: the bend quat moved from the arm chain to the hand
cluster; flat diff shows cluster-only rigid rotation, zero arm rows.

**Fire origin (verdict 4)**: trace origin follows the controller (engine
frozen, ours moved 43.8 UU for a 30 cm hand move) - the symptom is the tracer
FX spawn; GNames vocabulary staked for the hunt.

**Hide (verdict 5)**: one mode + capdepth slider (default 10 cm).

**Dyndot (verdict 6)**: `Trace`/FastTrace/SingleLineCheck stripped from the
name pool (exact-match verified) - script dispatch cannot trace; machinery
parked behind the future SingleLineCheck derivation. Fixed en route: a
per-cadence fname_find and a spinning test counter.

Inis byte-identical, command.txt deleted, games closed. The stance remains
visible in this build until the UBOOL lands - stated plainly in the S48
checklist.

### Session 47 (Infinite) - 2026-08-09 - I8 part 3, the remainder: boot pose measured, gate cleared, animtrans, scale audit, profile scaffold

Branch `si47-inf-i8-remainder` off the s46 tip (7245fa6). The S46 headset verdicts
were NOT in - nothing riding them was touched (glue algebra, fire substitution,
wrist-cap styles, wrist sliders all byte-identical), nothing merged, ZERO core
changes, all bioshockinf-local. Three sim boots, evidence before every lever.

**Boot-time glue arming: measured IMPOSSIBLE, closed.** Two boots (drive off, raw
bank): the never-fired idle pose (run A, 2 min) AND the resolve-time pose (run B,
23 s after `rig RESOLVED`, stable over a 10 s window) are both the full 101.11 deg
stance vs post-fire ready. The boot/checkpoint pose IS the stance - capturing qRef
at resolve would pin the stance and invert the glue. First-shot arming stays, now
as a measured certainty (TESTING note upgraded).

**The reapply-burst gate (carried s45b): measured-no-defect, closed.** Counters in
reapply() (maxAge / afterGap50 / skippedStale, in `bsibones` status). One boot,
every reachable gap class (drive toggle, tracking loss, pause): 33,255 replays,
skippedStale=0, maxAge=63 ms, afterGap50=6 (0.018%, same generation). The
edge-triggered release clears masks before staleness can accumulate; level
transitions stay covered by the rig-generation gate.

**ANIMTRANS: measured, then built.** New `bsibones travel <secs>` peak tracker
(~90 Hz per-dispatch anchor sampling). Engine truth, two runs each, repeatable:
reload moves R 21 UU (14 cm) and L 72 UU (48 cm, the cross-over rack); fire moves
R 5-7 UU; idle noise 0-2.7 UU. That earned the lever: dp base switches to the
ready anchor TRANSLATION banked alongside qRef (anim mode + valid capture +
120 UU broken-basis fallback). `bsihands animtrans on|off` + F10 checkbox,
default OFF, NOT persisted. Driven A/B: off = pinned (0.81 UU noise); on = the
authored travel to 1 UU (71.72/20.98) with rotation still glued (0.77 deg both).

**World-scale prep (I8 open box)**: 1.000 m -> exactly +150.0 UU, single-axis,
zero cross-axis; the full cm->UU audit table is in ENGINE_NOTES (every adapter
conversion rides the live fc.worldScale; the dot's cm->XR-meters is correctly
scale-free; two stale "fire seam is rotation-only" comments in aim.cpp fixed).

**Per-weapon profile SCAFFOLD** (I9 prep): profiles.cpp keyed by weapon class
name via the new `reflect::class_name_of` (UClass-fixpoint, best-effort derive);
zero entries, nothing consumed, nothing persisted; `bsiprofiles` status verb.
Measured: the fire seam's optional Weapon param is NULL on ordinary shots - the
pawn-side current-weapon source is I9 derivation work with the arsenal save.

**Battery green on the final build**: first shot SUBSTITUTED (77.4 UU), both
ready poses captured, aimRayMaxDevDegL/R = 0.0000 at opposite-angle stations
(legacy field 86 deg = the documented dual-beam artifact), 2 dots + projection
layers present, game alive, no new crash dumps, vrpreset.ini and XUserOptions.ini
verified byte-identical (fc /b), command.txt deleted.

### Session 46 (Infinite) - 2026-08-09 - I8 part 2: the stance killed, bullets from the gun, the wrist levers built

Branch `si46b-inf-stance-origin` off the s45b tip (f260f22). Six commits, nothing
merged, ZERO core/tools changes. The four open s45b headset findings, in order.

**Measure first, everywhere.** New instruments: `bsibones snap/diff` (bank/written
atom snapshots, sign-safe geodesic angles, rig-generation interlock) and `bsidiff`
(changed-dwords-only snapshot compare). The stance measured as a discrete LEFT-hand
pose (grip+palm rigid 101.11 deg + finger curl, re-onset ~2.5 min, holds), REPRODUCED
on demand by `bsicallat <attach> StartSubtleFidget` - the SubtleFidget lane named as
the mechanism by intervention. Console `set`-by-name proven dead (bHidden positive
control nulled) - one boot, pre-committed pivot to the compose-side lever.

**The ready-pose glue** (the (c) lever, generalized correctly for a name-flat
component-space bank): corr = qRef x conj(src[anchor]) folded per hand into the
compose; anchor pins to controller x captured-ready, relative articulation passes.
qRef auto-captures 1.2 s after every player shot via the new fire seam. A-B-A:
0.33 deg through a stance onset ON, uniform 101.11 deg the instant OFF, 0.06 back ON.

**The fire-origin seam**: offline dump re-run + thunk disasm named
AXPawn::XGetWeaponStartTraceLocation impl 0x5344A0, whose body calls the EXACT
GetPlayerViewPoint impl the camera drive hooks - trace origin = camera eye, the
parallax root read from the disassembly. One choke point (the Floating wrapper's 13
C++ callers route through it), no camera feedback (one-way dependency). Probe: one
call per player shot, 77.4 UU / 51.6 cm engine-vs-hand - the headset's hole-vs-dot
magnitude. Substitution ships ARMED (xyz only, player-pawn gate, 200 UU cap, latched
hand shared with the aim seam, origin sliders cm->UU). Both hands verified; vigor
casts route through the same seam.

**Wrist-cap styles** 0/1/2: style 1 (keep forearm twist) REJECTED flat - giant skin
hood; 0 vs 2 (pinch behind wrist) go to the headset. **Arm-relative wrist quat**
built inert and exact (20 deg commanded = 20.00 on exactly the 4 arm bones).

**Battery green on the shipping build**: 3 stations incl. rolls at 0.0000 both
hands, scale 0.5 uniform, arms transitions, release cycle, SR 90/90/180/90, single
resolve/zero fails, first-shot substitution + auto-captures, no new crash dumps.
vrpreset.ini and XUserOptions.ini byte-identical at close.

Traps recorded: zero-byte command.txt crashes launch-game.ps1 (delete, never
truncate); the offline dump regex misses pooled-suffix exec names (verify with
bsinative); game-shot saves extensionless files.

### Session 45b (Infinite) - 2026-08-08/09 - I8 flat half: the rig derived by intervention, the drive lands, every acceptance number exact

Branch `si45b-inf-hands` off the s44b tip (536649d) - the REDO: the user discarded the
original s45/s46 branches unread ("terrible results"); nothing from them was consulted.
Six commits, nothing merged, ZERO core/tools files touched.

**Derivation before code, all in one boot on the TWN2 save.** The s45b instruments
(bsiarray, bsidump, bsicallat parm echo + i:/n: args) turned the pawn's own
`GetFirstPersonAttachment` native into the front door; the viewmodel renderer fell out
as ONE XSkeletalMeshComponent (43 bones, hands AND pistol) on an attachment ACTOR whose
LocalToWorld rides the camera - the head-coupling is structural, and composing
SpaceBases writes through the live L2W inverse is what cancels it. Identification was
by intervention (HideBoneByName removed the viewmodel; R_Grip alone took hand+pistol
and left the forearm), bank identity by the engine's own GetBoneLocation (0.1 UU),
and the design rested on poke oracles: Morpheme restamps trans+quat+SCALE at tick
cadence even auto-paused, so BS2's scale-row pinning does not transfer, adoption takes
whole atoms, and release is stop-writing.

**The drive is BS2's shape with this game's facts**: one FrameContext feeding both the
ray and the model (agreement by construction), per-hand parallel arrays, name-derived
clusters (grip+palm+digits vs the 4-bone arm chain - the rig is NAME-FLAT, parents all
zero), arms game/follow/hide with the collapse rule, adopt-then-compose, pass-2
verbatim reapply on the camera fork. Acceptance: 1.000 m = 150.0 UU exact; five
stations incl. 60/-90 roll wrote the commanded rotator TO THE UNIT with
aimRayMaxDevDegL/R 0.0000 throughout; hide leaves hand+gun clean; scale 0.5 shrinks
hand+pistol uniformly about an unmoved anchor - the BS2 inverse-scale trap did not
reproduce, so no separate weapon lane; stick-Y moved NOTHING (bit-identical pose), so
publish_vr_gameplay stays uncalled and the snap landmine never arises.

**Calibration surface**: F10 HANDS+MODEL section (L/R radio convention) + aim trim/
origin on ray+laser+dot together; vrpreset 9 -> 36 keys (the user-approved batch).
Four traps recorded in ENGINE_NOTES, two of which cost a boot each: fname_text's
64-byte buffer contract, and the sim's dead `hand X aim pose` slot (aimWorld is always
grip+aimtrim). The other two: the UClass-fixpoint gate (a raw loadout-cache struct
walked as a fake UObject), and modal freezes vs the positive-control rule.

**Test-state discipline**: user's vrpreset.ini byte-identical at session end (backed
up first), XUserOptions untouched, no strays in the repo, all games closed. Headset
verdict rides TESTING.md "S45b hands checklist".

### Session 44 (Infinite) - 2026-08-06 - I7 CONTROLS: the per-game pad map, the Touch layout, and the aim seam derived

Branch `si44-inf-controls` off `bioshock-infinite` at 7c06d09. Five commits, nothing
merged. Scope held: pad map, bindings, aim - no performance work, no lens work, no
refactors.

**What landed.** (1) The instrument that was missing: `vrinput padlog on`, one line
per composed button/trigger EDGE with the bits named, plus a `loc.z` window on the
Infinite heartbeat. Every previous claim about an XR-to-pad mapping had to be inferred
from a game effect; now the bit is read directly. (2) `PadProfile` + two `constexpr
PadMap` tables in core - additive, opt-in, default BioShock 1, one relaxed load per
compose. (3) The Infinite profile armed at adapter init, `bsiinput padmap` + F10
radios, and `inputOn` as a 9th preset key so a headset boot comes up with a working
controller. (4) The aim seam derived and probed. `tools/padsweep.ps1` is committed so
the sweep is re-run rather than re-derived.

**The BS1 proof was run BEFORE any Infinite work**, deliberately, so a redesign would
have been cheap. Per control: faces still A/Y/X/B, RS-click still produces nothing,
grips at 0.60/0.75/0.60/0.50 reproduce the 0.70/0.55 hysteresis exactly, flick still
DU/DD/DL with no DR ever, turn suppression shown by an A-B pair rather than assumed.
claimRatioH 1.01769 == the banked 1.018 as the "nothing else moved" control, zero
faults, and zero `pad profile` lines in BS1's log.

**Two things the flat lane proved outright, and several it honestly could not.**
Crouch (a persistent ~110 UU toggle) and jump (a 98.6 UU arc inside one beat) are
quantitative proofs. LB/RT/LT each move 4-21% of the frame but the whole-frame diff
cannot say WHICH binding fired - RT and LT are near-identical region maps - so they
are recorded as "reaches the game", not as individually proven. NextWeapon and sprint
are bit-level only with the reason stated; sprint's confound was PROVEN by a control
run with no LS press at all, which stopped at the same wall.

**The session's biggest result is a falsification.** The plan opened with "the drive
adds head yaw to the view out-param only, so the shot stays where the body faces -
turn your head 90 deg and the shot goes 90 deg off", and authorised a fix for it. The
probe killed it in one A/B: with the hand parked and only the head moving, the
engine's aim tracks the head degree for degree. Aim is already head-coupled because
the aim chain sits downstream of the camera drive. The evidence-first gate stopped a
fix being built for a defect that does not exist.

**The aim write is implemented, executes at the call rate, and ships OFF** because its
downstream effect could not be established: two shots 70 deg apart in commanded aim
produced pixel-identical frames, and the positive control could not be built (seeing
an impact move needs the same view with different aim, which only the substitution
under test produces). Named negative, mechanism mapped, next instrument specified -
read the trace RESULT, then probe the controller's +0x2F4 that this function delegates
to.

**New blocker found:** the pause menu does not consume the synthetic pad (keyboard
Escape closes it; the game keeps polling XInput at ~92/s the whole time, so the state
arrives and the UI ignores it). Recorded with its leading candidate, not fixed - it
needs its own evidence rung.

**Test-state discipline:** the user's `vrpreset.ini` was backed up before the first
Infinite run and finished byte-identical (8 keys, 2064x2208, GC interval 300 and pose
lag 2 all untouched). All three games closed at the end.

### Session 43 (Infinite) - 2026-08-06 - THE STUTTER HUNT: the 30 s GC tick named by A-B-A, fix candidate live at native res

Branch `si43-inf-stutter` off `si42-inf-judder-bindings` (9dfcc44). Research rung first
(RESEARCH.md s43: vanilla fixes, VR prior art with license flags, UE3 internals -> a
7-entry ranked experiment list, committed before any change). Instrument rung second:
spike-triggered evidence capture in core (pair-close snapshot: per-phase last/max +
stage markers + the unattributed remainder; `spikes=N` on the TRACE pairs line; a 4 ms
mid-stall sampler reusing the s34 watchdog stack capture) - opt-in via `vrpace spike`,
armed with Infinite stereo, BS1 sim-lane proof named in the commit. Flat repro third:
the pad lane WALKS in the TWN2 save, and a turn-and-walk wander at native 2064x2208
reproduced the headset burst signature with EVERY spike outside our code (<= 12 ms in
our phases vs 27-340 ms unattributed) - SR/pacing exonerated. The cause fell to an
A-B-A intervention: the EXACT 30 s spike grid (game thread on an FEvent::Wait(100)
flush barrier, all threads idle) is the engine's timed GC
(TimeBetweenPurgingPendingKillObjects=30); setting 300 via the game folder's
DefaultEngine.ini (the propagation lane to boot-derived XEngine.ini, proven this
session) removed the idle grid AND took the matched wander from 4-7 spikes to 0; the
reversal leg brought the periodic stalls back. Candidate fix left LIVE (backup
`.bvr-bak-s43` beside it); headset verdict + an outdoor save are the session-44 asks;
the texture-pool lane is ranked next for any traversal residual. Rung 0's grant
combination was executed and FALSIFIED honestly (CreateInventory/AcquireWeapon
dispatch+return but never register in the plasmid/weapon cycles - the loadout-manager
registration is the missing seam); LoadCheckpoint's slow-load behaviour (60-100 s)
and one more attract freeze recorded. game-key.ps1 grew `-Game bsi`. The user's
mid-session directive - no fixes until the cause is guaranteed by evidence - is
recorded in the plan and honored by the A-B-A.

### Session 42 (Infinite) - 2026-08-05 - I6 judder flat half + I7 opens: pad lane live, the exec surface mapped honest

Branch `si42-inf-judder-bindings` off `si41-inf-lens-config` (a7a34be), three commits.
Rung 1: pair-cadence jitter instrument (TRACE pairs: interval mean/sd/min/max + waitGate)
plus the first-ever consumption of predictedDisplayPeriod; the sim GATES (pairs lock to
refresh at 72 AND 90, waitGate ~0.6 s/s) so the free-run beat needs a pipelining runtime -
VDXR answers via pacetrace on the next headset run; `vrpace sync` pair-open-only rate cap
lands default-off-in-core / on-with-Infinite-stereo (F10 A/B), measured near-inert against
a gating runtime and proven inert for BS1 (full sim lane, claimRatioH 1.01769, named in
the commit). Rung 2 falsified its own premise with instruments: script execs dead through
ConsoleCommand (pixel-identical screenshots), give-family names absent from a full GNames
dump, XCheatManager never spawned - so the lane pivoted to ProcessEvent-on-the-owning-
object: bsinames dump / bsifields (UObject::Name derived live at +0x18; PC field map incl.
pawn +0x1FC, XCamera +0x240 confirmed) / bsicallat; LoadCheckpoint proved as the
autonomous menu-to-save lane; AcquireWeapon identified as the s43 grant seam (wants a
weapon object); the weapon-in-hand acceptance blocked by BOTH user saves predating the
first story weapon - s43 wants a post-raffle save (or the archetype-lookup rung). The
bsilens viewmodel guard ran in-save: lens1 == lever exactly (delta 0.0%), lens2 absent,
caveated on the no-weapon state. Rung 3: bindings audit complete (retail pad map + chain
semantics -> ENGINE_NOTES; core's BS1 pad semantics wrong for Infinite on three named
counts); kXInputGetStateIatRva verified live and hijacked; the sim's right stick TURNED
the camera and A pressed - the synthetic-pad lane is live flat, per-game map to s43. One
pre-existing attract freeze (force-kill; two clean loads after with confirm-pump-first).
User's vrpreset.ini and 2064x2208 restored and verified untouched.

### Session 41 (Infinite) - 2026-08-05 - I6 flat half CLOSED: FOV lever + lens decoder + resolution + presets, every done-when number measured

Branch `si41-inf-lens-config` off `bioshock-infinite` (8feae7f), six feat commits + docs.
The session opened with a planning finding: the live XUserOptions.ini had FieldOfView=1.0
(slider at MAX) so the shipped claim default (slider-min 0.4317) was stale on this very
machine - the milestone's honesty problem demonstrated before any code.

The lever: set-by-name tried FIRST and measurably dead (`set XUserOptionsManager
FieldOfView` writes nothing - zero stable holders for the written value; FOV/SetFOV execs
inert), the live chain mapped by poke/rescan (camera holds 82.50f at [cam+0x214] and the
POV fov at [cam+0x3D0], tan(82.5/2)=0.8770=the decoded tanH exactly, every holder
recomputed per tick), so the lever ENFORCES both fields per camera dispatch - measured
exact and monotone at 110/130 deg, 0 faults, self-restoring disarm.

The law's anchor: at 1440x1440 the first-cut claim (current-aspect) sat 43.7% off and the
new lens decoder flagged it - the degrees value is horizontal at a FIXED 16:9 reference
(tanV = tan(deg/2)/1.7778 pinned; both decoders 0.6704 exact after the fix, claim delta
0.0%, claimRatioH 0.48705 vs 0.4871 predicted). The decoder (opt-in core UpdateSubresource
tap + adapter matrix-law vote: aspect-gated, 60%-of-16 majority, named runner-up, refusal
over confident-wrong) also caught the stale boot claim at 14.3% on its first round - the
audit instrument earned its keep twice in one session.

Resolution: bsires/picker applies live setres AND the section-scoped XUserOptions.ini
write; the next boot's `first Present: backbuffer 1440x1440` banked the DR-I8 acceptance
on the mod's own write path. xrEnumerateViewConfigurationViews landed additively in core
(recommended_eye_size; sim serves 2064x2208) with a full BS1 sim lane re-run as the named
inertness proof (claimRatioH 1.01769 = the banked 1.018). Config registry + named presets
landed adapter-local by decision (ARCHITECTURE log; core extraction deferred to healing):
preset round-trip 6/6 across a full restart, legacy vrpreset files still load, resolution
LATCHED on load (never auto-applied), and an `eye` preset banked (lever 137 = tanV 1.428 ~
the eye's vertical, 1600x1712).

Hazards: the pre-existing unattended-attract freeze hit twice (zero mod faults;
force-kill + relaunch) and the pump lags 30+ s at attract movie transitions -
send-one-confirm-one is now gotcha 20 (21 = xrsim-shot littering the CWD with
game-derived captures; deleted before commit). The headset half is a ready checklist
(TESTING.md "I6 in-headset checklist"): the filled-eye verdict + the three carried I5
items (world scale, judder at VD 72 Hz, 30-min soak + level transition).

### Session 40 (Infinite) - 2026-08-05 - I5 CLOSED as re-scoped: stereo flat-green AND headset-verified on VDXR

**The verdict (user, VDXR, same day):** "there's stereo 3d rendering and it's working
well"; nothing broke; slight judder on head motion. Log from their run: 77-80 eye pairs/s
(155-160 presents/s) at default scale, all SR gates exact, zero foreign skips, no faults -
above the 72 fps target; the judder fits ~80 pairs under a 90 Hz refresh (VD 72 Hz
suggested next time). The "looking through a window" percept was confirmed EXPECTED: the
honest claim renders 75 x 47 deg inside a ~108 x 110 deg eye - I6 exists to fill it.
Carried to I6's headset session: the world-scale tune (slider live, feel unjudgeable
through the window), the judder verdict, the 30-minute soak. I5's five flat boxes plus the
re-scoped Done-when are ticked in the ROADMAP.

Branch `si40-inf-stereo` off `bioshock-infinite` (d3007ba). The ladder ran exactly as
planned - mono projection, AlternateEye, SequentialReentry - each rung flat-validated
before the next, everything Infinite-local (+ an additive CMake file-list entry).

**Rung 1, the projection flip.** The adapter publishes the FOV claim per detour call from
the I2 law (`2*atan(tanV x aspect)`, tanV default = slider-min `kTanVSliderMin` 0.4317 -
now a named constant - with `bsifov tanv` as the lever and vrpreset persistence) plus
`publish_gameplay_view(true)` (core's cine fallback defaults ON; a stale publish parks the
quad - and its fovMismatch/screenOnly legs fail safe on UE3, verified in core source).
`vrstereo` one-toggle + F10 "VR stereo (I5)" section (posts to the game thread). Flat:
core's first projection-layer frame on toggle, audit `src=readback tanH=0.767467` (the law
exactly), projectionViews=2, **claimRatioH baseline 0.5576 derived fresh** vs the
symmetric 54-deg sim eye, window img-diff 13x floor under simhead yaw, quad fallback on
off. The claim's honest caveat is documented everywhere: no live option reader until I6,
so the in-game FOV slider must sit at minimum.

**Rung 2, AlternateEye.** `ue_rot_basis` added to inf_math.h (the Vengeance formula in
shape; the frame is the s39-measured one), `apply_eye_offset` = sign x ipd/2000 x
worldScale along the full-rotation right axis, `g_ipdMm` 63 default with F10 slider +
vrpreset. Flat: inter-eye |d| exact (3.150 UU at scale 50, 6.300 at 100), both signs
observed, L/R capture stats differ under AER.

**Rung 3, SequentialReentry - the session's real work.** The static walk to the scene
root dead-ended twice (basic-block boundaries, UTF-16 pointer noise), so the derivation
went LIVE: a caller census at the camera detour (ret `0x26B499` exactly once per present),
one-shot backtrace + raw stack scrape (`bsicam stack`), and one-shot vtable probes
(`bsicam scenedraw`/`vtprobe`). Chain: viewport draw `0x1FDE30` (thiscall+1 arg, ret 4;
canvas ctor `0x331110` -> client-draw dispatch ret `0x1FE05F` -> canvas dtor -> present
kick `0x1E50B0(1)`) -> client draw `0x26A3E0` (vtable `0xDE6FC8` slot +0x8 via jmp stub
`0x6F1360`; holds the IsA-gated controller camera loop) -> GetPlayerViewPoint. **Recorded
negative that chose the root:** doubling the CLIENT draw doubles camera+scene but NOT the
present (tag ring skews +1/tick) - the SR root must contain camera + scene + present
(ARCHITECTURE decision log). scenedraw.cpp doubles the viewport draw: deny-by-default on
gameplay caller ret `0x206309` (4 static callers), camera-silent/present-stall/teardown/
poison gates, SEH-guarded second call, draw-stage markers, pulse instrument. camera.cpp
pass-2 fork replays pass 1's CACHED base absolutely (+1 eye, 100 ms staleness, burst
counter on the pass seq). Pair pacing armed in the vrstereo ladder; NO 1t machinery -
threaded doubling ran clean, exactly as DR-I5 predicted.

**Flat acceptance:** `draws/s=90 2nd/s=90 presents/s=180 camReplays/s=90` at the sim's
90 Hz ceiling, call2 80-215 us, inter-eye exact, SR capture pair genuinely differing
(mean 0.42 / 1.09 % channels) against a byte-identical mono control pair, deny gate
observed refusing a foreign caller live, 15-minute armed soak with zero faults / zero
watchdog / zero poison and only self-healing tag-ring resyncs at attract transitions.
Full runbook: TESTING.md "I5 battery"; headset checklist "I5 in-headset checklist" (true
parallax, the CARRIED world-scale tune, 72 fps, 30 min, transition + quit-to-menu).

New derivation instruments kept on the seam: `bsicam callers` (ret-RVA census with
present-delta), `bsicam stack` (backtrace + call-preceded stack scrape), `bsicam
scenedraw` / `bsicam vtprobe` (live virtual-dispatch resolution). Two harness traps
recorded in ENGINE_NOTES s40 (PS 5.1 vs cmake stderr on reconfigure; game-shot -Out is
extensionless).

### Session 39 (Infinite) - 2026-08-05 - I4 6DoF head drive + simhead + vrrec, flat battery green; headset corner-lean pending

Branch `si39-inf-head-camera` off `bioshock-infinite` (2923d25). Order of work per the
BS1-learned rule: the flat instruments landed WITH the drive in one commit and were exercised
before any headset ask. `drive_view` in the GetPlayerViewPoint detour tail substitutes the
OUT-PARAMS only (never `[cam+0x3B8]`/engine memory): yaw additive on the game's own yaw,
pitch/roll absolute, position = recenter-relative XR delta -> UE axes -> x worldScale (default
50, UE3-canonical, NOT BS2's 100). Lane order replay -> simhead -> live; the live lane never
calls `set_camera_mode` - core flips quad->projection there, which is I5's rung (ARCHITECTURE
decision log). simhead is BS2's shape with the position triple; vrrec is BS1's BVRR v1
adapted to a present-count-edge cadence (this seam fires many times per frame); heartbeat
gained the `[bsi] drive:` FINAL-camera line with engineRot + pitchErr as the read-back
discipline (pitchErr logged, never published - the core servo would seize right-stick Y;
I7's lane). New files `inf_math.h` (adapter-local UE3 math, each convention a falsifiable
claim) and `recorder.cpp/.h`; zero core/shared/BS1/BS2 edits by construction.

The whole flat battery ran at the attended attract (which plays real gameplay scenes) and
passed with exact numbers: passthrough d=0; yaw residual exactly 5461 units (30 deg) on
three beats against a MOVING engine camera; pitch 3640 / roll 1820; all three position axes
exact in UU incl. the game-yaw rotation and world-up Z; worldscale doubling; the real
OpenXR path (`head rot/pos/orbit` -> xrLocateSpace -> get_head_pose) exact; vrrec 1077-frame
round trip with PLAY marks number-for-number identical to REC and lane=replay at xr=none;
window img-diff 6.12 mean / 23.4 % changed vs a 0.58 / 1.12 % floor (10x) with the heading
visibly rotated; 90.0 fps sustained; vrpreset round trip; clean WM_CLOSE exit. Runbook now in
TESTING.md "I4 battery" + the in-headset checklist for the Done-when corner-lean.

Found and recorded: VERIFICATION gotcha 17 - a sim `recenter` with the head yawed blanks the
quad capture (falsified both ways: yaw-110 -> 1 covered pixel with a bright window; yaw-0 ->
pixels back), and even then the captured quad sits off-centre - the capture's layer transform
after a reference-space change is a healing-lane candidate. The window is the pixel
instrument for camera-drive questions.

**Headset session (same day, VDXR): I4 CLOSED.** User verdicts: corner-lean tracked with no
drift; roll correct; the head-driven camera on the static big screen confirmed as the
intended MonoTracked rung. World-scale feel not judgeable on the mono screen - tune deferred
to I5's checklist. Core's VR-section camera toggle observed inert - by design (the
projection flip waits on the FOV law, I5/I6); noted in the checklist as expected noise. All
six I4 ROADMAP boxes ticked.

### Session 38 (Infinite) - 2026-08-05 - I3 sim battery green; the sim's clock bug, dark captures and sticky focus-lose found and fixed

Branch `si38-inf-headset-bringup` off `bioshock-infinite` (c595e27). Order of work: harness
first (`xrsim-launch`/`launch-game`/`xrsim-run` gained `-Game bsi`; bs1/bs2 preflights re-run
green as the inertness proof), then `bsivr on|off|status` (adapter-local over the public
`vr::set_enabled`; status reports `session_live()` because the F10 checkbox is a second
writer), then the attended sim battery (user drove boot -> save both boots).

**Everything in core just worked on Infinite** - no bring-up code was written. The battery
instead caught three SIM bugs, each with a derivation: (1) `now_xr_time` overflow -> pace
thread parked in `impl_WaitFrame`'s free-mode wait ~25 min in (WinDbg stack from a live
minidump; sawtooth `displayTimeNs` in capture JSONs; mod-side state `waitFrames=beginFrames+1`
with every present timing out its 200 ms handoff deadline - the mod's own 1:1 discipline held
correctly); (2) capture composite stored sRGB-decoded linear (Infinite quad capture meanLuma
0.05 vs ~10 in the window; PNG histogram cross-check matched the sim's stats, so the stats
were honest about wrong pixels); (3) timed `focus lose` clobbered to sticky by the
FOCUSED->VISIBLE edge (BS1's sequence uses explicit `focus regain`, so it never saw it).
All three fixed in `src/tools/xrsim/`, selftest green after each, and the full BS1 lane
re-run on the fixed sim (`-AllowStale` keeps BS1's shipped v0.7.0 mod): smoke green, stereo
projViews=2 / eyeSep 0.063 / claimRatioH 1.018 IDENTICAL to s37's geometric baseline, new
pixel-scale baseline L/R diff 11.53 (was 3.16 on the dark scale).

Measured on Infinite: FOCUSED ~600 ms from first Present; 90.0 fps; `step 5` exact; focus-loss
frame rate holds and submission continues through VISIBLE; `bsivr off/on` teardown /
re-bring-up ~250 ms; `hazard waitfail 1` -> teardown -> fresh session post-cooldown; live
`setres 1600x1200` -> queued XR rebuild -> pair 1600x1200 + quad 2.4x1.8 m + hfov 124.6 deg,
and back. Two boots, two orderly WM_CLOSE exits (4-6 s, no fault, no dump) - but with a live
sim session the `DLL_PROCESS_DETACH` breadcrumb is absent on BOTH games (sessionless s37 exits
logged it): sim-lane watch item, check the breadcrumb on a VDXR exit.

Traps catalogued for future sim sessions (VERIFICATION gotchas 13-16): the sim's LOCAL origin
starts at the FLOOR (send `recenter` before quad-pose captures - the eye-level quad otherwise
renders 1.6 m low at a grazing angle); `idle on <ms>` is persistent until `idle off`; a
`pace free` after step-credit exhaustion latches until the 30 s starve grant; Infinite
auto-pauses unfocused (foreground before gameplay captures; menu never unattended).

Headset run (user, same build): VDXR lane PASSED - "looks pretty good, no crashes or
freezes/hangs"; the log shows `VirtualDesktopXR` 1.0.10, the F10 VR A/B as a live VDXR
teardown/re-bring-up, and two boots. The VDXR exit also lacks the `DLL_PROCESS_DETACH`
breadcrumb, which RECLASSIFIES the watch item: live-session exits are TerminateProcess-class
on any runtime (benign - prompt, fault-free, dump-free); sessionless exits log the breadcrumb.
SteamVR lane deferred by the user (no Steam Link hardware, "not needed for the first
version") - debt carried on the release milestone. **I3 closed as re-scoped**: Done-when ticked with the
re-scope recorded; the cross-check box stays open pointing at the release milestone.

### Session 37 (Infinite) - 2026-08-05 - main merged into the Infinite line, and I2 CLOSED

Branch `si37-inf-merge-and-derisk2` off `bioshock-infinite`. Step 0 was the merge of `main`
(BS2 v0.7.0, 124 commits, base 76052f1): 6 conflicts, resolved per the standing policy
(keep-both on additive, BS2 wins behavioural - none were genuinely behavioural); the
decode-framedump param-block union kept both sides' instruments and the 336-cap removals; the
BS1/BS2 inertness proofs were RE-RUN on the merged tree and cited in the merge commit.
Acceptance: clean Release build, decoder self-tests + the offset-12 BS1 regression, BS1 AND BS2
smoked green on the xrsim simulator (BS1 to full stereo: projectionViews=2, eyeSep 0.063 m,
claimRatio 1.018; BS2 to XR-session FOCUSED + seam dispatch), Infinite to `camera: READ-ONLY
hook installed` / `pump=game` / `bsireflect selftest` 15/15.

Then the battery closed I2 - every remaining DR measured by effect on the merged build:
the TRANSFORM question (raw copy on path 2 -> I4 injects at the POV), DR-I4 (stereo negative,
live pool, with `AllowNvidiaStereo3d`@4154 as the positive control), DR-I5 (threads separate
under both `OneFrameThreadLag` positions; ring-shaped substrate; latency half deferred into I6),
DR-I6 (new `bsicall`/`bsiexec` by-name dispatch; **`setres` resized the backbuffer live**;
`set FOVAngle` no effect - property known dead), DR-I7 (HUD fingerprint confirmed at a second
scene+resolution; the eye-image rule is positional and classifier-free), DR-I8 (the real
config store is `XUserOptions.ini ResolutionX/Y` - `XEngine.ini` is a boot copy that silently
discards writes; `first Present: backbuffer 1600x1200` accepted; setres live too), and the FOV
law (VERTICAL-referenced: tanV pinned across aspects, tanH = tanV x aspect).

Also found: unattended attract/menu hangs the game - reproduced on the UNMODIFIED session-36
build, exonerating the merge; watchdog stack photos banked. Infinite exits cleanly via WM_CLOSE.
An electricity outage cost one mid-session reboot; nothing was lost. Instruments added:
`bsicall <Func> [float]`, `bsiexec <console cmd>`, `camera_tid()` and the `kFindFunctionRva`
interlock. NOT started, per the brief: anything I3+, the OpenXR runtime on Infinite, the repo
restructure.

### Session 42 - 2026-08-04/05 - the presentation lane: HUD panel, screens, cinematics, menukey, crosshair, flicker instrument

Branch `claude/bioshock2-presentation-vr-2a7b3a` off `bioshock-2`, merged back.
The plan's audit held: all presentation machinery is core; BS2 lacked dispatch and
consumption - EXCEPT the classifier itself never fired on BS2, which became the
session's core discovery: **BS2 is a backbuffer-composite pipeline** (gameswf on a
RENDER_TARGET-only backbuffer, INDEXED 6-idx tonemap sampling the 612-vote scene
leader - framedump_232940 evidence) and BS1's fingerprints structurally miss it.
Shipped a flag-gated core mode (adapter opt-in, BS1 bit-identical off) and the full
stack came alive: 12352 redirects, 0 leaks, 0 stranded, HUD quad as the 12th layer
(space=view), composite intact, preset round-trip through a relaunch.

Landed: vrhud force+counters; HUD quad + cine + crosshair preset keys (59 values at
boot); vrcine dumparm (edge-armed one-shot dumps; bars edge auto-armed at init -
the C10 bars-verts harvest); cine gate predicate fix (cinematic_hold, not the dead
letterbox()) + cine-drive consumption in camera/aim/hands/wskel + authored+look +
the s29 release interlock leg + wskel_release; menukey (pad A -> scancode Enter,
3-leg menu gate; title-continue proven NATIVE-A; gameplay negative clean); the
crosshair hidden by default via ShockPlayer.DisableReticle through the engine's own
FFC+ProcessEvent (the PE-by-name precedent; GNames reverse lookup; A/B proven);
the flicker instrument ([flick] per minute, catch phases + dmax + cadence baseline,
4 min proven; ambient baseline banked).

Discoveries banked in ENGINE_NOTES s42: the pause menu is SCREEN-ONLY (world absent,
unlike BS1) AND starves the whole PE-tail service lane (commands + pad dead while
paused - pre-existing, structural); the title screen preloads the save under itself;
BS2 HUD fills are textured (no BS1 bar-fill collision); postFx idles in gameplay
(post chain is CopySubRes-based). Deviations logged in ARCHITECTURE: postfx-cine
fallback OFF on BS2, no watchdog-exemption port (no watchdog exists), generic
screens, menukey as BS2-local translation, script-setter precedent.

Open into session 43: C10 bars constant (rides the auto-harvest), the headset
acceptance (checklist above), main-menu pad-A verdict, the flicker readout.
Teardown clean x4, zero dumps; sr eyes 6.30 exact; drive 90/s; BS1 untouched
beyond the three additive core seams.

### Session 41 rounds 2-3 - 2026-08-04 - in-headset acceptance, the bake, weapon offset

Two same-day headset passes. Round 2: EVERYTHING accepted (animations, sway,
arms-hide, uniform scale, per-weapon profiles, F10, no regressions); the user
calibrated all 8 weapons + both hands. Shipped in response: the calibration baked as
code defaults (virgin-boot proven), the flush-point flicker rung, and the per-weapon
WEAPON OFFSET (attach-pivot base - gun moves, hand/aim stay; flat-proven localized
with write-locs bit-identical). Round 3: offset "perfect" (values re-baked:
MachineGun -7.47, RivetGun -6.30, Shotgun -11.44 + modScale 0.79, Speargun
-7.24/-2.10); laser/dot per-hand F10 toggles + preset keys added; THE FLICKER
SURVIVES both rungs, reduced, now suspected TIME-correlated (~10 min) - reclassified
as a diagnosis item (instrumentation plan in ENGINE_NOTES s41 r3). Session-42 brief
(user): THE HUD LANE.

### Session 41 - 2026-08-04 - the holdable lane, the animation-preserving drive, polish flat-green

Branch `claude/bioshock2-holdable-polish-7cbec4` off `bioshock-2` (5f9b0da), merged back.
Every round-2 defect addressed in code and flat-verified in two sim boots at the save.

- **Retarget drive**: per-bone 32-byte adoption of the engine's evaluated pose (only
  when the bank stopped being our write; unconditional for bones entering the driven
  set; other-hand-masked skipped), compose `q = qtc * animQ` - engine animations play
  in the driven hand's space and restamps become input. The design review caught
  scale-adoption compounding BEFORE it shipped (the engine never restamps scale; the
  channel is pinned to g_ref, structurally). Absorb-then-recompose PE repaint (pass 1
  only; new side-effect-free `scenedraw::in_second_draw()`). `vrhands anim on|off` +
  `animtrans`. Flat: adoption ~7/drive/hand, drill-region motion on a trigger pulse,
  scale-flick decays with no alternation, write-loc 100 UU/m exact, anim-off bitwise
  stable. INSTRUMENT CORRECTION: axes 'cur' races the restamp war - use the new
  race-free `written q / anim q` line.
- **Holdable lane, constants all fresh**: UObject identity +0x28/+0x30 with UClass
  vtable 0x11E71F8 (3 classes stable); Hands.CurrentHoldable = hands+0x4B4 (seam find
  + switch diff agreeing; BS1's 0x45C did not transfer); weapon SkeletonInstance =
  holdable+0x430 (two-factor). `object_class_name`, `current_holdable`,
  oclass/holdscan/wskel derivation probes.
- **Uniform weapon scale** (`vrhands wscale`): the weapon's OWN pose bank, scale +
  translations about the component origin - the canister repro INVERTED (0.5 halves
  the whole rivet gun; 2.0 uniform; 1.0 restores + hands-off); weapon anims keep
  playing while scaled. scaleweapon fallback default OFF.
- **Per-weapon profiles** (session-21 rules a-d): RIGHT hand + wScale per weapon
  (USER DECISION: left stays global until per-plasmid keys exist), pre-fire keying,
  stash/seed-from-baseline/restore with no edit leak, weapons.ini round-trip (2
  profiles / 26 values), preset ordering load -> note_preset_baseline -> reapply,
  save chained. `vraim weapon|wsave|wkey`.
- **Polish**: arms-hide collapses onto the driven wrist (web fix); F10 top PRESET
  section (APPLY + SAVE, "applies/saves ALL settings and values", duplicates
  removed); vrinput default-ON at boot (proven pre-command at 90/s); per-hand
  aimRayMaxDevDegL/R in the sim (dual-beam L 0.0000 / R 0.0000; legacy field kept).
- Preset 43 values (was 22). Guards: 11 layers, sr eyes 6.30 exact, wait2/guardskips
  0, teardown 478/463 ms zero dumps. Core diff: ZERO (sim-tool JSON fields only).
- Session 42: in-headset acceptance, plasmid names in ContentBaked -> ability seam +
  per-plasmid left profiles, pad-A activation, the alt-tab wedge (own session).

### Session 40 - 2026-08-04 - BS2 plays on the controller; hands split; the ~90 deg fixed

Branch `claude/bs2-controller-input-decoupling-d7cc14` off `bioshock-2` (0abb6b5), merged
back. Three simulator boots at the user's save, every M10.1 code box ticked.

**The session turned on one offline question asked first**: does BS2's binary still contain
a per-frame pad poller that nothing calls? The plan made that a go/no-go gate rather than an
assumption, and it paid - `UWindowsViewport::UpdateInput` is at viewport vtable slot 73 with
ZERO callers, so BS1's shape ported. Pumping it per present + `SetUseController` (client slot
73; the shared slot number is a coincidence and is banked as two constants, because BS1's
single shared constant was a layout accident waiting to bite) makes the engine consume the
synthetic pad: sticks walk, triggers fire through the seam, the dpad navigates the menu, and
the game's own UI switches to controller prompts. The same offline pass answered session 39's
open question for free - the engine never calls XInputGetCapabilities, so the caps asymmetry
in the core bridge was never the cause, and the core "fix" it suggested (which would not have
been inert for BS1) was correctly not made.

**The ~90 deg misalignment was a composition bug, and a new instrument found it in one
reading.** `vrbones axes` prints the anchor's driven vs authored rotation - the mesh-level
read `aimRayMaxDevDeg` never was, which is exactly why the defect survived to the headset.
The rigid map was replacing the anchor's authored frame with the raw controller rotation;
that frame is ~81.6 deg off the view frame on this rig. `delta = qtc` instead of
`delta = qtc * conj(refQ_anchor)` keeps the authored pose and turns it, giving 0.21 deg at
rest. The planned baked constant turned out to be unnecessary - the correct composition is
self-deriving, so nothing needs re-banking when a weapon or animation changes.

**Hands split on real names**: the bone-name map auto-detected at SharedSkeletonData+0xB4
(64/64 named, single accepted candidate), giving left = 7 + 8..28 + 62 and right = 36 +
37..57 + 63, where 63 is the weapon attach (proven by driving that one bone and watching the
gun move). Per-hand tracking is exact: 35.0 UU on the moved hand, 0.0 on the other.

Also shipped: worldscale-independent `vrhands scale` (anchor invariant to 0.00 UU),
bullet-origin substitution (61.9 UU displacement, with the 200 UU clamp BS1 never had), dual
lasers + dual dots at the user's request (BS2 is natively dual-wield - the one core change,
strictly additive, 11 of 16 layers), an F10 per-hand calibration panel, and 14 preset keys
(22 values loaded on a fresh boot, was 8).

**Two findings worth more than the features.** `aimRayMaxDevDeg` assumes ONE laser: with both
beams live it reads 47-75 deg and looks precisely like an aim/model regression, while
single-beam at the same pose reads 0.0000 - the acceptance must be run single-beam until the
metric is per-hand. And `vrhands offset ...` had been silently parsed as `vrhands off` since
session 39 (prefix-matched verb where one verb prefixes another), which is why the preset kept
persisting zeros; the round-trip test is what exposed it.

Blocked, honestly: the ability-seam live check needs a projectile plasmid, and `<X>BasicPlasmid`
exists only for Telekinesis in the exe - Telekinesis provably does not traverse
GetPerfectFireStart. Per-weapon presets were deliberately split out of their box rather than
ticked, since no tuned value source exists until the user calibrates in-headset.

Teardown 486/523/211 ms, zero new dumps, across all three boots.

### Session 40 round 2 - 2026-08-04 - two in-headset passes, the retarget diagnosis, the holdable lane

Same-day continuation after the user's first look PASSED the core acceptance. Round-2
build shipped per-hand aim sliders in F10, arms follow/hide/game, the weapon-scale
toggle, a PE-lane restamp repaint, and the full preset (aim trims/pos, arms, scale-
weapon, dot length, turnscale/snap/ammomod - 14 -> 22+ keys) with a one-button APPLY
that also arms the controller. Second in-headset pass verdicts: flicker SURVIVES and is
triggered by scale changes; some weapon animations never play (the rigid drive erases
them - one root with the flicker; fix = retarget engine animation deltas onto the driven
frame); arms-hide stretches a web to the authored bone spots (collapse onto the wrist);
aim tuning must be per-weapon (BS1 weapons.ini shape); weapon scale must be uniform-down.
The scale experiment PROVED the ammo canister inversely rides pivot 63's scale in the
engine's attach math - unreachable from the AHands bank; the weapon's OWN skeleton is
the lane, shared with the per-weapon-profile holdable resolution. Also banked: one-shot
scale pokes do not render on this rig state (drive-path attribution only); a fixed
pre-existing `vrhands offset`-parsed-as-`off` bug would have kept persisting zeros; the
alt-tab pacing wedge reproduced twice with a clean signature (restart-only recovery).
Session 41 brief: holdable lane (uniform weapon scale + per-weapon profiles), the
animation-preserving drive, hide-collapse, F10 top APPLY/SAVE section, plasmid names in
ContentBaked, pad-A activation, per-hand aimRayMaxDevDeg.

### Session 39 - 2026-08-03 - BS2 motion controls: decoupled aim, laser + dot, bone drive, all flat

Branch `claude/bioshock2-motion-aiming-c7daed` off `bioshock-2` (a993e80), merged back to
`bioshock-2`. Priority 1 landed end to end, flat, in four unattended sim boots at the
user's save; priority 2 (cheats) landed as a boot-1 rider on the first try.

- **Stage 1, the probe**: GNames derived fresh offline (ctor 0xB813B0 -> worker 0xB81CE0
  -> GNames 0x1A614D0; BS1's session-20 recipe, every number new), Lane-A FName globals
  (BeginFiring/UseAbility/InitiateDamage resolve; GetPerfectFireStart/ApplyAimError have
  no cached global - they live in a boot-time batch registration at ~0x976640), and a
  PE census whose UFunction name offset self-derives (+0x28, validated by 73 sane
  names). Live verdict in one boot: the seam is NATIVE (0 PE hits over 6 fires),
  InitiateDamage is the PE-visible 1:1 anchor.
- **Stage 2, the seam**: weapon impl via vtable-slot census (slot 221/+0x374 ->
  0x89DCB0, shared by the whole weapon family incl. the drill), ability impl via a
  targeted .text sweep (0x81CE80, non-virtual, args incl. the `tester` ShockPlayer).
  Both hooks identity-gated and live from boot. Decoupled aim proven with the
  region-mean method (whole-frame diffs lie in this dark scene): impacts moved
  crosshair-band -> right-band (0.1% -> 12.4%) on a +30 deg substituted yaw with the
  camera heartbeat bit-identical.
- **Stage 3, rays/laser/dot**: b2r frame_context.h; the hand ray substitutes 1:1
  (delta 25.00 deg exact against a -25 deg sim hand); 8 compositor layers under SR
  stereo; dot round-trip 0.0000 UU.
- **Stage 4, the rig**: AHands -> SkeletonInstance at +0x430 (two-factor identity),
  64-bone hkQsTransform pose bank at +0x44 poke-proven to render; bones.cpp rigid
  cluster drive (no lock domain) + hands.cpp policy; coupling acceptance PASS -
  aimRayMaxDevDeg 0/0/0/0.02/0 constant, write-loc at exactly 100 UU/m (the fresh
  world-scale measurement), model-only diffs localized and moving.
- **Cheats**: F9=GiveAll + F12=GiveWeapon bound in [Default] only (backups kept),
  verified by effect; digit keys switch weapons flat. New harness facts: mouse
  buttons fire in gameplay (the raw-input wart is menu-only), the drill has no aim
  seam on air swings, `vraim`/`vrhands`/`vrbones` grammars shipped.
- **Input verdict**: the engine polls XInput twice at boot pre-bridge and never
  again; the WinDrv ini lever alone does NOT wake it. input_drive port = session 40
  P1; until then controllers aim/fire-substitute but do not drive locomotion.
- Teardown with everything armed: 470 ms, zero dumps. Core diff: ZERO. Session-37/38
  baselines observed intact live (sr eyes 6.30 exact, wait2/s=0, fov law, self-heal).

### Session 38 - 2026-08-03 - the exit crash was the game's own; instant dump-free closes; aim seam found

Branch `s38-b2r-teardown-and-aim` off `bioshock-2` (50a8131), merged back to `bioshock-2`.
The brief's priority 1 (teardown crash) inverted under evidence: `tools/read-dump.py` (new,
python-minidump) read all five banked dumps - three were the SAME null read at
`Bioshock2HD.exe+0x4FF0FE` (the engine's display-apply virtual dereferencing a nulled
subsystem member during close-time message dispatch, slot 61 of the engine-family vtable at
RVA 0x10BD7DC), and the "few-second exception loop" is a chained CSERHelper filter retrying
the faulting instruction 86k times. An unattended close-repro bisect (WM_CLOSE poster +
teardown-CPU sampling + the new `BVR_SKIP` subsystem lever + `BVR_VEH` first-chance
observer) then eliminated the mod entirely: the fault fires passive, without an XR session,
and in run G4 with EVERY hook skipped - while vanilla "exits cleanly" only because nothing
observes its 0.1-s-CPU teardown as CSERHelper eats the same fault. BS2R crashes on its own
exit path, per its Steam reputation; the mod had been amplifying that into a 58 MB dump per
close plus a multi-second spin.

Shipped accordingly: teardown-aware crash handling (overlay WndProc notes
WM_CLOSE/WM_DESTROY/WM_ENDSESSION; from then on a fault gets one log line, no dump,
immediate TerminateProcess) - closes now measure 0.1-0.3 s with zero dumps, faster than
vanilla; BS2 adapter hygiene gates (doubled draw, 1t force, FOV writes via live-CalcView
OFF edges, window self-heal) and a drain-guard poison-hardening for the gameplay-quit dump's
freed-scene class. Acceptance: three echo-verified armed closes + a 5-min armed soak (308
1T beats, wait2/s=0, guardskips 0) all closing instantly with zero dumps. The arm-timing
lesson: the poll gate ticks only after the menu's first CalcView (20-55 s variance) - key
scripted arms on the log line, never on a fixed wait.

Priority 2 (aim derisk, offline): BS2's fire seam family EXISTS with BS1's exact shape -
`GetPerfectFireStart(out loc, out Rotator, out effectLoc)` on Weapon AND AttackAbility, the
full BeginFiring/AnimNotify_UseAbility/InitiateDamage chain intact, and it is NATIVE
(`execGetPerfectFireStart` present as a WIDE string - this build stores script names UTF-16,
ASCII sweeps see nothing). BS1's nativemap recipe does not transfer verbatim; the dispatch
question (ProcessEvent-visible vs native-to-native) is queued as session 39's one live
probe. ENGINE_NOTES gained "Fire flow / aim" and the full teardown derivation.

Wrap-up round (user: "everything can be tested in flat so do it"): the in-game quit
acceptance PASSED flat (save written, WM_DESTROY noted post-save, zero dumps, bounded
exit) after catching a real deadlock in the first teardown gate (stopping the forced
inline flush mid-worker-teardown never completes; reverted - forced-inline runs through
close) and adding a 15 s exit watchdog to `note_teardown`. coupling-viewmodel PASSED at
the save (world 4-5x animation floor under yaw/strafe, drill block AT floor =
view-locked). Boot-to-save is now fully unattended (Space at the title continues into
the newest save; BS2 menus need KEYBOARD driving - their raw-input cursor ignores
SetCursorPos clicks; zombie-process handling fixed in game-key/game-shot/game-cmd). The
user's checklist for next boot is EMPTY but for headset ride-alongs. Session 39 brief
(user, 2026-08-03): motion controls - laser/dot aiming with models moving in sync with
the controllers (BS2-native: NO BS1 lock domain, whose `lock abs` correction caused
BS1's +-90 drift; `coupling-hand` + constant `aimRayMaxDevDeg` as the sync instrument),
BS1-parity controls incl. the thumbrest ammo modifier, cheats lane if time remains;
session 40: cheats spillover + aim/model sliders + per-weapon auto aim presets.

### Session 37 - 2026-08-02 - the letterbox was the window; the resolution picker ships LIVE

Branch `s37-b2r-res-picker` off `bioshock-2` (d875ee6), merged back to `bioshock-2`. The brief
was a BS1-parity picker + automatic FOV behind three blocking unknowns (aspect bisection, vrres
end-to-end, swapchain sizing); all three closed in one UNATTENDED screening pass, because BS2's
menu background turns out to classify as a strict-gameplay ShockPlayer view rendering the full
scene pipeline - the campaign ran there under the first-ever xrsim attach to BS2 (which worked
first try), spending zero user boots.

The reshaping find: the engine sizes its scene viewport to the window CLIENT while the backbuffer
holds the ini size, and the game's chromed window clamps at client height 1421 on this 1440p
desktop - sessions 32-33's "mystery ratio" 1.4413 = 2048/1421 is window arithmetic, and even
2560x1440 letterboxes (lb=1.0141). One GetClientRect replaced the planned 4-5 boot bisection: a
borderless client sized to the render (beyond the desktop where needed) renders full-height
square pixels at every aspect tried, and the engine follows a LIVE client resize with its own
ResizeBuffers - backbuffer, scene viewport, XR swapchain, auto FOV and claim all tracked
correctly through 16:9 -> 1.6 -> 0.9348 -> 0.9321 transitions with stereo-on-1t armed
(wait2/s=0 throughout, zero faults). So the picker ships BETTER than BS1 parity: `vrres
native|perf|sharp|max|flat / list / restore / WxH` and the F10 "RENDER RESOLUTION (applies
live)" section apply instantly via `apply_resolution()` (borderless resize -> ini persist ->
deferred re-verify - the engine persists its own live size into Shared.ini ON RESIZE one step
behind, which is the mechanism behind every historic "my resolution write did not stick" report;
it does not rewrite at exit). A stereo-gated self-heal fixes the letterbox every chromed boot
starts with (verified: 2064x2208 boot healed ~1 s after `vrstereo on`). The automatic FOV needed
no new mechanism - `vrfov` already runs the inverse law per CalcView; at native it writes option
138 and renders 107.7x111.4 deg against the 108x110 eye, ClaimRatioH 0.99521 (sim eye pinned to
the measured VDXR 54/55 this session, closing the s34 open item; claimRatioH is claim/EYE and
the honest 16:9-fill reads ~1.8 BY DESIGN - the guard is the law-derived expected value, matched
to four decimals).

Bugs found by the session's own instruments: `vrres list` fell to the status line (fgets newline
vs strcmp - token-match now), game-batch writes are LOST during scene transitions (the menu-scene
load stalls polls ~9 s; verify per-command echoes), and `vrres restore` dragged the engine to the
stale saved rect (restore now sizes the client for the current backbuffer).

Same-evening addendum: the user's first native run accepted the SHARPNESS and confirmed the
auto-FOV A/B, and hit the flagged fg defect - the viewmodel moved against the head. An in-save
three-probe sweep (drill drawn, fgfov 60/100/138) measured the fg lens law: `tanV = tan(d/2) *
G(aspect)`, G(0.9348) = 0.99488 constant to five digits, G(16:9) = 9/16 exactly (s33's identity)
- no natural closed form through both, so the shipped fix SELF-IDENTIFIES G live (fov-watch fg
sample paired with the value the match itself last wrote; converges in one sample; freezes when
the lenses merge; identity at 16:9).

CLOSED next day (2026-08-03): the fix verified in-sim first (G identified in one sample, wrote
111.7 at option 138, both dumps ONE law-exact cluster), then ACCEPTED in-headset - the user
tried multiple resolutions: "everything is perfect - the FOV is filling the screen and the
weapons/hand models are stable and glued". Their session log shows zero watchdogs/guardskips.
Remaining open item handed to session 38: the teardown crash on stereo-armed close - four dumps
banked, reproduced unattended under the sim (`+0x4FF0FE`, Draw/flush neighborhood), repro
recipe + candidate fix shape in Current state.

### Session 36 - 2026-08-02 - the BS2 stereo freeze is dead; full-rate stereo ships on 1t

Branch `s35-b2r-reentry-freeze` (worked via `claude/bioshock2-vrstereo-freeze-64334d`, same tip),
merged to `bioshock-2`. Nine commits.

Executed session 35's plan end to end, with three findings of its own. (1) The harness's first real
run caught its own bug - the gameplay regex was anchored ahead of the log's timestamps - and then
proved the whole lane: map boot to gameplay unattended, vanilla soak green, and the freeze
reproduced as exit 3 in 25 s with the wedged stack reading `B8108F BB1963 69FD33 4EF4A6` - the
session-35 chain confirmed live before a single fix landed. (2) The `wait2/s` counter refuted the
resolution/FOV trigger hypothesis outright: the second flush entered the INFINITE wait on EVERY
doubled frame at both 1920x1080 and 1280x720 - the race was never rare, only the lost wakeup was.
(3) The user's headset time found two real VDXR integration bugs the simulator structurally cannot
see (it force-grants focus): a never-focused session needs its frame loop to REACH focused
(bring-up exception added, lever-gated), and a detached session is stranded forever because VDXR
will not re-promote empty keepalives (BS2's detach default flipped OFF; the session self-heals on
refocus now that the wait is off-thread - lastEnd ~1 ms at VISIBLE).

The fix itself is BS1's session-8 cure duplicated with fresh constants - drain guard first,
flush-point force second, quotient untouched - and it was accepted the same day: 10-min decider
soak in the user's save, user-driven load-crossing matrix (save load, quit-to-menu x2, new game,
respawn; stereo armed and sticky, guardskips 0), post-flip smoke, and an immersive in-headset run
of full-rate stereo on 1t that the user called stable. `vrstereo on` now arms BS1's ladder by
default; `srdev` degraded to the repro escape. A watchdog lesson closed the loop: an episode the
game recovers from is a load, not the freeze - the soak now judges permanence, after `-KillOnFail`
killed a healthy game at the exact moment the user's save finished loading and read as a crash.

Deferred by the user's call, commands banked in Current state: the four-mode 10-minute matrix, the
BS1 regression soak, and the sim-vs-BS2 per-eye captures (selftest passed).

### Session 35 - 2026-08-01 - the BS2 freeze has a cause: Draw's tail calls a render flush point

Branch `s35-b2r-reentry-freeze` off `bioshock-2`. **Paused mid-session at the user's request; two
commits pushed, build clean, nothing half-applied.**

Session 34 left the root cause open and named "render single-threaded while stereo is armed, the way
BS1 does" as candidate #1, noting that BS2's equivalent had never been derived. It has now been
derived and verified against the shipped exe: `UGameEngine::Draw`'s tail makes exactly one call to a
render flush point at `0x69FC30`, whose threaded branch is the `Wait(INFINITE)` the watchdog caught,
and whose inline branch has **nothing after the drain** - the property that makes BS1's cure lossless.
Full table and RVAs in "Current state" above. Session 26's premise ("the Draw path has no submit
handshake"), the sole reason 1t was never ported to BS2, is refuted structurally rather than
empirically.

The user's recollection that the freeze began with the resolution/FOV work turns out to be
**compatible** with this rather than an alternative to it: `0xBB1950` skips the wait entirely when the
worker has already finished, so reachability is a pure timing question and a bigger render target is
exactly the kind of thing that makes the wait start being taken. Mechanism and trigger are different
questions and both have answers.

Two things landed, both prerequisites for measuring anything:

- `tools/soak.ps1` and `tools/game-key.ps1` - the project had no soak harness at all and no keyboard
  lane, so every soak in its history was manual and unattendable.
- The stall watchdog **could not fail an acceptance run**. Its trigger required an open draw stage,
  which only the doubled draw opens, so "zero WATCHDOG lines" was a guaranteed pass for every other
  mode; and `watchdog_all_threads()` had five silent exits and printed nothing every time. Had this
  been noticed later, the session could have declared a fix accepted on evidence that could not have
  contradicted it.

### Session 34 - 2026-08-01 - a simulated Quest 3, so agents can test their own VR work

Branch `s34-xrsim-simulated-headset`. The user's ask: stop being the manual tester for every VR
question across three parallel mods.

**Built `bvr_xrsim32.dll`**, a purpose-built 32-bit OpenXR runtime that presents as a Quest 3.
Nothing off the shelf could do this: Meta XR Simulator and OpenXR-Simulator are x64 (the game is
32-bit), `ox` has no D3D11, and an API layer cannot substitute because a layer needs a runtime
underneath and with no headset there is no session to intercept. Tractable because the mod's whole
OpenXR surface is 39 entry points, one extension and one interaction profile.

**Zero lines of the mod changed.** Selection is per-process via `XR_RUNTIME_JSON`, which the loader
checks before the registry. That was verified empirically on day one before any runtime code was
written: the game honours the env var on a direct launch and does NOT silently fall back to VDXR.
M9 confirms the endpoint - `bioshockvr.dll` and `xinput1_3.dll` hash identically with the sim target
off and on at one commit, and MSBuild does not even relink the mod.

**The compositor is the payload.** Composing each eye means reprojecting the projection layer
through the difference between the layer's tagged pose/fov and the eye's, which turns a claimed-fov
mismatch into visible magnification. It also retires a limitation TESTING.md has carried since M8:
XR quad layers never appear in a window screenshot, so the aim laser and HUD panel were unverifiable
outside a headset. `laser.xrs` now reads 7 quad layers on, 1 off.

**Six bugs found in the sim during bring-up**, each fixed and each worth remembering because they
are all the same shape - a thing that worked on the one path it was written for:
- unescaped Windows paths made `state.json` invalid JSON;
- the layer census only updated on capture frames;
- the ack channel deadlocked in step mode (`cmdSeq` published only from a blocked `xrWaitFrame`);
- `pacing_wake()` conflated a config change with an abort, putting the mod in a permanent teardown
  loop;
- `rig_defaults()` never set the FOV, so `reset` zeroed the optics and every capture went black;
- the launcher waited on a session the revert-Options dialog was blocking.

**Acceptance in gameplay**: stereo left-vs-right 3.16 against a 0.4 noise floor, head look monotonic
2.24/2.79/3.37, `claimRatioH` 0.98, 90/91.7/89.7 frames/s across a focus loss, exact frame stepping,
and a walked-away agent leaving the game alive for 40 s with the starvation grant firing on cue.

**Regression**: a normal launch still lands on VirtualDesktopXR, the ActiveRuntime registry values
are byte-identical on both views, and `XR_RUNTIME_JSON` is empty at every scope afterwards.

The markdown cleanup the user originally asked for was explicitly deferred mid-session; the sizing
findings are recorded in "Next steps" for whoever picks it up.

### Session 33 - 2026-07-31 - BS2 viewmodel lens DONE and accepted; the VR pacing bug found

Branch `s33-b2r-viewmodel-lens-match`, merged to main on the user's go-ahead after their
in-headset test.

**The result:** BS2's foreground lens is `PlayerController + 0x694` (float, degrees). Writing the
live world FOV into it every CalcView collapses the two lenses. User: *"I tested the match
viewmodel lens to the world and it worked, the weapon was not moving anymore."* Default ON.

**The method worth keeping:** poke each candidate field to a DIFFERENT distinctive FOV and take ONE
dump - the foreground cluster lands on the value only one candidate could have produced. That
replaced a one-at-a-time sweep which had reported six false positives in a row, because a lens
COUNT cannot distinguish "the cluster merged" from "the sampler missed it" while a lens VALUE can.

**Answered for free, from dumps already on disk:** the world FOV law is 16:9-referenced
(`tanV = tan(opt/2)*9/16`, aspect-invariant) - the OPPOSITE of BS1's - verified at two aspects x
two options with no relaunch spent. Session 32's "the projection degenerates off 16:9" is
retracted: the ray block's vertical slope carries a letterbox factor, and reading the pair as an
equality rejected every letterboxed block. So is "`0xAECACF` separates the two lenses".

**The new blocker:** the game is paced by an OpenXR session that is not FOCUSED, at the runtime's
not-visible cadence (~10 Hz). Not a hang - `lastWait 0 ms, timeouts 0`. Alt-tab reproduces it.
`vrcam off` could not undo it because only `on` called `set_enabled` - fixed. The rest is next
session's priority 1.

**Three of my own hypotheses died, all by measurement:** the fov watch (bisected out with a new
`vrhud fovwatch off`), alt-tab-as-root-cause (the user pushed back and was right; it is a
SYMPTOM of the pacing bug), and two attempts to make the live lens counter reliable - a head-slot
reservation (the fg pass moves) and a rotating stride phase (20x the frame time). Both dead ends
are written into the sampling site so they are not rediscovered.

**Tooling:** `disasm-rva.py`, `game-batch.ps1`, `launch-game.ps1`, `vrpreset` on BS2, `vrpace` /
`vrmirror` / `vrhud` dispatched for the first time, and an **F10 overlay section for the lens
test** - because typing a command in a headset means alt-tabbing, and alt-tab is the pacing bug.

### Session 36 - 2026-07-31 - I2 part 1: DR-I1 CLOSED offline; DR-I2 and DR-I3 built and waiting on the headset

Branch `si36-inf-derisk`. **BioShock 2 owned the machine for the entire session**, so nothing was
run live. That turned out to matter less than expected: DR-I1 needed no game at all, and the DR-I3
work that did need one was reshaped by evidence that made a live run premature anyway.

**DR-I1 is a PASS.** `UObject::ProcessEvent` `0xCFE70` (vtable slot `+0x7C`, thiscall, 3 stack args,
`ret 0xC`), `AActor::ProcessEvent` `0x19A150`, `UObject::FindFunctionChecked` `0xD1090`,
`UObject::FindFunction` at slot `+0x54`. Route: the UTF-16 `Failed to find function` literal gives
an address inside FindFunctionChecked; its 426 callers are the generated event stubs; the call that
follows each one is ProcessEvent, and 407 of 420 agree on slot `+0x7C`; reading that slot from 1582
candidate vtables gives 1038 votes for `0xCFE70` and 175 for `0x19A150`.

**Two methods paid for here that generalise.** First, the set of every `E8` call target in `.text`
(38,638 of them) *is* the list of function entry points, so "the function containing X" is a binary
search - no prologue heuristic, which matters because `find_function_start`'s backward walk returns
the **previous** function on this exe rather than failing. Second, and it cost a wrong answer before
it was caught: MSVC hoists the vtable pointer, so a UE3 event stub is `mov edi,[esi]` ... `call
FindFunctionChecked` ... `mov edx,[edi+0x7C]` / `call edx`. Searching for `call [reg+disp]` finds
**0 of 426**. The first histogram happily reported a nonsense negative disp8; disassembling an
actual caller is what exposed it.

**The census is now a committed tool** (`tools/pe-xref.ps1`) rather than a throwaway script, with
its predictions written down before the run. All of them held, and two unpredicted corroborations
came free: ProcessEvent's single `E8` caller is exactly the tail-call inside `AActor::ProcessEvent`,
and `GetPlayerViewPoint` has **0** absolute refs, so it is in no vtable - closing off a
"hook it through a vtable instead" alternative before anyone spent a session on it.

**A correction that matters for I2's test loadout:** `ConsoleCommand 0x136070`,
`AXPawn::SetWeapon 0x4F9ED0` and `AXWeapon::AddAmmo 0x5017D0` were recorded as implementation RVAs.
All three have 0 `E8` callers - they are exec thunks. Going by name through `ProcessEvent` is both
correct and cheaper, and needs no addresses beyond what DR-I1 just landed.

**DR-I2's hook is written but has never dispatched**, and the milestone box stays open because of
it. It is read-only by construction (out-params copied into `const` locals; no assignment through
them exists in the translation unit), and it refuses to install unless the live 12-byte prologue
matches *and* a `C2 08 00` is found in the body - an independent confirmation of the 2-stack-arg
count before the detour exists, because getting that wrong pops the RTC dialog that writes no dump.

**The command-pump handover became a lease.** Session 35 made it permanent, reasoning that resuming
on a stall would dispatch on the render thread during a load. The hazard is real but was stated too
broadly: what must not happen then is an *engine-touching* dispatch. As written, a camera hook that
went quiet left the mod with no command surface and no line saying so. Now the Present pump resumes
after 3 s in degraded mode, refusing only `mempoke*`/`pokeaddr*`/`memrestore`. It is also testable
on demand, which the original was not - `bsicam off`, wait, `vrcmd`.

**DR-I3 was reshaped by a banked negative nobody had noticed.** Core's live FOV watch *was* sampling
on Infinite (the session-35 dump shows 8 staging copies per frame from 3 distinct buffers) and
across **19,602 presents** it adopted nothing. That negative is real and bounded, and its three
holes are all in our own code: the `>= 320` byte tier gate - and Infinite's deferred lighting pass
uses the **160-byte** tier - plus the 1344-byte truncation and VS-only capture. Worse, a plain
`dumpframe full` would not have closed them: it gates on the buffer *object* changing, while
Infinite rewrites one object via `UpdateSubresource` 251 times a frame, so it both under-samples and
misattributes what it does capture. Spending the scarce live session on that instrument would have
produced a plausible wrong answer.

So the instrument was rebuilt first: `dumpframe cb` (mode 3) captures every `UpdateSubresource` into
a constant buffer at full size from the call parameter, and `-ScanMatrix` recovers `tanH = |c3|/|c0|`
from a 4x4 **with the object scale cancelling**, which is what makes it viable on a per-object
buffer. Every control passes, including two that are genuinely informative: `-ScanMatrix`
independently reproduces BS1's known lens by a different decode than the ray block, and it recovers
BS2's lens from the 2048x2048 dump where `-ScanLayout` finds nothing at all - the square-aspect case
that cost BS2 a session. Recorded honestly against it: on the BS1 control a degenerate 0.5/1.0 pair
scored 156 blocks and outvoted the true answer's 83, so plurality is not sufficient and the aspect
cross-check is load-bearing.

**The frame map itself is done offline** from the banked dump - the full deferred pass order, the
scene RTs, the shadow atlas, and the tonemap target. One trap recorded: **T9 is reused** as both the
G-buffer albedo and the tonemap output, so no descriptor-based rule can pick it and the Infinite
rule must be positional. A free DR-I7 half-answer fell out too: the Scaleform HUD is a contiguous
9-draw run on the backbuffer *after* the tonemap blit, which means T9 is HUD-free by construction
and the eye image needs no classifier at all.

**BS1 and BS2 verified unaffected rather than assumed so** (user directive): their sources are
byte-untouched, neither includes the core command header, both compute `full ? 2 : 1` when arming a
dump so mode 3 is unreachable, and the dump format's additivity was *tested* - the old decoder run
against a synthetic mode-3 dump returns a byte-identical answer.

**The machine freed up and the live battery ran in the same session.** All three de-risks pass.

The hook fires and the numbers are not BS2's: **9681 calls/s** peak against BS2's ~850, 4.07 M
lifetime calls, zero foreign-thread dispatches, and **separate game and render threads** (13120 vs
1992) - which is half of DR-I5 for free. The path census came back 100 % path 2, promoting
`[this+0x240]` from inferred to observed. The pump lease passed its positive control in both
directions on demand, and it earned its place: `bsicam status` **dispatched while degraded**, which
without the lease would have been impossible for the life of the process.

**The motion test is the cleanest single result of the session.** One slow 360-degree turn swept
yaw from -32392 to +32640 - **65032 of 65536 units, 99.2 % of a full 16-bit range** - which
falsifies the field ordering and the units-per-turn assumption in one motion rather than two
experiments. It also showed FRotator is **signed**, wrapping into `[-32768, +32767]`; a naive
unsigned read would have been wrong for half the circle, and that is exactly the class of bug the
read-only phase exists to catch.

**DR-I1's offline derivation survived contact with a live object.** Vtable slot `+0x7C` holds
`AActor::ProcessEvent`, not the `UObject` base - which is the prediction that only the
base/override split made available, and the one thing a vote count alone could never have produced.

**DR-I3's lens went from unknown to confirmed in one session.** `dumpframe cb` captured 1891
uploads including the 160-byte tier the old watch's gate had always excluded; sweeping every offset
gave 139 candidate 4x4s and the aspect filter left exactly one. Then the falsifiable prediction
closed it: the FOV slider min-to-max moved both axes by the same ratio to five significant figures
with the aspect preserved to 0.002 %. Worth stressing how close this came to going wrong - the top
four candidates by block count were all degenerate `tanH==tanV` pairs, and the best of them had
**more** support than the truth. Plurality would have picked the wrong one.

**A third instance of "a config value is a claim".** The ini predicted a 70-to-80.5 degree slider
and a 1.2094 tangent ratio. The frustum measures 75.01 to 82.50 and 1.1428. Nothing matched, and
had I5 started from the ini it would have modelled a lens the game does not render.

**Two things recorded against ourselves rather than glossed:** the heartbeat's
`returned-minus-cached` verdict compares path 1's source field while every sample took path 2, so
the transform question is still open and the line is misleading as written; and the selftest's
three "failures" were a wrong assertion of mine, not a broken instrument - the native registry is
per-class blocks separated by `{0,0}` sentinels, and the 46 it found is exactly
`APlayerController`'s census count.

### Session 35 - 2026-07-31 - I1 CLOSED: our code runs inside BioShock Infinite, and the command seam works with nothing hooked

Branch `bioshock-infinite`. The first session in which anything of ours executed inside
`BioShockInfinite.exe`. **Everything below was measured live**, not derived - which matters, because
every Infinite fact before today came from the disk image or from outside the process.

**What shipped.** `src/game/bioshockinf/` (adapter + patterns), `HostGame::Infinite`, data subdir
`bsi`, the build-fingerprint gate wired to the session-34 PE values, and a new core module
`core/framework/command` that owns the command-file poller and the shared vocabulary. The adapter
advertises **capabilities 0x0** and that is deliberate: a capability bit is earned by a hook observed
firing, never by an address being derived.

**The design change, and why it was worth doing first.** On BS1 and BS2 the `command.txt` poller
lives in the adapter and ticks off the camera hook, so a skeleton adapter has no way to talk to the
mod until the very thing being debugged works. Core now polls from the **Present** detour instead,
and Infinite was drivable from frame one with nothing hooked. The pump is **opt-in**
(`command::enable_present_pump()`), which is what let BS1 and BS2 stay **byte-untouched** - a
parallel BS2 session was live in `bioshock2r/camera.cpp` and neither shipped game could be
smoke-tested from this branch (user directive: consolidate later, in a healing pass). Handover to a
game-thread pump is **one-way**: engine-touching commands belong on the game thread, and a
"resume when the game thread goes quiet" rule would hand the render thread a dispatch during a load.

**A trap fixed, and the fix's own trap found by running it.** A pre-existing `command.txt` is now
skipped at startup rather than executed - the thing TESTING.md records as having bitten BS1 three
times, once producing a false result that was then chased as real. The first build primed on the
first *sighting* of the file, so on a game that starts with no command file (the documented hygiene)
the first real command was swallowed. Observed live, fixed, and both directions re-verified: a stale
file is skipped and named in the log; a fresh write dispatches.

**Live confirmations, all first-time:**

- **The `xinput1_3.dll` proxy works verbatim** and the Steam overlay does not interfere with
  injection - the proxy loads the mod from its own `DllMain`, long before anything calls
  `XInputGetState`. Whether the overlay eats the *input* thunk is still an I7 question.
- **The build fingerprint matches on all four fields**, and Infinite's exe carries a real PE
  checksum (`0x011590C3`) unlike BS1's. **ImageBase really is `0x00400000`.**
- **D3D11 from the inside**: backbuffer 2560x1440 `R8G8B8A8_UNORM` windowed, feature level 11_0,
  RTX 4060, frame inspector on 15/15 context slots, one real dump (482 events / 68 resources).
- **d3d11/dxgi being absent from the import table is a non-issue**, and structurally so:
  `bioshockvr.dll` links `d3d11` itself, so our own import table loads it before any of our code
  runs. Hooks at T+0.4 s, first Present at T+8.5 s, no retry path needed.
- **`CSERHelper.dll` displaces our unhandled-exception filter here too**, exactly as on the
  remasters - core's periodic re-arm is load-bearing on this game as well.
- **DR-I8 gets a free half-answer**: `XEngine.ini` says 2560x1440 and the backbuffer *is*
  2560x1440, so the config resolution is honoured by the renderer. That is more than BS2 could say -
  but it does not prove a *write* lands, which still needs a write, a relaunch, and the backbuffer
  as acceptance.
- The camera seam was **probed read-only and never hooked**: `0x1E10C0` holds an aligned-stack MSVC
  prologue (`push ebp; mov ebp,esp; and esp,-0x10; sub esp,0xA4`), consistent with the 4x4 SSE
  transform derived offline, and the exec thunk at `0x129280` is **frameless** - which independently
  explains why the `CC CC CC 55 8B EC` prologue heuristic could never find it.

**Acceptance was measured, never asserted.** F10 moved 5.7 % of channels against a 0.54 % ambient
floor (two shots with nothing between them); `vroverlay on|off` through the seam moved 4.6 %. The
screenshot is the acceptance, not the log echo. `dumpframe` wrote a real dump; an unknown command
logged one line; a menu quit logged `DLL_PROCESS_DETACH`.

**Recorded for later, not acted on:** core's letterbox detector (BS1-tuned) fires on Infinite's
cinematic bars and produces incoherent readings (`top 1440 px of 1440`); it needs re-deriving at
I10, and nothing consumes it yet.

### Session 34 part 2 - 2026-07-31 - I0 offline recon complete; the console is dead, the reflection lane is wide open

Branch `bioshock-infinite`. Still no adapter code. All findings are in
`docs/bioshockinfinite/ENGINE_NOTES.md` with per-row confidence; **nothing has been observed
executing** - every value is a structural fact from the disk image or, where marked, an inference
from shape.

**The one live test of the session, and it was a negative.** All six shipped debug binds are inert:
console on both `~` and `Tab`, `PageUp` ghost, `F1` wireframe, `F9` shot, `F7`/`F8` post-process,
`Delete` god. This **retracts part 1's optimistic reading**, which was drawn from config and
explicitly flagged unconfirmed. Corroborated rather than trusted: `F9`=`shot` produced no
screenshot anywhere, and `My Games\...\Binaries\Win32` exists and is empty - the instrument was
seen not to fire.

**But the diagnosis is narrower than "cheats are gone", and that decides the plan.** `SHOT`,
`SETRES` and `FULLSCREEN` are all still present as UTF-16 `Exec` literals, so the C++ handlers are
compiled in and F9 simply never reached one. `GOD`/`GHOST`/`WALK` being absent is *expected* - in
UE3 those are UnrealScript functions on `CheatManager`, so they live in the `.u` package. And
`UXCheatManager` is in the shipped build. The broken link is Infinite's custom `XCore.XPlayerInput`
binding parser, which does not forward console strings. **Recorded as a dead end so no future
session burns time on binds, launch flags or ini edits.**

The way in is reflection, and it is better than a console: `APlayerController::ConsoleCommand`
(native, impl RVA `0x136070`), or straight past it to `AXPawn::SetWeapon` (`0x4F9ED0`),
`AXWeapon::AddAmmo` (`0x5017D0`) and `AXPawn::AddInvulnerableFlag`. A test loadout therefore needs
no console at all - but it does need the adapter, so it moves to **I2**.

**The four offline questions, answered:**

1. **The native function table EXISTS** - the open risk, since UE3 favours indexed natives and
   BS1's fastest instrument looked lost. 2647 entries, 8-byte `{ const ANSICHAR* name; Native
   impl; }`, names `<Class>exec<Func>` in ASCII (BS1's were 12-byte and UTF-16). Same resolver
   recipe, zero hardcoded addresses.
2. **RTTI is present but useless** - all 270 type descriptors belong to third-party libraries
   (Wwise, Bullet, FaceFX, Beast, std); not one UE3 or XGame class. UE3 is built `/GR-`. The
   RTTI-walk lane both remaster adapters lean on is dead here, which is exactly why (1) matters.
3. **The camera seam is located**: `APlayerController::GetPlayerViewPoint`, **impl RVA
   `0x1E10C0`**, thiscall, 2 stack args, `ret 8` (so a probe hook takes 2 args - the RTC rule).
   Four internal paths converging on a 4x4 SSE transform, so the returned view is *transformed*,
   not a raw field read. `AActor::Location +0x44` / `Rotation +0x50` fall out of it.
   **Exec thunks are not the seam, reproduced offline**: every thunk checked has **0** `E8`
   callers, the implementation has 14. BS1 learned this by hooking four thunks and catching nothing
   across a live session of shooting; here it cost one scan.
4. **`GNames` at RVA `0xF9DFEC`** (Data/Num/Max), name hash at `0xF58BF8` (4096 buckets),
   `GNatives` at `0xF6DCB0`, `GMalloc` at `0xF71CC8`, `FFrame` `+0x14` Object / `+0x18` Code,
   `UObject::Class` `+0x20`, and a constant-time `IsA` via 16-bit interval fields at
   `UClass+0xC0/+0xC2`. **`FNameEntry` text is ASCII by default here, not UTF-16 as on BS1** - a
   `fname_text()` ported without reading the flag at `+0x8` bit 0 returns garbage.

**`GObjObjects` NOT found, and deprioritised on purpose.** `StaticFindObject` goes through the
object hash, not a linear walk. BS2's design takes live objects from hook parameters instead of
scanning, which is cheaper and avoids the class of stall and crash BS1's object scanner caused.
Three globals seen on that path are recorded as unidentified rather than guessed at.

Also identified: DLCA = Clash in the Clouds, DLCB = Burial at Sea Ep. 1, DLCC = Ep. 2. Saves live
in Steam cloud userdata (`userdata\<id>\8870\remote\SaveData\`), not under My Games, and Steam
Cloud will resurrect a deleted one.

### Session 34 - 2026-07-31 - BioShock Infinite project bootstrapped (planning only, no adapter code)

Branch `bioshock-infinite`, off `main`. A **planning session**: the deliverable is the project
scaffolding and the recon that de-risks it, not code. `src/` is untouched; the adapter skeleton is
milestone I1.

**Recon, all read-only, all verified against the shipped game files:**

- **32-bit x86**, fixed `ImageBase 0x00400000` with **ASLR OFF** (both remasters are rebased), LAA
  yes, `SizeOfImage 0x124F000`, TimeDateStamp 2022-05-11 18:29:09 UTC. Full fingerprint table in
  `docs/bioshockinfinite/ENGINE_NOTES.md`.
- **The injection vector is closed before a line of code**: the exe imports `XINPUT1_3.dll` by
  **ordinal 2 and 3**, identical to BS1 and BS2, so `src/proxy/` works verbatim. The game IAT slot
  for ordinal 2 is at RVA `0xCD4814`, so BS1's IAT-hijack lane (the Steam-overlay workaround)
  transfers directly too. `d3d11`/`dxgi` are NOT imported - they load dynamically, which
  `framework::init()` ordering must not assume away.
- **Full UE3 reflection is intact** (FName pool carries `PlayerController`, `GetPlayerViewPoint`,
  `UpdateRotation`, `PlayerCamera`, `CheatManager`, `MatineeCamera`, `Scaleform`, `GFxMovie`,
  `XHud`). So **BS2's ProcessEvent-by-name design is the seam to build**, not BS1's. No `CalcView`
  name exists - that is the Vengeance spelling.
- **A working cheat path exists, unlike either remaster.** The shipped `DefaultInput.ini` has a
  live `; --- Debug binds` block: `Delete`=`god`, `PageUp`=`ghost`, `End`=`preventdeath`,
  `PageDown`=`walk`, `F9`=`shot`, `F1/F2/F3`=`viewmode ...`, and `F7/F8`=
  `set D3DRenderDevice bUsePostProcessEffects False/True` - which is direct evidence that UE3's
  `set <class> <prop> <value>` works here. A second surface (base64 `DefaultDesignerControlPresets.ini`)
  lists God Mode, Ghost, GiveAmmo, Slomo, QuickSave/QuickLoad. **Public guides claim Infinite has
  no console; the game's own files disagree.** None of it confirmed live yet - that is I0, and the
  standing rule applies: verify by effect, never by return value.
- `OneFrameThreadLag=True` in `BaseEngine.ini` is a **config-level analogue of BS1's `reentry 1t`**,
  potentially buying single-threaded render without a flush-point hook. `bSmoothFrameRate=TRUE`
  must go for VR.
- Native FOV slider exists but caps at **+15%** (~70 to ~80.5 deg), so a lever is still needed -
  but the property chain is named, so `set`-by-name comes before any memory scan.
- `[Stereoscopic3D]` exists in the ini, but **no `bStereo`/`EyeSeparation`/`StereoDevice` names are
  in the exe**, which points at driver-side 3D Vision rather than an engine per-eye path. Planning
  for SequentialReentry; the native check is timeboxed to one slice in I2 so it cannot become a
  rabbit hole.
- UI is **Scaleform GFx**, not gameswf, so `core/gfx/hud_capture` is a worked example here, not a
  library. Cinematics split into two classes BS1 never had: Bink FMV (`binkw32.dll`, 100+ `.bik`)
  and engine-rendered Matinee.
- All three DLC installed (~25 GB). **In scope for tuning** by user directive: bring-up on the base
  campaign, but aim/scale/HUD/viewmodel calibration must hold in Burial at Sea 1 and 2 and Clash in
  the Clouds.

**Decisions taken with the user:** DLC in scope for tuning; milestones resequenced by engineering
dependency rather than by the stated priority order (the priorities became acceptance criteria);
the user does the first flat launch to create the UE3 user-config dir and a baseline; and the
BS2-conflict guard blocks only what actually contends for the headset.

**Shipped this session:** the `bioshock-infinite` branch; `docs/bioshockinfinite/{ROADMAP,
ENGINE_NOTES,TESTING}.md`; `tools/lib/assert-no-conflict.ps1`; `-Game bsi` wired through
`build`/`install`/`uninstall`/`tail-log`/`game-cmd`/`game-shot`/`game-click`; and root-doc updates
(CLAUDE.md, RESEARCH.md, ROADMAP.md).

**The BS2-conflict rule, now enforced.** BS2 development runs in parallel and only one game can own
the headset. `game-cmd`/`game-shot`/`game-click` with `-Game bsi` abort naming the offending
process and pid. `build`/`install`/`uninstall`/`package`/`tail-log` are deliberately NOT guarded -
they touch the disk, never the headset, and must keep working while BS2 runs. **Verified against a
real conflict**: BS2 was live (pid 24588) during this session, and the guard refused.

**The three things the BS1/BS2 history says to front-load, now written into the ladder:**

1. **The lens/projection-claim question is the highest-leverage thing in the project** - it caused
   BS1's M3 swim, blocked M4, produced the cinematic fisheye and the release-blocking warp, and is
   still BS2's open blocker at session 33. I5 builds a stride-sampled, majority-voted,
   structurally-validated **multi-lens** decoder before anything is tuned, and derives the law from
   two aspects because the conventions coincide exactly at 16:9.
2. **The flat harness is what made the remasters tractable**, and its two highest-payoff tools
   (`simhead`, `vrrec`) arrived at sessions 12 and 20 - far too late. Both are scheduled into I4.
3. **The policy gate** - check native, test whether the defect exists, only then port the cure -
   paid out three times on BS2 and is the preamble of the new ENGINE_NOTES.


### Session 32 - 2026-07-31 - BS2 resolution lane + the lens verdict; three BS1 assumptions died

Branch `s32-b2r-resolution-and-lens`. All BS2. Steps 0-2 of the brief completed; steps 3-4 not
reached and re-scoped in "Next steps 0".

**What shipped:** the frozen-pitch fix (`publish_pitch_error`), a `vrinput` dispatch that BS2 never
had, `vrres` + a BS2-specific `game_ini` module, BS2's cb0 ray-block offset as a per-game constant,
a widened + parameterised core fov watch, and five new instruments in `decode-framedump.ps1`.

**The three dead assumptions, all caught by acceptance tests rather than by reading code:**

1. **The ini file.** BS1's lane writes `[WinDrv.WindowsClient]` viewport keys. BS2 has them and
   ignores them; `Shared.ini [SharedOptions] ViewportX/Y` governs. Caught only because the
   acceptance criterion was the backbuffer at first Present - the write itself had logged
   `verified`. **A verified write is not an honoured one**, now a standing rule in the decision log
   alongside the `-> HANDLED` lesson.
2. **The square-backbuffer policy.** BS1's biggest banked conclusion does not transfer: BS2 renders
   2048x2048 as a letterboxed 2048x1421 with a black band and a degenerate projection (horizontal
   collapses 100 -> 67.7 deg, the ray block's vertical encodings disagree). 16:9 is the only aspect
   proven to render correctly. This inverted the session's own plan mid-flight.
3. **The cb0 layout.** BS2's ray block is at float 16, not 12 - and core was only COPYING 80 bytes,
   so no offset could have rescued it. Now a per-game constant with a self-correcting, logging
   hunt as backstop.

**The lens verdict (the thing that unblocks the viewmodel work):** BS2 has TWO lenses and, unlike
BS1's aspect-gated split, they differ AT 16:9 - a world lens tracking the FOV option and a second
fixed at tan(30) = 60 deg that ignores it, separated by callstack. That is a 2.06x angular-gain
error at option 100 and 3.99x at 130, present at the aspect the user actually tested, which matches
their "wrong depth / moves with the head" report where BS1's mechanism could not. **Not yet proven
to BE the viewmodel** - the holster test is queued and must be the identification, not draw counts.

**Method note worth carrying:** the first `-ScanLayout` run came back empty and looked like "BS2's
layout is a different shape". It was not - it was the square-aspect degeneracy breaking the check.
`-Diff` (two dumps at different FOV options, assuming nothing about layout) is what cracked it, and
re-scanning at 16:9 then found the offset cleanly. **Derive layouts at an aspect the game renders
correctly.**

**Regression gate:** BS1 `WORLD tanH=1.191754 tanV=1.191754` at 2048x2048, bit-identical to the
banked session-28 value, hunt never fired.

**Incident:** one black-screen hang after a force-kill mid-gameplay plus a resolution change; a
clean restart fixed it, and the same DLL had rendered fine minutes earlier, so it was not a mod
regression. Also re-confirmed the stale-`command.txt` trap - an old `vrres 2048x2048` re-applied at
boot.

**Still owed:** the pitch servo's sign (needs the headset; flat `pitchErr` is 0 by construction
with `drive=0`).

### Session 31 - 2026-07-31 - swing the wrench to swing the wrench, ACCEPTED IN-HEADSET

**Verdict: "I tested it and it's perfect."** Shipped default ON with a **3.6 m/s** fire threshold -
the user's own number after the live run, replacing the 2.2 m/s guess that shipped to it. Nothing
else needed tuning: delay stayed 0, which is the rising-edge bet paying off (the game's own wind-up
animation lands the hit where the arm is already going). Both values are the code defaults AND
written into the user's vrpreset.ini.

### Session 31 (build phase) - 2026-07-31 - the feature, and what flat could and could not prove

Branch `s31-b1r-swing-to-attack` off `main`. The user asked for their own play-test to become a
feature: trigger the melee hit from the physical swing. It was already ROADMAP line 539.

**Shipped:** `core/input/swing.{h,cpp}` - a speed threshold on the right hand, gated on the wrench
being the equipped holdable, composing a 120 ms RT pulse. Rising-edge fire, hysteresis re-arm,
cooldown, head-relative velocity, weapon-wheel suppression. `vrinput swing
on|off|status|threshold|rearm|cooldown|pulse|delay|rel|log|sim`, seven keys in vrpreset.ini, a
checkbox and three sliders in the overlay. Supporting changes: `aim::weapon_key_is()` (reuses the
per-weapon profile key as the identity source rather than resolving the holdable twice),
`bvr::vr::peek_head_pose()` (the same read as `get_head_pose` without the pose-tag audit stamp - a
second reader would have made that instrument lie), one line in `input_sync`, one gate publish in
CalcView beside `publish_vr_gameplay`.

**Three decisions worth keeping** (all in ARCHITECTURE's decision log): the detector lives where it
can be TESTED rather than where its data is born; it reads poses through the funnel so replays
drive it; and the gate is an identity test (the class name is `Wrench`) not a plausibility test,
because RT with a gun in hand is a shot.

**Flat run found two defects in its own feature** - a per-sample BLOCKED log (106 lines for one
swing) and a cooldown that could not be isolated until `sim` grew a repetition count - and both are
fixed and re-verified. It also measured two engine facts now in ENGINE_NOTES: a 120 ms synthetic RT
pulse fires the weapon (the "first pull only switches hands" caveat is about which hand is raised),
and the pulse reaches the engine's own fire path 8-11 ms later.

**Not verified, and it is the part that decides the feature:** no real swing has been measured, so
every threshold is a guess. In-headset checklist in TESTING.md; `swing status`'s peak-speed readout
is the first thing to collect.

**Harness note:** the stale-`command.txt`-at-boot trap bit again and produced a false negative -
`vrpreset save` left as the last command re-ran at the menu on the next launch and overwrote the
tuned ini with defaults. Clear `command.txt` before CLOSING the game, not only before launching it.

### Session 30 part 2 - 2026-07-30 - the wrench is FIXED, and the user's own observations found it

Part 1 closed the aim-seam lane by measurement and left three candidates. The user then came back
with four observations from playing the build, and three of them were better than anything on my
list.

**"The health bar has no colour."** That one landed on a fact I had measured the same session and
misread. `effectsInFrame` advances by exactly 2 per interval, every interval, with nothing on
screen - which I wrote up as "the fill is always drawn and usually transparent". It is two bars.
The health and EVE bar colour fills are textureless 5-vertex gameswf quads, which is the identical
fingerprint to the "full-screen effect fill" session 29 identified, so session 29 had been sending
the bar fills into the eye image while their frames stayed on the panel. One F10 toggle each way
confirmed it. The counter had been saying so since the change shipped.

**"It's either the size of the HUD or the size of the old resolution."** This is the one that
reframed the whole effects item. Those draws are authored in gameswf STAGE space, so routing one
in-frame cannot make it cover the eye - it makes it stage-sized inside it. Session 29's fix could
never have worked, and the open item is a GEOMETRY problem, not a routing one. Default is back to
the panel.

**"Is the water effect different from the alcohol effect?"** Yes, and asking was right: the
alcohol blur is a textured engine post effect and the water tint is a textureless gameswf fill,
different branches entirely. It also caught a real gap in my own work - I had rewritten the
post-FX rule that night and validated everything except the one draw it exists to protect, because
nobody was drunk while I measured. A/B'd in-headset: unaffected.

**"I'm almost sure it's the lock-on."** Wrong, and their own test killed it. Rather than trusting
the reasoning either way, we set the radius ABSURDLY high instead of to zero - 5000 felt identical
to 0, so that write never reaches the live object. Which also means `-> HANDLED` in the log proves
only that `Exec` recognised the command: `console_exec`'s output-device stub suppresses the error
a failed `set` would print, so a wrong class or property logs exactly like a success.

**The actual cause.** The camera heartbeat had the engine's own view pitch pinned at 49350 units -
-88.9 degrees, straight down - unmoving for fifty seconds while yaw ran freely. Two things met.
Pitch kill zeroes the composed right-stick Y so the stick cannot fight the HMD, but zeroing an
INPUT does not set a value: the engine's pitch can then never change again. And the camera write
is asymmetric - yaw is written RELATIVE (the engine's own plus a head residual, so it stays real)
while pitch is written ABSOLUTE from the head, discarding the engine's value unread. Nothing
corrects it, nothing notices, and the rendered view is the head's either way.

The user confirmed it visually before I had finished arguing it: `vrhands off` returns the
viewmodel to engine placement, and "this revealed that the hands were pointing downwards ... I saw
the hits hitting the floor". The lucky kills had been catching a leg on the way down.

Every part of the original report falls out: walls connect because you approach them level, fights
miss because the frozen value was steeply down, the opening rocks other players reported miss with
no combat at all because rocks are on the floor, and guns were always fine because we substitute
the whole fire ray at a seam melee lacks.

**The fix is a servo, not a write.** The game layer publishes head-pitch-minus-engine-pitch once
per CalcView, before the overwrite, and the bridge feeds a proportional stick value instead of the
hard zero. The game steers its own pitch through its own input path, so no engine memory is
written at all - none of the session-29 world-change hazards apply - it inherits the game's own
clamps, it is invisible because the rendered pitch is the head's regardless, and a stale publisher
fails open to ry = 0. Measured after: the engine pitch moved from -88.9 to -6.6 degrees. Verdict:
"it's working and I was able to hit him consistently."

Residual, stated because it is not zero: it stalls at err=4.3 deg because near convergence the
proportional stick falls under the game's own deadzone. Inside melee tolerance, not perfect.

**Method note.** Everything decisive this session came from a lever A/B that needed no rebuild,
and the two theories that died were killed by tests designed to kill them - the absurd radius
rather than a plausible-sounding argument, and a positive control on the stranded counter rather
than trusting a zero. Two of the three theories that died were mine and one was the user's; the
tests did not care which.

### Session 30 - 2026-07-30 - the wrench hypothesis dies to one read-only command, and a shipped HUD regression surfaces

Three release-blocking items. The wrench came first because it is game-breaking, and the brief
was explicit: disprove or confirm the leading hypothesis by measurement before any code.

**The wrench.** The hypothesis was well argued - the wrench is an `AWeapon`, our aim substitution
rewrites `GetPerfectFireStart` for `AWeapon` too, melee is a short trace, so an origin moved to
the controller starts it past the target. The designed fix was a melee carve-out behind two
levers. Before writing it, three documents in the tree were compared and they disagreed:
`aim.cpp:462` says the wrench's melee lands on the ABILITY seam, `ENGINE_NOTES:616` says no aim
seam fires for melee at all, and STATUS assumed the weapon seam. Three claims, so the first
measurement was simply "which seam does a swing reach".

`vraim probe on` + `vraim off` installs both hooks with `ray_for()` refusing, so the run cannot
change what it measures. The user drove a live session in the headset with the wrench equipped
(confirmed twice by `[aim] weapon profile 'Wrench' applied`). Result: the weapon-seam counter
never moved - it sat at 4 all session, all four from a Shotgun test fifteen minutes earlier - and
all six ability-seam calls carried `cls='ElectricBoltThreeAbility'`. **Melee reaches neither
seam.** The session-10 note was right and had simply never been re-tested. The carve-out, the
`vraim seam weapon off` A/B, and the `vraim origin off` A/B would all have done exactly nothing.
That is the whole value of the session's first hour.

Two things fell out for free. Every substituted plasmid cast read `hand=L src=fallback lt=0
rt=0`: the anim notify fires after the trigger is released, so the object-learning map never
learns and always takes the seam default. Harmless today because Left is correct for plasmids -
but it means anything else arriving on that seam would be aimed with the left trims, yaw +37 deg.
And the watch line put a number on the substitution for the first time: we move the fire origin
**40-47 cm**, every call. Invisible at rifle range, decisive at contact range, which is precisely
why the wrench theory was so plausible.

**Effects.** Session 29 routed the effect fill in-frame and the user reported "better, but still
not the whole view". The offline dumps suggested a second mechanism: `PassThrough` is the absence
of a routing instruction, and because the redirect binds our RTV through the ORIGINAL SetRT,
`on_setrt` never sees it and the classifier can believe a draw is in-frame while the device has
the capture RT bound. Shipped that as per-reason pass/STRANDED counters plus a one-shot
`OMGetRenderTargets` check - the instrument that could refute the diagnosis, in the same build.

It refuted it. Effect fills read `effect=127010/0`, never stranded. The device check confirmed
the flag is faithful (`our capture RT=92DB3E64 - CORRECT`), and a positive control proved the
counter can fire (restoring the old post-FX rule produced 36140 stranded passes). So routing is
excluded and the remaining explanation is the fill's extent or the projection claim.

The same run found something that was actually shipping. At the user's 2048x2048 square render
the post-FX rule (`srv0 dims == target dims`) is degenerate: the backbuffer IS 2048x2048, so the
game's own UI atlases match it. `postFxRejected=1604161` against `postFx=2` genuine - roughly 30
gameswf HUD draws per interval taking the in-frame exit, and under the old rule 43% of them were
stranded onto the panel while 57% reached the eye image. HUD elements routed by draw order. Fixed
structurally on bind flags rather than size, which is stronger at every resolution. Also bounded
the effect test by vertex count - it had been the residual "textureless and not 29", which was
sending the census's 1493-vertex vector shape into the eye image.

**What broke.** Probing `InitiateDamage` with `vraim scanimpl 226050 1` popped `Run-Time Check
Failure #0 - ESP was not properly saved`: the arg count must equal `ret imm / 4`, and both
`InitiateDamage` implementations are `ret 8`, so the right answer is 2. Disassembled all four
fire-flow implementations with capstone afterwards and tabulated them so the next attempt is a
one-liner. Two secondary lessons: RTC writes no crash dump (a Debug compiler check, not an SEH
fault, so it bypasses the crash handler entirely - the same shape as session 29's `write_n`
guard, where the safety net only made the failure quieter), and force-killing the game while that
modal dialog is up left the display mode unrestored.

**Not done:** the effect coverage screenshots (the `img-diff.ps1 -Grid/-Bands` extension is built
and self-tested against synthetic images, but the measurement was never taken), and the hands
regression checklist. `vrbones status` now prints the drive residue on demand, which is what that
checklist needs.

**Method notes worth carrying.** Measuring first killed a fix that was ready to build. The
instrument that can refute the diagnosis has now earned its keep twice - session 29 on the sticky
bone state, session 30 on effect routing. A negative result from an instrument that has never
been seen to fire is worth nothing, so the positive control (`vrcine postfx size`) matters as
much as the counter did. And a discriminator built from a numeric coincidence between two
quantities dies silently the day they coincide for another reason - which is the whole post-FX
story.

### Session 28 - 2026-07-30 - the yaw warp: TWO lenses, and the instrument was reading the wrong one

Priority was "pinpoint before fixing", with three formula rewrites reverted last session off a
bad instrument as the reason. So: no projection math was touched until the cause was measured.

Started by re-reading the session-27 log rather than the code, and got four eliminations from it
directly - no `[hud] letterbox` line anywhere (so the head drive was never suspended), no
`xr: cinematic quad ON` (so `cinematic_active()` never latched and the per-eye offset was never
suppressed), `g_lbUnsqueeze` default off (the capture is a plain full-rect copy), and the XR
swapchain equal to the backbuffer (so no image/fov aspect mismatch). That left the claim and the
pose, and the two things the session-27 write-up said were eliminated.

**Roll first, because it was the one BioVRDev fixed and we had not.** No headset needed:
`SubmitDetour` already logs the loc/rot the engine's own render submit receives, behind
`reentry dump`. `simhead 0 0 40` -> `rot=(0,28303,7281)=(0.0,155.5,40.0)deg` and the screenshot
shows the world rolled 40 deg. Roll survives; BS1R does not need the render-thread re-write. The
same dump also proved both SR passes submit a bit-identical rotation with a 6.36 UU lateral
offset (== ipd 63.4 at worldScale 100) that gains a 4.1 UU `dz` under roll - the per-eye rig is
correct, which retires the session-27 "next and most specific" suspect too.

**Then the decisive one, via the trustworthy path instead of the suspect one.**
`dumpframe full 2` + `tools/decode-framedump.ps1` - the offline decoder, which has always applied
the structural zero-slot validation the live watch does not - found **TWO tangent clusters in one
frame** at 2750x2850: `1.1918/1.2351` on 154 draws and `0.6468/0.6704` on 24 draws, 20 of them on
the 576-byte foreground tier. A `vrfgfov off` A/B identified them outright: only the second
cluster moved, landing on `0.4330127`, the native fg vertical already hardcoded in `patterns.h`.
Repeating at option 130 gave `2.1445/2.2225` for the world - `tan(65)` exactly. So the world lens
is horizontal-anchored (the ORIGINAL assumption, and BioVRDev's), the foreground lens is
vertical-anchored, they coincide only at 16:9, and the session-27 "law B" was the fg lens
confirmed three times.

**The bug then falls out.** The watch sampled the FIRST decodable draw; the fg draws are the first
draws of the pass on a tier that clears its size gate, carrying the same block at the same floats,
and the two cross-checks the decode ran are intra-axis so they carry no lens information. Off
16:9 that makes `fov_mismatch()` latch ON in normal gameplay, which routes the claim through the
`fovMm && stereoCine` branch and tags the projection layer with the viewmodel frustum - a 1.842x
under-claim at 2750x2850. Every constraint in the report follows: slider-independent (`k` has no
option term), clean at 16:9 (the lenses coincide), yaw-dominant, and BioVRDev not warping at the
same resolution because their un-substituted option-derived claim is simply correct. The
session-27 `src=live` reading was a true label around the wrong lens.

Also recorded, because it cost a session: **the pose audit cannot detect what it was built to
detect.** It diffs `projViews[0].pose` against `g_consumedHeadQuat`, and both come from the same
`xrLocateViews` generation, so `delta 0.00 deg` was guaranteed by construction.

**Shipped:** the watch now stride-samples up to 8 cb0 heads per interval (a first-8 sample gave
the viewmodel 5/8 votes - measured, and the reason the first cut of the fix still failed),
clusters them, and publishes the majority as the world lens with the runner-up as the fg lens,
behind the offline decoder's structural checks plus majority and coverage guards that refuse a
marginal round. The age gate moved into `fov_watch()` and every line now says FRESH or
STALE-DO-NOT-CONCLUDE in words. `fovaudit` reports both lenses, the vote split, the stride and
the live aspect on one line, plus a `laws` line. Flat gate at 2048x2048:
`WORLD tanH=1.191754 (6/8 votes) | FG 0.670361 | lenses=2 mismatch=0`, both 15 ms FRESH,
`ambiguous rounds 1` (the coverage guard catching the menu->gameplay transient), and no
`rendered-fov mismatch ON` line anywhere - where session 27 had one within seconds.

**Alt-tab (OPEN BUG 1) turned out to be an ordering bug, not the pace guard.**
`if (g_srPairOpen) return;` sat above `pump_events()`, so a pair-hold blindfolded the event pump
entirely: `g_state` frozen, nothing below able to re-arm, and the `!g_enabled` teardown the only
escape - which is precisely why the VR toggle was the only recovery. Pump moved above the hold,
hold aged at 500 ms with a force-abort through the existing leaked-frame close, leaked frames
closed before the pace guard can return, `g_unfocusedSinceMs` cleared on STOPPING and
`g_paceSkips` made per-episode (those two gates had silenced the only two lines that would have
explained anything), plus a 5 s `SUBMISSION IDLE (reason=...)` heartbeat naming the firing guard.

---

**IN-HEADSET ROUND 1: the warp was GONE, and two things came back with it.** The user had to turn
"Game FOV Write" off, alt-tab was still stuck, and - the important one - **the hand/gun model
started moving with the headset**, the thing sessions 13-16 were spent fixing.

**The hands were not a regression. They were the same defect from the other side.** One projection
layer carries ONE fov claim for the whole eye image, and the world and viewmodel were rendered
through different frustums, so only one could be geometrically correct at a time: the old bug
claimed the fg lens (hands right, world warping) and fixing the world moved the same 1.78x error
onto the hands. Only MATCHED lenses make both right - which is the `(4/3)*(h/w)` constant measured
earlier the same session and deliberately held back so it could not confound the warp test. It
became required rather than optional, and it landed. A second, coupled cause was in `bones.cpp`,
whose `render_lock_delta` asserted that with the match armed "k collapses to 1" - true at 16:9 only,
so at a square backbuffer the depth constraint AND the head-split lateral cancel were mis-scaled,
and that cancel is exactly the term that stops the rig sliding under head motion. `world_ndc` had
the same hardcoded `9/16` for the WORLD lens. Both read the live aspect now.

The user's instruction to use "the previous version and the other repo as research" paid off
directly: `docs/RESEARCH.md` has recorded since session 20 that BioVRDev use
`2*atan(tan(fov/2)*(4/3)/aspect)` - exactly this fix, written down and never acted on. Our `0.75`
is that expression evaluated at 16:9.

Flat gate, same instrument that found the original bug: the dump went from two clusters to **ONE**,
with the 576-byte fg tier now inside the world cluster. `vrfgfov legacy on|off` restores and removes
the split, both directions verified, giving an instant in-headset A/B.

**IN-HEADSET ROUND 2: "without changing anything it's perfect, both the world and the gun/hand
models."** No re-tune. That closed an open question in the process: the session-16 hand offsets were
suspected of having absorbed part of the 1.78x lens error while being tuned against it, and they had
not - they were correct all along, which retroactively validates the sessions 13-16 method as having
solved for the rig rather than papering over a projection error. It also confirms the bone-solve half
that flat could not reach (no XR session means no controller poses, so `render_lock_delta` is never
entered flat).

**Alt-tab, third attempt, and the first one built on a measurement.** The instrument shipped in
round 1 refuted round 1's own fix: `pairOpen=0` on every `SUBMISSION IDLE` line, so the pair-hold was
never the cause. The real one is a circular wait - VDXR drops to VISIBLE on alt-tab and will not
re-grant FOCUSED to an app that submits nothing, and the M8 guard made us submit nothing, so the
guard's effect kept its own precondition true. Corroborating: ZERO `xrWaitFrame blocked` lines in the
whole session, i.e. 5772 presents skipped without one slow wait to justify any of it - the guard
keyed on session STATE when the hazard is a slow WAIT. Since `xrWaitFrame` takes no timeout and never
can, it moved to a dedicated pace thread with the present thread waiting on the result behind a
deadline (200 ms FOCUSED, 20 ms otherwise). That retires the session-26 hang class permanently
instead of trading it for the freeze, and `teardown_session` now defers rather than hand the runtime
a freed session handle. **User: "the alt tab is working again".** Both session-27 open bugs closed.

**Two user questions answered with measurements rather than assurances.** Is it dynamic across
resolutions? Verified at two aspects: 2048x2048 -> `k=1.333333` / fg 115.6 deg, and 1920x1080 ->
`k=0.750000` / fg 83.6 deg - exactly the old hardcoded constant at 16:9, which is what protects the
sessions 13-16 calibration by construction. Checking it also found and closed a fragility: the
backbuffer dims were published only inside the letterbox watch's RGBA8 format whitelist and
staging-allocation success, so a user on another format would have silently reverted to the 16:9
constants and got the 1.78x viewmodel error back. And the whole defect class is now banked for
BioShock 2 in its ENGINE_NOTES as an ordered CHECKLIST rather than constants to copy - starting with
"does BS2 even have two lenses", which session 25's native-fg finding suggests it may not.

**Plus:** `vrstereo stereoonly on|off` and an overlay checkbox that drop only the doubling and
leave 1t and the head drive alone - the A/B that was confounded every previous time, verified
flat. `Bioshock.ini` restored to 2048x2048. And stage 2's blocked fg measurement fell out of the
same dumps: matching the world needs `(4/3)*(h/w)`, so the shipped `0.75` under-lenses the
viewmodel by `1.7778/aspect` - 1.78x at the square backbuffer the README recommends, which is the
"hands look huge" report quantified. Deliberately held back so it cannot confound the warp
re-test.

### Session 26 - 2026-07-29 - BS2 stereo: substrate derived and SR flat-green on the THREADED renderer, no 1t

Branched s26-m10-bs2-stereo off main. Commit 1 shipped the discovery instruments (BS1's
kick sampler + a BS2 deep-caller extension, and kick2 - the sampler hooked on
FEventWin::Trigger itself since the FF15 wrappers mask direct callers) plus the
AlternateEye offset wiring. The pre-derivation offline pass immediately CORRECTED the
session-25 recon: the "SetEvent wrappers" were thread Suspend/Resume + CRT once-init
machinery, the "pump candidates" were a semaphore ctor + an unrelated wait helper.
Live sampling then cracked the frame protocol in two windows: kick2 split the game
thread's five once-per-present Trigger sites and the deep chains + capstone walks
resolved them into Flash/FMOD lock-step kicks and the endframe signal - and the walk
from the Draw-tail chain landed on an aligned-stack ret-0x10 function at 0x4EE8D0 whose
identity fell out beautifully: it sits at slot +0x118 of the session-24 UGameEngine
vtable candidate 0x10BD7DC = UGameEngine::Draw, with BS1's exact ring-cursor offsets
(this+0x118/0x11C) and a tail gate into what session 25 had mislabeled the submit -
actually the FContentStreamingManager view hand-off (its BS1-submit shape match was the
mislabel; RTTI told the truth). Live hexdumps through the command seam verified the
streaming camera globals mirror the CalcView camera and the frame-id pair uses BS1's
high-bit-done convention. Commit 2's pass-through hooks then answered the two live
questions in one beat line: draws/s == presents/s == calcIn/s exactly (PlayerCalcView
runs ONCE inside every Draw - the static "it's tick-side" read was wrong, live wins),
single gameplay caller 0xCD5D7B, and presentTid != drawTid (threaded renderer). The
decisive design call followed the standing policy: BS2's Draw path has NO kick-and-wait
handshake, so commit 3 built SequentialReentry directly on the THREADED substrate -
maybe_second_draw (poison latch, deny-by-default caller gate, calcview-silent +
present-stall skips, SEH-guarded second call), pass-2 replay on the ProcessEvent
fn-match branch, pass-1 base cache + eye offsets, poll-gate hardening (commands never
execute mid-Draw), vrstereo one-toggle = camera mode -> stereo, NO 1t rung. Flat gates
all green first try: pulse call2 ~5 ms real work with presents == draws + pulses;
continuous 107/107 doubled at presents 214/s == 2x with instant off-recovery; stereo
per-eye delta 6.30 UU == expected EXACT with 2nd-pass replay 655/655 and zero
skips/faults; VRSTEREO READY; ~5 min stereo soak clean (monitor caught nothing, crash
dir unchanged). Carries closed: 0x106EE20 = plain APlayerController (RTTI); the
0x4FF0FE idle crash triaged to a transient-null viewport-window read in a focus-poll
path (rare idle race, dump cap already ships). Handoff: the in-headset depth +
world-scale checklist is in docs/bioshock2/TESTING.md - world-scale tuning is
unblocked for the first time.

**Same evening - the in-headset verdict and a hang.** User ran the checklist and accepted
stereo outright ("looks awesome, very good for everything"), world scale fine at the
default 100, with one deferred blemish: the viewmodel/hand models look weird in stereo,
same class as BS1's. Minutes later the game stopped responding. Triage found NO crash (no
dump, no fault, no poison) - a hard wedge at 0 CPU, log ending one line after
`xr: pace keepalive while VISIBLE`. The M8 unfocused-pace guard's 5 s "insurance" keepalive
ran a blocking `xrWaitFrame` (no timeout) with the headset idle and never returned; on BS2
that sits on the dedicated present thread and back-pressured the game thread through the
render ring. Core fix on branch s26-pace-hang-fix: keepalive retired (unfocused presents
always skip the wait; recovery was always event-driven via pump_events), plus a
belt-and-braces close of any leaked open XR frame before waiting. Installed to both games,
awaiting the user's headset-off/on re-test. Lesson worth keeping: a shipped "insurance"
path that calls an untimed blocking API is a hang waiting for the right day.

### Session 25 - 2026-07-29 - BS2 FOV: readback + write landed flat, fg apparatus proven unnecessary

Branched s25-m10-bs2-fov off main. Opened by discovering the session-24 build had crash-looped
on the user's machine: a null read at Bioshock2HD.exe+0x4FF0FE after ~11 min of idle gameplay,
and CSERHelper's chained filter retries the faulting instruction, so our filter wrote the SAME
55 MB dump once per second for 40 minutes - 2,083 dumps, 115 GB. Killed the zombie, kept two
exemplar dumps, deleted the rest, and fixed crash.cpp (repeat-fault suppressor + 3 dumps per
session cap) - the only BS1-shared file touched all session. Then the milestone work in plan
order: ported the value_scan/dumpframe routes into b2r's dispatcher (duplicate-now policy) and
added the b2r-first `vtscan` probe over a new duplicated heap scanner. Derivation ran mostly
WITHOUT the user: vtscan verified vtable 0x11523D8 live (real object among stack-slot false
positives), the ini's property adjacency (MouseIconScale=10 right before HorizontalFOV=100)
matched the object hexdump at +0x48/+0x4C, and the poke proof landed first try - 100->130
moved 39.3% of pixels (8.99 mean-abs), restore back to the 5.0% ambient floor, monotonic
through 150, no clamp. THE finding: the drill viewmodel re-lensed WITH the world in the poke
pair - BS2's foreground follows the world FOV natively, so fovA/fovB/kFgEyeComp/vrfgfov stay
unported (first applied case of the s24 policy; the offset ALSO proved the never-copy rule:
BS1's +0x8C reads 3 here). Wired the readback (claim == rendered, missing object claims 0 =
core's own fallback), the write block (vrfov headset-forced + gfov manual, both default OFF,
strict-gameplay gated, save/restore, stale-restore from the PE detour since BS2 has no
scenedraw hook), heartbeat fov= field, fovaudit, overlay controls, CAP_FOV_WRITE. Flat gates
passed at the menu including the negative one (gfov refuses to write outside gameplay). A
harness lesson re-learned the hard way: an unfocused BS2 pauses to ~1-2 ProcessEvent calls/s,
so a command can sit undispatched for minutes - pair EVERY game-cmd with a game-shot.
IN-HEADSET VERDICT (user, same day): **ACCEPTED - fisheye gone, world-drag gone**, viewmodel
unremarkable, Esc-pause restore edges exercised repeatedly in-headset (vrfov wrote 131 =
headset-derived, effectively BS1's 130; gfov lever re-defaulted to 130 for parity). User also
confirmed mono parallax depth reads well and asked for stereo - correctly not there yet;
world-scale perception likewise deferred to stereo (the mono slider only scales lean
translation, flat-proved working). PR opened and merged same session per the s24 precedent.

### Session 24 - 2026-07-29 - M10 started: BioShock 2 adapter to M3 parity

Branched s24-m10-bs2-adapter off main. Three BS1-touching refactors first (adapter registry
extracted from bioshock1r with exe-name dispatch; log::init grew a per-game subdir so BS2
lives in %LOCALAPPDATA%\BioshockVR\bs2\ while BS1's released layout stays byte-identical;
ue_math.h to game/shared) - BS1 regression smoke passed before any BS2 code ran. The
bioshock2r adapter then went up in two acts: the FName-chain scan resolved eventPlayerCalcView
first try and the hook NEVER FIRED - even in gameplay. Offline caller census (the new
must-run check): BS1's thunk has 29 static callers, BS2's has zero - the build inlined the
event dispatch everywhere. Re-seamed as FindFunctionChecked (learn the UFunction by FName
index) + outer ProcessEvent (mutate the param block post-original, resolved through the
controller vtable slot 3 stub chain, prologue-gated) - worked on the first launch: UFunction
learned in 4 s, both RTTI candidates runtime-verified, GAMEPLAY transition on save load. All
flat 6DOF checks passed integer-exact via the new position-capable simhead lane and the
final-camera heartbeat. Tools grew -Game bs1|bs2; docs split into per-game folders
(docs/bioshock1/, docs/bioshock2/); seam-leak inventory + three design decisions recorded in
the ARCHITECTURE log. Known gaps left deliberately: no FOV readback (expected in-headset
distortion - first follow-up), mono only, no aim/hands, BS1 command vocabulary not ported.
In-headset M3 verdict (user, Quest 3 / VDXR): PASSED - 6DOF head camera tracks in BS2.
Expected artifacts confirmed present: fisheye AND the world dragging with head turns - both
the one FOV-claim mismatch (no BS2 FOV readback yet), which makes the FOV work the clear next
step. Scale judgment deferred to stereo (cannot judge scale in mono - user's call).

### Session 23 - 2026-07-29 - clean-machine new-user flow + crash capture

Began with an external crash report and the user's ask for a full first-run pass.
Identified the reported artifacts as v0.2.0 by three independent methods before the
tester confirmed it, so the report was never evidence of a v0.3.0/v0.4.0 regression.
Ran the clean-machine flow end to end on the published v0.4.0 zip and it passed. Fixed a
user-reported load-crossing stereo drop (watchdog false-positive on loading screens),
verified flat. Added the thumbrest ammo modifier after confirming from the in-tree OpenXR
registry that the pad is a single bit and cannot flick. Then the real find: chasing "crash
to desktop with no dump" led to a filter re-arm check that immediately caught Steam's
CSERHelper.dll displacing our unhandled-exception filter - which is why the tester's newer
crashes produced no dump and no log line at all. Packaged v0.4.1 around the whole
diagnostics set. Three corrections worth keeping: v0.3.0 DOES have the weapon-scan backoff
(only full suppression was new), the AHands backoff already resets on world change (the
"hands not tracking" the user saw was my own test arming `vrstereo on` alone), and the
ungated fg-FOV write at menu time exists in v0.2.0 too, so it does not explain the version
split.


### 2026-07-29 - Session 22 (cinematics measured + fixed, fullscreen routing, first-boot fix, head-roll eyes, turn controls)

- CINEMATICS (plan item 1): BOTH on-file hypotheses disproven by
  measurement on the user's crash-site save (CalcView fires all ride,
  strict stays GAMEPLAY, renderer consumes our camera to the UU) - the
  real defect is the scene rendering its OWN 104-deg fov under a 130
  claim. Shipped the live rendered-fov watch (per-present cb0 tangent
  decode in hud_capture, zero-stall staging) + the cinematic fallback
  (strict/stale/fov-mismatch/screen-only legs, hysteresis, vrcine seam +
  overlay). Default = STEREO cinematics with the claim fixed to the
  measured fov (user's call mid-session); big screen = the toggle. Two
  descent replays flat-green. ENGINE_NOTES has the full derivation.
- FULLSCREEN ROUTING (items 2+7): dumps fingerprinted all three kinds -
  hack board = 322 gameswf draws / ZERO DrawIndexed (world absent),
  loading = 87, alcohol blur = post-tonemap draw sampling a
  backbuffer-sized texture. Shipped the screen-only interval detector
  (-> readable screen) + the in-frame post-effect skip (srv0-size
  discriminator; postFx=0 false positives). Live hack board verified
  with the user at the machine; BD FMV verdict is on the headset list.
- FIRST-BOOT RESTART (item 3): compose_over answers the dead boot probe
  with a neutral connected pad; marker machinery deleted; README note
  gone. Virgin-install gate PASSED (KB/M prompts untouched; IAT lane
  ~680 polls/s after mid-session vrinput on; dpad moved the menu).
- HEAD-ROLL EYES (item 4): apply_eye_offset -> full-rotation right axis
  (ue_rot_basis), AER path unified. Eye delta rotates exactly with roll
  (6.3/4.455/0 lateral at 0/45/90 deg), bit-identical at roll 0.
- TURN CONTROLS (item 5): smooth-turn scale + snap turn (recenter-
  composite steps via the body transfer). +-8192-unit exact steps, one
  per pulse; scale 2.0 -> composed rx exact. Persisted + overlay.
- WONKINESS (item 6): instrument shipped (sticklog + accessor); the
  headset capture + analysis queued.
- Harness: descent replays create a Welcome-to-Rapture AutoSave (CONTINUE
  moved!); loads show as screen-only pairs, no view-state/fov churn;
  fire tests need sim lanes re-armed or subs read as skips; user preset
  inis backed up (backup-s22-20260728, sha256) and restored byte-exact
  after the virgin test.

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
  (OpenXR-first for VDXR+SteamVR). Full findings Ã¢â€ â€™ RESEARCH.md.
- Decided architecture (C++20/x86, xinput proxy Ã¢â€ â€™ bioshockvr.dll, core+adapter split, stereo
  ladder with SequentialReentry as primary bet) Ã¢â€ â€™ ARCHITECTURE.md decision log.
- Built: repo + docs suite, CMake (VS2022 `-A Win32`, submodules minhook/imgui/OpenXR-SDK pinned),
  xinput proxy (ordinals verified against the real SysWOW64 DLL with dumpbin - game imports @2/@3),
  mod DLL (deferred init, logger, minidump handler, MinHook, kiero-style Present/ResizeBuffers
  hooks, ImGui overlay), tools scripts.
- **In-game smoke test passed** on first run (full init chain + D3D11 device info in log).
  Found + fixed: logger file locking (fopen_s denies sharing Ã¢â€ â€™ switched to `_wfsopen` with
  `_SH_DENYNO`), non-ASCII mojibake in log lines, missing `-Install` passthrough in build.ps1.
- Verified: LAA=YES; D3D11 confirmed at runtime; user ini path confirmed after first launch.
- Repo created and pushed: https://github.com/mohamad-balouza/bioshock-vr (public, MIT).

## Session log

### 2026-07-30 - session 29 (branch s29-b1r-cinematics-and-aim-dot): stage 3, accepted in-headset

Cutscene bars, cinematic drive modes, subtitles and the aim dot. v0.5.0 packaged, awaiting the
user's final go before merge and tag.

- **The bars were never a squeeze.** Retracted session 22's reading with two measurements taken
  before writing any code: the Nexus mod is a ONE-BYTE SWF edit zeroing the `WidescreenBars`
  PlaceObject2 scale, and a framedump inside the letterbox shows a full-frame tonemap with a
  textureless 29-vertex gameswf draw after it. `blit::stretch_band` deleted.
- **The in-headset failure was the HUD redirect eating the bar draw** before the pixel watch
  could see it - one root cause for "bars still present" AND "hands still wrong". Detection no
  longer depends on the pixel watch at all.
- **Both bone bugs were found by the instrument, not by reasoning**: the sticky collapse at
  cutscene entry (`hiddenHand=0 cacheAge=16ms`) and the stuck collapse from a mid-scene mode
  switch (`cacheAge=32578ms`, exactly when the gate re-closed).
- **The aim dot round-trips to 0.0000 UU** (80 nanometres) through the new `game_point_to_xr`,
  so dot == shot by shared data rather than shared algebra.
- Methodology that paid: measuring the mechanism before coding (the SWF diff cost minutes and
  killed a whole wrong approach); shipping the refuting instrument with the fix (the vertex-count
  log immediately proved "textureless = bars" would have been wrong); and stating plainly which
  claims flat could not test - all three of those turned out to matter in headset.

### Session 42 rounds 2-4 addendum - 2026-08-05 - in-headset acceptance -> v0.7.0 SHIPPED

Round-1 verdicts: HUD, crosshair, menus, pad-at-title/main-menu ALL GOOD. Fixed in
rounds 2-4 from the user's reports + their pre-cutscene save (replayed in the sim):
M7.5 body transfer ported (offset +0x1F8 DERIVED live, probe confirmed, camera
invariant bit-exact - movement follows the view, snap turn carries the pawn);
BS2 bars fingerprint derived (11-vert textureless; skipped 6288/6288 - no panel
bars; the cine hold sustains 43 s with authored camera AND hands); the reticle
transition fault fixed (freshness gate + call-time vtable + per-world latch).
Release defaults per the user: lasers off/dots on both hands, cineDrive
authored+look. **v0.7.0 released** (tag on bioshock-2, one zip, both games):
https://github.com/mohamad-balouza/bioshock-vr/releases/tag/v0.7.0
Open for later: subtitles verdict, flicker watch ([flick] armed), main-menu-A was
confirmed working by the user (menukey + native paths), plasmid-hand profiles.

### Post-release addendum - 2026-08-05 - BS1 in-headset verdict, merged to main

The user tested BioShock 1 in the headset on the released v0.7.0 binaries:
LOOKS GOOD, NO REGRESSIONS observed. With both games verified on the shipped
DLL (BS2 flat+headset across rounds 1-4, BS1 sim smoke + headset pass),
`bioshock-2` was merged to `main` - main now carries the full duology mod at
v0.7.0. The exhaustive BS1 regression checklist remains available for the
end-of-development pass, but the release-blocking question is answered.
