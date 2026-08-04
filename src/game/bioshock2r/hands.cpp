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

// DEFAULT ON since the session-39 wrap (user request, first-look build):
// inert without strict gameplay + the HMD driving + a tracked hand.
std::atomic<bool> g_enabled{true};
std::atomic<bool> g_useAimPose{true}; // aim pose default: the barrel agrees with the ray
// Per-hand model trims (deg) and grip offset (cm), the tuning surface. Applied
// in the controller's LOCAL frame (model_pose_from_xr) / the final trimmed
// basis. Session 40: per hand, because the two clusters ride two controllers
// and each needs its own calibration.
std::atomic<float> g_trimPitch[2] = {{0.0f}, {0.0f}};
std::atomic<float> g_trimYaw[2] = {{0.0f}, {0.0f}};
std::atomic<float> g_trimRoll[2] = {{0.0f}, {0.0f}};
std::atomic<float> g_offFwdCm[2] = {{0.0f}, {0.0f}};
std::atomic<float> g_offRightCm[2] = {{0.0f}, {0.0f}};
std::atomic<float> g_offUpCm[2] = {{0.0f}, {0.0f}};
// Telemetry.
std::atomic<uint32_t> g_frames[2] = {{0}, {0}};
std::atomic<bool> g_wasDriving[2] = {{false}, {false}};

int hand_arg(const char* s) {
    return (s && (*s == 'l' || *s == 'L')) ? 0 : 1;
}

// Whole-token match. `strncmp(args, "off", 3)` also accepts "offset", which is
// exactly what it did before session 40: every `vrhands offset ...` silently
// DISABLED the hands instead of setting an offset (found by the preset
// round-trip - the ini kept saving zeros).
bool is_verb(const char* args, const char* verb) {
    size_t n = strlen(verb);
    if (strncmp(args, verb, n) != 0) return false;
    char t = args[n];
    return t == '\0' || t == ' ' || t == '\n' || t == '\r' || t == '\t';
}

} // namespace

void on_calcview(const FrameContext& ctx, bool strictGameplay) {
    bool want = g_enabled.load(std::memory_order_relaxed) && strictGameplay &&
                ctx.vrDriving;
    bool useAim = g_useAimPose.load(std::memory_order_relaxed);
    // Each hand drives and releases INDEPENDENTLY: a lost left controller must
    // never hand the right hand's cluster back mid-aim.
    for (int h = 0; h < 2; ++h) {
        bvr::vr::HeadPose hp{};
        if (!want || !bvr::vr::get_hand_pose(h, useAim, hp)) {
            if (g_wasDriving[h].exchange(false, std::memory_order_relaxed))
                bones::release(want ? "hand untracked" : "gate closed", h);
            continue;
        }
        float pos[3] = {hp.px, hp.py, hp.pz};
        float quat[4] = {hp.qx, hp.qy, hp.qz, hp.qw};
        GamePose gp = model_pose_from_xr(ctx, pos, quat,
                                         g_trimPitch[h].load(std::memory_order_relaxed),
                                         g_trimYaw[h].load(std::memory_order_relaxed),
                                         g_trimRoll[h].load(std::memory_order_relaxed));
        // Grip offset along the FINAL trimmed basis, cm -> UU by worldScale/100.
        float fwd[3], right[3], up[3];
        ue_rot_basis(gp.rot, fwd, right, up);
        float k = ctx.worldScale / 100.0f;
        float oF = g_offFwdCm[h].load(std::memory_order_relaxed) * k;
        float oR = g_offRightCm[h].load(std::memory_order_relaxed) * k;
        float oU = g_offUpCm[h].load(std::memory_order_relaxed) * k;
        gp.loc.x += fwd[0] * oF + right[0] * oR + up[0] * oU;
        gp.loc.y += fwd[1] * oF + right[1] * oR + up[1] * oU;
        gp.loc.z += fwd[2] * oF + right[2] * oR + up[2] * oU;

        if (bones::drive(ctx, gp, h)) {
            g_wasDriving[h].store(true, std::memory_order_relaxed);
            g_frames[h].fetch_add(1, std::memory_order_relaxed);
        }
    }
}

bool enabled() {
    return g_enabled.load(std::memory_order_relaxed);
}

float trim_pitch(int h) { return g_trimPitch[h & 1].load(std::memory_order_relaxed); }
float trim_yaw(int h) { return g_trimYaw[h & 1].load(std::memory_order_relaxed); }
float trim_roll(int h) { return g_trimRoll[h & 1].load(std::memory_order_relaxed); }
float off_fwd_cm(int h) { return g_offFwdCm[h & 1].load(std::memory_order_relaxed); }
float off_right_cm(int h) { return g_offRightCm[h & 1].load(std::memory_order_relaxed); }
float off_up_cm(int h) { return g_offUpCm[h & 1].load(std::memory_order_relaxed); }

void set_trim(int h, float p, float y, float r) {
    h &= 1;
    g_trimPitch[h].store(p, std::memory_order_relaxed);
    g_trimYaw[h].store(y, std::memory_order_relaxed);
    g_trimRoll[h].store(r, std::memory_order_relaxed);
}

void set_offset(int h, float f, float r, float u) {
    h &= 1;
    g_offFwdCm[h].store(f, std::memory_order_relaxed);
    g_offRightCm[h].store(r, std::memory_order_relaxed);
    g_offUpCm[h].store(u, std::memory_order_relaxed);
}

