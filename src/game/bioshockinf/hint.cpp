#include "game/bioshockinf/hint.h"

#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstring>

#include "core/hooks/pattern_scan.h"
#include "core/util/log.h"
#include "game/bioshockinf/camera.h"
#include "game/bioshockinf/gfx.h"
#include "game/bioshockinf/reflect.h"

namespace bvr::bsi::hint {
namespace {

using bvr::pattern_scan::is_memory_valid;

constexpr int kMaxInst = 8;

// Per-class cache: the hint widgets are the oracle's primary surface, the
// containers ride along as a diagnostic (their counts/bits print in status;
// the aggregate reads HINTS only until validation says otherwise).
struct ClassCache {
    const char* cls;
    void* inst[kMaxInst] = {};
    int count = 0;
    uint32_t isShownOff = 0;  // derived per boot BY NAME; 0 = not yet
    uint32_t isShownMask = 0;
    bool refused = false;     // derivation refusal latch, per class per boot
};

ClassCache g_hints{"XClikButtonHint"};
ClassCache g_containers{"XClikButtonHintsContainer"};

std::atomic<bool> g_watch{false};
uint64_t g_lastTickMs = 0;
uint64_t g_nextSweepMs = 0;
uint64_t g_sweepBackoffMs = 5000;
uint32_t g_sweeps = 0, g_edges = 0;
int g_lastAggregate = -1; // -1 unknown, 0 off, 1 on - the watch edge detector

// s58: the VALIDATED oracle - the PC's ButtonUseTarget (InterfaceProperty,
// offset derived BY NAME per boot; +0x176C on this build). It is the
// interaction system's live selected USE target: set while the arming cone
// holds, null otherwise - the widget bits above turned out to be sticky
// bookkeeping (ENGINE_NOTES s58), so THIS field is the sweep's ground truth.
uint32_t g_useOff = 0;
bool g_useRefused = false;
void* g_lastUseTarget = nullptr; // watch edge detector

void* use_target(char* cls, size_t clsSize) {
    if (cls && clsSize) cls[0] = '\0';
    void* pc = camera::last_player_controller();
    if (!pc) return nullptr;
    if (g_useOff == 0) {
        if (g_useRefused) return nullptr;
        uint32_t off = 0;
        if (!reflect::find_property_offset(pc, "ButtonUseTarget", "InterfaceProperty",
                                           &off) ||
            off == 0 || off > 0x4000) {
            g_useRefused = true;
            BVR_LOG("[bsi] hint: REFUSED - ButtonUseTarget did not derive on PC %p", pc);
            return nullptr;
        }
        g_useOff = off;
        BVR_LOG("[bsi] hint: ButtonUseTarget derived at PC+0x%X", off);
    }
    if (!is_memory_valid(pc, g_useOff + 4)) return nullptr;
    void* t = *reinterpret_cast<void* const*>(static_cast<const uint8_t*>(pc) + g_useOff);
    if (t && cls && clsSize && !reflect::class_name_of(t, cls, clsSize))
        snprintf(cls, clsSize, "(unreadable)");
    return t;
}

bool inst_alive(const ClassCache& c, void* p) {
    const size_t span = c.isShownOff ? c.isShownOff + 4 : 0x120;
    if (!p || !is_memory_valid(p, span)) return false;
    char cls[64] = {};
    return reflect::class_name_of(p, cls, sizeof cls) && strcmp(cls, c.cls) == 0;
}

// Raw dword the IsShown bit lives in (diagnostic value; the aggregate applies
// the mask). Returns false when the instance is dead or nothing derived.
bool read_bits(const ClassCache& c, void* p, uint32_t* out) {
    if (!c.isShownOff || !inst_alive(c, p)) return false;
    *out = *reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>(p) +
                                              c.isShownOff);
    return true;
}

void drop(ClassCache& c) { c.count = 0; }

void sweep_class(ClassCache& c) {
    void* found[kMaxInst] = {};
    const int n = gfx::find_instances(c.cls, found, kMaxInst);
    if (n <= 0) {
        drop(c);
        BVR_LOG("[bsi] hint: sweep found no live %s (%s)", c.cls,
                n < 0 ? "prerequisites missing" : "none on this level");
        return;
    }
    // Offset derivation, once per boot per class, BY NAME on a live instance -
    // a wrong layout must refuse, never misread (the s49b refusal pattern;
    // read-only here, so a refusal only silences the oracle, never corrupts).
    if (c.isShownOff == 0 && !c.refused) {
        uint32_t off = 0, mask = 0;
        if (!reflect::find_bool_property_bit(found[0], "IsShown", &off, &mask) ||
            off == 0 || off > 0x1000 || mask == 0) {
            c.refused = true;
            BVR_LOG("[bsi] hint: REFUSED - IsShown did not derive on %s %p "
                    "(fallback: bsiprop field walk for the tracking property)",
                    c.cls, found[0]);
        } else {
            c.isShownOff = off;
            c.isShownMask = mask;
            BVR_LOG("[bsi] hint: %s IsShown derived at +0x%X mask 0x%X", c.cls, off,
                    mask);
        }
    }
    memcpy(c.inst, found, sizeof found);
    c.count = n;
    BVR_LOG("[bsi] hint: %d live %s instance(s) cached", n, c.cls);
}

void sweep() {
    ++g_sweeps;
    g_nextSweepMs = g_lastTickMs + g_sweepBackoffMs;
    if (g_sweepBackoffMs < 60000) g_sweepBackoffMs *= 2;
    sweep_class(g_hints);
    sweep_class(g_containers);
    if (g_hints.count > 0) g_sweepBackoffMs = 5000; // fresh level, fresh budget
}

// The oracle aggregate: any live hint widget with its IsShown bit set.
// Returns -1 when nothing is readable (no instances / underived / refused).
int aggregate(char* detail, size_t detailSize) {
    int n = 0;
    int shown = 0, readable = 0;
    if (detail && detailSize) detail[0] = '\0';
    for (int i = 0; i < g_hints.count; ++i) {
        uint32_t bits = 0;
        if (!read_bits(g_hints, g_hints.inst[i], &bits)) continue;
        ++readable;
        const bool on = (bits & g_hints.isShownMask) != 0;
        if (on) ++shown;
        if (detail && detailSize)
            n += sprintf_s(detail + n, detailSize - n, " %p=0x%X%s", g_hints.inst[i],
                           bits, on ? "*" : "");
    }
    if (!readable) return -1;
    return shown > 0 ? 1 : 0;
}

void log_state(const char* why) {
    char detail[192] = {};
    const int agg = aggregate(detail, sizeof detail);
    BVR_LOG("[bsi] hint: PROMPT %s (%s; hints %d:%s)",
            agg == 1 ? "ON" : (agg == 0 ? "off" : "UNREADABLE"), why, g_hints.count,
            detail[0] ? detail : " none");
}

} // namespace

