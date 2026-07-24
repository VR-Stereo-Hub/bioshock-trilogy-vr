// DR-5 probe (SequentialReentry groundwork), v3: the seam is the GAME-THREAD
// frame SUBMIT (kFrameSubmitRva) - it stores the camera into the
// submitted-frame globals and SetEvents the render pump, which drains once
// per Present (ENGINE_NOTES "Scene-draw architecture"; drain re-entry is
// REFUTED - the v2 drain/flush hooks remain only as instruments). The submit
// hook adds per-call arg telemetry (loc/rot/arg3 identity, presents-delta)
// and the double-submit: call the original a second time with COPIED loc/rot
// args, yaw delta on the rot copy - a second full engine frame per game
// tick. Hook lifecycle stays command-gated end to end; second original calls
// are SEH-guarded with a poison latch.

#include "game/bioshock1r/scenedraw.h"

#include "core/gfx/frame_inspector.h"
#include "core/hooks/d3d11_hook.h"
#include "core/util/log.h"
#include "core/vr/openxr_runtime.h"
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

// Drain/flush are void __thiscall with zero stack args; __fastcall with a
// dummy EDX slot is register/stack/cleanup-identical - same trick as
// CalcViewDetour.
using RenderFn = void(__fastcall*)(void* self, void* edx);

// The frame submit is `ret 0xC` (3 stack args, callee-clean) with a DEAD ECX
// at entry (disk-image byte walk 2026-07-24: the prologue `push ecx` is stack
// alloc; the first ECX use is `mov ecx,[ebp+0x10]` loading arg3). A
// __fastcall detour with ECX/EDX passthrough + 3 stack params is
// stack/cleanup-identical, and safe even if ECX were live.
struct FVec3 { float x, y, z; };
struct FRot3 { int32_t pitch, yaw, roll; }; // 65536 units per full turn
constexpr float kSubmitRotUnitsPerDeg = 65536.0f / 360.0f;
using SubmitFn = void(__fastcall*)(void* ecx, void* edx, FVec3* loc,
                                   FRot3* rot, void* arg3);

// The scene BUILD root (kSceneBuildRva) is `ret 0x10` with a live ECX this
// and 4 stack args; same fastcall-passthrough trick, 4 stack params. The
// second call passes the ORIGINAL args through unchanged - the camera enters
// via CalcView re-running inside the build (CalcViewDetour applies the yaw
// during our second pass; see second_pass_for_current_thread).
using BuildFn = void(__fastcall*)(void* ecx, void* edx, void* a1, void* a2,
                                  void* a3, void* a4);

const uint8_t* g_imageBase = nullptr;
size_t g_imageSize = 0;

struct HookSlot {
    const char* name = nullptr;
    void* target = nullptr;
    void* original = nullptr;         // trampoline; cast to the slot's signature
    bool created = false;             // game thread only
    std::atomic<bool> enabled{false};
};
HookSlot g_drain{"drain"};
HookSlot g_flush{"flush"};
HookSlot g_submit{"submit"};
HookSlot g_build{"build"};

// Controls: command poller (game thread) writes, detour threads read.
std::atomic<bool>  g_doubleCall{false};
std::atomic<int>   g_pulseCount{0};     // pending one-shot double calls
std::atomic<float> g_secondYawDeg{0.0f};
std::atomic<bool>  g_latchClear{false}; // zero [queue+0x58] before 2nd call
std::atomic<uint32_t> g_arg3Filter{0};  // double only submits whose arg3 low
                                        // dword matches; 0 = any call
std::atomic<int>   g_dumpRemaining{0};  // per-call submit dump lines left
// M4 rung 2: SequentialReentry stereo. While on, every build is doubled
// (pass 1 = left eye, pass 2 = right - camera.cpp applies the offsets) and
// each nested submit pushes its eye tag to core/vr for per-Present capture.
std::atomic<bool>  g_stereo{false};
std::atomic<uint32_t> g_stereoSkips{0}; // second calls skipped (render stalled)
std::atomic<bool>  g_poisoned{false};
std::atomic<uint32_t> g_lastExcCode{0};
std::atomic<uint32_t> g_lastExcRva{0};

// Telemetry. g_activeTid is nonzero exactly while a depth-0 hooked call is
// in flight on that thread; g_secondPassTid likewise for the re-entry call.
std::atomic<uint32_t> g_drainEntries{0};
std::atomic<uint32_t> g_flushEntries{0};
std::atomic<uint32_t> g_submitEntries{0};
std::atomic<uint32_t> g_buildEntries{0};
std::atomic<uint32_t> g_submitNested{0}; // submits seen at depth>0 (in build)
std::atomic<uint32_t> g_secondCalls{0};
std::atomic<uint32_t> g_activeTid{0};
std::atomic<int>      g_activeDepth{0};
std::atomic<uint32_t> g_secondPassTid{0};
std::atomic<uint32_t> g_calcInside{0};
std::atomic<uint32_t> g_calcOutside{0};
std::atomic<uint32_t> g_secondPassHits{0};
std::atomic<uint32_t> g_lastCalcTid{0};
std::atomic<uint32_t> g_call1Us{0}, g_call2Us{0};
std::atomic<uint32_t> g_lastSecondDraws{0};
std::atomic<uint32_t> g_callerRvas[4]{};

// Heartbeat bookkeeping - beat thread only (whichever detour beats).
uint64_t g_lastBeatMs = 0;
uint32_t g_beatDrain = 0, g_beatFlush = 0, g_beatSubmit = 0, g_beatBuild = 0,
         g_beatSecond = 0, g_beatCalcIn = 0, g_beatCalcOut = 0;
