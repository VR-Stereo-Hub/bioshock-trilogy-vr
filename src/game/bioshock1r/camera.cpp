// Hook behavior (call the original, then adjust the writable out-params;
// publish state through atomics) follows
// itsloopyo/bioshock-remastered-headtracking (MIT), src/engine_hook.rs.

#include "game/bioshock1r/camera.h"

#include "core/util/log.h"
#include "core/vr/openxr_runtime.h"
#include "game/bioshock1r/patterns.h"

#include <windows.h>
#include <MinHook.h>

#include <imgui.h>

#include <atomic>
#include <cmath>
#include <cstdint>

namespace bvr::b1r::camera {
namespace {

struct FVector { float x, y, z; };             // Unreal units
struct FRotator { int32_t pitch, yaw, roll; }; // 65536 units per full turn

constexpr float kPi = 3.14159265f;
constexpr float kRotUnitsPerDegree = 65536.0f / 360.0f;
constexpr float kRotUnitsPerRadian = 65536.0f / (2.0f * kPi);

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
std::atomic<float> g_worldScale{50.0f};        // Unreal units per meter
std::atomic<bool>  g_recenterRequested{true};  // auto-recenter on first drive
std::atomic<bool>  g_vrDriving{false};         // telemetry for the UI

// Telemetry: game thread writes, overlay thread reads.
std::atomic<uint32_t> g_callCount{0};
std::atomic<float>    g_lastLocX{0.0f}, g_lastLocY{0.0f}, g_lastLocZ{0.0f};
std::atomic<int32_t>  g_lastPitch{0}, g_lastYaw{0}, g_lastRoll{0};
std::atomic<float>    g_lastFov{0.0f};
// Display only. The overlay never dereferences this - all game-memory access
// happens on the game thread inside the detour, where `this` is alive.
std::atomic<void*>    g_playerController{nullptr};

using CalcViewFn = void(__fastcall*)(void* self, void* edx, void** viewActor,
                                     FVector* loc, FRotator* rot);
CalcViewFn g_original = nullptr;
void* g_target = nullptr;
std::atomic<bool> g_hookLive{false};
std::atomic<bool> g_loggedFirstFire{false};

// Game-thread-only bookkeeping (never touched by the overlay).
bool g_wasOverridingFov = false;
float g_savedFov = 0.0f;
uint64_t g_lastHeartbeatMs = 0;
uint32_t g_heartbeatBaseCount = 0;
bool g_haveRecenter = false;
bvr::vr::HeadPose g_recenterPose{};
float g_recenterYawRad = 0.0f;

float* fov_ptr(void* pc) {
    return reinterpret_cast<float*>(static_cast<uint8_t*>(pc) + patterns::kFovLiveOffset);
}

// ---- XR -> Unreal conversion (adapter owns all unit/axis semantics) --------
// XR LOCAL space: right +X, up +Y, forward -Z, meters, right-handed.
// UE2.5: forward +X, right +Y, up +Z; FRotator 65536 units/turn, positive yaw
// turns toward +Y (right), positive pitch looks up, positive roll tilts
// clockwise (right).

void quat_rotate(float qx, float qy, float qz, float qw, const float v[3], float out[3]) {
    float t[3] = {2.0f * (qy * v[2] - qz * v[1]), 2.0f * (qz * v[0] - qx * v[2]),
                  2.0f * (qx * v[1] - qy * v[0])};
    out[0] = v[0] + qw * t[0] + (qy * t[2] - qz * t[1]);
    out[1] = v[1] + qw * t[1] + (qz * t[0] - qx * t[2]);
    out[2] = v[2] + qw * t[2] + (qx * t[1] - qy * t[0]);
}

void xr_to_ue(const float v[3], float out[3]) {
    out[0] = -v[2]; // XR -Z (forward) -> UE +X
    out[1] = v[0];  // XR +X (right)   -> UE +Y
    out[2] = v[1];  // XR +Y (up)      -> UE +Z
}

struct UeAngles { float yawRad, pitchRad, rollRad; };

UeAngles hmd_angles(const bvr::vr::HeadPose& hp) {
    const float kFwd[3] = {0.0f, 0.0f, -1.0f};
    const float kUp[3] = {0.0f, 1.0f, 0.0f};
    float fxr[3], uxr[3], f[3], u[3];
    quat_rotate(hp.qx, hp.qy, hp.qz, hp.qw, kFwd, fxr);
    quat_rotate(hp.qx, hp.qy, hp.qz, hp.qw, kUp, uxr);
    xr_to_ue(fxr, f);
    xr_to_ue(uxr, u);

    UeAngles a{};
    a.yawRad = atan2f(f[1], f[0]);
    float len2d = sqrtf(f[0] * f[0] + f[1] * f[1]);
    a.pitchRad = atan2f(f[2], len2d);
    if (len2d > 0.001f) { // gimbal guard: keep roll 0 when looking straight up/down
        // Zero-roll frame from the forward vector, then measure the actual up
        // vector against it. rn = normalize(cross(worldUp, f)), un = cross(f, rn).
        float rn[3] = {-f[1] / len2d, f[0] / len2d, 0.0f};
        float un[3] = {-f[2] * rn[1], f[2] * rn[0], f[0] * rn[1] - f[1] * rn[0]};
        a.rollRad = atan2f(u[0] * rn[0] + u[1] * rn[1],
                           u[0] * un[0] + u[1] * un[1] + u[2] * un[2]);
    }
    return a;
}

// eventPlayerCalcView is __thiscall; __fastcall with a dummy EDX slot is
// register/stack/cleanup-identical and works as a plain free function.
void __fastcall CalcViewDetour(void* self, void* edx, void** viewActor,
                               FVector* loc, FRotator* rot) {
    g_original(self, edx, viewActor, loc, rot);

    g_playerController.store(self, std::memory_order_relaxed);
    uint32_t count = g_callCount.fetch_add(1, std::memory_order_relaxed) + 1;

    float gameFov = *fov_ptr(self);
    g_lastFov.store(gameFov, std::memory_order_relaxed);
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
    bvr::vr::HeadPose hp{};
    if (loc && rot && bvr::vr::vr_camera_mode() && bvr::vr::get_head_pose(hp)) {
        UeAngles a = hmd_angles(hp);
        if (g_recenterRequested.exchange(false, std::memory_order_relaxed) || !g_haveRecenter) {
            g_recenterPose = hp;
            g_recenterYawRad = a.yawRad;
            g_haveRecenter = true;
            BVR_LOG("[b1r] vr camera recentered (yaw %.1f deg)", a.yawRad * 57.29578f);
        }

        int32_t gameYawUnits = rot->yaw;
        float gameYawRad = static_cast<float>(gameYawUnits) / kRotUnitsPerRadian;
        rot->pitch = static_cast<int32_t>(a.pitchRad * kRotUnitsPerRadian);
        rot->roll = static_cast<int32_t>(a.rollRad * kRotUnitsPerRadian);
        rot->yaw = gameYawUnits +
                   static_cast<int32_t>((a.yawRad - g_recenterYawRad) * kRotUnitsPerRadian);

        float dxr[3] = {hp.px - g_recenterPose.px, hp.py - g_recenterPose.py,
                        hp.pz - g_recenterPose.pz};
        float d[3];
        xr_to_ue(dxr, d);
        float scale = g_worldScale.load(std::memory_order_relaxed);
        // Into the recenter-local frame, then out by the game yaw (which is
        // where recenter-forward points now, since our yaw is purely additive).
        float c = cosf(-g_recenterYawRad), s = sinf(-g_recenterYawRad);
        float lx = d[0] * c - d[1] * s;
        float ly = d[0] * s + d[1] * c;
        float cg = cosf(gameYawRad), sg = sinf(gameYawRad);
        loc->x += (lx * cg - ly * sg) * scale;
        loc->y += (lx * sg + ly * cg) * scale;
        loc->z += d[2] * scale;

        vrFov = bvr::vr::suggested_hfov_deg();
        vrDrove = true;
    }
    g_vrDriving.store(vrDrove, std::memory_order_relaxed);

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

    if (ImGui::CollapsingHeader("VR camera (M3)", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Text(g_vrDriving.load(std::memory_order_relaxed)
                        ? "camera: driven by HMD pose"
                        : "camera: game (enable VR camera mode in the VR section)");
        atomic_slider("World scale (UU per m)", g_worldScale, 10.0f, 200.0f);
        if (ImGui::Button("Recenter (seated pose + view yaw)"))
            g_recenterRequested.store(true, std::memory_order_relaxed);
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

        bool fov = g_fovOverride.load(std::memory_order_relaxed);
        if (ImGui::Checkbox("FOV override (game value restored when off)", &fov))
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
