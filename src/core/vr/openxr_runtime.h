#pragma once
// In-process OpenXR runtime (M2): session on the game's own D3D11 device,
// frame pacing grafted onto the Present hook, and the game frame shown on a
// quad layer ("cinema screen") in the headset. Everything is fail-soft: no
// runtime, no headset, or any XR error just leaves the game running flat.
//
// Threading: init_instance() runs on the framework init thread before the
// D3D11 hooks install; everything else runs on the game's render thread
// inside the Present/ResizeBuffers detours.

#include <cstdint>

struct IDXGISwapChain;
struct ID3D11DeviceContext;
struct ID3D11Texture2D;

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
// width/height/format are the game's ResizeBuffers arguments (0 = unchanged in
// DXGI's own convention). A same-size resize keeps the XR swapchains alive -
// see the session-23 note at the implementation.
void on_resize(unsigned width, unsigned height, unsigned format);

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

// The same read WITHOUT the pose-tag audit stamp, for readers that are not the
// camera drive. get_head_pose records "what the game thread consumed" for the
// submitted-vs-consumed audit; a second caller would corrupt that instrument.
bool peek_head_pose(HeadPose& out);

// --- M6: controller poses for decoupled aim ---------------------------------
// Latest predicted GRIP pose of a hand (0 = left, 1 = right), located at the
// SAME predicted display time as the head pose above, so an aim ray built from
// it belongs to the same instant as the camera. False while that hand is not
// tracked (no session, unfocused, controller asleep) - callers must then fall
// back to the game's own aim rather than freezing on a stale pose.
// `aimPose` true = the runtime's pointing ray (aiming), false = the grip pose
// (hand/weapon placement).
bool get_hand_pose(int hand, bool aimPose, HeadPose& out);

// --- Session 20: vrrec record+replay support ---------------------------------
// Sim overlay on the hand-pose funnel: while armed, every consumer of
// get_hand_pose/input_get_hand_pose (fire ray, viewmodel, laser) reads the
// injected poses. The recorder writes one set per replayed frame; a flat
// drive command can arm a static set for recording without a headset.
void set_sim_hand_pose(int hand, bool aimPose, bool valid, const float pos3[3],
                       const float quat4[4]);
void clear_sim_hand_poses();

// True while an XR session object exists (any state). `vrrec play` refuses
// while this holds - a live session and a replay would be two writers on the
// same funnel.
bool session_live();

// The last xrWaitFrame's predictedDisplayTime (0 with no session) - recorded
// as per-frame metadata.
int64_t last_predicted_time();

// Session 42: the last xrWaitFrame's predictedDisplayPeriod in ns (0 with no
// session, or on a runtime that leaves it unfilled). The runtime's own refresh
// interval - the input a pair-rate sync targets. Purely informational.
int64_t display_period_ns();

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

// AlternateEye stereo: one eye per frame, the compositor reprojecting the other.
// Judders, but it is REAL stereo and it never re-enters the engine's draw -
// which is the difference that matters on BioShock 2, where draw re-entrancy is
// the measured cause of the hard freeze (session 34).
void set_alternate_eye(bool on);

// --- s50 (Infinite): rendered-pose eye tags ----------------------------------
// The projection layer historically tags each eye's image with the RUNTIME'S
// located view pose. The game, however, renders each eye from the ADAPTER'S
// camera: the written base offset half-IPD (the adapter's ipd slider) along
// the head's right axis, both eyes PARALLEL. Wherever the runtime's located
// views differ from that - per-eye cant, a different IPD - the layer's claim
// misdescribes the render and the compositor's per-eye reprojection injects
// an off-center-growing disparity error (invisible on the far world, a
// near-field depth drift on the hands: the Infinite "FOV-edge" symptom).
// With this ON the tag is rebuilt as the parallel pair the game actually
// rendered: midpoint of the located pair, nlerp'd shared orientation,
// +-ipdMm/2000 along that orientation's right axis. On a runtime whose views
// are already parallel at the same IPD (the sim) this is bit-for-bit
// identity. Default OFF - BS1/BS2 behavior is untouched unless an adapter
// arms it (keep-the-mods-decoupled: additive, opt-in).
void set_eye_tag_rendered(bool on);
bool eye_tag_rendered();
void set_eye_tag_ipd_mm(float mm);

