// BS2 render-substrate discovery instruments (session 26). The static route
// to the frame submit is dead on this build (docs/bioshock2/ENGINE_NOTES.md:
// the engine's ONLY static kernel32!SetEvent path is the virtually-dispatched
// event-object Trigger method; everything else the session-25 recon flagged
// turned out to be thread-suspend or CRT once-init machinery). So the submit
// is found LIVE, the way BS1 originally did it: sample SetEvent callers on
// the game thread during steady gameplay, take the per-frame-cadence one,
// walk the sampled return RVA back to the enclosing entry offline.
//
// Ported shapes from bioshock1r/scenedraw.cpp (values never copied): the
// lock-free KickSlot table, the MinHook-on-kernel32 sampler lifecycle, the
// conservative call-preceded stack scan, prologue-gated install/disable.

#include "game/bioshock2r/scenedraw.h"

#include "core/gfx/frame_inspector.h"
#include "core/hooks/d3d11_hook.h"
#include "core/util/crash.h"
#include "core/util/log.h"
#include "core/vr/openxr_runtime.h"
#include "game/bioshock2r/bones.h"
#include "game/bioshock2r/camera.h"
#include "game/bioshock2r/patterns.h"

#include <windows.h>
#include <MinHook.h>

#include <imgui.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <intrin.h>

namespace bvr::b2r::scenedraw {
namespace {

const uint8_t* g_imageBase = nullptr;
size_t g_imageSize = 0;
bvr::pattern_scan::ProcessImage g_image{};

// UGameEngine::Draw is `ret 0x10` with a live ECX this (the engine object) -
// __fastcall passthrough with a dummy EDX slot and 4 stack params is
// register/stack/cleanup-identical (same trick as the BS1 build detour).
using DrawFn = void(__fastcall*)(void* ecx, void* edx, void* a1, void* a2, void* a3,
                                 void* a4);
// FContentStreamingManager view hand-off: `ret 0xC`, live ECX this,
// args (FVector* loc, FRotator* rot, void* obj). Telemetry hook only.
using StreamFn = void(__fastcall*)(void* ecx, void* edx, void* loc, void* rot, void* a3);
// Render flush point: `ret 8`, live ECX this (the render mgr), 2 stack args
// (scene, view group). ARG COUNT IS LOAD-BEARING: ret imm / 4 == 2 - a
// mismatch pops the no-dump ESP RTC modal (see ENGINE_NOTES arg-chain trap).
using FlushPointFn = void(__fastcall*)(void* ecx, void* edx, void* scene, void* group);
// The drain (the flush point's inline branch body): thiscall, ZERO stack args
// (the inline call site pushes nothing and cleans nothing after).
using DrainFn = void(__fastcall*)(void* ecx, void* edx);

struct HookSlot {
    const char* name = nullptr;
    void* target = nullptr;
    void* original = nullptr; // trampoline; cast to the slot's signature
    bool created = false;     // game thread only
    std::atomic<bool> enabled{false};
};
HookSlot g_draw{"draw"};
HookSlot g_stream{"stream"};
HookSlot g_flushpoint{"flushpoint"};
HookSlot g_drain{"drain"};

// Nonzero exactly while a depth-0 hooked render call is in flight on that
// thread. The poll-gate deferral (inside_hooked_call) and the calcview
// in/out attribution key on these.
std::atomic<uint32_t> g_activeTid{0};
std::atomic<int> g_activeDepth{0};
std::atomic<uint32_t> g_calcInside{0};
std::atomic<uint32_t> g_calcOutside{0};
std::atomic<uint32_t> g_lastCalcTid{0};

// Telemetry (detour threads write, beat/overlay read).
std::atomic<uint32_t> g_drawEntries{0};
std::atomic<uint32_t> g_streamEntries{0};
std::atomic<uint32_t> g_lastDrawTid{0};
std::atomic<uint32_t> g_drawUs{0};
std::atomic<uint32_t> g_callerRvas[4]{};
std::atomic<int> g_dumpRemaining{0}; // stream-args dump lines left

// SequentialReentry controls + telemetry (session 26; premise REVISED in
// session 35). The original bet - "the threaded substrate needs no 1t
// because the Draw path has no submit handshake" - is refuted: Draw's tail
// calls a render flush point (see the flush-chain constants in patterns.h)
// whose threaded branch is a flag-test-then-INFINITE-wait, and the doubled
// draw races it. That is the vrstereo freeze. The session-26 comment's own
// escape clause applies: it proved unstable, so the 1t machinery is derived
// (session 36) and the doubled draw runs on it.
std::atomic<bool> g_doubleCall{false};
std::atomic<int> g_pulseCount{0};
std::atomic<float> g_secondYawDeg{30.0f}; // probe mode: yaw on pass 2
std::atomic<bool> g_stereo{false};
std::atomic<uint32_t> g_stereoSkips{0}; // pass 2 skipped (gate/stall)
// s62 [pair] instrument: split the two skip REASONS out of g_stereoSkips
// (which keeps its combined meaning for the heartbeat). Every skip here fires
// AFTER DrawDetour already pushed the LEFT tag, so each one is a lone-left
// pair break on the present side - the prime issue-#31 suspect on a slow rig.
std::atomic<uint32_t> g_skipCalcSilent{0};   // calcview_silent gate
std::atomic<uint32_t> g_skipPresentStall{0}; // presentDelta == 0 gate
std::atomic<uint32_t> g_secondCalls{0};
std::atomic<uint32_t> g_secondPassTid{0};
std::atomic<uint32_t> g_secondPassHits{0};
std::atomic<uint32_t> g_call2Us{0};
std::atomic<uint32_t> g_foreignCallerSkips{0}; // non-gameplay-caller Draws seen armed
std::atomic<bool> g_poisoned{false};
std::atomic<uint32_t> g_lastExcCode{0};
std::atomic<uint32_t> g_lastExcRva{0};
std::atomic<int> g_vrstereoPending{-1}; // -1 none, 0 off, 1 on
// Backend selector (session 36). `vrstereo on` arms AlternateEye unless srdev
// is set; SequentialReentry (the doubled draw) stays reachable for development
// but can no longer be armed by accident - it wedges on the flush handshake
// until the 1t port lands. g_vrStereoArmed is what the one-toggle UI reflects,
// backend-independent.
std::atomic<bool> g_srDev{false};
std::atomic<bool> g_vrStereoArmed{false};

// Flush-point instrumentation (session 36). A hook on the render flush point
// (patterns.h flush-chain constants) counts, for the SECOND draw of a pair,
// whether the gate's completion latch was already set (the engine's wait is
// skipped) or clear (the Wait(INFINITE) WILL be entered - the freeze window).
// wait2/s turns "did the resolution/FOV work make the race reachable?" into
// a number. Passive until 1t arms; it also proves the detour transparent on
// the hot path before anything is forced.
std::atomic<bool> g_inSecondDraw{false};
std::atomic<uint32_t> g_flushPointEntries{0};
std::atomic<uint32_t> g_waitTaken2{0}; // second-flush entries, latch CLEAR
std::atomic<uint32_t> g_latchSet2{0};  // second-flush entries, latch SET

// Structural 1t (session 36; BS1's session-8 cure duplicated with BS2's
// constants - never shared, never core). The flush-point detour forces the
// byte-confirmed INLINE branch: args into the render mgr, mode stamped
// single-threaded, drain called on the game thread. The hw-thread quotient
// is NEVER touched (its load-path consumers must see the true core count).
std::atomic<bool> g_forceInline{false};
std::atomic<uint32_t> g_forcedInline{0};
std::atomic<uint32_t> g_drainEntries{0};
std::atomic<uint32_t> g_drainGuardSkips{0};

// ---- Session 34: THE FREEZE - WHAT IT IS, AND WHAT IT IS NOT --------------
// Localised end to end with the stall watchdog (suspend the wedged thread,
// read its context, scan its stack for game-image return addresses, resolve
// the module of eip), then disassembled offline:
//
//   secondDraw stuck 4218 ms, eip in ntdll!NtWaitForSingleObject+0xC
//   nearest game frame returns into a thiscall wrapper that is literally
//       push [ebp+8] ; push [ecx+4] ; call KERNEL32!WaitForSingleObject
//   and its CALLER is:
//       mov esi, ecx                 ; this
//       cmp dword ptr [esi+8], 0     ; completion flag
//       jne  skip                    ; already done -> no wait
//       mov ecx, [esi+0x10]          ; event object
//       push -1                      ; INFINITE
//       call [eax+0x14]              ; virtual Wait(INFINITE)
//
// So the re-entered draw blocks in a CROSS-THREAD COMPLETION HANDSHAKE with an
// INFINITE timeout, guarded by a "already finished" flag - the classic shape in
// which a wakeup delivered between the flag test and the wait is lost forever.
// SequentialReentry doubles these handshakes per frame and shifts their timing,
// which is why the freeze is non-deterministic, stereo-only, and reproduces
// with no headset and no XR session. It is the engine's own race; VR makes it
// fire. It is NOT a VR bug and NOT caused by anything this session changed
// (reproduced identically on main).
//
// REFUTED, and the refutation is the useful part: the wrapper sitting next to
// the Wait one calls KERNEL32!PulseEvent, whose documented lost-wakeup
// behaviour fits the symptom exactly. Redirecting that import to SetEvent (which
// latches) was implemented, armed at init, and MEASURED: `PulseEvent calls 0`.
// The engine never calls it on this path. Adjacency in the binary is not a
// calling relationship, and the fix was inert - removed rather than left in as
// dead code that would look like a safeguard.
//
// ALSO REFUTED, by turning the freeze into something worse: bounding that
// INFINITE wait (IAT-clamp KERNEL32!WaitForSingleObject, scoped by a
// thread_local to exactly the re-entered call) stops the hang and the game
// CRASHES instead - `fault at 101E1A4B` repeated 86000 times. The caller
// ignores the wait's return value, so the timeout is not itself fatal; the
// engine simply proceeds to use a resource that is genuinely not ready. The
// wait cannot be shortcut, which also confirms it IS the freeze point.
//
// Session 26's premise is refuted too. Its comment above claims "the Draw path
// has no submit handshake (that spin-wait belongs to the streaming manager)",
// and that is the reason 1t was never ported to BS2. The doubled draw plainly
// reaches a blocking cross-thread wait.
//
// The principled fix is to remove the cross-thread handshake from the doubled
// draw entirely - i.e. render single-threaded while stereo is armed, which is
// exactly what BioShock 1 does (`reentry 1t`) and why BS1 does not hang here.
// Session 26 deliberately did not port that rung to BS2; this evidence is the
// reason to revisit that decision.
// ---- Session 34: THE RIG. Hiding the Big Daddy helmet. ----------------------
// Measured, not guessed. An A/B/A frame-dump triple at foreground FOV
// 60 / 137 / 60 from one standing position (docs/bioshock2/ENGINE_NOTES.md)
// showed that the ONLY foreground constants that move with the fov are the
// projection tangents and the terms that scale with them. The near plane holds
// at 10 UU and nothing resembling an eye position moves.
//
// So BS1'S ZOOM-PULL DOES NOT EXIST HERE. The foreground eye is fixed and a
// wider lens simply reveals more of a mesh that was always a few inches in
// front of it - the helmet's porthole ring. Screenshots confirm it: at fg 60
// the helmet is off-screen entirely, at fg 137 you are looking through the
// porthole and it owns most of the frame. There is no "push it to the
// periphery" position available, because at that distance the periphery IS
// most of the view; the only lever that gives the user their FOV back is not
// drawing it.
//
// Identified by INDEX COUNT, because inside one pass nothing else separates
// two meshes - the weapon and the helmet share the lens, the render target and
// the callstack. Counts are per-mesh and stable. The numbers and their
// derivation live in patterns.h, per the never-copy rule.
//
// KNOWN LIMITATION, stated rather than discovered later: an index count is a
// GLOBAL key, not a foreground-pass one. Any world mesh anywhere in the game
// that happens to carry the same count would be skipped too. Nothing of the
// sort was visible at the test location - the world renders complete and
// correct with the ring gone - but "not visible here" is not "cannot happen",
// which is why this ships DEFAULT OFF behind a toggle. Tightening the key to
// the foreground pass is the follow-up.
constexpr uint32_t kRigMaxCounts = 8;
std::atomic<uint32_t> g_rigCounts[kRigMaxCounts]{};
std::atomic<bool> g_rigHide{false};
std::atomic<uint32_t> g_rigSkips{0};

// Render thread, once per DrawIndexed. Integer compares only - no device
// calls, no logging: this is the hottest callback in the process.
bool rig_mesh_skip(unsigned indexCount) {
    if (!g_rigHide.load(std::memory_order_relaxed)) return false;
    for (uint32_t i = 0; i < kRigMaxCounts; ++i) {
        uint32_t want = g_rigCounts[i].load(std::memory_order_relaxed);
        if (want == 0) continue;
        if (want == indexCount) {
            g_rigSkips.fetch_add(1, std::memory_order_relaxed);
            return true;
        }
    }
    return false;
}
// Draw (game) thread only: present count at the previous depth-0 entry -
// doubling is skipped while presents are stalled (unfocused window).
uint32_t g_lastDrawPresentLow = 0;
// Draw-head camera probe: what Draw sees in the viewport camera actor's
// cached fields (the tick-side CalcView product = the pass-2 poke target).
std::atomic<float> g_camSrcLoc[3]{};
std::atomic<int32_t> g_camSrcRot[3]{};
std::atomic<uint32_t> g_camActorProbes{0};

// Heartbeat bookkeeping - beat thread (the Draw thread) only.
uint64_t g_lastBeatMs = 0;
uint32_t g_beatDraws = 0, g_beatStreams = 0, g_beatCalcIn = 0, g_beatCalcOut = 0,
         g_beatSecond = 0, g_beatWait2 = 0, g_beatSet2 = 0, g_beatForced = 0;
uint64_t g_beatPresents = 0;

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
        if (cur == 0) {
            slot.store(rva, std::memory_order_relaxed);
            return;
        }
    }
}

