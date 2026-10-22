# BS1 held-gun mechanics: audit and redesign

Written 2026-10-21 on branch `claude/bs1-manual-reload` at 74d975d, against the 21:12-21:15 headset run
(`%LOCALAPPDATA%\BioshockVR\bioshockvr.log`). Part 1 is an audit of what exists: every piece of state,
every place the mod overrides the game, every latched value and the conditions that make it wrong, the
grip ownership flow, and the open bugs with the log lines that show them. Part 2 is a proposed design.
No code was changed for this document.

Sources read in full: `manual_reload.cpp/.h`, `reload_rig.h`, `two_hand.cpp/.h`, `two_hand_math.h`,
`dial_input.cpp` (the grip arbiter), `sound_engine.cpp/.h`, `dropped_part.cpp/.h`, `handback.cpp`,
the free-hand substitution, gun-in-hand rest, anchor and mirror code in `bones.cpp` (lines 6890-7420,
7950-8710, 10450-10530), `aim.cpp`'s barrel axis (1744-1830), `tools/tests/reload-rig-tests.cpp`,
`tools/tests/two-hand-tests.cpp`, and ENGINE_NOTES 2026-10-20/21 (lines 6585-7307). BRVR's two-handed
grab (`docs/brvr-reference/docs/modules/hands.md`, M6-S2; `docs/INVARIANTS.md` lines 620-640) was read
for parity. BRVR was not edited.

Every finding below says whether it is read from the code, read from the log, or inferred.

---

## Part 1: the audit

### 1.1 The shape of the thing: who runs where

Five modules decide what a hand, a gun part and a squeeze do, on two threads, with no shared record of
the gun's state. Each keeps its own copy of what it needs and resets it on its own schedule.

| Module | Thread | Decides | Keeps its own copy of |
|---|---|---|---|
| `reload_rig::Machine` (pure) | game | the reload's phase, what the free hand carries, whether its grip is claimed | magazine out (`mag_out()`), part carried, chambered |
| `manual_reload.cpp` (the glue) | game, plus `pad()` on XInput | starts/holds/runs clips, drops props, plays sounds, blocks fire, counts dry clicks | `g_needCock`, `g_magOut`, `g_chambered`, `g_blockFire`, three `Pump`s, the seat, the spare |
| `reload_rig::Pump` x3 | game | the shotgun's strokes (between shots, the reload's last pump, the dry cock) | the ratchet reference per instance |
| `two_hand.cpp` + `two_hand_math::Grip` | XInput (grip), game (distance) | whether the free hand holds the gun, the hold's ease | `held_`, the snapshot of distance/slide/paused |
| `dial_input.cpp grab()` | XInput | which grab owns a squeeze: hand-over, two-handed, reload, or the game | `s_owner`, the hand-over's own eligibility |
| `bones.cpp` compose | game | where every hand bone and the gun's sockets are drawn; feeds the three above | eight latches (1.4) |
| `handback.cpp` | game | whether a clip owns a hand (at the gun / in view / recoil) | `g_hand/g_carry/g_gun/g_kick` blends |
| `sound_engine.cpp` | game, plus FMOD's thread | mutes a gun's events, plays them, cuts parts | mute table, live part channels |
| `dropped_part.cpp` | game | spawns and moves the drop and the spare | the capture, the last drop |
| `aim.cpp` | game | the barrel axis per weapon | `g_barrel.axis` |

The order within one compose matters and is a source of bugs on its own (read from `bones.cpp`):

1. `m4_pose` mirrors the pose and latches the gun's sideways axis (7269).
2. The free-hand substitution block (7988-8170): grip pose, designed grip, kept rest, start-clip hold.
   It reads the gun's attach bone 43 RAW here.
3. The shotgun's gun-in-hand rest substitutes 43/44 (8178-8245) AFTER step 2, so the free hand was kept
   relative to the clip's 43 while everything after uses the rest's 43.
4. The gun hand is placed; the two-handed anchor is latched/read; `two_hand::publish` (8446-8543).
5. `manual_reload::tick` runs inside the gun hand's branch (8670), so every `manual_reload::*` query
   the earlier steps made this frame answered with LAST frame's state. `handles_reloads()` and
   `hands_state` were added to paper over exactly this (ENGINE_NOTES 00:43 run).
6. The free hand is placed and `manual_reload::carry` moves the carried part (8672-8685).

### 1.2 State inventory

"Reset" names what clears it. "Writer" is the only writer unless more are listed.

#### `reload_rig.h` - `Machine` (one instance, `g_machine`)

| Member | What | Writer | Readers | Reset |
|---|---|---|---|---|
| `state_` Idle/Starting/Running | the reload | `update` | `active()`, `mag_out()`, `hand_free()` | `reset()`, clip gone, abort |
| `sub_` Wait/Drag/Guided | within a phase | `enter`, phase code | phase code, `hand_free()` | `enter` |
| `prog_`, `phase_` | which program, which phase | `begin`, `next`, Insert drop-back | the glue (sounds, catch, drops) | `begin` |
| `scale_` | actual clip length / recorded | `begin` | `T()` | `begin` |
| `playT_`, `lastNow_`, `lastT_`, `lastProgress_` | the clip time being driven | phase code | phase code | `enter` |
| `carrying_` | a part rides the hand | Fetch, Insert, Carry | `mag_out()`, `hand_free()`, `spare_free()` | `begin` does NOT clear it; `running` on clip end does |
| `carriedFromEntry_` | the shotgun's shell taken at the pouch to start | `idle` | `enter` (Fetch) | `enter` |
| `ejecting_` | the button started this one: eject when the clip starts | `idle` | `starting` | `starting`, timeout |
| `dropping_`, `dropAt_`, `lying_` | the spent part falling, then lying | `start_drop`, `running` | `mag_out()`, `hand_free()`, Fetch | `begin` |
| `snap_` | an Insert reached its latch | Insert, Fetch (caught) | Insert | `begin`, Insert end |
| `skipped_` | a phase was skipped (bolt with a round in) | `enter` | `update` (one log line) | `update` |
| `jumpNext_` | the next hold is SET not played (the carry hold onto the insert) | Fetch/Insert | `from_hold` | `from_hold` |
| `grab_` | where a Slide took hold | Slide, `enter` | Slide | - |
| `grabU_` | the grab radius in rig units | `update` | `enter` | every update |

#### `reload_rig.h` - `Pump` (three instances in the glue: `g_pump` between shots, `g_endPump` the reload's last pump, `g_dryPump` the dry cock)

| Member | What | Reset |
|---|---|---|
| `stage_` Off/Firing/Back/Fwd/Done | the stroke | `fireClip` false, or `clipNew` |
| `ref_`, `refSet_` | the ratchet's reference (the farthest the other way) | `clipNew`, a fresh hold (`rebase_`) |
| `rebase_` | take the reference from the next reading | set by not-held / settling, cleared on read |

