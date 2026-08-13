#pragma once
// I8 s47 scaffold -> s52 THE PER-WEAPON PRESETS (ROADMAP I9, pulled forward).
//
// The KEY is the weapon's ARCHETYPE NAME, not its class: every live weapon and
// vigor on this build is literal class XWeapon (measured s52 - class_name_of
// answers "XWeapon" for the pistol, the shotgun and both vigors alike); the
// durable identity is the ObjectArchetype at UObject+0x24 (PistolFounder,
// Plasmid_EnrageFounder, ...). Archetype names survive object churn across
// switches, saves and level transitions, and DLC weapons arrive as new
// archetypes, so the name key covers Burial at Sea without index arithmetic.
//
// IDENTITY SOURCE (derived s52): AXPawn::GetEquippedWeapon(int selector) via
// ProcessEvent - selector 0 answers the GUN hand (right), selector 1 the
// VIGOR hand (left); return lands at parms+4. The fire seam's Weapon param is
// NULL on ordinary shots (s47) and is NOT the identity source.
//
// WHAT A PROFILE STORES: one hand's full lever set - aim trim P/Y, ray origin
// F/R/U (aim.cpp atomics) and model trim P/Y/R, offset F/R/U, scale
// (hands.cpp atomics). Gun profiles drive the RIGHT hand's levers, Plasmid_*
// profiles the LEFT - this game never crosses them (right = gun, left =
// vigor, always).
//
// THE TUNING WORKFLOW (auto-capture): tick() polls both identities ~1 Hz on
// the game thread; on a key CHANGE it captures the live levers into the
// OUTGOING key's entry, then applies the INCOMING key's entry if one exists -
// so "hold the weapon, move the F10 sliders, switch away" IS the save. A key
// with no entry leaves the levers untouched (the empty-profile path is
// byte-identical to the pre-profile build - the s52 flat gate).
//
// PERSISTENCE: %LOCALAPPDATA%\BioshockVR\bsi\weapons.ini, flat
// `<Archetype>.<lever>=%.4f` lines, written on capture and `bsiprofiles
// save`, loaded at adapter init. A separate file from vrpreset.ini so older
// builds and the byte-identical-restore rule are untouched.

#include <cstddef>

namespace bvr::bsi::profiles {

// Load weapons.ini (adapter init; fail-soft, missing file = no entries).
void init();

// ~1 Hz identity poll + capture/apply (game thread - the camera detour's
// throttle block). No-op until the reflection gates open.
void tick();

// The fire seam's legacy latch (s47) - kept as status-line context only.
void note_weapon_object(void* weaponObj);

// s52 round 3 (headset verdict): when a hand holds NOTHING, release the
// controller drive for it entirely - the game's authored arms play instead
// (which also kills the double hands in every scripted intro scene and the
// weird bare-hand rotation in one stroke). Default ON; F10 checkbox +
// `handsHideEmpty` preset key.
bool hide_empty_hands();
void set_hide_empty_hands(bool on);
// True when the identity poll last read that hand as empty (hand 0 = L /
// vigor side, 1 = R / gun side). Game thread.
bool hand_empty(int hand);

// `bsiprofiles` - status | list | save | clear <key>|all | apply. Returns
// false when the command is not ours.
bool handle_command(const char* cmd, const char* args);

// Overlay section (render thread - reads snapshots, posts nothing).
void draw_debug_ui();

} // namespace bvr::bsi::profiles
