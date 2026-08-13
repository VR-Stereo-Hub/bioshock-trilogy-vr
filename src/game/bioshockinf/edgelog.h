#pragma once
// s51: THE EDGE-TELEMETRY LANE - the FOV-edge drift's insurance policy.
//
// The drift ("hand LEFT of view center reads closer, RIGHT reads away, worst
// left, stationary head") has survived three exonerations by measurement: the
// projection split (s49), the compose chain (s50), the eye-tag claim (s50).
// This lane records the ENTIRE chain numerically while the user reproduces
// the symptom in the headset, so the logs alone can locate which stage's
// numbers bend when the perceived depth bends - even on a null result.
//
// `bsicam edgelog on|off|status`. OFF by default; safe to leave on for
// minutes: samples go to an in-memory ring at ~30 Hz (zero I/O, zero
// BVR_LOG per sample), and `off` flushes one TSV to the data dir
// (edgelog-<tick>.tsv) with a fixed header row.
//
// Per sample, the chain stages: the runtime's located per-eye view poses +
// fovs and the submitted per-eye pose tags + claimed fov tangents (core
// EdgeViewSnapshot), the consumed head pose, the written camera (frame
// context base + final rotator + both per-eye cameras), the right hand's XR
// grip pose, both hands' composed model targets (gp), the driven component's
// LocalToWorld rows/translation, and present/second-pass/frame ids.

#include <cstdint>

namespace bvr::bsi::edgelog {

// Game-thread sampler; called from the pass-1 camera detour tail. No-op
// while disarmed (one relaxed load).
void tick(uint64_t nowMs);

// The `bsicam edgelog ...` verb body ("on" | "off" | "status" | "").
void handle_verb(const char* rest);

} // namespace bvr::bsi::edgelog
