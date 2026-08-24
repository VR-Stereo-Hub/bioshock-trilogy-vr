#include "game/bioshock1r/bioshock1r_adapter.h"

#include "core/gfx/hud_capture.h"
#include "core/ui/overlay.h"
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
#include "game/bioshock1r/scripted.h"
#include <cstdlib>  // _wtof, for the ini's float keys
#include <cstring>

#include <windows.h>

#include "core/input/xinput_bridge.h"
#include "game/bioshock1r/game_ini.h"
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

    // ---- THE BS1 OPT-IN BLOCK. COPY IT WHOLE, DO NOT RE-DERIVE IT. ---------
    //
    // Added in the PR-50 review pass (VOID, 2026-08-23). Each line below turns
    // on an s63 control change that lives in src/core/ and therefore reaches
    // BioShock 1, BioShock 2 and Infinite at once. Only BioShock 1 has been in
    // a headset with any of them, so core ships the PRE-s63 behaviour and this
    // is the single place that asks for the new one.
    //
    // WHEN BS2 OR INFINITE ARE TESTED: paste this block into that adapter's
    // init() unchanged. Nothing here is BS1-specific in its VALUES - it is
    // BS1-specific only in having been verified. Once all three are in, flip
    // the core defaults and delete the block from all three adapters.
    //
    // What each one is, and what off means, is on the declarations in
    // core/input/xinput_bridge.h and core/ui/overlay.h. The scene-leader latch
    // just below is the same pattern and travels with this block.
    bvr::input::set_flick_fourth_direction(true);   // right flick + the held hint bit
    bvr::input::set_menu_modifier_context_help(true); // modifier + menu -> BACK, held
    bvr::input::set_chord_tap_opens_panel(true);    // both-sticks TAP opens F10
    bvr::input::set_flick_press_threshold(0.5f);    // was 0.65 with no dominance test
    bvr::overlay::set_pad_drive(true);              // ray as cursor, RT as click

    // Walking into a wall must not look like the pause menu (s65). The world
    // pass is identified by a DSV-bound draw COUNT >= 32, and a view filled by
    // one near surface draws less than that - so screen_only() tripped, core's
    // wantCine dropped the projection to the M2 quad, and the player got an
    // anchored square with a frozen hand, toggling as they faced the wall.
    // Measured 2026-08-23: 19 transitions in one run, "swf draws 145" identical
    // on both sides, so the count was the whole verdict.
    //
    // The latch keeps calling a target the world while it still draws anything,
    // once it has been confidently identified as one. BS1 ONLY: BS2 and
    // Infinite keep the count-only rule until somebody puts a headset on for
    // them, and then they opt in on this same line.
    bvr::hud::set_scene_leader_latch(true);

    // ---- BioshockVR.ini [VR] keys this adapter owns -------------------------
    // PRECEDENCE, and it is not the obvious one: init() runs BEFORE
    // arm_vr_preset() -> load_vr_preset_values(), so a saved vrpreset.ini
    // OVERRIDES everything read here. That is the existing "tuned sliders over
    // defaults" contract and it is deliberate - the F10 save is how an
    // in-headset calibration is kept. It is also easy to mistake for the ini
    // being ignored, which is why the echo below says so out loud. (Making the
    // ini the single source of truth is blocked on the F10 writer, which
    // rewrites vrpreset.ini wholesale and drops unknown keys - docs/CONTROLS.md
    // names it as the blocker. Do not try to solve it from here.)
    {
        wchar_t ini[MAX_PATH];
        swprintf_s(ini, L"%s\\BioshockVR.ini", bvr::log::data_dir());
        // GameTurnSpeed: the game's own sensitivity slider, the other half of
        // the TurnAxisMax cap. -1 (the default) leaves the player's own choice
        // alone; BRVR ships 70, which is "7" in the options menu.
        const int turn = GetPrivateProfileIntW(L"VR", L"GameTurnSpeed", -1, ini);
        if (turn >= 0) game_ini::write_turn_sensitivity(turn);

        // Floats go through the string reader - GetPrivateProfileInt cannot do
        // decimals, and 0.80 is the whole point. Same idiom as TurnAxisMax in
        // core's xr-input reader. An out-of-range value is REFUSED and logged
        // loudly rather than clamped: a silent clamp reads in the headset
        // exactly like the key not being implemented at all.
        wchar_t wv[24] = {};

        // HandsScale / GunScale: BRVR's key names and BRVR's 0.8 defaults, so a
        // config carries between the two mods. Both also live on `vrhands
        // scale` / `vrhands wscale` and on the F10 sliders, which is how the
        // number gets found in a headset without a rebuild per guess.
        GetPrivateProfileStringW(L"VR", L"HandsScale", L"", wv, 24, ini);
        if (wv[0]) {
            const float v = static_cast<float>(_wtof(wv));
            if (v >= 0.05f && v <= 5.0f) bones::set_scale(-1, v);
            else BVR_LOG("[b1r] HandsScale must be between 0.05 and 5.0 - ignoring");
        }
        GetPrivateProfileStringW(L"VR", L"GunScale", L"", wv, 24, ini);
        if (wv[0]) {
            const float v = static_cast<float>(_wtof(wv));
            if (v >= 0.05f && v <= 5.0f) bones::set_weapon_scale(v);
            else BVR_LOG("[b1r] GunScale must be between 0.05 and 5.0 - ignoring");
        }
        // CameraHeightOffset: cm above the pawn's own eye point, + up. BRVR's
        // key, name and units; it ships 9 and so do we. Stored as UU.
        GetPrivateProfileStringW(L"VR", L"CameraHeightOffset", L"", wv, 24, ini);
        if (wv[0]) {
            const float v = static_cast<float>(_wtof(wv));
            if (v >= -100.0f && v <= 100.0f) camera::set_head_up_cm(v);
            else BVR_LOG("[b1r] CameraHeightOffset must be -100 to 100 cm - ignoring");
        }

        BVR_LOG("[b1r] ini: HandsScale %.3f | GunScale %.3f | CameraHeightOffset "
                "%.1f UU (a saved vrpreset.ini loads LATER and overrides all three)",
                bones::scale(1), bones::weapon_scale(), camera::head_up_uu());
    }
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
    scripted::draw_debug_ui();
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
