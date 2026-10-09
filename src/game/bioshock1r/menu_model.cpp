#include "menu_model.h"
#include <cassert>
#include <cmath>

namespace bvr::b1r::menu {
namespace {
using S = Setting; using T = Tab; using L = Level; using K = Kind;
using P = Scope; using C = Condition;
// Stable keys are also the on-disk schema. Labels may change without losing settings.
const Spec kSpecs[] = {
    {S::TextSize,"TextSize",T::Display,"",L::Basic,"Text size",K::Slider,0.8f,1.5f,"%.0f%%",nullptr},
    {S::ViewLevel,"ViewLevel",T::Display,"",L::Basic,"Options",K::Choice,0,2,nullptr,"Basic\0Advanced\0Debug\0"},
    {S::Height,"Height",T::Comfort,"",L::Basic,"Height offset",K::Slider,-150,150,"%+.0f cm",nullptr},
    {S::HandSize,"HandSize",T::Hands,"Hand and weapon size",L::Basic,"Hand size",K::Slider,.2f,4,"%.2fx",nullptr,P::Hand,C::LegacyHands},
    {S::WeaponSize,"WeaponSize",T::Hands,"Hand and weapon size",L::Basic,"Weapon size",K::Slider,.2f,4,"%.2fx",nullptr,P::Global,C::SkeletalWeapon,"Changes skeletal weapons; the rigid wrench does not use this scale path."},
    {S::PlaceForward,"PlaceForward",T::Hands,"Hand position",L::Basic,"Forward / backward",K::Slider,-60,60,"%+.1f cm",nullptr,P::Profile},
    {S::PlaceRight,"PlaceRight",T::Hands,"Hand position",L::Basic,"Right / left",K::Slider,-60,60,"%+.1f cm",nullptr,P::Profile},
    {S::PlaceUp,"PlaceUp",T::Hands,"Hand position",L::Basic,"Up / down",K::Slider,-60,60,"%+.1f cm",nullptr,P::Profile},
    {S::ModelPitch,"ModelPitch",T::Hands,"Hand rotation",L::Basic,"Tilt",K::Slider,-180,180,"%+.1f deg",nullptr,P::Profile},
    {S::ModelYaw,"ModelYaw",T::Hands,"Hand rotation",L::Basic,"Turn",K::Slider,-180,180,"%+.1f deg",nullptr,P::Profile},
    {S::ModelRoll,"ModelRoll",T::Hands,"Hand rotation",L::Basic,"Roll",K::Slider,-180,180,"%+.1f deg",nullptr,P::Profile},
    {S::GripForward,"GripForward",T::Hands,"Grip pivot",L::Advanced,"Forward / backward",K::Slider,-100,100,"%+.1f cm",nullptr,P::Profile},
    {S::GripRight,"GripRight",T::Hands,"Grip pivot",L::Advanced,"Right / left",K::Slider,-100,100,"%+.1f cm",nullptr,P::Profile},
    {S::GripUp,"GripUp",T::Hands,"Grip pivot",L::Advanced,"Up / down",K::Slider,-100,100,"%+.1f cm",nullptr,P::Profile},
    {S::ShoulderForward,"ShoulderForward",T::IK,"Shoulder position",L::Basic,"Forward / backward",K::Slider,-60,60,"%+.1f cm",nullptr,P::Hand,C::LegacyHands},
    {S::ShoulderRight,"ShoulderRight",T::IK,"Shoulder position",L::Basic,"Right / left",K::Slider,-60,60,"%+.1f cm",nullptr,P::Hand,C::LegacyHands},
    {S::ShoulderUp,"ShoulderUp",T::IK,"Shoulder position",L::Basic,"Up / down",K::Slider,-80,40,"%+.1f cm",nullptr,P::Hand,C::LegacyHands},
    {S::ArmLength,"ArmLength",T::IK,"Arm length and elbows",L::Basic,"Arm length",K::Slider,.5f,1.5f,"%.2fx",nullptr},
    {S::ElbowOut,"ElbowOut",T::IK,"Arm length and elbows",L::Basic,"Elbows outward",K::Slider,0,1,"%.2f",nullptr},
    {S::Reticle,"Reticle",T::Aim,"Reticle",L::Basic,"Show the reticle",K::Toggle,0,1},
    {S::ReticleDistance,"ReticleDistance",T::Aim,"Reticle",L::Basic,"Reticle distance",K::Slider,.5f,20,"%.1f m",nullptr,P::Global,C::Reticle},
    {S::ReticleSize,"ReticleSize",T::Aim,"Reticle",L::Basic,"Reticle size",K::Slider,.1f,3,"%.2f deg",nullptr,P::Global,C::Reticle},
    {S::Laser,"Laser",T::Aim,"Aim laser",L::Basic,"Show the aim laser",K::Toggle,0,1},
    {S::LaserDots,"LaserDots",T::Aim,"Aim laser",L::Advanced,"Laser dots",K::Slider,1,8,"%.0f",nullptr,P::Global,C::Laser},
    {S::LaserReach,"LaserReach",T::Aim,"Aim laser",L::Advanced,"Laser reach",K::Slider,1,20,"%.1f m",nullptr,P::Global,C::Laser},
    {S::LaserSize,"LaserSize",T::Aim,"Aim laser",L::Advanced,"Laser dot size",K::Slider,.1f,3,"%.2f deg",nullptr,P::Global,C::Laser},
    {S::GameCrosshair,"GameCrosshair",T::Aim,"Reticle visibility",L::Advanced,"Show the game's crosshair",K::Toggle,0,1},
    {S::HideUnarmedReticle,"HideUnarmedReticle",T::Aim,"Reticle visibility",L::Advanced,"Hide reticle with empty hands",K::Toggle,0,1},
    {S::HideSceneReticle,"HideSceneReticle",T::Aim,"Reticle visibility",L::Advanced,"Hide reticle during cutscenes",K::Toggle,0,1},
    {S::AimPitch,"AimPitch",T::Aim,"Aim alignment",L::Advanced,"Tilt",K::Slider,-90,90,"%+.1f deg",nullptr,P::Profile},
    {S::AimYaw,"AimYaw",T::Aim,"Aim alignment",L::Advanced,"Turn",K::Slider,-90,90,"%+.1f deg",nullptr,P::Profile},
    {S::AimForward,"AimForward",T::Aim,"Aim origin",L::Advanced,"Forward / backward",K::Slider,-30,30,"%+.1f cm",nullptr,P::Profile},
    {S::AimRight,"AimRight",T::Aim,"Aim origin",L::Advanced,"Right / left",K::Slider,-30,30,"%+.1f cm",nullptr,P::Profile},
    {S::AimUp,"AimUp",T::Aim,"Aim origin",L::Advanced,"Up / down",K::Slider,-30,30,"%+.1f cm",nullptr,P::Profile},
    {S::SnapTurn,"SnapTurn",T::Controls,"Turning",L::Basic,"Turn style",K::Choice,0,1,nullptr,"Smooth\0Snap\0"},
    {S::SnapAngle,"SnapAngle",T::Controls,"Turning",L::Basic,"Turn angle",K::Slider,15,90,"%.0f deg",nullptr,P::Global,C::Snap},
    {S::TurnSpeed,"TurnSpeed",T::Controls,"Turning",L::Basic,"Turn speed",K::Slider,.2f,3,"%.2fx",nullptr,P::Global,C::Smooth},
    {S::StickDeadzone,"StickDeadzone",T::Controls,"Turning",L::Advanced,"Stick deadzone",K::Slider,0,.4f,"%.2f",nullptr},
    {S::AmmoModifier,"AmmoModifier",T::Controls,"Controller shortcuts",L::Basic,"Ammo selection modifier",K::Choice,0,4,nullptr,"Off\0Right thumbrest\0Right-stick click\0Left grip\0Left thumbrest\0"},
    {S::AmmoStick,"AmmoStick",T::Controls,"Controller shortcuts",L::Basic,"Ammo selection stick",K::Choice,0,1,nullptr,"Right stick\0Left stick\0"},
    {S::R3Jump,"R3Jump",T::Controls,"Controller shortcuts",L::Basic,"Right-stick click jumps",K::Toggle,0,1,nullptr,nullptr,P::Global,C::R3Free},
    {S::Swing,"Swing",T::Controls,"Motion wrench",L::Basic,"Swing to attack",K::Toggle,0,1},
    {S::SwingSpeed,"SwingSpeed",T::Controls,"Motion wrench",L::Basic,"Swing speed needed",K::Slider,.2f,5,"%.2f m/s",nullptr,P::Global,C::Swing},
    {S::SwingCooldown,"SwingCooldown",T::Controls,"Motion wrench",L::Basic,"Time between swings",K::Slider,100,1500,"%.0f ms",nullptr,P::Global,C::Swing},
    {S::SwingDelay,"SwingDelay",T::Controls,"Motion wrench",L::Advanced,"Attack delay",K::Slider,0,500,"%.0f ms",nullptr,P::Global,C::Swing},
    {S::BodyFollow,"BodyFollow",T::Comfort,"Movement",L::Basic,"Walk toward your view",K::Toggle,0,1},
    {S::BodyRate,"BodyRate",T::Comfort,"Movement",L::Advanced,"Body turning response",K::Slider,0,10,"%.2f /s",nullptr,P::Global,C::BodyFollow,"Zero follows immediately."},
    {S::BodyDeadzone,"BodyDeadzone",T::Comfort,"Movement",L::Advanced,"Head-turn deadzone",K::Slider,0,60,"%.1f deg",nullptr,P::Global,C::BodyFollow},
    {S::RemoveBob,"RemoveBob",T::Comfort,"Camera motion",L::Basic,"Remove camera shake and head bob",K::Toggle,0,1},
    {S::WorldScale,"WorldScale",T::Comfort,"World scale",L::Basic,"World scale",K::Slider,.5f,2,"%.2fx",nullptr},
    {S::ViewForward,"ViewForward",T::Comfort,"Viewpoint",L::Advanced,"Forward / backward",K::Slider,-80,80,"%+.1f cm",nullptr},
    {S::CineDrive,"CineDrive",T::Comfort,"Cutscenes and special cameras",L::Advanced,"Head movement",K::Choice,0,2,nullptr,"Headset controls view\0Game camera\0Game camera + look around\0"},
    {S::HideBars,"HideBars",T::Comfort,"Cutscenes and special cameras",L::Advanced,"Hide cinematic black borders",K::Toggle,0,1},
    {S::SceneTurning,"SceneTurning",T::Comfort,"Cutscenes and special cameras",L::Advanced,"Allow turning during cutscenes",K::Toggle,0,1},
    {S::SceneTurnRate,"SceneTurnRate",T::Comfort,"Cutscenes and special cameras",L::Advanced,"Cutscene turn speed",K::Slider,15,180,"%.0f deg/s",nullptr},
    {S::SceneRecentre,"SceneRecentre",T::Comfort,"Cutscenes and special cameras",L::Advanced,"When the scene turns your view",K::Choice,0,2,nullptr,"Keep my turn offset\0Gradually return to scene\0Return immediately\0"},
    {S::RotationFollow,"RotationFollow",T::Comfort,"Cutscenes and special cameras",L::Advanced,"Follow game camera rotation",K::Choice,0,2,nullptr,"All axes\0Horizontal only\0Neither\0",P::Global,C::GameOwnsView},
    {S::HudPanel,"HudPanel",T::HUD,"HUD placement",L::Basic,"HUD on its own panel",K::Toggle,0,1,nullptr,nullptr,P::Global,C::Always,"Turning this off changes HUD routing; it does not hide the game's interface."},
    {S::HudDistance,"HudDistance",T::HUD,"HUD placement",L::Advanced,"Distance",K::Slider,.3f,5,"%.2f m",nullptr},
    {S::HudWidth,"HudWidth",T::HUD,"HUD placement",L::Advanced,"Width",K::Slider,.3f,4,"%.2f m",nullptr},
    {S::HudHeight,"HudHeight",T::HUD,"HUD placement",L::Advanced,"Height offset",K::Slider,-2,2,"%+.2f m",nullptr},
    {S::CustomFov,"CustomFov",T::Display,"Field of view",L::Basic,"Custom gameplay FOV",K::Toggle,0,1,nullptr,nullptr,P::Global,C::Always,"Enabling custom FOV disables forced headset FOV. Changes affect gameplay, not menus."},
    {S::GameFov,"GameFov",T::Display,"Field of view",L::Basic,"Gameplay FOV",K::Slider,75,150,"%.0f deg",nullptr,P::Global,C::CustomFov},
    {S::ScreenPlacement,"ScreenPlacement",T::Display,"Flat screen for menus and loading",L::Advanced,"Screen placement",K::Choice,0,2,nullptr,"Anchor when opened\0Follow head\0Recenter origin\0"},
    {S::ScreenHeight,"ScreenHeight",T::Display,"Flat screen for menus and loading",L::Advanced,"Height offset",K::Slider,-2,2,"%+.2f m",nullptr},
    {S::ScreenWidth,"ScreenWidth",T::Display,"Flat screen for menus and loading",L::Advanced,"Width",K::Slider,.5f,5,"%.2f m",nullptr},
    {S::AutoStart,"AutoStart",T::Display,"Startup",L::Advanced,"Start VR automatically",K::Toggle,0,1},
    {S::HandMode,"HandMode",T::Diagnostics,"Hand implementation",L::Debug,"Hand implementation",K::Choice,2,4,nullptr,"Bones\0BRVR\0Dishonored\0"},
    {S::HandEnabled,"HandEnabled",T::Diagnostics,"Hand implementation",L::Debug,"Drive hands from controllers",K::Toggle,0,1},
    {S::ArmSolver,"ArmSolver",T::Diagnostics,"Arm solver",L::Debug,"Arm solver",K::Choice,0,1,nullptr,"Legacy\0Dishonored\0",P::Global,C::SolverChoice},
    {S::ArmTwist,"ArmTwist",T::Diagnostics,"Legacy arm solver",L::Debug,"Arm twist limit",K::Slider,0,180,"%.0f deg",nullptr,P::Global,C::LegacySolver},
    {S::HumerusTwist,"HumerusTwist",T::Diagnostics,"Legacy arm solver",L::Debug,"Humerus twist limit",K::Slider,0,180,"%.0f deg",nullptr,P::Global,C::LegacySolver},
    {S::WristFollow,"WristFollow",T::Diagnostics,"Legacy arm solver",L::Debug,"Elbow follows wrist",K::Slider,0,1,"%.2f",nullptr,P::Global,C::LegacySolver},
    {S::ElbowSmoothing,"ElbowSmoothing",T::Diagnostics,"Legacy arm solver",L::Debug,"Elbow smoothing",K::Slider,0,500,"%.0f ms",nullptr,P::Global,C::LegacySolver},
    {S::LatePosition,"LatePosition",T::Diagnostics,"Legacy hand drive",L::Debug,"Replay actor position after tick",K::Toggle,0,1,nullptr,nullptr,P::Global,C::LegacyHands},
    {S::GripRoll,"GripRoll",T::Diagnostics,"Hand implementation",L::Debug,"Grip offset follows wrist roll",K::Toggle,0,1},
    {S::ModelAimPose,"ModelAimPose",T::Diagnostics,"Legacy hand drive",L::Debug,"Align model to aim pose",K::Toggle,0,1,nullptr,nullptr,P::Global,C::HandPoseChoice},
    {S::HideInactive,"HideInactive",T::Hands,"Arm visibility and game animations",L::Advanced,"Hide inactive hand",K::Toggle,0,1,nullptr,nullptr,P::Global,C::LegacyHands},
    {S::ArmAppearance,"ArmAppearance",T::Hands,"Arm visibility and game animations",L::Advanced,"Arm behavior",K::Choice,0,2,nullptr,"Game animation\0Follow controllers\0Hide both arms\0",P::Global,C::LegacyHands},
    {S::FreeArmAppearance,"FreeArmAppearance",T::Hands,"Arm visibility and game animations",L::Advanced,"Hide other arm",K::Toggle,0,1,nullptr,nullptr,P::Global,C::LegacyHands},
    {S::AnimationAllowed,"AnimationAllowed",T::Hands,"Arm visibility and game animations",L::Advanced,"Play weapon animations",K::Toggle,0,1,nullptr,nullptr,P::Profile,C::LegacyHands},
    {S::EquipAnimation,"EquipAnimation",T::Hands,"Arm visibility and game animations",L::Advanced,"Play equip animation",K::Toggle,0,1,nullptr,nullptr,P::Global,C::LegacyHands},
    {S::RenderLock,"RenderLock",T::Diagnostics,"Legacy render lock",L::Debug,"Render lock",K::Choice,0,2,nullptr,"Off\0Absolute\0Difference\0",P::Global,C::LegacyHands},
    {S::RenderLateral,"RenderLateral",T::Diagnostics,"Legacy render lock",L::Debug,"Lateral gain",K::Slider,0,2,"%.2f",nullptr,P::Global,C::LegacyHands},
    {S::RenderDepth,"RenderDepth",T::Diagnostics,"Legacy render lock",L::Debug,"Depth gain",K::Slider,0,2,"%.2f",nullptr,P::Global,C::LegacyHands},
    {S::AimEnabled,"AimEnabled",T::Diagnostics,"Aim implementation",L::Debug,"Controller aim",K::Toggle,0,1},
    {S::AimRuntimePose,"AimRuntimePose",T::Diagnostics,"Aim implementation",L::Debug,"Use runtime aim pose",K::Toggle,0,1},
    {S::AimHandOrigin,"AimHandOrigin",T::Diagnostics,"Aim implementation",L::Debug,"Ray starts at hand",K::Toggle,0,1},
    {S::LockOnDisabled,"LockOnDisabled",T::Diagnostics,"Aim implementation",L::Debug,"Disable gamepad lock-on",K::Toggle,0,1,nullptr,nullptr,P::Global,C::Always,"To re-enable lock-on, save Debug defaults and restart. The original radius is not restored live."},
    {S::EyeSeparation,"EyeSeparation",T::Diagnostics,"Projection calibration",L::Debug,"Rendered eye separation",K::Slider,45,80,"%.1f mm",nullptr,P::Global,C::Always,"Changes rendered eye offsets, not the physical headset IPD."},
    {S::InputEnabled,"InputEnabled",T::Runtime,"Runtime switches",L::Debug,"VR controller input",K::Toggle,0,1},
    {S::CameraEnabled,"CameraEnabled",T::Runtime,"Runtime switches",L::Debug,"VR camera",K::Toggle,0,1},
    {S::WalkProbe,"WalkProbe",T::Diagnostics,"Movement diagnostics",L::Debug,"Log walk direction",K::Toggle,0,1},
    {S::MirrorWalk,"MirrorWalk",T::Diagnostics,"Movement diagnostics",L::Debug,"Mirror movement direction",K::Toggle,0,1},
    {S::InstantWalk,"InstantWalk",T::Diagnostics,"Movement diagnostics",L::Debug,"Instant movement direction",K::Toggle,0,1},
    {S::SceneDetection,"SceneDetection",T::Diagnostics,"Scene detection",L::Debug,"Scripted scene detection",K::Toggle,0,1},
    {S::WorldFovGuard,"WorldFovGuard",T::Diagnostics,"Scene detection",L::Debug,"Gameplay FOV guard",K::Toggle,0,1},
    {S::SceneHold,"SceneHold",T::Diagnostics,"Scene detection",L::Debug,"Scene hold",K::Slider,0,10000,"%.0f ms",nullptr},
    {S::SubtitleInFrame,"SubtitleInFrame",T::Diagnostics,"HUD routing",L::Debug,"Cutscene subtitles in frame",K::Toggle,0,1},
    {S::EffectsInFrame,"EffectsInFrame",T::Diagnostics,"HUD routing",L::Debug,"Fullscreen effects in frame",K::Toggle,0,1},
    {S::EffectVertexLimit,"EffectVertexLimit",T::Diagnostics,"HUD routing",L::Debug,"Effect vertex limit",K::Slider,0,256,"%.0f",nullptr},
    {S::EffectRtOnly,"EffectRtOnly",T::Diagnostics,"HUD routing",L::Debug,"Use render-target discriminator",K::Toggle,0,1},
    {S::ScreenDistance,"ScreenDistance",T::Display,"Flat screen for menus and loading",L::Advanced,"Distance",K::Slider,.5f,5,"%.2f m",nullptr},
    {S::CutsceneDisplay,"CutsceneDisplay",T::Comfort,"Cutscenes and special cameras",L::Advanced,"Cutscene display",K::Choice,0,1,nullptr,"Flat screen\0Immersive stereo\0",P::Global,C::Always,"Stereo is used where the game supplies a usable projection. Other special cameras still fall back to a flat screen."},
    {S::HideSceneArms,"HideSceneArms",T::Hands,"Arm visibility and game animations",L::Advanced,"Hide idle arms during scenes",K::Toggle,0,1,nullptr,nullptr,P::Global,C::LegacyHands},
    {S::GameSceneHands,"GameSceneHands",T::Hands,"Arm visibility and game animations",L::Advanced,"Game controls hands during scenes",K::Toggle,0,1,nullptr,nullptr,P::Global,C::LegacyHands},
    {S::PinPosition,"PinPosition",T::Hands,"Arm visibility and game animations",L::Advanced,"Keep hand position during animations",K::Toggle,0,1,nullptr,nullptr,P::Hand,C::LegacyHands},
    {S::PinRotation,"PinRotation",T::Hands,"Arm visibility and game animations",L::Advanced,"Keep hand rotation during animations",K::Toggle,0,1,nullptr,nullptr,P::Hand,C::LegacyHands},
    {S::HideHeldArm,"HideHeldArm",T::Hands,"Arm visibility and game animations",L::Advanced,"Hide held arm",K::Toggle,0,1,nullptr,nullptr,P::Global,C::LegacyHands},
    {S::BoneProbes,"BoneProbes",T::Diagnostics,"Hand implementation",L::Debug,"Log hand and arm probes",K::Toggle,0,1},
    {S::HandsArmsSize,"HandsArmsSize",T::Hands,"Hand and weapon size",L::Basic,"Hands and arms size",K::Slider,.5f,2,"%.2fx",nullptr,P::Global,C::ComposedHands,"Changes both hands and arms together. Weapon size remains separate."},
    {S::ShoulderBarForward,"ShoulderBarForward",T::IK,"Shoulder position",L::Basic,"Forward / backward",K::Slider,-60,60,"%+.1f cm",nullptr,P::Global,C::ComposedHands},
    {S::ShoulderBarRight,"ShoulderBarRight",T::IK,"Shoulder position",L::Basic,"Right / left",K::Slider,-40,40,"%+.1f cm",nullptr,P::Global,C::ComposedHands},
    {S::ShoulderBarUp,"ShoulderBarUp",T::IK,"Shoulder position",L::Basic,"Up / down",K::Slider,-80,30,"%+.1f cm",nullptr,P::Global,C::ComposedHands},
    {S::ShoulderWidth,"ShoulderWidth",T::IK,"Shoulder position",L::Basic,"Shoulder width",K::Slider,10,90,"%.1f cm",nullptr,P::Global,C::ComposedHands},
    {S::ShouldersLinked,"ShouldersLinked",T::IK,"Shoulder position",L::Basic,"Shoulders move together",K::Toggle,0,1,nullptr,nullptr,P::Global,C::ComposedHands},
};
static_assert(std::size(kSpecs) == static_cast<std::size_t>(S::Count));
}
const Spec* specs(std::size_t& count) { count = std::size(kSpecs); return kSpecs; }
const Spec& spec(Setting id) {
    const auto index = static_cast<std::size_t>(id);
    assert(index < std::size(kSpecs) && kSpecs[index].id == id);
    return kSpecs[index];
}
const char* tab_name(Tab tab) {
    const char* names[] = {"Hands","IK","Aim","Controls","Comfort","HUD","Display","Runtime","Diagnostics"};
    return names[static_cast<int>(tab)];
}
bool is_player_setting(Setting id) { return spec(id).level != Level::Debug; }
bool valid_value(const Spec& s, float value) {
    return std::isfinite(value) && value >= s.min && value <= s.max &&
        (s.kind == Kind::Slider || value == std::floor(value));
}
bool visible(const Spec& s, Level level, const Context& c) {
    if (s.level > level) return false;
    switch(s.condition) {
        case Condition::ComposedHands: return c.handMode>=4;
        case Condition::LegacyHands: return c.handMode < 4;
        case Condition::LegacySolver: return c.handMode < 4 && !c.solverV2;
        case Condition::SolverChoice: return c.handMode < 4 && c.hasSolverChoice;
        case Condition::HandPoseChoice: return c.handMode < 3;
        default: return true;
    }
}
const char* disabled_reason(const Spec& s, const Backend& b, int hand) {
    const auto c = b.context();
    if(!c.engineReady && s.id!=S::TextSize && s.id!=S::ViewLevel)
        return "Waiting for the game's camera hook.";
    if (s.scope == Scope::Profile && (hand < 0 || c.profile.empty() || c.profileHand != hand))
        return "Equip an item in the selected hand to edit its profile.";
    switch(s.condition) {
        case Condition::Swing: if(!b.read(S::Swing,0)) return "Enable Swing to attack to adjust this."; break;
        case Condition::Reticle: if(!b.read(S::Reticle,0)) return "Enable the reticle to adjust this."; break;
        case Condition::Laser: if(!b.read(S::Laser,0)) return "Enable the aim laser to adjust this."; break;
        case Condition::BodyFollow: if(!b.read(S::BodyFollow,0)) return "Enable Walk toward your view to adjust this."; break;
        case Condition::CustomFov: if(!b.read(S::CustomFov,0)) return "Enable custom gameplay FOV to adjust this."; break;
        case Condition::R3Free: if(b.read(S::AmmoModifier,0)==2) return "Right-stick click is assigned to ammo selection."; break;
        case Condition::GameOwnsView: if(b.read(S::CineDrive,0)==0) return "The headset currently owns the view."; break;
        case Condition::SkeletalWeapon: if(c.profile.empty() || c.profileHand != 1 || c.profile=="Wrench") return "Equip a skeletal weapon to adjust its size."; break;
        default: break;
    }
    if(s.id==S::SceneTurnRate && !b.read(S::SceneTurning,0)) return "Enable turning during cutscenes to adjust this.";
    if((s.id==S::FreeArmAppearance || s.id==S::HideHeldArm) && b.read(S::ArmAppearance,0)==2) return "Arm behavior is set to hide both arms.";
    if((s.id==S::HudDistance || s.id==S::HudWidth || s.id==S::HudHeight) && !b.read(S::HudPanel,0)) return "Enable the HUD panel to adjust its placement.";
    return nullptr;
}
} // namespace bvr::b1r::menu
