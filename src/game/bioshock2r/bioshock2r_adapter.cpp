#include "game/bioshock2r/bioshock2r_adapter.h"

#include "core/util/log.h"
#include "game/bioshock2r/camera.h"
#include "game/bioshock2r/patterns.h"

namespace bvr::b2r {

uint32_t Bioshock2RAdapter::capabilities() const {
    // CAP_FOV_WRITE since session 25: the UShockUserSettings HorizontalFOV
    // offset is runtime-verified and the write is save/restore-gated
    // (camera.cpp write block; patterns.h has the derivation).
    return camera::hook_live() ? (game::CAP_CAMERA_OVERRIDE | game::CAP_FOV_WRITE) : 0u;
}

bool Bioshock2RAdapter::init(const bvr::pattern_scan::ProcessImage& image) {
    patterns::Symbols symbols{};
    if (!patterns::resolve(image, symbols)) return false; // resolve() logged why
    camera::init_image(image); // vtable-RVA identity checks need the bounds
    if (!camera::install(symbols)) return false;
    BVR_LOG("[b2r] adapter ready, capabilities 0x%X", capabilities());
    return true;
}

void Bioshock2RAdapter::setFov(float hfovDeg) {
    camera::set_fov_override(hfovDeg); // <= 0 turns the manual write off
}

void Bioshock2RAdapter::drawDebugUi() {
    camera::draw_debug_ui();
}

bvr::game::IGameAdapter* create_adapter() {
    static Bioshock2RAdapter instance;
    return &instance;
}

} // namespace bvr::b2r
