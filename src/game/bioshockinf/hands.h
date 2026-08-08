#pragma once
// Hand/viewmodel drive POLICY for BioShock Infinite: which XR pose family,
// per-hand trims/offsets/scale, arms mode, anim mode - and the per-frame
// funnel from the camera drive's FrameContext into bones::drive. BS2's
// hands.cpp in shape; every value fresh.
//
// Split per hand THROUGHOUT (parallel 2-element arrays, hand 0 = left/vigor,
// 1 = right/weapon): each hand drives independently and releases on its OWN
// controller's loss - there is no "inactive hand" to hide on this game, the
// rig always has both.

#include <cstdint>

#include "game/bioshockinf/frame_context.h"

namespace bvr::bsi::hands {

// Called at the pass-1 tail of the camera detour, AFTER drive_view published
// the FrameContext (the model write must never precede the ray basis it has
// to agree with).
void on_view(const FrameContext& fc, uint64_t nowMs);

// bsihands <verb>. Whole-token verbs (the BS2 "off"/"offset" prefix bug rule).
bool handle_command(const char* cmd, const char* args);

void draw_debug_ui();

// Preset plumbing (config KeyDesc getters/setters live in camera.cpp).
float trim_get(int hand, int axis);           // 0 pitch 1 yaw 2 roll (deg)
void trim_set(int hand, int axis, float v);
float offset_get(int hand, int axis);         // 0 fwd 1 right 2 up (cm)
void offset_set(int hand, int axis, float v);
float scale_get(int hand);
void scale_set(int hand, float v);
bool enabled();
void set_enabled(bool on);
int arms_mode();
void set_arms_mode(int m);
bool anim_mode();
void set_anim_mode(bool on);

} // namespace bvr::bsi::hands
