// Port of the proven xr_hello32 flow (DR-1) into the game process. The big
// differences from the standalone probe: the session binds the game's own
// ID3D11Device (grabbed from the hooked swapchain), bring-up is lazy and
// retried on a cooldown from the render thread, and each frame submits one
// quad layer containing a copy of the game backbuffer.

#include "core/vr/openxr_runtime.h"

#include "core/gfx/blit.h"
#include "core/gfx/hud_capture.h"
#include "core/util/log.h"
#include "core/util/xr_math.h"

#ifdef BVR_WITH_OPENXR

#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D11

#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include "core/vr/openxr_input.h"

#include <imgui.h>

#include <tlhelp32.h>
#include <shlobj.h>
#include <share.h>
#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>
#include <vector>

namespace bvr::vr {
namespace {

XrInstance g_instance = XR_NULL_HANDLE;
char g_runtimeName[XR_MAX_RUNTIME_NAME_SIZE] = "none";
XrSystemId g_system = XR_NULL_SYSTEM_ID;
XrSession g_session = XR_NULL_HANDLE;
XrSpace g_space = XR_NULL_HANDLE;
XrSpace g_viewSpace = XR_NULL_HANDLE;
XrSessionState g_state = XR_SESSION_STATE_UNKNOWN;
std::atomic<bool> g_sessionBegun{false}; // read from the game thread via vr_camera_mode()
bool g_frameOpen = false;
XrFrameState g_frameState{XR_TYPE_FRAME_STATE};

// Two identical backbuffer-sized swapchains: index 0 serves the quad screen
// and mono projection (and the left eye under AlternateEye), index 1 exists
// only for the AlternateEye right eye. Both live and die together.
XrSwapchain g_swapchains[2] = {XR_NULL_HANDLE, XR_NULL_HANDLE};
std::vector<XrSwapchainImageD3D11KHR> g_images[2];
uint32_t g_swapW = 0, g_swapH = 0;
uint32_t g_backbufferFmt = 0; // DXGI format the live swapchains were built for
// Set by on_resize (which runs inside the game's ResizeBuffersDetour, at an
// arbitrary point in the frame) and consumed by the frame loop at a point where
// no XR frame is open. See the long note in on_resize.
std::atomic<bool> g_resizePending{false};

ID3D11Device* g_device = nullptr;          // game device, AddRef'd
ID3D11DeviceContext* g_context = nullptr;  // immediate context, AddRef'd

// M7 aim laser. One tiny swapchain holding a soft dot, drawn as several quad
// layers along the aim ray. Runtimes are only required to accept 16 layers, so
// the dot count is capped well under that with the game's own layer included.
constexpr int kMaxLaserDots = 8;
constexpr uint32_t kLaserTexSize = 64;
XrSwapchain g_laserSwapchain = XR_NULL_HANDLE;
std::vector<XrSwapchainImageD3D11KHR> g_laserImages;
ID3D11Texture2D* g_laserDot = nullptr; // CPU-generated source, copied in per frame
std::atomic<bool> g_laserOn{false};
std::atomic<int> g_laserHand{1};
std::atomic<float> g_laserPitchTrim{0.0f}, g_laserYawTrim{0.0f};
std::atomic<float> g_laserPosFwdCm{0.0f}, g_laserPosRightCm{0.0f}, g_laserPosUpCm{0.0f};
std::atomic<int> g_laserDots{6};
std::atomic<float> g_laserNearM{0.30f}, g_laserFarM{6.0f}, g_laserSizeDeg{0.7f};
// Session 20 muzzle ray (see LaserConfig): beam along the rendered barrel.
std::atomic<bool> g_laserMuzzle{false};
std::atomic<float> g_laserMuzzleD0[3] = {0.0f, 0.0f, -1.0f};
std::atomic<float> g_laserModelPitchTrim{0.0f}, g_laserModelYawTrim{0.0f},
    g_laserModelRollTrim{0.0f};
std::atomic<uint32_t> g_laserLayersSubmitted{0};
std::atomic<bool> g_loggedFirstLaser{false};

// Session 40: an ADDITIVE second laser + dot slot, for games whose hands are
// both active at once (BS2's native dual-wield). Nothing writes these unless a
// game calls set_laser_slot(1, ...) / set_aim_dot_slot(1, ...), so BS1's
// submit path is unchanged - it sees `on == false` and builds zero layers.
// The layer budget is shared, not grown: both beams together are still capped
// at kMaxLaserDots quads, so the worst case stays 1 projection + 8 laser +
// 2 dots + 1 HUD = 12 of the 16 layers a runtime must accept.
std::atomic<bool> g_laser2On{false};
std::atomic<int> g_laser2Hand{0};
std::atomic<float> g_laser2PitchTrim{0.0f}, g_laser2YawTrim{0.0f};
std::atomic<float> g_laser2PosFwdCm{0.0f}, g_laser2PosRightCm{0.0f}, g_laser2PosUpCm{0.0f};
std::atomic<int> g_laser2Dots{4};
std::atomic<float> g_laser2NearM{0.30f}, g_laser2FarM{6.0f}, g_laser2SizeDeg{0.7f};

// Session 29 aim dot: one more quad off the SAME tiny swapchain, positioned
// from a point the game thread already converted into XR space (see
// AimDotConfig). Stamped so a stale publish cannot leave a dot hanging in the
// world after the ray stops being substituted.
std::atomic<bool> g_dotOn{false};
std::atomic<bool> g_dotValid{false};
std::atomic<float> g_dotX{0.0f}, g_dotY{0.0f}, g_dotZ{0.0f};
std::atomic<float> g_dotSizeDeg{0.5f};
std::atomic<uint64_t> g_dotStampMs{0};
std::atomic<uint32_t> g_dotLayersSubmitted{0};
std::atomic<bool> g_loggedFirstDot{false};
// Second dot slot (session 40, same additive rule as the second laser).
std::atomic<bool> g_dot2On{false};
std::atomic<bool> g_dot2Valid{false};
std::atomic<float> g_dot2X{0.0f}, g_dot2Y{0.0f}, g_dot2Z{0.0f};
std::atomic<float> g_dot2SizeDeg{0.5f};
std::atomic<uint64_t> g_dot2StampMs{0};
constexpr uint64_t kDotStaleMs = 250; // matches aim.cpp's ray_for() freshness gate
// s51 (Infinite FOV-edge discriminator): a reference quad parked AT the
// located grip pose, compositor-correct by construction - core positions it
// per present from the SAME locate the projection layer uses, no game-thread
// hop. In the headset: if this quad and the rendered hand model separate
// while the hand sweeps off-center, the error is in the game-render/
// projection/submission lane; if they move together, the hand's composed
// world position itself is wrong. Default OFF; armed by the adapter.
std::atomic<bool> g_handQuadOn{false};
std::atomic<int> g_handQuadHand{1}; // 0 = left, 1 = right
std::atomic<float> g_handQuadSizeDeg{1.5f};
std::atomic<bool> g_loggedFirstHandQuad{false};
// s51: the VDXR view logger's remaining-frames counter (bounded burst).
std::atomic<int> g_viewLogLeft{0};
// s51: the edge-telemetry snapshot (armed only; game thread copies out).
std::atomic<bool> g_edgeSnapOn{false};
std::mutex g_edgeSnapMutex;
EdgeViewSnapshot g_edgeSnap;

// Controls (overlay writes, render thread reads).
std::atomic<bool> g_enabled{true};        // kill switch: tears the session down
std::atomic<float> g_screenDistM{1.75f};  // quad distance in meters
std::atomic<float> g_screenWidthM{2.4f};  // quad width in meters
std::atomic<bool> g_cameraMode{false};    // M3: drive the game camera from the HMD

// Session 19 HUD floating quad: the gameswf HUD captured by core/gfx/
// hud_capture is copied into its own swapchain and composited head-locked
// (g_viewSpace) during stereo gameplay. Sliders persist via vrpreset.ini.
XrSwapchain g_hudSwapchain = XR_NULL_HANDLE;
std::vector<XrSwapchainImageD3D11KHR> g_hudImages;
uint32_t g_hudSwapW = 0, g_hudSwapH = 0;
int64_t g_swapFormat = 0; // the format create_swapchains picked (lazy HUD create)
std::atomic<float> g_hudDistM{1.30f};
std::atomic<float> g_hudWidthM{1.25f};
std::atomic<float> g_hudUpM{-0.10f};
std::atomic<uint32_t> g_hudFramesSubmitted{0};
std::atomic<bool> g_loggedFirstHudQuad{false};
// s52: the HUD quad's texture provider (Infinite's GFx lane). Null = BS1's
// bvr::hud::texture() path, byte-identical for games that never set it.
std::atomic<HudTextureProviderFn> g_hudTexProvider{nullptr};

// Cached backbuffer RTV for the post-capture window HUD composite.
ID3D11RenderTargetView* g_backbufferRtv = nullptr;
ID3D11Texture2D* g_backbufferForRtv = nullptr; // identity only, never deref'd

// Bring-up retry (render thread only).
uint64_t g_nextRetryMs = 0;
bool g_loggedNoHmd = false;
uint32_t g_framesSubmitted = 0;

// M3 pose plumbing. The head pose crosses to the game thread (mutex); the
// per-eye views are only used on the render thread for layer submission.
std::mutex g_poseMutex;
HeadPose g_headPose{};
bool g_poseValid = false;
XrView g_views[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
// The previous locate's views = the generation the CURRENTLY-presented game
// content was rendered from (see the copy in on_present_begin).
XrView g_viewsContent[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
bool g_viewsContentValid = false;
bool g_viewsValid = false;
// Session 43b (the Infinite "jumpy camera"): the lockstep assumption behind
// g_viewsContent - "locate N feeds the tick that presents at N+1" - was
// calibrated on BS1's 1T (single-threaded) renderer. Infinite's substrate is
// threaded and ring-buffered with OneFrameThreadLag, so its content may lag
// the locate by TWO generations; attributing it one-back leaves a constant
// one-period pose error that scales with head speed (reprojection wobble -
// the reported percept). The selector below picks which generation the SR
// capture attributes: 0 = fresh (g_views), 1 = one back (g_viewsContent,
// THE DEFAULT = today's behavior, BS1/BS2 never change it), 2 = two back
// (g_viewsPrev2). The in-headset A/B is the discriminator: whichever lag
// kills the wobble names the pipeline's true depth by intervention.
XrView g_viewsPrev2[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
bool g_viewsPrev2Valid = false;
std::atomic<int> g_poseLag{1};
// Telemetry: yaw delta between consecutive locate generations (deg) - the
// size of the attribution error one generation of lag would cause at the
// current head speed. Read by the adapter's F10 section.
std::atomic<float> g_poseGenDeltaDeg{0.0f};
std::atomic<float> g_hfovDeg{0.0f};      // circumscribed symmetric hfov, read cross-thread
// Session 34: the headset eye's own half-angles in degrees (0 until the first
// locate). Exposed so an adapter can say, on screen, how much of the eye its
// render is actually filling - which is the whole diagnosis of "black bars".
std::atomic<float> g_headsetHalfH{0.0f}, g_headsetHalfV{0.0f};
// Session 41: the runtime's recommended per-eye render size, from
// xrEnumerateViewConfigurationViews at bring-up (0 until then). Purely
// informational - nothing in core reads these; swapchain sizing stays
// backbuffer-derived. Exposed for adapters' resolution pickers.
std::atomic<uint32_t> g_recommendedEyeW{0}, g_recommendedEyeH{0};
std::atomic<float> g_renderedHfov{0.0f}; // fov the game actually rendered (adapter readback)
// Distortion calibration: the readback reads the same engine address we write,
// so under forcing it echoes our own value and cannot see how the RENDERER
// interprets it (vfov? 4:3-referenced? clamped downstream?). This manual
// override claims an arbitrary hfov instead; in-headset, the world stops
// warping on head rotation exactly when the claim matches what the engine
// truly rendered - the locked slider value measures the real fov.
std::atomic<bool> g_claimFovManual{false};
std::atomic<float> g_claimFovDeg{100.0f};
// True only when everything the projection layer needs is in place (views
// located, fov known, swapchain alive). The camera drive is gated on this so
// a head-driven camera can never appear on the flat quad screen.
std::atomic<bool> g_projectionReady{false};
int g_lastLayer = 0; // 0 none, 1 quad, 2 projection (render thread only)

// Session 22 cinematic fallback: the adapter's strict gameplay-view verdict,
// packed {tickMs << 1 | strict} so one relaxed load is coherent (a two-atomic
// pair would race between value and stamp). Zero = never published. Staleness
// IS the cutscene signal: scripted cameras bypass CalcView, so the publisher
// simply stops. The 300 ms render threshold sits BELOW the game side's 400 ms
// FOV restore on purpose - the quad showing a 130-FOV image for ~100 ms is
// invisible; a projection layer over a restored-75 render would not be.
std::atomic<uint64_t> g_gameplayView{0};
std::atomic<bool> g_cineEnabled{true};
std::atomic<bool> g_cineActive{false}; // written by the render thread only
// "vrcine mode stereo": during fov-mismatch scenes keep the projection and
// claim the MEASURED fov instead of the option (stereo cinematics with
// head-look; screen-only and strict-false/stale intervals still drop to the
// quad - a 2D board has no stereo content). DEFAULT per the user's call
// 2026-07-29: stereo; "vrcine mode quad" / the overlay toggle is the A/B.
std::atomic<bool> g_cineStereo{true};
// Session 29: what the VR rig does while a cinematic holds (see CineDrive).
// Default Authored - the authored camera and hands play exactly as flat.
std::atomic<int> g_cineDrive{static_cast<int>(CineDrive::Authored)};
int g_cineStreak = 0;                  // render thread only (hysteresis)
std::atomic<uint32_t> g_cineEnters{0}, g_cineExits{0}, g_cinePresents{0};
constexpr uint64_t kCineStaleMs = 300;
constexpr int kCineHysteresis = 3;
std::atomic<bool> g_loggedFirstProjection{false};
std::atomic<bool> g_loggedFirstStereo{false};
uint64_t g_lastProjBlockedLogMs = 0;

// Session-21 FOV audit: the tangents the projection layer was last TAGGED
// with and which source produced the claimed hfov. Logged on change at the
// submission site and readable by the game thread's `fovaudit` command; the
// flat gate compares these against tangents recovered from dumpframe cb0
// blocks. The pose audit (default off) additionally logs the yaw the layer
// is tagged with vs the yaw the game thread last consumed from the head-pose
// funnel - a generation-skew instrument, in-headset only (flat has no session).
constexpr const char* kFovSrcNames[4] = {"readback", "fallback", "manual", "live"};
std::atomic<float> g_auditTanH{0.0f};
std::atomic<float> g_auditTanV{0.0f};
std::atomic<int> g_auditFovSrc{-1};
std::atomic<bool> g_poseAudit{false};
std::atomic<float> g_consumedHeadQuat[4] = {}; // x,y,z,w - game-thread stamp
std::atomic<uint32_t> g_consumedHeadCount{0};
uint64_t g_lastPoseAuditLogMs = 0; // render thread only

// M4 rung 1: AlternateEye stereo (AER). The render thread owns everything here
// except g_aerEyeSign, which CalcView on the game thread reads through
// current_eye_sign() to pick the per-frame eye offset. The sign also encodes
// which offset is baked into the NEXT backbuffer we will see: it is published
// at Present-tail and consumed by the following game frame's CalcView.
std::atomic<bool> g_aerEnabled{false};   // overlay checkbox
std::atomic<bool> g_aerSwapEyes{false};  // diagnostic: negate the sign (inverted-depth test)
std::atomic<int> g_aerEyeSign{0};        // -1 left, +1 right, 0 = AER off
int g_currentEye = 0;                    // eye slot the next captured frame belongs to
XrPosef g_eyePose[2] = {};               // pose claimed for each eye's held image
bool g_eyeValid[2] = {false, false};     // eye slot holds a released image + pose

// s50 (Infinite): rendered-pose eye tags - see the header comment. Default
// OFF; only an adapter arms it (additive, BS1/BS2 untouched). The ipd mirror
// tracks the arming adapter's slider so the tag reconstructs the SAME offset
// apply_eye_offset baked into the render.
std::atomic<bool> g_eyeTagRendered{false};
std::atomic<float> g_eyeTagIpdMm{63.0f};

// Rebuild one eye's layer tag as the PARALLEL camera the game rendered:
// midpoint of the located pair, nlerp'd shared orientation, +-ipd/2 along
// that orientation's right axis (eye 0 = left = -x, matching the adapter's
// apply_eye_offset signs). Identity when the located pair is already
// parallel at the same IPD.
XrPosef parallel_eye_tag(const XrPosef& l, const XrPosef& r, int eye, float ipdMm) {
    float q[4] = {l.orientation.x + r.orientation.x, l.orientation.y + r.orientation.y,
                  l.orientation.z + r.orientation.z, l.orientation.w + r.orientation.w};
    // nlerp at t=0.5 with hemisphere guard: located pairs are near-identical,
    // but a sign-flipped quat pair would cancel - fall back to the left eye.
    const float dot = l.orientation.x * r.orientation.x + l.orientation.y * r.orientation.y +
                      l.orientation.z * r.orientation.z + l.orientation.w * r.orientation.w;
    if (dot < 0.0f) {
        q[0] = l.orientation.x;
        q[1] = l.orientation.y;
        q[2] = l.orientation.z;
        q[3] = l.orientation.w;
    }
    const float n = sqrtf(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    if (n > 1e-6f) {
        q[0] /= n;
        q[1] /= n;
        q[2] /= n;
        q[3] /= n;
    } else {
        q[0] = q[1] = q[2] = 0.0f;
        q[3] = 1.0f;
    }
    const float half = (eye == 0 ? -0.5f : 0.5f) * ipdMm / 1000.0f;
    const float axis[3] = {half, 0.0f, 0.0f}; // XR +X = right in view space
    float off[3];
    bvr::xrmath::quat_rotate(q[0], q[1], q[2], q[3], axis, off);
    XrPosef out{};
    out.orientation = {q[0], q[1], q[2], q[3]};
    out.position = {(l.position.x + r.position.x) * 0.5f + off[0],
                    (l.position.y + r.position.y) * 0.5f + off[1],
                    (l.position.z + r.position.z) * 0.5f + off[2]};
    return out;
}

// M4 rung 2 (SequentialReentry): SPSC eye-tag ring, game thread pushes at
// engine submit, render thread pops at Present-tail (see header). Normal
// depth is <= 2 (one L/R pair in flight); deeper means a skew - the consumer
// clears it. Ring slots hold the eye sign; indices are monotonic.
constexpr uint32_t kSrRingSize = 8; // power of two
std::atomic<uint32_t> g_srHead{0};  // push cursor (game thread)
std::atomic<uint32_t> g_srTail{0};  // pop cursor (render thread)
std::atomic<int8_t> g_srRing[kSrRingSize] = {};
std::atomic<uint32_t> g_srPushed{0}, g_srPopped{0}, g_srDropped{0},
    g_srCleared{0};
std::atomic<bool> g_loggedFirstSr{false};
// Session 28: a live-lens claim substitution must announce itself once. An
// unexplained src=live is what let the yaw warp hide for a session.
std::atomic<bool> g_loggedLiveClaim{false};

// M4 rung 2 polish (session 7, after the first in-headset stereo test):
// xr-frame-per-pair pacing. Per-present xrWaitFrame gave the two presents of
// one stereo pair SEPARATE predicted display times (~one compositor period
// apart), so the pair was submitted with poses located at two different
// times while both images were rendered from ONE head sample - under head
// motion the compositor reprojected the eyes inconsistently (user report:
// eyes feel weird on head movement), and the second blocking wait halved the
// game tick. With pair pacing, a LEFT-tagged present leaves the XR frame
// OPEN (captures its eye, no submit/end) and the RIGHT-tagged present
// completes it: one waitFrame, one locate, one consistent pose pair, one
// blocking wait per game tick. Kill switch in the overlay for live A/B.
std::atomic<bool> g_srPairPacing{true};
bool g_srPairOpen = false; // present thread only
uint64_t g_srPairOpenMs = 0; // when the hold was armed (present thread only)
uint64_t g_lastIdleLogMs = 0; // rate limit for the SUBMISSION IDLE heartbeat
// Session 28: the hold is a bounded promise, not a latch. The right eye follows
// its sibling within the time the game takes to BUILD one frame - ~1-4 ms
// measured, and the whole pair fits inside one compositor period. 500 ms is
// three orders of magnitude of slack and still bounds the alt-tab case, where
// presents stop mid-pair and no sibling is ever coming.
constexpr uint32_t kPairHoldMaxMs = 500;
std::atomic<uint32_t> g_srPairs{0}, g_srPairAborts{0};
std::atomic<bool> g_loggedFirstPair{false};

// Session 42 (Infinite I6 judder): pair-CADENCE statistics. The judder question
// is not "how many pairs per second" but "how EVENLY are they spaced" - a mean
// of 13.9 ms with 5 ms of swing reprojects differently every frame while both
// numbers round to 72/s. Present thread writes at each pair CLOSE; the 1 Hz
// trace thread reads, derives mean/stddev and window-resets min/max (the reset
// races lose at most one sample - diagnostics, not accounting). Samples above
// 1 s are discarded: a load screen or alt-tab would otherwise poison sumsq
// for the whole window.
std::atomic<int64_t> g_pairLastCloseQpc{0};
std::atomic<uint32_t> g_pairIntCount{0};
std::atomic<uint64_t> g_pairIntSumUs{0};
std::atomic<uint64_t> g_pairIntSumSqUs{0};
std::atomic<uint32_t> g_pairIntMinUs{0xFFFFFFFFu};
std::atomic<uint32_t> g_pairIntMaxUs{0};
// How long the present thread actually spent BLOCKED in the wait handoff, per
// trace window. This is the gating discriminator the pairs/s number cannot
// give: free-running pairs spend ~0 ms/s here; pairs gated by xrWaitFrame
// spend the whole non-render remainder of each second here.
std::atomic<uint64_t> g_pairWaitSumUs{0};
// The runtime's own frame period from xrWaitFrame - never consumed anywhere
// in this codebase until now (only the sim ever WROTE it). It is the refresh
// input a pace sync needs, published so the fix can target measured reality.
std::atomic<int64_t> g_displayPeriodNs{0};

// Session 43 (Infinite stutter hunt): SPIKE-TRIGGERED EVIDENCE CAPTURE.
// The s42 headset run named the judder as 39-113 ms pair intervals in bursts;
// the 1 Hz aggregates can say THAT a second was bad, never WHICH phase carried
// the stall. When armed, a pair interval above 2x the display period writes a
// one-line snapshot to pacetrace.log at the moment of the pair close: the
// per-phase last table, the per-phase maxima SINCE THE PREVIOUS SPIKE (reset
// after each snapshot - that is what scopes the attribution to this burst),
// the live stage markers, and the unattributed remainder (interval minus our
// two detour halves - large remainder = the stall is the game/GPU, not us).
// DEFAULT OFF IN CORE (the set_pace_detach pattern): BS1/BS2 never arm it and
// the disarmed check is one relaxed load on a path that already samples QPC.
// The Infinite adapter arms it with stereo; `vrpace spike on|off` is the seam.
std::atomic<bool> g_spikeTrace{false};
std::atomic<uint32_t> g_spikeCount{0};   // lifetime; TRACE pairs prints the window delta
std::atomic<uint32_t> g_spikeSeq{0};     // snapshot sequence number in the log
// The IN-PROGRESS sampler (s43 escalation): the pair-close snapshot proved the
// stall lives OUTSIDE our phases (unattributed 27-340 ms of every wander
// spike), so the next question is WHICH ENGINE FUNCTION - answerable only by
// catching the stalled thread mid-stall. A 4 ms poller watches the age of the
// last pair close; once it exceeds the sample threshold (2.5x period) with no
// new close, it stack-captures the draw thread (and the rest, via the s34
// watchdog machinery) ONCE per episode, rate-limited. RVAs go to
// pacetrace.log; tools/disasm-rva.py turns them into engine names.
std::atomic<uint32_t> g_spikeStacks{0};        // captures taken (telemetry + cap)
constexpr uint32_t kSpikeStackMax = 40;        // per-session log bound
HANDLE g_spikeSamplerThread = nullptr;

// M8 release blocker (a): the headset-disconnect stall guard. When the
// headset idles, the runtime drops the session out of FOCUSED and xrWaitFrame
// starts blocking for seconds per call, dragging the flat window under 1 fps
// (log signature: presents=0/s, `xr: session state VISIBLE`). Once a session
// has been FOCUSED, losing FOCUSED switches pacing to SKIP: presents stop
// calling the blocking wait entirely and the game runs free, while
// pump_events keeps running every present so the return to FOCUSED is acted
// on immediately.
// The guard NEVER engages before the first FOCUSED: during bring-up
// (SYNCHRONIZED -> VISIBLE -> FOCUSED) the runtime needs submitted frames to
// advance its own state machine, so a naive "skip whenever not FOCUSED"
// would deadlock session start.
//
// Session 26 - THE LOW-CADENCE KEEPALIVE IS GONE, and removing it is a HANG
// FIX, not a tuning change. It used to let one real paced frame through every
// 5 s while unfocused, as insurance in case a runtime wanted to see frames
// before re-granting FOCUSED. But xrWaitFrame takes no timeout, so that one
// frame is an UNBOUNDED block, and it bit in the field (2026-07-29, BS2 in
// stereo, VDXR): the session dropped to VISIBLE with the headset idle, the
// FIRST keepalive fired immediately (g_nextKeepaliveMs started at 0) and
// never returned - the render thread stalled inside Present, back-pressured
// the game thread through the command ring, and the process wedged at 0 CPU
// with 'Not Responding' (kill required, no crash, no dump). Recovery never
// depended on the keepalive: pump_events runs every present ABOVE this guard
// and the return to FOCUSED arrives as a session EVENT, not something the app
// earns by submitting frames. Worst case without it is "stays unfocused",
// which is visible, recoverable and infinitely better than a wedged game.
// `vrpace off` still restores the old always-pace behavior for A/B.
std::atomic<bool> g_paceGuard{true};      // A/B knob (`vrpace off` = old behavior)
std::atomic<bool> g_everFocused{false};   // written on the present thread
uint64_t g_unfocusedSinceMs = 0;          // present thread only; 0 = not skipping
std::atomic<uint32_t> g_paceSkips{0};
std::atomic<uint32_t> g_lastWaitMs{0}; // last xrWaitFrame block, telemetry
std::atomic<bool> g_simIdle{false};    // flat stand-in (`vrpace simidle on`)

// ---- Session 28: xrWaitFrame OFF the present thread ------------------------
// The alt-tab freeze, measured. Alt-tab drops the session FOCUSED -> VISIBLE and
// VDXR never re-grants FOCUSED, because the M8 guard makes us submit NOTHING
// while unfocused and a runtime will not promote an app that submits nothing.
// That is a circular wait, and the log named it outright: `SUBMISSION IDLE
// (reason=pace guard: session not FOCUSED | state=VISIBLE ... pairOpen=0)`
// repeating with no FOCUSED line until the VR toggle tore the session down. It
// also refutes the session-26 claim that "recovery is event-driven, not
// something an app earns by submitting frames" - on VDXR it IS earned.
//
// We cannot simply keep waiting while unfocused: xrWaitFrame takes no timeout,
// with the headset idle it never returns, and on the present thread that wedged
// the process in the field twice. The only design that satisfies both is to move
// the unbounded call OFF the present thread, so the present thread can give up
// on a wait without being stuck by it. Corroboration from the same log: ZERO
// `xrWaitFrame blocked` lines all session, so 5772 presents were skipped without
// a single slow wait to justify it - the guard keys on session STATE when the
// thing it must actually avoid is a slow WAIT.
//
// Protocol - strictly ONE wait per begin, so the frame sequence stays matched:
//   present thread  no request outstanding -> post one; then wait on `done` with
//                   a deadline. Signalled -> consume the frame state and go on to
//                   xrBeginFrame. Timed out -> return, leaving the request
//                   outstanding to be consumed by a later present.
//   pace thread     park on `req`, call xrWaitFrame, publish, signal `done`.
// The deadline is generous while FOCUSED (the headset still paces the game) and
// short otherwise (an unresponsive runtime must not drag the flat window down).
// `vrpace thread off` restores the exact pre-session-28 inline behaviour.
std::atomic<bool> g_paceOffThread{true};
HANDLE g_paceThread = nullptr;
HANDLE g_paceReq = nullptr;  // auto-reset, present -> pace
HANDLE g_paceDone = nullptr; // auto-reset, pace -> present
std::atomic<bool> g_paceRun{false};
bool g_paceOutstanding = false; // present thread only
XrFrameState g_paceFrameState{XR_TYPE_FRAME_STATE}; // handed over via g_paceDone
std::atomic<int> g_paceResult{0};
std::atomic<uint32_t> g_paceTimeouts{0}, g_paceHandoffs{0};
// Session 54: the pace-thread request now has a KIND, written by the present
// thread before SetEvent(g_paceReq) and echoed back in g_paceResKind with the
// result. Wait = the classic single xrWaitFrame. FeedCycle = a whole
// wait+begin+end cycle submitting the feed snapshot (see THE PACE FEED below).
// FeedFinish = begin+end from the ALREADY-banked wait result - needed because
// a wait that completed just before an episode began must still be matched by
// a begin (per spec the next xrWaitFrame blocks until the previous frame is
// begun), and the present thread is detached and must not do it inline.
constexpr int kPaceReqWait = 0;
constexpr int kPaceReqFeedCycle = 1;
constexpr int kPaceReqFeedFinish = 2;
// s54b: close an already-begun frame (the pair-hold spanning a demote) with an
// empty xrEndFrame ON THE PACE THREAD. The 2026-08-11 doff freeze taught the
// lesson: at the demote edge the present thread must make NO blocking XR call
// at all - the inline idle-close was the one such call left on the detach
// path, and a doff-teardown VDXR can sit inside xrEndFrame indefinitely.
constexpr int kPaceReqCloseOpen = 3;
std::atomic<int> g_paceReqKind{kPaceReqWait};
std::atomic<int> g_paceResKind{kPaceReqWait};
std::atomic<int64_t> g_feedCloseTimeNs{0}; // displayTime for a CloseOpen request
// Teardown deferral: destroying a session while the pace thread is parked inside
// xrWaitFrame on it is a use-after-free inside the runtime. If a wait will not
// come back, we keep the session alive (which is exactly today's behaviour) and
// retry from the present loop rather than crash.
const char* g_teardownPending = nullptr;
constexpr uint32_t kPaceDeadlineFocusedMs = 200;
constexpr uint32_t kPaceDeadlineIdleMs = 20;

// ---- Session 34: DETACHED PACING - an unfocused session must not own the
// ---- game thread's frame loop ------------------------------------------------
// Session 28 stopped SKIPPING frames while unfocused, and that reasoning holds:
// a runtime will not re-grant FOCUSED to an app that submits nothing, so
// skipping is what made the alt-tab freeze permanent. What it missed is that
// "not blocked" is not "not harmed" - the present thread still walked the whole
// frame loop at the runtime's not-visible cadence (~10 Hz measured), and the
// game inherited it. In a headset that reads as a freeze; flat it reads as
// `draws/s 10`.
//
// The fix keeps both properties by SPLITTING THE TWO DUTIES onto two threads:
//   submission  the pace thread runs the entire frame loop by itself -
//               xrWaitFrame -> xrBeginFrame -> xrEndFrame with zero layers -
//               so the app keeps a live frame loop and can earn FOCUSED back.
//   pacing      the present thread makes NO blocking XR call at all, so
//               whatever the runtime does to its own cadence cannot reach the
//               game thread. That is deliberately phase-agnostic: it holds
//               whether the blocking call is xrWaitFrame, xrWaitSwapchainImage
//               (which waits XR_INFINITE_DURATION) or xrEndFrame.
// Zero layers is the honest submission here: while merely SYNCHRONIZED,
// shouldRender is false and there is nothing to show anyway. While VISIBLE it
// means the headset image holds still during the seconds the desktop has focus,
// which is the alt-tab-to-type case this exists to fix.
//
// WHICH CALL BLOCKS - MEASURED, not inferred (session 34, and the whole reason
// the phase timers went in first). At the FOCUSED -> VISIBLE transition:
//
//   presentBegin=152417 presentEnd=101853 wait=10782 beginFrame=29 locate=15
//   acquire=11 capture=4 endFrame=101847 composite=47      (max us)
//
// **xrEndFrame is the pacer at 101.8 ms** - one not-visible period, matching
// the 10 Hz and `call2Us 99765` exactly. NOT the frame handoff, which session
// 33 named and which maxes at 10.8 ms; not xrWaitSwapchainImage at 11 us.
//
// WHY THE FRAME LOOP IS NOT MOVED OFF THE PRESENT THREAD - a dead end, recorded
// so nobody re-walks it. The first cut of this fix had the pace thread run the
// whole loop (wait/begin/end, zero layers) while unfocused. It works, and the
// log shows it handing over and back cleanly - but it CRASHED the game nine
// milliseconds into the second detach: a write to NULL inside game code, then a
// chained handler retrying the faulting instruction 86000 times. The reason is
// structural: xrEndFrame touches the D3D11 device, the present thread keeps
// running composite_hud/mirror_present on the same IMMEDIATE CONTEXT, and a
// D3D11 immediate context is not thread-safe. Session 28 got away with moving
// xrWaitFrame off-thread precisely because that call touches no D3D. Doing this
// properly needs the XR session on its own device with shared textures, which
// is a bigger change than a pacing fix should carry.
//
// THE RATE-LIMITED KEEPALIVE IS ALSO A DEAD END, and this one was measured in
// the field rather than reasoned about. Running the frame loop once a second
// while unfocused survived four intervals and wedged on the fifth:
//
//   19:45:09 VISIBLE -> DETACHED (keepalive 1000 ms)
//   19:45:09/10/11/12  SUBMISSION IDLE, frames=5896   <- alive, one loop per second
//   19:45:13           nothing, ever again; present thread blocked in a wait
//
// So while unfocused xrEndFrame does not block for ~100 ms - it eventually
// blocks FOREVER, exactly as session 28 found for xrWaitFrame with the headset
// idle. Rate-limiting only chooses which call wedges.
//
// WHAT SHIPS: while the session is not FOCUSED the present thread makes NO
// blocking OpenXR call at all - no wait, no begin, no end. Recovery is by
// EVENT: pump_events runs first thing every present, is non-blocking, and needs
// no submission to see FOCUSED come back.
//
// This looks like session 28's rejected "skip", and the difference matters:
// session 28 rejected it because FOCUSED never returned, but its real bug was
// that the pair-hold returned ABOVE pump_events so no events were polled at
// all - fixed since, separately. Tonight's log settles it directly: FOCUSED was
// re-granted after an episode of 755 unpaced presents carrying just 4 submitted
// frames, so this runtime plainly does not require a frame stream to hand focus
// back. `vrpace keepalive <ms>` can put submission back for anyone whose
// runtime does; it defaults to 0 because 0 is the setting that survived.
//
// DEFAULT OFF IN CORE, ON PER GAME. BioShock 1 is the headset-accepted baseline
// and the project rule is that a core change must not move a BS1 path; the BS2
// adapter opts in via set_pace_detach(), BS1 keeps byte-identical behaviour and
// can opt in later on its own test. `vrpace detach on|off` is the live A/B, and
// proving causation with it is part of the acceptance.
std::atomic<bool> g_paceDetach{false};
// While unfocused, run the frame loop at most this often. The whole cost of an
// unfocused session is now one blocking xrEndFrame per interval instead of one
// per present. Tunable live (`vrpace keepalive <ms>`) because the right value
// is a judgement about hitch-versus-recovery that only the headset can settle.
// 0 = NEVER touch the frame loop while unfocused, which is the default and the
// only setting shown to survive. Non-zero is kept as an experiment lever only.
std::atomic<uint32_t> g_paceKeepaliveMs{0};
uint64_t g_lastKeepaliveMs = 0; // present thread only
bool g_detachedNow = false;     // present thread only
std::atomic<uint32_t> g_detachSkips{0}, g_detachKeepalives{0}, g_detachEpisodes{0};

// ---- Session 54: THE PACE FEED - keepalives that CARRY LAYERS ---------------
// The raffle-wedge root cause, measured from the s53 logs (ENGINE_NOTES s54):
// when VDXR demotes to VISIBLE it holds shouldRender=0, so the inline frame
// loop submits zero-layer frames; VDXR then (a) refuses to re-promote an app
// that submits no layers (the BS2 session-36 finding, re-measured: parked
// VISIBLE for minutes at 72 empty frames/s) and (b) throttles the empty loop's
// xrEndFrame to ~87 ms per call, pacing the game thread to ~10 presents/s -
// the announcer-stops half of the wedge.
//
// The feed fixes both halves at once: while the session is not FOCUSED (after
// having held it), the present thread DETACHES (the existing session-34 lever,
// implied by feed) so the game free-runs, and the pace thread runs the frame
// cycle itself, re-submitting the last healthy layer set with a fresh
// predictedDisplayTime. No re-acquire and no D3D11 work is needed: the
// compositor composites a swapchain's most-recently-RELEASED image, so a
// cached layer struct is a complete keepalive. Stale poses are fine - the
// compositor reprojects, and a frozen-but-present app is exactly what VD
// re-promotes.
//
// DEFAULT OFF IN CORE, ON PER GAME (the set_pace_detach pattern): BS1/BS2
// never arm it and take no new branch - even the snapshot banking is gated on
// the flag. The Infinite adapter arms it; `vrpace feed on|off` is the live A/B.
std::atomic<bool> g_paceFeed{false};
std::atomic<uint32_t> g_feedCycles{0};  // frame cycles run by the pace thread
std::atomic<uint32_t> g_feedLayered{0}; // of those, cycles that carried a layer
uint64_t g_feedErrorHoldMs = 0;         // present thread only: backoff after a failed cycle
std::mutex g_feedSnapMutex;
struct FeedSnap {
    bool valid = false;
    bool isProj = false;
    XrCompositionLayerProjection proj{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    XrCompositionLayerProjectionView views[2] = {
        {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW},
        {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}};
    XrCompositionLayerQuad quad{XR_TYPE_COMPOSITION_LAYER_QUAD};
};
FeedSnap g_feedSnap; // guarded by g_feedSnapMutex; proj.views is fixed up at use

// ---- Session 42: OPT-IN PAIR-RATE SYNC to the display refresh ---------------
// The Infinite judder investigation. The measured facts (sim, refresh 72,
// TRACE pairs): when the runtime's xrWaitFrame strictly gates - the sim's free
// mode does - the wait handoff locks pairs to the refresh (72/s, waitGate
// ~615 ms/s) and this lever adds nothing. But nothing REQUIRES a runtime to
// gate: one that pipelines (returns waits early while frames are in flight)
// lets the pair rate free-run at the game's own present speed - 77-80 pairs/s
// against a 72 Hz display is a ~5 Hz interference beat, the first suspect for
// judder-with-good-frames. This lever puts the gate on OUR side of the API:
// before OPENING a pair (and only then - delaying the closing RIGHT present
// would stretch the intra-pair gap that pair pacing exists to keep at 1-4 ms),
// the present thread waits until the next tick of a monotonic
// one-period-per-pair schedule. Period source: the runtime's own
// predictedDisplayPeriod, else a commanded Hz (`vrpace sync <hz>`).
//
// DEFAULT OFF IN CORE, ON PER GAME (the set_pace_detach pattern): BS1/BS2
// never call set_pace_sync and never take a single new branch with the flag
// false. The Infinite adapter arms it with stereo; `vrpace sync on|off` and
// the overlay checkbox are the live A/B a headset session can drive.
std::atomic<bool> g_paceSync{false};
std::atomic<uint32_t> g_paceSyncHz{0}; // 0 = use the runtime's period
int64_t g_paceSyncNextQpc = 0;         // present thread only; 0 = resync
std::atomic<uint32_t> g_paceSyncDelays{0};   // pairs actually delayed
std::atomic<uint64_t> g_paceSyncDelayUs{0};  // total delay imposed
std::atomic<uint32_t> g_paceSyncResyncs{0};  // schedule re-anchors (late arrivals)
// pace_sync_gate() itself is defined below phase_record - it needs the QPC
// plumbing that section owns.

// ---- Session 34: present-path PHASE TIMING ---------------------------------
// Session 33 concluded that the frame HANDOFF paces the game thread. Its own
// telemetry refuses that: `lastWait 0 ms` says xrWaitFrame returned instantly,
// and `timeouts 0` says the present thread never once reached its 20 ms handoff
// deadline - so neither of those two calls blocked, while the game still ran at
// 10 Hz with call2Us 99765. The ~100 ms was in a present-path phase that had
// never been timed (xrWaitSwapchainImage waits XR_INFINITE_DURATION; xrEndFrame
// was never measured at all).
//
// These timers exist so the phase that blocks is NAMED before anything is
// changed. A fix aimed at the wrong phase is indistinguishable from no fix, and
// this project has already spent a session on a mechanism its own numbers
// contradicted. `presentBegin` and `presentEnd` are the whole detour halves, so
// "is the stall even ours?" is answerable in one glance: if both are near zero
// while the game crawls, the pacer is not in this file.
enum PacePhase {
    kPhPresentBegin = 0, // the whole on_present_begin
    kPhPresentEnd,       // the whole on_present_end
    kPhWait,             // handoff wait, or the inline xrWaitFrame
    kPhBeginFrame,       // xrBeginFrame
    kPhLocate,           // xrLocateSpace + xrLocateViews
    kPhAcquire,          // xrAcquireSwapchainImage + xrWaitSwapchainImage (eye)
    kPhCapture,          // capture_frame blit
    kPhEndFrame,         // xrEndFrame
    kPhComposite,        // composite_hud + mirror_present
    kPhCount,
};
const char* const kPhaseNames[kPhCount] = {"presentBegin", "presentEnd", "wait",
                                           "beginFrame",   "locate",     "acquire",
                                           "capture",      "endFrame",   "composite"};
std::atomic<uint32_t> g_phaseLastUs[kPhCount]{};
std::atomic<uint32_t> g_phaseMaxUs[kPhCount]{};
std::atomic<bool> g_lastShouldRender{false};
// Presents seen by this module, so the overlay can show a rate without pulling
// in the d3d11 hook. THE number the user is judging: it is what "the game is at
// 10 Hz" means, on screen, next to the toggle that changes it.
std::atomic<uint32_t> g_presentsSeen{0};
int64_t g_qpcFreq = 0;

int64_t phase_now() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}

void phase_record(int ph, int64_t t0) {
    if (g_qpcFreq == 0) {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        g_qpcFreq = f.QuadPart ? f.QuadPart : 1;
    }
    uint32_t us = static_cast<uint32_t>((phase_now() - t0) * 1000000 / g_qpcFreq);
    g_phaseLastUs[ph].store(us, std::memory_order_relaxed);
    if (us > g_phaseMaxUs[ph].load(std::memory_order_relaxed))
        g_phaseMaxUs[ph].store(us, std::memory_order_relaxed);
}

// Session 42 pair-rate sync (state + rationale at the g_paceSync block above).
// Delay the opening of the next pair until the schedule's next tick. Coarse
// Sleep down to the last ~2 ms, then yield-spin on QPC - a raw Sleep quantum
// would add its own +-1.5 ms of jitter, which is the quantity under repair.
void pace_sync_gate() {
    if (!g_paceSync.load(std::memory_order_relaxed) ||
        !g_srPairPacing.load(std::memory_order_relaxed))
        return;
    int64_t periodNs = 0;
    uint32_t hz = g_paceSyncHz.load(std::memory_order_relaxed);
    if (hz > 0)
        periodNs = 1000000000LL / hz;
    else
        periodNs = g_displayPeriodNs.load(std::memory_order_relaxed);
    if (periodNs <= 0 || g_qpcFreq == 0) return; // nothing to sync to (yet)
    const int64_t periodQpc = static_cast<int64_t>(
        static_cast<double>(periodNs) * static_cast<double>(g_qpcFreq) / 1.0e9);
    if (periodQpc <= 0) return;
    int64_t now = phase_now();
    // (Re)anchor: first pair, or the game fell more than two periods behind
    // (load screen, alt-tab) - chasing a stale schedule would burst-open pairs.
    if (g_paceSyncNextQpc == 0 || now - g_paceSyncNextQpc > 2 * periodQpc) {
        if (g_paceSyncNextQpc != 0)
            g_paceSyncResyncs.fetch_add(1, std::memory_order_relaxed);
        g_paceSyncNextQpc = now;
    }
    if (now < g_paceSyncNextQpc) {
        g_paceSyncDelays.fetch_add(1, std::memory_order_relaxed);
        g_paceSyncDelayUs.fetch_add(
            static_cast<uint64_t>((g_paceSyncNextQpc - now) * 1000000 / g_qpcFreq),
            std::memory_order_relaxed);
        for (;;) {
            int64_t rem = g_paceSyncNextQpc - phase_now();
            if (rem <= 0) break;
            int64_t remMs = rem * 1000 / g_qpcFreq;
            if (remMs > 2)
                Sleep(static_cast<DWORD>(remMs - 1));
            else
                Sleep(0); // yield-spin the tail
        }
    }
    g_paceSyncNextQpc += periodQpc;
}

// ---- WHAT IS THE PRESENT THREAD INSIDE, RIGHT NOW? -------------------------
// The timers above only record a span once it RETURNS. A call that never
// returns is exactly the case we care about, and it makes them silent - which
// is indistinguishable from "nothing happened". These two say what the present
// thread entered and when, so the trace thread below can report a call that is
// still running after N seconds. Written by the present thread, read by the
// trace thread; relaxed is fine because a torn read only misnames one sample.
std::atomic<int> g_curPhase{-1};
std::atomic<int64_t> g_curPhaseT0{0};
// Which segment of the Present detour we are in, including the segments this
// file does not own (overlay, HUD capture, frame inspector, the game's own
// Present). String literals only.
std::atomic<const char*> g_presentStage{nullptr};
std::atomic<int64_t> g_presentStageT0{0};
std::atomic<const char*> g_drawStage{nullptr};
std::atomic<int64_t> g_drawStageT0{0};
std::atomic<uint32_t> g_drawStageTid{0};
// Session 35: STICKY thread ids, never cleared, plus a fire counter.
// g_drawStageTid is zeroed when the stage closes, and the old watchdog trigger
// required a stage to be OPEN - so it could only ever fire inside the doubled
// draw, the one path that opens one. Every other wedge (present thread, render
// worker, a build with stereo off) produced TRACE lines and no capture at all,
// which made a clean soak of any non-stereo mode vacuous rather than reassuring.
// These give the trigger something to aim at when no stage is open.
std::atomic<uint32_t> g_lastDrawTidSticky{0};
std::atomic<uint32_t> g_presentTidSticky{0};
std::atomic<uint32_t> g_watchdogFires{0};

// Records on every return path, which is what makes the two whole-half timers
// trustworthy - on_present_begin alone has nine early returns.
struct PhaseScope {
    int ph;
    int64_t t0;
    int prevPh;
    int64_t prevT0;
    explicit PhaseScope(int p) : ph(p), t0(phase_now()) {
        prevPh = g_curPhase.load(std::memory_order_relaxed);
        prevT0 = g_curPhaseT0.load(std::memory_order_relaxed);
        g_curPhaseT0.store(t0, std::memory_order_relaxed);
        g_curPhase.store(p, std::memory_order_relaxed);
    }
    ~PhaseScope() {
        phase_record(ph, t0);
        g_curPhase.store(prevPh, std::memory_order_relaxed);
        g_curPhaseT0.store(prevT0, std::memory_order_relaxed);
    }
    PhaseScope(const PhaseScope&) = delete;
    PhaseScope& operator=(const PhaseScope&) = delete;
};


// Names the exact call in flight for the trace thread, restoring the enclosing
// span on the way out. Used only around calls that can actually block, so the
// trace can say "IN endFrame for 4200 ms" instead of the containing half.
struct PhaseMark {
    int prevPh;
    int64_t prevT0;
    explicit PhaseMark(int p) {
        prevPh = g_curPhase.load(std::memory_order_relaxed);
        prevT0 = g_curPhaseT0.load(std::memory_order_relaxed);
        g_curPhaseT0.store(phase_now(), std::memory_order_relaxed);
        g_curPhase.store(p, std::memory_order_relaxed);
    }
    ~PhaseMark() {
        g_curPhase.store(prevPh, std::memory_order_relaxed);
        g_curPhaseT0.store(prevT0, std::memory_order_relaxed);
    }
    PhaseMark(const PhaseMark&) = delete;
    PhaseMark& operator=(const PhaseMark&) = delete;
};

// A phase that blocks for longer than this while the session is NOT focused is
// the bug: nothing off the headset's own display cadence has any business
// costing the game thread a frame's worth of time.
constexpr uint32_t kPhaseAlarmUs = 8000;
uint64_t g_lastPhaseLogMs = 0;

// M8 release blocker (b): the desktop mirror. Under SequentialReentry every
// Present alternates the backbuffer between the two eyes, so the flat window
// cannot be streamed, recorded, or shown. Fix: LEFT-eye presents snapshot the
// backbuffer (read-only - the compositor's eye capture is untouched); on
// RIGHT-eye presents, AFTER the right eye has been captured into its XR
// swapchain, the held left image is copied back over the backbuffer, so the
// real Present always displays the LEFT eye. Runs off the same sr eye tags
// the capture uses, and also on presents with no open XR frame (pace-guard
// skips, session gone) - the game keeps presenting alternating eyes there
// and the window still needs the pin.
// (Session-17 screenshot clue, explained: the pair's two presents have very
// unequal display time - L is visible only while the game builds the R frame,
// R through the whole next blocking xrWaitFrame - so DWM-sourced captures
// land on R with high probability. Same phase 12/12 was duty-cycle skew, not
// absence of alternation; a 60 Hz recorder still catches L slices.)
std::atomic<bool> g_mirror{true};        // `vrmirror off` = old alternation
ID3D11Texture2D* g_mirrorTex = nullptr;  // held left-eye image, present thread
uint32_t g_mirrorW = 0, g_mirrorH = 0;
DXGI_FORMAT g_mirrorFmt = DXGI_FORMAT_UNKNOWN;
bool g_mirrorHeld = false;               // a left image has been snapshotted
std::atomic<uint32_t> g_mirrorHolds{0}, g_mirrorBlits{0};
std::atomic<bool> g_loggedFirstMirror{false};

// Pop one tag; 0 = none pending (mono/AER frame).
int sr_pop_eye() {
    uint32_t tail = g_srTail.load(std::memory_order_relaxed);
    uint32_t head = g_srHead.load(std::memory_order_acquire);
    if (tail == head) return 0;
    if (head - tail > 2) {
        // More than a pair in flight: submit/present pairing skewed (mode
        // boundary). Drop everything and resync from mono.
        g_srTail.store(head, std::memory_order_relaxed);
        g_srCleared.fetch_add(1, std::memory_order_relaxed);
        BVR_LOG("xr: sr tag ring skewed (depth %u) - cleared", head - tail);
        return 0;
    }
    int sign = g_srRing[tail & (kSrRingSize - 1)].load(std::memory_order_relaxed);
    g_srTail.store(tail + 1, std::memory_order_release);
    g_srPopped.fetch_add(1, std::memory_order_relaxed);
    return sign;
}

const char* res_str(XrResult r) {
    static char buf[XR_MAX_RESULT_STRING_SIZE];
    if (g_instance != XR_NULL_HANDLE && xrResultToString(g_instance, r, buf) == XR_SUCCESS)
        return buf;
    sprintf_s(buf, "XrResult(%d)", static_cast<int>(r));
    return buf;
}

const char* state_str(XrSessionState s) {
    switch (s) {
        case XR_SESSION_STATE_IDLE: return "IDLE";
        case XR_SESSION_STATE_READY: return "READY";
        case XR_SESSION_STATE_SYNCHRONIZED: return "SYNCHRONIZED";
        case XR_SESSION_STATE_VISIBLE: return "VISIBLE";
        case XR_SESSION_STATE_FOCUSED: return "FOCUSED";
        case XR_SESSION_STATE_STOPPING: return "STOPPING";
        case XR_SESSION_STATE_LOSS_PENDING: return "LOSS_PENDING";
        case XR_SESSION_STATE_EXITING: return "EXITING";
        default: return "UNKNOWN";
    }
}

// The stall-guard decision for one present, shared verbatim by the real pace
// path and the flat simulation (which forces state/everFocused). True = this
// present must NOT run the blocking pacing. Present thread only.
// The pace thread body: park, wait a frame, publish, signal. The ONLY place
// xrWaitFrame is called once g_paceOffThread is on. It may block forever without
// consequence - nothing else runs on this thread.

// Session 54: one feed frame - begin, then end carrying the snapshot layer.
// Pace thread only, and only while the present thread is detached (so no other
// thread is inside the frame loop). The snapshot is copied out under its lock
// and submitted outside it: xrEndFrame may block ~87 ms under VD's
// unfocused-app throttle and must never hold the lock across that.
XrResult feed_submit_cycle(XrSession s, const XrFrameState& fs) {
    XrFrameBeginInfo fbi{XR_TYPE_FRAME_BEGIN_INFO};
    XrResult r = xrBeginFrame(s, &fbi);
    if (XR_FAILED(r)) return r;

    FeedSnap snap;
    {
        std::lock_guard<std::mutex> lk(g_feedSnapMutex);
        snap = g_feedSnap;
    }
    XrCompositionLayerProjection proj{};
    XrCompositionLayerQuad quad{};
    const XrCompositionLayerBaseHeader* layer = nullptr;
    if (snap.valid) {
        if (snap.isProj) {
            proj = snap.proj;
            proj.views = snap.views; // the banked copy, never the dead stack pointer
            layer = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&proj);
        } else {
            quad = snap.quad;
            layer = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&quad);
        }
    }

    XrFrameEndInfo fei{XR_TYPE_FRAME_END_INFO};
    fei.displayTime = fs.predictedDisplayTime;
    fei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    fei.layerCount = layer ? 1 : 0;
    fei.layers = layer ? &layer : nullptr;
    r = xrEndFrame(s, &fei);
    if (XR_SUCCEEDED(r)) {
        if (g_feedCycles.fetch_add(1, std::memory_order_relaxed) == 0)
            BVR_LOG("xr: pace feed live - re-submitting the last %s layer while the "
                    "session is not FOCUSED (the game thread is detached and free)",
                    layer ? (snap.isProj ? "projection" : "screen-quad") : "EMPTY (no snapshot yet)");
        if (layer) {
            g_feedLayered.fetch_add(1, std::memory_order_relaxed);
            // Feed frames count as submitted so the TRACE submitted/s column -
            // the instrument that diagnosed the wedge - shows them. Never
            // concurrent with the inline increment: feed runs only detached.
            ++g_framesSubmitted;
        }
    }
    return r;
}

DWORD WINAPI pace_thread_proc(void*) {
    while (g_paceRun.load(std::memory_order_relaxed)) {
        if (WaitForSingleObject(g_paceReq, INFINITE) != WAIT_OBJECT_0) break;
        if (!g_paceRun.load(std::memory_order_relaxed)) break;
        const int kind = g_paceReqKind.load(std::memory_order_relaxed);
        XrSession s = g_session; // a request is only posted with a live session
        XrFrameState fs{XR_TYPE_FRAME_STATE};
        XrResult r = XR_ERROR_SESSION_LOST;
        uint64_t t0 = GetTickCount64();
        if (s != XR_NULL_HANDLE) {
            if (kind == kPaceReqCloseOpen) {
                // s54b: the frame is already BEGUN on the present thread (a
                // pair-hold spanned the demote); it only owes its end, and
                // that end must not be able to block the game.
                XrFrameEndInfo idle{XR_TYPE_FRAME_END_INFO};
                idle.displayTime = g_feedCloseTimeNs.load(std::memory_order_relaxed);
                idle.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
                r = xrEndFrame(s, &idle);
            } else if (kind == kPaceReqFeedFinish) {
                // The wait already completed (banked in g_paceFrameState by the
                // previous request); this request only owes its begin+end.
                fs = g_paceFrameState;
                r = XR_SUCCESS;
            } else {
                XrFrameWaitInfo fwi{XR_TYPE_FRAME_WAIT_INFO};
                r = xrWaitFrame(s, &fwi, &fs);
            }
            if ((kind == kPaceReqFeedCycle || kind == kPaceReqFeedFinish) &&
                XR_SUCCEEDED(r))
                r = feed_submit_cycle(s, fs);
        }
        uint32_t ms = static_cast<uint32_t>(GetTickCount64() - t0);
        g_lastWaitMs.store(ms, std::memory_order_relaxed);
        if (ms > 1000) {
            static uint64_t lastStallLogMs = 0;
            if (t0 - lastStallLogMs > 5000) {
                lastStallLogMs = t0;
                BVR_LOG("xr: %s blocked %u ms on the pace thread (state "
                        "%s) - the present thread was NOT held by it",
                        kind == kPaceReqWait ? "xrWaitFrame" : "a feed cycle",
                        ms, state_str(g_state));
            }
        }
        // A CloseOpen result carries no frame state - never clobber a banked
        // wait with zeros (FeedFinish reads it).
        if (kind != kPaceReqCloseOpen) g_paceFrameState = fs;
        g_paceResKind.store(kind, std::memory_order_relaxed);
        g_paceResult.store(static_cast<int>(r), std::memory_order_relaxed);
        SetEvent(g_paceDone);
    }
    return 0;
}

bool pace_thread_start() {
    if (g_paceThread) return true;
    if (!g_paceReq) g_paceReq = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!g_paceDone) g_paceDone = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!g_paceReq || !g_paceDone) return false;
    g_paceRun.store(true, std::memory_order_relaxed);
    g_paceThread = CreateThread(nullptr, 0, &pace_thread_proc, nullptr, 0, nullptr);
    if (!g_paceThread) {
        g_paceRun.store(false, std::memory_order_relaxed);
        return false;
    }
    BVR_LOG("xr: pace thread started - xrWaitFrame no longer runs on the present "
            "thread, so an unbounded block cannot wedge the game (session 28)");
    return true;
}

bool pace_should_skip(XrSessionState state, bool everFocused, uint64_t now) {
    if (!g_paceGuard.load(std::memory_order_relaxed)) return false;
    if (state == XR_SESSION_STATE_FOCUSED) return false;
    if (!everFocused) return false; // bring-up: frames are how we REACH focused
    // SESSION 28: with the wait off the present thread there is no reason to stop
    // submitting, and a strong reason not to - VDXR will not re-grant FOCUSED to
    // an app that submits nothing, so skipping here is what made the alt-tab
    // freeze permanent (measured: state stuck VISIBLE, FOCUSED never returning,
    // and zero slow waits all session to justify the skip). The unbounded wait
    // that this guard existed to dodge can no longer reach the present thread,
    // so the deadline in on_present_begin replaces the skip entirely.
    if (g_paceOffThread.load(std::memory_order_relaxed)) return false;
    // Unfocused after having held FOCUSED: skip the blocking wait ALWAYS (see
    // the keepalive post-mortem above). One line per unfocused episode.
    if (g_unfocusedSinceMs == 0) {
        g_unfocusedSinceMs = now;
        BVR_LOG("xr: pacing SKIPPED while %s (headset idle) - the game keeps "
                "running; recovery is event-driven, no paced frame is issued "
                "until the runtime re-grants FOCUSED",
                state_str(state));
    }
    g_paceSkips.fetch_add(1, std::memory_order_relaxed);
    return true;
}

// ---- THE PACE TRACE, on its OWN thread -------------------------------------
// Every other log line in this file is written by the present thread. When that
// thread is blocked - which is the entire failure mode under investigation -
// the log goes silent, and silence reads exactly like "nothing is wrong". Three
// sessions have now been spent inferring from that silence.
//
// This thread reports once a second regardless of what the game is doing, and
// carries the four facts that separate the candidate explanations:
//
//   presents/s     is the GAME still presenting? BS2 pauses presenting when its
//                  window loses focus - documented in this repo's own harness
//                  scripts - so an alt-tab freeze may be the game's own
//                  behaviour and not the XR session at all. Nothing so far has
//                  told these two apart.
//   fg             is our window foreground? The above only means anything
//                  alongside this.
//   submitted/s    are frames reaching the headset? A live game with a frozen
//                  headset image and a frozen game look identical from inside
//                  the headset, and they need opposite fixes.
//   IN <phase>     what the present thread entered and has not come back from.
//                  This is the one the after-the-fact timers structurally
//                  cannot report.
HANDLE g_traceThread = nullptr;
std::atomic<bool> g_traceRun{false};
std::atomic<uint32_t> g_presentsAtTrace{0};

// Sampled ON THE PRESENT THREAD and read by the trace. USER32 calls can block
// on a wedged UI thread, and the trace must touch nothing that the freeze can
// hold - a stale flag is honest, a blocked tracer is not.
std::atomic<bool> g_appForeground{false};

void sample_foreground() {
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    g_appForeground.store(pid == GetCurrentProcessId(), std::memory_order_relaxed);
}

// The trace writes to its OWN file with its OWN handle. BVR_LOG serialises on a
// process-global std::mutex, so a thread wedged anywhere near a log call takes
// the tracer down with it - which is exactly what happened: the BS2 stereo
// freeze produced ZERO trace lines because the tracer was queued behind the
// very stall it was built to describe. An instrument that shares a lock with
// its subject is not an instrument.
FILE* g_traceFile = nullptr;

void trace_write(const char* line) {
    if (!g_traceFile) {
        wchar_t path[MAX_PATH];
        const wchar_t* base = bvr::log::data_dir();
        if (!base || !base[0]) return;
        swprintf_s(path, L"%s\\pacetrace.log", base);
        // _SH_DENYWR so the file can be tailed WHILE the game is frozen -
        // the default deny-all open made the trace unreadable at exactly the
        // moment it mattered.
        g_traceFile = _wfsopen(path, L"a", _SH_DENYWR);
        if (!g_traceFile) return;
    }
    SYSTEMTIME st{};
    GetLocalTime(&st);
    fprintf(g_traceFile, "[%02u:%02u:%02u.%03u] %s\n", st.wHour, st.wMinute, st.wSecond,
            st.wMilliseconds, line);
    fflush(g_traceFile); // the process may be killed at any moment
}

// Session 43: one spike, one line, at the pair close that measured it (present
// thread - trace_write's FILE* is CRT-locked per call, so the 1 Hz thread and
// this writer interleave lines, never bytes). Prints the per-phase maxima
// accumulated since the previous spike, then resets them - consecutive spikes
// in a burst therefore each carry their OWN attribution window. The
// unattributed remainder is interval minus both detour-half maxima: when it
// carries nearly the whole interval, the stall lives outside this file
// (game sim/draw/GPU/driver), which is exactly the discriminator the hunt
// needs first.
void spike_capture(uint32_t intervalUs, int64_t periodNs) {
    uint32_t seq = g_spikeSeq.fetch_add(1, std::memory_order_relaxed) + 1;
    uint32_t lastUs[kPhCount], maxUs[kPhCount];
    for (int i = 0; i < kPhCount; ++i) {
        lastUs[i] = g_phaseLastUs[i].load(std::memory_order_relaxed);
        maxUs[i] = g_phaseMaxUs[i].exchange(0, std::memory_order_relaxed);
    }
    uint32_t oursUs = maxUs[kPhPresentBegin] + maxUs[kPhPresentEnd];
    uint32_t unattrUs = intervalUs > oursUs ? intervalUs - oursUs : 0;
    const char* pstage = g_presentStage.load(std::memory_order_relaxed);
    const char* dstage = g_drawStage.load(std::memory_order_relaxed);
    char buf[640];
    int n = sprintf_s(buf,
                      "TRACE spike #%u interval %.1f ms (%.1fx period) | unattributed "
                      "%.1f ms (ours max %.1f) | lastWait %u ms | stage %s draw %s | "
                      "syncDelays %u | max-since-prev/last us:",
                      seq, intervalUs / 1000.0,
                      periodNs > 0 ? intervalUs * 1000.0 / periodNs : 0.0,
                      unattrUs / 1000.0, oursUs / 1000.0,
                      g_lastWaitMs.load(std::memory_order_relaxed),
                      pstage ? pstage : "-", dstage ? dstage : "-",
                      g_paceSyncDelays.load(std::memory_order_relaxed));
    for (int i = 0; i < kPhCount && n > 0 && n < static_cast<int>(sizeof(buf)) - 48; ++i)
        n += sprintf_s(buf + n, sizeof(buf) - n, " %s=%u/%u", kPhaseNames[i], maxUs[i],
                       lastUs[i]);
    trace_write(buf);
}

// ---- Session 34: THE STALL WATCHDOG ----------------------------------------
// The freeze is a call inside the game that never returns, and the mod is not
// on that stack - so no amount of our own logging can name it. This suspends
// the wedged thread, reads its instruction pointer, and scans its stack for
// return addresses that fall inside the game image. Those RVAs, run through
// tools/disasm-rva.py, are what turn "secondDraw never returned" into a named
// engine function.
//
// Suspending a thread from another thread is only safe because we do nothing
// that can allocate or take a lock while it is suspended: read the context,
// copy raw dwords, resume. The formatting happens afterwards.
uintptr_t g_exeLo = 0, g_exeHi = 0;

void capture_exe_bounds() {
    if (g_exeLo) return;
    HMODULE exe = GetModuleHandleW(nullptr);
    if (!exe) return;
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(exe);
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(reinterpret_cast<uint8_t*>(exe) +
                                                   dos->e_lfanew);
    g_exeLo = reinterpret_cast<uintptr_t>(exe);
    g_exeHi = g_exeLo + nt->OptionalHeader.SizeOfImage;
}

// Resolve an address to "module+offset" (a DLL name is not game-derived).
void describe_addr(uintptr_t a, char* out, size_t cap) {
    out[0] = 0;
    if (!a) return;
    HMODULE hm = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(a), &hm) &&
        hm) {
        wchar_t wpath[MAX_PATH] = {};
        GetModuleFileNameW(hm, wpath, MAX_PATH);
        const wchar_t* b = wcsrchr(wpath, L'\\');
        b = b ? b + 1 : wpath;
        char nm[64] = {};
        WideCharToMultiByte(CP_UTF8, 0, b, -1, nm, sizeof(nm), nullptr, nullptr);
        sprintf_s(out, cap, "%s+0x%X", nm,
                  static_cast<unsigned>(a - reinterpret_cast<uintptr_t>(hm)));
    } else {
        sprintf_s(out, cap, "%08X", static_cast<unsigned>(a));
    }
}

