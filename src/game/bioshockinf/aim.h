#pragma once
// I7 aim lane (session 44): decoupled aim for BioShock Infinite.
//
// THE SEAM, derived this session and recorded in ENGINE_NOTES with its method:
// `APawn::GetBaseAimRotation` is a VIRTUAL at pawn vtable +0x2E8
// (patterns::kPawnGetBaseAimRotationVtblOffset), signature
// `FRotator* __thiscall (FRotator* retBuf)` - ONE stack arg, `ret 4`. Its body
// is stock UE3: with a controller it delegates to the CONTROLLER's own vtable
// slot +0x2F4, and without one it copies the pawn's Rotation at +0x50 (with the
// RemoteViewPitch `[pawn+0x235] << 8` fixup that identifies the function beyond
// doubt).
//
// That delegation is the whole problem this lane exists to fix. The I4 camera
// drive writes head yaw as a residual on the GetPlayerViewPoint OUT-PARAM only,
// deliberately, so the engine's own rotation keeps belonging to the engine -
// which means the CONTROLLER's rotation, and therefore the pawn's aim, stays
// where the BODY faces. Turn your head and the shot does not follow. This is
// derived from the disassembly, not inferred from behaviour.
//
// Shape is BioShock 1/2's ("call the original, then adjust the out-param"),
// numbers and addresses are neither's - different engine, derived fresh.
//
// SHIPS AS A PROBE. `bsiaim probe on` installs the hook in telemetry mode and
// REFUSES to substitute, so a diagnostic cannot change what it measures; the
// write is a separate, explicit `bsiaim on`.

#include <cstdint>

namespace bvr::bsi::aim {

// Install/remove the seam hook. Lazy: the implementation is read off a LIVE
// pawn's vtable rather than from a static RVA (it is a virtual), so this can
// only run once a pawn exists. Called from the camera detour's throttled tick.
// Returns false and logs the refusing gate on any failure.
bool try_install();

// `bsiaim ...`. Returns false when the subcommand is not ours.
bool handle_command(const char* cmd, const char* args);

// Overlay section (render thread: atomics only).
void draw_debug_ui();

// Does anything in this lane want the seam hook installed? The camera tick asks
// before doing any work, so a disarmed lane costs one relaxed load.
bool wants_install();

// Per-hand AIM tuning (hand 0 = left, 1 = right) for the F10 panel and the
// vrpreset lane. BS2's aim.h shape; every default is zero, derived fresh.
//
// The trims ride BOTH the game-side ray and core's LaserConfig from ONE pair of
// atomics, so the beam and the bullet cannot carry different ones. There is no
// roll trim by design: roll is innermost in xr_local_trim_quat, so it cannot
// steer a ray.
float trim_pitch(int hand);
float trim_yaw(int hand);
void set_trim(int hand, float pitchDeg, float yawDeg);

// Ray ORIGIN offset, cm along the final trimmed basis. Moves the BEAM and the
// DOT together. It does NOT move the bullet: the engine's own fire start point
// stands until the GetWeaponStartTraceLocation substitution lands, so these are
// stored and persisted but must not be presented as an aim adjustment until
// then - a slider that moves the beam and not the bullet makes the beam lie.
float pos_fwd_cm(int hand);
float pos_right_cm(int hand);
float pos_up_cm(int hand);
void set_pos(int hand, float fwdCm, float rightCm, float upCm);

// Per-hand laser and dot, and the shared beam/dot length.
bool laser_hand(int hand);
void set_laser_hand(int hand, bool on);
bool dot_hand(int hand);
void set_dot_hand(int hand, bool on);
float dot_dist_m();
void set_dot_dist_m(float m);

} // namespace bvr::bsi::aim