void tick(uint64_t nowMs) {
    if (!g_watch.load(std::memory_order_relaxed)) return;
    if (nowMs - g_lastTickMs < 1000) return;
    g_lastTickMs = nowMs;

    // Cached instances must stay provably alive - a level swap frees them.
    for (int i = 0; i < g_hints.count; ++i) {
        if (!inst_alive(g_hints, g_hints.inst[i])) {
            drop(g_hints);
            drop(g_containers);
            g_lastAggregate = -1;
            BVR_LOG("[bsi] hint: instances dropped (died) - re-sweep on backoff");
            break;
        }
    }

    if (g_hints.count == 0) {
        // Auto re-sweep only in live gameplay on the backoff schedule (the
        // sweep is a one-shot ~0.5 s game-thread hitch, never a cadence).
        if (camera::frame_context().valid && nowMs >= g_nextSweepMs) sweep();
        return;
    }

    const int agg = aggregate(nullptr, 0);
    if (agg != g_lastAggregate) {
        g_lastAggregate = agg;
        ++g_edges;
        log_state("watch edge");
    }

    // s58: the use-target edge - the oracle the caller sweep validated. One
    // line per target change, so a headset A/B reads clean off the log.
    char cls[64] = {};
    void* t = use_target(cls, sizeof cls);
    if (t != g_lastUseTarget) {
        g_lastUseTarget = t;
        ++g_edges;
        if (t)
            BVR_LOG("[bsi] hint: USE target -> %s %p%s", cls, t,
                    camera::head_use_enabled() ? " (head-directed)" : " (body-locked)");
        else
            BVR_LOG("[bsi] hint: USE target -> null%s",
                    camera::head_use_enabled() ? " (head-directed)" : " (body-locked)");
    }
}

bool handle_command(const char* cmd, const char* args) {
    if (strcmp(cmd, "bsihint") != 0) return false;
    if (!args) args = "";
    while (*args == ' ') ++args;
    // The recorder passes args with a trailing newline (the s51 token trap -
    // verbs must accept \n/\r or `bsihint watch on` degrades to a status print).
    if (strncmp(args, "scan", 4) == 0) {
        drop(g_hints);
        drop(g_containers);
        g_sweepBackoffMs = 5000;
        g_lastTickMs = GetTickCount64();
        sweep();
        log_state("scan");
        return true;
    }
    if (strncmp(args, "watch", 5) == 0) {
        const char* v = args + 5;
        while (*v == ' ') ++v;
        if (strncmp(v, "on", 2) == 0 &&
            (v[2] == '\0' || v[2] == ' ' || v[2] == '\n' || v[2] == '\r')) {
            g_watch.store(true, std::memory_order_relaxed);
            g_lastAggregate = -1; // first watch tick logs the current state
            BVR_LOG("[bsi] hint: watch ON (1 Hz, logs on state change)");
        } else if (strncmp(v, "off", 3) == 0) {
            g_watch.store(false, std::memory_order_relaxed);
            BVR_LOG("[bsi] hint: watch off");
        } else {
            BVR_LOG("[bsi] usage: bsihint watch on|off");
        }
        return true;
    }
    // Status: one fresh read, plus the derivation facts the sweep banked.
    char detail[192] = {};
    const int agg = aggregate(detail, sizeof detail);
    char cls[64] = {};
    void* t = use_target(cls, sizeof cls);
    BVR_LOG("[bsi] hint: USE target %s (off +0x%X%s, %s) | widget PROMPT %s | "
            "watch %s edges %u | %s x%d%s | %s x%d%s | sweeps %u | "
            "bsihint scan|watch on|off|status",
            t ? cls : "null", g_useOff, g_useRefused ? " REFUSED" : "",
            camera::head_use_enabled() ? "head-directed" : "body-locked",
            agg == 1 ? "ON" : (agg == 0 ? "off" : "UNREADABLE"),
            g_watch.load(std::memory_order_relaxed) ? "ON" : "off", g_edges,
            g_hints.cls, g_hints.count, g_hints.refused ? " REFUSED" : "",
            g_containers.cls, g_containers.count, g_containers.refused ? " REFUSED" : "",
            g_sweeps);
    return true;
}

} // namespace bvr::bsi::hint