// Snapshot EVERY thread in the process, one at a time. A deadlock has two
// sides, and naming only the side that stopped first cannot distinguish "waits
// on the render thread" from "waits on something nobody will ever signal".
// Suspends one thread at a time and formats afterwards, so the watchdog can
// never be holding a CRT lock that a suspended thread needs.
// SESSION 35: this printed NOTHING, every time, and the reason it printed
// nothing was indistinguishable from "there was nothing to print" - it had five
// exits and all five were silent. A diagnostic that cannot report its own
// failure is worse than none, because a blank section reads as an answer.
//
// Two substantive changes beyond the logging:
//   - the `nf == 0` filter is GONE. It was justified as "only threads with game
//     code are interesting", but the other side of a deadlock is precisely a
//     worker parked in ntdll with no game frames near the top of its stack. The
//     filter was hiding the one thread this function exists to find.
//   - the scan window matches watchdog_capture (8192 dwords / 24 frames). At
//     1024/6 it gave up before reaching the first in-image return on any thread
//     with a deep native prologue.
void watchdog_all_threads() {
    capture_exe_bounds();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) {
        char m[128];
        sprintf_s(m, "  WATCHDOG: thread snapshot FAILED (%lu) - no thread list this episode",
                  GetLastError());
        trace_write(m);
        return;
    }
    THREADENTRY32 te{};
    te.dwSize = sizeof(te);
    DWORD self = GetCurrentThreadId();
    DWORD pid = GetCurrentProcessId();
    int seen = 0, reported = 0, openFail = 0, suspFail = 0, ctxFail = 0, noEip = 0;
    DWORD firstOpenErr = 0;
    if (!Thread32First(snap, &te)) {
        char m[128];
        sprintf_s(m, "  WATCHDOG: Thread32First FAILED (%lu)", GetLastError());
        trace_write(m);
        CloseHandle(snap);
        return;
    }
    do {
        if (te.th32OwnerProcessID != pid || te.th32ThreadID == self) continue;
        ++seen;
        HANDLE th = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE,
                               te.th32ThreadID);
        if (!th) {
            if (!firstOpenErr) firstOpenErr = GetLastError();
            ++openFail;
            continue;
        }
        CONTEXT ctx{};
        ctx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
        uintptr_t eip = 0, esp = 0;
        uintptr_t fr[24] = {};
        int nf = 0;
        bool suspended = false, gotCtx = false;
        if (SuspendThread(th) != static_cast<DWORD>(-1)) {
            suspended = true;
            if (GetThreadContext(th, &ctx)) {
                gotCtx = true;
                eip = ctx.Eip;
                esp = ctx.Esp;
                const uintptr_t* sp = reinterpret_cast<const uintptr_t*>(esp);
                for (int i = 0; i < 8192 && nf < 24; ++i) {
                    uintptr_t v = 0;
                    __try {
                        v = sp[i];
                    } __except (EXCEPTION_EXECUTE_HANDLER) {
                        break;
                    }
                    if (v >= g_exeLo && v < g_exeHi) fr[nf++] = v;
                }
            }
            ResumeThread(th);
        }
        CloseHandle(th);
        if (!suspended) { ++suspFail; continue; }
        if (!gotCtx) { ++ctxFail; continue; }
        if (!eip) { ++noEip; continue; }

        char where[96];
        describe_addr(eip, where, sizeof(where));
        // Chunked: msg[320] silently truncated the RVA list at ~24 entries, and a
        // truncated stack reads as a short one.
        char msg[320];
        int n = sprintf_s(msg, "  thread %5u at %-30s exe:", te.th32ThreadID, where);
        if (nf == 0) {
            sprintf_s(msg + n, sizeof(msg) - n, " (none)");
            trace_write(msg);
        } else {
            for (int i = 0; i < nf; ++i) {
                if (n > static_cast<int>(sizeof(msg)) - 12) {
                    trace_write(msg);
                    n = sprintf_s(msg, "  thread %5u  ...cont exe:", te.th32ThreadID);
                }
                n += sprintf_s(msg + n, sizeof(msg) - n, " %X",
                               static_cast<unsigned>(fr[i] - g_exeLo));
            }
            trace_write(msg);
        }
        if (++reported >= 64) break;
    } while (Thread32Next(snap, &te));
    CloseHandle(snap);

    // The tally is the point: "reported=0 openFail=37" and "reported=0 seen=0"
    // are completely different bugs, and the old code rendered both as silence.
    char tally[224];
    sprintf_s(tally,
              "  WATCHDOG threads: seen=%d reported=%d openFail=%d(err %lu) "
              "suspFail=%d ctxFail=%d noEip=%d",
              seen, reported, openFail, firstOpenErr, suspFail, ctxFail, noEip);
    trace_write(tally);
}

