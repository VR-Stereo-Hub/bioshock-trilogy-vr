# BS1 held-gun mechanics: every combination, and what each one does

Written 2026-10-21 at 2d206da (steps 1 and 2 of `HELD_GUN_AUDIT.md` 2.8 in). This is the step-3
specification: every gun, every hold, every ammunition count and type, both hands, every magazine and
shell state, every input, mixed and matched, and the behaviour of each cell. A cell is marked by where it
stands:

- **done** - implemented and host-tested as of 2d206da (steps 1 and 2); the headset check is listed in
  STATUS.md
- **③ ④ ⑤ ⑥** - the step of 2.8 that makes it so (3 ownership table, 4 suppression, 5 pump, 6 sounds)
- **gap** - found by this enumeration; proposed behaviour given, not yet decided

The point of the layout is that the space is a PRODUCT OF SMALL ORTHOGONAL RULES, not a list of special
cases. Section 1 names the rules; sections 3-6 are their product written out per gun so nothing hides.

---

## 1. The six rules that collapse the space

| Rule | What it removes from the enumeration | Status |
|---|---|---|
| **R1 Mirror.** Every point of a gun is defined in the right-hand frame; the left-hand hold is the same table through one reflection (`gun_geometry::mirror_point`). | The "gun hand" dimension: every row below holds for both hands. | done (step 1) |
| **R2 Fire.** The trigger reaches the game iff `fire_allowed`: no manual reload, a magazine in, cocked, no pump held for the hands. | The trigger column collapses to one function of the state record. | done (step 2) |
| **R3 Click.** Every pull refused by R2 clicks with the game's own DryFiring; the game's own click stays for the one case R2 lets through (an empty idle gun, which the game reloads or clicks itself). | The sound column for the trigger. | done (step 2) |
| **R4 Owner.** A squeeze of the free grip belongs to ONE owner, decided on its rising edge by the gun's state and where the hand is: a zone priority per state, then distance; held until the grip comes up. | The "who gets this squeeze" column becomes a lookup. | ③ (today: nearest-wins idle, absolute claim active) |
| **R5 Pose.** While two hands are on the gun the free hand is drawn at the gun's rest pose, whatever any clip does; while it carries a part it wears that part's designed grip. | The "what is drawn" column for the free hand. | done (step 1) for held; carries done since 10-20 |
| **R6 Game.** What the game is not allowed to do while the mod drives a gun is one record per gun: X, the trigger, its reload, clip motion, the free hand's clip pose, its sound cues, the magazine's visibility. | The "what does the game do meanwhile" column. | ④ (today: the same facts, spread across flags) |

Ammunition TYPE is not a dimension of the mechanics at all: the mod never reads which type is loaded. It
reaches the mechanics in three places only (section 7): the visible magazine's mesh (the game's), the dry
click's sound tag (the game's), and a SWITCH of type (pending differs from current), which is a state bit.

## 2. The dimensions and the zones

**State record** (`gun_state.h`, one per tick): gun kind; rounds, magazine size, spare rounds; switching;
magazine (in, out, in hand); a spare in hand; reload stage (none, starting, waiting, carrying, dropping,
seating, pumping, finishing); a part carried; the reload waiting for a free hand; the last pump's wait;
cocked; a shot's pump held; two hands on the gun.

**Inputs:** the free hand's grip (rising edge, held, release); the gun hand's trigger; X; an ammunition
switch (the wheel); a weapon switch; a plasmid switch; the free hand letting go of a carried part.

**Zones** the free hand can be in when it squeezes (grip-frame distances, the 12 cm grab radius unless said):

| Zone | What it is | Which guns |
|---|---|---|
| FORE | within 12 cm of the fore-grip point (`gun_geometry` table) | Tommy, shotgun, launcher, chem, crossbow |
| PART | within reach of the part a reload wants now: the pistol's barrel latch (fold path start), its cylinder (pull start), the Tommy's drum (pull start, 12 cm), its bolt (either side, 15 cm), the shotgun's port (a carried shell within 15 cm) | per stage |
| POUCH | the waist: 38-100 cm below the head and within 50 cm of it sideways | all guns that reload |
| HANDS | the two controllers within 12 cm of each other | all |
| ELSE | none of the above | all |

