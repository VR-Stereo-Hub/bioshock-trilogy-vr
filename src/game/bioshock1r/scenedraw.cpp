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
#include "game/bioshock1r/camera.h"
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

// The flush-point (kFlushPointRva) is `ret 8` with a DEAD ECX at entry (its
// prologue immediately loads arg2 into ECX): __fastcall passthrough with 2
// stack params. arg1 = scene object, arg2 = the 16-dword view group.
using FlushPointFn = void(__fastcall*)(void* ecx, void* edx, void* scene,
                                       void* group);

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
HookSlot g_flushpoint{"flushpoint"};
HookSlot g_fgctor{"fgctor"}; // session 21: the FOREGROUND SCENE NODE ctor

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
// Drains skipped by the empty-slot guard (session-7 forensics: entering the
// drain with [this+0xC] == NULL is the recurring drain+0x33 crash).
std::atomic<uint32_t> g_drainGuardSkips{0};
// `reentry 1t on` - STRUCTURAL single-threading (session 8): the flush-point
// hook forces the fully-decoded inline branch in the detour, so every scene
// flush drains on the game thread while the hw-thread numerator global stays
// untouched - its load-path consumers (the session-7 19:54 loader crash) see
// the true core count. Armed/disarmed per call; the hook stays installed.
std::atomic<bool> g_forceInline{false};
std::atomic<uint32_t> g_flushPointEntries{0};
std::atomic<uint32_t> g_forcedInline{0};
// `reentry 1tpoke on` saved hardware-thread count (0 = not poked). The
// legacy session-7 switch: poke [kNumHwThreadsRva] to 1 so the engine's own
// quotient check picks inline. Kept as a fallback/diagnostic - NOT load-safe
// (off before any save load / level transition).
std::atomic<uint32_t> g_savedNumHwThreads{0};
// One-toggle "VR stereo" (session 8): vrstereo on = structural 1t + VR
// camera mode + stereo, in that order; off reverses it. No sequencing waits
// needed - load-crossing soaks proved 1t/stereo are safe to arm any time
// (menu included) and sticky across loads; camera mode is a request the
// core engages when the XR session is ready. The overlay checkbox cannot
// install hooks from the render thread, so it posts a request here and the
// game thread applies it from note_calcview (outside any hooked call).
std::atomic<int> g_vrstereoPending{-1}; // -1 none, 0 off, 1 on
// Deadlock watchdog (session 6): the doubled render strands the engine's
// command-queue event protocol - game thread parked mid-build waiting for
// "render done" while the render thread waits for more work; a lost wakeup
// on the engine's own auto-reset events. This thread re-fires those events
// when the EXACT deadlock state is detected: the game thread stuck INSIDE a
// hooked call (g_activeDepth > 0) with builds AND presents frozen. The depth
// gate matters - a normal unfocused pause also freezes both counters, but
// with the game thread parked OUTSIDE our detours; kicking the pump then
// would drain a stale queue (the session-5 crash mode).
HANDLE g_watchdogThread = nullptr;      // created on first 'stereo on', kept
std::atomic<bool> g_watchdogExit{false};
std::atomic<uint32_t> g_watchdogKicks{0};
// Event re-kicks are OPT-IN ('reentry wdkick on'): live results 2026-07-24
// were detection ALWAYS correct, but kicking a desynced protocol crashed
// the drain (threaded mode) - waking a waiter whose peer is still stuck
// consumes corrupted state. Detection+log is the safe default.
std::atomic<bool> g_wdKickEnabled{false};
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
         g_beatSecond = 0, g_beatCalcIn = 0, g_beatCalcOut = 0,
         g_beatForced = 0;
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

