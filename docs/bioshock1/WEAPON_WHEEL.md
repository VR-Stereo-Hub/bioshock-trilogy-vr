# BS1 weapon and plasmid wheels in VR: the Dishonored VR hand dial, ported

Status (2026-10-15): **the stage seam (3.8) and the dial input (3.1-3.7, 3.9) are built and
host-tested, not run in the game.** `wheel_state.cpp` hooks the HUD movie's call routine
(ENGINE_NOTES "The HUD movie's call routine") and `wheel_mirror.h` rebuilds both wheels from it;
`wheel_dial.h` is the pure dial, `dial_input.cpp` steers the game's sticks through
`xinput_bridge::set_stick_override`. F10 > Controls > Weapon and plasmid dial, off until enabled;
the settings listed in 3.15 that belong to the input are there. One finding the design did not
have: the movie ignores the stick for its open animation (~210 ms) and reacts only to wedge
CROSSINGS, so the dial holds neutral for 250 ms after the wheel shows (`Config::armMs`).

2026-10-16: **the Dial anchor, the name panel and the close lease (3.9-3.11) are built and
host-tested, not run in the game.** The HUD table's wheel is now three rows: `Wheel` (the ring,
0.365..0.660 x 0.200..0.717), `WheelName` (the highlighted item's name, 0.360..0.670 x
0.125..0.178) and `WheelLabel` (SELECT WEAPON / SELECT PLASMID, 0.400..0.630 x 0.738..0.790),
all owning what they cover while open. With the dial on and a tracked press, `dial_input` publishes
the pose the press froze (seqlock, once per press) and `hud_elements` poses the ring at it at
F10 > Controls > Weapon and plasmid dial > **Dial width** (0.35 m) and **Dial distance** (along the
dial's facing), the name rigidly above the ring as the movie draws it, and hides the button line.
The open state comes from the mirror (`wheel_state`), and the lease keeps the pieces up and posed
for 250 ms after it closes (the close animation; the "+3 presents" half is not needed when the
time runs on the provider's own clock, which only ticks on presents). Unarmed or untracked, the
wheel stays on the HUD panel as before. Log: `[hudel] the weapon wheel at the hand dial ...` once
per opening. Not built yet: immersion (3.13), gates (3.14). Host suites `wheel-dial` (81 checks)
and `hud-elements` (53).

Original status: design. Written 2026-10-15 from a read of the Dishonored VR mod's
hand dial (`C:\dev\Dishonored-VR`, VR-126 and its follow-ups) and of BS1's own wheels
(ENGINE_NOTES "2026-10-15 - The weapon and plasmid wheels"). Every Dishonored behaviour below
cites the file it lives in. Engine numbers are BS1's own; none is copied from Dishonored.

The goal: hold a grip, the game's wheel appears in the world at that hand, and moving the
hand picks a slot - for both of BS1's wheels, in every stage from 0 items to 8.

## 1. What BS1 gives us

Short form of the ENGINE_NOTES section, with what each fact means for the port.

| BS1 fact | Consequence for the dial |
|---|---|
| Two wheels: RB = weapons (`weaponScrollBar`), LB = plasmids (`abilityScrollBar`). The VR pad already composes the right grip to RB and the left grip to LB | Two dials, one per hand: the right grip opens the weapon dial at the right hand, the left grip the plasmid dial at the left. Dishonored has one (left grip) |
| Both wheels are the same 8-position radial at stage (662, 340), drawn into the HUD capture | One crop serves both; the HUD element table's `Wheel` row (live while a bumper is held) is the piece |
| Slot index fixed: 0 up, 1 down, 2 left, 3 right, 4 up-left, 5 up-right, 6 down-right, 7 down-left; items fill in pickup order | The dial's wedge for an index is fixed, so it can be drawn and snapped without asking the game |
| Empty slots draw nothing: 2 items = a vertical pair, 4 = a cross, 6 = cross + upper diagonals, 8 = full ring | THE STAGES: the dial must only ever select a FILLED index |
| 0 items: the wheel never opens. 1 item: the bumper just equips it | The hand must not steer anything in those stages (see 3.8) |
| Plasmids: 2 slots at the start, up to 6 (BasePlasmidSlots + Gene Bank upgrades); indices 6-7 never fill | The plasmid dial tops out at 6 |
| `UpdateRadialAxis` wedges: diagonals when both axes are past 0.2 and inside 0.9; cardinals past 0.901 with the other axis under 0.5 | Unequal wedges on a unit circle: cardinals about 51.6 deg wide, diagonals about 38.4 deg. A raw hand angle would make diagonals hard to hit (see 3.4) |
| Weapons commit on bumper RELEASE (pending while held); plasmids switch the moment the stick lands | The dial must keep pointing at the chosen weapon until the grip opens; a plasmid can change mid-sweep |
| A bumper TAP (no radial input) cycles to the next weapon with ammo / the next plasmid | A grip tap with no hand movement must stay a tap: no direction injected below the neutral radius |
| The game feeds the radial from whichever stick last crossed into a new wedge (`UpdateRadialMenu`, right preferred), game deadzone 0.225 on both | Inject the hand's direction on ONE stick (the right) and hold the other at zero, or the left stick steals the wheel |
| Release sends `UnKillLook` + `ForceUnPause`: the game pauses and kills stick look while a wheel is open | World time stops; head tracking is ours and should keep working (to verify) |
| Opening the other hand's wheel swaps the raised hand first (`AutoSwap`, `SwitchHandModes`) | Opening the plasmid dial while holding a gun is legal and switches hands |
| The wheel's title is the highlighted item's name (filled at run time); "SELECT WEAPON / PLASMID" under it | The name above the ring is worth keeping as its own small panel (Dishonored's side panels) |

## 2. Dishonored's dial, behaviour by behaviour

Each item: what it does, where, and why (the headset report that produced it).

1. **Grip opens it.** Left grip past 0.9 (release under 0.7) is `PowerWheel` = LB
   (`pad_bridge.cpp`, `CONTROLLER_BINDS.md`). The same press drives the game's own wheel.
2. **Opening origin, frozen.** On the grip's rising edge the dial centre is the grip position
   in XR LOCAL space and never moves after (`weapon_dial.h` `State::update`). The hand can then
   move freely to point; the dial stays where the hand WAS.
3. **Opening orientation, frozen.** The dial faces the opening EYE position in the horizontal
   plane and stays upright - not the head's rotation (build 439: opening while looking away left
   an awkward yaw). Options `WeaponDialEntryTilt` (also take the head's pitch/roll) and
   `WeaponDialEntryYaw` (take the head's yaw instead of facing the eye position), both default
   off, apply at the NEXT opening, never mid-gesture (`hud_anchor.h` `OpeningOrientation`).
   The visual quad, the input axes and the distance offset all share that one frozen frame.
4. **Hand to stick.** Hand displacement from the origin is projected on the frozen dial's
   right/up axes; depth movement does not select. **Direction only** (default on): any movement
   past the neutral radius (default 2 mm, F10 0.5-10 mm) outputs FULL magnitude at that angle -
   a tiny movement picks a wedge. Analog mode (off): `WeaponDialRadius` (0.04 m) of travel is
   full input.
5. **Continuous output, angle preserved.** The direction goes to the game as a continuous left
   stick with an angle-preserving radial deadzone; the right stick is zeroed so two navigation
   streams cannot compete. VR-126's root cause of "cardinals are easy, diagonals are hard": the
   menu path had been stepping each axis independently (+-32000 pulses on separate repeat
   timers) and a per-axis deadzone bent angles. The wheel context is exempt from menu stepping.
6. **The hand only.** With the dial on, both sticks are IGNORED while the wheel is up
   (`wheel_motion_only`): the stick that was running or turning when the grip closed must not
   pick a wedge. (The F10 help still says "either stick overrides" - the code is the truth.)
7. **Tracking loss.** Losing the hand neutralises the dial and it does not re-seed until the
   grip is released and pressed again. Non-finite poses are refused.
8. **Refusal grace.** If the game has not shown the wheel within `WheelOpenGraceMs` (250 ms) of
   the grip, the hand stops steering the stick for that press; the bumper itself still reaches
   the game (2026-10-14: in the prologue, with nothing to select, the pawn walked where the hand
   pointed for 5 s holds). One log line per refused press.
9. **Release.** The grip's release is the game's commit. While the game still reports its wheel
   after the release, gameplay axes stay live (`released_wheel`) - the closing wheel must not eat
   the right stick.
10. **Close without a flash.** A visual lease keeps the wheel's image, crop, pose and alpha for
    250 ms after release (the movie's exported close animation) plus three presents after the
    closing flag clears (`hud_menu_lifecycle.h` `WheelVisualLease`). Another menu, a load or a
    cinematic pre-empts it at once.
11. **Every opening is fresh.** Each real screen entry re-seeds the visual origin from the
    CURRENT hand, even when the game opened the wheel without the grip path (build 387 reused a
    previous opening's origin).
12. **Placement.** Quad at origin + opening-forward x `WeaponDialDistance` (-0.30 .. +0.50 m,
    default 0); width 0.35 m (0.15-1.2); source crop 0.40 x 0.40 of the capture around the
    centre; circular crop feathered to zero in the HUD alpha shader on the wheel's sink only,
    round in metres (pixel aspect corrected); its own alpha group (`WeaponDialAlpha`).
13. **Side panels.** The D-pad shortcuts and the health/mana potions are cut from the SAME
    wheel capture into separate quads with their own anchors, offsets, sizes, crops and alpha
    (`WheelSidePanels`, VR-129). The dial crop alone hid them.
14. **Immersion while open.** Per-context `HeadLook` (the world camera follows the head while
    the game is paused - otherwise turning shows the frozen FOV rectangle) and `NoBlur` (zero
    the UI blur weight, restore exactly on exit). A turn made inside the wheel carries through a
    context hand-over to the exit (VR-166). Head-mouse emulation pauses while the wheel is held
    and re-syncs on close (`head_track.cpp`).
15. **Gates while held.** The held wheel blocks the zoom gesture, the zoom tap, physical crouch
    and other gestures (`g_wheelHeld` in `pad_bridge.cpp`); with the F10 panel up the hand does
    not steer the dial.
16. **Instruments.** `hud/dial: open|close tracked valid centre width radius crop`,
    `hud/dial: visual entry`, `pad/wheel: source=hand|stick|neutral raw out` every 500 ms,
    the refusal line once per press, the visual context and lease lines.
17. **A cost that showed up.** With the wheel open, the share of one-eye-only ticks rose from 10%
    to 47% over three builds (VR-142) - a per-hand-draw head-pose lock. The dial is per-present
    work only.

## 3. The port, decision by decision

**3.1 Two dials.** Right grip -> weapon dial at the right hand; left grip -> plasmid dial at the
left. Each has its own `State` (origin, orientation, held, valid, lost). Only one can be open
(the movie's `ContextOpenWeaponMenu`/`ContextOpenAbilityMenu` refuse the second); a second grip
while one dial is open is ignored by the dial (the bumper still reaches the game, which decides).

**3.2 Origin and orientation.** Exactly as 2.2-2.3: frozen at the grip's rising edge, facing the
opening eye position, upright; the same two entry options. Shared code: a BS1 copy of
`weapon_dial.h`'s math (decoupling rule: copied, not shared with another game).

**3.3 Hand to direction.** As 2.4, direction-only by default, neutral radius 2 mm. Both F10
modes kept.

**3.4 Snap to a FILLED slot (new, BS1-specific).** BS1's wedges are unequal (section 1) and half
the indices may be empty. The dial therefore does not pass the raw angle through: it picks the
nearest FILLED slot direction to the hand's angle and outputs that slot's canonical vector -
cardinals (+-1, 0)/(0, +-1), diagonals (+-0.707, +-0.707) - which lands squarely inside the
movie's wedge for that index. Every filled slot then gets an equal share of the circle: 2 items
= two halves (up / down), 4 = quarters, 8 = 45 deg each. Requires knowing `slotsFilled` per
wheel (3.8). Fallback while it is unknown: raw angle, unit magnitude (still hits every wedge,
unequal widths).

**3.5 Injection.** While a dial is steering: the right stick axes the game receives = the
snapped vector; the left stick = 0 (the movie takes whichever stick last changed wedge). Turning
is already killed by the game (`KillLook`); the composer's turn gate and the d-pad lane already
stand down while a grip is held (`openxr_input.cpp`, "radials read the stick").

**3.6 Hand only, taps preserved.** Sticks ignored while a dial steers (2.6). Below the neutral
radius nothing is injected, so a grip tap stays the game's tap-to-cycle.

**3.7 Commit semantics.** Weapons: the dial keeps outputting the last chosen slot until the grip
opens - returning the hand toward the centre must not clear it (the movie ignores (0,0), and the
pending weapon stays). Plasmids: the selection follows the hand live; that is the game's rule
and stays.

**3.8 Knowing the stage - the one engine seam this needs.** Dishonored reads the game's UI
context ("Wheel is up"). BS1 has no such signal today: the top movie stays `HUDRadial.swf`, and
the wheel lives inside it. Needed, per wheel: is it open, and how many slots are filled (and,
for labels, which item is in each). Candidate seams, in order of preference:

- **Mirror the HUD's own calls.** Hook the native that delivers `CallMethod*` to a gameswf movie
  (the path `UFlashMovie::execCallMethod*` and the C++ callers share) and watch for `AddWeapon`,
  `RemoveWeapon`, `EmptyWeapon`, `AddAbility`, `RemoveAbility`, `EmptyAbility`, `SelectWeapon`,
  `SetActiveHands`, plus the opening functions. The mod then knows slot contents exactly as the
  movie does, by class name (also the item names for the title). Rare calls; one string compare
  each. Needs IDA: the function and its calling convention (`anthropic-skills:engine-offset`).
- Read `_root.HandsIcons.weaponScrollBar.slotsFilled` / `_root.weaponScrollActive` through the
  gameswf variable API on the `FlashMovie.movie_interface` pointer. Also IDA, riskier (a call
  into gameswf from our thread).
- Count from the engine inventory (`ShockPlayer`, `PlasmidManager`): weapons held and equipped
  plasmid slots. Fragile against what the HUD actually shows (the camera, empty slots).

Until the seam exists, the dial uses Dishonored's refusal grace (2.8) with the grip-hold time
as the only signal and the raw angle (3.4 fallback): with 0 or 1 item nothing opens, and after
250 ms the hand stops steering, so the worst case is 250 ms of injected direction on a press the
game did not turn into a wheel (the game kills look only when the wheel opens, so that window
could turn the view - the main reason to build the seam first).

**3.9 Tracking loss, refusal, release, close lease.** As 2.7-2.10. Lease 250 ms + 3 presents
(the BS1 close is frames 11-14 at 24 fps, about 170 ms, so 250 ms covers it).

**3.10 Every opening fresh.** As 2.11, keyed on our open signal.

**3.11 Visual.** The HUD element table already has the `Wheel` row (rectangle 0.37..0.67 x
0.14..0.80, owner of everything it covers while open). Add one anchor kind, **Dial**: the frozen
origin + forward x distance, the frozen orientation, width 0.35 m. Crop the RING only (stage
about 470..845 x 145..515 around (657, 330)): the wheel's background is transparent, so a
rectangle around the ring needs no circular mask and no shader. The title (item name) and the
"SELECT ..." line become an optional **name panel** above the dial (a second crop of the same
capture, Dishonored's side-panel pattern). Separate rows for the weapon and plasmid dials are
not needed: one Wheel row, the anchor following whichever grip opened it.

**3.12 Stages, drawn.** Nothing to do per stage visually: the movie draws only filled slots, so
2 / 4 / 6 / 8 items already look right. The crop is the full ring in every stage.

**3.13 Immersion.** Pause: the game pauses itself (`ForceUnPause` on close). Head look: verify
first - BS1's camera drive is ours and the pause menu already renders a live world, so the head
may already look around with the wheel open; port Dishonored's HeadLook only if the headset says
the view freezes. Blur: BS1's wheel has no blur to remove (to verify).

**3.14 Gates.** While a dial steers: no swing-to-attack and no reload gesture from that hand, no
snap/smooth turn (already), no d-pad lane (already). The hands and the laser keep drawing (the
game is paused; whether the laser should hide is a headset call, default: hide it while a dial
is open).

**Wedge arithmetic (for 3.4 and its test).** On a unit circle the movie's diagonals hold
25.8-64.2 deg (both axes inside 0.9) and the cardinals the rest (other axis under 0.5), so a raw
hand angle gives cardinals 51.6 deg and diagonals 38.4 deg. The snapped vectors sit at the
centres: (1,0) and (0.707,0.707) also survive the game's per-axis 0.225 deadzone with room.

**3.15 F10 (HUD > Weapon and plasmid dial).** Enable (default off until the headset accepts it),
distance offset, direction only, neutral radius, hand travel (analog mode), dial width, follow
head tilt / horizontal angle on opening, snap to filled slots, name panel on/off + its offset and
size, refusal grace (ms). Every key saves through `menu-settings.ini`.

**3.16 Instruments.** `[dial] open weapon|plasmid hand=R|L slots=N origin= facing=`, `[dial]
close committed=<index> held=<ms>`, `[dial] refused (no wheel after N ms)`, `[dial] 3 s: hand ->
slot samples, stick out`, `[dial] stage weapons=N plasmids=M` on every change (from 3.8).

**3.17 Cost.** Per present: two pose reads, a projection, a snap over at most 8 directions.
No per-draw work, no lock, no allocation, no readback.

## 4. Stages to test (each in the headset)

| Stage | Expected |
|---|---|
| Weapons 0 (before the wrench) | Right grip: nothing opens, the hand never steers, after 250 ms `[dial] refused` |
| Weapons 1 | Right grip equips it; no dial |
| Weapons 2 (wrench, pistol) | Dial with up/down only; any hand movement up or down picks; sideways picks the nearer |
| Weapons 4 / 6 / 8 | Quarters / sixths (cross + upper diagonals) / eighths; the lower-left (index 7) reachable |
| Weapon held, release on a slot | Equips on release; moving back to centre before release keeps the choice |
| Grip tap, no movement | Cycles to the next weapon with ammo |
| Plasmids 2 / 4 / 6 | Left-hand dial; the plasmid switches as the hand crosses slots |
| Plasmid dial opened with a gun raised | Hands swap, dial at the left hand |
| Opening while looking 60 deg to the side | Dial at the hand, upright, facing your position |
| Hand lost mid-gesture | Neutral; reopening re-seeds |
| Close | No flash of the wheel on the HUD panel |

## 5. Order of work

1. The stage seam (3.8, IDA) and its `[dial] stage` line - flat-testable, no headset.
2. The dial input (3.1-3.7, 3.9) with host tests: every direction x every stage x both modes,
   frozen origin and orientation, tracking loss, refusal grace, taps, commit semantics, the
   snapped vectors landing in the movie's own wedges (port `UpdateRadialAxis`'s thresholds into
   the test).
3. The Dial anchor and the name panel (3.11) on the HUD element planner.
4. F10 and instruments (3.15, 3.16).
5. Headset: section 4's table.
