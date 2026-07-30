// Hook behavior (call the original, then adjust the writable out-params;
// publish state through atomics) follows
// itsloopyo/bioshock-remastered-headtracking (MIT), src/engine_hook.rs.

#include "game/bioshock1r/camera.h"

#include "core/debug/value_scan.h"
#include "core/gfx/frame_inspector.h"
#include "core/gfx/hud_capture.h"
#include "core/ui/overlay.h"
#include "core/input/xinput_bridge.h"
#include "core/util/log.h"
#include "game/bioshock1r/aim.h"
#include "game/bioshock1r/body.h"
#include "game/bioshock1r/bones.h"
#include "game/bioshock1r/console_exec.h"
#include "game/bioshock1r/game_ini.h"
#include "core/vr/openxr_runtime.h"
#include "game/bioshock1r/hands.h"
#include "game/bioshock1r/input_drive.h"
#include "game/bioshock1r/patterns.h"
#include "game/bioshock1r/recorder.h"
#include "game/bioshock1r/scenedraw.h"
#include "game/shared/ue_math.h"

#include <windows.h>
#include <MinHook.h>

#include <imgui.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <share.h>

namespace bvr::b1r::camera {
namespace {

// FVector/FRotator, the rotation-unit constants and the XR->UE conversion all
// live in ue_math.h so aim.cpp shares this file's exact conventions.

// Controls: overlay thread writes, game thread reads. All relaxed - x86
// lock-free, and a field arriving one frame late (or a torn group, e.g. new X
// with old Y for one frame) is fine for debug sliders.
std::atomic<float> g_offsetX{0.0f}, g_offsetY{0.0f}, g_offsetZ{0.0f};
std::atomic<float> g_yawDeg{0.0f}, g_pitchDeg{0.0f}, g_rollDeg{0.0f};
std::atomic<bool>  g_wobble{false};
std::atomic<float> g_wobbleAmp{10.0f};
std::atomic<bool>  g_fovOverride{false};
std::atomic<float> g_fovDeg{100.0f};
// Heartbeat on by default during the M1-M3 bring-up phase - 1 line/s proves
// per-frame firing in every session log. Toggle off in the overlay.
std::atomic<bool>  g_logCamera{true};

// M3 VR camera drive.
// Session 16 part 3: default 100 by the user's in-headset calibration - at
// 100 the viewmodel matches the real hand in size AND distance (angular size
// and stereo finally agree; at 50 the oversized mesh read "too close"). The
// world reads ~half size in exchange; the user judged it acceptable. A
// world/hands scale SPLIT (own stereo for the viewmodel) is the M9 polish
// item if that trade ever bothers.
std::atomic<float> g_worldScale{100.0f};       // Unreal units per meter
// User head-anchor offset (session 16 part 3): the pawn's authored eye
// height reads wrong once worldScale moves (60 UU = 0.6 m at 100 UU/m - the
// "head very wrong" report); vertical + view-forward sliders correct the
// anchor. Defaults 0 by the user's call (session 16 part 4) - they tune by
// eye and persist their value via the VR preset ini.
std::atomic<float> g_headOffUpUu{0.0f};
std::atomic<float> g_headOffFwdUu{0.0f};
// VR preset 1 (session 16 part 3): one button/command arming the user's full
// VR configuration - every switch they flipped by hand, in a safe order,
// plus tuned slider values (vrpreset.ini once saved; shipped defaults
// otherwise). The overlay buttons only set pending flags - the apply/save
// run on the game thread next frame.
std::atomic<bool> g_vrPresetPending{false};
std::atomic<bool> g_vrPresetSavePending{false};
// Render-resolution request from the overlay. The overlay runs on the RENDER
// thread and this does file I/O plus a read-back, so it goes through the
// established pending-atomic seam and is performed on the game thread, next to
// the preset consumers. Packed as one 64-bit value so the pair cannot tear.
std::atomic<uint64_t> g_resWritePending{0};

// M8 session 18 part 2: the flat-screen crosshair, DEFAULT HIDDEN (user ask).
// The lever is `ShockPlayer.bReticleDisabled` - the game's own RenderReticle
// then pushes "NoReticle" to the flash HUD every frame (script source read
// straight from ShockGame.U; ENGINE_NOTES session 18 part 2). It is written
// through the engine's own console SET handler via the exec seam, so no
// property offset or bitmask is ever needed, and SET also writes the class
// default, so pawns spawned later (load crossings) inherit it. Re-asserted
// on a slow cadence in case anything script-side calls EnableReticle.
std::atomic<bool> g_crosshairVisible{false}; // `vrxhair on` re-shows it
int g_crosshairApplied = -1;                 // last state pushed (-1 = never)
uint64_t g_crosshairAssertMs = 0;            // game thread only
// Session 22 round 5 (user ask, pre-release): the pad SOFT LOCK-ON (aim
// magnetism - GamepadPlayerInput.SoftLockOnRadius, found in the reticle
// script source) drags aim toward targets because our motion controllers
// register as a gamepad. Default DISABLED in VR via the same engine-SET
// mechanism as the crosshair; re-asserted on the same slow cadence. NOTE:
// unchecking only stops the assert - the game's own radius returns on the
// next game restart (SET edits are memory-only and the original default is
// not readable through this seam).
std::atomic<bool> g_lockOnDisabled{true};
int g_lockOnApplied = -1;
uint64_t g_lockOnAssertMs = 0;
std::atomic<bool>  g_recenterRequested{true};  // auto-recenter on first drive
std::atomic<bool>  g_vrDriving{false};         // telemetry for the UI
std::atomic<bool>  g_forceHeadsetFov{false};   // session 4: now writes the REAL control (the
                                               // UShockUserSettings HorizontalFOV int that the
                                               // renderer consumes per frame, no 130 cap) when
                                               // it is resolved. Still default OFF: widening
                                               // is the user's in-headset call.

// Session 4: direct game-FOV write through the settings object (the video
// option's storage). Distinct from the dead PC+0xE0 override above.
std::atomic<bool>  g_gameFovWrite{false};
std::atomic<float> g_gameFovDeg{130.0f};
std::atomic<int32_t> g_lastOptionFov{0};       // telemetry: what the option holds now

// M4 rung 1: AlternateEye. Half-IPD camera shift per eye, eye picked by
// vr::current_eye_sign() (0 while AER is off).
std::atomic<float> g_ipdMm{63.0f};
// Head-offset telemetry: the recenter-relative offset applied to loc this
// frame, in UU - makes the world-scale slider's effect a number on screen.
std::atomic<float> g_headOffX{0.0f}, g_headOffY{0.0f}, g_headOffZ{0.0f};

// Telemetry: game thread writes, overlay thread reads.
std::atomic<uint32_t> g_callCount{0};
std::atomic<float>    g_lastLocX{0.0f}, g_lastLocY{0.0f}, g_lastLocZ{0.0f};
std::atomic<int32_t>  g_lastPitch{0}, g_lastYaw{0}, g_lastRoll{0};
std::atomic<float>    g_lastFov{0.0f};
// Display only. The overlay never dereferences this - all game-memory access
// happens on the game thread inside the detour, where `this` is alive.
std::atomic<void*>    g_playerController{nullptr};
std::atomic<void*>    g_lastViewActor{nullptr}; // *view_actor out-param (the pawn)

// Foreground lens match (session 15): the renderer consumes the PC's
// ForegroundFovAngle (patterns.h kPcForegroundFovOffset) EVERY FRAME; writing
// the world-equivalent 4:3 spec re-lenses the whole rig to the WORLD lens
// (dump-proven: every vm draw joins the world projection cluster, lighting
// tiers included). Default ON since session 16: the driven path's pull at
// the matched lens calibrated small (+11.5 UU, vrbones lockpull default) and
// the full flat-stereo acceptance ladder passed at k=1 - the rig renders
// through the honest lens at world-correct size/depth/parallax. `vrfgfov
// off` restores the session-14 narrow-lens configuration for A/B.
std::atomic<bool>  g_fgFovMatch{true};
std::atomic<float> g_fgFovSaved{0.0f};   // engine value to restore on disable
std::atomic<float> g_fgFovWritten{0.0f}; // telemetry: what we wrote last
// Session 28: `vrfgfov legacy on` restores the pre-session-28 hardcoded 0.75
// match constant, which is (4/3)*(9/16) - correct at 16:9 and 1.7778/aspect too
// narrow everywhere else. Kept as an instant in-headset A/B for the viewmodel,
// because the aspect correction is the change that made the hands and the world
// geometrically correct AT THE SAME TIME and that pairing needs confirming.
std::atomic<bool>  g_fgFovLegacy{false};
std::atomic<float> g_fgFovK{0.75f};      // telemetry: the match constant used

using CalcViewFn = void(__fastcall*)(void* self, void* edx, void** viewActor,
                                     FVector* loc, FRotator* rot);
CalcViewFn g_original = nullptr;
void* g_target = nullptr;
std::atomic<bool> g_hookLive{false};
std::atomic<bool> g_loggedFirstFire{false};

// Game-thread-only bookkeeping (never touched by the overlay).
bool g_wasOverridingFov = false;
float g_savedFov = 0.0f;
bool g_wasWritingGameFov = false;
int32_t g_savedGameFov = 0;
// WHICH object g_savedGameFov was captured from (session 27). Without this the
// restore could write object A's saved value into object B after a re-scan
// picked a different instance, and a value restored into the wrong object is
// indistinguishable from corruption. Also gates the write itself: the pointer
// must still be the one we bound to.
const int32_t* g_savedGameFovOwner = nullptr;

// SEH-guarded write of the settings FOV int. The pointer comes from an object
// search, and while the search is now stack-safe and liveness-checked
// (heap_scan.h), "the address passed two identity predicates" is still not the
// same as "the engine owns this and it is safe to poke". A fault here must
// never take the game down; it disarms the writer instead. Logging happens at
// the call site, never inside the guard.
bool write_option_fov(int32_t* dst, int32_t value) {
    __try {
        if (*dst != value) *dst = value;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
uint64_t g_lastHeartbeatMs = 0;
uint32_t g_heartbeatBaseCount = 0;
bool g_haveRecenter = false;
bvr::vr::HeadPose g_recenterPose{};
// The seated frame's yaw zero, in ROTATOR UNITS (65536/turn), not radians.
// Integer because the M7.5 body yaw transfer moves it: the camera and the
// whole controller-to-world mapping are functions of (gameYaw - recenterYaw),
// so the transfer adds T to the body and exactly T to this - and in integers
// that cancellation is exact rather than merely close (body.h has the proof).
// It also cannot accumulate: wrap_rot keeps it in (-180, +180] deg, where a
// bare float would have drifted into ulps larger than a rotation unit after a
// few minutes of spinning.
int32_t g_recenterYawUnits = 0;
// Float mirror, for the FrameContext and the telemetry only.
float recenter_yaw_rad() { return g_recenterYawUnits / kRotUnitsPerRadian; }

// Synthetic HMD lane (session 12 part 4): `simhead <yaw> <pitch> <roll>
// [holdMs]` feeds a scripted head pose through the REAL camera drive -
// recenter, additive yaw, head offset, stereo passes, bone drive - so the
// full in-headset pipeline runs flat, headset-free. The simpose hand lane's
// twin: park the hand, sweep the head, screenshot - the head-coupling
// reports become reproducible (or provably game-clean) without a user in
// the loop. Self-expiring like every synthetic lane.
struct SimHead {
    float yawDeg = 0.0f, pitchDeg = 0.0f, rollDeg = 0.0f;
    uint64_t deadline = 0;
};
SimHead g_simHead;

// M4 rung 2 (SequentialReentry): pass-1 caches the fully-driven camera here
// (post head drive + debug offsets, PRE eye offset) so pass 2 replays the
// exact same base with the opposite eye - both eyes share one head sample
// even though a Present lands between the two CalcView calls. Game thread
// only; pass 2 always immediately follows its pass 1.
bool g_srBaseValid = false;
FVector g_srBaseLoc{};
FRotator g_srBaseRot{};
// Session 22: age + eye-offset latch for the pass-2 replay. The stamp kills
// the stale-base hazard - CalcView can go silent for minutes during scripted
// scenes, and pass 2 must never replay a pre-cutscene camera. The latch
// carries pass-1's strict-gameplay decision so a non-gameplay pair renders
// both eyes IDENTICAL (the quad screen shows one image; an IPD offset
// between its two source presents would jitter it).
uint64_t g_srBaseStampMs = 0;
bool g_srBaseEyed = false;

// Session 29 authored+look: the head orientation the cutscene STARTED at.
// Deltas are measured from here, so the opening frame of every shot is framed
// exactly as authored no matter where the player happened to be looking.
// Invalidated on both cinematic edges - carrying it across shots would make
// the next one open at whatever angle the last one ended on.
bool g_cineLookValid = false;
int32_t g_cineLookPitch = 0, g_cineLookYaw = 0, g_cineLookRoll = 0;
// Last normal-pass CalcView tick (game thread). Scripted cameras bypass
// CalcView entirely, so staleness here is the cutscene signal for the
// BuildDetour-side FOV restore and the second-build skip.
uint64_t g_lastCalcViewMs = 0;

// Session 21 fg view-sync stash: the final per-eye camera (post eye offset)
// of the current pair. Game thread only (1t); freshness-gated by stamp so a
// mode flip cannot leave scenedraw substituting stale poses.
FVector g_eyeCamLoc[2] = {};
FRotator g_eyeCamRot[2] = {};
uint64_t g_eyeCamStampMs[2] = {};

float* fov_ptr(void* pc) {
    return reinterpret_cast<float*>(static_cast<uint8_t*>(pc) + patterns::kFovLiveOffset);
}

// Half-IPD shift along view-right of `rot`, sign -1 = left eye. Shared by
// both SequentialReentry passes and the AER path. Session 22: the right axis
// comes from the FULL rotation (ue_rot_basis) - the old yaw-only right kept
// the virtual eyes horizontal while real eyes stack vertically under head
// roll (the "weird stereoscopy when tilting the head sideways" report). At
// roll 0 the basis row reduces to (-sin yaw, cos yaw, 0), bit-identical to
// the old formula - the neutral-roll rendering is unchanged by construction.
void apply_eye_offset(FVector* loc, const FRotator& rot, int sign) {
    float fwd[3], right[3], up[3];
    ue_rot_basis(rot, fwd, right, up);
    float halfIpdUu = static_cast<float>(sign) *
                      (g_ipdMm.load(std::memory_order_relaxed) / 2000.0f) *
                      g_worldScale.load(std::memory_order_relaxed);
    loc->x += right[0] * halfIpdUu;
    loc->y += right[1] * halfIpdUu;
    loc->z += right[2] * halfIpdUu;
}

// Automated-test seam: %LOCALAPPDATA%\BioshockVR\command.txt is polled at 1 Hz
// on the game thread; when its write time changes, every line is applied and
// logged. Lets a test harness drive the debug controls without the overlay.
// Camera commands: "fov <deg>", "fov off", "gfov <deg>", "gfov off",
// "offset <x> <y> <z>", "camrot <pitch> <yaw> <roll>" (render-camera-only
// rotation offset in degrees - the flat stand-in for HMD head-look),
// "recenter", "fovaudit [pose on|off]" (session 21: option vs submitted vs
// option-derived fov side by side; `pose on` arms the tagged-vs-consumed
// yaw log for the headset).
// "vrcine on|off|mode quad|mode stereo|status" (session 22: cinematic quad
// fallback - scripted scenes and menus drop the projection layer to the
// big-screen quad; the detector is strict-view/staleness/rendered-vs-option
// fov mismatch; "mode stereo" keeps the projection through fov-mismatch
// scenes and claims the measured fov; status prints counters + fov watch).
// Discovery commands (route to core/debug/value_scan; game thread only):
//   memscan <f>  memrescan <f>  memlist [n]  memread <idx>
//   memscani <u>  memrescani <u>   (integer-typed variants)
//   mempoke <idx> <f>  mempoke <lo>-<hi> <f>  mempokei ...same with <u>
//   memrestore  memptr <idx> [maxDeltaHex]
//   pokeaddr <hex> <f>  pokeaddri <hex> <u>  hexdump <hex> <len>
//   strscan <text>  membases  dumpframe [full]
// VR one-toggle (session 8): "vrstereo on|off" - sequences structural 1t +
// VR camera mode + SequentialReentry stereo; sticky across loads. Also
// reachable as "reentry vrstereo on|off" and as the overlay checkbox.
// Synthetic gamepad (M5, routes to core/input/xinput_bridge):
//   vrinput on|off|status  vrinput test stick l|r <x> <y> [holdMs]
//   vrinput test trig l|r <0..255> [holdMs]
//   vrinput test press <A|B|X|Y|LB|RB|START|BACK|LS|RS|DU|DD|DL|DR> [holdMs]
//   vrinput test clear   (test holds self-expire; slots only feed the game
//   while vrinput is on)
// Decoupled aim (M6, routes to game/bioshock1r/aim):
//   vraim on|off|status  vraim probe on|off  vraim dump <n>
//   vraim origin on|off  vraim seam <firestart|aimerror|viewpoint|viewdir> on|off
//   vraim test l|r <yawDeg> <pitchDeg> [holdMs]   vraim test clear
// Engine console commands without the dead Tab console (console_exec):
//   exec <command>   (enters at UWindowsViewport::Exec)
//   execc <command>  (enters at UWindowsClient::Exec)
// DR-5 reentry probe (routes to game/bioshock1r/scenedraw; command-gated -
// nothing is hooked without these):
//   reentry hook [build|submit|drain|flush] (default build - the DR-5 seam)
//   reentry stereo on|off (M4 rung 2: L/R double-render + eye-tagged capture)
//   reentry 1t on|off (structural single-threaded render - flush-point hook,
//   load-safe)  reentry 1tpoke on|off (legacy hw-thread poke - NOT load-safe)
//   reentry unhook  reentry on|off  reentry pulse  reentry yaw <deg>
//   reentry dump <n> (per-call submit arg telemetry)
//   reentry arg3 <hex|off> (double-submit call-site filter)
//   reentry latchclear on|off  reentry reset  reentry status
//   reentry kick on|off (SetEvent caller sampler)  reentry calcstack (one-shot
//   game-thread stack scan)
uint64_t g_lastCmdPollMs = 0;
FILETIME g_lastCmdWrite{};

// Dispatch one command line. `cmd` is the first whitespace-delimited token;
// `args` is the remainder of the line (may be empty). Tokenizing on the first
// word first means no command can be shadowed by another's prefix.
// Defined below (VR preset 1); the command dispatcher reaches them too.
void apply_vr_preset();
void save_vr_preset();

void apply_command(const char* cmd, const char* args) {
    float v = 0.0f, x = 0.0f, y = 0.0f, z = 0.0f;
    unsigned lo = 0, hi = 0, n = 0;
    unsigned addr = 0, len = 0;

    if (strcmp(cmd, "fov") == 0) {
        if (strncmp(args, "off", 3) == 0) {
            g_fovOverride.store(false, std::memory_order_relaxed);
            BVR_LOG("[b1r] command: fov off");
        } else if (sscanf_s(args, "%f", &v) == 1) {
            g_fovDeg.store(v, std::memory_order_relaxed);
            g_fovOverride.store(true, std::memory_order_relaxed);
            BVR_LOG("[b1r] command: fov %.1f", v);
        }
    } else if (strcmp(cmd, "gfov") == 0) {
        if (strncmp(args, "off", 3) == 0) {
            g_gameFovWrite.store(false, std::memory_order_relaxed);
            BVR_LOG("[b1r] command: gfov off");
        } else if (sscanf_s(args, "%f", &v) == 1) {
            g_gameFovDeg.store(v, std::memory_order_relaxed);
            g_gameFovWrite.store(true, std::memory_order_relaxed);
            BVR_LOG("[b1r] command: gfov %.1f", v);
        }
    } else if (strcmp(cmd, "buildgate") == 0) {
        patterns::handle_buildgate_command(args);
    } else if (strcmp(cmd, "vrres") == 0) {
        // The eye render IS the game's backbuffer, so the game's resolution is
        // the VR resolution. `SETRES` faults (ENGINE_NOTES session 27), so the
        // game's own ini is the only working lever and a change lands on the
        // next launch. Deliberately explicit rather than automatic.
        unsigned rw = 0, rh = 0;
        if (sscanf_s(args, "%ux%u", &rw, &rh) == 2 && rw && rh) {
            game_ini::write_viewport(rw, rh);
        } else if (sscanf_s(args, "%u %u", &rw, &rh) == 2 && rw && rh) {
            game_ini::write_viewport(rw, rh);
        } else {
            unsigned bw = 0, bh = 0;
            bvr::vr::fov_audit(nullptr, nullptr, nullptr, &bw, &bh);
            game_ini::log_status(bw, bh);
        }
    } else if (strcmp(cmd, "recenter") == 0) {
        g_recenterRequested.store(true, std::memory_order_relaxed);
        BVR_LOG("[b1r] command: recenter");
    } else if (strcmp(cmd, "offset") == 0) {
        if (sscanf_s(args, "%f %f %f", &x, &y, &z) == 3) {
            g_offsetX.store(x, std::memory_order_relaxed);
            g_offsetY.store(y, std::memory_order_relaxed);
            g_offsetZ.store(z, std::memory_order_relaxed);
            BVR_LOG("[b1r] command: offset %.1f %.1f %.1f", x, y, z);
        }
    } else if (strcmp(cmd, "simhead") == 0) {
        // Synthetic HMD (see SimHead above). Arming from idle recenters onto
        // the sim's first pose - start at `simhead 0 0 0`, then change angles
        // to "turn the head" without touching the recenter.
        if (strncmp(args, "off", 3) == 0) {
            g_simHead.deadline = 0;
            BVR_LOG("[b1r] command: simhead off");
        } else {
            int hold = 0;
            int n = sscanf_s(args, "%f %f %f %d", &x, &y, &z, &hold);
            if (n >= 3) {
                bool wasIdle = GetTickCount64() >= g_simHead.deadline;
                g_simHead.yawDeg = x;
                g_simHead.pitchDeg = y;
                g_simHead.rollDeg = z;
                if (hold <= 0) hold = 120000;
                g_simHead.deadline = GetTickCount64() + static_cast<uint64_t>(hold);
                if (wasIdle) g_recenterRequested.store(true, std::memory_order_relaxed);
                BVR_LOG("[b1r] command: simhead yaw %.1f pitch %.1f roll %.1f for %d ms%s",
                        x, y, z, hold, wasIdle ? " (recentering onto first sim pose)" : "");
            } else {
                BVR_LOG("[b1r] usage: simhead <yawDeg> <pitchDeg> <rollDeg> [holdMs] | "
                        "simhead off");
            }
        }
    } else if (strcmp(cmd, "camrot") == 0) {
        // Render-camera-only rotation offset (degrees), the seam twin of the
        // overlay's camera test sliders. The flat stand-in for HMD head-look:
        // it rotates the CalcView out-params (and fc) but never the pawn, so
        // it splits "render camera" from "actor rotation" exactly like a
        // headset does - the discriminator the M7-v2 head-coupling fix needed.
        if (sscanf_s(args, "%f %f %f", &x, &y, &z) == 3) {
            g_pitchDeg.store(x, std::memory_order_relaxed);
            g_yawDeg.store(y, std::memory_order_relaxed);
            g_rollDeg.store(z, std::memory_order_relaxed);
            BVR_LOG("[b1r] command: camrot %.1f %.1f %.1f", x, y, z);
        }
    } else if (strcmp(cmd, "fgstack") == 0) {
        unsigned shots = 3;
        sscanf_s(args, "%u", &shots);
        bvr::frame_inspector::set_cb_watch(
            patterns::kFgCbFingerprint, patterns::kFgCbFingerprintFirst,
            _countof(patterns::kFgCbFingerprint), patterns::kFgCbTransformFirst,
            patterns::kFgCbTransformCount, patterns::kFgCbBytes);
        bvr::frame_inspector::cb_watch_log_stacks(static_cast<int>(shots));
        BVR_LOG("[b1r] fgstack: cb watch armed on the fg fingerprint, logging %u writer "
                "callstacks",
                shots);
    } else if (strcmp(cmd, "vrfgfov") == 0) {
        // Session 28: `legacy on|off` flips the match constant between the
        // aspect-correct (4/3)*(h/w) and the pre-session-28 hardcoded 0.75, for
        // an instant in-headset viewmodel A/B. Identical at 16:9.
        if (strncmp(args, "legacy", 6) == 0) {
            bool leg = strstr(args + 6, "off") == nullptr;
            g_fgFovLegacy.store(leg, std::memory_order_relaxed);
            BVR_LOG("[b1r] fg lens match constant: %s (last k=%.6f, last written "
                    "%.1f deg) - legacy 0.75 is (4/3)*(9/16), correct at 16:9 only",
                    leg ? "LEGACY 0.75" : "aspect-correct (4/3)*(h/w)",
                    g_fgFovK.load(std::memory_order_relaxed),
                    g_fgFovWritten.load(std::memory_order_relaxed));
            return;
        }
        bool on = strncmp(args, "off", 3) != 0;
        g_fgFovMatch.store(on, std::memory_order_relaxed);
        BVR_LOG("[b1r] fg lens match %s (last written %.1f deg, k=%.6f)",
                on ? "ON" : "off",
                g_fgFovWritten.load(std::memory_order_relaxed),
                g_fgFovK.load(std::memory_order_relaxed));
    } else if (strcmp(cmd, "fginfo") == 0) {
        BVR_LOG("[b1r] fginfo: pc=%p viewActor=%p hands=%p weapon=%p (fsweep/hexdump "
                "targets)",
                g_playerController.load(std::memory_order_relaxed),
                g_lastViewActor.load(std::memory_order_relaxed), hands::hands_actor(),
                hands::weapon_actor());
    } else if (strcmp(cmd, "fovaudit") == 0) {
        // Session 21 FOV audit: the three fov truths side by side - the
        // engine option we write, what the runtime last TAGGED the projection
        // layer with, and the option-derived expectation at the swap aspect.
        // The RENDERED side comes from `dumpframe full 2` decoded by
        // tools/decode-framedump.ps1; the flat gate is rendered == submitted.
        // "fovaudit pose on|off" arms the tagged-vs-consumed yaw log (headset).
        if (strncmp(args, "pose", 4) == 0) {
            bvr::vr::set_pose_audit(strstr(args + 4, "on") != nullptr);
        } else if (strncmp(args, "eyes", 4) == 0) {
            // Session 22 head-roll gate: the per-eye camera stash and their
            // delta vector. Under `simhead 0 0 <roll>` the delta must rotate
            // with the roll (|z| -> halfIpd at 90 deg); pre-fix it stayed
            // horizontal at every roll.
            uint64_t nowMs = GetTickCount64();
            BVR_LOG("[b1r] eyecam L=(%.3f %.3f %.3f) R=(%.3f %.3f %.3f) "
                    "d=(%.3f %.3f %.3f) ageL=%llums ageR=%llums",
                    g_eyeCamLoc[0].x, g_eyeCamLoc[0].y, g_eyeCamLoc[0].z,
                    g_eyeCamLoc[1].x, g_eyeCamLoc[1].y, g_eyeCamLoc[1].z,
                    g_eyeCamLoc[1].x - g_eyeCamLoc[0].x,
                    g_eyeCamLoc[1].y - g_eyeCamLoc[0].y,
                    g_eyeCamLoc[1].z - g_eyeCamLoc[0].z,
                    static_cast<unsigned long long>(nowMs - g_eyeCamStampMs[0]),
                    static_cast<unsigned long long>(nowMs - g_eyeCamStampMs[1]));
        } else {
            int32_t* opt = patterns::hfov_option_ptr();
            float tanH = 0.0f, tanV = 0.0f;
            int src = -1;
            unsigned sw = 0, sh = 0;
            bvr::vr::fov_audit(&tanH, &tanV, &src, &sw, &sh);
            // Option-derived expectation, from the WORLD lens law measured in
            // session 28: tanH = tan(option/2) (aspect-independent), tanV =
            // tanH * (h/w). Flat there is no XR session, so fall back to the
            // real BACKBUFFER dims - never to a hardcoded 9/16, which is right
            // only at 16:9 and is how a 1.84x error stayed invisible.
            unsigned bbW = 0, bbH = 0;
            bool haveBb = bvr::hud::backbuffer_dims(&bbW, &bbH);
            unsigned aw = (sw && sh) ? sw : bbW;
            unsigned ah = (sw && sh) ? sh : bbH;
            float optTanH = 0.0f, optTanV = 0.0f;
            if (opt) {
                optTanH = tanf(static_cast<float>(*opt) * 0.5f / kRadToDeg);
                optTanV = optTanH * ((aw && ah) ? (static_cast<float>(ah) /
                                                   static_cast<float>(aw))
                                                : 1.0f);
            }
            (void)haveBb;
            BVR_LOG("[b1r] fovaudit: option=%d gfovWrite=%s(%.1f) | submitted tanH=%.6f "
                    "tanV=%.6f src=%s swap=%ux%u | option-derived tanH=%.6f tanV=%.6f",
                    opt ? *opt : -1,
                    g_gameFovWrite.load(std::memory_order_relaxed) ? "on" : "off",
                    g_gameFovDeg.load(std::memory_order_relaxed), tanH, tanV,
                    src == 0   ? "readback"
                    : src == 1 ? "fallback"
                    : src == 2 ? "manual"
                    : src == 3 ? "live"
                               : "none",
                    sw, sh, optTanH, optTanV);
            // Session 22 live fov watch, session 28: BOTH lenses, each labelled
            // FRESH or STALE in words. maxAge 0 so a stale value still prints -
            // several session-27 conclusions were taken from samples that
            // printed age>9000ms, stale by the rule the same line printed, so
            // the word is now on the line and nothing has to be inferred.
            float liveTanH = 0.0f, liveTanV = 0.0f;
            unsigned long long liveAge = 0;
            if (bvr::hud::fov_watch(&liveTanH, &liveTanV, &liveAge, 0)) {
                float fgH = 0.0f, fgV = 0.0f;
                unsigned long long fgAge = 0;
                bool haveFg = bvr::hud::fov_watch_fg(&fgH, &fgV, &fgAge, 0);
                BVR_LOG("[b1r] fovaudit live: WORLD tanH=%.6f tanV=%.6f (%.2f deg) "
                        "age=%llums %s | FG tanH=%.6f tanV=%.6f age=%llums %s | "
                        "lenses=%d mismatch=%d cineActive=%d",
                        liveTanH, liveTanV, 2.0f * atanf(liveTanH) * kRadToDeg,
                        liveAge, liveAge <= 500 ? "FRESH" : "STALE - DO NOT CONCLUDE",
                        haveFg ? fgH : 0.0f, haveFg ? fgV : 0.0f, fgAge,
                        !haveFg ? "n/a (single lens - 16:9)"
                                : (fgAge <= 500 ? "FRESH" : "STALE"),
                        bvr::hud::fov_lens_count(),
                        bvr::hud::fov_mismatch() ? 1 : 0,
                        bvr::vr::cinematic_active() ? 1 : 0);
                // The two laws, spelled out against this backbuffer so the
                // reader never has to redo the arithmetic (session 28 measured:
                // world is horizontal-anchored, fg is vertical-anchored).
                if (opt && aw && ah) {
                    float a = static_cast<float>(aw) / static_cast<float>(ah);
                    BVR_LOG("[b1r] fovaudit laws (aspect %.5f from %ux%u): world "
                            "expects tanH=tan(opt/2)=%.6f tanV=tanH*h/w=%.6f | fg "
                            "matches the world when the 0.75 becomes "
                            "(4/3)*h/w=%.6f (shipped 0.75 is that only at 16:9; "
                            "here it under-lenses the viewmodel by %.4fx)",
                            a, aw, ah, optTanH, optTanV,
                            (4.0f / 3.0f) / a, (4.0f / 3.0f) / a / 0.75f);
                }
            } else {
                BVR_LOG("[b1r] fovaudit live: no decoded scene tangents yet");
            }
        }
    } else if (strcmp(cmd, "fsweep") == 0) {
        float lo = 0.0f, hi = 0.0f;
        if (sscanf_s(args, "%x %u %f %f", &addr, &len, &lo, &hi) == 4)
            bvr::value_scan::float_sweep(addr, len, lo, hi);
        else
            BVR_LOG("[b1r] usage: fsweep <hexaddr> <len> <lo> <hi>");
    } else if (strcmp(cmd, "memscan") == 0) {
        if (sscanf_s(args, "%f", &v) == 1) bvr::value_scan::scan_f32(v);
    } else if (strcmp(cmd, "memrescan") == 0) {
        if (sscanf_s(args, "%f", &v) == 1) bvr::value_scan::rescan_f32(v);
    } else if (strcmp(cmd, "memscani") == 0) {
        if (sscanf_s(args, "%u", &n) == 1) bvr::value_scan::scan_u32(n);
    } else if (strcmp(cmd, "memrescani") == 0) {
        if (sscanf_s(args, "%u", &n) == 1) bvr::value_scan::rescan_u32(n);
    } else if (strcmp(cmd, "memlist") == 0) {
        bvr::value_scan::list(sscanf_s(args, "%u", &n) == 1 ? n : 32);
    } else if (strcmp(cmd, "memread") == 0) {
        if (sscanf_s(args, "%u", &n) == 1) bvr::value_scan::read_at(n);
    } else if (strcmp(cmd, "mempoke") == 0) {
        if (sscanf_s(args, "%u-%u %f", &lo, &hi, &v) == 3)
            bvr::value_scan::poke_range(lo, hi, v);
        else if (sscanf_s(args, "%u %f", &n, &v) == 2)
            bvr::value_scan::poke(n, v);
    } else if (strcmp(cmd, "mempokei") == 0) {
        unsigned iv = 0;
        if (sscanf_s(args, "%u-%u %u", &lo, &hi, &iv) == 3)
            bvr::value_scan::poke_range_u32(lo, hi, iv);
        else if (sscanf_s(args, "%u %u", &n, &iv) == 2)
            bvr::value_scan::poke_u32(n, iv);
    } else if (strcmp(cmd, "memrestore") == 0) {
        bvr::value_scan::restore_all();
    } else if (strcmp(cmd, "memptr") == 0) {
        unsigned maxDelta = 0x400;
        if (sscanf_s(args, "%u %x", &n, &maxDelta) >= 1)
            bvr::value_scan::ptr_scan(n, maxDelta);
    } else if (strcmp(cmd, "pokeaddr") == 0) {
        if (sscanf_s(args, "%x %f", &addr, &v) == 2)
            bvr::value_scan::poke_addr(addr, v);
    } else if (strcmp(cmd, "pokeaddri") == 0) {
        unsigned iv = 0;
        if (sscanf_s(args, "%x %u", &addr, &iv) == 2)
            bvr::value_scan::poke_addr_u32(addr, iv);
    } else if (strcmp(cmd, "hexdump") == 0) {
        if (sscanf_s(args, "%x %u", &addr, &len) >= 1)
            bvr::value_scan::hexdump(addr, len ? len : 64);
    } else if (strcmp(cmd, "strscan") == 0) {
        char text[96];
        if (sscanf_s(args, "%95s", text, static_cast<unsigned>(sizeof text)) == 1)
            bvr::value_scan::log_string_scan(text);
    } else if (strcmp(cmd, "membases") == 0) {
        bvr::value_scan::log_module_bases();
    } else if (strcmp(cmd, "dumpframe") == 0) {
        // dumpframe [full] [n] - n > 1 records consecutive present windows
        // (both halves of a stereo pair; files suffixed _qN).
        bool full = strncmp(args, "full", 4) == 0;
        int count = 1;
        sscanf_s(full ? args + 4 : args, " %d", &count);
        bvr::frame_inspector::arm(full ? 2 : 1, count);
    } else if (strcmp(cmd, "vrinput") == 0) {
        input::handle_command(args); // M5 synthetic gamepad; logs its own echoes
    } else if (strcmp(cmd, "vraim") == 0) {
        aim::handle_command(args); // M6 decoupled aim; logs its own echoes
    } else if (strcmp(cmd, "vrhands") == 0) {
        hands::handle_command(args); // M7 viewmodel; logs its own echoes
    } else if (strcmp(cmd, "vrbones") == 0) {
        bones::handle_command(args); // M7-v2 skeleton probes; logs its own echoes
    } else if (strcmp(cmd, "vrbody") == 0) {
        body::handle_command(args); // M7.5 yaw transfer; logs its own echoes
    } else if (strcmp(cmd, "vrrec") == 0) {
        recorder::handle_command(args); // session 20 record+replay; logs its own echoes
    } else if (strcmp(cmd, "vrfgnode") == 0) {
        scenedraw::handle_fgnode_command(args); // session 21 fg-scene-node instrument
    } else if (strcmp(cmd, "exec") == 0) {
        console_exec::run_viewport(args); // engine console command, viewport chain
    } else if (strcmp(cmd, "execc") == 0) {
        console_exec::run_client(args); // same, entering at UWindowsClient::Exec
    } else if (strcmp(cmd, "exece") == 0) {
        console_exec::run_engine(args); // same, entering at UGameEngine::Exec
    } else if (strcmp(cmd, "vrstereo") == 0) {
        // One-toggle VR stereo (session 8): "vrstereo on|off" at top level
        // == "reentry vrstereo ..." - the streamlined in-headset flow.
        char line[32];
        _snprintf_s(line, sizeof line, _TRUNCATE, "vrstereo %s", args);
        scenedraw::handle_command(line);
    } else if (strcmp(cmd, "vrpreset") == 0) {
        if (strncmp(args, "save", 4) == 0) save_vr_preset();
        else apply_vr_preset();
    } else if (strcmp(cmd, "vrpace") == 0) {
        bvr::vr::handle_pace_command(args); // M8 disconnect-stall guard
    } else if (strcmp(cmd, "vrmirror") == 0) {
        bvr::vr::handle_mirror_command(args); // M8 single-eye desktop mirror
    } else if (strcmp(cmd, "vrcine") == 0) {
        bvr::vr::handle_cine_command(args); // session 22 cinematic quad fallback
    } else if (strcmp(cmd, "vroverlay") == 0) {
        bool on = strncmp(args, "on", 2) == 0;
        bvr::overlay::set_visible(on);
        BVR_LOG("[b1r] overlay %s (seam request)", on ? "ON" : "off");
    } else if (strcmp(cmd, "vrhud") == 0) {
        // Session 19 HUD capture: gameswf HUD redirected off the game frame
        // (clean eyes) and shown as a floating quad in stereo.
        if (strncmp(args, "force on", 8) == 0) {
            bvr::hud::set_force(true);
        } else if (strncmp(args, "force off", 9) == 0) {
            bvr::hud::set_force(false);
        } else if (strncmp(args, "on", 2) == 0) {
            bvr::hud::set_enabled(true);
        } else if (strncmp(args, "off", 3) == 0) {
            bvr::hud::set_enabled(false);
        } else {
            unsigned hd = 0, rd = 0, lk = 0, iv = 0;
            bvr::hud::get_counters(&hd, &rd, &lk, &iv);
            unsigned lbT = 0, lbB = 0;
            bool lb = bvr::hud::letterbox(&lbT, &lbB);
            BVR_LOG("[hud] status: %s force=%d | hudDraws=%u redirects=%u leaks=%u "
                    "hudIntervals=%u | postFx=%u screenOnly=%d letterbox=%d(%u/%u) "
                    "(vrhud on|off|force on|force off|status)",
                    bvr::hud::enabled() ? "ON" : "off", bvr::hud::force() ? 1 : 0, hd, rd,
                    lk, iv, bvr::hud::postfx_count(), bvr::hud::screen_only() ? 1 : 0,
                    lb ? 1 : 0, lbT, lbB);
        }
    } else if (strcmp(cmd, "vrxhair") == 0) {
        // M8 part 2: the flat-screen crosshair. Default HIDDEN; "on" re-shows.
        if (strncmp(args, "on", 2) == 0) {
            g_crosshairVisible.store(true, std::memory_order_relaxed);
            BVR_LOG("[b1r] crosshair ON (flat reticle re-enabled)");
        } else if (strncmp(args, "off", 3) == 0) {
            g_crosshairVisible.store(false, std::memory_order_relaxed);
            BVR_LOG("[b1r] crosshair OFF (ShockPlayer.bReticleDisabled via engine SET)");
        } else {
            BVR_LOG("[b1r] crosshair %s (applied=%d) - vrxhair on|off|status",
                    g_crosshairVisible.load(std::memory_order_relaxed) ? "ON" : "off",
                    g_crosshairApplied);
        }
    } else if (strcmp(cmd, "reentry") == 0) {
        scenedraw::handle_command(args); // DR-5 probe; logs its own echoes
    }
}

// ---- VR preset 1 (session 16 part 3) ---------------------------------------
// Toggles are implied ON; the ini persists only slider VALUES the user tuned.

void vr_preset_path(wchar_t* out, size_t count) {
    swprintf_s(out, count, L"%s\\vrpreset.ini", bvr::log::data_dir());
}

void save_vr_preset() {
    wchar_t path[MAX_PATH];
    vr_preset_path(path, MAX_PATH);
    FILE* f = nullptr;
    if (_wfopen_s(&f, path, L"w") != 0 || !f) {
        BVR_LOG("[b1r] could not write vrpreset.ini");
        return;
    }
    fprintf(f, "# BioShock VR - VR preset 1 slider values (toggles are implied ON)\n");
    fprintf(f, "worldScale=%.1f\n", g_worldScale.load(std::memory_order_relaxed));
    fprintf(f, "headUpUu=%.1f\n", g_headOffUpUu.load(std::memory_order_relaxed));
    fprintf(f, "headFwdUu=%.1f\n", g_headOffFwdUu.load(std::memory_order_relaxed));
    fprintf(f, "ipdMm=%.1f\n", g_ipdMm.load(std::memory_order_relaxed));
    fprintf(f, "gameFovDeg=%.1f\n", g_gameFovDeg.load(std::memory_order_relaxed));
    fprintf(f, "aimTrimLPitch=%.1f\n", aim::trim_pitch_deg(0));
    fprintf(f, "aimTrimLYaw=%.1f\n", aim::trim_yaw_deg(0));
    fprintf(f, "aimTrimRPitch=%.1f\n", aim::trim_pitch_deg(1));
    fprintf(f, "aimTrimRYaw=%.1f\n", aim::trim_yaw_deg(1));
    fprintf(f, "aimPosLFwd=%.1f\n", aim::pos_fwd_cm(0));
    fprintf(f, "aimPosLRight=%.1f\n", aim::pos_right_cm(0));
    fprintf(f, "aimPosLUp=%.1f\n", aim::pos_up_cm(0));
    fprintf(f, "aimPosRFwd=%.1f\n", aim::pos_fwd_cm(1));
    fprintf(f, "aimPosRRight=%.1f\n", aim::pos_right_cm(1));
    fprintf(f, "aimPosRUp=%.1f\n", aim::pos_up_cm(1));
    fprintf(f, "bodyRate=%.2f\n", body::rate_per_sec());
    fprintf(f, "bodyDeadzoneDeg=%.1f\n", body::deadzone_deg());
    fprintf(f, "turnScale=%.2f\n", bvr::input::turn_scale());
    fprintf(f, "snapTurn=%d\n", bvr::input::snap_turn() ? 1 : 0);
    fprintf(f, "snapAngleDeg=%.0f\n", bvr::input::snap_angle_deg());
    fprintf(f, "laserOn=%d\n", aim::laser_enabled() ? 1 : 0);
    fprintf(f, "cineBarsHidden=%d\n", bvr::hud::bars_hidden() ? 1 : 0);
    fprintf(f, "cineDrive=%d\n", static_cast<int>(bvr::vr::cine_drive()));
    fprintf(f, "aimDotOn=%d\n", aim::dot_enabled() ? 1 : 0);
    fprintf(f, "aimDotDistM=%.2f\n", aim::dot_dist_m());
    fprintf(f, "aimDotSizeDeg=%.2f\n", aim::dot_size_deg());
    fprintf(f, "lockOnDisabled=%d\n",
            g_lockOnDisabled.load(std::memory_order_relaxed) ? 1 : 0);
    fprintf(f, "crosshairVisible=%d\n",
            g_crosshairVisible.load(std::memory_order_relaxed) ? 1 : 0);
    {
        float hd = 0, hw = 0, hu = 0;
        bvr::vr::get_hud_quad(&hd, &hw, &hu);
        fprintf(f, "hudQuadDistM=%.2f\n", hd);
        fprintf(f, "hudQuadWidthM=%.2f\n", hw);
        fprintf(f, "hudQuadUpM=%.2f\n", hu);
    }
    fclose(f);
    BVR_LOG("[b1r] VR preset values saved to vrpreset.ini");
    // The per-hand model offsets live in hands.ini; saving them here too makes
    // the one in-headset save button cover every tuned slider. Same for the
    // session-21 per-weapon profiles (weapons.ini).
    hands::save_offsets();
    aim::save_weapon_profiles();
}

void load_vr_preset_values() {
    wchar_t path[MAX_PATH];
    vr_preset_path(path, MAX_PATH);
    FILE* f = nullptr;
    if (_wfopen_s(&f, path, L"r") != 0 || !f) return; // no file = shipped defaults
    char line[128];
    int n = 0;
    float lp = aim::trim_pitch_deg(0), ly = aim::trim_yaw_deg(0);
    float rp = aim::trim_pitch_deg(1), ry = aim::trim_yaw_deg(1);
    float plf = aim::pos_fwd_cm(0), plr = aim::pos_right_cm(0), plu = aim::pos_up_cm(0);
    float prf = aim::pos_fwd_cm(1), prr = aim::pos_right_cm(1), pru = aim::pos_up_cm(1);
    float bodyRate = body::rate_per_sec(), bodyDz = body::deadzone_deg();
    float hudD = 0, hudW = 0, hudU = 0;
    bvr::vr::get_hud_quad(&hudD, &hudW, &hudU);
    while (fgets(line, sizeof line, f)) {
        char key[48] = {};
        float v = 0.0f;
        if (sscanf_s(line, "%47[^=]=%f", key, static_cast<unsigned>(sizeof key), &v) != 2)
            continue;
        ++n;
        if (strcmp(key, "worldScale") == 0) g_worldScale.store(v, std::memory_order_relaxed);
        else if (strcmp(key, "headUpUu") == 0) g_headOffUpUu.store(v, std::memory_order_relaxed);
        else if (strcmp(key, "headFwdUu") == 0) g_headOffFwdUu.store(v, std::memory_order_relaxed);
        else if (strcmp(key, "ipdMm") == 0) g_ipdMm.store(v, std::memory_order_relaxed);
        else if (strcmp(key, "gameFovDeg") == 0) g_gameFovDeg.store(v, std::memory_order_relaxed);
        else if (strcmp(key, "aimTrimLPitch") == 0) lp = v;
        else if (strcmp(key, "aimTrimLYaw") == 0) ly = v;
        else if (strcmp(key, "aimTrimRPitch") == 0) rp = v;
        else if (strcmp(key, "aimTrimRYaw") == 0) ry = v;
        else if (strcmp(key, "aimPosLFwd") == 0) plf = v;
        else if (strcmp(key, "aimPosLRight") == 0) plr = v;
        else if (strcmp(key, "aimPosLUp") == 0) plu = v;
        else if (strcmp(key, "aimPosRFwd") == 0) prf = v;
        else if (strcmp(key, "aimPosRRight") == 0) prr = v;
        else if (strcmp(key, "aimPosRUp") == 0) pru = v;
        else if (strcmp(key, "bodyRate") == 0) bodyRate = v;
        else if (strcmp(key, "bodyDeadzoneDeg") == 0) bodyDz = v;
        else if (strcmp(key, "turnScale") == 0) bvr::input::set_turn_scale(v);
        else if (strcmp(key, "snapTurn") == 0) bvr::input::set_snap_turn(v != 0.0f);
        else if (strcmp(key, "snapAngleDeg") == 0) bvr::input::set_snap_angle_deg(v);
        else if (strcmp(key, "laserOn") == 0)
            aim::handle_command(v != 0.0f ? "laser on" : "laser off");
        else if (strcmp(key, "cineBarsHidden") == 0)
            bvr::hud::set_bars_hidden(v != 0.0f);
        else if (strcmp(key, "cineDrive") == 0) {
            int m = static_cast<int>(v);
            if (m >= 0 && m <= 2) bvr::vr::set_cine_drive(static_cast<bvr::vr::CineDrive>(m));
        }
        else if (strcmp(key, "aimDotOn") == 0)
            aim::handle_command(v != 0.0f ? "dot on" : "dot off");
        else if (strcmp(key, "aimDotDistM") == 0) {
            char cmd[48];
            sprintf_s(cmd, "dot dist %.2f", v);
            aim::handle_command(cmd);
        } else if (strcmp(key, "aimDotSizeDeg") == 0) {
            char cmd[48];
            sprintf_s(cmd, "dot size %.2f", v);
            aim::handle_command(cmd);
        } else if (strcmp(key, "lockOnDisabled") == 0)
            g_lockOnDisabled.store(v != 0.0f, std::memory_order_relaxed);
        else if (strcmp(key, "crosshairVisible") == 0)
            g_crosshairVisible.store(v != 0.0f, std::memory_order_relaxed);
        else if (strcmp(key, "hudQuadDistM") == 0) hudD = v;
        else if (strcmp(key, "hudQuadWidthM") == 0) hudW = v;
        else if (strcmp(key, "hudQuadUpM") == 0) hudU = v;
        else --n;
    }
    fclose(f);
    aim::set_trim(0, lp, ly);
    aim::set_trim(1, rp, ry);
    aim::set_pos_offset(0, plf, plr, plu);
    aim::set_pos_offset(1, prf, prr, pru);
    body::set_tuning(bodyRate, bodyDz);
    if (hudD > 0.0f && hudW > 0.0f) bvr::vr::set_hud_quad(hudD, hudW, hudU);
    if (n) BVR_LOG("[b1r] VR preset: %d value(s) loaded from vrpreset.ini", n);
}

void apply_vr_preset() {
    BVR_LOG("[b1r] VR PRESET 1: arming the full VR configuration");
    bvr::vr::set_enabled(true);        // paces the game to the headset
    bvr::vr::set_camera_mode(true);    // 6DOF head drive
    bvr::vr::set_sr_pair_pacing(true); // one waitFrame per eye pair
    input::handle_command("on");       // motion controllers as gamepad
    // SESSION 28: the preset no longer forces the game-FOV write ON.
    //
    // It used to push option 130, and 130 came from the "129.5 circumscribing"
    // arithmetic, which solved `tan(option/2)*9/16*aspect = tan(H/2)` - i.e. it
    // was derived from the WRONG world-lens law. The measured law is
    // `tanH = tan(option/2)` with no aspect term, so at a square backbuffer
    // option 100 already renders exactly 100x100 deg - close to ideal for a
    // Quest-class eye - and 130 over-widens the render by 30 deg, wasting pixels
    // and shrinking the world inside the eye's actual FOV. It also drags the
    // foreground lens out with it (~141 deg at option 130 square).
    // The user confirmed in-headset that the write had to be turned OFF, which
    // is what the corrected law predicts. `gfov <deg>` still arms it manually,
    // and the global default was already false - this line was the only thing
    // turning it on.
    aim::handle_command("on");         // controller aim (R weapon / L plasmid)
    aim::handle_command("pose aim");   // the runtime AIM pose
    aim::handle_command("origin on");  // ray starts at the hand
    // Session 22: the laser no longer arms here - OFF by default (user's
    // call), applied from the persisted `laserOn` ini key in the load below.
    hands::handle_command("on");       // viewmodel follows the controller
    hands::handle_command("pose aim"); // align to the AIM ray
    body::handle_command("on");        // M7.5: stick-forward = look direction
    load_vr_preset_values();           // tuned sliders (ini) over defaults
    aim::note_preset_baseline();       // seed source for new weapon profiles
    aim::reapply_weapon_profile();     // the active weapon profile beats the baseline
    scenedraw::handle_command("vrstereo on"); // last: 1t + stereo, sticky
    BVR_LOG("[b1r] VR PRESET 1 armed (unwind: vrstereo off + overlay checkboxes)");
}

// Re-assert interval for the engine-exec upkeep below. Was 15 s, which meant a
// shipping session made two calls into the engine's SET handler through a
// hand-built FOutputDevice stub every 15 seconds forever - five times in the 85 s
// of the session-27 crash report. The stub is now measured and self-disabling
// (console_exec.cpp), but the exposure was still 20x more than the job needs:
// the state is re-asserted on the events that can actually undo it (entering
// gameplay, a pawn or level change - see note_world_event) and this timer is only
// a slow safety net for a script-side caller we have not identified.
constexpr uint64_t kExecReassertMs = 300000; // 5 minutes

// Crosshair upkeep (see the globals): push the wanted state through the
// engine's SET handler on change, on a world event, and on the slow safety
// net. Game thread.
void assert_crosshair(uint64_t now) {
    int want = g_crosshairVisible.load(std::memory_order_relaxed) ? 1 : 0;
    bool due = want != g_crosshairApplied ||
               (want == 0 && now - g_crosshairAssertMs >= kExecReassertMs);
    if (!due) return;
    g_crosshairApplied = want;
    g_crosshairAssertMs = now;
    console_exec::run_engine(want ? "set ShockPlayer bReticleDisabled False"
                                  : "set ShockPlayer bReticleDisabled True");
}

// Session 22: zero the pad soft lock-on radius while disabled (default).
// Only asserts the DISABLED state - see the declaration note (the game's
// own value returns on restart when the user re-enables it).
void assert_lockon(uint64_t now) {
    int want = g_lockOnDisabled.load(std::memory_order_relaxed) ? 1 : 0;
    bool due = want != g_lockOnApplied ||
               (want == 1 && now - g_lockOnAssertMs >= kExecReassertMs);
    if (!due) return;
    bool first = g_lockOnApplied != want;
    g_lockOnApplied = want;
    g_lockOnAssertMs = now;
    if (want)
        console_exec::run_engine("set GamepadPlayerInput SoftLockOnRadius 0");
    else if (first)
        BVR_LOG("[b1r] pad lock-on re-enabled: the game's own radius returns "
                "on the next game restart");
}

// One engine-state re-assert, driven by an event rather than a timer. Called on
// the gameplay-view transition, which is the point a new pawn or a fresh level
// can have reset the properties we set.
void note_world_event(const char* why) {
    g_crosshairApplied = -1; // sentinel: forces one assert on the next tick
    g_lockOnApplied = -1;
    BVR_LOG("[b1r] engine-state re-assert queued (%s)", why);
}

void poll_command_file(uint64_t now) {
    if (now - g_lastCmdPollMs < 1000) return;
    g_lastCmdPollMs = now;
    static wchar_t path[MAX_PATH];
    if (!path[0]) {
        // Composed from data_dir() so the per-game subdir (log.cpp) is
        // honored; for BioShock 1 the resulting string is unchanged.
        const wchar_t* dir = bvr::log::data_dir();
        if (!dir[0]) return; // log::init failed - no data dir to poll
        swprintf_s(path, L"%s\\command.txt", dir);
    }
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (!GetFileAttributesExW(path, GetFileExInfoStandard, &fad)) return;
    if (CompareFileTime(&fad.ftLastWriteTime, &g_lastCmdWrite) == 0) return;
    g_lastCmdWrite = fad.ftLastWriteTime;
    FILE* f = _wfsopen(path, L"rt", _SH_DENYNO);
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        char cmd[32];
        int consumed = 0;
        if (sscanf_s(line, "%31s%n", cmd, static_cast<unsigned>(sizeof cmd), &consumed) != 1)
            continue;
        const char* args = line + consumed;
        while (*args == ' ' || *args == '\t') ++args;
        apply_command(cmd, args);
    }
    fclose(f);
}

// XR -> Unreal conversion lives in ue_math.h (shared with the aim ray).
UeAngles hmd_angles(const bvr::vr::HeadPose& hp) {
    return ue_angles_from_xr_quat(hp.qx, hp.qy, hp.qz, hp.qw);
}

// eventPlayerCalcView is __thiscall; __fastcall with a dummy EDX slot is
// register/stack/cleanup-identical and works as a plain free function.
void __fastcall CalcViewDetour(void* self, void* edx, void** viewActor,
                               FVector* loc, FRotator* rot) {
    // DR-5/M4 second pass: while the reentry probe is inside its SECOND
    // build call, run only the original plus the second-pass camera - the
    // full body below must not run twice per frame (it would eat recenter
    // requests, double-poll the command file, and re-run the fov
    // save/restore state machines). Stereo replays pass-1's cached base with
    // the RIGHT eye offset; the probe's yaw delta is the non-stereo fallback.
    float reentryYawDeg = 0.0f;
    if (scenedraw::second_pass_for_current_thread(&reentryYawDeg)) {
        g_original(self, edx, viewActor, loc, rot);
        // Session 22: the 100 ms age gate keeps a resuming CalcView from
        // replaying a base armed before a scene (pass 2 always follows its
        // pass 1 within one tick; anything older is a stale pair).
        if (scenedraw::stereo_active() && g_srBaseValid && loc && rot &&
            GetTickCount64() - g_srBaseStampMs <= 100) {
            *loc = g_srBaseLoc;
            *rot = g_srBaseRot;
            if (g_srBaseEyed) apply_eye_offset(loc, *rot, +1);
            g_eyeCamLoc[1] = *loc; // fg view-sync stash, RIGHT eye
            g_eyeCamRot[1] = *rot;
            g_eyeCamStampMs[1] = GetTickCount64();
        } else if (rot) {
            rot->yaw += static_cast<int32_t>(reentryYawDeg * kRotUnitsPerDegree);
        }
        // The engine's CalcView above re-evaluated the skeleton over the bone
        // drive's pass-1 write; without this the RIGHT eye bakes the engine
        // pose while the left bakes ours (live-proven under flat stereo).
        bones::reapply();
        return;
    }
    g_original(self, edx, viewActor, loc, rot);
    scenedraw::note_calcview();

    g_playerController.store(self, std::memory_order_relaxed);
    uint32_t count = g_callCount.fetch_add(1, std::memory_order_relaxed) + 1;

    float gameFov = *fov_ptr(self);
    g_lastFov.store(gameFov, std::memory_order_relaxed);
    // Auto-claim (session 4): the UShockUserSettings HorizontalFOV int is
    // what the renderer truly consumes each frame, so claiming it keeps the
    // projection layer honest with zero manual matching. While we WRITE the
    // option (VR force / gfov) the readback echoes our write - which is
    // correct, because the renderer really renders it (no cap, ENGINE_NOTES).
    // Fallback while the settings object is not alive yet: the old PC+0xE0
    // telemetry field, better than claiming nothing.
    int32_t* optionFov = patterns::hfov_option_ptr();
    g_lastOptionFov.store(optionFov ? *optionFov : 0, std::memory_order_relaxed);
    bvr::vr::set_rendered_hfov(optionFov ? static_cast<float>(*optionFov) : gameFov);
    if (loc) {
        g_lastLocX.store(loc->x, std::memory_order_relaxed);
        g_lastLocY.store(loc->y, std::memory_order_relaxed);
        g_lastLocZ.store(loc->z, std::memory_order_relaxed);
    }
    if (rot) {
        g_lastPitch.store(rot->pitch, std::memory_order_relaxed);
        g_lastYaw.store(rot->yaw, std::memory_order_relaxed);
        g_lastRoll.store(rot->roll, std::memory_order_relaxed);
    }

    if (!g_loggedFirstFire.exchange(true)) {
        BVR_LOG("[b1r] calcview first fire: pc=%p viewactor=%p loc=(%.1f %.1f %.1f) "
                "rot=(%d %d %d) fov=%.1f",
                self, viewActor ? *viewActor : nullptr,
                loc ? loc->x : 0.0f, loc ? loc->y : 0.0f, loc ? loc->z : 0.0f,
                rot ? rot->pitch : 0, rot ? rot->yaw : 0, rot ? rot->roll : 0, gameFov);
    }

    uint64_t now = GetTickCount64();
    g_lastCalcViewMs = now; // session 22: cutscene-silence detector
    poll_command_file(now);
    // Overlay preset buttons land here (game thread; the overlay draws on
    // the render thread and only sets the pending flags).
    if (g_vrPresetPending.exchange(false, std::memory_order_relaxed)) apply_vr_preset();
    if (g_vrPresetSavePending.exchange(false, std::memory_order_relaxed)) save_vr_preset();
    if (uint64_t req = g_resWritePending.exchange(0, std::memory_order_relaxed)) {
        game_ini::write_viewport(static_cast<uint32_t>(req >> 32),
                                 static_cast<uint32_t>(req & 0xFFFFFFFFu));
    }
    // M5: pump the engine's own pad pipeline against the synthetic gamepad
    // (self-throttles to once per present; no-op while vrinput is off).
    input_drive::on_frame(now);
    assert_crosshair(now); // M8 part 2: flat crosshair hidden by default
    assert_lockon(now);    // session 22: pad aim magnetism off by default
    if (g_logCamera.load(std::memory_order_relaxed)) {
        if (g_lastHeartbeatMs == 0) {
            g_lastHeartbeatMs = now;
            g_heartbeatBaseCount = count;
        } else if (now - g_lastHeartbeatMs >= 1000) {
            BVR_LOG("[b1r] camera: loc=(%.1f %.1f %.1f) rot=(%d %d %d) fov=%.1f (%u calls/s)",
                    loc ? loc->x : 0.0f, loc ? loc->y : 0.0f, loc ? loc->z : 0.0f,
                    rot ? rot->pitch : 0, rot ? rot->yaw : 0, rot ? rot->roll : 0,
                    gameFov, count - g_heartbeatBaseCount);
            g_lastHeartbeatMs = now;
            g_heartbeatBaseCount = count;
        }
    } else {
        g_lastHeartbeatMs = 0;
    }

    // M3: drive the camera from the HMD pose. Pitch/roll are absolute (head
    // owns them); yaw is additive on the game's yaw so mouse/gamepad turning
    // still works; position adds the recenter-relative head offset, rotated
    // into the game yaw frame and scaled UU-per-meter.
    bool vrDrove = false;
    float vrFov = 0.0f;
    // M6: the aim ray must be built in the SAME frame as the camera, so keep
    // the pre-head-offset camera loc and the yaw the drive added.
    FVector baseLoc = loc ? *loc : FVector{};
    float driveYawOffsetRad = 0.0f;
    // The same quantity in rotator units - what M7.5 transfers to the body.
    int32_t residualUnits = 0;
    int32_t gameYawUnitsRaw = rot ? rot->yaw : 0; // the engine's own body yaw

    // Session 22: the strict gameplay-view verdict (body.cpp predicate, no
    // menu-attract escape hatch) now gates the live head drive, the FOV
    // write, and the eye offsets, so it is computed up front - it used to
    // live in the FrameContext publish below. Published to the render thread
    // too: the cinematic quad fallback keys on it (and on its staleness -
    // scripted cameras bypass CalcView entirely).
    bool strictGameplay = body::is_gameplay_view(viewActor ? *viewActor : nullptr);
    bvr::vr::publish_gameplay_view(strictGameplay);

    // Session 29: the cinematic drive policy (vrcine drive off|authored|
    // authored+look). `cineHold` is the draw-based signal ORed with the pixel
    // watch - it must NOT be plain letterbox(), because with the bars
    // suppressed there are no black pixels left to detect.
    bool cineHold = bvr::hud::cinematic_hold();
    bvr::vr::CineDrive cineMode = bvr::vr::cine_drive();
    bool cineSuspend = cineHold && cineMode == bvr::vr::CineDrive::Authored;
    bool cineLook = cineHold && cineMode == bvr::vr::CineDrive::AuthoredLook;

    bvr::vr::HeadPose hp{};
    bool driveHead = false;
    bool liveHead = false;
    if (recorder::playing()) {
        // Session 20 replay lane: the recorded head (position + quat) drives
        // the camera; a frame recorded with no drive faithfully leaves the
        // camera alone. The sim and live lanes are locked out while playing -
        // and so is the auto-recenter (play restored the recorded reference).
        driveHead = recorder::replay_head(hp);
    } else if (now < g_simHead.deadline) {
        // Synthetic head pose, XR convention (same quat builder simpose uses
        // for the hand). Position stays at the recenter origin - rotation is
        // what every head-coupling report has been about.
        float q[4];
        xr_local_trim_quat(g_simHead.pitchDeg / kRadToDeg, g_simHead.yawDeg / kRadToDeg,
                           g_simHead.rollDeg / kRadToDeg, q);
        hp = {};
        hp.qx = q[0];
        hp.qy = q[1];
        hp.qz = q[2];
        hp.qw = q[3];
        driveHead = true;
    } else if (strictGameplay && !bvr::vr::cinematic_active() && !cineSuspend &&
               bvr::vr::vr_camera_mode() && bvr::vr::get_head_pose(hp)) {
        // Session 22: the live lane is gated on the strict view AND the
        // cinematic fallback - the HMD must not steer scripted/menu cameras
        // (their content lands on the quad screen, and head-steering it
        // would wobble the whole screen). Round 2: also suspended while the
        // engine letterbox is up (plasmid FMV sequences) so the AUTHORED
        // camera choreography plays exactly like flat - stereo stays (the
        // eye offsets ride the authored camera). The sim and replay lanes
        // above stay ungated for flat tests.
        driveHead = true;
        liveHead = true;
    }
    if (loc && rot && driveHead) {
        UeAngles a = hmd_angles(hp);
        if (g_recenterRequested.exchange(false, std::memory_order_relaxed) || !g_haveRecenter) {
            g_recenterPose = hp;
            g_recenterYawUnits = static_cast<int32_t>(lroundf(a.yawRad * kRotUnitsPerRadian));
            g_haveRecenter = true;
            body::on_reset("recentered");
            BVR_LOG("[b1r] vr camera recentered (yaw %.1f deg)", a.yawRad * 57.29578f);
        }

        // Session 29 authored+look: the head adds a rotation DELTA on top of
        // the authored camera and nothing else.
        //
        // This cannot reuse the gameplay path. Two lines below write pitch and
        // roll ABSOLUTELY from the head - fine when the head owns the camera,
        // fatal here, because it would erase the authored choreography (the
        // session-22 heartbeat measured authored roll walking -7773..-8189
        // through the wake-up shot). The reference is the head orientation at
        // the moment the cutscene began, so the first cinematic frame is
        // framed exactly as authored and the player's own motion accumulates
        // from there. No positional term at all: the camera can never be
        // dollied out of the authored shot or into geometry.
        if (cineLook) {
            int32_t hp_ = static_cast<int32_t>(lroundf(a.pitchRad * kRotUnitsPerRadian));
            int32_t hy_ = static_cast<int32_t>(lroundf(a.yawRad * kRotUnitsPerRadian));
            int32_t hr_ = static_cast<int32_t>(lroundf(a.rollRad * kRotUnitsPerRadian));
            if (!g_cineLookValid) {
                g_cineLookPitch = hp_;
                g_cineLookYaw = hy_;
                g_cineLookRoll = hr_;
                g_cineLookValid = true;
                BVR_LOG("[b1r] authored+look reference captured (head pitch %.1f yaw %.1f "
                        "roll %.1f deg) - the authored shot starts unmodified",
                        a.pitchRad * 57.29578f, a.yawRad * 57.29578f, a.rollRad * 57.29578f);
            }
            rot->pitch += wrap_rot(hp_ - g_cineLookPitch);
            rot->yaw += wrap_rot(hy_ - g_cineLookYaw);
            rot->roll += wrap_rot(hr_ - g_cineLookRoll);
            // residualUnits stays 0: the look must not reach body::on_calcview
            // or the pawn silently rotates under the authored camera.
            driveYawOffsetRad = 0.0f;
            vrDrove = true;
        } else {

        // Integer all the way through: the head-look residual is the ONLY
        // thing added to the game's own yaw, and the M7.5 transfer subtracts
        // from it by exactly the units it hands to the body.
        int32_t gameYawUnits = rot->yaw;
        int32_t headYawUnits = static_cast<int32_t>(lroundf(a.yawRad * kRotUnitsPerRadian));
        residualUnits = wrap_rot(headYawUnits - g_recenterYawUnits);
        float gameYawRad = static_cast<float>(gameYawUnits) / kRotUnitsPerRadian;
        rot->pitch = static_cast<int32_t>(a.pitchRad * kRotUnitsPerRadian);
        rot->roll = static_cast<int32_t>(a.rollRad * kRotUnitsPerRadian);
        driveYawOffsetRad = static_cast<float>(residualUnits) / kRotUnitsPerRadian;
        rot->yaw = gameYawUnits + residualUnits;

        float dxr[3] = {hp.px - g_recenterPose.px, hp.py - g_recenterPose.py,
                        hp.pz - g_recenterPose.pz};
        float d[3];
        xr_to_ue(dxr, d);
        float scale = g_worldScale.load(std::memory_order_relaxed);
        // Into the recenter-local frame, then out by the game yaw (which is
        // where recenter-forward points now, since our yaw is purely additive).
        float recenterYawRad = recenter_yaw_rad();
        float c = cosf(-recenterYawRad), s = sinf(-recenterYawRad);
        float lx = d[0] * c - d[1] * s;
        float ly = d[0] * s + d[1] * c;
        float cg = cosf(gameYawRad), sg = sinf(gameYawRad);
        float ox = (lx * cg - ly * sg) * scale;
        float oy = (lx * sg + ly * cg) * scale;
        float oz = d[2] * scale;
        loc->x += ox;
        loc->y += oy;
        loc->z += oz;
        g_headOffX.store(ox, std::memory_order_relaxed);
        g_headOffY.store(oy, std::memory_order_relaxed);
        g_headOffZ.store(oz, std::memory_order_relaxed);

        // User head-anchor offset (sliders; see the atomics above). Vertical
        // is world-up; forward rides the final view yaw, horizontal only.
        float hoUp = g_headOffUpUu.load(std::memory_order_relaxed);
        float hoFwd = g_headOffFwdUu.load(std::memory_order_relaxed);
        if (hoUp != 0.0f || hoFwd != 0.0f) {
            float vyaw = static_cast<float>(rot->yaw) / kRotUnitsPerRadian;
            loc->x += cosf(vyaw) * hoFwd;
            loc->y += sinf(vyaw) * hoFwd;
            loc->z += hoUp;
        }

        // AlternateEye (M4 rung 1): shift the camera half an IPD along
        // view-right; core flips the sign after each submitted frame so
        // successive game frames render alternating eyes. Suppressed under
        // SequentialReentry stereo (rung 2), which applies both eye offsets
        // itself at the end of this body.
        int eyeSign = scenedraw::stereo_active() ? 0 : bvr::vr::current_eye_sign();
        // Session 22: routed through apply_eye_offset so SR and AER share ONE
        // implementation (full-rotation right axis, head-roll correct).
        if (eyeSign != 0) apply_eye_offset(loc, *rot, eyeSign);

        vrFov = g_forceHeadsetFov.load(std::memory_order_relaxed)
                    ? bvr::vr::suggested_hfov_deg()
                    : 0.0f; // 0 = leave the game's own FOV in place
        vrDrove = true;

        // In-headset telemetry (vrbones log on): the RAW head sample this
        // frame's camera was driven from, plus the recenter reference.
        if (bones::telemetry_on()) {
            static uint64_t lastTlm = 0;
            if (now - lastTlm >= 200) {
                lastTlm = now;
                BVR_LOG("[tlm] head xr p=(%.3f %.3f %.3f) yaw=%.1f pitch=%.1f roll=%.1f | "
                        "recenter yaw=%.1f p=(%.3f %.3f %.3f) | headOff=(%.1f %.1f %.1f)",
                        hp.px, hp.py, hp.pz, a.yawRad * 57.29578f, a.pitchRad * 57.29578f,
                        a.rollRad * 57.29578f, recenter_yaw_rad() * 57.29578f, g_recenterPose.px,
                        g_recenterPose.py, g_recenterPose.pz, ox, oy, oz);
            }
        }
        } // else (not cineLook)
    }

    // Game-FOV write via the settings object (the renderer's real per-frame
    // source). Precedence: VR forced headset fov > manual gfov. One-shot
    // save/restore so the user's option value returns untouched.
    if (optionFov) {
        // Session 22: both wants are gated on the strict view, so leaving
        // gameplay (scripted camera that still CalcViews, menus) restores the
        // authored FOV through the existing latch and re-arms on return. The
        // CalcView-SILENT case (descent) cannot reach this code - scenedraw's
        // BuildDetour calls restore_game_fov_if_stale() for it.
        bool wantVr = strictGameplay && vrDrove && vrFov > 0.0f;
        bool wantManual = strictGameplay && g_gameFovWrite.load(std::memory_order_relaxed);
        if (wantVr || wantManual) {
            if (!g_wasWritingGameFov) {
                g_savedGameFov = *optionFov;
                g_savedGameFovOwner = optionFov;
                g_wasWritingGameFov = true;
                BVR_LOG("[b1r] game fov write ON (saved option %d from %p)", g_savedGameFov,
                        optionFov);
            } else if (optionFov != g_savedGameFovOwner) {
                // The scan re-bound to a different instance while we were
                // writing. The old object's saved value means nothing here, so
                // re-seed rather than carrying it across.
                BVR_LOG("[b1r] game fov owner changed %p -> %p - re-seeding the saved option "
                        "(was %d, now %d)",
                        static_cast<const void*>(g_savedGameFovOwner), optionFov, g_savedGameFov,
                        *optionFov);
                g_savedGameFov = *optionFov;
                g_savedGameFovOwner = optionFov;
            }
            float want = wantVr ? vrFov : g_gameFovDeg.load(std::memory_order_relaxed);
            int32_t wantInt = static_cast<int32_t>(want + 0.5f);
            if (!write_option_fov(optionFov, wantInt)) {
                BVR_LOG("[b1r] game fov write FAULTED at %p - disarming the FOV writer", optionFov);
                g_wasWritingGameFov = false;
                g_savedGameFovOwner = nullptr;
                g_gameFovWrite.store(false, std::memory_order_relaxed);
                g_forceHeadsetFov.store(false, std::memory_order_relaxed);
            }
        } else if (g_wasWritingGameFov) {
            // Only restore into the object the value came from.
            if (optionFov == g_savedGameFovOwner && write_option_fov(optionFov, g_savedGameFov))
                BVR_LOG("[b1r] game fov write OFF (restored option %d)", g_savedGameFov);
            else
                BVR_LOG("[b1r] game fov write OFF (saved %d NOT restored - object is %p, the "
                        "value came from %p)",
                        g_savedGameFov, optionFov,
                        static_cast<const void*>(g_savedGameFovOwner));
            g_wasWritingGameFov = false;
            g_savedGameFovOwner = nullptr;
        }
    }
    g_vrDriving.store(vrDrove, std::memory_order_relaxed);
    if (!vrDrove) {
        g_headOffX.store(0.0f, std::memory_order_relaxed);
        g_headOffY.store(0.0f, std::memory_order_relaxed);
        g_headOffZ.store(0.0f, std::memory_order_relaxed);
    }

    if (loc) {
        loc->x += g_offsetX.load(std::memory_order_relaxed);
        loc->y += g_offsetY.load(std::memory_order_relaxed);
        loc->z += g_offsetZ.load(std::memory_order_relaxed);
        if (g_wobble.load(std::memory_order_relaxed)) {
            // 1 Hz vertical bob; the 60 s modulus is a whole number of periods,
            // so the wrap is seamless and sinf keeps full precision.
            float t = static_cast<float>(now % 60000) / 1000.0f;
            loc->z += g_wobbleAmp.load(std::memory_order_relaxed) * sinf(2.0f * kPi * t);
        }
    }
    if (rot) {
        rot->pitch += static_cast<int32_t>(g_pitchDeg.load(std::memory_order_relaxed) * kRotUnitsPerDegree);
        rot->yaw   += static_cast<int32_t>(g_yawDeg.load(std::memory_order_relaxed) * kRotUnitsPerDegree);
        rot->roll  += static_cast<int32_t>(g_rollDeg.load(std::memory_order_relaxed) * kRotUnitsPerDegree);
    }

    // M6/M7: publish the frame the camera just produced so the aim ray and the
    // hand viewmodel both land in exactly this frame (pre eye-offset - the eye
    // shift belongs to the render, not to where the player is standing).
    {
        FrameContext fc{};
        fc.vrDriving = vrDrove;
        if (loc) {
            fc.camX = loc->x;
            fc.camY = loc->y;
            fc.camZ = loc->z;
        }
        fc.baseX = baseLoc.x;
        fc.baseY = baseLoc.y;
        fc.baseZ = baseLoc.z;
        if (rot) {
            fc.camPitch = rot->pitch;
            fc.camYaw = rot->yaw;
            fc.camRoll = rot->roll;
        }
        fc.driveYawOffsetRad = driveYawOffsetRad;
        fc.recenterYawRad = recenter_yaw_rad();
        fc.recenterPx = g_recenterPose.px;
        fc.recenterPy = g_recenterPose.py;
        fc.recenterPz = g_recenterPose.pz;
        fc.worldScale = g_worldScale.load(std::memory_order_relaxed);
        fc.viewActor = viewActor ? *viewActor : nullptr;
        fc.pc = self;
        g_lastViewActor.store(fc.viewActor, std::memory_order_relaxed);

        // Strict gameplay-view (hoisted above the drive lanes since session
        // 22), published to the input bridge as the stick-pitch-kill gate
        // and logged on transition - the harness's generic "in gameplay"
        // signal (boot.ps1 watches for this line; any save, any level).
        bvr::input::publish_vr_gameplay(vrDrove && strictGameplay);
        static int s_lastViewState = -1;
        int viewState = strictGameplay ? 1 : 0;
        if (viewState != s_lastViewState) {
            s_lastViewState = viewState;
            BVR_LOG("[b1r] view state: %s",
                    strictGameplay ? "GAMEPLAY (ShockPlayer view)" : "menu/cutscene");
            // The object scanners latch dormant after repeated misses rather
            // than rescanning forever (session 27); entering gameplay is the
            // event that plausibly created what they were looking for, so it is
            // the one place allowed to wake them.
            if (strictGameplay) {
                patterns::hfov_scan_rearm("entered gameplay view");
                aim::weapon_scan_rearm("entered gameplay view");
                // Same event drives the engine-property re-assert, which used to
                // run off a 15 s timer forever (session 27).
                note_world_event("entered gameplay view");
            }
        }

        // Session 29: the cinematic edge instrument.
        //
        // The roadmap item says "the hands/aim/laser drives are ungated during
        // cinematics". Reading the code says the opposite - driveHead is false
        // under a letterbox, so vrDrove is false, and all three consumers
        // already bail on ctx.vrDriving - which would make the real defect our
        // STICKY bone state rather than a missing gate. That distinction comes
        // from reading, not measuring, so this line exists to refute it: it
        // reports, at each edge, whether the drives were actually live and what
        // the bone drive left behind. Believe the line, not the paragraph.
        {
            bool lb = bvr::hud::letterbox(nullptr, nullptr);
            bool bars = bvr::hud::bar_draw_active();
            bool cine = bvr::vr::cinematic_active();
            int cineState = (lb || bars || cine) ? 1 : 0;
            static int s_lastCine = -1;
            // s_lastCine < 0 is the startup baseline, not an edge - adopting it
            // silently keeps the log honest about what a transition means.
            if (s_lastCine < 0 && cineState == 0) {
                s_lastCine = 0;
            } else if (cineState != s_lastCine) {
                bool entering = s_lastCine != 1 && cineState == 1;
                s_lastCine = cineState;
                int hidden = -1;
                unsigned long long cacheAge = 0;
                bool refValid = false;
                bones::debug_state(&hidden, &cacheAge, &refValid);
                // Both cinematic sources are reported separately and never
                // merged into one verdict: they are independent measurements
                // (a draw-call fingerprint vs a backbuffer readback), so
                // agreement is evidence and disagreement is a bug worth
                // seeing. With bars hidden, barDraw=1 lb=0 is the EXPECTED
                // steady state, not a fault.
                BVR_LOG("[b1r] cine edge %s (barDraw=%d letterbox=%d cineQuad=%d) | drives: "
                        "vrDriving=%d strict=%d aimArmed=%d | bones: hiddenHand=%d "
                        "cacheAge=%llums refValid=%d",
                        cineState ? "ENTER" : "exit", bars ? 1 : 0, lb ? 1 : 0, cine ? 1 : 0,
                        vrDrove ? 1 : 0, strictGameplay ? 1 : 0, aim::active() ? 1 : 0, hidden,
                        cacheAge, refValid ? 1 : 0);
                // Entering is where the release has to happen: reapply() would
                // otherwise keep repainting our pose over the authored
                // animation for another ~6 frames, dirty flag included.
                if (entering && cineMode != bvr::vr::CineDrive::Off)
                    bones::release("cinematic started");
                // Both edges drop the look reference so the next shot opens
                // framed as authored rather than wherever this one ended.
                g_cineLookValid = false;
            }
        }

        // Radial-wheel pitch guard (session 19 part 2): the stick-pitch kill
        // lifts while a grip/bumper is held so the weapon wheel can read
        // stick Y - but the wheel's binding state keeps the look axis bound
        // too, so REAL pitch accumulates on the PC during the hold. Snapshot
        // the PC pitch at bumper-down, write it back at release: the wheel
        // selects, the body pitch returns exactly where it was. Same field
        // the stick writes (PC rotation, pitch at +0x0), so the engine's own
        // clamp semantics hold.
        {
            bool lb = false, rb = false;
            bvr::input::last_composed_bumpers(&lb, &rb);
            bool bumperHeld = lb || rb;
            static bool s_wasHeld = false;
            static int32_t s_savedPitch = 0;
            static void* s_savedPc = nullptr;
            int32_t* pcPitch = reinterpret_cast<int32_t*>(static_cast<uint8_t*>(self) +
                                                          patterns::kActorViewDirOffset);
            if (bumperHeld && !s_wasHeld && vrDrove && strictGameplay) {
                s_savedPitch = *pcPitch;
                s_savedPc = self;
            } else if (!bumperHeld && s_wasHeld) {
                if (s_savedPc == self) *pcPitch = s_savedPitch;
                s_savedPc = nullptr; // never restore across a world change
            }
            s_wasHeld = bumperHeld;
        }

        // THE HARD-INVARIANT INSTRUMENT (session 17). Run a FIXED XR pose
        // through the unmodified context and log where it lands. This is the
        // exact function the aim ray and the viewmodel both call, with the
        // real recenter values, independent of any synthetic lane - so with a
        // static scene, any change in this line between `vrbody on` and
        // `vrbody off` IS the hand-follows-the-head defect. gameYaw and
        // recenterYaw must move together (that is the relabel); loc and rot
        // must not move at all.
        if (bones::telemetry_on()) {
            static uint64_t lastXrmap = 0;
            if (now - lastXrmap >= 200) {
                lastXrmap = now;
                const float probePos[3] = {0.15f, -0.20f, -0.35f};
                const float probeQuat[4] = {0.0f, 0.0f, 0.0f, 1.0f};
                GamePose gp = xr_pose_to_game(fc, probePos, probeQuat);
                BVR_LOG("[tlm] xrmap loc=(%.3f %.3f %.3f) rot=(%d %d %d) "
                        "gameYaw=%.5f recenterYaw=%.5f camYaw=%d dyaw=%.5f",
                        gp.loc.x, gp.loc.y, gp.loc.z, gp.rot.pitch, gp.rot.yaw, gp.rot.roll,
                        static_cast<double>(fc.camYaw) / kRotUnitsPerRadian -
                            fc.driveYawOffsetRad,
                        fc.recenterYawRad, fc.camYaw, fc.driveYawOffsetRad);
            }
        }

        // Session 20 vrrec: record (or replay) this frame's input state -
        // BEFORE aim/hands consume it, so a replayed frame feeds all three
        // funnel consumers one consistent world. Once per game tick by
        // construction (the stereo second pass skips this whole body).
        recorder::on_tick(fc, hp, vrDrove, liveHead);

        aim::on_calcview(fc);
        // The viewmodel write goes LAST in the frame: the engine placed the
        // hands during its own tick, so ours has to be the one that survives.
        hands::on_calcview(fc);
    }

    // Foreground lens match: post-tick, pre-render, every frame - nothing
    // engine-side fights the write (flat-proven: a poke held for minutes).
    {
        float* fgFov = reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(self) +
                                                patterns::kPcForegroundFovOffset);
        int32_t* opt = patterns::hfov_option_ptr();
        if (g_fgFovMatch.load(std::memory_order_relaxed)) {
            if (fgFov && opt && *opt > 0) {
                if (g_fgFovSaved.load(std::memory_order_relaxed) == 0.0f)
                    g_fgFovSaved.store(*fgFov, std::memory_order_relaxed);
                float tanW = tanf(static_cast<float>(*opt) * 0.5f / kRadToDeg);
                // SESSION 28: the match constant is (4/3)*(h/w), NOT 0.75.
                //
                // The two passes anchor OPPOSITE axes (ENGINE_NOTES "Session
                // 28", dump-measured): the world pass fixes the horizontal
                // (tanH = tan(option/2), tanV = tanH*h/w) and the foreground
                // pass fixes the vertical (tanV = tan(fgFov/2)*3/4, tanH =
                // tanV*w/h). Equating the two verticals:
                //     tan(fgHalf)*3/4 = tan(option/2)*(h/w)
                //  => tan(fgHalf)    = tan(option/2)*(4/3)*(h/w)
                // 0.75 IS (4/3)*(9/16), i.e. that expression at 16:9 and
                // nowhere else. Off 16:9 the shipped constant left the fg lens
                // 1.7778/aspect narrower than the world - 1.78x at the square
                // backbuffer the README recommends.
                //
                // Why this is load-bearing rather than cosmetic: ONE projection
                // layer claim serves the whole eye image, so while the two
                // lenses differ only one of {world, viewmodel} can be
                // geometrically correct. Before the session-28 watch fix the
                // claim accidentally carried the FG lens - hands right, world
                // warping. Fixing the world moved the error onto the hands
                // ("the gun moves when the headset moves"). Matching the lenses
                // is what makes both correct at once, and it also makes
                // bones.cpp's render-lock assumption ("k collapses to 1") TRUE
                // off 16:9, which it silently was not.
                //
                // Independent corroboration: BioVRDev use exactly
                // 2*atan(tan(fov/2)*(4/3)/aspect) - recorded in
                // docs/RESEARCH.md since session 20 and never acted on.
                //
                // At 16:9 this is bit-identical to the shipped 0.75, so the
                // session-16 in-headset calibration is preserved by
                // construction. `vrfgfov legacy on` restores the old constant
                // for an instant A/B.
                float k = 0.75f;
                unsigned bbW = 0, bbH = 0;
                if (!g_fgFovLegacy.load(std::memory_order_relaxed) &&
                    bvr::hud::backbuffer_dims(&bbW, &bbH) && bbW && bbH)
                    k = (4.0f / 3.0f) * (static_cast<float>(bbH) /
                                         static_cast<float>(bbW));
                float fg = 2.0f * atanf(tanW * k) * kRadToDeg;
                *fgFov = fg;
                g_fgFovWritten.store(fg, std::memory_order_relaxed);
                g_fgFovK.store(k, std::memory_order_relaxed);
            }
        } else {
            float saved = g_fgFovSaved.load(std::memory_order_relaxed);
            if (saved != 0.0f && fgFov) {
                *fgFov = saved;
                g_fgFovSaved.store(0.0f, std::memory_order_relaxed);
                g_fgFovWritten.store(0.0f, std::memory_order_relaxed);
            }
        }
    }

    // SequentialReentry stereo (M4 rung 2): this normal pass is the LEFT eye.
    // Cache the final un-eyed camera for pass 2's replay, then offset. Works
    // with or without the VR drive (flat A/B testing uses the game camera).
    if (loc && rot && scenedraw::stereo_active()) {
        g_srBaseLoc = *loc;
        g_srBaseRot = *rot;
        g_srBaseValid = true;
        g_srBaseStampMs = now;
        // Session 22: while the cinematic quad fallback holds (or the view is
        // not strict gameplay), both eyes stay IDENTICAL - the quad shows one
        // image per present and an IPD offset would jitter it. The renderer
        // consumes CalcView's camera even in scripted scenes (dump-proven),
        // so this suppression is load-bearing, not belt-and-suspenders.
        g_srBaseEyed = strictGameplay && !bvr::vr::cinematic_active();
        if (g_srBaseEyed) apply_eye_offset(loc, *rot, -1);
        g_eyeCamLoc[0] = *loc; // fg view-sync stash, LEFT eye
        g_eyeCamRot[0] = *rot;
        g_eyeCamStampMs[0] = GetTickCount64();
    } else {
        g_srBaseValid = false;
    }

    // FOV: the VR drive wins over the manual override; both share the same
    // save/restore bookkeeping so the game value returns when everything is off.
    bool wantVrFov = vrDrove && vrFov > 0.0f;
    if (wantVrFov || g_fovOverride.load(std::memory_order_relaxed)) {
        if (!g_wasOverridingFov) {
            g_savedFov = gameFov; // remember the game's value to restore later
            g_wasOverridingFov = true;
        }
        *fov_ptr(self) = wantVrFov ? vrFov : g_fovDeg.load(std::memory_order_relaxed);
    } else if (g_wasOverridingFov) {
        *fov_ptr(self) = g_savedFov; // one-shot restore
        g_wasOverridingFov = false;
    }

    // M7.5 body-follows-head yaw transfer, LAST in the frame and for a
    // load-bearing reason: aim::on_calcview and hands::on_calcview above
    // consumed the (driveYawOffset, recenterYaw) pair, and that pair must
    // describe the body facing the engine actually has RIGHT NOW. The write
    // below is state for the NEXT frame - by then the engine's own rot->yaw
    // carries the transferred amount and the residual has shrunk by exactly
    // the same integer, so the camera and the hand mapping are unchanged.
    // Transfer first and the hand would be off by T for one frame, every
    // frame: the very defect the hard invariant forbids.
    // Camera continuity, sampled EVERY frame (a 5 Hz line cannot see a
    // single-frame jump). The transfer's whole safety story is "the camera is
    // unchanged", so this tracks the largest per-frame step in the final
    // camera yaw and counts the frames that exceed a couple of rotation units.
    // Arming the transfer must not move either number: if the recenter fails
    // to absorb what the body took, camYaw jumps ~8192 units and then runs
    // away, and this line shows it inside one second.
    if (rot && bones::telemetry_on()) {
        static bool haveYs = false;
        static int32_t prevYs = 0;
        static int32_t maxStep = 0;
        static uint32_t nbig = 0, frames = 0;
        static uint64_t lastYs = 0;
        if (haveYs) {
            int32_t step = wrap_rot(rot->yaw - prevYs);
            if (abs(step) > abs(maxStep)) maxStep = step;
            if (abs(step) > 2) ++nbig;
        }
        prevYs = rot->yaw;
        haveYs = true;
        ++frames;
        if (now - lastYs >= 1000) {
            lastYs = now;
            BVR_LOG("[tlm] yawstep max=%+d units (%.4f deg) nbig=%u frames=%u | "
                    "camYaw=%d gameYaw=%d resid=%+d recenter=%.4f deg",
                    maxStep, maxStep / kRotUnitsPerDegree, nbig, frames, rot->yaw,
                    gameYawUnitsRaw, residualUnits,
                    g_recenterYawUnits / kRotUnitsPerDegree);
            maxStep = 0;
            nbig = 0;
            frames = 0;
        }
    }

    {
        int32_t moved = body::on_calcview(self, viewActor ? *viewActor : nullptr,
                                          gameYawUnitsRaw, residualUnits, vrDrove);
        // Absorb EXACTLY what the body took - never the amount we asked for.
        // These two are the same integer, which is what makes the invariant a
        // theorem instead of a tolerance.
        if (moved) g_recenterYawUnits = wrap_rot(g_recenterYawUnits + moved);
    }

    // Session 22 snap turn: shift the recenter composite by one step per
    // queued edge - subtracting from recenterYaw raises the residual exactly
    // like a physical head turn, and the M7.5 transfer above carries the
    // body on the following frames (instant at the shipped rate 0). The
    // composite invariant is untouched by construction; effect lands next
    // CalcView (this frame's residual was already consumed).
    if (g_haveRecenter && vrDrove) {
        if (int steps = bvr::input::take_snap_steps()) {
            int32_t units = static_cast<int32_t>(
                lroundf(bvr::input::snap_angle_deg() * kRotUnitsPerDegree * steps));
            g_recenterYawUnits = wrap_rot(g_recenterYawUnits - units);
            BVR_LOG("[b1r] snap turn %+d step(s) (%.0f deg each)", steps,
                    bvr::input::snap_angle_deg());
        }
    }
}

void atomic_slider(const char* label, std::atomic<float>& value, float lo, float hi) {
    float v = value.load(std::memory_order_relaxed);
    if (ImGui::SliderFloat(label, &v, lo, hi)) value.store(v, std::memory_order_relaxed);
}

} // namespace

bool install(void* eventPlayerCalcView) {
    if (!eventPlayerCalcView) return false;

    MH_STATUS status = MH_CreateHook(eventPlayerCalcView,
                                     reinterpret_cast<void*>(&CalcViewDetour),
                                     reinterpret_cast<void**>(&g_original));
    if (status != MH_OK) {
        BVR_LOG("[b1r] MH_CreateHook(calcview) failed: %s", MH_StatusToString(status));
        return false;
    }
    // Self-enabling so this hook's activation never rides on another module's
    // MH_EnableHook(MH_ALL_HOOKS).
    status = MH_EnableHook(eventPlayerCalcView);
    if (status != MH_OK) {
        BVR_LOG("[b1r] MH_EnableHook(calcview) failed: %s", MH_StatusToString(status));
        MH_RemoveHook(eventPlayerCalcView);
        return false;
    }

    g_target = eventPlayerCalcView;
    g_hookLive.store(true, std::memory_order_relaxed);
    BVR_LOG("[b1r] calcview hook installed (target %p)", eventPlayerCalcView);
    return true;
}

bool fg_fov_match_active() {
    return g_fgFovMatch.load(std::memory_order_relaxed) &&
           g_fgFovWritten.load(std::memory_order_relaxed) > 0.0f;
}

bool hook_live() {
    return g_hookLive.load(std::memory_order_relaxed);
}

// Session 22: scripted cameras bypass eventPlayerCalcView, so the FOV write's
// normal restore path (inside CalcViewDetour) cannot run during them. The
// scene-build detour - which keeps firing on the same game thread - calls
// these instead. Re-arm is automatic on the first CalcView after the scene.
bool calcview_silent(uint64_t staleMs) {
    return g_lastCalcViewMs != 0 && GetTickCount64() - g_lastCalcViewMs > staleMs;
}

void restore_game_fov_if_stale(uint64_t staleMs) {
    if (!g_wasWritingGameFov || !calcview_silent(staleMs)) return;
    int32_t* optionFov = patterns::hfov_option_ptr();
    if (!optionFov) return;
    // Same ownership rule as the CalcView path: never write a value back into an
    // object it did not come from (session 27).
    if (optionFov != g_savedGameFovOwner) {
        BVR_LOG("[b1r] game fov stale-restore SKIPPED - object is %p, the saved %d came from %p",
                optionFov, g_savedGameFov, static_cast<const void*>(g_savedGameFovOwner));
        g_wasWritingGameFov = false;
        g_savedGameFovOwner = nullptr;
        return;
    }
    bool ok = write_option_fov(optionFov, g_savedGameFov);
    g_wasWritingGameFov = false;
    g_savedGameFovOwner = nullptr;
    BVR_LOG("[b1r] game fov write OFF (%s option %d - calcview silent %llu ms)",
            ok ? "restored" : "FAULTED restoring", g_savedGameFov,
            static_cast<unsigned long long>(GetTickCount64() - g_lastCalcViewMs));
}

void get_recenter_state(bvr::vr::HeadPose* pose, int32_t* yawUnits, float* worldScale) {
    if (pose) *pose = g_recenterPose;
    if (yawUnits) *yawUnits = g_recenterYawUnits;
    if (worldScale) *worldScale = g_worldScale.load(std::memory_order_relaxed);
}

bool driven_eye_cam(int eye, float loc[3], int32_t rot[3]) {
    if (eye < 0 || eye > 1) return false;
    if (GetTickCount64() - g_eyeCamStampMs[eye] > 200) return false; // stale/idle
    loc[0] = g_eyeCamLoc[eye].x;
    loc[1] = g_eyeCamLoc[eye].y;
    loc[2] = g_eyeCamLoc[eye].z;
    rot[0] = g_eyeCamRot[eye].pitch;
    rot[1] = g_eyeCamRot[eye].yaw;
    rot[2] = g_eyeCamRot[eye].roll;
    return true;
}

void set_recenter_state(const bvr::vr::HeadPose& pose, int32_t yawUnits, float worldScale) {
    g_recenterPose = pose;
    g_recenterYawUnits = yawUnits;
    g_worldScale.store(worldScale, std::memory_order_relaxed);
    g_haveRecenter = true;
    // A pending auto-recenter would re-reference the mapping onto the first
    // replayed head pose, throwing away the state just restored.
    g_recenterRequested.store(false, std::memory_order_relaxed);
    BVR_LOG("[b1r] recenter state SET (yaw %d units, worldScale %.1f) - vrrec play restore",
            yawUnits, worldScale);
}

void set_fov_override(float hfovDeg) {
    if (hfovDeg > 0.0f) {
        g_fovDeg.store(hfovDeg, std::memory_order_relaxed);
        g_fovOverride.store(true, std::memory_order_relaxed);
    } else {
        g_fovOverride.store(false, std::memory_order_relaxed);
    }
}

void draw_debug_ui() {
    if (!hook_live()) {
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f),
                           "CalcView: scan FAILED - running flat");
        return;
    }

    ImGui::Text("CalcView hook: LIVE @ %p", g_target);

    // Calls/sec sampled on the UI thread once per second.
    static uint64_t lastSample = 0;
    static uint32_t lastCount = 0;
    static uint32_t callsPerSec = 0;
    uint64_t now = GetTickCount64();
    uint32_t total = g_callCount.load(std::memory_order_relaxed);
    if (lastSample == 0) {
        lastSample = now;
        lastCount = total;
    } else if (now - lastSample >= 1000) {
        callsPerSec = static_cast<uint32_t>(
            (total - lastCount) * 1000ull / (now - lastSample));
        lastSample = now;
        lastCount = total;
    }

    int32_t pitch = g_lastPitch.load(std::memory_order_relaxed);
    int32_t yaw = g_lastYaw.load(std::memory_order_relaxed);
    int32_t roll = g_lastRoll.load(std::memory_order_relaxed);
    ImGui::Text("calls: %u total, %u/s", total, callsPerSec);
    ImGui::Text("pc: %p", g_playerController.load(std::memory_order_relaxed));
    ImGui::Text("loc: %.1f %.1f %.1f",
                g_lastLocX.load(std::memory_order_relaxed),
                g_lastLocY.load(std::memory_order_relaxed),
                g_lastLocZ.load(std::memory_order_relaxed));
    ImGui::Text("rot: %d %d %d (%.1f %.1f %.1f deg)", pitch, yaw, roll,
                pitch / kRotUnitsPerDegree, yaw / kRotUnitsPerDegree,
                roll / kRotUnitsPerDegree);
    ImGui::Text("fov: %.1f deg", g_lastFov.load(std::memory_order_relaxed));
    int32_t optFov = g_lastOptionFov.load(std::memory_order_relaxed);
    if (optFov > 0)
        ImGui::Text("option hfov: %d deg (UShockUserSettings, auto-claimed)", optFov);
    else
        ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.4f, 1.0f),
                           "option hfov: settings object not resolved");

    if (ImGui::CollapsingHeader("VR camera (M3/M4)", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Text(g_vrDriving.load(std::memory_order_relaxed)
                        ? "camera: driven by HMD pose"
                        : "camera: game (enable VR camera mode in the VR section)");
        ImGui::Text("head offset: (%.1f %.1f %.1f) UU",
                    g_headOffX.load(std::memory_order_relaxed),
                    g_headOffY.load(std::memory_order_relaxed),
                    g_headOffZ.load(std::memory_order_relaxed));
        if (ImGui::Button("VR PRESET 1 - everything on"))
            g_vrPresetPending.store(true, std::memory_order_relaxed);
        ImGui::SameLine();
        if (ImGui::Button("Save preset values"))
            g_vrPresetSavePending.store(true, std::memory_order_relaxed);
        bool xhair = g_crosshairVisible.load(std::memory_order_relaxed);
        if (ImGui::Checkbox("Flat-screen crosshair (default off in VR)", &xhair))
            g_crosshairVisible.store(xhair, std::memory_order_relaxed);
        bool lockoff = g_lockOnDisabled.load(std::memory_order_relaxed);
        if (ImGui::Checkbox("Lock-on disabled (pad aim magnetism off)", &lockoff))
            g_lockOnDisabled.store(lockoff, std::memory_order_relaxed);
        // ---- render resolution ------------------------------------------
        // This is the sharpness control, and it is the game's own resolution
        // because the eye render IS the backbuffer. It cannot be applied live:
        // the engine's SETRES faults (ENGINE_NOTES session 27), so the only
        // working lever is the config the engine reads at startup. Say so
        // plainly rather than letting a slider imply an instant effect.
        if (ImGui::CollapsingHeader("Render resolution (applies on next launch)")) {
            static bool s_read = false;
            static int s_w = 0, s_h = 0;
            unsigned liveW = 0, liveH = 0;
            bvr::vr::fov_audit(nullptr, nullptr, nullptr, &liveW, &liveH);
            game_ini::Viewport v = game_ini::read_viewport();
            if (!s_read && v.valid) {
                s_read = true;
                s_w = static_cast<int>(v.windowedW);
                s_h = static_cast<int>(v.windowedH);
            }
            if (!v.valid) {
                ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f),
                                   "Bioshock.ini not found - cannot set the resolution");
            } else {
                ImGui::Text("ini: %ux%u   live backbuffer: %ux%u", v.windowedW, v.windowedH,
                            liveW, liveH);
                if (liveW && liveH) {
                    // A headset eye is near square. A 16:9 buffer spends most of
                    // its width outside the lenses, which is the whole reason
                    // this control exists - quantify it instead of asserting it.
                    float aspect = static_cast<float>(liveW) / static_cast<float>(liveH);
                    ImGui::Text("aspect %.3f (1.000 is ideal; a headset eye is near square)",
                                aspect);
                    if (aspect > 1.25f || aspect < 0.8f)
                        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f),
                                           "far from square: much of this render falls "
                                           "outside the lenses");
                }
                // A dropdown of named modes with a Custom escape hatch - the
                // shape every game's video options uses, so it needs no
                // explaining. The list is square-first because a headset eye is
                // near square, with one 16:9 entry for playing flat. The top end
                // is deliberately generous: a user on a Dream Air runs 7680x4320
                // happily, so clamping low would be the bug, not the safety.
                struct Mode {
                    const char* name;
                    int w, h;
                };
                static const Mode kModes[] = {
                    {"1920 x 1080  (16:9, for flat play)", 1920, 1080},
                    {"2048 x 2048  (4.2 MPx, balanced)", 2048, 2048},
                    {"2560 x 2560  (6.6 MPx, sharper)", 2560, 2560},
                    {"3072 x 3072  (9.4 MPx, high-end GPU)", 3072, 3072},
                    {"4096 x 4096  (16.8 MPx, very demanding)", 4096, 4096},
                    {"Custom...", 0, 0},
                };
                const int kCustom = static_cast<int>(std::size(kModes)) - 1;

                // Preselect whatever the ini already says, so the dropdown opens
                // showing the truth rather than a default.
                static int s_sel = -1;
                if (s_sel < 0) {
                    s_sel = kCustom;
                    for (int i = 0; i < kCustom; ++i)
                        if (kModes[i].w == s_w && kModes[i].h == s_h) s_sel = i;
                }
                const char* preview = kModes[s_sel].name;
                if (ImGui::BeginCombo("Resolution", preview)) {
                    for (int i = 0; i < static_cast<int>(std::size(kModes)); ++i) {
                        if (ImGui::Selectable(kModes[i].name, s_sel == i)) {
                            s_sel = i;
                            if (i != kCustom) {
                                s_w = kModes[i].w;
                                s_h = kModes[i].h;
                            }
                        }
                    }
                    ImGui::EndCombo();
                }
                if (s_sel == kCustom) {
                    ImGui::InputInt("width", &s_w, 64, 256);
                    ImGui::InputInt("height", &s_h, 64, 256);
                    if (s_w < 1024) s_w = 1024;
                    if (s_h < 1024) s_h = 1024;
                    if (s_w > 8192) s_w = 8192;
                    if (s_h > 8192) s_h = 8192;
                }
                ImGui::Text("selected: %d x %d, %.1f MPx per eye", s_w, s_h,
                            static_cast<double>(s_w) * s_h / 1.0e6);
                if (ImGui::Button("Write to Bioshock.ini")) {
                    g_resWritePending.store((static_cast<uint64_t>(s_w) << 32) |
                                                static_cast<uint32_t>(s_h),
                                            std::memory_order_relaxed);
                }
                ImGui::SameLine();
                ImGui::TextDisabled("restart the game for it to take effect");
            }
        }
        atomic_slider("World scale (UU per m)", g_worldScale, 10.0f, 200.0f);
        atomic_slider("IPD (mm)", g_ipdMm, 55.0f, 75.0f);
        atomic_slider("Head offset up (UU)", g_headOffUpUu, -150.0f, 150.0f);
        atomic_slider("Head offset fwd (UU)", g_headOffFwdUu, -80.0f, 80.0f);
        if (ImGui::Button("Recenter (seated pose + view yaw)"))
            g_recenterRequested.store(true, std::memory_order_relaxed);
        bool forceFov = g_forceHeadsetFov.load(std::memory_order_relaxed);
        if (ImGui::Checkbox("Force headset FOV (off = game FOV, narrower)", &forceFov))
            g_forceHeadsetFov.store(forceFov, std::memory_order_relaxed);
    }

    if (ImGui::CollapsingHeader("Camera debug", ImGuiTreeNodeFlags_DefaultOpen)) {
        atomic_slider("Offset X (UU)", g_offsetX, -500.0f, 500.0f);
        atomic_slider("Offset Y (UU)", g_offsetY, -500.0f, 500.0f);
        atomic_slider("Offset Z (UU)", g_offsetZ, -500.0f, 500.0f);
        atomic_slider("Yaw offset (deg)", g_yawDeg, -180.0f, 180.0f);
        atomic_slider("Pitch offset (deg)", g_pitchDeg, -180.0f, 180.0f);
        atomic_slider("Roll offset (deg)", g_rollDeg, -180.0f, 180.0f);

        bool wobble = g_wobble.load(std::memory_order_relaxed);
        if (ImGui::Checkbox("Wobble test (1 Hz vertical)", &wobble))
            g_wobble.store(wobble, std::memory_order_relaxed);
        atomic_slider("Wobble amplitude (UU)", g_wobbleAmp, 0.0f, 50.0f);

        bool gfov = g_gameFovWrite.load(std::memory_order_relaxed);
        if (ImGui::Checkbox("Game FOV write (settings object, real control)", &gfov))
            g_gameFovWrite.store(gfov, std::memory_order_relaxed);
        atomic_slider("Game FOV (deg)", g_gameFovDeg, 75.0f, 150.0f);

        bool fov = g_fovOverride.load(std::memory_order_relaxed);
        if (ImGui::Checkbox("FOV override (PC+0xE0, dead field - diagnostics)", &fov))
            g_fovOverride.store(fov, std::memory_order_relaxed);
        atomic_slider("FOV (deg)", g_fovDeg, 40.0f, 140.0f);

        bool logCam = g_logCamera.load(std::memory_order_relaxed);
        if (ImGui::Checkbox("Log camera (1 Hz to file)", &logCam))
            g_logCamera.store(logCam, std::memory_order_relaxed);

        if (ImGui::Button("Reset offsets")) {
            g_offsetX.store(0.0f, std::memory_order_relaxed);
            g_offsetY.store(0.0f, std::memory_order_relaxed);
            g_offsetZ.store(0.0f, std::memory_order_relaxed);
            g_yawDeg.store(0.0f, std::memory_order_relaxed);
            g_pitchDeg.store(0.0f, std::memory_order_relaxed);
            g_rollDeg.store(0.0f, std::memory_order_relaxed);
            g_wobble.store(false, std::memory_order_relaxed);
        }
    }
}

} // namespace bvr::b1r::camera