void watchdog_capture(uint32_t tid, const char* what, int64_t stuckMs) {
    capture_exe_bounds();
    if (!g_exeLo || !tid) return;
    HANDLE th = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION,
                           FALSE, tid);
    if (!th) {
        char msg[128];
        sprintf_s(msg, "WATCHDOG: OpenThread(%u) failed (%lu)", tid, GetLastError());
        trace_write(msg);
        return;
    }
    CONTEXT ctx{};
    ctx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
    uintptr_t frames[24] = {};
    int nFrames = 0;
    uintptr_t eip = 0, esp = 0, ebp = 0;
    if (SuspendThread(th) != static_cast<DWORD>(-1)) {
        if (GetThreadContext(th, &ctx)) {
            eip = ctx.Eip;
            esp = ctx.Esp;
            ebp = ctx.Ebp;
            // Raw stack scan: every dword in the first 8 KB that lands inside
            // the game image is a plausible return address. Crude, but it needs
            // no symbols and no unwind data, and the histogram is unambiguous.
            const uintptr_t* p = reinterpret_cast<const uintptr_t*>(esp);
            for (int i = 0; i < 2048 && nFrames < 24; ++i) {
                uintptr_t v = 0;
                __try {
                    v = p[i];
                } __except (EXCEPTION_EXECUTE_HANDLER) {
                    break;
                }
                if (v >= g_exeLo && v < g_exeHi) frames[nFrames++] = v;
            }
        }
        ResumeThread(th);
    }
    CloseHandle(th);

    // WHICH MODULE is the wedged thread actually inside? This is the datum that
    // separates "blocked in the graphics driver" from "blocked on a lock", and
    // those need opposite fixes. A DLL name is not game-derived content.
    char mod[64] = "?";
    if (eip) {
        HMODULE hm = nullptr;
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                   GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCWSTR>(eip), &hm) &&
            hm) {
            wchar_t wpath[MAX_PATH] = {};
            GetModuleFileNameW(hm, wpath, MAX_PATH);
            const wchar_t* base = wcsrchr(wpath, L'\\');
            base = base ? base + 1 : wpath;
            WideCharToMultiByte(CP_UTF8, 0, base, -1, mod, sizeof(mod), nullptr, nullptr);
            sprintf_s(mod + strlen(mod), sizeof(mod) - strlen(mod), "+0x%X",
                      static_cast<unsigned>(eip - reinterpret_cast<uintptr_t>(hm)));
        }
    }

    char msg[640];
    int n = sprintf_s(msg, "WATCHDOG %s stuck %lld ms tid=%u eip=%08X in %s esp=%08X ebp=%08X rva:",
                      what, static_cast<long long>(stuckMs), tid,
                      static_cast<unsigned>(eip), mod, static_cast<unsigned>(esp),
                      static_cast<unsigned>(ebp));
    for (int i = 0; i < nFrames && n > 0 && n < static_cast<int>(sizeof(msg)) - 16; ++i)
        n += sprintf_s(msg + n, sizeof(msg) - n, " %X",
                       static_cast<unsigned>(frames[i] - g_exeLo));
    trace_write(msg);
    trace_write("WATCHDOG all threads with game code on the stack:");
    watchdog_all_threads();
}

// Session 43: the spike-in-progress sampler (rationale at g_spikeStacks).
// 4 ms poll while armed, 50 ms idle poll while not. One capture per episode:
// an episode is "the pair that has not closed yet", identified by the QPC of
// the PREVIOUS close - once a new close lands, the episode key changes and the
// sampler re-arms. The 900 ms ceiling matches the pair-stat exclusion: a load
// screen or alt-tab is not judder, and suspending threads through one helps
// nobody. 2 s between captures bounds burst cost; kSpikeStackMax bounds the
// session log.
DWORD WINAPI spike_sampler_proc(void*) {
    int64_t sampledEpisode = 0;
    uint64_t lastCaptureMs = 0;
    while (g_traceRun.load(std::memory_order_relaxed)) {
        if (!g_spikeTrace.load(std::memory_order_relaxed) ||
            g_spikeStacks.load(std::memory_order_relaxed) >= kSpikeStackMax) {
            Sleep(50);
            continue;
        }
        Sleep(4);
        int64_t lastClose = g_pairLastCloseQpc.load(std::memory_order_relaxed);
        if (lastClose == 0 || lastClose == sampledEpisode || g_qpcFreq == 0) continue;
        int64_t ageUs = (phase_now() - lastClose) * 1000000 / g_qpcFreq;
        int64_t periodNs = g_displayPeriodNs.load(std::memory_order_relaxed);
        int64_t thresholdUs = periodNs > 0 ? periodNs / 400 : 31250; // 2.5x period
        if (thresholdUs < 20000) thresholdUs = 20000;
        if (ageUs < thresholdUs || ageUs > 900000) continue;
        uint64_t nowMs = GetTickCount64();
        if (nowMs - lastCaptureMs < 2000) continue;
        sampledEpisode = lastClose;
        lastCaptureMs = nowMs;
        g_spikeStacks.fetch_add(1, std::memory_order_relaxed);
        char hdr[160];
        sprintf_s(hdr, "SPIKE-SAMPLE #%u mid-stall age %lld ms (draw tid %u, then all threads):",
                  g_spikeStacks.load(std::memory_order_relaxed),
                  static_cast<long long>(ageUs / 1000),
                  g_lastDrawTidSticky.load(std::memory_order_relaxed));
        trace_write(hdr);
        watchdog_capture(g_lastDrawTidSticky.load(std::memory_order_relaxed),
                         "spike-draw", ageUs / 1000);
    }
    return 0;
}

DWORD WINAPI trace_thread_proc(void*) {
    uint32_t lastPresents = 0;
    uint32_t lastSubmitted = 0;
    uint32_t lastKeepalives = 0;
    uint32_t lastSkips = 0;
    // Session 42 pair-cadence baselines (deltas per 1 s tick, like presents).
    uint32_t lastPairs = 0;
    uint32_t lastPairCount = 0;
    uint64_t lastPairSumUs = 0;
    uint64_t lastPairSumSqUs = 0;
    uint64_t lastPairWaitUs = 0;
    uint32_t lastSpikes = 0; // session 43: spike count baseline for the window delta
    while (g_traceRun.load(std::memory_order_relaxed)) {
        Sleep(1000);
        if (!g_traceRun.load(std::memory_order_relaxed)) break;
        // Session 34: report with NO session at all, and detect the stall from
        // PRESENTS HAVING STOPPED rather than from one of our own phases being
        // open. The first cut keyed on g_curPhase and stayed silent through the
        // real hang, because the present thread was wedged OUTSIDE every span
        // this file wraps - the same "silence reads as calm" failure the trace
        // exists to remove, reintroduced one level up.
        //
        // The BOOKKEEPING BELOW MUST RUN EVERY TICK. The first cut did the
        // `continue` above the `lastPresents = presents` update at the bottom of
        // the loop, so lastPresents stayed 0 forever, `stalled` could never
        // become true, and the tracer was structurally incapable of firing -
        // which is why the freeze produced no trace file at all. Deltas are
        // computed and the baselines rolled BEFORE any decision to skip.
        uint32_t presents = g_presentsSeen.load(std::memory_order_relaxed);
        uint32_t submitted = g_framesSubmitted;
        uint32_t keepalives = g_detachKeepalives.load(std::memory_order_relaxed);
        uint32_t skips = g_detachSkips.load(std::memory_order_relaxed);
        uint32_t dPresents = presents - lastPresents;
        uint32_t dSubmitted = submitted - lastSubmitted;
        uint32_t dKeepalives = keepalives - lastKeepalives;
        uint32_t dSkips = skips - lastSkips;
        bool seenAny = lastPresents > 0;
        lastPresents = presents;
        lastSubmitted = submitted;
        lastKeepalives = keepalives;
        lastSkips = skips;

        bool stalled = seenAny && dPresents == 0;

        // Once per stall episode, and only after it is clearly not a hitch,
        // photograph the wedged thread's stack. This is the step that names the
        // engine function; everything before it could only say "stopped".
        //
        // SESSION 35: the trigger used to require an OPEN DRAW STAGE
        // (`dw && dtid && dms >= 4000`). Only the doubled draw ever opens one,
        // so the watchdog was structurally incapable of firing in any other mode
        // - and "zero WATCHDOG lines" was therefore a guaranteed pass for
        // vanilla, vrcam and vraer rather than evidence about them. It now fires
        // on the stall itself and aims at whatever thread it can name.
        {
            static bool fired = false;
            static int stalledTicks = 0;
            if (!stalled) {
                fired = false;
                stalledTicks = 0;
            } else if (!fired) {
                ++stalledTicks; // one tick per second
                const char* dw = g_drawStage.load(std::memory_order_relaxed);
                uint32_t dtid = g_drawStageTid.load(std::memory_order_relaxed);
                int64_t dms = g_qpcFreq ? (phase_now() -
                                           g_drawStageT0.load(std::memory_order_relaxed)) *
                                              1000 / g_qpcFreq
                                        : 0;
                if (dw && dtid && dms >= 4000) {
                    // Best case: a named stage is open, so we know what blocked.
                    fired = true;
                    g_watchdogFires.fetch_add(1, std::memory_order_relaxed);
                    watchdog_capture(dtid, dw, dms);
                } else if (stalledTicks >= 4) {
                    // Presents have stopped for 4 s with no stage open. Aim at
                    // the last thread known to draw, else the present thread; if
                    // neither is known, the all-threads sweep still runs, which
                    // is the whole reason it was worth repairing.
                    fired = true;
                    g_watchdogFires.fetch_add(1, std::memory_order_relaxed);
                    uint32_t tid = g_lastDrawTidSticky.load(std::memory_order_relaxed);
                    if (!tid) tid = g_presentTidSticky.load(std::memory_order_relaxed);
                    if (tid) {
                        watchdog_capture(tid, "presentsStopped",
                                         static_cast<int64_t>(stalledTicks) * 1000);
                    } else {
                        char m[160];
                        sprintf_s(m, "WATCHDOG presentsStopped %d ms, no thread to aim at - "
                                     "sweeping all threads",
                                  stalledTicks * 1000);
                        trace_write(m);
                        watchdog_all_threads();
                    }
                }
            }
        }
        if (g_session == XR_NULL_HANDLE && !g_simIdle.load(std::memory_order_relaxed) &&
            !stalled)
            continue;

        // Is a call still in flight, and for how long?
        char stuck[96] = "-";
        int ph = g_curPhase.load(std::memory_order_relaxed);
        if (ph >= 0 && ph < kPhCount && g_qpcFreq) {
            int64_t t0 = g_curPhaseT0.load(std::memory_order_relaxed);
            int64_t ms = (phase_now() - t0) * 1000 / g_qpcFreq;
            if (ms >= 200)
                sprintf_s(stuck, "IN %s for %lld ms", kPhaseNames[ph],
                          static_cast<long long>(ms));
        }

        char draw[128] = "-";
        {
            const char* nm = g_drawStage.load(std::memory_order_relaxed);
            if (nm && g_qpcFreq) {
                int64_t ms = (phase_now() - g_drawStageT0.load(std::memory_order_relaxed)) *
                             1000 / g_qpcFreq;
                sprintf_s(draw, "%s for %lld ms", nm, static_cast<long long>(ms));
            }
        }
        char stage[128] = "-";
        {
            const char* nm = g_presentStage.load(std::memory_order_relaxed);
            if (nm && g_qpcFreq) {
                int64_t ms = (phase_now() - g_presentStageT0.load(std::memory_order_relaxed)) *
                             1000 / g_qpcFreq;
                sprintf_s(stage, "%s for %lld ms", nm, static_cast<long long>(ms));
            }
        }
        char line[640];
        sprintf_s(line,
                  "TRACE %s%s detached=%d fg=%d | presents/s %u submitted/s %u | "
                  "keepalive %u ms (unpaced %u/s, ka %u/s) | lastEnd %u ms lastWait %u ms "
                  "shouldRender=%d | phase: %s | stage: %s | draw: %s",
                  state_str(g_state),
                  g_everFocused.load(std::memory_order_relaxed) ? "" : "/never",
                  g_detachedNow ? 1 : 0,
                  g_appForeground.load(std::memory_order_relaxed) ? 1 : 0,
                  dPresents, dSubmitted,
                  g_paceKeepaliveMs.load(std::memory_order_relaxed), dSkips,
                  dKeepalives,
                  g_phaseLastUs[kPhEndFrame].load(std::memory_order_relaxed) / 1000,
                  g_lastWaitMs.load(std::memory_order_relaxed),
                  g_lastShouldRender.load(std::memory_order_relaxed) ? 1 : 0, stuck, stage,
                  draw);
        trace_write(line);

        // Session 42: pair CADENCE, one line per tick while pairs flow. This is
        // the judder instrument - pairs/s alone cannot distinguish "72 evenly
        // spaced" from "77 free-running with a 5 Hz beat"; the interval spread
        // and the wait-gate share can. min/max are window-reset by exchange
        // (the race with a concurrent sample loses at most that sample).
        {
            uint32_t pairs = g_srPairs.load(std::memory_order_relaxed);
            uint32_t cnt = g_pairIntCount.load(std::memory_order_relaxed);
            uint64_t sum = g_pairIntSumUs.load(std::memory_order_relaxed);
            uint64_t sumSq = g_pairIntSumSqUs.load(std::memory_order_relaxed);
            uint64_t waitUs = g_pairWaitSumUs.load(std::memory_order_relaxed);
            uint32_t spikes = g_spikeCount.load(std::memory_order_relaxed);
            uint32_t dPairs = pairs - lastPairs;
            uint32_t dCnt = cnt - lastPairCount;
            uint64_t dSum = sum - lastPairSumUs;
            uint64_t dSumSq = sumSq - lastPairSumSqUs;
            uint64_t dWaitUs = waitUs - lastPairWaitUs;
            uint32_t dSpikes = spikes - lastSpikes;
            lastPairs = pairs;
            lastPairCount = cnt;
            lastPairSumUs = sum;
            lastPairSumSqUs = sumSq;
            lastPairWaitUs = waitUs;
            lastSpikes = spikes;
            if (dPairs > 0) {
                uint32_t minUs = g_pairIntMinUs.exchange(0xFFFFFFFFu, std::memory_order_relaxed);
                uint32_t maxUs = g_pairIntMaxUs.exchange(0, std::memory_order_relaxed);
                if (minUs == 0xFFFFFFFFu) minUs = 0;
                uint32_t meanUs = 0, sdUs = 0;
                if (dCnt > 0) {
                    meanUs = static_cast<uint32_t>(dSum / dCnt);
                    uint64_t meanSq = dSum / dCnt * (dSum / dCnt);
                    uint64_t var = dSumSq / dCnt > meanSq ? dSumSq / dCnt - meanSq : 0;
                    sdUs = static_cast<uint32_t>(sqrt(static_cast<double>(var)));
                }
                int64_t periodNs = g_displayPeriodNs.load(std::memory_order_relaxed);
                char pl[320];
                sprintf_s(pl,
                          "TRACE pairs %u/s | interval us mean=%u sd=%u min=%u max=%u | "
                          "spikes=%u | waitGate %llu ms/s timeouts=%u | period %.2f ms "
                          "(%.1f Hz)",
                          dPairs, meanUs, sdUs, minUs, maxUs, dSpikes,
                          static_cast<unsigned long long>(dWaitUs / 1000),
                          g_paceTimeouts.load(std::memory_order_relaxed),
                          periodNs > 0 ? periodNs / 1.0e6 : 0.0,
                          periodNs > 0 ? 1.0e9 / periodNs : 0.0);
                trace_write(pl);
            }
        }

    }
    return 0;
}

void trace_thread_start() {
    if (g_traceThread) return;
    g_traceRun.store(true, std::memory_order_relaxed);
    g_traceThread = CreateThread(nullptr, 0, &trace_thread_proc, nullptr, 0, nullptr);
    if (g_traceThread)
        BVR_LOG("xr: pace trace started - 1 Hz from its OWN thread, so it keeps "
                "reporting while the present thread is blocked");
    // Session 43: the spike-in-progress sampler rides the same run flag. While
    // the spike trace is disarmed (BS1/BS2 always; Infinite without stereo) it
    // is a 20 Hz relaxed-load poll that takes no action.
    if (!g_spikeSamplerThread)
        g_spikeSamplerThread = CreateThread(nullptr, 0, &spike_sampler_proc, nullptr, 0, nullptr);
}