Three instances means three ratchets that can be mid-stroke at once; which one the hands drive is
decided by the glue's three `if` blocks (1079-1057, 1354-1388, 1402-1448), not by the gun's state.

#### `manual_reload.cpp` globals (game thread unless noted)

| Name | What | Writer | Readers | Reset |
|---|---|---|---|---|
| `g_on` (atomic) | F10 Manual reload | `menu_write` | everything | - |
| `g_machine`, `g_cfg` | the machine, its radii | - | - | `reset()` on weapon change / no weapon / off |
| `g_pump`, `g_pumpCfg` | the shot pump (waitT 0.45, waitForHands) | `tick` | `pumping()` | `fireClip` false |
| `g_endPump`, `g_endPumpDone`, `g_endPumpWas` | the reload's last pump | `tick` | `pumping()`, `Input::pumpDone` | an `off` update when `pumpWait` drops |
| `g_dryPump` | the dry cock | `tick` | `tick` | an `off` update when `dry` drops |
| `g_needCock` | the shotgun loaded from empty, not yet pumped | `tick` (set at a reload's start when `!g_chambered`; cleared by either pump's `run`) | `g_blockFire`, `dry` | not the shotgun; NEVER on a weapon change (survives a switch, which is intended) |
| `g_blockFire` (atomic) | the trigger reaches nothing | `tick` | `pad()` (XInput) | no weapon |
| `g_handsReload`, `g_handles`, `g_ejectable`, `g_sticky`, `g_handFree`, `g_active`, `g_pumpWait`, `g_pumping` (atomics) | game -> XInput / bones flags | `tick` | `pad()`, `dial_input`, `bones.cpp`, `two_hand.cpp`, `handback.cpp` | no weapon (`g_pumpWait`/`g_pumping` only by the next tick) |
| `g_dryClick` | a pump refused a pull this tick | `tick` | `tick` (dry fire) | each tick |
| `g_fireClipMs` (atomic) | the Fire clip last seen | `tick` | `pumping()` (400 ms grace) | never |
| `g_gripDown`, `g_gripPresses` (atomic) | the free grip from XInput | `pad()` | `tick` | - |
| `g_claim`, `g_reach`, `g_claimMs` (atomic) | the reload's claim and reach for the arbiter | `tick` | `pad()`, `reach()` | stale after 250 ms |
| `g_reloadButtonMs` | the last X the GAME saw | `pad()` (XInput) | `in.adopt` (a reload within 1.5 s of it is the button's) | never |
| `g_ejectReq` (atomic), `g_xDown`, `g_suppressUntilUp` | X handling | `pad()` | `tick` | - |
| `g_triggerPulls` (atomic), `g_triggerWas`, `g_seenPulls`, `s_dryPulls` | the trigger's rising edges, counted twice | `pad()` | the pump, the dry fire | never |
| `g_gunKeep` | how much of the gun hand's own clip motion is layered | `tick` | `bones.cpp` | 1.0 with no weapon |
| `g_weapon`, `g_cls` | the gun in hand, its class | `tick` | everything | weapon change |
| `g_lastHandle`, `g_lenName`, `g_len` | the clip handle / name / length seen | `tick` | `in.clipNew` | weapon change (handle only) |
| `g_paused`, `g_heldW`, `g_heldH`, `g_chasing`, `g_rateW`, `g_rateH` | what `hold()`/`run()` did to the clips | `hold`, `run`, `let_go` | same | `let_go` |
| `g_can`, `g_canMs`, `g_spareAmmo` | can_reload's 250 ms cache | `can_reload` | `in.canReload`, `in.spareOk` | weapon change |
| `g_switchMs` | the last ammunition switch seen | `tick` | `afterSwitch` (1.5 s) | never |
| `g_carryMode`, `g_carryPart`, `g_carryAtPalm`, `g_carryWeapon` | what the free hand carries | `apply_parts` | `carry()`, `carry_grip()`, `two_hand_allowed()` | each tick |
| `g_havokCopy`, `g_copyRounds` | the eject's copy | `tick` | `apply_parts` | machine idle |
| `g_handLock`, `g_lockW`, `g_lockMs` | the bolt lock, eased | `tick`, `hand_lock` | `bones.cpp` | - |
| `g_seatValid`, `g_seatBone`, `g_seatP` | THE LATCH: the spent part's seat (weapon space) | `tick` (from the rest seat, or 0.06 s into the fall) | `carry()`, `catch_check` | machine idle, each eject |
| `g_restSeatGun`, `g_restSeatBone`, `g_restSeatP`, `g_restSeatMs` | the Tommy drum's seat read at idle every 0.5 s | `tick` | `tick` | never (keyed by weapon pointer) |
| `g_partSeatCm`, `g_partSeatMs` | the carried part's distance from the seat (150 ms fresh) | `carry()` | `in.partSeatCm` | - |
| `g_uuPerCm` | - | `tick` | `carry()` | - |
| `g_fallInit`, `g_fallStart`, `g_floorZ` | the gun's own part falling | `tick`, `apply_parts` | `apply_parts` | each eject |
| `g_chambered` | the gun had rounds as the reload began | `tick` (every idle frame via a script call) | `in.chambered`, `g_needCock` | - |
| `g_spare` | the spare drum prop | `tick` | `carry_grip`, `place_spare`, `in.spareInHand` | destroyed on no weapon / wrong gun / dead |
| `g_amRelValid`, `g_amRelP/Q`, `g_amGunS`, `g_amRelMs` | the AmmoModel against its bone, measured idle every 0.5 s | `measure_ammo_model` | `place_spare`, `catch_check` | never (not per weapon!) |
| `g_flying[4]` | thrown drums, 4 s catchable | `tick` | `catch_check` | age, dead |
| `g_throw[]`, `g_throwN`, `g_throwAt`, `g_throwFedUs` | the throw history | `throw_feed` | `throw_velocity` | 100 ms gap |
| `g_ejectedThis`, `g_showAmmoFor` | show the magazine once the reload is over | `tick`, `reload_left` | `tick` | machine idle |
| `g_magOut`, `g_magOutAskMs`, `g_magOutHideMs`, `g_magOutAsks` | the Tommy left without a drum | `reload_left`, `tick` | `in.magOut`, `tommyDry`, `noDrum`, the seat read | a new drum, or 3 refused reloads |
| `g_watch`, `g_watchMs`, `g_starts/done/aborts` | diagnostics | - | - | - |
| statics: `s_flips/s_ms` (weapon_space), `s_lastUs` (place_spare), `s_was/s_since` (held_seconds), `s_fireHandle`, `s_strokeMs`, `s_switchSeen`, `s_dryWas`, `s_cfg` | - | - | - | - |

#### `two_hand.cpp` + `two_hand_math.h`

| Name | Thread | What | Reset |
|---|---|---|---|
| `g_on`, `g_grabCm`, `g_shots`, `g_shotAmp` | any | F10 | - |
| `g_snap` + seqlock | game -> XInput | distCm, slideCm, paused, ms | stale after 250 ms |
| `g_grip` (`Grip::held_`, `gripWas_`) | XInput | the hold | the grip coming up only: line 192 forces `slideCm` 0 while held, and `distCm` is not consulted while held |
| `g_suppressUntilUp`, `g_lastBuzzMs`, `g_lastProbeMs` | XInput | bumper withheld, buzz, probe | grip up |
| `g_held` (atomic), `g_takeHeld`, `g_grabs`, `g_liveDistCm` | both | - | - |
| `g_classOf`, `g_classTwoHanded` | game | class cache per holdable | holdable change |
| `g_weight`, `g_weightHeld`, `g_weightMs`, `g_weightFrom` | game | the hold's ease (120 in / 150 out) | - |
| `g_kick`, `melee()` `s_of/s_melee`, `shot()` `s_kicks/s_both/s_logMs` | game | the kick | - |

Note `two_hand.cpp:192`: with the grip held, `slideCm` is forced to 0 for EVERY gun, so the release
radius (20 cm) and the slid-off path in `Grip` are dead code; only the grip coming up lets go. But the
hold's EFFECT is gated elsewhere: the gun hand's branch publishes nothing and applies no aim turn
whenever `two_hand_allowed()` is false (bones.cpp 8446), and the free hand is carried onto the gun only
at `twoHold`, which is 0 while `paused`. So a hold can be "held" in `g_held` while the gun stops turning
to the hand and the hand goes back to its controller - two different answers to "are two hands on the
gun" (item 6 in 1.6).

#### `dial_input.cpp grab()` (XInput)

| Name | What | Reset |
|---|---|---|
| `g_grabOn`, `g_grabCm` | F10 | - |
| `s_owner` (kNone/kHandOver/kTwoHand/kReload), `s_freeWas` | the owner of the current squeeze | grip up |
| `g_grabPhys`, `g_eligible`, `g_physGripWas[2]` | the hand-over's own grab | grip up |
| `g_handovers`, `g_liveDistCm`, `g_lastBuzzMs`, `g_lastProbeMs` | - | - |

#### `bones.cpp` latches (game thread)

| Name | Lines | What | Latched when | Reset |
|---|---|---|---|---|
| `s_restWeapon/s_restAxis/s_restU` | 7274 | the gun's sideways axis read at rest | `Idling` (any idle: ability idle too), dot > 0.6, once per weapon | weapon change |
| `s_axisWeapon/s_haveU/s_provisional/s_u/s_uAxis` | 7315 | the axis actually flipped (provisional until rest) | first mirrored frame; re-taken at rest | weapon change |
| `g_mirU`, `g_mirPlane` | 6900 | the flip axis and centre plane this frame | every MIRRORED frame only (`m4_pose` returns at 7297 before writing them when the mirror is off) | never: stale with the mirror off |
| `rigid_centre` `s_actor/s_n/s_sum` | 7199 | a rigid holdable's centre, 16-frame average | first 16 frames per actor | actor change |
| `s_freeRel[]`, `s_freeRelValid` | 7989 | the free hand's rest on the gun, relative to bone 43 | `!active && Idling && !offRest && (held || !valid)` - the FIRST idle frame seen, any idle | gun change, mirror change |
| `s_fingerRel[]`, `s_fingerRelValid`, `s_wristRel`, `s_palmLocal`, `s_fingerGun`, `s_restMirror` | 8028 | the grip pose: fingers and the wrist's turn against bone 43 | `!active && held && Idling`, every such frame | gun change, mirror change |
| `s_lastRel[]`, `s_lastRelValid` | 8151 | the free hand last frame, against bone 43 (for the start clip) | every frame not held through a start clip | never |
| `s_sub[]`, `s_left[]`, `s_gsub[]` | - | scratch poses | - | - |
| `GunRest s_rests[4]` (`rel[2][2]`, `ok[2]`), `s_idleSince`, `s_idleGun` | 8183 | the shotgun's sockets 43/44 against the gun hand's wrist, per gun per hand | `Idling` settled 0.5 s, not `EmptyFidget` (first); then every idle frame within 3 deg | ring of 4 guns |
| `s_gunRest[2]`, `s_gunRestValid[2]` | 8266 | the gun hand's wrist before a clip's own motion (recoil) | every frame no hand-back touches it | - |
| `s_ancRelBy[2]`, `s_ancOkBy[2]`, `s_ancGun` | 8459 | THE TWO-HANDED ANCHOR: the free palm against bone 43, per hand | `Idling && !EmptyFidget && (held || !ok)`: the first idle seen, then EVERY idle frame while held | holdable change |
| `s_twoTurnLocal[2]`, `s_twoTurnW[2]` | 8270 | the two-handed turn (no longer read by a reload) | - | - |
| `s_joinMinCm`, `s_carrying`, `s_lock`, `s_told/s_toldRatio`, `s_shots/s_firing` | - | diagnostics and eases | - | - |
| `hands_clip_is` `s_handle/s_name` | 6948 | the Hands actor's clip name per handle | handle change | - |
| `g_wPartOn/Off/Q/Pose/Scale[]`, `g_wPartAny` | 963 | the weapon parts the reload moves | `set_weapon_part_*` | `clear_weapon_part_offsets` each tick, bind |
| `g_wAnim[]`, `g_wRef[]`, `g_wWritten[]`, `g_wWrittenScale`, `g_wDrawnScale` | 944-969 | the weapon skeleton lane | `wskel_compose` | bind/release |

#### `handback.cpp`, `sound_engine.cpp`, `dropped_part.cpp`, `aim.cpp`

| Name | What | Reset |
|---|---|---|
| `handback` `g_hand/g_carry/g_gun/g_kick[2]` (Handoff blends), `g_mask`, `g_atGun`, `g_last`, `g_attackTrigger`, `g_lastPulseMs` | which hand a clip owns, eased | per tick |
| `handback` levers `g_melee/g_swing/g_scripted/g_fire/g_reload/g_gunMotion/g_joinCm/g_recoil/g_force` | `vrhands handback ...` | - |
| `sound_engine` `g_orig`, `g_target`, `g_tried` | the TriggerEffectEvent hook | once |
| `sound_engine` `g_playOrig`, `g_ch*`, `g_snd*` | the FMOD hook | once |
| `sound_engine` `g_hands`, `g_weapon`, `g_cls`, `g_muteOn` | the context (every tick) | each tick |
| `sound_engine` `kMuted[]` | Shotgun PumpShotgun, ShotgunReloadEnd; MachineGun ReloadTommy | constant |
| `sound_engine` `g_live[]` (mutex), `t_want`, `t_caught`, `t_event`, `t_playing` | parts playing, per-thread markers | part end |
| `dropped_part` `g_cap`, `g_lastRounds`, `g_lastDrop` | the capture and the last drop | `capture`, `drop` |
| `aim` `g_barrel.axis/axisDot/dirValid/local[]` | the barrel axis per weapon (chosen once, dot >= 0.9) | weapon change |

### 1.3 Every place the mod overrides the game

| What the game would do | Where the mod intervenes | Condition | Thread |
|---|---|---|---|
| X = reload (XENON_X) | `manual_reload::pad` strips X | `g_handsReload` (the gun has a program) and fresh | XInput |
| X = reload | ...and turns a press into an eject request | `g_ejectable` (pistol, Tommy) | XInput |
| LB = the free hand's bumper (plasmid wheel) | `manual_reload::pad` strips LB | the reload's claim, or a squeeze it took until it comes up | XInput |
| LB | `two_hand::pad` strips LB | eligible (within 12 cm) or held, or a squeeze it took | XInput |
| LB / RB | `dial_input::grab` strips the grabbing hand's bumper | the hand-over's eligibility (hands within 12 cm) | XInput |
| RT = fire | `manual_reload::pad` zeroes RT | `g_blockFire`: shotgun reloading or needs cocking; Tommy with no drum | XInput |
| the reload clip plays through | `hold()`: pause both clips (weapon + Hands actor's, every frame) and set their time | the machine's `Output::hold` | game |
| the clip's rate | `chase_rate`: 1-4x to reach a forward time by playing | `Output::chase` | game |
| the clip's time | `run(jump)`: set then resume | `Output::run/jump`; `Finish` | game |
| the Fire clip's pump | `Pump`: hold at 0.45 s, follow the strokes, run at the catch | shotgun, `Firing/PostFiring` | game |
| the end clip's pump | `g_endPump` the same over 0.409-0.676 | `PumpHand` phase | game |
| the gun hand's clip motion | `gun_motion_keep()`: 0.5 x the recoil to 0.45 s for the shotgun; 0.30-0.45 s of a Fire clip with rounds for the pistol/Tommy; 0 for every other clip of a gun with a program | always while a program exists | game |
| a clip owns the hands (hand-back) | `handback`: a reload is the hands' from its first frame (`handles_reloads`) | `Reloading` | game |
| the free hand's clip pose | bones.cpp substitution: grip-pose fingers + wrist turn; designed grip; kept rest; start-clip hold | held / carrying / keep / start clip | game |
| sockets 43/44 (the drawn gun) in the clip | bones.cpp 8178: put back against the wrist as at idle | shotgun, every state but Idling (re-latch) and Firing | game |
| the weapon's part bones | `set_weapon_part_offset/pose/scale` through `wskel_compose` | carry, fall, the dry cock's pump travel | game |
| effect events (sound cues) | `sound_engine::detour` drops muted events | `g_muteOn` (= a weapon is in hand and the feature is on) and the class/event is in `kMuted`; NOT "the mod is driving this clip" | game |
| effect events | `sound_engine::play`: fires an event itself | strokes, the Tommy's parts, DryFiring | game |
| FMOD playSound | `play_detour`: the first channel inside a `play_part` is cut to a window and stopped by `tick()` | `t_want` set | FMOD caller |
| `Reload` (script) | `script::call(weapon, "Reload")` | `Act::StartReload`; the mag-out re-ask every 1.5 s (3 tries) | game |
| `ShowAmmoModel` / `HideAmmoModel` | called by name | on a fetch, on seating (ejecting programs), when a reload is over, mag-out hide every 300 ms | game |
| the magazine actor | `dropped_part`: a spawned PhysicalReactiveActor as a drop or a held spare; fields written each frame | eject, spare take/let go/catch | game |
| the ammunition switch | observed only (Pending != Current): drops the drum, marks `afterSwitch` 1.5 s | Tommy | game |
| haptics | pulses for grab/tick/seat/deny/drop/buzz/kick | everywhere | both |

Numbers of each kind: 3 pad bits and 1 trigger; 4 clip-time operations on up to 3 handles; 2 blend
lanes for motion; 3 bone-substitution layers plus 1 part layer; 2 event hooks; 3 script calls; 1
spawned actor family.

### 1.4 Latched and remembered values, and what makes each wrong

Each row: what is latched, from what frame, and the known or inferable way it goes wrong. R = read
from code, L = seen in a log, I = inferred.

| Latch | Taken from | Goes wrong when |
|---|---|---|
| two-handed anchor `s_ancRel` (per hand) | the clip's free palm, first idle frame per gun; re-latched EVERY idle frame while held (R) | (a) the first idle after an equip is the equip swing's last frame (R, the same trap ENGINE_NOTES 20:54 names for the gun rest, which got a 0.5 s settle; the anchor did not); (b) while held at idle the palm read is the SUBSTITUTED one (grip pose; kept rest), so the anchor becomes whatever the rest was (R); (c) when a shot's clip ends in `Idling` with the pump still returning (26-35 units off for 0.1 s, ENGINE_NOTES 21:07), a held hand re-latches from those frames - `pumping()`'s 400 ms grace covers the slide-off test, not this latch (R; not caught in this log: its pumps ran straight into a reload); (d) a hand with no latch takes the other hand's reflected with `g_mirU/g_mirPlane`, which with the mirror OFF are stale from the last mirrored frame (R) |
| free hand's rest on the gun `s_freeRel` | the FIRST idle frame seen after a reset; reset on a gun change AND on a mirror change (a hand-over) (R) | (a) after a hand-over mid-reload nothing re-latches it until the next idle, so the kept-on-gun substitution has nothing to keep and the two-handed carry draws the free hand where the CLIP has it - at the pouch, under the gun hand (R; L: bug 3); (b) the first `WeaponIdling` frame after an equip is the equip's end pose (no settle rule here, unlike `s_gunRel`), `offRest` then measures the shotgun's clip against that rest, `keep` draws the hand there while held, and the anchor re-latch (above, b) copies it (R; L: 21:15:15.627 then the hold at 21:15:18 - bug 5). (`AbilityIdling` also counts as idle, but the holdable is null in ability mode so that latch is reset again on the re-equip.) |
| grip pose `s_fingerRel`, `s_wristRel` | every idle frame while held | the same post-pump frames as (c): a half-open hand becomes the grip pose until the next idle hold (I) |
| shotgun gun-in-hand rest `s_gunRel` (per gun per hand) | a settled idle (0.5 s, not EmptyFidget), then every idle frame within 3 deg | after a plasmid mid-reload and a re-equip the settle rule now holds (ENGINE_NOTES 20:54); still re-latched while the free hand is substituted, consistent only by luck of ordering (1.1 step 3) |
| the gun's sideways axis `s_restU` | any `Idling` frame with dot > 0.6, once per weapon | `AbilityIdling` counts as rest; a one-handed idle of the shotgun has the gun turned; both are "rest" to it (R) |
| the mirror plane `g_mirPlane` | the median of the weapon skeleton's bone coordinates along u (constant per gun) or render bounds | correct in principle; but it is only WRITTEN on mirrored frames, so the reflection of the anchor to the unmirrored hand uses the last mirrored gun's values (R) |
| the Tommy drum's seat `g_restSeatP` | TG_AmmoClip's pose at idle every 0.5 s | fine in itself; but `g_seatValid` is cleared at every eject and on idle, and the spent part's own seat read 0.06 s into a fall gives the pistol a seat only when it ejects (R) |
| the AmmoModel against its bone `g_amRel` | idle, steady, every 0.5 s | NOT per weapon: the last gun's value is used for a new Tommy until its first steady idle (R) |
| `g_chambered` | a script call every idle frame | the value at the reload's start is right; the per-frame script call is a cost for nothing |
| `g_needCock` | set when a shotgun reload begins with no rounds | never cleared on a weapon change (intended), never cleared if the game refills the gun by a path the mod did not see (I) |
| the barrel axis `g_barrel.axis` | the first frame the gun lies within 0.9 of the aim ray | fixed for the equip-swing case (23:21 run); still one-shot per weapon |
| `Pump::ref_` | the controllers' distance | the gun hand's motion and any sideways motion read as a stroke (1.6, bug 4) |
| `g_reloadButtonMs` | the last X the GAME saw | with X stripped for every gun with a program it is never updated, so every reload is "adopted" - harmless now, but `in.adopt` is a dead distinction |
| `held_seconds()` | one static for all three pumps | a fresh hold's settle is shared; fine |

### 1.5 Grip ownership: from a squeeze to an owner

The squeeze of the free hand's grip (always LB after the role mapping) is decided ONCE on its rising
edge in `dial_input::grab` (XInput): each grab reports its reach as distance over its radius, and the
smallest reach <= 1 wins; nothing <= 1 means the game gets the bumper. The owner is held until the grip
comes up, with one hand-on: a reload that ends under the squeeze passes it to the two-handed grip if the
hand is within reach (`take_held_grip`).

The reaches (read from code):

| Grab | Reach | Note |
|---|---|---|
| hand-over | controllers' distance / 12 cm | only when no wheel/overlay is up |
| two-handed | `distCm` / 12 cm; 0 while held; -1 when paused, stale, or no grab point | the grab point is the latched anchor (1.4) |
| manual reload, idle machine | `Output::reach`: Pull start distance / 12 cm; pouch zone 0.5; spare zone 0.25; -1 none | |
| manual reload, active machine | 0 if the machine claims this frame, else -1 | NOT a distance: an active reload's claim always wins while the hand is within the part's reach (Pull/Slide) or at the pouch (Fetch) |

So "nearest wins" is only true between the three idle grabs. With a reload running the question is
"does the reload claim the hand this frame", decided inside the machine per phase:

| Machine phase | Claims the squeeze when | The squeeze is free (two-handed / hand-over) when |
|---|---|---|
| Pull / Slide, waiting | the hand is within 12 cm (x grabScale) of the path start | elsewhere (`hand_free`) |
| Pull / Slide, dragging | always | - |
| Carry | always (the part is in the hand) | - |
| Fetch | at the pouch, or a spare in hand | away from the pouch |
| Insert | always | - |
| Flick / Play / Finish | Flick, Play: always (the default at line 670); Finish: never | Finish |
| PumpHand | never | always (the pump IS the two-handed grip) |
| dropping (eject) | always | - |
| idle, spare in hand | always | - |

And what each owner does with the hand, per gun state. "2H" is the two-handed grip, "HO" the hand-over.
The gun state column is the one every module would read in the design; today it is spread over
`mag_out()`, `g_magOut`, `g_needCock`, `g_chambered`, `can_reload` and the hands state.

| Gun state | Pistol | Tommy Gun | Shotgun |
|---|---|---|---|
| loaded, idle | HO near the other hand; reload at the fold path start; game elsewhere | HO; 2H at the latched fore-grip; reload at the drum (its path starts about 20 units behind and 12 below the fore-grip, so a hand reaching under the fore-grip is inside both reaches and the arbiter picks the nearer); spare at the pouch (0.25) | HO; 2H at the latched pump; shell from the pouch (0.5) |
| empty (ran dry), the game's reload adopted | held at the fold path start (Pull wait): the reload claims the hand near it, 2H/HO elsewhere; no click from anyone until the hand pulls (bug 2 for the pistol too) | held at the drum (Pull wait, no eject unless a switch): the reload's claim is reach 0, so inside the drum's 12 cm the 2H grip can never win, however much nearer the fore-grip is (bug 1) | the shotgun's own reload starts at the pouch (Fetch): 2H allowed anywhere, HO allowed; trigger dead |
| magazine out (dropped / carried away) | Fetch: pouch claims; 2H not applicable; HO allowed | Fetch: pouch claims; 2H allowed (hand_free); HO allowed; a click per pull from the mod | n/a (shells) |
| reloading, part in hand | the reload owns the hand | the reload owns the hand | the reload owns the hand; 2H refused while a shell is carried |
| reloading, last pump | n/a | n/a | 2H owns the hand; the end pump drives the clip |
| needs cocking (reload cut short) | n/a | n/a | idle: 2H at the pump drives `g_dryPump`; trigger dead; the click plays per pull |
| left hand (mirrored) | the same, with the anchor reflected if that hand has none | the same | the same, plus the per-hand gun rest |

### 1.6 The open bugs from the 2026-10-21 runs

**1. The empty Tommy's grip goes to the drum well instead of the fore-grip.** Read from code: a dry
Tommy's adopted reload waits at Pull phase 0 (`ejectAdopted` is false unless an ammunition switch is
near), where `claim = near` with `near_phase` over 12 cm of the drum path's first point; the glue turns
an active claim into reach 0 (`g_reach = o.active ? (o.claim ? 0 : -1) : o.reach`), which beats any
two-handed reach. The drum path's first point (19.3, 3.3, -11.8) sits about 20 rig units behind and 12
below the fore-grip, so a hand reaching for the fore-grip from below or behind is inside the drum's
reach, and then the reload always wins, not the nearer grab. With the machine idle and the drum out
(`g_magOut`) the same squeeze IS nearest-wins, so the behaviour differs by which path emptied the gun.
Earlier the free hand's rest latch moved the two-handed target to the pouch in the same state
(ENGINE_NOTES 20:35), fixed by latching the anchor at rest; the reach rule remains. This run's log has
no clean instance (its reload-0.00 decisions at 21:13:45 and 21:13:52 are at the pouch); the mechanism
is read from the code.

**2. The Tommy's dry fire clicks once.** Read from code and log. The game's own `DryFiring` fires from
`PlayerWeapon` on an empty pull outside the reload state: the first empty pull both clicks and starts the
game's reload (21:13:18.055), after which the game is in `WeaponReloading` and pulls reach nothing. The
mod's click (`tommyDry`) covers only `g_magOut` or `mag_out()`, and `mag_out()` is false at Pull phase 0
(`phase_ > 0 && phase_ < ff` fails), so while the dry gun waits for the hand to pull the drum nobody
clicks. The log confirms clicks only once the machine reaches the Fetch (21:13:19.188 onward, 21:13:41,
after an eject or a switch). The pistol has the same gap and no mod click at all.

**3. The shotgun's grip after a hand-over lands under the other hand.** Read from code and log. The gun
changed hands at 21:14:57.6 while its reload waited at the pouch (`Reloading` since 21:14:18). A mirror
change resets `s_freeRelValid` and `s_fingerRelValid` (bones.cpp 8040-8045), and both re-latch only at
an idle, which never came: the next grabs (21:14:58.7, 21:15:00.8) were taken with `keep` true and
nothing to keep, so the two-handed carry (`carried_on_gun`, 8588) drew the free hand where the reload
clip has it - at the pouch, below the gun hand. The grab itself was 9.3 / 8.8 cm from the anchor,
10 cm back along the gun (`fwd -10.1 right +1.1 up +0.4`); that part is the anchor (1.4, anchor a-d;
the exact latch frame is not in this log). The 20:36 recording's upside-down shotgun was a different
latch (the per-hand gun rest, since fixed). Mirrored or not, the free hand's place on the gun must not
depend on a latch that a hand-over clears.

**4. A half pump goes back, then sticks forward.** Read from the log (21:14:16.647-21:14:17.802): the
line "back stroke 2.7 cm of 6" is printed at the transition from Firing to Back, which happens once the
hands are 2.5 cm closer than their farthest (`earlyCm`), not at completion; the stroke completed 43 ms
later (6 cm in 43 ms, 1.4 m/s). The controllers then went on closing from 23.9 to 13.9 cm apart (`[grab]
hands N cm apart`) while the drawn hand slid 20 cm forward along the gun: the ratchet's forward reference
chased the closest distance, so "forward stroke 1.0 cm" held until the hands opened 6 cm from 13.9 and
completed at 21:14:17.8. Cause: the stroke is the DISTANCE BETWEEN THE CONTROLLERS, which conflates the
gun hand's motion and any motion not along the gun with the pump; the fore-end's travel along the
gun's axis from where the hold took it is what a pump is.

**5. No cocking after a reload: the grip measured ten grab radii off.** Read from the log: the end clip
began at 21:15:34.28 (`PumpHand`, the squeeze free for the two-handed grip); the squeeze at 21:15:35.27
read two-handed 9.92 radii (119 cm) and went to the game, which opened the wheel; the lowering ended the
reload. The chain, read from the code with the log's timestamps: the plasmid switch at 21:15:14.7 ran
`AbilityIdling` then `WeaponEquipping`, and the first `WeaponIdling` frame (21:15:15.627) latched
`s_freeRel` from the equip's end pose (1.4, `s_freeRel` b). The anchor was still the old good one at
21:15:18.0 (8.4 cm; a grab at 5.9 cm). Held at idle from 21:15:18.3, the shotgun's `offRest` compared
the clip's wrist with that rest, `keep` drew the free hand AT the rest, and the anchor was re-latched
every frame from the substituted palm (1.4, anchor b) - the equip's end pose, far from the pump. Let go
at 21:15:22.7, the anchor stayed there. The two squeezes before 21:15:35, at the pouch, read 10.2 and
10.6 radii: a point 120 cm from both the pouch and the pump is nowhere near the gun, which fits. The
equip-end pose itself is inferred, not recorded; a pose recording over a plasmid switch would show it.

**Also found, not in the tester's list:**

6. "Two hands on the gun" has two answers: `two_hand::held()` (the grip's own state, XInput) and the
   compose's `twoHold`/aim turn, which are 0 whenever `two_hand_allowed()` is false or a clip is
   `paused` (bones.cpp 8446, 8517). While a shell is carried on the shotgun, or a reload runs with the
   grip not yet held, a hold can be held and invisible at once: the gun stops turning to the hand and
   the hand returns to its controller, with no log line.
7. `sound_engine` mutes whenever the feature is on and the gun's class is in `kMuted`, not when the mod
   is driving a reload or a pump, so the game's own cue is muted even for a reload the game finishes
   itself (a plasmid-interrupted one, 21:15:35.9: the end clip's pump ran with its sound muted and no
   stroke of ours to play it).
8. `g_amRel` (the AmmoModel offset) and `g_restSeat*` are keyed loosely: the first per gun pointer, the
   second not at all.
9. `g_chambered` makes a script call every idle frame on every gun with a program.
10. `in.adopt` is dead: X never reaches the game on a gun with a program, so `g_reloadButtonMs` is
    never updated and every reload is "adopted".
11. The bolt Slide's `claim = near` only while waiting means a two-handed squeeze at the fore-grip during
    the Tommy's bolt phase goes to the two-handed grip, after which the bolt cannot be taken without
    letting go (by design, but undocumented).
12. The shotgun's `two_hand_allowed()` is false while a shell is carried, which through item 6 makes
    an existing hold invisible rather than refusing a new grab. The rule is backwards: a grab should be
    refused by the owner table, and a hold, once taken, should be drawn until the grip comes up.
13. Three `Pump` instances with three separate `if` blocks: the end pump and the dry pump cannot both be
    live, but nothing says so; the dry pump's `pi.clipT = 2.0` fakes a clip.
14. The free-hand substitution and the anchor use bone 43 from different stages of the compose (1.1).

### 1.7 What the F10 overlay already has (RULE 3)

Manual reload on/off (`ManualReload`); the two-handed grip on/off and its distance (`TwoHandEnabled`,
`TwoHandDistance`); gunshot haptics and strength; the hand-over grab on/off and its distance
(`GrabEnabled`, `GrabDistance`); mirrored hands (`GrabMirror`). Console levers: `vrhands handback`
(recoil, reload place, gun motion, join), `vrreload` (status, probe, watch, hold/release, give),
`vraim barrel` (origin, aim, axis source), `vrbones mirrorpin`. The design below adds no F10 control;
the pump's stroke length and the grab radius stay where they are.

---

## Part 2: the proposed design

The six changes the tester named, in the order they should land. Each one removes a class of latches or
copies rather than adding a rule. Nothing here changes `src/core/`.

### 2.1 One gun-state record

One struct per held gun, owned by the glue, written by exactly one function per field, read by every
module through `const` access. It replaces `mag_out()`, `g_magOut`, `g_needCock`, `g_chambered`,
`g_blockFire`, `can_reload`'s cache, `g_spare.actor != nullptr`, and the hands-state checks sprinkled
through bones.cpp.

```
struct GunState {
    void* weapon; GunKind kind;              // Pistol, Tommy, Shotgun, Other
    int rounds, magazineSize, spareRounds;   // read from the game, rate-limited, one place
    Magazine magazine;                       // In, Out (dropped), InHand (carried), Spare (a spare prop in the free hand)
    bool cocked;                             // shotgun: false from a reload begun empty until a stroke completes
    void* ammoClass;                         // Current; pending != current => switching
    ReloadStage stage;                       // None, Waiting(phase), Carrying(part), Seating, Pumping, Finishing
    Hold hold;                               // which physical hand holds the gun; mirrored or not
    bool twoHanded;                          // the free hand holds the fore-end
    PumpStage pump;                          // Off, Wait, Back, Forward (one pump, not three)
};
```

Rules: the record is updated at the top of the compose, before any module runs; the machine's `Output`
and the game reads feed it; nobody else writes it. `fireAllowed()`, `dryClick()`, `twoHandAllowed()`
are functions OF the record, not flags. The dry click becomes one rule: a trigger rise with
`fireAllowed()` false plays `DryFiring` - which closes bug 2 for every gun and every stage, including
the pistol.

### 2.2 One ownership table

Replace the arbiter's three reaches plus the machine's per-phase `claim` with one table keyed by
(gun kind, stage, magazine, cocked) that names, for each of {free hand, gun hand} x {grip squeeze,
trigger} and for each gun part {fore-grip, magazine part, bolt/pump, shell port, pouch}, its owner:
`Game`, `HandOver`, `TwoHand`, `Reload(part)`, `Spare`, `None`. The arbiter asks the table, then applies
distance only among the owners the table allows, with the reload's claim expressed as a DISTANCE to its
part like the others (the "0 = always" rule goes).

A sketch of the Tommy's rows, the ones that matter for bugs 1 and 2:

| stage / magazine | fore-grip squeeze | drum squeeze | pouch squeeze | trigger |
|---|---|---|---|---|
| None / In | TwoHand | Reload(pull) if rounds < size, else TwoHand | Spare if spareRounds > rounds | Game |
| Waiting(pull) / In (dry) | TwoHand | Reload(pull) | Spare | dry click |
| Waiting(fetch) / Out | TwoHand | None | Reload(fetch) | dry click |
| Carrying / InHand | TwoHand (refuse grab, keep a hold) | Reload | Reload | dry click |
| Waiting(bolt) / In | TwoHand | Reload(bolt) | Spare | dry click |

The distance tie between the fore-grip and the drum (bug 1) is resolved by GEOMETRY, not by reach: the
drum's grab zone is the drum well's own volume (a radius around the magazine seat, 2.3), the fore-grip's
is the fore-grip point; a hand nearer the fore-grip gets the fore-grip. Where both are within reach and
the distances are within 3 cm of each other, the fore-grip wins on a two-handed gun (the tester's rule:
you can always grip).

The hand-over stays as it is: hands together with no other owner in reach.

### 2.3 Gun geometry from the skeleton, once per gun

Every point the mechanics need is a constant of the gun, in the grip bone's frame (bone 43), computed
once when the weapon skeleton lane binds (`wskel` bind, which already copies the rest bank `g_wRef`) and
mirrored for the left hand by ONE function. No frame of any clip is latched at runtime.

| Quantity | Source | Today |
|---|---|---|
| grip socket | bone 43 (the socket aliases it with no offset, Session 91) | same |
| barrel axis | the gun body's +X or the muzzle socket record, chosen per gun by a table verified offline | chosen on the first lined-up frame |
| sideways axis `u` | the socket record's axis; which of the three is sideways is a per-gun constant (every log of every gun says socket Y; the wrench's bone Y) | `gun_side_axis` per frame, latched at rest |
| centre plane | the median of `g_wRef` bone coordinates along `u` (constant); rigid holdables: render bounds, as now | the same arithmetic, but only written on mirrored frames |
| fore-grip point (the two-handed anchor) | the free palm against bone 43 in the gun's idle clip at a known time, measured OFFLINE by `tools/blender/game_frames.py` / `reload_pose.py` (the shotgun's is already measured: 49.8 / 15.4 / 16.0; the Tommy's from `FidgetTommyGun`), shipped as a constant beside the reload paths | latched from live idle frames |
| magazine seat | the part bone's pose in `g_wRef` (Tommy TG_AmmoClip, pistol DrumParent in the open gun from the clip at 0.466, measured offline) | read live at idle; read 0.06 s into a fall |
| shell port | the insert path's last point (already a constant) | same |
| pump travel and axis | SG_Pump's displacement in the Fire clip (18 units along -X, measured) | same constant, but the stroke is measured on the controllers |
| the AmmoModel against its bone | the AmmunitionModelSocket record (derivable; to verify against one live measurement) | measured live every 0.5 s |

The mirror function: `mirror_point(p) = p - 2 (dot(p, u) - plane) u` in the grip frame, and for a pose
the existing `mirror_free_rel`. Used by the anchor, the designed grips, the spare's hold, the paths. The
free hand's rest on the gun (`s_freeRel`) is REPLACED by the fore-grip point plus the grip pose; the
grip pose's fingers come from a designed grip per gun (`grip_solve.py` against the fore-grip mesh, as
the cylinder and shell grips already do) instead of a latched idle frame. That removes `s_freeRel`,
`s_fingerRel`, `s_wristRel`, `s_lastRel`, `s_ancRelBy`, `s_restU`, `s_u`, `g_mirU`, `g_mirPlane`,
`g_restSeat*`, `g_amRel*` and the EmptyFidget / settle / first-idle rules around them.