bool read_u32_guarded(const void* addr, uint32_t* out) {
    __try {
        *out = *static_cast<const volatile uint32_t*>(addr);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool write_u32_guarded(void* addr, uint32_t value) {
    __try {
        *static_cast<volatile uint32_t*>(addr) = value;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Live render-mode detector (session-7 forensics, corrected same session).
// Mirrors the static config checks of the flush-point's own decision chain
// (0x61D260, fully decoded in ENGINE_NOTES): the next scene flush hands off
// to the pump thread only if the pump infrastructure exists (the two
// globals; created at first world load, NULL at the menu), the editor-class
// force-inline global is clear, and the hardware-thread quotient exceeds 1.
// The chain's remaining checks are per-frame scene vetoes we cannot (and
// need not) predict. Session-6 correction: `-onethread` is NOT PARSED by
// the remaster (no such string in the image) - all three 2026-07-24
// drain+0x33 minidumps were threaded-mode pump crashes, including the run
// mislabeled "onethread". The quotient check makes this detector honest
// under `reentry 1t on` (the real single-threading switch).
bool render_is_threaded() {
    // Structural 1t first: with the flush-point hook forcing the inline
    // branch, the next flush WILL drain inline no matter what the engine's
    // own chain would have picked.
    if (g_forceInline.load(std::memory_order_relaxed) &&
        g_flushpoint.enabled.load(std::memory_order_relaxed))
        return false;
    uint32_t ev = 0, obj = 0;
    read_u32_guarded(g_imageBase + patterns::kPumpKickEventPtrRva, &ev);
    read_u32_guarded(g_imageBase + patterns::kRenderThreadObjRva, &obj);
    if (ev == 0 && obj == 0) return false; // pump infra never created
    uint32_t forceInline = 0;
    read_u32_guarded(g_imageBase + patterns::kForceNonThreadedRenderRva,
                     &forceInline);
    if (forceInline != 0) return false;
    uint32_t num = 0, div = 0;
    read_u32_guarded(g_imageBase + patterns::kNumHwThreadsRva, &num);
    read_u32_guarded(g_imageBase + patterns::kThreadDivisorRva, &div);
    if (div != 0 && num / div <= 1) return false; // inline path selected
    return true;
}

// SetEvent on an engine event OBJECT (vtable-checked; HANDLE at obj+4).
// Returns true if a handle was actually signaled.
bool kick_engine_event(uint32_t objAddr, const char* name) {
    uint32_t vt = 0, h = 0;
    if (!objAddr) return false;
    if (!read_u32_guarded(reinterpret_cast<void*>(objAddr), &vt) ||
        vt != reinterpret_cast<uintptr_t>(g_imageBase) + patterns::kEventVtableRva)
        return false;
    if (!read_u32_guarded(reinterpret_cast<void*>(objAddr + 4), &h) || !h)
        return false;
    BOOL ok = SetEvent(reinterpret_cast<HANDLE>(static_cast<uintptr_t>(h)));
    BVR_LOG("[reentry] watchdog: SetEvent(%s) -> %d", name, ok ? 1 : 0);
    return ok != 0;
}

DWORD WINAPI WatchdogMain(void*) {
    uint32_t lastBuilds = 0;
    uint64_t lastPresents = 0;
    int stallTicks = 0;
    for (;;) {
        Sleep(100);
        if (g_watchdogExit.load(std::memory_order_relaxed)) return 0;
        if (!g_stereo.load(std::memory_order_relaxed)) { stallTicks = 0; continue; }
        uint32_t builds = g_buildEntries.load(std::memory_order_relaxed);
        uint64_t presents = bvr::d3d11_hook::present_count();
        bool inside = g_activeDepth.load(std::memory_order_relaxed) > 0;
        if (builds != lastBuilds || presents != lastPresents || !inside) {
            stallTicks = 0;
            lastBuilds = builds;
            lastPresents = presents;
            continue;
        }
        ++stallTicks;
        bool kick = g_wdKickEnabled.load(std::memory_order_relaxed);
        if (stallTicks == 3) {
            // 300 ms stuck inside a hooked call, nothing moving: the
            // deadlock signature.
            g_watchdogKicks.fetch_add(1, std::memory_order_relaxed);
            BVR_LOG("[reentry] watchdog: deadlock state detected (300 ms, "
                    "depth>0, builds/presents frozen)%s",
                    kick ? " - kicking pump event" : " (kicks off - log only)");
            if (kick) {
                uint32_t evObj = 0;
                if (read_u32_guarded(g_imageBase + patterns::kPumpKickEventPtrRva,
                                     &evObj))
                    kick_engine_event(evObj, "pump-kick");
            }
        } else if (stallTicks == 6 && kick) {
            // Render side did not move: kick the queue's flush events (the
            // game thread's flush-point waits on [queue+0x10]).
            BVR_LOG("[reentry] watchdog: still stalled - kicking queue events");
            uint32_t mgr = 0, queue = 0, evObj = 0;
            if (read_u32_guarded(g_imageBase + patterns::kRenderMgrGlobalRva,
                                 &mgr) &&
                mgr && read_u32_guarded(reinterpret_cast<void*>(mgr + 4), &queue) &&
                queue) {
                if (read_u32_guarded(
                        reinterpret_cast<void*>(queue + patterns::kQueueEventBOffset),
                        &evObj))
                    kick_engine_event(evObj, "queue-flush-B");
                if (read_u32_guarded(
                        reinterpret_cast<void*>(queue + patterns::kQueueEventAOffset),
                        &evObj))
                    kick_engine_event(evObj, "queue-flush-A");
            }
        } else if (stallTicks >= 12) {
            // 1.2 s and still wedged: recovery failed, stop doubling so the
            // game (if it ever unsticks) is not immediately re-wedged.
            BVR_LOG("[reentry] watchdog: recovery FAILED - stereo auto-off "
                    "(game likely needs a kill)");
            g_stereo.store(false, std::memory_order_relaxed);
            g_doubleCall.store(false, std::memory_order_relaxed);
            stallTicks = 0;
        }
    }
}

void ensure_watchdog() {
    if (g_watchdogThread) return;
    g_watchdogThread = CreateThread(nullptr, 0, &WatchdogMain, nullptr, 0, nullptr);
    BVR_LOG("[reentry] deadlock watchdog thread %s",
            g_watchdogThread ? "started" : "FAILED to start");
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
        g_beatForced = g_forcedInline.load(std::memory_order_relaxed);
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
    uint32_t forced = g_forcedInline.load(std::memory_order_relaxed);
    uint64_t presents = bvr::d3d11_hook::present_count();
    BVR_LOG("[reentry] mode=%s drain=%u/s flush=%u/s forced=%u/s build=%u/s "
            "submit=%u/s (nested=%u) 2nd=%u/s presents=%u/s "
            "calcview in=%u out=%u/s call1=%uus call2=%uus beatTid=%u "
            "calcTid=%u guardskips=%u callers=%X,%X,%X,%X%s",
            render_is_threaded() ? "MT" : "1T",
            drains - g_beatDrain, flushes - g_beatFlush,
            forced - g_beatForced,
            builds - g_beatBuild, submits - g_beatSubmit,
            g_submitNested.load(std::memory_order_relaxed),
            seconds - g_beatSecond,
            static_cast<uint32_t>(presents - g_beatPresents),
            calcIn - g_beatCalcIn, calcOut - g_beatCalcOut,
            g_call1Us.load(std::memory_order_relaxed),
            g_call2Us.load(std::memory_order_relaxed), beatTid,
            g_lastCalcTid.load(std::memory_order_relaxed),
            g_drainGuardSkips.load(std::memory_order_relaxed),
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
    g_beatForced = forced;
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
                        void* a4, uint32_t tid, uint32_t presentDelta,
                        uint32_t callerRva) {
    if (g_poisoned.load(std::memory_order_relaxed)) return;
    // Gameplay-caller gate (session 7, 19:54 load crash): loads/transitions
    // run the build from other call sites against half-built world state -
    // never double those. Steady-state gameplay is the only doubling target.
    if (callerRva != patterns::kSceneBuildGameplayRetRva) {
        g_stereoSkips.fetch_add(1, std::memory_order_relaxed);
        return;
    }
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

    // Session 22: with CalcView silent (scripted scene) pass 2 would just
    // re-render an identical camera - no eye offsets arm, and the cinematic
    // fallback is showing the quad. Skip the wasted double build (and its
    // blocking waitFrame). Same-thread ordering makes this exact: a stalled
    // game thread stalls builds too, so silence here can only mean the engine
    // is deliberately skipping CalcView.
    if (camera::calcview_silent(400)) {
        g_stereoSkips.fetch_add(1, std::memory_order_relaxed);
        return;
    }

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
    if (stereo) bvr::vr::sr_push_eye(+1); // pass 2 = RIGHT eye (see BuildDetour)
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
    uint32_t callerRva = to_rva(_ReturnAddress());
    note_caller(callerRva);
    if (n <= 2)
        BVR_LOG("[reentry] build fired #%u (tid %u, caller 0x%X, ecx %p, a1 %p)",
                n, tid, callerRva, ecx, a1);

    uint32_t presentLow =
        static_cast<uint32_t>(bvr::d3d11_hook::present_count());
    uint32_t presentDelta = presentLow - g_lastBuildPresentLow;
    g_lastBuildPresentLow = presentLow;

    // Session 22: scripted cameras (bathysphere descent) bypass CalcView, so
    // the FOV write's restore path never runs there - do it from here, the
    // game-thread hook that keeps firing during scenes, BEFORE the build so
    // this very frame renders at the authored FOV. Re-arm is automatic when
    // CalcView resumes (camera.cpp owns the latch). 400 ms sits ABOVE the
    // render side's 300 ms quad fallback on purpose (quad-over-forced-FOV for
    // ~100 ms is invisible; projection-over-restored-FOV would not be).
    if (depth == 0) camera::restore_game_fov_if_stale(400);

    // Stereo eye tag for pass 1 (LEFT), pushed BEFORE the original: the
    // pass's Present strictly follows (async via the pump in threaded mode,
    // inline inside this very call in single-threaded mode), so the tag is
    // in the ring by the time Present-tail pops it. Pass 2's tag is pushed
    // in maybe_second_build the same way.
    if (depth == 0 && g_stereo.load(std::memory_order_relaxed))
        bvr::vr::sr_push_eye(-1);

    LARGE_INTEGER t0, t1;
    QueryPerformanceCounter(&t0);
    reinterpret_cast<BuildFn>(g_build.original)(ecx, edx, a1, a2, a3,
                                                a4); // never guarded
    QueryPerformanceCounter(&t1);
    g_call1Us.store(qpc_us(t0, t1), std::memory_order_relaxed);

    if (depth == 0)
        maybe_second_build(ecx, edx, a1, a2, a3, a4, tid, presentDelta,
                           callerRva);

    if (!g_drain.enabled.load(std::memory_order_relaxed) &&
        !g_flush.enabled.load(std::memory_order_relaxed))
        heartbeat(tid);
    if (g_activeDepth.fetch_sub(1, std::memory_order_relaxed) == 1)
        g_activeTid.store(0, std::memory_order_relaxed);
}

// ---- session 21: fg scene node instrument (world-pass re-homing) -----------
// The scene build constructs a per-frame FOREGROUND SCENE NODE for the
// first-person rig (patterns::kFgSceneNodeCtorRva; layout facts in
// patterns.h). This read-only hook snapshots the ctor's parent-view argument
// plus the whole node at TWO points - ctor tail and frame submit - so
// `vrfgnode dump` shows where the fg view diverges from the parent (world)
// view: at construction, during command record, or not at all (then the
// divergence is bake-side actor-frame math). Discovery instrument only;
// nothing here writes engine state.
using FgCtorFn = void*(__fastcall*)(void* self, void* edx, void* scene, void* parentView,
                                    void* a3, float ofsX, float ofsY, float ofsZ,
                                    int32_t rotA, int32_t rotB, int32_t rotC, float fovA,
                                    float fovB);
std::atomic<bool> g_fgWatch{false};
std::atomic<uint32_t> g_fgCtorCalls{0};
// fg view-sync (the crossed-eye fix): substitute the ctor's camera args with
// the CORRECT eye's driven camera from camera.cpp's stash. Default OFF.
std::atomic<bool> g_fgSync{false};
std::atomic<uint32_t> g_fgSyncSubs{0};
std::atomic<uint32_t> g_fgSyncMisses{0};
// fovA-arg override (0 = off): the ctor's first fov input (from PC+0x45C,
// engine-restamped to 75.0 every frame - unpokeable as data). If the bake's
// eye pull-back derives from the fovA/fovB pair, overriding fovA == fovB
// moves the rig's rendered depth - the pull's origin discriminator.
std::atomic<float> g_fgFovAOverride{0.0f};
// Snapshot state: written on the game thread inside the build (1t), read by
// the command poller on the same thread - plain fields are safe.
void* g_fgNodeScene = nullptr;
void* g_fgNodeLast = nullptr;
void* g_fgNodeParentView = nullptr;
float g_fgCtorOfs[3] = {};
int32_t g_fgCtorRot[3] = {};
float g_fgCtorFov[2] = {};
// Per-eye claim check: under SR stereo the two builds of one pair should
// carry +-IPD/2-offset eye cameras in their WORLD views - do the fg nodes?
// Two slots, alternating per ctor call; the dump prints both.
float g_fgCtorOfs2[2][3] = {};
int32_t g_fgCtorRot2[2][3] = {};
uint32_t g_fgCtorSlot = 0;
uint8_t g_fgParentSnap[0x100];
uint8_t g_fgNodeSnapCtor[patterns::kFgSceneNodeBytes];
uint8_t g_fgNodeSnapSubmit[patterns::kFgSceneNodeBytes];
bool g_fgSnapCtorOk = false;
bool g_fgSnapSubmitOk = false;

bool snap_mem(void* dst, const void* src, size_t n) {
    __try {
        memcpy(dst, src, n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void* __fastcall FgCtorDetour(void* self, void* edx, void* scene, void* parentView, void* a3,
                              float ofsX, float ofsY, float ofsZ, int32_t rotA, int32_t rotB,
                              int32_t rotC, float fovA, float fovB) {
    // fg view-sync: the engine hands this ctor the camera one BUILD stale,
    // which under SR stereo is the OTHER eye's camera (crossed eyes,
    // full-IPD lateral error + inverted rig disparity - flat-measured,
    // ENGINE_NOTES session 21). Substitute the CORRECT eye's driven camera
    // from the previous pair: both eyes then come from one consistent pose
    // sample, correct-eye, one pair stale (same order of lag as stock).
    if (g_fgSync.load(std::memory_order_relaxed) && g_stereo.load(std::memory_order_relaxed)) {
        int eye = g_secondPassTid.load(std::memory_order_relaxed) == GetCurrentThreadId() ? 1 : 0;
        float sl[3];
        int32_t sr[3];
        if (camera::driven_eye_cam(eye, sl, sr)) {
            ofsX = sl[0]; ofsY = sl[1]; ofsZ = sl[2];
            rotA = sr[0]; rotB = sr[1]; rotC = sr[2];
            g_fgSyncSubs.fetch_add(1, std::memory_order_relaxed);
        } else {
            g_fgSyncMisses.fetch_add(1, std::memory_order_relaxed);
        }
    }
    float fovAOv = g_fgFovAOverride.load(std::memory_order_relaxed);
    if (fovAOv != 0.0f) fovA = fovAOv < 0.0f ? fovB : fovAOv; // -1 = "match fovB"
    void* ret = reinterpret_cast<FgCtorFn>(g_fgctor.original)(
        self, edx, scene, parentView, a3, ofsX, ofsY, ofsZ, rotA, rotB, rotC, fovA, fovB);
    g_fgCtorCalls.fetch_add(1, std::memory_order_relaxed);
    if (g_fgWatch.load(std::memory_order_relaxed)) {
        g_fgNodeScene = scene;
        g_fgNodeLast = self;
        g_fgNodeParentView = parentView;
        g_fgCtorOfs[0] = ofsX; g_fgCtorOfs[1] = ofsY; g_fgCtorOfs[2] = ofsZ;
        g_fgCtorRot[0] = rotA; g_fgCtorRot[1] = rotB; g_fgCtorRot[2] = rotC;
        g_fgCtorFov[0] = fovA; g_fgCtorFov[1] = fovB;
        // Slot by PASS: 0 = first build of the pair (LEFT eye), 1 = second
        // (RIGHT). Under sync the values are post-substitution.
        uint32_t slot =
            g_secondPassTid.load(std::memory_order_relaxed) == GetCurrentThreadId() ? 1u : 0u;
        g_fgCtorSlot = slot;
        g_fgCtorOfs2[slot][0] = ofsX; g_fgCtorOfs2[slot][1] = ofsY; g_fgCtorOfs2[slot][2] = ofsZ;
        g_fgCtorRot2[slot][0] = rotA; g_fgCtorRot2[slot][1] = rotB; g_fgCtorRot2[slot][2] = rotC;
        g_fgSnapCtorOk = snap_mem(g_fgParentSnap, parentView, sizeof g_fgParentSnap) &&
                         snap_mem(g_fgNodeSnapCtor, self, sizeof g_fgNodeSnapCtor);
        g_fgSnapSubmitOk = false; // re-armed by the next submit
    }
    return ret;
}

void __fastcall SubmitDetour(void* ecx, void* edx, FVec3* loc, FRot3* rot,
                             void* arg3) {
    uint32_t tid = GetCurrentThreadId();
    int depth = g_activeDepth.fetch_add(1, std::memory_order_relaxed);
    if (depth == 0) {
        g_activeTid.store(tid, std::memory_order_relaxed);
    } else {
        g_submitNested.fetch_add(1, std::memory_order_relaxed);
    }
    g_submitEntries.fetch_add(1, std::memory_order_relaxed);
    note_caller(to_rva(_ReturnAddress()));

    // fg-node instrument: re-read the node at submit time (post-record) so
    // the dump can diff ctor-state vs recorded-state. SEH-guarded - the node
    // lives in the frame pool and a stale pointer must not fault.
    if (g_fgWatch.load(std::memory_order_relaxed) && g_fgNodeLast && !g_fgSnapSubmitOk)
        g_fgSnapSubmitOk = snap_mem(g_fgNodeSnapSubmit, g_fgNodeLast, sizeof g_fgNodeSnapSubmit);

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

// Forced-inline scene flush (session 8, the structural `1t`): reproduce the
// flush-point's byte-confirmed INLINE branch - copy the args into the render
// manager, stamp mode single-threaded, call the drain on this thread. The
// hw-thread numerator is NEVER touched, so the quotient family's load-path
// consumers (0x4D0E24 et al - the session-7 19:54 loader crash) keep seeing
// the true core count. Returns 1 on success, 0 when the mgr global is still
// null (engine too early - let the original decide), -1 on a fault (the
// filter recorded code/rva). No C++ objects in this frame (SEH + unwinding
// = C2712).
int force_inline_flush(void* scene, void* group) {
    __try {
        uint8_t* mgr = *reinterpret_cast<uint8_t* const*>(
            g_imageBase + patterns::kRenderMgrGlobalRva);
        if (!mgr) return 0;
        *reinterpret_cast<void**>(mgr + patterns::kMgrSceneSlotOffset) = scene;
        const uint32_t* src = static_cast<const uint32_t*>(group);
        uint32_t* dst =
            reinterpret_cast<uint32_t*>(mgr + patterns::kMgrViewGroupOffset);
        for (uint32_t i = 0; i < patterns::kMgrViewGroupDwords; ++i)
            dst[i] = src[i];
        *reinterpret_cast<uint32_t*>(mgr + patterns::kMgrThreadedFlagOffset) = 0;
        *reinterpret_cast<uint32_t*>(mgr + patterns::kMgrFlushSeenOffset) = 1;
        // Call through the drain's TARGET address, not a trampoline: the
        // DrainDetour (empty-slot guard + telemetry) must stay in the path.
        // Heartbeat consequence: the drain's caller RVA reads as a
        // bioshockvr.dll address instead of 0x61D367 - expected, not a bug.
        reinterpret_cast<RenderFn>(const_cast<uint8_t*>(g_imageBase) +
                                   patterns::kDrainRva)(mgr, nullptr);
        return 1;
    } __except (reentry_filter(GetExceptionCode(), GetExceptionInformation())) {
        return -1;
    }
}

void __fastcall FlushPointDetour(void* ecx, void* edx, void* scene,
                                 void* group) {
    g_flushPointEntries.fetch_add(1, std::memory_order_relaxed);
    if (g_forceInline.load(std::memory_order_relaxed)) {
        int r = force_inline_flush(scene, group);
        if (r > 0) {
            g_forcedInline.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        if (r < 0) {
            // A layout assumption broke mid-flush: mgr may be half-written
            // and the frame half-drained - running the original now could
            // double-drain the same ring. Drop this flush, disarm, poison.
            g_forceInline.store(false, std::memory_order_relaxed);
            g_stereo.store(false, std::memory_order_relaxed);
            g_doubleCall.store(false, std::memory_order_relaxed);
            g_poisoned.store(true, std::memory_order_relaxed);
            BVR_LOG("[reentry] flushpoint forced-inline FAULTED code=0x%08X "
                    "rva=0x%X - 1t disarmed, stereo/doubling off, POISONED "
                    "('reentry reset' to clear)",
                    g_lastExcCode.load(std::memory_order_relaxed),
                    g_lastExcRva.load(std::memory_order_relaxed));
            return;
        }
        // r == 0: mgr not created yet - the original handles pre-init state.
    }
    reinterpret_cast<FlushPointFn>(g_flushpoint.original)(ecx, edx, scene,
                                                          group);
}

void __fastcall DrainDetour(void* self, void* edx) {
    uint32_t tid = GetCurrentThreadId();
    // Empty-slot guard (session-7 minidump forensics): the drain head loads
    // the submitted-frame context from [this+0xC] and walks its +0x40 member
    // three instructions in, with NO null check - entering with the slot
    // NULL is the recurring drain+0x33 crash (a pump woken with no pending
    // frame: watchdog kick, desynced-protocol stray wake, or - under
    // `reentry 1t on` - the submit's kick landing after the inline drain
    // already consumed the frame). A null slot can never be drained, so the
    // skip is universally safe and the guard runs whenever this hook is
    // installed. Skips get their own counter and do NOT count as drains.
    // Threaded-mode caveat: the null read races the producer, so a skip can
    // eat an auto-reset wake - a possible stall, strictly better than the
    // crash it replaces.
    {
        uint32_t frameCtx = 0;
        if (read_u32_guarded(static_cast<const uint8_t*>(self) +
                                 patterns::kQueueFrameCtxOffset,
                             &frameCtx) &&
            frameCtx == 0) {
            uint32_t n =
                g_drainGuardSkips.fetch_add(1, std::memory_order_relaxed) + 1;
            if (n <= 3)
                BVR_LOG("[reentry] drain-guard: skipped empty-slot drain #%u "
                        "(tid %u, [this+0xC]=0 - the drain+0x33 crash state)",
                        n, tid);
            return;
        }
    }
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

// The one-toggle compound (game thread only, outside hooked calls). Order
// matters ON: 1t first so stereo's substrate gate sees 1T; camera mode is
// just a request the core engages at session-ready. OFF reverses.
void apply_vrstereo(bool on) {
    if (on) {
        BVR_LOG("[reentry] VRSTEREO ON: sequencing 1t -> camera mode -> stereo");
        if (!g_forceInline.load(std::memory_order_relaxed))
            handle_command("1t on");
        bvr::vr::set_camera_mode(true);
        if (!g_stereo.load(std::memory_order_relaxed))
            handle_command("stereo on");
        bool ok = g_forceInline.load(std::memory_order_relaxed) &&
                  g_stereo.load(std::memory_order_relaxed);
        BVR_LOG("[reentry] VRSTEREO %s (1t=%d stereo=%d; sticky across loads; "
                "'vrstereo off' reverses)",
                ok ? "READY" : "INCOMPLETE - see refusals above",
                g_forceInline.load(std::memory_order_relaxed) ? 1 : 0,
                g_stereo.load(std::memory_order_relaxed) ? 1 : 0);
    } else {
        BVR_LOG("[reentry] VRSTEREO OFF: stereo -> camera mode -> 1t");
        if (g_stereo.load(std::memory_order_relaxed))
            handle_command("stereo off");
        bvr::vr::set_camera_mode(false);
        if (g_forceInline.load(std::memory_order_relaxed))
            handle_command("1t off");
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
        BVR_LOG("[reentry] command needs a verb: vrstereo on|off|"
                "hook [build|submit|drain|flush]|"
                "stereo on|force|off|1t on|off|1tpoke on|off|unhook|on|off|pulse|"
                "yaw|dump|arg3|latchclear|reset|status|kick|calcstack");
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
        g_forceInline.store(false, std::memory_order_relaxed);
        disable_slot(g_flushpoint);
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
        bool force = strncmp(rest, "force", 5) == 0;
        if (strncmp(rest, "on", 2) == 0 || force) {
            if (g_poisoned.load(std::memory_order_relaxed)) {
                BVR_LOG("[reentry] stereo refused: POISONED ('reentry reset' to clear)");
            } else if (!force && render_is_threaded()) {
                // Session-7 forensics: every stereo deadlock AND every
                // drain+0x33 crash specimen was the threaded pump protocol;
                // the "onethread" crash run was a mislaunch (pump thread in
                // the dump). Refuse the substrate instead of re-proving it.
                BVR_LOG("[reentry] stereo refused: THREADED renderer live - "
                        "the deadlock + empty-wake crash substrate. 'reentry "
                        "1t on' first (single-threaded render), or 'reentry "
                        "stereo force' for experiments");
            } else if (install_slot(g_build, patterns::kSceneBuildRva,
                                    reinterpret_cast<void*>(&BuildDetour),
                                    patterns::kSceneBuildPrologue,
                                    sizeof patterns::kSceneBuildPrologue) &&
                       install_slot(g_submit, patterns::kFrameSubmitRva,
                                    reinterpret_cast<void*>(&SubmitDetour),
                                    patterns::kFrameSubmitPrologue,
                                    sizeof patterns::kFrameSubmitPrologue)) {
                // Drain empty-slot guard: insurance, not a gate - stereo
                // still arms if this install fails.
                if (!install_slot(g_drain, patterns::kDrainRva,
                                  reinterpret_cast<void*>(&DrainDetour),
                                  patterns::kDrainPrologue,
                                  sizeof patterns::kDrainPrologue))
                    BVR_LOG("[reentry] drain-guard install FAILED - stereo "
                            "continues unguarded");
                ensure_watchdog();
                g_stereo.store(true, std::memory_order_relaxed);
                BVR_LOG("[reentry] STEREO ON (%s render): every build doubled "
                        "L/R, submits tagged for per-present eye capture "
                        "(watchdog + drain-guard armed)",
                        render_is_threaded() ? "THREADED - forced, crash-prone"
                                             : "single-threaded");
            }
        } else {
            g_stereo.store(false, std::memory_order_relaxed);
            BVR_LOG("[reentry] stereo off (hooks stay; 'reentry unhook' to drop)");
        }
    } else if (strcmp(verb, "1t") == 0) {
        // STRUCTURAL single-threading (session 8): flush-point hook forces
        // the inline branch; numerator global untouched. Load-safe and
        // menu-safe (session-8 soaks: save load, quit-to-menu, new game,
        // bathysphere transition - all clean with this armed); pre-world
        // arming is inert until the render manager exists (detour falls
        // through to the original while mgr is null).
        if (strncmp(rest, "on", 2) == 0) {
            if (g_forceInline.load(std::memory_order_relaxed)) {
                BVR_LOG("[reentry] 1t already on");
            } else if (g_poisoned.load(std::memory_order_relaxed)) {
                BVR_LOG("[reentry] 1t refused: POISONED ('reentry reset' to clear)");
            } else if (!install_slot(g_drain, patterns::kDrainRva,
                                     reinterpret_cast<void*>(&DrainDetour),
                                     patterns::kDrainPrologue,
                                     sizeof patterns::kDrainPrologue)) {
                // Once flushes inline, any straggling pump wake finds an
                // EMPTY frame slot - only the drain-hook guard stands
                // between that and drain+0x33.
                BVR_LOG("[reentry] 1t refused: drain-guard install failed");
            } else if (!install_slot(g_flushpoint, patterns::kFlushPointRva,
                                     reinterpret_cast<void*>(&FlushPointDetour),
                                     patterns::kFlushPointPrologue,
                                     sizeof patterns::kFlushPointPrologue)) {
                BVR_LOG("[reentry] 1t refused: flush-point hook install failed");
            } else {
                g_forceInline.store(true, std::memory_order_relaxed);
                BVR_LOG("[reentry] 1t ON (structural): flush-point forces the "
                        "inline branch - scene flushes drain on the game "
                        "thread, hw-thread global UNTOUCHED (loaders see the "
                        "true core count). Drain caller RVA now reads inside "
                        "bioshockvr.dll - expected.");
            }
        } else {
            if (!g_forceInline.exchange(false, std::memory_order_relaxed)) {
                BVR_LOG("[reentry] 1t was not on");
            } else {
                BVR_LOG("[reentry] 1t off: flush-point back to the engine's "
                        "own decision (hook stays installed, passive)%s",
                        g_stereo.load(std::memory_order_relaxed)
                            ? " (WARNING: stereo still on - now on the "
                              "THREADED substrate)"
                            : "");
            }
        }
    } else if (strcmp(verb, "1tpoke") == 0) {
        // LEGACY fallback (session 7): poke the hw-thread numerator so the
        // engine's own quotient check picks inline. NOT load-safe - the
        // global has load-path consumers (19:54 loader crash). Prefer '1t'.
        uint8_t* numAddr = const_cast<uint8_t*>(g_imageBase) +
                           patterns::kNumHwThreadsRva;
        if (strncmp(rest, "on", 2) == 0) {
            uint32_t pumpEv = 0, pumpObj = 0;
            read_u32_guarded(g_imageBase + patterns::kPumpKickEventPtrRva, &pumpEv);
            read_u32_guarded(g_imageBase + patterns::kRenderThreadObjRva, &pumpObj);
            uint32_t cur = 0;
            if (g_savedNumHwThreads.load(std::memory_order_relaxed) != 0) {
                BVR_LOG("[reentry] 1tpoke already on");
            } else if (pumpEv == 0 && pumpObj == 0) {
                // Session-7 19:54 loader-thread crash: the hw-thread global
                // has LOAD-PATH consumers - flipping it before/during a level
                // load crashes the loader. Menu = the world does not exist
                // yet = the next thing is a load. Arm only in gameplay.
                BVR_LOG("[reentry] 1tpoke refused: no world loaded yet - load "
                        "into gameplay first (the poke across a level load "
                        "crashes the loader)");
            } else if (!install_slot(g_drain, patterns::kDrainRva,
                                     reinterpret_cast<void*>(&DrainDetour),
                                     patterns::kDrainPrologue,
                                     sizeof patterns::kDrainPrologue)) {
                BVR_LOG("[reentry] 1tpoke refused: drain-guard install failed");
            } else if (!read_u32_guarded(numAddr, &cur) || cur == 0) {
                BVR_LOG("[reentry] 1tpoke refused: hw-thread global unreadable");
            } else if (!write_u32_guarded(numAddr, 1)) {
                BVR_LOG("[reentry] 1tpoke refused: hw-thread global unwritable");
            } else {
                g_savedNumHwThreads.store(cur, std::memory_order_relaxed);
                BVR_LOG("[reentry] 1tpoke ON: hw-thread count %u -> 1. "
                        "WARNING: 'reentry 1tpoke off' BEFORE loading a save "
                        "or crossing a level transition (load-path crash, "
                        "session 7). Prefer 'reentry 1t' (load-safe hook)",
                        cur);
            }
        } else {
            uint32_t saved =
                g_savedNumHwThreads.exchange(0, std::memory_order_relaxed);
            if (saved == 0) {
                BVR_LOG("[reentry] 1tpoke was not on");
            } else if (write_u32_guarded(numAddr, saved)) {
                BVR_LOG("[reentry] 1tpoke off: hw-thread count restored to %u%s",
                        saved,
                        g_stereo.load(std::memory_order_relaxed) &&
                                !g_forceInline.load(std::memory_order_relaxed)
                            ? " (WARNING: stereo still on - now on the "
                              "THREADED substrate)"
                            : "");
            } else {
                BVR_LOG("[reentry] 1tpoke off FAILED to restore (global "
                        "unwritable?)");
            }
        }
    } else if (strcmp(verb, "vrstereo") == 0) {
        apply_vrstereo(strncmp(rest, "on", 2) == 0);
    } else if (strcmp(verb, "wdkick") == 0) {
        bool on = strncmp(rest, "on", 2) == 0;
        g_wdKickEnabled.store(on, std::memory_order_relaxed);
        BVR_LOG("[reentry] watchdog kicks %s (recovery kicks crashed a desynced "
                "protocol live - detect-only is the safe default)",
                on ? "ON" : "off");
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
        bool hook1t = g_forceInline.load(std::memory_order_relaxed);
        bool poke1t = g_savedNumHwThreads.load(std::memory_order_relaxed) != 0;
        BVR_LOG("[reentry] status: mode=%s 1t=%s flushpt=%u(forced=%u) "
                "stereo=%d(skips=%u,wdkicks=%u,"
                "guardskips=%u) build=%s submit=%s drain=%s "
                "flush=%s double=%d pulse=%d yaw=%.1f dump=%d arg3=%08X "
                "latchclear=%d poisoned=%d kick=%d builds=%u submits=%u drains=%u "
                "flushes=%u seconds=%u draws2=%u",
                render_is_threaded() ? "MT" : "1T",
                hook1t ? (poke1t ? "hook+poke" : "hook")
                       : (poke1t ? "poke" : "off"),
                g_flushPointEntries.load(std::memory_order_relaxed),
                g_forcedInline.load(std::memory_order_relaxed),
                g_stereo.load(std::memory_order_relaxed) ? 1 : 0,
                g_stereoSkips.load(std::memory_order_relaxed),
                g_watchdogKicks.load(std::memory_order_relaxed),
                g_drainGuardSkips.load(std::memory_order_relaxed),
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

void request_vrstereo(bool on) {
    g_vrstereoPending.store(on ? 1 : 0, std::memory_order_relaxed);
}

void handle_fgnode_command(const char* args) {
    if (strncmp(args, "fova", 4) == 0) {
        float v = 0.0f;
        if (strstr(args + 4, "off"))
            v = 0.0f;
        else if (strstr(args + 4, "match"))
            v = -1.0f;
        else if (sscanf_s(args + 4, " %f", &v) != 1)
            v = 0.0f;
        if (v != 0.0f && !g_fgctor.enabled.load(std::memory_order_relaxed) &&
            !install_slot(g_fgctor, patterns::kFgSceneNodeCtorRva,
                          reinterpret_cast<void*>(&FgCtorDetour),
                          patterns::kFgSceneNodeCtorPrologue,
                          sizeof patterns::kFgSceneNodeCtorPrologue))
            return;
        g_fgFovAOverride.store(v, std::memory_order_relaxed);
        BVR_LOG("[fgnode] fovA override %s (%.2f; -1 = match fovB)", v != 0.0f ? "ON" : "off",
                v);
    } else if (strncmp(args, "sync", 4) == 0) {
        bool on = strstr(args + 4, "off") == nullptr;
        if (on && !g_fgctor.enabled.load(std::memory_order_relaxed) &&
            !install_slot(g_fgctor, patterns::kFgSceneNodeCtorRva,
                          reinterpret_cast<void*>(&FgCtorDetour),
                          patterns::kFgSceneNodeCtorPrologue,
                          sizeof patterns::kFgSceneNodeCtorPrologue))
            return;
        g_fgSync.store(on, std::memory_order_relaxed);
        BVR_LOG("[fgnode] view-sync %s (subs %u misses %u) - fg scene gets the CORRECT "
                "eye's driven camera%s",
                on ? "ON" : "off", g_fgSyncSubs.load(std::memory_order_relaxed),
                g_fgSyncMisses.load(std::memory_order_relaxed),
                on ? "; requires vrstereo" : "");
    } else if (strncmp(args, "on", 2) == 0) {
        if (install_slot(g_fgctor, patterns::kFgSceneNodeCtorRva,
                         reinterpret_cast<void*>(&FgCtorDetour),
                         patterns::kFgSceneNodeCtorPrologue,
                         sizeof patterns::kFgSceneNodeCtorPrologue)) {
            g_fgWatch.store(true, std::memory_order_relaxed);
            BVR_LOG("[fgnode] watch ON (fg scene node ctor hooked; 'vrfgnode dump' logs "
                    "the ctor-vs-submit snapshots)");
        }
    } else if (strncmp(args, "off", 3) == 0) {
        g_fgWatch.store(false, std::memory_order_relaxed);
        g_fgSync.store(false, std::memory_order_relaxed);
        disable_slot(g_fgctor);
        BVR_LOG("[fgnode] watch + sync off");
    } else if (strncmp(args, "dump", 4) == 0) {
        BVR_LOG("[fgnode] ctorCalls=%u scene=%p node=%p pview=%p snaps ctor=%d submit=%d",
                g_fgCtorCalls.load(std::memory_order_relaxed), g_fgNodeScene, g_fgNodeLast,
                g_fgNodeParentView, g_fgSnapCtorOk ? 1 : 0, g_fgSnapSubmitOk ? 1 : 0);
        if (!g_fgSnapCtorOk) {
            BVR_LOG("[fgnode] no ctor snapshot yet (watch on + gameplay view needed)");
            return;
        }
        BVR_LOG("[fgnode] ctor args: ofs=(%.4g %.4g %.4g) rot=(%d %d %d) fovA=%.2f fovB=%.2f",
                g_fgCtorOfs[0], g_fgCtorOfs[1], g_fgCtorOfs[2], g_fgCtorRot[0], g_fgCtorRot[1],
                g_fgCtorRot[2], g_fgCtorFov[0], g_fgCtorFov[1]);
        BVR_LOG("[fgnode] pair slots: pass1/L ofs=(%.6g %.6g %.6g) rot=(%d %d %d) | pass2/R "
                "ofs=(%.6g %.6g %.6g) rot=(%d %d %d)",
                g_fgCtorOfs2[0][0], g_fgCtorOfs2[0][1], g_fgCtorOfs2[0][2], g_fgCtorRot2[0][0],
                g_fgCtorRot2[0][1], g_fgCtorRot2[0][2], g_fgCtorOfs2[1][0], g_fgCtorOfs2[1][1],
                g_fgCtorOfs2[1][2], g_fgCtorRot2[1][0], g_fgCtorRot2[1][1], g_fgCtorRot2[1][2]);
        // Parent view block (scene+0x118): 64 floats.
        const float* pv = reinterpret_cast<const float*>(g_fgParentSnap);
        for (int i = 0; i < 64; i += 8)
            BVR_LOG("[fgnode] pview+0x%03X: %.4g %.4g %.4g %.4g %.4g %.4g %.4g %.4g", i * 4,
                    pv[i], pv[i + 1], pv[i + 2], pv[i + 3], pv[i + 4], pv[i + 5], pv[i + 6],
                    pv[i + 7]);
        // Locate the parent-view copy inside the node (first 16-byte run).
        int foundAt = -1;
        for (size_t off = 0; off + 16 <= sizeof g_fgNodeSnapCtor; off += 4) {
            if (memcmp(g_fgNodeSnapCtor + off, g_fgParentSnap, 16) == 0) {
                foundAt = static_cast<int>(off);
                break;
            }
        }
        BVR_LOG("[fgnode] parent-view first 16 bytes %s in node(ctor)%s0x%X",
                foundAt >= 0 ? "FOUND" : "NOT found", foundAt >= 0 ? " at +" : " (searched +0..+",
                foundAt >= 0 ? static_cast<unsigned>(foundAt)
                             : static_cast<unsigned>(sizeof g_fgNodeSnapCtor));
        // The node's own view region (+0x140..+0x200) at ctor time.
        const float* nv = reinterpret_cast<const float*>(g_fgNodeSnapCtor);
        for (int i = 0x140 / 4; i < 0x200 / 4; i += 8)
            BVR_LOG("[fgnode] node+0x%03X: %.4g %.4g %.4g %.4g %.4g %.4g %.4g %.4g", i * 4,
                    nv[i], nv[i + 1], nv[i + 2], nv[i + 3], nv[i + 4], nv[i + 5], nv[i + 6],
                    nv[i + 7]);
        BVR_LOG("[fgnode] node vec3@0x310=(%.4g %.4g %.4g) fovs@0x3F0=(%.2f %.2f)",
                nv[0x310 / 4], nv[0x314 / 4], nv[0x318 / 4], nv[0x3F0 / 4], nv[0x3F4 / 4]);
        // Submit-vs-ctor diff, 16-byte rows.
        if (g_fgSnapSubmitOk) {
            char rows[256];
            size_t len = 0;
            int changed = 0;
            for (size_t off = 0; off < sizeof g_fgNodeSnapCtor; off += 16) {
                if (memcmp(g_fgNodeSnapCtor + off, g_fgNodeSnapSubmit + off, 16) == 0) continue;
                ++changed;
                if (len < sizeof rows - 12)
                    len += sprintf_s(rows + len, sizeof rows - len, " +0x%X",
                                     static_cast<unsigned>(off));
            }
            BVR_LOG("[fgnode] submit-vs-ctor: %d changed row(s):%s", changed,
                    changed ? rows : " none");
            if (changed) {
                const float* sv = reinterpret_cast<const float*>(g_fgNodeSnapSubmit);
                for (int i = 0x140 / 4; i < 0x200 / 4; i += 8)
                    BVR_LOG("[fgnode] subm+0x%03X: %.4g %.4g %.4g %.4g %.4g %.4g %.4g %.4g",
                            i * 4, sv[i], sv[i + 1], sv[i + 2], sv[i + 3], sv[i + 4], sv[i + 5],
                            sv[i + 6], sv[i + 7]);
            }
        }
    } else {
        BVR_LOG("[fgnode] usage: vrfgnode on|off|dump|sync on|sync off (hook=%d watch=%d "
                "sync=%d calls=%u subs=%u misses=%u)",
                g_fgctor.enabled.load(std::memory_order_relaxed) ? 1 : 0,
                g_fgWatch.load(std::memory_order_relaxed) ? 1 : 0,
                g_fgSync.load(std::memory_order_relaxed) ? 1 : 0,
                g_fgCtorCalls.load(std::memory_order_relaxed),
                g_fgSyncSubs.load(std::memory_order_relaxed),
                g_fgSyncMisses.load(std::memory_order_relaxed));
    }
}

void note_calcview() {
    uint32_t tid = GetCurrentThreadId();
    g_lastCalcTid.store(tid, std::memory_order_relaxed);
    if (g_activeTid.load(std::memory_order_relaxed) == tid) {
        g_calcInside.fetch_add(1, std::memory_order_relaxed);
    } else {
        g_calcOutside.fetch_add(1, std::memory_order_relaxed);
        // Overlay-posted vrstereo request: apply on the game thread OUTSIDE
        // any hooked call (hook installs must not run mid-build/mid-drain).
        // Act on the EXCHANGED value, not a pre-read - a request posted
        // between load and exchange must not be swallowed.
        if (g_vrstereoPending.load(std::memory_order_relaxed) >= 0) {
            int pending =
                g_vrstereoPending.exchange(-1, std::memory_order_relaxed);
            if (pending >= 0) apply_vrstereo(pending == 1);
        }
    }
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
    // The one-toggle: sequences 1t + camera mode + stereo (sticky across
    // loads). Applied on the game thread via the pending request - the
    // overlay may be drawing on the render thread.
    bool vrOn = g_forceInline.load(std::memory_order_relaxed) &&
                g_stereo.load(std::memory_order_relaxed);
    bool vrToggle = vrOn;
    if (ImGui::Checkbox("VR stereo (1t + camera mode + stereo)", &vrToggle) &&
        vrToggle != vrOn)
        request_vrstereo(vrToggle);
    bool hook1t = g_forceInline.load(std::memory_order_relaxed);
    bool poke1t = g_savedNumHwThreads.load(std::memory_order_relaxed) != 0;
    ImGui::Text("render %s  1t %s  hooks: build %s, submit %s, drain %s, "
                "flushpt %s%s%s",
                render_is_threaded() ? "MT" : "1T",
                hook1t ? (poke1t ? "hook+poke" : "hook")
                       : (poke1t ? "poke" : "off"),
                g_build.enabled.load(std::memory_order_relaxed) ? "ON" : "off",
                g_submit.enabled.load(std::memory_order_relaxed) ? "ON" : "off",
                g_drain.enabled.load(std::memory_order_relaxed) ? "ON" : "off",
                g_flushpoint.enabled.load(std::memory_order_relaxed) ? "ON"
                                                                     : "off",
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
