#include "game/bioshock2r/bioshock2r_adapter.h"

#include "core/util/log.h"
#include "game/bioshock2r/camera.h"
#include "game/bioshock2r/patterns.h"

namespace bvr::b2r {

uint32_t Bioshock2RAdapter::capabilities() const {
    // No CAP_FOV_WRITE: BS2 has no runtime-verified FOV source yet (the
    // UShockUserSettings candidate is unconsumed at M3).
    return camera::hook_live() ? game::CAP_CAMERA_OVERRIDE : 0u;
}

bool Bioshock2RAdapter::init(const bvr::pattern_scan::ProcessImage& image) {
    patterns::Symbols symbols{};
    if (!patterns::resolve(image, symbols)) return false; // resolve() logged why
    camera::init_image(image); // vtable-RVA identity checks need the bounds
    if (!camera::install(symbols)) return false;
    BVR_LOG("[b2r] adapter ready, capabilities 0x%X", capabilities());
    return true;
}

void Bioshock2RAdapter::setFov(float) {
    // No FOV lever on this game yet - see capabilities().
}

void Bioshock2RAdapter::drawDebugUi() {
    camera::draw_debug_ui();
}

bvr::game::IGameAdapter* create_adapter() {
    static Bioshock2RAdapter instance;
    return &instance;
}

} // namespace bvr::b2r
