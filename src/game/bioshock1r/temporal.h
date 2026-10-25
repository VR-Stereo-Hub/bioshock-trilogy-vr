#pragma once
// BioShock 1: NVIDIA DLAA / DLSS Super Resolution on each eye, and the alternate frame warp
// (AFW). The Dishonored VR mod's process (docs/bioshock1/TEMPORAL.md), on BioShock's own
// scene depth and cameras:
//
//   1. Depth. The draw tap (core/gfx/frame_inspector) follows the game's depth binding;
//      the backbuffer-size D24 surface that took the most DrawIndexed calls in an eye's
//      interval is copied before it is cleared or unbound (ENGINE_NOTES "D3D11 frame map").
//   2. Cameras. CalcView publishes the final camera of every build (note_camera), so each
//      eye image is paired with exactly the camera it was rendered with.
//   3. Guides. temporal_gpu turns depth + the eye's two cameras into camera motion vectors.
//   4. DLSS. The x64 NGX helper (core/gfx/dlss_client, src/tools/dlss_host) reconstructs
//      each eye with its own history; the result replaces the backbuffer copy into that
//      eye's swapchain (core's eye-processor seam). Super Resolution renders at the game's
//      resolution below the selected output and keeps the eye swapchains at that output.
//   5. AFW. AlternateEye renders one eye per present; afw_gpu rebuilds the other eye at the
//      same instant from both eyes' images and depths (core's held-rebuild seam), instead
//      of leaving it to the compositor's rotation-only reprojection.
//
// Everything is OFF by default: with Upscaler Off and AFW off the module registers no
// seam and BS1 renders exactly as before. Live switches: F10 Display > "Anti-aliasing
// and upscaling" and "Stereo rendering" (alt_eye.h); seam words `dlss ...` and `afw ...`.

#include "game/bioshock1r/menu_model.h"
#include "game/shared/ue_math.h"

namespace bvr::b1r { struct FrameContext; }

namespace bvr::b1r::temporal {

// Adapter init (game thread): reads nothing from the engine; arms nothing while off.
void init();
// Apply the latest mode/output intent once no live resize is in flight. Game thread.
void tick_resolution();
// 2026-10-25: the draw tap armed whether or not DLSS/AFW is on (gun_flip.cpp rides it). Idempotent.
void arm_draw_tap();

// Game thread, CalcView of the RENDER VIEW only (scenedraw::in_gameplay_build - one per
// frame; the engine's other CalcView calls are not frames): the FINAL camera of the image
// being made (after every offset). eyeSign: -1 left, +1 right, 0 mono. Under AFW each call
// also names the image for the eye source, in presentation order.
void note_camera(int eyeSign, const bvr::ue::FVector& loc, const bvr::ue::FRotator& rot,
                 const FrameContext* tracking = nullptr);

// Game thread (alt_eye.h owns the stereo method): arms or disarms AFW's core seams - the
// AlternateEye path, the eye source and the held-eye rebuild.
void set_afw(bool on);

// `dlss on|off|dlaa|sr|quality <0-4>|model <0-3>|retry|status|audit on|off|near <uu>|far <uu>|body <m>`
// `afw tint on|off|stale on|off|grid <n>|status` (`afw on|off` is alt_eye's)
void handle_command(const char* verb, const char* args);

// Status lines for the F10 Display tab (render thread, inside the menu's ImGui frame).
void draw_status();

// The AFW capture (Dishonored's `afw dump`, F10 Diagnostics): `presents` consecutive rebuilds
// (1..32), starting delayMs from now, written to <data_dir>\dumps\afw-<stamp>\ - per rebuilt
// present both sources' colour, depth and hands mask, the rebuilt image, and a text header
// with both cameras, the target, every lever and the sizes. tools\afw-replay.ps1 reads it
// back, re-runs the production rebuild with any lever changed, writes viewable images and
// scores each rebuild against the next native frame of that eye. Game output: local only.
// Each captured present stalls the frame on its readbacks (~120 MB at 2750x2850).
void request_dump(int presents, uint32_t delayMs);
// True while the in-image laser (laser_draw.h) was drawn into an eye image in the last 250 ms;
// aim.cpp then keeps the XR laser and reticle quads off. Any thread.
bool laser_drawn_recently();
const char* dump_status();

// Lifetime counters (render thread writes; read for status and the integration test).
struct Telemetry {
    uint64_t eyeFrames = 0, dlssFrames = 0, taaFrames = 0, noDepth = 0, noCamera = 0;
    uint64_t depthCopies = 0;
    uint32_t lastWorldDraws = 0;     // draws in the depth pass the last eye image used
    uint32_t lastFgDraws = 0;        // draws in the foreground (hands) pass it used, 0 = none
    uint64_t afwRebuilds = 0, afwWithHeld = 0, afwFallbacks = 0;
    uint64_t afwForegroundMotion = 0; // rebuilds carrying held foreground through player locomotion
    int helperState = 0;             // 0 idle, 1 starting, 2 up, 3 failed, 4 stopping
    uint64_t probePairs = 0;         // hand-depth probe: native pairs read back, ever
    double probeHands = 0, probeWorld = 0;   // its lifetime medians (0 = no samples)
};
Telemetry telemetry();

// Diagnostics: the guides of the last eye image processed (render thread; borrowed).
struct Guides { void* motion; void* linearZ; void* dlssDepth; void* hands; };   // ID3D11Texture2D*
Guides debug_guides();

// F10 (menu_backend): NaN / false = not this module's setting.
float menu_read(menu::Setting id);
bool menu_write(menu::Setting id, float value);

} // namespace bvr::b1r::temporal
