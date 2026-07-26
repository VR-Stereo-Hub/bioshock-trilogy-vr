#include "game/bioshock1r/bioshock1r_adapter.h"

#include "core/util/log.h"
#include "game/bioshock1r/aim.h"
#include "game/bioshock1r/bones.h"
#include "game/bioshock1r/camera.h"
#include "game/bioshock1r/hands.h"
#include "game/bioshock1r/input_drive.h"
#include "game/bioshock1r/patterns.h"
#include "game/bioshock1r/scenedraw.h"

namespace bvr::b1r {

uint32_t Bioshock1RAdapter::capabilities() const {
    uint32_t caps = camera::hook_live() ? (game::CAP_CAMERA_OVERRIDE | game::CAP_FOV_WRITE) : 0u;
    if (scenedraw::hook_live()) caps |= game::CAP_SCENE_REENTRY;
    if (aim::hook_live()) caps |= game::CAP_AIM_OVERRIDE;
    return caps;
}

bool Bioshock1RAdapter::init(const bvr::pattern_scan::ProcessImage& image) {
    patterns::Symbols symbols{};
    if (!patterns::resolve(image, symbols)) return false; // resolve() logged why
    if (!camera::install(symbols.eventPlayerCalcView)) return false;
    scenedraw::init(image); // stashes the image only - hooks are command-gated
    aim::init(image, symbols); // same: M6 seam hooks are command-gated
    hands::init(image);        // M7 viewmodel; the actor is located lazily
    bones::init(image);        // M7-v2 skeleton drive; located lazily off the actor
    BVR_LOG("[b1r] adapter ready, capabilities 0x%X", capabilities());
    return true;
}

void Bioshock1RAdapter::setFov(float hfovDeg) {
    camera::set_fov_override(hfovDeg);
}

void Bioshock1RAdapter::drawDebugUi() {
    camera::draw_debug_ui();
    aim::draw_debug_ui();
    hands::draw_debug_ui();
    input_drive::draw_debug_ui();
    scenedraw::draw_debug_ui();
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
