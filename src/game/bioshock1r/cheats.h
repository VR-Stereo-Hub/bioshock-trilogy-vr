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

// F10 > Cheats.
void draw_panel();

} // namespace bvr::b1r::cheats