Zones overlap. FORE and the Tommy's drum PART are about 20 units apart, so a hand under the fore-grip is
in both; POUCH and HANDS overlap when the gun hand is at the waist. R4's priority decides overlaps, then
the nearer target; today the idle grabs are nearest-wins and an active reload's claim is absolute (bug 1).

## 3. Pistol (one-handed; `FastReload`/`Reload`; ejects; the cylinder is the gun's own bones)

Magazine 6 (upgrades raise it). Ammunition: Bullet, AntiPersonnel, ArmorPiercing.

| State | FORE | PART | POUCH | HANDS | ELSE | Trigger | X |
|---|---|---|---|---|---|---|---|
| idle, rounds < size, spare > 0 | n/a | start the reload (grab the latch) **done** | nothing (no spare prop) | hand-over **done** | game (wheel) | fires **done** | eject: the game reloads, held open, the cylinder falls (its rounds as a world prop) **done** |
| idle, full | n/a | nothing (the game refuses) **done** | nothing | hand-over | game | fires | deny buzz **done** |
| idle, rounds < size, no spare rounds | n/a | nothing | nothing | hand-over | game | fires | deny buzz **done** |
| idle, empty, spare > 0 | n/a | start **done** | nothing | hand-over | game | the game's pull: its auto-reload begins, adopted, held at the latch with the cylinder in **done** | eject |
| idle, empty, no spare | n/a | nothing | nothing | hand-over | game | the game's own click **done** | deny |
| waiting at the latch (adopted or started) | n/a | the reload (fold the barrel) **done** | nothing | hand-over (hand free) **done** | game | click **done (step 2)** | drop the cylinder (ejectNow) **done** |
| waiting at the cylinder pull | n/a | the reload **done** | nothing | hand-over | game | click | drop |
| carrying the spent cylinder | the reload's grip | - | - | - | - | click | - |
| let go, spent cylinder | falls out of the gun (own bones, 0.12 s), its rounds as a prop **done** | | | | | | |
| dropping (0.7 s) | squeeze swallowed **gap**: should be hand-over / game | - | - | **gap** | **gap** | click | - |
| waiting at the pouch (magazine out) | n/a | - | take a cylinder **done** | hand-over **done** | game **done** | click **done** | nothing (**gap**: no way to abort; proposal: X at the pouch cancels the reload, the gun stays open until the weapon is switched) |
| carrying the new cylinder (designed grip) | the reload's | guided within 6 cm of the path **done** | - | - | - | click | - |
| let go near the frame (10 cm of the seat, or 20 cm slap, or past half way) | seated: the clip plays it home **done** | | | | | | |
| let go far | it falls (no world cylinder exists: it is simply gone), back to the pouch **done** | | | | | | |
| seating (clip plays) | squeeze swallowed **gap**: hand-over / game | | | | | click | |
| waiting at the flick | gun-hand flick 320 deg/s, or push the barrel up its path **done** | | | hand-over | game | click | |
| finishing | - | - | - | hand-over | game | click until the clip ends (the game refills at its end) **done** | |
| ammunition switch while waiting | the cylinder drops as X does **done** | | | | | | |
| ammunition switch at idle (pending differs) | the game's own reload for the switch, adopted with an eject **done** | | | | | | |
| weapon switch mid-reload | the game aborts; the pistol comes back closed with its old rounds; its magazine shown again if ejected **done** | | | | | | |
| plasmid switch mid-reload | the same | | | | | | |

Left hand (R1): identical; the pistol has no fore-grip, so the mirror affects only the drawn gun (its
centre plane 1.4-2.2 units off the root, **done**) and the carried cylinder's designed grip (mirrored by
`mirror_free_rel`, **done**).

