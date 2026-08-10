#pragma once
// s52 (I9): the cinematic gate + head radio.
//
// Infinite has TWO cinematic classes and only ONE needs a detector:
// - Bink FMV (attract, credits, reels): the camera seam goes SILENT while
//   presents continue, so every drive stops writing by construction (hands,
//   aim, fire all hang off camera-driven ticks) and core's stale-publish leg
//   quads the movie. Nothing to gate.
// - Engine Matinee: the camera KEEPS dispatching with an authored view
//   target, so the hand drive would fight the authored animation. THIS is
//   what the detector catches: a ~2 Hz game-thread poll of the PC's
//   GetViewTarget (ProcessEvent by name, GNames 17299) - the moment the view
//   target's class stops being the pawn/XCamera pair and reads as a Matinee/
//   cinematic camera, the hold opens.
//
// Consumers of hold():
// - hands.cpp folds it into `want` (release flows through the per-hand
//   g_wasDriving edge INSIDE the hands tick - the BS2 s29 lesson, never from
//   the cine edge itself);
// - aim.cpp gates the ray substitution and the laser/dot publishes;
// - fire.cpp gates the origin substitution;
// - camera.cpp's live drive lane consults the HEAD RADIO: "head look"
//   (default - the additive drive stays on, proven perfect in the
//   stereo-only build) vs "fixed head" (drive suspended, authored camera
//   untouched, stereo eye offsets still apply);
// - the flourish chord suspends during a hold so face-A reaches interactive
//   prompts (the raffle lesson).
//
// `bsicine force on|off` fakes a hold for flat edge testing - the sim cannot
// conjure a Matinee on demand.

#include <cstdint>

namespace bvr::bsi::cine {

void tick(uint64_t nowMs); // game thread (camera detour block)
bool hold();               // a Matinee-class hold (or a forced one) is open
bool head_look();          // radio: true = additive head look during holds
void set_head_look(bool on);

bool handle_command(const char* cmd, const char* args); // bsicine
void draw_debug_ui();

} // namespace bvr::bsi::cine
