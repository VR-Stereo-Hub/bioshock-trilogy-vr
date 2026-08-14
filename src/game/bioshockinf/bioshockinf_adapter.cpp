#include "game/bioshockinf/bioshockinf_adapter.h"

#include "core/framework/command.h"
#include "core/hooks/d3d11_hook.h"
#include "core/input/xinput_bridge.h"
#include "core/util/log.h"
#include "core/vr/openxr_runtime.h"
#include "game/bioshockinf/camera.h"
#include "game/bioshockinf/config.h"
#include "game/bioshockinf/aim.h"
#include "game/bioshockinf/arsenal.h"
#include "game/bioshockinf/cine.h"
#include "game/bioshockinf/bones.h"
#include "game/bioshockinf/fidget.h"
#include "game/bioshockinf/fire.h"
#include "game/bioshockinf/fxorigin.h"
#include "game/bioshockinf/gfx.h"
#include "game/bioshockinf/hands.h"
#include "game/bioshockinf/hide.h"
#include "game/bioshockinf/hud.h"
#include "game/bioshockinf/input_drive.h"
#include "game/bioshockinf/hint.h"
#include "game/bioshockinf/lens.h"
#include "game/bioshockinf/melee.h"
#include "game/bioshockinf/xhair.h"
#include "game/bioshockinf/patterns.h"
#include "game/bioshockinf/profiles.h"
#include "game/bioshockinf/recorder.h"
#include "game/bioshockinf/reflect.h"
#include "game/bioshockinf/scenedraw.h"

#include <imgui.h>

#include <atomic>
#include <cstring>