// --- s51 (Infinite): hand-anchored reference quad -----------------------------
// A small compositor quad parked AT the located grip pose, built per present
// on the render thread from the same locate generation as the projection
// layer - compositor-correct by construction. The in-headset discriminator:
// sweep the hand off-center; quad and rendered hand model SEPARATING means
// the game-render/projection/submission lane is wrong, moving TOGETHER means
// the composed hand world position itself is wrong. Default OFF - additive,
// armed by the Infinite adapter only.
void set_hand_ref_quad(bool on, int hand, float sizeDeg);
bool hand_ref_quad_on();

// --- s51 (Infinite): VDXR view logger ----------------------------------------
// Log the runtime's LOCATED view poses/fovs per eye for the next n frames
// (bounded burst, self-expiring): per frame both eyes' position + orientation
// + fov angles, plus derived inter-eye position delta, orientation delta
// (cant) and per-eye fov asymmetry. Answers in the log whether ANY per-eye
// cant/IPD delta exists under a runtime (VDXR) without a picture verdict.
void arm_view_log(int frames);

// --- s51 (Infinite): edge-telemetry snapshot ---------------------------------
// The render-thread half of the FOV-edge telemetry lane: when armed, each
// present banks the located views, the submitted per-eye pose tags and the
// claimed fov tangents under a small mutex; the game-thread sampler copies
// them out. OFF by default - zero work on BS1/BS2 unless an adapter arms it.
struct EdgeViewSnapshot {
    bool valid = false;
    uint64_t stampMs = 0;       // GetTickCount64 at fill
    float locPos[2][3];         // located view positions, m (XR space)
    float locQuat[2][4];        // located view orientations, xyzw
    float locFov[2][4];         // located fov angles L/R/U/D, radians
    float tagPos[2][3];         // submitted (claimed) per-eye pose tags
    float tagQuat[2][4];
    float claimTanH = 0.0f;     // claimed symmetric half-fov tangents
    float claimTanV = 0.0f;
};
void set_edge_snapshot(bool on);
bool get_edge_snapshot(EdgeViewSnapshot& out);

// --- M8: headset-disconnect stall guard --------------------------------------
// "vrpace ..." seam (game thread). When the session leaves FOCUSED after
// having held it, presents skip the blocking xrWaitFrame so the flat window
// keeps running while the headset idles; a 5 s keepalive still paces one real
// frame so the runtime can re-grant FOCUSED even if it wants to see frames.
//   on | off          the guard (off = pre-M8 stall behavior, live A/B)
//   thread on|off     xrWaitFrame off the present thread (session 28)
//   detach on|off     SESSION 34: while the session is not FOCUSED, the pace
//                     thread owns the WHOLE frame loop and the present thread
//                     makes no blocking XR call, so the runtime's not-visible
//                     cadence (~10 Hz measured) cannot pace the game. Frames
//                     keep being submitted, so FOCUSED can still be re-granted -
//                     session 28's requirement is preserved, not undone.
//                     Live A/B: with it off the 10 Hz comes straight back.
//   spike on|off      SESSION 43: the spike-triggered evidence capture (see
//                     set_spike_trace below); bare `vrpace spike` = telemetry
//   simidle on|off    flat stand-in for a headset idle: the same guard
//                     decision runs with the state forced VISIBLE and a 1 s
//                     sleep in place of the runtime's blocked wait (flat has
//                     no XR session, so this is how the guard is verified)
//   status            guard state, detach state, skip/handoff/last-wait
//                     telemetry, and the per-phase present-path timings
void handle_pace_command(const char* args);

// --- Session 34: present-detour stage marker ---------------------------------
// The pace trace can only name a stall inside code it wraps. The BS2 stereo
// hang turned out to sit OUTSIDE all of it - our phases had exited and the
// trace stayed silent, which is the same "silence reads as calm" trap the trace
// existed to remove. The Present detour therefore stamps which segment it is
// in; `name` must be a string literal (stored by pointer, read cross-thread).
// Null clears it.
void set_present_stage(const char* name);

// Same, for the GAME/DRAW thread. The BS2 stereo freeze turned out to wedge
// with the present detour fully exited (stage null), i.e. upstream of Present
// entirely - so the draw path needs its own marker or the trace can only say
// "everything stopped" without saying where.
void set_draw_stage(const char* name);

