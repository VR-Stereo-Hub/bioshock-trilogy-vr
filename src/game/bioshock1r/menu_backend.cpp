#include "menu_backend.h"
#include "menu_bindings.h"
#include "menu_preferences.h"
#include "menu_theme.h"
#include "aim.h"
#include "body.h"
#include "bones.h"
#include "camera.h"
#include "game_ini.h"
#include "hands.h"
#include "scripted.h"
#include "core/gfx/frame_inspector.h"
#include "core/gfx/hud_capture.h"
#include "core/input/swing.h"
#include "core/input/xinput_bridge.h"
#include "core/util/log.h"
#include "core/vr/openxr_runtime.h"
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

namespace bvr::b1r::menu {
namespace {
using S=Setting;
std::atomic<float> g_textSize{1};
std::atomic<int> g_level{0};
// Only the game thread owns the preference map and profile transitions. The
// render thread submits values, never pointers to engine objects.
Preferences g_preferences;
bool g_loaded=false;
std::mutex g_mutex;
std::string g_profile;
int g_profileHand=-1;
std::string g_notice;
struct Edit { Setting id; int hand; float value; bool commit; std::string profile; };
std::vector<Edit> g_edits;
std::vector<Action> g_actions;

float raw_read(Setting id,int hand) {
    hand=hand==0?0:1;
    if(id==S::TextSize) return g_textSize.load(std::memory_order_relaxed);
    if(id==S::ViewLevel) return static_cast<float>(g_level.load(std::memory_order_relaxed));
    float value=camera::menu_read(id);if(std::isfinite(value)) return value;
    value=aim::menu_read(id);if(std::isfinite(value)) return value;
    value=hands::menu_read(id);if(std::isfinite(value)) return value;
    value=bones::menu_read(id,hand);if(std::isfinite(value)) return value;
    float a=0,b=0,c=0;
    switch(id) {
#if __has_include("hand_compose.h")
        // Current mode-4 API (hand branch f318f3d or newer).
        case S::HandsArmsSize: return bones::m4_arm_size();
        case S::ShouldersLinked: return bones::m4_shoulders_linked();
        case S::ShoulderBarForward: case S::ShoulderBarRight: case S::ShoulderBarUp: case S::ShoulderWidth: {
            float width; bones::m4_shoulders(&a,&b,&c,&width);
            return id==S::ShoulderBarForward?a:id==S::ShoulderBarRight?b:id==S::ShoulderBarUp?c:width;
        }
#endif
        case S::HandSize: return bones::scale(hand);
        case S::WeaponSize: return bones::weapon_scale();
        case S::PlaceForward: case S::PlaceRight: case S::PlaceUp:
            hands::view_offset_cm(hand,&a,&b,&c);return id==S::PlaceForward?a:id==S::PlaceRight?b:c;
        case S::GripForward: case S::GripRight: case S::GripUp:
            hands::model_offset_cm(hand,&a,&b,&c);return id==S::GripForward?a:id==S::GripRight?b:c;
        case S::ModelPitch: return hands::model_trim_pitch_deg(hand);
        case S::ModelYaw: return hands::model_trim_yaw_deg(hand);
        case S::ModelRoll: return hands::model_trim_roll_deg(hand);
        case S::ShoulderForward: case S::ShoulderRight: case S::ShoulderUp:
            bones::shoulder_cm(hand,&a,&b,&c);return id==S::ShoulderForward?a:id==S::ShoulderRight?b:c;
        case S::ArmLength: return bones::arm_scale();
        case S::ElbowOut: return bones::elbow_out();
        case S::ArmTwist: return bones::arm_twist_limit_deg();
        case S::HumerusTwist: return bones::humerus_twist_limit_deg();
        case S::WristFollow: return bones::elbow_follow_wrist();
        case S::ElbowSmoothing: return static_cast<float>(bones::elbow_smooth_ms());
        case S::AnimationAllowed: return bones::anim_allowed();
        case S::AimPitch: return aim::trim_pitch_deg(hand);
        case S::AimYaw: return aim::trim_yaw_deg(hand);
        case S::AimForward: return aim::pos_fwd_cm(hand);
        case S::AimRight: return aim::pos_right_cm(hand);
        case S::AimUp: return aim::pos_up_cm(hand);
        case S::SnapTurn: return input::snap_turn();
        case S::SnapAngle: return input::snap_angle_deg();
        case S::TurnSpeed: return input::turn_scale();
        case S::StickDeadzone: return input::stick_deadzone();
        case S::AmmoModifier: return static_cast<float>(input::dpad_modifier());
        case S::AmmoStick: return input::dpad_select_left();
        case S::R3Jump: return input::jump_on_r3();
        case S::Swing: return input::swing::enabled();
        case S::SwingSpeed: return input::swing::threshold_ms();
        case S::SwingCooldown: return static_cast<float>(input::swing::cooldown_ms());
        case S::SwingDelay: return static_cast<float>(input::swing::delay_ms());
        case S::BodyFollow: return body::enabled();
        case S::BodyRate: return body::rate_per_sec();
        case S::BodyDeadzone: return body::deadzone_deg();
        case S::CineDrive: return static_cast<float>(vr::cine_drive());
        case S::HideBars: return hud::bars_hidden();
        case S::SceneTurning: return scripted::scripted_turn();
        case S::SceneTurnRate: return scripted::scripted_turn_deg_per_sec();
        case S::SceneRecentre: return static_cast<float>(scripted::scripted_recentre_mode());
        case S::RotationFollow: return static_cast<float>(scripted::rot_follow());
        case S::HideSceneReticle: return scripted::hide_aim_in_scenes();
        case S::HudPanel: return hud::enabled();
        case S::HudDistance: case S::HudWidth: case S::HudHeight:
            vr::get_hud_quad(&a,&b,&c);return id==S::HudDistance?a:id==S::HudWidth?b:c;
        case S::ScreenPlacement: return static_cast<float>(vr::screen_place_mode());
        case S::ScreenHeight: return vr::screen_height_m();
        case S::ScreenWidth: return vr::screen_width_m();
        case S::ScreenDistance: return vr::screen_distance_m();
        case S::CutsceneDisplay: return vr::cinematic_stereo();
        case S::InputEnabled: return input::enabled();
        case S::CameraEnabled: return vr::vr_camera_mode();
        case S::WalkProbe: return body::walk_probe_on();
        case S::MirrorWalk: return body::move_yaw_sign()<0;
        case S::InstantWalk: return body::move_dir_instant();
        case S::SceneDetection: return scripted::enabled();
        case S::WorldFovGuard: return scripted::world_fov_guard();
        case S::SceneHold: return static_cast<float>(scripted::hold_ms());
        case S::SubtitleInFrame: return hud::cine_subs_in_frame();
        case S::EffectsInFrame: return hud::effects_in_frame();
        case S::EffectVertexLimit: return static_cast<float>(hud::effect_max_verts());
        case S::EffectRtOnly: return hud::postfx_rt_only();
        case S::HideSceneArms: return scripted::hide_rig_in_scenes();
        case S::GameSceneHands: return scripted::freeze_hands_in_scenes();
        default: return std::numeric_limits<float>::quiet_NaN();
    }
}
bool raw_write(Setting id,int hand,float value) {
    if(!valid_value(spec(id),value)) return false;
    hand=hand==0?0:1;
    if(id==S::TextSize) {g_textSize.store(value,std::memory_order_relaxed);return true;}
    if(id==S::ViewLevel) {g_level.store(static_cast<int>(value),std::memory_order_relaxed);return true;}
    if(camera::menu_write(id,value)||aim::menu_write(id,value)||hands::menu_write(id,value)||bones::menu_write(id,hand,value)) return true;
    float a=0,b=0,c=0;
    switch(id) {
#if __has_include("hand_compose.h")
        case S::HandsArmsSize: bones::set_m4_arm_size(value);break;
        case S::ShouldersLinked: bones::set_m4_shoulders_linked(value!=0);break;
        case S::ShoulderBarForward: case S::ShoulderBarRight: case S::ShoulderBarUp: case S::ShoulderWidth: {
            float width; bones::m4_shoulders(&a,&b,&c,&width);
            bones::set_m4_shoulders(id==S::ShoulderBarForward?value:a,id==S::ShoulderBarRight?value:b,
                id==S::ShoulderBarUp?value:c,id==S::ShoulderWidth?value:width);break;
        }
#endif
        case S::HandSize: bones::set_scale(hand,value);break;
        case S::WeaponSize: bones::set_weapon_scale(value);break;
        case S::PlaceForward: case S::PlaceRight: case S::PlaceUp:
            hands::view_offset_cm(hand,&a,&b,&c);
            hands::set_view_offset_cm(hand,id==S::PlaceForward?value:a,id==S::PlaceRight?value:b,id==S::PlaceUp?value:c);break;
        case S::GripForward: case S::GripRight: case S::GripUp:
            hands::model_offset_cm(hand,&a,&b,&c);
            hands::set_model_offset_cm(hand,id==S::GripForward?value:a,id==S::GripRight?value:b,id==S::GripUp?value:c);break;
        case S::ModelPitch: case S::ModelYaw: case S::ModelRoll:
            hands::set_model_trim_deg(hand,id==S::ModelPitch?value:hands::model_trim_pitch_deg(hand),
                id==S::ModelYaw?value:hands::model_trim_yaw_deg(hand),id==S::ModelRoll?value:hands::model_trim_roll_deg(hand));break;
        case S::ShoulderForward: case S::ShoulderRight: case S::ShoulderUp:
            bones::shoulder_cm(hand,&a,&b,&c);
            bones::set_shoulder_cm(hand,id==S::ShoulderForward?value:a,id==S::ShoulderRight?value:b,id==S::ShoulderUp?value:c);break;
        case S::ArmLength: bones::set_arm_scale(value);break;
        case S::ElbowOut: bones::set_elbow_out(value);break;
        case S::ArmTwist: bones::set_arm_twist_limit_deg(value);break;
        case S::HumerusTwist: bones::set_humerus_twist_limit_deg(value);break;
        case S::WristFollow: bones::set_elbow_follow_wrist(value);break;
        case S::ElbowSmoothing: bones::set_elbow_smooth_ms(static_cast<unsigned>(value));break;
        case S::AnimationAllowed: bones::set_anim_allowed(value!=0);break;
        case S::AimPitch: aim::set_trim(hand,value,aim::trim_yaw_deg(hand));break;
        case S::AimYaw: aim::set_trim(hand,aim::trim_pitch_deg(hand),value);break;
        case S::AimForward: case S::AimRight: case S::AimUp:
            aim::set_pos_offset(hand,id==S::AimForward?value:aim::pos_fwd_cm(hand),
                id==S::AimRight?value:aim::pos_right_cm(hand),id==S::AimUp?value:aim::pos_up_cm(hand));break;
        case S::SnapTurn: input::set_snap_turn(value!=0);break;
        case S::SnapAngle: input::set_snap_angle_deg(value);break;
        case S::TurnSpeed: input::set_turn_scale(value);break;
        case S::StickDeadzone: input::set_stick_deadzone(value);break;
        case S::AmmoModifier: input::set_dpad_modifier(static_cast<int>(value));break;
        case S::AmmoStick: input::set_dpad_select_left(value!=0);break;
        case S::R3Jump: input::set_jump_on_r3(value!=0);break;
        case S::Swing: input::swing::set_enabled(value!=0);break;
        case S::SwingSpeed: input::swing::set_threshold_ms(value);break;
        case S::SwingCooldown: input::swing::set_cooldown_ms(static_cast<uint32_t>(value));break;
        case S::SwingDelay: input::swing::set_delay_ms(static_cast<uint32_t>(value));break;
        case S::BodyFollow: body::handle_command(value!=0?"on":"off");break;
        case S::BodyRate: body::set_tuning(value,body::deadzone_deg());break;
        case S::BodyDeadzone: body::set_tuning(body::rate_per_sec(),value);break;
        case S::CineDrive: vr::set_cine_drive(static_cast<vr::CineDrive>(static_cast<int>(value)));break;
        case S::HideBars: hud::set_bars_hidden(value!=0);break;
        case S::SceneTurning: scripted::set_scripted_turn(value!=0);break;
        case S::SceneTurnRate: scripted::set_scripted_turn_deg_per_sec(value);break;
        case S::SceneRecentre: scripted::set_scripted_recentre_mode(static_cast<int>(value));break;
        case S::RotationFollow: scripted::set_rot_follow(static_cast<scripted::RotFollow>(static_cast<int>(value)));break;
        case S::HideSceneReticle: scripted::set_hide_aim_in_scenes(value!=0);break;
        case S::HudPanel: hud::set_enabled(value!=0);break;
        case S::HudDistance: case S::HudWidth: case S::HudHeight:
            vr::get_hud_quad(&a,&b,&c);vr::set_hud_quad(id==S::HudDistance?value:a,id==S::HudWidth?value:b,id==S::HudHeight?value:c);break;
        case S::ScreenPlacement: vr::set_screen_place_mode(static_cast<int>(value));break;
        case S::ScreenHeight: vr::set_screen_height_m(value);break;
        case S::ScreenWidth: vr::set_screen_width_m(value);break;
        case S::ScreenDistance: vr::set_screen_distance_m(value);break;
        case S::CutsceneDisplay: vr::set_cinematic_stereo(value!=0);break;
        case S::InputEnabled: input::set_enabled(value!=0);break;
        case S::CameraEnabled: vr::set_camera_mode(value!=0);break;
        case S::WalkProbe: body::set_walk_probe(value!=0);break;
        case S::MirrorWalk: body::set_move_yaw_sign(value!=0?-1:1);break;
        case S::InstantWalk: body::set_move_dir_instant(value!=0);break;
        case S::SceneDetection: scripted::set_enabled(value!=0);break;
        case S::WorldFovGuard: scripted::set_world_fov_guard(value!=0);break;
        case S::SceneHold: scripted::set_hold_ms(static_cast<int>(value));break;
        case S::SubtitleInFrame: hud::set_cine_subs_in_frame(value!=0);break;
        case S::EffectsInFrame: hud::set_effects_in_frame(value!=0);break;
        case S::EffectVertexLimit: hud::set_effect_max_verts(static_cast<unsigned>(value));break;
        case S::EffectRtOnly: hud::set_postfx_rt_only(value!=0);break;
        case S::HideSceneArms: scripted::set_hide_rig_in_scenes(value!=0);break;
        case S::GameSceneHands: scripted::set_freeze_hands_in_scenes(value!=0);break;
        default: return false;
    }
    return true;
}
void notice(const char* text) {std::lock_guard<std::mutex> lock(g_mutex);g_notice=text;}
std::wstring preference_path() {return std::wstring(log::data_dir())+L"\\menu-settings.ini";}
bool save_preferences() {
    const auto path=preference_path(),temporary=path+L".tmp",backup=path+L".bak";
    const auto text=g_preferences.serialize();
    FILE* file=nullptr;
    if(_wfopen_s(&file,temporary.c_str(),L"wb") || !file) {
        notice("Settings could not be saved. Check folder permissions.");return false;
    }
    const bool written=fwrite(text.data(),1,text.size(),file)==text.size();
    const bool closed=fclose(file)==0;
    if(!written || !closed) {notice("Settings could not be saved; the previous file is intact.");return false;}
    if(GetFileAttributesW(path.c_str())!=INVALID_FILE_ATTRIBUTES && !CopyFileW(path.c_str(),backup.c_str(),FALSE)) {
        notice("Settings backup failed; the previous file is intact.");return false;
    }
    if(!MoveFileExW(temporary.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)) {
        notice("Settings could not be saved; the previous file is intact.");return false;
    }
    BVR_LOG("[b1r] F10 preferences saved (%zu entries)",g_preferences.entries().size());
    notice("Settings saved. F10 closes.");return true;
}
class LiveBackend final : public Backend {
public:
    float read(Setting id,int hand) const override {
        // A paused game may not drain the game-thread queue this frame. Keep
        // showing the pending edit, including on release, so the old module
        // value cannot overwrite the final slider sample before it is saved.
        std::lock_guard<std::mutex> lock(g_mutex);
        const auto scope=spec(id).scope;
        for(auto i=g_edits.rbegin();i!=g_edits.rend();++i)
            if(i->id==id && (scope==Scope::Global || i->hand==hand) &&
                (scope!=Scope::Profile || (i->profile==g_profile && i->hand==g_profileHand))) return i->value;
        return raw_read(id,hand);
    }
    void edit(Setting id,int hand,float value,bool commit) override {
        if(!valid_value(spec(id),value)) return;
        // Presentation responds immediately even when gameplay is paused.
        if(id==S::TextSize) g_textSize.store(value,std::memory_order_relaxed);
        if(id==S::ViewLevel) g_level.store(static_cast<int>(value),std::memory_order_relaxed);
        std::lock_guard<std::mutex> lock(g_mutex);
        const auto profile=spec(id).scope==Scope::Profile?g_profile:std::string();
        if(spec(id).scope==Scope::Profile && (profile.empty() || g_profileHand!=hand)) {
            g_notice="Equip an item in the selected hand before tuning its profile.";return;
        }
        // Coalesce drag samples without losing a release/save event.
        for(auto& edit:g_edits) if(edit.id==id && edit.hand==hand && edit.profile==profile) {
            edit.value=value;edit.commit|=commit;return;
        }
        g_edits.push_back({id,hand,value,commit,profile});
        g_notice=is_player_setting(id)?"Changes pending game update...":"Debug changes are session-only.";
    }
    Context context() const override {
        Context c;
        c.handMode=static_cast<int>(hands::menu_read(S::HandMode));c.maxHandMode=hands::menu_max_mode();
        c.solverV2=bones::menu_read(S::ArmSolver,0)!=0;c.hasSolverChoice=bones::menu_has_solver_choice();
        c.cameraHook=camera::hook_live();c.engineReady=c.cameraHook;
        vr::HeadPose pose{};c.tracking=vr::peek_head_pose(pose);
        std::lock_guard<std::mutex> lock(g_mutex);
        c.profile=g_profile;c.profileHand=g_profileHand;c.notice=g_notice;
        return c;
    }
    void action(Action action) override {std::lock_guard<std::mutex> lock(g_mutex);g_actions.push_back(action);}
    void diagnostics(Tab tab) override {
        if(tab==Tab::Display && theme::section("Resolution (render size)")) {
            unsigned width=0,height=0;hud::backbuffer_dims(&width,&height);
            ImGui::Text("Current render: %u x %u",width,height);
            theme::note("Resolution changes need a restart. In-game config writes are unavailable because the game can overwrite them when it exits.");
        }
        if(tab==Tab::Runtime && theme::section("Runtime status")) {
            ImGui::Text("%.1f fps  /  %.2f ms",ImGui::GetIO().Framerate,1000.f/std::max(ImGui::GetIO().Framerate,1.f));
            ImGui::Text("OpenXR session: %s",vr::session_live()?"active":"unavailable");
            ImGui::Text("Camera hook: %s",camera::hook_live()?"ready":"unavailable");
            theme::note("F10 settings are written to menu-settings.ini in the mod's data folder.");
        }
        if(tab==Tab::Diagnostics && theme::section("Frame capture")) {
            if(theme::button("Capture frame")) frame_inspector::arm(1);
            ImGui::SameLine();if(theme::button("Capture with buffers")) frame_inspector::arm(2);
            theme::note("Captures are written beside the mod log.");
        }
        if((tab==Tab::Runtime||tab==Tab::Diagnostics) && theme::section("Settings storage")) {
            if(theme::button("Retry saving")) action(Action::RetrySave);
            theme::note("Player preferences override legacy ini values. Hand alignment is saved for the equipped profile. Debug choices are not autosaved.");
        }
    }
};
LiveBackend g_backend;
}
Backend& live_backend() {return g_backend;}
void profile_changed(const char* key,int hand) {
    const std::string profile=key?key:"";
    {
        std::lock_guard<std::mutex> lock(g_mutex);g_profile=profile;g_profileHand=profile.empty()?-1:hand;
    }
    if(!g_loaded || profile.empty()) return;
    for(const auto& [name,p]:g_preferences.entries())
        if(p.profile==profile && p.hand==hand) raw_write(p.id,p.hand,p.value);
}
void reapply_preferences() {
    if(!g_loaded) return;
    // Centimetre-valued offsets must be converted using the saved world scale.
    for(const auto& [name,p]:g_preferences.entries()) if(p.id==S::WorldScale) raw_write(p.id,p.hand,p.value);
    for(const auto& [name,p]:g_preferences.entries())
        if(p.profile.empty() && p.id!=S::WorldScale) raw_write(p.id,p.hand,p.value);
    std::string profile;int hand;
    {std::lock_guard<std::mutex> lock(g_mutex);profile=g_profile;hand=g_profileHand;}
    if(!profile.empty()) profile_changed(profile.c_str(),hand);
}
void load_preferences() {
    if(g_loaded) {reapply_preferences();return;}
    g_loaded=true;
    FILE* file=nullptr;
    if(_wfopen_s(&file,preference_path().c_str(),L"rb") || !file) return;
    std::string contents;char chunk[4096];size_t count;
    while((count=fread(chunk,1,sizeof chunk,file))!=0 && contents.size()<1024*1024) contents.append(chunk,count);
    fclose(file);
    const unsigned rejected=g_preferences.parse(contents);
    if(rejected) BVR_LOG("[b1r] F10 preferences ignored %u invalid/unknown entries",rejected);
    reapply_preferences();
}
void tick() {
    std::vector<Edit> edits;std::vector<Action> actions;std::string profile;int hand;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        edits.swap(g_edits);actions.swap(g_actions);profile=g_profile;hand=g_profileHand;
    }
    bool save=false;
    for(const auto& edit:edits) {
        if(!edit.profile.empty() && (edit.profile!=profile || edit.hand!=hand)) {
            notice("The equipped item changed; its previous profile was not edited.");continue;
        }
        if(!visible(spec(edit.id),Level::Debug,g_backend.context())) {
            notice("That control no longer applies to the active implementation.");continue;
        }
        if(!raw_write(edit.id,edit.hand,edit.value)) {notice("That setting could not be applied.");continue;}
        if(edit.commit && is_player_setting(edit.id)) {
            const int target=spec(edit.id).scope==Scope::Global?-1:edit.hand;
            if(g_preferences.set({edit.id,target,edit.profile,edit.value})) save=true;
        }
    }
    for(auto action:actions) {
        if(action==Action::Recenter) camera::menu_recenter();
        if(action==Action::RecenterScreen) vr::release_screen_anchor();
        if(action==Action::RetrySave) save=true;
        if(action==Action::SaveDebugDefaults) {
            std::size_t count;const auto* all=specs(count);const auto ctx=g_backend.context();
            for(std::size_t i=0;i<count;++i) if(all[i].level==Level::Debug && visible(all[i],Level::Debug,ctx)) {
                const float value=raw_read(all[i].id,-1);
                if(valid_value(all[i],value)) g_preferences.set({all[i].id,-1,"",value},true);
            }
            save=true;
        }
    }
    if(save) save_preferences();
}
} // namespace bvr::b1r::menu