uint64_t g_beatPresents = 0;

// Submit (game) thread only: present count at the previous submit entry, for
// the submits-per-present instrument in the dump lines.
uint32_t g_lastSubmitPresentLow = 0;
// Build (game) thread only: present count at the previous build entry. The
// double-call is skipped while presents are stalled (unfocused window /
// hitching render thread) - doubling has no value then and the stall is the
// prime suspect in the one observed continuous-mode hang (TESTING.md).
uint32_t g_lastBuildPresentLow = 0;

// SetEvent kick sampler (game-thread build/kick discovery). Table of
// distinct (tid, caller-rva) pairs with counts; dumped on "kick off".
using SetEventFn = BOOL(WINAPI*)(HANDLE);
SetEventFn g_origSetEvent = nullptr;
void* g_setEventTarget = nullptr;
bool g_kickCreated = false;           // game thread only
std::atomic<bool> g_kickSampling{false};
struct KickSlot {
    std::atomic<uint32_t> key{0};
    std::atomic<uint32_t> tid{0};
    std::atomic<uint32_t> rva{0};
    std::atomic<uint32_t> count{0};
};
KickSlot g_kickSlots[10];

// One-shot CalcView stack scan request.
std::atomic<int> g_calcstackPending{0};

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

bool call_submit_guarded(SubmitFn fn, void* ecx, void* edx, FVec3* loc,
                         FRot3* rot, void* arg3) {
    __try {
        fn(ecx, edx, loc, rot, arg3);
        return true;
    } __except (reentry_filter(GetExceptionCode(), GetExceptionInformation())) {
        return false;
    }
}

bool call_build_guarded(BuildFn fn, void* ecx, void* edx, void* a1, void* a2,
                        void* a3, void* a4) {
    __try {
        fn(ecx, edx, a1, a2, a3, a4);
        return true;
    } __except (reentry_filter(GetExceptionCode(), GetExceptionInformation())) {
        return false;
    }
}

