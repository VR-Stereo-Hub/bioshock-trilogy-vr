# BS1 Telekinesis: the native code, where it aims from, and two holds at once

2026-10-25, static only (headless IDA, BioshockHD.exe md5 `b4db411ab3e5994b6cd39f8dd91fd990`, the build every
`patterns.h` RVA here is for). Every claim below is what the binary says. **Checked live 2026-10-25 00:38-01:09 in
the simulator** (the 11:01 save, Medical Pavilion), the section just below; the open items at the end are what is
still unchecked. Scripts: `tools\ida\tk1_*.py` .. `tk9_*.py`
(outputs in `<ida_workspace>\out\bs1\`, never committed). All addresses are RVAs (image base 0x10900000 in IDA).

The question: Telekinesis aims with the head in VR. The owner wants it aimed by the casting hand's controller, and
wants two independent holds, one per hand, now that the mod runs a second Hands actor for the right hand
(`second_hands.h`).

## Checked live (simulator, 2026-10-25)

- **The view-ray hook works as predicted.** With +0x1C0F70 detoured, a pick, the hold point and the throw all follow
  the casting hand's controller ray; `this` tells the hands apart (the right hand's own object).
- **`TelekinesisTwoAbility` is the second object, with no native call.** It is found by NAME in the player's
  `ShockPlayer.PossibleAbilities` (its path is `ShockGame.ShockDesignerClasses.TelekinesisTwoAbility`, and
  `DynamicLoadObject` gives nothing for it either way); `GetAbilityFromClass` then returns the player's own object of
  that class. Two objects, two targets: `LEFT HOLDING <prop> | RIGHT HOLDING <corpse>` at once (00:55), each moving
  with its own hand.
- **The mod's 30 Hz tick of the second object holds it.** The state's kind byte at state+4 reads 0 idle, 1 pulling,
  2 holding as listed. A target picked in Idle that the pull cannot move stays as the target with the state Idle
  (the pinned hanging corpses).
- **A press is the pick, holding the trigger keeps the hold.** A 250 ms tap picks and lets go; the next press drops a
  stale target.
- **A Telekinesis hold keeps the game's hands in their fire state** for its whole length, so a cast from the other
  hand cannot be attributed by "which hands is firing": the casting ability object says it (`second_hands::on_stack`).

## Short answer

- Telekinesis never goes through `GetPerfectFireStart` (it is not a `UAttackAbility`) and never reads the pawn's
  view-point / view-direction vtable slots (+0x360 / +0x35C). **All three of its aiming decisions - picking the
  target, placing the held object, and the throw direction - get their origin and direction from ONE small
  function, `UTelekinesisAbility` view ray at +0x1C0F70**, which asks the local PlayerController's
  `eventPlayerCalcView` (+0x1BE7A0 - the very function the mod's camera detour hooks) for the camera location and
  rotation and turns the rotation into a unit vector.
- **Recommended hook: detour +0x1C0F70** (thiscall `(ability, FVector* outOrigin, FVector* outDir)`, `ret 8`). It
  has exactly three callers, all telekinesis, and `this` is the ability object, so a per-hand ray is a lookup on
  `this`. Writing the hand ray there and NOT calling the original also stops Telekinesis from re-entering the camera
  detour several times a frame.
- **A second hold needs a second `UTelekinesisAbility` object AND the mod ticking it.** The ability object holds
  all per-hold state (target, state, Havok controller, damage listener). The engine ticks only the ability of the
  player's `ActiveAbility` class, from `AShockPlayer`'s tick, with a single global time accumulator - a second
  object would sit frozen in its pull. The object can be made with the engine's own `StaticConstructObject`
  (+0x700500) exactly as `ShockPlayer.GetAbilityFromClass` makes abilities; a fresh one initialises itself on
  first use.

## 1. The native code

### Class and natives

| what | RVA | how found |
|---|---|---|
| `UTelekinesisAbility` vtable | 0xD7F430 | MSVC RTTI `.?AVUTelekinesisAbility@@` (tk1). 58 slots; only slot 0 (the destructor, +0x1C35E0) differs from `UAbility` (0xD7EACC) - the class adds no C++ virtuals and overrides none |
| class registration (size 0x13C) | +0x1C0BD0, called once from the StaticClass getter +0x1F2520 | the wide string `TelekinesisAbility` passed to the UClass constructor with size 316 (tk4) |
| internal constructor | +0x1C36E0 | registration callback; it only writes the vtable |
| `execOnOtherActorDestroyed` | +0x1C2370 | native table entry `intUTelekinesisAbilityexecOnOtherActorDestroyed` (.data 0x11B9CE0), the thunk written into entry+4 by the static initialiser at +0x20800 (tk1) |
| `execGetState` | +0x1C0F40 | entry 0x11B9CF8 |
| `execDropObject` | +0x1C1860 -> drop +0x1C1740 | entry 0x11B9CEC |
| `execStartedUsingAbility` | +0x1C0D30 | entry 0x11B9D10 |
| `execUseAbilityRelease` | +0x1C0DE0 | entry 0x11B9D04 |

The script functions `UseAbility`, `StartedUsingAbility` and `UseAbilityRelease` in the corpus are the script side of
these natives; the work is all native.

### Field layout (script order, confirmed by the natives' offsets)

| offset | field | evidence |
|---|---|---|
| +0xE0 | bitfield: bit0 `bInitialized`, bit1 `bLastUseSuccessful` | the lazy init in every native tests bit0; start clears/sets bit1 |
| +0xE4 | `CurrentState` (pointer to a state object) | GetState reads its kind byte at state+4 |
| +0xE8 | `DamageListener` | created on entering Pulling, cleared on Throwing/Dropping |
| +0xEC | `State` (enum byte, mirrors state+4) | written beside every +0xE4 write |
| +0xF0 | `ConeAngle` | the cone test's cosine |
| +0xF8 | `AcquisitionDistance` | pull -> hold switch distance |
| +0xFC | `BreakageDistance` | hold -> drop distance |
| +0x100 | `Range` | the pick trace length and search radius |
| +0x104 | `Target` | |
| +0x108 | `Player` | set from the Instigator argument of StartedUsingAbility / UseAbilityRelease |
| +0x10C | `MaximumHoldingDistance` | hold point |
| +0x120 / +0x124 | `TargetAssistanceCosine` / `TargetAssistanceRange` | throw aim assist |
| +0x128 | `ExplosiveProjectilePreferenceFactor` | pick scoring |
| +0x12C | `TelekinesesStartTime` | start/release |
| +0x134 | `Controller` (Havok-side helper object) | created by +0x1C2780 |
| +0x138 | `ActionListener` | never seen written in the functions read |

### The state machine

The states are C++ objects of classes `TelekinesisIdle / Pulling / Holding / Throwing / Dropping` (base
`TelekinesisState`, RTTI). **Each is a function-local static singleton shared by every ability object** - e.g. the
Idle getter +0x1C05E0 returns one static instance, Pulling is built by +0x1C0730, Holding by +0x2B48C0, Throwing by
+0x2B0300. They carry only a vtable, their kind byte (+4: 0 IDLE, 1 PULLING, 2 HOLDING, 3 THROWING, 4 DROPPING) and
a name; all per-hold data lives in the ability. So sharing them between two ability objects is safe.

State vtable (4 slots, all thiscall with the state in ecx unused, the ability on the stack):

| slot | meaning | Idle | Pulling | Holding | Throwing | Dropping |
|---|---|---|---|---|---|---|
| 0 | on press (from StartedUsingAbility / UseAbilityRelease) | start: pick + enter Pulling (+0x1C2450) | drop (+0x1C1740) | -> Throwing (+0x2B08F0) | - | - |
| 1 | Tick(ability, float dt), `ret 8` | - | controller pull (+0x2B0400) | controller hold (+0x2B09A0) | controller throw, then -> Idle (+0x2B0E20) | - |
| 2 | enter | - | +0x2B0440 | +0x2B09D0 | +0x2B0E90 | +0x2B1140 |
| 3 | exit | - | +0x2B0730 | +0x2B0C60 | - | - |

State vtables: Idle 0xD7F5A4, Pulling 0xD7F57C, Holding 0xDB49F0, Throwing 0xDB46AC, Dropping 0xD7F590.

The controller (`TelekinesisController`, vtable 0xD7F55C) is a 12/16-byte heap object made by +0x1C2780 when a hold
starts: `TelekinesisRigidBodyController` (vtable 0xD7F51C) for a rigid body, `TelekinesisRagdollController` (0xD7F53C)
for a ragdoll. It points back at the ability. Slots 4/5/6 are the per-tick pull / hold / throw: rigid body +0x2B4DE0
/ +0x2B50A0 / +0x2B5400, ragdoll +0x2B5780 / +0x2B5B20 / +0x2B5EE0. The pull switches to Holding when the body is
within `AcquisitionDistance` of the hold point; the hold drops when it is more than `BreakageDistance` away.

The flow: press -> `execStartedUsingAbility` lazily initialises the object if bit0 is clear (state Idle, no target,
no listener), stores the Player, and calls the state's slot 0. Idle's slot 0 (+0x1C2450) picks a target (+0x1C1220);
on success it registers the ability in a global observer array (VA 0x11C561FC - the actor-destroyed observer list,
shared with many other classes, not a telekinesis singleton), bumps a counter on the target (+0x3A4), creates the
controller and enters Pulling. A later press in Holding throws; release/drop goes through +0x1C1740 (Dropping, then
Idle, then release the target and destroy the controller via +0x1C23F0).

### (a) Picking the target: +0x1C1220

Gets origin and direction from the view ray +0x1C0F70, then: a box trace (extent 10 UU) from the origin along the
direction for `Range`; if it hits something telekinesis can move, that is the target. Otherwise a radius query of
`Range` around the origin, scoring every candidate by its squared distance from the ray, keeping those whose
direction cosine beats `ConeAngle`, weighting explosive projectiles by `ExplosiveProjectilePreferenceFactor`, and
requiring line of sight from the origin to the candidate's bounds (+0x1C1010: traces from the origin to the box
centre and its eight corners). Both "can be telekinesised" tests are script interface calls on the actor
(+0x1C3390, +0x1C3300).

### (b) The hold point: +0x1C1A60

Gets origin and direction from +0x1C0F70; hold distance = `MaximumHoldingDistance` scaled by the object's bounding
radius, plus a float on the pawn at +0x2FC (probably its collision radius - unverified); point = origin + direction
x distance; then a sweep from the origin to that point (trace flags 198) pulls the point in front of any wall.
Returns the point in Havok units (x 0.02) as a 4-vector. Called every pull and hold tick.

### (c) The throw direction: +0x1C1FB0

Gets origin and direction from +0x1C0F70; then aim assist: a radius query of `TargetAssistanceRange` for live pawns
whose direction from the origin is within `TargetAssistanceCosine` of the view direction and visible (same LOS
test); the nearest one's eye point wins, else the raw view direction. The throw itself costs EVE on the player
(+0x1CF630, `BioAmmoReduction_Modifier`).

## 2. Where the origin and direction come from

**+0x1C0F70, `__thiscall(UTelekinesisAbility* this, FVector* outOrigin, FVector* outDir)`, callee pops 8 (`ret 8`).**
Prologue `55 8B EC 83 EC 1C 53 8B D9 56 57 8B` (no relative branch in the first 12 bytes: safe for MinHook).

What it does: takes `this->Player` (+0x108); finds the level's local PlayerController (+0x1BE660, cached on the
LevelInfo at +0x800); calls **`eventPlayerCalcView` +0x1BE7A0** (`APlayerController`, thiscall `(ViewActor*&,
FVector& loc, FRotator& rot)`, a `ProcessEvent` thunk - ENGINE_NOTES "RESOLVED live: RVA 0x1BE7A0") with the
player as the view actor; copies the location to `outOrigin`; converts the rotation with `FRotator::Vector`
(+0x1BC870) into `outDir` (unit length).

Its only callers (tk2/tk9), with their return addresses:

| caller | call | return address |
|---|---|---|
| target pick +0x1C1220 | +0x1C1258 | +0x1C125D |
| hold point +0x1C1A60 | +0x1C1AEB | +0x1C1AF0 |
| throw +0x1C1FB0 | +0x1C1FF1 | +0x1C1FF6 |

And its own call into `eventPlayerCalcView` is at +0x1C0FE1, return address **+0x1C0FE6**.

Not used by Telekinesis: `UAttackAbility::GetPerfectFireStart` +0x1BC220 (callers are InitiateDamage +0x1BBD80 and
its thunk only), the pawn vtable +0x360 / +0x35C, `Pawn.GetViewRotation`, `EyePosition`.

**Consequence today:** every telekinesis pick, every pull/hold tick (30 Hz, see section 4) and every throw calls the
mod's `CalcViewDetour` (camera.cpp), which has no caller filter and runs its whole body - head offset, the
alternate-eye offset, `alt_eye::pair_lock` (keyed by return address, which here is +0x1C0FE6), and the per-eye
camera stash. So the head aiming is not a separate bug; it is the camera, possibly with half an IPD of eye offset
in it, and these extra calls may disturb camera state mid-frame. Unverified live, but worth knowing.

### Recommended hook design

**Option A (recommended): detour +0x1C0F70 and replace it.**

```
void __fastcall TkViewRay(void* ability, void* /*edx*/, FVector* outOrigin, FVector* outDir);
```

- Decide the hand from `ability`: the object in the main hands' `CurrentAbility` (Pawn+0x724 -> Hands+0x454,
  `kHandsCurrentAbilityOffset`) is the left hand's; the object in `second_hands::actor()`'s `CurrentAbility` is the
  right hand's. While both hands still share one object, fall back to whichever hands started the current hold
  (record it at StartedUsingAbility time, e.g. with `second_hands::on_stack()` in an `execStartedUsingAbility`
  detour) - not whichever trigger is down now, or a held object would jump hands.
- Write the hand ray (world origin in UU, unit direction - the same ray `aim.cpp` already builds for plasmid casts)
  and return WITHOUT calling the original. Call the original only when VR aim is off or the pose is unavailable.
- Game thread only (all three callers run under `AShockPlayer`'s tick or a script native).
- The origin is used for traces and radius queries, so a hand inside a wall makes the pick fail exactly as a gun
  would; the hold point is then measured from the hand, which is what holding in front of the hand means. The hold
  distance adds the pawn's +0x2FC float, tuned for an eye origin; a hand origin may want that shortened (a later
  tuning item, not a correctness one).

**Option B (if a byte-verified hook on 0x1C0F70 is ever refused):** in `CalcViewDetour`, if
`_ReturnAddress() == base + 0x1C0FE6`, write the hand origin and the hand direction as an `FRotator`, and return
before the camera body runs. It loses `this`, so per-hand selection needs a thread-local set by detours on the
three callers, which is why A is better.

## 3. A second, independent TelekinesisAbility object

### How the engine makes abilities

- `ShockPlayer.GetAbilityFromClass` is native: `intAShockPlayerexecGetAbilityFromClass` -> thunk +0x1CC980 -> C++
  **+0x1D8540**, thiscall `(AShockPlayer*, UClass*)`, `ret 4`. It looks the class up in the native `AbilityMap`
  (a `TMap<UClass*, UAbility*>` at pawn+0xF40: pairs array +0xF40, hash +0xF4C, hash size +0xF50) and on a miss
  constructs one and inserts it. **One ability object per class per player** - which is why both hands share one
  object when they hold the same plasmid.
- The construction is **`StaticConstructObject` at +0x700500**, `__cdecl`, 10 dword arguments, plain `retn`;
  prologue `55 8B EC 83 E4 F8 51 53 56 57 FF 75`. Found as the callee of `UObject::execNew`
  (`intUObjectexecNew` -> +0x733250), which passes (Class, Outer, Name as two dwords, Flags, 0, 0, Stack as the
  error device, 0, 0). `GetAbilityFromClass` passes (Class, the player's outermost object - walk Outer at +0x1C to
  the top, NAME_None as 0,0, Flags 0, 0, 0, 0, the global output device at .data 0x11B6970 (VA 0x11AB6970), 0, 0).
  After allocating it calls the class's post-construct hook (UClass+0x570).
- There is no `StaticDuplicateObject` in this build (no string or native for it); `StaticFindObject` is +0x6F7C70
  (behind `intUObjectexecFindObject` +0x735050), useful to find the class by name.

### What a fresh object needs

Nothing by hand. Its natives check bit0 of +0xE0 on every entry and, if clear, set the state to the Idle singleton,
clear the damage listener and target, and set the bit (the same reset is +0x1C1880, called from an `AShockPlayer`
virtual at vtable +0x118). `Player` is stored from the Instigator argument of StartedUsingAbility, and the
controller is created per hold. Config and defaults come from the class default object through
`StaticConstructObject`.

### Two ways to get one

1. **`StaticConstructObject(TelekinesisAbility class, outer, NAME_None, 0, 0, 0, 0, GError-device, 0, 0)`**, exactly as
   +0x1D8540 does, then give it to the second hands with `SetActiveAbility` (what `second_hands.cpp` already does
   for the shared object). The second hands' `CurrentAbility` property is then what keeps it alive for the garbage
   collector; when the second hands are destroyed, call `DropObject` on it first and forget the pointer. Consider
   `RF_Transient` in the flags so it can never be serialised (the flag's value in this build is not derived yet -
   leave flags 0 until it is).
2. **No native call: `GetAbilityFromClass` with the OTHER telekinesis class** (`ShockPlayer.PossibleAbilities[23]` is
   `TelekinesisAbility`, `[24]` is `TelekinesisTwoAbility`, a config-only subclass that runs the same native code).
   The player's `AbilityMap` then owns it, GC-safe for the player's lifetime. Cost: the right hand runs the other
   tier's config values. A quick prototype path; option 1 is the faithful one.

## 4. Per-player singletons that stop two holds today

| what | where | effect on a second hold | what the mod does |
|---|---|---|---|
| **The tick** | `AShockPlayer` vtable +0x1B8 = +0x1CFE70; the block at +0x1D01A1..+0x1D0211 | ticks only `GetAbilityFromClass(ActiveAbility)` (pawn+0xF24 = `ActiveAbility`, the field before `AvailableAbilities`/`PossibleAbilities`/`AbilityMap`), and only if it IsA Telekinesis, has a Target and a state: adds dt to a **global static accumulator** (VA 0x11CA782C) and, once it passes 1/30 s (constant VA 0x1174E69C = 0.033333), calls `CurrentState->slot1(ability, accumulated)` and zeroes it | tick the second object itself on the game thread with its OWN accumulator: `if (ab+0x104 && ab+0xE4) ((void(__thiscall*)(void*, void*, float))(**(void***)state)[1])(state, ab, acc)` at 30 Hz. Never touch the engine's accumulator. If the second object's class is ever the ActiveAbility class it would be ticked twice - it is not, by construction |
| player-level drop | `ShockPlayer` native at +0x1CAD70 (registered at +0x1C610 / +0x2034A) | drops only `Pawn.Hands(+0x724).CurrentAbility(+0x454)`; `SetActiveAbilityClass` calls it on every plasmid change | `DropObject` the second object on plasmid change, second-hands teardown, menus and loads |
| effects | Pulling/Holding/Throwing enter/exit fire `TelekinesisActive` effect events on the player AND on pawn+0x724 (the main Hands actor) | the second hold's glow and sounds play on the main hands | accept, or retarget later; cosmetic |
| `GetCurrentTelekinesisTarget` | `ShockGameInfo` (script): the main hands' ability's Target; the event name is bound natively near +0x49D590 | reports only the main hold to whatever asks | accept |
| same target twice | the pick (+0x1C1220) does not visibly exclude an object another ability holds; the target only gets a counter at +0x3A4 (and a ragdoll counter at +0x93C) | both hands could grab one object and two controllers fight over it | after the second object starts a hold, if its Target (+0x104) equals the first's, `DropObject` it (or filter in a pick detour) |

Not singletons (safe): the state objects (stateless statics), the actor-destroyed observer array (a list), the
controller and damage listener (per ability object), the cached FNames.

## 5. Risks and open items (need a run)

- Live-verified on the simulator (the section at the top): the hook, the second object, the mod's tick, two holds.
  Not yet seen in the headset.
- Confirm `CalcViewDetour` really sees the +0x1C0FE6 return address during a hold, and whether those calls move
  the eye stash or the pair lock (camera.cpp); if they do, Option A also fixes a camera side effect.
- The hold distance's pawn +0x2FC term is unidentified (collision radius is a guess).
- The `RF_Transient` value and whether `StaticConstructObject`'s flag argument needs it.
- Whether the script `IsAffectedByTelekinesis` checks reject an object already held.
- Ticking from the mod: the engine ticks inside `AShockPlayer::Tick`; the mod's per-frame game-thread point
  (`bones.cpp m4_frame`) is a different moment in the frame. Harmless for physics targets in principle; watch for
  a one-frame lag in the right hand's hold.