// --- kick samplers -----------------------------------------------------------
// Table of distinct (tid, caller-rva) pairs with counts, dumped on "off".
// BS2 extension over BS1's table: because the engine reaches SetEvent through
// FF15 wrapper methods (the direct return RVA lands in the wrapper, not the
// interesting caller), each slot deep-captures up to 3 further call-preceded
// exe return RVAs from the sampling thread's stack - on FIRST insertion only,
// so the steady-state cost stays two relaxed atomics per call.
constexpr int kDeepRvas = 3;
struct KickSlot {
    std::atomic<uint32_t> key{0};
    std::atomic<uint32_t> tid{0};
    std::atomic<uint32_t> rva{0};
    std::atomic<uint32_t> count{0};
    std::atomic<uint32_t> deep[kDeepRvas]{};
};

// Conservative call-preceded stack scan (BS1 log_game_stack heuristic): a
// stack dword qualifies if it points into the exe AND the bytes before it
// decode as a plausible CALL. Collect up to `cap` hits, skipping `skipRva`
// (the direct return RVA - already in the slot).
int scan_stack_rvas(uint32_t skipRva, uint32_t* out, int cap) {
    void** sp = reinterpret_cast<void**>(_AddressOfReturnAddress());
    int found = 0;
    for (int i = 0; i < 1024 && found < cap; ++i) {
        if (!bvr::pattern_scan::is_memory_valid(&sp[i], sizeof(void*))) break;
        const uint8_t* p = static_cast<const uint8_t*>(sp[i]);
        uint32_t rva = to_rva(p);
        if (rva == 0xFFFFFFFFu || rva == skipRva) continue;
        if (!bvr::pattern_scan::is_memory_valid(p - 6, 6)) continue;
        bool call = p[-5] == 0xE8 ||                                     // call rel32
                    (p[-6] == 0xFF && p[-5] == 0x15) ||                  // call [m32]
                    (p[-6] == 0xFF && p[-5] >= 0x90 && p[-5] <= 0x97) || // call [reg+d32]
                    (p[-2] == 0xFF && p[-1] >= 0xD0 && p[-1] <= 0xD7) || // call reg
                    (p[-2] == 0xFF && p[-1] >= 0x10 && p[-1] <= 0x17) || // call [reg]
                    (p[-3] == 0xFF && p[-2] >= 0x50 && p[-2] <= 0x57);   // call [reg+d8]
        if (!call) continue;
        bool dup = false;
        for (int j = 0; j < found; ++j)
            if (out[j] == rva) dup = true;
        if (!dup) out[found++] = rva;
    }
    return found;
}