// Guarded arg snapshot (dump + double-submit): the pointers come straight
// from game code - never trust them.
bool copy_submit_args(const FVec3* loc, const FRot3* rot, FVec3* locOut,
                      FRot3* rotOut) {
    __try {
        *locOut = *loc;
        *rotOut = *rot;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// arg3 identity for the dump lines: engine objects lead with a vtable.
uint32_t read_arg3_vtable_rva(void* arg3) {
    __try {
        return to_rva(*static_cast<void**>(arg3));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0xFFFFFFFFu;
    }
}

// Zero [queue+0x58] (queue = [mgr+4]). Session-5 correction: that field is
// the PUMP EXIT flag, not a per-frame latch - kept only as an experiment
// lever, default off.
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

void heartbeat(uint32_t beatTid) {
    uint64_t now = GetTickCount64();
    if (g_lastBeatMs == 0) {
        // Seed every base on the first beat so the first printed line is a
        // true 1 s delta (not counters-since-boot).
        g_lastBeatMs = now;
        g_beatDrain = g_drainEntries.load(std::memory_order_relaxed);
        g_beatFlush = g_flushEntries.load(std::memory_order_relaxed);
        g_beatSubmit = g_submitEntries.load(std::memory_order_relaxed);
        g_beatBuild = g_buildEntries.load(std::memory_order_relaxed);
        g_beatSecond = g_secondCalls.load(std::memory_order_relaxed);
        g_beatCalcIn = g_calcInside.load(std::memory_order_relaxed);
        g_beatCalcOut = g_calcOutside.load(std::memory_order_relaxed);
        g_beatPresents = bvr::d3d11_hook::present_count();
        return;
    }
    if (now - g_lastBeatMs < 1000) return;
    g_lastBeatMs = now;

    uint32_t drains = g_drainEntries.load(std::memory_order_relaxed);
    uint32_t flushes = g_flushEntries.load(std::memory_order_relaxed);
    uint32_t submits = g_submitEntries.load(std::memory_order_relaxed);
    uint32_t builds = g_buildEntries.load(std::memory_order_relaxed);
    uint32_t seconds = g_secondCalls.load(std::memory_order_relaxed);
    uint32_t calcIn = g_calcInside.load(std::memory_order_relaxed);
    uint32_t calcOut = g_calcOutside.load(std::memory_order_relaxed);
    uint64_t presents = bvr::d3d11_hook::present_count();
    BVR_LOG("[reentry] drain=%u/s flush=%u/s build=%u/s submit=%u/s "
            "(nested=%u) 2nd=%u/s presents=%u/s "
            "calcview in=%u out=%u/s call1=%uus call2=%uus beatTid=%u "
            "calcTid=%u callers=%X,%X,%X,%X%s",
            drains - g_beatDrain, flushes - g_beatFlush,
            builds - g_beatBuild, submits - g_beatSubmit,
            g_submitNested.load(std::memory_order_relaxed),
            seconds - g_beatSecond,
            static_cast<uint32_t>(presents - g_beatPresents),
            calcIn - g_beatCalcIn, calcOut - g_beatCalcOut,
            g_call1Us.load(std::memory_order_relaxed),
            g_call2Us.load(std::memory_order_relaxed), beatTid,
            g_lastCalcTid.load(std::memory_order_relaxed),
            g_callerRvas[0].load(std::memory_order_relaxed),
            g_callerRvas[1].load(std::memory_order_relaxed),
            g_callerRvas[2].load(std::memory_order_relaxed),
            g_callerRvas[3].load(std::memory_order_relaxed),
            g_poisoned.load(std::memory_order_relaxed) ? " POISONED" : "");
    g_beatDrain = drains;
    g_beatFlush = flushes;
    g_beatSubmit = submits;
    g_beatBuild = builds;
    g_beatSecond = seconds;
    g_beatCalcIn = calcIn;
    g_beatCalcOut = calcOut;
    g_beatPresents = presents;
}

// Second-call machinery, shared by both detours (runs at depth 0 only).
void maybe_second_call(HookSlot& slot, void* self, void* edx, uint32_t tid) {
    bool want = false;
    bool isPulse = false;
    if (g_poisoned.load(std::memory_order_relaxed)) return;
    if (g_doubleCall.load(std::memory_order_relaxed)) {
        want = true;
    } else {
        int pulses = g_pulseCount.load(std::memory_order_relaxed);
        while (pulses > 0 &&
               !g_pulseCount.compare_exchange_weak(pulses, pulses - 1,
                                                   std::memory_order_relaxed)) {}
        if (pulses > 0) { want = true; isPulse = true; }
    }
    if (!want) return;

    uint64_t drawsBefore = bvr::frame_inspector::draw_call_census();
    if (g_latchClear.load(std::memory_order_relaxed)) clear_drain_latch();
    LARGE_INTEGER t2, t3;
    QueryPerformanceCounter(&t2);
    g_secondPassTid.store(tid, std::memory_order_relaxed);
    bool ok = call_original_guarded(reinterpret_cast<RenderFn>(slot.original),
                                    self, edx);
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
        BVR_LOG("[reentry] %s second call FAULTED code=0x%08X rva=0x%X - POISONED "
                "(hook stays pass-through; 'reentry reset' to clear)",
                slot.name, g_lastExcCode.load(std::memory_order_relaxed),
                g_lastExcRva.load(std::memory_order_relaxed));
    } else if (isPulse) {
        BVR_LOG("[reentry] %s pulse ok: call1=%uus call2=%uus draws2=%u yaw=%.1f "
                "latchclear=%d",
                slot.name, g_call1Us.load(std::memory_order_relaxed), call2Us,
                draws2, g_secondYawDeg.load(std::memory_order_relaxed),
                g_latchClear.load(std::memory_order_relaxed) ? 1 : 0);
    }
}

// Submit-specific double-call (game thread, depth 0 only): same
// controls/poison as maybe_second_call, but the second call gets COPIED
// loc/rot args with the yaw delta on the rot copy (the originals belong to
// the engine), and an optional arg3 filter targets one call site once the
// dump telemetry identifies the main-scene submit.
void maybe_second_submit(void* ecx, void* edx, FVec3* loc, FRot3* rot,
                         void* arg3, uint32_t tid) {
    if (g_poisoned.load(std::memory_order_relaxed)) return;
    // The build slot owns the double-call controls while it is enabled
    // (submit-alone double-calls are absorbed by the engine - live-verified
    // 2026-07-24: presents did not double, the yawed camera never rendered).
    if (g_build.enabled.load(std::memory_order_relaxed)) return;
    uint32_t filter = g_arg3Filter.load(std::memory_order_relaxed);
    if (filter &&
        static_cast<uint32_t>(reinterpret_cast<uintptr_t>(arg3)) != filter)
        return;
    bool want = false;
    bool isPulse = false;
    if (g_doubleCall.load(std::memory_order_relaxed)) {
        want = true;
    } else {
        int pulses = g_pulseCount.load(std::memory_order_relaxed);
        while (pulses > 0 &&
               !g_pulseCount.compare_exchange_weak(pulses, pulses - 1,
                                                   std::memory_order_relaxed)) {}
        if (pulses > 0) { want = true; isPulse = true; }
    }
    if (!want) return;

    FVec3 locCopy;
    FRot3 rotCopy;
    if (!copy_submit_args(loc, rot, &locCopy, &rotCopy)) {
        BVR_LOG("[reentry] submit second call SKIPPED: loc/rot unreadable (%p/%p)",
                loc, rot);
        return;
    }
    rotCopy.yaw += static_cast<int32_t>(
        g_secondYawDeg.load(std::memory_order_relaxed) * kSubmitRotUnitsPerDeg);

    LARGE_INTEGER t2, t3;
    QueryPerformanceCounter(&t2);
    g_secondPassTid.store(tid, std::memory_order_relaxed);
    bool ok = call_submit_guarded(reinterpret_cast<SubmitFn>(g_submit.original),
                                  ecx, edx, &locCopy, &rotCopy, arg3);
    g_secondPassTid.store(0, std::memory_order_relaxed); // also on fault path
    QueryPerformanceCounter(&t3);
    uint32_t call2Us = qpc_us(t2, t3);
    g_call2Us.store(call2Us, std::memory_order_relaxed);
    g_secondCalls.fetch_add(1, std::memory_order_relaxed);
    if (!ok) {
        g_poisoned.store(true, std::memory_order_relaxed);
        g_doubleCall.store(false, std::memory_order_relaxed);
        g_pulseCount.store(0, std::memory_order_relaxed);
        BVR_LOG("[reentry] submit second call FAULTED code=0x%08X rva=0x%X - "
                "POISONED (hook stays pass-through; 'reentry reset' to clear)",
                g_lastExcCode.load(std::memory_order_relaxed),
                g_lastExcRva.load(std::memory_order_relaxed));
    } else if (isPulse) {
        // The extra Present is async on the render thread; auto-arm a few
        // dump lines so the presentD blip shows without a second command.
        g_dumpRemaining.store(4, std::memory_order_relaxed);
        BVR_LOG("[reentry] submit pulse ok: call1=%uus call2=%uus yaw=%.1f "
                "arg3=%p (watch presentD on the next dump lines)",
                g_call1Us.load(std::memory_order_relaxed), call2Us,
                g_secondYawDeg.load(std::memory_order_relaxed), arg3);
    }
}

// Second call of the scene build: original args passed through unchanged;
// the yaw enters via CalcViewDetour's second-pass path (g_secondPassTid).
// Runs at depth 0 only, game thread.
void maybe_second_build(void* ecx, void* edx, void* a1, void* a2, void* a3,
                        void* a4, uint32_t tid, uint32_t presentDelta) {
    if (g_poisoned.load(std::memory_order_relaxed)) return;
    bool stereo = g_stereo.load(std::memory_order_relaxed);
    bool want = false;
    bool isPulse = false;
    if (g_doubleCall.load(std::memory_order_relaxed) || stereo) {
        want = true;
    } else {
        int pulses = g_pulseCount.load(std::memory_order_relaxed);
        while (pulses > 0 &&
               !g_pulseCount.compare_exchange_weak(pulses, pulses - 1,
                                                   std::memory_order_relaxed)) {}
        if (pulses > 0) { want = true; isPulse = true; }
    }
    if (!want) return;

    // Stall guard 1 (reactive): no present since the previous build means the
    // render thread is paused (unfocused) or wedged - do not stack a second
    // frame onto a stalled pipeline.
    if (presentDelta == 0) {
        g_stereoSkips.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    // Stall guard 2 (predictive - session 6, THREE live hangs): the engine's
    // command-ring event protocol has a lost-wakeup race between "ring full"
    // (game thread, build) and "ring empty" (render thread, drain) waits -
    // hang thread-dump: game thread waiting at exe+0x61D38E from build site
    // 0x4CDCD7, render thread waiting inside the drain at +0x30, both
    // stranded. Drain the pipeline OURSELVES before the second call: poll
    // until both frame-id completion bits are set AND the command ring's
    // producer/consumer cursors are equal (ecx = the build's queue object;
    // +0x118/+0x11C is the same cursor pair the submit call-site gate
    // checks). Starting pass 2 against a truly idle ring keeps the engine's
    // racy full/empty waits from engaging. Bounded, skip on timeout (render
    // paused/stalled). A poll cannot lose a wakeup.
    const volatile int32_t* frameA = reinterpret_cast<const volatile int32_t*>(
        g_imageBase + patterns::kFrameIdPairRva);
    const volatile int32_t* frameB = reinterpret_cast<const volatile int32_t*>(
        g_imageBase + patterns::kFrameIdPairRva + patterns::kFrameIdSecondOffset);
    const volatile uint32_t* ringProd = reinterpret_cast<const volatile uint32_t*>(
        static_cast<const uint8_t*>(ecx) + patterns::kQueueSegProdOffset);
    const volatile uint32_t* ringCons = reinterpret_cast<const volatile uint32_t*>(
        static_cast<const uint8_t*>(ecx) + patterns::kQueueSegConsOffset);
    LARGE_INTEGER w0, w1;
    QueryPerformanceCounter(&w0);
    bool drained = false;
    for (;;) {
        if (*frameA < 0 && *frameB < 0 && *ringProd == *ringCons) {
            drained = true;
            break;
        }
        QueryPerformanceCounter(&w1);
        if (qpc_us(w0, w1) > 20000) break; // 20 ms: pump is stalled
        YieldProcessor();
    }
    if (!drained) {
        g_stereoSkips.fetch_add(1, std::memory_order_relaxed);
        if (isPulse)
            BVR_LOG("[reentry] pulse skipped: pipeline never drained "
                    "(render stalled)");
        return;
    }

    uint32_t presentsBefore =
        static_cast<uint32_t>(bvr::d3d11_hook::present_count());
    uint32_t submitsBefore = g_submitEntries.load(std::memory_order_relaxed);
    LARGE_INTEGER t2, t3;
    QueryPerformanceCounter(&t2);
    g_secondPassTid.store(tid, std::memory_order_relaxed);
    bool ok = call_build_guarded(reinterpret_cast<BuildFn>(g_build.original),
                                 ecx, edx, a1, a2, a3, a4);
    g_secondPassTid.store(0, std::memory_order_relaxed); // also on fault path
    QueryPerformanceCounter(&t3);
    uint32_t call2Us = qpc_us(t2, t3);
    g_call2Us.store(call2Us, std::memory_order_relaxed);
    g_secondCalls.fetch_add(1, std::memory_order_relaxed);
    if (!ok) {
        g_poisoned.store(true, std::memory_order_relaxed);
        g_doubleCall.store(false, std::memory_order_relaxed);
        g_pulseCount.store(0, std::memory_order_relaxed);
        BVR_LOG("[reentry] build second call FAULTED code=0x%08X rva=0x%X - "
                "POISONED (hook stays pass-through; 'reentry reset' to clear)",
                g_lastExcCode.load(std::memory_order_relaxed),
                g_lastExcRva.load(std::memory_order_relaxed));
    } else if (isPulse) {
        // submitsD > 0 proves the second build re-submitted; presentsD needs
        // a beat for the render thread to catch up - the auto-armed dump
        // lines show it.
        g_dumpRemaining.store(4, std::memory_order_relaxed);
        BVR_LOG("[reentry] build pulse ok: call1=%uus call2=%uus yaw=%.1f "
                "submitsD=%u presentsD=%u 2nd-pass calc hits=%u",
                g_call1Us.load(std::memory_order_relaxed), call2Us,
                g_secondYawDeg.load(std::memory_order_relaxed),
                g_submitEntries.load(std::memory_order_relaxed) - submitsBefore,
                static_cast<uint32_t>(bvr::d3d11_hook::present_count()) -
                    presentsBefore,
                g_secondPassHits.load(std::memory_order_relaxed));
    }
}

void __fastcall BuildDetour(void* ecx, void* edx, void* a1, void* a2, void* a3,
                            void* a4) {
    uint32_t tid = GetCurrentThreadId();
    int depth = g_activeDepth.fetch_add(1, std::memory_order_relaxed);
    if (depth == 0) g_activeTid.store(tid, std::memory_order_relaxed);
    uint32_t n = g_buildEntries.fetch_add(1, std::memory_order_relaxed) + 1;
    note_caller(to_rva(_ReturnAddress()));
    if (n <= 2)
        BVR_LOG("[reentry] build fired #%u (tid %u, caller 0x%X, ecx %p, a1 %p)",
                n, tid, to_rva(_ReturnAddress()), ecx, a1);

    uint32_t presentLow =
        static_cast<uint32_t>(bvr::d3d11_hook::present_count());
    uint32_t presentDelta = presentLow - g_lastBuildPresentLow;
    g_lastBuildPresentLow = presentLow;

    LARGE_INTEGER t0, t1;
    QueryPerformanceCounter(&t0);
    reinterpret_cast<BuildFn>(g_build.original)(ecx, edx, a1, a2, a3,
                                                a4); // never guarded
    QueryPerformanceCounter(&t1);
    g_call1Us.store(qpc_us(t0, t1), std::memory_order_relaxed);

    if (depth == 0)
        maybe_second_build(ecx, edx, a1, a2, a3, a4, tid, presentDelta);

    if (!g_drain.enabled.load(std::memory_order_relaxed) &&
        !g_flush.enabled.load(std::memory_order_relaxed))
        heartbeat(tid);
    if (g_activeDepth.fetch_sub(1, std::memory_order_relaxed) == 1)
        g_activeTid.store(0, std::memory_order_relaxed);
}

void __fastcall SubmitDetour(void* ecx, void* edx, FVec3* loc, FRot3* rot,
                             void* arg3) {
    uint32_t tid = GetCurrentThreadId();
    int depth = g_activeDepth.fetch_add(1, std::memory_order_relaxed);
    if (depth == 0) {
        g_activeTid.store(tid, std::memory_order_relaxed);
    } else {
        g_submitNested.fetch_add(1, std::memory_order_relaxed);
        // Stereo eye tag: this submit's frame will Present exactly once;
        // tell core/vr which eye it carries (pass 2 = right).
        if (g_stereo.load(std::memory_order_relaxed)) {
            bool pass2 = g_secondPassTid.load(std::memory_order_relaxed) == tid;
            bvr::vr::sr_push_eye(pass2 ? +1 : -1);
        }
    }
    g_submitEntries.fetch_add(1, std::memory_order_relaxed);
    note_caller(to_rva(_ReturnAddress()));

    uint32_t presentLow =
        static_cast<uint32_t>(bvr::d3d11_hook::present_count());
    uint32_t presentDelta = presentLow - g_lastSubmitPresentLow;
    g_lastSubmitPresentLow = presentLow;

    int dump = g_dumpRemaining.load(std::memory_order_relaxed);
    while (dump > 0 &&
           !g_dumpRemaining.compare_exchange_weak(dump, dump - 1,
                                                  std::memory_order_relaxed)) {}
    if (dump > 0) {
        FVec3 l{};
        FRot3 r{};
        bool argsOk = copy_submit_args(loc, rot, &l, &r);
        BVR_LOG("[reentry] submit #%u tid=%u presentD=%u caller=0x%X "
                "loc=(%.1f,%.1f,%.1f) rot=(%d,%d,%d)=(%.1f,%.1f,%.1f)deg "
                "arg3=%p vt=0x%X%s",
                g_submitEntries.load(std::memory_order_relaxed), tid,
                presentDelta, to_rva(_ReturnAddress()), l.x, l.y, l.z, r.pitch,
                r.yaw, r.roll, r.pitch / kSubmitRotUnitsPerDeg,
                r.yaw / kSubmitRotUnitsPerDeg, r.roll / kSubmitRotUnitsPerDeg,
                arg3, read_arg3_vtable_rva(arg3),
                argsOk ? "" : " ARGS-UNREADABLE");
    }

    LARGE_INTEGER t0, t1;
    QueryPerformanceCounter(&t0);
    reinterpret_cast<SubmitFn>(g_submit.original)(ecx, edx, loc, rot,
                                                  arg3); // never guarded
    QueryPerformanceCounter(&t1);
    g_call1Us.store(qpc_us(t0, t1), std::memory_order_relaxed);

    if (depth == 0) maybe_second_submit(ecx, edx, loc, rot, arg3, tid);

    // Single beat writer: drain wins, then flush (existing rule), then us.
    if (!g_drain.enabled.load(std::memory_order_relaxed) &&
        !g_flush.enabled.load(std::memory_order_relaxed))
        heartbeat(tid);
    if (g_activeDepth.fetch_sub(1, std::memory_order_relaxed) == 1)
        g_activeTid.store(0, std::memory_order_relaxed);
}

void __fastcall DrainDetour(void* self, void* edx) {
    uint32_t tid = GetCurrentThreadId();
    int depth = g_activeDepth.fetch_add(1, std::memory_order_relaxed);
    if (depth == 0) g_activeTid.store(tid, std::memory_order_relaxed);
    g_drainEntries.fetch_add(1, std::memory_order_relaxed);
    note_caller(to_rva(_ReturnAddress()));

    LARGE_INTEGER t0, t1;
    QueryPerformanceCounter(&t0);
    reinterpret_cast<RenderFn>(g_drain.original)(self, edx); // never guarded
    QueryPerformanceCounter(&t1);
    g_call1Us.store(qpc_us(t0, t1), std::memory_order_relaxed);

    if (depth == 0) maybe_second_call(g_drain, self, edx, tid);

    heartbeat(tid);
    if (g_activeDepth.fetch_sub(1, std::memory_order_relaxed) == 1)
        g_activeTid.store(0, std::memory_order_relaxed);
}

void __fastcall FlushDetour(void* self, void* edx) {
    uint32_t tid = GetCurrentThreadId();
    g_flushEntries.fetch_add(1, std::memory_order_relaxed);
    note_caller(to_rva(_ReturnAddress()));
    reinterpret_cast<RenderFn>(g_flush.original)(self, edx);
    // A flush firing at all is news - log the first few immediately.
    uint32_t n = g_flushEntries.load(std::memory_order_relaxed);
    if (n <= 3)
        BVR_LOG("[reentry] flush fired #%u (tid %u, caller 0x%X)", n, tid,
                to_rva(_ReturnAddress()));
    if (!g_drain.enabled.load(std::memory_order_relaxed)) heartbeat(tid);
}

BOOL WINAPI SetEventDetour(HANDLE h) {
    if (g_kickSampling.load(std::memory_order_relaxed)) {
        uint32_t tid = GetCurrentThreadId();
        uint32_t rva = to_rva(_ReturnAddress());
        uint32_t key = (tid * 2654435761u) ^ rva;
        if (key == 0) key = 1;
        for (auto& s : g_kickSlots) {
            uint32_t k = s.key.load(std::memory_order_relaxed);
            if (k == key) { s.count.fetch_add(1, std::memory_order_relaxed); break; }
            if (k == 0) {
                if (s.key.compare_exchange_strong(k, key, std::memory_order_relaxed)) {
                    s.tid.store(tid, std::memory_order_relaxed);
                    s.rva.store(rva, std::memory_order_relaxed);
                    s.count.store(1, std::memory_order_relaxed);
                    break;
                }
            }
        }
    }
    return g_origSetEvent(h);
}

// Conservative one-shot stack scan on the game thread (from inside the
// CalcView detour): log every stack dword that points into the exe image AND
// is preceded by a plausible CALL encoding. Same heuristic family as
// frame_inspector's capture. One log line; game thread only.
void log_game_stack() {
    void** sp = reinterpret_cast<void**>(_AddressOfReturnAddress());
    char line[512];
    int pos = 0;
    int found = 0;
    for (int i = 0; i < 2048 && found < 24 && pos < 480; ++i) {
        if (!bvr::pattern_scan::is_memory_valid(&sp[i], sizeof(void*))) break;
        const uint8_t* p = static_cast<const uint8_t*>(sp[i]);
        uint32_t rva = to_rva(p);
        if (rva == 0xFFFFFFFFu) continue;
        if (!bvr::pattern_scan::is_memory_valid(p - 6, 6)) continue;
        bool call = p[-5] == 0xE8 ||                                  // call rel32
                    (p[-6] == 0xFF && p[-5] == 0x15) ||               // call [m32]
                    (p[-6] == 0xFF && p[-5] >= 0x90 && p[-5] <= 0x97) || // call [reg+d32]
                    (p[-2] == 0xFF && p[-1] >= 0xD0 && p[-1] <= 0xD7) || // call reg
                    (p[-2] == 0xFF && p[-1] >= 0x10 && p[-1] <= 0x17) || // call [reg]
                    (p[-3] == 0xFF && p[-2] >= 0x50 && p[-2] <= 0x57);   // call [reg+d8]
        if (!call) continue;
        pos += _snprintf_s(line + pos, sizeof(line) - pos, _TRUNCATE, " %X", rva);
        ++found;
    }
    line[pos] = '\0';
    BVR_LOG("[reentry] calcstack (game tid %u):%s", GetCurrentThreadId(), line);
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
    g_lastBeatMs = 0; // reseed heartbeat bases on next beat
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

void kick_sampler(bool on) {
    if (on) {
        if (!g_kickCreated) {
            HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
            g_setEventTarget =
                k32 ? reinterpret_cast<void*>(GetProcAddress(k32, "SetEvent")) : nullptr;
            if (!g_setEventTarget) { BVR_LOG("[reentry] kick: SetEvent not resolved"); return; }
            MH_STATUS st = MH_CreateHook(g_setEventTarget,
                                         reinterpret_cast<void*>(&SetEventDetour),
                                         reinterpret_cast<void**>(&g_origSetEvent));
            if (st != MH_OK) {
                BVR_LOG("[reentry] kick: MH_CreateHook(SetEvent) failed: %s",
                        MH_StatusToString(st));
                return;
            }
            g_kickCreated = true;
        }
        for (auto& s : g_kickSlots) {
            s.key.store(0, std::memory_order_relaxed);
            s.count.store(0, std::memory_order_relaxed);
        }
        MH_STATUS st = MH_EnableHook(g_setEventTarget);
        if (st != MH_OK) {
            BVR_LOG("[reentry] kick: MH_EnableHook failed: %s", MH_StatusToString(st));
            return;
        }
        g_kickSampling.store(true, std::memory_order_relaxed);
        BVR_LOG("[reentry] kick sampler ON (process-wide SetEvent hook)");
    } else {
        g_kickSampling.store(false, std::memory_order_relaxed);
        if (g_kickCreated) MH_DisableHook(g_setEventTarget);
        BVR_LOG("[reentry] kick sampler OFF; distinct SetEvent callers:");
        for (auto& s : g_kickSlots) {
            if (s.key.load(std::memory_order_relaxed) == 0) continue;
            uint32_t rva = s.rva.load(std::memory_order_relaxed);
            BVR_LOG("[reentry]   tid=%u caller=%s0x%X count=%u",
                    s.tid.load(std::memory_order_relaxed),
                    rva == 0xFFFFFFFFu ? "(non-exe) " : "exe+",
                    rva, s.count.load(std::memory_order_relaxed));
        }
    }
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
        BVR_LOG("[reentry] command needs a verb: hook [build|submit|drain|flush]|"
                "stereo on|off|unhook|on|off|pulse|yaw|dump|arg3|latchclear|"
                "reset|status|kick|calcstack");
        return;
    }
    const char* rest = args + consumed;
    while (*rest == ' ' || *rest == '\t') ++rest;

    bool anyHook = g_drain.enabled.load(std::memory_order_relaxed) ||
                   g_flush.enabled.load(std::memory_order_relaxed) ||
                   g_submit.enabled.load(std::memory_order_relaxed) ||
                   g_build.enabled.load(std::memory_order_relaxed);

    if (strcmp(verb, "hook") == 0) {
        if (strncmp(rest, "flush", 5) == 0) {
            install_slot(g_flush, patterns::kRenderFlushRva,
                         reinterpret_cast<void*>(&FlushDetour),
                         patterns::kRenderFlushPrologue,
                         sizeof patterns::kRenderFlushPrologue);
        } else if (strncmp(rest, "drain", 5) == 0) {
            install_slot(g_drain, patterns::kDrainRva,
                         reinterpret_cast<void*>(&DrainDetour),
                         patterns::kDrainPrologue, sizeof patterns::kDrainPrologue);
        } else if (strncmp(rest, "submit", 6) == 0) {
            install_slot(g_submit, patterns::kFrameSubmitRva,
                         reinterpret_cast<void*>(&SubmitDetour),
                         patterns::kFrameSubmitPrologue,
                         sizeof patterns::kFrameSubmitPrologue);
        } else { // default: the scene BUILD root (the DR-5 seam)
            install_slot(g_build, patterns::kSceneBuildRva,
                         reinterpret_cast<void*>(&BuildDetour),
                         patterns::kSceneBuildPrologue,
                         sizeof patterns::kSceneBuildPrologue);
        }
    } else if (strcmp(verb, "unhook") == 0) {
        g_doubleCall.store(false, std::memory_order_relaxed);
        g_pulseCount.store(0, std::memory_order_relaxed);
        g_stereo.store(false, std::memory_order_relaxed);
        disable_slot(g_drain);
        disable_slot(g_flush);
        disable_slot(g_submit);
        disable_slot(g_build);
    } else if (strcmp(verb, "on") == 0) {
        if (!anyHook) {
            BVR_LOG("[reentry] on refused: no hook enabled");
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
        if (!anyHook) {
            BVR_LOG("[reentry] pulse refused: no hook enabled");
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
    } else if (strcmp(verb, "stereo") == 0) {
        if (strncmp(rest, "on", 2) == 0) {
            if (g_poisoned.load(std::memory_order_relaxed)) {
                BVR_LOG("[reentry] stereo refused: POISONED ('reentry reset' to clear)");
            } else if (install_slot(g_build, patterns::kSceneBuildRva,
                                    reinterpret_cast<void*>(&BuildDetour),
                                    patterns::kSceneBuildPrologue,
                                    sizeof patterns::kSceneBuildPrologue) &&
                       install_slot(g_submit, patterns::kFrameSubmitRva,
                                    reinterpret_cast<void*>(&SubmitDetour),
                                    patterns::kFrameSubmitPrologue,
                                    sizeof patterns::kFrameSubmitPrologue)) {
                g_stereo.store(true, std::memory_order_relaxed);
                BVR_LOG("[reentry] STEREO ON: every build doubled L/R, submits "
                        "tagged for per-present eye capture");
            }
        } else {
            g_stereo.store(false, std::memory_order_relaxed);
            BVR_LOG("[reentry] stereo off (hooks stay; 'reentry unhook' to drop)");
        }
    } else if (strcmp(verb, "dump") == 0) {
        int n = 0;
        if (sscanf_s(rest, "%d", &n) != 1 || n <= 0) n = 8;
        if (n > 32) n = 32;
        g_dumpRemaining.store(n, std::memory_order_relaxed);
        BVR_LOG("[reentry] dump armed: next %d submit calls%s", n,
                g_submit.enabled.load(std::memory_order_relaxed)
                    ? ""
                    : " (submit hook is OFF - 'reentry hook' first)");
    } else if (strcmp(verb, "arg3") == 0) {
        unsigned v = 0;
        if (strncmp(rest, "off", 3) == 0) {
            g_arg3Filter.store(0, std::memory_order_relaxed);
            BVR_LOG("[reentry] arg3 filter off (double-submit matches any call)");
        } else if (sscanf_s(rest, "%x", &v) == 1 && v != 0) {
            g_arg3Filter.store(v, std::memory_order_relaxed);
            BVR_LOG("[reentry] arg3 filter = %08X (double-submit only on match)", v);
        } else {
            BVR_LOG("[reentry] arg3 needs <hex-ptr>|off");
        }
    } else if (strcmp(verb, "latchclear") == 0) {
        bool on = strncmp(rest, "on", 2) == 0;
        g_latchClear.store(on, std::memory_order_relaxed);
        BVR_LOG("[reentry] latchclear %s (note: [queue+0x58] is the pump EXIT flag)",
                on ? "on" : "off");
    } else if (strcmp(verb, "reset") == 0) {
        g_poisoned.store(false, std::memory_order_relaxed);
        BVR_LOG("[reentry] poison cleared (last fault code=0x%08X rva=0x%X)",
                g_lastExcCode.load(std::memory_order_relaxed),
                g_lastExcRva.load(std::memory_order_relaxed));
    } else if (strcmp(verb, "kick") == 0) {
        kick_sampler(strncmp(rest, "on", 2) == 0);
    } else if (strcmp(verb, "calcstack") == 0) {
        g_calcstackPending.store(1, std::memory_order_relaxed);
        BVR_LOG("[reentry] calcstack armed (next CalcView logs a stack scan)");
    } else if (strcmp(verb, "status") == 0) {
        BVR_LOG("[reentry] status: stereo=%d(skips=%u) build=%s submit=%s drain=%s "
                "flush=%s double=%d pulse=%d yaw=%.1f dump=%d arg3=%08X "
                "latchclear=%d poisoned=%d kick=%d builds=%u submits=%u drains=%u "
                "flushes=%u seconds=%u draws2=%u",
                g_stereo.load(std::memory_order_relaxed) ? 1 : 0,
                g_stereoSkips.load(std::memory_order_relaxed),
                g_build.enabled.load(std::memory_order_relaxed) ? "on" : "off",
                g_submit.enabled.load(std::memory_order_relaxed) ? "on" : "off",
                g_drain.enabled.load(std::memory_order_relaxed) ? "on" : "off",
                g_flush.enabled.load(std::memory_order_relaxed) ? "on" : "off",
                g_doubleCall.load(std::memory_order_relaxed) ? 1 : 0,
                g_pulseCount.load(std::memory_order_relaxed),
                g_secondYawDeg.load(std::memory_order_relaxed),
                g_dumpRemaining.load(std::memory_order_relaxed),
                g_arg3Filter.load(std::memory_order_relaxed),
                g_latchClear.load(std::memory_order_relaxed) ? 1 : 0,
                g_poisoned.load(std::memory_order_relaxed) ? 1 : 0,
                g_kickSampling.load(std::memory_order_relaxed) ? 1 : 0,
                g_buildEntries.load(std::memory_order_relaxed),
                g_submitEntries.load(std::memory_order_relaxed),
                g_drainEntries.load(std::memory_order_relaxed),
                g_flushEntries.load(std::memory_order_relaxed),
                g_secondCalls.load(std::memory_order_relaxed),
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

bool stereo_active() {
    return g_stereo.load(std::memory_order_relaxed) &&
           g_build.enabled.load(std::memory_order_relaxed) &&
           !g_poisoned.load(std::memory_order_relaxed);
}

void note_calcview() {
    uint32_t tid = GetCurrentThreadId();
    g_lastCalcTid.store(tid, std::memory_order_relaxed);
    if (g_activeTid.load(std::memory_order_relaxed) == tid)
        g_calcInside.fetch_add(1, std::memory_order_relaxed);
    else
        g_calcOutside.fetch_add(1, std::memory_order_relaxed);
    if (g_calcstackPending.load(std::memory_order_relaxed) > 0 &&
        g_calcstackPending.exchange(0, std::memory_order_relaxed) > 0)
        log_game_stack();
}

bool hook_live() {
    return g_drain.enabled.load(std::memory_order_relaxed) ||
           g_flush.enabled.load(std::memory_order_relaxed) ||
           g_submit.enabled.load(std::memory_order_relaxed) ||
           g_build.enabled.load(std::memory_order_relaxed);
}

void draw_debug_ui() {
    if (!ImGui::CollapsingHeader("Reentry probe (DR-5)")) return;
    ImGui::Text("hooks: build %s, submit %s, drain %s, flush %s%s%s",
                g_build.enabled.load(std::memory_order_relaxed) ? "ON" : "off",
                g_submit.enabled.load(std::memory_order_relaxed) ? "ON" : "off",
                g_drain.enabled.load(std::memory_order_relaxed) ? "ON" : "off",
                g_flush.enabled.load(std::memory_order_relaxed) ? "ON" : "off",
                g_stereo.load(std::memory_order_relaxed) ? "  STEREO" : "",
                g_poisoned.load(std::memory_order_relaxed) ? "  POISONED" : "");
    ImGui::Text("double-call %s  yaw %.1f  arg3 %08X  2nd calls %u  draws2 %u",
                g_doubleCall.load(std::memory_order_relaxed) ? "ON" : "off",
                g_secondYawDeg.load(std::memory_order_relaxed),
                g_arg3Filter.load(std::memory_order_relaxed),
                g_secondCalls.load(std::memory_order_relaxed),
                g_lastSecondDraws.load(std::memory_order_relaxed));
    ImGui::Text("builds %u  submits %u  drains %u  flushes %u  calcview in/out "
                "%u/%u  2nd-pass hits %u",
                g_buildEntries.load(std::memory_order_relaxed),
                g_submitEntries.load(std::memory_order_relaxed),
                g_drainEntries.load(std::memory_order_relaxed),
                g_flushEntries.load(std::memory_order_relaxed),
                g_calcInside.load(std::memory_order_relaxed),
                g_calcOutside.load(std::memory_order_relaxed),
                g_secondPassHits.load(std::memory_order_relaxed));
    ImGui::TextDisabled(
        "control via seam: reentry hook|unhook|on|off|pulse|yaw|dump|arg3|kick");
}

} // namespace bvr::b1r::scenedraw
