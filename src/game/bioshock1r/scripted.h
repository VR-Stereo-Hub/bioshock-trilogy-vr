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

// MASTER SWITCH. With it off nothing here reads engine memory and nothing is
// applied to the camera, so the whole module can be taken out of the frame in a
// headset without a rebuild. That is the cheapest way to answer "did this cause
// it?" - one toggle instead of a bisect across builds and sessions.
bool enabled();
void set_enabled(bool on);

// Game thread, from CalcViewDetour, EVERY call - the comfort accumulators are
// advanced here precisely because this is the one entry point that never stops
// running. `playerController` is CalcView's own `self`, `gameYawUnits` is the
// engine's own yaw before anything touches it, `turnStickX` is the turn axis.
void observe(void* playerController, int gameYawUnits, float turnStickX);

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

// ---- the gameplay rotation freeze ------------------------------------------
//
// RETRACTS s64 part 1. That session measured the head drive overwriting
// rot->pitch and rot->roll ABSOLUTELY and concluded BRVR's
// FreezeGameplayRotation was redundant here. It is not: yaw is composed as
// `gameYawUnits + residualUnits`, so the engine's own yaw reaches the view
// untouched, and screenshake and the auto-pan onto enemy groups both arrive
// through it. Reported from a headset 2026-08-22 and fixed here.
bool freeze_game_rot();
void set_freeze_game_rot(bool on);
float freeze_bleed_deg_per_sec();
void set_freeze_bleed_deg_per_sec(float d);
bool freeze_holding(); // the game yaw is being held back RIGHT NOW

// True when anything actually needs the turn axis this frame. The accessor that
// supplies it takes the input mutex, and this runs on the game thread every
// CalcView, so the caller skips the read entirely when nothing is listening.
bool wants_turn_axis();

// The exact number of yaw units OUR OWN body transfer just handed to the pawn.
//
// THIS IS WHY LEFT/RIGHT CAME OUT INVERTED. body::on_calcview steers the pawn to
// follow your head, which moves the ENGINE yaw by that amount on the next frame
// - and the freeze, seeing the engine yaw move with the stick centred, dutifully
// absorbed it. So turning your head left moved the view right by exactly the
// amount the body had followed. Pitch was unaffected because the body only ever
// transfers yaw, which is precisely the "up and down is correct" half of the
// report.
//
// Same integer, same discipline as the recenter absorption in camera.cpp: the
// freeze subtracts what the body took before deciding what the game injected.
void note_body_yaw(int movedUnits);

// Live values for the F10 readout, in degrees.
float freeze_offset_deg();
float scripted_turn_deg();

// ---- turning yourself during a scripted scene ------------------------------
//
// A scripted sequence pushes NullInput, so the game DISCARDS stick input and a
// turn routed through the game does nothing. This is a mod-side accumulator
// applied to the CAMERA ONLY - never to Controller.Rotation, which is BRVR's
// invariant 1 ("the write itself is the damage").
bool scripted_turn();
void set_scripted_turn(bool on);
float scripted_turn_deg_per_sec();
void set_scripted_turn_deg_per_sec(float d);

// ---- the scene turns you, so hand your own turning back --------------------
//
// Ported from BRVR's ScriptedRecentre. Without it a scene's authored rotation
// lands ON TOP of however far you turned yourself, so a scene that means to
// point you at a doorway points you at the doorway plus your own offset - and
// the more you looked around during it, the more wrong the framing.
//
//   0  off  - scene rotation lands on top of your own turning
//   1  wash - spend |d| of your offset for every d the scene turns (the default,
//             and BRVR's shipped value; proportional, so a big authored turn
//             takes all of it back and a nudge takes a nudge)
//   2  drop - the whole offset goes the moment the scene first turns you
//
// Only ever REDUCES the accumulator toward zero, so it can neither add rotation
// nor overshoot past centre. Preset key `scriptedRecentre`.
int scripted_recentre_mode();
void set_scripted_recentre_mode(int m);

// The total yaw the camera should be adjusted by this frame: the freeze offset
// it is declining, plus the player's own turn during a scripted scene.
//
// APPLY IT UNCONDITIONALLY, not inside the head-drive block. Both halves have to
// work while the GAME owns the camera, which is when that block does not run.
int yaw_adjust_units();

// ---- the rig during a scripted scene ---------------------------------------
//
// Two halves of the same idea: while the game is running an authored moment,
// your hands are not yours.
//
// freeze_hands_in_scenes(): stop the controller driving the bone cluster and
// hand the skeleton back to the engine, so the authored animation plays and you
// cannot drag the rig around mid-scene.
//
// hide_rig_in_scenes(): hide arms, hands and weapon while a scene is running and
// NO scripted animation is playing - the forced-move phase, where the game is
// walking you into position and your hands have nothing to do - then show them
// the moment an animation starts, because that animation is what you are meant
// to be watching. Uses DrawScale3D; see bones::set_actor_hidden.
bool hide_rig_in_scenes();
void set_hide_rig_in_scenes(bool on);

// True when the rig should be hidden RIGHT NOW. Game thread.
bool want_rig_hidden();

// ---- the arm hide runs on measured rig MOTION ------------------------------
//
// Round 7 killed the old predicate `scripted_window() && !scripted_anim()`: the
// animation flag is already true on the first frame of the window, so it answers
// "is a sequence running" and never "do the hands have anything to do".
//
// note_hand_motion() takes bones::hand_motion()'s output once per CalcView.
// `have == false` means CANNOT ANSWER and routes to arms VISIBLE.
//
// DrawScale3D IS A SAFE HIDE - the bone array keeps being evaluated behind a
// hidden actor (measured unconfounded, round 11: 3 samples taken while hidden,
// 3 distinct bone positions). Round 10 claimed the opposite from a run where the
// hide was DRIVEN by the motion reading, which guaranteed the correlation it
// then read as causation.
void note_hand_motion(bool have, float smoothed, float raw, int bone,
                      const float pos[3], bool rigHidden, int skelDirty);

// The latched verdict: moving, or moved within arm_hold_ms().
bool hands_moving();
void arm_motion_readout(float* raw, float* smoothed, int* bone, bool* blind);

// Preset keys `scriptedArmMotion` / `scriptedArmHoldMs`.
//
// THE HOLD IS THE LOAD-BEARING ONE. 229 of 336 samples inside scripted windows
// read raw exactly 0.0000 - CalcView fires far above the animation tick rate, so
// most consecutive reads see the same pose. A short hold therefore makes a
// moving rig look still. BRVR ships 300 and ran 4000 in a headset; the measured
// distribution says its live value was right, and 4000 is the default here.
//
// Separate from hold_ms(), which holds the scripted WINDOW open across the
// forced-move/animation gap.
float arm_motion_threshold();
void set_arm_motion_threshold(float t);
int arm_hold_ms();
void set_arm_hold_ms(int ms);

bool freeze_hands_in_scenes();
void set_freeze_hands_in_scenes(bool on);

// The scene hold, in ms. Preset-backed like every other F10 value.
int hold_ms();
void set_hold_ms(int ms);

// Publishes what the interface-screen detector saw, and whether a scene claimed
// it, so the F10 readout can explain the PausePC.swf suppression rather than
// leaving it invisible.
void publish_panel_state(bool panelUp, bool suppressed);

// F10 "Scripted events" section.
void draw_debug_ui();

} // namespace bvr::b1r::scripted