What stays latched: the shotgun's gun-in-hand rest (`s_gunRel`) IS a geometry constant too - sockets
43/44 against the wrist come from the rig's idle clip at a known time - so it moves into the same table.

### 2.4 One suppression layer, declared per gun

A per-gun record of what the game is not allowed to do while the mod drives that gun, applied by one
function at the top of the compose and torn down by the same function:

```
struct Suppress {
    bool reloadButton;        // X never reaches the game (every gun with a program)
    bool trigger;             // the trigger is the mod's (per GunState::fireAllowed)
    bool autoReload;          // the game's own reload is adopted, never allowed to play through
    bool handsClipMotion;     // the gun hand's clip motion: a per-clip keep table (Fire: recoil window; others: 0)
    bool freeHandClip;        // the free hand's clip pose is never drawn while the hold or a carry owns it
    const wchar_t* mutedEvents[4]; // PumpShotgun, ShotgunReloadEnd, ReloadTommy, ...
    bool ammoModelVisibility; // the clip's show/hide notifies are overridden by GunState::magazine
};
```

Today this is spread across `g_handsReload`, `g_blockFire`, `g_gunKeep` and `gun_motion_keep()`,
`handback`'s manual-reload branch, `keep`/`offRest`/`start_clip` in bones.cpp, `kMuted` plus `g_muteOn`,
and the four `ShowAmmoModel`/`HideAmmoModel` call sites. One record per gun kind, one reader. The
mute applies while the gun's record is active, which is "the mod drives this gun's reloads and pumps",
the same condition as today but named once.

