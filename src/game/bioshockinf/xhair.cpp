#include "game/bioshockinf/xhair.h"

#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstring>

#include "core/hooks/pattern_scan.h"
#include "core/util/log.h"
#include "game/bioshockinf/camera.h"
#include "game/bioshockinf/gfx.h"
#include "game/bioshockinf/reflect.h"

#include "imgui.h"

namespace bvr::bsi::xhair {
namespace {

using bvr::pattern_scan::is_memory_valid;

constexpr const char* kClassName = "XClikHUDCrosshair";
// Masks at the derived IsShown offset (both bools pack one dword; declaration
// order; verified live s57 - see the header). The OFFSET is derived by name
// per boot; only the sub-dword bit assignment is a constant.
constexpr uint32_t kMaskIsShown = 0x1;
constexpr uint32_t kMaskCenterpoint = 0x2;
constexpr uint32_t kMaskBoth = kMaskIsShown | kMaskCenterpoint;
constexpr int kMaxInst = 8;

std::atomic<bool> g_on{true};
void* g_inst[kMaxInst] = {};
uint32_t g_origBits[kMaxInst] = {}; // the bits each instance carried pre-clear
bool g_cleared[kMaxInst] = {};
int g_count = 0;
uint32_t g_isShownOff = 0;      // derived per boot; 0 = not derived yet
bool g_refused = false;         // permanent offset-derivation refusal this boot
uint64_t g_lastTickMs = 0;
uint64_t g_nextSweepMs = 0;
uint64_t g_sweepBackoffMs = 5000;
uint32_t g_sweeps = 0, g_reasserts = 0, g_clears = 0;

bool inst_alive(void* p) {
    if (!p || !is_memory_valid(p, 0x120)) return false;
    char cls[64] = {};
    return reflect::class_name_of(p, cls, sizeof cls) && strcmp(cls, kClassName) == 0;
}

uint32_t* bits_ptr(void* inst) {
    return reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(inst) + g_isShownOff);
}

void drop_all(const char* why) {
    if (g_count)
        BVR_LOG("[bsi] xhair: instances dropped (%s) - re-derive on the next sweep",
                why);
    g_count = 0;
    memset(g_cleared, 0, sizeof g_cleared);
}

// Restore the original bits on every still-alive cleared instance (lever off
// or lane shutdown). Skips dead instances silently.
void restore_all() {
    for (int i = 0; i < g_count; ++i) {
        if (!g_cleared[i] || !inst_alive(g_inst[i])) continue;
        uint32_t* d = bits_ptr(g_inst[i]);
        *d = (*d & ~kMaskBoth) | g_origBits[i];
        g_cleared[i] = false;
    }
}

void sweep(uint64_t nowMs) {
    ++g_sweeps;
    g_nextSweepMs = nowMs + g_sweepBackoffMs;
    if (g_sweepBackoffMs < 60000) g_sweepBackoffMs *= 2;
    void* found[kMaxInst] = {};
    const int n = gfx::find_instances(kClassName, found, kMaxInst);
    if (n <= 0) {
        BVR_LOG("[bsi] xhair: sweep %u found no live %s (%s) - retry in %llu s",
                g_sweeps, kClassName,
                n < 0 ? "prerequisites missing" : "none on this level",
                static_cast<unsigned long long>(g_sweepBackoffMs / 1000));
        return;
    }
    // Offset derivation, once per boot, BY NAME on a live instance - a wrong
    // class layout must refuse, never write (the s49b refusal pattern).
    if (g_isShownOff == 0) {
        uint32_t off = 0;
        if (!reflect::find_property_offset(found[0], "IsShown", "BoolProperty", &off) ||
            off == 0 || off > 0x1000) {
            g_refused = true;
            BVR_LOG("[bsi] xhair: REFUSED - IsShown did not derive on %p (lane off "
                    "this boot)",
                    found[0]);
            return;
        }
        g_isShownOff = off;
        BVR_LOG("[bsi] xhair: IsShown derived at +0x%X", off);
    }
    memcpy(g_inst, found, sizeof found);
    g_count = n;
    memset(g_cleared, 0, sizeof g_cleared);
    g_sweepBackoffMs = 5000; // fresh level, fresh budget
    BVR_LOG("[bsi] xhair: %d live instance(s) cached (sweep %u)", n, g_sweeps);
}

} // namespace

