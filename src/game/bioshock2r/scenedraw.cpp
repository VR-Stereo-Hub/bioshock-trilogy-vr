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

#include "core/hooks/d3d11_hook.h"
#include "core/util/log.h"
#include "core/vr/openxr_runtime.h"
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

struct HookSlot {
    const char* name = nullptr;
    void* target = nullptr;
    void* original = nullptr; // trampoline; cast to the slot's signature
    bool created = false;     // game thread only
    std::atomic<bool> enabled{false};
};
HookSlot g_draw{"draw"};
HookSlot g_stream{"stream"};

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

// SequentialReentry controls + telemetry (session 26). The BS2 primary bet
// runs the double Draw on the THREADED substrate: the Draw path has no
// submit handshake (that spin-wait belongs to the streaming manager), the
// ring is cursor-based, and the endframe render sync runs once per tick
// AFTER the doubled call - so a second Draw just enqueues a second scene +
// present. 1t machinery is derived only if this proves unstable (the
// standing "BS2 is not bound by BS1's methods" policy gate, applied).
std::atomic<bool> g_doubleCall{false};
std::atomic<int> g_pulseCount{0};
std::atomic<float> g_secondYawDeg{30.0f}; // probe mode: yaw on pass 2
std::atomic<bool> g_stereo{false};
std::atomic<uint32_t> g_stereoSkips{0}; // pass 2 skipped (gate/stall)
std::atomic<uint32_t> g_secondCalls{0};
std::atomic<uint32_t> g_secondPassTid{0};
std::atomic<uint32_t> g_secondPassHits{0};
std::atomic<uint32_t> g_call2Us{0};
std::atomic<uint32_t> g_foreignCallerSkips{0}; // non-gameplay-caller Draws seen armed
std::atomic<bool> g_poisoned{false};
std::atomic<uint32_t> g_lastExcCode{0};
std::atomic<uint32_t> g_lastExcRva{0};
std::atomic<int> g_vrstereoPending{-1}; // -1 none, 0 off, 1 on
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
         g_beatSecond = 0;
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

// The second Draw of a pair (game thread, depth 0, after the first call
// returned). ORIGINAL args pass through unchanged - the camera enters via
// the CalcView dispatch that re-fires inside the Draw (live-verified
// exactly-once-per-Draw), which the ProcessEvent seam replays as the RIGHT
// eye (second_pass_replay in camera.cpp).
void maybe_second_draw(void* ecx, void* edx, void* a1, void* a2, void* a3, void* a4,
                       uint32_t callerRva, uint32_t presentDelta) {
    if (g_poisoned.load(std::memory_order_relaxed)) return;
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
        return;
    }
    // Present-stall guard: no present landed between the previous Draw and
    // this one (unfocused window / hitching render thread) - doubling has no
    // value and a stall is the prime wedge suspect. Pulses bypass it so the
    // instrument stays usable for A/B while paused.
    if (presentDelta == 0 && !pulse) {
        g_stereoSkips.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    if (g_stereo.load(std::memory_order_relaxed)) bvr::vr::sr_push_eye(+1);
    g_secondPassTid.store(GetCurrentThreadId(), std::memory_order_relaxed);
    LARGE_INTEGER t0, t1, freq;
    QueryPerformanceCounter(&t0);
    bool ok = call_draw_guarded(reinterpret_cast<DrawFn>(g_draw.original), ecx, edx, a1,
                                a2, a3, a4);
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
        return;
    }
    if (now - g_lastBeatMs < 1000) return;
    uint32_t draws = g_drawEntries.load(std::memory_order_relaxed);
    uint32_t streams = g_streamEntries.load(std::memory_order_relaxed);
    uint32_t calcIn = g_calcInside.load(std::memory_order_relaxed);
    uint32_t calcOut = g_calcOutside.load(std::memory_order_relaxed);
    uint32_t seconds = g_secondCalls.load(std::memory_order_relaxed);
    uint64_t presents = bvr::d3d11_hook::present_count();
    BVR_LOG("[reentry] beat: draws/s=%u 2nd/s=%u presents/s=%llu stream/s=%u calc "
            "in/out=%u/%u drawTid=%u presentTid=%u calcTid=%u drawUs=%u camSrc=(%.1f "
            "%.1f %.1f | %d %d %d) callers=%X,%X,%X,%X",
            draws - g_beatDraws, seconds - g_beatSecond,
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
            g_callerRvas[3].load(std::memory_order_relaxed));
    g_lastBeatMs = now;
    g_beatDraws = draws;
    g_beatStreams = streams;
    g_beatCalcIn = calcIn;
    g_beatCalcOut = calcOut;
    g_beatSecond = seconds;
    g_beatPresents = presents;
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

