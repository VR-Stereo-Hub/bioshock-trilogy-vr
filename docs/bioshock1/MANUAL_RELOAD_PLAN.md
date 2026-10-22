# BS1 manual reloading: the rest of it (plan, 2026-10-22)

The pistol, Tommy Gun and shotgun reload by hand on `staging` (merge 7c7562f, HELD_GUN_AUDIT.md,
HELD_GUN_MATRIX.md). This is the plan for the three guns that do not yet - the grenade launcher,
the crossbow, the chemical thrower - and for the three aim faults found on the 17:53-18:10 run
with them in hand. Branch `claude/bs1-manual-reload-2` off `staging`.

Everything below was measured offline (Blender on the exported rigs, the script corpus, the run's
log); nothing was run in the game. Sections 1-3 are facts, 4-6 the work.

## 1. What each gun's reload physically is (the game's own clips)

Rigs: `WP_GrenadeLauncherMesh` (8 bones), `WP_CrossbowMesh` (15), `WP_ChemicalThrowerMesh` (8),
exported from `ShockGame.U` with `tools\bsmesh\bs2gltf.py` to `<ws>\exports\bs1\`. The hands'
clips are in `NEWPlayerHands-all.glb` (130, all weapons and plasmids). Per bone, how far each clip
moves it (`glb_motion`, max over the clip, rig units):

**Grenade launcher** - `ReloadLauncher` 4.00 s (hands) / `Reload` 4.00 s (gun); a revolving
chamber (`MainBarrelBone` turns 70-75 deg per shot in `Fire` 1.43 s, and in the reload).
- `Dummy02` (the handle and trigger section) slides 29 units back: the breech opens.
- `MainBarrelBone` slides 20 forward and turns 70 deg.
- `AmmoBoxDummy` moves 52 units (the ammo box comes OFF the gun), `AmmoDummy` turns 90 deg (the
  grenades go in). `AmmunitionModelSocket="AmmoDummySocket"`; the grenade model shows while
  `RoundsRemaining >= 1`.
- The gun hand turns 44-60 deg for the whole middle of the clip (0.5-3.5 s); the free fist goes
  from the fore-grip (51 21 15 against R_grip) up and forward to (69 12 40) at 1.2 s and holds
  about (48 10 38) through 2.0-3.2 s - above the open breech - then back.
- `FireLastLauncher` 0.70 s is the last round's own clip.

**So the launcher coming apart is the game's own reload**: the box leaves the gun with the hand,
the breech slides open. In VR it reads as the gun splitting because the mod draws the gun hand on
the controller while the clip moves the detached parts in the gun's frame, with no hand on them.
A manual reload owns exactly those parts (as the Tommy's drum), which is the fix.

**Crossbow** - `ReloadCrossbow` 3.07 s / `Reload` 3.00 s; magazine 4 (the log: `rounds 0/4`).
- `Fire` 2.00 s re-cocks the crossbow itself after every shot: `X_Lever` 75 deg, `X_stringPull`
  32 units, `X_HookSlide` 16, `X_Latch` 90 deg, the arms 25 deg. The free hand works the lever at
  0.3-0.9 s of the reload too (fist at (-28 -12 31) to (0 -16 54): behind and above the grip).
- `X_AmmoBox` (the bolt box) turns 169 deg and travels 120 units: the free hand takes it off,
  flips it (1.2-1.5 s, the fist 80 units out at (26 -54 60)), and puts it back (1.8-2.7 s).
- `X_BoltSocket` slides 10 (the bolt seats). `AmmunitionModelSocket="AmmoDummy"` on it.
- `CanBeZoomed`, with its own `ZoomingIn/ZoomedInFidget/ZoomedInFire` clips (FOV 15).
- No `muzzle` socket (its sockets: `XB_UpgradeA/B`, `AmmoDummy`).

**Chemical thrower** - `ReloadChem` 4.50 s / `Reload` 4.50 s (two rate bands, 0-1.89 and
1.9-3.0: the fast-reload upgrade).
- `Bone_Wrench` turns 80 deg (and 65 deg in `Fire`: the free hand works the valve wrench to
  spray), `Bone_Faucet` 46 deg, `Bone_Hose1` moves 50 units (the hose pulled to the tank).
- The free fist goes from its rest (12 -11 23) behind the grip to (-13 37 2) at 1.0 s, works
  about (-15 -5 10) for 1.5-3.0 s, returns by 4.0.
- Sounds on the run: `ReloadChemThrow` (hands), `ReloadHoseSpurt` and `WrenchReloadLeak`
  (weapon); firing cues `ChemThrowPullTrigger` / `ChemThrowPushTrigger` (hands).
- `DamageEmitterSocket="muzzle"`: the spray is a `DamageEmitter` actor `AttachToBone`'d to the
  muzzle socket (Weapon.uc `OnSpawnedDamageEmitter`), so it points where the socket points.

## 2. The three aim faults, from the run's log

| Fault | What the log says | Cause |
|---|---|---|
| Crossbow reticle left of where the bolt lands | no `[aim] barrel origin` line on any crossbow equip (every other gun logs one) | the barrel origin needs the mesh's `muzzle` socket (`muzzle.cpp`); the crossbow has none, so the bolt leaves from the game's own fire start while the reticle sits on the controller ray: parallax |
| Chem spray misses the reticle | `muzzle at fwd +35.9 right -34.2 up -20.8 cm from the ray`, `muzzle socket +X ... 12.7 deg apart` from the body, body 0.97 with the ray | the emitter is attached to the socket, 34 cm left, 21 cm down and ~27 deg off the ray; bullets are redirected by the fire-start seam, a particle emitter is not |
| Launcher splits | section 1 | the game's reload, with nothing tying the detached box to a hand |

## 3. What already covers the three guns

`gun_geometry.h` has their fore-grips, side axes and rest poses (measured 10-21); the two-handed
grip, hand-over, dial and arbiter work on them (the run: all three `is two-handed`). The sound
engine logs their events. `gun_state`, `gun_suppress`, `gun_sounds` and `reload_rig` treat them as
`Kind::Other`: the game keeps its reload, its clips, its cues.

## 3b. The launcher's reload, timed (reload_pose.py, gun model space, 2026-10-22 evening)

| Clip time | What moves |
|---|---|
| 0.0-0.5 | the gun hand tips the gun up (26 -> 44 deg); nothing on the gun |
| 0.5-0.75 | `MainBarrelBone` (the revolving chamber, with the ammo box on it) turns 70 deg: the box swings from (23 2 2) to (23 -43 -15), the loading port comes round |
| 1.0-1.25 | the breech opens: `Dummy02` (the handle section) slides 29 back (21.7 -> -7.4 along X), the barrel 20 forward (42 -> 62) |
| 1.25-3.0 | `AmmoDummy` (the grenade pack, the ammo model) travels from (38 -27 -29) to (32 -6 -3): the pack pushed home over 1.75 s; the breech drifts shut (the barrel back to 43 by 2.0, the handle to 5.6 by 2.0 and 22 by 3.0) |
| 3.0-3.25 | the chamber turns back (58 -> 6 deg), the box with it |
| 3.25-4.0 | the gun hand levels (33 deg at 3.75, 2 at 3.95) |

The free fist: at the fore-grip (51 21 15) until 0.25, up and forward to (69 12 40) at 1.2 - on the
barrel section as the gun tips - and about (48 10 38) through 2.0-3.2 (over the open port), back by 3.95.

The program this suggests: the free hand grips the barrel section; the gun hand pulls back (the
two controllers separating along the barrel, the pump's own measure) and the clip follows the stroke
from 0.5 to 1.25 - the chamber turns, the breech opens; Fetch the pack at the pouch, Insert it along
`AmmoDummy`'s 35-unit path to the port; push the breech shut (the controllers closing) with the clip
played 3.0 -> 3.25; Finish. What the exported clip cannot say is which section the game's own hand
holds while the handle moves 29 units relative to the grip root - the pose recorder (step 7, in) with
the launcher in hand once answers that from the engine's own bones, and the program is written
against that recording.

## 4. The work, in order

Each step is one commit with its host tests, built and installed; the simulator for the mechanics
(one run per step, asked for), the headset for the feel.

1. **Crossbow barrel origin** - DONE 12b8836, unverified in a run. `muzzle::locate` falls
   back to a per-gun muzzle point when the mesh has no `muzzle` socket: a `gun_geometry::Gun`
   column `muzzle` (bone 43 frame; the crossbow's from `X_BoltSocket` + 22 along X, the bolt's
   rest, measured in Blender like the fore-grip). Log the rejected axis dots so the next run says
   why a gun has no origin. Check: a crossbow equip logs a barrel origin; the dot lands on the bolt.
2. **Chem thrower spray along the ray** - DONE 7136b24, unverified in a run. The mod already knows the ray and the drawn body each
   frame (`update_barrel_direction`). Each tick while the thrower is in hand, write the
   `CurrentDamageEmitter`'s `RelativeRotation` (a script property on the weapon, the emitter is
   attached to the socket) so the emitter's +X is the ray in the socket's frame. Fallback if the
   relative rotation is ignored by the attachment: rotate the muzzle socket's bone write in the
   weapon lane for this gun only. Check in the simulator with a capture: the spray's first
   particles leave along the laser.
3. **The launcher's manual reload** - the first new program, the one the tester sees broken.
   `Kind::Launcher`; the program (`reload_rig.h`): Pull the breech open (the hand on the handle
   section, `Dummy02`'s 29-unit slide as a `Slide` phase along the gun), Pull the ammo box off
   (`AmmoBoxDummy`, a carried part as the Tommy's drum, with its designed grip from
   `grip_solve.py`), Fetch grenades at the pouch and Insert them into the box or the chamber (the
   clip's `AmmoDummy` 90-deg turn is the seat), Insert the box back, Slide the breech shut. The
   clip times from `reload_pose.py` on the two glbs (`--part AmmoBoxDummy`). The chamber's
   75-deg turn per shot stays the game's (the Fire clip). Geometry: the handle and box fist
   centres into `gun_geometry`. Suppression record: X, the adopted dry reload, the free hand,
   the ammo model. Sounds: `GrenadeLauncherEquip` is the only weapon event seen so far; a run
   with the reload logs the rest.
4. **The crossbow's manual reload.** `Kind::Crossbow`. Two mechanics: the lever (a `Slide` of
   `X_Lever` 75 deg - the free hand pulls it back and lets it return; the same stroke the Fire
   clip plays itself after each shot, which the matrix's shotgun rule suggests making the
   player's: the crossbow waits uncocked after a shot until the lever is worked, a deliberate
   stroke) and the bolt box (Pull it off, Fetch bolts at the pouch, Insert them, Insert the box
   back; the box is the carried part, 169 deg and 120 units in the clip, so it is drawn on the
   hand, never from the clip). The zoom clips stay the game's. Rest pose and fore-grip exist.
5. **The chem thrower's manual reload.** `Kind::Chem`. Fetch a tank (the `ChemicalThrower_*`
   ammunition is the tank; the ammo model socket is to be found - the class names no
   `AmmunitionModelSocket`), Insert it at the tank mount, then the wrench: a `Slide` of
   `Bone_Wrench` 80 deg (the hand on the wrench, turned through its arc; `Fire` turns it 65 deg
   too, so the spray's own trigger is the wrench in the game's fiction - keep the trigger as the
   trigger, the wrench for the reload). The hose's 50-unit pull is cosmetic; let the clip have it
   or carry the hose end with the hand. Sounds: `ReloadChemThrow`, `ReloadHoseSpurt`,
   `WrenchReloadLeak` as parts, timelines from `fsb-probe.ps1` on the exported sounds.
6. **The ammunition variants' sound timelines** (all six guns). The electric buck's six takes are
   known by FMOD length (757-1175 ms); export each `HandsShotgunReloadEnd_ammo*` /
   `HandsReloadTommy_ammo*` response's sounds with UModel, probe each with `fsb-probe.ps1`, and
   add rows keyed by length (`gun_sounds::Row::lengthMs` already selects the take). Until then a
   variant plays whole once (the current rule).
7. **The pose recorder for any gun** - DONE cec65e8. `bones.cpp` writes `Shotgun.bvrpose` for the shotgun only;
   make it the gun in hand, so steps 3-5 can be checked frame by frame against the clips
   (`pose_capture.py --record`) the way the shotgun was.
8. **Headset pass** on all six guns with the matrix as the checklist: every row of
   HELD_GUN_MATRIX.md sections 3-5, then the three new sections it gets from steps 3-5 (write
   them before the code, as the shotgun's was).

## 5. What stays the game's

The crossbow's zoom (its own clips and FOV), the launcher's chamber rotation per shot, the chem
thrower's pilot light and spurts, the research camera, the wrench. Plasmids are untouched.

## 6. Open questions for the tester

- The launcher: should the box come off the gun by hand (the game's way, step 3) or stay on with
  a lid, grenades dropped straight into the chamber? The game's clip detaches it.
- The crossbow after a shot: the game re-cocks it itself in the Fire clip. Make the lever the
  player's, like the shotgun's pump (the gun waits uncocked), or leave it?
- The chem thrower's reload today takes 4.5 s of valve work; a manual one is as long as the hand
  takes. Fine?