bool handle_command(const char* args) {
    float a = 0.0f, b = 0.0f, c = 0.0f;
    char side[8] = {};
    if (is_verb(args, "on")) {
        g_enabled.store(true, std::memory_order_relaxed);
        BVR_LOG("[b2r] command: vrhands on (left cluster -> left controller, "
                "right cluster + weapon -> right)");
        return true;
    }
    if (is_verb(args, "off")) {
        g_enabled.store(false, std::memory_order_relaxed);
        bones::release("vrhands off", -1);
        BVR_LOG("[b2r] command: vrhands off");
        return true;
    }
    if (is_verb(args, "status") || *args == '\0') {
        BVR_LOG("[b2r] vrhands status: %s pose %s",
                g_enabled.load(std::memory_order_relaxed) ? "ON" : "off",
                g_useAimPose.load(std::memory_order_relaxed) ? "aim" : "grip");
        for (int h = 0; h < 2; ++h) {
            float x = 0, y = 0, z = 0;
            uint64_t age = 0;
            bool haveWrite = bones::last_write(h, &x, &y, &z, &age);
            BVR_LOG("[b2r]   %s: frames %u, trim (%.1f %.1f %.1f) offset "
                    "(%.1f %.1f %.1f) cm, scale %.2f",
                    h ? "R" : "L", g_frames[h].load(std::memory_order_relaxed),
                    g_trimPitch[h].load(std::memory_order_relaxed),
                    g_trimYaw[h].load(std::memory_order_relaxed),
                    g_trimRoll[h].load(std::memory_order_relaxed),
                    g_offFwdCm[h].load(std::memory_order_relaxed),
                    g_offRightCm[h].load(std::memory_order_relaxed),
                    g_offUpCm[h].load(std::memory_order_relaxed), bones::scale_of(h));
            if (haveWrite)
                BVR_LOG("[b2r]     last write loc=(%.1f %.1f %.1f) age %llu ms", x, y, z,
                        static_cast<unsigned long long>(age));
        }
        return true;
    }
    // trim l|r <p> <y> <r>
    if (sscanf_s(args, "trim %7s %f %f %f", side, (unsigned)sizeof side, &a, &b, &c) == 4) {
        int h = hand_arg(side);
        set_trim(h, a, b, c);
        BVR_LOG("[b2r] command: vrhands trim %s %.1f %.1f %.1f", h ? "r" : "l", a, b, c);
        return true;
    }
    // offset l|r <f> <r> <u>
    if (sscanf_s(args, "offset %7s %f %f %f", side, (unsigned)sizeof side, &a, &b, &c) ==
        4) {
        int h = hand_arg(side);
        set_offset(h, a, b, c);
        BVR_LOG("[b2r] command: vrhands offset %s %.1f %.1f %.1f cm", h ? "r" : "l", a, b,
                c);
        return true;
    }
    // scale [l|r] <f> - bare form sets both (the common case: the whole rig is
    // the wrong size). Decoupled from worldscale by design.
    if (strncmp(args, "scale", 5) == 0) {
        if (sscanf_s(args, "scale %7s %f", side, (unsigned)sizeof side, &a) == 2 &&
            (side[0] == 'l' || side[0] == 'L' || side[0] == 'r' || side[0] == 'R')) {
            int h = hand_arg(side);
            bones::set_scale(h, a);
            BVR_LOG("[b2r] command: vrhands scale %s %.2f", h ? "r" : "l", a);
        } else if (sscanf_s(args, "scale %f", &a) == 1) {
            bones::set_scale(0, a);
            bones::set_scale(1, a);
            BVR_LOG("[b2r] command: vrhands scale %.2f (both hands)", a);
        } else {
            BVR_LOG("[b2r] vrhands scale [l|r] <factor>");
        }
        return true;
    }
    if (strncmp(args, "pose", 4) == 0) {
        g_useAimPose.store(strstr(args, "grip") == nullptr, std::memory_order_relaxed);
        BVR_LOG("[b2r] command: vrhands pose %s",
                g_useAimPose.load(std::memory_order_relaxed) ? "aim" : "grip");
        return true;
    }
    if (strncmp(args, "arms", 4) == 0) {
        int mode = strstr(args, "hide") ? 2 : strstr(args, "game") ? 0 : 1;
        bones::set_arms_mode(mode);
        BVR_LOG("[b2r] command: vrhands arms %s",
                mode == 2 ? "hide" : mode == 0 ? "game" : "follow");
        return true;
    }
    if (strncmp(args, "scaleweapon", 11) == 0) {
        bones::set_scale_attach(strstr(args, "off") == nullptr);
        BVR_LOG("[b2r] command: vrhands scaleweapon %s (off = weapon keeps authored "
                "size; some attachments inverse-scale)",
                bones::scale_attach() ? "on" : "off");
        return true;
    }
    // animtrans BEFORE anim: is_verb keeps them apart, but parse the longer
    // token first anyway (the off/offset lesson).
    if (sscanf_s(args, "animtrans %f", &a) == 1) {
        bones::set_anim_trans(a);
        BVR_LOG("[b2r] command: vrhands animtrans %.2f (authored wrist travel "
                "re-added; 0 = glued to the controller)",
                bones::anim_trans());
        return true;
    }
    if (is_verb(args, "anim")) {
        bones::set_anim_mode(strstr(args, "off") == nullptr);
        BVR_LOG("[b2r] command: vrhands anim %s (%s)",
                bones::anim_mode() ? "on" : "off",
                bones::anim_mode()
                    ? "engine animations compose into the driven frame"
                    : "rigid reference drive - animations frozen");
        return true;
    }
    BVR_LOG("[b2r] vrhands: on|off | status | trim l|r <p> <y> <r> | "
            "offset l|r <f> <r> <u> | scale [l|r] <f> | pose aim|grip | "
            "arms follow|hide|game | scaleweapon on|off | anim on|off | "
            "animtrans <0..1>");
    return true;
}

} // namespace bvr::b2r::hands