// How many times the stall watchdog has fired this run. Session 35: the trigger
// no longer needs an open draw stage, so this counts wedges in ANY mode - which
// is what lets a soak of vanilla/vrcam/vraer mean something instead of passing
// by construction.
uint32_t watchdog_fires();

// Detached pacing, set by the game adapter at init. DEFAULT OFF in core: the
// project rule is that a core change must not move a BioShock 1 path, and BS1
// is the headset-accepted baseline. The BS2 adapter turns it on; BS1 can opt in
// later on its own in-headset test.
void set_pace_detach(bool on);

// Session 54: THE PACE FEED - keepalives that CARRY LAYERS (the raffle-wedge
// root fix; mechanism in ENGINE_NOTES bioshockinfinite s54). While the session
// is not FOCUSED after having held it, the present thread detaches (feed
// implies detach) and the pace thread runs the frame cycle itself,
// re-submitting the last healthy projection/screen layer with a fresh
// displayTime - so the game is never paced by a parked session AND the runtime
// keeps seeing a rendering app, which is what VDXR re-promotes. DEFAULT OFF IN
// CORE, on per game (the set_pace_detach pattern): BS1/BS2 never call this and
// take no new branch - even the snapshot banking is gated on the flag. The
// Infinite adapter arms it; `vrpace feed on|off` is the live A/B.
void set_pace_feed(bool on);

// Session 42: opt-in pair-rate sync - before OPENING a stereo pair the present
// thread waits for the next tick of a one-period-per-pair schedule (period =
// the runtime's predictedDisplayPeriod, or a `vrpace sync <hz>` override).
// Exists for runtimes whose xrWaitFrame pipelines instead of gating: there the
// pair rate free-runs at the game's present speed and beats against the
// display refresh (the Infinite judder suspect). DEFAULT OFF IN CORE, on per
// game - BS1/BS2 never call this and take no new branch. `vrpace sync` is the
// live A/B; the overlay checkbox rides under SR pair pacing.
void set_pace_sync(bool on);

// Session 43b (the Infinite "jumpy camera"): which locate generation the
// SequentialReentry capture attributes its eyes to. 0 = the fresh locate,
// 1 = one generation back (the historical default - calibrated on BS1's
// single-threaded renderer, where "locate N feeds the tick presenting at
// N+1" holds exactly), 2 = two back (candidate for a threaded ring-buffered
// renderer with OneFrameThreadLag, i.e. Infinite). Mis-attribution by one
// generation = a constant one-period pose error that scales with head speed
// (reprojection wobble). DEFAULT 1 IN CORE - BS1/BS2 never call this and
// behave byte-identically; the Infinite adapter exposes the in-headset A/B.
void set_pose_lag(int lag);
int get_pose_lag();
// Rotation delta between consecutive locate generations (deg) - the error
// magnitude one generation of mis-attribution costs at the current head
// speed. For the adapter's F10 telemetry next to the A/B.
float get_pose_gen_delta_deg();

// Session 43: opt-in spike-triggered evidence capture. When armed, a pair
// interval above 2x the runtime's display period writes one snapshot line to
// pacetrace.log at the pair close: per-phase last + max-since-previous-spike
// tables, the live stage markers, and the unattributed remainder (interval
// minus our detour halves - large remainder = the stall is the game/GPU, not
// this module). The 1 Hz TRACE pairs line carries spikes=<count>/window as the
// lever-A/B metric. DEFAULT OFF IN CORE, on per game (the set_pace_detach
// pattern); `vrpace spike on|off` is the live seam.
void set_spike_trace(bool on);

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

// The headset eye's own HALF-angles in degrees (Quest 3 via VDXR: 54 x 55 - an
// essentially square eye). False until the first xrLocateViews. An adapter uses
// this to report how much of the eye its render actually fills: a 16:9 render
// leaves the vertical short, and that shortfall IS the black bands the user
// sees, not any kind of letterbox.
bool headset_half_fov_deg(float* halfHDeg, float* halfVDeg);

// Session 41: the runtime's recommended per-eye render size, from
// xrEnumerateViewConfigurationViews at bring-up. False (and 0/0) until a
// session has been brought up. Purely informational: core's eye swapchains
// stay backbuffer-sized; adapters use this to annotate resolution pickers
// (the correct-FOV-policy input the docs flagged as missing on BS1/BS2).
bool recommended_eye_size(uint32_t* w, uint32_t* h);

