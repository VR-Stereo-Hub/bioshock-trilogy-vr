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

// Reserved for the substrate hooks (commit 2+): nonzero exactly while a
// depth-0 hooked render call is in flight on that thread. The poll-gate
// deferral (inside_hooked_call) and the calcview in/out attribution key on
// these from day one so the camera-side plumbing lands once.
std::atomic<uint32_t> g_activeTid{0};
std::atomic<int> g_activeDepth{0};
std::atomic<uint32_t> g_calcInside{0};
std::atomic<uint32_t> g_calcOutside{0};

// One-shot CalcView stack scan request.
std::atomic<int> g_calcstackPending{0};

uint32_t to_rva(const void* p) {
    uintptr_t d = reinterpret_cast<uintptr_t>(p) - reinterpret_cast<uintptr_t>(g_imageBase);
    return d < g_imageSize ? static_cast<uint32_t>(d) : 0xFFFFFFFFu;
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

} // namespace

void init(const bvr::pattern_scan::ProcessImage& image) {
    g_imageBase = image.base;
    g_imageSize = image.size;
}

void handle_command(const char* args) {
    char verb[16] = {};
    int consumed = 0;
    if (sscanf_s(args, "%15s%n", verb, static_cast<unsigned>(sizeof verb), &consumed) != 1) {
        BVR_LOG("[reentry] command needs a verb: kick on|off|kick2 on|off|"
                "calcstack|status (substrate verbs land once the RVAs are derived)");
        return;
    }
    const char* rest = args + consumed;
    while (*rest == ' ' || *rest == '\t') ++rest;

    if (strcmp(verb, "kick") == 0) {
        kick_sampler(strncmp(rest, "on", 2) == 0);
    } else if (strcmp(verb, "kick2") == 0) {
        kick2_sampler(strncmp(rest, "on", 2) == 0);
    } else if (strcmp(verb, "calcstack") == 0) {
        g_calcstackPending.store(1, std::memory_order_relaxed);
        BVR_LOG("[reentry] calcstack armed (next CalcView dispatch logs a stack scan)");
    } else if (strcmp(verb, "status") == 0) {
        BVR_LOG("[reentry] status: kick=%d kick2=%d calcview in/out %u/%u "
                "(BS2 substrate hooks not derived yet - session 26 ladder)",
                g_kickSampling.load(std::memory_order_relaxed) ? 1 : 0,
                g_kick2Sampling.load(std::memory_order_relaxed) ? 1 : 0,
                g_calcInside.load(std::memory_order_relaxed),
                g_calcOutside.load(std::memory_order_relaxed));
    } else {
        BVR_LOG("[reentry] unknown verb '%s' (BS2 has kick|kick2|calcstack|status; "
                "hook/1t/stereo/vrstereo land after the substrate derivation)",
                verb);
    }
}

void note_calcview() {
    uint32_t tid = GetCurrentThreadId();
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
    return g_trigger2Enabled.load(std::memory_order_relaxed);
}

bool stereo_active() {
    return false; // SR lands with the substrate commits
}

void draw_debug_ui() {
    if (!ImGui::CollapsingHeader("Reentry probe (BS2 discovery)")) return;
    ImGui::Text("samplers: kick %s  kick2 %s  calcview in/out %u/%u",
                g_kickSampling.load(std::memory_order_relaxed) ? "ON" : "off",
                g_kick2Sampling.load(std::memory_order_relaxed) ? "ON" : "off",
                g_calcInside.load(std::memory_order_relaxed),
                g_calcOutside.load(std::memory_order_relaxed));
    ImGui::TextDisabled("control via seam: reentry kick|kick2|calcstack|status");
}

} // namespace bvr::b2r::scenedraw