void tick(uint64_t nowMs) {
    if (g_refused) return;
    if (nowMs - g_lastTickMs < 1000) return;
    g_lastTickMs = nowMs;

    const bool on = g_on.load(std::memory_order_relaxed);

    // Cached instances must stay provably alive - a level swap frees them.
    for (int i = 0; i < g_count; ++i) {
        if (!inst_alive(g_inst[i])) {
            drop_all("instance died");
            break;
        }
    }

    if (!on) {
        restore_all();
        return;
    }

    if (g_count == 0) {
        // Only hunt in live gameplay (the sweep needs the reflection lane and
        // a real level), and only on the backoff schedule - the sweep is a
        // one-shot ~0.5 s game-thread hitch, never a cadence.
        if (camera::frame_context().valid && nowMs >= g_nextSweepMs) sweep(nowMs);
        return;
    }

    // Clear + slow re-assert watchdog (combat re-shows are the expected
    // fighter; flat observation saw none, headset combat will tell).
    for (int i = 0; i < g_count; ++i) {
        uint32_t* d = bits_ptr(g_inst[i]);
        const uint32_t bits = *d & kMaskBoth;
        if (!g_cleared[i]) {
            g_origBits[i] = bits;
            *d &= ~kMaskBoth;
            g_cleared[i] = true;
            ++g_clears;
        } else if (bits) {
            *d &= ~kMaskBoth;
            ++g_reasserts;
        }
    }
}

bool enabled() { return g_on.load(std::memory_order_relaxed); }
void set_enabled(bool on) {
    if (g_on.exchange(on, std::memory_order_relaxed) != on)
        BVR_LOG("[bsi] xhair: crosshair hide %s", on ? "ON" : "off (bits restored)");
}

bool handle_command(const char* cmd, const char* args) {
    if (strcmp(cmd, "bsixhair") != 0) return false;
    if (!args) args = "";
    while (*args == ' ') ++args;
    // The recorder passes args with a trailing newline (the s51 token trap -
    // this check must accept it or `bsixhair on` prints status instead).
    if (strncmp(args, "on", 2) == 0 &&
        (args[2] == '\0' || args[2] == ' ' || args[2] == '\n' || args[2] == '\r')) {
        set_enabled(true);
        return true;
    }
    if (strncmp(args, "off", 3) == 0) {
        set_enabled(false);
        return true;
    }
    if (strncmp(args, "derive", 6) == 0) {
        g_nextSweepMs = 0;
        g_sweepBackoffMs = 5000;
        drop_all("manual derive");
        BVR_LOG("[bsi] xhair: sweep forced on the next tick");
        return true;
    }
    BVR_LOG("[bsi] xhair: %s off=+0x%X inst=%d cleared=%d sweeps=%u clears=%u "
            "reasserts=%u%s | bsixhair on|off|derive",
            g_on.load(std::memory_order_relaxed) ? "ON" : "off", g_isShownOff, g_count,
            g_cleared[0] ? 1 : 0, g_sweeps, g_clears, g_reasserts,
            g_refused ? " REFUSED" : "");
    return true;
}

void draw_debug_ui() {
    bool on = g_on.load(std::memory_order_relaxed);
    if (ImGui::Checkbox("hide the game crosshair (s57)", &on)) set_enabled(on);
    ImGui::SameLine();
    ImGui::Text("inst %d, reasserts %u%s", g_count, g_reasserts,
                g_refused ? " REFUSED" : "");
}

} // namespace bvr::bsi::xhair