// The adapter reports the horizontal FOV the game is actually rendering with
// (read back from the engine every frame). Projection-layer submission claims
// this value, so claimed fov matches the rendered image even when an engine
// FOV write is clamped or ignored - mismatch there shows up as fisheye or
// binocular-scope distortion in the headset.
void set_rendered_hfov(float hfovDeg);

// --- Session 21: FOV audit ---------------------------------------------------
// The tangents the projection layer was last TAGGED with, plus which source
// produced the claimed hfov (0 = adapter readback, 1 = circumscribed fallback,
// 2 = manual claim slider) and the swapchain dims. Zero/-1 until the first
// projection-layer frame. Read by the `fovaudit` seam command; the flat gate
// compares these against tangents recovered from dumpframe cb0 blocks.
void fov_audit(float* tanH, float* tanV, int* src, unsigned* swapW, unsigned* swapH);

// Pose-tag audit (default off, log-only): per stereo submission (rate-limited)
// log the yaw the layer is TAGGED with against the yaw the game thread last
// CONSUMED from the head-pose funnel. A steady nonzero delta means the image
// is attributed to a pose generation the game never rendered from - the
// next-cheapest suspect for a yaw-dependent lateral drift after the fov.
void set_pose_audit(bool on);

// --- Session 22: cinematic quad fallback --------------------------------------
// The adapter publishes the strict gameplay-view verdict once per CalcView.
// The projection layer drops to the M2 quad screen (3-present hysteresis both
// edges) while ANY of: the verdict reads false (menu attract, scripted view
// actor); the publish goes STALE (a camera path that bypasses CalcView); or
// the live fov watch reports the game rendering a DIFFERENT fov than the
// option (the bathysphere descent: renders 104, option/claim 130 - the
// measured cause of the "fisheye + no fusion" cutscene percept; the scripted
// camera still flows through CalcView there, so only this leg catches it).
// "vrcine on|off|status" is the live A/B; "vrcine mode stereo" keeps the
// projection during fov-mismatch scenes and fixes the CLAIM to the measured
// fov instead (stereo cinematics - opt-in experiment, default quad).
void publish_gameplay_view(bool strictGameplay);
void handle_cine_command(const char* args);

// A full-screen pause interface is up. Published by the adapter from the game's
// own pause field, because the pause menu draws OVER a live world and so is
// invisible to every render-side signal the verdict already has: CalcView keeps
// firing, the view actor is still the pawn, the fov matches, and the frame is
// not pure gameswf. Games that never call this behave exactly as before.
void publish_ui_pause(bool paused);

// Aiming furniture should be hidden right now. Published by the adapter, for the
// same reason publish_ui_pause is: the reasons are all game-side facts no
// render-side signal can see. Consumed ONLY to suppress the aim laser and the
// aim dot. It changes no setting and nothing else reads it, so a game that never
// calls this behaves exactly as before.
//
// The adapter owns the POLICY - core deliberately does not know why. BS1
// publishes it for two reasons: a scripted scene owns the view, or nothing is
// equipped at all (no weapon and no plasmid, so there is nothing to aim).
//
// The game's OWN reticle is not here: it goes through that game's engine SET
// handler, so each adapter hides its own (BS1: camera.cpp assert_crosshair).
void publish_aim_suppressed(bool active);

// --- Screen placement: where the cinema quad hangs ---------------------------
// Applies to every frame that goes to the quad instead of the projection layer:
// menus, the map, the manual, machine flows, the hack board, loading screens
// and cutscenes. `vrscreen anchor mode anchor|head|origin`.
//
//   anchor (default) - world-locked, placed ONCE from the head pose at the
//                      moment the screen opens, yaw flattened so it is never
//                      tilted. It stays put while you look around, which is
//                      what makes it readable, and re-places on the next
//                      screen. Ported from the BRVR mod.
//   head             - rides the view space. Follows your gaze; swims.
//   origin           - world-locked at the recenter origin's forward. The
//                      pre-port default, and the reason a menu could open
//                      behind a player who had turned since recentring.
//
// release_screen_anchor() forces the next screen frame to re-place. Call it
// from a recenter so the screen comes back to the player.
void handle_screen_command(const char* args);
void release_screen_anchor();
int screen_place_mode();
void set_screen_place_mode(int mode);
const char* screen_place_name(int mode);
float screen_height_m();
void set_screen_height_m(float m);
// Quad width in metres. The core DEFAULT is unchanged (2.4); a game opts into a
// different size from its own adapter, the same way screen placement does.
float screen_width_m();
void set_screen_width_m(float m);

