#include "game/bioshock2r/bioshock2r_adapter.h"

#include "core/gfx/hud_capture.h"
#include "core/util/log.h"
#include "core/vr/openxr_runtime.h"
#include "game/bioshock2r/aim.h"
#include "game/bioshock2r/camera.h"
#include "game/bioshock2r/hands.h"
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
    // CAP_AIM_OVERRIDE since session 39: both GetPerfectFireStart impls
    // hooked (identity-gated in patterns::resolve_gpfs_impls).
    if (aim::hook_live()) caps |= game::CAP_AIM_OVERRIDE;
    // CAP_HANDS_ATTACH since session 39: the bone drive is armed (the rig
    // itself resolves lazily at the first gameplay frame).
    if (hands::enabled()) caps |= game::CAP_HANDS_ATTACH;
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
    // Session 42: BS2 composites gameswf directly on the BACKBUFFER (bind =
    // RENDER_TARGET only) and tonemaps via an INDEXED 6-index quad sampling
    // the scene-vote leader (framedump_232940 event 934 - derivation in
    // docs/bioshock2/ENGINE_NOTES.md session 42). BS1's fingerprints never
    // fire on that shape, so the HUD classifier needs the opt-in; core
    // default stays off (BS1 bit-identical).
    bvr::hud::set_backbuffer_composite(true);
    // Session 42 (user decision): harvest the first cutscene's fingerprint
    // automatically - BS2's letterbox-bar vertex count is still underived
    // (never copy BS1's 29), and the transition is over before any hand-armed
    // dump could land. One-shot, self-disarms; ~0.5 s hitch once per run.
    bvr::hud::set_dump_on_edge(1 /* bar-draw rising */, 2);
    // Session 42 deviation from BS1 (ARCHITECTURE decision log): BS1 falls
    // back to the SIZE-ONLY post-FX rule during cutscenes (its cine shots
    // carry no HUD art, and the bind test cost a visible floating screen).
    // BS2 renders a SQUARE-ish backbuffer where size-only is maximally
    // degenerate (it matches the UI atlases) - ship the fallback OFF and
    // enable it only if a headset session actually shows the artifact.
    bvr::hud::set_postfx_cine_size(false);
    // Session 34 armed detached pacing here (hand the frame loop to the pace
    // thread whenever the session is not FOCUSED) against the ~10 Hz unfocused
    // cadence. Session 36, first real VDXR attach since: DETACH STRANDS THE
    // HEADSET after any focus loss - empty keepalive frames are exactly what
    // VDXR refuses to re-promote (state parked VISIBLE, shouldRender=0,
    // forever), and the user hit it on the first double-tap. With the wait
    // off-thread the frame loop while unfocused is cheap (lastEnd ~1 ms
    // measured at VISIBLE), and full submission is what lets VD re-grant
    // FOCUSED by itself - so the default is now OFF and the session
    // SELF-HEALS on refocus. `vrpace detach on` remains the live A/B; the
    // real fix (keepalives that carry layers) is queued in STATUS. Core
    // still ships the lever OFF, so BS1 is untouched either way.
    bvr::vr::set_pace_detach(false);
    if (!camera::install(symbols)) return false;
    // Session 39: the fire-chain dispatch probe (fail-soft per name; the seam
    // itself lands after the probe's live verdict). Needs the installed
    // FindFunctionChecked/ProcessEvent detours, so after camera::install.
    aim::init(image, symbols);
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
