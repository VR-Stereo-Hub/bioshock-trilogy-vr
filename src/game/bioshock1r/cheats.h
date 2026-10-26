#pragma once
// F10 > Cheats (2026-10-20): every weapon, for testing them in VR before the story hands them over.
//
// The game's own developer functions do the work, by name (script_call.h), exactly as its
// ShockCheatManager.GiveAll does: ShockPlayer.UnlockInventorySlots, ShockPawn.GiveWeapon for each
// weapon the player does not hold yet (GetHoldableByClassName first, so nothing is given twice), and
// ShockPlayer.GiveItem for a full load of every ammunition type. "Upgraded" then does what the game
// does for a New Game Plus weapon on spawn (PlayerWeapon.PostBeginPlay): each of the weapon's
// UpgradeStats gets PointsAllocated = 1, and the first's attachment (GiveAltFireMod) and the
// second's (GiveStrictlySuperiorMod) are shown. ShockPlayer.AddWeaponStatUpgrade is NOT used: it
// calls Weapon.AddStatUpgrade, which is empty in this build.
//
// A request is queued from the menu and run once on the game thread, while a weapon is in hand
// (its owner is the player pawn the calls go to).

namespace bvr::b1r::cheats {

// Any thread (the F10 panel).
void request_all_weapons(bool upgraded);

// Game thread, once per frame: `holdable` = the weapon in hand (hands::current_holdable).
void tick(void* holdable);

// 2026-10-25: every active plasmid (the game's own ShockPlayer.TestAddAvailablePlasmid / UnlockTrackSlot /
// TestEquipPlasmid, as ShockCheatManager.GEP), and infinite EVE (AddBioAmmo up to GetMaxBioAmmo once a second).
// They reach the player through the hands actor's PawnOwner, so no weapon need be in hand.
void request_all_plasmids();
// The plasmid in hand, by its ability class name (ShockPlayer.EquipAbilityByClassName) - the harness's `vrcheat plasmid`.
void request_equip_plasmid(const char* abilityClass);
void set_infinite_eve(bool on);
bool infinite_eve();
// Game thread, once per frame: the hands actor.
void tick_hands(void* handsActor);

// F10 > Cheats.
void draw_panel();

} // namespace bvr::b1r::cheats
