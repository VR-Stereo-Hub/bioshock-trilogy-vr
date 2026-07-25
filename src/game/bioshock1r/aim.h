#pragma once
// M6 decoupled aim: the fire ray follows the CONTROLLERS while the camera
// keeps following the HMD.
//
// The engine's fire path asks four UnrealScript natives for the numbers that
// become a shot (ENGINE_NOTES "Fire flow / aim"):
//   AWeapon::GetPerfectFireStart  -> trace origin
//   AWeapon::ApplyAimError        -> trace direction (perfect dir in, spread dir out)
//   APawn::GetViewPoint           -> eye position (the generic view query)
//   APawn::GetViewDirection       -> view direction
// Each is a `void __thiscall execFoo(FFrame& Stack, void* Result)` thunk, so
// this module hooks them, calls the original, and rewrites `Result` with the
// hand's ray - the same "call the original, adjust the out-params" shape the
// CalcView camera hook uses. AActor::Trace is hooked read-only, as the
// hit-point telemetry that makes flat verification objective.
//
// Everything is command-gated (`vraim ...`) and fail-soft: unresolved natives,
// no hand pose, a scripted (cutscene) camera, or the master switch off all
// leave the engine's own values untouched.

#include "core/hooks/pattern_scan.h"
#include "game/bioshock1r/patterns.h"

#include <cstdint>

namespace bvr::b1r::aim {

enum class Hand { Left = 0, Right = 1 };

// Resolve-time wiring. Nothing is hooked here (see `vraim probe`/`vraim on`).
void init(const bvr::pattern_scan::ProcessImage& image, const patterns::Symbols& symbols);

// Published once per frame by the CalcView drive, on the game thread, AFTER it
// has produced the final camera. The hand rays are built in exactly this
// frame - same recenter pose, same game yaw, same world scale - so the aim ray
// and the camera can never disagree about where the player is standing.
struct FrameContext {
    bool vrDriving = false;   // HMD is driving the camera this frame
    float camX = 0.0f, camY = 0.0f, camZ = 0.0f;   // final camera loc, UU (incl. head offset)
    float baseX = 0.0f, baseY = 0.0f, baseZ = 0.0f; // camera loc BEFORE the head offset
    int32_t camPitch = 0, camYaw = 0, camRoll = 0;  // final camera rot, 65536 units/turn
    float driveYawOffsetRad = 0.0f; // yaw the head drive added on top of the game yaw
    float recenterYawRad = 0.0f;    // XR yaw at recenter
    float recenterPx = 0.0f, recenterPy = 0.0f, recenterPz = 0.0f; // XR meters at recenter
    float worldScale = 50.0f;       // UU per meter
    void* viewActor = nullptr;      // *view_actor out-param (cutscene guard)
    void* pc = nullptr;             // the PlayerController the hook fired on
};
void on_calcview(const FrameContext& ctx);

// Seam command handler: args after the "vraim" verb (game thread).
//   on | off | status
//   probe on|off            install/enable the seam hooks in telemetry mode
//   dump <n>                log the next n calls per seam in full detail
//   test l|r <yawDeg> <pitchDeg> [holdMs]   synthetic hand aim (view-relative)
//   test clear
//   origin on|off           hand origin (default) vs the engine's own origin
//   seam <firestart|aimerror|viewpoint|viewdir> on|off
void handle_command(const char* args);

// Overlay section (render thread).
void draw_debug_ui();

// True while any seam hook is installed and enabled.
bool hook_live();

// True while substitution is armed (master switch + a usable hand ray).
bool active();

} // namespace bvr::b1r::aim
