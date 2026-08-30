#pragma once
// M6 decoupled aim: the fire ray follows the CONTROLLERS while the camera
// keeps following the HMD.
//
// Every shot in this engine starts by asking ONE function where it begins and
// where it points (ENGINE_NOTES "Fire flow / aim"):
//   AWeapon::GetPerfectFireStart        - guns and the wrench (right hand)
//   UAttackAbility::GetPerfectFireStart - plasmids and abilities (left hand)
// Both are C++ implementations called by the matching native InitiateDamage,
// and both fill out-params that the engine THEN puts its own spread on - so
// this module hooks them, lets the original run, and rewrites the out-params
// with the hand's ray. Per-weapon accuracy still applies on top, and the shape
// is the same "call the original, adjust the out-params" the CalcView camera
// hook uses.
//
// Everything is command-gated (`vraim ...`) and fail-soft: unresolved symbols,
// no hand pose, a scripted (cutscene) camera, an AI's weapon, or the master
// switch off all leave the engine's own values untouched.

#include "core/hooks/pattern_scan.h"
#include "game/bioshock1r/frame_context.h"
#include "game/bioshock1r/patterns.h"

#include <cstdint>

namespace bvr::b1r::aim {

enum class Hand { Left = 0, Right = 1 };

// Resolve-time wiring. Nothing is hooked here (see `vraim probe`/`vraim on`).
void init(const bvr::pattern_scan::ProcessImage& image, const patterns::Symbols& symbols);

// Called once per frame by the CalcView drive, on the game thread, AFTER it has
// produced the final camera, with the frame context it just published
// (frame_context.h). The hand rays are built in exactly this frame - same
// recenter pose, same game yaw, same world scale - so the aim ray and the
// camera can never disagree about where the player is standing.
void on_calcview(const FrameContext& ctx);

// s67 CROSSHAIR trim, for the in-headset tuner (BRVR calls it CursorOffset).
// This is the AIM ray: the laser, the dot and the bullet all come off it, so
// moving it moves all three together by construction.
// s70d: PER WEAPON AGAIN. The global crosshair was tried and reverted.
//
// s70 made it global at the tester's request - "the crosshair is a global
// position", the one deviation they wanted from BRVR. Tested, and the report was
// "now my crosshair is way off for all of my weapons".
//
// It is the third independent time this answer has come back the same way:
// s67 tried a global crosshair and recorded that it "does not [serve every gun],
// for the same reason the grip offsets are per weapon"; BRVR itself keys
// cursorRot per slot AND per plasmid, in the config the tester calls perfect;
// and now s70. The seeded table says why in one line - the weapons span 0.83 to
// -6.67 in pitch and -6.70 to -14.70 in yaw, and the plasmids sit at -11.00 and
// +37.00. A single number cannot be right for a set that wide.
//
// If it is wanted global again, the thing to change is not this: give the tuner
// a way to copy one weapon's trim to all of them, so they are all THE SAME by
// choice while each stays individually settable.
void aim_trim_deg(int hand, float* pitchDeg, float* yawDeg);
void set_aim_trim_all(float pitchDeg, float yawDeg);
// s70n: per hand. set_aim_trim_all writes hand 1 whatever you ask, which is why
// the numpad's crosshair mode could never move a plasmid.
void set_aim_trim(int hand, float pitchDeg, float yawDeg);

// Seam command handler: args after the "vraim" verb (game thread).
//   on | off | status
//   probe on|off            install/enable the seam hooks in telemetry mode
//   dump <n>                log the next n calls per seam in full detail
//   test l|r <yawDeg> <pitchDeg> [holdMs]   synthetic hand aim (view-relative)
//   test clear
//   pose aim|grip           the runtime's pointing ray (default) vs the grip axis
//   cal <pitchDeg> [yawDeg] aim trim, degrees; +pitch aims higher
//   laser on|off            visible dots along this ray (M7; XR quad layers)
//   laser <dots> <nearM> <farM> <sizeDeg>   laser shape
//   origin on|off           hand origin (default) vs the engine's own origin
//   seam <weapon|ability> on|off
//   scan <Class> <Func> [n] / scanoff   hook ANY name-based native read-only
//                           (fire-flow investigation without a rebuild)
//   synccheck               sweep ~20 controller orientations (incl. roll)
//                           through the ray AND model pose->rot chains and
//                           print the ray-vs-barrel divergence per pose, for
//                           the live trims and a canonical 10/10 trim
//                           (session 20; the pure chains live in
//                           frame_context.h)
//   muzzle on|off           right-hand ray + laser follow the RENDERED barrel
//                           (bones 43->44 of the per-weapon reference pose) -
//                           per-weapon automatic, no manual trim; default off
//                           until the in-headset verdict (session 20)
void handle_command(const char* args);

// Overlay section (render thread).
void draw_debug_ui();

// True while any seam hook is installed and enabled.
bool hook_live();

// The weapon object the trigger-keyed hand map currently attributes to the
// RIGHT hand - i.e. the equipped gun, once it has fired at least once. Null
// until then, and cleared on world change. Game thread only. The M7 viewmodel
// uses it to drive the gun's own actor.
void* learned_weapon_object();

// True while the ACTIVE per-weapon profile key equals `name` - i.e. that class
// is the equipped holdable (the key is maintained from Hands.CurrentHoldable).
// Session 31's swing gesture gates on weapon_key_is("Wrench"). Game thread.
bool weapon_key_is(const char* name);

// The active weapon profile's key, for logging. Writes "-" when none is
// resolved. Safe from any thread (the name is mutex-guarded).
void weapon_key_name(char* out, size_t count);

// The live aim trim (degrees), PER HAND (0 = left/plasmid, 1 = right/weapon)
// since session 16 part 3 - shared so the laser, the fire ray and the M7
// viewmodel stay one ray. set_trim is the VR-preset load path.
float trim_pitch_deg(int hand);
float trim_yaw_deg(int hand);
void set_trim(int hand, float pitchDeg, float yawDeg);

// The aim-ray ORIGIN offset (cm, in the trimmed ray's frame), PER HAND -
// session 18 part 2. Moves the laser AND the fire origin together (applied
// once at ray build); the viewmodel keeps its own offsets. `vraim pos [l|r]
// <fwd> <right> <up>`; set_pos_offset is the VR-preset load path.
float pos_fwd_cm(int hand);
float pos_right_cm(int hand);
float pos_up_cm(int hand);
void set_pos_offset(int hand, float fwdCm, float rightCm, float upCm);

// Session 22: the laser is OFF by default (user's call - it was a
// calibration tool; profiles are calibrated now). The preset applies the
// persisted vrpreset.ini choice (`laserOn`), so opting back in sticks.
bool laser_enabled();

// Session 29 aim dot (default OFF, persisted as aimDot*/vrpreset.ini). The dot
// is placed from the FIRE-SEAM ray itself, so unlike the laser it cannot drift
// from the bullet - see AimDotConfig.
bool dot_enabled();
float dot_dist_m();
float dot_size_deg();

// True while substitution is armed (master switch + a usable hand ray).
bool active();

// Session 21 per-weapon profiles: persist the R-hand trim/offset map
// (weapons.ini, keyed by weapon class name). Called by `vrpreset save`'s
// chain (one in-headset save button covers everything) and `vraim wsave`.
// Game thread.
void save_weapon_profiles();

// Re-apply the ACTIVE weapon profile over the R atomics without stashing -
// the VR preset's value load runs at arbitrary time vs the first profile
// resolve and must not leave preset baselines over a live profile. Called
// at apply_vr_preset's tail. Game thread.
void reapply_weapon_profile();

// Capture the just-loaded preset R values as the seed for new weapon
// profiles, and un-idle the profile resolver (it waits for a value source
// so a pre-preset resolve can never seed a profile from zeros - the
// headset-run-1 race). Called right after load_vr_preset_values(). Game
// thread.
void note_preset_baseline();

// Wake the weapon-actor scan fallback, which latches dormant after repeated
// misses so it can never resume walking memory on a cadence. Call on an event
// that plausibly created a weapon: entering the gameplay view, a pawn change, a
// level load. `why` is logged. Game thread.
void weapon_scan_rearm(const char* why);

} // namespace bvr::b1r::aim
