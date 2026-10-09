#pragma once

// The BS1 menu's presentation contract. No engine pointers, hooks or file I/O.
// The offscreen renderer compiles this same catalogue and view as the DLL.
#include <cstddef>
#include <string>

namespace bvr::b1r::menu {
enum class Tab { Hands, IK, Aim, Controls, Comfort, HUD, Display, Runtime, Diagnostics };
enum class Level { Basic, Advanced, Debug };
enum class Kind { Toggle, Slider, Choice };
enum class Scope { Global, Hand, Profile };
enum class Condition {
    Always, Snap, Smooth, Swing, Reticle, Laser, BodyFollow, CustomFov,
    LegacyHands, LegacySolver, SolverChoice, HandPoseChoice, R3Free,
    GameOwnsView, SkeletalWeapon, ComposedHands
};
enum class Setting {
    TextSize, ViewLevel, Height,
    HandSize, WeaponSize, PlaceForward, PlaceRight, PlaceUp,
    ModelPitch, ModelYaw, ModelRoll, GripForward, GripRight, GripUp,
    ShoulderForward, ShoulderRight, ShoulderUp, ArmLength, ElbowOut,
    Reticle, ReticleDistance, ReticleSize, Laser, LaserDots, LaserReach, LaserSize,
    GameCrosshair, HideUnarmedReticle, HideSceneReticle,
    AimPitch, AimYaw, AimForward, AimRight, AimUp,
    SnapTurn, SnapAngle, TurnSpeed, StickDeadzone, AmmoModifier, AmmoStick, R3Jump,
    Swing, SwingSpeed, SwingCooldown, SwingDelay,
    BodyFollow, BodyRate, BodyDeadzone, RemoveBob, WorldScale, ViewForward,
    CineDrive, HideBars, SceneTurning, SceneTurnRate, SceneRecentre, RotationFollow,
    HudPanel, HudDistance, HudWidth, HudHeight,
    CustomFov, GameFov, ScreenPlacement, ScreenHeight, ScreenWidth, AutoStart,
    HandMode, HandEnabled, ArmSolver, ArmTwist, HumerusTwist, WristFollow, ElbowSmoothing,
    LatePosition, GripRoll, ModelAimPose, HideInactive, ArmAppearance, FreeArmAppearance,
    AnimationAllowed, EquipAnimation, RenderLock, RenderLateral, RenderDepth,
    AimEnabled, AimRuntimePose, AimHandOrigin, LockOnDisabled, EyeSeparation,
    InputEnabled, CameraEnabled, WalkProbe, MirrorWalk, InstantWalk,
    SceneDetection, WorldFovGuard, SceneHold, SubtitleInFrame,
    EffectsInFrame, EffectVertexLimit, EffectRtOnly,
    ScreenDistance, CutsceneDisplay, HideSceneArms, GameSceneHands,
    PinPosition, PinRotation, HideHeldArm, BoneProbes,
    HandsArmsSize, ShoulderBarForward, ShoulderBarRight, ShoulderBarUp, ShoulderWidth, ShouldersLinked,
    Count
};
struct Spec {
    Setting id;
    const char* key;
    Tab tab;
    const char* section;
    Level level;
    const char* label;
    Kind kind;
    float min, max;
    const char* format;
    const char* choices; // double-NUL terminated, choice value starts at min
    Scope scope = Scope::Global;
    Condition condition = Condition::Always;
    const char* help = nullptr;
};
struct Context {
    int handMode = 3;
    int maxHandMode = 3;
    bool solverV2 = false;
    bool hasSolverChoice = false;
    bool tracking = false;
    bool cameraHook = false;
    bool engineReady = false;
    int profileHand = -1;
    std::string profile;
    std::string notice;
};
enum class Action { Recenter, RecenterScreen, SaveDebugDefaults, RetrySave, ResetMenuOverrides };
struct Backend {
    virtual ~Backend() = default;
    virtual float read(Setting id, int hand) const = 0;
    // Changes are live; commit is true for a toggle/choice or slider release.
    virtual void edit(Setting id, int hand, float value, bool commit) = 0;
    virtual Context context() const = 0;
    virtual void action(Action action) = 0;
    virtual void diagnostics(Tab tab) = 0;
};
const Spec* specs(std::size_t& count);
const Spec& spec(Setting id);
const char* tab_name(Tab tab);
bool is_player_setting(Setting id);
bool valid_value(const Spec& spec, float value);
// Hidden = not applicable to this implementation. Disabled = applicable but
// currently unavailable, with an explanation adjacent to the control.
bool visible(const Spec& spec, Level level, const Context& context);
const char* disabled_reason(const Spec& spec, const Backend& backend, int hand);
} // namespace bvr::b1r::menu
