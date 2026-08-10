#include "game/bioshockinf/cine.h"

#include "core/hooks/pattern_scan.h"
#include "core/input/xinput_bridge.h"
#include "core/util/log.h"
#include "game/bioshockinf/camera.h"
#include "game/bioshockinf/patterns.h"
#include "game/bioshockinf/reflect.h"

#include <imgui.h>
#include <windows.h>

#include <atomic>
#include <cstring>

namespace bvr::bsi::cine {
namespace {

std::atomic<bool> g_hold{false};
std::atomic<bool> g_force{false};
std::atomic<bool> g_headLook{true}; // the radio; persisted as cineHeadLook
uint64_t g_lastPollMs = 0;
char g_vtClass[64] = "";   // last view-target class (game thread; display)
bool g_cineModeBit = false; // last bCinematicMode read (display)
int g_matineeStreak = 0;   // hysteresis: 2 consecutive verdicts flip the hold
int g_normalStreak = 0;
uint32_t g_edges = 0;
uint32_t g_polls = 0, g_pollFails = 0;

// A view-target class that means "an authored cinematic owns the camera".
// The pawn (XHuman) and the player camera (XCamera) are gameplay; Matinee
// holds read as MatineeCamera (the FName exists in the shipped pool) or a
// CameraActor-family class. Substring match keeps DLC variants covered; the
// edge log prints the exact class so a miss is visible, not silent.
bool is_cinematic_class(const char* cls) {
    if (!cls[0]) return false;
    return strstr(cls, "Matinee") != nullptr || strstr(cls, "CameraActor") != nullptr ||
           strstr(cls, "Cinematic") != nullptr;
}

void apply_hold(bool on, const char* why) {
    if (g_hold.exchange(on, std::memory_order_relaxed) == on) return;
    ++g_edges;
    // The chord eats face-A while the left thumbrest is touched; during a
    // hold that press must reach the game (the raffle's "throw the ball"
    // prompt ate NOTHING in the stereo-only build - never again).
    bvr::input::set_flourish_chord_suspended(on);
    // s52 round 2 (headset verdict): a cutscene must START centered on
    // wherever the player is looking - re-baseline the head-look residual at
    // the open edge so the authored view (or the head-look compose on top of
    // it) begins at the current head direction, not the pre-scene body yaw.
    if (on) camera::request_recenter();
    BVR_LOG("[bsi] cine: hold %s (%s) - hands/aim/fire release via their own "
            "ticks; head radio = %s%s",
            on ? "OPEN" : "closed", why,
            g_headLook.load(std::memory_order_relaxed) ? "head look" : "fixed head",
            on ? "; recentered on the current head" : "");
}

} // namespace

void tick(uint64_t nowMs) {
    if (nowMs - g_lastPollMs < 500) return;
    g_lastPollMs = nowMs;
    if (g_force.load(std::memory_order_relaxed)) {
        apply_hold(true, "forced");
        return;
    }
    void* pc = camera::last_player_controller();
    if (!pc) return;

    // s52 round 2 (headset verdict: "I can see BOTH hands in most cutscenes
    // at the beginning of a new game"): the scripted FIRST-PERSON cutscenes
    // never repossess the camera - the view target stays the pawn, so the
    // Matinee leg below cannot see them. The engine-side signal for those is
    // APlayerController.bCinematicMode (Kismet's Toggle Cinematic Mode sets
    // it). One-shot property derivation (the s48b walker), then a plain
    // gated bit read per poll.
    static uint32_t s_cineOff = 0, s_cineMask = 0;
    static int s_cineState = 0; // 0 underived, 1 derived, -1 unavailable
    if (s_cineState == 0)
        s_cineState =
            reflect::find_bool_property_bit(pc, "bCinematicMode", &s_cineOff, &s_cineMask)
                ? 1
                : -1;
    bool cineModeBit = false;
    if (s_cineState == 1 &&
        bvr::pattern_scan::is_memory_valid(pc, s_cineOff + 4))
        cineModeBit = (*reinterpret_cast<const uint32_t*>(
                           static_cast<const uint8_t*>(pc) + s_cineOff) &
                       s_cineMask) != 0;

    // fname_find is a whole-pool linear scan - NEVER on a cadence (the s52
    // stutter: this poll at 500 ms scanned ~70k names per tick and hitched
    // the game 2-3 times a second). Resolve once, dispatch by index.
    static int32_t s_gvtIdx = -1;
    if (s_gvtIdx < 0) s_gvtIdx = reflect::find_function_index("GetViewTarget");
    if (s_gvtIdx < 0) return; // GNames not populated yet
    uint8_t parms[64] = {};
    if (!reflect::call_on_object_by_index(pc, s_gvtIdx, parms)) {
        ++g_pollFails;
        return; // gates closed (menu, load) - the hold keeps its last state;
                // the stop-writing property covers the silent stretch anyway
    }
    ++g_polls;
    // No-arg UFunction: the return lands at parms+0.
    void* vt = nullptr;
    memcpy(&vt, parms, sizeof vt);
    if (!vt || !bvr::pattern_scan::is_memory_valid(vt, 0x30)) {
        ++g_pollFails;
        return;
    }
    char cls[64] = {};
    if (!reflect::class_name_of(vt, cls, sizeof cls) || !cls[0]) {
        ++g_pollFails;
        return;
    }
    if (strcmp(cls, g_vtClass) != 0) {
        BVR_LOG("[bsi] cine: view target class '%s' -> '%s'", g_vtClass, cls);
        strcpy_s(g_vtClass, cls);
    }
    g_cineModeBit = cineModeBit;
    if (is_cinematic_class(cls) || cineModeBit) {
        g_normalStreak = 0;
        if (++g_matineeStreak >= 2)
            apply_hold(true, cineModeBit ? "bCinematicMode" : cls);
    } else {
        g_matineeStreak = 0;
        if (++g_normalStreak >= 2) apply_hold(false, cls);
    }
}

bool hold() { return g_hold.load(std::memory_order_relaxed); }
bool head_look() { return g_headLook.load(std::memory_order_relaxed); }
void set_head_look(bool on) {
    if (g_headLook.exchange(on, std::memory_order_relaxed) != on)
        BVR_LOG("[bsi] cine: head radio = %s", on ? "HEAD LOOK (additive)" : "FIXED HEAD");
}

bool handle_command(const char* cmd, const char* args) {
    if (strcmp(cmd, "bsicine") != 0) return false;
    if (!args) args = "";
    while (*args == ' ') ++args;
    auto tok = [&](const char* base, const char* w) {
        const size_t n = strlen(w);
        if (strncmp(base, w, n) != 0) return false;
        const char c = base[n];
        return c == '\0' || c == ' ' || c == '\n' || c == '\r' || c == '\t';
    };
    if (tok(args, "force")) {
        const char* rest = args + 5;
        while (*rest == ' ') ++rest;
        if (tok(rest, "on")) g_force.store(true, std::memory_order_relaxed);
        else if (tok(rest, "off")) {
            g_force.store(false, std::memory_order_relaxed);
            // Drop the forced hold immediately rather than waiting for two
            // clean polls - the flat edge test measures THIS transition.
            g_matineeStreak = g_normalStreak = 0;
            apply_hold(false, "force off");
        }
        BVR_LOG("[bsi] cine: force %s", g_force.load(std::memory_order_relaxed) ? "ON" : "off");
    } else if (tok(args, "head")) {
        const char* rest = args + 4;
        while (*rest == ' ') ++rest;
        if (tok(rest, "look")) set_head_look(true);
        else if (tok(rest, "fixed")) set_head_look(false);
        else
            BVR_LOG("[bsi] cine: head radio = %s (bsicine head look|fixed)",
                    head_look() ? "head look" : "fixed head");
    } else {
        BVR_LOG("[bsi] cine: hold %s%s, radio %s, view target '%s', bCinematicMode=%d | "
                "polls %u fails %u edges %u | usage: bsicine status|force on|off|"
                "head look|fixed",
                hold() ? "OPEN" : "closed",
                g_force.load(std::memory_order_relaxed) ? " (FORCED)" : "",
                head_look() ? "head look" : "fixed head", g_vtClass,
                g_cineModeBit ? 1 : 0, g_polls, g_pollFails, g_edges);
    }
    return true;
}

void draw_debug_ui() {
    if (!ImGui::CollapsingHeader("CINEMATICS (I9)")) return;
    ImGui::Text("hold %s   view target: %s", hold() ? "OPEN" : "closed",
                g_vtClass[0] ? g_vtClass : "-");
    int mode = head_look() ? 0 : 1;
    bool ch = ImGui::RadioButton("Head look during cinematics (additive)", &mode, 0);
    ch |= ImGui::RadioButton("Fixed head (authored camera untouched)", &mode, 1);
    if (ch) set_head_look(mode == 0);
    ImGui::TextDisabled("bsicine force on|off fakes a hold for testing");
}

} // namespace bvr::bsi::cine
