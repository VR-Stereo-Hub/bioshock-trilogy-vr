#pragma once
// BioShock Infinite's camera seam: APlayerController::GetPlayerViewPoint.
//
// I2 / DR-I2 installed this READ-ONLY; I4 (session 39) added the 6DoF drive as
// a separate function (drive_view) called from the detour tail. The WRITE
// TARGET IS THE OUT-PARAMS ONLY - nothing here ever writes [cam+0x3B8] or any
// other engine memory, so the engine's own camera state stays engine-owned and
// keeps moving under mouse/pad (the BS1 pitch-freeze class of bug cannot
// occur), and drive-off is a byte-identical passthrough. The read-only probe
// machinery underneath is unchanged and its returned-minus-source instrument
// keeps measuring the ORIGINAL's output (snapshot taken before the drive).
//
// Why this target and not the exec thunk at 0x129280: a static E8 caller census
// (session 34, reproduced by tools/pe-xref.ps1 in session 36) gives the thunk
// ZERO callers and the implementation FOURTEEN. Native C++ callers bypass the
// script thunks entirely, exactly as on BioShock 1. Hook implementations.

#include "core/hooks/pattern_scan.h"
#include "core/vr/openxr_runtime.h"
#include "game/bioshockinf/frame_context.h"

#include <cstdint>

namespace bvr::bsi::camera {

// Installs the read-only detour. Gated on patterns::rva_trusted() and on the
// target's live prologue matching patterns::kGetPlayerViewPointPrologue, so a
// differently-linked build REFUSES rather than detouring whatever is at the
// RVA. Fail soft: false means the game runs flat, never that it breaks.
bool install(const bvr::pattern_scan::ProcessImage& image);

// True once the hook has actually been OBSERVED firing - not merely installed.
// This is what earns CAP_CAMERA_OVERRIDE, a deliberate divergence from BS1 and
// BS2, which both key their capability bit off a successful install. An address
// resolving proves nothing; a detour running proves everything.
bool has_fired();

// True while the detour is installed and enabled.
bool hook_live();

// Milliseconds since the last time the detour ran, or 0 if it never has. The
// game-thread command pump rides on this hook, so a silent camera is also a
// silent command surface - see the lease in core/framework/command.
uint64_t silent_ms();

// The most recent `this` the detour saw: a live APlayerController*, or null
// before the hook has fired. This is how the reflection lane gets a real
// UObject without a GObjObjects scan - BS2's design, and the reason
// GObjObjects was deprioritised rather than hunted. Treat as a weak reference:
// it is only known to have been valid at the last dispatch.
void* last_player_controller();

// s58: head-directed USE policy (config `interactHeadUse`, default on). ON
// removes the USE-target consumer (patterns.h kInteractionUseViewCallerRva)
// from the seeded deny set so USE arming follows the composed head view;
// OFF restores the full s56 body-locked set. Enforced by the setter in
// whichever order config load / F10 / commands run.
bool head_use_enabled();
void set_head_use(bool on);

// Rung 3c: how many doubled draws have had their camera replayed (one BURST
// per pass-2 attempt, however many times the seam dispatched inside it).
// The SR acceptance gate is bursts == scenedraw's second-draw count.
uint32_t second_pass_replays();

// The single thread the detour has ever dispatched on (0 before first fire).
// Anything that calls INTO the engine - bsicall's ProcessEvent dispatch - must
// verify it is running on this thread first: under the command lease a silent
// game thread hands the pump back to the Present thread in degraded mode, and
// an engine call from the render thread is exactly the cross-thread hazard the
// 0-foreign-dispatch measurement says the engine itself never takes.
uint32_t camera_tid();

// `bsicam ...`. Returns false when the subcommand is not ours.
bool handle_command(const char* args);

// The I4 drive's own seam verbs - simhead / recenter / worldscale / vrpreset -
// plus the I5 stereo verbs: `vrstereo on|off` (the one-toggle: master enable +
// camera mode, i.e. core's quad->projection flip; OFF returns to the mono
// quad), `vraer on|off` (AlternateEye - arms vrstereo + core's AER flag),
// `ipd <mm>` and `bsifov [tanv <v>]` (the projection claim lever + audit
// readout). Routed here from the adapter table. Returns false when the verb
// is not ours.
bool handle_drive_verb(const char* cmd, const char* args);

// The recenter state vrrec serializes in its file header and restores on play
// (the whole xr->game mapping routes through it). The setter also clears any
// pending auto-recenter, or the first replayed frame would re-reference the
// mapping and throw away what was just restored.
void get_recenter_state(bvr::vr::HeadPose* pose, int32_t* yawUnits, float* worldScale);
void set_recenter_state(const bvr::vr::HeadPose& pose, int32_t yawUnits, float worldScale);

// Loads the persisted worldScale (vrpreset.ini) if present. Called once from
// adapter init; touches no engine state.
void load_vr_preset();

// I7 aim (session 44): everything the aim seam needs to build a controller ray
// in GAME rotation units, from the same basis the view drive uses - so the shot
// and the view agree by construction instead of by two parallel derivations.
//   gameYawUnits    the engine's OWN yaw at the last dispatch (pre-drive)
//   recenterYawUnits the yaw the recenter pinned; the residual is measured off it
// Returns false unless the drive is live, a recenter exists and the snapshot is
// fresh - i.e. unless a substitution would be meaningful. Read-only.
bool aim_basis(int32_t* gameYawUnits, int32_t* recenterYawUnits);

// I8 (session 45b): the full per-dispatch view basis - aim_basis plus the
// pre-drive engine camera location, the recenter XR position and the live
// world scale - published by drive_view pass 1. The ray chain and the model
// chain both consume THIS object, which is what makes model-vs-ray agreement
// a construction. Returns a reference whose .valid is false whenever the
// head drive did not drive the last dispatch. Game thread only.
const bvr::bsi::FrameContext& frame_context();

// The `inputOn` registry value as it stands after load_vr_preset (session 44).
// The adapter applies this DIRECTLY at init rather than through the posting
// lane: a refused camera hook must not be able to leave the player in the
// headset with no controller and no way to type a command. Also drains any
// pending post, so the first detour call does not re-apply it.
bool input_armed_at_boot();

// The projection claim's vertical half-tangent (I5/I6). The setter exists for
// the lens decoder's track mode; while the FOV lever is armed the claim
// re-derives from the lever on every publish, so the lever always wins.
float claim_tan_v();
void set_claim_tan_v(float v);

// s52: body-follows-head locomotion toggle (atomic; F10 checkbox + `bsibody`
// + the inputBodyFollow preset key all land here). While on, drive_view
// publishes the yaw residual to the input bridge each dispatch.
bool body_follow_head();
void set_body_follow_head(bool on);

// s52 round 2: programmatic recenter (same latch the `recenter` verb sets).
// The cinematic gate fires it on a hold-open edge so a cutscene starts
// centered on wherever the player is looking.
void request_recenter();

// s51: edge-telemetry taps - read-only copies of the last dispatch's chain
// stages (game thread only, same discipline as frame_context()). False until
// the corresponding stage has run once.
bool last_head_pose(bvr::vr::HeadPose& out);       // the consumed head pose
bool final_camera(FVector& loc, FRotator& rot);    // written camera, post-eye
bool eye_loc(int e, FVector& out);                 // per-eye camera, 0=L 1=R

// Overlay section.
void draw_debug_ui();

} // namespace bvr::bsi::camera
