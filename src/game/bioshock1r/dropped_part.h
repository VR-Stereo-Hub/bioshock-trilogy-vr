#pragma once
// A spent magazine falling out of the gun (2026-10-20, the manual reload's eject).
//
// The visible magazine is the weapon's AmmoModel (Weapon.uc: a VisibleAmmoModel actor attached to
// the gun, its StaticMesh the current ammunition's VisualAmmoModel - the Tommy Gun's drum
// TG_AmmoModel, the pistol's PI_AmmoType). The drop is a NEW actor the game's own script API makes
// (script_call.h, by name): Actor.Spawn of ShockGame.PhysicalReactiveActor - the game's Havok prop,
// spawnable at runtime (not bNoDelete like Engine.HavokActor), Physics=PHYS_Havok, not blocking the
// player, and telekinesis can throw it - given the magazine's mesh and size, put where the
// magazine was, its physics rebuilt for that mesh (SetPhysics none, then Havok) and set moving
// (HavokSetLinearVelocity / HavokSetAngularVelocity). LifeSpan removes it after a while. Enemies'
// dropped weapons are the same idea (AIAttachment: bIsHavokPhysicsEventually), done natively.

namespace bvr::b1r::dropped_part {

// Game thread. Remember the weapon's visible magazine as it is now: mesh, size, world pose.
// False when there is none (no AmmoModel, no mesh) or script properties are unavailable.
bool capture(void* weapon);

// Game thread. Drop the remembered magazine as a physics prop (once; the capture is spent).
// `velocity` (world units per second) is added to a short fall. False when nothing was captured or
// the engine refused; the log says which step.
// `hands` (the Hands actor) is the last-resort spawner.
bool drop(void* weapon, void* hands, const float velocity[3]);

} // namespace bvr::b1r::dropped_part