The free hand's rule becomes: while `GunState::twoHanded`, the free hand is drawn at the fore-grip point
with the designed grip pose, full stop - no `keep`, no `offRest`, no start-clip special case, no
post-pump grace. A clip that wants the free hand (a reload) gets it only when the hold ends, and the
hold ends only when the grip comes up (the shotgun's rule, made every gun's; item 6 goes).

### 2.5 The pump and the grip as tested state machines

**The grip** (`two_hand_math::Grip`) keeps its shape; its input changes: `distCm` to the fore-grip point
from 2.3, `slideCm` removed, a `grabAllowed` bit from the ownership table (a carried shell refuses a NEW
grab; an existing hold is never dropped by it). Tests: grab, let go, a refused grab keeps a hold, a hold
survives a frame with no measure, a hold survives a reload start, a hand-over while held ends the hold
(the roles swap).

**The pump** becomes one `Pump` with one config, driven by the gun's record:

- The stroke is the free controller's travel ALONG THE GUN'S BARREL AXIS (2.3), measured in the gun
  hand's controller frame, from where the hold took it; the ratchet keeps its farthest-the-other-way
  reference and its 6 cm; the gun hand's own motion cancels because the measure is relative to it.
- Three uses, one instance: between shots (the Fire clip waits at 0.45), the reload's last pump (the end
  clip waits at 0.409), the dry cock (no clip; the pump bone drawn by the stroke). The use is a field of
  the config chosen from `GunState::stage`, and a stroke in progress is never reset by the use changing.
