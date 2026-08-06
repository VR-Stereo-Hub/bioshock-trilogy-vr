#include "game/bioshockinf/hands.h"

#include "core/util/log.h"
#include "core/vr/openxr_runtime.h"
#include "game/bioshockinf/aim.h"
#include "game/bioshockinf/inf_math.h"
#include "game/bioshockinf/rig.h"

#include <imgui.h>
#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstring>

namespace bvr::bsi::hands {
namespace {

// SHIPS OFF, together with rig's disarmed write. `bsihands status` has to print
// a target that tracks a controller sweep before anything is armed.
std::atomic<bool> g_enabled{false};
std::atomic<bool> g_useAimPose{true};

// BS2's parallel arrays, no per-hand struct. All zero: derived fresh, never
// carried over from another game.
std::atomic<float> g_trimPitch[2] = {{0.0f}, {0.0f}};
std::atomic<float> g_trimYaw[2] = {{0.0f}, {0.0f}};
std::atomic<float> g_trimRoll[2] = {{0.0f}, {0.0f}};
std::atomic<float> g_offFwdCm[2] = {{0.0f}, {0.0f}};
std::atomic<float> g_offRightCm[2] = {{0.0f}, {0.0f}};
std::atomic<float> g_offUpCm[2] = {{0.0f}, {0.0f}};
std::atomic<bool> g_wasDriving[2] = {{false}, {false}};
std::atomic<uint32_t> g_frames[2] = {{0}, {0}};

// Game thread only: the last computed target per hand, stored whether or not
// anything was written. That is what lets status print a target BEFORE the
// write is armed - the pre-arm oracle.
GamePose g_lastTarget[2]{};
bool g_haveTarget[2] = {false, false};

// Whole-token match. `strncmp(args, "off", 3)` also accepts "offset", which on
// BS2 meant every `vrhands offset ...` silently DISABLED the hands instead of
// setting an offset - found only by a preset round-trip that kept saving zeros.
bool is_verb(const char* args, const char* verb) {
    const size_t n = strlen(verb);
    if (strncmp(args, verb, n) != 0) return false;
    const char t = args[n];
    return t == '\0' || t == ' ' || t == '\n' || t == '\r' || t == '\t';
}

int hand_arg(const char* s) { return (s && (*s == 'l' || *s == 'L')) ? 0 : 1; }

void tick(const char* site) {
    FrameContext ctx{};
    const bool haveCtx = frame_context::read(&ctx);
    const bool want = g_enabled.load(std::memory_order_relaxed) && haveCtx && ctx.vrDriving;
    const bool useAim = g_useAimPose.load(std::memory_order_relaxed);
    (void)site;

    for (int h = 0; h < 2; ++h) {
        bvr::vr::HeadPose hp{};
        if (!want || !bvr::vr::get_hand_pose(h, useAim, hp)) {
            // The exchange edge: release fires EXACTLY ONCE per transition, and
            // each hand owns its own edge so a lost LEFT controller cannot hand
            // the RIGHT hand's carrier back mid-aim.
            if (g_wasDriving[h].exchange(false, std::memory_order_relaxed))
                rig::release(want ? "hand untracked" : "gate closed");
            g_haveTarget[h] = false;
            continue;
        }
        // R5: the drive target is the mesh COMPONENT, whose transform is
        // VIEW-RELATIVE - so the pose is built directly in the head's frame and
        // never goes through world space at all. That removes the whole
        // xr_pose_to_game round trip from the model lane (the aim ray still
        // needs it, because a fire ray IS a world quantity).
        //
        // Sign/axis convention, MEASURED not assumed (s46 R5): SetTranslation
        // v0,0,-40 on the component moved the gun DOWN, so the component's
        // local frame is UE's usual X forward / Y right / Z up, matching
        // xr_to_ue's output directly.
        bvr::vr::HeadPose head{};
        if (!bvr::vr::get_head_pose(head)) {
            if (g_wasDriving[h].exchange(false, std::memory_order_relaxed))
                rig::release("no head pose");
            g_haveTarget[h] = false;
            continue;
        }
        // Trim first, in the controller's LOCAL frame - the same compose the ray
        // and core's laser use, so a trimmed model and a trimmed beam stay
        // congruent.
        float trim[4], q[4];
        xr_local_trim_quat(g_trimPitch[h].load(std::memory_order_relaxed) / kRadToDeg,
                           g_trimYaw[h].load(std::memory_order_relaxed) / kRadToDeg,
                           g_trimRoll[h].load(std::memory_order_relaxed) / kRadToDeg, trim);
        const float hq[4] = {hp.qx, hp.qy, hp.qz, hp.qw};
        quat_mul(hq, trim, q);

        // Into the head's local frame: conj(headQuat) applied to both the offset
        // and the orientation.
        const float hc[4] = {-head.qx, -head.qy, -head.qz, head.qw};
        const float dxr[3] = {hp.px - head.px, hp.py - head.py, hp.pz - head.pz};
        float dLocal[3];
        quat_rotate(hc[0], hc[1], hc[2], hc[3], dxr, dLocal);
        float ue[3];
        xr_to_ue(dLocal, ue);

        float qRel[4];
        quat_mul(hc, q, qRel);
        const UeAngles a = ue_angles_from_xr_quat(qRel[0], qRel[1], qRel[2], qRel[3]);

        GamePose gp{};
        gp.loc.x = ue[0] * ctx.worldScale;
        gp.loc.y = ue[1] * ctx.worldScale;
        gp.loc.z = ue[2] * ctx.worldScale;
        gp.rot.pitch = static_cast<int32_t>(a.pitchRad * kRotUnitsPerRadian);
        gp.rot.yaw = static_cast<int32_t>(a.yawRad * kRotUnitsPerRadian);
        gp.rot.roll = static_cast<int32_t>(a.rollRad * kRotUnitsPerRadian);

        // Offsets in the FINAL TRIMMED basis - unchanged in meaning, just
        // expressed in the component's frame now.
        float fwd[3], right[3], up[3];
        ue_rot_basis(gp.rot, fwd, right, up);
        const float k = ctx.worldScale / 100.0f;
        const float oF = g_offFwdCm[h].load(std::memory_order_relaxed) * k;
        const float oR = g_offRightCm[h].load(std::memory_order_relaxed) * k;
        const float oU = g_offUpCm[h].load(std::memory_order_relaxed) * k;
        gp.loc.x += fwd[0] * oF + right[0] * oR + up[0] * oU;
        gp.loc.y += fwd[1] * oF + right[1] * oR + up[1] * oU;
        gp.loc.z += fwd[2] * oF + right[2] * oR + up[2] * oU;

        // ALWAYS stored, written or not.
        g_lastTarget[h] = gp;
        g_haveTarget[h] = true;

        if (rig::drive(h, gp)) {
            g_wasDriving[h].store(true, std::memory_order_relaxed);
            g_frames[h].fetch_add(1, std::memory_order_relaxed);
        }
    }
}

} // namespace

void on_draw_entry() {
    if (rig::write_point() != rig::WritePoint::DrawEntry) return;
    rig::note_hit(rig::WritePoint::DrawEntry);
    tick("drawentry");
}
void on_draw_exit() {
    if (rig::write_point() != rig::WritePoint::DrawExit) return;
    rig::note_hit(rig::WritePoint::DrawExit);
    tick("drawexit");
}
void on_camera_tail() {
    if (rig::write_point() != rig::WritePoint::CameraTail) return;
    rig::note_hit(rig::WritePoint::CameraTail);
    tick("cameratail");
}
void on_second_pass() { rig::reapply(); }

bool enabled() { return g_enabled.load(std::memory_order_relaxed); }
bool use_aim_pose() { return g_useAimPose.load(std::memory_order_relaxed); }
void set_use_aim_pose(bool on) { g_useAimPose.store(on, std::memory_order_relaxed); }

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
        rig::warm_names();
        BVR_LOG("[bsi] hands ON - targets are computed every frame. The WRITE is still "
                "gated by `bsihands arm` and `bsihands probe off`.");
        return true;
    }
    if (is_verb(args, "off")) {
        g_enabled.store(false, std::memory_order_relaxed);
        rig::release("bsihands off");
        BVR_LOG("[bsi] hands off");
        return true;
    }
    if (is_verb(args, "status") || *args == '\0') {
        BVR_LOG("[bsi] hands: %s | pose %s",
                g_enabled.load(std::memory_order_relaxed) ? "ON" : "off",
                g_useAimPose.load(std::memory_order_relaxed) ? "aim" : "grip");
        FrameContext ctx{};
        uint32_t pub = 0, ret = 0, ref = 0, foreign = 0;
        frame_context::stats(&pub, &ret, &ref, &foreign);
        if (frame_context::read(&ctx, 0)) {
            BVR_LOG("[bsi] ctx: driving=%d recenter=%d gameYaw=%d recenterYaw=%d "
                    "worldScale=%.1f base=(%.1f %.1f %.1f) age %llu ms",
                    ctx.vrDriving ? 1 : 0, ctx.haveRecenter ? 1 : 0, ctx.gameYawUnits,
                    ctx.recenterYawUnits, ctx.worldScale, ctx.baseX, ctx.baseY, ctx.baseZ,
                    static_cast<unsigned long long>(GetTickCount64() - ctx.stamp));
        } else {
            BVR_LOG("[bsi] ctx: UNAVAILABLE (drive off, no recenter, or stale)");
        }
        BVR_LOG("[bsi] ctx: seqlock publishes=%u retries=%u refusals=%u foreignWrites=%u",
                pub, ret, ref, foreign);
        for (int h = 0; h < 2; ++h) {
            BVR_LOG("[bsi]   %s: frames %u trim (%.2f %.2f %.2f) offset (%.1f %.1f %.1f) cm "
                    "scale %.2f",
                    h ? "R" : "L", g_frames[h].load(std::memory_order_relaxed),
                    trim_pitch(h), trim_yaw(h), trim_roll(h), off_fwd_cm(h),
                    off_right_cm(h), off_up_cm(h), rig::scale_of(h));
            if (g_haveTarget[h])
                BVR_LOG("[bsi]     TARGET loc=(%.1f %.1f %.1f) rot=(%d %d %d)%s",
                        g_lastTarget[h].loc.x, g_lastTarget[h].loc.y, g_lastTarget[h].loc.z,
                        g_lastTarget[h].rot.pitch, g_lastTarget[h].rot.yaw,
                        g_lastTarget[h].rot.roll,
                        h == 0 ? "  -- NOT WRITTEN: there is ONE FP attachment actor and it "
                                 "is the right hand's. The second-carrier question is OPEN; "
                                 "see rig.h."
                               : "");
            else
                BVR_LOG("[bsi]     no target (hands off, no context, or no hand pose)");
        }
        rig::status_log();
        return true;
    }
    if (sscanf_s(args, "trim %7s %f %f %f", side, (unsigned)sizeof side, &a, &b, &c) == 4) {
        const int h = hand_arg(side);
        set_trim(h, a, b, c);
        BVR_LOG("[bsi] hands: trim %s %.2f %.2f %.2f deg (MODEL only - the ray wears "
                "bsiaim trim)",
                h ? "r" : "l", a, b, c);
        return true;
    }
    if (sscanf_s(args, "offset %7s %f %f %f", side, (unsigned)sizeof side, &a, &b, &c) == 4) {
        const int h = hand_arg(side);
        set_offset(h, a, b, c);
        BVR_LOG("[bsi] hands: offset %s %.1f %.1f %.1f cm (final trimmed basis)",
                h ? "r" : "l", a, b, c);
        return true;
    }
    if (strncmp(args, "scale", 5) == 0) {
        if (sscanf_s(args, "scale %7s %f", side, (unsigned)sizeof side, &a) == 2 &&
            (side[0] == 'l' || side[0] == 'L' || side[0] == 'r' || side[0] == 'R')) {
            const int h = hand_arg(side);
            rig::set_scale(h, a);
            BVR_LOG("[bsi] hands: scale %s %.2f", h ? "r" : "l", a);
        } else if (sscanf_s(args, "scale %f", &a) == 1) {
            rig::set_scale(0, a);
            rig::set_scale(1, a);
            BVR_LOG("[bsi] hands: scale %.2f (both hands, decoupled from worldscale)", a);
        } else {
            BVR_LOG("[bsi] hands: scale [l|r] <factor>");
        }
        return true;
    }
    if (is_verb(args, "pose")) {
        set_use_aim_pose(strstr(args, "grip") == nullptr);
        BVR_LOG("[bsi] hands: pose %s (SHARED with the aim ray - they can never differ)",
                use_aim_pose() ? "aim" : "grip");
        return true;
    }
    if (strncmp(args, "arms", 4) == 0) {
        const int mode = strstr(args, "hide") ? 2 : strstr(args, "game") ? 0 : 1;
        rig::set_arms_mode(mode);
        return true;
    }
    if (rig::handle_command(args)) return true;

    BVR_LOG("[bsi] bsihands: on|off | status | trim l|r <p> <y> <r> | offset l|r <f> <r> <u> "
            "| scale [l|r] <f> | pose aim|grip | arms game|follow|hide | probe on|off | "
            "arm on|off | point off|drawentry|drawexit|cameratail | listdump | warm | reset");
    return true;
}

