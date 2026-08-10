#pragma once
// s52: THE CHEATED ARSENAL - `bsigive`. Grants any weapon or vigor by
// ARCHETYPE NAME through the proven reflection lane, so the per-weapon
// calibration save can be built without playing to each pickup (ROADMAP I9,
// user directive 2026-08-10).
//
// The recipe, derived live s52 (every rung measured on the Blue Ribbon save):
//   1. DynamicLoadObject("PreCoalescedItemAssets.<Archetype>") -> archetype
//   2. pawn   AcquireWeapon(archetype)  -> a new XWeapon instance joins the
//                                          manager's carried list (+0x200..)
//   3. manager EquipWeapon(instance)    -> in hand (GetEquippedWeapon flips)
//   4. instance AddAmmo(count)          -> ammo
//
// This CORRECTS the s43 falsification: AcquireWeapon wants the ARCHETYPE
// object (all live weapons are literal class XWeapon; identity lives at
// UObject+0x24 ObjectArchetype), not the class or the CDO - those dispatch
// clean and grant nothing, which is what s43 measured.
//
// Game thread only (rides the reflection lane's own gates). Acceptance is a
// measured effect - the slot list, GetEquippedWeapon, the ammo HUD - never a
// log line.

namespace bvr::bsi::arsenal {

// `bsigive <ArchetypeName|Full.Path> [ammo]` | `bsigive list`.
// A bare name gets the "PreCoalescedItemAssets." package prefix. Returns
// false when the command is not ours.
bool handle_command(const char* cmd, const char* args);

} // namespace bvr::bsi::arsenal
