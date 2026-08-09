#pragma once
// I8 s47: the per-weapon profile SCAFFOLD - structure only, deliberately.
//
// The ROADMAP I9 arsenal save is where per-weapon values get DERIVED (the
// opening hours have no weapon, then the Sky-Hook, then one gun - calibration
// that only holds with a full loadout is not calibration, user directive
// 2026-07-31). Until that save exists: the table is EMPTY, nothing consumes an
// override, and nothing is persisted (preset keys are earned by implemented
// levers, the animTrans precedent).
//
// The KEY is the weapon CLASS NAME, read via reflect::class_name_of (the
// UClass-fixpoint-gated walker) - the only identity that survives weapon
// object churn across switches, saves and level transitions. Burial at Sea
// adds weapons the base game does not have, so a name key also covers DLC
// without index arithmetic.

namespace bvr::bsi::profiles {

// Per-weapon override slots. All future: aim trim P/Y, ray-origin F/R/U,
// model trim/offset/scale deltas - added at I9 WITH their headset-derived
// values, never before.
struct Profile {
    const char* className;
};

// Lookup by class name; nullptr = no override (today: always - the table has
// no entries).
const Profile* find(const char* className);

// The fire seam's player path reports its Weapon param here (game thread).
// Latches the class name once per POINTER change - the object pointer is
// transient (s46), the name is the durable key. A null pointer latches
// "(none)": the seam's optional Weapon param arrives null on ordinary shots
// (measured s47), so the pawn-side current-weapon source is I9 derivation
// work, not a guess here.
void note_weapon_object(void* weaponObj);

// `bsiprofiles` - status only. Returns false when the command is not ours.
bool handle_command(const char* cmd, const char* args);

} // namespace bvr::bsi::profiles
