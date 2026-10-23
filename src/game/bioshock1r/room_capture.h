// BioShock 1: the room around the flat screen, per level (2026-10-23).
//
// The core keeps a world-yaw-aligned cubemap of the room and shows it under the
// flat screen (core/gfx/cube_snapshot, openxr_runtime "room snapshot"). This
// module is BS1's side of it:
//
//   - the WORLD YAW: every camera frame, the yaw that carries a headset-frame
//     direction into the game world (the pawn's yaw less the recenter yaw) goes
//     to the runtime, so a recenter or a stick turn never moves the room;
//   - ONE ROOM PER LEVEL on disk: %LOCALAPPDATA%\BioshockVR\rooms\<map>.cube.
//     When the map changes (screens::map_name) the live cube is reset and the
//     level's file, if there is one, is loaded as the room shown;
//   - the CAPTURE SWEEP (the F10 "Capture this room" toggle, `vrroom capture`):
//     the hands and the gun are hidden (the scene rig collapse), the camera is
//     driven through 38 directions covering the sphere, each held a few frames
//     and splatted once settled, while the headset shows the cube filling in;
//     then the cube is saved as this level's room and loaded back as the one
//     shown. About ten seconds.
//
// Game thread (CalcView) throughout; the runtime does the GPU and file work on
// the present thread by request.

#pragma once

#include "core/vr/openxr_runtime.h"

#include <cstdint>

namespace bvr::b1r::room_capture {

// Once per CalcView, after the camera drive: the map watch and the sweep.
void on_calcview(bool strictGameplay);

// The sweep's orientation in place of the located head, while it runs.
bool override_head(bvr::vr::HeadPose* hp);

// The pawn's yaw less the recenter yaw, in rotator units, and the camera's
// location (UE units), from the camera drive.
void publish_world_yaw(int32_t bodyYawUnits, const float camLoc[3]);

// The hands and the gun away (hands.cpp's rig collapse) while the sweep runs.
bool hide_rig();

void start();
void cancel();
bool active();

// The circle the loading board is cut to (fractions of the frame's height; 0 = none).
float circle();
void set_circle(float radiusFrac);

// "vrroom capture|cancel|save|load|clear|circle <r>|status"
void handle_command(const char* args);

} // namespace bvr::b1r::room_capture
