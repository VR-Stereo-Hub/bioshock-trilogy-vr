#pragma once
// In-process OpenXR runtime (M2): session on the game's own D3D11 device,
// frame pacing grafted onto the Present hook, and the game frame shown on a
// quad layer ("cinema screen") in the headset. Everything is fail-soft: no
// runtime, no headset, or any XR error just leaves the game running flat.
//
// Threading: init_instance() runs on the framework init thread before the
// D3D11 hooks install; everything else runs on the game's render thread
// inside the Present/ResizeBuffers detours.

struct IDXGISwapChain;

namespace bvr::vr {

// Create the XrInstance (loads the active 32-bit runtime). Fail-soft.
void init_instance();

// Present-hook head: bring up / pump the session, xrWaitFrame + xrBeginFrame.
// xrWaitFrame blocks, which paces the game to the headset refresh while a
// session is running.
void on_present_begin(IDXGISwapChain* swapchain);

// Present-hook tail: copy the backbuffer (incl. overlay) into the quad-layer
// swapchain and xrEndFrame.
void on_present_end(IDXGISwapChain* swapchain);

// ResizeBuffers: drop backbuffer-size-dependent resources (quad swapchain).
void on_resize();

// Status + controls section for the overlay.
void draw_debug_ui();

// --- M3: head pose for the camera drive ------------------------------------
// Core speaks meters + quaternions (XR convention); the game adapter owns the
// conversion to engine units.

struct HeadPose {
    float px, py, pz;     // meters, XR LOCAL space (right +X, up +Y, fwd -Z)
    float qx, qy, qz, qw; // orientation quaternion
};

// Latest predicted head pose (located at Present-head for the upcoming
// display time). False while not tracking.
bool get_head_pose(HeadPose& out);

// --- M6: controller poses for decoupled aim ---------------------------------
// Latest predicted GRIP pose of a hand (0 = left, 1 = right), located at the
// SAME predicted display time as the head pose above, so an aim ray built from
// it belongs to the same instant as the camera. False while that hand is not
// tracked (no session, unfocused, controller asleep) - callers must then fall
// back to the game's own aim rather than freezing on a stale pose.
// `aimPose` true = the runtime's pointing ray (aiming), false = the grip pose
// (hand/weapon placement).
bool get_hand_pose(int hand, bool aimPose, HeadPose& out);

// True when the user enabled VR camera mode AND a session is running; the
// adapter drives the game camera from the HMD only while this holds. Frame
// submission switches from the quad to a projection layer at the same time.
bool vr_camera_mode();

// Programmatic camera-mode request (same flag the overlay checkbox writes).
// The drive still engages only once the session + projection are ready -
// this just records intent, so it is safe to call any time (adapter
// one-toggle flows use it).
void set_camera_mode(bool on);

// Same contract for the master enable and the SR pair pacing flag - the
// adapter's VR-preset flow arms them programmatically (session 16 part 3).
void set_enabled(bool on);
void set_sr_pair_pacing(bool on);

// --- M8: headset-disconnect stall guard --------------------------------------
// "vrpace ..." seam (game thread). When the session leaves FOCUSED after
// having held it, presents skip the blocking xrWaitFrame so the flat window
// keeps running while the headset idles; a 5 s keepalive still paces one real
// frame so the runtime can re-grant FOCUSED even if it wants to see frames.
//   on | off          the guard (off = pre-M8 stall behavior, live A/B)
//   simidle on|off    flat stand-in for a headset idle: the same guard
//                     decision runs with the state forced VISIBLE and a 1 s
//                     sleep in place of the runtime's blocked wait (flat has
//                     no XR session, so this is how the guard is verified)
//   status            guard state + skip/keepalive/last-wait telemetry
void handle_pace_command(const char* args);

// --- M8: desktop mirror ------------------------------------------------------
// "vrmirror ..." seam (game thread). Under SequentialReentry stereo the flat
// window alternates L/R eyes per present; the mirror pins it to the LEFT eye
// (left presents snapshot the backbuffer, right presents re-show the held
// image AFTER the right eye's XR capture, so the headset feed is untouched).
//   on | off | status   (off = the pre-M8 alternation, live A/B)
void handle_mirror_command(const char* args);

// Symmetric horizontal FOV (degrees) circumscribing the headset's per-eye
// FOV at the backbuffer aspect - what the game should render with in camera
// mode. 0 until the first views are located.
float suggested_hfov_deg();

// The adapter reports the horizontal FOV the game is actually rendering with
// (read back from the engine every frame). Projection-layer submission claims
// this value, so claimed fov matches the rendered image even when an engine
// FOV write is clamped or ignored - mismatch there shows up as fisheye or
// binocular-scope distortion in the headset.
void set_rendered_hfov(float hfovDeg);

// --- M4 rung 1: AlternateEye stereo -----------------------------------------
// Which eye the NEXT game frame should render for: -1 left, +1 right, 0 =
// AlternateEye off (render centered, exactly the M3 behavior). The adapter's
// CalcView drive shifts the camera by sign * IPD/2 along view-right. The
// render thread flips the sign after each submitted frame, matching the eye
// whose swapchain the next Present's backbuffer copy will feed; each eye's
// last image + pose is held for the compositor to reproject on its off frame.
int current_eye_sign();

// --- M4 rung 2: SequentialReentry stereo ------------------------------------
// The game adapter double-calls the engine's scene build, rendering two
// frames per game tick (left eye then right - DR-5). Because every submitted
// frame Presents exactly once, eye attribution rides a tiny SPSC tag ring:
// the GAME thread pushes the eye sign of each frame at its engine submit
// (strictly before that frame's Present), and the render thread pops one tag
// per Present at the tail, capturing the backbuffer into that eye's
// swapchain (same pair as AER). Presents without a tag take the mono/AER
// path unchanged. If the ring depth ever exceeds one pair the render thread
// clears it and logs (self-heal after a mode-boundary skew).
void sr_push_eye(int eyeSign); // game thread, at submit; -1 left, +1 right

// --- M7: the aim laser ------------------------------------------------------
// A row of soft dots along the hand's aim ray, submitted as extra XR quad
// layers. Doing it as compositor layers rather than as geometry in the game
// scene means it lives purely in XR space: correct in both eyes for free, with
// no game-space projection, no engine hook and nothing the renderer can clip.
//
// It also doubles as the aim CALIBRATION tool - the dots follow the same aim
// pose and the same pitch/yaw trim the fire ray uses, so where they point is
// where the shot goes.
struct LaserConfig {
    bool enabled = false;
    int hand = 1;              // 0 left, 1 right
    float pitchTrimDeg = 0.0f; // must match the fire ray's trim
    float yawTrimDeg = 0.0f;
    float posFwdCm = 0.0f;     // ray ORIGIN offset in the trimmed ray's frame,
    float posRightCm = 0.0f;   // matching the game-side fire-origin offset
    float posUpCm = 0.0f;      // (cm here; the game side scales by worldScale)
    int dots = 6;              // clamped to the layer budget
    float nearM = 0.30f;       // first dot, meters from the controller
    float farM = 6.0f;         // last dot
    float sizeDeg = 0.7f;      // angular diameter, so the beam reads evenly
};

// Publish the laser state (game thread, once per frame). The render thread
// builds the layers from it at submit time.
void set_laser(const LaserConfig& cfg);

} // namespace bvr::vr