// The DETACHED-PACING decision for one present, factored out so the flat
// simulation below runs the IDENTICAL logic against a forced state. True = this
// present must make no blocking OpenXR call. Present thread only.
//
// Being able to run this flat matters more than it looks: VDXR creates no
// session without a headset, so the real path is unreachable on a desk, and
// three builds went to the user unverified because of it.
bool detach_skip_decision(uint64_t now, bool focused) {
    // Session 54: the pace feed IMPLIES detach - feeding only makes sense with
    // the present thread out of the frame loop. `vrpace detach` stays the
    // independent legacy lever (BS2's session-34/36 A/B).
    if (!g_paceDetach.load(std::memory_order_relaxed) &&
        !g_paceFeed.load(std::memory_order_relaxed)) {
        g_detachedNow = false; // never leave the flag set behind a disabled lever
        return false;
    }
    // Bring-up exception (session 36, found by the first real VDXR attach
    // since this lever landed): a NEVER-focused session must keep its frame
    // loop running - frames are how the runtime walks READY -> SYNCHRONIZED
    // -> VISIBLE -> FOCUSED, so detaching here parks the headset on a void
    // forever. Identical to pace_should_skip's exception; detach is for
    // sessions that HAD focus and lost it (alt-tab), not for first attach.
    if (!g_everFocused.load(std::memory_order_relaxed)) {
        g_detachedNow = false;
        return false;
    }
    if (focused) {
        if (g_detachedNow) {
            g_detachedNow = false;
            BVR_LOG("xr: ATTACHED - session FOCUSED again, full-rate pacing resumes "
                    "(%u presents ran unpaced, %u keepalive frames submitted)",
                    g_detachSkips.load(std::memory_order_relaxed),
                    g_detachKeepalives.load(std::memory_order_relaxed));
        }
        return false;
    }
    uint32_t every = g_paceKeepaliveMs.load(std::memory_order_relaxed);
    if (!g_detachedNow) {
        g_detachedNow = true;
        g_lastKeepaliveMs = now;
        g_detachEpisodes.fetch_add(1, std::memory_order_relaxed);
        BVR_LOG("xr: DETACHED (session %s) - the present thread now makes NO blocking "
                "OpenXR call (keepalive %u ms; 0 = none). Recovery is by EVENT: "
                "pump_events runs first thing every present and needs no submission.",
                state_str(g_state), every);
    }
    if (every == 0 || now - g_lastKeepaliveMs < every) {
        g_detachSkips.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    g_lastKeepaliveMs = now;
    g_detachKeepalives.fetch_add(1, std::memory_order_relaxed);
    return false;
}

// One line per 5 s, and ONLY while unfocused with some phase over the alarm -
// the session-33 lesson that a diagnostic on the present path needs a hard rate
// limit rather than a change test (a change test can flicker at present rate,
// which is how the fov-watch line took the game to 40 fps and then wedged it).
// Never fires on a healthy focused session.
void phase_heartbeat_maybe(uint64_t now) {
    if (g_state == XR_SESSION_STATE_FOCUSED) return;
    uint32_t worst = 0;
    for (int i = kPhWait; i < kPhCount; ++i) {
        uint32_t v = g_phaseMaxUs[i].load(std::memory_order_relaxed);
        if (v > worst) worst = v;
    }
    if (worst < kPhaseAlarmUs) return;
    if (now - g_lastPhaseLogMs < 5000) return;
    g_lastPhaseLogMs = now;
    char buf[512];
    int n = sprintf_s(buf, "xr: present phases (max us, state %s shouldRender=%d):",
                      state_str(g_state),
                      g_lastShouldRender.load(std::memory_order_relaxed) ? 1 : 0);
    for (int i = 0; i < kPhCount && n > 0 && n < static_cast<int>(sizeof(buf)) - 40; ++i)
        n += sprintf_s(buf + n, sizeof(buf) - n, " %s=%u", kPhaseNames[i],
                       g_phaseMaxUs[i].load(std::memory_order_relaxed));
    BVR_LOG("%s", buf);
    for (int i = 0; i < kPhCount; ++i) g_phaseMaxUs[i].store(0, std::memory_order_relaxed);
}

void release_mirror() {
    if (g_mirrorTex) {
        g_mirrorTex->Release();
        g_mirrorTex = nullptr;
    }
    g_mirrorHeld = false;
    g_mirrorW = g_mirrorH = 0;
    g_mirrorFmt = DXGI_FORMAT_UNKNOWN;
}

// The eye pin for one present (see the block comment at the globals). Uses
// the game device straight off the backbuffer, so it works with or without a
// live XR session. eyeSign: -1 = this backbuffer is the LEFT eye (snapshot),
// +1 = RIGHT eye (re-blit the held left over it - call only AFTER the right
// eye's XR capture), 0 = mono (nothing to pin).
void mirror_present(IDXGISwapChain* swapchain, int eyeSign) {
    if (eyeSign == 0 || !g_mirror.load(std::memory_order_relaxed)) return;
    ID3D11Texture2D* backbuffer = nullptr;
    if (FAILED(swapchain->GetBuffer(0, IID_PPV_ARGS(&backbuffer))) || !backbuffer) return;
    D3D11_TEXTURE2D_DESC bd{};
    backbuffer->GetDesc(&bd);
    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    backbuffer->GetDevice(&dev);
    if (dev) dev->GetImmediateContext(&ctx);
    if (ctx) {
        if (g_mirrorTex &&
            (g_mirrorW != bd.Width || g_mirrorH != bd.Height || g_mirrorFmt != bd.Format))
            release_mirror();
        if (!g_mirrorTex && eyeSign < 0) {
            D3D11_TEXTURE2D_DESC td = bd;
            td.Usage = D3D11_USAGE_DEFAULT;
            td.BindFlags = 0;
            td.CPUAccessFlags = 0;
            td.MiscFlags = 0;
            if (SUCCEEDED(dev->CreateTexture2D(&td, nullptr, &g_mirrorTex))) {
                g_mirrorW = bd.Width;
                g_mirrorH = bd.Height;
                g_mirrorFmt = bd.Format;
            } else {
                g_mirrorTex = nullptr; // failed create: mirror silently off
            }
        }
        if (g_mirrorTex) {
            if (eyeSign < 0) {
                ctx->CopyResource(g_mirrorTex, backbuffer);
                g_mirrorHeld = true;
                g_mirrorHolds.fetch_add(1, std::memory_order_relaxed);
            } else if (g_mirrorHeld) {
                ctx->CopyResource(backbuffer, g_mirrorTex);
                g_mirrorBlits.fetch_add(1, std::memory_order_relaxed);
                if (!g_loggedFirstMirror.exchange(true))
                    BVR_LOG("xr: desktop mirror pinned to the LEFT eye "
                            "(right-eye presents re-show the held left image)");
            }
        }
        ctx->Release();
    }
    if (dev) dev->Release();
    backbuffer->Release();
}

void reset_aer() {
    g_eyeValid[0] = g_eyeValid[1] = false;
    g_currentEye = 0;
    g_aerEyeSign.store(0, std::memory_order_relaxed);
}

void destroy_laser() {
    if (g_laserSwapchain != XR_NULL_HANDLE) {
        xrDestroySwapchain(g_laserSwapchain);
        g_laserSwapchain = XR_NULL_HANDLE;
    }
    g_laserImages.clear();
    if (g_laserDot) {
        g_laserDot->Release();
        g_laserDot = nullptr;
    }
}

void destroy_hud_swapchain() {
    if (g_hudSwapchain != XR_NULL_HANDLE) {
        xrDestroySwapchain(g_hudSwapchain);
        g_hudSwapchain = XR_NULL_HANDLE;
    }
    g_hudImages.clear();
    g_hudSwapW = g_hudSwapH = 0;
}

void destroy_swapchains() {
    // Session 54: the feed snapshot references these swapchains - drop it
    // BEFORE they die so a feed cycle can never submit a dead handle. (Feed
    // cycles cannot be in flight here: teardown drains the outstanding pace
    // request first, and the resize rebuild runs only on the attached path.)
    {
        std::lock_guard<std::mutex> lk(g_feedSnapMutex);
        g_feedSnap.valid = false;
    }
    for (int i = 0; i < 2; ++i) {
        if (g_swapchains[i] != XR_NULL_HANDLE) {
            xrDestroySwapchain(g_swapchains[i]);
            g_swapchains[i] = XR_NULL_HANDLE;
        }
        g_images[i].clear();
    }
    destroy_laser();
    destroy_hud_swapchain();
    g_swapW = g_swapH = 0;
    g_backbufferFmt = 0;
    // Whatever a queued rebuild was for, it has just happened.
    g_resizePending.store(false, std::memory_order_relaxed);
    reset_aer(); // the held eye images died with the swapchains
}

// Lazy: sized to the HUD capture RT, format = the eye swapchains' pick
// (CopyResource-compatible UNORM/sRGB family).
void create_hud_swapchain(uint32_t w, uint32_t h) {
    destroy_hud_swapchain();
    if (!g_swapFormat || g_session == XR_NULL_HANDLE) return;
    XrSwapchainCreateInfo sci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    sci.usageFlags = XR_SWAPCHAIN_USAGE_SAMPLED_BIT | XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
    sci.format = g_swapFormat;
    sci.sampleCount = 1;
    sci.width = w;
    sci.height = h;
    sci.faceCount = 1;
    sci.arraySize = 1;
    sci.mipCount = 1;
    if (XR_FAILED(xrCreateSwapchain(g_session, &sci, &g_hudSwapchain))) {
        BVR_LOG("xr: HUD swapchain creation failed");
        g_hudSwapchain = XR_NULL_HANDLE;
        return;
    }
    uint32_t count = 0;
    xrEnumerateSwapchainImages(g_hudSwapchain, 0, &count, nullptr);
    g_hudImages.assign(count, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
    if (XR_FAILED(xrEnumerateSwapchainImages(
            g_hudSwapchain, count, &count,
            reinterpret_cast<XrSwapchainImageBaseHeader*>(g_hudImages.data())))) {
        BVR_LOG("xr: HUD swapchain image enumeration failed");
        destroy_hud_swapchain();
        return;
    }
    g_hudSwapW = w;
    g_hudSwapH = h;
    BVR_LOG("xr: HUD quad swapchain ready (%ux%u, %u images)", w, h, count);
}

// Post-capture window composite: draw the captured HUD back onto the
// backbuffer so the FLAT window keeps its HUD while the compositor feed
// stays clean. Runs at most once per present, always AFTER the eye capture
// and AFTER the mirror's right-half re-blit. Uses the swapchain's own
// device/context so it also works with no XR session (flat testing).
void composite_hud(IDXGISwapChain* swapchain) {
    if (!bvr::hud::redirected_this_interval()) return;
    ID3D11Texture2D* bb = nullptr;
    if (FAILED(swapchain->GetBuffer(0, IID_PPV_ARGS(&bb))) || !bb) return;
    if (g_backbufferRtv && g_backbufferForRtv != bb) {
        g_backbufferRtv->Release();
        g_backbufferRtv = nullptr;
    }
    ID3D11Device* dev = nullptr;
    swapchain->GetDevice(IID_PPV_ARGS(&dev));
    if (!dev) {
        bb->Release();
        return;
    }
    if (!g_backbufferRtv) {
        if (FAILED(dev->CreateRenderTargetView(bb, nullptr, &g_backbufferRtv))) {
            dev->Release();
            bb->Release();
            return;
        }
        g_backbufferForRtv = bb;
    }
    D3D11_TEXTURE2D_DESC d{};
    bb->GetDesc(&d);
    ID3D11DeviceContext* ctx = nullptr;
    dev->GetImmediateContext(&ctx);
    if (ctx) {
        ID3D11ShaderResourceView* srv = bvr::hud::srv(ctx); // alpha-repaired copy
        if (srv) bvr::blit::alpha_premul(ctx, g_backbufferRtv, srv, d.Width, d.Height);
        ctx->Release();
    }
    dev->Release();
    bb->Release();
}

void teardown_session(const char* why) {
    g_detachedNow = false;

    // SESSION 28: never call xrDestroySession while the pace thread is parked
    // inside xrWaitFrame on that session - that is a use-after-free inside the
    // runtime. Give an outstanding wait a bounded chance to come back; if it
    // will not, keep the session alive and retry from the present loop. Worst
    // case is "the session stays up but idle", which is exactly the pre-session-28
    // behaviour, and strictly better than a crash.
    if (g_paceOutstanding) {
        if (WaitForSingleObject(g_paceDone, 200) == WAIT_OBJECT_0) {
            g_paceOutstanding = false;
        } else {
            if (g_teardownPending != why)
                BVR_LOG("xr: teardown (%s) DEFERRED - a wait is still in flight on "
                        "the pace thread; retrying each present rather than "
                        "destroying a session the runtime is still inside",
                        why);
            g_teardownPending = why;
            return;
        }
    }
    g_teardownPending = nullptr;
    BVR_LOG("xr: session teardown (%s)", why);
    input_on_session_teardown(); // action spaces are session children
    destroy_swapchains();
    {
        std::lock_guard<std::mutex> lock(g_poseMutex);
        g_poseValid = false;
    }
    g_viewsValid = false;
    g_viewsContentValid = false;
    g_projectionReady.store(false, std::memory_order_relaxed);
    g_hfovDeg.store(0.0f, std::memory_order_relaxed);
    if (g_viewSpace != XR_NULL_HANDLE) { xrDestroySpace(g_viewSpace); g_viewSpace = XR_NULL_HANDLE; }
    if (g_space != XR_NULL_HANDLE) { xrDestroySpace(g_space); g_space = XR_NULL_HANDLE; }
    if (g_session != XR_NULL_HANDLE) { xrDestroySession(g_session); g_session = XR_NULL_HANDLE; }
    if (g_context) { g_context->Release(); g_context = nullptr; }
    if (g_device) { g_device->Release(); g_device = nullptr; }
    g_sessionBegun = false;
    g_frameOpen = false;
    g_srPairOpen = false;
    g_system = XR_NULL_SYSTEM_ID;
    g_state = XR_SESSION_STATE_UNKNOWN;
    g_everFocused = false;
    g_unfocusedSinceMs = 0;
    g_framesSubmitted = 0;
    g_feedErrorHoldMs = 0; // a fresh session gets a fresh feed
    // The req/res kinds reset with the request protocol so a stale FEED result
    // can never be misread by the next session's first inline consume.
    g_paceReqKind.store(kPaceReqWait, std::memory_order_relaxed);
    g_paceResKind.store(kPaceReqWait, std::memory_order_relaxed);
    g_nextRetryMs = GetTickCount64() + 5000; // cooldown before the next attempt
}

// A soft round dot with a solid core, premultiplied so the compositor can
// blend it with plain source-alpha. Generated on the CPU once per session -
// this is a handful of kilobytes and needs no shader, no render target and no
// interaction with the game's own D3D state.
void create_laser(int64_t format) {
    uint32_t px[kLaserTexSize * kLaserTexSize];
    for (uint32_t y = 0; y < kLaserTexSize; ++y) {
        for (uint32_t x = 0; x < kLaserTexSize; ++x) {
            float dx = (x + 0.5f) / kLaserTexSize * 2.0f - 1.0f;
            float dy = (y + 0.5f) / kLaserTexSize * 2.0f - 1.0f;
            float r = sqrtf(dx * dx + dy * dy);
            float a = r >= 1.0f ? 0.0f : powf(1.0f - r, 1.5f);
            if (r < 0.25f) a = 1.0f; // bright core, so the beam stays readable
            // Red laser, premultiplied (rgb already scaled by alpha).
            uint8_t rr = static_cast<uint8_t>(255.0f * a);
            uint8_t gg = static_cast<uint8_t>(60.0f * a);
            uint8_t bb = static_cast<uint8_t>(40.0f * a);
            uint8_t aa = static_cast<uint8_t>(255.0f * a);
            px[y * kLaserTexSize + x] = (static_cast<uint32_t>(aa) << 24) |
                                        (static_cast<uint32_t>(bb) << 16) |
                                        (static_cast<uint32_t>(gg) << 8) | rr;
        }
    }

    D3D11_TEXTURE2D_DESC td{};
    td.Width = kLaserTexSize;
    td.Height = kLaserTexSize;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = static_cast<DXGI_FORMAT>(format);
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA init{px, kLaserTexSize * 4, 0};
    if (FAILED(g_device->CreateTexture2D(&td, &init, &g_laserDot))) {
        BVR_LOG("xr: laser dot texture creation failed - aim laser unavailable");
        g_laserDot = nullptr;
        return;
    }

    XrSwapchainCreateInfo sci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    sci.usageFlags = XR_SWAPCHAIN_USAGE_SAMPLED_BIT | XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
    sci.format = format;
    sci.sampleCount = 1;
    sci.width = kLaserTexSize;
    sci.height = kLaserTexSize;
    sci.faceCount = 1;
    sci.arraySize = 1;
    sci.mipCount = 1;
    XrResult r = xrCreateSwapchain(g_session, &sci, &g_laserSwapchain);
    if (XR_FAILED(r)) {
        BVR_LOG("xr: laser swapchain creation failed: %s", res_str(r));
        g_laserSwapchain = XR_NULL_HANDLE;
        destroy_laser();
        return;
    }
    uint32_t count = 0;
    xrEnumerateSwapchainImages(g_laserSwapchain, 0, &count, nullptr);
    g_laserImages.assign(count, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
    if (XR_FAILED(xrEnumerateSwapchainImages(
            g_laserSwapchain, count, &count,
            reinterpret_cast<XrSwapchainImageBaseHeader*>(g_laserImages.data())))) {
        BVR_LOG("xr: laser swapchain image enumeration failed");
        destroy_laser();
        return;
    }
    BVR_LOG("xr: aim laser ready (%ux%u dot, %u images)", kLaserTexSize, kLaserTexSize, count);
}

bool create_swapchains(IDXGISwapChain* swapchain) {
    DXGI_SWAP_CHAIN_DESC desc{};
    if (FAILED(swapchain->GetDesc(&desc))) return false;

    // Pick a swapchain format CopyResource-compatible with the backbuffer
    // (same typeless family). Prefer the sRGB view so the compositor reads the
    // game's gamma-encoded output correctly.
    uint32_t formatCount = 0;
    xrEnumerateSwapchainFormats(g_session, 0, &formatCount, nullptr);
    std::vector<int64_t> formats(formatCount);
    xrEnumerateSwapchainFormats(g_session, formatCount, &formatCount, formats.data());
    int64_t pick = 0;
    for (int64_t want : {static_cast<int64_t>(DXGI_FORMAT_R8G8B8A8_UNORM_SRGB),
                         static_cast<int64_t>(DXGI_FORMAT_R8G8B8A8_UNORM)}) {
        for (int64_t f : formats)
            if (f == want) { pick = f; break; }
        if (pick) break;
    }
    if (!pick) {
        BVR_LOG("xr: no R8G8B8A8 swapchain format offered - cannot copy backbuffer");
        return false;
    }

    XrSwapchainCreateInfo sci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    sci.usageFlags = XR_SWAPCHAIN_USAGE_SAMPLED_BIT | XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
    sci.format = pick;
    sci.sampleCount = 1;
    sci.width = desc.BufferDesc.Width;
    sci.height = desc.BufferDesc.Height;
    sci.faceCount = 1;
    sci.arraySize = 1;
    sci.mipCount = 1;
    uint32_t imageCount = 0;
    for (int i = 0; i < 2; ++i) {
        XrResult r = xrCreateSwapchain(g_session, &sci, &g_swapchains[i]);
        if (XR_FAILED(r)) {
            BVR_LOG("xr: xrCreateSwapchain failed: %s", res_str(r));
            g_swapchains[i] = XR_NULL_HANDLE;
            destroy_swapchains();
            return false;
        }
        xrEnumerateSwapchainImages(g_swapchains[i], 0, &imageCount, nullptr);
        g_images[i].assign(imageCount, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
        r = xrEnumerateSwapchainImages(
            g_swapchains[i], imageCount, &imageCount,
            reinterpret_cast<XrSwapchainImageBaseHeader*>(g_images[i].data()));
        if (XR_FAILED(r)) {
            BVR_LOG("xr: xrEnumerateSwapchainImages failed: %s", res_str(r));
            destroy_swapchains();
            return false;
        }
    }

    g_swapW = desc.BufferDesc.Width;
    g_swapH = desc.BufferDesc.Height;
    g_swapFormat = pick; // the HUD quad swapchain creates lazily with this
    g_backbufferFmt = desc.BufferDesc.Format; // for the same-size resize guard
    BVR_LOG("xr: swapchain pair %ux%u format %lld (%u images each)", g_swapW, g_swapH,
            static_cast<long long>(pick), imageCount);

    create_laser(pick); // fail-soft: no laser just means no dots
    return true;
}

// One bring-up attempt: system -> requirements -> session on the game device
// -> LOCAL space. Any failure logs, tears down, and arms the retry cooldown.
void try_bring_up(IDXGISwapChain* swapchain) {
    XrSystemGetInfo sgi{XR_TYPE_SYSTEM_GET_INFO};
    sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    XrResult r = xrGetSystem(g_instance, &sgi, &g_system);
    if (r == XR_ERROR_FORM_FACTOR_UNAVAILABLE) {
        if (!g_loggedNoHmd) {
            BVR_LOG("xr: no headset connected (runtime '%s') - will keep retrying quietly",
                    g_runtimeName);
            g_loggedNoHmd = true;
        }
        g_nextRetryMs = GetTickCount64() + 5000;
        return;
    }
    if (XR_FAILED(r)) {
        BVR_LOG("xr: xrGetSystem failed: %s", res_str(r));
        g_nextRetryMs = GetTickCount64() + 5000;
        return;
    }
    g_loggedNoHmd = false;

    XrSystemProperties sp{XR_TYPE_SYSTEM_PROPERTIES};
    xrGetSystemProperties(g_instance, g_system, &sp);
    BVR_LOG("xr: system '%s' (max layers %u)", sp.systemName,
            sp.graphicsProperties.maxLayerCount);

    // Session 41 (BSI I6): ask the runtime what per-eye render size it
    // recommends. Purely informational - a failure logs once and changes no
    // control flow, and nothing downstream conditions on the result (the eye
    // swapchains stay backbuffer-sized). Adapters read it via
    // recommended_eye_size() to annotate their resolution pickers.
    {
        XrViewConfigurationView vcv[2];
        vcv[0] = {XR_TYPE_VIEW_CONFIGURATION_VIEW};
        vcv[1] = {XR_TYPE_VIEW_CONFIGURATION_VIEW};
        uint32_t nViews = 0;
        const XrResult vr = xrEnumerateViewConfigurationViews(
            g_instance, g_system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 2, &nViews,
            vcv);
        if (XR_SUCCEEDED(vr) && nViews >= 1) {
            g_recommendedEyeW.store(vcv[0].recommendedImageRectWidth,
                                    std::memory_order_relaxed);
            g_recommendedEyeH.store(vcv[0].recommendedImageRectHeight,
                                    std::memory_order_relaxed);
            BVR_LOG("xr: view config recommends %ux%u per eye (max %ux%u, %u views)",
                    vcv[0].recommendedImageRectWidth, vcv[0].recommendedImageRectHeight,
                    vcv[0].maxImageRectWidth, vcv[0].maxImageRectHeight, nViews);
        } else {
            BVR_LOG("xr: xrEnumerateViewConfigurationViews failed: %s (informational only)",
                    res_str(vr));
        }
    }

    // Spec requires querying graphics requirements before xrCreateSession.
    PFN_xrGetD3D11GraphicsRequirementsKHR getReqs = nullptr;
    xrGetInstanceProcAddr(g_instance, "xrGetD3D11GraphicsRequirementsKHR",
                          reinterpret_cast<PFN_xrVoidFunction*>(&getReqs));
    XrGraphicsRequirementsD3D11KHR reqs{XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR};
    if (!getReqs || XR_FAILED(r = getReqs(g_instance, g_system, &reqs))) {
        BVR_LOG("xr: xrGetD3D11GraphicsRequirementsKHR failed: %s",
                getReqs ? res_str(r) : "proc not found");
        g_system = XR_NULL_SYSTEM_ID;
        g_nextRetryMs = GetTickCount64() + 5000;
        return;
    }

    if (FAILED(swapchain->GetDevice(IID_PPV_ARGS(&g_device)))) {
        BVR_LOG("xr: could not get game device from swapchain");
        teardown_session("no device");
        return;
    }
    g_device->GetImmediateContext(&g_context);
    BVR_LOG("xr: game device feature level 0x%X, runtime min 0x%X",
            g_device->GetFeatureLevel(), reqs.minFeatureLevel);

    XrGraphicsBindingD3D11KHR binding{XR_TYPE_GRAPHICS_BINDING_D3D11_KHR};
    binding.device = g_device;
    XrSessionCreateInfo sci{XR_TYPE_SESSION_CREATE_INFO};
    sci.next = &binding;
    sci.systemId = g_system;
    r = xrCreateSession(g_instance, &sci, &g_session);
    if (XR_FAILED(r)) {
        BVR_LOG("xr: xrCreateSession failed: %s", res_str(r));
        teardown_session("create failed");
        return;
    }

    XrReferenceSpaceCreateInfo rsci{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    rsci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    rsci.poseInReferenceSpace.orientation.w = 1.0f;
    r = xrCreateReferenceSpace(g_session, &rsci, &g_space);
    if (XR_FAILED(r)) {
        BVR_LOG("xr: xrCreateReferenceSpace failed: %s", res_str(r));
        teardown_session("space failed");
        return;
    }
    rsci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW; // head pose via xrLocateSpace
    r = xrCreateReferenceSpace(g_session, &rsci, &g_viewSpace);
    if (XR_FAILED(r)) {
        BVR_LOG("xr: xrCreateReferenceSpace(VIEW) failed: %s", res_str(r));
        teardown_session("view space failed");
        return;
    }

    if (!create_swapchains(swapchain)) {
        teardown_session("swapchain failed");
        return;
    }

    // M5: grip action spaces + attach (once per session; attach failure only
    // costs controller input, never the display).
    input_on_session_created(g_session, g_space);

    BVR_LOG("xr: session created on the game device - waiting for READY");
}

void pump_events() {
    XrEventDataBuffer ev{XR_TYPE_EVENT_DATA_BUFFER};
    while (xrPollEvent(g_instance, &ev) == XR_SUCCESS) {
        if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
            auto* sc = reinterpret_cast<XrEventDataSessionStateChanged*>(&ev);
            g_state = sc->state;
            BVR_LOG("xr: session state %s", state_str(g_state));
            switch (g_state) {
                case XR_SESSION_STATE_READY: {
                    XrSessionBeginInfo sbi{XR_TYPE_SESSION_BEGIN_INFO};
                    sbi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                    XrResult r = xrBeginSession(g_session, &sbi);
                    if (XR_FAILED(r)) {
                        BVR_LOG("xr: xrBeginSession failed: %s", res_str(r));
                        teardown_session("begin failed");
                    } else {
                        g_sessionBegun = true;
                        BVR_LOG("xr: session running - game is now paced by the headset");
                        trace_thread_start();
                    }
                    break;
                }
                case XR_SESSION_STATE_FOCUSED:
                    // Session 28: log the resume whenever there WAS an unfocused
                    // episode, without also requiring g_everFocused - STOPPING
                    // clears that latch, so the old condition silently swallowed
                    // the first resume after every stop.
                    if (g_unfocusedSinceMs != 0)
                        BVR_LOG("xr: FOCUSED again after %llu ms unfocused - full "
                                "pacing resumes (guard skipped %u presents this "
                                "episode)",
                                static_cast<unsigned long long>(GetTickCount64() -
                                                                g_unfocusedSinceMs),
                                g_paceSkips.load(std::memory_order_relaxed));
                    g_everFocused = true;
                    g_unfocusedSinceMs = 0;
                    g_paceSkips.store(0, std::memory_order_relaxed);
                    break;
                case XR_SESSION_STATE_STOPPING:
                    if (g_sessionBegun) {
                        xrEndSession(g_session);
                        g_sessionBegun = false;
                        // The next bring-up must pace freely again (bring-up
                        // needs frames), so the focus latch resets with it.
                        g_everFocused = false;
                        // Session 28: clear the unfocused stamp too. It used to
                        // survive STOPPING, and both of the lines that would
                        // tell a stuck user what happened are gated on it: the
                        // "pacing SKIPPED" line only prints when the stamp is 0,
                        // and the "FOCUSED again" line needs the stamp non-zero
                        // AND g_everFocused, which STOPPING just cleared. So
                        // after one STOPPING episode the log went silent in both
                        // directions and the alt-tab freeze looked like nothing
                        // happening at all.
                        g_unfocusedSinceMs = 0;
                        g_paceSkips.store(0, std::memory_order_relaxed);
                        BVR_LOG("xr: session stopped (headset idle?) - waiting for READY again");
                    }
                    break;
                case XR_SESSION_STATE_LOSS_PENDING:
                case XR_SESSION_STATE_EXITING:
                    teardown_session(state_str(g_state));
                    return; // g_session is gone; stop pumping
                default:
                    break;
            }
        } else if (ev.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) {
            teardown_session("instance loss pending");
            return;
        } else if (ev.type == XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED) {
            // s62: log-only. Bindings are resolved runtime-side; this line just
            // marks WHEN a SteamVR-family runtime rebound the controllers. Do
            // not call xrGetCurrentInteractionProfile here - it would grow the
            // mod's runtime surface past the 39 entry points the SteamVR shim
            // mirrors.
            BVR_LOG("xr: interaction profile changed (runtime rebound controllers)");
        }
        ev = {XR_TYPE_EVENT_DATA_BUFFER};
    }
}

// --------------------------------------------------------------------------
// SteamVR shim runtime selection (s62). SteamVR ships no 32-bit OpenXR
// runtime; the release zip carries bvr_steamvr32.dll (an OpenXR-on-OpenVR
// runtime, src/tools/ovrshim/) + openvr_api.dll beside the mod. When the
// native runtime attempt fails, the mod writes a runtime manifest and points
// the statically linked loader at it via XR_RUNTIME_JSON, then retries -
// verified loader mechanic: while no runtime is loaded, every pre-instance
// call re-runs discovery and reads the env var first (manifest_file.cpp:654),
// and a failed xrCreateInstance unloads again (loader_core.cpp:305). No
// registry writes, no admin, no loader-file swapping; a healthy native
// runtime (VDXR) keeps today's control flow byte-for-byte.
//
// Mode override: %LOCALAPPDATA%\BioshockVR\xr.ini, [runtime] mode=
// auto|native|steamvr. Deliberately NOT a vrpreset key - the F10 preset save
// rewrites vrpreset.ini wholesale and would silently drop an unknown key.

// Root data dir (%LOCALAPPDATA%\BioshockVR), NOT the per-game subdir - the
// runtime choice is machine-wide, like the runtimes themselves.
bool xr_root_dir(wchar_t* out /*MAX_PATH*/) {
    wchar_t local[MAX_PATH];
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, local)))
        return false;
    swprintf_s(out, MAX_PATH, L"%s\\BioshockVR", local);
    return true;
}

// Returns the mode string ("auto"/"native"/"steamvr"); logs only if the file
// exists so a default setup stays log-quiet.
void xr_mode_read(char* mode, size_t n) {
    strcpy_s(mode, n, "auto");
    wchar_t root[MAX_PATH];
    if (!xr_root_dir(root)) return;
    wchar_t ini[MAX_PATH];
    swprintf_s(ini, L"%s\\xr.ini", root);
    if (GetFileAttributesW(ini) == INVALID_FILE_ATTRIBUTES) return;
    wchar_t val[32] = {};
    GetPrivateProfileStringW(L"runtime", L"mode", L"auto", val, 32, ini);
    char narrow[32] = {};
    WideCharToMultiByte(CP_UTF8, 0, val, -1, narrow, sizeof(narrow), nullptr, nullptr);
    if (_stricmp(narrow, "native") == 0 || _stricmp(narrow, "steamvr") == 0 ||
        _stricmp(narrow, "auto") == 0)
        strcpy_s(mode, n, narrow);
    else
        BVR_LOG("xr: xr.ini [runtime] mode '%s' not recognized - using 'auto'", narrow);
    BVR_LOG("xr: runtime mode '%s' (xr.ini)", mode);
}

// The shim ships beside bioshockvr.dll (both live next to the game exe).
bool shim_dll_path(wchar_t* out /*MAX_PATH*/) {
    HMODULE self = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&init_instance), &self))
        return false;
    wchar_t path[MAX_PATH];
    if (!GetModuleFileNameW(self, path, MAX_PATH)) return false;
    wchar_t* slash = wcsrchr(path, L'\\');
    if (!slash) return false;
    *slash = 0;
    swprintf_s(out, MAX_PATH, L"%s\\bvr_steamvr32.dll", path);
    return GetFileAttributesW(out) != INVALID_FILE_ATTRIBUTES;
}

// BOM-less UTF-8 with an absolute, JSON-escaped library_path - same contract
// tools/xrsim-install.ps1 documents for the sim's manifest. Rewritten every
// launch so a moved game folder self-heals.
bool shim_write_manifest(const wchar_t* shimDll, wchar_t* manifestOut /*MAX_PATH*/) {
    wchar_t root[MAX_PATH];
    if (!xr_root_dir(root)) return false;
    wchar_t dir[MAX_PATH];
    swprintf_s(dir, L"%s\\steamvr", root);
    CreateDirectoryW(root, nullptr);
    CreateDirectoryW(dir, nullptr);
    swprintf_s(manifestOut, MAX_PATH, L"%s\\bvr_steamvr32.json", dir);

    char dllUtf8[MAX_PATH * 3] = {};
    WideCharToMultiByte(CP_UTF8, 0, shimDll, -1, dllUtf8, sizeof(dllUtf8), nullptr, nullptr);
    char escaped[MAX_PATH * 6] = {};
    size_t j = 0;
    for (size_t i = 0; dllUtf8[i] && j + 2 < sizeof(escaped); ++i) {
        if (dllUtf8[i] == '\\') escaped[j++] = '\\';
        escaped[j++] = dllUtf8[i];
    }

    FILE* f = _wfsopen(manifestOut, L"wb", _SH_DENYWR); // "wb": no BOM, no CRLF
    if (!f) return false;
    fprintf(f,
            "{\n  \"file_format_version\": \"1.0.0\",\n  \"runtime\": {\n"
            "    \"name\": \"BioshockVR SteamVR shim (OpenVR)\",\n"
            "    \"library_path\": \"%s\"\n  }\n}\n",
            escaped);
    fclose(f);
    return true;
}

bool process_is_elevated() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
    TOKEN_ELEVATION elev{};
    DWORD n = 0;
    const bool ok =
        GetTokenInformation(token, TokenElevation, &elev, sizeof(elev), &n) != 0;
    CloseHandle(token);
    return ok && elev.TokenIsElevated != 0;
}

// Logging-only heuristic: what does the 32-bit ActiveRuntime key claim? From
// this 32-bit process a plain HKLM open lands on the WOW6432Node view. NEVER
// short-circuits - the authoritative test stays "did xrCreateInstance
// succeed" (the registry can lie; the loader's own probe cannot).
void log_active_runtime_expectation() {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Khronos\\OpenXR\\1", 0,
                      KEY_READ, &key) != ERROR_SUCCESS) {
        BVR_LOG("xr: no 32-bit ActiveRuntime key registered - expecting to use "
                "the SteamVR shim if the native attempt fails");
        return;
    }
    wchar_t val[MAX_PATH] = {};
    DWORD len = sizeof(val);
    DWORD type = 0;
    const LSTATUS rs =
        RegQueryValueExW(key, L"ActiveRuntime", nullptr, &type,
                         reinterpret_cast<LPBYTE>(val), &len);
    RegCloseKey(key);
    if (rs != ERROR_SUCCESS || type != REG_SZ || !val[0]) {
        BVR_LOG("xr: 32-bit ActiveRuntime value missing - expecting to use the "
                "SteamVR shim if the native attempt fails");
        return;
    }
    char narrow[MAX_PATH * 3] = {};
    WideCharToMultiByte(CP_UTF8, 0, val, -1, narrow, sizeof(narrow), nullptr, nullptr);
    if (wcsstr(val, L"steamxr"))
        BVR_LOG("xr: 32-bit ActiveRuntime points at SteamVR ('%s') which has no "
                "32-bit support - expecting the shim", narrow);
    else if (GetFileAttributesW(val) == INVALID_FILE_ATTRIBUTES)
        BVR_LOG("xr: 32-bit ActiveRuntime manifest missing on disk ('%s') - "
                "expecting the shim", narrow);
    else
        BVR_LOG("xr: 32-bit ActiveRuntime: '%s'", narrow);
}

// One native-or-shim instance attempt: enumerate -> D3D11 check -> create.
// `label` names the attempt in every log line; `quietExplainer` suppresses
// the SteamVR wall of text when a shim fallback is about to run anyway.
XrResult try_create_instance(const char* label, bool quietExplainer) {
    uint32_t extCount = 0;
    XrResult r = xrEnumerateInstanceExtensionProperties(nullptr, 0, &extCount, nullptr);
    if (XR_FAILED(r)) {
        BVR_LOG("xr: [%s] no 32-bit OpenXR runtime reachable (%d)", label,
                static_cast<int>(r));
        return r;
    }
    std::vector<XrExtensionProperties> exts(extCount, {XR_TYPE_EXTENSION_PROPERTIES});
    xrEnumerateInstanceExtensionProperties(nullptr, extCount, &extCount, exts.data());
    bool hasD3D11 = false;
    for (const auto& e : exts)
        if (strcmp(e.extensionName, XR_KHR_D3D11_ENABLE_EXTENSION_NAME) == 0) hasD3D11 = true;
    if (!hasD3D11) {
        BVR_LOG("xr: [%s] runtime lacks XR_KHR_D3D11_enable", label);
        return XR_ERROR_EXTENSION_NOT_PRESENT;
    }

    const char* enabled[] = {XR_KHR_D3D11_ENABLE_EXTENSION_NAME};
    XrInstanceCreateInfo ici{XR_TYPE_INSTANCE_CREATE_INFO};
    strcpy_s(ici.applicationInfo.applicationName, "bioshock-vr");
    ici.applicationInfo.applicationVersion = 1;
    strcpy_s(ici.applicationInfo.engineName, "bioshock-vr");
    ici.applicationInfo.apiVersion = XR_API_VERSION_1_0;
    ici.enabledExtensionCount = 1;
    ici.enabledExtensionNames = enabled;
    r = xrCreateInstance(&ici, &g_instance);
    if (XR_FAILED(r)) {
        BVR_LOG("xr: [%s] xrCreateInstance failed: %s", label, res_str(r));
        // XR_ERROR_RUNTIME_UNAVAILABLE from a 32-bit process is very rarely a
        // broken install: SteamVR has never shipped a 32-bit OpenXR runtime, and
        // BioShock is a 32-bit game. Anyone on Lighthouse hardware (Index, Vive)
        // or running Steam Link, i.e. with SteamVR as the active runtime, lands
        // here every single time. Saying so turns "the mod does nothing" into an
        // actionable report - and it is a whole class of them.
        if (r == XR_ERROR_RUNTIME_UNAVAILABLE && !quietExplainer) {
            BVR_LOG("xr: -----------------------------------------------------------");
            BVR_LOG("xr: The active OpenXR runtime has no 32-bit support. This game is");
            BVR_LOG("xr: 32-bit, so VR cannot start. SteamVR is the usual cause: it has");
            BVR_LOG("xr: never shipped a 32-bit OpenXR runtime, which also covers Index,");
            BVR_LOG("xr: Vive and Steam Link setups.");
            BVR_LOG("xr: Fix: copy bvr_steamvr32.dll and openvr_api.dll from the release");
            BVR_LOG("xr: zip next to the game exe (beside bioshockvr.dll) - the mod then");
            BVR_LOG("xr: falls back to its bundled SteamVR shim automatically. Or set a");
            BVR_LOG("xr: runtime that ships 32-bit (Virtual Desktop's VDXR, Oculus/Meta)");
            BVR_LOG("xr: as the active OpenXR runtime and relaunch.");
            BVR_LOG("xr: -----------------------------------------------------------");
        }
        g_instance = XR_NULL_HANDLE;
        return r;
    }
    return XR_SUCCESS;
}

} // namespace

void init_instance() {
    char mode[16];
    xr_mode_read(mode, sizeof(mode));

    wchar_t shimDll[MAX_PATH];
    const bool shimPresent = shim_dll_path(shimDll);
    const bool wantNative = _stricmp(mode, "steamvr") != 0;
    const bool mayFallBack = shimPresent && _stricmp(mode, "native") != 0;

    XrResult r = XR_ERROR_RUNTIME_UNAVAILABLE;
    if (wantNative) {
        if (_stricmp(mode, "auto") == 0) log_active_runtime_expectation();
        r = try_create_instance("native", /*quietExplainer=*/mayFallBack);
    }

    if (XR_FAILED(r) && mayFallBack) {
        if (wantNative)
            BVR_LOG("xr: native runtime unavailable - falling back to the SteamVR shim");
        else
            BVR_LOG("xr: runtime mode 'steamvr' - using the SteamVR shim directly");
        if (process_is_elevated())
            BVR_LOG("xr: WARNING - game is running elevated; the shim cannot be "
                    "selected (XR_RUNTIME_JSON is ignored for admin processes). "
                    "Run the game non-elevated.");
        wchar_t manifest[MAX_PATH];
        if (!shim_write_manifest(shimDll, manifest)) {
            BVR_LOG("xr: could not write the shim manifest - VR disabled, game runs flat");
            return;
        }
        char manifestUtf8[MAX_PATH * 3] = {};
        WideCharToMultiByte(CP_UTF8, 0, manifest, -1, manifestUtf8,
                            sizeof(manifestUtf8), nullptr, nullptr);
        BVR_LOG("xr: shim manifest: %s", manifestUtf8);
        SetEnvironmentVariableW(L"XR_RUNTIME_JSON", manifest);
        r = try_create_instance("steamvr shim", /*quietExplainer=*/true);
        if (XR_FAILED(r))
            BVR_LOG("xr: SteamVR shim also failed (%s) - VR disabled, game runs "
                    "flat (is SteamVR installed?)", res_str(r));
    } else if (XR_FAILED(r) && !shimPresent && _stricmp(mode, "steamvr") == 0) {
        BVR_LOG("xr: runtime mode 'steamvr' but bvr_steamvr32.dll is not beside "
                "the mod - VR disabled, game runs flat");
    }

    if (XR_FAILED(r)) {
        if (_stricmp(mode, "native") == 0 || !mayFallBack)
            BVR_LOG("xr: VR disabled, game runs flat");
        return;
    }

    XrInstanceProperties ip{XR_TYPE_INSTANCE_PROPERTIES};
    xrGetInstanceProperties(g_instance, &ip);
    strcpy_s(g_runtimeName, ip.runtimeName);
    BVR_LOG("xr: instance created on runtime '%s' %u.%u.%u", ip.runtimeName,
            XR_VERSION_MAJOR(ip.runtimeVersion), XR_VERSION_MINOR(ip.runtimeVersion),
            XR_VERSION_PATCH(ip.runtimeVersion));

    input_create(g_instance); // M5: action set + touch bindings (fail-soft)
}

