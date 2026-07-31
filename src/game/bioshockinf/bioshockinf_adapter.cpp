#include "game/bioshockinf/bioshockinf_adapter.h"

#include "core/framework/command.h"
#include "core/hooks/d3d11_hook.h"
#include "core/util/log.h"
#include "game/bioshockinf/patterns.h"

#include <imgui.h>

#include <atomic>
#include <cstring>

namespace bvr::bsi {
namespace {

std::atomic<bool> g_loggedFovRequest{false};

void log_status() {
    const patterns::Symbols& s = patterns::symbols();
    BVR_LOG("[bsi] status: build %s (rva_trusted=%s) | image %p size 0x%zX | camera-seam probe "
            "%s | presents %llu | capabilities 0x0 (no engine hook until I2)",
            s.buildVerified ? "VERIFIED" : "NOT RECOGNISED",
            patterns::rva_trusted() ? "yes" : "no", s.imageBase, s.imageSize,
            s.viewPointReadable ? "readable" : "not read",
            static_cast<unsigned long long>(bvr::d3d11_hook::present_count()));
}

} // namespace

uint32_t BioshockInfAdapter::capabilities() const {
    // Nothing yet, and that is the honest answer: I1 installs no engine hook at
    // all. A bit here is earned by a hook observed firing (I2), never by an
    // address being derived.
    return 0;
}

bool BioshockInfAdapter::init(const bvr::pattern_scan::ProcessImage& image) {
    patterns::Symbols symbols{};
    const bool verified = patterns::resolve(image, symbols); // logs every stage

    // THE POINT OF I1. Core polls command.txt from Present, so this adapter has
    // a working command surface before it has a single engine hook - on BS1 and
    // BS2 the poller lives in the adapter and ticks off the camera hook, which
    // means no way to talk to the mod until the thing being debugged works.
    bvr::command::enable_present_pump();
    BVR_LOG("[bsi] adapter ready - command seam live on the Present thread, no engine hook "
            "required. Build gate %s.", verified ? "PASSED" : "CLOSED (features stand down)");
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
    ImGui::Text("presents: %llu   capabilities: 0x0",
                static_cast<unsigned long long>(bvr::d3d11_hook::present_count()));
    ImGui::TextDisabled("no engine hook installed - camera seam is I2 (DR-I2)");
    ImGui::TextDisabled("seam: bsi | buildgate off|on|status | vrcmd | vroverlay off");
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
    return false; // core's shared vocabulary gets a look next
}

bvr::game::IGameAdapter* create_adapter() {
    static BioshockInfAdapter instance;
    return &instance;
}

} // namespace bvr::bsi
