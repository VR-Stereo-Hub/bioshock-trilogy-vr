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

// True while substitution is armed (master switch + a usable hand ray).
bool active();

} // namespace bvr::b1r::aim
