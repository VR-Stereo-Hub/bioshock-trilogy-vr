// Hook behavior (call the original, then adjust the writable out-params;
// publish state through atomics) follows
// itsloopyo/bioshock-remastered-headtracking (MIT), src/engine_hook.rs.

#include "game/bioshock1r/camera.h"

#include "core/debug/value_scan.h"
#include "core/gfx/frame_inspector.h"
#include "core/input/xinput_bridge.h"
#include "core/util/log.h"
#include "game/bioshock1r/aim.h"
#include "game/bioshock1r/body.h"
#include "game/bioshock1r/bones.h"
#include "game/bioshock1r/console_exec.h"
#include "core/vr/openxr_runtime.h"
#include "game/bioshock1r/hands.h"
#include "game/bioshock1r/input_drive.h"
#include "game/bioshock1r/patterns.h"
#include "game/bioshock1r/scenedraw.h"
#include "game/bioshock1r/ue_math.h"

#include <windows.h>
#include <MinHook.h>

#include <imgui.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
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

float* fov_ptr(void* pc) {
    return reinterpret_cast<float*>(static_cast<uint8_t*>(pc) + patterns::kFovLiveOffset);
}

// Half-IPD shift along view-right of `rot`, sign -1 = left eye. Same math the
// AER path uses, shared by both SequentialReentry passes.
void apply_eye_offset(FVector* loc, const FRotator& rot, int sign) {
    float yawRad = static_cast<float>(rot.yaw) / kRotUnitsPerRadian;
    float halfIpdUu = static_cast<float>(sign) *
                      (g_ipdMm.load(std::memory_order_relaxed) / 2000.0f) *
                      g_worldScale.load(std::memory_order_relaxed);
    loc->x += -sinf(yawRad) * halfIpdUu;
    loc->y += cosf(yawRad) * halfIpdUu;
}

// Automated-test seam: %LOCALAPPDATA%\BioshockVR\command.txt is polled at 1 Hz
// on the game thread; when its write time changes, every line is applied and
// logged. Lets a test harness drive the debug controls without the overlay.
// Camera commands: "fov <deg>", "fov off", "gfov <deg>", "gfov off",
// "offset <x> <y> <z>", "camrot <pitch> <yaw> <roll>" (render-camera-only
// rotation offset in degrees - the flat stand-in for HMD head-look),
// "recenter".
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
        bool on = strncmp(args, "off", 3) != 0;
        g_fgFovMatch.store(on, std::memory_order_relaxed);
        BVR_LOG("[b1r] fg lens match %s (last written %.1f)", on ? "ON" : "off",
                g_fgFovWritten.load(std::memory_order_relaxed));
    } else if (strcmp(cmd, "fginfo") == 0) {
        BVR_LOG("[b1r] fginfo: pc=%p viewActor=%p (fsweep them for the fg fov property)",
                g_playerController.load(std::memory_order_relaxed),
                g_lastViewActor.load(std::memory_order_relaxed));
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
        bvr::frame_inspector::arm(strncmp(args, "full", 4) == 0 ? 2 : 1);
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
    fprintf(f, "bodyRate=%.2f\n", body::rate_per_sec());
    fprintf(f, "bodyDeadzoneDeg=%.1f\n", body::deadzone_deg());
    fclose(f);
    BVR_LOG("[b1r] VR preset values saved to vrpreset.ini");
    // The per-hand model offsets live in hands.ini; saving them here too makes
    // the one in-headset save button cover every tuned slider.
    hands::save_offsets();
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
    float bodyRate = body::rate_per_sec(), bodyDz = body::deadzone_deg();
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
        else if (strcmp(key, "bodyRate") == 0) bodyRate = v;
        else if (strcmp(key, "bodyDeadzoneDeg") == 0) bodyDz = v;
        else --n;
    }
    fclose(f);
    aim::set_trim(0, lp, ly);
    aim::set_trim(1, rp, ry);
    body::set_tuning(bodyRate, bodyDz);
    if (n) BVR_LOG("[b1r] VR preset: %d value(s) loaded from vrpreset.ini", n);
}