void on_present_begin(IDXGISwapChain* swapchain) {
    PhaseScope psBegin(kPhPresentBegin); // records on every return path
    g_presentsSeen.fetch_add(1, std::memory_order_relaxed);
    // Sticky, for the watchdog to aim at when no draw stage is open (session 35).
    g_presentTidSticky.store(GetCurrentThreadId(), std::memory_order_relaxed);
    trace_thread_start(); // idempotent; the flat simulation needs it too
    sample_foreground();  // USER32 on THIS thread, never on the tracer's
    // Flat stand-in for the headset-idle stall (flat has no XR session, so the
    // real path below never runs): the SAME guard decision runs with the state
    // forced VISIBLE and the focus latch forced, and a 1 s sleep stands in for
    // the runtime's blocked xrWaitFrame on frames the guard lets through.
    // Acceptance: `vrpace simidle on` with the guard ON holds presents/s near
    // the free-running rate (one 1 s keepalive hitch per 5 s); with the guard
    // OFF it collapses under 1/s - the stall being fixed, reproduced.
    if (g_simIdle.load(std::memory_order_relaxed)) {
        // SESSION 34: this now runs the SHIPPED decision (detach_skip_decision)
        // against a forced not-FOCUSED state, with a 1 s sleep standing in for
        // the blocking xrEndFrame the runtime performs while unfocused - which
        // is the call the phase timers measured at 101.8 ms and which, left
        // long enough, never returns at all.
        //
        // Flat acceptance, runnable on a desk with no headset:
        //   vrpace simidle on              -> presents/s must stay at the free
        //                                     running rate (detach ON, keepalive 0)
        //   vrpace detach off + simidle on -> presents/s must collapse to ~1
        // If those two do not differ, the fix is not doing anything, and no
        // amount of headset time will tell you that any faster.
        if (!detach_skip_decision(GetTickCount64(), false)) Sleep(1000);
        return;
    }

    if (g_instance == XR_NULL_HANDLE) return;

    // A teardown that had to be deferred (pace thread still inside xrWaitFrame)
    // retries here, first thing, every present.
    if (g_teardownPending && g_session != XR_NULL_HANDLE) {
        teardown_session(g_teardownPending);
        if (g_session != XR_NULL_HANDLE) return; // still deferred
    }

    if (!g_enabled.load(std::memory_order_relaxed)) {
        if (g_session != XR_NULL_HANDLE) teardown_session("disabled in overlay");
        return;
    }

    if (g_session == XR_NULL_HANDLE) {
        if (GetTickCount64() < g_nextRetryMs) return;
        try_bring_up(swapchain);
        if (g_session == XR_NULL_HANDLE) return;
    }

    // SESSION 28 - pump_events() now runs ABOVE the pair-hold return, and the
    // hold is aged. This is OPEN BUG 1 (VR freezes permanently after alt-tab).
    // The pair-hold used to return before the pump, so while it was set no XR
    // events were polled at all: g_state could never update, the pace guard
    // could never see FOCUSED again, and nothing below could re-arm. Alt-tab
    // stops the game presenting mid-pair, so a LEFT-tagged hold survived the
    // whole unfocused window with no sibling coming, and the only path in this
    // function that cleared anything was the `!g_enabled` teardown just above -
    // which is exactly why the VR off/on toggle was the sole recovery.
    // Pumping first costs one extra xrPollEvent on the completing present and
    // preserves the hold's invariant, which is about waitFrame and locateViews,
    // not events: neither runs on this path.
    pump_events();
    if (g_session == XR_NULL_HANDLE || !g_sessionBegun) {
        g_srPairOpen = false; // never strand a hold across a stopped session
        return;
    }

    // SESSION 34 - DETACHED PACING. Everything below this point makes blocking
    // OpenXR calls on the present thread, and while the session is not FOCUSED
    // those calls run at the runtime's not-visible cadence and pace the game
    // with it. Hand the whole frame loop to the pace thread instead: it keeps
    // submitting (so FOCUSED can be re-granted - session 28's requirement) and
    // this thread returns immediately (so the game keeps its frame rate).
    // pump_events above stays here and is non-blocking, which is what lets the
    // return to FOCUSED be seen at all.
    if (detach_skip_decision(GetTickCount64(), g_state == XR_SESSION_STATE_FOCUSED)) {
        // No BLOCKING OpenXR call on this thread this present. A frame must
        // never be left open across this return.
        const bool feedArmed = g_paceFeed.load(std::memory_order_relaxed);
        if (g_frameOpen) {
            if (feedArmed && pace_thread_start()) {
                // s54b (the doff freeze): the close goes to the PACE thread.
                // A doff-teardown VDXR can sit inside xrEndFrame indefinitely,
                // and this thread pumping is what everything else (DXGI
                // included) deadlocks behind. One request at a time: if one
                // is outstanding, retry the close next present.
                if (!g_paceOutstanding) {
                    g_feedCloseTimeNs.store(g_frameState.predictedDisplayTime,
                                            std::memory_order_relaxed);
                    g_paceReqKind.store(kPaceReqCloseOpen, std::memory_order_relaxed);
                    g_paceOutstanding = true;
                    SetEvent(g_paceReq);
                    g_frameOpen = false;
                }
            } else {
                // The legacy detach lever (BS2's session-34 A/B) keeps its
                // historical inline close - behaviour unchanged without feed.
                XrFrameEndInfo idle{XR_TYPE_FRAME_END_INFO};
                idle.displayTime = g_frameState.predictedDisplayTime;
                idle.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
                xrEndFrame(g_session, &idle);
                g_frameOpen = false;
            }
        }
        g_srPairOpen = false;
        // Session 54: THE FEED. Keep the compositor fed with layer-carrying
        // frames from the PACE thread so the runtime can re-promote (the
        // detached-with-nothing-submitted state is exactly the measured park).
        // Request-driven: at most one cycle in flight, posted per present, so
        // the single-caller frame-loop discipline is preserved and the feed
        // cadence is min(present rate, runtime cadence).
        if (g_paceFeed.load(std::memory_order_relaxed) && g_sessionBegun &&
            pace_thread_start()) {
            uint64_t now = GetTickCount64();
            if (g_paceOutstanding &&
                WaitForSingleObject(g_paceDone, 0) == WAIT_OBJECT_0) {
                const int kind = g_paceResKind.load(std::memory_order_relaxed);
                const XrResult r =
                    static_cast<XrResult>(g_paceResult.load(std::memory_order_relaxed));
                g_paceOutstanding = false;
                if (kind == kPaceReqWait && XR_SUCCEEDED(r)) {
                    // A wait completed unbegun (posted before the episode).
                    // Per spec the next xrWaitFrame blocks until this frame is
                    // begun - hand it to the pace thread to finish as a feed
                    // frame instead of beginning it inline.
                    g_paceReqKind.store(kPaceReqFeedFinish, std::memory_order_relaxed);
                    g_paceOutstanding = true;
                    SetEvent(g_paceReq);
                } else if (XR_FAILED(r)) {
                    // Do not spin on a broken session at present rate; events
                    // (pumped above) drive teardown/recovery.
                    g_feedErrorHoldMs = now + 1000;
                    static std::atomic<bool> loggedFeedFail{false};
                    if (!loggedFeedFail.exchange(true))
                        BVR_LOG("xr: feed cycle failed: %s (backing off 1 s between "
                                "attempts; logged once)", res_str(r));
                }
            }
            if (!g_paceOutstanding && now >= g_feedErrorHoldMs) {
                g_paceReqKind.store(kPaceReqFeedCycle, std::memory_order_relaxed);
                g_paceOutstanding = true;
                SetEvent(g_paceReq);
            }
        }
        return;
    }

    // Pair pacing: the previous (LEFT-tagged) present left the XR frame open;
    // this present completes the pair at its tail. No second waitFrame, no
    // re-locate - the pair shares one prediction and one pose set.
    if (g_srPairOpen) {
        if (GetTickCount64() - g_srPairOpenMs <= kPairHoldMaxMs) return;
        // The sibling never came. Drop the hold and fall through so the
        // leaked-frame close below reclaims the open frame.
        BVR_LOG("xr: pair hold outlived %u ms with no right-eye present - "
                "aborting it (presents stopped mid-pair? alt-tab/level load)",
                kPairHoldMaxMs);
        g_srPairOpen = false;
        g_srPairAborts.fetch_add(1, std::memory_order_relaxed);
    }

    // M8 (a): headset idle (session left FOCUSED after having held it) - skip
    // the blocking pacing so the flat window keeps running. Events were
    // already pumped above, so recovery needs no paced frame to be seen.
    // Session 28: close any leaked frame BEFORE the guard can return, so a
    // frame cannot sit open for the whole unfocused episode either.
    if (pace_should_skip(g_state, g_everFocused, GetTickCount64())) {
        if (g_frameOpen) {
            XrFrameEndInfo idle{XR_TYPE_FRAME_END_INFO};
            idle.displayTime = g_frameState.predictedDisplayTime;
            idle.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
            xrEndFrame(g_session, &idle);
            g_frameOpen = false;
            g_srPairOpen = false;
        }
        return;
    }

    // Deferred swapchain teardown for a real size change (see on_resize). This
    // is the safe point: past the pair-hold early return, before any wait, so no
    // XR frame is open and the compositor is not holding an image we are about
    // to free. Both flags are checked anyway - the cost is two atomic loads and
    // the failure mode it prevents is a use-after-free inside the runtime.
    if (g_resizePending.load(std::memory_order_acquire) && !g_frameOpen && !g_srPairOpen) {
        g_resizePending.store(false, std::memory_order_relaxed);
        BVR_LOG("xr: performing the queued XR swapchain rebuild (no frame open)");
        destroy_swapchains();
    }

    // A mid-session ResizeBuffers destroys the swapchains; recreate them at
    // the new backbuffer size (and recompute the fov, which depends on aspect).
    if (g_swapchains[0] == XR_NULL_HANDLE) {
        g_hfovDeg.store(0.0f, std::memory_order_relaxed);
        if (!create_swapchains(swapchain)) {
            g_projectionReady.store(false, std::memory_order_relaxed);
            return;
        }
    }

    // Belt and braces (session 26): never wait with an XR frame still begun.
    // The pair-hold is the ONLY intended open-frame state and it returns far
    // above this point, so reaching here with g_frameOpen set means a frame
    // leaked - and waiting on a leaked frame is the other way a runtime can
    // block forever. Close it and take the dropped frame instead.
    if (g_frameOpen) {
        XrFrameEndInfo leaked{XR_TYPE_FRAME_END_INFO};
        leaked.displayTime = g_frameState.predictedDisplayTime;
        leaked.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
        xrEndFrame(g_session, &leaked);
        g_frameOpen = false;
        g_srPairOpen = false;
        BVR_LOG("xr: closed a leaked open frame before waiting (pair aborted?)");
    }

    // Session 42: the opt-in pair-rate sync. This point is reached only by a
    // pair-OPENING present (the RIGHT present returned at the pair-hold above),
    // which is the one place a delay respects the intra-pair gap. No-op unless
    // a game armed it (BS1/BS2 never do).
    pace_sync_gate();

    XrResult r;
    int64_t tPhase = phase_now();
    if (g_paceOffThread.load(std::memory_order_relaxed) && pace_thread_start()) {
        // One request outstanding at a time keeps wait:begin at 1:1.
        if (!g_paceOutstanding) {
            g_paceReqKind.store(kPaceReqWait, std::memory_order_relaxed);
            g_paceOutstanding = true;
            SetEvent(g_paceReq);
        }
        uint32_t deadline = g_state == XR_SESSION_STATE_FOCUSED
                                ? kPaceDeadlineFocusedMs
                                : kPaceDeadlineIdleMs;
        bool signalled;
        {
            PhaseMark mark(kPhWait);
            signalled = WaitForSingleObject(g_paceDone, deadline) == WAIT_OBJECT_0;
        }
        phase_record(kPhWait, tPhase);
        // Session 42: accumulate the block for the cadence trace (timeout path
        // included - the time was spent either way). phase_record just stored
        // this span's us; reusing it avoids a second QPC conversion.
        g_pairWaitSumUs.fetch_add(g_phaseLastUs[kPhWait].load(std::memory_order_relaxed),
                                  std::memory_order_relaxed);
        if (!signalled) {
            // The runtime has not come back yet. Give up on THIS present only -
            // the request stays outstanding and a later present consumes it. The
            // game keeps running either way, which is the whole point.
            g_paceTimeouts.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        g_paceOutstanding = false;
        // Session 54: the completed request may be a FEED cycle from the
        // just-ended detached episode. Its frame was fully begun+ended on the
        // pace thread - there is nothing to consume inline. Take one dropped
        // present; the next one posts a fresh WAIT and paces normally.
        if (g_paceResKind.load(std::memory_order_relaxed) != kPaceReqWait) return;
        g_paceHandoffs.fetch_add(1, std::memory_order_relaxed);
        g_frameState = g_paceFrameState;
        r = static_cast<XrResult>(g_paceResult.load(std::memory_order_relaxed));
    } else {
        XrFrameWaitInfo fwi{XR_TYPE_FRAME_WAIT_INFO};
        g_frameState = {XR_TYPE_FRAME_STATE};
        uint64_t waitStart = GetTickCount64();
        r = xrWaitFrame(g_session, &fwi, &g_frameState);
        uint32_t waitMs = static_cast<uint32_t>(GetTickCount64() - waitStart);
        g_lastWaitMs.store(waitMs, std::memory_order_relaxed);
        // Telemetry for the disconnect stall: a healthy wait is one display
        // period. Long blocks with their session state tell us how THIS runtime
        // behaves when the headset idles.
        if (waitMs > 1000) {
            static uint64_t lastStallLogMs = 0;
            if (waitStart - lastStallLogMs > 5000) {
                lastStallLogMs = waitStart;
                BVR_LOG("xr: xrWaitFrame blocked %u ms INLINE on the present "
                        "thread (state %s) - `vrpace thread on` moves it off",
                        waitMs, state_str(g_state));
            }
        }
        phase_record(kPhWait, tPhase);
        g_pairWaitSumUs.fetch_add(g_phaseLastUs[kPhWait].load(std::memory_order_relaxed),
                                  std::memory_order_relaxed);
    }
    g_lastShouldRender.store(g_frameState.shouldRender != XR_FALSE, std::memory_order_relaxed);
    // Session 42: the runtime's own frame period, previously discarded. 0 stays
    // 0 on runtimes that do not fill it; consumers must treat that as unknown.
    g_displayPeriodNs.store(static_cast<int64_t>(g_frameState.predictedDisplayPeriod),
                            std::memory_order_relaxed);
    if (XR_FAILED(r)) {
        BVR_LOG("xr: xrWaitFrame failed: %s", res_str(r));
        teardown_session("waitframe failed");
        return;
    }
    tPhase = phase_now();
    XrFrameBeginInfo fbi{XR_TYPE_FRAME_BEGIN_INFO};
    r = xrBeginFrame(g_session, &fbi);
    phase_record(kPhBeginFrame, tPhase);
    if (XR_FAILED(r)) {
        BVR_LOG("xr: xrBeginFrame failed: %s", res_str(r));
        teardown_session("beginframe failed");
        return;
    }
    g_frameOpen = true;
    tPhase = phase_now(); // locate span closes after xrLocateViews below

    // M3: locate the head pose + per-eye views for the predicted display time.
    // The head pose feeds the CalcView camera drive on the game thread; the
    // views feed projection-layer submission at Present-tail.
    XrSpaceLocation sl{XR_TYPE_SPACE_LOCATION};
    bool poseOk =
        XR_SUCCEEDED(xrLocateSpace(g_viewSpace, g_space, g_frameState.predictedDisplayTime, &sl)) &&
        (sl.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) &&
        (sl.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT);
    {
        std::lock_guard<std::mutex> lock(g_poseMutex);
        g_poseValid = poseOk;
        if (poseOk) {
            g_headPose = {sl.pose.position.x, sl.pose.position.y, sl.pose.position.z,
                          sl.pose.orientation.x, sl.pose.orientation.y,
                          sl.pose.orientation.z, sl.pose.orientation.w};
        }
    }

    // The backbuffer this present carries was rendered by the game from the
    // PREVIOUS locate's head sample (lockstep: locate N feeds the tick that
    // presents at N+1). Keep that generation around - captured content must
    // be submitted with the pose it was RENDERED from, or the whole game
    // layer slides against head motion by one cycle of rotation (the M4-era
    // "head bobbing", glaring once a hand-anchored gun sat next to the
    // zero-latency laser).
    g_viewsPrev2[0] = g_viewsContent[0]; // s43b: keep one more generation for
    g_viewsPrev2[1] = g_viewsContent[1]; // the lag-2 attribution candidate
    g_viewsPrev2Valid = g_viewsContentValid;
    g_viewsContent[0] = g_views[0];
    g_viewsContent[1] = g_views[1];
    g_viewsContentValid = g_viewsValid;

    XrViewLocateInfo vli{XR_TYPE_VIEW_LOCATE_INFO};
    vli.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    vli.displayTime = g_frameState.predictedDisplayTime;
    vli.space = g_space;
    XrViewState vs{XR_TYPE_VIEW_STATE};
    uint32_t viewCount = 0;
    g_views[0] = {XR_TYPE_VIEW};
    g_views[1] = {XR_TYPE_VIEW};
    g_viewsValid =
        XR_SUCCEEDED(xrLocateViews(g_session, &vli, &vs, 2, &viewCount, g_views)) &&
        viewCount == 2 && (vs.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT);
    phase_record(kPhLocate, tPhase);
    // s51: the VDXR view logger - a bounded, self-expiring burst (the fov-watch
    // lesson: nothing unthrottled lives on the present path). Answers whether
    // the runtime reports ANY per-eye cant/IPD delta.
    if (g_viewsValid && g_viewLogLeft.load(std::memory_order_relaxed) > 0) {
        g_viewLogLeft.fetch_sub(1, std::memory_order_relaxed);
        const XrView& L = g_views[0];
        const XrView& R = g_views[1];
        const float dx = R.pose.position.x - L.pose.position.x;
        const float dy = R.pose.position.y - L.pose.position.y;
        const float dz = R.pose.position.z - L.pose.position.z;
        // Inter-eye orientation delta (the cant question): angle of qL^-1*qR.
        const float dot = L.pose.orientation.x * R.pose.orientation.x +
                          L.pose.orientation.y * R.pose.orientation.y +
                          L.pose.orientation.z * R.pose.orientation.z +
                          L.pose.orientation.w * R.pose.orientation.w;
        const float cl = fminf(1.0f, fabsf(dot));
        const float cantDeg = 2.0f * acosf(cl) * 57.29578f;
        BVR_LOG("xr: viewlog L pos %.4f %.4f %.4f quat %.4f %.4f %.4f %.4f fov "
                "%.2f/%.2f/%.2f/%.2f",
                L.pose.position.x, L.pose.position.y, L.pose.position.z,
                L.pose.orientation.x, L.pose.orientation.y, L.pose.orientation.z,
                L.pose.orientation.w, L.fov.angleLeft * 57.29578f,
                L.fov.angleRight * 57.29578f, L.fov.angleUp * 57.29578f,
                L.fov.angleDown * 57.29578f);
        BVR_LOG("xr: viewlog R pos %.4f %.4f %.4f quat %.4f %.4f %.4f %.4f fov "
                "%.2f/%.2f/%.2f/%.2f",
                R.pose.position.x, R.pose.position.y, R.pose.position.z,
                R.pose.orientation.x, R.pose.orientation.y, R.pose.orientation.z,
                R.pose.orientation.w, R.fov.angleLeft * 57.29578f,
                R.fov.angleRight * 57.29578f, R.fov.angleUp * 57.29578f,
                R.fov.angleDown * 57.29578f);
        BVR_LOG("xr: viewlog derived: eyeSep %.4f m (d %.4f %.4f %.4f) | cant %.4f deg "
                "| fovAsym L %.2f R %.2f deg",
                sqrtf(dx * dx + dy * dy + dz * dz), dx, dy, dz, cantDeg,
                (fabsf(L.fov.angleLeft) - fabsf(L.fov.angleRight)) * 57.29578f,
                (fabsf(R.fov.angleLeft) - fabsf(R.fov.angleRight)) * 57.29578f);
    }
    if (!g_viewsContentValid && g_viewsValid) {
        // Session start: no previous generation yet - better a one-frame
        // fresh-pose attribution than none.
        g_viewsContent[0] = g_views[0];
        g_viewsContent[1] = g_views[1];
        g_viewsContentValid = true;
    }
    // s43b telemetry: inter-generation rotation delta = the pose error ONE
    // generation of mis-attribution costs at the current head speed.
    if (g_viewsValid && g_viewsContentValid) {
        const XrQuaternionf& a = g_views[0].pose.orientation;
        const XrQuaternionf& b = g_viewsContent[0].pose.orientation;
        float dot = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
        if (dot < 0.0f) dot = -dot;
        if (dot > 1.0f) dot = 1.0f;
        g_poseGenDeltaDeg.store(2.0f * acosf(dot) * 57.29578f, std::memory_order_relaxed);
    }

    // Circumscribed symmetric FOV for the game render, computed once per
    // session (needs the backbuffer aspect from the quad swapchain).
    if (g_viewsValid && g_hfovDeg.load(std::memory_order_relaxed) == 0.0f && g_swapW != 0) {
        float maxHalfH = 0.0f, maxHalfV = 0.0f;
        for (const XrView& v : g_views) {
            maxHalfH = fmaxf(maxHalfH, fmaxf(-v.fov.angleLeft, v.fov.angleRight));
            maxHalfV = fmaxf(maxHalfV, fmaxf(-v.fov.angleDown, v.fov.angleUp));
        }
        float aspect = static_cast<float>(g_swapW) / static_cast<float>(g_swapH);
        float halfH = fmaxf(maxHalfH, atanf(tanf(maxHalfV) * aspect));
        float deg = fminf(halfH * 2.0f * 57.29578f, 160.0f);
        g_hfovDeg.store(deg, std::memory_order_relaxed);
        // The eye's OWN geometry, kept for anyone who needs to compare what the
        // game renders against what the headset can show. On a Quest 3 through
        // VDXR this is 54 x 55 deg half-angles - an essentially SQUARE eye - and
        // that asymmetry against a 16:9 render is what the black bands are.
        g_headsetHalfH.store(maxHalfH * 57.29578f, std::memory_order_relaxed);
        g_headsetHalfV.store(maxHalfV * 57.29578f, std::memory_order_relaxed);
        BVR_LOG("xr: headset fov half-angles h=%.1f v=%.1f deg -> game hfov %.1f deg "
                "(aspect %.3f)",
                maxHalfH * 57.29578f, maxHalfV * 57.29578f, deg, aspect);
    }

    // M5: one action sync per XR frame (with pair pacing that is once per eye
    // pair == once per game tick). Composes and publishes the synthetic pad.
    input_sync(g_session, g_frameState.predictedDisplayTime);

    // Single readiness gate for projection mode (and, through vr_camera_mode,
    // for the camera drive): never let a head-driven camera show on the quad.
    bool ready = g_viewsValid && g_hfovDeg.load(std::memory_order_relaxed) > 0.0f &&
                 g_swapchains[0] != XR_NULL_HANDLE;
    g_projectionReady.store(ready, std::memory_order_relaxed);
    if (!ready && g_cameraMode.load(std::memory_order_relaxed)) {
        uint64_t now = GetTickCount64();
        if (now - g_lastProjBlockedLogMs > 5000) {
            g_lastProjBlockedLogMs = now;
            BVR_LOG("xr: camera mode requested but projection not ready "
                    "(views %d, hfov %.1f, swapchain %d)",
                    g_viewsValid ? 1 : 0, g_hfovDeg.load(std::memory_order_relaxed),
                    g_swapchains[0] != XR_NULL_HANDLE ? 1 : 0);
        }
    }
}

// Quaternion whose +Z axis points along `f` - the direction a quad layer's
// face looks toward, so billboarding a dot at the head is "aim +Z at the eyes".
XrQuaternionf quat_facing(const float f[3]) {
    // Zero-roll frame: right = normalize(up x f), then up' = f x right.
    float up[3] = {0.0f, 1.0f, 0.0f};
    if (fabsf(f[1]) > 0.999f) { up[0] = 1.0f; up[1] = 0.0f; } // f is vertical
    float rx = up[1] * f[2] - up[2] * f[1];
    float ry = up[2] * f[0] - up[0] * f[2];
    float rz = up[0] * f[1] - up[1] * f[0];
    float rl = sqrtf(rx * rx + ry * ry + rz * rz);
    if (rl < 1e-6f) rl = 1.0f;
    rx /= rl; ry /= rl; rz /= rl;
    float ux = f[1] * rz - f[2] * ry;
    float uy = f[2] * rx - f[0] * rz;
    float uz = f[0] * ry - f[1] * rx;

    // Rotation matrix with columns (right, up', f) -> quaternion.
    float m00 = rx, m01 = ux, m02 = f[0];
    float m10 = ry, m11 = uy, m12 = f[1];
    float m20 = rz, m21 = uz, m22 = f[2];
    XrQuaternionf q{};
    float tr = m00 + m11 + m22;
    if (tr > 0.0f) {
        float s = sqrtf(tr + 1.0f) * 2.0f;
        q.w = 0.25f * s;
        q.x = (m21 - m12) / s;
        q.y = (m02 - m20) / s;
        q.z = (m10 - m01) / s;
    } else if (m00 > m11 && m00 > m22) {
        float s = sqrtf(1.0f + m00 - m11 - m22) * 2.0f;
        q.w = (m21 - m12) / s;
        q.x = 0.25f * s;
        q.y = (m01 + m10) / s;
        q.z = (m02 + m20) / s;
    } else if (m11 > m22) {
        float s = sqrtf(1.0f + m11 - m00 - m22) * 2.0f;
        q.w = (m02 - m20) / s;
        q.x = (m01 + m10) / s;
        q.y = 0.25f * s;
        q.z = (m12 + m21) / s;
    } else {
        float s = sqrtf(1.0f + m22 - m00 - m11) * 2.0f;
        q.w = (m10 - m01) / s;
        q.x = (m02 + m20) / s;
        q.y = (m12 + m21) / s;
        q.z = 0.25f * s;
    }
    return q;
}

// Session 40: the geometry below is unchanged, but it now reads a plain
// snapshot instead of the globals directly, so a SECOND laser slot can reuse
// it (BS2 is natively dual-wield and shows both hands' beams at once; see
// set_laser_slot). build_laser_layers() still snapshots slot 0 and behaves
// exactly as before - the BS1 path is untouched by construction.
struct LaserSnapshot {
    bool on = false;
    int hand = 1;
    float pitchTrim = 0.0f, yawTrim = 0.0f;
    float posFwdCm = 0.0f, posRightCm = 0.0f, posUpCm = 0.0f;
    int dots = 6;
    float nearM = 0.30f, farM = 6.0f, sizeDeg = 0.7f;
    bool muzzle = false;
    float d0[3] = {0.0f, 0.0f, -1.0f};
    float modelPitch = 0.0f, modelYaw = 0.0f, modelRoll = 0.0f;
};

// Fill `quads` with the dots along one laser's ray and return how many were
// built, capped at `budget` quads. Render thread, projection mode only.
uint32_t build_laser_from(const LaserSnapshot& ls, XrCompositionLayerQuad* quads,
                          int budget) {
    if (!ls.on || budget < 1) return 0;
    if (g_laserSwapchain == XR_NULL_HANDLE || !g_laserDot || !g_viewsValid) return 0;

    int hand = ls.hand;
    float pos[3], quat[4];
    if (!input_get_hand_pose(hand, true, pos, quat)) return 0; // AIM pose = the fire ray

    // Session 20 unification: the laser composes its pitch/yaw trim as a
    // quaternion in the controller's LOCAL frame - the model's and the fire
    // ray's EXACT algebra (core/util/xr_math.h). The old spherical
    // decomposition added the trim in world angles, which matched the other
    // two only at the tuning pose. If these ever disagree again the laser
    // stops being a calibration tool and becomes a lie.
    constexpr float kDegToRad = 3.14159265f / 180.0f;
    const float fwd[3] = {0.0f, 0.0f, -1.0f};
    float trim[4], q2[4], d[3];
    if (ls.muzzle) {
        // Muzzle ray: the beam follows the RENDERED barrel - the MODEL's trim
        // (roll included: it moves an off-axis vector) applied to the barrel
        // axis the game side derived from the driven rig this frame.
        const float d0[3] = {ls.d0[0], ls.d0[1], ls.d0[2]};
        bvr::xrmath::xr_local_trim_quat(ls.modelPitch * kDegToRad,
                                        ls.modelYaw * kDegToRad,
                                        ls.modelRoll * kDegToRad, trim);
        bvr::xrmath::quat_mul(quat, trim, q2);
        bvr::xrmath::quat_rotate(q2[0], q2[1], q2[2], q2[3], d0, d);
    } else {
        bvr::xrmath::xr_local_trim_quat(ls.pitchTrim * kDegToRad,
                                        ls.yawTrim * kDegToRad, 0.0f, trim);
        bvr::xrmath::quat_mul(quat, trim, q2);
        bvr::xrmath::quat_rotate(q2[0], q2[1], q2[2], q2[3], fwd, d);
    }

    // Ray ORIGIN offset (cm -> m) in the trimmed ray's ZERO-ROLL frame, built
    // from the ray's YAW angle exactly like the game-side build (aim.cpp
    // ue_rot_basis at roll 0) so the beam and the fire origin move together
    // by construction. Angle-built right stays defined at ANY pitch - the old
    // d x worldUp cross degenerated near vertical and silently dropped the
    // right/up offset components there.
    {
        float ofM = ls.posFwdCm * 0.01f;
        float orM = ls.posRightCm * 0.01f;
        float ouM = ls.posUpCm * 0.01f;
        if (ofM != 0.0f || orM != 0.0f || ouM != 0.0f) {
            float yaw = atan2f(d[0], -d[2]);
            float right[3] = {cosf(yaw), 0.0f, sinf(yaw)};
            float up2[3] = {right[1] * d[2] - right[2] * d[1],
                            right[2] * d[0] - right[0] * d[2],
                            right[0] * d[1] - right[1] * d[0]}; // right x d
            pos[0] += d[0] * ofM + right[0] * orM + up2[0] * ouM;
            pos[1] += d[1] * ofM + right[1] * orM + up2[1] * ouM;
            pos[2] += d[2] * ofM + right[2] * orM + up2[2] * ouM;
        }
    }

    // Billboard against the head (midpoint of the two eyes).
    float head[3] = {(g_views[0].pose.position.x + g_views[1].pose.position.x) * 0.5f,
                     (g_views[0].pose.position.y + g_views[1].pose.position.y) * 0.5f,
                     (g_views[0].pose.position.z + g_views[1].pose.position.z) * 0.5f};

    int n = ls.dots;
    if (n < 1) n = 1;
    if (n > budget) n = budget;
    float nearM = ls.nearM;
    float farM = ls.farM;
    if (nearM < 0.05f) nearM = 0.05f;
    if (farM < nearM * 1.01f) farM = nearM * 1.01f;
    float sizeRad = ls.sizeDeg * kDegToRad;

    XrSwapchainSubImage sub{};
    sub.swapchain = g_laserSwapchain;
    sub.imageRect = {{0, 0},
                     {static_cast<int32_t>(kLaserTexSize), static_cast<int32_t>(kLaserTexSize)}};

    uint32_t built = 0;
    for (int i = 0; i < n; ++i) {
        // Geometric spacing, so the dots read as a beam receding into the
        // distance instead of bunching up at the far end.
        float t = n == 1 ? 0.0f : static_cast<float>(i) / static_cast<float>(n - 1);
        float dist = nearM * powf(farM / nearM, t);
        float p[3] = {pos[0] + d[0] * dist, pos[1] + d[1] * dist, pos[2] + d[2] * dist};

        float toHead[3] = {head[0] - p[0], head[1] - p[1], head[2] - p[2]};
        float len = sqrtf(toHead[0] * toHead[0] + toHead[1] * toHead[1] + toHead[2] * toHead[2]);
        if (len < 0.02f) continue; // dot is inside the head; skip it
        toHead[0] /= len; toHead[1] /= len; toHead[2] /= len;

        XrCompositionLayerQuad& q = quads[built];
        q = {XR_TYPE_COMPOSITION_LAYER_QUAD};
        // Premultiplied alpha, so plain source-alpha blending is correct.
        q.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
        q.space = g_space;
        q.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
        q.subImage = sub;
        q.pose.position = {p[0], p[1], p[2]};
        q.pose.orientation = quat_facing(toHead);
        // Constant ANGULAR size as seen from the head: an even-width beam.
        float side = 2.0f * len * tanf(sizeRad * 0.5f);
        q.size = {side, side};
        ++built;
    }

    if (built && !g_loggedFirstLaser.exchange(true))
        BVR_LOG("xr: aim laser live (%u dot layers, hand %c)", built, hand ? 'R' : 'L');
    return built;
}

LaserSnapshot snapshot_laser_slot(int slot) {
    LaserSnapshot ls;
    if (slot == 1) {
        // Slot 1 (session 40): the additive second beam. Nothing publishes it
        // unless a game calls set_laser_slot(1, ...) - BS1 never does, so its
        // submit path sees `on == false` and adds no layers at all.
        ls.on = g_laser2On.load(std::memory_order_relaxed);
        ls.hand = g_laser2Hand.load(std::memory_order_relaxed);
        ls.pitchTrim = g_laser2PitchTrim.load(std::memory_order_relaxed);
        ls.yawTrim = g_laser2YawTrim.load(std::memory_order_relaxed);
        ls.posFwdCm = g_laser2PosFwdCm.load(std::memory_order_relaxed);
        ls.posRightCm = g_laser2PosRightCm.load(std::memory_order_relaxed);
        ls.posUpCm = g_laser2PosUpCm.load(std::memory_order_relaxed);
        ls.dots = g_laser2Dots.load(std::memory_order_relaxed);
        ls.nearM = g_laser2NearM.load(std::memory_order_relaxed);
        ls.farM = g_laser2FarM.load(std::memory_order_relaxed);
        ls.sizeDeg = g_laser2SizeDeg.load(std::memory_order_relaxed);
        return ls;
    }
    ls.on = g_laserOn.load(std::memory_order_relaxed);
    ls.hand = g_laserHand.load(std::memory_order_relaxed);
    ls.pitchTrim = g_laserPitchTrim.load(std::memory_order_relaxed);
    ls.yawTrim = g_laserYawTrim.load(std::memory_order_relaxed);
    ls.posFwdCm = g_laserPosFwdCm.load(std::memory_order_relaxed);
    ls.posRightCm = g_laserPosRightCm.load(std::memory_order_relaxed);
    ls.posUpCm = g_laserPosUpCm.load(std::memory_order_relaxed);
    ls.dots = g_laserDots.load(std::memory_order_relaxed);
    ls.nearM = g_laserNearM.load(std::memory_order_relaxed);
    ls.farM = g_laserFarM.load(std::memory_order_relaxed);
    ls.sizeDeg = g_laserSizeDeg.load(std::memory_order_relaxed);
    ls.muzzle = g_laserMuzzle.load(std::memory_order_relaxed);
    for (int i = 0; i < 3; ++i) ls.d0[i] = g_laserMuzzleD0[i].load(std::memory_order_relaxed);
    ls.modelPitch = g_laserModelPitchTrim.load(std::memory_order_relaxed);
    ls.modelYaw = g_laserModelYawTrim.load(std::memory_order_relaxed);
    ls.modelRoll = g_laserModelRollTrim.load(std::memory_order_relaxed);
    return ls;
}

uint32_t build_laser_layers(XrCompositionLayerQuad* quads) {
    return build_laser_from(snapshot_laser_slot(0), quads, kMaxLaserDots);
}

// Session 29: the acquire/copy/release used to live inside build_laser_layers,
// under `if (built)`. The aim dot references the SAME swapchain, and two
// acquires on one swapchain in a single frame is a spec violation - so the
// publish is hoisted here and called ONCE per present if either consumer built
// anything. Every quad then references this most recently released image.
bool publish_laser_image() {
    if (g_laserSwapchain == XR_NULL_HANDLE || !g_laserDot) return false;
    uint32_t index = 0;
    XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    if (XR_FAILED(xrAcquireSwapchainImage(g_laserSwapchain, &ai, &index))) return false;
    XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wi.timeout = XR_INFINITE_DURATION;
    if (XR_SUCCEEDED(xrWaitSwapchainImage(g_laserSwapchain, &wi)))
        g_context->CopyResource(g_laserImages[index].texture, g_laserDot);
    XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    xrReleaseSwapchainImage(g_laserSwapchain, &ri);
    return true;
}

// Fill one quad with the aim dot and return 1 if it was built.
//
// Unlike the laser this computes NO ray: the point arrived from the game
// thread already in XR space, converted from the exact fire-seam ray by
// game_point_to_xr. All that happens here is billboarding and sizing, so
// there is no second algebra that can drift from the first.
uint32_t build_aim_dot_slot(XrCompositionLayerQuad* quad, int slot) {
    const bool two = (slot == 1);
    if (!(two ? g_dot2On : g_dotOn).load(std::memory_order_relaxed)) return 0;
    if (!(two ? g_dot2Valid : g_dotValid).load(std::memory_order_relaxed)) return 0;
    if (g_laserSwapchain == XR_NULL_HANDLE || !g_laserDot || !g_viewsValid) return 0;
    // A publish that stopped arriving must not leave a dot floating: the ray
    // going stale is exactly the state ray_for() refuses to substitute in.
    uint64_t stamp = (two ? g_dot2StampMs : g_dotStampMs).load(std::memory_order_relaxed);
    if (stamp == 0 || GetTickCount64() - stamp > kDotStaleMs) return 0;

    float p[3] = {(two ? g_dot2X : g_dotX).load(std::memory_order_relaxed),
                  (two ? g_dot2Y : g_dotY).load(std::memory_order_relaxed),
                  (two ? g_dot2Z : g_dotZ).load(std::memory_order_relaxed)};
    float head[3] = {(g_views[0].pose.position.x + g_views[1].pose.position.x) * 0.5f,
                     (g_views[0].pose.position.y + g_views[1].pose.position.y) * 0.5f,
                     (g_views[0].pose.position.z + g_views[1].pose.position.z) * 0.5f};
    float toHead[3] = {head[0] - p[0], head[1] - p[1], head[2] - p[2]};
    float len = sqrtf(toHead[0] * toHead[0] + toHead[1] * toHead[1] + toHead[2] * toHead[2]);
    if (len < 0.02f) return 0; // inside the head
    toHead[0] /= len; toHead[1] /= len; toHead[2] /= len;

    constexpr float kDegToRad = 3.14159265f / 180.0f;
    float sizeRad = (two ? g_dot2SizeDeg : g_dotSizeDeg).load(std::memory_order_relaxed) *
                    kDegToRad;

    XrCompositionLayerQuad& q = *quad;
    q = {XR_TYPE_COMPOSITION_LAYER_QUAD};
    q.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
    q.space = g_space;
    q.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    q.subImage.swapchain = g_laserSwapchain;
    q.subImage.imageRect = {
        {0, 0}, {static_cast<int32_t>(kLaserTexSize), static_cast<int32_t>(kLaserTexSize)}};
    q.pose.position = {p[0], p[1], p[2]};
    q.pose.orientation = quat_facing(toHead);
    float side = 2.0f * len * tanf(sizeRad * 0.5f);
    q.size = {side, side};

    if (!g_loggedFirstDot.exchange(true))
        BVR_LOG("xr: aim dot live (xr %.3f %.3f %.3f, %.2f m from the head) - this is the "
                "fire-seam ray point, not a reconstruction",
                p[0], p[1], p[2], len);
    return 1;
}

uint32_t build_aim_dot_layer(XrCompositionLayerQuad* quad) {
    return build_aim_dot_slot(quad, 0);
}

// s51: the hand-anchored reference quad (see the state block comment). Same
// no-second-algebra rule as the aim dot: the position IS the located grip
// point in g_space, no offset, no ray math - only billboarding and sizing.
uint32_t build_hand_ref_quad(XrCompositionLayerQuad* quad) {
    if (!g_handQuadOn.load(std::memory_order_relaxed)) return 0;
    if (g_laserSwapchain == XR_NULL_HANDLE || !g_laserDot || !g_viewsValid) return 0;
    float p[3], hq[4];
    if (!input_get_hand_pose(g_handQuadHand.load(std::memory_order_relaxed),
                             /*aimPose=*/false, p, hq))
        return 0; // untracked hand: no quad rather than a stale one
    float head[3] = {(g_views[0].pose.position.x + g_views[1].pose.position.x) * 0.5f,
                     (g_views[0].pose.position.y + g_views[1].pose.position.y) * 0.5f,
                     (g_views[0].pose.position.z + g_views[1].pose.position.z) * 0.5f};
    float toHead[3] = {head[0] - p[0], head[1] - p[1], head[2] - p[2]};
    float len = sqrtf(toHead[0] * toHead[0] + toHead[1] * toHead[1] + toHead[2] * toHead[2]);
    if (len < 0.02f) return 0; // inside the head
    toHead[0] /= len; toHead[1] /= len; toHead[2] /= len;

    constexpr float kDegToRad = 3.14159265f / 180.0f;
    const float sizeRad = g_handQuadSizeDeg.load(std::memory_order_relaxed) * kDegToRad;

    XrCompositionLayerQuad& q = *quad;
    q = {XR_TYPE_COMPOSITION_LAYER_QUAD};
    q.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
    q.space = g_space;
    q.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    q.subImage.swapchain = g_laserSwapchain;
    q.subImage.imageRect = {
        {0, 0}, {static_cast<int32_t>(kLaserTexSize), static_cast<int32_t>(kLaserTexSize)}};
    q.pose.position = {p[0], p[1], p[2]};
    q.pose.orientation = quat_facing(toHead);
    const float side = 2.0f * len * tanf(sizeRad * 0.5f);
    q.size = {side, side};

    if (!g_loggedFirstHandQuad.exchange(true))
        BVR_LOG("xr: hand ref quad live (grip %c at xr %.3f %.3f %.3f, %.2f m from the "
                "head) - compositor-correct by construction",
                g_handQuadHand.load(std::memory_order_relaxed) ? 'R' : 'L', p[0], p[1],
                p[2], len);
    return 1;
}

// Yaw of an XR-space orientation (forward = -Z, right = +X), for the pose
// audit. Absolute convention is arbitrary; only deltas are read.
float xr_quat_yaw_deg(float qx, float qy, float qz, float qw) {
    float fwd[3] = {0.0f, 0.0f, -1.0f}, f[3];
    bvr::xrmath::quat_rotate(qx, qy, qz, qw, fwd, f);
    return atan2f(-f[0], -f[2]) * 57.29578f;
}

// SESSION 29 - the session-22 letterbox UNSQUEEZE lived here and is GONE.
//
// It assumed the engine squeezed cinematic content into a middle band over
// black, so the eye capture stretched that band back across the full image.
// Three in-headset rounds could not make it remove the bars, and the reason is
// that the premise was false: the bars are a gameswf DRAW (character 292,
// "WidescreenBars") painted over a FULL-FRAME tonemap. Proven twice over -
// the Nexus "Fullscreen Cutscenes" mod is a one-byte edit zeroing that
// sprite's PlaceObject2 scale, and a framedump taken inside the letterbox
// shows the tonemap covering the whole 2048x2048 viewport with the bar draw
// after it (ENGINE_NOTES session 29).
//
// So the stretch could only ever have CROPPED real picture and distorted the
// aspect. It is deleted rather than defaulted off: a content-destroying lever
// with a disproven rationale is a hazard, not an option. The fix is not to
// issue the draw - see hud_capture's DrawVerdict::Skip and `vrcine bars`.
void capture_frame(ID3D11Texture2D* dst, ID3D11Texture2D* backbuffer) {
    g_context->CopyResource(dst, backbuffer);
}

void on_present_end(IDXGISwapChain* swapchain) {
    PhaseScope psEnd(kPhPresentEnd); // records on every return path
    phase_heartbeat_maybe(GetTickCount64());
    if (!g_frameOpen) {
        // No XR frame this present (session gone, or the pace guard skipped
        // it). The game may still be presenting alternating stereo eyes -
        // keep draining the tag ring and keep the window pinned to one eye.
        int64_t tComp = phase_now();
        mirror_present(swapchain, sr_pop_eye());
        composite_hud(swapchain); // the window keeps its HUD even with no session
        phase_record(kPhComposite, tComp);
        // SESSION 28: name the guard, on a heartbeat, while submission is idle.
        // "Present keeps running but the headset is frozen" was diagnosable only
        // by reading the source and guessing which early return had fired, and
        // two of the lines that would have said so were suppressed. Now one line
        // every 5 s names the actual reason for as long as it lasts.
        uint64_t now = GetTickCount64();
        if (g_session != XR_NULL_HANDLE && now - g_lastIdleLogMs >= 5000) {
            g_lastIdleLogMs = now;
            const char* why = !g_enabled.load(std::memory_order_relaxed)
                                  ? "VR disabled in overlay"
                              : !g_sessionBegun ? "session not begun (waiting "
                                                  "for READY after a STOPPING)"
                              : g_detachedNow
                                  ? "DETACHED: session not FOCUSED, so the present "
                                    "thread makes no blocking OpenXR call at all; "
                                    "recovery is by event (session 34)"
                              : g_state != XR_SESSION_STATE_FOCUSED
                                  ? "pace guard: session not FOCUSED"
                              : g_swapchains[0] == XR_NULL_HANDLE
                                  ? "no XR swapchains (rebuild pending?)"
                                  : "frame not begun (waitFrame/beginFrame path)";
            BVR_LOG("xr: SUBMISSION IDLE (reason=%s | state=%s everFocused=%d "
                    "pairOpen=%d skips=%u frames=%u) - flat rendering continues",
                    why, state_str(g_state), g_everFocused ? 1 : 0,
                    g_srPairOpen ? 1 : 0,
                    g_paceSkips.load(std::memory_order_relaxed), g_framesSubmitted);
        }
        return;
    }
    g_lastIdleLogMs = 0; // submitting again - re-arm the idle heartbeat
    bool pairSecond = g_srPairOpen; // this present completes an open pair
    g_srPairOpen = false;
    g_frameOpen = false; // the pair-hold path below re-arms both

    XrCompositionLayerQuad quad{XR_TYPE_COMPOSITION_LAYER_QUAD};
    XrCompositionLayerProjection proj{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    XrCompositionLayerProjectionView projViews[2] = {
        {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW},
        {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}};
    XrCompositionLayerQuad laserQuads[kMaxLaserDots] = {};
    XrCompositionLayerQuad dotQuad{XR_TYPE_COMPOSITION_LAYER_QUAD};
    XrCompositionLayerQuad dot2Quad{XR_TYPE_COMPOSITION_LAYER_QUAD};
    XrCompositionLayerQuad handQuad{XR_TYPE_COMPOSITION_LAYER_QUAD};
    XrCompositionLayerQuad hudQuad{XR_TYPE_COMPOSITION_LAYER_QUAD};
    // The game frame is layer 0; the aim laser(s) add one quad per dot on top -
    // BOTH slots share the kMaxLaserDots budget - then up to two aim dots, the
    // s51 hand ref quad and the HUD quad (worst case 13 of the 16 runtimes
    // must accept).
    const XrCompositionLayerBaseHeader* layers[1 + kMaxLaserDots + 4] = {};
    uint32_t layerCount = 0;

    // Claim the fov the game actually rendered with (adapter readback);
    // fall back to the circumscribed target before the first readback lands.
    // The manual calibration override beats both (see its declaration).
    int hfovSrc = 0; // fov audit: 0 readback, 1 fallback, 2 manual
    float hfovDeg = g_renderedHfov.load(std::memory_order_relaxed);
    if (hfovDeg <= 0.0f) {
        hfovDeg = g_hfovDeg.load(std::memory_order_relaxed);
        hfovSrc = 1;
    }
    if (g_claimFovManual.load(std::memory_order_relaxed)) {
        hfovDeg = g_claimFovDeg.load(std::memory_order_relaxed);
        hfovSrc = 2;
    }
    bool projectionMode = g_cameraMode.load(std::memory_order_relaxed) &&
                          g_projectionReady.load(std::memory_order_relaxed) && hfovDeg > 0.0f;

    // Session 22 cinematic fallback: drop to the M2 quad screen while the
    // published gameplay verdict is false (scripted view actor, menu
    // attract), stale (a camera path bypassing CalcView), or the live fov
    // watch says the game renders a DIFFERENT fov than the option/claim (the
    // bathysphere descent: renders 104, claims 130 - measured; the scripted
    // camera still CalcViews there). Everything downstream self-adjusts per
    // present: the HUD quad, laser layers, and the HUD redirect gate all key
    // on projectionMode/srFrame, and reset_aer clears the held eye images
    // once srFrame drops. "vrcine mode stereo" instead keeps the projection
    // through fov-mismatch scenes and claims the MEASURED fov.
    if (g_cineEnabled.load(std::memory_order_relaxed) && projectionMode) {
        uint64_t pv = g_gameplayView.load(std::memory_order_relaxed);
        uint64_t stampMs = pv >> 1;
        bool strict = (pv & 1) != 0;
        bool stale = stampMs == 0 || GetTickCount64() - stampMs > kCineStaleMs;
        bool fovMm = bvr::hud::fov_mismatch();
        bool screenOnly = bvr::hud::screen_only(); // hack/loading/FMV screens
        bool stereoCine = g_cineStereo.load(std::memory_order_relaxed);
        bool wantCine = stale || !strict || screenOnly || (fovMm && !stereoCine);
        bool active = g_cineActive.load(std::memory_order_relaxed);
        if (wantCine != active) {
            if (++g_cineStreak >= kCineHysteresis) {
                g_cineStreak = 0;
                active = wantCine;
                g_cineActive.store(active, std::memory_order_relaxed);
                if (active)
                    g_cineEnters.fetch_add(1, std::memory_order_relaxed);
                else
                    g_cineExits.fetch_add(1, std::memory_order_relaxed);
                BVR_LOG("xr: cinematic quad %s (strict=%d stale=%d fovMismatch=%d "
                        "screenOnly=%d)",
                        active ? "ON" : "off", strict ? 1 : 0, stale ? 1 : 0,
                        fovMm ? 1 : 0, screenOnly ? 1 : 0);
            }
        } else {
            g_cineStreak = 0;
        }
        if (active) {
            projectionMode = false;
            g_cinePresents.fetch_add(1, std::memory_order_relaxed);
        } else if (fovMm && stereoCine) {
            // Claim-fix stereo cinematics: tag the layer with the fov the
            // game ACTUALLY renders (live watch), not the ignored option.
            //
            // SESSION 28 - this branch caused the yaw warp, and the reason is
            // worth stating where it happened. `fov_watch` used to return the
            // FOREGROUND lens off 16:9, which also latched fovMm permanently ON
            // during normal gameplay, so normal gameplay came through here and
            // the projection layer was tagged with the viewmodel frustum -
            // tanH 0.6468 over a world rendered at tanH 1.1918 at 2750x2850, a
            // 1.84x under-claim. `src=live` on the audit line was TRUE and the
            // inference drawn from it ("live means it tracks the render") was
            // false, because it was tracking the wrong lens.
            // The watch now votes for the world lens, so fovMm no longer latches
            // off 16:9 and this branch is back to being what it was designed
            // for: a genuine scripted-camera fov change (the bathysphere
            // descent renders 104 while the option reads 130). It is logged on
            // entry from now on - a substitution must never again be invisible.
            float t = 0.0f;
            unsigned long long age = 0;
            if (bvr::hud::fov_watch(&t, nullptr, &age, 500) && t > 0.05f) {
                hfovDeg = 2.0f * atanf(t) * 57.29578f;
                hfovSrc = 3; // "live"
                if (!g_loggedLiveClaim.exchange(true))
                    BVR_LOG("xr: claim substituted from the live WORLD lens "
                            "(hfov %.2f deg, age %llums) - the fov-mismatch "
                            "verdict is latched, which off 16:9 used to mean the "
                            "watch had picked the viewmodel lens; verify with "
                            "`fovaudit` that lenses/votes look right",
                            hfovDeg, age);
            }
        }
    } else {
        g_cineStreak = 0;
        if (g_cineActive.load(std::memory_order_relaxed) &&
            !g_cineEnabled.load(std::memory_order_relaxed))
            g_cineActive.store(false, std::memory_order_relaxed); // kill switch
    }

    // SequentialReentry (rung 2): one tag pop per Present, ALWAYS - the ring
    // must drain even in quad mode so a mode change cannot leave stale tags.
    // A tagged present carries a known eye (game thread pushed the sign at
    // this frame's engine submit); sign -1 = left = eye index 0, same
    // convention AER validated in-headset (depth not inverted).
    int srSign = sr_pop_eye();
    bool srFrame = projectionMode && srSign != 0;
    // HUD capture gate (session 19): the gameswf redirect runs only while
    // stereo gameplay frames flow (menus stop the eye tags -> gate drops).
    bvr::hud::set_gate(srFrame);

    // Mirror, left half: snapshot the LEFT eye before anything else runs
    // (read-only - the XR eye capture below is unaffected). The right half
    // runs after the capture block, once the right eye is safely captured.
    if (srSign < 0) mirror_present(swapchain, srSign);

    // Pair-pacing bookkeeping and the hold decision. A LEFT-tagged present
    // holds the frame open for its RIGHT sibling; anything unexpected on the
    // completing present (mode boundary, stereo toggled mid-pair) falls
    // through to the normal single-present submission and resyncs.
    if (pairSecond) {
        if (srSign == +1) {
            g_srPairs.fetch_add(1, std::memory_order_relaxed);
            // Pair-cadence sample (session 42): stamp the CLOSE of the pair -
            // the one moment per pair that exists exactly once on exactly one
            // thread. g_qpcFreq is initialized by the PhaseScope wrapping this
            // very function, so it is live by the first pair.
            int64_t nowQpc = phase_now();
            int64_t prevQpc = g_pairLastCloseQpc.exchange(nowQpc, std::memory_order_relaxed);
            if (prevQpc != 0 && g_qpcFreq != 0) {
                uint64_t us64 = static_cast<uint64_t>(nowQpc - prevQpc) * 1000000u /
                                static_cast<uint64_t>(g_qpcFreq);
                if (us64 < 1000000u) {
                    uint32_t us = static_cast<uint32_t>(us64);
                    g_pairIntCount.fetch_add(1, std::memory_order_relaxed);
                    g_pairIntSumUs.fetch_add(us, std::memory_order_relaxed);
                    g_pairIntSumSqUs.fetch_add(static_cast<uint64_t>(us) * us,
                                               std::memory_order_relaxed);
                    uint32_t m = g_pairIntMinUs.load(std::memory_order_relaxed);
                    while (us < m &&
                           !g_pairIntMinUs.compare_exchange_weak(m, us, std::memory_order_relaxed)) {}
                    m = g_pairIntMaxUs.load(std::memory_order_relaxed);
                    while (us > m &&
                           !g_pairIntMaxUs.compare_exchange_weak(m, us, std::memory_order_relaxed)) {}
                    // Session 43: spike-triggered evidence capture. Threshold =
                    // 2x the display period (fallback 25 ms when the runtime
                    // has not published one - the worst legitimate pair at any
                    // supported refresh is well under that). The >= 1 s case
                    // above stays excluded on purpose: load screens and
                    // alt-tabs are not judder.
                    if (g_spikeTrace.load(std::memory_order_relaxed)) {
                        int64_t periodNs = g_displayPeriodNs.load(std::memory_order_relaxed);
                        uint32_t thresholdUs = periodNs > 0
                            ? static_cast<uint32_t>(periodNs / 500) // 2x, ns -> us
                            : 25000u;
                        if (us > thresholdUs) {
                            g_spikeCount.fetch_add(1, std::memory_order_relaxed);
                            spike_capture(us, periodNs);
                        }
                    }
                }
            }
        } else {
            g_srPairAborts.fetch_add(1, std::memory_order_relaxed);
        }
    }
    bool pairHold = srFrame && srSign < 0 && !pairSecond &&
                    g_srPairPacing.load(std::memory_order_relaxed) &&
                    g_frameState.shouldRender && g_swapchains[0] != XR_NULL_HANDLE;

    // AlternateEye bookkeeping. imageSign is the eye offset baked into THIS
    // backbuffer: the sign was published at the tail of the previous Present
    // and consumed by the game frame that produced the current image. Only a
    // frame carrying the current eye's offset is captured into that eye's
    // swapchain; anything else (mono, or the un-offset frame right after
    // enabling) flows through index 0 like M3. SR frames bypass all of it.
    bool aerActive = projectionMode && g_aerEnabled.load(std::memory_order_relaxed);
    if (!aerActive && !srFrame) reset_aer();
    int imageSign = g_aerEyeSign.load(std::memory_order_relaxed);
    int eyeFlip = g_aerSwapEyes.load(std::memory_order_relaxed) ? -1 : 1;
    int currentEyeSign = (g_currentEye == 0 ? -1 : 1) * eyeFlip;
    bool eyeCaptured = false;

    if (g_frameState.shouldRender && g_swapchains[0] != XR_NULL_HANDLE) {
        ID3D11Texture2D* backbuffer = nullptr;
        if (SUCCEEDED(swapchain->GetBuffer(0, IID_PPV_ARGS(&backbuffer)))) {
            int srEye = srSign < 0 ? 0 : 1;
            int target = srFrame ? srEye
                         : (aerActive && imageSign == currentEyeSign) ? g_currentEye
                                                                      : 0;
            uint32_t index = 0;
            int64_t tAcq = phase_now();
            XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
            if (XR_SUCCEEDED(xrAcquireSwapchainImage(g_swapchains[target], &ai, &index))) {
                XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
                wi.timeout = XR_INFINITE_DURATION;
                bool imageReady;
                {
                    PhaseMark mark(kPhAcquire); // XR_INFINITE_DURATION lives here
                    imageReady = XR_SUCCEEDED(xrWaitSwapchainImage(g_swapchains[target], &wi));
                }
                phase_record(kPhAcquire, tAcq);
                if (imageReady) {
                    // Same size + same typeless family (guaranteed at creation),
                    // so a straight GPU copy carries the frame - overlay
                    // included. Under an engine letterbox the copy becomes an
                    // unsqueeze blit instead (session 22, capture_frame).
                    int64_t tCap = phase_now();
                    capture_frame(g_images[target][index].texture, backbuffer);
                    phase_record(kPhCapture, tCap);
                }
                XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
                xrReleaseSwapchainImage(g_swapchains[target], &ri);

                // Captured content is attributed to the locate generation it
                // was RENDERED from (g_viewsContent), never the fresh one -
                // the compositor reprojects from there to display time.
                // s43b: the generation is selectable (g_poseLag doc at the
                // state block). Default 1 == the historical g_viewsContent
                // behavior; only the Infinite adapter ever changes it.
                if (srFrame) {
                    int lag = g_poseLag.load(std::memory_order_relaxed);
                    // Pick the pose GENERATION as a pair - the s50 rendered
                    // tag needs both eyes of the same locate to reconstruct
                    // the parallel render camera.
                    const XrView* gen;
                    if (lag == 0 && g_viewsValid)
                        gen = g_views;
                    else if (lag == 2 && g_viewsPrev2Valid)
                        gen = g_viewsPrev2;
                    else
                        gen = g_viewsContent;
                    if (g_eyeTagRendered.load(std::memory_order_relaxed))
                        g_eyePose[srEye] =
                            parallel_eye_tag(gen[0].pose, gen[1].pose, srEye,
                                             g_eyeTagIpdMm.load(std::memory_order_relaxed));
                    else
                        g_eyePose[srEye] = gen[srEye].pose;
                    g_eyeValid[srEye] = true;
                    if (!g_loggedFirstSr.exchange(true))
                        BVR_LOG("xr: first SequentialReentry eye frame captured "
                                "(eye %c)", srEye == 0 ? 'L' : 'R');
                } else if (aerActive && target == g_currentEye &&
                           imageSign == currentEyeSign) {
                    if (g_eyeTagRendered.load(std::memory_order_relaxed))
                        g_eyePose[g_currentEye] = parallel_eye_tag(
                            g_viewsContent[0].pose, g_viewsContent[1].pose, g_currentEye,
                            g_eyeTagIpdMm.load(std::memory_order_relaxed));
                    else
                        g_eyePose[g_currentEye] = g_viewsContent[g_currentEye].pose;
                    g_eyeValid[g_currentEye] = true;
                    eyeCaptured = true;
                }

                if (pairHold) {
                    // Left eye captured; submission happens when the RIGHT
                    // present completes this XR frame. Both eye poses come
                    // from this frame's single locate (g_views is untouched
                    // until the next waitFrame).
                    g_srPairOpen = true;
                    g_srPairOpenMs = GetTickCount64(); // aged; see kPairHoldMaxMs
                    g_frameOpen = true;
                    if (!g_loggedFirstPair.exchange(true))
                        BVR_LOG("xr: pair pacing live (one waitFrame per eye "
                                "pair)");
                    backbuffer->Release();
                    composite_hud(swapchain); // capture done - window gets HUD
                    return;
                }

                XrSwapchainSubImage sub{};
                sub.swapchain = g_swapchains[target];
                sub.imageRect = {{0, 0},
                                 {static_cast<int32_t>(g_swapW), static_cast<int32_t>(g_swapH)}};

                if (projectionMode) {
                    // fov = the symmetric fov the game rendered with (hfov
                    // written by the adapter, vfov via aspect).
                    float halfH = hfovDeg * 0.5f / 57.29578f;
                    float halfV = atanf(tanf(halfH) * static_cast<float>(g_swapH) /
                                        static_cast<float>(g_swapW));
                    // FOV audit (session 21): the tangents this layer is
                    // TAGGED with, logged on change. The flat gate compares
                    // them against tangents recovered from dumpframe cb0.
                    float tanClaimH = tanf(halfH), tanClaimV = tanf(halfV);
                    if (tanClaimH != g_auditTanH.load(std::memory_order_relaxed) ||
                        tanClaimV != g_auditTanV.load(std::memory_order_relaxed) ||
                        hfovSrc != g_auditFovSrc.load(std::memory_order_relaxed)) {
                        g_auditTanH.store(tanClaimH, std::memory_order_relaxed);
                        g_auditTanV.store(tanClaimV, std::memory_order_relaxed);
                        g_auditFovSrc.store(hfovSrc, std::memory_order_relaxed);
                        BVR_LOG("xr: fovaudit submit tanH=%.6f tanV=%.6f (hfov %.2f deg, "
                                "src=%s, swap %ux%u, symmetric both eyes)",
                                tanClaimH, tanClaimV, hfovDeg, kFovSrcNames[hfovSrc],
                                g_swapW, g_swapH);
                    }
                    // AER: each eye shows its swapchain's most recently
                    // released image with the pose stored at its capture (the
                    // compositor reprojects the stale eye). Until both eyes
                    // hold an offset image: M3 mono - fresh image to both eyes
                    // with the per-eye located poses. Converges in 2 frames.
                    bool stereo = (aerActive || srFrame) && g_eyeValid[0] &&
                                  g_eyeValid[1];
                    for (int eye = 0; eye < 2; ++eye) {
                        if (stereo) {
                            projViews[eye].pose = g_eyePose[eye];
                            projViews[eye].subImage = sub;
                            projViews[eye].subImage.swapchain = g_swapchains[eye];
                        } else {
                            projViews[eye].pose = g_viewsContent[eye].pose;
                            projViews[eye].subImage = sub;
                        }
                        projViews[eye].fov = {-halfH, halfH, halfV, -halfV};
                    }
                    // s51: bank the edge-telemetry snapshot (armed only; the
                    // game-thread sampler copies it out - see the header).
                    if (g_edgeSnapOn.load(std::memory_order_relaxed)) {
                        std::lock_guard<std::mutex> lk(g_edgeSnapMutex);
                        g_edgeSnap.valid = g_viewsValid;
                        g_edgeSnap.stampMs = GetTickCount64();
                        for (int e = 0; e < 2; ++e) {
                            g_edgeSnap.locPos[e][0] = g_views[e].pose.position.x;
                            g_edgeSnap.locPos[e][1] = g_views[e].pose.position.y;
                            g_edgeSnap.locPos[e][2] = g_views[e].pose.position.z;
                            g_edgeSnap.locQuat[e][0] = g_views[e].pose.orientation.x;
                            g_edgeSnap.locQuat[e][1] = g_views[e].pose.orientation.y;
                            g_edgeSnap.locQuat[e][2] = g_views[e].pose.orientation.z;
                            g_edgeSnap.locQuat[e][3] = g_views[e].pose.orientation.w;
                            g_edgeSnap.locFov[e][0] = g_views[e].fov.angleLeft;
                            g_edgeSnap.locFov[e][1] = g_views[e].fov.angleRight;
                            g_edgeSnap.locFov[e][2] = g_views[e].fov.angleUp;
                            g_edgeSnap.locFov[e][3] = g_views[e].fov.angleDown;
                            g_edgeSnap.tagPos[e][0] = projViews[e].pose.position.x;
                            g_edgeSnap.tagPos[e][1] = projViews[e].pose.position.y;
                            g_edgeSnap.tagPos[e][2] = projViews[e].pose.position.z;
                            g_edgeSnap.tagQuat[e][0] = projViews[e].pose.orientation.x;
                            g_edgeSnap.tagQuat[e][1] = projViews[e].pose.orientation.y;
                            g_edgeSnap.tagQuat[e][2] = projViews[e].pose.orientation.z;
                            g_edgeSnap.tagQuat[e][3] = projViews[e].pose.orientation.w;
                        }
                        g_edgeSnap.claimTanH = tanf(halfH);
                        g_edgeSnap.claimTanV = tanf(halfV);
                    }
                    // Pose-tag audit (session 21, armed by `fovaudit pose on`):
                    // tagged-vs-consumed yaw, rate-limited. In-headset only.
                    if (stereo && g_poseAudit.load(std::memory_order_relaxed)) {
                        uint64_t now = GetTickCount64();
                        if (now - g_lastPoseAuditLogMs >= 500) {
                            g_lastPoseAuditLogMs = now;
                            float yawTag = xr_quat_yaw_deg(
                                projViews[0].pose.orientation.x, projViews[0].pose.orientation.y,
                                projViews[0].pose.orientation.z, projViews[0].pose.orientation.w);
                            float yawUse = xr_quat_yaw_deg(
                                g_consumedHeadQuat[0].load(std::memory_order_relaxed),
                                g_consumedHeadQuat[1].load(std::memory_order_relaxed),
                                g_consumedHeadQuat[2].load(std::memory_order_relaxed),
                                g_consumedHeadQuat[3].load(std::memory_order_relaxed));
                            float d = yawTag - yawUse;
                            while (d > 180.0f) d -= 360.0f;
                            while (d < -180.0f) d += 360.0f;
                            BVR_LOG("xr: poseaudit tagged yaw %.2f vs consumed %.2f "
                                    "(delta %.2f deg, samples %u)",
                                    yawTag, yawUse, d,
                                    g_consumedHeadCount.load(std::memory_order_relaxed));
                        }
                    }
                    proj.space = g_space;
                    proj.viewCount = 2;
                    proj.views = projViews;
                    layers[0] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&proj);
                    g_lastLayer = 2;
                    if (!g_loggedFirstProjection.exchange(true))
                        BVR_LOG("xr: first projection-layer frame (claimed hfov %.1f deg)",
                                hfovDeg);
                    if (stereo && !g_loggedFirstStereo.exchange(true))
                        BVR_LOG("xr: alternate-eye stereo live (both eyes hold offset images)");
                } else {
                    float width = g_screenWidthM.load(std::memory_order_relaxed);
                    // Session 22 (user feedback, first headset run): SCREEN-ONLY
                    // intervals (hack minigame, loading screens - world-less 2D
                    // boards) ride the HEAD-LOCKED view space, exactly like the
                    // pause-menu panel, so the board is centered on wherever the
                    // player is looking instead of the recenter-origin facing.
                    // Cinematic scenes (fov-mismatch/strict legs) and the plain
                    // camera-off screen keep the world-locked space unchanged.
                    bool headLock = g_cineActive.load(std::memory_order_relaxed) &&
                                    bvr::hud::screen_only() &&
                                    g_viewSpace != XR_NULL_HANDLE;
                    quad.space = headLock ? g_viewSpace : g_space;
                    quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
                    quad.subImage = sub;
                    quad.pose.orientation.w = 1.0f;
                    quad.pose.position = {0.0f, 0.0f,
                                          -g_screenDistM.load(std::memory_order_relaxed)};
                    quad.size = {width, width * static_cast<float>(g_swapH) /
                                            static_cast<float>(g_swapW)};
                    layers[0] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&quad);
                    g_lastLayer = 1;
                }
                layerCount = 1;
            }
            backbuffer->Release();
        }
    }

    // Mirror, right half: the right eye's XR capture is done (or was skipped
    // this present) - pin the backbuffer to the held left image before the
    // real Present displays it. The window HUD composite comes after (the
    // re-blit would overwrite it).
    int64_t tComp = phase_now();
    if (srSign > 0) mirror_present(swapchain, srSign);
    composite_hud(swapchain);
    phase_record(kPhComposite, tComp);

    // Aim laser on top of the game frame - projection mode only, since in quad
    // ("cinema screen") mode there is no world for it to point into.
    if (layerCount && projectionMode) {
        // Slot 0 first, then the session-40 second beam within whatever is
        // LEFT of the shared dot budget - the layer arrays never grow, so a
        // dual-wield game cannot push the total past what slot 0 alone could.
        uint32_t dots = build_laser_from(snapshot_laser_slot(0), laserQuads, kMaxLaserDots);
        uint32_t dots2 = build_laser_from(snapshot_laser_slot(1), laserQuads + dots,
                                          kMaxLaserDots - static_cast<int>(dots));
        uint32_t aimDot = build_aim_dot_slot(&dotQuad, 0);
        uint32_t aimDot2 = build_aim_dot_slot(&dot2Quad, 1);
        uint32_t handRef = build_hand_ref_quad(&handQuad);
        // ONE acquire feeds every quad that referenced this swapchain, lasers
        // and aim dots alike - two acquires in a frame would be invalid.
        if ((dots || dots2 || aimDot || aimDot2 || handRef) && !publish_laser_image()) {
            dots = 0;
            dots2 = 0;
            aimDot = 0;
            aimDot2 = 0;
            handRef = 0;
        }
        for (uint32_t i = 0; i < dots + dots2; ++i)
            layers[layerCount++] =
                reinterpret_cast<const XrCompositionLayerBaseHeader*>(&laserQuads[i]);
        if (aimDot)
            layers[layerCount++] =
                reinterpret_cast<const XrCompositionLayerBaseHeader*>(&dotQuad);
        if (aimDot2)
            layers[layerCount++] =
                reinterpret_cast<const XrCompositionLayerBaseHeader*>(&dot2Quad);
        if (handRef)
            layers[layerCount++] =
                reinterpret_cast<const XrCompositionLayerBaseHeader*>(&handQuad);
        g_laserLayersSubmitted.store(dots + dots2, std::memory_order_relaxed);
        g_dotLayersSubmitted.store(aimDot + aimDot2, std::memory_order_relaxed);
    } else {
        g_laserLayersSubmitted.store(0, std::memory_order_relaxed);
        g_dotLayersSubmitted.store(0, std::memory_order_relaxed);
    }

    // HUD floating quad (session 19): head-locked, fed from the gameswf
    // capture - or, s52, from a game-registered provider (Infinite's GFx
    // lane). Submitted only in projection mode with fresh HUD content and
    // a live view space.
    if (layerCount && projectionMode && g_viewSpace != XR_NULL_HANDLE) {
        HudTextureProviderFn prov = g_hudTexProvider.load(std::memory_order_relaxed);
        ID3D11Texture2D* hudTex =
            prov ? prov(g_context) : bvr::hud::texture(g_context); // alpha-repaired
        if (hudTex) {
            D3D11_TEXTURE2D_DESC hd{};
            hudTex->GetDesc(&hd);
            if (g_hudSwapchain == XR_NULL_HANDLE || g_hudSwapW != hd.Width ||
                g_hudSwapH != hd.Height)
                create_hud_swapchain(hd.Width, hd.Height);
            if (g_hudSwapchain != XR_NULL_HANDLE) {
                uint32_t idx = 0;
                XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
                if (XR_SUCCEEDED(xrAcquireSwapchainImage(g_hudSwapchain, &ai, &idx))) {
                    XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
                    wi.timeout = XR_INFINITE_DURATION;
                    if (XR_SUCCEEDED(xrWaitSwapchainImage(g_hudSwapchain, &wi)))
                        g_context->CopyResource(g_hudImages[idx].texture, hudTex);
                    XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
                    xrReleaseSwapchainImage(g_hudSwapchain, &ri);

                    // The processed capture is premultiplied rgb + repaired
                    // alpha - premultiplied compositor semantics (no
                    // UNPREMULTIPLIED bit).
                    hudQuad.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
                    hudQuad.space = g_viewSpace; // head-locked
                    hudQuad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
                    hudQuad.subImage.swapchain = g_hudSwapchain;
                    hudQuad.subImage.imageRect = {
                        {0, 0}, {static_cast<int32_t>(hd.Width), static_cast<int32_t>(hd.Height)}};
                    hudQuad.pose.orientation.w = 1.0f;
                    hudQuad.pose.position = {0.0f, g_hudUpM.load(std::memory_order_relaxed),
                                             -g_hudDistM.load(std::memory_order_relaxed)};
                    float w = g_hudWidthM.load(std::memory_order_relaxed);
                    hudQuad.size = {w, w * static_cast<float>(hd.Height) /
                                           static_cast<float>(hd.Width)};
                    layers[layerCount++] =
                        reinterpret_cast<const XrCompositionLayerBaseHeader*>(&hudQuad);
                    g_hudFramesSubmitted.fetch_add(1, std::memory_order_relaxed);
                    if (!g_loggedFirstHudQuad.exchange(true))
                        BVR_LOG("xr: HUD quad live (%ux%u, %.2f m wide at %.2f m)",
                                hd.Width, hd.Height, w,
                                g_hudDistM.load(std::memory_order_relaxed));
                }
            }
        }
    }

    XrFrameEndInfo fei{XR_TYPE_FRAME_END_INFO};
    fei.displayTime = g_frameState.predictedDisplayTime;
    fei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    fei.layerCount = layerCount;
    fei.layers = layerCount ? layers : nullptr;
    int64_t tEnd = phase_now();
    XrResult r;
    {
        PhaseMark mark(kPhEndFrame); // the measured pacer - name it while in flight
        r = xrEndFrame(g_session, &fei);
    }
    phase_record(kPhEndFrame, tEnd);
    if (XR_FAILED(r)) {
        BVR_LOG("xr: xrEndFrame failed: %s", res_str(r));
        teardown_session("endframe failed");
        return;
    }
    if (layerCount && ++g_framesSubmitted == 1)
        BVR_LOG("xr: first frame submitted to the headset (%ux%u quad)", g_swapW, g_swapH);

    // Session 54: bank the feed snapshot - the layer set the pace thread will
    // re-submit while the session is parked not-FOCUSED. Projection (g_lastLayer
    // 2) or the screen quad (1); the HUD/laser quads are skipped, a keepalive
    // does not need them. Gated on the feed lever so BS1/BS2 do no new work.
    if (g_paceFeed.load(std::memory_order_relaxed) && layerCount &&
        (g_lastLayer == 2 || g_lastLayer == 1)) {
        std::lock_guard<std::mutex> lk(g_feedSnapMutex);
        g_feedSnap.valid = true;
        g_feedSnap.isProj = (g_lastLayer == 2);
        if (g_feedSnap.isProj) {
            g_feedSnap.proj = proj;
            g_feedSnap.views[0] = projViews[0];
            g_feedSnap.views[1] = projViews[1];
        } else {
            g_feedSnap.quad = quad;
        }
    }

    // AER sign publish, AFTER submit: eye flip only once an offset frame was
    // actually captured, so CalcView for the next game frame simulates exactly
    // the eye the next Present's copy will feed (design: STATUS.md session 2).
    if (aerActive) {
        if (eyeCaptured) g_currentEye ^= 1;
        g_aerEyeSign.store((g_currentEye == 0 ? -1 : 1) * eyeFlip,
                           std::memory_order_relaxed);
    }
}

