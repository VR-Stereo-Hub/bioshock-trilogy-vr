#pragma once
// BS2 aim module. Session 39 stage 1: the DISPATCH PROBE that settles whether
// the fire chain (BeginFiring / UseAbility / InitiateDamage /
// GetPerfectFireStart) is ProcessEvent-visible (-> by-name seam, BS2-native)
// or native-to-native all the way (-> BS1-style impl hooks with fresh RVAs).
// docs/bioshock2/ENGINE_NOTES.md "Fire flow / aim" has the derisk; the seam
// hook itself lands only after the probe's verdict.
//
// Two instruments, both armed by `vraim probe on` and read by
// `vraim probe dump`:
//  - fire-watch: per-name UFunction pointers learned through the existing
//    FindFunctionChecked detour (keyed on the Lane-A FName index globals,
//    patterns::resolve_fire_names), hit-counted in the ProcessEvent detour.
//    A positive is decisive; a negative is not (a name can reach
//    ProcessEvent as a script-linked UFunction* without any native global).
//  - census: EVERY ProcessEvent dispatch's function name index, deduped into
//    a fixed table and printed with GNames text - the ground truth that
//    catches names the fire-watch cannot see. Needs the UFunction name-field
//    offset, which is SELF-DERIVED at runtime: the learned PlayerCalcView
//    UFunction* is scanned for a dword equal to *fnameIndexGlobal (with a
//    zero number dword behind it) - zero layout constants assumed.

// Stage 2 (same session): the SEAM ITSELF - MinHook detours on both impls
// (patterns::resolve_gpfs_impls; weapon body covers the whole weapon family
// incl. the drill, ability body covers the plasmid arc). The detours call the
// original FIRST and then substitute the out-params - BS1's flat-proven
// property, re-proven here with the decal test. Substitution sources, in
// priority order: the synthetic test ray (`vraim test r <yaw> <pitch>`,
// an OFFSET from the live view rotation - the decal proof's lane), then the
// XR hand ray (lands with the frame-context work).

#include "core/hooks/pattern_scan.h"
#include "game/bioshock2r/frame_context.h"
#include "game/bioshock2r/patterns.h"
#include "game/shared/ue_math.h"

#include <atomic>
#include <cstdint>

namespace bvr::b2r::aim {

// Probe master switch. The camera detours read this INLINE (one relaxed load
// per event when disarmed) - ProcessEvent traffic is the whole script engine,
// and the fast path must stay tiny.
extern std::atomic<bool> g_probeArmed;

// Resolve the fire-chain name globals (fail-soft per name), install the
// GetPerfectFireStart impl hooks (telemetry-mode until `vraim on`), and stash
// what the probe needs. Call after camera::install succeeds.
void init(const bvr::pattern_scan::ProcessImage& image, const patterns::Symbols& symbols);

// True while at least one GetPerfectFireStart impl hook is live (the
// adapter's CAP_AIM_OVERRIDE gate).
bool hook_live();

// Called from camera.cpp's detours ONLY while g_probeArmed (game thread).
void probe_findfunc(uint32_t nameIndex, uint32_t nameNumber, void* fn);
void probe_process_event(void* fn);

// Per-frame entry point, called from the camera's CalcView tail on pass 1
// (post drive, PRE eye offset). Builds this frame's hand rays through the
// frame context and publishes the laser + aim dot; the fire seam reads the
// result. Game thread only.
void on_calcview(const FrameContext& ctx, bool strictGameplay);

// 1 Hz probe summary while armed; rides the camera poll lane's maintenance
// tick (game thread, outside hooked calls).
void poll_tick(uint64_t now);

// `vraim <args>` from the command seam. Returns true when consumed.
bool handle_command(const char* args);

// The last built hand ray (session 40, for the vrbones axes instrument):
// false while the ray is invalid or stale. Game thread only.
bool last_ray(int hand, FVector* origin, FRotator* rot);

// Per-hand aim tuning accessors (session 40 round 2, for the F10 panel and
// the vrpreset lane). Atomics throughout - any thread.
float trim_pitch(int hand);
float trim_yaw(int hand);
void set_trim(int hand, float pitchDeg, float yawDeg);
float pos_fwd_cm(int hand);
float pos_right_cm(int hand);
float pos_up_cm(int hand);
void set_pos(int hand, float fwdCm, float rightCm, float upCm);
bool origin_on();
void set_origin(bool on);
float dot_dist_m();
void set_dot_dist_m(float m);
// Per-hand laser/dot enables (session 41 round 3; F10 + preset).
bool laser_hand(int hand);
void set_laser_hand(int hand, bool on);
bool dot_hand(int hand);
void set_dot_hand(int hand, bool on);

// The last weapon object seen at the fire seam (diagnostic ground truth for
// the session-41 holdable derivation). May be stale or dead - never
// dereference without fresh validation.
void* last_weapon_this();

// Per-weapon profiles (session 41): RIGHT-hand aim/model tuning + uniform
// weapon scale, keyed by the holdable's class, weapons.ini-persisted (BS1's
// session-21 shape - see the block comment in aim.cpp for the four rules).
// Ordering contract with the preset (BS1 camera.cpp:934-936 parity):
// load preset values -> note_preset_baseline() -> reapply_weapon_profile().
void save_weapon_profiles();      // chained from the preset save
void note_preset_baseline();      // un-idles the resolver; new profiles seed from it
void reapply_weapon_profile();    // active profile OVER the preset (no stash)
void weapon_key_ui(char* out, size_t cap); // "-" when keyless; any thread

} // namespace bvr::b2r::aim
