#include "game/bioshockinf/bioshockinf_adapter.h"

#include "core/framework/command.h"
#include "core/hooks/d3d11_hook.h"
#include "core/util/log.h"
#include "core/vr/openxr_runtime.h"
#include "game/bioshockinf/camera.h"
#include "game/bioshockinf/patterns.h"
#include "game/bioshockinf/reflect.h"

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
    return camera::has_fired() ? bvr::game::CAP_CAMERA_OVERRIDE : 0u;
}

bool BioshockInfAdapter::init(const bvr::pattern_scan::ProcessImage& image) {
    patterns::Symbols symbols{};
    const bool verified = patterns::resolve(image, symbols); // logs every stage

    // THE POINT OF I1. Core polls command.txt from Present, so this adapter has
    // a working command surface before it has a single engine hook - on BS1 and
    // BS2 the poller lives in the adapter and ticks off the camera hook, which
    // means no way to talk to the mod until the thing being debugged works.
    bvr::command::enable_present_pump();

    reflect::init(image);

    // DR-I2. Read-only, prologue-gated, and fail-soft: a refusal logs and the
    // game runs flat. Once it fires it takes over the command poll from the
    // Present pump (see the lease in core/framework/command).
    const bool hooked = camera::install(image);

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
    ImGui::TextDisabled("seam: bsi | buildgate | bsicam | bsivr | bsireflect | bsinative | bsicall | vrcmd");

    camera::draw_debug_ui();
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
                    "heartbeat on|off|on|off");
        return true;
    }
    if (strcmp(cmd, "bsivr") == 0) {
        // Scripted lever on core's master VR enable (the same flag the F10
        // checkbox writes), so the sim battery can drive teardown/re-bring-up
        // without hazard injection. `status` reports the observable state -
        // session_live() - rather than echoing the flag, because the checkbox
        // is a second writer and an echo could go stale.
        if (args && strcmp(args, "on") == 0) {
            bvr::vr::set_enabled(true);
            BVR_LOG("[bsi] vr: enable requested - bring-up happens from Present "
                    "(watch for 'xr: session created')");
        } else if (args && strcmp(args, "off") == 0) {
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