void on_resize(unsigned width, unsigned height, unsigned format) {
    // Backbuffer-derived views MUST go regardless: DXGI fails the game's
    // ResizeBuffers call outright while anyone still holds a buffer reference.
    // Our own copy textures are merely size-tied and recreate lazily - cheap.
    release_mirror();
    if (g_backbufferRtv) {
        g_backbufferRtv->Release();
        g_backbufferRtv = nullptr;
        g_backbufferForRtv = nullptr;
    }

    // Session 23: an external machine issues mid-session ResizeBuffers calls at
    // the SAME size (focus/mode churn - DisplayFusion and the 2K overlay are
    // both live there), and unconditionally xrDestroySwapchain'ing on every one
    // tears XR swapchains out from under the compositor. A captured crash shows
    // VDXR calling into d3d11 with 0xDEDEDEDE-poisoned pointers - freed-object
    // shape. The XR swapchains hold runtime images, not DXGI backbuffer
    // references, so they cannot block the game's resize: keep them whenever
    // the geometry they were built for is unchanged. Width/height/format of 0
    // mean "unchanged" in DXGI's own convention; anything unknown falls through
    // to the old destroy-and-recreate path.
    const bool sameSize = g_swapchains[0] != XR_NULL_HANDLE && g_swapW && g_swapH &&
                          (width == 0 || width == g_swapW) &&
                          (height == 0 || height == g_swapH) &&
                          (format == 0 /*DXGI_FORMAT_UNKNOWN*/ || format == g_backbufferFmt);
    if (sameSize) {
        BVR_LOG("xr: same-size ResizeBuffers (%ux%u fmt %u) - XR swapchains kept",
                width, height, format);
        return;
    }

    // A REAL size change used to destroy the swapchains right here. This
    // function runs inside the game's ResizeBuffersDetour, which can land
    // anywhere - including between xrBeginFrame and xrEndFrame, with the
    // compositor still holding images from the frame in flight. That is the
    // documented-open half of the session-23 crash (the same-size case above got
    // its guard then; the real-size case did not), and the captured dump for it
    // is 22 frames of VDXR calling into d3d11 through 0xDEDEDEDE pointers.
    //
    // So only QUEUE it. The frame loop performs the destroy at a point where it
    // can prove no XR frame is open, and the existing null-swapchain branch in
    // on_present_begin then rebuilds at the new size.
    g_resizePending.store(true, std::memory_order_release);
    BVR_LOG("xr: real ResizeBuffers (%ux%u fmt %u) - XR swapchain rebuild QUEUED for a safe "
            "point in the frame loop",
            width, height, format);
}