// --- Session 29: what the VR rig does while a cinematic holds ----------------
// `vrcine drive off|authored|authored+look` (default authored). The verb is
// `drive` and not `mode` because `vrcine mode` already means quad-vs-stereo,
// and its parser is a strncmp chain that would silently accept a wrong value.
//
//   Off          - no cinematic special-casing: head/hands/aim keep driving
//                  straight through the cutscene, overriding the authored
//                  camera. The pre-session-22 behaviour, kept as the A/B.
//   Authored     - the authored camera and the authored hands play, exactly as
//                  flat. Every VR drive is suspended and the bone drive is
//                  RELEASED (see bones::release - stopping the drive is not the
//                  same as handing the skeleton back).
//   AuthoredLook - authored choreography, but the head may look around: the
//                  head's rotation DELTA since the cutscene began is added on
//                  top of the authored rotation, with no positional offset at
//                  all, so the shot cannot be dollied into geometry and the
//                  authored pitch/roll survives.
enum class CineDrive { Off = 0, Authored = 1, AuthoredLook = 2 };
CineDrive cine_drive();
void set_cine_drive(CineDrive mode);
const char* cine_drive_name(CineDrive mode);
// True while the quad fallback is ACTIVE (render thread state). The adapter
// suppresses the per-eye offsets while it holds (both presents must carry
// the same image or the quad jitters by an IPD - the renderer consumes
// CalcView's camera even in scripted scenes, dump-proven) and skips the live
// head drive (head-steering a scripted camera wobbles the screen content).
bool cinematic_active();
// The hfov (deg) the adapter last read back from the engine's FOV option -
// the projection claim source, and the fov watch's comparison baseline.
float rendered_hfov_deg();

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

// --- s62: the [pair] probe (issue #31, BS2 left-eye double image) -----------
// Cumulative counters over the per-eye present pairing layer - the quantity
// the bone-restamp [flick] instrument does NOT measure. The right-tagged
// present submits BOTH eyes' most-recently-released images, so a pair break
// parks ONE eye on an old image - WHICH eye depends on the break: a break on
// the right present leaves the LEFT stale, but a lone-left break (pass 2
// skipped) captures a fresh left and submits with the RIGHT old. The s62
// premise that every break lands on the left was wrong (found this session);
// watch staleL AND staleR. ageMax tracks the worst observed capture age at
// submit. Read by the BS2 adapter's minute line; purely additive (nothing in
// core prints, BS1/BSI behavior unchanged).
struct PairProbe {
    uint32_t pairs = 0;          // completed L->R pairs
    uint32_t aborts = 0;         // total pair aborts (all kinds)
    uint32_t abortExpired = 0;   //   hold outlived kPairHoldMaxMs
    uint32_t abortLeft = 0;      //   a second LEFT arrived while a pair was open
    uint32_t abortUntagged = 0;  //   an untagged present completed the pair
    uint32_t cap[2] = {0, 0};    // SR captures per eye (L, R)
    uint32_t acqFail = 0;        // xrAcquireSwapchainImage failures
    uint32_t waitFail = 0;       // xrWaitSwapchainImage failures (release still runs)
    uint32_t untaggedProj = 0;   // untagged presents captured in projection mode
    uint32_t rebuilds = 0;       // swapchain destroy/recreate cycles
    uint32_t stereoSubmits = 0;  // submits with both eyes valid
    uint32_t staleL = 0;         // stereo submits with left capture age > 50 ms
    uint32_t staleR = 0;
    uint32_t ageMaxL = 0;        // worst capture age at submit, ms - DRAINED on read
    uint32_t ageMaxR = 0;        //   (window = the caller's own read cadence)
    uint32_t ringPushed = 0, ringPopped = 0, ringDropped = 0, ringCleared = 0;
    uint32_t popDeep = 0;        // pops that found a full pair queued (H3 phase-offset tell)
    bool mirrorOn = false;       // desktop mirror pin state (vrmirror)
};
void pair_probe(PairProbe* out);

