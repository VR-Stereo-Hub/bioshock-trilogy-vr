#include "game/bioshock1r/bioshock1r_adapter.h"

#include "core/util/log.h"
#include "game/bioshock1r/patterns.h"

#include <imgui.h>

namespace bvr::b1r {

uint32_t Bioshock1RAdapter::capabilities() const {
    return 0u; // camera/FOV caps land with the calcview hook
}

bool Bioshock1RAdapter::init(const bvr::pattern_scan::ProcessImage& image) {
    patterns::Symbols symbols{};
    if (!patterns::resolve(image, symbols)) return false; // resolve() logged why
    return true;
}

void Bioshock1RAdapter::setFov(float) {}

void Bioshock1RAdapter::drawDebugUi() {
    ImGui::Text("bioshock1r: scan-only build");
}

} // namespace bvr::b1r

namespace bvr::game {

// Single-game registry for now; multi-game dispatch (bioshock2r) moves this
// out into its own module when the second adapter lands.
namespace {
IGameAdapter* g_adapter = nullptr;
}

void init_adapter() {
    static b1r::Bioshock1RAdapter instance;

    pattern_scan::ProcessImage image{};
    if (!pattern_scan::capture_main_module(image)) {
        BVR_LOG("[b1r] capture_main_module failed - camera features disabled, game runs flat");
    } else {
        instance.init(image); // fail-soft: failure paths log internally
    }
    // Published even on failure so the overlay can show "scan FAILED".
    // init_adapter() runs on the init thread before the D3D11 hooks install,
    // so the overlay cannot observe a half-initialized adapter.
    g_adapter = &instance;
}

IGameAdapter* adapter() {
    return g_adapter;
}

} // namespace bvr::game