void draw_debug_ui() {
    if (g_instance == XR_NULL_HANDLE) {
        ImGui::Text("VR: no OpenXR runtime - flat mode");
        return;
    }
    if (g_session == XR_NULL_HANDLE) {
        ImGui::Text("VR: runtime '%s', no session (headset off?)", g_runtimeName);
    } else {
        ImGui::Text("VR: '%s' session %s, %u frames", g_runtimeName, state_str(g_state),
                    g_framesSubmitted);
    }

    // ---- VR PACING: the session-34 fix, judged in the headset ---------------
    // FIRST and open by default. The whole point is that the alt-tab A/B must
    // not require alt-tabbing: reaching a keyboard is what drops the session out
    // of FOCUSED, which is the very transition this section exists to fix.
    if (ImGui::CollapsingHeader("VR PACING  <-- the freeze fix",
                                ImGuiTreeNodeFlags_DefaultOpen)) {
        // Presents per second, sampled here so it is live while the user looks.
        static uint64_t lastSampleMs = 0;
        static uint32_t lastPresents = 0;
        static uint32_t presentsPerSec = 0;
        uint64_t nowMs = GetTickCount64();
        uint32_t presents = g_presentsSeen.load(std::memory_order_relaxed);
        if (lastSampleMs == 0) {
            lastSampleMs = nowMs;
            lastPresents = presents;
        } else if (nowMs - lastSampleMs >= 1000) {
            presentsPerSec = static_cast<uint32_t>((presents - lastPresents) * 1000ull /
                                                   (nowMs - lastSampleMs));
            lastSampleMs = nowMs;
            lastPresents = presents;
        }
        bool focused = g_state == XR_SESSION_STATE_FOCUSED;
        ImGui::Text("session %s%s | presents/s %u", state_str(g_state),
                    g_everFocused.load(std::memory_order_relaxed) ? "" : " (never focused)",
                    presentsPerSec);

        bool detach = g_paceDetach.load(std::memory_order_relaxed);
        if (ImGui::Checkbox("Do not let an unfocused headset pace the game", &detach))
            set_pace_detach(detach);
        ImGui::TextWrapped(
            "ON = while the session is not FOCUSED the frame loop runs on its own "
            "thread, so the game keeps its frame rate and the runtime still gets "
            "frames (which is how FOCUSED comes back). OFF = the old behaviour: "
            "the game runs at the runtime's not-visible cadence, about 10 Hz, "
            "which in the headset reads as a freeze.");
        if (g_detachedNow)
            ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f),
                               "DETACHED right now - the game is NOT being paced");
        ImGui::Text("episodes %u | unpaced presents %u | keepalive frames %u",
                    g_detachEpisodes.load(std::memory_order_relaxed),
                    g_detachSkips.load(std::memory_order_relaxed),
                    g_detachKeepalives.load(std::memory_order_relaxed));
        int ka = static_cast<int>(g_paceKeepaliveMs.load(std::memory_order_relaxed));
        if (ImGui::SliderInt("Keepalive every (ms)", &ka, 250, 5000))
            g_paceKeepaliveMs.store(static_cast<uint32_t>(ka), std::memory_order_relaxed);
        ImGui::TextWrapped(
            "How often the frame loop runs while unfocused. Each one costs a "
            "single ~100 ms hitch; between them the game runs free. Too rare and "
            "the headset may be slower to hand focus back.");

        // Session 54: the raffle-wedge root fix.
        bool feed = g_paceFeed.load(std::memory_order_relaxed);
        if (ImGui::Checkbox("Feed the compositor while parked (raffle-wedge fix)", &feed))
            set_pace_feed(feed);
        ImGui::TextWrapped(
            "ON = while the session is not FOCUSED the pace thread re-submits the "
            "last real image (layers included), so the runtime keeps seeing a "
            "rendering app and hands FOCUSED back on its own - the raffle-class "
            "wedge cannot park. OFF = empty frames only, the measured park.");
        ImGui::Text("feed cycles %u (%u layered)",
                    g_feedCycles.load(std::memory_order_relaxed),
                    g_feedLayered.load(std::memory_order_relaxed));

        // The phase table. This is the instrument that named the blocking call;
        // it stays visible because "which call owns the frame time" is the only
        // question that distinguishes a fix from a coincidence.
        if (ImGui::TreeNode("Present-path phases (last / max us)")) {
            ImGui::Text("shouldRender = %d",
                        g_lastShouldRender.load(std::memory_order_relaxed) ? 1 : 0);
            for (int i = 0; i < kPhCount; ++i)
                ImGui::Text("%-13s %6u / %6u", kPhaseNames[i],
                            g_phaseLastUs[i].load(std::memory_order_relaxed),
                            g_phaseMaxUs[i].load(std::memory_order_relaxed));
            if (ImGui::Button("Reset maxima"))
                for (int i = 0; i < kPhCount; ++i)
                    g_phaseMaxUs[i].store(0, std::memory_order_relaxed);
            ImGui::TreePop();
        }
    }

    bool enabled = g_enabled.load(std::memory_order_relaxed);
    if (ImGui::Checkbox("VR enabled (paces game to headset)", &enabled))
        g_enabled.store(enabled, std::memory_order_relaxed);

    bool camMode = g_cameraMode.load(std::memory_order_relaxed);
    if (ImGui::Checkbox("VR camera mode (6DOF head drive)", &camMode))
        g_cameraMode.store(camMode, std::memory_order_relaxed);
    if (camMode) {
        bool aer = g_aerEnabled.load(std::memory_order_relaxed);
        if (ImGui::Checkbox("AlternateEye stereo test (judders)", &aer))
            g_aerEnabled.store(aer, std::memory_order_relaxed);
        bool pair = g_srPairPacing.load(std::memory_order_relaxed);
        if (ImGui::Checkbox("SR pair pacing (one waitFrame per eye pair)", &pair))
            g_srPairPacing.store(pair, std::memory_order_relaxed);
        if (pair) {
            // Session 42 judder A/B: evenly spaced pair opens vs the game's own
            // present speed. Headset-judgeable, so it must live on a checkbox.
            bool sync = g_paceSync.load(std::memory_order_relaxed);
            if (ImGui::Checkbox("Sync pair rate to headset refresh (judder A/B)", &sync))
                g_paceSync.store(sync, std::memory_order_relaxed);
        }
        bool cine = g_cineEnabled.load(std::memory_order_relaxed);
        if (ImGui::Checkbox("Cinematic auto-detect (cutscenes/screens)", &cine))
            g_cineEnabled.store(cine, std::memory_order_relaxed);
        if (cine) {
            bool stereoC = g_cineStereo.load(std::memory_order_relaxed);
            if (ImGui::Checkbox("Cinematics as stereo projection (off = big screen)",
                                &stereoC))
                g_cineStereo.store(stereoC, std::memory_order_relaxed);
        }
        // Session 29 cinematic behaviour.
        bool barsHide = bvr::hud::bars_hidden();
        if (ImGui::Checkbox("Hide cutscene black bars", &barsHide))
            bvr::hud::set_bars_hidden(barsHide);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Skips the WidescreenBars gameswf draw. The picture under "
                              "the bars is really there - nothing is cropped or stretched.");
        bool fxFrame = bvr::hud::effects_in_frame();
        if (ImGui::Checkbox("Full-screen effects across the view", &fxFrame))
            bvr::hud::set_effects_in_frame(fxFrame);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Water and damage flashes are gameswf fills with no texture, so "
                              "they used to land on the HUD panel.\n"
                              "UNTICK to put them back on the panel. Does NOT affect the "
                              "alcohol blur, which is a textured engine post effect on the "
                              "checkbox below.\n"
                              "Suspected session-30 side effect: the health and EVE bar COLOUR "
                              "fills carry the same fingerprint, so ticked may be sending them "
                              "into the world and leaving the bars looking empty.");
        // Session 30: the post-FX discriminator, in the menu because the alcohol
        // blur is the one draw this rule exists to protect and the A/B has to be
        // done in the headset while drunk.
        bool fxRt = bvr::hud::postfx_rt_only();
        if (ImGui::Checkbox("Post effects: source must be a render target", &fxRt))
            bvr::hud::set_postfx_rt_only(fxRt);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("TICKED (session 30 default): the alcohol blur stays in the "
                              "frame because it samples something the engine RENDERED.\n"
                              "UNTICKED: the old size-only rule, which at a square render "
                              "target also matched the game's own UI atlases and sent about 30 "
                              "HUD draws per interval into the eye image.\n"
                              "If the alcohol blur looks wrong, UNTICK to revert session 30.");
        {
            bvr::hud::RouteStats rs{};
            bvr::hud::get_route_stats(&rs);
            unsigned strandedTotal = 0;
            for (int i = 0; i < bvr::hud::kRoutePassCount; ++i) strandedTotal += rs.stranded[i];
            ImGui::Text("  routing: postFx %u (rejected %u) | effects in-frame %u (over bound "
                        "%u) | stranded %u",
                        rs.postFx, rs.postFxRejected, rs.effectsInFrame, rs.effectsRejected,
                        strandedTotal);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("postFx should CLIMB while you are drunk and sit still "
                                  "otherwise. effects in-frame climbs by 2 every frame, which "
                                  "is the count that made the bar-fill theory.");
        }
        bool subsFrame = bvr::hud::cine_subs_in_frame();
        if (ImGui::Checkbox("Cutscene subtitles in-frame (off = readable panel)", &subsFrame))
            bvr::hud::set_cine_subs_in_frame(subsFrame);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("OFF (default): subtitles ride the head-locked HUD panel - one "
                              "image in both eyes.\n"
                              "ON: they render into the frame, where each eye is captured "
                              "from a different game frame and the text can double.");
        {
            static const char* kDriveNames[] = {"off (VR drives run through cutscenes)",
                                                "authored (camera + hands as flat)",
                                                "authored + head look"};
            int dm = g_cineDrive.load(std::memory_order_relaxed);
            if (dm < 0 || dm > 2) dm = 1;
            if (ImGui::Combo("During cutscenes", &dm, kDriveNames, 3))
                set_cine_drive(static_cast<CineDrive>(dm));
        }
        if (aer) {
            ImGui::SameLine();
            bool swap = g_aerSwapEyes.load(std::memory_order_relaxed);
            if (ImGui::Checkbox("Swap eyes (inverted-depth test)", &swap))
                g_aerSwapEyes.store(swap, std::memory_order_relaxed);
        }
        bool manualFov = g_claimFovManual.load(std::memory_order_relaxed);
        if (ImGui::Checkbox("Manual claimed FOV (distortion calibration)", &manualFov)) {
            g_claimFovManual.store(manualFov, std::memory_order_relaxed);
            if (manualFov) {
                // Snap the slider to the current effective claim as a start point.
                float cur = g_renderedHfov.load(std::memory_order_relaxed);
                if (cur <= 0.0f) cur = g_hfovDeg.load(std::memory_order_relaxed);
                if (cur > 0.0f) g_claimFovDeg.store(cur, std::memory_order_relaxed);
            }
        }
        if (manualFov) {
            float v = g_claimFovDeg.load(std::memory_order_relaxed);
            if (ImGui::SliderFloat("Claimed hfov (deg) - stop the swim", &v, 40.0f, 160.0f))
                g_claimFovDeg.store(v, std::memory_order_relaxed);
        }
    }
    const char* layerName = g_lastLayer == 2 ? "projection" : g_lastLayer == 1 ? "quad" : "none";
    int eyeSign = g_aerEyeSign.load(std::memory_order_relaxed);
    float readback = g_renderedHfov.load(std::memory_order_relaxed);
    float claimed = g_claimFovManual.load(std::memory_order_relaxed)
                        ? g_claimFovDeg.load(std::memory_order_relaxed)
                        : (readback > 0.0f ? readback
                                           : g_hfovDeg.load(std::memory_order_relaxed));
    ImGui::Text("layer: %s%s | target %.1f | readback %.1f | claimed %.1f",
                layerName, eyeSign == 0 ? "" : eyeSign < 0 ? " (AER eye L)" : " (AER eye R)",
                g_hfovDeg.load(std::memory_order_relaxed), readback, claimed);
    ImGui::Text("laser: %s | %u dot layer(s) submitted",
                g_laserSwapchain != XR_NULL_HANDLE ? "ready" : "unavailable",
                g_laserLayersSubmitted.load(std::memory_order_relaxed));
    uint32_t srPushed = g_srPushed.load(std::memory_order_relaxed);
    if (srPushed)
        ImGui::Text("SR tags: pushed %u popped %u dropped %u cleared %u  eyes %d/%d"
                    "  pairs %u aborts %u",
                    srPushed, g_srPopped.load(std::memory_order_relaxed),
                    g_srDropped.load(std::memory_order_relaxed),
                    g_srCleared.load(std::memory_order_relaxed),
                    g_eyeValid[0] ? 1 : 0, g_eyeValid[1] ? 1 : 0,
                    g_srPairs.load(std::memory_order_relaxed),
                    g_srPairAborts.load(std::memory_order_relaxed));
    if (camMode && !g_projectionReady.load(std::memory_order_relaxed))
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f),
                           "projection NOT ready - drive is held off (see log)");
    uint32_t paceSkips = g_paceSkips.load(std::memory_order_relaxed);
    if (paceSkips)
        ImGui::Text("pace guard: skipped %u waits, last wait %u ms", paceSkips,
                    g_lastWaitMs.load(std::memory_order_relaxed));
    uint32_t mirrorBlits = g_mirrorBlits.load(std::memory_order_relaxed);
    if (mirrorBlits)
        ImGui::Text("mirror: %s | %u left holds, %u re-blits",
                    g_mirror.load(std::memory_order_relaxed) ? "left eye" : "OFF",
                    g_mirrorHolds.load(std::memory_order_relaxed), mirrorBlits);
    uint32_t cineEnters = g_cineEnters.load(std::memory_order_relaxed);
    if (cineEnters || g_cineActive.load(std::memory_order_relaxed))
        ImGui::Text("cinematic: %s | enters %u exits %u presents %u",
                    g_cineActive.load(std::memory_order_relaxed) ? "ACTIVE (quad)" : "off",
                    cineEnters, g_cineExits.load(std::memory_order_relaxed),
                    g_cinePresents.load(std::memory_order_relaxed));

    input_draw_debug_ui(); // M5 action-layer status line

    if (!camMode) {
        float dist = g_screenDistM.load(std::memory_order_relaxed);
        if (ImGui::SliderFloat("Screen distance (m)", &dist, 0.5f, 5.0f))
            g_screenDistM.store(dist, std::memory_order_relaxed);
        float width = g_screenWidthM.load(std::memory_order_relaxed);
        if (ImGui::SliderFloat("Screen width (m)", &width, 0.5f, 6.0f))
            g_screenWidthM.store(width, std::memory_order_relaxed);
    }

    // Session 19 HUD quad: capture toggle + head-locked placement.
    bool hudOn = bvr::hud::enabled();
    if (ImGui::Checkbox("VR HUD (gameswf on a floating quad)", &hudOn))
        bvr::hud::set_enabled(hudOn);
    float hd = g_hudDistM.load(std::memory_order_relaxed);
    if (ImGui::SliderFloat("HUD distance (m)", &hd, 0.5f, 3.0f))
        g_hudDistM.store(hd, std::memory_order_relaxed);
    float hw = g_hudWidthM.load(std::memory_order_relaxed);
    if (ImGui::SliderFloat("HUD width (m)", &hw, 0.3f, 3.0f))
        g_hudWidthM.store(hw, std::memory_order_relaxed);
    float hu = g_hudUpM.load(std::memory_order_relaxed);
    if (ImGui::SliderFloat("HUD height offset (m)", &hu, -1.0f, 1.0f))
        g_hudUpM.store(hu, std::memory_order_relaxed);
    ImGui::Text("HUD quad frames %u", g_hudFramesSubmitted.load(std::memory_order_relaxed));
}

bool get_head_pose(HeadPose& out) {
    std::lock_guard<std::mutex> lock(g_poseMutex);
    if (!g_poseValid) return false;
    out = g_headPose;
    // Pose-tag audit stamp: the orientation the game thread actually consumed
    // (compared against the submitted layer pose while the audit is armed).
    g_consumedHeadQuat[0].store(out.qx, std::memory_order_relaxed);
    g_consumedHeadQuat[1].store(out.qy, std::memory_order_relaxed);
    g_consumedHeadQuat[2].store(out.qz, std::memory_order_relaxed);
    g_consumedHeadQuat[3].store(out.qw, std::memory_order_relaxed);
    g_consumedHeadCount.fetch_add(1, std::memory_order_relaxed);
    return true;
}

bool peek_head_pose(HeadPose& out) {
    // Same read as get_head_pose WITHOUT the pose-tag audit stamp. That stamp
    // means "the orientation the game thread actually consumed", and a second
    // reader on the present thread would quietly make the audit lie about both
    // its count and its value. Session 31's swing detector needs the head's
    // position (to subtract the head's own motion from the hand's) and has no
    // business in that instrument.
    std::lock_guard<std::mutex> lock(g_poseMutex);
    if (!g_poseValid) return false;
    out = g_headPose;
    return true;
}

bool get_hand_pose(int hand, bool aimPose, HeadPose& out) {
    float p[3], q[4];
    if (!input_get_hand_pose(hand, aimPose, p, q)) return false;
    out = {p[0], p[1], p[2], q[0], q[1], q[2], q[3]};
    return true;
}

void set_sim_hand_pose(int hand, bool aimPose, bool valid, const float pos3[3],
                       const float quat4[4]) {
    input_set_sim_hand(hand, aimPose, valid, pos3, quat4);
}

void clear_sim_hand_poses() {
    input_clear_sim_hands();
}

bool session_live() {
    return g_session != XR_NULL_HANDLE;
}

int64_t last_predicted_time() {
    return static_cast<int64_t>(g_frameState.predictedDisplayTime);
}

int64_t display_period_ns() {
    return g_displayPeriodNs.load(std::memory_order_relaxed);
}

bool vr_camera_mode() {
    return g_cameraMode.load(std::memory_order_relaxed) &&
           g_sessionBegun.load(std::memory_order_relaxed) &&
           g_projectionReady.load(std::memory_order_relaxed);
}

void set_camera_mode(bool on) {
    bool was = g_cameraMode.exchange(on, std::memory_order_relaxed);
    if (was != on)
        BVR_LOG("xr: camera mode %s (request; drive engages when the session "
                "and projection are ready)",
                on ? "ON" : "off");
}

void set_enabled(bool on) {
    bool was = g_enabled.exchange(on, std::memory_order_relaxed);
    if (was != on) BVR_LOG("xr: VR %s (preset/programmatic)", on ? "ENABLED" : "disabled");
}

// Session 34: AlternateEye armable programmatically. It is the only stereo path
// on BS2 that does NOT re-enter the engine's Draw, and draw re-entrancy is the
// measured freeze - so this needs to be soak-testable from the command surface,
// not just from a checkbox nobody can reach while the game is wedged.
void set_alternate_eye(bool on) {
    g_aerEnabled.store(on, std::memory_order_relaxed);
    BVR_LOG("xr: AlternateEye stereo %s (per-eye images on alternate frames; the "
            "compositor reprojects the stale eye - judders, but never re-enters "
            "the game's draw)",
            on ? "ON" : "off");
}

void set_eye_tag_rendered(bool on) {
    const bool was = g_eyeTagRendered.exchange(on, std::memory_order_relaxed);
    if (was != on)
        BVR_LOG("xr: eye tags = %s (rendered = the parallel pair the game drew: "
                "located midpoint +-ipd/2 along its right axis; located = the "
                "runtime's raw per-eye poses, the historical behavior)",
                on ? "RENDERED-POSE" : "located");
}

bool eye_tag_rendered() { return g_eyeTagRendered.load(std::memory_order_relaxed); }

void set_eye_tag_ipd_mm(float mm) {
    if (mm > 30.0f && mm < 90.0f) g_eyeTagIpdMm.store(mm, std::memory_order_relaxed);
}

void set_hand_ref_quad(bool on, int hand, float sizeDeg) {
    const bool was = g_handQuadOn.exchange(on, std::memory_order_relaxed);
    if (hand == 0 || hand == 1) g_handQuadHand.store(hand, std::memory_order_relaxed);
    if (sizeDeg > 0.1f && sizeDeg < 20.0f)
        g_handQuadSizeDeg.store(sizeDeg, std::memory_order_relaxed);
    if (was != on)
        BVR_LOG("xr: hand ref quad = %s (hand %c, %.1f deg) - parked at the located "
                "grip pose, the FOV-edge one-look discriminator",
                on ? "ON" : "off", g_handQuadHand.load(std::memory_order_relaxed) ? 'R' : 'L',
                g_handQuadSizeDeg.load(std::memory_order_relaxed));
}

bool hand_ref_quad_on() { return g_handQuadOn.load(std::memory_order_relaxed); }

void set_edge_snapshot(bool on) {
    const bool was = g_edgeSnapOn.exchange(on, std::memory_order_relaxed);
    if (was != on) BVR_LOG("xr: edge-telemetry snapshot %s", on ? "ARMED" : "off");
    if (!on) {
        std::lock_guard<std::mutex> lk(g_edgeSnapMutex);
        g_edgeSnap.valid = false;
    }
}

bool get_edge_snapshot(EdgeViewSnapshot& out) {
    std::lock_guard<std::mutex> lk(g_edgeSnapMutex);
    out = g_edgeSnap;
    return out.valid;
}

void arm_view_log(int frames) {
    if (frames < 1) frames = 10;
    if (frames > 60) frames = 60;
    g_viewLogLeft.store(frames, std::memory_order_relaxed);
    BVR_LOG("xr: view log ARMED for %d frames (located per-eye pose + fov, plus "
            "inter-eye deltas)",
            frames);
}

void set_sr_pair_pacing(bool on) {
    g_srPairPacing.store(on, std::memory_order_relaxed);
}

void set_pace_sync(bool on) {
    bool was = g_paceSync.exchange(on, std::memory_order_relaxed);
    if (was != on)
        BVR_LOG("xr: pair-rate sync %s (adapter/preset; `vrpace sync` for the "
                "live A/B and telemetry)",
                on ? "ON" : "off");
}

void set_pose_lag(int lag) {
    if (lag < 0) lag = 0;
    if (lag > 2) lag = 2;
    int was = g_poseLag.exchange(lag, std::memory_order_relaxed);
    if (was != lag)
        BVR_LOG("xr: pose attribution lag %d -> %d generation(s) (0=fresh, "
                "1=the historical one-back, 2=two-back for a threaded "
                "one-frame-lag renderer)",
                was, lag);
}

int get_pose_lag() { return g_poseLag.load(std::memory_order_relaxed); }

float get_pose_gen_delta_deg() {
    return g_poseGenDeltaDeg.load(std::memory_order_relaxed);
}

void set_spike_trace(bool on) {
    bool was = g_spikeTrace.exchange(on, std::memory_order_relaxed);
    if (was != on) {
        // Re-arming resets the STACK budget, not the spike counter: boot 4 of
        // s43 spent all 40 stack captures on menu/load stalls and reached
        // gameplay with a mute sampler. The spike counter itself keeps
        // counting (the TRACE pairs deltas depend on it being monotonic).
        if (on) g_spikeStacks.store(0, std::memory_order_relaxed);
        BVR_LOG("xr: spike trace %s (pair interval > 2x period -> per-phase "
                "snapshot in pacetrace.log; %u captured so far)",
                on ? "ON" : "off", g_spikeCount.load(std::memory_order_relaxed));
    }
}

