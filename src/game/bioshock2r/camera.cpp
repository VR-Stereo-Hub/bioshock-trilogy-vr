// Drive behavior (call the original, then adjust the writable out-values;
// publish state through atomics) follows
// itsloopyo/bioshock-remastered-headtracking (MIT), src/engine_hook.rs,
// via the bioshock1r camera module this file is the M3 subset of. The seam
// itself differs from BS1: ProcessEvent filtered to the PlayerCalcView
// UFunction (see patterns.h - BS2 inlined the event dispatch, the thunk is
// dead code).

#include "game/bioshock2r/camera.h"

#include "core/debug/value_scan.h"
#include "core/gfx/frame_inspector.h"
#include "core/gfx/hud_capture.h"
#include "core/input/xinput_bridge.h"
#include "core/ui/overlay.h"
#include "core/util/log.h"
#include "core/vr/openxr_runtime.h"
#include "game/shared/ue_math.h"

#include <windows.h>
#include <MinHook.h>

#include <imgui.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <share.h>

namespace bvr::b2r::camera {
namespace {

// FVector/FRotator, the rotation-unit constants and the XR->UE conversion all
// live in game/shared/ue_math.h - the engine conventions are identical across
// the two remasters (same build session).

// The inlined dispatch sites build exactly this block and read it back after
// ProcessEvent returns (offline disasm, patterns.h) - so mutating loc/rot
// here AFTER calling the original is equivalent to BS1's out-param writes.
struct CalcViewParams {
    void* viewActor;
    FVector loc;
    FRotator rot;
};
static_assert(sizeof(CalcViewParams) == 0x1C, "param block is 0x1C bytes on this engine");

// Controls: overlay thread writes, game thread reads. All relaxed - x86
// lock-free, and a field arriving one frame late is fine for debug sliders.
std::atomic<float> g_offsetX{0.0f}, g_offsetY{0.0f}, g_offsetZ{0.0f};
// Heartbeat on by default during bring-up - 1 line/s proves per-frame firing
// in every session log. Toggle off with `camlog off` / the overlay.
std::atomic<bool> g_logCamera{true};

// FOV (session 25). The readback claims whatever the game renders (honest
// projection = no fisheye/world-drag); both WRITE levers ship DEFAULT OFF
// per the every-lever-off rule. `vrfov` asks for the headset-suggested hfov
// in strict gameplay while the HMD drives; `gfov` is the manual test lever.
std::atomic<int32_t> g_lastOptionFov{0}; // telemetry: 0 = object not located
std::atomic<bool> g_forceHeadsetFov{false};
std::atomic<bool> g_gameFovWrite{false};
std::atomic<float> g_gameFovDeg{100.0f};

// M3 VR camera drive. worldScale default follows BS1's in-headset calibration
// (100 UU/m, session 16) as the starting point - BS2 gets its own verdict
// from the user before anything is persisted.
std::atomic<float> g_worldScale{100.0f}; // Unreal units per meter
std::atomic<float> g_headOffUpUu{0.0f};
std::atomic<float> g_headOffFwdUu{0.0f};
std::atomic<bool> g_recenterRequested{true}; // auto-recenter on first drive
std::atomic<bool> g_vrDriving{false};        // telemetry for the UI
// Head-offset telemetry: the recenter-relative offset applied to loc this
// frame, in UU - makes the world-scale value's effect a number in the log,
// which is what the flat 6DOF check measures.
std::atomic<float> g_headOffX{0.0f}, g_headOffY{0.0f}, g_headOffZ{0.0f};

// Telemetry: game thread writes, overlay thread reads.
std::atomic<uint32_t> g_callCount{0};
std::atomic<float> g_lastLocX{0.0f}, g_lastLocY{0.0f}, g_lastLocZ{0.0f};
std::atomic<int32_t> g_lastPitch{0}, g_lastYaw{0}, g_lastRoll{0};
// Display only - the overlay never dereferences these.
std::atomic<void*> g_playerController{nullptr};
std::atomic<void*> g_lastViewActor{nullptr};
std::atomic<uint32_t> g_lastVtblRva{0}; // observed view-actor vtable RVA
std::atomic<void*> g_calcViewFn{nullptr}; // learned PlayerCalcView UFunction*

// ProcessEvent(UFunction*, void* parms, void* result): __thiscall, ret 0xC.
// __fastcall with a dummy EDX slot is register/stack/cleanup-identical.
using ProcessEventFn = void(__fastcall*)(void* self, void* edx, void* fn, void* parms,
                                         void* result);
// FindFunctionChecked(FName{index,number}, UBOOL global): __thiscall, ret 0xC.
using FindFuncFn = void*(__fastcall*)(void* self, void* edx, uint32_t nameIndex,
                                      uint32_t nameNumber, uint32_t global);
ProcessEventFn g_originalPE = nullptr;
FindFuncFn g_originalFF = nullptr;
void* g_peTarget = nullptr;
const uint8_t* g_fnameIndexGlobal = nullptr;
std::atomic<bool> g_hookLive{false};
std::atomic<bool> g_loggedFirstFire{false};

// Game-thread-only bookkeeping (never touched by the overlay).
uintptr_t g_imageBase = 0;
size_t g_imageSize = 0;
uint64_t g_lastHeartbeatMs = 0;
uint32_t g_heartbeatBaseCount = 0;
// FOV write latch (game thread only): one-shot save of the user's option on
// the ON edge, restored on the OFF edge - the option ini value is the user's
// property and must survive every path out of gameplay.
bool g_wasWritingGameFov = false;
int32_t g_savedGameFov = 0;
uint64_t g_lastCalcViewMs = 0;
bool g_haveRecenter = false;
bvr::vr::HeadPose g_recenterPose{};
// The seated frame's yaw zero, in ROTATOR UNITS (65536/turn), integer for the
// same exactness reasons as BS1 (see bioshock1r/camera.cpp; the M7.5 body
// transfer that motivated it will want the same invariant here eventually).
int32_t g_recenterYawUnits = 0;
float recenter_yaw_rad() { return g_recenterYawUnits / kRotUnitsPerRadian; }

// Synthetic HMD lane, extended over BS1's: `simhead <yaw> <pitch> <roll>
// [px py pz] [holdMs]` feeds a scripted head pose - ROTATION AND POSITION -
// through the real drive, so the full 6DOF xr-to-ue mapping is provable flat
// from the log (BS1's lane was rotation-only). Self-expiring.
struct SimHead {
    float yawDeg = 0.0f, pitchDeg = 0.0f, rollDeg = 0.0f;
    float px = 0.0f, py = 0.0f, pz = 0.0f; // meters, XR local space
    uint64_t deadline = 0;
};
SimHead g_simHead;

uint64_t g_lastCmdPollMs = 0;
FILETIME g_lastCmdWrite{};

// --- gameplay-view predicate -------------------------------------------------
// Strict form (BS1's body.cpp predicate): the view actor's vtable must be
// AShockPlayer's. Deliberately no `viewActor == pc` escape hatch (that hatch
// exists in BS1 only for the aim ray, which does not exist here yet).

bool read_ptr(void* addr, void** out) {
    if (!bvr::pattern_scan::is_memory_valid(addr, sizeof(void*))) return false;
    *out = *static_cast<void**>(addr);
    return true;
}

// Observed vtable RVA of the view actor, 0 if unreadable/foreign. This is the
// runtime verdict on the offline RTTI candidates: logged on every change, so
// a wrong candidate RVA names its own correction from any session log.
uint32_t observed_vtable_rva(void* viewActor) {
    void* vtbl = nullptr;
    if (!viewActor || !read_ptr(viewActor, &vtbl)) return 0;
    uintptr_t v = reinterpret_cast<uintptr_t>(vtbl);
    if (!g_imageBase || v < g_imageBase || v >= g_imageBase + g_imageSize) return 0;
    return static_cast<uint32_t>(v - g_imageBase);
}

bool is_gameplay_view_rva(uint32_t vtblRva) {
    return vtblRva == patterns::kShockPlayerVtableRva;
}

// --- command seam ------------------------------------------------------------
// <data_dir>\command.txt (= %LOCALAPPDATA%\BioshockVR\bs2\command.txt) polled
// at 1 Hz on the game thread, same contract as BS1's seam - but driven from
// the ProcessEvent detour, so it works at the main menu too (BS2's menu never
// runs PlayerCalcView, unlike BS1's attract scene). M3 vocabulary:
//   recenter                     re-reference the seated pose
//   offset <x> <y> <z>           debug camera offset in UU (0 0 0 clears)
//   worldscale <v>               UU per meter
//   headoff <up> <fwd>           head-anchor offset in UU
//   simhead <yaw> <pitch> <roll> [px py pz] [holdMs] | simhead off
//   vrcam on|off                 VR enable + camera mode (core funnels)
//   camlog on|off                1 Hz heartbeat
//   vroverlay on|off             core overlay visibility (bring-up A/B)
//   vrcine <args>                core cinematic-fallback A/B (vrcine status..)
// FOV (session 25; both write levers DEFAULT OFF):
//   vrfov on|off|status          forced headset FOV write (strict gameplay +
//                                HMD driving only; save/restore of the option)
//   gfov <deg>|off               manual game FOV write (flat test lever)
//   fovaudit                     option vs submitted claim vs rendered fov
// Discovery commands (route to core/debug/value_scan; game thread only),
// ported from BS1's dispatcher for the session-25 FOV derivation - the
// duplicate-now seam policy applies (see the ARCHITECTURE decision log):
//   memscan <f>  memrescan <f>  memlist [n]  memread <idx>
//   memscani <u>  memrescani <u>   (integer-typed variants)
//   mempoke <idx> <f>  mempoke <lo>-<hi> <f>  mempokei ...same with <u>
//   memrestore  memptr <idx> [maxDeltaHex]
//   pokeaddr <hex> <f>  pokeaddri <hex> <u>  hexdump <hex> <len>
//   fsweep <hexaddr> <len> <lo> <hi>  strscan <text>  membases
//   dumpframe [full] [n]
//   vtscan <hexRva> [needBytesHex]  (b2r-first: one-shot heap scan for live
//   objects whose dword0 == base + RVA - the candidate-vtable verifier)

void apply_command(const char* cmd, const char* args) {
    float v = 0.0f, x = 0.0f, y = 0.0f, z = 0.0f;
    unsigned lo = 0, hi = 0, n = 0;
    unsigned addr = 0, len = 0;

    if (strcmp(cmd, "recenter") == 0) {
        g_recenterRequested.store(true, std::memory_order_relaxed);
        BVR_LOG("[b2r] command: recenter");
    } else if (strcmp(cmd, "offset") == 0) {
        if (sscanf_s(args, "%f %f %f", &x, &y, &z) == 3) {
            g_offsetX.store(x, std::memory_order_relaxed);
            g_offsetY.store(y, std::memory_order_relaxed);
            g_offsetZ.store(z, std::memory_order_relaxed);
            BVR_LOG("[b2r] command: offset %.1f %.1f %.1f", x, y, z);
        }
    } else if (strcmp(cmd, "worldscale") == 0) {
        if (sscanf_s(args, "%f", &x) == 1 && x > 0.0f) {
            g_worldScale.store(x, std::memory_order_relaxed);
            BVR_LOG("[b2r] command: worldscale %.1f", x);
        }
    } else if (strcmp(cmd, "headoff") == 0) {
        if (sscanf_s(args, "%f %f", &x, &y) == 2) {
            g_headOffUpUu.store(x, std::memory_order_relaxed);
            g_headOffFwdUu.store(y, std::memory_order_relaxed);
            BVR_LOG("[b2r] command: headoff up %.1f fwd %.1f", x, y);
        }
    } else if (strcmp(cmd, "simhead") == 0) {
        if (strncmp(args, "off", 3) == 0) {
            g_simHead.deadline = 0;
            BVR_LOG("[b2r] command: simhead off");
        } else {
            // 3 args = angles; 4 = angles + holdMs (BS1-compatible); 6 =
            // angles + position; 7 = angles + position + holdMs.
            float v[7] = {};
            int n = sscanf_s(args, "%f %f %f %f %f %f %f", &v[0], &v[1], &v[2], &v[3],
                             &v[4], &v[5], &v[6]);
            if (n == 3 || n == 4 || n == 6 || n == 7) {
                bool wasIdle = GetTickCount64() >= g_simHead.deadline;
                g_simHead.yawDeg = v[0];
                g_simHead.pitchDeg = v[1];
                g_simHead.rollDeg = v[2];
                g_simHead.px = n >= 6 ? v[3] : 0.0f;
                g_simHead.py = n >= 6 ? v[4] : 0.0f;
                g_simHead.pz = n >= 6 ? v[5] : 0.0f;
                int hold = n == 4   ? static_cast<int>(v[3])
                           : n == 7 ? static_cast<int>(v[6])
                                    : 0;
                if (hold <= 0) hold = 120000;
                g_simHead.deadline = GetTickCount64() + static_cast<uint64_t>(hold);
                if (wasIdle) g_recenterRequested.store(true, std::memory_order_relaxed);
                BVR_LOG("[b2r] command: simhead yaw %.1f pitch %.1f roll %.1f "
                        "pos (%.2f %.2f %.2f) for %d ms%s",
                        v[0], v[1], v[2], g_simHead.px, g_simHead.py, g_simHead.pz, hold,
                        wasIdle ? " (recentering onto first sim pose)" : "");
            } else {
                BVR_LOG("[b2r] usage: simhead <yaw> <pitch> <roll> [px py pz] [holdMs] | "
                        "simhead off");
            }
        }
    } else if (strcmp(cmd, "vrcam") == 0) {
        bool on = strncmp(args, "off", 3) != 0;
        if (on) bvr::vr::set_enabled(true);
        bvr::vr::set_camera_mode(on);
        BVR_LOG("[b2r] command: vrcam %s", on ? "on (VR enabled + camera mode)" : "off");
    } else if (strcmp(cmd, "camlog") == 0) {
        g_logCamera.store(strncmp(args, "off", 3) != 0, std::memory_order_relaxed);
        BVR_LOG("[b2r] command: camlog %s", strncmp(args, "off", 3) != 0 ? "on" : "off");
    } else if (strcmp(cmd, "vroverlay") == 0) {
        bvr::overlay::set_visible(strncmp(args, "off", 3) != 0);
        BVR_LOG("[b2r] command: vroverlay %s", strncmp(args, "off", 3) != 0 ? "on" : "off");
    } else if (strcmp(cmd, "vrcine") == 0) {
        bvr::vr::handle_cine_command(args); // core detector A/B on a new game
    } else if (strcmp(cmd, "vrfov") == 0) {
        // Forced headset FOV, DEFAULT OFF: in strict gameplay while the HMD
        // drives, write the headset-suggested hfov into the game option so
        // the image fills the headset. Off restores on the next CalcView.
        if (strncmp(args, "status", 6) == 0) {
            BVR_LOG("[b2r] vrfov status: force=%s suggested=%.1f option=%d "
                    "writing=%d savedOption=%d",
                    g_forceHeadsetFov.load(std::memory_order_relaxed) ? "on" : "off",
                    bvr::vr::suggested_hfov_deg(),
                    g_lastOptionFov.load(std::memory_order_relaxed),
                    g_wasWritingGameFov ? 1 : 0, g_savedGameFov);
        } else {
            bool on = strncmp(args, "off", 3) != 0;
            g_forceHeadsetFov.store(on, std::memory_order_relaxed);
            BVR_LOG("[b2r] command: vrfov %s (suggested headset hfov %.1f deg; engages "
                    "in strict gameplay while the HMD drives)",
                    on ? "on" : "off", bvr::vr::suggested_hfov_deg());
        }
    } else if (strcmp(cmd, "gfov") == 0) {
        // Manual game-option FOV write (flat test lever + clamp probe).
        if (strncmp(args, "off", 3) == 0) {
            g_gameFovWrite.store(false, std::memory_order_relaxed);
            BVR_LOG("[b2r] command: gfov off");
        } else if (sscanf_s(args, "%f", &v) == 1 && v > 0.0f) {
            g_gameFovDeg.store(v, std::memory_order_relaxed);
            g_gameFovWrite.store(true, std::memory_order_relaxed);
            BVR_LOG("[b2r] command: gfov %.1f (writes in strict gameplay)", v);
        } else {
            BVR_LOG("[b2r] usage: gfov <deg> | gfov off");
        }
    } else if (strcmp(cmd, "fovaudit") == 0) {
        // The three fov truths side by side (BS1 session-21 instrument, minus
        // the pose/eyes stereo sub-forms BS2 does not have yet): the engine
        // option, what the runtime last tagged the projection layer with, and
        // the option-derived expectation at the swap aspect. The RENDERED
        // side comes from the core fov watch when it decodes, else from
        // `dumpframe full 2` + tools/decode-framedump.ps1.
        int32_t* opt = patterns::hfov_option_ptr();
        float tanH = 0.0f, tanV = 0.0f;
        int src = -1;
        unsigned sw = 0, sh = 0;
        bvr::vr::fov_audit(&tanH, &tanV, &src, &sw, &sh);
        // Option-derived expectation. Flat there is no session (swap dims
        // 0x0) - assume the 16:9 render aspect.
        float optTanH = 0.0f, optTanV = 0.0f;
        if (opt) {
            optTanH = tanf(static_cast<float>(*opt) * 0.5f / kRadToDeg);
            optTanV = optTanH * ((sw && sh) ? (static_cast<float>(sh) / static_cast<float>(sw))
                                            : (9.0f / 16.0f));
        }
        BVR_LOG("[b2r] fovaudit: option=%d gfovWrite=%s(%.1f) vrfov=%s | submitted "
                "tanH=%.6f tanV=%.6f src=%s swap=%ux%u | option-derived tanH=%.6f "
                "tanV=%.6f",
                opt ? *opt : -1,
                g_gameFovWrite.load(std::memory_order_relaxed) ? "on" : "off",
                g_gameFovDeg.load(std::memory_order_relaxed),
                g_forceHeadsetFov.load(std::memory_order_relaxed) ? "on" : "off", tanH,
                tanV,
                src == 0   ? "readback"
                : src == 1 ? "fallback"
                : src == 2 ? "manual"
                : src == 3 ? "live"
                           : "none",
                sw, sh, optTanH, optTanV);
        float liveTanH = 0.0f, liveTanV = 0.0f;
        unsigned long long liveAge = 0;
        if (bvr::hud::fov_watch(&liveTanH, &liveTanV, &liveAge))
            BVR_LOG("[b2r] fovaudit live: rendered tanH=%.4f tanV=%.4f (%.1f deg) "
                    "age=%llums | mismatch=%d cineActive=%d",
                    liveTanH, liveTanV, 2.0f * atanf(liveTanH) * kRadToDeg, liveAge,
                    bvr::hud::fov_mismatch() ? 1 : 0,
                    bvr::vr::cinematic_active() ? 1 : 0);
        else
            BVR_LOG("[b2r] fovaudit live: no decoded scene tangents yet");
    } else if (strcmp(cmd, "fsweep") == 0) {
        float flo = 0.0f, fhi = 0.0f;
        if (sscanf_s(args, "%x %u %f %f", &addr, &len, &flo, &fhi) == 4)
            bvr::value_scan::float_sweep(addr, len, flo, fhi);
        else
            BVR_LOG("[b2r] usage: fsweep <hexaddr> <len> <lo> <hi>");
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
        // (files suffixed _qN). Same core frame inspector as BS1; the dump
        // lands in this game's data dir via log::data_dir().
        bool full = strncmp(args, "full", 4) == 0;
        int count = 1;
        sscanf_s(full ? args + 4 : args, " %d", &count);
        bvr::frame_inspector::arm(full ? 2 : 1, count);
    } else if (strcmp(cmd, "vtscan") == 0) {
        // vtscan <hexRva> [needBytesHex] - one-shot candidate-vtable verifier:
        // logs every live object whose dword0 == base + RVA. The accept
        // callback never chooses, so the census covers ALL matches; the
        // summary's chosen=00000000 is expected. EXPENSIVE (full 4 GB walk) -
        // probe use only, never wire onto a cadence.
        unsigned needBytes = 0x100;
        if (sscanf_s(args, "%x %x", &addr, &needBytes) >= 1) {
            uint32_t rva = addr;
            patterns::scan_for_vtable_object(
                rva, needBytes,
                [](void* obj, void* user) -> bool {
                    BVR_LOG("[b2r] vtscan 0x%X match @ %p",
                            *static_cast<const uint32_t*>(user), obj);
                    return false;
                },
                &rva, "vtscan", nullptr);
        } else {
            BVR_LOG("[b2r] usage: vtscan <hexRva> [needBytesHex]");
        }
    } else {
        BVR_LOG("[b2r] unknown command: %s (see the vocabulary comment in camera.cpp; "
                "BS1-only levers like vrstereo/vraim/reentry/exec are not ported yet)",
                cmd);
    }
}

void poll_command_file(uint64_t now) {
    if (now - g_lastCmdPollMs < 1000) return;
    g_lastCmdPollMs = now;
    static wchar_t path[MAX_PATH];
    if (!path[0]) {
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

// --- the camera tail ---------------------------------------------------------
// Runs after the original ProcessEvent returned for a PlayerCalcView dispatch:
// the script has produced the camera, the inlined caller has not yet read the
// block back. Everything here is the BS1 CalcView detour body reshaped onto
// the param block.
void calcview_tail(void* self, CalcViewParams* p) {
    g_playerController.store(self, std::memory_order_relaxed);
    uint32_t count = g_callCount.fetch_add(1, std::memory_order_relaxed) + 1;

    FVector* loc = &p->loc;
    FRotator* rot = &p->rot;
    g_lastLocX.store(loc->x, std::memory_order_relaxed);
    g_lastLocY.store(loc->y, std::memory_order_relaxed);
    g_lastLocZ.store(loc->z, std::memory_order_relaxed);
    g_lastPitch.store(rot->pitch, std::memory_order_relaxed);
    g_lastYaw.store(rot->yaw, std::memory_order_relaxed);
    g_lastRoll.store(rot->roll, std::memory_order_relaxed);

    if (!g_loggedFirstFire.exchange(true)) {
        BVR_LOG("[b2r] calcview first fire: pc=%p viewactor=%p loc=(%.1f %.1f %.1f) "
                "rot=(%d %d %d)",
                self, p->viewActor, loc->x, loc->y, loc->z, rot->pitch, rot->yaw, rot->roll);
    }

    uint64_t now = GetTickCount64();
    g_lastCalcViewMs = now;

    // FOV readback (session 25): claim what the game actually renders, every
    // call, menus included - BS1 shape. Null option object -> claim 0, which
    // core treats exactly like "no readback yet" (falls back to the headset
    // target, src=fallback), so nothing regresses before the first scan
    // lands. While the write block below holds the option, the readback
    // echoes the written value - correct, the renderer really renders it.
    int32_t* optionFov = patterns::hfov_option_ptr();
    g_lastOptionFov.store(optionFov ? *optionFov : 0, std::memory_order_relaxed);
    bvr::vr::set_rendered_hfov(optionFov ? static_cast<float>(*optionFov) : 0.0f);

    // Gameplay verdict + candidate-RVA self-diagnosis. Published every call:
    // core's cinematic fallback keys on this verdict AND its staleness, so a
    // silent adapter would park the headset on the quad screen permanently.
    void* va = p->viewActor;
    g_lastViewActor.store(va, std::memory_order_relaxed);
    uint32_t vtblRva = observed_vtable_rva(va);
    g_lastVtblRva.store(vtblRva, std::memory_order_relaxed);
    bool strictGameplay = is_gameplay_view_rva(vtblRva);
    bvr::vr::publish_gameplay_view(strictGameplay);

    {
        static uint32_t s_lastLoggedRva = 0xFFFFFFFFu; // game thread only
        if (vtblRva != s_lastLoggedRva) {
            s_lastLoggedRva = vtblRva;
            const char* verdict =
                vtblRva == patterns::kShockPlayerVtableRva ? "AShockPlayer candidate VERIFIED"
                : vtblRva == patterns::kShockPlayerControllerVtableRva
                    ? "AShockPlayerController candidate VERIFIED (menu shape)"
                    : "unrecognized - candidates may be wrong";
            BVR_LOG("[b2r] view actor vtable RVA 0x%X - %s (candidates: player 0x%X, pc 0x%X)",
                    vtblRva, verdict, patterns::kShockPlayerVtableRva,
                    patterns::kShockPlayerControllerVtableRva);
        }
    }

    // Transition line - same phrase as BS1 so the harness's generic "save is
    // loaded" detector transfers to this game unchanged.
    {
        static int s_lastViewState = -1; // game thread only
        int viewState = strictGameplay ? 1 : 0;
        if (viewState != s_lastViewState) {
            s_lastViewState = viewState;
            BVR_LOG("[b2r] view state: %s",
                    strictGameplay ? "GAMEPLAY (ShockPlayer view)" : "menu/cutscene");
            // A view-state change is the cheap signal that the object world
            // changed under us - the moment to retry a dormant settings scan.
            patterns::hfov_scan_rearm("view state change");
        }
    }

    // M3: drive the camera from the HMD pose. Pitch/roll are absolute (head
    // owns them); yaw is additive on the game's yaw so stick/mouse turning
    // still works; position adds the recenter-relative head offset, rotated
    // into the game yaw frame and scaled UU-per-meter. Identical math to
    // BS1's shipped drive (bioshock1r/camera.cpp) - a deliberate duplication,
    // recorded as a seam leak for a later game/shared unification.
    bool vrDrove = false;
    bvr::vr::HeadPose hp{};
    bool driveHead = false;
    if (now < g_simHead.deadline) {
        float q[4];
        xr_local_trim_quat(g_simHead.pitchDeg / kRadToDeg, g_simHead.yawDeg / kRadToDeg,
                           g_simHead.rollDeg / kRadToDeg, q);
        hp = {};
        hp.px = g_simHead.px;
        hp.py = g_simHead.py;
        hp.pz = g_simHead.pz;
        hp.qx = q[0];
        hp.qy = q[1];
        hp.qz = q[2];
        hp.qw = q[3];
        driveHead = true; // sim lane stays ungated for flat tests
    } else if (strictGameplay && !bvr::vr::cinematic_active() &&
               !bvr::hud::letterbox(nullptr, nullptr) && bvr::vr::vr_camera_mode() &&
               bvr::vr::get_head_pose(hp)) {
        // Live lane gated exactly like BS1: the HMD must not steer scripted
        // or menu cameras (their content lands on the quad screen).
        driveHead = true;
    }
    if (driveHead) {
        UeAngles a = ue_angles_from_xr_quat(hp.qx, hp.qy, hp.qz, hp.qw);
        if (g_recenterRequested.exchange(false, std::memory_order_relaxed) || !g_haveRecenter) {
            g_recenterPose = hp;
            g_recenterYawUnits = static_cast<int32_t>(lroundf(a.yawRad * kRotUnitsPerRadian));
            g_haveRecenter = true;
            BVR_LOG("[b2r] vr camera recentered (yaw %.1f deg)", a.yawRad * kRadToDeg);
        }

        // Integer all the way through (see BS1's invariant note): the
        // head-look residual is the ONLY thing added to the game's own yaw.
        int32_t gameYawUnits = rot->yaw;
        int32_t headYawUnits = static_cast<int32_t>(lroundf(a.yawRad * kRotUnitsPerRadian));
        int32_t residualUnits = wrap_rot(headYawUnits - g_recenterYawUnits);
        float gameYawRad = static_cast<float>(gameYawUnits) / kRotUnitsPerRadian;
        rot->pitch = static_cast<int32_t>(a.pitchRad * kRotUnitsPerRadian);
        rot->roll = static_cast<int32_t>(a.rollRad * kRotUnitsPerRadian);
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

        // User head-anchor offset. Vertical is world-up; forward rides the
        // final view yaw, horizontal only.
        float hoUp = g_headOffUpUu.load(std::memory_order_relaxed);
        float hoFwd = g_headOffFwdUu.load(std::memory_order_relaxed);
        if (hoUp != 0.0f || hoFwd != 0.0f) {
            float vyaw = static_cast<float>(rot->yaw) / kRotUnitsPerRadian;
            loc->x += cosf(vyaw) * hoFwd;
            loc->y += sinf(vyaw) * hoFwd;
            loc->z += hoUp;
        }
        vrDrove = true;
    }
    g_vrDriving.store(vrDrove, std::memory_order_relaxed);
    if (!vrDrove) {
        g_headOffX.store(0.0f, std::memory_order_relaxed);
        g_headOffY.store(0.0f, std::memory_order_relaxed);
        g_headOffZ.store(0.0f, std::memory_order_relaxed);
    }
    // Stick-pitch-kill gate for the core input bridge, same funnel BS1 feeds.
    bvr::input::publish_vr_gameplay(vrDrove && strictGameplay);

    // FOV write (session 25, BS1 write-block shape): strict gameplay only,
    // VR wants it only while the HMD actually drives, manual lever for flat
    // tests. One-shot save/restore of the user's option value around the
    // whole written span; leaving gameplay restores immediately, and the
    // stale-restore in the ProcessEvent detour covers CalcView-silent
    // scripted scenes. No clamp needed: derivation showed the renderer
    // consumes at least up to 150 unclamped, and suggested_hfov_deg caps at
    // 160 on its own.
    if (optionFov) {
        float vrFov = g_forceHeadsetFov.load(std::memory_order_relaxed)
                          ? bvr::vr::suggested_hfov_deg()
                          : 0.0f;
        bool wantVr = strictGameplay && vrDrove && vrFov > 0.0f;
        bool wantManual =
            strictGameplay && g_gameFovWrite.load(std::memory_order_relaxed);
        if (wantVr || wantManual) {
            if (!g_wasWritingGameFov) {
                g_savedGameFov = *optionFov;
                g_wasWritingGameFov = true;
                BVR_LOG("[b2r] game fov write ON (saved option %d)", g_savedGameFov);
            }
            float want = wantVr ? vrFov : g_gameFovDeg.load(std::memory_order_relaxed);
            int32_t wantInt = static_cast<int32_t>(want + 0.5f);
            if (*optionFov != wantInt) *optionFov = wantInt;
        } else if (g_wasWritingGameFov) {
            *optionFov = g_savedGameFov;
            g_wasWritingGameFov = false;
            BVR_LOG("[b2r] game fov write OFF (restored option %d)", g_savedGameFov);
        }
    }

    // Debug camera offset - the cheapest "the block is writable on this game
    // too" proof, log-measurable via the heartbeat.
    loc->x += g_offsetX.load(std::memory_order_relaxed);
    loc->y += g_offsetY.load(std::memory_order_relaxed);
    loc->z += g_offsetZ.load(std::memory_order_relaxed);

    // Heartbeat LAST so it reports the FINAL camera handed back to the game -
    // drive, offsets and all. This is what the flat 6DOF checks measure
    // (offset -> exact UU delta; simhead -> exact rotator units; sim position
    // -> headOff), so it must not read the pre-drive values.
    if (g_logCamera.load(std::memory_order_relaxed)) {
        if (g_lastHeartbeatMs == 0) {
            g_lastHeartbeatMs = now;
            g_heartbeatBaseCount = count;
        } else if (now - g_lastHeartbeatMs >= 1000) {
            BVR_LOG("[b2r] camera: loc=(%.1f %.1f %.1f) rot=(%d %d %d) fov=%d "
                    "headOff=(%.1f %.1f %.1f) drive=%d (%u calls/s)",
                    loc->x, loc->y, loc->z, rot->pitch, rot->yaw, rot->roll,
                    g_lastOptionFov.load(std::memory_order_relaxed),
                    g_headOffX.load(std::memory_order_relaxed),
                    g_headOffY.load(std::memory_order_relaxed),
                    g_headOffZ.load(std::memory_order_relaxed), vrDrove ? 1 : 0,
                    count - g_heartbeatBaseCount);
            g_lastHeartbeatMs = now;
            g_heartbeatBaseCount = count;
        }
    } else {
        g_lastHeartbeatMs = 0;
    }
}

// The CalcView-silent hole (BS1 session-22 lesson, bathysphere descent):
// scripted cameras can stop PlayerCalcView entirely, so the write block's
// OFF edge never runs and the user's option would stay overwritten. BS1
// restores from its scene-build detour; BS2 has no scenedraw hook, but
// ProcessEvent keeps firing for every script event, so the restore ticks
// from the detour below. Game thread only.
void restore_game_fov_if_stale(uint64_t staleMs) {
    if (!g_wasWritingGameFov) return; // steady-state cost: one bool read
    uint64_t now = GetTickCount64();
    if (g_lastCalcViewMs == 0 || now - g_lastCalcViewMs < staleMs) return;
    int32_t* optionFov = patterns::hfov_option_ptr();
    if (!optionFov) return;
    *optionFov = g_savedGameFov;
    g_wasWritingGameFov = false;
    BVR_LOG("[b2r] game fov write OFF (restored option %d - calcview silent %llu ms)",
            g_savedGameFov,
            static_cast<unsigned long long>(now - g_lastCalcViewMs));
}

// --- the detours -------------------------------------------------------------

// FindFunctionChecked: learn the PlayerCalcView UFunction pointer with zero
// UObject-layout assumptions - the inlined camera sites resolve the name on
// EVERY dispatch, so the cache stays fresh from the first gameplay frame.
void* __fastcall FindFuncDetour(void* self, void* edx, uint32_t nameIndex,
                                uint32_t nameNumber, uint32_t global) {
    void* fn = g_originalFF(self, edx, nameIndex, nameNumber, global);
    if (g_fnameIndexGlobal && fn &&
        nameIndex == *reinterpret_cast<const uint32_t*>(g_fnameIndexGlobal)) {
        void* prev = g_calcViewFn.exchange(fn, std::memory_order_relaxed);
        if (prev != fn)
            BVR_LOG("[b2r] PlayerCalcView UFunction learned: %p (was %p, this=%p)", fn,
                    prev, self);
    }
    return fn;
}

// ProcessEvent: EVERY script event in the game passes through here - the
// pre-filter work must stay tiny. Camera work happens only on a pointer match
// with the learned UFunction; the 1 Hz command poll ticks through a cheap
// call counter so the seam works at the menu too (no CalcView there on BS2).
void __fastcall ProcessEventDetour(void* self, void* edx, void* fn, void* parms,
                                   void* result) {
    g_originalPE(self, edx, fn, parms, result);

    static uint32_t s_pollGate = 0; // game thread only
    if ((++s_pollGate & 0xFF) == 0) poll_command_file(GetTickCount64());
    if ((s_pollGate & 0x3F) == 0) restore_game_fov_if_stale(400);

    if (fn && parms && fn == g_calcViewFn.load(std::memory_order_relaxed))
        calcview_tail(self, static_cast<CalcViewParams*>(parms));
}

void atomic_slider(const char* label, std::atomic<float>& value, float lo, float hi) {
    float v = value.load(std::memory_order_relaxed);
    if (ImGui::SliderFloat(label, &v, lo, hi)) value.store(v, std::memory_order_relaxed);
}

} // namespace

void init_image(const bvr::pattern_scan::ProcessImage& image) {
    g_imageBase = reinterpret_cast<uintptr_t>(image.base);
    g_imageSize = image.size;
}

bool install(const patterns::Symbols& symbols) {
    if (!symbols.processEvent || !symbols.findFuncChecked || !symbols.fnameIndexGlobal)
        return false;
    g_fnameIndexGlobal = symbols.fnameIndexGlobal;

    MH_STATUS status = MH_CreateHook(symbols.findFuncChecked,
                                     reinterpret_cast<void*>(&FindFuncDetour),
                                     reinterpret_cast<void**>(&g_originalFF));
    if (status != MH_OK) {
        BVR_LOG("[b2r] MH_CreateHook(findfunc) failed: %s", MH_StatusToString(status));
        return false;
    }
    status = MH_CreateHook(symbols.processEvent,
                           reinterpret_cast<void*>(&ProcessEventDetour),
                           reinterpret_cast<void**>(&g_originalPE));
    if (status != MH_OK) {
        BVR_LOG("[b2r] MH_CreateHook(processevent) failed: %s", MH_StatusToString(status));
        MH_RemoveHook(symbols.findFuncChecked);
        return false;
    }
    // Self-enabling so these hooks' activation never rides on another
    // module's MH_EnableHook(MH_ALL_HOOKS). FindFunctionChecked goes live
    // FIRST: ProcessEvent's filter no-ops until the learner has run, so this
    // order can never dispatch on a stale null.
    status = MH_EnableHook(symbols.findFuncChecked);
    if (status == MH_OK) status = MH_EnableHook(symbols.processEvent);
    if (status != MH_OK) {
        BVR_LOG("[b2r] MH_EnableHook(calcview seam) failed: %s", MH_StatusToString(status));
        MH_RemoveHook(symbols.findFuncChecked);
        MH_RemoveHook(symbols.processEvent);
        return false;
    }

    g_peTarget = symbols.processEvent;
    g_hookLive.store(true, std::memory_order_relaxed);
    BVR_LOG("[b2r] calcview seam installed (ProcessEvent %p + FindFunctionChecked %p)",
            symbols.processEvent, symbols.findFuncChecked);
    return true;
}

bool hook_live() {
    return g_hookLive.load(std::memory_order_relaxed);
}

void set_fov_override(float hfovDeg) {
    if (hfovDeg > 0.0f) {
        g_gameFovDeg.store(hfovDeg, std::memory_order_relaxed);
        g_gameFovWrite.store(true, std::memory_order_relaxed);
    } else {
        g_gameFovWrite.store(false, std::memory_order_relaxed);
    }
}

void draw_debug_ui() {
    if (!hook_live()) {
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f),
                           "CalcView seam: scan FAILED - running flat");
        return;
    }

