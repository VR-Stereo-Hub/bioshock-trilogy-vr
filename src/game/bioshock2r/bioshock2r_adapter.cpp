#include "game/bioshock2r/bioshock2r_adapter.h"

#include "core/gfx/hud_capture.h"
#include "core/util/log.h"
#include "game/bioshock2r/camera.h"
#include "game/bioshock2r/patterns.h"
#include "game/bioshock2r/scenedraw.h"

namespace bvr::b2r {

uint32_t Bioshock2RAdapter::capabilities() const {
    // CAP_FOV_WRITE since session 25: the UShockUserSettings HorizontalFOV
    // offset is runtime-verified and the write is save/restore-gated
    // (camera.cpp write block; patterns.h has the derivation).
    // CAP_SCENE_REENTRY since session 26: advertised while a reentry hook is
    // live (the double-Draw SR machinery in scenedraw.cpp).
    uint32_t caps =
        camera::hook_live() ? (game::CAP_CAMERA_OVERRIDE | game::CAP_FOV_WRITE) : 0u;
    if (scenedraw::hook_live()) caps |= game::CAP_SCENE_REENTRY;
    return caps;
}

bool Bioshock2RAdapter::init(const bvr::pattern_scan::ProcessImage& image) {
    patterns::Symbols symbols{};
    if (!patterns::resolve(image, symbols)) return false; // resolve() logged why
    camera::init_image(image); // vtable-RVA identity checks need the bounds
    scenedraw::init(image);    // RVA math for the discovery instruments
    // BS2's cb0 ray block sits at float 16, not BS1's 12 (session 32; the
    // derivation is in patterns.h next to the constant). Core defaults to
    // BS1's, so without this the live fov watch decodes nothing on BS2 -
    // which is exactly the state session 26 recorded.
    bvr::hud::set_ray_block_offset(patterns::kRayBlockCb0FloatIndex);
    if (!camera::install(symbols)) return false;
    BVR_LOG("[b2r] adapter ready, capabilities 0x%X", capabilities());
    return true;
}

void Bioshock2RAdapter::setFov(float hfovDeg) {
    camera::set_fov_override(hfovDeg); // <= 0 turns the manual write off
}

void Bioshock2RAdapter::drawDebugUi() {
    camera::draw_debug_ui();
    scenedraw::draw_debug_ui();
}

bvr::game::IGameAdapter* create_adapter() {
    static Bioshock2RAdapter instance;
    return &instance;
}

} // namespace bvr::b2r