## 4. Tommy Gun (two-handed; `Reload`; ejects; the drum is the AmmoModel; a spare prop)

Magazine 40. Ammunition: Bullet, ArmorPiercingBullet, FrozenBullet (the game's RubberBullet class is unused).

| State | FORE | PART | POUCH | HANDS | ELSE | Trigger | X |
|---|---|---|---|---|---|---|---|
| idle, loaded, rounds < size, spare rounds > rounds | two-hand **done** | start (grab the drum) **done**; FORE wins a tie **③** | take a SPARE drum **done** | hand-over **done** | game | fires | eject: the drum drops, take a new one **done** |
| idle, full | two-hand | nothing | spare if spare rounds > rounds **done** | hand-over | game | fires | deny |
| idle, no spare rounds | two-hand | nothing | nothing **done** | hand-over | game | fires | deny |
| idle, empty, no switch near | two-hand | the game's auto-reload is adopted at the DRUM (nothing dropped): see "waiting at the drum" | spare **done** | hand-over | game | the game's pull: its reload begins **done**; then click | - |
| idle, empty, switching (or within 1.5 s of a switch) | two-hand | adopted WITH an eject: the drum drops, then the pouch **done** | | | | | |
| waiting at the drum (dry gun) | two-hand (hand free) **done**, but the drum's claim is absolute inside 12 cm (bug 1) - **③**: FORE/drum by distance, FORE wins ties | pull the drum **done** | spare **done** | hand-over **done** | game | click **done (step 2)** | drop the drum **done** |
| pulling the drum (dragging) | the reload's | the reload's | - | - | - | click | - |
| carrying the spent drum | the reload's | - | - | - | - | click | - |
| let go, spent drum | thrown as a Havok copy with the hand's velocity (Alyx's release) **done** | | | | | | |
| dropping (0.7 s; 0.2 s with a spare in hand) | squeeze swallowed **gap**: should be two-hand / hand-over | - | - | - | - | click | - |
| waiting at the pouch, magazine out, no spare in hand | two-hand (hand free) **done** | - | take a drum **done**; a thrown drum within 15 cm of the seat is CAUGHT **done** | hand-over **done** | game | click **done** | nothing |
| waiting at the pouch, a spare already in hand | the grip is the spare's; at the fetch it goes straight in (0.2 s) **done** | | | | | click | X with a spare in hand before the drop: the game is asked to reload, the drum drops, the spare goes in **done** |
| carrying the new drum (the clip's own hold at 0.30 s) | refused (carrying) **done** | guided within 6 cm; the latch at 10 cm of the seat **done** | - | - | - | click | - |
| let go: within 10 cm of the seat, or 20 cm (slap), or guided | seated: the clip plays it home **done** | | | | | | |
| let go far | thrown (a full drum: catchable 4 s) **done**; back to the pouch | | | | | | |
| seating (clip plays) | squeeze swallowed **gap**: two-hand should be allowed | | | | | click | |
| waiting at the bolt (rounds were 0 at the start) | two-hand **done** | rack the bolt, either side, 15 cm **done** | spare **done** | hand-over | game | click | - |
| the bolt skipped (rounds > 0 at the start) | straight to the end **done** | | | | | | |
| bolt runs home, finishing | two-hand **done** | - | spare | hand-over | game | click until the clip ends **done** | - |
| idle, LEFT without a drum (switched away mid-reload) | two-hand **done** | - | the game is re-asked to reload every 1.5 s (3 tries), adopted at the pouch **done** | hand-over | game | click **done** | - |
| ...the game refuses 3 times (no rounds) | the drum is shown again, the gun counts it **done** | | | | | | |
| spare in hand, any idle or waiting-at-part state | the grip holds the spare; let go: thrown **done** | | | | | | |
| spare in hand + weapon switch | the spare is destroyed **done** (**gap**: it could be kept per gun; proposal: destroy, as now) | | | | | | |
| spare in hand + hand-over | **gap**: the spare is attached to the LEFT wrist bone, which is now the gun hand; proposal: a hand-over with a spare in hand is refused (the squeeze is the spare's, so it cannot happen today either: a held grip never rises) | | | | | | |
| ammunition switch while waiting at the drum / pouch / bolt | the drum drops as X does; the game ends its reload and begins one for the new type, adopted **done** | | | | | | |
| weapon or plasmid switch while the drum is out or carried | the gun stays WITHOUT a drum (hidden, fire refused) until a new one goes in **done** | | | | | | |
| weapon switch while a new drum is seated or later | the gun keeps its drum **done** | | | | | | |

Left hand (R1): the fore-grip, the rest pose and the seat reflect about the centre plane **done**; the
spare's hold against the wrist is mirrored **done**; the throw and the catch are world-space, unchanged.

## 5. Shotgun (two-handed; `SingleFrame` / `Reload_Loop` / `Reload`; shells; a pump; a chamber)

Magazine 4. Ammunition: 00Buck, IonicBuck, HighExplosiveBuck. The game's reload is three-part: a loop per
missing shell, the end (the pump) only when full; a switch of type restarts it.

| State | FORE | PART | POUCH | HANDS | ELSE | Trigger | X |
|---|---|---|---|---|---|---|---|
| idle, cocked, rounds < size, spare > 0 | two-hand **done** | - | take a shell: the reload starts **done** | hand-over **done** | game | fires; then the pump waits (below) | never the game's **done** |
| idle, full | two-hand | - | nothing (the game refuses) | hand-over | game | fires | - |
| idle, no spare | two-hand | - | nothing | hand-over | game | fires; empty: the game's click | - |
| after a shot: the Fire clip held at 0.45 s for the hands, one- or two-handed | a stroke on the FORE hold pumps it (6 cm back, 6 cm forward, measured between the controllers **done**; along the barrel axis **⑤**) | - | - | - | - | click **done (step 2)** | - |
| a fresh FORE grab during the wait | settles 0.3 s, then strokes count **done** | | | | | | |
| mid-stroke | the hold lets go only when the grip comes up **done** | | | | | | |
| stroke done | cocked; the next shot; the stroke sounds as parts of the reload-end sound **done** | | | | | | |
| idle, empty (ran dry) | two-hand **done** | - | the game's own reload (start clip, then the first loop) is adopted at the pouch **done** | hand-over | game | click | - |
| waiting at the pouch (a loop) | two-hand **done**: the hand is kept on the gun while held | - | take a shell **done** | hand-over | game | click **done** | - |
| carrying a shell (designed grip) | a NEW hold refused **done** (an existing hold persists in `g_held` but is not drawn: **④** refuse the grab, never drop a hold) | press into the port: guided within 6 cm **done** | - | - | - | click | - |
| let go within 15 cm of the port | pressed in: the clip plays it home **done** | | | | | | |
| let go far | it falls, back to the pouch **done** | | | | | | |
| the loop plays out; the game wants another | the next loop: waiting at the pouch again **done** | | | | | | |
| ending a reload EARLY (shells in, not full) | **gap**: the trigger is refused during the whole reload, so only a weapon or plasmid switch ends it; the game would otherwise cut it short and fire. Proposal (③+⑤): a pump stroke on the FORE hold while the reload waits at the pouch ends the loop and cocks the gun (the game's clip is let run to the end's pump, the stroke drives it) | | | | | | |
| the end clip (full): the last pump | two-hand = the pump: the end clip waits at 0.409 s for the strokes **done** | - | - | - | - | click until pumped **done** | - |
| cut short before its last pump (a switch) | UNCOCKED, per weapon, surviving the switch **done (step 2)** | | | | | | |
| idle, uncocked | two-hand: a dry cock, the pump bone drawn with the strokes **done** | - | a shell: a reload begun with shells in stays uncocked until ITS last pump **done** | hand-over | game | click **done** | - |
| ammunition switch mid-reload | the hands lower (Offscreen), the game restarts the reload for the new type; each restart is adopted; the free hand holds through the start clip **done** | | | | | | |
| weapon / plasmid switch mid-reload | the game aborts; shells already in stay; uncocked if before the last pump **done** | | | | | | |
| mirrored hold (gun in the left hand) | the fore-grip and rest pose reflected **done**; the gun's rest in the hand per hand **done**; the pump measured on the controllers is hand-agnostic **done** | | | | | | |

## 6. The other holdables (no program: the game's own reload)

| Gun | FORE | POUCH / PART | HANDS | Trigger | X |
|---|---|---|---|---|---|
| grenade launcher, chemical thrower, crossbow | two-hand at the table's fore-grip **done** (grip pose **done**) | nothing: the game's reload plays, the free hand goes with its clip (hand-back "at the gun") **done** | hand-over | the game's | the game's reload |
| wrench, research camera | n/a | n/a | hand-over | the game's | the game's |

Giving one of these a manual reload is a program in `reload_rig.h` plus the rows above; the geometry,
state and ownership need no change.

## 7. Ammunition type

| Where type reaches the mechanics | How | Status |
|---|---|---|
| the visible magazine's mesh | the game's `VisualAmmoModel` per type; `ShowAmmoModel` sets it; the spare prop copies the mesh in hand at the time it is taken | done (the spare's mesh is the type held when taken: a spare of AP taken, then a switch to Frozen before it goes in, shows AP while the game loads Frozen: **gap**, cosmetic; proposal: re-mesh the spare on a switch) |
| the dry click's sound | the current ammunition's class name as the event's tag | done |
| a switch (pending differs from current) | one state bit (`switching`); the Tommy and pistol drop the magazine; the shotgun restarts loops | done |
| the drum's / cylinder's / shell's geometry | identical across types (one model per gun's magazine; the types differ by material) | n/a |

## 8. The gaps this enumeration found, and the proposed answers

| # | Gap | Proposal | Step |
|---|---|---|---|
| G1 | During the 0.7 s drop and while a part is being seated by the clip, the machine claims every squeeze; a two-handed grab or a hand-over then is swallowed | the owner table: `Dropping` and `Seating` allow FORE and HANDS (the reload holds nothing of the free hand) | ③ |
| G2 | The shotgun's reload cannot be ended early by the player (the trigger is refused throughout; the game would fire) | a pump stroke while the loop waits at the pouch ends the loop and cocks | ③ ⑤ |
| G3 | The pistol's reload waiting at the pouch has no abort (no X there, no spare prop) | X at the pouch cancels: the gun stays open and empty until the weapon is switched; or let a weapon switch be the abort (as now) | decide |
| G4 | A spare drum rides the LEFT wrist bone; a hand-over while holding it cannot happen (the grip is down), but a plasmid switch destroys it | keep: the spare is destroyed on any switch (one line of log) | none |
| G5 | A spare taken of one type, the type switched before it goes in | re-set the prop's mesh when `switching` flips | ⑥ or later |
| G6 | A shotgun hold "held but not drawn" while a shell is carried (audit item 6) | the owner table refuses the GRAB; a hold already on is drawn until the grip comes up | ④ |
| G7 | The Tommy's reload claims the squeeze absolutely inside the drum's 12 cm (bug 1) | a distance like every other owner; FORE wins a tie within 3 cm on a two-handed gun | ③ |
| G8 | An empty idle pistol or Tommy with no spare rounds: the game's own click, but the magazine-out Tommy asks the game to reload three times then shows the drum | fine as is; the record now logs it | none |

## 9. What the table buys

Adding a gun: a geometry row (two Blender runs), a reload program, and its rows in section 3-6 shape.
Adding a hold rule (say, a grip on the Tommy's drum well to pull it out two-handed): one owner row. Adding
an ammunition behaviour: it is a game fact, read through the state record. The left hand: nothing, ever.
