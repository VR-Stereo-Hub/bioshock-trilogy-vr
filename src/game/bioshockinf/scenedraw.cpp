#include "game/bioshockinf/scenedraw.h"

#include "core/hooks/d3d11_hook.h"
#include "core/util/crash.h"
#include "core/util/log.h"
#include "core/vr/openxr_runtime.h"
#include "game/bioshockinf/camera.h"
#include "game/bioshockinf/patterns.h"

#include <MinHook.h>
#include <imgui.h>
#include <intrin.h>
#include <windows.h>

#include <atomic>
#include <cstring>

namespace bvr::bsi::scenedraw {
namespace {

// The doubling root's contract (patterns.h derivation): the VIEWPORT draw -
// __thiscall on the viewport with ONE stack arg, `ret 4`. Its call tree is
// canvas -> client Draw (the camera + scene build) -> present kick, so a
// doubled call yields a whole second eye INCLUDING its present - doubling
// one level down (the client draw) was measured to skew the tag ring +1 per
// tick because the present stays single. Same __fastcall-with-dummy-EDX
// trick as the camera detour, same RTC rule on the arg count.
using DrawFn = void(__fastcall*)(void* self, void* edx, void* a1);

DrawFn g_original = nullptr;
void* g_target = nullptr;
std::atomic<bool> g_hookLive{false};

// Doubling state.
std::atomic<bool> g_stereo{false};    // the SR arm (vrstereo full ladder)
std::atomic<int> g_pulseCount{0};     // one-shot doubles for A/B (`reentry pulse`)
std::atomic<bool> g_poisoned{false};  // set on a second-draw fault; `reentry reset`

// Depth/tid latch: the doubling decision runs only at depth 0 on the single
// drawing thread; the latch also attributes pass-2 camera dispatches.
std::atomic<int> g_depth{0};
std::atomic<uint32_t> g_drawTid{0};
std::atomic<uint32_t> g_secondPassTid{0};
std::atomic<uint32_t> g_secondPassSeq{0};
std::atomic<bool> g_inSecondDraw{false};

// Counters (the acceptance instruments - see `reentry status`).
std::atomic<uint32_t> g_draws{0};
std::atomic<uint32_t> g_secondDraws{0};
std::atomic<uint32_t> g_foreignCallerSkips{0};
std::atomic<uint32_t> g_stereoSkips{0}; // camera-silent / present-stall / teardown
std::atomic<uint32_t> g_lastExcCode{0};
std::atomic<uint32_t> g_lastExcRva{0};
std::atomic<uint32_t> g_call2Us{0};

// Caller census at THIS seam (the deny gate's own evidence): the derivation
// says a single virtual dispatch site reaches the root - this census is what
// verifies that live, including across loads and menus.
constexpr size_t kCallerSlots = 8;
struct CallerSlot {
    std::atomic<uint32_t> rva{0};
    std::atomic<uint32_t> count{0};
};
CallerSlot g_callers[kCallerSlots];

// Heartbeat deltas (game thread only).
uint64_t g_lastBeatMs = 0;
uint32_t g_beatDraws = 0, g_beatSecond = 0;
uint64_t g_beatPresents = 0;
uint32_t g_beatCamReplays = 0;
uint64_t g_lastDrawPresent = 0;

uint32_t to_rva(const void* p) {
    const patterns::Symbols& s = patterns::symbols();
    if (!s.imageBase) return 0xFFFFFFFFu;
    const uintptr_t d =
        reinterpret_cast<uintptr_t>(p) - reinterpret_cast<uintptr_t>(s.imageBase);
    return d < s.imageSize ? static_cast<uint32_t>(d) : 0xFFFFFFFFu;
}

void note_caller(uint32_t rva) {
    for (auto& slot : g_callers) {
        uint32_t cur = slot.rva.load(std::memory_order_relaxed);
        if (cur == rva) {
            slot.count.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        if (cur == 0 && slot.rva.compare_exchange_strong(cur, rva)) {
            slot.count.fetch_add(1, std::memory_order_relaxed);
            return;
        }
    }
}

// SEH filter for the SECOND call only (the first stays unguarded - swallowing
// a vanilla-path crash would destroy real crash dumps). C++ throws and stack
// overflow pass through; everything else is recorded and handled.
int reentry_filter(unsigned code, EXCEPTION_POINTERS* ep) {
    if (code == 0xE06D7363u) return EXCEPTION_CONTINUE_SEARCH;
    if (code == EXCEPTION_STACK_OVERFLOW) return EXCEPTION_CONTINUE_SEARCH;
    g_lastExcCode.store(code, std::memory_order_relaxed);
    g_lastExcRva.store(ep ? to_rva(ep->ExceptionRecord->ExceptionAddress) : 0,
                       std::memory_order_relaxed);
    return EXCEPTION_EXECUTE_HANDLER;
}

// No C++ objects in this frame (SEH + unwinding = C2712).
bool call_draw_guarded(DrawFn fn, void* self, void* edx, void* a1) {
    __try {
        fn(self, edx, a1);
        return true;
    } __except (reentry_filter(GetExceptionCode(), GetExceptionInformation())) {
        return false;
    }
}

void maybe_second_draw(void* self, void* edx, void* a1, uint32_t callerRva,
                       uint64_t presentDelta) {
    if (g_poisoned.load(std::memory_order_relaxed)) return;
    // No doubled draws once the window began closing (BS2 session 38).
    if (bvr::crash::teardown_seen()) return;
    bool pulse = false;
    if (g_pulseCount.load(std::memory_order_relaxed) > 0)
        pulse = g_pulseCount.fetch_sub(1, std::memory_order_relaxed) > 0;
    const bool stereo = g_stereo.load(std::memory_order_relaxed);
    if (!pulse && !stereo) return;

    // Deny-by-default caller gate: ONLY the census-verified per-tick
    // gameplay dispatcher may be doubled (4 static callers exist).
    if (callerRva != patterns::kViewportDrawGameplayRetRva) {
        g_foreignCallerSkips.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    // Camera-silent hole: the controller loop inside the root just ran if a
    // player camera exists; silence means menu/load/no-player - a doubled
    // frame would replay a stale base.
    if (camera::silent_ms() > 400) {
        g_stereoSkips.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    // Present-stall guard: no present landed since the previous Draw
    // (unfocused / hitching render thread). Pulses bypass it so the A/B
    // instrument stays usable while paused.
    if (presentDelta == 0 && !pulse) {
        g_stereoSkips.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    if (stereo) bvr::vr::sr_push_eye(+1);
    g_secondPassSeq.fetch_add(1, std::memory_order_relaxed);
    g_secondPassTid.store(GetCurrentThreadId(), std::memory_order_relaxed);
    LARGE_INTEGER t0, t1, freq;
    QueryPerformanceCounter(&t0);
    // The pace tracer can only name a stall inside code it brackets - this
    // marker is what lets it say "the game is sitting inside the re-entered
    // scene draw" (how BS2's freeze was localized).
    bvr::vr::set_draw_stage("secondDraw");
    g_inSecondDraw.store(true, std::memory_order_relaxed);
    const bool ok = call_draw_guarded(g_original, self, edx, a1);
    g_inSecondDraw.store(false, std::memory_order_relaxed);
    bvr::vr::set_draw_stage(nullptr);
    QueryPerformanceCounter(&t1);
    QueryPerformanceFrequency(&freq);
    g_secondPassTid.store(0, std::memory_order_relaxed);
    g_call2Us.store(
        static_cast<uint32_t>((t1.QuadPart - t0.QuadPart) * 1000000 / freq.QuadPart),
        std::memory_order_relaxed);
    if (!ok) {
        g_poisoned.store(true, std::memory_order_relaxed);
        g_stereo.store(false, std::memory_order_relaxed);
        BVR_LOG("[bsi][reentry] second draw FAULTED code=0x%08X rva=0x%X - POISONED "
                "(doubling disarmed; `reentry reset` to clear)",
                g_lastExcCode.load(std::memory_order_relaxed),
                g_lastExcRva.load(std::memory_order_relaxed));
        return;
    }
    g_secondDraws.fetch_add(1, std::memory_order_relaxed);
    if (pulse)
        BVR_LOG("[bsi][reentry] pulse: second draw ok, call2=%u us, presentDelta=%llu",
                g_call2Us.load(std::memory_order_relaxed),
                static_cast<unsigned long long>(presentDelta));
}

void heartbeat(uint64_t now) {
    if (g_lastBeatMs == 0 || now - g_lastBeatMs >= 5000) {
        if (g_lastBeatMs != 0 &&
            (g_stereo.load(std::memory_order_relaxed) ||
             g_secondDraws.load(std::memory_order_relaxed) != g_beatSecond)) {
            const uint32_t draws = g_draws.load(std::memory_order_relaxed);
            const uint32_t second = g_secondDraws.load(std::memory_order_relaxed);
            const uint64_t presents = bvr::d3d11_hook::present_count();
            const uint32_t replays = camera::second_pass_replays();
            const uint64_t dt = now - g_lastBeatMs;
            BVR_LOG("[bsi][reentry] beat: draws/s=%llu 2nd/s=%llu presents/s=%llu "
                    "camReplays/s=%llu skips f=%u s=%u call2=%u us drawTid=%u presentTid=%u",
                    (draws - g_beatDraws) * 1000ull / dt,
                    (second - g_beatSecond) * 1000ull / dt,
                    (presents - g_beatPresents) * 1000ull / dt,
                    (replays - g_beatCamReplays) * 1000ull / dt,
                    g_foreignCallerSkips.load(std::memory_order_relaxed),
                    g_stereoSkips.load(std::memory_order_relaxed),
                    g_call2Us.load(std::memory_order_relaxed),
                    g_drawTid.load(std::memory_order_relaxed),
                    bvr::d3d11_hook::last_present_tid());
        }
        g_lastBeatMs = now;
        g_beatDraws = g_draws.load(std::memory_order_relaxed);
        g_beatSecond = g_secondDraws.load(std::memory_order_relaxed);
        g_beatPresents = bvr::d3d11_hook::present_count();
        g_beatCamReplays = camera::second_pass_replays();
    }
}

void __fastcall DrawDetour(void* self, void* edx, void* a1) {
    const uint32_t tid = GetCurrentThreadId();
    const int depth = g_depth.fetch_add(1, std::memory_order_relaxed);
    const uint32_t callerRva = to_rva(_ReturnAddress());
    if (depth == 0) {
        g_drawTid.store(tid, std::memory_order_relaxed);
        note_caller(callerRva);
        g_draws.fetch_add(1, std::memory_order_relaxed);
        // Pass-1 eye tag, pushed BEFORE the original so the tag is in the
        // ring by the time this pass's Present pops it. Gated exactly like
        // the doubling so tags and doubles can never go one-sided.
        if (g_stereo.load(std::memory_order_relaxed) &&
            !g_poisoned.load(std::memory_order_relaxed) &&
            callerRva == patterns::kViewportDrawGameplayRetRva &&
            camera::silent_ms() <= 400)
            bvr::vr::sr_push_eye(-1);
    }

    g_original(self, edx, a1);

    if (depth == 0) {
        const uint64_t present = bvr::d3d11_hook::present_count();
        const uint64_t presentDelta = present - g_lastDrawPresent;
        g_lastDrawPresent = present;
        // Depth latch still held: pass 2's camera dispatches attribute as
        // "inside" and the pump stays deferred for the whole pair.
        maybe_second_draw(self, edx, a1, callerRva, presentDelta);
        heartbeat(GetTickCount64());
    }
    g_depth.fetch_sub(1, std::memory_order_relaxed);
}

void log_status() {
    BVR_LOG("[bsi][reentry] status: hook=%s stereo=%s poisoned=%s draws=%u 2nd=%u "
            "camReplays=%u skips foreign=%u stereo=%u call2=%u us lastExc=0x%08X@0x%X",
            g_hookLive.load() ? "installed" : "NOT installed",
            g_stereo.load() ? "ARMED" : "off", g_poisoned.load() ? "YES" : "no",
            g_draws.load(), g_secondDraws.load(), camera::second_pass_replays(),
            g_foreignCallerSkips.load(), g_stereoSkips.load(), g_call2Us.load(),
            g_lastExcCode.load(), g_lastExcRva.load());
    for (auto& slot : g_callers) {
        const uint32_t rva = slot.rva.load(std::memory_order_relaxed);
        if (!rva) break;
        BVR_LOG("[bsi][reentry] status: caller ret 0x%08X count=%u%s", rva,
                slot.count.load(std::memory_order_relaxed),
                rva == patterns::kViewportDrawGameplayRetRva ? " (the gameplay dispatch)"
                                                             : "");
    }
}

} // namespace

bool install() {
    if (g_hookLive.load()) return true;
    if (!patterns::rva_trusted()) {
        BVR_LOG("[bsi][reentry] hook NOT installed - build gate closed");
        return false;
    }
    const uint8_t* target = patterns::rva_to_address(patterns::kViewportDrawRva, 64);
    if (!target) {
        BVR_LOG("[bsi][reentry] hook NOT installed - RVA 0x%X unreadable",
                patterns::kViewportDrawRva);
        return false;
    }
    if (memcmp(target, patterns::kViewportDrawPrologue,
               sizeof patterns::kViewportDrawPrologue) != 0) {
        BVR_LOG("[bsi][reentry] prologue MISMATCH at RVA 0x%X - REFUSING hook",
                patterns::kViewportDrawRva);
        return false;
    }
    // Cross-check the derivation chain live before patching code: the client
    // draw's vtable slot (+0x8 at kSceneDrawVtableRva) must still hold the
    // jmp stub landing on the client-draw body this root dispatches into. A
    // build where the chain moved refuses.
    const uint8_t* vt = patterns::rva_to_address(patterns::kSceneDrawVtableRva, 12);
    if (vt) {
        const uint32_t slot2 = *reinterpret_cast<const uint32_t*>(vt + 8);
        const patterns::Symbols& s = patterns::symbols();
        const uint32_t stubRva =
            slot2 - static_cast<uint32_t>(reinterpret_cast<uintptr_t>(s.imageBase));
        if (stubRva != patterns::kSceneDrawStubRva) {
            BVR_LOG("[bsi][reentry] client-draw vtable slot 2 holds RVA 0x%X, expected "
                    "stub 0x%X - derivation chain broken, REFUSING hook",
                    stubRva, patterns::kSceneDrawStubRva);
            return false;
        }
    }
    void* addr = const_cast<uint8_t*>(target);
    MH_STATUS status = MH_CreateHook(addr, reinterpret_cast<void*>(&DrawDetour),
                                     reinterpret_cast<void**>(&g_original));
    if (status != MH_OK) {
        BVR_LOG("[bsi][reentry] MH_CreateHook failed: %s", MH_StatusToString(status));
        return false;
    }
    status = MH_EnableHook(addr);
    if (status != MH_OK) {
        BVR_LOG("[bsi][reentry] MH_EnableHook failed: %s", MH_StatusToString(status));
        MH_RemoveHook(addr);
        g_original = nullptr;
        return false;
    }
    g_target = addr;
    g_hookLive.store(true, std::memory_order_relaxed);
    BVR_LOG("[bsi][reentry] hook installed on the viewport draw root (RVA 0x%X, prologue "
            "and client-draw chain both verified). Doubling ships OFF - the detour "
            "observes until stereo arms.",
            patterns::kViewportDrawRva);
    return true;
}

bool hook_live() {
    return g_hookLive.load(std::memory_order_relaxed);
}

bool stereo_active() {
    return g_stereo.load(std::memory_order_relaxed);
}

bool set_stereo(bool on) {
    if (on) {
        if (!g_hookLive.load(std::memory_order_relaxed)) {
            BVR_LOG("[bsi][reentry] stereo refused - hook not installed");
            return false;
        }
        if (g_poisoned.load(std::memory_order_relaxed)) {
            BVR_LOG("[bsi][reentry] stereo refused - POISONED (`reentry reset` to clear)");
            return false;
        }
        g_stereo.store(true, std::memory_order_relaxed);
        BVR_LOG("[bsi][reentry] STEREO ARMED - doubling per gameplay tick (threaded "
                "substrate, no 1t rung by design - DR-I5)");
    } else {
        g_stereo.store(false, std::memory_order_relaxed);
        BVR_LOG("[bsi][reentry] stereo off");
    }
    return true;
}

bool second_pass_for_current_thread() {
    return g_secondPassTid.load(std::memory_order_relaxed) == GetCurrentThreadId();
}

bool in_second_draw() {
    return g_inSecondDraw.load(std::memory_order_relaxed);
}

uint32_t second_pass_seq() {
    return g_secondPassSeq.load(std::memory_order_relaxed);
}

bool handle_command(const char* args) {
    if (!args) args = "";
    while (*args == ' ') ++args;
    if (strncmp(args, "status", 6) == 0 || *args == '\0') {
        log_status();
        return true;
    }
    if (strncmp(args, "reset", 5) == 0) {
        g_poisoned.store(false, std::memory_order_relaxed);
        g_lastExcCode.store(0, std::memory_order_relaxed);
        g_lastExcRva.store(0, std::memory_order_relaxed);
        BVR_LOG("[bsi][reentry] poison cleared");
        return true;
    }
    if (strncmp(args, "pulse", 5) == 0) {
        int n = 1;
        sscanf_s(args + 5, "%d", &n);
        if (n < 1) n = 1;
        if (n > 300) n = 300;
        if (!g_hookLive.load(std::memory_order_relaxed)) {
            BVR_LOG("[bsi][reentry] pulse refused - hook not installed (vrstereo on "
                    "installs it)");
            return true;
        }
        g_pulseCount.store(n, std::memory_order_relaxed);
        BVR_LOG("[bsi][reentry] pulse: doubling the next %d gameplay draw(s)", n);
        return true;
    }
    if (strncmp(args, "stereo", 6) == 0) {
        const char* v = args + 6;
        while (*v == ' ') ++v;
        set_stereo(strncmp(v, "on", 2) == 0);
        return true;
    }
    return false;
}

void draw_debug_ui() {
    if (!ImGui::CollapsingHeader("Reentry / SR stereo (I5)")) return;
    ImGui::Text("hook: %s   stereo: %s   poisoned: %s",
                g_hookLive.load() ? "installed" : "not installed",
                g_stereo.load() ? "ARMED" : "off", g_poisoned.load() ? "YES" : "no");
    ImGui::Text("draws %u | 2nd %u | camReplays %u", g_draws.load(), g_secondDraws.load(),
                camera::second_pass_replays());
    ImGui::Text("skips: foreign %u stereo %u | call2 %u us", g_foreignCallerSkips.load(),
                g_stereoSkips.load(), g_call2Us.load());
    ImGui::Text("tid: draw %u present %u", g_drawTid.load(),
                bvr::d3d11_hook::last_present_tid());
}

} // namespace bvr::bsi::scenedraw
