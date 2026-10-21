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

#include <cstdint>

namespace bvr::b1r::dropped_part {

// Game thread. Remember the weapon's visible magazine as it is now: mesh, size, world pose.
// False when there is none (no AmmoModel, no mesh) or script properties are unavailable.
bool capture(void* weapon);

// Game thread. Drop the remembered magazine as a physics prop (once; the capture is spent).
// `velocity` (world units per second) is added to a short fall. False when nothing was captured or
// the engine refused; the log says which step.
// `hands` (the Hands actor) is the last-resort spawner.
bool drop(void* weapon, void* hands, const float velocity[3]);
// The last drop was the pistol's ROUNDS (PI_AmmoType by name), not the visible part itself: the gun's
// own cylinder still has to fall out (briefly - it is drawn in the hands' layer, never on the floor).
bool dropped_rounds_only();
// The actor of the last drop that flew (the Havok copy), or null. Only while it is checked alive (alive()).
struct Prop {
    void* actor = nullptr;
    void* cls = nullptr;     // its class and name as made, and the level it was made in: a prop whose
    int32_t name = -1;       // memory no longer reads back the same (a level change freed it) is dead
    void* level = nullptr;
};
Prop last_dropped();

// 2026-10-20, the spare magazine in the hand: a prop like the drop's - the weapon's visible magazine's mesh
// and size - with its physics off, moved by hand each frame (move), until it is let go (release: a drop,
// thrown) or goes into the gun (destroy). Game thread.
Prop make_held(void* weapon, void* hands);
bool alive(const Prop& p, void* weapon); // the same actor, in the weapon's level
void move(const Prop& p, const float loc[3], const int32_t rot[3]);
bool release(const Prop& p, const float velocity[3]);
void destroy(const Prop& p);
bool location(const Prop& p, float loc[3], int32_t rot[3]);
// The weapon's visible magazine now: where it is, its turn and size. False with none (or no mesh).
bool ammo_model_pose(void* weapon, float loc[3], int32_t rot[3], float* scale);

} // namespace bvr::b1r::dropped_part
