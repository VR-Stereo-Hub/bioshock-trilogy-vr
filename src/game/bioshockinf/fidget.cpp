#include "game/bioshockinf/fidget.h"

#include <windows.h>

#include <atomic>
#include <cstring>

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

bool wants_install() {
    return g_mode.load(std::memory_order_relaxed) != 0 &&
           !g_installed.load(std::memory_order_relaxed) && bones::attachment() != nullptr;
}

bool try_install() {
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
        BVR_LOG("[bsi] fidget: %s mode=%d | events=%u startSeen=%u blocked=%u | "
                "bsifidget probe|on|off|status",
                g_installed.load() ? "INSTALLED" : "not installed",
                g_mode.load(std::memory_order_relaxed),
                g_events.load(std::memory_order_relaxed),
                g_startSeen.load(std::memory_order_relaxed),
                g_blocked.load(std::memory_order_relaxed));
    }
    return true;
}

void draw_debug_ui() {
    // s48: demoted from "root kill" to observer - the stance starts natively
    // (see the header); the block stays available for the layered kill later.
    bool on = g_mode.load(std::memory_order_relaxed) == 2;
    if (ImGui::Checkbox("block StartSubtleFidget events (NOT the stance root - s48)", &on))
        g_mode.store(on ? 2 : 1, std::memory_order_relaxed);
    ImGui::SameLine();
    ImGui::Text("seen %u blocked %u", g_startSeen.load(std::memory_order_relaxed),
                g_blocked.load(std::memory_order_relaxed));
}

} // namespace bvr::bsi::fidget
