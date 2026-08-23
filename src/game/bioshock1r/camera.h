#pragma once
// PlayerCalcView hook for BioShock 1 Remastered: per-frame camera telemetry,
// debug location/rotation offsets, wobble test, and the live FOV override.

#include "core/vr/openxr_runtime.h"

#include <cstdint>

namespace bvr::b1r::camera {

// MinHook hook on eventPlayerCalcView; enables itself. False = flat mode.
bool install(void* eventPlayerCalcView);

bool hook_live();

// True while the foreground lens match is armed AND writing (session 15):
// the rig renders through the WORLD lens, so the render-lock solve must use
// the world tan scales and k = 1.
bool fg_fov_match_active();

void set_fov_override(float hfovDeg); // <= 0 disables; game value restored

// Session 20 vrrec: the recenter reference + world scale are the xr->game
// mapping's hidden state. The recorder snapshots them into a recording's
// header and restores them on play; set_ also suppresses the pending
// auto-recenter so the first replayed frame cannot re-reference itself.
void get_recenter_state(bvr::vr::HeadPose* pose, int32_t* yawUnits, float* worldScale);
void set_recenter_state(const bvr::vr::HeadPose& pose, int32_t yawUnits, float worldScale);

// BioshockVR.ini `CameraHeightOffset`, in CENTIMETRES with + up, which is
// BRVR's key, name and units. Stored as UU (the unit every slider and the VR
// preset use) by converting once against the CURRENT world scale - so this is
// an ini-time conversion, not a live one, exactly like every other UU-valued
// slider here. At the shipped worldScale of 100 the two units are the same
// number anyway. Returns the UU actually stored, for the caller's log echo.
float set_head_up_cm(float cm);
float head_up_uu();

// Session 21 fg view-sync: the FINAL per-eye camera of the most recent
// SequentialReentry pair (0 = left, 1 = right), stashed after each pass's
// eye offset. The engine's fg scene node ctor runs BEFORE CalcView inside
// each build and so receives the camera one BUILD stale - the OTHER eye's
// camera under SR (the crossed-eye defect, ENGINE_NOTES session 21).
// scenedraw's ctor detour substitutes these instead: correct eye, one PAIR
// stale, both eyes from the same pair (consistent disparity). False when
// no fresh stash exists (stereo off, drive idle > 200 ms, or never driven).
bool driven_eye_cam(int eye, float loc[3], int32_t rot[3]);

// Session 22 cinematic fallback, called from scenedraw's BuildDetour (same
// game thread): scripted cameras bypass eventPlayerCalcView, so the FOV
// write's normal restore path cannot run during them.
//   calcview_silent          true once the normal CalcView pass has been
//                            quiet for more than staleMs (0 = never fired
//                            reads as NOT silent - conservative at boot)
//   restore_game_fov_if_stale  one-shot restore of the FOV option the write
//                            latched; re-arms automatically when CalcView
//                            resumes (the latch lives in CalcViewDetour)
bool calcview_silent(uint64_t staleMs);
void restore_game_fov_if_stale(uint64_t staleMs);

// Full ImGui section: hook status, telemetry, and all debug controls.
// Called from the overlay through IGameAdapter::drawDebugUi().
void draw_debug_ui();


// True when the vrpreset.ini just loaded carried an explicit cineDrive value.
// The adapter uses it to apply BS1's own default only when the user has not
// already chosen - a saved preset must still win once they have.
bool preset_had_cine_drive();
int  preset_version();
} // namespace bvr::b1r::camera