void handle_pace_command(const char* args) {
    char verb[16] = {};
    int consumed = 0;
    if (sscanf_s(args, "%15s%n", verb, static_cast<unsigned>(sizeof verb), &consumed) != 1)
        verb[0] = '\0';
    const char* rest = args + consumed;
    while (*rest == ' ' || *rest == '\t') ++rest;

    if (strcmp(verb, "on") == 0) {
        g_paceGuard.store(true, std::memory_order_relaxed);
        BVR_LOG("xr: pace guard ON (unfocused session skips the blocking wait)");
    } else if (strcmp(verb, "off") == 0) {
        g_paceGuard.store(false, std::memory_order_relaxed);
        BVR_LOG("xr: pace guard OFF (pre-M8 behavior: every present waits, the "
                "flat window stalls when the headset idles)");
    } else if (strcmp(verb, "thread") == 0) {
        bool on = strncmp(rest, "off", 3) != 0;
        g_paceOffThread.store(on, std::memory_order_relaxed);
        BVR_LOG("xr: xrWaitFrame %s (session 28: off-thread is what lets us keep "
                "submitting while VISIBLE, which is how FOCUSED gets re-granted "
                "after an alt-tab; inline is the pre-session-28 behaviour and "
                "reinstates the skip guard)",
                on ? "OFF the present thread (deadline 200 ms focused / 20 ms idle)"
                   : "INLINE on the present thread");
    } else if (strcmp(verb, "keepalive") == 0) {
        unsigned ms = 0;
        if (sscanf_s(rest, "%u", &ms) == 1 && ms >= 100 && ms <= 10000) {
            g_paceKeepaliveMs.store(ms, std::memory_order_relaxed);
            BVR_LOG("xr: unfocused keepalive every %u ms", ms);
        } else {
            BVR_LOG("xr: usage: vrpace keepalive <100..10000 ms>  (current %u)",
                    g_paceKeepaliveMs.load(std::memory_order_relaxed));
        }
    } else if (strcmp(verb, "detach") == 0) {
        bool on = strncmp(rest, "off", 3) != 0;
        g_paceDetach.store(on, std::memory_order_relaxed);
        BVR_LOG("xr: detached pacing %s - while the session is not FOCUSED the "
                "pace thread %s. This is the session-34 A/B: with it off the game "
                "runs at the runtime's not-visible cadence (~10 Hz measured).",
                on ? "ON" : "off",
                on ? "runs the frame loop at most once per keepalive interval, so "
                     "the blocking xrEndFrame costs one hitch per interval"
                   : "runs the frame loop every present, at the runtime's cadence");
    } else if (strcmp(verb, "feed") == 0) {
        // Session 54: the raffle-wedge root fix A/B (see set_pace_feed).
        bool on = strncmp(rest, "off", 3) != 0;
        set_pace_feed(on);
        BVR_LOG("xr: (feed %s via vrpace; cycles so far %u, %u layered)",
                on ? "on" : "off", g_feedCycles.load(std::memory_order_relaxed),
                g_feedLayered.load(std::memory_order_relaxed));
    } else if (strcmp(verb, "sync") == 0) {
        unsigned hz = 0;
        if (strncmp(rest, "on", 2) == 0) {
            g_paceSyncHz.store(0, std::memory_order_relaxed);
            g_paceSync.store(true, std::memory_order_relaxed);
            BVR_LOG("xr: pair-rate sync ON (target = the runtime's "
                    "predictedDisplayPeriod, currently %.2f ms)",
                    g_displayPeriodNs.load(std::memory_order_relaxed) / 1.0e6);
        } else if (strncmp(rest, "off", 3) == 0) {
            g_paceSync.store(false, std::memory_order_relaxed);
            g_paceSyncNextQpc = 0; // present thread races this benignly: worst
                                   // case one extra resync, counted
            BVR_LOG("xr: pair-rate sync OFF (pairs open at the game's own present "
                    "speed; the runtime's wait may or may not gate them)");
        } else if (sscanf_s(rest, "%u", &hz) == 1 && hz >= 10 && hz <= 500) {
            g_paceSyncHz.store(hz, std::memory_order_relaxed);
            g_paceSync.store(true, std::memory_order_relaxed);
            BVR_LOG("xr: pair-rate sync ON at a COMMANDED %u Hz (overrides the "
                    "runtime period; `vrpace sync on` returns to the period)",
                    hz);
        } else {
            BVR_LOG("xr: pair-rate sync %s (hz override %u) | delayed %u pairs, "
                    "%llu ms total | resyncs %u | usage: vrpace sync on|off|<10..500>",
                    g_paceSync.load(std::memory_order_relaxed) ? "ON" : "off",
                    g_paceSyncHz.load(std::memory_order_relaxed),
                    g_paceSyncDelays.load(std::memory_order_relaxed),
                    static_cast<unsigned long long>(
                        g_paceSyncDelayUs.load(std::memory_order_relaxed) / 1000),
                    g_paceSyncResyncs.load(std::memory_order_relaxed));
        }
    } else if (strcmp(verb, "spike") == 0) {
        // Session 43: the spike-triggered evidence capture (state + rationale
        // at the g_spikeTrace block). Bare `vrpace spike` prints telemetry.
        if (strncmp(rest, "on", 2) == 0) {
            set_spike_trace(true);
        } else if (strncmp(rest, "off", 3) == 0) {
            set_spike_trace(false);
        } else {
            BVR_LOG("xr: spike trace %s | %u spikes captured (threshold 2x period, "
                    "snapshots in pacetrace.log; spikes/s on the TRACE pairs line) | "
                    "usage: vrpace spike on|off",
                    g_spikeTrace.load(std::memory_order_relaxed) ? "ON" : "off",
                    g_spikeCount.load(std::memory_order_relaxed));
        }
    } else if (strcmp(verb, "simidle") == 0) {
        bool on = strncmp(rest, "on", 2) == 0;
        g_simIdle.store(on, std::memory_order_relaxed);
        BVR_LOG("xr: simulated idle %s%s", on ? "ON" : "off",
                on ? " (flat stand-in: state VISIBLE, 1 s block per paced frame; "
                     "with the guard off, commands crawl at ~1/s until it lands)"
                   : "");
    } else {
        BVR_LOG("xr: pace guard %s | wait %s | session %s everFocused=%d | skips %u "
                "lastWait %u ms | handoffs %u timeouts %u | simidle %s "
                "(vrpace on|off|thread on|off|detach on|off|sync|spike|simidle on|off|status)",
                g_paceGuard.load(std::memory_order_relaxed) ? "ON" : "off",
                g_paceOffThread.load(std::memory_order_relaxed) ? "off-thread" : "inline",
                state_str(g_state), g_everFocused.load(std::memory_order_relaxed) ? 1 : 0,
                g_paceSkips.load(std::memory_order_relaxed),
                g_lastWaitMs.load(std::memory_order_relaxed),
                g_paceHandoffs.load(std::memory_order_relaxed),
                g_paceTimeouts.load(std::memory_order_relaxed),
                g_simIdle.load(std::memory_order_relaxed) ? "ON" : "off");
        // Session 42: lifetime pair cadence (the per-second numbers live on the
        // TRACE pairs line in pacetrace.log; this is the cheap always-there view).
        {
            uint32_t cnt = g_pairIntCount.load(std::memory_order_relaxed);
            uint64_t sum = g_pairIntSumUs.load(std::memory_order_relaxed);
            int64_t periodNs = g_displayPeriodNs.load(std::memory_order_relaxed);
            BVR_LOG("xr: pair cadence: pairs %u aborts %u | lifetime mean interval %u us "
                    "over %u samples | runtime period %.2f ms (%.1f Hz) | per-second "
                    "jitter on the TRACE pairs line (pacetrace.log)",
                    g_srPairs.load(std::memory_order_relaxed),
                    g_srPairAborts.load(std::memory_order_relaxed),
                    cnt ? static_cast<uint32_t>(sum / cnt) : 0, cnt,
                    periodNs > 0 ? periodNs / 1.0e6 : 0.0,
                    periodNs > 0 ? 1.0e9 / periodNs : 0.0);
        }
        BVR_LOG("xr: detach %s (keepalive every %u ms) | detachedNow=%d | episodes %u "
                "| unpaced presents %u, keepalive frames %u",
                g_paceDetach.load(std::memory_order_relaxed) ? "ON" : "off",
                g_paceKeepaliveMs.load(std::memory_order_relaxed), g_detachedNow ? 1 : 0,
                g_detachEpisodes.load(std::memory_order_relaxed),
                g_detachSkips.load(std::memory_order_relaxed),
                g_detachKeepalives.load(std::memory_order_relaxed));
        BVR_LOG("xr: feed %s | cycles %u (%u layered) | snapshot %s "
                "(vrpace feed on|off - keepalives that carry layers, s54)",
                g_paceFeed.load(std::memory_order_relaxed) ? "ON" : "off",
                g_feedCycles.load(std::memory_order_relaxed),
                g_feedLayered.load(std::memory_order_relaxed),
                [] {
                    std::lock_guard<std::mutex> lk(g_feedSnapMutex);
                    return g_feedSnap.valid ? (g_feedSnap.isProj ? "projection" : "screen-quad")
                                            : "none";
                }());
        // The phase table is the whole point of the instrument: it says WHICH
        // call owns the frame time, which is the question session 33 answered
        // by inference and got wrong.
        char buf[512];
        int n = sprintf_s(buf, "xr: present phases last/max us (shouldRender=%d):",
                          g_lastShouldRender.load(std::memory_order_relaxed) ? 1 : 0);
        for (int i = 0; i < kPhCount && n > 0 && n < static_cast<int>(sizeof(buf)) - 48; ++i)
            n += sprintf_s(buf + n, sizeof(buf) - n, " %s=%u/%u", kPhaseNames[i],
                           g_phaseLastUs[i].load(std::memory_order_relaxed),
                           g_phaseMaxUs[i].load(std::memory_order_relaxed));
        BVR_LOG("%s", buf);
    }
}

void handle_mirror_command(const char* args) {
    if (strncmp(args, "on", 2) == 0) {
        g_mirror.store(true, std::memory_order_relaxed);
        BVR_LOG("xr: desktop mirror ON (window pinned to the LEFT eye under stereo)");
    } else if (strncmp(args, "off", 3) == 0) {
        g_mirror.store(false, std::memory_order_relaxed);
        BVR_LOG("xr: desktop mirror OFF (pre-M8 behavior: the window alternates "
                "eyes under stereo)");
    } else {
        BVR_LOG("xr: mirror %s | holds %u blits %u (vrmirror on|off|status)",
                g_mirror.load(std::memory_order_relaxed) ? "ON" : "off",
                g_mirrorHolds.load(std::memory_order_relaxed),
                g_mirrorBlits.load(std::memory_order_relaxed));
    }
}

float suggested_hfov_deg() {
    return g_hfovDeg.load(std::memory_order_relaxed);
}

bool headset_half_fov_deg(float* halfH, float* halfV) {
    float h = g_headsetHalfH.load(std::memory_order_relaxed);
    float v = g_headsetHalfV.load(std::memory_order_relaxed);
    if (halfH) *halfH = h;
    if (halfV) *halfV = v;
    return h > 0.0f && v > 0.0f;
}

bool recommended_eye_size(uint32_t* w, uint32_t* h) {
    const uint32_t rw = g_recommendedEyeW.load(std::memory_order_relaxed);
    const uint32_t rh = g_recommendedEyeH.load(std::memory_order_relaxed);
    if (w) *w = rw;
    if (h) *h = rh;
    return rw > 0 && rh > 0;
}

void set_rendered_hfov(float hfovDeg) {
    g_renderedHfov.store(hfovDeg, std::memory_order_relaxed);
}

void set_present_stage(const char* name) {
    g_presentStageT0.store(phase_now(), std::memory_order_relaxed);
    g_presentStage.store(name, std::memory_order_relaxed);
}

void set_draw_stage(const char* name) {
    g_drawStageT0.store(phase_now(), std::memory_order_relaxed);
    if (name) g_lastDrawTidSticky.store(GetCurrentThreadId(), std::memory_order_relaxed);
    g_drawStageTid.store(name ? GetCurrentThreadId() : 0u, std::memory_order_relaxed);
    g_drawStage.store(name, std::memory_order_relaxed);
}

uint32_t watchdog_fires() {
    return g_watchdogFires.load(std::memory_order_relaxed);
}

void set_pace_detach(bool on) {
    if (g_paceDetach.exchange(on, std::memory_order_relaxed) == on) return;
    BVR_LOG("xr: detached pacing %s by the game adapter (an unfocused session "
            "will %space the game thread)",
            on ? "ENABLED" : "disabled", on ? "no longer " : "");
}

void set_pace_feed(bool on) {
    if (g_paceFeed.exchange(on, std::memory_order_relaxed) == on) return;
    BVR_LOG("xr: pace feed %s by the game adapter - while the session is not "
            "FOCUSED the present thread detaches and the pace thread re-submits "
            "the last healthy layer set (keepalives that carry layers, s54)",
            on ? "ENABLED" : "disabled");
}

void fov_audit(float* tanH, float* tanV, int* src, unsigned* swapW, unsigned* swapH) {
    if (tanH) *tanH = g_auditTanH.load(std::memory_order_relaxed);
    if (tanV) *tanV = g_auditTanV.load(std::memory_order_relaxed);
    if (src) *src = g_auditFovSrc.load(std::memory_order_relaxed);
    if (swapW) *swapW = g_swapW;
    if (swapH) *swapH = g_swapH;
}

void set_pose_audit(bool on) {
    bool was = g_poseAudit.exchange(on, std::memory_order_relaxed);
    if (was != on) BVR_LOG("xr: pose audit %s (tagged-vs-consumed yaw, stereo only)",
                           on ? "ON" : "off");
}

void publish_gameplay_view(bool strictGameplay) {
    g_gameplayView.store((GetTickCount64() << 1) | (strictGameplay ? 1u : 0u),
                         std::memory_order_relaxed);
}

CineDrive cine_drive() {
    return static_cast<CineDrive>(g_cineDrive.load(std::memory_order_relaxed));
}

const char* cine_drive_name(CineDrive mode) {
    switch (mode) {
        case CineDrive::Off: return "off (VR drives run through cutscenes)";
        case CineDrive::AuthoredLook: return "authored+look (authored camera + head look)";
        default: return "authored (authored camera and hands, VR drives suspended)";
    }
}

void set_cine_drive(CineDrive mode) {
    int v = static_cast<int>(mode);
    if (g_cineDrive.exchange(v, std::memory_order_relaxed) != v)
        BVR_LOG("xr: cinematic drive = %s", cine_drive_name(mode));
}

void handle_cine_command(const char* args) {
    if (strncmp(args, "drive off", 9) == 0) {
        set_cine_drive(CineDrive::Off);
    } else if (strncmp(args, "drive authored+look", 19) == 0) {
        set_cine_drive(CineDrive::AuthoredLook);
    } else if (strncmp(args, "drive authored", 14) == 0) {
        set_cine_drive(CineDrive::Authored);
    } else if (strncmp(args, "bars hide", 9) == 0) {
        bvr::hud::set_bars_hidden(true);
    } else if (strncmp(args, "bars show", 9) == 0) {
        bvr::hud::set_bars_hidden(false);
    } else if (strncmp(args, "effects verts", 13) == 0) {
        unsigned n = 0;
        if (sscanf_s(args + 13, "%u", &n) == 1 && n >= 3)
            bvr::hud::set_effect_max_verts(n);
        else
            BVR_LOG("xr: usage: vrcine effects verts <n>  (current %u)",
                    bvr::hud::effect_max_verts());
    } else if (strncmp(args, "effects frame", 13) == 0) {
        bvr::hud::set_effects_in_frame(true);
    } else if (strncmp(args, "effects panel", 13) == 0) {
        bvr::hud::set_effects_in_frame(false);
    } else if (strncmp(args, "postfx cine on", 14) == 0) {
        bvr::hud::set_postfx_cine_size(true);
    } else if (strncmp(args, "postfx cine off", 15) == 0) {
        bvr::hud::set_postfx_cine_size(false);
    } else if (strncmp(args, "postfx rt", 9) == 0) {
        bvr::hud::set_postfx_rt_only(true);
    } else if (strncmp(args, "postfx size", 11) == 0) {
        bvr::hud::set_postfx_rt_only(false);
    } else if (strncmp(args, "restorert on", 12) == 0) {
        bvr::hud::set_restore_rt(true);
    } else if (strncmp(args, "restorert off", 13) == 0) {
        bvr::hud::set_restore_rt(false);
    } else if (strncmp(args, "subs panel", 10) == 0) {
        bvr::hud::set_cine_subs_in_frame(false);
    } else if (strncmp(args, "subs frame", 10) == 0) {
        bvr::hud::set_cine_subs_in_frame(true);
    } else if (strncmp(args, "bars verts", 10) == 0) {
        unsigned n = 0;
        if (sscanf_s(args + 10, "%u", &n) == 1 && n >= 3)
            bvr::hud::set_bar_verts(n);
        else
            BVR_LOG("xr: usage: vrcine bars verts <n>  (current %u)", bvr::hud::bar_verts());
    } else if (strncmp(args, "dumparm off", 11) == 0) {
        bvr::hud::set_dump_on_edge(0, 2);
        BVR_LOG("xr: edge dump DISARMED");
    } else if (strncmp(args, "dumparm bars", 12) == 0) {
        // Session 42: transitions (loading, FMV entry) are over before the 1 Hz
        // command poll could arm a dump by hand - stage it on the rising edge.
        int n = 2;
        sscanf_s(args + 12, "%d", &n);
        bvr::hud::set_dump_on_edge(1, n);
        BVR_LOG("xr: edge dump armed on the next BAR-DRAW rising edge (%d windows)", n);
    } else if (strncmp(args, "dumparm screen", 14) == 0) {
        int n = 2;
        sscanf_s(args + 14, "%d", &n);
        bvr::hud::set_dump_on_edge(2, n);
        BVR_LOG("xr: edge dump armed on the next SCREEN-ONLY rising edge (%d windows)", n);
    } else if (strncmp(args, "dumparm letterbox", 17) == 0) {
        int n = 2;
        sscanf_s(args + 17, "%d", &n);
        bvr::hud::set_dump_on_edge(3, n);
        BVR_LOG("xr: edge dump armed on the next LETTERBOX pixel-watch rising edge "
                "(%d windows)", n);
    } else if (strncmp(args, "unsqueeze", 9) == 0) {
        // Session 29: RETIRED, not merely defaulted off. The unsqueeze assumed
        // the cinematic content was anamorphically squeezed into a middle band
        // (session 22's reading). It is not: the bars are a gameswf draw
        // painted OVER a full-frame tonemap (SWF byte-diff + framedump, see
        // ENGINE_NOTES session 29), so stretching a band would crop real
        // picture and distort the aspect. `vrcine bars hide|show` replaces it.
        BVR_LOG("xr: 'vrcine unsqueeze' is RETIRED - the bars are a gameswf draw over a "
                "full-frame image, never a squeeze; use 'vrcine bars hide|show'");
    } else if (strncmp(args, "mode stereo", 11) == 0) {
        g_cineStereo.store(true, std::memory_order_relaxed);
        BVR_LOG("xr: cinematic mode STEREO (fov-mismatch scenes keep the projection, "
                "claim = measured fov; strict-false/stale still drop to the quad)");
    } else if (strncmp(args, "mode quad", 9) == 0) {
        g_cineStereo.store(false, std::memory_order_relaxed);
        BVR_LOG("xr: cinematic mode QUAD (fov-mismatch scenes drop to the big screen)");
    } else if (strncmp(args, "on", 2) == 0) {
        g_cineEnabled.store(true, std::memory_order_relaxed);
        BVR_LOG("xr: cinematic fallback ON (non-gameplay views drop to the quad screen)");
    } else if (strncmp(args, "off", 3) == 0) {
        g_cineEnabled.store(false, std::memory_order_relaxed);
        BVR_LOG("xr: cinematic fallback OFF (pre-session-22: cutscenes submit as a "
                "mis-claimed projection layer)");
    } else {
        uint64_t pv = g_gameplayView.load(std::memory_order_relaxed);
        uint64_t ageMs = pv ? GetTickCount64() - (pv >> 1) : 0;
        float t = 0.0f, tv = 0.0f;
        unsigned long long fovAge = 0;
        bool haveFov = bvr::hud::fov_watch(&t, &tv, &fovAge, 0); // print stale too
        unsigned barsSkipped = 0, barIntervals = 0, barVerts = 0;
        bvr::hud::get_bar_stats(&barsSkipped, &barIntervals, &barVerts);
        unsigned lbTop = 0, lbBot = 0;
        bool lbPix = bvr::hud::letterbox(&lbTop, &lbBot);
        bool barDraw = bvr::hud::bar_draw_active();
        // The two cinematic sources, side by side and never merged. With bars
        // hidden the pixel watch SHOULD read 0 while the draw signal holds -
        // that is the design working, not the sources disagreeing.
        BVR_LOG("xr: cine bars=%s (looking for %u verts) | barDraw=%d (skipped %u, "
                "intervals %u, last %u verts) | pixelWatch=%d (top %u bot %u) | sources %s",
                bvr::hud::bars_hidden() ? "HIDDEN" : "shown", bvr::hud::bar_verts(),
                barDraw ? 1 : 0, barsSkipped, barIntervals, barVerts, lbPix ? 1 : 0, lbTop,
                lbBot,
                barDraw == lbPix ? "AGREE"
                : bvr::hud::bars_hidden()
                    ? "differ (expected: bars hidden, so nothing black to see)"
                    : "DIFFER - unexpected with bars shown");
        // Session 30 routing. `stranded` is the number that decides whether
        // "in-frame" ever meant it: a pass issued while our capture RT was
        // still bound landed on the HUD panel regardless of the verdict.
        bvr::hud::RouteStats rs{};
        bvr::hud::get_route_stats(&rs);
        unsigned strandedTotal = 0;
        for (int i = 0; i < bvr::hud::kRoutePassCount; ++i) strandedTotal += rs.stranded[i];
        BVR_LOG("xr: cine effects=%s bound=%u verts (inFrame %u, over-bound %u) | "
                "stranded effect=%u total=%u restored=%u (restorert %s) | postFx=%u "
                "(rejected by the bind test %u, square target=%d, rule=%s)",
                bvr::hud::effects_in_frame() ? "IN-FRAME" : "panel",
                bvr::hud::effect_max_verts(), rs.effectsInFrame, rs.effectsRejected,
                rs.stranded[bvr::hud::kRouteEffect], strandedTotal, rs.restored,
                bvr::hud::restore_rt() ? "ON" : "off", rs.postFx,
                rs.postFxRejected, rs.squareTarget ? 1 : 0,
                bvr::hud::postfx_rt_only()
                    ? (bvr::hud::postfx_cine_size() ? "render-target (size-only in cutscenes)"
                                                    : "render-target")
                    : "size-only");
        int dumpEdge = 0, dumpCount = 0;
        bvr::hud::get_dump_on_edge(&dumpEdge, &dumpCount);
        BVR_LOG("xr: cine %s mode=%s active=%d | enters %u exits %u presents %u | "
                "published strict=%d age=%llums | WORLD tanH=%.4f age=%llums "
                "mismatch=%d screenOnly=%d dumparm=%s (vrcine on|off|mode quad|mode "
                "stereo|bars hide|show|effects frame|panel|effects verts <n>|postfx "
                "rt|size|subs panel|frame|dumparm bars|screen <n>|off|status)",
                g_cineEnabled.load(std::memory_order_relaxed) ? "ON" : "off",
                g_cineStereo.load(std::memory_order_relaxed) ? "stereo" : "quad",
                g_cineActive.load(std::memory_order_relaxed) ? 1 : 0,
                g_cineEnters.load(std::memory_order_relaxed),
                g_cineExits.load(std::memory_order_relaxed),
                g_cinePresents.load(std::memory_order_relaxed),
                pv ? static_cast<int>(pv & 1) : -1,
                static_cast<unsigned long long>(ageMs),
                haveFov ? t : 0.0f, haveFov ? fovAge : 0,
                bvr::hud::fov_mismatch() ? 1 : 0, bvr::hud::screen_only() ? 1 : 0,
                dumpEdge == 0 ? "off" : (dumpEdge == 1 ? "bars" : "screen"));
    }
}

bool cinematic_active() {
    return g_cineActive.load(std::memory_order_relaxed);
}

float rendered_hfov_deg() {
    return g_renderedHfov.load(std::memory_order_relaxed);
}

int current_eye_sign() {
    return g_aerEyeSign.load(std::memory_order_relaxed);
}

void set_laser(const LaserConfig& cfg) {
    g_laserOn.store(cfg.enabled, std::memory_order_relaxed);
    g_laserHand.store(cfg.hand ? 1 : 0, std::memory_order_relaxed);
    g_laserPitchTrim.store(cfg.pitchTrimDeg, std::memory_order_relaxed);
    g_laserYawTrim.store(cfg.yawTrimDeg, std::memory_order_relaxed);
    g_laserPosFwdCm.store(cfg.posFwdCm, std::memory_order_relaxed);
    g_laserPosRightCm.store(cfg.posRightCm, std::memory_order_relaxed);
    g_laserPosUpCm.store(cfg.posUpCm, std::memory_order_relaxed);
    g_laserDots.store(cfg.dots, std::memory_order_relaxed);
    g_laserNearM.store(cfg.nearM, std::memory_order_relaxed);
    g_laserFarM.store(cfg.farM, std::memory_order_relaxed);
    g_laserSizeDeg.store(cfg.sizeDeg, std::memory_order_relaxed);
    g_laserMuzzle.store(cfg.muzzle, std::memory_order_relaxed);
    for (int i = 0; i < 3; ++i)
        g_laserMuzzleD0[i].store(cfg.muzzleD0[i], std::memory_order_relaxed);
    g_laserModelPitchTrim.store(cfg.modelPitchTrimDeg, std::memory_order_relaxed);
    g_laserModelYawTrim.store(cfg.modelYawTrimDeg, std::memory_order_relaxed);
    g_laserModelRollTrim.store(cfg.modelRollTrimDeg, std::memory_order_relaxed);
}

void set_aim_dot(const AimDotConfig& cfg) {
    g_dotOn.store(cfg.enabled, std::memory_order_relaxed);
    g_dotSizeDeg.store(cfg.sizeDeg, std::memory_order_relaxed);
    g_dotValid.store(cfg.valid, std::memory_order_relaxed);
    if (!cfg.valid) return; // keep the last point; the stamp is what expires it
    g_dotX.store(cfg.posXr[0], std::memory_order_relaxed);
    g_dotY.store(cfg.posXr[1], std::memory_order_relaxed);
    g_dotZ.store(cfg.posXr[2], std::memory_order_relaxed);
    g_dotStampMs.store(GetTickCount64(), std::memory_order_relaxed);
}

void set_laser_slot(int slot, const LaserConfig& cfg) {
    if (slot != 1) {
        set_laser(cfg);
        return;
    }
    g_laser2On.store(cfg.enabled, std::memory_order_relaxed);
    g_laser2Hand.store(cfg.hand ? 1 : 0, std::memory_order_relaxed);
    g_laser2PitchTrim.store(cfg.pitchTrimDeg, std::memory_order_relaxed);
    g_laser2YawTrim.store(cfg.yawTrimDeg, std::memory_order_relaxed);
    g_laser2PosFwdCm.store(cfg.posFwdCm, std::memory_order_relaxed);
    g_laser2PosRightCm.store(cfg.posRightCm, std::memory_order_relaxed);
    g_laser2PosUpCm.store(cfg.posUpCm, std::memory_order_relaxed);
    g_laser2Dots.store(cfg.dots, std::memory_order_relaxed);
    g_laser2NearM.store(cfg.nearM, std::memory_order_relaxed);
    g_laser2FarM.store(cfg.farM, std::memory_order_relaxed);
    g_laser2SizeDeg.store(cfg.sizeDeg, std::memory_order_relaxed);
    // The muzzle-ray lane stays slot-0 only: it exists for BS1's rendered
    // barrel and has no second-hand meaning yet.
}

void set_aim_dot_slot(int slot, const AimDotConfig& cfg) {
    if (slot != 1) {
        set_aim_dot(cfg);
        return;
    }
    g_dot2On.store(cfg.enabled, std::memory_order_relaxed);
    g_dot2SizeDeg.store(cfg.sizeDeg, std::memory_order_relaxed);
    g_dot2Valid.store(cfg.valid, std::memory_order_relaxed);
    if (!cfg.valid) return;
    g_dot2X.store(cfg.posXr[0], std::memory_order_relaxed);
    g_dot2Y.store(cfg.posXr[1], std::memory_order_relaxed);
    g_dot2Z.store(cfg.posXr[2], std::memory_order_relaxed);
    g_dot2StampMs.store(GetTickCount64(), std::memory_order_relaxed);
}

const char* session_state_name() {
    if (g_session == XR_NULL_HANDLE) return "none";
    return state_str(g_state);
}

bool ever_focused() { return g_everFocused.load(std::memory_order_relaxed); }

void set_hud_quad(float distM, float widthM, float upM) {
    g_hudDistM.store(distM, std::memory_order_relaxed);
    g_hudWidthM.store(widthM, std::memory_order_relaxed);
    g_hudUpM.store(upM, std::memory_order_relaxed);
}

void get_hud_quad(float* distM, float* widthM, float* upM) {
    if (distM) *distM = g_hudDistM.load(std::memory_order_relaxed);
    if (widthM) *widthM = g_hudWidthM.load(std::memory_order_relaxed);
    if (upM) *upM = g_hudUpM.load(std::memory_order_relaxed);
}

void set_hud_texture_provider(HudTextureProviderFn fn) {
    g_hudTexProvider.store(fn, std::memory_order_relaxed);
}

void sr_push_eye(int eyeSign) {
    uint32_t head = g_srHead.load(std::memory_order_relaxed);
    uint32_t tail = g_srTail.load(std::memory_order_acquire);
    if (head - tail >= kSrRingSize) {
        // No consumer (no XR session) or consumer stalled: drop, count it.
        g_srDropped.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    g_srRing[head & (kSrRingSize - 1)].store(static_cast<int8_t>(eyeSign),
                                             std::memory_order_relaxed);
    g_srHead.store(head + 1, std::memory_order_release);
    g_srPushed.fetch_add(1, std::memory_order_relaxed);
}

} // namespace bvr::vr

#else // !BVR_WITH_OPENXR

namespace bvr::vr {

void init_instance() {
    BVR_LOG("xr: built without OpenXR support - VR disabled");
}
void on_present_begin(IDXGISwapChain*) {}
void on_present_end(IDXGISwapChain*) {}
void on_resize(unsigned, unsigned, unsigned) {}
void draw_debug_ui() {}
bool get_head_pose(HeadPose&) { return false; }
bool peek_head_pose(HeadPose&) { return false; }
bool get_hand_pose(int, bool, HeadPose&) { return false; }
void set_sim_hand_pose(int, bool, bool, const float[3], const float[4]) {}
void clear_sim_hand_poses() {}
bool session_live() { return false; }
int64_t last_predicted_time() { return 0; }
int64_t display_period_ns() { return 0; }
bool vr_camera_mode() { return false; }
void set_camera_mode(bool) {}
void set_alternate_eye(bool) {}
void set_eye_tag_rendered(bool) {}
bool eye_tag_rendered() { return false; }
void set_eye_tag_ipd_mm(float) {}
void set_hand_ref_quad(bool, int, float) {}
bool hand_ref_quad_on() { return false; }
void arm_view_log(int) {}
void set_edge_snapshot(bool) {}
bool get_edge_snapshot(EdgeViewSnapshot&) { return false; }
void set_enabled(bool) {}
void set_sr_pair_pacing(bool) {}
void handle_pace_command(const char*) {}
void set_pace_detach(bool) {}
void set_pace_sync(bool) {}
void set_spike_trace(bool) {}
void set_pose_lag(int) {}
int get_pose_lag() { return 1; }
float get_pose_gen_delta_deg() { return 0.0f; }
void set_present_stage(const char*) {}
void set_draw_stage(const char*) {}
uint32_t watchdog_fires() { return 0; }
void handle_mirror_command(const char*) {}
float suggested_hfov_deg() { return 0.0f; }
bool headset_half_fov_deg(float* halfH, float* halfV) {
    if (halfH) *halfH = 0.0f;
    if (halfV) *halfV = 0.0f;
    return false;
}
bool recommended_eye_size(uint32_t* w, uint32_t* h) {
    if (w) *w = 0;
    if (h) *h = 0;
    return false;
}
void set_rendered_hfov(float) {}
void fov_audit(float* tanH, float* tanV, int* src, unsigned* swapW, unsigned* swapH) {
    if (tanH) *tanH = 0.0f;
    if (tanV) *tanV = 0.0f;
    if (src) *src = -1;
    if (swapW) *swapW = 0;
    if (swapH) *swapH = 0;
}
void set_pose_audit(bool) {}
void publish_gameplay_view(bool) {}
void handle_cine_command(const char*) {}
bool cinematic_active() { return false; }
CineDrive cine_drive() { return CineDrive::Authored; }
void set_cine_drive(CineDrive) {}
const char* cine_drive_name(CineDrive) { return "authored"; }
float rendered_hfov_deg() { return 0.0f; }
int current_eye_sign() { return 0; }
void sr_push_eye(int) {}
void set_laser(const LaserConfig&) {}
void set_aim_dot(const AimDotConfig&) {}
void set_hud_quad(float, float, float) {}
void get_hud_quad(float* d, float* w, float* u) {
    if (d) *d = 0;
    if (w) *w = 0;
    if (u) *u = 0;
}
void set_hud_texture_provider(HudTextureProviderFn) {}

} // namespace bvr::vr

#endif
