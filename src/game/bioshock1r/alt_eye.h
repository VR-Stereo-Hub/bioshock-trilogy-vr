#pragma once
// BioShock 1: alternate-eye stereo methods - one eye rendered per game frame.
//
//   Both eyes every frame  SequentialReentry (scenedraw.h): the default, unchanged.
//   Alternate eye (AER)    BRVR's method, ported: left frame, right frame, one XR frame per
//                          pair. The pair is made ONE instant three ways - the right frame's
//                          cameras are the left frame's (pair lock, per CalcView call site),
//                          the world does not advance on the right frame (the frame-delta
//                          clamp: the right frame's delta is carried into the next left
//                          frame, so no time is lost), and both eyes are submitted together
//                          with one head pose (core's SequentialReentry pair path, fed by
//                          vr::sr_push_eye).
//   Alternate frame warp   The Dishonored VR mod's method: every frame is its own XR frame
//   (AFW)                  and the eye not rendered is rebuilt from both eyes' images and
//                          depth (temporal.h / afw_gpu.h). The world clamp is OFF here - each
//                          frame is shown on its own, so freezing half of them would halve the
//                          motion rate (Dishonored's AFW guide 4.1).
//
// The eye is decided ONCE PER FRAME on the game thread, and every CalcView call of that
// frame renders it. The engine calls CalcView about four times a frame (the render view
// inside the scene build plus movement/AI/audio consumers - BRVR measured 4.2), and the
// player scene node is constructed AFTER the build's CalcView. That call commits the
// rendered eye and advances the next eye. The constructor must use the committed eye,
// not the already-advanced next eye (ENGINE_NOTES: tenth-run call-order correction).
// The flip and the temporal tag still happen once per build, before its Present.

#include "core/hooks/pattern_scan.h"
#include "core/vr/openxr_runtime.h"
#include "game/bioshock1r/menu_model.h"
#include "game/shared/ue_math.h"

namespace bvr::b1r::alt_eye {

enum Method { kBoth = 0, kAer = 1, kAfw = 2 };

// Adapter init: stashes the image base. Nothing is hooked until AER is first chosen.
void init(const bvr::pattern_scan::ProcessImage& image);

// Any thread: ask for a method; the game thread applies it at its next CalcView outside the
// scene build (hooks and mode switches never run mid-build).
void request(int method, const char* who);
// The method in force (game thread's view).
int method();

// CalcView, every call, game thread, after the original: applies a pending method change
// (outside a build) and returns the eye this frame renders, -1 left / +1 right, or 0 (no
// alternate-eye method, or not a stereo frame). `stereoView` = strict gameplay, no cinematic.
int frame_sign(bool stereoView);

// Scene-build entry, before the original: invalidate the previous build's latch. A
// scripted/menu build without CalcView must not substitute a retained gameplay camera.
void begin_render_build();

// The eye committed by THIS build's CalcView, with no side effect. The player-scene
// constructor runs afterwards, when frame_sign() already describes the NEXT frame.
// Returns 0 before a commit, for mono, or with an alternate-eye method off.
int current_sign();

// CalcView, right after the live head sample is read: AER uses ONE head sample per pair
// (BRVR latches the HMD pose on the left frame and holds it for both eyes), so everything
// the head drives in the right frame - the camera, the hands' frame, the aim - is the left
// frame's. The first call of a pair stores it; every later call of the pair gets it back.
void latch_head(bvr::vr::HeadPose* hp, int sign);

// CalcView, before the eye offset, on the camera every later offset rides on. AER's right
// frame: replaced by this call site's camera from the left frame of the same pair. AER's
// left frame: stored. Otherwise untouched. `site` = CalcView's return address.
void pair_lock(void* site, bvr::ue::FVector* loc, bvr::ue::FRotator* rot, int sign);

// CalcView inside the gameplay scene build (the render view), game thread, after every
// offset: tags this frame's eye (AER: vr::sr_push_eye) and advances the eye for the next
// frame. sign = the eye this frame rendered (0 = a mono frame: the next pair starts LEFT).
void commit_render_view(int sign);

// The frame-delta hook's whole decision (game thread; the hook converts and forwards):
// the delta the engine object `self` advances by this frame. AER, a pair's right frame:
// 0, the time banked per object; the next frame: its own delta plus the bank. Two
// receiver objects are tracked; a third identity (a load) resets both banks.
float clamp_delta(void* self, float deltaSeconds);

// `aer on|off|status|clamp on|off|lock on|off` and `afw on|off` (temporal.h owns the rest
// of `afw ...`).
void handle_command(const char* verb, const char* args);

// F10: StereoMethod. NaN / false = not this module's setting.
float menu_read(menu::Setting id);
bool menu_write(menu::Setting id, float value);

// Status lines for the F10 Display tab (render thread, inside the menu's ImGui frame).
void draw_status();

// Counters (game thread writes; read for status and the integration test).
struct Telemetry {
    uint64_t frames[2] = {};          // render views tagged LEFT / RIGHT
    uint64_t monoFrames = 0;          // render views with no eye
    uint64_t locked = 0, lockMisses = 0;   // pair-lock substitutions / right-frame sites with no left
    uint64_t frozen = 0, released = 0;     // frame-delta calls held / paid out (AER)
    uint64_t carryDropped = 0;        // implausible carries discarded (a load)
    double interEyeMoveSum = 0, interEyeMoveMax = 0;   // uu the render view moved within a pair
    uint64_t interEyeN = 0;
    bool clampHooked = false;
};
Telemetry telemetry();

} // namespace bvr::b1r::alt_eye