// retAddr MUST be captured with _ReturnAddress() in the detour body itself -
// taken here it would name the detour, not the hooked function's caller.
void kick_note(KickSlot* slots, int nslots, std::atomic<bool>& gate, void* retAddr) {
    if (!gate.load(std::memory_order_relaxed)) return;
    uint32_t tid = GetCurrentThreadId();
    uint32_t rva = to_rva(retAddr);
    uint32_t key = (tid * 2654435761u) ^ rva;
    if (key == 0) key = 1;
    for (int i = 0; i < nslots; ++i) {
        KickSlot& s = slots[i];
        uint32_t k = s.key.load(std::memory_order_relaxed);
        if (k == key) {
            s.count.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        if (k == 0) {
            if (s.key.compare_exchange_strong(k, key, std::memory_order_relaxed)) {
                s.tid.store(tid, std::memory_order_relaxed);
                s.rva.store(rva, std::memory_order_relaxed);
                s.count.store(1, std::memory_order_relaxed);
                uint32_t deep[kDeepRvas] = {};
                int n = scan_stack_rvas(rva, deep, kDeepRvas);
                for (int j = 0; j < n; ++j)
                    s.deep[j].store(deep[j], std::memory_order_relaxed);
                return;
            }
        }
    }
}

void kick_report(const char* what, KickSlot* slots, int nslots) {
    BVR_LOG("[reentry] %s sampler OFF; distinct callers:", what);
    for (int i = 0; i < nslots; ++i) {
        KickSlot& s = slots[i];
        if (s.key.load(std::memory_order_relaxed) == 0) continue;
        uint32_t rva = s.rva.load(std::memory_order_relaxed);
        char deepBuf[64] = "";
        size_t len = 0;
        for (int j = 0; j < kDeepRvas; ++j) {
            uint32_t d = s.deep[j].load(std::memory_order_relaxed);
            if (!d || len >= sizeof deepBuf - 12) continue;
            len += _snprintf_s(deepBuf + len, sizeof deepBuf - len, _TRUNCATE, "%s0x%X",
                               len ? "," : " deep=", d);
        }
        BVR_LOG("[reentry]   tid=%u caller=%s0x%X count=%u%s",
                s.tid.load(std::memory_order_relaxed),
                rva == 0xFFFFFFFFu ? "(non-exe) " : "exe+", rva,
                s.count.load(std::memory_order_relaxed), deepBuf);
    }
}

void kick_reset(KickSlot* slots, int nslots) {
    for (int i = 0; i < nslots; ++i) {
        slots[i].key.store(0, std::memory_order_relaxed);
        slots[i].count.store(0, std::memory_order_relaxed);
        for (auto& d : slots[i].deep) d.store(0, std::memory_order_relaxed);
    }
}

// kick: process-wide kernel32!SetEvent hook (BS1-proven safe; short windows).
using SetEventFn = BOOL(WINAPI*)(HANDLE);
SetEventFn g_origSetEvent = nullptr;
void* g_setEventTarget = nullptr;
bool g_kickCreated = false; // game thread only
std::atomic<bool> g_kickSampling{false};
KickSlot g_kickSlots[10];
uint64_t g_kickOnPresents = 0; // game thread only: presents at sampler ON

BOOL WINAPI SetEventDetour(HANDLE h) {
    kick_note(g_kickSlots, 10, g_kickSampling, _ReturnAddress());
    return g_origSetEvent(h);
}

// kick2: hook on the event-object Trigger method itself. Its _ReturnAddress()
// is the engine-side (virtual) call site - no wrapper masking, no stack scan
// ambiguity. __thiscall with zero stack args returning BOOL: __fastcall with
// a dummy EDX slot is register/stack/cleanup-identical.
using TriggerFn = uint32_t(__fastcall*)(void* self, void* edx);
TriggerFn g_origTrigger = nullptr;
void* g_triggerTarget = nullptr;
bool g_trigger2Created = false; // game thread only
std::atomic<bool> g_trigger2Enabled{false};
std::atomic<bool> g_kick2Sampling{false};
KickSlot g_kick2Slots[10];
uint64_t g_kick2OnPresents = 0; // game thread only

uint32_t __fastcall TriggerDetour(void* self, void* edx) {
    kick_note(g_kick2Slots, 10, g_kick2Sampling, _ReturnAddress());
    return g_origTrigger(self, edx);
}

void kick_sampler(bool on) {
    if (on) {
        if (!g_kickCreated) {
            HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
            g_setEventTarget =
                k32 ? reinterpret_cast<void*>(GetProcAddress(k32, "SetEvent")) : nullptr;
            if (!g_setEventTarget) {
                BVR_LOG("[reentry] kick: SetEvent not resolved");
                return;
            }
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
        kick_reset(g_kickSlots, 10);
        MH_STATUS st = MH_EnableHook(g_setEventTarget);
        if (st != MH_OK) {
            BVR_LOG("[reentry] kick: MH_EnableHook failed: %s", MH_StatusToString(st));
            return;
        }
        g_kickOnPresents = bvr::d3d11_hook::present_count();
        g_kickSampling.store(true, std::memory_order_relaxed);
        BVR_LOG("[reentry] kick sampler ON (process-wide SetEvent hook)");
    } else {
        g_kickSampling.store(false, std::memory_order_relaxed);
        if (g_kickCreated) MH_DisableHook(g_setEventTarget);
        BVR_LOG("[reentry] kick window presents delta: %llu",
                static_cast<unsigned long long>(bvr::d3d11_hook::present_count() -
                                                g_kickOnPresents));
        kick_report("kick", g_kickSlots, 10);
    }
}

void kick2_sampler(bool on) {
    if (on) {
        if (!g_imageBase) {
            BVR_LOG("[reentry] kick2: no image base - init failed?");
            return;
        }
        if (!g_trigger2Created) {
            g_triggerTarget = const_cast<uint8_t*>(g_imageBase) + patterns::kEventTriggerRva;
            // Opcode-only prologue gate: the FF15 operand bytes embed the
            // ASLR-rebased IAT VA, so only the leading opcodes are stable.
            if (!bvr::pattern_scan::is_memory_valid(g_triggerTarget,
                                                    sizeof patterns::kEventTriggerPrologue) ||
                memcmp(g_triggerTarget, patterns::kEventTriggerPrologue,
                       sizeof patterns::kEventTriggerPrologue) != 0) {
                BVR_LOG("[reentry] kick2: Trigger prologue mismatch at %p - build "
                        "changed? REFUSING hook",
                        g_triggerTarget);
                return;
            }
            MH_STATUS st = MH_CreateHook(g_triggerTarget,
                                         reinterpret_cast<void*>(&TriggerDetour),
                                         reinterpret_cast<void**>(&g_origTrigger));
            if (st != MH_OK) {
                BVR_LOG("[reentry] kick2: MH_CreateHook(Trigger) failed: %s",
                        MH_StatusToString(st));
                return;
            }
            g_trigger2Created = true;
        }
        kick_reset(g_kick2Slots, 10);
        MH_STATUS st = MH_EnableHook(g_triggerTarget);
        if (st != MH_OK) {
            BVR_LOG("[reentry] kick2: MH_EnableHook failed: %s", MH_StatusToString(st));
            return;
        }
        g_kick2OnPresents = bvr::d3d11_hook::present_count();
        g_trigger2Enabled.store(true, std::memory_order_relaxed);
        g_kick2Sampling.store(true, std::memory_order_relaxed);
        BVR_LOG("[reentry] kick2 sampler ON (event Trigger method 0x%X hooked)",
                patterns::kEventTriggerRva);
    } else {
        g_kick2Sampling.store(false, std::memory_order_relaxed);
        if (g_trigger2Created) MH_DisableHook(g_triggerTarget);
        g_trigger2Enabled.store(false, std::memory_order_relaxed);
        BVR_LOG("[reentry] kick2 window presents delta: %llu",
                static_cast<unsigned long long>(bvr::d3d11_hook::present_count() -
                                                g_kick2OnPresents));
        kick_report("kick2", g_kick2Slots, 10);
    }
}

// Conservative one-shot stack scan on the game thread (from the CalcView
// dispatch): log every stack dword that points into the exe AND is preceded
// by a plausible CALL encoding. One log line; game thread only.
void log_game_stack() {
    uint32_t rvas[24] = {};
    int found = scan_stack_rvas(0, rvas, 24);
    char line[512];
    int pos = 0;
    for (int i = 0; i < found && pos < 480; ++i)
        pos += _snprintf_s(line + pos, sizeof(line) - pos, _TRUNCATE, " %X", rvas[i]);
    line[pos] = '\0';
    BVR_LOG("[reentry] calcstack (game tid %u):%s", GetCurrentThreadId(), line);
}

// --- substrate hooks (commit 2: pass-through + telemetry only) ---------------

bool install_slot(HookSlot& slot, uint32_t rva, void* detour, const uint8_t* prologue,
                  size_t prologueLen) {
    if (!g_imageBase) {
        BVR_LOG("[reentry] no image base - init failed?");
        return false;
    }
    if (slot.enabled.load(std::memory_order_relaxed)) {
        BVR_LOG("[reentry] %s hook already enabled", slot.name);
        return true;
    }
    slot.target = const_cast<uint8_t*>(g_imageBase) + rva;
    if (!slot.created) {
        if (!bvr::pattern_scan::is_memory_valid(slot.target, prologueLen) ||
            memcmp(slot.target, prologue, prologueLen) != 0) {
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
    MH_STATUS st = MH_EnableHook(slot.target); // self-enabling, never MH_ALL_HOOKS
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
    // thread could still be returning through it.
    MH_STATUS st = MH_DisableHook(slot.target);
    slot.enabled.store(false, std::memory_order_relaxed);
    BVR_LOG("[reentry] %s hook disabled (%s)", slot.name, MH_StatusToString(st));
}

// Draw-head camera probe: what the engine will render this frame, read from
// the viewport camera actor's cached fields. Guarded - a1 can be anything
// during teardown.
void probe_cam_actor(void* a1) {
    using namespace bvr::pattern_scan;
    if (!a1 || !is_memory_valid(static_cast<uint8_t*>(a1) + patterns::kViewportCamActorOffset,
                                sizeof(void*)))
        return;
    uint8_t* cam = *reinterpret_cast<uint8_t**>(static_cast<uint8_t*>(a1) +
                                                patterns::kViewportCamActorOffset);
    if (!cam || !is_memory_valid(cam + patterns::kCamActorLocOffset, 24)) return;
    const float* loc = reinterpret_cast<const float*>(cam + patterns::kCamActorLocOffset);
    const int32_t* rot = reinterpret_cast<const int32_t*>(cam + patterns::kCamActorRotOffset);
    for (int i = 0; i < 3; ++i) {
        g_camSrcLoc[i].store(loc[i], std::memory_order_relaxed);
        g_camSrcRot[i].store(rot[i], std::memory_order_relaxed);
    }
    g_camActorProbes.fetch_add(1, std::memory_order_relaxed);
}

// SEH filter for the SECOND Draw call only (the first stays unguarded -
// swallowing a vanilla-path crash would destroy real crash dumps). C++
// throws and stack overflow pass through to the game's own handling.
int reentry_filter(unsigned code, EXCEPTION_POINTERS* ep) {
    if (code == 0xE06D7363u) return EXCEPTION_CONTINUE_SEARCH;
    if (code == EXCEPTION_STACK_OVERFLOW) return EXCEPTION_CONTINUE_SEARCH;
    g_lastExcCode.store(code, std::memory_order_relaxed);
    g_lastExcRva.store(ep ? to_rva(ep->ExceptionRecord->ExceptionAddress) : 0,
                       std::memory_order_relaxed);
    return EXCEPTION_EXECUTE_HANDLER;
}

// No C++ objects in this frame (SEH + unwinding = C2712).
bool call_draw_guarded(DrawFn fn, void* ecx, void* edx, void* a1, void* a2, void* a3,
                       void* a4) {
    __try {
        fn(ecx, edx, a1, a2, a3, a4);
        return true;
    } __except (reentry_filter(GetExceptionCode(), GetExceptionInformation())) {
        return false;
    }
}

// SEH-guarded u32 read (BS1's shape, duplicated per the decoupling rule).
bool read_u32_guarded(const void* addr, uint32_t* out) {
    __try {
        *out = *static_cast<const volatile uint32_t*>(addr);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Beat-line render-mode label. Structural 1t first (the forced inline branch
// overrides whatever the engine's chain would pick); otherwise mirror the
// chain's confirmed LAST test, the hw-thread quotient. Read-only, always.
const char* render_mode_label() {
    if (g_forceInline.load(std::memory_order_relaxed) &&
        g_flushpoint.enabled.load(std::memory_order_relaxed))
        return "1T";
    uint32_t num = 0, div = 0;
    read_u32_guarded(g_imageBase + patterns::kHwThreadsRva, &num);
    read_u32_guarded(g_imageBase + patterns::kThreadDivisorRva, &div);
    return (div != 0 && num / div > 1) ? "MT" : "1T";
}

void __fastcall DrainDetour(void* self, void* edx) {
    // Empty-slot guard (BS1's drain+0x33 crash shape, BS2's offset): the
    // drain head loads the scene from [this+0x24] and dereferences it with
    // NO null check. A null slot can never be drained, so the skip is
    // universally safe and runs whenever this hook is installed - under 1t
    // it is what stands between a straggling pump wake (finding the slot the
    // inline drain already consumed) and that crash. Skips do NOT count as
    // drains.
    {
        uint32_t scene = 0;
        if (read_u32_guarded(static_cast<const uint8_t*>(self) +
                                 patterns::kMgrSceneSlotOffset,
                             &scene) &&
            scene == 0) {
            uint32_t n = g_drainGuardSkips.fetch_add(1, std::memory_order_relaxed) + 1;
            if (n <= 3)
                BVR_LOG("[reentry] drain-guard: skipped empty-slot drain #%u (tid %u, "
                        "[this+0x%X]=0 - the null-scene crash state)",
                        n, GetCurrentThreadId(), patterns::kMgrSceneSlotOffset);
            return;
        }
        // Session 38 hardening: a FREED-but-non-null scene passes the null
        // guard and faults inside the drain (the 0xDEDEDEDE DEP-execute dump
        // shape, gameplay-quit close). The engine pool fills freed memory
        // with 0xDE; a first dword that reads as pool poison, or does not
        // read at all, can never be a live scene. No layout assumption
        // beyond "readable" - a live scene's first dword always reads.
        if (scene != 0) {
            uint32_t head = 0;
            bool ok = read_u32_guarded(reinterpret_cast<const void*>(scene), &head);
            if (!ok || head == 0xDEDEDEDEu || head == 0xDDDDDDDDu) {
                uint32_t n = g_drainGuardSkips.fetch_add(1, std::memory_order_relaxed) + 1;
                if (n <= 3)
                    BVR_LOG("[reentry] drain-guard: skipped DEAD-scene drain #%u "
                            "(tid %u, scene=%08X head=%s%08X)",
                            n, GetCurrentThreadId(), scene, ok ? "" : "unreadable ",
                            head);
                return;
            }
        }
    }
    g_drainEntries.fetch_add(1, std::memory_order_relaxed);
    reinterpret_cast<DrainFn>(g_drain.original)(self, edx); // never guarded
}

// Forced-inline scene flush (BS1's session-8 structural 1t, BS2 constants):
// reproduce the flush point's byte-confirmed INLINE branch - copy the args
// into the render manager, stamp mode single-threaded, call the drain on
// this thread. The hw-thread quotient is NEVER touched. Returns 1 on
// success, 0 when the mgr global is still null (engine too early - let the
// original decide), -1 on a fault (the filter recorded code/rva). No C++
// objects in this frame (SEH + unwinding = C2712).
int force_inline_flush(void* scene, void* group) {
    __try {
        uint8_t* mgr = *reinterpret_cast<uint8_t* const*>(
            g_imageBase + patterns::kRenderMgrGlobalRva);
        if (!mgr) return 0;
        *reinterpret_cast<void**>(mgr + patterns::kMgrSceneSlotOffset) = scene;
        const uint32_t* src = static_cast<const uint32_t*>(group);
        uint32_t* dst = reinterpret_cast<uint32_t*>(mgr + patterns::kMgrViewGroupOffset);
        for (uint32_t i = 0; i < patterns::kMgrViewGroupDwords; ++i) dst[i] = src[i];
        *reinterpret_cast<uint32_t*>(mgr + patterns::kMgrThreadedFlagOffset) = 0;
        *reinterpret_cast<uint32_t*>(mgr + patterns::kMgrFlushSeenOffset) = 1;
        // Call through the drain's TARGET address, not a trampoline: the
        // DrainDetour (empty-slot guard + telemetry) must stay in the path.
        reinterpret_cast<DrainFn>(const_cast<uint8_t*>(g_imageBase) +
                                  patterns::kDrainRva)(mgr, nullptr);
        return 1;
    } __except (reentry_filter(GetExceptionCode(), GetExceptionInformation())) {
        return -1;
    }
}

// Render flush point detour (session 36). Two duties:
// 1. `reentry 1t`: with g_forceInline set, reproduce the INLINE branch via
//    force_inline_flush and never let the engine's threaded hand-off run -
//    the Wait(INFINITE) the doubled draw races is structurally removed.
// 2. wait2/set2 instrumentation: for second-draw flushes the ENGINE will
//    decide (1t off), sample the gate's completion latch ([mgr+4] -> gate,
//    [gate+8] -> latch) nanoseconds before the engine's own `cmp [esi+8],0`:
//    latch clear = the Wait(INFINITE) will be entered. Under 1t wait2/s
//    reads 0 - correctly, the freeze window is gone.
void __fastcall FlushPointDetour(void* ecx, void* edx, void* scene, void* group) {
    g_flushPointEntries.fetch_add(1, std::memory_order_relaxed);
    // Session 41 round 2 (the residual left-eye flicker): this is the LAST
    // game-thread point before the inline drain submits the pass. A skeleton
    // restamp that landed after the final PE dispatch of the pass slips past
    // the PE-lane repaint and renders raw for one eye - absorb-and-recompose
    // it here. Self-gating (one 48-byte sentinel per driven hand when fresh,
    // no-op otherwise), pass-2 verbatim semantics live inside pe_repaint.
    // Session 42: site 1 = flush point, for the flicker catch-phase split.
    bvr::b2r::bones::pe_repaint(1);
    // Session 38: forcing the inline branch KEEPS RUNNING through teardown, on
    // purpose. The first version of this gate stopped forcing once a close
    // message arrived, on the theory that the engine's own (threaded) decision
    // is vanilla behaviour - and the in-game quit then DEADLOCKED after
    // WM_DESTROY (blocked, 0 CPU, one thread left, ~2 min): handing the flush
    // back to a render-worker handshake while the workers are being torn down
    // waits forever. Inline draining needs no worker, and the drain guard below
    // is what makes it safe against a freed scene.
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
            BVR_LOG("[reentry] flushpoint forced-inline FAULTED code=0x%08X rva=0x%X "
                    "- 1t disarmed, stereo/doubling off, POISONED ('reentry reset' "
                    "to clear)",
                    g_lastExcCode.load(std::memory_order_relaxed),
                    g_lastExcRva.load(std::memory_order_relaxed));
            return;
        }
        // r == 0: mgr not created yet - the original handles pre-init state.
    }
    if (g_inSecondDraw.load(std::memory_order_relaxed) &&
        g_secondPassTid.load(std::memory_order_relaxed) == GetCurrentThreadId()) {
        using namespace bvr::pattern_scan;
        uint8_t* mgr = static_cast<uint8_t*>(ecx);
        if (mgr && is_memory_valid(mgr + 4, sizeof(void*))) {
            uint8_t* gate = *reinterpret_cast<uint8_t**>(mgr + 4);
            if (gate && is_memory_valid(gate + 8, sizeof(uint32_t))) {
                if (*reinterpret_cast<uint32_t*>(gate + 8))
                    g_latchSet2.fetch_add(1, std::memory_order_relaxed);
                else
                    g_waitTaken2.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }
    reinterpret_cast<FlushPointFn>(g_flushpoint.original)(ecx, edx, scene, group);
}

// The second Draw of a pair (game thread, depth 0, after the first call
// returned). ORIGINAL args pass through unchanged - the camera enters via
// the CalcView dispatch that re-fires inside the Draw (live-verified
// exactly-once-per-Draw), which the ProcessEvent seam replays as the RIGHT
// eye (second_pass_replay in camera.cpp).
void maybe_second_draw(void* ecx, void* edx, void* a1, void* a2, void* a3, void* a4,
                       uint32_t callerRva, uint32_t presentDelta) {
    if (g_poisoned.load(std::memory_order_relaxed)) return;
    // Session 38: no doubled draws once the window began closing - the engine
    // is tearing the scene down and mod machinery must not run on it.
    if (bvr::crash::teardown_seen()) return;
    bool pulse = false;
    if (g_pulseCount.load(std::memory_order_relaxed) > 0) {
        pulse = g_pulseCount.fetch_sub(1, std::memory_order_relaxed) > 0;
        if (!pulse) g_pulseCount.store(0, std::memory_order_relaxed);
    }
    bool continuous = g_doubleCall.load(std::memory_order_relaxed) ||
                      g_stereo.load(std::memory_order_relaxed);
    if (!pulse && !continuous) return;

    // Deny-by-default caller gate (the 1t-load-hazard lesson in BS2 form):
    // ONLY the census-verified gameplay dispatcher may be doubled. Loaders,
    // teardown paths, anything unknown = skip + count.
    if (callerRva != patterns::kSceneBuildGameplayRetRva) {
        g_foreignCallerSkips.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    // Scripted-camera hole: CalcView silent means the scene bypasses the
    // camera dispatch - a doubled frame would replay a stale base.
    if (camera::calcview_silent(400)) {
        g_stereoSkips.fetch_add(1, std::memory_order_relaxed);
        g_skipCalcSilent.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    // Present-stall guard: no present landed between the previous Draw and
    // this one (unfocused window / hitching render thread) - doubling has no
    // value and a stall is the prime wedge suspect. Pulses bypass it so the
    // instrument stays usable for A/B while paused.
    if (presentDelta == 0 && !pulse) {
        g_stereoSkips.fetch_add(1, std::memory_order_relaxed);
        g_skipPresentStall.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    // SESSION 34 - THE FREEZE IS IN HERE, AND FOCUS IS NOT THE GATE.
    // The pace trace caught the wedge in the act, flat, no headset, xr=none:
    //
    //   TRACE ... presents/s 0 | phase: - | stage: - | draw: secondDraw for 18105 ms
    //
    // `stage: -` means the Present detour had fully exited, so the wedge is
    // upstream of Present entirely - the game's own RE-ENTERED Draw never
    // returns. Every "hang after alt-tab" in this project is this, including
    // the one session 33 attributed to XR pacing (it reproduces with no XR
    // session at all).
    //
    // A foreground gate was tried here and REMOVED: the freeze reproduces with
    // the window focused (trace line above carries fg=1, recorded ~20 s before
    // the test even defocused the game). The alt-tab correlation that motivated
    // it was coincidence. Session 26's presentDelta guard is the same shape of
    // guess and does not prevent it either.
    //
    // Still unknown: WHY the second call blocks. It is not a deadlock on
    // anything this mod holds - the mod is not in the stack at that point.
    if (g_stereo.load(std::memory_order_relaxed)) bvr::vr::sr_push_eye(+1);
    g_secondPassTid.store(GetCurrentThreadId(), std::memory_order_relaxed);
    LARGE_INTEGER t0, t1, freq;
    QueryPerformanceCounter(&t0);
    // Session 34: the BS2 stereo freeze wedges with the Present detour fully
    // exited, so it is upstream of Present. This marker is what lets the pace
    // trace say whether the game is sitting inside the RE-ENTERED scene draw.
    bvr::vr::set_draw_stage("secondDraw");
    g_inSecondDraw.store(true, std::memory_order_relaxed);
    bool ok = call_draw_guarded(reinterpret_cast<DrawFn>(g_draw.original), ecx, edx, a1,
                                a2, a3, a4);
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
        g_doubleCall.store(false, std::memory_order_relaxed);
        g_stereo.store(false, std::memory_order_relaxed);
        BVR_LOG("[reentry] second draw FAULTED code=0x%08X rva=0x%X - POISONED "
                "(doubling disarmed; 'reentry reset' to clear)",
                g_lastExcCode.load(std::memory_order_relaxed),
                g_lastExcRva.load(std::memory_order_relaxed));
        return;
    }
    g_secondCalls.fetch_add(1, std::memory_order_relaxed);
    if (pulse)
        BVR_LOG("[reentry] pulse: second draw ok, call2=%u us, presentDelta=%u, "
                "presents now %llu",
                g_call2Us.load(std::memory_order_relaxed), presentDelta,
                static_cast<unsigned long long>(bvr::d3d11_hook::present_count()));
}

void heartbeat(uint64_t now) {
    if (g_lastBeatMs == 0) {
        g_lastBeatMs = now;
        g_beatDraws = g_drawEntries.load(std::memory_order_relaxed);
        g_beatStreams = g_streamEntries.load(std::memory_order_relaxed);
        g_beatCalcIn = g_calcInside.load(std::memory_order_relaxed);
        g_beatCalcOut = g_calcOutside.load(std::memory_order_relaxed);
        g_beatSecond = g_secondCalls.load(std::memory_order_relaxed);
        g_beatPresents = bvr::d3d11_hook::present_count();
        g_beatWait2 = g_waitTaken2.load(std::memory_order_relaxed);
        g_beatSet2 = g_latchSet2.load(std::memory_order_relaxed);
        g_beatForced = g_forcedInline.load(std::memory_order_relaxed);
        return;
    }
    if (now - g_lastBeatMs < 1000) return;
    uint32_t draws = g_drawEntries.load(std::memory_order_relaxed);
    uint32_t streams = g_streamEntries.load(std::memory_order_relaxed);
    uint32_t calcIn = g_calcInside.load(std::memory_order_relaxed);
    uint32_t calcOut = g_calcOutside.load(std::memory_order_relaxed);
    uint32_t seconds = g_secondCalls.load(std::memory_order_relaxed);
    uint32_t wait2 = g_waitTaken2.load(std::memory_order_relaxed);
    uint32_t set2 = g_latchSet2.load(std::memory_order_relaxed);
    uint32_t forced = g_forcedInline.load(std::memory_order_relaxed);
    uint64_t presents = bvr::d3d11_hook::present_count();
    BVR_LOG("[reentry] beat: mode=%s draws/s=%u 2nd/s=%u forced/s=%u wait2/s=%u "
            "set2/s=%u presents/s=%llu "
            "stream/s=%u calc "
            "in/out=%u/%u drawTid=%u presentTid=%u calcTid=%u drawUs=%u camSrc=(%.1f "
            "%.1f %.1f | %d %d %d) callers=%X,%X,%X,%X guardskips=%u%s",
            render_mode_label(),
            draws - g_beatDraws, seconds - g_beatSecond, forced - g_beatForced,
            wait2 - g_beatWait2, set2 - g_beatSet2,
            static_cast<unsigned long long>(presents - g_beatPresents),
            streams - g_beatStreams, calcIn - g_beatCalcIn, calcOut - g_beatCalcOut,
            g_lastDrawTid.load(std::memory_order_relaxed),
            bvr::d3d11_hook::last_present_tid(),
            g_lastCalcTid.load(std::memory_order_relaxed),
            g_drawUs.load(std::memory_order_relaxed),
            g_camSrcLoc[0].load(std::memory_order_relaxed),
            g_camSrcLoc[1].load(std::memory_order_relaxed),
            g_camSrcLoc[2].load(std::memory_order_relaxed),
            g_camSrcRot[0].load(std::memory_order_relaxed),
            g_camSrcRot[1].load(std::memory_order_relaxed),
            g_camSrcRot[2].load(std::memory_order_relaxed),
            g_callerRvas[0].load(std::memory_order_relaxed),
            g_callerRvas[1].load(std::memory_order_relaxed),
            g_callerRvas[2].load(std::memory_order_relaxed),
            g_callerRvas[3].load(std::memory_order_relaxed),
            g_drainGuardSkips.load(std::memory_order_relaxed),
            g_poisoned.load(std::memory_order_relaxed) ? " POISONED" : "");
    g_lastBeatMs = now;
    g_beatDraws = draws;
    g_beatStreams = streams;
    g_beatCalcIn = calcIn;
    g_beatCalcOut = calcOut;
    g_beatSecond = seconds;
    g_beatPresents = presents;
    g_beatWait2 = wait2;
    g_beatSet2 = set2;
    g_beatForced = forced;

    // ---- Session 42: the [flick] minute line (flicker diagnosis) -----------
    // 60 s sub-window on the same game-thread beat host. Deltas per window;
    // `min` is ABSOLUTE minutes since the first beat so the user's "~10 min"
    // onset reads straight off the log. dmax is the window's worst
    // write->catch latency per phase - the survivor discriminator (a catch
    // that sat most of the pass plausibly rendered before it was caught).
    static uint64_t s_flickStartMs = 0;
    static uint64_t s_flickWindowMs = 0;
    static bones::FlickerStats s_prev = {};
    static bvr::vr::PairProbe s_prevPair = {};
    static uint32_t s_prevSkipSilent = 0, s_prevSkipStall = 0, s_prevSkipForeign = 0;
    static uint32_t s_prevStreams = 0, s_prevWait2 = 0, s_prevSet2 = 0,
                    s_prevFlush = 0;
    if (s_flickWindowMs == 0) {
        s_flickStartMs = now;
        s_flickWindowMs = now;
        bones::flicker_snapshot(&s_prev);
        bvr::vr::pair_probe(&s_prevPair);
        s_prevSkipSilent = g_skipCalcSilent.load(std::memory_order_relaxed);
        s_prevSkipStall = g_skipPresentStall.load(std::memory_order_relaxed);
        s_prevSkipForeign = g_foreignCallerSkips.load(std::memory_order_relaxed);
        s_prevStreams = streams;
        s_prevWait2 = wait2;
        s_prevSet2 = set2;
        s_prevFlush = g_flushPointEntries.load(std::memory_order_relaxed);
        return;
    }
    if (now - s_flickWindowMs < 60000) return;
    s_flickWindowMs = now;
    bones::FlickerStats cur = {};
    bones::flicker_snapshot(&cur);
    uint32_t flush = g_flushPointEntries.load(std::memory_order_relaxed);
    if (bones::flicker_log()) {
        uint32_t d[4][2];
        for (int p = 0; p < 4; ++p)
            for (int k = 0; k < 2; ++k)
                d[p][k] = cur.catches[p][k] - s_prev.catches[p][k];
        BVR_LOG("[flick] min=%llu pe1=%u/%u pe2=%u/%u fl1=%u/%u fl2=%u/%u "
                "dmax=%u/%u/%u/%u ms drv=%u/%u adopts=%u/%u wAdopt=%u "
                "stream=%u wait2=%u set2=%u flush=%u world=%u resc=%u",
                static_cast<unsigned long long>((now - s_flickStartMs) / 60000),
                d[0][0], d[0][1], d[1][0], d[1][1], d[2][0], d[2][1], d[3][0],
                d[3][1], cur.dmaxMs[0], cur.dmaxMs[1], cur.dmaxMs[2], cur.dmaxMs[3],
                cur.driveAdoptEvents[0] - s_prev.driveAdoptEvents[0],
                cur.driveAdoptEvents[1] - s_prev.driveAdoptEvents[1],
                cur.adopts[0] - s_prev.adopts[0], cur.adopts[1] - s_prev.adopts[1],
                cur.wAdopts - s_prev.wAdopts, streams - s_prevStreams,
                wait2 - s_prevWait2, set2 - s_prevSet2, flush - s_prevFlush,
                cur.worldChanges - s_prev.worldChanges,
                cur.wRescans - s_prev.wRescans);
    }
    // ---- Session 62: the [pair] minute line (issue #31 widening) -----------
    // The reporter's [flick] came back CLEAN, which the playbook reads as "the
    // surviving defect is NOT on the driven bone banks" - so this line covers
    // the layer [flick] cannot see: per-eye present pairing. All quantities
    // are per-window deltas except age (current window max, ms). A healthy
    // minute reads pairs~=capL~=capR, everything else 0, age < ~20 ms.
    //   ab: total pair aborts = exp (hold expired) + lft (second left while a
    //       pair was open = lone-left) + unt (untagged completed the pair)
    //   stale: stereo submits whose eye capture was > 50 ms old - the direct
    //       measure of "the left eye showed an old frame"
    //   skip: pass-2 skips AFTER the left tag was pushed (silent/stall/foreign)
    //   ring: pushed/popped/dropped/skew-cleared; reb: swapchain rebuilds
    if (bones::flicker_log()) {
        bvr::vr::PairProbe pp = {};
        bvr::vr::pair_probe(&pp);
        uint32_t skipSilent = g_skipCalcSilent.load(std::memory_order_relaxed);
        uint32_t skipStall = g_skipPresentStall.load(std::memory_order_relaxed);
        uint32_t skipForeign = g_foreignCallerSkips.load(std::memory_order_relaxed);
        BVR_LOG("[pair] min=%llu pairs=%u ab=%u(exp=%u lft=%u unt=%u) "
                "cap=%u/%u stale=%u/%u age<=%u/%u ms acqF=%u waitF=%u untag=%u "
                "skip=%u/%u/%u ring=%u/%u/%u skew=%u reb=%u mirror=%d",
                static_cast<unsigned long long>((now - s_flickStartMs) / 60000),
                pp.pairs - s_prevPair.pairs, pp.aborts - s_prevPair.aborts,
                pp.abortExpired - s_prevPair.abortExpired,
                pp.abortLeft - s_prevPair.abortLeft,
                pp.abortUntagged - s_prevPair.abortUntagged,
                pp.cap[0] - s_prevPair.cap[0], pp.cap[1] - s_prevPair.cap[1],
                pp.staleL - s_prevPair.staleL, pp.staleR - s_prevPair.staleR,
                pp.ageMaxL, pp.ageMaxR,
                pp.acqFail - s_prevPair.acqFail,
                pp.waitFail - s_prevPair.waitFail,
                pp.untaggedProj - s_prevPair.untaggedProj,
                skipSilent - s_prevSkipSilent, skipStall - s_prevSkipStall,
                skipForeign - s_prevSkipForeign,
                pp.ringPushed - s_prevPair.ringPushed,
                pp.ringPopped - s_prevPair.ringPopped,
                pp.ringDropped - s_prevPair.ringDropped,
                pp.ringCleared - s_prevPair.ringCleared,
                pp.rebuilds - s_prevPair.rebuilds, pp.mirrorOn ? 1 : 0);
        s_prevPair = pp;
        s_prevSkipSilent = skipSilent;
        s_prevSkipStall = skipStall;
        s_prevSkipForeign = skipForeign;
    }
    s_prev = cur;
    s_prevStreams = streams;
    s_prevWait2 = wait2;
    s_prevSet2 = set2;
    s_prevFlush = flush;
}

void __fastcall DrawDetour(void* ecx, void* edx, void* a1, void* a2, void* a3, void* a4) {
    uint32_t tid = GetCurrentThreadId();
    uint32_t callerRva = to_rva(_ReturnAddress());
    g_lastDrawTid.store(tid, std::memory_order_relaxed);
    note_caller(callerRva);
    g_drawEntries.fetch_add(1, std::memory_order_relaxed);
    int depth = g_activeDepth.fetch_add(1, std::memory_order_relaxed);
    uint32_t presentLowAtEntry = static_cast<uint32_t>(bvr::d3d11_hook::present_count());
    if (depth == 0) {
        g_activeTid.store(tid, std::memory_order_relaxed);
        probe_cam_actor(a1);
        // Pass-1 eye tag: this Draw's present captures as the LEFT eye. The
        // camera side caches the driven base and applies -IPD/2 on the
        // CalcView dispatch inside this call. A later pass-2 skip leaves a
        // lone tag; core's tag ring self-heals at depth > 2.
        if (g_stereo.load(std::memory_order_relaxed) &&
            !g_poisoned.load(std::memory_order_relaxed) &&
            callerRva == patterns::kSceneBuildGameplayRetRva)
            bvr::vr::sr_push_eye(-1);
    }

    LARGE_INTEGER t0, t1, freq;
    QueryPerformanceCounter(&t0);
    reinterpret_cast<DrawFn>(g_draw.original)(ecx, edx, a1, a2, a3, a4);
    QueryPerformanceCounter(&t1);
    QueryPerformanceFrequency(&freq);
    g_drawUs.store(static_cast<uint32_t>((t1.QuadPart - t0.QuadPart) * 1000000 /
                                         freq.QuadPart),
                   std::memory_order_relaxed);

    if (depth == 0) {
        // Second pass while the depth/tid latch is still held: the pass-2
        // CalcView dispatch must attribute as inside, and the command poller
        // must stay deferred for the whole pair.
        maybe_second_draw(ecx, edx, a1, a2, a3, a4, callerRva,
                          presentLowAtEntry - g_lastDrawPresentLow);
        g_lastDrawPresentLow = presentLowAtEntry;
        g_activeTid.store(0, std::memory_order_relaxed);
        heartbeat(GetTickCount64());
    }
    g_activeDepth.fetch_sub(1, std::memory_order_relaxed);
}

void __fastcall StreamViewDetour(void* ecx, void* edx, void* loc, void* rot, void* a3) {
    g_streamEntries.fetch_add(1, std::memory_order_relaxed);
    if (g_dumpRemaining.load(std::memory_order_relaxed) > 0) {
        g_dumpRemaining.fetch_sub(1, std::memory_order_relaxed);
        const float* l = static_cast<const float*>(loc);
        const int32_t* r = static_cast<const int32_t*>(rot);
        bool ok = bvr::pattern_scan::is_memory_valid(loc, 12) &&
                  bvr::pattern_scan::is_memory_valid(rot, 12);
        if (ok)
            BVR_LOG("[reentry] stream view: tid=%u loc=(%.1f %.1f %.1f) rot=(%d %d %d) "
                    "obj=%p",
                    GetCurrentThreadId(), l[0], l[1], l[2], r[0], r[1], r[2], a3);
    }
    reinterpret_cast<StreamFn>(g_stream.original)(ecx, edx, loc, rot, a3);
}

// The one-toggle compound (game thread only, outside hooked calls). Session
// 36: full-rate SequentialReentry ON 1t is the default - BS1's shipped
// design, earned back by the acceptance soak (10 min in the user's save,
// zero unrecovered watchdogs) after the 1t port removed the flush-handshake
// race that session 35 derived. AlternateEye remains reachable via `vraer`;
// the doubled draw WITHOUT 1t remains reachable via `reentry srdev on` (the
// freeze-repro lane, dev only).
void apply_vrstereo(bool on) {
    if (on && bvr::crash::teardown_seen()) {
        BVR_LOG("[reentry] VRSTEREO ON refused - window teardown in progress");
        return;
    }
    if (on) {
        // BS1's arming order, load-bearing: 1t FIRST (remove the flush
        // handshake), then camera mode, then the doubled draw.
        BVR_LOG("[reentry] VRSTEREO ON (full-rate SR stereo on 1t): "
                "1t -> camera mode -> stereo");
        if (!g_forceInline.load(std::memory_order_relaxed)) handle_command("1t on");
        bvr::vr::set_enabled(true);
        bvr::vr::set_camera_mode(true);
        // SR presents an L/R pair per tick; pair pacing is meaningful only
        // here, so it is armed here rather than in the preset (session 36).
        bvr::vr::set_sr_pair_pacing(true);
        if (!g_stereo.load(std::memory_order_relaxed)) handle_command("stereo on");
        bool ok = g_forceInline.load(std::memory_order_relaxed) &&
                  g_stereo.load(std::memory_order_relaxed);
        BVR_LOG("[reentry] VRSTEREO %s (1t=%d stereo=%d; sticky across loads via "
                "the gameplay-caller gate; 'vrstereo off' reverses)",
                ok ? "READY" : "INCOMPLETE - see refusals above",
                g_forceInline.load(std::memory_order_relaxed) ? 1 : 0,
                g_stereo.load(std::memory_order_relaxed) ? 1 : 0);
        g_vrStereoArmed.store(ok, std::memory_order_relaxed);
    } else {
        // Symmetric OFF: disarm BOTH backends regardless of how ON was reached,
        // so an srdev flip between on and off cannot strand one of them armed
        // (the asymmetric-off trap vrcam had, session 33). BS1's reverse
        // order: stereo off before 1t off, so the doubled draw never runs a
        // frame on the threaded substrate.
        BVR_LOG("[reentry] VRSTEREO OFF: alternate-eye + stereo -> camera mode -> 1t");
        bvr::vr::set_alternate_eye(false);
        if (g_stereo.load(std::memory_order_relaxed)) handle_command("stereo off");
        bvr::vr::set_camera_mode(false);
        if (g_forceInline.load(std::memory_order_relaxed)) handle_command("1t off");
        g_vrStereoArmed.store(false, std::memory_order_relaxed);
    }
}

} // namespace

void init(const bvr::pattern_scan::ProcessImage& image) {
    g_imageBase = image.base;
    g_imageSize = image.size;
    g_image = image;
    // The rig veto is registered unconditionally but does nothing until
    // g_rigHide is set - core's hook is a null check per draw otherwise.
    bvr::frame_inspector::set_mesh_skip(&rig_mesh_skip);
    // The helmet's meshes, derived from the foreground pass of a full frame
    // dump and confirmed by screenshot A/B (patterns.h carries the numbers and
    // their derivation). HIDDEN BY DEFAULT since session 36 - the user judged
    // the porthole in-headset ("annoying") and instructed the flip; edge-of-
    // FOV placement is not possible for a single mesh. `reentry rig show` /
    // the F10 checkbox restores it. KNOWN CAVEAT, first check next session:
    // the index count is a GLOBAL key, not a foreground-pass one - an
    // unrelated 3810-index mesh in an untested map would vanish too.
    for (uint32_t i = 0; i < patterns::kRigMeshCount && i < kRigMaxCounts; ++i)
        g_rigCounts[i].store(patterns::kRigMeshIndexCounts[i], std::memory_order_relaxed);
    g_rigHide.store(true, std::memory_order_relaxed);
}

void handle_command(const char* args) {
    char verb[16] = {};
    int consumed = 0;
    if (sscanf_s(args, "%15s%n", verb, static_cast<unsigned>(sizeof verb), &consumed) != 1) {
        BVR_LOG("[reentry] command needs a verb: vrstereo on|off|1t on|off|"
                "srdev on|off|hook [draw|stream]|unhook|dump <n>|kick on|off|"
                "kick2 on|off|calcstack|status");
        return;
    }
    const char* rest = args + consumed;
    while (*rest == ' ' || *rest == '\t') ++rest;

    if (strcmp(verb, "vrstereo") == 0) {
        apply_vrstereo(strncmp(rest, "on", 2) == 0);
    } else if (strcmp(verb, "rig") == 0) {
        // rig hide|show | skip <indexCount> | clear | status
        // `skip` is the identification lane: nominate an index count from a
        // frame dump's foreground cluster, then look. Nothing here is guessed
        // from a draw count - the only honest way to name a mesh is to make it
        // disappear and see what went with it.
        if (strncmp(rest, "hide", 4) == 0 || strncmp(rest, "show", 4) == 0) {
            bool hide = strncmp(rest, "hide", 4) == 0;
            // Zero the counter on every arm, so `status` reports THIS episode.
            // A cumulative total across a whole identification sweep is not an
            // instrument - it answered "is 3810 over-skipping?" with a number
            // that was mostly other candidates' draws.
            g_rigSkips.store(0, std::memory_order_relaxed);
            g_rigHide.store(hide, std::memory_order_relaxed);
            BVR_LOG("[reentry] rig %s", hide ? "HIDDEN" : "shown");
        } else if (strncmp(rest, "skip", 4) == 0) {
            unsigned n = 0;
            if (sscanf_s(rest + 4, "%u", &n) == 1 && n > 0) {
                bool placed = false;
                for (uint32_t i = 0; i < kRigMaxCounts && !placed; ++i) {
                    if (g_rigCounts[i].load(std::memory_order_relaxed) == 0) {
                        g_rigCounts[i].store(n, std::memory_order_relaxed);
                        placed = true;
                    }
                }
                BVR_LOG("[reentry] rig skip %u %s", n,
                        placed ? "armed (`rig hide` to apply)" : "REFUSED - list full");
            } else {
                BVR_LOG("[reentry] usage: rig skip <indexCount>");
            }
        } else if (strncmp(rest, "clear", 5) == 0) {
            for (uint32_t i = 0; i < kRigMaxCounts; ++i)
                g_rigCounts[i].store(0, std::memory_order_relaxed);
            BVR_LOG("[reentry] rig list cleared");
        } else {
            char list[128] = {};
            int n = 0;
            for (uint32_t i = 0; i < kRigMaxCounts; ++i) {
                uint32_t v = g_rigCounts[i].load(std::memory_order_relaxed);
                if (v && n >= 0 && n < static_cast<int>(sizeof(list)) - 16)
                    n += sprintf_s(list + n, sizeof(list) - n, "%u ", v);
            }
            BVR_LOG("[reentry] rig %s | index counts: %s| skipped %u draws (core %u)",
                    g_rigHide.load(std::memory_order_relaxed) ? "HIDDEN" : "shown",
                    list[0] ? list : "(none) ",
                    g_rigSkips.load(std::memory_order_relaxed),
                    bvr::frame_inspector::mesh_skips());
        }
    } else if (strcmp(verb, "stereo") == 0) {
        if (strncmp(rest, "on", 2) == 0) {
            // Without 1t the doubled draw wedges on the flush handshake
            // (session 35). The raw seam therefore requires 1t to be armed -
            // or srdev, the deliberate dev/repro escape that runs the doubled
            // draw on the threaded substrate anyway.
            if (!g_forceInline.load(std::memory_order_relaxed) &&
                !g_srDev.load(std::memory_order_relaxed)) {
                BVR_LOG("[reentry] stereo REFUSED: no 1t armed and the doubled draw "
                        "races Draw's tail flush handshake ('vrstereo on' arms the "
                        "full ladder; 'reentry srdev on' deliberately runs the raw "
                        "doubled draw for repro work)");
                return;
            }
            if (g_poisoned.load(std::memory_order_relaxed)) {
                BVR_LOG("[reentry] stereo refused: poisoned ('reentry reset' first)");
                return;
            }
            if (!g_draw.enabled.load(std::memory_order_relaxed)) {
                if (!patterns::verify_draw_chain(g_image)) return;
                if (!install_slot(g_draw, patterns::kSceneBuildRva,
                                  reinterpret_cast<void*>(&DrawDetour),
                                  patterns::kSceneBuildPrologue,
                                  sizeof patterns::kSceneBuildPrologue))
                    return;
            }
            // The wait2/set2 instrument (best-effort here; the 1t verb makes
            // this same hook mandatory before anything is forced).
            if (!g_flushpoint.enabled.load(std::memory_order_relaxed)) {
                if (patterns::verify_flush_chain(g_image))
                    install_slot(g_flushpoint, patterns::kFlushPointRva,
                                 reinterpret_cast<void*>(&FlushPointDetour),
                                 patterns::kFlushPointPrologue,
                                 sizeof patterns::kFlushPointPrologue);
                if (!g_flushpoint.enabled.load(std::memory_order_relaxed))
                    BVR_LOG("[reentry] flush-point hook not armed - wait2/set2 "
                            "will read 0 (stereo still runs)");
            }
            g_stereo.store(true, std::memory_order_relaxed);
            BVR_LOG("[reentry] STEREO ON (threaded substrate; every gameplay draw "
                    "doubles L/R, eye-tagged for per-present capture)");
        } else {
            g_stereo.store(false, std::memory_order_relaxed);
            BVR_LOG("[reentry] stereo off");
        }
    } else if (strcmp(verb, "pulse") == 0) {
        int n = 0;
        if (sscanf_s(rest, "%d", &n) != 1 || n <= 0) n = 1;
        if (n > 8) n = 8;
        if (!g_draw.enabled.load(std::memory_order_relaxed)) {
            BVR_LOG("[reentry] pulse needs the draw hook ('reentry hook' first)");
            return;
        }
        g_pulseCount.store(n, std::memory_order_relaxed);
        BVR_LOG("[reentry] pulse armed: next %d gameplay draw(s) double (yaw %.1f on "
                "pass 2 unless stereo)",
                n, g_secondYawDeg.load(std::memory_order_relaxed));
    } else if (strcmp(verb, "on") == 0) {
        if (!g_draw.enabled.load(std::memory_order_relaxed)) {
            BVR_LOG("[reentry] 'on' needs the draw hook ('reentry hook' first)");
            return;
        }
        g_doubleCall.store(true, std::memory_order_relaxed);
        BVR_LOG("[reentry] continuous double-draw ON (yaw %.1f on pass 2)",
                g_secondYawDeg.load(std::memory_order_relaxed));
    } else if (strcmp(verb, "off") == 0) {
        g_doubleCall.store(false, std::memory_order_relaxed);
        g_pulseCount.store(0, std::memory_order_relaxed);
        BVR_LOG("[reentry] double-draw off");
    } else if (strcmp(verb, "yaw") == 0) {
        float v = 0.0f;
        if (sscanf_s(rest, "%f", &v) == 1) {
            g_secondYawDeg.store(v, std::memory_order_relaxed);
            BVR_LOG("[reentry] second-pass yaw = %.1f deg", v);
        }
    } else if (strcmp(verb, "1t") == 0) {
        // STRUCTURAL single-threading (BS1's session-8 cure, BS2 constants):
        // the flush-point hook forces the inline branch; the hw-thread
        // quotient global is untouched. Pre-world arming is inert until the
        // render manager exists (the detour falls through while mgr is null).
        // INSTALL ORDER IS LOAD-BEARING: drain guard FIRST - once flushes
        // drain inline, any straggling pump wake finds an EMPTY scene slot,
        // and only the guard stands between that and the null-scene crash.
        if (strncmp(rest, "on", 2) == 0) {
            if (g_forceInline.load(std::memory_order_relaxed)) {
                BVR_LOG("[reentry] 1t already on");
            } else if (g_poisoned.load(std::memory_order_relaxed)) {
                BVR_LOG("[reentry] 1t refused: POISONED ('reentry reset' to clear)");
            } else if (!install_slot(g_drain, patterns::kDrainRva,
                                     reinterpret_cast<void*>(&DrainDetour),
                                     patterns::kDrainPrologue,
                                     sizeof patterns::kDrainPrologue)) {
                BVR_LOG("[reentry] 1t refused: drain-guard install failed");
            } else if (!(patterns::verify_flush_chain(g_image) &&
                         install_slot(g_flushpoint, patterns::kFlushPointRva,
                                      reinterpret_cast<void*>(&FlushPointDetour),
                                      patterns::kFlushPointPrologue,
                                      sizeof patterns::kFlushPointPrologue))) {
                BVR_LOG("[reentry] 1t refused: flush-point hook install failed");
            } else {
                g_forceInline.store(true, std::memory_order_relaxed);
                BVR_LOG("[reentry] 1t ON (structural): flush-point forces the inline "
                        "branch - scene flushes drain on the game thread, hw-thread "
                        "quotient UNTOUCHED (loaders see the true core count)");
            }
        } else {
            if (!g_forceInline.exchange(false, std::memory_order_relaxed)) {
                BVR_LOG("[reentry] 1t was not on");
            } else {
                BVR_LOG("[reentry] 1t off: flush-point back to the engine's own "
                        "decision (hooks stay installed, passive)%s",
                        g_stereo.load(std::memory_order_relaxed)
                            ? " (WARNING: stereo still on - now on the THREADED "
                              "substrate, which wedges)"
                            : "");
            }
        }
    } else if (strcmp(verb, "srdev") == 0) {
        // Since the default flip (session 36) this no longer selects the
        // backend - it is the dev/repro escape that lets the raw `stereo`
        // verb run the doubled draw WITHOUT 1t (the freeze-repro lane).
        bool on = strncmp(rest, "on", 2) == 0;
        g_srDev.store(on, std::memory_order_relaxed);
        BVR_LOG("[reentry] srdev %s: raw 'stereo on' without 1t is %s (the doubled "
                "draw on the threaded substrate WEDGES - repro use only)",
                on ? "on" : "off", on ? "ALLOWED" : "refused");
    } else if (strcmp(verb, "reset") == 0) {
        g_poisoned.store(false, std::memory_order_relaxed);
        BVR_LOG("[reentry] poison cleared (last fault code=0x%08X rva=0x%X)",
                g_lastExcCode.load(std::memory_order_relaxed),
                g_lastExcRva.load(std::memory_order_relaxed));
    } else if (strcmp(verb, "hook") == 0) {
        if (strncmp(rest, "stream", 6) == 0) {
            install_slot(g_stream, patterns::kStreamViewRva,
                         reinterpret_cast<void*>(&StreamViewDetour),
                         patterns::kStreamViewPrologue,
                         sizeof patterns::kStreamViewPrologue);
        } else { // default: UGameEngine::Draw (the SR seam candidate)
            // Build-identity gate first: the vtable slot chain must land on
            // the constant (pure image reads; a wrong candidate refuses).
            if (!patterns::verify_draw_chain(g_image)) return;
            install_slot(g_draw, patterns::kSceneBuildRva,
                         reinterpret_cast<void*>(&DrawDetour),
                         patterns::kSceneBuildPrologue,
                         sizeof patterns::kSceneBuildPrologue);
        }
    } else if (strcmp(verb, "unhook") == 0) {
        g_doubleCall.store(false, std::memory_order_relaxed);
        g_pulseCount.store(0, std::memory_order_relaxed);
        g_stereo.store(false, std::memory_order_relaxed);
        // 1t must disarm BEFORE its hooks drop: with the flush detour gone,
        // nothing may still force inline drains past a disabled drain guard.
        g_forceInline.store(false, std::memory_order_relaxed);
        disable_slot(g_draw);
        disable_slot(g_stream);
        disable_slot(g_flushpoint);
        disable_slot(g_drain);
    } else if (strcmp(verb, "dump") == 0) {
        int n = 0;
        if (sscanf_s(rest, "%d", &n) != 1 || n <= 0) n = 8;
        if (n > 32) n = 32;
        g_dumpRemaining.store(n, std::memory_order_relaxed);
        BVR_LOG("[reentry] dump armed: next %d stream-view calls%s", n,
                g_stream.enabled.load(std::memory_order_relaxed)
                    ? ""
                    : " (stream hook is OFF - 'reentry hook stream' first)");
    } else if (strcmp(verb, "kick") == 0) {
        kick_sampler(strncmp(rest, "on", 2) == 0);
    } else if (strcmp(verb, "kick2") == 0) {
        kick2_sampler(strncmp(rest, "on", 2) == 0);
    } else if (strcmp(verb, "calcstack") == 0) {
        g_calcstackPending.store(1, std::memory_order_relaxed);
        BVR_LOG("[reentry] calcstack armed (next CalcView dispatch logs a stack scan)");
    } else if (strcmp(verb, "status") == 0) {
        BVR_LOG("[reentry] status: mode=%s draw=%s stream=%s 1t=%d forced=%u "
                "guardskips=%u drains=%u wait2=%u set2=%u stereo=%d double=%d pulse=%d "
                "yaw=%.1f seconds=%u skips=%u foreign=%u 2ndHits=%u call2Us=%u "
                "poisoned=%d kick=%d kick2=%d calcview in/out %u/%u draws=%u",
                render_mode_label(),
                g_draw.enabled.load(std::memory_order_relaxed) ? "on" : "off",
                g_stream.enabled.load(std::memory_order_relaxed) ? "on" : "off",
                g_forceInline.load(std::memory_order_relaxed) ? 1 : 0,
                g_forcedInline.load(std::memory_order_relaxed),
                g_drainGuardSkips.load(std::memory_order_relaxed),
                g_drainEntries.load(std::memory_order_relaxed),
                g_waitTaken2.load(std::memory_order_relaxed),
                g_latchSet2.load(std::memory_order_relaxed),
                g_stereo.load(std::memory_order_relaxed) ? 1 : 0,
                g_doubleCall.load(std::memory_order_relaxed) ? 1 : 0,
                g_pulseCount.load(std::memory_order_relaxed),
                g_secondYawDeg.load(std::memory_order_relaxed),
                g_secondCalls.load(std::memory_order_relaxed),
                g_stereoSkips.load(std::memory_order_relaxed),
                g_foreignCallerSkips.load(std::memory_order_relaxed),
                g_secondPassHits.load(std::memory_order_relaxed),
                g_call2Us.load(std::memory_order_relaxed),
                g_poisoned.load(std::memory_order_relaxed) ? 1 : 0,
                g_kickSampling.load(std::memory_order_relaxed) ? 1 : 0,
                g_kick2Sampling.load(std::memory_order_relaxed) ? 1 : 0,
                g_calcInside.load(std::memory_order_relaxed),
                g_calcOutside.load(std::memory_order_relaxed),
                g_drawEntries.load(std::memory_order_relaxed));
    } else {
        BVR_LOG("[reentry] unknown verb '%s' (BS2 has vrstereo|1t|srdev|stereo|pulse|"
                "on|off|yaw|reset|hook|unhook|dump|kick|kick2|calcstack|status)",
                verb);
    }
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

bool inside_hooked_call() {
    return g_activeDepth.load(std::memory_order_relaxed) > 0 &&
           g_activeTid.load(std::memory_order_relaxed) == GetCurrentThreadId();
}

bool hook_live() {
    return g_draw.enabled.load(std::memory_order_relaxed) ||
           g_stream.enabled.load(std::memory_order_relaxed) ||
           g_trigger2Enabled.load(std::memory_order_relaxed);
}

bool stereo_active() {
    return g_stereo.load(std::memory_order_relaxed) &&
           g_draw.enabled.load(std::memory_order_relaxed) &&
           !g_poisoned.load(std::memory_order_relaxed);
}

bool second_pass_for_current_thread(float* yawDegOut) {
    uint32_t t = g_secondPassTid.load(std::memory_order_relaxed);
    if (t == 0 || t != GetCurrentThreadId()) return false;
    g_secondPassHits.fetch_add(1, std::memory_order_relaxed);
    *yawDegOut = g_secondYawDeg.load(std::memory_order_relaxed);
    return true;
}

bool in_second_draw() {
    uint32_t t = g_secondPassTid.load(std::memory_order_relaxed);
    return t != 0 && t == GetCurrentThreadId();
}

void request_vrstereo(bool on) {
    g_vrstereoPending.store(on ? 1 : 0, std::memory_order_relaxed);
}

void apply_pending_vrstereo() {
    // Act on the EXCHANGED value, not a pre-read - a request posted between
    // load and exchange must not be swallowed.
    if (g_vrstereoPending.load(std::memory_order_relaxed) >= 0) {
        int pending = g_vrstereoPending.exchange(-1, std::memory_order_relaxed);
        if (pending >= 0) apply_vrstereo(pending == 1);
    }
}

bool rig_hidden() {
    return g_rigHide.load(std::memory_order_relaxed);
}

void set_rig_hidden(bool on) {
    g_rigHide.store(on, std::memory_order_relaxed);
}

void draw_debug_ui() {
    if (!ImGui::CollapsingHeader("Reentry / stereo (BS2)")) return;
    // The one-toggle: applied on the game thread via the pending request -
    // the overlay may be drawing on the render thread.
    bool armed = g_vrStereoArmed.load(std::memory_order_relaxed);
    bool toggle = armed;
    if (ImGui::Checkbox("VR stereo (full-rate SR on 1t)", &toggle) && toggle != armed)
        request_vrstereo(toggle);
    ImGui::Text("hooks: draw %s  stream %s%s%s  samplers: kick %s  kick2 %s",
                g_draw.enabled.load(std::memory_order_relaxed) ? "ON" : "off",
                g_stream.enabled.load(std::memory_order_relaxed) ? "ON" : "off",
                g_stereo.load(std::memory_order_relaxed) ? "  STEREO" : "",
                g_poisoned.load(std::memory_order_relaxed) ? "  POISONED" : "",
                g_kickSampling.load(std::memory_order_relaxed) ? "ON" : "off",
                g_kick2Sampling.load(std::memory_order_relaxed) ? "ON" : "off");
    ImGui::Text("draws %u  2nd %u  skips %u/%u  calc in/out %u/%u  drawTid %u "
                "presentTid %u",
                g_drawEntries.load(std::memory_order_relaxed),
                g_secondCalls.load(std::memory_order_relaxed),
                g_stereoSkips.load(std::memory_order_relaxed),
                g_foreignCallerSkips.load(std::memory_order_relaxed),
                g_calcInside.load(std::memory_order_relaxed),
                g_calcOutside.load(std::memory_order_relaxed),
                g_lastDrawTid.load(std::memory_order_relaxed),
                bvr::d3d11_hook::last_present_tid());
    ImGui::TextDisabled("control via seam: reentry vrstereo|stereo|pulse|on|off|yaw|"
                        "reset|hook|unhook|dump|kick|kick2|calcstack|status");
}

} // namespace bvr::b2r::scenedraw
