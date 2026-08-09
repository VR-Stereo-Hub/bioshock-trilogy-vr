#include "game/bioshockinf/fxorigin.h"

#include "core/hooks/pattern_scan.h"
#include "core/util/log.h"
#include "game/bioshockinf/bones.h"
#include "game/bioshockinf/patterns.h"

#include <MinHook.h>
#include <imgui.h>
#include <windows.h>

#include <atomic>
#include <cstring>

namespace bvr::bsi::fxorigin {
namespace {

// The seam's signature (patterns.h derivation): a `__thiscall` VIRTUAL with
// ZERO stack args - plain `ret`, hence zero declared args here (the RTC
// no-dump rule, inverted-form gate like the fidget impl hook).
using UpdateAttachmentsFn = void(__fastcall*)(void* self, void* edx);

UpdateAttachmentsFn g_original = nullptr;
void* g_target = nullptr;

// HONEST STATUS (s50 flat measurement): this hook is NOT the frozen-FX family
// fix. The dirty-count instrument below proved SpaceBases already holds the
// composed atoms at virtually every attach update (cleanTicks ~100%) - the
// render-side drive covers the attach lane on its own, and the FROZEN family
// (charge plume, ready sparkle, muzzle/tracer FX) reads its positions from a
// lane that never touches SpaceBases (every candidate falsified in
// ENGINE_NOTES "s50"). What the substitute DOES cover: the rare ticks where
// the engine's eval restamps SpaceBases between our writes (dirtyTicks 1-2
// per ~10 min) - without the reapply those are one-frame authored-pose
// flashes on everything the attach walker positions. Cheap, safe, measured.
std::atomic<bool> g_probe{true};       // install + observe (read-only)
std::atomic<bool> g_substitute{true};  // reapply composed atoms before the update
std::atomic<bool> g_installed{false};

// The updater runs for EVERY skeletal component on the map, every tick. The
// gate is self == the driven FP component; the split is telemetry.
std::atomic<uint32_t> g_calls{0};
std::atomic<uint32_t> g_playerCalls{0};
std::atomic<uint32_t> g_reapplies{0};
// s50 eval-ordering instrument: how many driven atoms the engine had
// overwritten since our previous write, sampled at each reapply. Nonzero =
// the anim eval restamps SpaceBases between our writes (the frozen-FX reader
// could be seeing those); zero = SpaceBases was still ours.
std::atomic<uint32_t> g_lastDirty{0};
std::atomic<uint32_t> g_dirtyTicks{0};
std::atomic<uint32_t> g_cleanTicks{0};

uint64_t g_lastLogMs = 0;

void __fastcall UpdateAttachmentsDetour(void* self, void* edx) {
    g_calls.fetch_add(1, std::memory_order_relaxed);
    if (self && self == bones::component()) {
        g_playerCalls.fetch_add(1, std::memory_order_relaxed);
        if (g_substitute.load(std::memory_order_relaxed)) {
            // Repaint OUR composed atoms over the engine's fresh anim eval so
            // the attachment transforms (and every later socket read this
            // tick) come from the driven pose. reapply() carries its own
            // rig-intact + freshness gates and no-ops when the drive is off.
            const int dirty = bones::reapply();
            g_lastDirty.store(static_cast<uint32_t>(dirty), std::memory_order_relaxed);
            (dirty ? g_dirtyTicks : g_cleanTicks).fetch_add(1, std::memory_order_relaxed);
            g_reapplies.fetch_add(1, std::memory_order_relaxed);
        }
        const uint64_t now = GetTickCount64();
        if (now - g_lastLogMs >= 5000) {
            g_lastLogMs = now;
            BVR_LOG("[bsi] fxorigin: attach update on the FP component - %s | calls=%u "
                    "player=%u reapplied=%u | pre-reapply overwritten atoms: last=%u "
                    "dirtyTicks=%u cleanTicks=%u",
                    g_substitute.load(std::memory_order_relaxed) ? "COMPOSED ATOMS REAPPLIED"
                                                                 : "probe (pass-through)",
                    g_calls.load(std::memory_order_relaxed),
                    g_playerCalls.load(std::memory_order_relaxed),
                    g_reapplies.load(std::memory_order_relaxed),
                    g_lastDirty.load(std::memory_order_relaxed),
                    g_dirtyTicks.load(std::memory_order_relaxed),
                    g_cleanTicks.load(std::memory_order_relaxed));
        }
    }
    g_original(self, edx);
}

} // namespace

bool wants_install() {
    return (g_probe.load(std::memory_order_relaxed) ||
            g_substitute.load(std::memory_order_relaxed)) &&
           !g_installed.load(std::memory_order_relaxed);
}

bool try_install() {
    if (g_installed.load(std::memory_order_relaxed)) return true;
    if (!patterns::rva_trusted()) {
        BVR_LOG("[bsi] fxorigin: REFUSED - build gate closed");
        return false;
    }
    uint8_t* impl = const_cast<uint8_t*>(patterns::image_base()) +
                    patterns::kSkelCompUpdateAttachmentsRva;
    if (!bvr::pattern_scan::is_memory_valid(impl, 0x440)) {
        BVR_LOG("[bsi] fxorigin: REFUSED - impl rva 0x%X not readable",
                patterns::kSkelCompUpdateAttachmentsRva);
        return false;
    }
    // Gate 1: the pinned prologue (kGetPlayerViewPointPrologue discipline).
    if (memcmp(impl, patterns::kSkelCompUpdateAttachmentsPrologue,
               sizeof patterns::kSkelCompUpdateAttachmentsPrologue) != 0) {
        BVR_LOG("[bsi] fxorigin: REFUSED - prologue at rva 0x%X does not match the "
                "derivation (stale build?)",
                patterns::kSkelCompUpdateAttachmentsRva);
        return false;
    }
    // Gate 2: the class identity. The RVA must be what the live
    // XSkeletalMeshComponent vtable carries in the derived slot - a stronger
    // claim than bytes alone, and it breaks loudly on a relinked build.
    const uint8_t* vtable = patterns::image_base() + patterns::kSkelCompVtableRva;
    if (!bvr::pattern_scan::is_memory_valid(vtable, patterns::kSkelCompUpdateAttachmentsVtableSlot * 4 + 4)) {
        BVR_LOG("[bsi] fxorigin: REFUSED - vtable rva 0x%X not readable",
                patterns::kSkelCompVtableRva);
        return false;
    }
    const void* slotTarget = reinterpret_cast<const void* const*>(
        vtable)[patterns::kSkelCompUpdateAttachmentsVtableSlot];
    if (slotTarget != impl) {
        BVR_LOG("[bsi] fxorigin: REFUSED - vtable slot %u holds %p, expected rva 0x%X",
                patterns::kSkelCompUpdateAttachmentsVtableSlot, slotTarget,
                patterns::kSkelCompUpdateAttachmentsRva);
        return false;
    }
    // Gate 3: the arity, inverted form (fidget impl precedent) - the epilogue
    // must be a PLAIN `ret` (8B E5 5D C3) with no `ret imm16` before it.
    bool sawPlainRet = false;
    for (size_t i = 0; i + 4 < 0x440; ++i) {
        if (impl[i] == 0xC2 && impl[i + 2] == 0x00) {
            BVR_LOG("[bsi] fxorigin: REFUSED - unexpected `ret %u` at +0x%X (arity drift)",
                    impl[i + 1], static_cast<unsigned>(i));
            return false;
        }
        if (impl[i] == 0x8B && impl[i + 1] == 0xE5 && impl[i + 2] == 0x5D &&
            impl[i + 3] == 0xC3) {
            sawPlainRet = true;
            break;
        }
    }
    if (!sawPlainRet) {
        BVR_LOG("[bsi] fxorigin: REFUSED - no plain-ret epilogue in 0x440 bytes at rva 0x%X",
                patterns::kSkelCompUpdateAttachmentsRva);
        return false;
    }
    if (MH_CreateHook(impl, reinterpret_cast<void*>(&UpdateAttachmentsDetour),
                      reinterpret_cast<void**>(&g_original)) != MH_OK) {
        BVR_LOG("[bsi] fxorigin: MH_CreateHook failed at rva 0x%X",
                patterns::kSkelCompUpdateAttachmentsRva);
        return false;
    }
    if (MH_EnableHook(impl) != MH_OK) {
        BVR_LOG("[bsi] fxorigin: MH_EnableHook failed at rva 0x%X",
                patterns::kSkelCompUpdateAttachmentsRva);
        return false;
    }
    g_target = impl;
    g_installed.store(true, std::memory_order_relaxed);
    BVR_LOG("[bsi] fxorigin: attach-update seam hooked - XSkeletalMeshComponent attachment "
            "updater rva 0x%X (vtable slot %u). Mode: %s. Instrument + edge cover: repaints "
            "the composed atoms before the walk so the rare eval restamps never reach the "
            "attachments (the frozen-FX family reads elsewhere - see ENGINE_NOTES s50).",
            patterns::kSkelCompUpdateAttachmentsRva,
            patterns::kSkelCompUpdateAttachmentsVtableSlot,
            g_substitute.load(std::memory_order_relaxed) ? "REAPPLYING" : "PROBE (read-only)");
    return true;
}

bool handle_command(const char* cmd, const char* args) {
    if (strcmp(cmd, "bsifx") != 0) return false;
    if (!args) args = "";
    while (*args == ' ') ++args;

    if (strncmp(args, "probe on", 8) == 0) {
        g_probe.store(true, std::memory_order_relaxed);
        BVR_LOG("[bsi] fxorigin: PROBE armed - installs on the next camera tick, read-only");
    } else if (strncmp(args, "probe off", 9) == 0) {
        g_probe.store(false, std::memory_order_relaxed);
        BVR_LOG("[bsi] fxorigin: probe off (an installed hook stays installed and passes "
                "through)");
    } else if (strncmp(args, "on", 2) == 0) {
        g_probe.store(true, std::memory_order_relaxed);
        g_substitute.store(true, std::memory_order_relaxed);
        BVR_LOG("[bsi] fxorigin: pre-walk reapply ARMED (edge cover for eval restamps; "
                "NOT the frozen-FX family fix)");
    } else if (strncmp(args, "off", 3) == 0) {
        g_substitute.store(false, std::memory_order_relaxed);
        BVR_LOG("[bsi] fxorigin: reapply off (the rare eval-restamp ticks reach the "
                "attachments again)");
    } else {
        BVR_LOG("[bsi] fxorigin: installed=%d probe=%d write=%d | calls=%u player=%u "
                "reapplied=%u | bsifx probe on|off | on|off | status",
                g_installed.load(std::memory_order_relaxed) ? 1 : 0,
                g_probe.load(std::memory_order_relaxed) ? 1 : 0,
                g_substitute.load(std::memory_order_relaxed) ? 1 : 0,
                g_calls.load(std::memory_order_relaxed),
                g_playerCalls.load(std::memory_order_relaxed),
                g_reapplies.load(std::memory_order_relaxed));
    }
    return true;
}

void draw_debug_ui() {
    if (!ImGui::CollapsingHeader("ATTACH UPDATE (s50) - instrument + eval-restamp cover"))
        return;
    bool sub = g_substitute.load(std::memory_order_relaxed);
    if (ImGui::Checkbox("Pre-walk reapply (covers rare eval restamps)", &sub)) {
        if (sub) g_probe.store(true, std::memory_order_relaxed);
        g_substitute.store(sub, std::memory_order_relaxed);
    }
    ImGui::Text("hook %s   calls %u   player %u   reapplied %u",
                g_installed.load(std::memory_order_relaxed) ? "LIVE" : "not installed",
                g_calls.load(std::memory_order_relaxed),
                g_playerCalls.load(std::memory_order_relaxed),
                g_reapplies.load(std::memory_order_relaxed));
    ImGui::TextDisabled("bsifx probe on|off | on|off | status");
}

} // namespace bvr::bsi::fxorigin