namespace bvr::bsi {
namespace {

std::atomic<bool> g_loggedFovRequest{false};

void log_status() {
    const patterns::Symbols& s = patterns::symbols();
    BVR_LOG("[bsi] status: build %s (rva_trusted=%s) | image %p size 0x%zX | camera hook %s, "
            "fired %s | GNames Num=%d | presents %llu",
            s.buildVerified ? "VERIFIED" : "NOT RECOGNISED",
            patterns::rva_trusted() ? "yes" : "no", s.imageBase, s.imageSize,
            camera::hook_live() ? "installed" : "NOT installed",
            camera::has_fired() ? "YES" : "no", patterns::fname_count(),
            static_cast<unsigned long long>(bvr::d3d11_hook::present_count()));
}

} // namespace

uint32_t BioshockInfAdapter::capabilities() const {
    // A bit is earned by a hook OBSERVED FIRING, never by an address resolving
    // or even by a successful install - a deliberate divergence from BS1 and
    // BS2, which both key this off hook_live(). On this game the camera hook is
    // read-only in I2, so the bit advertises that the seam is real and reached,
    // which is exactly what a later milestone needs to know.
    uint32_t caps = camera::has_fired() ? bvr::game::CAP_CAMERA_OVERRIDE : 0u;
    // Same rule for the SR bit: earned by the scene-draw hook being LIVE (it
    // observes every frame once installed), not by an address resolving.
    if (scenedraw::hook_live()) caps |= bvr::game::CAP_SCENE_REENTRY;
    return caps;
}

bool BioshockInfAdapter::init(const bvr::pattern_scan::ProcessImage& image) {
    patterns::Symbols symbols{};
    const bool verified = patterns::resolve(image, symbols); // logs every stage

    // THE POINT OF I1. Core polls command.txt from Present, so this adapter has
    // a working command surface before it has a single engine hook - on BS1 and
    // BS2 the poller lives in the adapter and ticks off the camera hook, which
    // means no way to talk to the mod until the thing being debugged works.
    bvr::command::enable_present_pump();

    // s62b: must land before the game creates its D3D device - see gfx.h.
    gfx::install_dxgi11_upgrade();

    reflect::init(image);

    // DR-I2 seam + the I4 drive (drive ships OFF; out-param substitution
    // only). Prologue-gated and fail-soft: a refusal logs and the game runs
    // flat. Once it fires it takes over the command poll from the Present
    // pump (see the lease in core/framework/command).
    const bool hooked = camera::install(image);

    // Persisted tuning (worldScale). File read only - touches no engine state.
    camera::load_vr_preset();
    // s52: per-weapon overrides (weapons.ini). File read only; applies happen
    // on the game thread's identity poll (profiles::tick).
    profiles::init();
    // s52: the GFx HUD lane - classifier armed; the redirect engages per-tick
    // only while an XR session is live (hud::tick).
    hud::init();

    // S43b, HEADSET-VERIFIED (user, 2026-08-06): this engine's renderer is
    // threaded with OneFrameThreadLag, so presented content is TWO locate
    // generations old - the historical one-back attribution wobbled with head
    // speed ("jumpy camera"); lag 2 is "extremely smooth". Named by the
    // in-headset A/B (ENGINE_NOTES s43b); the F10 radios / `bsipose` stay
    // live for re-derivation. BS1/BS2 never call this - their core default
    // (1) is untouched.
    bvr::vr::set_pose_lag(2);

    // s50: rendered-pose eye tags, DEFAULT ON for Infinite (the FOV-edge
    // lever). The projection layer then describes the parallel camera this
    // adapter actually renders (base +- ipd/2 along the head's right) instead
    // of the runtime's located per-eye poses - identical on a parallel-view
    // runtime (the sim), removes the cant/IPD claim-vs-render mismatch under
    // VDXR. `bsicam eyetag off` is the in-headset A/B. BS1/BS2 never call
    // this - their tags stay historical.
    bvr::vr::set_eye_tag_rendered(true);

    // s54: THE PACE FEED (layer-carrying keepalives while parked VISIBLE;
    // ENGINE_NOTES s54) was armed here for one headset session and DISARMED
    // s54b BY USER DIRECTIVE: with it armed, alt-tab/doff froze the game
    // outright (draw thread wedged in win32u, presents 0, ghost -> the known
    // exit-path fault on close) - strictly worse than the pre-s54 behaviour
    // it replaced (the ~10 Hz VISIBLE throttle + the VR-toggle protocol).
    // The raffle scene-stall it was built for turned out NOT to be the
    // session park at all (it stalls fully FOCUSED with input live). The
    // machinery stays in core, default off; `vrpace feed on` remains the
    // controlled-experiment lever for a future dashboard A/B.

    // I7 controls (session 44). The map FIRST, then the lane: core's default is
    // BioShock 1's table, which is wrong here on four counts (the face re-route,
    // the consumed RS-click, the missing fourth flick direction, and the ammomod
    // preference that does not apply where RS-click is a real binding). Armed
    // directly rather than through the posting lane, and before the arm, so the
    // very first composed pad already carries Infinite's map. BS1/BS2 never call
    // set_pad_profile - their composed pad is byte-identical (proved on the sim
    // lane, ENGINE_NOTES s44).
    input_drive::arm_pad_profile();
    // s50: the flourish chord (left thumbrest + A) - the XR composer consumes
    // A while the rest is touched and counts the edges; fidget::flourish_tick
    // drains them on the game thread. Infinite-only (default off in core).
    bvr::input::arm_flourish_chord(true);
    // Then the pad itself, from the preset (`inputOn`, default on). This is what
    // makes a headset boot come up with a working controller: everything judged
    // by eye must be an F10 control, and reaching F10 needs a controller.
    input_drive::set_enabled_from_config(camera::input_armed_at_boot());

    BVR_LOG("[bsi] adapter ready - build gate %s, camera hook %s. Command seam is live either "
            "way (Present pump), and hands over to the game thread on the hook's first fire.",
            verified ? "PASSED" : "CLOSED (features stand down)",
            hooked ? "INSTALLED (read-only)" : "not installed");
    return verified;
}

void BioshockInfAdapter::setFov(float hfovDeg) {
    if (!g_loggedFovRequest.exchange(true)) {
        BVR_LOG("[bsi] setFov(%.1f) ignored - no FOV lever exists yet. Infinite's native slider "
                "spans only ~70 to ~80.5 deg and the property chain is NAMED, so the lever gets "
                "built at I5 via `set`-by-name rather than a memory scan.", hfovDeg);
    }
}

void BioshockInfAdapter::drawDebugUi() {
    if (!ImGui::CollapsingHeader("BioShock Infinite (I1 skeleton)",
                                 ImGuiTreeNodeFlags_DefaultOpen))
        return;
    const patterns::Symbols& s = patterns::symbols();
    if (s.buildVerified) {
        ImGui::Text("host build: VERIFIED (pe-timestamp 0x%08X)", patterns::kHostTimeDateStamp);
    } else {
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f),
                           "host build NOT RECOGNISED - addresses stand down");
    }
    ImGui::Text("image: %p size 0x%zX%s", s.imageBase, s.imageSize,
                reinterpret_cast<uintptr_t>(s.imageBase) == patterns::kExpectedImageBase
                    ? " (fixed base, ASLR off)"
                    : " (REBASED)");
    if (s.viewPointReadable) {
        ImGui::Text("GetPlayerViewPoint RVA 0x%X: %02X %02X %02X %02X %02X %02X %02X %02X "
                    "(read-only probe)",
                    patterns::kGetPlayerViewPointRva, s.viewPointBytes[0], s.viewPointBytes[1],
                    s.viewPointBytes[2], s.viewPointBytes[3], s.viewPointBytes[4],
                    s.viewPointBytes[5], s.viewPointBytes[6], s.viewPointBytes[7]);
    }
    ImGui::Text("presents: %llu   capabilities: 0x%X",
                static_cast<unsigned long long>(bvr::d3d11_hook::present_count()),
                capabilities());
    ImGui::TextDisabled("seam: bsi | buildgate | bsicam | bsivr | vrstereo | vraer | reentry | "
                        "bsifov | bsilens | bsires | ipd | simhead | recenter | worldscale | "
                        "vrpreset | vrrec | bsireflect | bsinative | bsicall | vrcmd");

    camera::draw_debug_ui();
    config::draw_debug_ui();
    input_drive::draw_debug_ui();
    hands::draw_debug_ui();
    aim::draw_debug_ui();
    fire::draw_debug_ui();
    fxorigin::draw_debug_ui();
    hud::draw_debug_ui();
    cine::draw_debug_ui();
    arsenal::draw_debug_ui();
    profiles::draw_debug_ui();
    lens::draw_debug_ui();
    scenedraw::draw_debug_ui();
    reflect::draw_debug_ui();
}

