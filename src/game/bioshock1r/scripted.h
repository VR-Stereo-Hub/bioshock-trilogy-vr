// BS1 scripted-event signals - the M7 window machinery, ported from BRVR.
//
// WHAT THIS IS NOT. It is not a cutscene detector. This repo's cutscene
// detector is `wantCine` in core's openxr_runtime.cpp and it is already the
// stronger of the two mods' - BRVR's own docs/CONSOLIDATION.md (2026-08-15,
// written for this merge) records that BRVR shipped exactly one cutscene
// signal, the letterbox bar draw, and never wired a single caller to it. That
// same draw is `bvr::hud::bar_draw_active()` here, consumed, and combined with
// four more signals BRVR has none of. Nothing about that path is touched here.
//
// WHAT THIS IS. A scripted SEQUENCE is a different question from a cutscene:
// the world renders, the HUD may be up, and the game is moving the player
// through an authored moment. BRVR calls the answer its M7 window and this is
// that, ported. It exists so the comfort toggles above it can tell "the game
// is moving you on purpose" from "you are playing".
//
// EVERY OFFSET HERE IS RE-DERIVED, NOT TRUSTED. The numbers came from BRVR and
// are cited in docs/bioshock1/ENGINE_NOTES.md with their derivations, but a
// carried-over offset is a guess until this build agrees with it. So each read
// is gated on an anchor check that uses THIS repo's own name system - which
// BRVR did not have and had to approximate - and every signal fails closed
// with a loud log line rather than reporting a value it cannot stand behind.

#pragma once

namespace bvr::b1r::scripted {

// Game thread, from CalcViewDetour, every call. `playerController` is
// CalcView's own `self`. Cheap: three dword reads once the anchor has locked.
void observe(void* playerController);

// A scripted hand-animation sequence is running - the game is deliberately
// animating the player's hands as part of an authored moment.
//
// hands+0x594 bit 2, CurrentlyExecutingScriptedHandAnimationSequence.
//
// IT DOES NOT COVER the Little Sister rescue or the EVE injection. BRVR
// measured both leaving this bit clear: they are Hands *states*, a different
// mechanism. A build where a rescue fires this has a WRONG OFFSET - it has not
// improved on BRVR. See the verification note in ENGINE_NOTES.
bool scripted_anim();

// The game is interpolating the player into position and heading -
// StartForcePlayerMove, which runs BEFORE a scripted animation begins and for
// the whole of a bathysphere boarding.
//
// controller+0x9E0, bIsForcingPlayerMove. Anything that writes the aim field
// must stand down while this is true or it fights the interpolation.
bool forced_move();

// The player is riding a bathysphere. pawn+0x464 bit 1, Pawn.bCannotFall.
//
// It exists so the gameplay rotation freeze can leave the ride alone: a
// bathysphere is not a scripted animation, so without this the freeze applies
// there too and the camera stops following the sphere. BRVR shipped that exact
// bug for as long as this signal was missing.
bool bathysphere();

// The union of scripted_anim() and forced_move(), HELD open for hold_ms()
// after both drop. ANYTHING ASKING "is a scene running" WANTS THIS.
//
// One scene raises those two in sequence and they normally overlap. When the
// order reversed on BRVR's Little Sister crawl the union went false for a
// single frame at 231 CalcView/s, the aim was released and re-armed inside a
// live scene, and the field finished 18.6 deg off the pawn for the rest of it.
bool scripted_window();

// TRUE once the anchor check has positively identified the hands actor and its
// computed name slots. Every signal above is held false until it does.
bool anchor_ok();

// Level/save load, or the hands actor being destroyed: a held window must not
// cross worlds. Called automatically when the hands pointer changes.
void reset();

// ---- comfort: the game's own rotation, when the game owns the camera -------
//
// WHEN THIS BITES, measured against this tree rather than assumed. During
// ordinary VR gameplay the head drive OVERWRITES rot->pitch and rot->roll
// absolutely (camera.cpp, the `rot->pitch = a.pitchRad * ...` pair), so the
// game's shake, kick and auto-pan never reach the player in those axes - only
// yaw passes, and it passes as `gameYaw + headResidual`.
//
// The authored pitch DOES reach the player in exactly one case: when the head
// drive does not run at all and CalcView's rotator passes through untouched.
// That is a scripted or cinematic camera taking the view. It is the case the
// comfort complaint is about, and it is the only case this policy touches.
enum class RotFollow { Both = 0, HorizontalOnly = 1, Neither = 2 };
RotFollow rot_follow();
void set_rot_follow(RotFollow m);
const char* rot_follow_name(RotFollow m);

// Call once per CalcView, AFTER the head drive has had its chance.
//
// `gameOwnsCamera` is that drive's own verdict inverted: true when the mod did
// not write the rotator this frame. `sceneActive` narrows that to the cases
// worth acting on - a cutscene or a scripted event - because a MENU also takes
// the camera and holding a menu's framing is both pointless and a source of
// log noise on every inventory open.
//
// A no-op at RotFollow::Both, which is the default and is bit-for-bit today's
// behaviour.
void apply_rotation_policy(bool gameOwnsCamera, bool sceneActive, int* pitch, int* yaw,
                           int* roll);

// The scene hold, in ms. Preset-backed like every other F10 value.
int hold_ms();
void set_hold_ms(int ms);

// F10 "Scripted events" section.
void draw_debug_ui();

} // namespace bvr::b1r::scripted
