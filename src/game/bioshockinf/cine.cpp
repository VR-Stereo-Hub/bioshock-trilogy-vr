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

// s52 round 3: TWO hold flavors (user verdict "double hands on every scene
// with hand movement - door opens, the box handoff"):
// - CAMERA hold: an authored camera owns the view (Matinee view target or
//   the cinematic-mode bits). Full treatment - recenter on open, the head
//   radio applies, drives release.
// - SCRIPTED hold: the game locked player input (the IgnoreMoveInput/
//   IgnoreLookInput counters) while keeping the first-person camera - the
//   door grabs, the box handoff, every scripted FP hand animation. The HAND
//   drive and the aim/fire substitutions release so only the authored hands
//   show; the HEAD keeps driving and NO recenter fires (a one-second door
//   grab must not snap the world sideways).
// hold() = either (hands/aim/fire/chord consumers); camera_hold() = camera
// only (drive_view's radio + the open-edge recenter).
std::atomic<bool> g_camHold{false};
std::atomic<bool> g_scriptHold{false};
std::atomic<bool> g_force{false};
std::atomic<bool> g_headLook{true}; // the radio; persisted as cineHeadLook
uint64_t g_lastPollMs = 0;
char g_vtClass[64] = "";     // last view-target class (game thread; display)
bool g_cineModeBit = false;  // bCinematicMode || bInCinematicMode (display)
bool g_inputLocked = false;  // ignore-input counters nonzero (display)
bool g_vtMatinee = false;    // last view-target verdict (500 ms poll)
int g_matineeStreak = 0;     // hysteresis on the view-target leg only
int g_normalStreak = 0;
uint32_t g_edges = 0;
uint32_t g_polls = 0, g_pollFails = 0;

// One-shot property derivations on the PC (the s48b walker). Each leg can
// fail independently on this build; status shows what derived.
struct BoolBit {
    const char* name;
    uint32_t off = 0, mask = 0;
    int state = 0; // 0 underived, 1 derived, -1 unavailable
};
BoolBit g_bCine{"bCinematicMode"};
BoolBit g_bInCine{"bInCinematicMode"};
// s52 round 3 CORRECTIONS (both measured): "IgnoreMoveInput" resolves to a
// FUNCTION on this build (a null-class find_property_offset matched it and
// the leg read garbage - held open permanently), and the bIgnore* bool
// mirrors do not exist as properties at all. The working signal is the
// engine's own QUERY functions - IsMoveInputIgnored/IsLookInputIgnored -
// dispatched by cached index on the 500 ms lane (UBOOL return at parms+0).

void derive_props(void* pc) {
    for (BoolBit* b : {&g_bCine, &g_bInCine})
        if (b->state == 0)
            b->state = reflect::find_bool_property_bit(pc, b->name, &b->off, &b->mask)
                           ? 1
                           : -1;
    static bool logged = false;
    if (!logged) {
        logged = true;
        BVR_LOG("[bsi] cine: property legs - bCinematicMode %s, bInCinematicMode %s; "
                "input-lock via IsMoveInputIgnored/IsLookInputIgnored polls",
                g_bCine.state == 1 ? "DERIVED" : "unavailable",
                g_bInCine.state == 1 ? "DERIVED" : "unavailable");
    }
}

// True when either input-ignored query answers yes (0 on any refusal - the
// fail-safe is "no hold", never a stuck one).
bool poll_input_locked(void* pc) {
    static int32_t s_moveIdx = -1, s_lookIdx = -1;
    if (s_moveIdx < 0) s_moveIdx = reflect::find_function_index("IsMoveInputIgnored");
    if (s_lookIdx < 0) s_lookIdx = reflect::find_function_index("IsLookInputIgnored");
    for (int32_t idx : {s_moveIdx, s_lookIdx}) {
        if (idx < 0) continue;
        uint8_t parms[64] = {};
        if (!reflect::call_on_object_by_index(pc, idx, parms)) continue;
        int32_t ret = 0;
        memcpy(&ret, parms, sizeof ret);
        if (ret) return true;
    }
    return false;
}

bool read_bit(const void* pc, const BoolBit& b) {
    if (b.state != 1 || !bvr::pattern_scan::is_memory_valid(pc, b.off + 4)) return false;
    return (*reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>(pc) + b.off) &
            b.mask) != 0;
}


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

void sync_chord() {
    bvr::input::set_flourish_chord_suspended(g_camHold.load(std::memory_order_relaxed) ||
                                             g_scriptHold.load(std::memory_order_relaxed));
}

void apply_cam_hold(bool on, const char* why) {
    if (g_camHold.exchange(on, std::memory_order_relaxed) == on) return;
    ++g_edges;
    sync_chord();
    // s52 round 2 (headset-verified): a cutscene starts centered on wherever
    // the player is looking. CAMERA holds only - never the scripted ones.
    if (on) camera::request_recenter();
    BVR_LOG("[bsi] cine: CAMERA hold %s (%s) - drives release via their own ticks; "
            "head radio = %s%s",
            on ? "OPEN" : "closed", why,
            g_headLook.load(std::memory_order_relaxed) ? "head look" : "fixed head",
            on ? "; recentered on the current head" : "");
}

