#pragma once
// I6 rung 2: the live lens decoder (session 41).
//
// Consumes RAW 80-byte constant-buffer samples from core's opt-in
// UpdateSubresource tap (frame_inspector::set_cb_upload_tap - the Map/Unmap
// watch never sees this engine's uploads) and decodes them by the UE3 matrix
// law (patterns.h "the lens"): row-major 4x4 at float 0, tanH = |c3|/|c0|,
// tanV = |c3|/|c1|, object scale cancelling. Everything game-specific -
// decode, structural validation, clustering, the vote - lives HERE, not in
// core; core only captures bytes.
//
// The vote (the BS1 lesson, ROADMAP I6): stride-sampled, majority-voted,
// structurally validated, MULTI-lens. A sample is valid only if the matrix is
// orthogonal AND tanH/tanV matches the live backbuffer aspect (the
// load-bearing filter - degenerate matrices outvoted the truth in the s36
// census). Valid samples cluster by tanV; a round publishes only when the
// top cluster holds a clear majority, names the runner-up as a second lens
// (viewmodel suspect) rather than folding it in, and REFUSES the round
// otherwise - a refused round keeps the last published lens with its age
// showing, it never invents a value.
//
// Default mode is AUDIT: the decoder compares its winner against the claim
// (camera::claim_tan_v) and goes loud on mismatch; it never writes. `bsilens
// track on` lets a majority round write the claim - and the FOV lever still
// wins while armed, because publish_projection_claim derives the claim from
// the lever AFTER this module's tick.

#include <cstdint>

namespace bvr::bsi::lens {

// Game thread, called from the camera detour tail. 1 Hz internally: drains
// the tap ring, closes a voting round, publishes/refuses, audits the claim.
void tick(uint64_t nowMs);

// `bsilens on|off|track on|off|status`. Returns false when not ours.
bool handle_command(const char* cmd, const char* args);

// The last published (majority) lens. False until the first majority round.
bool primary(float* tanH, float* tanV, uint64_t* ageMs);

// Overlay section.
void draw_debug_ui();

} // namespace bvr::bsi::lens
