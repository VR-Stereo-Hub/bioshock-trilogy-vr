#include "game/bioshock2r/hands.h"

#include "core/util/log.h"
#include "core/vr/openxr_runtime.h"
#include "game/bioshock2r/bones.h"
#include "game/shared/ue_math.h"

#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstring>

namespace bvr::b2r::hands {
namespace {

std::atomic<bool> g_enabled{false};
std::atomic<bool> g_useAimPose{true}; // aim pose default: the barrel agrees with the ray
// Model trims (deg) and grip offset (cm), the tuning surface. Applied in the
// controller's LOCAL frame (model_pose_from_xr) / the final trimmed basis.
std::atomic<float> g_trimPitch{0.0f}, g_trimYaw{0.0f}, g_trimRoll{0.0f};
std::atomic<float> g_offFwdCm{0.0f}, g_offRightCm{0.0f}, g_offUpCm{0.0f};
// Telemetry.
std::atomic<uint32_t> g_frames{0};
std::atomic<bool> g_wasDriving{false};

} // namespace

void on_calcview(const FrameContext& ctx, bool strictGameplay) {
    bool want = g_enabled.load(std::memory_order_relaxed) && strictGameplay &&
                ctx.vrDriving;
    if (!want) {
        if (g_wasDriving.exchange(false, std::memory_order_relaxed))
            bones::release("gate closed");
        return;
    }
    bvr::vr::HeadPose hp{};
    if (!bvr::vr::get_hand_pose(1, g_useAimPose.load(std::memory_order_relaxed), hp)) {
        if (g_wasDriving.exchange(false, std::memory_order_relaxed))
            bones::release("hand untracked");
        return;
    }
    float pos[3] = {hp.px, hp.py, hp.pz};
    float quat[4] = {hp.qx, hp.qy, hp.qz, hp.qw};
    GamePose gp = model_pose_from_xr(ctx, pos, quat,
                                     g_trimPitch.load(std::memory_order_relaxed),
                                     g_trimYaw.load(std::memory_order_relaxed),
                                     g_trimRoll.load(std::memory_order_relaxed));
    // Grip offset along the FINAL trimmed basis, cm -> UU by worldScale/100.
    float fwd[3], right[3], up[3];
    ue_rot_basis(gp.rot, fwd, right, up);
    float k = ctx.worldScale / 100.0f;
    float oF = g_offFwdCm.load(std::memory_order_relaxed) * k;
    float oR = g_offRightCm.load(std::memory_order_relaxed) * k;
    float oU = g_offUpCm.load(std::memory_order_relaxed) * k;
    gp.loc.x += fwd[0] * oF + right[0] * oR + up[0] * oU;
    gp.loc.y += fwd[1] * oF + right[1] * oR + up[1] * oU;
    gp.loc.z += fwd[2] * oF + right[2] * oR + up[2] * oU;

    if (bones::drive(ctx, gp)) {
        g_wasDriving.store(true, std::memory_order_relaxed);
        g_frames.fetch_add(1, std::memory_order_relaxed);
    }
}

bool enabled() {
    return g_enabled.load(std::memory_order_relaxed);
}

bool handle_command(const char* args) {
    float a = 0.0f, b = 0.0f, c = 0.0f;
    if (strncmp(args, "on", 2) == 0 && (args[2] == '\0' || args[2] == '\n' || args[2] == ' ')) {
        g_enabled.store(true, std::memory_order_relaxed);
        BVR_LOG("[b2r] command: vrhands on (rig rides the RIGHT controller)");
        return true;
    }
    if (strncmp(args, "off", 3) == 0) {
        g_enabled.store(false, std::memory_order_relaxed);
        bones::release("vrhands off");
        BVR_LOG("[b2r] command: vrhands off");
        return true;
    }
    if (strncmp(args, "status", 6) == 0 || *args == '\0') {
        float x = 0, y = 0, z = 0;
        uint64_t age = 0;
        bool haveWrite = bones::last_write(&x, &y, &z, &age);
        BVR_LOG("[b2r] vrhands status: %s pose %s, frames %u, trim (%.1f %.1f %.1f) "
                "offset (%.1f %.1f %.1f) cm",
                g_enabled.load(std::memory_order_relaxed) ? "ON" : "off",
                g_useAimPose.load(std::memory_order_relaxed) ? "aim" : "grip",
                g_frames.load(std::memory_order_relaxed),
                g_trimPitch.load(std::memory_order_relaxed),
                g_trimYaw.load(std::memory_order_relaxed),
                g_trimRoll.load(std::memory_order_relaxed),
                g_offFwdCm.load(std::memory_order_relaxed),
                g_offRightCm.load(std::memory_order_relaxed),
                g_offUpCm.load(std::memory_order_relaxed));
        if (haveWrite)
            BVR_LOG("[b2r]   last write loc=(%.1f %.1f %.1f) age %llu ms", x, y, z,
                    static_cast<unsigned long long>(age));
        return true;
    }
    if (sscanf_s(args, "trim %f %f %f", &a, &b, &c) == 3) {
        g_trimPitch.store(a, std::memory_order_relaxed);
        g_trimYaw.store(b, std::memory_order_relaxed);
        g_trimRoll.store(c, std::memory_order_relaxed);
        BVR_LOG("[b2r] command: vrhands trim %.1f %.1f %.1f", a, b, c);
        return true;
    }
    if (sscanf_s(args, "offset %f %f %f", &a, &b, &c) == 3) {
        g_offFwdCm.store(a, std::memory_order_relaxed);
        g_offRightCm.store(b, std::memory_order_relaxed);
        g_offUpCm.store(c, std::memory_order_relaxed);
        BVR_LOG("[b2r] command: vrhands offset %.1f %.1f %.1f cm", a, b, c);
        return true;
    }
    if (strncmp(args, "pose", 4) == 0) {
        g_useAimPose.store(strstr(args, "grip") == nullptr, std::memory_order_relaxed);
        BVR_LOG("[b2r] command: vrhands pose %s",
                g_useAimPose.load(std::memory_order_relaxed) ? "aim" : "grip");
        return true;
    }
    BVR_LOG("[b2r] vrhands: on|off | status | trim <p> <y> <r> | offset <f> <r> <u> | "
            "pose aim|grip");
    return true;
}

} // namespace bvr::b2r::hands