// The one-toggle compound (game thread only, outside hooked calls). BS2
// sequence: camera mode -> stereo. NO 1t rung - threaded-mode doubling is
// the primary bet on this game (see the module-head note); compare BS1's
// apply_vrstereo, which fronts a structural single-threading step.
void apply_vrstereo(bool on) {
    if (on) {
        BVR_LOG("[reentry] VRSTEREO ON: sequencing camera mode -> stereo (threaded "
                "substrate - the BS2 primary bet, no 1t)");
        bvr::vr::set_camera_mode(true);
        if (!g_stereo.load(std::memory_order_relaxed)) handle_command("stereo on");
        bool ok = g_stereo.load(std::memory_order_relaxed);
        BVR_LOG("[reentry] VRSTEREO %s (stereo=%d; sticky across loads via the "
                "gameplay-caller gate; 'vrstereo off' reverses)",
                ok ? "READY" : "INCOMPLETE - see refusals above", ok ? 1 : 0);
    } else {
        BVR_LOG("[reentry] VRSTEREO OFF: stereo -> camera mode");
        if (g_stereo.load(std::memory_order_relaxed)) handle_command("stereo off");
        bvr::vr::set_camera_mode(false);
    }
}

} // namespace

void init(const bvr::pattern_scan::ProcessImage& image) {
    g_imageBase = image.base;
    g_imageSize = image.size;
    g_image = image;
}

void handle_command(const char* args) {
    char verb[16] = {};
    int consumed = 0;
    if (sscanf_s(args, "%15s%n", verb, static_cast<unsigned>(sizeof verb), &consumed) != 1) {
        BVR_LOG("[reentry] command needs a verb: hook [draw|stream]|unhook|dump <n>|"
                "kick on|off|kick2 on|off|calcstack|status");
        return;
    }
    const char* rest = args + consumed;
    while (*rest == ' ' || *rest == '\t') ++rest;

    if (strcmp(verb, "vrstereo") == 0) {
        apply_vrstereo(strncmp(rest, "on", 2) == 0);
    } else if (strcmp(verb, "stereo") == 0) {
        if (strncmp(rest, "on", 2) == 0) {
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
        disable_slot(g_draw);
        disable_slot(g_stream);
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
        BVR_LOG("[reentry] status: draw=%s stream=%s stereo=%d double=%d pulse=%d "
                "yaw=%.1f seconds=%u skips=%u foreign=%u 2ndHits=%u call2Us=%u "
                "poisoned=%d kick=%d kick2=%d calcview in/out %u/%u draws=%u",
                g_draw.enabled.load(std::memory_order_relaxed) ? "on" : "off",
                g_stream.enabled.load(std::memory_order_relaxed) ? "on" : "off",
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
        BVR_LOG("[reentry] unknown verb '%s' (BS2 has vrstereo|stereo|pulse|on|off|yaw|"
                "reset|hook|unhook|dump|kick|kick2|calcstack|status)",
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

void draw_debug_ui() {
    if (!ImGui::CollapsingHeader("Reentry / stereo (BS2)")) return;
    // The one-toggle: applied on the game thread via the pending request -
    // the overlay may be drawing on the render thread.
    bool srOn = stereo_active();
    bool srToggle = srOn;
    if (ImGui::Checkbox("VR stereo (camera mode + stereo)", &srToggle) && srToggle != srOn)
        request_vrstereo(srToggle);
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