void draw_debug_ui() {
    if (!ImGui::CollapsingHeader("HANDS + WEAPON (I8)  <-- CALIBRATE HERE",
                                 ImGuiTreeNodeFlags_DefaultOpen))
        return;
    bool on = g_enabled.load(std::memory_order_relaxed);
    if (ImGui::Checkbox("Compute hand targets every frame", &on)) {
        g_enabled.store(on, std::memory_order_relaxed);
        if (on) rig::warm_names();
        else rig::release("F10 off");
    }
    bool armed = rig::armed();
    if (ImGui::Checkbox("ARM the viewmodel write", &armed)) {
        rig::set_armed(armed);
        if (!armed) rig::release("F10 disarm");
    }
    bool probe = rig::probe();
    if (ImGui::Checkbox("Probe only (compute and count, write nothing)", &probe))
        rig::set_probe(probe);

    int pt = static_cast<int>(rig::write_point());
    ImGui::TextUnformatted("Write point:");
    ImGui::SameLine();
    if (ImGui::RadioButton("off", &pt, 0)) rig::set_write_point(rig::WritePoint::Off);
    ImGui::SameLine();
    if (ImGui::RadioButton("draw entry", &pt, 1))
        rig::set_write_point(rig::WritePoint::DrawEntry);
    ImGui::SameLine();
    if (ImGui::RadioButton("draw exit", &pt, 2))
        rig::set_write_point(rig::WritePoint::DrawExit);
    ImGui::SameLine();
    if (ImGui::RadioButton("camera tail", &pt, 3))
        rig::set_write_point(rig::WritePoint::CameraTail);

    static int tuneHand = 1;
    ImGui::Separator();
    ImGui::RadioButton("L (vigor)", &tuneHand, 0);
    ImGui::SameLine();
    ImGui::RadioButton("R (weapon)", &tuneHand, 1);
    if (tuneHand == 0)
        ImGui::TextDisabled("The left hand's target is computed but NOT written: there is "
                            "one FP attachment actor and it is the right hand's.");

    float p = trim_pitch(tuneHand), y = trim_yaw(tuneHand), r = trim_roll(tuneHand);
    bool ch = false;
    ch |= ImGui::SliderFloat("model trim pitch (deg)", &p, -180.0f, 180.0f);
    ch |= ImGui::SliderFloat("model trim yaw (deg)", &y, -180.0f, 180.0f);
    ch |= ImGui::SliderFloat("model trim roll (deg)", &r, -180.0f, 180.0f);
    if (ch) set_trim(tuneHand, p, y, r);

    float of = off_fwd_cm(tuneHand), orr = off_right_cm(tuneHand), ou = off_up_cm(tuneHand);
    bool oc = false;
    oc |= ImGui::SliderFloat("model offset fwd (cm)", &of, -60.0f, 60.0f);
    oc |= ImGui::SliderFloat("model offset right (cm)", &orr, -60.0f, 60.0f);
    oc |= ImGui::SliderFloat("model offset up (cm)", &ou, -60.0f, 60.0f);
    if (oc) set_offset(tuneHand, of, orr, ou);

    float sc = rig::scale_of(tuneHand);
    if (ImGui::SliderFloat("model SCALE (independent of worldscale)", &sc, 0.2f, 4.0f))
        rig::set_scale(tuneHand, sc);
    if (ImGui::Button("scale both hands to this")) {
        rig::set_scale(0, sc);
        rig::set_scale(1, sc);
    }
    float ws = rig::weapon_scale();
    if (ImGui::SliderFloat("WEAPON scale (uniform)", &ws, 0.3f, 2.5f)) rig::set_weapon_scale(ws);

    ImGui::Separator();
    ImGui::TextUnformatted("AIM ray (this hand) - where the laser and bullets go:");
    float ap = aim::trim_pitch(tuneHand), ay = aim::trim_yaw(tuneHand);
    bool ac = false;
    ac |= ImGui::SliderFloat("aim trim pitch (deg)", &ap, -30.0f, 30.0f);
    ac |= ImGui::SliderFloat("aim trim yaw (deg)", &ay, -30.0f, 30.0f);
    if (ac) aim::set_trim(tuneHand, ap, ay);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("There is no aim ROLL slider on purpose: roll is innermost in the "
                          "trim compose, so it cannot steer a ray.");

    // The ray-origin sliders move the BEAM and the DOT but not the BULLET,
    // because GetWeaponStartTraceLocation is not substituted yet. Shipping them
    // live would make the beam lie, so they are visibly disabled with the reason
    // on screen rather than quietly absent.
    ImGui::BeginDisabled(true);
    float rf = aim::pos_fwd_cm(tuneHand), rr = aim::pos_right_cm(tuneHand),
          ru = aim::pos_up_cm(tuneHand);
    ImGui::SliderFloat("ray origin fwd (cm)", &rf, -60.0f, 60.0f);
    ImGui::SliderFloat("ray origin right (cm)", &rr, -60.0f, 60.0f);
    ImGui::SliderFloat("ray origin up (cm)", &ru, -60.0f, 60.0f);
    bool bulletsFromHand = false;
    ImGui::Checkbox("bullets from the HAND (origin substitution)", &bulletsFromHand);
    ImGui::EndDisabled();
    ImGui::TextDisabled("Ray origin is locked until the GetWeaponStartTraceLocation seam\n"
                        "lands: it would move the beam and the dot but NOT the bullet,\n"
                        "and a beam that lies is worse than no slider.");

    float dd = aim::dot_dist_m();
    if (ImGui::SliderFloat("aim dot / beam length (m)", &dd, 0.5f, 15.0f))
        aim::set_dot_dist_m(dd);
    ImGui::TextUnformatted("laser:");
    ImGui::SameLine();
    bool lzL = aim::laser_hand(0), lzR = aim::laser_hand(1);
    if (ImGui::Checkbox("L##laser", &lzL)) aim::set_laser_hand(0, lzL);
    ImGui::SameLine();
    if (ImGui::Checkbox("R##laser", &lzR)) aim::set_laser_hand(1, lzR);
    ImGui::SameLine();
    ImGui::TextUnformatted("  aim dot:");
    ImGui::SameLine();
    bool dtL = aim::dot_hand(0), dtR = aim::dot_hand(1);
    if (ImGui::Checkbox("L##dot", &dtL)) aim::set_dot_hand(0, dtL);
    ImGui::SameLine();
    if (ImGui::Checkbox("R##dot", &dtR)) aim::set_dot_hand(1, dtR);

    ImGui::Separator();
    int arms = rig::arms_mode();
    ImGui::TextUnformatted("Arms:");
    ImGui::SameLine();
    if (ImGui::RadioButton("follow", &arms, 1)) rig::set_arms_mode(1);
    ImGui::SameLine();
    if (ImGui::RadioButton("hide", &arms, 2)) rig::set_arms_mode(2);
    ImGui::SameLine();
    if (ImGui::RadioButton("game", &arms, 0)) rig::set_arms_mode(0);
    ImGui::TextDisabled("Arms mode is stored but NOT applied: the first-person mesh\n"
                        "component is not identified. R1 ruled out both pawn meshes.");

    bool aimPose = use_aim_pose();
    if (ImGui::Checkbox("model and ray both on the AIM pose", &aimPose))
        set_use_aim_pose(aimPose);
}

} // namespace bvr::bsi::hands
