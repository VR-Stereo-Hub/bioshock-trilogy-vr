#include "game/bioshock1r/bioshock1r_adapter.h"

#include "core/util/log.h"
#include "game/bioshock1r/aim.h"
#include "game/bioshock1r/body.h"
#include "game/bioshock1r/bones.h"
#include "game/bioshock1r/camera.h"
#include "game/bioshock1r/hands.h"
#include "game/bioshock1r/input_drive.h"
#include "game/bioshock1r/patterns.h"
#include "game/bioshock1r/scenedraw.h"
#include <cstring>

#include "game/bioshock1r/screens.h"

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
    body::init(image);         // M7.5 body yaw transfer; default off, probe-gated
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
    body::draw_debug_ui();
    input_drive::draw_debug_ui();
    scenedraw::draw_debug_ui();
}

bool Bioshock1RAdapter::handleCommand(const char* cmd, const char* args) {
    if (strcmp(cmd, "vrscreens") == 0) {
        screens::handle_command(args);
        return true;
    }
    return false;
}

bvr::game::IGameAdapter* create_adapter() {
    static Bioshock1RAdapter instance;
    return &instance;
}

} // namespace bvr::b1r
