// Hook behavior (call the original, then adjust the writable out-params;
// publish state through atomics) follows
// itsloopyo/bioshock-remastered-headtracking (MIT), src/engine_hook.rs,
// via the bioshock1r camera module this file is the M3 subset of.

#include "game/bioshock2r/camera.h"

#include "core/gfx/hud_capture.h"
#include "core/input/xinput_bridge.h"
#include "core/ui/overlay.h"
#include "core/util/log.h"
#include "core/vr/openxr_runtime.h"
#include "game/bioshock2r/patterns.h"
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

// Controls: overlay thread writes, game thread reads. All relaxed - x86
// lock-free, and a field arriving one frame late is fine for debug sliders.
std::atomic<float> g_offsetX{0.0f}, g_offsetY{0.0f}, g_offsetZ{0.0f};
// Heartbeat on by default during bring-up - 1 line/s proves per-frame firing
// in every session log. Toggle off with `camlog off` / the overlay.
std::atomic<bool> g_logCamera{true};

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

using CalcViewFn = void(__fastcall*)(void* self, void* edx, void** viewActor,
                                     FVector* loc, FRotator* rot);
CalcViewFn g_original = nullptr;
void* g_target = nullptr;
std::atomic<bool> g_hookLive{false};
std::atomic<bool> g_loggedFirstFire{false};

// Game-thread-only bookkeeping (never touched by the overlay).
uintptr_t g_imageBase = 0;
size_t g_imageSize = 0;
uint64_t g_lastHeartbeatMs = 0;
uint32_t g_heartbeatBaseCount = 0;
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
// AShockPlayer's. The menu/attract scene CalcViews with viewActor == the
// PlayerController itself, so this reads false there - deliberately no
// `viewActor == pc` escape hatch (that hatch exists in BS1 only for the aim
// ray, which does not exist here yet).

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
// at 1 Hz on the game thread, same contract as BS1's seam. M3 vocabulary:
//   recenter                     re-reference the seated pose
//   offset <x> <y> <z>           debug camera offset in UU (0 0 0 clears)
//   worldscale <v>               UU per meter
//   headoff <up> <fwd>           head-anchor offset in UU
//   simhead <yaw> <pitch> <roll> [px py pz] [holdMs] | simhead off
//   vrcam on|off                 VR enable + camera mode (core funnels)
//   camlog on|off                1 Hz heartbeat
//   vroverlay on|off             core overlay visibility (bring-up A/B)
//   vrcine <args>                core cinematic-fallback A/B (vrcine status..)

