#include "game/bioshockinf/input_drive.h"

#include "core/input/xinput_bridge.h"
#include "core/util/log.h"
#include "game/bioshockinf/camera.h"
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

const char* profile_name() {
    return bvr::input::pad_profile() == bvr::input::PadProfile::Infinite ? "infinite"
                                                                        : "bioshock1";
}

} // namespace

void arm_pad_profile() {
    // The audited retail map (ENGINE_NOTES "The audited retail pad map") is the
    // spec: straight-through faces, RS-click FORWARDED for XToggleZoom, and a
    // fourth flick direction for the nav cycle pair. Core's default is BS1's
    // map and BS1/BS2 never call this.
    bvr::input::set_pad_profile(bvr::input::PadProfile::Infinite);
    // s52: this game's bumpers are momentary weapon/plasmid cycle taps, not
    // BS1's radial holds - the pitch kill must hold through them or every
    // grip squeeze leaks stick pitch into the engine basis.
    bvr::input::set_pitch_kill_lift_on_bumpers(false);
}

bool enabled() { return g_enabled.load(std::memory_order_relaxed); }

void set_enabled_from_config(bool on) { apply(on); }

bool handle_command(const char* cmd, const char* args) {
    if (strcmp(cmd, "bsiinput") != 0) return false;
    if (args && strncmp(args, "padmap", 6) == 0) {
        // The live A/B. `bs1` is not a supported way to play Infinite - it is
        // the control that shows the map is what changed, so a headset report
        // of "button X does the wrong thing" can be bisected without a rebuild.
        const char* rest = args + 6;
        while (*rest == ' ') ++rest;
        if (strncmp(rest, "bs1", 3) == 0)
            bvr::input::set_pad_profile(bvr::input::PadProfile::Bioshock1);
        else if (strncmp(rest, "inf", 3) == 0)
            bvr::input::set_pad_profile(bvr::input::PadProfile::Infinite);
        BVR_LOG("[bsi] input: pad map = %s (bsiinput padmap inf|bs1|status; "
                "`vrinput padlog on` prints the composed bit for every control)",
                profile_name());
    } else if (args && strncmp(args, "on", 2) == 0) {
        apply(true);
    } else if (args && strncmp(args, "off", 3) == 0) {
        apply(false);
    } else {
        BVR_LOG("[bsi] input: %s, IAT %s, pad map %s | usage: bsiinput on|off|padmap|status "
                "(vrinput <...> reaches the core bridge's own verbs: padlog, pitchkill, "
                "turnscale, test, ...)",
                g_enabled.load(std::memory_order_relaxed) ? "ON" : "off",
                g_hijacked.load(std::memory_order_relaxed) ? "hijacked" : "not hijacked",
                profile_name());
    }
    return true;
}

void draw_debug_ui() {
    if (!ImGui::CollapsingHeader("INPUT (I7)")) return;
    bool on = g_enabled.load(std::memory_order_relaxed);
    if (ImGui::Checkbox("Synthetic pad (Touch -> XInput via IAT)", &on)) apply(on);
    // The in-headset A/B for the pad map. Anything judged by eye has to be a
    // control here rather than a typed command: alt-tabbing to type destabilises
    // the XR session.
    int prof = bvr::input::pad_profile() == bvr::input::PadProfile::Infinite ? 1 : 0;
    ImGui::TextDisabled("Pad map");
    bool changed = ImGui::RadioButton("Infinite (A jump, B crouch, X use, Y melee, RS zoom)",
                                      &prof, 1);
    changed |= ImGui::RadioButton("BioShock 1 (control: B jump, Y med, RS eaten)", &prof, 0);
    if (changed)
        bvr::input::set_pad_profile(prof == 1 ? bvr::input::PadProfile::Infinite
                                              : bvr::input::PadProfile::Bioshock1);
    // s52: body-follows-head. The pitch-kill and snap-turn controls live in
    // core's own INPUT overlay section; this one is adapter state.
    bool bf = camera::body_follow_head();
    if (ImGui::Checkbox("BODY FOLLOWS HEAD (stick walks along head yaw)", &bf))
        camera::set_body_follow_head(bf);
    ImGui::TextDisabled("bsiinput on|off|padmap|status; bsibody on|off; core verbs via vrinput");
}

} // namespace bvr::bsi::input_drive
