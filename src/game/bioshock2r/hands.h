#pragma once
// BS2 hands POLICY: which controller drives the rig, with what trims and
// offsets. Computes the model pose through the SAME frame context the aim
// ray uses (frame_context.h - the agreement is the whole point) and hands it
// to bones::drive. The aim trim is deliberately NOT applied to the model
// (BS1 shape: the model wears its own trims; the ray wears the aim trims;
// they meet because both ride the same controller pose).
//
// Session-39 scope: the whole rig rides the RIGHT controller (weapon hand).
// The per-hand cluster split (left = plasmid hand, BS2 native dual-wield)
// lands with the bone-name map in session 40.

#include "game/bioshock2r/frame_context.h"

#include <cstdint>

namespace bvr::b2r::hands {

// Per-frame entry point, camera CalcView tail, AFTER aim::on_calcview
// (deliberately last, BS1 ordering). Game thread only.
void on_calcview(const FrameContext& ctx, bool strictGameplay);

// `vrhands <args>`: on|off | status | trim <p> <y> <r> | offset <f> <r> <u> |
// pose aim|grip. Returns true when consumed.
bool handle_command(const char* args);

bool enabled();

} // namespace bvr::b2r::hands