void apply_script_hold(bool on) {
    if (g_scriptHold.exchange(on, std::memory_order_relaxed) == on) return;
    ++g_edges;
    sync_chord();
    BVR_LOG("[bsi] cine: SCRIPTED hold %s (input %slocked) - hand/aim/fire release; "
            "head keeps driving, no recenter",
            on ? "OPEN" : "closed", on ? "" : "un");
}

} // namespace

void tick(uint64_t nowMs) {
    void* pc = camera::last_player_controller();
    if (!pc || !bvr::pattern_scan::is_memory_valid(pc, 0x400)) return;

    if (g_force.load(std::memory_order_relaxed)) {
        apply_cam_hold(true, "forced");
        return;
    }

    // Slow lane (500 ms): property derivations + the view-target ProcessEvent
    // poll. fname_find/property walks are one-shot; the poll dispatches by a
    // cached index (the s52 stutter rule).
    if (nowMs - g_lastPollMs >= 500) {
        g_lastPollMs = nowMs;
        derive_props(pc);
        g_inputLocked = poll_input_locked(pc);
        static int32_t s_gvtIdx = -1;
        if (s_gvtIdx < 0) s_gvtIdx = reflect::find_function_index("GetViewTarget");
        if (s_gvtIdx >= 0) {
            uint8_t parms[64] = {};
            if (reflect::call_on_object_by_index(pc, s_gvtIdx, parms)) {
                ++g_polls;
                void* vt = nullptr;
                memcpy(&vt, parms, sizeof vt); // no-arg: return at parms+0
                char cls[64] = {};
                if (vt && bvr::pattern_scan::is_memory_valid(vt, 0x30) &&
                    reflect::class_name_of(vt, cls, sizeof cls) && cls[0]) {
                    if (strcmp(cls, g_vtClass) != 0) {
                        BVR_LOG("[bsi] cine: view target class '%s' -> '%s'", g_vtClass,
                                cls);
                        strcpy_s(g_vtClass, cls);
                    }
                    if (is_cinematic_class(cls)) {
                        g_normalStreak = 0;
                        if (++g_matineeStreak >= 2) g_vtMatinee = true;
                    } else {
                        g_matineeStreak = 0;
                        if (++g_normalStreak >= 2) g_vtMatinee = false;
                    }
                } else {
                    ++g_pollFails;
                }
            } else {
                ++g_pollFails;
            }
        }
    }

    // Fast lane (every camera dispatch): the derived bits are plain gated
    // reads - cheap enough to sample at full rate, which is what catches a
    // one-second door grab the 500 ms poll plus hysteresis would miss.
    g_cineModeBit = read_bit(pc, g_bCine) || read_bit(pc, g_bInCine);
    // g_inputLocked refreshes on the 500 ms lane (a ProcessEvent query has
    // no business running per dispatch).

    const bool cam = g_vtMatinee || g_cineModeBit;
    apply_cam_hold(cam, g_cineModeBit ? "cinematic-mode bit" : g_vtClass);
    apply_script_hold(!cam && g_inputLocked);
}

bool hold() {
    return g_camHold.load(std::memory_order_relaxed) ||
           g_scriptHold.load(std::memory_order_relaxed);
}
bool camera_hold() { return g_camHold.load(std::memory_order_relaxed); }
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
            // Drop the forced hold immediately rather than waiting for clean
            // polls - the flat edge test measures THIS transition.
            g_matineeStreak = g_normalStreak = 0;
            g_vtMatinee = false;
            apply_cam_hold(false, "force off");
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
        BVR_LOG("[bsi] cine: cam hold %s%s, scripted hold %s, radio %s | view target "
                "'%s', cineBit=%d, inputLocked=%d | legs: bCine %s bInCine %s ignMove %s "
                "ignLook %s | polls %u fails %u edges %u | usage: bsicine status|"
                "force on|off|head look|fixed",
                camera_hold() ? "OPEN" : "closed",
                g_force.load(std::memory_order_relaxed) ? " (FORCED)" : "",
                g_scriptHold.load(std::memory_order_relaxed) ? "OPEN" : "closed",
                head_look() ? "head look" : "fixed head", g_vtClass,
                g_cineModeBit ? 1 : 0, g_inputLocked ? 1 : 0,
                g_bCine.state == 1 ? "ok" : "-", g_bInCine.state == 1 ? "ok" : "-",
                "fn", "fn", g_polls, g_pollFails, g_edges);
    }
    return true;
}

void draw_debug_ui() {
    if (!ImGui::CollapsingHeader("CINEMATICS (I9)")) return;
    ImGui::Text("camera hold %s   scripted hold %s", camera_hold() ? "OPEN" : "closed",
                g_scriptHold.load(std::memory_order_relaxed) ? "OPEN" : "closed");
    ImGui::Text("view target: %s", g_vtClass[0] ? g_vtClass : "-");
    int mode = head_look() ? 0 : 1;
    bool ch = ImGui::RadioButton("Head look during cinematics (additive)", &mode, 0);
    ch |= ImGui::RadioButton("Fixed head (authored camera untouched)", &mode, 1);
    if (ch) set_head_look(mode == 0);
    ImGui::TextDisabled("bsicine force on|off fakes a camera hold for testing");
}

} // namespace bvr::bsi::cine
