#include "game/bioshockinf/fidget.h"

#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstring>

#include <MinHook.h>
#include <intrin.h>

#include "core/hooks/pattern_scan.h"
#include "core/util/log.h"
#include "game/bioshockinf/bones.h"
#include "game/bioshockinf/patterns.h"
#include "game/bioshockinf/reflect.h"

#include <imgui.h>

namespace bvr::bsi::fidget {
namespace {

using ProcessEventFn = void(__fastcall*)(void* self, void* edx, void* func, void* parms,
                                         void* result);

// 0 = off (slot restored), 1 = probe (log, pass through), 2 = filter (block).
// Default PROBE (s48 verdict): the clean-boot A/B PROVED the stance starts
// NATIVELY - a full 8-minute boot with the filter armed from resolve entered
// the stance with events=1, startSeen=0, blocked=0, so the ProcessEvent
// dispatch is a sometimes-notification, not the initiator. Blocking it does
// not kill the stance and could starve script-side listeners, so the default
// observes instead. The remaining root is the bDisableSubtleFidget UBOOL
// (the native selector's own gate) - bsiprop/bsipropbit are built for exactly
// that derivation; it needs one booted save to finish.
std::atomic<int> g_mode{1};
std::atomic<bool> g_installed{false};

// s48b: the property-side stance kill, and what the live A/Bs measured:
//  1. bDisableSubtleFidget SET on the INSTANCE: stance re-entered anyway.
//  2. SubtleFidgetTimeRange {120, 240} s (EXACTLY the measured 2-4 min
//     re-onset window) starved to {1e9, 1e9} on the INSTANCE, write verified
//     held: stance re-entered anyway. The live scheduler reads neither
//     instance property.
//  3. The ARCHETYPE (the spawn template at the ObjectArchetype slot, itself
//     an XFirstPersonAttachment carrying the authored {120, 240}): starved
//     too, bools set on both - **stance re-entered anyway (+5:20)**. FOUR
//     property-side hypotheses are now falsified with held writes verified;
//     the live consumer keeps its OWN copy of the timing - the
//     XFidgetAnimationSelection anim-tree node is the next hunt (find the
//     instance off the component's anim tree and starve or neutralize THERE).
// This lane therefore ships DEFAULT OFF (evidence-first: no memory write
// without a proven effect); it stays as the property-side layer for the
// eventual layered kill and as the ready-made apply plumbing once the real
// consumer is found. Self-deriving offsets (0x214/0x1, 0x26C on this build);
// the walk refuses on layout drift.
std::atomic<bool> g_rootKill{false};
void* g_appliedOn = nullptr;   // attachment instance last applied to
bool g_walkRefused = false;    // one refusal disables the lane for the boot
uint32_t g_boolOff = 0, g_boolMask = 0;
uint32_t g_rangeOff = 0;
float g_rangeSaved[2] = {120.0f, 240.0f}; // authored values, for the OFF lever
constexpr float kStarve = 1.0e9f;
uint32_t g_applies = 0;

void** g_slot = nullptr;          // the patched vtable slot
ProcessEventFn g_orig = nullptr;  // its original occupant
int32_t g_startIdx = -1;          // GNames index of StartSubtleFidget
int32_t g_nameOff = -1;           // UObject::Name byte offset (cached)

std::atomic<uint32_t> g_events{0};
std::atomic<uint32_t> g_startSeen{0};
std::atomic<uint32_t> g_blocked{0};

uint32_t rva_of(const void* p) {
    const uint8_t* base = patterns::image_base();
    return (base && p) ? static_cast<uint32_t>(static_cast<const uint8_t*>(p) - base) : 0;
}

// ---- s49: the IMPL hook - the choke point every dispatch route reaches ------
// AXFirstPersonAttachment::StartSubtleFidget's C++ body (kStartSubtleFidget-
// ImplRva). Every route to the event - the SetTimer executor (which does NOT
// go through the attachment's vtable +0x7C on this build; that is why the s48
// clean boot read events=1), script dispatch, CallFunction - executes the
// native thunk, and the thunk calls THIS. Blocking here also skips the impl's
// own re-arm tail (it SetTimers itself from SubtleFidgetTimeRange), so a
// blocked chain stays down until the next equip/fire-reset arm site fires.
// 1 = probe (log every call, pass through - the route instrument),
// 2 = block for the resolved attachment, 0 = pass silently (hook stays).
std::atomic<int> g_implMode{1};
std::atomic<bool> g_implInstalled{false};
using StartFidgetFn = void(__fastcall*)(void* self, void* edx);
StartFidgetFn g_implOrig = nullptr;
uint8_t* g_implTarget = nullptr;
std::atomic<uint32_t> g_implCalls{0};
std::atomic<uint32_t> g_implOurs{0};
std::atomic<uint32_t> g_implBlocked{0};
uint64_t g_implLastMs = 0; // game-thread writes only (the impl runs game-side)

void __fastcall StartFidgetImplDetour(void* self, void* edx) {
    g_implCalls.fetch_add(1, std::memory_order_relaxed);
    const uint64_t now = GetTickCount64();
    const uint64_t since = g_implLastMs ? now - g_implLastMs : 0;
    g_implLastMs = now;
    const bool ours = self == bones::attachment();
    if (ours) g_implOurs.fetch_add(1, std::memory_order_relaxed);
    const bool block = ours && g_implMode.load(std::memory_order_relaxed) == 2;
    BVR_LOG("[bsi] fidget: StartSubtleFidget IMPL on %p (%s, %+.1f s since last) - %s",
            self, ours ? "THE attachment" : "another object",
            static_cast<double>(since) / 1000.0,
            block ? "BLOCKED (anim + self re-arm both skipped)" : "passed through");
    if (block) {
        g_implBlocked.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (g_implOrig) g_implOrig(self, edx);
}

bool try_install_impl() {
    if (g_implInstalled.load(std::memory_order_relaxed)) return true;
    if (!patterns::rva_trusted()) return false;
    uint8_t* impl =
        const_cast<uint8_t*>(patterns::image_base()) + patterns::kStartSubtleFidgetImplRva;
    if (!bvr::pattern_scan::is_memory_valid(impl, 0x100)) {
        BVR_LOG("[bsi] fidget: impl REFUSED - rva 0x%X not readable",
                patterns::kStartSubtleFidgetImplRva);
        return false;
    }
    // Gate 1: the pinned prologue (push esi; mov esi,ecx; test [esi+0x214],1 -
    // the bDisableSubtleFidget test doubles as an identity check).
    if (memcmp(impl, patterns::kStartSubtleFidgetImplPrologue,
               sizeof patterns::kStartSubtleFidgetImplPrologue) != 0) {
        BVR_LOG("[bsi] fidget: impl REFUSED - prologue at rva 0x%X does not match the "
                "derivation (stale build?)",
                patterns::kStartSubtleFidgetImplRva);
        return false;
    }
    // Gate 2: the arity. __thiscall, ZERO stack args - the body must end in a
    // plain `pop esi; ret` (5E C3, at impl+0xCB in the derivation) with no
    // `ret imm` before it, or the detour's signature is wrong for this build.
    size_t retOff = 0;
    for (size_t i = 0; i + 1 < 0x100; ++i) {
        if (impl[i] == 0x5E && impl[i + 1] == 0xC3) {
            retOff = i + 1;
            break;
        }
    }
    if (!retOff) {
        BVR_LOG("[bsi] fidget: impl REFUSED - no `pop esi; ret` in the first 0x100 bytes "
                "at rva 0x%X",
                patterns::kStartSubtleFidgetImplRva);
        return false;
    }
    for (size_t i = 0; i + 2 < retOff; ++i) {
        if (impl[i] == 0xC2 && impl[i + 2] == 0x00) {
            BVR_LOG("[bsi] fidget: impl REFUSED - `ret imm` at +0x%zX before the plain ret "
                    "(arg count drifted?)",
                    i);
            return false;
        }
    }
    if (MH_CreateHook(impl, reinterpret_cast<void*>(&StartFidgetImplDetour),
                      reinterpret_cast<void**>(&g_implOrig)) != MH_OK) {
        BVR_LOG("[bsi] fidget: MH_CreateHook failed at rva 0x%X",
                patterns::kStartSubtleFidgetImplRva);
        return false;
    }
    if (MH_EnableHook(impl) != MH_OK) {
        BVR_LOG("[bsi] fidget: MH_EnableHook failed at rva 0x%X",
                patterns::kStartSubtleFidgetImplRva);
        return false;
    }
    g_implTarget = impl;
    g_implInstalled.store(true, std::memory_order_relaxed);
    BVR_LOG("[bsi] fidget: StartSubtleFidget IMPL hooked at rva 0x%X (every dispatch "
            "route reaches this body: thunk, timer executor, C++). Mode: %s.",
            patterns::kStartSubtleFidgetImplRva,
            g_implMode.load() == 2 ? "BLOCK (for the resolved attachment)"
                                   : "PROBE (log only)");
    return true;
}

// ---- s49b: the ACTION-PLAY probe - one rung below StartSubtleFidget ---------
// The s49 clean leg falsified the StartSubtleFidget lane outright (stance
// re-entered with ZERO impl calls), so the starter is one of the OTHER 11
// callers of the network's play-anim-action-by-name entry - or a Morpheme-
// internal transition that never goes by name. This probe decides which: it
// logs every by-name action entering any XMorphemeNetwork (name text, self,
// whether self is the FP attachment's runtime network at component+0x228,
// and the CALLER's return RVA - the return address names the scheduler).
// Block mode refuses one configured name index on the FP network only.
std::atomic<int> g_actMode{1}; // 1 = probe, 2 = block g_actBlockIdx, 0 = silent
std::atomic<bool> g_actInstalled{false};
std::atomic<int32_t> g_actBlockIdx{-1};
using PlayActionFn = void(__fastcall*)(void* self, void* edx, int32_t nameIdx,
                                       int32_t nameNum, uint32_t a3, float blend,
                                       uint32_t a5);
PlayActionFn g_actOrig = nullptr;
std::atomic<uint32_t> g_actCalls{0};
std::atomic<uint32_t> g_actBlocked{0};

void* fp_network() {
    uint8_t* comp = static_cast<uint8_t*>(bones::component());
    if (!comp || !bvr::pattern_scan::is_memory_valid(comp + 0x228, 4)) return nullptr;
    return *reinterpret_cast<void**>(comp + 0x228);
}

void __fastcall PlayActionDetour(void* self, void* edx, int32_t nameIdx, int32_t nameNum,
                                 uint32_t a3, float blend, uint32_t a5) {
    g_actCalls.fetch_add(1, std::memory_order_relaxed);
    const uint32_t callerRva = rva_of(_ReturnAddress());
    const bool ours = self && self == fp_network();
    char name[patterns::kFNameTextBufMin] = {};
    if (nameIdx > 0 && nameIdx < patterns::fname_count())
        patterns::fname_text(nameIdx, name, sizeof name);
    const bool block = ours && g_actMode.load(std::memory_order_relaxed) == 2 &&
                       nameIdx == g_actBlockIdx.load(std::memory_order_relaxed);
    BVR_LOG("[bsi] fidget: anim action '%s' (idx %d num %d, a3=%u blend=%.2f a5=%u) on "
            "%p (%s) from caller rva 0x%X - %s",
            name[0] ? name : "?", nameIdx, nameNum, a3, static_cast<double>(blend), a5,
            self, ours ? "the FP network" : "another network", callerRva,
            block ? "BLOCKED" : "passed");
    if (block) {
        g_actBlocked.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (g_actOrig) g_actOrig(self, edx, nameIdx, nameNum, a3, blend, a5);
}

bool try_install_act() {
    if (g_actInstalled.load(std::memory_order_relaxed)) return true;
    if (!patterns::rva_trusted()) return false;
    uint8_t* impl =
        const_cast<uint8_t*>(patterns::image_base()) + patterns::kPlayAnimActionByNameRva;
    if (!bvr::pattern_scan::is_memory_valid(impl, 0x200)) return false;
    if (memcmp(impl, patterns::kPlayAnimActionByNamePrologue,
               sizeof patterns::kPlayAnimActionByNamePrologue) != 0) {
        BVR_LOG("[bsi] fidget: act REFUSED - prologue at rva 0x%X does not match the "
                "derivation (stale build?)",
                patterns::kPlayAnimActionByNameRva);
        return false;
    }
    bool sawRet = false;
    for (size_t i = 0; i + 2 < 0x200; ++i) {
        if (impl[i] == 0xC2 && impl[i + 1] == patterns::kPlayAnimActionByNameRetImm &&
            impl[i + 2] == 0x00) {
            sawRet = true;
            break;
        }
    }
    if (!sawRet) {
        BVR_LOG("[bsi] fidget: act REFUSED - no `ret 0x%X` in the first 0x200 bytes at "
                "rva 0x%X (arg count drifted?)",
                patterns::kPlayAnimActionByNameRetImm, patterns::kPlayAnimActionByNameRva);
        return false;
    }
    if (MH_CreateHook(impl, reinterpret_cast<void*>(&PlayActionDetour),
                      reinterpret_cast<void**>(&g_actOrig)) != MH_OK)
        return false;
    if (MH_EnableHook(impl) != MH_OK) return false;
    g_actInstalled.store(true, std::memory_order_relaxed);
    BVR_LOG("[bsi] fidget: anim-action-by-name hooked at rva 0x%X (the network's play "
            "entry, 12 callers). Mode: %s.",
            patterns::kPlayAnimActionByNameRva,
            g_actMode.load() == 2 ? "BLOCK (configured name on the FP network)"
                                  : "PROBE (log every by-name action)");
    return true;
}

// ---- s49b: the REQUEST-POST probe - the Morpheme network's front door -------
// Falsifications 5+6 proved the stance neither starts via StartSubtleFidget
// nor via a by-name anim action. The next layer down is the request lane:
// EVERY request entering any XMorphemeNetwork passes kPostRequestRva (8 E8
// callers; derivation in patterns.h). Probe logs the 16-byte descriptor, the
// priority arg, FP-network attribution and the CALLER return RVA. Block mode
// refuses a configured descriptor dword0 (the request id) on the FP network.
std::atomic<int> g_reqMode{1}; // 1 = probe, 2 = block g_reqBlockId, 0 = silent
std::atomic<bool> g_reqInstalled{false};
std::atomic<uint32_t> g_reqBlockId{0xFFFFFFFF};
using PostRequestFn = void(__cdecl*)(void* runtime, void* desc, void* params);
PostRequestFn g_reqOrig = nullptr;
std::atomic<uint32_t> g_reqCalls{0};
std::atomic<uint32_t> g_reqBlocked{0};

// The FP network's Morpheme runtime ([network+0x118]) - what the inner post
// receives as its first arg.
void* fp_runtime() {
    uint8_t* net = static_cast<uint8_t*>(fp_network());
    if (!net || !bvr::pattern_scan::is_memory_valid(net + 0x118, 4)) return nullptr;
    return *reinterpret_cast<void**>(net + 0x118);
}

void __cdecl PostRequestDetour(void* runtime, void* desc, void* params) {
    g_reqCalls.fetch_add(1, std::memory_order_relaxed);
    const uint32_t callerRva = rva_of(_ReturnAddress());
    const bool ours = runtime && runtime == fp_runtime();
    uint32_t d[4] = {};
    uint32_t id = 0xFFFF, type = 0xFF;
    if (desc && bvr::pattern_scan::is_memory_valid(desc, 16)) {
        memcpy(d, desc, 16);
        id = *reinterpret_cast<const uint16_t*>(static_cast<const uint8_t*>(desc) + 8);
        type = *reinterpret_cast<const uint8_t*>(static_cast<const uint8_t*>(desc) + 0xC);
    }
    const bool block = ours && g_reqMode.load(std::memory_order_relaxed) == 2 &&
                       id == g_reqBlockId.load(std::memory_order_relaxed);
    // NPC networks post too - ours always logs; foreign posts 1-in-64.
    if (ours || (g_reqCalls.load(std::memory_order_relaxed) & 63) == 0) {
        BVR_LOG("[bsi] fidget: net request id=%u type=%u {%08X %08X %08X %08X} on "
                "runtime %p (%s) from caller rva 0x%X - %s",
                id, type, d[0], d[1], d[2], d[3], runtime,
                ours ? "the FP network" : "another network", callerRva,
                block ? "BLOCKED" : "passed");
    }
    if (block) {
        g_reqBlocked.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (g_reqOrig) g_reqOrig(runtime, desc, params);
}

bool try_install_req() {
    if (g_reqInstalled.load(std::memory_order_relaxed)) return true;
    if (!patterns::rva_trusted()) return false;
    uint8_t* impl =
        const_cast<uint8_t*>(patterns::image_base()) + patterns::kPostRequestInnerRva;
    if (!bvr::pattern_scan::is_memory_valid(impl, 0x100)) return false;
    if (memcmp(impl, patterns::kPostRequestInnerPrologue,
               sizeof patterns::kPostRequestInnerPrologue) != 0) {
        BVR_LOG("[bsi] fidget: req REFUSED - prologue at rva 0x%X does not match the "
                "derivation (stale build?)",
                patterns::kPostRequestInnerRva);
        return false;
    }
    // __cdecl (the wrapper does `add esp, 0xC` after the call): a `ret imm`
    // anywhere before the first plain ret would mean the shape drifted.
    // (A cdecl pass-through detour is arity-safe; the gate is the prologue.)
    if (MH_CreateHook(impl, reinterpret_cast<void*>(&PostRequestDetour),
                      reinterpret_cast<void**>(&g_reqOrig)) != MH_OK)
        return false;
    if (MH_EnableHook(impl) != MH_OK) return false;
    g_reqInstalled.store(true, std::memory_order_relaxed);
    BVR_LOG("[bsi] fidget: request-post hooked at the INNER funnel rva 0x%X (all five "
            "post paths converge here). Mode: %s.",
            patterns::kPostRequestInnerRva,
            g_reqMode.load() == 2 ? "BLOCK (configured id on the FP network)"
                                  : "PROBE (log every request)");
    return true;
}

void __fastcall PeDetour(void* self, void* edx, void* func, void* parms, void* result) {
    g_events.fetch_add(1, std::memory_order_relaxed);
    // One raw 4-byte read. Safety argument: `func` is the UFunction the engine
    // is about to execute through this very call - if it were unreadable the
    // original would fault on it first. g_nameOff was verified at install.
    if (func && g_nameOff >= 0) {
        const int32_t idx =
            *reinterpret_cast<const int32_t*>(static_cast<const uint8_t*>(func) + g_nameOff);
        if (idx == g_startIdx) {
            g_startSeen.fetch_add(1, std::memory_order_relaxed);
            const bool ours = self == bones::attachment();
            const bool block = ours && g_mode.load(std::memory_order_relaxed) == 2;
            BVR_LOG("[bsi] fidget: StartSubtleFidget dispatched on %p (%s) - %s", self,
                    ours ? "THE attachment" : "another object",
                    block ? "BLOCKED at the root" : "passed through (probe)");
            if (block) {
                g_blocked.fetch_add(1, std::memory_order_relaxed);
                return; // the stance never starts
            }
        }
    }
    g_orig(self, edx, func, parms, result);
}

void uninstall(const char* why) {
    if (!g_installed.load(std::memory_order_relaxed)) return;
    if (*g_slot != reinterpret_cast<void*>(&PeDetour)) {
        BVR_LOG("[bsi] fidget: slot no longer holds the filter (someone re-patched?) - "
                "leaving it alone (%s)",
                why);
        g_installed.store(false, std::memory_order_relaxed);
        return;
    }
    DWORD old = 0;
    if (VirtualProtect(g_slot, sizeof(void*), PAGE_READWRITE, &old)) {
        *g_slot = reinterpret_cast<void*>(g_orig);
        VirtualProtect(g_slot, sizeof(void*), old, &old);
        g_installed.store(false, std::memory_order_relaxed);
        BVR_LOG("[bsi] fidget: slot RESTORED to rva 0x%X (%s)", rva_of((void*)g_orig), why);
    }
}

} // namespace

void tick_apply() {
    if (!g_rootKill.load(std::memory_order_relaxed) || g_walkRefused) return;
    void* attach = bones::attachment();
    if (!attach || attach == g_appliedOn) return;
    char cls[64] = {};
    if (!reflect::class_name_of(attach, cls, sizeof cls) ||
        strcmp(cls, "XFirstPersonAttachment") != 0)
        return; // not resolved far enough yet - retry next tick
    if (!g_rangeOff) {
        const bool haveBool = reflect::find_bool_property_bit(
            attach, "bDisableSubtleFidget", &g_boolOff, &g_boolMask);
        const bool haveRange = reflect::find_property_offset(
            attach, "SubtleFidgetTimeRange", "StructProperty", &g_rangeOff);
        if (!haveRange) {
            g_walkRefused = true; // a 600+-field walk must not spin at 1 Hz
            BVR_LOG("[bsi] fidget: root kill REFUSED - SubtleFidgetTimeRange did not "
                    "derive (bool %s); the stance stays this boot",
                    haveBool ? "found" : "also missing");
            return;
        }
        BVR_LOG("[bsi] fidget: derived SubtleFidgetTimeRange at attachment+0x%X%s",
                g_rangeOff,
                haveBool ? " (and bDisableSubtleFidget for the defense bit)" : "");
    }
    // Starve the instance AND its archetype (the ObjectArchetype slot sits at
    // Name+0xC per the s48 layout; the archetype is itself an
    // XFirstPersonAttachment carrying the authored range, and is the template
    // future spawns copy from).
    void* targets[2] = {attach, nullptr};
    const int nameOff = reflect::uobject_name_offset();
    if (nameOff >= 0 &&
        bvr::pattern_scan::is_memory_valid(static_cast<uint8_t*>(attach) + nameOff + 0xC,
                                           4)) {
        void* arch = *reinterpret_cast<void**>(static_cast<uint8_t*>(attach) + nameOff + 0xC);
        char acls[64] = {};
        if (arch && reflect::class_name_of(arch, acls, sizeof acls) &&
            strcmp(acls, "XFirstPersonAttachment") == 0)
            targets[1] = arch;
    }
    for (void* t : targets) {
        if (!t) continue;
        float* range = reinterpret_cast<float*>(static_cast<uint8_t*>(t) + g_rangeOff);
        if (!bvr::pattern_scan::is_memory_valid(range, 8)) continue;
        // Bank the authored values once (the OFF lever restores them) - but
        // only when they read authored, not our own starve.
        if (t == attach && range[0] < kStarve * 0.5f && range[0] > 0.0f) {
            g_rangeSaved[0] = range[0];
            g_rangeSaved[1] = range[1];
        }
        range[0] = kStarve;
        range[1] = kStarve;
        if (g_boolMask) {
            uint8_t* p = static_cast<uint8_t*>(t) + g_boolOff;
            if (bvr::pattern_scan::is_memory_valid(p, 4))
                *reinterpret_cast<uint32_t*>(p) |= g_boolMask;
        }
    }
    g_appliedOn = attach;
    ++g_applies;
    BVR_LOG("[bsi] fidget: stance starve applied on attachment %p%s%p - "
            "SubtleFidgetTimeRange {%.0f, %.0f} -> {1e9, 1e9}%s (apply #%u)",
            attach, targets[1] ? " + archetype " : " (no archetype) ",
            targets[1] ? targets[1] : nullptr, g_rangeSaved[0], g_rangeSaved[1],
            g_boolMask ? " + bDisableSubtleFidget set" : "", g_applies);
}

bool wants_install() {
    if (g_implMode.load(std::memory_order_relaxed) != 0 &&
        !g_implInstalled.load(std::memory_order_relaxed))
        return true;
    if (g_actMode.load(std::memory_order_relaxed) != 0 &&
        !g_actInstalled.load(std::memory_order_relaxed))
        return true;
    if (g_reqMode.load(std::memory_order_relaxed) != 0 &&
        !g_reqInstalled.load(std::memory_order_relaxed))
        return true;
    return g_mode.load(std::memory_order_relaxed) != 0 &&
           !g_installed.load(std::memory_order_relaxed) && bones::attachment() != nullptr;
}

bool try_install() {
    // s49: the impl hook rides the same 1 Hz lane but has no attachment
    // dependency (static RVA + prologue gate) - attempt it first so a slow
    // rig resolve cannot delay the route instrument.
    if (g_implMode.load(std::memory_order_relaxed) != 0) try_install_impl();
    if (g_actMode.load(std::memory_order_relaxed) != 0) try_install_act();
    if (g_reqMode.load(std::memory_order_relaxed) != 0) try_install_req();
    if (g_installed.load(std::memory_order_relaxed)) return true;
    if (!patterns::rva_trusted()) return false;
    void* attach = bones::attachment();
    if (!attach) return false;

    // Identity: the attachment must walk as a genuine UObject whose class
    // names XFirstPersonAttachment (this also derives the name offset).
    char cls[64] = {};
    if (!reflect::class_name_of(attach, cls, sizeof cls) ||
        strcmp(cls, "XFirstPersonAttachment") != 0) {
        BVR_LOG("[bsi] fidget: REFUSED - attachment %p classes as '%s', not "
                "XFirstPersonAttachment",
                attach, cls);
        return false;
    }
    g_nameOff = reflect::uobject_name_offset();
    if (g_nameOff < 0) return false;

    // The event's identity, once (fname_find is linear - never on a cadence).
    if (g_startIdx < 0) g_startIdx = patterns::fname_find("StartSubtleFidget");
    if (g_startIdx < 0) {
        BVR_LOG("[bsi] fidget: REFUSED - StartSubtleFidget not in GNames (pool not "
                "populated yet?)");
        return false;
    }

    // The slot and its occupant. An occupant that is neither derived
    // ProcessEvent RVA means the vtable is not what the derivation says.
    void** vt = *reinterpret_cast<void***>(attach);
    if (!bvr::pattern_scan::is_memory_valid(vt, patterns::kProcessEventVtableOffset + 4))
        return false;
    void** slot = vt + patterns::kProcessEventVtableOffset / 4;
    const uint32_t occRva = rva_of(*slot);
    if (occRva != patterns::kActorProcessEventRva && occRva != patterns::kProcessEventRva) {
        BVR_LOG("[bsi] fidget: REFUSED - slot +0x%X holds rva 0x%X, neither "
                "AActor::ProcessEvent (0x%X) nor the UObject base (0x%X)",
                patterns::kProcessEventVtableOffset, occRva,
                patterns::kActorProcessEventRva, patterns::kProcessEventRva);
        return false;
    }

    DWORD old = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) {
        BVR_LOG("[bsi] fidget: VirtualProtect failed on the slot");
        return false;
    }
    g_slot = slot;
    g_orig = reinterpret_cast<ProcessEventFn>(*slot);
    *slot = reinterpret_cast<void*>(&PeDetour); // one aligned pointer write - atomic on x86
    VirtualProtect(slot, sizeof(void*), old, &old);
    g_installed.store(true, std::memory_order_relaxed);
    BVR_LOG("[bsi] fidget: ProcessEvent slot +0x%X on the attachment vtable patched "
            "(was rva 0x%X = %s). Mode %s; StartSubtleFidget = GNames %d. The vtable is "
            "class-wide, so this survives attachment recreation across loads; the block "
            "itself is gated to the resolved attachment object.",
            patterns::kProcessEventVtableOffset, occRva,
            occRva == patterns::kActorProcessEventRva ? "AActor::ProcessEvent"
                                                      : "UObject::ProcessEvent",
            g_mode.load() == 2 ? "FILTER (block)" : "PROBE (log only)", g_startIdx);
    return true;
}

bool handle_command(const char* cmd, const char* args) {
    if (strcmp(cmd, "bsifidget") != 0) return false;
    if (!args) args = "";
    while (*args == ' ') ++args;
    if (strncmp(args, "root", 4) == 0) {
        const char* rest = args + 4;
        while (*rest == ' ') ++rest;
        if (strncmp(rest, "on", 2) == 0) {
            g_rootKill.store(true, std::memory_order_relaxed);
            g_appliedOn = nullptr; // re-apply on the next tick
        } else if (strncmp(rest, "off", 3) == 0) {
            g_rootKill.store(false, std::memory_order_relaxed);
            // Restore the authored range and clear the bit so OFF is a true
            // A/B lever, not just stop-applying.
            if (g_appliedOn) {
                if (g_rangeOff) {
                    float* range = reinterpret_cast<float*>(
                        static_cast<uint8_t*>(g_appliedOn) + g_rangeOff);
                    if (bvr::pattern_scan::is_memory_valid(range, 8)) {
                        range[0] = g_rangeSaved[0];
                        range[1] = g_rangeSaved[1];
                    }
                }
                if (g_boolMask) {
                    uint8_t* p = static_cast<uint8_t*>(g_appliedOn) + g_boolOff;
                    if (bvr::pattern_scan::is_memory_valid(p, 4))
                        *reinterpret_cast<uint32_t*>(p) &= ~g_boolMask;
                }
            }
            g_appliedOn = nullptr;
        }
        BVR_LOG("[bsi] fidget: root kill %s (applies=%u, range %s+0x%X {%.0f, %.0f} "
                "authored, bool +0x%X mask 0x%X)",
                g_rootKill.load() ? "ON" : "off", g_applies,
                g_rangeOff ? "attachment" : "underived", g_rangeOff, g_rangeSaved[0],
                g_rangeSaved[1], g_boolOff, g_boolMask);
        return true;
    }
    if (strncmp(args, "act", 3) == 0) {
        const char* rest = args + 3;
        while (*rest == ' ') ++rest;
        if (strncmp(rest, "probe", 5) == 0) {
            g_actMode.store(1, std::memory_order_relaxed);
            BVR_LOG("[bsi] fidget: act PROBE - every by-name anim action is logged with "
                    "its caller rva");
        } else if (strncmp(rest, "block", 5) == 0) {
            int idx = -1;
            if (sscanf_s(rest + 5, "%d", &idx) == 1 && idx > 0) {
                g_actBlockIdx.store(idx, std::memory_order_relaxed);
                g_actMode.store(2, std::memory_order_relaxed);
                char nm[patterns::kFNameTextBufMin] = {};
                if (idx < patterns::fname_count())
                    patterns::fname_text(idx, nm, sizeof nm);
                BVR_LOG("[bsi] fidget: act BLOCK - name idx %d ('%s') on the FP network "
                        "is refused at the play entry",
                        idx, nm[0] ? nm : "?");
            } else {
                BVR_LOG("[bsi] fidget: act block needs a GNames index - bsifidget act "
                        "block <idx>");
            }
        } else if (strncmp(rest, "off", 3) == 0) {
            g_actMode.store(0, std::memory_order_relaxed);
            BVR_LOG("[bsi] fidget: act OFF - actions pass through silently (hook stays)");
        } else {
            BVR_LOG("[bsi] fidget: act %s mode=%d blockIdx=%d | calls=%u blocked=%u | "
                    "bsifidget act probe|block <idx>|off",
                    g_actInstalled.load() ? "INSTALLED" : "not installed",
                    g_actMode.load(std::memory_order_relaxed),
                    g_actBlockIdx.load(std::memory_order_relaxed),
                    g_actCalls.load(std::memory_order_relaxed),
                    g_actBlocked.load(std::memory_order_relaxed));
        }
        return true;
    }
    if (strncmp(args, "req", 3) == 0) {
        const char* rest = args + 3;
        while (*rest == ' ') ++rest;
        if (strncmp(rest, "probe", 5) == 0) {
            g_reqMode.store(1, std::memory_order_relaxed);
            BVR_LOG("[bsi] fidget: req PROBE - every Morpheme request is logged with its "
                    "descriptor and caller rva");
        } else if (strncmp(rest, "block", 5) == 0) {
            unsigned id = 0;
            if (sscanf_s(rest + 5, "%x", &id) == 1) {
                g_reqBlockId.store(id, std::memory_order_relaxed);
                g_reqMode.store(2, std::memory_order_relaxed);
                BVR_LOG("[bsi] fidget: req BLOCK - descriptor dword0 0x%08X on the FP "
                        "network is refused at the post entry",
                        id);
            } else {
                BVR_LOG("[bsi] fidget: req block needs a hex descriptor id - bsifidget "
                        "req block <hexId>");
            }
        } else if (strncmp(rest, "off", 3) == 0) {
            g_reqMode.store(0, std::memory_order_relaxed);
            BVR_LOG("[bsi] fidget: req OFF - requests pass through silently (hook stays)");
        } else {
            BVR_LOG("[bsi] fidget: req %s mode=%d blockId=0x%08X | calls=%u blocked=%u | "
                    "bsifidget req probe|block <hexId>|off",
                    g_reqInstalled.load() ? "INSTALLED" : "not installed",
                    g_reqMode.load(std::memory_order_relaxed),
                    g_reqBlockId.load(std::memory_order_relaxed),
                    g_reqCalls.load(std::memory_order_relaxed),
                    g_reqBlocked.load(std::memory_order_relaxed));
        }
        return true;
    }
    if (strncmp(args, "impl", 4) == 0) {
        const char* rest = args + 4;
        while (*rest == ' ') ++rest;
        if (strncmp(rest, "probe", 5) == 0) {
            g_implMode.store(1, std::memory_order_relaxed);
            BVR_LOG("[bsi] fidget: impl PROBE - every StartSubtleFidget impl call is "
                    "logged and passes through (installs on the next camera tick if not "
                    "yet)");
        } else if (strncmp(rest, "block", 5) == 0) {
            g_implMode.store(2, std::memory_order_relaxed);
            BVR_LOG("[bsi] fidget: impl BLOCK - StartSubtleFidget on the resolved "
                    "attachment is refused at the native body (anim + self re-arm both "
                    "skipped)");
        } else if (strncmp(rest, "off", 3) == 0) {
            g_implMode.store(0, std::memory_order_relaxed);
            BVR_LOG("[bsi] fidget: impl OFF - calls pass through silently (hook stays "
                    "installed)");
        } else {
            BVR_LOG("[bsi] fidget: impl %s mode=%d | calls=%u ours=%u blocked=%u | "
                    "bsifidget impl probe|block|off",
                    g_implInstalled.load() ? "INSTALLED" : "not installed",
                    g_implMode.load(std::memory_order_relaxed),
                    g_implCalls.load(std::memory_order_relaxed),
                    g_implOurs.load(std::memory_order_relaxed),
                    g_implBlocked.load(std::memory_order_relaxed));
        }
        return true;
    }
    if (strncmp(args, "probe", 5) == 0) {
        g_mode.store(1, std::memory_order_relaxed);
        BVR_LOG("[bsi] fidget: PROBE - StartSubtleFidget passes through and is logged "
                "(installs on the next camera tick if not yet)");
    } else if (strncmp(args, "on", 2) == 0) {
        g_mode.store(2, std::memory_order_relaxed);
        BVR_LOG("[bsi] fidget: FILTER ON - StartSubtleFidget on the attachment is refused "
                "at dispatch");
    } else if (strncmp(args, "off", 3) == 0) {
        g_mode.store(0, std::memory_order_relaxed);
        uninstall("bsifidget off");
    } else {
        BVR_LOG("[bsi] fidget: %s mode=%d | events=%u startSeen=%u blocked=%u | impl %s "
                "mode=%d calls=%u ours=%u blocked=%u | bsifidget probe|on|off | "
                "bsifidget impl probe|block|off | bsifidget root on|off",
                g_installed.load() ? "INSTALLED" : "not installed",
                g_mode.load(std::memory_order_relaxed),
                g_events.load(std::memory_order_relaxed),
                g_startSeen.load(std::memory_order_relaxed),
                g_blocked.load(std::memory_order_relaxed),
                g_implInstalled.load() ? "INSTALLED" : "not installed",
                g_implMode.load(std::memory_order_relaxed),
                g_implCalls.load(std::memory_order_relaxed),
                g_implOurs.load(std::memory_order_relaxed),
                g_implBlocked.load(std::memory_order_relaxed));
    }
    return true;
}

void draw_debug_ui() {
    // s49: the impl hook - the root lever. Radio mirrors `bsifidget impl ...`.
    int implMode = g_implMode.load(std::memory_order_relaxed);
    ImGui::Text("stance root (StartSubtleFidget impl):");
    ImGui::SameLine();
    if (ImGui::RadioButton("probe##fidgetimpl", implMode == 1))
        handle_command("bsifidget", "impl probe");
    ImGui::SameLine();
    if (ImGui::RadioButton("BLOCK##fidgetimpl", implMode == 2))
        handle_command("bsifidget", "impl block");
    ImGui::SameLine();
    ImGui::Text("calls %u ours %u blocked %u",
                g_implCalls.load(std::memory_order_relaxed),
                g_implOurs.load(std::memory_order_relaxed),
                g_implBlocked.load(std::memory_order_relaxed));
    // s48b: the property-side starve - measured INSUFFICIENT (the anim-tree
    // node keeps its own timing copy); kept as a layer, default off.
    bool root = g_rootKill.load(std::memory_order_relaxed);
    if (ImGui::Checkbox("stance property starve (s48b: NOT sufficient alone)", &root)) {
        handle_command("bsifidget", root ? "root on" : "root off");
    }
    ImGui::SameLine();
    ImGui::Text("applies %u | events seen %u blocked %u", g_applies,
                g_startSeen.load(std::memory_order_relaxed),
                g_blocked.load(std::memory_order_relaxed));
}

} // namespace bvr::bsi::fidget
