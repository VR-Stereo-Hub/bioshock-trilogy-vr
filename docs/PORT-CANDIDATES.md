# Port candidates: BioShock 1 behaviours not yet tested on BS2 / Infinite

**What this file is for.** The repo's standing rule is that a BS1 change either
stays in `src/game/bioshock1r/`, or - if it genuinely has to live in
`src/core/` - **defaults to the pre-existing behaviour, with the game that wants
the new behaviour opting in from its own adapter**. That rule is good, and it has
a cost: every such change leaves a switch nobody will ever find again, buried in
one adapter's `init()` and one commit message.

This is the index of those switches. **One row per BS1-proven behaviour that BS2
or Infinite could plausibly want but has never been tested with.** Each row says
the exact line that turns it on, so adopting one after a headset session is a
one-line change rather than an archaeology session.

**Nothing here is a bug list.** These are all deliberate, working defaults. A row
graduates out of this file when the behaviour has been in a headset for that game
and either shipped or been rejected - record which, and why, in that game's
`ENGINE_NOTES.md`.

**Adding a row is part of making the change**, not a follow-up chore. If a commit
adds a core default that only one game opts into, it adds the row too.

---

## How to adopt one

1. Read the "Why it might not transfer" column first. Several of these are BS1
   compensations for BS1-specific defects, and the hard rules are explicit that
   BS2 and Infinite are not bound by BS1's methods - **test whether the defect
   even exists there before porting the fix.**
2. Add the opt-in line to that game's adapter `init()`.
3. Test in a headset. The simulator can prove a value took; it cannot answer
   comfort, scale or judder.
4. Move the row out of this file and into the game's `ENGINE_NOTES.md` with the
   verdict.

---

## Core switches BS1 opts into (BS2 / Infinite keep the old default)

| Behaviour | Opt-in line | Core default | Why it might not transfer |
|---|---|---|---|
| **Anchored cinema screens** | `bvr::vr::set_screen_place_mode(0)` in the adapter's `init()` | recenter-origin placement | Headset-verified on BS1 only. BS2 and Infinite have never been in a headset with anchored placement at all, so the failure modes are unknown. |
| **Smaller anchored screens (1.9 m)** | `bvr::vr::set_screen_width_m(1.9f)` | `2.4f` | Pure taste, and it is coupled to screen placement above - adopt the two together or not at all. |
| **Face buttons straight through** | `bvr::input::set_pad_passthrough_default(true)` | the session-19 rearrangement (`kPadMapBioshock1`) | Infinite has its OWN audited map (`kPadMapInfinite`) and must not take this. BS2 shares BS1's map and has never been in a headset with passthrough. |
| **BRVR control defaults** (d-pad modifier 1, left stick selects, R3 jump) | `bvr::input::set_pad_brvr_defaults(true)` | the pre-s63 legacy heuristic | The modifier choice is HARDWARE-dependent, not game-dependent (see `docs/CONTROLS.md`) - but which stick can be spared while selecting is not. On a game where the left stick is doing something else, flipping it is wrong. |

## Core behaviours that are global today (already affect all three games)

These went into core WITHOUT a per-game gate, so BS2 and Infinite are already
running them. Listed because "already shipped, never tested there" is a risk of
its own, not because anything needs turning on.

