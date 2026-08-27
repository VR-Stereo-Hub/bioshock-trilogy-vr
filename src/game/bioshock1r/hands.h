#pragma once
// M7 visible hands + weapons: the first-person viewmodel follows the
// CONTROLLER instead of the camera.
//
// The engine draws the player's hands and current weapon as one actor
// (`AHands`, a single mesh carrying the hands plus a short forearm stub) that
// it places relative to the view every tick. This module finds that live actor
// by its class vtable - the same heap scan that locates UShockUserSettings,
// since there is no static pointer to it - and rewrites its Location/Rotation
// each frame from the hand's GRIP pose.
//
// GRIP, not AIM: the grip pose is where the hand physically is, which is what a
// model wants. The aim pose stays with the fire ray (aim.cpp). Both are built
// from the same per-frame transform (frame_context.h) so the drawn gun and the
// fired bullet cannot disagree.
//
// Everything is command-gated (`vrhands ...`) and fail-soft: no actor found, no
// pose, a scripted camera, or the master switch off all leave the engine's own
// placement untouched.

#include "core/hooks/pattern_scan.h"
#include "game/bioshock1r/frame_context.h"

namespace bvr::b1r::hands {

// Resolve-time wiring. Nothing is scanned here - the actor does not exist until
// a world is loaded, so the lookup is lazy and re-validating.
void init(const bvr::pattern_scan::ProcessImage& image);

// Once per frame from the CalcView drive, on the game thread, after the camera
// is final. This is deliberately the LAST thing to touch the viewmodel in the
// frame: the engine places it during its own tick, so our write has to land
// afterwards to win.
void on_calcview(const FrameContext& ctx);

// Seam command handler: args after the "vrhands" verb (game thread).
//   on | off | status
//   mode bones|hands|gun    bones (default, M7-v2) drives the hand-cluster
//                           BONES so the attached weapon follows engine-side
//                           (see bones.h); hands pins the AHands actor
//                           (retired - eye-anchor pivot lever); gun is inert
//                           (the renderer ignores an attached weapon's actor
//                           fields)
//   pose aim|grip           aim = the ray the laser and bullets use (default),
//                           so the barrel agrees with them by construction
//   scale [l|r|both] <f>    per-cluster viewmodel scale about the anchor
//                           (session 61; 1.0 = authored, no side = both;
//                           probe modes via `vrbones scalemode` - see
//                           bones.h set_scale for the s16-dead-end story)
//   wscale <f>              uniform weapon scale (session 61; 1.0 = authored,
//                           lane drops itself) - drives the holdable's OWN
//                           skeleton, animations keep playing (bones.h
//                           set_weapon_scale)
//   probe [n]               log every AHands + player-weapon instance, choose none
//   hand l|r|auto           which controller drives the model (auto = the hand
//                           whose trigger was last pulled; default)
//   pos [l|r] <fwd> <right> <up>  model offset in cm, in the model's final
//                           frame; no side = BOTH hands (legacy form, what the
//                           harness and old scripts use)
//   rot [l|r] <pitch> <yaw> <roll>  mesh-alignment trim in degrees, composed in
//                           the controller's LOCAL frame (holds at any
//                           orientation); no side = BOTH hands
//   hideinactive on|off     collapse the whole INACTIVE hand's cluster while
//                           the other drives (default ON; the weapon-attach
//                           bone hides by translation - see bones.h)
//   fname <index>|weapon    resolve a name index to its string via GNames
//                           (session 20); `weapon` reads the cached weapon
//                           actor's attach-bone FName
//   swaykill on|off|status  freeze the drive's reference against the idle
//                           animation's breathing (default ON; session 20 -
//                           real animations pass the threshold)
//   save | reload           persist / re-read the offsets (per-hand keys in
//                           hands.ini; a legacy suffix-less key loads to both)
//   test <dYaw> <dPitch> [distUU] [holdMs]   camera-relative placement (proves
//                           the write lands; no pose math)
//   simpose <yaw> <pitch> <roll> [holdMs]    synthetic XR controller pose fed
//                           through the REAL mapping path (headset-free check
//                           of the whole transform chain)
//   testclear
void handle_command(const char* args);

// Which controller currently owns the viewmodel (0 left, 1 right): the hand
// whose trigger last fired, or the forced choice. Shared with the aim laser so
// the beam leaves the hand that is actually holding the weapon.
int active_hand();

// Live mesh-alignment trim (degrees, per hand) - read by `vraim synccheck` so
// its model chain sweeps the REAL tuned values (session 20).
// The MODEL GRIP OFFSET, in cm, in the controller's own final frame (fwd along
// the barrel as oriented, then right, then up). This is what makes a weapon
// pivot about its GRIP instead of about its actor origin: with all three at
// zero the model's origin is pinned to the controller and everything else
// swings on a radius as you rotate - reported as "the weapon is connected to a
// circle and the axis is the controller".
//
// PER WEAPON, NOT PER HAND. BRVR tuned the same three numbers per slot and its
// shipped values are nowhere near each other - forward 58 for the wrench, 44
// for the pistol, 16 for the shotgun - because the number IS the model's
// origin-to-grip vector and every model has its own. aim.cpp's weapon profiles
// carry them and push them here on a weapon change; these accessors are that
// seam. The F10 sliders still edit the live value, which is how a profile gets
// its numbers in the first place (it is a visual judgement - BRVR's note is
// blunt that it "cannot be made from a log").
void model_offset_cm(int hand, float* fwdCm, float* rightCm, float* upCm);
void set_model_offset_cm(int hand, float fwdCm, float rightCm, float upCm);

// Per weapon since s65, for the same reason the grip offset is: a residual
// ROTATION error shows up as a POSITION error at the end of the grip lever
// (err ~ r * delta), so it vanishes at whatever orientation the offset was
// tuned in and returns everywhere else. Tuning position alone cannot fix it -
// which is why BRVR's shipped table carries a rotation column per slot, and
// why its pistol needs 8 degrees of yaw.
void set_model_trim_deg(int hand, float pitchDeg, float yawDeg, float rollDeg);

float model_trim_pitch_deg(int hand);
float model_trim_yaw_deg(int hand);
float model_trim_roll_deg(int hand);

// The live actors this module tracks (null until found/learned). The session-20
// muzzle probe inspects the WEAPON's own skeleton, which is produced here every
// frame; game thread only, revalidated by the caller's own reads.
void* hands_actor();
void* weapon_actor();

// Like weapon_actor(), but SCANS when the cache is empty (trigger-learned
// object preferred; else the anchored heap scan, 2 s internal cooldown).
// The session-21 per-weapon profile key source - pre-fire weapons resolve
// too. Game thread; callers gate on gameplay view to avoid cutscene scans.
void* resolve_weapon_actor(const FrameContext& ctx);

// True while a sliced sweep for the weapon actor is mid-flight, i.e. a null
// return from resolve_weapon_actor means "not finished" and NOT "not there".
// Callers with a miss counter must not count the former (session 27). Game
// thread.
bool weapon_scan_in_progress();

// Hands.CurrentHoldable read raw off the rig, CLASS-AGNOSTIC (session 21
// part 3: the MachineGun/GrenadeLauncher native vtable differs from
// kPlayerWeaponVtableRva, so vtable-gated paths rejected them and pinned
// the stale weapon). False = the rig is unknown/unreadable (fall back to
// the legacy paths); true with *out possibly null. The per-weapon profile
// layer's identity source - it validates by CLASS NAME, not vtable.
bool current_holdable(void** out);

// Is a weapon OR a plasmid equipped? Ported from BRVR, which gates its crosshair
// on exactly this. Returns TRUE on every failure path - the consumer is a
// cosmetic suppression and must fail towards the crosshair being shown.
bool armed();

// s68c: the equipped PLASMID actor, from Hands.CurrentAbility. Abilities live in
// their own slot, which is why current_holdable() reads NULL with a plasmid up.
// This is the per-plasmid identity the viewmodel drive needs to notice one
// plasmid replacing another. False when the rig is unreadable; *out null means
// no plasmid equipped.
bool current_ability(void** out);

// Persist the per-hand model offsets to hands.ini (same as `vrhands save`).
// Called by `vrpreset save` too, so the one in-headset save button covers the
// model sliders along with the preset's own values.
void save_offsets();

// s67 view-frame placement (where the gun SITS; cannot create an orbit, unlike
// the grip offset which is the pivot). Per weapon since s67 - the shotgun sits
// differently in the hand from the pistol.
//
// s68: and PER HAND. It was one global triple serving both hands while the
// per-weapon profiles rewrote it from the RIGHT hand on every weapon change, so
// switching to a gun and back left the left-hand PLASMIDS parked at the gun's
// placement with nothing to restore them. hand 0 = left/plasmid, 1 = right/weapon.
void view_offset_cm(int hand, float* fwdCm, float* rightCm, float* upCm);
void set_view_offset_cm(int hand, float fwdCm, float rightCm, float upCm);

// s67: re-apply the viewmodel rotation AFTER the game tick has reset it.
//
// BRVR measured the tick erasing ROLL by 5-102 deg (pitch and yaw held), and
// fixed it by writing the rotator a second time from Present. Verified on this
// machine: BRVR's readback reports r=0.0 all run, and its viewmodel is clean.
//
// Called from scenedraw's build detour at depth 0 - game thread, after
// CalcView, before the frame is built. Same place camera.cpp restores a stale
// FOV from, and for the same reason. Cheap and safe when nothing was written.
void late_write();

// Overlay section (render thread).
void draw_debug_ui();

// True while the viewmodel is being driven.
bool active();

} // namespace bvr::b1r::hands
