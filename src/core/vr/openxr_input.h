// OpenXR action layer: Quest 3 Touch controllers -> the synthetic gamepad.
//
// Owns one action set ("gameplay") with stick/trigger/grip/button/pose
// actions, suggests oculus/touch_controller bindings, and once per frame
// composes an input::Gamepad published to core/input/xinput_bridge. Grip
// poses are created now (session-lifetime spaces) but only consumed from M6.
//
// This header is included ONLY inside openxr_runtime.cpp's BVR_WITH_OPENXR
// block - the runtime passes its private handles at five narrow lifecycle
// points instead of exposing them globally. Mapping table + rationale:
// docs/ARCHITECTURE.md "Controller mapping".

#pragma once

#include <openxr/openxr.h>

namespace bvr::vr {

// After xrCreateInstance: create the action set + actions, suggest bindings.
void input_create(XrInstance instance);

// After session + reference spaces exist: create grip action spaces (session
// children) and attach the action set (once per session - the attach flag
// clears in input_on_session_teardown).
void input_on_session_created(XrSession session, XrSpace baseSpace);

// From session teardown: destroy action spaces, clear the attach flag.
void input_on_session_teardown();

// Once per frame (Present-head, after xrWaitFrame; with pair pacing this is
// once per eye pair == once per game tick): xrSyncActions + read + publish.
// XR_SESSION_NOT_FOCUSED is a success code (actions read inactive) - it
// publishes an inactive pad and must never tear the session down.
void input_sync(XrSession session, XrTime predictedDisplayTime);

// One status line inside vr::draw_debug_ui().
void input_draw_debug_ui();

// M6: latest predicted pose of a hand (0 = left, 1 = right), located in
// input_sync against the app space at the same predicted display time as the
// head pose. `aimPose` picks the runtime's AIM pose - "where this controller
// points", which is what aiming wants - over the GRIP pose, which runs along the
// handle and reads tens of degrees low as a pointing ray (hand rendering wants
// grip). False while that hand is not tracked. Position in meters, orientation
// as a quaternion - XR convention, like HeadPose; the adapter converts to
// Unreal units.
bool input_get_hand_pose(int hand, bool aimPose, float* pos3, float* quat4);

// Session 20 vrrec: sim overlay on the funnel above. While any slot is armed,
// input_get_hand_pose serves the injected poses to ALL consumers (ray, model,
// laser); clear restores the live slots. Game-thread writers.
void input_set_sim_hand(int hand, bool aimPose, bool valid, const float pos3[3],
                        const float quat4[4]);
void input_clear_sim_hands();

} // namespace bvr::vr
