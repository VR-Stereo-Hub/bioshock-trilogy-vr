#pragma once
// s57 (I8): the MELEE window - the fix for the two-part melee regression.
//
// The evidence (s57 probe boot): melee (pad Y / key V) dispatches through the
// SAME fire seam as gunshots with weapon=NULL (the weapon-param discriminator
// is dead), so s50/s51's fireglue treats every melee press as a shot: the
// 1500 ms full-hand ready substitution FREEZES the authored swing, and the
// +1.2 s ready capture BANKS a mid-swing pose ("ready pose captured" logged
// 1.2 s after a V press, left anchor quat 0.933/-0.252/...), poisoning the
// bank for later gunshots. The execution mini-cutscene registers as a cine
// hold, where the default force-hide eats the game's authored execution hand
// (the exact s53 ENGINE_NOTES warning).
//
// The discriminator: the Y-press edge from the mod's OWN composed pad
// (bvr::input::last_composed_buttons) - causally upstream of the melee chain
// (Touch Y -> XINPUT_GAMEPAD_Y -> XPerformDedicatedMeleeAttackNonBlocking).
// A fire-seam dispatch within kYAssocMs of a Y edge (or inside a live melee
// window) is melee. The raffle skyhook QTE stays byte-identical: a dispatch
// with cine::hold() already OPEN is never classified (the QTE press arrives
// mid-hold; the execution's hold only opens AFTER the press).
//
// The window (kWindowMs, refreshed on repeat presses):
//  - fireglue is skipped AND any live fire window/pending capture from a
//    preceding gunshot is cancelled (mixed combat must not freeze the swing);
//  - the per-hand empty hides release (vigor-only: the swing hand must show);
//  - if a hold OPENS inside the window (the execution), the hide gate
//    releases rig-wide until the hold closes + a short tail - the authored
//    execution hand SHOWS;
//  - mode `release` additionally releases the bone drive so the authored
//    swing plays 1:1 (headset A/B alternative; `glueskip` - the exact
//    pre-s50 state the user judged good - is the default).

#include <cstdint>

namespace bvr::bsi::melee {

// Game thread, camera dispatch cadence, BEFORE hide::tick (the hide gate
// reads this tick's release verdict). Tracks the Y edge + window/hold state.
void tick(uint64_t nowMs);

// Fire-seam callback (fire.cpp, player-gated dispatch). Returns true when the
// dispatch is melee-classified AND the window path handled it - the caller
// must then SKIP bones::note_player_fire(). holdOpen = cine::hold() at
// dispatch; a dispatch during an already-open hold is never classified.
bool classify_dispatch(uint64_t nowMs, bool holdOpen);

// hide.cpp: true while the hide gate must stand down (window live or the
// execution hold release, incl. its tail).
bool hide_release();

// hands.cpp: true when the bone drive for this hand must release (mode
// `release` only, while the window is live).
bool drive_release(int hand);

// hide.cpp: the hand to bone-hide during the SWING (s57b headset verdict:
// the melee anim also articulates the GUN arm, which lurches through the
// compose - the user wants it gone for the swing). Returns 1 (right) for
// the first ~0.9 s of the window, -1 otherwise - and ALWAYS -1 during the
// execution release (the authored execution shows everything).
int swing_hide_hand();
bool hide_gun();
void set_hide_gun(bool on); // config key meleeHideGun

// Persisted mode (0 off | 1 glueskip | 2 release); config key meleeFixMode.
int mode();
void set_mode(int m);

bool handle_command(const char* cmd, const char* args); // bsimelee
void draw_debug_ui(); // nested in the HANDS + MODEL F10 section

} // namespace bvr::bsi::melee