    ImGui::Text("ProcessEvent hook: LIVE @ %p", g_peTarget);
    void* fn = g_calcViewFn.load(std::memory_order_relaxed);
    if (fn)
        ImGui::Text("PlayerCalcView UFunction: %p", fn);
    else
        ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.4f, 1.0f),
                           "PlayerCalcView UFunction: not seen yet (load a save)");

    static uint64_t lastSample = 0;
    static uint32_t lastCount = 0;
    static uint32_t callsPerSec = 0;
    uint64_t now = GetTickCount64();
    uint32_t total = g_callCount.load(std::memory_order_relaxed);
    if (lastSample == 0) {
        lastSample = now;
        lastCount = total;
    } else if (now - lastSample >= 1000) {
        callsPerSec =
            static_cast<uint32_t>((total - lastCount) * 1000ull / (now - lastSample));
        lastSample = now;
        lastCount = total;
    }

    int32_t pitch = g_lastPitch.load(std::memory_order_relaxed);
    int32_t yaw = g_lastYaw.load(std::memory_order_relaxed);
    int32_t roll = g_lastRoll.load(std::memory_order_relaxed);
    ImGui::Text("calcview calls: %u total, %u/s", total, callsPerSec);
    ImGui::Text("pc: %p  view actor: %p (vtbl RVA 0x%X)",
                g_playerController.load(std::memory_order_relaxed),
                g_lastViewActor.load(std::memory_order_relaxed),
                g_lastVtblRva.load(std::memory_order_relaxed));
    ImGui::Text("loc: %.1f %.1f %.1f", g_lastLocX.load(std::memory_order_relaxed),
                g_lastLocY.load(std::memory_order_relaxed),
                g_lastLocZ.load(std::memory_order_relaxed));
    ImGui::Text("rot: %d %d %d (%.1f %.1f %.1f deg)", pitch, yaw, roll,
                pitch / kRotUnitsPerDegree, yaw / kRotUnitsPerDegree,
                roll / kRotUnitsPerDegree);

    if (ImGui::CollapsingHeader("VR camera (M3)", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Text(g_vrDriving.load(std::memory_order_relaxed)
                        ? "camera: driven by HMD pose"
                        : "camera: game (press VR camera ON, needs gameplay view)");
        ImGui::Text("head offset: (%.1f %.1f %.1f) UU",
                    g_headOffX.load(std::memory_order_relaxed),
                    g_headOffY.load(std::memory_order_relaxed),
                    g_headOffZ.load(std::memory_order_relaxed));
        if (ImGui::Button("VR camera ON (enable + camera mode)")) {
            bvr::vr::set_enabled(true);
            bvr::vr::set_camera_mode(true);
        }
        ImGui::SameLine();
        if (ImGui::Button("VR camera OFF")) bvr::vr::set_camera_mode(false);
        if (ImGui::Button("Recenter (seated pose + view yaw)"))
            g_recenterRequested.store(true, std::memory_order_relaxed);
        atomic_slider("World scale (UU per m)", g_worldScale, 10.0f, 200.0f);
        atomic_slider("Head offset up (UU)", g_headOffUpUu, -150.0f, 150.0f);
        atomic_slider("Head offset fwd (UU)", g_headOffFwdUu, -80.0f, 80.0f);
        bool forceFov = g_forceHeadsetFov.load(std::memory_order_relaxed);
        if (ImGui::Checkbox("Force headset FOV (off = game FOV, narrower)", &forceFov))
            g_forceHeadsetFov.store(forceFov, std::memory_order_relaxed);
    }

    if (ImGui::CollapsingHeader("Camera debug")) {
        atomic_slider("Offset X (UU)", g_offsetX, -500.0f, 500.0f);
        atomic_slider("Offset Y (UU)", g_offsetY, -500.0f, 500.0f);
        atomic_slider("Offset Z (UU)", g_offsetZ, -500.0f, 500.0f);
        bool logCam = g_logCamera.load(std::memory_order_relaxed);
        if (ImGui::Checkbox("Log camera (1 Hz to file)", &logCam))
            g_logCamera.store(logCam, std::memory_order_relaxed);
        int32_t optFov = g_lastOptionFov.load(std::memory_order_relaxed);
        if (optFov > 0)
            ImGui::Text("fov option: %d (readback claims it)", optFov);
        else
            ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.4f, 1.0f),
                               "fov option: settings object not located yet");
        bool gfovOn = g_gameFovWrite.load(std::memory_order_relaxed);
        if (ImGui::Checkbox("Game FOV write (manual)", &gfovOn))
            g_gameFovWrite.store(gfovOn, std::memory_order_relaxed);
        atomic_slider("Game FOV (deg)", g_gameFovDeg, 60.0f, 150.0f);
    }
}

} // namespace bvr::b2r::camera