// Issue #31 event-edge witness (this session): timestamped one-shot log lines
// at each pairing-break site, so a reported sighting can be matched to a
// mechanism by wall clock. Off by default; only the BS2 adapter arms it.
void set_pair_edge_log(bool on);
bool pair_edge_log();
// The eye sign the CURRENT present popped (-1 left, +1 right, 0 none) - lets
// per-present consumers running after the pop (hud interval roll) attribute
// their interval to an eye.
int sr_last_pop_sign();

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
    // Session 20 muzzle ray: when on, the beam leaves along the RENDERED
    // barrel instead of the trimmed controller forward - direction =
    // (q_ctrl (x) model trim) applied to the barrel axis d0 (XR frame; the
    // game side derives d0 from the driven rig's reference pose each frame).
    // Roll matters for an off-axis vector, so the model ROLL trim rides too.
    bool muzzle = false;
    float muzzleD0[3] = {0.0f, 0.0f, -1.0f};
    float modelPitchTrimDeg = 0.0f;
    float modelYawTrimDeg = 0.0f;
    float modelRollTrimDeg = 0.0f;
};

// Publish the laser state (game thread, once per frame). The render thread
// builds the layers from it at submit time.
void set_laser(const LaserConfig& cfg);

// --- Session 29: the aim dot -----------------------------------------------
// One quad on the ray the BULLET uses, not on a reconstruction of it.
//
// The laser above re-derives its ray on the RENDER thread from the controller
// pose (fresher, but a parallel computation), so beam and bullet agree by
// shared algebra rather than by shared data. The dot takes the other trade
// deliberately: the game thread converts the FINAL fire-seam ray point - the
// same FVector/FRotator that gets written into GetPerfectFireStart - back into
// XR space and publishes the finished point. One frame older than the laser,
// and exactly where the shot starts. That is what makes "fire at a wall, nudge
// the dot onto the bullet hole" an exact calibration rather than a close one.
//
// posXr is already in XR LOCAL space (meters); the render thread only
// billboards it at the head and sizes it. `valid` false = publish nothing,
// which is also how the dot reports that the fire substitution is not live.
struct AimDotConfig {
    bool enabled = false;
    bool valid = false;      // the ray passed the same gate ray_for() applies
    float posXr[3] = {0.0f, 0.0f, 0.0f};
    float sizeDeg = 0.5f;    // angular diameter, like the laser's dots
};

void set_aim_dot(const AimDotConfig& cfg);

// --- Session 40: a SECOND laser + dot slot (additive) -----------------------
// For games whose hands are both active at once - BS2 is natively dual-wield,
// so the weapon hand and the plasmid hand each need their own beam and dot,
// where BS1 only ever has one active hand. Slot 0 is exactly what set_laser /
// set_aim_dot already publish; slot 1 is inert until a game writes it, so a
// game that never calls these submits precisely the layers it did before.
// The dot budget is SHARED (kMaxLaserDots across both beams) rather than
// doubled - the compositor layer arrays do not grow.
void set_laser_slot(int slot, const LaserConfig& cfg);
void set_aim_dot_slot(int slot, const AimDotConfig& cfg);

// Session 19 HUD floating quad placement (meters; head-locked). Persisted by
// the VR preset; sliders in the VR overlay section.
void set_hud_quad(float distM, float widthM, float upM);
void get_hud_quad(float* distM, float* widthM, float* upM);

// s52 (Infinite I9): the HUD quad's texture source, as a provider seam. When
// set, the quad consumer prefers the provider's texture; null (the default,
// and a null provider RESULT) falls through to bvr::hud::texture() - BS1's
// path, byte-identical for games that never call this.
using HudTextureProviderFn = ID3D11Texture2D* (*)(ID3D11DeviceContext* ctx);
void set_hud_texture_provider(HudTextureProviderFn fn);

// Session 33: the session's state as a short string ("FOCUSED", "SYNCHRONIZED",
// "none"...) and whether it has EVER been FOCUSED. Cheap, for a heartbeat.
//
// This exists because "the game hangs a few seconds after enabling VR" took an
// hour to attribute, and the answer was one line of state: a running session
// that never reached FOCUSED still PACES the game, and the runtime's
// not-visible cadence is around 10 Hz. Nothing was blocked - lastWait 0 ms,
// timeouts 0 - so every wedge-shaped hypothesis was wrong. Put the state where
// it is read every second and the next person spends a second on it.
const char* session_state_name();
bool ever_focused();

} // namespace bvr::vr