| Behaviour | Where | Risk to BS2 / Infinite |
|---|---|---|
| **`JumpOnR3` defaults ON** | `openxr_input.cpp` `g_jumpOnR3{true}` | Was a real leak until 2026-08-22: on Infinite the lane claimed the click, emitted nothing (its map has no `jumpBit`) and blocked the RS-click forward, eating `XToggleZoom`. Now guarded on `map.jumpBit`. **BS2 still gets R3 jump untested** - it has a jump bit, so the lane is live there. |
| **`TurnAxisMax` / turn-curve shaping** | `openxr_input.cpp` | Derived from BS1's turn-rate cliff (0.98 -> ~105 deg/s, 1.00 -> ~200). **BS2 and Infinite have their own sensitivity curves and the cliff may sit elsewhere or not exist** - re-measure before trusting the cap. |
| **`StickPrecomp`** | `openxr_input.cpp`, default on | Undoes the game's PER-AXIS stick deadzone (0.225, from BS1's `User.ini`). **The number is BS1's.** Infinite is a different engine entirely; BS2's bindings need checking. A wrong deadzone here bends the walk direction rather than fixing it. |
| **F10 "D-pad modifier" controls** (added 2026-08-22) | `xinput_bridge.cpp` | The panel now offers the five BRVR modes to every game. BS2 and Infinite default to the legacy heuristic and are unaffected until someone picks a mode - but the control is now reachable, so a mode CAN be selected on an untested game. It logs what it did. |
| **R3+L3 is now TAP = open F10, HOLD = recenter** | `openxr_input.cpp`, the chord block | **A behaviour change BS2 and Infinite never opted into.** All three drain `take_recenter_chord()` (`bioshock2r/camera.cpp:2419`, `bioshockinf/camera.cpp:1551`), and a TAP used to recenter - it now opens the panel instead, with recenter moved to a ~600 ms hold. Nothing becomes unreachable, but the muscle memory changes. Splitting it per game would need a third routing rule for one gesture; revisit only if a tester objects. |
| **`flickHoldBits` + `flickRight`: the HINT direction is emitted, and held** | `openxr_input.cpp`, the pad maps | **Infinite is unchanged** (`flickHoldBits = 0`, its own map). **BS2 shares `kPadMapBioshock1` and takes both changes untested**: it now emits `DPAD_RIGHT` where it previously emitted nothing, and holds it rather than pulsing. On BS1 that is the hint button and the route to the map. **Check what BS2 binds DPAD_RIGHT to before assuming it is the same** - if it is something destructive, BS2 needs its own map rather than sharing BS1's. Everything else stays pulsed on both. |
| **MODIFIER + menu (or X+Y) = BACK instead of START** | `openxr_input.cpp`, the menu lane | All three games. BACK is `ShowContextHelp` on BS1; **check what BS2 and Infinite bind BACK to before assuming this is safe there** - on a game where BACK is "leave the level" this would be destructive. The unmodified menu button still pauses, and the long-press fallback is unchanged. |
| **RT and the right stick are swallowed while the F10 panel is open** | `xinput_bridge.cpp` `compose_synthetic` | Applies to all three games. Trigger = click, right stick = scroll, and only while the panel is visible - the cost is "you cannot shoot or turn with the menu open". The left stick stays live. Harmless in principle; untested on BS2/Infinite. |
| **Aim laser and aim dot are suppressed while the panel is open** | `openxr_runtime.cpp` `build_laser_from` / `build_aim_dot_slot` | All three games. The beam lands on the panel and fights the cursor for the same pixels. Suppression only - the user's `laserOn` setting is untouched. The CROSSHAIR half of this is BS1-only (`bioshock1r/camera.cpp` `assert_crosshair`), because it goes through that game's engine SET handler; BS2/Infinite would each need their own. |

## BS1-only work that BS2 / Infinite would have to reimplement

Not switches - duplicated code, per the "keep the per-game mods decoupled,
duplicate code is fine" directive. Listed so the work is findable.

| Behaviour | BS1 location | Notes for a port |
|---|---|---|
| **Rigid-holdable DrawScale scaling** | `bioshock1r/bones.cpp`, the `ds_*` lane | Headset-confirmed on BS1 2026-08-22: the WEAPON actor's `DrawScale` (+0x2AC) sizes geometry, the RIG actor's does not. **The offset and the dirty protocol are BS1's own** - derive them fresh for BS2, and Infinite is UE3, where the shape itself is suspect. BS2's melee weapons may not even be rigid. |
| **`HandsScale` / `GunScale` / `CameraHeightOffset` ini keys** | `bioshock1r_adapter.cpp` | The reader is ~30 lines of `GetPrivateProfile*`. BS2 and Infinite read their own `BioshockVR.ini` from their own data dirs, so the keys can carry the same names without collision. |
| **Head-bob / landing-dip / damage-shake removal** | `bioshock1r/camera.cpp`, gated on `g_killHeadBob` | Substitutes `Pawn.Location + eyeHeight` for the camera origin. **Depends on BS1's `+0x550` eye-height field being STATIC** (measured: spread 0.00). Re-measure the equivalent field before assuming the same trick works. |
| **The `baseLoc` re-take after the head-bob substitution** | `bioshock1r/camera.cpp` | The s63 gun-bob fix. Any game that adopts the head-bob removal above inherits this bug the moment it does - the viewmodel and the aim ray are built on a snapshot taken BEFORE the substitution. Port the fix in the same change, not after. |

---

## Log

- **2026-08-22** - file created. Seeded from s63's core changes and from the
  adapter opt-ins that predate it. The `JumpOnR3` / Infinite `XToggleZoom`
  interaction was found while writing it, and fixed rather than recorded.
