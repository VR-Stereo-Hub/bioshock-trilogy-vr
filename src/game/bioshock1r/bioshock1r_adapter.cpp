#include "game/bioshock1r/bioshock1r_adapter.h"

#include "core/util/log.h"
#include "core/vr/openxr_runtime.h"
#include "game/bioshock1r/aim.h"
#include "game/bioshock1r/body.h"
#include "game/bioshock1r/bones.h"
#include "game/bioshock1r/camera.h"
#include "game/bioshock1r/hands.h"
#include "game/bioshock1r/input_drive.h"
#include "game/bioshock1r/patterns.h"
#include "game/bioshock1r/scenedraw.h"
#include <cstring>

#include "core/input/xinput_bridge.h"
#include "game/bioshock1r/probe_bob.h"
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
    // Anchored screen placement is a CORE behaviour and this is where BS1 -
    // and only BS1 - asks for it. It is headset-verified here and nowhere
    // else; BS2 and Infinite keep the recenter-origin placement they shipped
    // with until somebody tests them, and then they opt in on this same line.
    // A saved VR preset can still override it afterwards (vrpreset load).
    bvr::vr::set_screen_place_mode(0);
    // User ask (2026-08-22): the anchored menus read too large. BS1 only - the
    // core default stays 2.4 for BS2 and Infinite, neither of which has been in
    // a headset with anchored placement at all. The F10 slider and
    // `vrscreen width` still move it, and a saved preset still overrides.
    bvr::vr::set_screen_width_m(1.9f);
    // Face buttons pass straight through by default (user directive,
    // 2026-08-22): A=use, B=med hypo, X=reload/hack/EVE, Y=jump, which is the
    // game's own pad layout and the BRVR mod's stock semantics. The session-19
    // rearrangement (Touch B=jump, Touch Y=first aid) is still one line away -
    // `profile = session19` in controls.ini - and is what BioShock 2 keeps,
    // since it shares the profile and has never been in a headset with this.
    bvr::input::set_pad_passthrough_default(true);
    // ...and the BRVR mod's control defaults with it (user directive): d-pad
    // modifier on the RIGHT thumbrest with the LEFT stick selecting, which is
    // where a d-pad lives on a real pad, and R3 jumping since zoom is already
    // unreachable here. BRVR ships ControllerDpadModifier=1, ControllerDpadFlip=0,
    // JumpOnR3=1. BioShock 2 shares this pad profile and keeps the older
    // heuristic until somebody puts a headset on for it.
    bvr::input::set_pad_brvr_defaults(true);
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
    if (strcmp(cmd, "vrprobe") == 0) {
        const char* a = args;
        while (*a == ' ') ++a;
        if (strncmp(a, "bob", 3) == 0) {
            probe_bob::handle_command(a + 3);
            return true;
        }
        return false;
    }
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
