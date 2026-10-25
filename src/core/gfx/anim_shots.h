// Animation snapshots (2026-10-23): a diagnostic that keeps pictures of what the
// headset showed while an animation was playing, so a problem the tester calls out
// ("the hand on the lever looked wrong", "the hand was low") can be looked at
// afterwards instead of guessed from numbers.
//
// While it is on and the game adapter names an animation (set_label, non-empty),
// the left eye's image is copied every kIntervalMs into a small GPU ring at a
// reduced size (kWidth wide); when the animation ends (the label goes empty or
// names another one) the event's frames are read back a few per present and
// written as BMPs, with a text file of each frame's time and label, into
// %LOCALAPPDATA%\BioshockVR\...\dumps\anim-<date>-<time>-<label>\. Only the newest
// kKeepEvents event folders are kept on disk; older ones are deleted as new ones
// are written. Off by default; render thread except set_label / set_enabled.

#pragma once

#include <d3d11.h>

#include <cstdint>

namespace bvr::anim_shots {

constexpr uint32_t kWidth = 640;       // the saved frames' width; height keeps the eye's aspect
constexpr uint32_t kIntervalMs = 150;  // between frames while an animation plays
constexpr uint32_t kMaxFrames = 40;    // per event (6 s); later frames of a long one are dropped
constexpr uint32_t kKeepEvents = 20;   // event folders kept on disk

void set_enabled(bool on);
bool enabled();

// The animation playing now, in words (the adapter's: the hands' state, the gun, the
// reload step); empty or null: none. Any thread.
void set_label(const char* label);

// The eye image just captured (render thread, after the capture): a frame is taken
// when one is due.
void on_eye_image(ID3D11DeviceContext* ctx, ID3D11Texture2D* eyeImage);

// Once per present (render thread): writes a few finished frames to disk.
void service(ID3D11DeviceContext* ctx);

// Events written since start-up (each one is also logged with its folder).
uint32_t events_written();

// Device objects (session teardown).
void release();

} // namespace bvr::anim_shots
