// DR-5 probe (SequentialReentry groundwork). The frame root ignores its ECX
// and reads all state from the static render-manager global (session-5
// prologue walk, ENGINE_NOTES "Scene-draw architecture"), so the detour's
// only job is telemetry + optionally calling the original a second time.
// Hook lifecycle is command-gated end to end; the second original call is
// SEH-guarded with a poison latch (a faulted re-entry never re-arms without
// an explicit "reentry reset").

#include "game/bioshock1r/scenedraw.h"

#include "core/gfx/frame_inspector.h"
#include "core/hooks/d3d11_hook.h"
#include "core/util/log.h"
#include "game/bioshock1r/patterns.h"

#include <windows.h>
#include <MinHook.h>

#include <imgui.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <intrin.h>

namespace bvr::b1r::scenedraw {
namespace {

// Both targets are void __thiscall with zero stack args (both frame-root
// exits ret C3; the drain's only call site passes ecx with no pushes and no
// stack fixup after). __fastcall with a dummy EDX slot is register/stack/
// cleanup-identical - same trick as CalcViewDetour.
using RenderFn = void(__fastcall*)(void* self, void* edx);

const uint8_t* g_imageBase = nullptr;
size_t g_imageSize = 0;

struct HookSlot {
    const char* name = nullptr;
    void* target = nullptr;
    RenderFn original = nullptr;
    bool created = false;             // game thread only
    std::atomic<bool> enabled{false};
};
HookSlot g_root{"root"};
HookSlot g_drain{"drain"};

// Controls: command poller (game thread) writes, detour thread reads.
std::atomic<bool>  g_doubleCall{false};
std::atomic<int>   g_pulseCount{0};     // pending one-shot double calls
std::atomic<float> g_secondYawDeg{0.0f};
std::atomic<bool>  g_latchClear{false}; // zero [queue+0x58] before 2nd call
std::atomic<bool>  g_poisoned{false};
std::atomic<uint32_t> g_lastExcCode{0};
std::atomic<uint32_t> g_lastExcRva{0};

// Telemetry. g_rootTid is nonzero exactly while a depth-0 root call is in
// flight on that thread; g_secondPassTid likewise for the re-entry call.
std::atomic<uint32_t> g_rootEntries{0};
std::atomic<uint32_t> g_secondCalls{0};
std::atomic<uint32_t> g_rootTid{0};
std::atomic<int>      g_rootDepth{0};
std::atomic<uint32_t> g_secondPassTid{0};
std::atomic<uint32_t> g_calcInRoot{0};
std::atomic<uint32_t> g_calcOutside{0};
std::atomic<uint32_t> g_secondPassHits{0};
std::atomic<uint32_t> g_lastCalcTid{0};
std::atomic<uint32_t> g_call1Us{0}, g_call2Us{0};
std::atomic<uint32_t> g_lastSecondDraws{0};
std::atomic<uint32_t> g_drainEntries{0};
std::atomic<uint32_t> g_concurrentEntries{0};
std::atomic<uint32_t> g_callerRvas[4]{};

// Heartbeat bookkeeping - detour thread only.
uint64_t g_lastBeatMs = 0;
uint32_t g_beatRoot = 0, g_beatSecond = 0, g_beatCalcIn = 0, g_beatCalcOut = 0,
         g_beatDrain = 0;
uint64_t g_beatPresents = 0, g_beatDraws = 0;

uint32_t to_rva(const void* p) {
    uintptr_t d = reinterpret_cast<uintptr_t>(p) - reinterpret_cast<uintptr_t>(g_imageBase);
    return d < g_imageSize ? static_cast<uint32_t>(d) : 0xFFFFFFFFu;
}

void note_caller(uint32_t rva) {
    for (auto& slot : g_callerRvas) {
        uint32_t cur = slot.load(std::memory_order_relaxed);
        if (cur == rva) return;
        if (cur == 0) { slot.store(rva, std::memory_order_relaxed); return; }
    }
}

// SEH filter for the SECOND original call only (the first stays unguarded -
// swallowing a vanilla-path crash would destroy real crash dumps). C++
// throws (0xE06D7363) and stack overflow pass through to the game's own
// handling; everything else is recorded and handled.
int reentry_filter(unsigned code, EXCEPTION_POINTERS* ep) {
    if (code == 0xE06D7363u) return EXCEPTION_CONTINUE_SEARCH;
    if (code == EXCEPTION_STACK_OVERFLOW) return EXCEPTION_CONTINUE_SEARCH;
    g_lastExcCode.store(code, std::memory_order_relaxed);
    g_lastExcRva.store(ep ? to_rva(ep->ExceptionRecord->ExceptionAddress) : 0,
                       std::memory_order_relaxed);
    return EXCEPTION_EXECUTE_HANDLER;
}

// No C++ objects in this frame (SEH + unwinding = C2712).
bool call_original_guarded(RenderFn fn, void* self, void* edx) {
    __try {
        fn(self, edx);
        return true;
    } __except (reentry_filter(GetExceptionCode(), GetExceptionInformation())) {
        return false;
    }
}

// Zero the drain-skip guard [queue+0x58] (queue = [mgr+4], mgr from the
// static global) so a re-entry drains again if the first call latched it.
bool clear_drain_latch() {
    __try {
        const uint8_t* mgr =
            *reinterpret_cast<uint8_t* const*>(g_imageBase + patterns::kRenderMgrGlobalRva);
        if (!mgr) return false;
        uint8_t* queue = *reinterpret_cast<uint8_t* const*>(mgr + 4);
        if (!queue) return false;
        *reinterpret_cast<uint32_t*>(queue + patterns::kQueueDrainGuardOffset) = 0;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

uint32_t qpc_us(const LARGE_INTEGER& a, const LARGE_INTEGER& b) {
    static LARGE_INTEGER freq{};
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    return static_cast<uint32_t>((b.QuadPart - a.QuadPart) * 1000000 / freq.QuadPart);
}

void heartbeat() {
    uint64_t now = GetTickCount64();
    if (g_lastBeatMs == 0) { g_lastBeatMs = now; }
    if (now - g_lastBeatMs < 1000) return;
    g_lastBeatMs = now;

    uint32_t roots = g_rootEntries.load(std::memory_order_relaxed);
    uint32_t seconds = g_secondCalls.load(std::memory_order_relaxed);
    uint32_t calcIn = g_calcInRoot.load(std::memory_order_relaxed);
    uint32_t calcOut = g_calcOutside.load(std::memory_order_relaxed);
    uint32_t drains = g_drainEntries.load(std::memory_order_relaxed);
    uint64_t presents = bvr::d3d11_hook::present_count();
    uint32_t dRoot = roots - g_beatRoot;
    uint32_t dCalcIn = calcIn - g_beatCalcIn;
    uint32_t dDrain = drains - g_beatDrain;
    uint64_t dPresent = presents - g_beatPresents;
    BVR_LOG("[reentry] root=%u/s 2nd=%u/s presents=%u/s calcview in=%u out=%u/s "
            "drain=%u/s call1=%uus call2=%uus tid root=%u calc=%u "
            "callers=%X,%X,%X,%X%s",
            dRoot, seconds - g_beatSecond, static_cast<uint32_t>(dPresent),
            dCalcIn, calcOut - g_beatCalcOut, dDrain,
            g_call1Us.load(std::memory_order_relaxed),
            g_call2Us.load(std::memory_order_relaxed),
            g_rootTid.load(std::memory_order_relaxed),
            g_lastCalcTid.load(std::memory_order_relaxed),
            g_callerRvas[0].load(std::memory_order_relaxed),
            g_callerRvas[1].load(std::memory_order_relaxed),
            g_callerRvas[2].load(std::memory_order_relaxed),
            g_callerRvas[3].load(std::memory_order_relaxed),
            g_poisoned.load(std::memory_order_relaxed) ? " POISONED" : "");
    g_beatRoot = roots;
    g_beatSecond = seconds;
    g_beatCalcIn = calcIn;
    g_beatCalcOut = calcOut;
    g_beatDrain = drains;
    g_beatPresents = presents;
}

void __fastcall FrameRootDetour(void* self, void* edx) {
    uint32_t tid = GetCurrentThreadId();
    uint32_t prevTid = g_rootTid.load(std::memory_order_relaxed);
    if (prevTid != 0 && prevTid != tid)
        g_concurrentEntries.fetch_add(1, std::memory_order_relaxed);
    int depth = g_rootDepth.fetch_add(1, std::memory_order_relaxed);
    if (depth == 0) g_rootTid.store(tid, std::memory_order_relaxed);
    g_rootEntries.fetch_add(1, std::memory_order_relaxed);
    note_caller(to_rva(_ReturnAddress()));

    LARGE_INTEGER t0, t1;
    QueryPerformanceCounter(&t0);
    g_root.original(self, edx); // vanilla path: never guarded
    QueryPerformanceCounter(&t1);
    g_call1Us.store(qpc_us(t0, t1), std::memory_order_relaxed);

    bool wantSecond = false;
    bool isPulse = false;
    if (depth == 0 && !g_poisoned.load(std::memory_order_relaxed)) {
        if (g_doubleCall.load(std::memory_order_relaxed)) {
            wantSecond = true;
        } else {
            int pulses = g_pulseCount.load(std::memory_order_relaxed);
            while (pulses > 0 &&
                   !g_pulseCount.compare_exchange_weak(pulses, pulses - 1,
                                                       std::memory_order_relaxed)) {}
            if (pulses > 0) { wantSecond = true; isPulse = true; }
        }
    }
    if (wantSecond) {
        uint64_t drawsBefore = bvr::frame_inspector::draw_call_census();
        if (g_latchClear.load(std::memory_order_relaxed)) clear_drain_latch();
        LARGE_INTEGER t2, t3;
        QueryPerformanceCounter(&t2);
        g_secondPassTid.store(tid, std::memory_order_relaxed);
        bool ok = call_original_guarded(g_root.original, self, edx);
        g_secondPassTid.store(0, std::memory_order_relaxed); // also on fault path
        QueryPerformanceCounter(&t3);
        uint32_t call2Us = qpc_us(t2, t3);
        uint32_t draws2 =
            static_cast<uint32_t>(bvr::frame_inspector::draw_call_census() - drawsBefore);
        g_call2Us.store(call2Us, std::memory_order_relaxed);
        g_lastSecondDraws.store(draws2, std::memory_order_relaxed);
        g_secondCalls.fetch_add(1, std::memory_order_relaxed);
        if (!ok) {
            g_poisoned.store(true, std::memory_order_relaxed);
            g_doubleCall.store(false, std::memory_order_relaxed);
            g_pulseCount.store(0, std::memory_order_relaxed);
            BVR_LOG("[reentry] second call FAULTED code=0x%08X rva=0x%X - POISONED "
                    "(hook stays pass-through; 'reentry reset' to clear)",
                    g_lastExcCode.load(std::memory_order_relaxed),
                    g_lastExcRva.load(std::memory_order_relaxed));
        } else if (isPulse) {
            BVR_LOG("[reentry] pulse ok: call1=%uus call2=%uus draws2=%u yaw=%.1f "
                    "latchclear=%d",
                    g_call1Us.load(std::memory_order_relaxed), call2Us, draws2,
                    g_secondYawDeg.load(std::memory_order_relaxed),
                    g_latchClear.load(std::memory_order_relaxed) ? 1 : 0);
        }
    }

    heartbeat();
    if (g_rootDepth.fetch_sub(1, std::memory_order_relaxed) == 1)
        g_rootTid.store(0, std::memory_order_relaxed);
}

void __fastcall DrainDetour(void* self, void* edx) {
    g_drainEntries.fetch_add(1, std::memory_order_relaxed);
    g_drain.original(self, edx);
    // Fallback telemetry when only the drain is hooked (root hook rejected).
    if (!g_root.enabled.load(std::memory_order_relaxed)) heartbeat();
}

bool prologue_matches(const void* target, const uint8_t* expect, size_t n) {
    return bvr::pattern_scan::is_memory_valid(target, n) &&
           memcmp(target, expect, n) == 0;
}

bool install_slot(HookSlot& slot, uint32_t rva, void* detour,
                  const uint8_t* prologue, size_t prologueLen) {
    if (!g_imageBase) { BVR_LOG("[reentry] no image base - init failed?"); return false; }
    if (slot.enabled.load(std::memory_order_relaxed)) {
        BVR_LOG("[reentry] %s hook already enabled", slot.name);
        return true;
    }
    slot.target = const_cast<uint8_t*>(g_imageBase) + rva;
    if (!slot.created) {
        if (!prologue_matches(slot.target, prologue, prologueLen)) {
            BVR_LOG("[reentry] %s prologue mismatch at %p - build changed? REFUSING hook",
                    slot.name, slot.target);
            return false;
        }
        MH_STATUS st = MH_CreateHook(slot.target, detour,
                                     reinterpret_cast<void**>(&slot.original));
        if (st != MH_OK) {
            BVR_LOG("[reentry] MH_CreateHook(%s) failed: %s", slot.name,
                    MH_StatusToString(st));
            return false;
        }
        slot.created = true;
    }
    // Self-enabling like camera::install - activation never rides on another
    // module's MH_ALL_HOOKS.
    MH_STATUS st = MH_EnableHook(slot.target);
    if (st != MH_OK) {
        BVR_LOG("[reentry] MH_EnableHook(%s) failed: %s", slot.name, MH_StatusToString(st));
        return false;
    }
    slot.enabled.store(true, std::memory_order_relaxed);
    BVR_LOG("[reentry] %s hook ENABLED (target %p, rva 0x%X)", slot.name, slot.target, rva);
    return true;
}

void disable_slot(HookSlot& slot) {
    if (!slot.enabled.load(std::memory_order_relaxed)) return;
    // Disable only - MH_RemoveHook would free the trampoline while another
    // thread could still be returning through it. Disable freezes threads and
    // relocates any IP inside the patched head; deeper frames are unaffected.
    MH_STATUS st = MH_DisableHook(slot.target);
    slot.enabled.store(false, std::memory_order_relaxed);
    BVR_LOG("[reentry] %s hook disabled (%s)", slot.name, MH_StatusToString(st));
}

} // namespace

void init(const bvr::pattern_scan::ProcessImage& image) {
    g_imageBase = image.base;
    g_imageSize = image.size;
}

void handle_command(const char* args) {
    char verb[16] = {};
    int consumed = 0;
    if (sscanf_s(args, "%15s%n", verb, static_cast<unsigned>(sizeof verb), &consumed) != 1) {
        BVR_LOG("[reentry] command needs a verb: hook|unhook|on|off|pulse|yaw|latchclear|reset|status");
        return;
    }
    const char* rest = args + consumed;
    while (*rest == ' ' || *rest == '\t') ++rest;

    if (strcmp(verb, "hook") == 0) {
        if (strncmp(rest, "drain", 5) == 0) {
            install_slot(g_drain, patterns::kDrainRva,
                         reinterpret_cast<void*>(&DrainDetour),
                         patterns::kDrainPrologue, sizeof patterns::kDrainPrologue);
        } else {
            install_slot(g_root, patterns::kFrameRootRva,
                         reinterpret_cast<void*>(&FrameRootDetour),
                         patterns::kFrameRootPrologue, sizeof patterns::kFrameRootPrologue);
        }
    } else if (strcmp(verb, "unhook") == 0) {
        g_doubleCall.store(false, std::memory_order_relaxed);
        g_pulseCount.store(0, std::memory_order_relaxed);
        disable_slot(g_root);
        disable_slot(g_drain);
    } else if (strcmp(verb, "on") == 0) {
        if (!g_root.enabled.load(std::memory_order_relaxed)) {
            BVR_LOG("[reentry] on refused: root hook not enabled");
        } else if (g_poisoned.load(std::memory_order_relaxed)) {
            BVR_LOG("[reentry] on refused: POISONED ('reentry reset' to clear)");
        } else {
            g_doubleCall.store(true, std::memory_order_relaxed);
            BVR_LOG("[reentry] double-call ON (yaw %.1f)",
                    g_secondYawDeg.load(std::memory_order_relaxed));
        }
    } else if (strcmp(verb, "off") == 0) {
        g_doubleCall.store(false, std::memory_order_relaxed);
        g_pulseCount.store(0, std::memory_order_relaxed);
        BVR_LOG("[reentry] double-call off");
    } else if (strcmp(verb, "pulse") == 0) {
        if (!g_root.enabled.load(std::memory_order_relaxed)) {
            BVR_LOG("[reentry] pulse refused: root hook not enabled");
        } else if (g_poisoned.load(std::memory_order_relaxed)) {
            BVR_LOG("[reentry] pulse refused: POISONED ('reentry reset' to clear)");
        } else {
            g_pulseCount.fetch_add(1, std::memory_order_relaxed);
            BVR_LOG("[reentry] pulse armed (yaw %.1f)",
                    g_secondYawDeg.load(std::memory_order_relaxed));
        }
    } else if (strcmp(verb, "yaw") == 0) {
        float deg = 0.0f;
        if (sscanf_s(rest, "%f", &deg) == 1) {
            g_secondYawDeg.store(deg, std::memory_order_relaxed);
            BVR_LOG("[reentry] second-pass yaw = %.1f deg", deg);
        }
    } else if (strcmp(verb, "latchclear") == 0) {
        bool on = strncmp(rest, "on", 2) == 0;
        g_latchClear.store(on, std::memory_order_relaxed);
        BVR_LOG("[reentry] latchclear %s", on ? "on" : "off");
    } else if (strcmp(verb, "reset") == 0) {
        g_poisoned.store(false, std::memory_order_relaxed);
        BVR_LOG("[reentry] poison cleared (last fault code=0x%08X rva=0x%X)",
                g_lastExcCode.load(std::memory_order_relaxed),
                g_lastExcRva.load(std::memory_order_relaxed));
    } else if (strcmp(verb, "status") == 0) {
        BVR_LOG("[reentry] status: root=%s drain=%s double=%d pulse=%d yaw=%.1f "
                "latchclear=%d poisoned=%d entries=%u seconds=%u concurrent=%u "
                "draws2=%u",
                g_root.enabled.load(std::memory_order_relaxed) ? "on" : "off",
                g_drain.enabled.load(std::memory_order_relaxed) ? "on" : "off",
                g_doubleCall.load(std::memory_order_relaxed) ? 1 : 0,
                g_pulseCount.load(std::memory_order_relaxed),
                g_secondYawDeg.load(std::memory_order_relaxed),
                g_latchClear.load(std::memory_order_relaxed) ? 1 : 0,
                g_poisoned.load(std::memory_order_relaxed) ? 1 : 0,
                g_rootEntries.load(std::memory_order_relaxed),
                g_secondCalls.load(std::memory_order_relaxed),
                g_concurrentEntries.load(std::memory_order_relaxed),
                g_lastSecondDraws.load(std::memory_order_relaxed));
    } else {
        BVR_LOG("[reentry] unknown verb '%s'", verb);
    }
}

bool second_pass_for_current_thread(float* yawDegOut) {
    uint32_t t = g_secondPassTid.load(std::memory_order_relaxed);
    if (t == 0 || t != GetCurrentThreadId()) return false;
    g_secondPassHits.fetch_add(1, std::memory_order_relaxed);
    *yawDegOut = g_secondYawDeg.load(std::memory_order_relaxed);
    return true;
}

void note_calcview() {
    uint32_t tid = GetCurrentThreadId();
    g_lastCalcTid.store(tid, std::memory_order_relaxed);
    if (g_rootTid.load(std::memory_order_relaxed) == tid)
        g_calcInRoot.fetch_add(1, std::memory_order_relaxed);
    else
        g_calcOutside.fetch_add(1, std::memory_order_relaxed);
}

bool hook_live() {
    return g_root.enabled.load(std::memory_order_relaxed) ||
           g_drain.enabled.load(std::memory_order_relaxed);
}

void draw_debug_ui() {
    if (!ImGui::CollapsingHeader("Reentry probe (DR-5)")) return;
    ImGui::Text("hooks: root %s, drain %s%s",
                g_root.enabled.load(std::memory_order_relaxed) ? "ON" : "off",
                g_drain.enabled.load(std::memory_order_relaxed) ? "ON" : "off",
                g_poisoned.load(std::memory_order_relaxed) ? "  POISONED" : "");
    ImGui::Text("double-call %s  yaw %.1f  2nd calls %u  draws2 %u",
                g_doubleCall.load(std::memory_order_relaxed) ? "ON" : "off",
                g_secondYawDeg.load(std::memory_order_relaxed),
                g_secondCalls.load(std::memory_order_relaxed),
                g_lastSecondDraws.load(std::memory_order_relaxed));
    ImGui::Text("root entries %u  calcview in/out %u/%u  2nd-pass hits %u",
                g_rootEntries.load(std::memory_order_relaxed),
                g_calcInRoot.load(std::memory_order_relaxed),
                g_calcOutside.load(std::memory_order_relaxed),
                g_secondPassHits.load(std::memory_order_relaxed));
    ImGui::TextDisabled("control via seam: reentry hook|unhook|on|off|pulse|yaw <deg>");
}

} // namespace bvr::b1r::scenedraw