void apply_command(const char* cmd, const char* args) {
    float x = 0.0f, y = 0.0f, z = 0.0f;

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
                int hold = n == 4 ? static_cast<int>(v[3])
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
    } else {
        BVR_LOG("[b2r] unknown command: %s (M3 seam - BS1's wider vocabulary has not "
                "been ported; see camera.cpp)",
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

// --- the detour --------------------------------------------------------------
// eventPlayerCalcView is __thiscall; __fastcall with a dummy EDX slot is
// register/stack/cleanup-identical and works as a plain free function.
// The menu fires this far above frame rate (~7000/s observed on BS1's
// uncapped menu) - everything in here is throttled or O(1).
void __fastcall CalcViewDetour(void* self, void* edx, void** viewActor,
                               FVector* loc, FRotator* rot) {
    g_original(self, edx, viewActor, loc, rot);

    g_playerController.store(self, std::memory_order_relaxed);
    uint32_t count = g_callCount.fetch_add(1, std::memory_order_relaxed) + 1;

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
        BVR_LOG("[b2r] calcview first fire: pc=%p viewactor=%p loc=(%.1f %.1f %.1f) "
                "rot=(%d %d %d)",
                self, viewActor ? *viewActor : nullptr, loc ? loc->x : 0.0f,
                loc ? loc->y : 0.0f, loc ? loc->z : 0.0f, rot ? rot->pitch : 0,
                rot ? rot->yaw : 0, rot ? rot->roll : 0);
    }

    uint64_t now = GetTickCount64();
    poll_command_file(now);

    // Gameplay verdict + candidate-RVA self-diagnosis. Published every call:
    // core's cinematic fallback keys on this verdict AND its staleness, so a
    // silent adapter would park the headset on the quad screen permanently.
    void* va = viewActor ? *viewActor : nullptr;
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
        }
    }

    if (g_logCamera.load(std::memory_order_relaxed)) {
        if (g_lastHeartbeatMs == 0) {
            g_lastHeartbeatMs = now;
            g_heartbeatBaseCount = count;
        } else if (now - g_lastHeartbeatMs >= 1000) {
            // No fov field: BS2 has no verified FOV source yet (the
            // UShockUserSettings candidate is unconsumed at M3).
            BVR_LOG("[b2r] camera: loc=(%.1f %.1f %.1f) rot=(%d %d %d) "
                    "headOff=(%.1f %.1f %.1f) (%u calls/s)",
                    loc ? loc->x : 0.0f, loc ? loc->y : 0.0f, loc ? loc->z : 0.0f,
                    rot ? rot->pitch : 0, rot ? rot->yaw : 0, rot ? rot->roll : 0,
                    g_headOffX.load(std::memory_order_relaxed),
                    g_headOffY.load(std::memory_order_relaxed),
                    g_headOffZ.load(std::memory_order_relaxed),
                    count - g_heartbeatBaseCount);
            g_lastHeartbeatMs = now;
            g_heartbeatBaseCount = count;
        }
    } else {
        g_lastHeartbeatMs = 0;
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
    if (loc && rot && driveHead) {
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

    // Debug camera offset - the cheapest "the out-params are writable on this
    // game too" proof, log-measurable via the heartbeat.
    if (loc) {
        loc->x += g_offsetX.load(std::memory_order_relaxed);
        loc->y += g_offsetY.load(std::memory_order_relaxed);
        loc->z += g_offsetZ.load(std::memory_order_relaxed);
    }
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

bool install(void* eventPlayerCalcView) {
    if (!eventPlayerCalcView) return false;

    MH_STATUS status = MH_CreateHook(eventPlayerCalcView,
                                     reinterpret_cast<void*>(&CalcViewDetour),
                                     reinterpret_cast<void**>(&g_original));
    if (status != MH_OK) {
        BVR_LOG("[b2r] MH_CreateHook(calcview) failed: %s", MH_StatusToString(status));
        return false;
    }
    // Self-enabling so this hook's activation never rides on another module's
    // MH_EnableHook(MH_ALL_HOOKS).
    status = MH_EnableHook(eventPlayerCalcView);
    if (status != MH_OK) {
        BVR_LOG("[b2r] MH_EnableHook(calcview) failed: %s", MH_StatusToString(status));
        MH_RemoveHook(eventPlayerCalcView);
        return false;
    }

    g_target = eventPlayerCalcView;
    g_hookLive.store(true, std::memory_order_relaxed);
    BVR_LOG("[b2r] calcview hook installed (target %p)", eventPlayerCalcView);
    return true;
}

bool hook_live() {
    return g_hookLive.load(std::memory_order_relaxed);
}

void draw_debug_ui() {
    if (!hook_live()) {
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f),
                           "CalcView: scan FAILED - running flat");
        return;
    }

    ImGui::Text("CalcView hook: LIVE @ %p", g_target);

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
    ImGui::Text("calls: %u total, %u/s", total, callsPerSec);
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
    }

    if (ImGui::CollapsingHeader("Camera debug")) {
        atomic_slider("Offset X (UU)", g_offsetX, -500.0f, 500.0f);
        atomic_slider("Offset Y (UU)", g_offsetY, -500.0f, 500.0f);
        atomic_slider("Offset Z (UU)", g_offsetZ, -500.0f, 500.0f);
        bool logCam = g_logCamera.load(std::memory_order_relaxed);
        if (ImGui::Checkbox("Log camera (1 Hz to file)", &logCam))
            g_logCamera.store(logCam, std::memory_order_relaxed);
    }
}

} // namespace bvr::b2r::camera