- The clip follows the stroke 1:1 over the pump's travel; the catch plays the rest.

Tests (host, pure): a fast pump (both strokes in 60 ms, each at least 6 cm); a half pump (4 cm back, let
go, 4 cm back again: no stroke; then 6 cm: back done); a hand that wanders sideways 15 cm (no stroke); the
gun hand moving 20 cm while the free hand holds still (no stroke); a hand swap mid-stroke (the stroke is
dropped, the next hold starts fresh); the three uses in sequence with one instance.

**The reload machine** (`reload_rig::Machine`) keeps its programs; its inputs become the gun record plus
the hand; `claim` is replaced by "which part the reload wants and its distance". Tests to add: switching
weapons mid-reload (the record's stage goes to None, the magazine state survives on that gun), a
hand-over mid-reload (the part stays with the hand that carries it; the fore-grip's owner swaps), an
empty gun and a magazine-less gun in both hands (the same ownership answers with mirrored geometry), the
pistol's dry wait clicks.

### 2.6 Sounds as a table of action to part

```
struct SoundPart { GunKind gun; Action action; const char* event; unsigned fromMs, toMs; }; // toMs 0: whole
// Shotgun: PumpBack  -> ShotgunReloadEnd 580-710;  PumpForward -> ShotgunReloadEnd 780-1060;
//          DryFire   -> DryFiring whole (the ammunition's tag)
// Tommy:   DrumOut   -> ReloadTommy 0-560; DrumIn -> ReloadTommy 1275-1925; BoltBack -> ReloadTommy 2225-2700
// Pistol:  (to be read from a run's log: its reload events are not yet listed)
```

Actions are emitted by the machine and the pump as events on the gun record (`drumOut`, `drumSeated`,
`boltBack`, `pumpBack`, `pumpForward`, `dryFire`, `shellIn`), and one function plays the row. The
phase-number tests in `tommy_sound` and the `wasFwd`/`backDone` dance in `stroke_sound` go. The part
times are the `pcm` timeline ones already in `manual_reload.cpp` (ENGINE_NOTES 20:35 correction).

### 2.7 Behaviours the tester has confirmed, and where each lives afterwards

| Behaviour | Where |
|---|---|
| Tommy spare drum: pre-pull, throw and catch, slap in | `GunState::magazine = Spare`; the ownership table's pouch row; the machine's Fetch/Insert as today; the catch by the seat constant |
| shotgun: the grip is never lost while held | `Grip` ignores everything but the grip bit (2.5) |
| shotgun: cocking only by hand; no firing before cocking | `GunState::cocked`, `fireAllowed()`; one `Pump` with `waitForHands` |
| shotgun: the dry click | `dryClick()` on every refused pull |
| shotgun: the pump sound split into two strokes | the sound table's PumpBack/PumpForward rows |
| the pistol's cylinder: carried with the designed grip, seated by the latch | unchanged; the seat from the open-gun constant |
| X does nothing on guns reloaded by hand | `Suppress::reloadButton`; the eject stays the pistol's and Tommy's |
| the hand-over by hands together | unchanged |
| BRVR parity: 12 cm grab, 20 cm release hysteresis, rising edge grabs, bumper gated on eligibility, the anchor a latch of the game's own hand | kept, with the anchor as an offline constant instead of a runtime latch - BRVR latched it because it could not read a driven cluster; the constant is the same number taken once |

### 2.8 Order of work and what each step removes

1. Geometry table and its mirror function, with host tests against the measured constants and the
   existing pose recordings (`pose_capture.py --record` replays can assert the fore-grip point against
   the drawn palm). Removes eleven latches (2.3). Headset check: grabs land on the fore-grip in both hands,
   after a plasmid switch, after a pump, on an empty gun.
2. `GunState` and the dry click. Removes six copies of the gun's state. Headset check: a click on every
   refused pull, every gun, every stage.
3. The ownership table and the arbiter on it. Fixes bug 1. Headset check: the empty Tommy's fore-grip.
4. `Suppress` per gun, replacing the scattered flags and the free-hand rules. Fixes item 6 and 12.
5. One `Pump` on the barrel axis. Fixes bug 4 and 5's second half (a hold that measures right can cock).
6. The sound table. No behaviour change intended; a regression check by log (`[sound] played ...`).

Each step is one commit with its tests and its ENGINE_NOTES/ARCHITECTURE entry; none is pushed or merged.
`git diff main...HEAD -- src/core/` stays as it is: everything above is in `src/game/bioshock1r/`.

### 2.9 Open questions for the tester before step 1

- The fore-grip point for the Tommy Gun: taken from `FidgetTommyGun` at rest, or from where the hand
  actually grabs (the two differ by a few cm; the log's grab distances say the clip's point is good).
- The drum well's grab radius (step 3): the whole drum (about 8 cm) or a point at its front.
- Whether a two-handed hold should survive a hand-over (the roles swap and the old free hand becomes
  the gun hand), or end.
- The pistol's reload sounds: a run with the pistol reloading, so its events can be listed.
