#pragma once

// The BS1 menu's presentation contract. No engine pointers, hooks or file I/O.
// The offscreen renderer compiles this same catalogue and view as the DLL.
#include <cstddef>
#include <cstdint>
#include <string>

// The HUD's pieces, in hud_elements.cpp's table order: (suffix, F10 label, help).
// The enum, the F10 rows and the selector's choices are all generated from this.
#define BVR_HUD_PIECES(X)                                                                          \
    X(Health, "Health bar", "The red health bar with its first-aid gauge.")                        \
    X(Eve, "EVE bar", "The blue EVE bar with its hypo gauge. The two gauges are one picture in the game; each goes with its own bar.") \
    X(Reticle, "Game crosshair", "The game's own crosshair. The mod's reticle and laser are on the Aim tab.") \
    X(Target, "Enemy name and health", "The enemy's name and the health arc above the crosshair.")   \
    X(Prompt, "Use and hack prompts", "Pick up, use, hack, search and the container list, right of the crosshair.") \
    X(Ammo, "Ammo", "The ammo holder, shown while a weapon is in your hand. Bottom left.")         \
    X(Plasmid, "Plasmid", "The plasmid gear and name, shown while a plasmid is in your hand. The game draws it where the ammo goes.") \
    X(Wheel, "Weapon and plasmid wheel", "The selection wheel's ring while it is open. With Pick with your hand on (Controls tab) it opens at the hand instead.") \
    X(WheelName, "Wheel item name", "The highlighted item's name above the wheel. At the hand dial it stays above the ring.") \
    X(WheelLabel, "Wheel button line", "SELECT WEAPON / SELECT PLASMID under the wheel. Hidden at the hand dial.") \
    X(Subtitles, "Subtitles", "Dialogue subtitles and the area name, bottom centre.")              \
    X(Tutorial, "Tutorial messages", "The training messages above the subtitles.")                \
    X(Objective, "Objective", "The current goal, top right.")                                     \
    X(Messages, "Radio messages", "Radio and speech lines under the objective.")                  \
    X(Money, "Money and ADAM", "The dollar and ADAM counters, right edge.")                       \
    X(AudioLog, "Audio log", "The playing audio log or radio speaker, bottom right.")             \
    X(Escort, "Little Sister and gatherer", "The escorted Little Sister's health and the gatherer meter, under the bars.") \
    X(Timers, "Challenge timers", "The challenge room timers, left.")
#define BVR_HUD_ENUM_ANCHOR(n, label, help) HudEl##n,
#define BVR_HUD_ENUM_PLACE(n, label, help) HudX##n, HudY##n, HudScale##n,

namespace bvr::b1r::menu {
enum class Tab { Hands, IK, Aim, Controls, Comfort, HUD, Display, Runtime, Diagnostics };
enum class Level { Basic, Advanced, Debug };
enum class Kind { Toggle, Slider, Choice };
enum class Scope { Global, Hand, Profile };
enum class Condition {
    Always, Snap, Smooth, Swing, Reticle, Laser, BodyFollow, CustomFov,
    LegacyHands, LegacySolver, SolverChoice, HandPoseChoice, R3Free,
    GameOwnsView, SkeletalWeapon, ComposedHands, HudPiece
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
    HandsArmsSize, ShoulderBarForward, ShoulderBarRight, ShoulderBarUp, ShoulderWidth, WeaponFollowsHands,
    HandPitch, HandYaw, HandRoll, HandGripForward, HandGripRight, HandGripUp,
    UpperArmLength, ForearmLength, ShowArms,
    Upscaler, DlssQuality, DlssModel, StereoMethod, AfwTint, TemporalAudit,
    PlasmidFxAlong, PlasmidFxAcross, PlasmidFxOut,
    AfwRebuild, AfwHandGain, AfwHandsMask, HandDepthProbe,
    OutputWidth, OutputHeight, TemporalTaa, HandMotionVectors, BarrelOrigin, BarrelAim, LaserInImage,
    ReloadPlace, ReloadGunMotion, ReloadJoin, GunRecoil,
    // The HUD pieces (hud_elements.cpp): one anchor each, in the element table's order,
    // then the hand panel, the piece being adjusted and its placement, then the screens.
    BVR_HUD_PIECES(BVR_HUD_ENUM_ANCHOR)
    HudHandWidth, HudHandLift, HudHandFace, HudHandTilt,
    HudPiece,
    BVR_HUD_PIECES(BVR_HUD_ENUM_PLACE)
    // ORDER = kSpecs' row order: spec(id) indexes the table by this value (static_assert'ed).
    HudScreenPause, HudScreenMap, HudHideBackdrop, HudScreenMachines,
    // The weapon and plasmid dial (dial_input.cpp, swap_hold.cpp).
    DialEnabled, DialGripTrigger, DialHoldSwap, DialDirectionOnly, DialNeutral, DialTravel, DialSnap,
    DialEntryTilt, DialEntryYaw, DialWidth, DialDistance,
    // Grab and hand-over (dial_input.cpp's pad override).
    GrabEnabled, GrabDistance, GrabMirror,
    // The two-handed grip and the gunshot kick (two_hand.cpp).
    TwoHandEnabled, TwoHandDistance, ShotHaptics, ShotHapticStrength,
    // The manual reload (manual_reload.cpp).
    ManualReload,
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
    int handMode = 4;
    int maxHandMode = 4;
    bool solverV2 = false;
    bool hasSolverChoice = false;
    bool tracking = false;
    bool cameraHook = false;
    bool engineReady = false;
    int profileHand = -1;
    int hudPiece = 0; // the piece F10 > HUD > Adjust a piece shows the placement rows of
    std::string profile;
    std::string notice;
};
enum class Action { Recenter, RecenterScreen, SaveDebugDefaults, RetrySave, ResetMenuOverrides };
struct ResolutionState {
    uint32_t referenceW = 0, referenceH = 0;
    uint32_t renderW = 0, renderH = 0, outputW = 0, outputH = 0;
    bool busy = false;
    std::string status;
};
struct Backend {
    virtual ~Backend() = default;
    virtual float read(Setting id, int hand) const = 0;
    // Changes are live; commit is true for a toggle/choice or slider release.
    virtual void edit(Setting id, int hand, float value, bool commit) = 0;
    virtual Context context() const = 0;
    virtual void action(Action action) = 0;
    virtual void diagnostics(Tab tab) = 0;
    // s86f: the hand step rows (Dishonored's MpTrimViewStep). hand 0/1; rot false = move
    // (axis 0 right, 1 forward, 2 up; cm), true = turn (0 yaw, 1 pitch, 2 roll; degrees).
    virtual void hand_step(int, bool, int, float) {}
    virtual ResolutionState resolution() const { return {}; }
    virtual void temporal_status() {}
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
