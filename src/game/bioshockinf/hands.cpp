#include "game/bioshockinf/hands.h"

#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstring>

#include "core/util/log.h"
#include "core/vr/openxr_runtime.h"
#include "game/bioshockinf/bones.h"

#include "imgui.h"

namespace bvr::bsi::hands {
namespace {

// Policy state. Atomics: sliders write from the render thread, the drive
// reads on the game thread. Defaults: everything 0/1.0 (the rig is placed
// raw and the F10 sliders calibrate from there - no BS2 number transfers),
// arms FOLLOW (the user's BS2 verdict), anim ON (adopt-then-compose), pose
// family AIM (the model must ride the SAME family as the ray - BS1's first
// headset failure was model on grip with the laser on aim).
std::atomic<bool> g_on{true};
std::atomic<float> g_trim[2][3] = {};   // [hand][pitch/yaw/roll] deg
std::atomic<float> g_offCm[2][3] = {};  // [hand][fwd/right/up] cm
std::atomic<float> g_scale[2] = {{1.0f}, {1.0f}};
std::atomic<int> g_armsMode{1};
std::atomic<bool> g_animMode{true};
std::atomic<bool> g_useAimPose{true};
std::atomic<bool> g_wasDriving[2] = {};
uint32_t g_frames[2] = {};
// s46: arms-hide wrist-cap style (0 collapse-at-grip / 1 keep forearm twist /
// 2 collapse behind the wrist) and the per-hand ARM-RELATIVE wrist adjustment
// (deg, arm chain only, about the grip - headset finding 5's lever). Neither
// is persisted this session: the wrist trim defaults 0 = inert, and a preset
// key earns its place only after the headset says the lever earns its keep.
std::atomic<int> g_hideStyle{0};
std::atomic<float> g_wristDeg[2][3] = {};

bool is_verb(const char* args, const char* verb) {
    const size_t n = strlen(verb);
    if (strncmp(args, verb, n) != 0) return false;
    const char c = args[n];
    return c == '\0' || c == ' ' || c == '\n' || c == '\r' || c == '\t';
}

int hand_arg(const char* s) {
    return (*s == 'l' || *s == 'L') ? 0 : 1;
}

} // namespace

void on_view(const FrameContext& fc, uint64_t nowMs) {
    (void)nowMs;
    const bool want = g_on.load(std::memory_order_relaxed) && fc.valid;
    const bool useAim = g_useAimPose.load(std::memory_order_relaxed);
    const int armsMode = g_armsMode.load(std::memory_order_relaxed);
    const bool animMode = g_animMode.load(std::memory_order_relaxed);
    for (int h = 0; h < 2; ++h) {
        bvr::vr::HeadPose hp{};
        if (!want || !bvr::vr::get_hand_pose(h, useAim, hp)) {
            // Edge-triggered release: a left-hand loss must never disturb the
            // right hand's live drive.
            if (g_wasDriving[h].exchange(false, std::memory_order_relaxed))
                bones::release(want ? "hand untracked" : "gate closed", h);
            continue;
        }
        const float pos[3] = {hp.px, hp.py, hp.pz};
        const float quat[4] = {hp.qx, hp.qy, hp.qz, hp.qw};
        GamePose gp = model_pose_from_xr(
            fc, pos, quat, g_trim[h][0].load(std::memory_order_relaxed),
            g_trim[h][1].load(std::memory_order_relaxed),
            g_trim[h][2].load(std::memory_order_relaxed));
        // Grip offset in the FINAL trimmed basis, cm -> UU by worldScale/100.
        const float cmToUu = fc.worldScale / 100.0f;
        const float f = g_offCm[h][0].load(std::memory_order_relaxed) * cmToUu;
        const float r = g_offCm[h][1].load(std::memory_order_relaxed) * cmToUu;
        const float u = g_offCm[h][2].load(std::memory_order_relaxed) * cmToUu;
        if (f != 0.0f || r != 0.0f || u != 0.0f) {
            float fwd[3], right[3], up[3];
            ue_rot_basis(gp.rot, fwd, right, up);
            gp.loc.x += fwd[0] * f + right[0] * r + up[0] * u;
            gp.loc.y += fwd[1] * f + right[1] * r + up[1] * u;
            gp.loc.z += fwd[2] * f + right[2] * r + up[2] * u;
        }
        const float wristDeg[3] = {g_wristDeg[h][0].load(std::memory_order_relaxed),
                                   g_wristDeg[h][1].load(std::memory_order_relaxed),
                                   g_wristDeg[h][2].load(std::memory_order_relaxed)};
        if (bones::drive(fc, gp, h, g_scale[h].load(std::memory_order_relaxed), armsMode,
                         animMode, g_hideStyle.load(std::memory_order_relaxed), wristDeg)) {
            g_wasDriving[h].store(true, std::memory_order_relaxed);
            ++g_frames[h];
        }
    }
}

// ---- preset plumbing ---------------------------------------------------------
float trim_get(int hand, int axis) { return g_trim[hand & 1][axis % 3]; }
void trim_set(int hand, int axis, float v) { g_trim[hand & 1][axis % 3] = v; }
float offset_get(int hand, int axis) { return g_offCm[hand & 1][axis % 3]; }
void offset_set(int hand, int axis, float v) { g_offCm[hand & 1][axis % 3] = v; }
float scale_get(int hand) { return g_scale[hand & 1]; }
void scale_set(int hand, float v) {
    if (v > 0.05f && v < 20.0f) g_scale[hand & 1] = v;
}
bool enabled() { return g_on.load(std::memory_order_relaxed); }
void set_enabled(bool on) {
    g_on.store(on, std::memory_order_relaxed);
    if (!on) bones::release("hands off", -1);
}
int arms_mode() { return g_armsMode.load(std::memory_order_relaxed); }
void set_arms_mode(int m) {
    if (m >= 0 && m <= 2) g_armsMode.store(m, std::memory_order_relaxed);
}
bool anim_mode() { return g_animMode.load(std::memory_order_relaxed); }
void set_anim_mode(bool on) { g_animMode.store(on, std::memory_order_relaxed); }

bool handle_command(const char* cmd, const char* args) {
    if (strcmp(cmd, "bsihands") != 0) return false;
    while (args && *args == ' ') ++args;
    if (!args || !*args || is_verb(args, "status")) {
        BVR_LOG("[bsi] hands: %s pose=%s arms=%d anim=%d | L trim(%.1f %.1f %.1f) "
                "off(%.1f %.1f %.1f) scale %.3f frames %u | R trim(%.1f %.1f %.1f) "
                "off(%.1f %.1f %.1f) scale %.3f frames %u",
                g_on.load() ? "ON" : "off", g_useAimPose.load() ? "aim" : "grip",
                g_armsMode.load(), (int)g_animMode.load(), g_trim[0][0].load(),
                g_trim[0][1].load(), g_trim[0][2].load(), g_offCm[0][0].load(),
                g_offCm[0][1].load(), g_offCm[0][2].load(), g_scale[0].load(), g_frames[0],
                g_trim[1][0].load(), g_trim[1][1].load(), g_trim[1][2].load(),
                g_offCm[1][0].load(), g_offCm[1][1].load(), g_offCm[1][2].load(),
                g_scale[1].load(), g_frames[1]);
        for (int h = 0; h < 2; ++h) {
            FVector loc;
            FRotator rot;
            uint32_t drives = 0, adopts = 0;
            if (bones::last_write(h, loc, rot, drives, adopts))
                BVR_LOG("[bsi] hands:   %c last write loc=(%.1f %.1f %.1f) "
                        "rot=(%d %d %d) drives=%u adopts=%u",
                        h ? 'R' : 'L', loc.x, loc.y, loc.z, rot.pitch, rot.yaw, rot.roll,
                        drives, adopts);
        }
        return true;
    }
    if (is_verb(args, "on")) {
        set_enabled(true);
        BVR_LOG("[bsi] hands: ON");
        return true;
    }
    if (is_verb(args, "off")) {
        set_enabled(false);
        BVR_LOG("[bsi] hands: off (released)");
        return true;
    }
    if (is_verb(args, "pose")) {
        g_useAimPose.store(strstr(args, "grip") == nullptr, std::memory_order_relaxed);
        BVR_LOG("[bsi] hands: pose family = %s (the ray uses aim; grip is the A/B)",
                g_useAimPose.load() ? "aim" : "grip");
        return true;
    }
    if (is_verb(args, "arms")) {
        int m = strstr(args, "hide") ? 2 : (strstr(args, "game") ? 0 : 1);
        set_arms_mode(m);
        BVR_LOG("[bsi] hands: arms mode %d (0 game / 1 follow / 2 hide)", m);
        return true;
    }
    if (is_verb(args, "animtrans")) {
        BVR_LOG("[bsi] hands: animtrans is not implemented on this game yet - the anchor "
                "glues to the controller (authored wrist travel rides the next session)");
        return true;
    }
    if (is_verb(args, "anim")) {
        set_anim_mode(strstr(args, "off") == nullptr);
        BVR_LOG("[bsi] hands: anim %s (off = rigid snapshot, the rest-oracle mode)",
                g_animMode.load() ? "ON (adopt-then-compose)" : "OFF");
        return true;
    }
    if (is_verb(args, "trim")) {
        char hs[4] = {};
        float p = 0, y = 0, r = 0;
        if (sscanf_s(args + 4, " %3s %f %f %f", hs, 4u, &p, &y, &r) == 4) {
            const int h = hand_arg(hs);
            g_trim[h][0] = p;
            g_trim[h][1] = y;
            g_trim[h][2] = r;
            BVR_LOG("[bsi] hands: trim %c = %.2f %.2f %.2f deg", h ? 'R' : 'L', p, y, r);
        } else {
            BVR_LOG("[bsi] hands: usage - bsihands trim l|r <pitch> <yaw> <roll>");
        }
        return true;
    }
    if (is_verb(args, "offset")) {
        char hs[4] = {};
        float f = 0, r = 0, u = 0;
        if (sscanf_s(args + 6, " %3s %f %f %f", hs, 4u, &f, &r, &u) == 4) {
            const int h = hand_arg(hs);
            g_offCm[h][0] = f;
            g_offCm[h][1] = r;
            g_offCm[h][2] = u;
            BVR_LOG("[bsi] hands: offset %c = %.1f %.1f %.1f cm", h ? 'R' : 'L', f, r, u);
        } else {
            BVR_LOG("[bsi] hands: usage - bsihands offset l|r <fwd> <right> <up>");
        }
        return true;
    }
    if (is_verb(args, "hidestyle")) {
        int s = -1;
        if (sscanf_s(args + 9, " %d", &s) == 1 && s >= 0 && s <= 2) {
            g_hideStyle.store(s, std::memory_order_relaxed);
            BVR_LOG("[bsi] hands: hide style %d (0 collapse-at-grip / 1 keep forearm "
                    "twist / 2 collapse behind the wrist) - visible with arms=hide only",
                    s);
        } else {
            BVR_LOG("[bsi] hands: usage - bsihands hidestyle 0|1|2");
        }
        return true;
    }
    if (is_verb(args, "wrist")) {
        char hs[4] = {};
        float p = 0, y = 0, r = 0;
        if (sscanf_s(args + 5, " %3s %f %f %f", hs, 4u, &p, &y, &r) == 4) {
            const int h = hand_arg(hs);
            g_wristDeg[h][0] = p;
            g_wristDeg[h][1] = y;
            g_wristDeg[h][2] = r;
            BVR_LOG("[bsi] hands: wrist %c = %.1f %.1f %.1f deg (ARM chain only, about "
                    "the grip - hand/aim/laser untouched)",
                    h ? 'R' : 'L', p, y, r);
        } else {
            BVR_LOG("[bsi] hands: usage - bsihands wrist l|r <pitch> <yaw> <roll>");
        }
        return true;
    }
    if (is_verb(args, "scale")) {
        char hs[4] = {};
        float v = 0;
        const int n = sscanf_s(args + 5, " %3s %f", hs, 4u, &v);
        if (n == 2) {
            scale_set(hand_arg(hs), v);
            BVR_LOG("[bsi] hands: scale %c = %.3f", hand_arg(hs) ? 'R' : 'L', v);
        } else if (n == 1 && sscanf_s(args + 5, " %f", &v) == 1) {
            scale_set(0, v);
            scale_set(1, v);
            BVR_LOG("[bsi] hands: scale BOTH = %.3f", v);
        } else {
            BVR_LOG("[bsi] hands: usage - bsihands scale [l|r] <factor>");
        }
        return true;
    }
    BVR_LOG("[bsi] hands: verbs - on off status pose aim|grip arms game|follow|hide "
            "anim on|off trim l|r <p y r> offset l|r <f r u> scale [l|r] <v> "
            "hidestyle 0|1|2 wrist l|r <p y r>");
    return true;
}

void draw_debug_ui() {
    if (!ImGui::CollapsingHeader("HANDS + MODEL (I8)  <-- CALIBRATE HERE",
                                 ImGuiTreeNodeFlags_DefaultOpen))
        return;
    // Anything judged BY EYE gets a control here, never a console command
    // (alt-tabbing to type destabilises the XR session). BS1's convention:
    // ONE slider set plus a tuning-hand radio, sliders write atomics directly.
    bool on = g_on.load(std::memory_order_relaxed);
    if (ImGui::Checkbox("drive hands/weapon from the controllers", &on)) set_enabled(on);
    static int tuneHand = 1; // start on the weapon hand
    ImGui::RadioButton("L (vigor)", &tuneHand, 0);
    ImGui::SameLine();
    ImGui::RadioButton("R (weapon)", &tuneHand, 1);

    float v;
    const char* axesT[3] = {"model trim pitch (deg)", "model trim yaw (deg)",
                            "model trim roll (deg)"};
    for (int a = 0; a < 3; ++a) {
        v = g_trim[tuneHand][a].load(std::memory_order_relaxed);
        if (ImGui::SliderFloat(axesT[a], &v, -180.0f, 180.0f)) g_trim[tuneHand][a] = v;
    }
    const char* axesO[3] = {"model offset fwd (cm)", "model offset right (cm)",
                            "model offset up (cm)"};
    for (int a = 0; a < 3; ++a) {
        v = g_offCm[tuneHand][a].load(std::memory_order_relaxed);
        if (ImGui::SliderFloat(axesO[a], &v, -60.0f, 60.0f)) g_offCm[tuneHand][a] = v;
    }
    v = g_scale[tuneHand].load(std::memory_order_relaxed);
    if (ImGui::SliderFloat("model scale (x, independent of worldscale)", &v, 0.2f, 4.0f))
        scale_set(tuneHand, v);
    if (ImGui::Button("scale both hands to this")) {
        scale_set(0, v);
        scale_set(1, v);
    }

    // s46 (headset finding 5): the ARM-RELATIVE wrist adjustment - rotates the
    // arm chain about the grip, hand/aim/laser untouched. Rides the tuning-hand
    // radio like every other per-hand slider. Zero = inert.
    const char* axesW[3] = {"wrist pitch (deg, arm vs hand)", "wrist yaw (deg, arm vs hand)",
                            "wrist roll (deg, arm vs hand)"};
    for (int a = 0; a < 3; ++a) {
        v = g_wristDeg[tuneHand][a].load(std::memory_order_relaxed);
        if (ImGui::SliderFloat(axesW[a], &v, -60.0f, 60.0f)) g_wristDeg[tuneHand][a] = v;
    }

    ImGui::Separator();
    int arms = g_armsMode.load(std::memory_order_relaxed);
    ImGui::Text("arms:");
    ImGui::SameLine();
    if (ImGui::RadioButton("follow the hands", &arms, 1)) set_arms_mode(arms);
    ImGui::SameLine();
    if (ImGui::RadioButton("hide", &arms, 2)) set_arms_mode(arms);
    ImGui::SameLine();
    if (ImGui::RadioButton("game", &arms, 0)) set_arms_mode(arms);
    // s46 (headset finding 4): the wrist-cap style, meaningful with arms=hide.
    if (arms == 2) {
        int hs = g_hideStyle.load(std::memory_order_relaxed);
        ImGui::Text("wrist cap:");
        ImGui::SameLine();
        if (ImGui::RadioButton("pinch at grip", &hs, 0))
            g_hideStyle.store(hs, std::memory_order_relaxed);
        ImGui::SameLine();
        if (ImGui::RadioButton("keep forearm twist", &hs, 1))
            g_hideStyle.store(hs, std::memory_order_relaxed);
        ImGui::SameLine();
        if (ImGui::RadioButton("pinch behind wrist", &hs, 2))
            g_hideStyle.store(hs, std::memory_order_relaxed);
    }

    bool anim = g_animMode.load(std::memory_order_relaxed);
    if (ImGui::Checkbox("engine animations on driven hands (fire/reload)", &anim))
        set_anim_mode(anim);
    bool aim = g_useAimPose.load(std::memory_order_relaxed);
    if (ImGui::Checkbox("aim-pose family (off = grip, A/B only)", &aim))
        g_useAimPose.store(aim, std::memory_order_relaxed);

    bones::draw_debug_ui();
}

} // namespace bvr::bsi::hands