void apply_vr_preset() {
    BVR_LOG("[b1r] VR PRESET 1: arming the full VR configuration");
    bvr::vr::set_enabled(true);        // paces the game to the headset
    bvr::vr::set_camera_mode(true);    // 6DOF head drive
    bvr::vr::set_sr_pair_pacing(true); // one waitFrame per eye pair
    input::handle_command("on");       // motion controllers as gamepad
    g_gameFovWrite.store(true, std::memory_order_relaxed);
    aim::handle_command("on");         // controller aim (R weapon / L plasmid)
    aim::handle_command("pose aim");   // the runtime AIM pose
    aim::handle_command("origin on");  // ray starts at the hand
    aim::handle_command("laser on");
    hands::handle_command("on");       // viewmodel follows the controller
    hands::handle_command("pose aim"); // align to the AIM ray
    body::handle_command("on");        // M7.5: stick-forward = look direction
    load_vr_preset_values();           // tuned sliders (ini) over defaults
    scenedraw::handle_command("vrstereo on"); // last: 1t + stereo, sticky
    BVR_LOG("[b1r] VR PRESET 1 armed (unwind: vrstereo off + overlay checkboxes)");
}

void poll_command_file(uint64_t now) {
    if (now - g_lastCmdPollMs < 1000) return;
    g_lastCmdPollMs = now;
    static wchar_t path[MAX_PATH];
    if (!path[0]) {
        wchar_t base[MAX_PATH];
        if (!GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH)) return;
        swprintf_s(path, L"%s\\BioshockVR\\command.txt", base);
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
        if (scenedraw::stereo_active() && g_srBaseValid && loc && rot) {
            *loc = g_srBaseLoc;
            *rot = g_srBaseRot;
            apply_eye_offset(loc, *rot, +1);
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
    poll_command_file(now);
    // Overlay preset buttons land here (game thread; the overlay draws on
    // the render thread and only sets the pending flags).
    if (g_vrPresetPending.exchange(false, std::memory_order_relaxed)) apply_vr_preset();
    if (g_vrPresetSavePending.exchange(false, std::memory_order_relaxed)) save_vr_preset();
    // M5: pump the engine's own pad pipeline against the synthetic gamepad
    // (self-throttles to once per present; no-op while vrinput is off).
    input_drive::on_frame(now);
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
    bvr::vr::HeadPose hp{};
    bool simHead = now < g_simHead.deadline;
    if (simHead) {
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
    }
    if (loc && rot &&
        (simHead || (bvr::vr::vr_camera_mode() && bvr::vr::get_head_pose(hp)))) {
        UeAngles a = hmd_angles(hp);
        if (g_recenterRequested.exchange(false, std::memory_order_relaxed) || !g_haveRecenter) {
            g_recenterPose = hp;
            g_recenterYawUnits = static_cast<int32_t>(lroundf(a.yawRad * kRotUnitsPerRadian));
            g_haveRecenter = true;
            body::on_reset("recentered");
            BVR_LOG("[b1r] vr camera recentered (yaw %.1f deg)", a.yawRad * 57.29578f);
        }

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
        if (eyeSign != 0) {
            float finalYawRad = static_cast<float>(rot->yaw) / kRotUnitsPerRadian;
            float halfIpdUu =
                static_cast<float>(eyeSign) *
                (g_ipdMm.load(std::memory_order_relaxed) / 2000.0f) * scale;
            loc->x += -sinf(finalYawRad) * halfIpdUu;
            loc->y += cosf(finalYawRad) * halfIpdUu;
        }

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
    }

    // Game-FOV write via the settings object (the renderer's real per-frame
    // source). Precedence: VR forced headset fov > manual gfov. One-shot
    // save/restore so the user's option value returns untouched.
    if (optionFov) {
        bool wantVr = vrDrove && vrFov > 0.0f;
        bool wantManual = g_gameFovWrite.load(std::memory_order_relaxed);
        if (wantVr || wantManual) {
            if (!g_wasWritingGameFov) {
                g_savedGameFov = *optionFov;
                g_wasWritingGameFov = true;
                BVR_LOG("[b1r] game fov write ON (saved option %d)", g_savedGameFov);
            }
            float want = wantVr ? vrFov : g_gameFovDeg.load(std::memory_order_relaxed);
            int32_t wantInt = static_cast<int32_t>(want + 0.5f);
            if (*optionFov != wantInt) *optionFov = wantInt;
        } else if (g_wasWritingGameFov) {
            *optionFov = g_savedGameFov;
            g_wasWritingGameFov = false;
            BVR_LOG("[b1r] game fov write OFF (restored option %d)", g_savedGameFov);
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
                float fg = 2.0f * atanf(tanW * 0.75f) * kRadToDeg;
                *fgFov = fg;
                g_fgFovWritten.store(fg, std::memory_order_relaxed);
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
        apply_eye_offset(loc, *rot, -1);
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
