#include "game/bioshockinf/input_drive.h"

#include "core/input/xinput_bridge.h"
#include "core/util/log.h"
#include "game/bioshockinf/patterns.h"

#include <atomic>
#include <cstring>
#include <imgui.h>
#include <windows.h>

namespace bvr::bsi::input_drive {
namespace {

std::atomic<bool> g_hijacked{false};
std::atomic<bool> g_enabled{false};
// Overlay posting lane (render thread checkbox -> game/present state change is
// not needed here: set_enabled/hijack are atomic-latch operations inside the
// bridge, safe from the overlay thread; the arm still logs on the toggle).

// The one derivation this lane consumes: the game's XINPUT1_3 ord-2 IAT slot
// (patterns::kXInputGetStateIatRva, s34 PE parse). Before consuming it, the
// slot's CURRENT target must resolve into a loaded module - a wrong RVA would
// read as garbage here and the arm refuses instead of re-pointing it.
bool arm_hijack() {
    if (g_hijacked.load(std::memory_order_relaxed)) return true;
    if (!patterns::rva_trusted()) {
        BVR_LOG("[bsi] input: REFUSED - build gate closed");
        return false;
    }
    const uint8_t* base = patterns::image_base();
    if (!base) {
        BVR_LOG("[bsi] input: REFUSED - no image base");
        return false;
    }
    void** slot = reinterpret_cast<void**>(
        const_cast<uint8_t*>(base) + patterns::kXInputGetStateIatRva);
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(slot, &mbi, sizeof mbi) || mbi.State != MEM_COMMIT) {
        BVR_LOG("[bsi] input: REFUSED - IAT slot RVA 0x%X is not committed memory",
                patterns::kXInputGetStateIatRva);
        return false;
    }
    void* target = *slot;
    HMODULE mod = nullptr;
    wchar_t modName[MAX_PATH] = L"";
    if (target &&
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           static_cast<LPCWSTR>(target), &mod) &&
        mod) {
        GetModuleFileNameW(mod, modName, MAX_PATH);
    }
    if (!modName[0]) {
        BVR_LOG("[bsi] input: REFUSED - IAT slot 0x%X target %p does not resolve into a "
                "loaded module; the derivation is stale, do not re-point it",
                patterns::kXInputGetStateIatRva, target);
        return false;
    }
    const wchar_t* baseName = wcsrchr(modName, L'\\');
    baseName = baseName ? baseName + 1 : modName;
    BVR_LOG("[bsi] input: IAT slot 0x%X verified - current target %p in %S "
            "(the chain we compose on top of)",
            patterns::kXInputGetStateIatRva, target, baseName);
    if (!bvr::input::hijack_import_slot(slot)) {
        BVR_LOG("[bsi] input: hijack_import_slot refused (see core log line)");
        return false;
    }
    g_hijacked.store(true, std::memory_order_relaxed);
    return true;
}

void apply(bool on) {
    if (on) {
        if (!arm_hijack()) return;
        bvr::input::set_enabled(true);
        g_enabled.store(true, std::memory_order_relaxed);
        BVR_LOG("[bsi] input: synthetic pad ON - core XR actions -> pad via the IAT wrapper; "
                "the game's own DefaultInput.ini bindings route from here (no UpdateInput "
                "pump on this engine - it polls XInput itself)");
    } else {
        bvr::input::set_enabled(false);
        g_enabled.store(false, std::memory_order_relaxed);
        BVR_LOG("[bsi] input: synthetic pad off (the IAT wrapper stays installed and passes "
                "through - compose is gated on the bridge enable)");
    }
}

} // namespace

bool handle_command(const char* cmd, const char* args) {
    if (strcmp(cmd, "bsiinput") != 0) return false;
    if (args && strncmp(args, "on", 2) == 0) {
        apply(true);
    } else if (args && strncmp(args, "off", 3) == 0) {
        apply(false);
    } else {
        BVR_LOG("[bsi] input: %s, IAT %s | usage: bsiinput on|off|status (vrinput <...> "
                "reaches the core bridge's own verbs: pitchkill, turnscale, test, ...)",
                g_enabled.load(std::memory_order_relaxed) ? "ON" : "off",
                g_hijacked.load(std::memory_order_relaxed) ? "hijacked" : "not hijacked");
    }
    return true;
}

void draw_debug_ui() {
    if (!ImGui::CollapsingHeader("INPUT (I7)")) return;
    bool on = g_enabled.load(std::memory_order_relaxed);
    if (ImGui::Checkbox("Synthetic pad (Touch -> XInput via IAT)", &on)) apply(on);
    ImGui::TextDisabled("bsiinput on|off|status; core verbs via vrinput");
}

} // namespace bvr::bsi::input_drive
