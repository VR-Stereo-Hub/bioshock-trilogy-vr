#pragma once
// The first-person skeleton drive for BioShock Infinite (UE3 6829) - the
// MECHANISM half (hands.cpp is policy). BS2's bones.cpp in SHAPE, rebuilt on
// this game's own derivations (patterns.h "The first-person rig"); no number
// and no code crossed over.
//
// What is deliberately DIFFERENT from BS2, each because of a measured fact:
//  - The write target is USkeletalMeshComponent::SpaceBases (component-space
//    32-byte FBoneAtoms), reached through the live LocalToWorld inverse - not
//    an actor-relative Havok bank. L2W follows the HEAD (the attachment rides
//    the camera), and composing through its inverse per frame is exactly what
//    cancels the head-coupling.
//  - Clusters are DERIVED per resolve from RefSkeleton's parent map (the
//    subtree under L_Grip / R_Grip) instead of baked index ranges: the rig
//    publishes its own structure, and hiding R_Grip proved the weapon rides
//    the grip subtree, so the holdable comes along for free.
//  - The engine restamps EVERYTHING, scale included, at tick cadence even
//    while auto-paused (s45b poke oracles). So: adoption takes whole atoms
//    (guarded by the memcmp-vs-our-write rule so we never adopt ourselves),
//    release is just "stop writing" (the next tick restores truth), and BS2's
//    never-adopt-scale row-pinning has no purchase here.
//
// What transfers unchanged because the reasons transfer:
//  - adopt-then-compose ("the restamp is INPUT, not an enemy"): q_i = qtc (x)
//    animQ_i with qtc = conj(q_component) (x) q_target - authored/animated
//    local frames are PRESERVED and rotated, never replaced (BS2's ~81.6 deg
//    constant-offset lesson).
//  - the quat-norm sanity guard on every adopt (torn reads).
//  - pass 2 of the stereo doubling replays the written bank VERBATIM (BS1's
//    live-proven one-eye-engine/one-eye-ours rivalry).
//  - every engine write sits behind identity gates (vtable + owner + array
//    triple + is_memory_valid); a failed gate drops the rig rather than
//    writing through a stale pointer (BS1's freed-skeleton save-load hang).

#include <cstdint>

#include "game/bioshockinf/frame_context.h"

namespace bvr::bsi::bones {

// Arms handling for the non-cluster arm-chain bones. Plain ints because the
// F10 radio binds one and the preset stores a float.
//  0 = game: never written (engine animates them against the OLD hand pose)
//  1 = follow: composed rigidly in the hand's frame (default - BS2 verdict)
//  2 = hide: collapsed onto the driven grip AND zero scale (the collapse is
//      what kills the skin-web; zero scale alone stretches vertices from the
//      driven wrist to the hidden bone's authored spot - BS2 s41)
// Leaving 1/2 needs no explicit restore on THIS game: the engine restamps the
// whole atom (scale included) on the next tick - measured, not assumed.

// Resolve/refresh the rig lazily (camera 1 Hz throttle calls this). Fail-soft.
void tick_resolve(uint64_t nowMs);

// Drive one hand's cluster (and its arm chain per armsMode) toward the
// game-space target. Pass-1 game thread only. Returns false when the rig is
// not resolved or the identity gates refused.
//
// s46 additions:
//  - hideStyle (armsMode 2 only): 0 = collapse all four arm bones onto the
//    grip (the s45b behaviour), 1 = keep the forearm-twist bones arm21/arm22
//    driven and collapse only arm1/ArmParent, 2 = collapse onto a point ~10 cm
//    BEHIND the grip along the controller's forward.
//  - wristDeg: per-call ARM-RELATIVE wrist adjustment {pitch, yaw, roll} in
//    degrees, an extra quat in the ARM chain's compose only, about the grip -
//    hand cluster, aim and laser untouched (headset finding 5's mechanism:
//    the hand-vs-forearm RELATIVE angle is the adopted engine pose, which no
//    rigid model trim can change). nullptr or zeros = byte-identical compose.
bool drive(const FrameContext& fc, const GamePose& target, int hand, float scale,
           int armsMode, bool animMode, int hideStyle, const float wristDeg[3]);

// Stop driving one hand (or both when hand < 0). Clears masks; no restore
// write - see the restamp fact above.
void release(const char* why, int hand);

// Pass-2 verbatim repaint of everything the last pass-1 drive wrote (100 ms
// staleness gate). Called from the camera detour's second-pass fork.
void reapply();

// World/possession change: drop the resolved rig entirely and re-resolve.
void on_world_change(const char* why);

bool handle_command(const char* cmd, const char* args);
void draw_debug_ui();

// For hands.cpp's status line and the flat sweep: the last written model
// location/rotation per hand (game space), plus drive/adopt counters.
bool last_write(int hand, FVector& loc, FRotator& rot, uint32_t& drives, uint32_t& adopts);

} // namespace bvr::bsi::bones
