#include "game/bioshockinf/melee.h"

#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstring>

#include "core/input/xinput_bridge.h"
#include "core/util/log.h"
#include "game/bioshockinf/bones.h"
#include "game/bioshockinf/cine.h"

#include "imgui.h"

namespace bvr::bsi::melee {
namespace {

// XINPUT_GAMEPAD_Y (0x8000) without dragging in <Xinput.h> - the same manual
// constant style the composed-pad consumers already use.
constexpr uint16_t kPadY = 0x8000;

// A fire-seam dispatch this close after a Y edge is the melee chain (the
// chain execs on the press; the dispatch follows within a frame or two -
// measured pairs land in the same millisecond as the swing start).
constexpr uint64_t kYAssocMs = 600;
// Swing window: covers the ~0.5-1 s swing plus the hold-open latency of the
// execution detector (scripted-hold leg <= 500 ms, camera-hold leg ~1 s).
constexpr uint64_t kWindowMs = 1500;
// After the execution hold closes, policy resumes after this tail (the game
// restamps its own visibility around the handback).
constexpr uint64_t kExecTailMs = 300;
// s57b: how long the GUN hand stays bone-hidden after a melee dispatch -
// the swing itself (~0.6 s) plus a beat; shorter than the full window so
// the gun is back before the next input matters.
constexpr uint64_t kSwingHideMs = 900;

std::atomic<int> g_mode{1}; // 0 off (control) | 1 glueskip (default) | 2 release
std::atomic<bool> g_hideGun{true};       // s57b: hide the gun hand for the swing
std::atomic<uint64_t> g_hideUntilMs{0};
std::atomic<uint64_t> g_windowUntilMs{0};
std::atomic<uint64_t> g_testUntilMs{0}; // `bsimelee swing [ms]` - the pad-free flat lane
std::atomic<uint64_t> g_lastYDownMs{0};
std::atomic<bool> g_execRelease{false};
bool g_prevY = false;
bool g_execHold = false;         // a hold opened while the window was live
uint64_t g_execTailUntilMs = 0;  // release lingers past the hold close
std::atomic<uint32_t> g_dispatches{0}, g_windows{0}, g_execReleases{0};
std::atomic<uint32_t> g_holdSkips{0}; // dispatches ignored because a hold was open

bool window_live(uint64_t nowMs) {
    return nowMs < g_windowUntilMs.load(std::memory_order_relaxed);
}

} // namespace

void tick(uint64_t nowMs) {
    // Y-edge tracking off the composed pad (updates whenever the game polls
    // XInput - dead only in the sim's pad-outage boots, where the test lane
    // `bsimelee swing` stands in).
    uint16_t buttons = 0;
    bvr::input::last_composed_buttons(&buttons);
    const bool y = (buttons & kPadY) != 0;
    if (y && !g_prevY) g_lastYDownMs.store(nowMs, std::memory_order_relaxed);
    g_prevY = y;

    // The execution: a hold OPENING while the melee window is live is the
    // melee-execution mini-cutscene (the press started it; boat/doors/raffle
    // holds open with no live window and never reach this branch).
    const bool hold = cine::hold();
    if (!g_execHold && hold && window_live(nowMs) &&
        g_mode.load(std::memory_order_relaxed) != 0) {
        g_execHold = true;
        g_execReleases.fetch_add(1, std::memory_order_relaxed);
        BVR_LOG("[bsi] melee: EXECUTION hold - hide gate released until the hold "
                "closes (+%llu ms tail)",
                static_cast<unsigned long long>(kExecTailMs));
    }
    if (g_execHold) {
        if (hold) {
            g_execTailUntilMs = nowMs + kExecTailMs;
        } else if (nowMs >= g_execTailUntilMs) {
            g_execHold = false;
            BVR_LOG("[bsi] melee: execution release closed - hide policy resumes");
        }
    }
    g_execRelease.store(g_execHold, std::memory_order_relaxed);
}

bool classify_dispatch(uint64_t nowMs, bool holdOpen) {
    if (g_mode.load(std::memory_order_relaxed) == 0) return false;
    if (holdOpen) {
        // The raffle-QTE rule: a press with the hold ALREADY open runs the
        // exact pre-s57 path (including the moot fireglue open).
        g_holdSkips.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    const bool test = nowMs < g_testUntilMs.load(std::memory_order_relaxed);
    const bool yRecent =
        nowMs - g_lastYDownMs.load(std::memory_order_relaxed) <= kYAssocMs;
    const bool live = window_live(nowMs);
    if (!test && !yRecent && !live) return false;
    if (!live) {
        g_windows.fetch_add(1, std::memory_order_relaxed);
        BVR_LOG("[bsi] melee: window OPEN (%s) - fireglue skipped/cancelled for "
                "%llu ms, empty-hand hides released",
                test ? "test lane" : "Y edge",
                static_cast<unsigned long long>(kWindowMs));
    }
    g_windowUntilMs.store(nowMs + kWindowMs, std::memory_order_relaxed);
    g_hideUntilMs.store(nowMs + kSwingHideMs, std::memory_order_relaxed);
    g_dispatches.fetch_add(1, std::memory_order_relaxed);
    return true;
}

int swing_hide_hand() {
    if (g_mode.load(std::memory_order_relaxed) == 0 ||
        !g_hideGun.load(std::memory_order_relaxed))
        return -1;
    if (g_execRelease.load(std::memory_order_relaxed)) return -1;
    return GetTickCount64() < g_hideUntilMs.load(std::memory_order_relaxed) ? 1 : -1;
}

bool hide_gun() { return g_hideGun.load(std::memory_order_relaxed); }
void set_hide_gun(bool on) {
    if (g_hideGun.exchange(on, std::memory_order_relaxed) != on)
        BVR_LOG("[bsi] melee: gun-hand swing hide %s", on ? "ON" : "off");
}

bool hide_release() {
    if (g_mode.load(std::memory_order_relaxed) == 0) return false;
    return window_live(GetTickCount64()) ||
           g_execRelease.load(std::memory_order_relaxed);
}

bool drive_release(int hand) {
    (void)hand; // both hands release symmetrically (the vigor arm swings too)
    if (g_mode.load(std::memory_order_relaxed) != 2) return false;
    return window_live(GetTickCount64());
}

int mode() { return g_mode.load(std::memory_order_relaxed); }
void set_mode(int m) {
    if (m < 0 || m > 2) return;
    if (g_mode.exchange(m, std::memory_order_relaxed) != m)
        BVR_LOG("[bsi] melee: mode %s",
                m == 0 ? "OFF (control - the broken pre-s57 behavior)"
                       : (m == 1 ? "GLUESKIP (default - drive stays, fireglue skipped)"
                                 : "RELEASE (drive releases for the window)"));
}

bool handle_command(const char* cmd, const char* args) {
    if (strcmp(cmd, "bsimelee") != 0) return false;
    if (!args) args = "";
    while (*args == ' ') ++args;
    if (strncmp(args, "mode ", 5) == 0) {
        const char* m = args + 5;
        while (*m == ' ') ++m;
        if (strncmp(m, "off", 3) == 0) set_mode(0);
        else if (strncmp(m, "glueskip", 8) == 0) set_mode(1);
        else if (strncmp(m, "release", 7) == 0) set_mode(2);
        else BVR_LOG("[bsi] melee: unknown mode - off|glueskip|release");
        return true;
    }
    if (strncmp(args, "hidegun", 7) == 0) {
        const char* h = args + 7;
        while (*h == ' ') ++h;
        if (strncmp(h, "on", 2) == 0) set_hide_gun(true);
        else if (strncmp(h, "off", 3) == 0) set_hide_gun(false);
        else BVR_LOG("[bsi] melee: hidegun %s | bsimelee hidegun on|off",
                     hide_gun() ? "ON" : "off");
        return true;
    }
    if (strncmp(args, "swing", 5) == 0) {
        // The flat test lane: classify the NEXT dispatches as melee without a
        // pad (the sim pad outage) - drive the swing with the V key.
        unsigned ms = 3000;
        sscanf_s(args + 5, "%u", &ms);
        if (ms > 30000) ms = 30000;
        g_testUntilMs.store(GetTickCount64() + ms, std::memory_order_relaxed);
        BVR_LOG("[bsi] melee: test lane ARMED for %u ms - fire-seam dispatches "
                "classify as melee",
                ms);
        return true;
    }
    const uint64_t now = GetTickCount64();
    BVR_LOG("[bsi] melee: mode=%s window=%s exec=%s | dispatches=%u windows=%u "
            "execReleases=%u holdSkips=%u | bsimelee mode off|glueskip|release | "
            "swing [ms]",
            g_mode.load(std::memory_order_relaxed) == 0
                ? "off"
                : (g_mode.load(std::memory_order_relaxed) == 1 ? "glueskip" : "release"),
            window_live(now) ? "LIVE" : "idle",
            g_execRelease.load(std::memory_order_relaxed) ? "RELEASED" : "-",
            g_dispatches.load(std::memory_order_relaxed),
            g_windows.load(std::memory_order_relaxed),
            g_execReleases.load(std::memory_order_relaxed),
            g_holdSkips.load(std::memory_order_relaxed));
    return true;
}

void draw_debug_ui() {
    int m = g_mode.load(std::memory_order_relaxed);
    ImGui::Text("MELEE window (s57):");
    ImGui::SameLine();
    if (ImGui::RadioButton("off##melee", m == 0)) set_mode(0);
    ImGui::SameLine();
    if (ImGui::RadioButton("glueskip##melee", m == 1)) set_mode(1);
    ImGui::SameLine();
    if (ImGui::RadioButton("release##melee", m == 2)) set_mode(2);
    bool hg = g_hideGun.load(std::memory_order_relaxed);
    if (ImGui::Checkbox("hide the gun hand during the swing (s57b)", &hg))
        set_hide_gun(hg);
    ImGui::Text("  windows %u  execution releases %u",
                g_windows.load(std::memory_order_relaxed),
                g_execReleases.load(std::memory_order_relaxed));
}

} // namespace bvr::bsi::melee
