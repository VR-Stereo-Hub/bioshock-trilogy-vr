#include "game/bioshockinf/hud.h"

#include "core/gfx/gfx_hud.h"
#include "core/util/log.h"
#include "core/vr/openxr_runtime.h"

#include <imgui.h>

#include <atomic>
#include <cstring>

namespace bvr::bsi::hud {
namespace {

std::atomic<bool> g_on{true};       // the lane (classifier + provider)
std::atomic<bool> g_redirect{true}; // policy: redirect WHEN a session is live

bool token(const char* args, const char* w) {
    const size_t n = strlen(w);
    if (strncmp(args, w, n) != 0) return false;
    const char c = args[n];
    return c == '\0' || c == ' ' || c == '\n' || c == '\r' || c == '\t';
}

} // namespace

void init() {
    bvr::gfx_hud::set_armed(g_on.load(std::memory_order_relaxed));
    bvr::vr::set_hud_texture_provider(&bvr::gfx_hud::texture);
}

void tick() {
    // The redirect engages only while the XR session is live: without a
    // compositor there is no quad, and a redirected HUD would simply vanish
    // from the window. One relaxed store per tick; gfx_hud logs edges.
    const bool want = g_on.load(std::memory_order_relaxed) &&
                      g_redirect.load(std::memory_order_relaxed) && bvr::vr::session_live();
    bvr::gfx_hud::set_redirect(want);
}

bool handle_command(const char* cmd, const char* args) {
    if (strcmp(cmd, "bsihud") != 0) return false;
    if (!args) args = "";
    while (*args == ' ') ++args;
    if (token(args, "on")) {
        g_on.store(true, std::memory_order_relaxed);
        bvr::gfx_hud::set_armed(true);
    } else if (token(args, "off")) {
        g_on.store(false, std::memory_order_relaxed);
        bvr::gfx_hud::set_armed(false);
        bvr::gfx_hud::set_redirect(false);
    } else if (token(args, "redirect")) {
        const char* rest = args + 8;
        while (*rest == ' ') ++rest;
        if (token(rest, "on")) g_redirect.store(true, std::memory_order_relaxed);
        else if (token(rest, "off")) g_redirect.store(false, std::memory_order_relaxed);
        BVR_LOG("[bsi] hud: redirect policy %s (engages only with a live XR session)",
                g_redirect.load(std::memory_order_relaxed) ? "ON" : "off");
    } else if (token(args, "fx")) {
        const char* rest = args + 2;
        while (*rest == ' ') ++rest;
        if (token(rest, "on")) bvr::gfx_hud::set_fx_passthrough(true);
        else if (token(rest, "off")) bvr::gfx_hud::set_fx_passthrough(false);
        else
            BVR_LOG("[bsi] hud: fx passthrough %s (bsihud fx on|off - full-screen GFx "
                    "effects on the eye image vs the quad)",
                    bvr::gfx_hud::fx_passthrough() ? "ON" : "off");
    } else {
        const bvr::gfx_hud::Census c = bvr::gfx_hud::census();
        BVR_LOG("[bsi] hud: lane %s, redirect policy %s (live %s) | frames %u, boundaries "
                "%u, hudDraws %u (last frame %u), redirected %u, FULL-SCREEN-ish post "
                "draws %u | usage: bsihud on|off|redirect on|off|status",
                g_on.load(std::memory_order_relaxed) ? "ON" : "off",
                g_redirect.load(std::memory_order_relaxed) ? "on" : "off",
                bvr::gfx_hud::redirect_on() ? "ENGAGED" : "idle", c.frames, c.boundaries,
                c.hudDraws, c.lastFrameHud, c.redirected, c.bigPostDraws);
    }
    return true;
}

void draw_debug_ui() {
    if (!ImGui::CollapsingHeader("HUD (I9)")) return;
    bool on = g_on.load(std::memory_order_relaxed);
    if (ImGui::Checkbox("GFx HUD lane (classifier + quad)", &on)) {
        g_on.store(on, std::memory_order_relaxed);
        bvr::gfx_hud::set_armed(on);
        if (!on) bvr::gfx_hud::set_redirect(false);
    }
    bool rd = g_redirect.load(std::memory_order_relaxed);
    if (ImGui::Checkbox("Redirect HUD to the quad (XR sessions only)", &rd))
        g_redirect.store(rd, std::memory_order_relaxed);
    bool fx = bvr::gfx_hud::fx_passthrough();
    if (ImGui::Checkbox("Full-screen FX stay on the eye image", &fx))
        bvr::gfx_hud::set_fx_passthrough(fx);
    const bvr::gfx_hud::Census c = bvr::gfx_hud::census();
    ImGui::Text("boundaries %u  hud draws %u (last %u)  redirected %u", c.boundaries,
                c.hudDraws, c.lastFrameHud, c.redirected);
    ImGui::TextDisabled("quad size/distance: VR overlay section (set_hud_quad sliders)");
}

} // namespace bvr::bsi::hud
