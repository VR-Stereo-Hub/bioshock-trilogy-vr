#pragma once
// BioShock Infinite's camera seam: APlayerController::GetPlayerViewPoint.
//
// I2 / DR-I2 installs this READ-ONLY. The detour calls the original and then
// only OBSERVES - it never writes through the out-params, and no assignment
// through them exists anywhere in camera.cpp. The 6DoF override is I4 work and
// arrives as a new function, not by loosening this one.
//
// Why this target and not the exec thunk at 0x129280: a static E8 caller census
// (session 34, reproduced by tools/pe-xref.ps1 in session 36) gives the thunk
// ZERO callers and the implementation FOURTEEN. Native C++ callers bypass the
// script thunks entirely, exactly as on BioShock 1. Hook implementations.

#include "core/hooks/pattern_scan.h"

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

// `bsicam ...`. Returns false when the subcommand is not ours.
bool handle_command(const char* args);

// Overlay section.
void draw_debug_ui();

} // namespace bvr::bsi::camera