bool BioshockInfAdapter::handleCommand(const char* cmd, const char* args) {
    if (strcmp(cmd, "bsi") == 0) {
        log_status();
        return true;
    }
    if (strcmp(cmd, "buildgate") == 0) {
        patterns::handle_buildgate_command(args);
        return true;
    }
    if (strcmp(cmd, "bsicam") == 0) {
        if (!camera::handle_command(args))
            BVR_LOG("[bsi] camera: unknown subcommand. bsicam status|paths|tid|matrix|"
                    "heartbeat on|off|drive on|off|on|off");
        return true;
    }
    // I4 drive verbs (simhead / recenter / worldscale / vrpreset) + the I5
    // stereo verbs (vrstereo / vraer / ipd / bsifov) + vrrec.
    if (camera::handle_drive_verb(cmd, args)) return true;
    if (lens::handle_command(cmd, args)) return true;
    if (input_drive::handle_command(cmd, args)) return true;
    if (aim::handle_command(cmd, args)) return true;
    if (fire::handle_command(cmd, args)) return true;
    if (fxorigin::handle_command(cmd, args)) return true;
    if (fidget::handle_command(cmd, args)) return true;
    if (hands::handle_command(cmd, args)) return true;
    if (hide::handle_command(cmd, args)) return true;
    if (melee::handle_command(cmd, args)) return true;
    if (xhair::handle_command(cmd, args)) return true;
    if (hint::handle_command(cmd, args)) return true;
    if (gfx::handle_command(cmd, args)) return true;
    if (bones::handle_command(cmd, args)) return true;
    if (profiles::handle_command(cmd, args)) return true;
    if (arsenal::handle_command(cmd, args)) return true;
    if (hud::handle_command(cmd, args)) return true;
    if (cine::handle_command(cmd, args)) return true;
    if (strcmp(cmd, "reentry") == 0) {
        if (!scenedraw::handle_command(args))
            BVR_LOG("[bsi] reentry: unknown subcommand. reentry status|reset|pulse [n]|"
                    "stereo on|off");
        return true;
    }
    if (strcmp(cmd, "vrrec") == 0) {
        recorder::handle_command(args);
        return true;
    }
    if (strcmp(cmd, "bsivr") == 0) {
        // Scripted lever on core's master VR enable (the same flag the F10
        // checkbox writes), so the sim battery can drive teardown/re-bring-up
        // without hazard injection. `status` reports the observable state -
        // session_live() - rather than echoing the flag, because the checkbox
        // is a second writer and an echo could go stale.
        char sub[16] = "";
        if (args) sscanf_s(args, "%15s", sub, static_cast<unsigned>(sizeof sub));
        if (strcmp(sub, "on") == 0) {
            bvr::vr::set_enabled(true);
            BVR_LOG("[bsi] vr: enable requested - bring-up happens from Present "
                    "(watch for 'xr: session created')");
        } else if (strcmp(sub, "off") == 0) {
            bvr::vr::set_enabled(false);
            BVR_LOG("[bsi] vr: disable requested - teardown happens from Present "
                    "(watch for 'xr: session teardown')");
        } else {
            BVR_LOG("[bsi] vr: session %s | usage: bsivr on|off|status",
                    bvr::vr::session_live() ? "LIVE" : "not live");
        }
        return true;
    }
    if (reflect::handle_command(cmd, args)) return true;
    return false; // core's shared vocabulary gets a look next
}

bvr::game::IGameAdapter* create_adapter() {
    static BioshockInfAdapter instance;
    return &instance;
}

} // namespace bvr::bsi
