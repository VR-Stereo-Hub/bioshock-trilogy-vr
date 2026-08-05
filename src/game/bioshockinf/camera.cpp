#include "game/bioshockinf/camera.h"

#include "core/framework/command.h"
#include "core/gfx/hud_capture.h"
#include "core/hooks/d3d11_hook.h"
#include "core/util/log.h"
#include "game/bioshockinf/inf_math.h"
#include "game/bioshockinf/patterns.h"
#include "game/bioshockinf/recorder.h"

#include <MinHook.h>
#include <imgui.h>
#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <share.h>
#include <string>

namespace bvr::bsi::camera {
namespace {

// The UE3 PODs (FVector/FRotator), rotator constants and the XR<->UE mapping
// live in inf_math.h - adapter-local on purpose, NOT game/shared/ue_math.h:
// that header states it is the Vengeance/UE2.5 family's convention, and this
// game's patterns.h states that on UE3 even shapes are suspect. Same layout by
// coincidence is not the same layout by contract.

// APlayerController::GetPlayerViewPoint is __thiscall. __fastcall with a dummy
// EDX slot is register-, stack- and cleanup-identical, and works as a plain
// free function.
//
// EXACTLY TWO STACK ARGS. The target is `ret 8`, and `ret imm / 4 == 2` is a
// hard requirement: a mismatch returns on a misaligned stack and pops a
// "Run-Time Check Failure #0 - ESP was not properly saved" dialog which writes
// NO crash dump (RTC is a Debug compiler check, not an SEH fault). ONE typedef
// serves both the trampoline pointer and the detour so the two cannot disagree.
using GetViewPointFn = void(__fastcall*)(void* self, void* edx, FVector* loc, FRotator* rot);

GetViewPointFn g_original = nullptr;
void* g_target = nullptr;
std::atomic<bool> g_hookLive{false};
std::atomic<bool> g_enabled{true};
std::atomic<bool> g_fired{false};
std::atomic<bool> g_loggedFirstFire{false};
std::atomic<uint32_t> g_callCount{0};
std::atomic<uint64_t> g_lastCallMs{0};

// Heartbeat state. Game thread only - it is only ever touched inside the
// throttled block, which the tid latch confines to one thread.
std::atomic<bool> g_heartbeat{true};
int g_beatsLeft = 10; // self-expiring burst; `bsicam heartbeat on` re-arms it
uint64_t g_lastBeatMs = 0;
uint32_t g_beatBaseCount = 0;

// ---------------------------------------------------------------------------
// I4 drive state (session 39). The drive writes ONLY the detour's out-params -
// see camera.h. Atomics are the overlay/command edges; everything else is
// game thread only (the tid latch confines the drive to one thread).
// ---------------------------------------------------------------------------
std::atomic<bool> g_driveEnabled{false}; // every new render lever ships OFF
// Unreal units per meter. 50 is UE3's CANONICAL scale (1 uu = 2 cm), NOT
// BS1/BS2's calibrated 100 (Vengeance is a different engine; never copy a
// number between games). The headset session calibrates the real value via
// the F10 slider; the flat battery only proves the code applies whatever is
// set here.
std::atomic<float> g_worldScale{50.0f};
std::atomic<bool> g_recenterRequested{true}; // auto-recenter on first drive
std::atomic<bool> g_vrDriving{false};        // telemetry for the UI
// Head-offset telemetry: the recenter-relative offset applied to loc this
// frame, in UU - makes the world-scale value's effect a number in the log,
// which is what the flat 6DoF check measures.
std::atomic<float> g_headOffX{0.0f}, g_headOffY{0.0f}, g_headOffZ{0.0f};

// Game thread only.
bool g_haveRecenter = false;
bvr::vr::HeadPose g_recenterPose{};
// The seated frame's yaw zero, in ROTATOR UNITS (65536/turn), integer for
// exactness - wrap_rot subtraction on integers is exact, floats drift.
int32_t g_recenterYawUnits = 0;
float recenter_yaw_rad() { return g_recenterYawUnits / kRotUnitsPerRadian; }

// The head-vs-engine pitch error, computed against the engine's own pitch
// BEFORE the drive overwrites the out-param. It exists because the drive
// heartbeat reports the FINAL rot by design - so it shows the head's pitch,
// never the engine's, and could not by itself show the engine's pitch stuck
// (the BS1 -88.9 lesson; the engine base itself is on the heartbeat as
// engineRot, from the pre-drive snapshot). NOTE: unlike BS2, the error is
// LOGGED ONLY - it is never published to the input bridge in I4
// (publish_vr_gameplay would arm the shared pitch kill and seize right-stick
// Y; that lane is I7's).
float g_pitchErrDeg = 0.0f;

// What the last drive actually did, for the heartbeat. Game thread only.
const char* g_driveLane = "off";
FVector g_finalLoc{};
FRotator g_finalRot{};
bool g_finalValid = false;

// ---------------------------------------------------------------------------
// I5 stereo state (session 40). Rung 1 is the projection flip: the adapter
// finally feeds core an HONEST fov claim and asks for camera mode, and core
// swaps the quad for a projection layer (openxr_runtime projectionMode).
// ---------------------------------------------------------------------------
// The claimed VERTICAL half-tangent. The I2/I37 law: the game's FOV option is
// vertical-referenced and tanH = tanV x aspect at any aspect (measured at two
// aspects, ENGINE_NOTES "The FOV law"). Default = the slider at MINIMUM (the
// shipped default position, tanV pinned 0.4317). There is no live option
// reader yet (that lever is I6's); if the user moves the in-game slider the
// claim goes stale - correct it with `bsifov tanv <v>` and verify against a
// `dumpframe cb` decode. A WRONG CLAIM POISONS EVERY STEREO JUDGMENT (BS1's
// M4), which is why this is loud in the log and visible in the overlay.
std::atomic<float> g_claimTanV{patterns::kTanVSliderMin};
// The vrstereo latch (what the toggle last applied), telemetry for UI/status.
std::atomic<bool> g_stereoArmed{false};
// F10 -> game thread: -1 none, 0 pending off, 1 pending on. The overlay draws
// on the render thread and must never touch engine state or (later, rung 3)
// install hooks; the detour consumes this on the next call - BS2's
// request_vrstereo shape.
std::atomic<int> g_vrstereoPending{-1};
// What publish_projection_claim last computed, for the overlay/heartbeat.
std::atomic<float> g_lastClaimHfovDeg{0.0f};
std::atomic<float> g_lastClaimAspect{0.0f};

// Synthetic HMD lane (BS2's shape, WITH the position triple): `simhead <yaw>
// <pitch> <roll> [px py pz] [holdMs]` feeds a scripted head pose through the
// real drive, so the full 6DoF xr-to-ue mapping is provable flat from the
// log. Self-expiring; ungated by session state so it works with no XR
// session at all.
struct SimHead {
    float yawDeg = 0.0f, pitchDeg = 0.0f, rollDeg = 0.0f;
    float px = 0.0f, py = 0.0f, pz = 0.0f; // meters, XR local space
    uint64_t deadline = 0;
};
SimHead g_simHead;

// vrrec cadence: the detour fires many times per rendered frame, so the
// recorder tap advances on a present-count edge (see recorder.h). Game
// thread only.
uint64_t g_lastPresentSeen = 0;

// Thread identity. The camera hook installs at ~T+0.4 s and the first Present
// lands at ~T+8.5 s, so the FIRST detour fire can precede any Present and
// d3d11_hook::last_present_tid() may still be 0. The comparison therefore lives
// on the throttled path, not in the first-fire line.
std::atomic<uint32_t> g_cameraTid{0};
std::atomic<uint32_t> g_foreignTidCalls{0};
std::atomic<bool> g_loggedThreadSplit{false};

// Path census. GetPlayerViewPoint has four internal paths and only the first is
// a cheap cached read; which one runs tells us whether +0x248 and +0x240 mean
// what session 34 inferred from shape.
std::atomic<uint32_t> g_pathCached{0};   // [this+0x248] bit 0 set
std::atomic<uint32_t> g_pathCamera{0};   // clear, and [this+0x240] non-null
std::atomic<uint32_t> g_pathTarget{0};   // clear, and [this+0x240] null
std::atomic<uint32_t> g_pathUnknown{0};  // `this` unreadable
std::atomic<bool> g_loggedMatrix{false};
std::atomic<void*> g_lastSelf{nullptr};

// Silence detection. The game-thread pump rides on this hook, so a camera that
// stops is a command surface that stops - and that must be a timestamped fact
// in the log, not a mystery. Detected on RESUME by comparing against the
// previous call's timestamp, which needs no extra state and no second thread:
// the only observer that can be sure a gap ended is the call that ends it.
constexpr uint64_t kSilenceReportMs = 2000;

struct Snapshot {
    FVector loc{};
    FRotator rot{};
    // The PRE-TRANSFORM source for whichever internal path actually ran, and
    // its name. Session 36 shipped this comparing only against [this+0x24C],
    // which is path 1's source - while every observed sample took path 2 and
    // read [cam+0x3B8]. It compared the wrong field and its "raw copy" verdict
    // was worthless. Now the source follows the path.
    FVector sourceLoc{};
    const char* sourceName = "";
    bool sourceValid = false;
    bool valid = false;
};
Snapshot g_last;

// ---------------------------------------------------------------------------
// The 1 Hz probe. `this` is an engine pointer we did not create, so every read
// is SEH-guarded and the guarded body is POD-only: no logging and no allocation
// inside the guard, because MSVC under /EHsc does not run destructors during
// SEH unwinding and a fault taken while the log mutex was held would leave it
// held for the life of the process (BioShock 1 learned that expensively).
//
// This runs at 1 Hz and never per call: is_memory_valid is a VirtualQuery, and
// this detour can run 4000+ times a second.
// ---------------------------------------------------------------------------
struct Probe {
    bool ok = false;
    bool cachedFlag = false;
    bool cameraNonNull = false;
    FVector cachedLoc{};  // [this+0x24C], path 1's source
    FVector camLoc{};     // [cam+0x3B8],  path 2's source
    bool camLocRead = false;
    float matrix[16] = {};
    bool matrixOk = false;
};

Probe probe_self(void* self) {
    Probe p{};
    if (!self) return p;
    __try {
        const uint8_t* base = static_cast<const uint8_t*>(self);
        p.cachedFlag =
            (*reinterpret_cast<const uint8_t*>(base + patterns::kPcCachedPovFlagOffset) & 1u) != 0;
        const uint8_t* cam =
            *reinterpret_cast<const uint8_t* const*>(base + patterns::kPcCameraOffset);
        p.cameraNonNull = cam != nullptr;
        memcpy(&p.cachedLoc, base + patterns::kPcCachedLocOffset, sizeof p.cachedLoc);
        memcpy(p.matrix, base + patterns::kPcViewTransformOffset, sizeof p.matrix);
        p.matrixOk = true;
        // Second-level dereference, and the reason the whole probe is SEH
        // guarded: `cam` is an engine pointer we neither created nor own.
        if (cam) {
            memcpy(&p.camLoc, cam + patterns::kCameraPovLocOffset, sizeof p.camLoc);
            p.camLocRead = true;
        }
        p.ok = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        p.ok = false;
    }
    return p;
}

void log_matrix_once(const Probe& p) {
    if (!p.matrixOk) return;
    if (g_loggedMatrix.exchange(true)) return;
    // All four internal paths converge on a 4x4 SSE transform fed from
    // [this+0x430]. Whether the returned view is TRANSFORMED or merely copied
    // decides where an I4 HMD pose has to be injected, so it is measured here
    // rather than assumed.
    char line[320];
    int n = 0;
    line[0] = '\0';
    for (int i = 0; i < 16 && n >= 0 && static_cast<size_t>(n) + 12 < sizeof line; ++i)
        n += _snprintf_s(line + n, sizeof line - n, _TRUNCATE, "%.4f%s", p.matrix[i],
                         (i % 4 == 3) ? " | " : " ");
    BVR_LOG("[bsi] camera: [this+0x430] 4x4 = %s", line);
}

// ---------------------------------------------------------------------------
// The I4 drive (session 39). Writes ONLY through the out-params, after the
// original has filled them - the engine's camera state is never touched, so
// drive-off is a byte-identical passthrough and the engine's own view keeps
// moving under mouse/pad (read back every beat as engineRot on the drive
// heartbeat). Runs on EVERY detour call so all of a frame's callers see one
// consistent substituted view; the math is a handful of trig calls, far below
// this seam's 9681/s ceiling.
//
// Lane priority is BS1's proven order: vrrec replay -> simhead -> live. While
// a replay is loaded the live/sim lanes are never consulted (a replayed frame
// must never recenter or read the live head); an entry recorded with the
// camera not driven leaves the camera to the game, faithfully.
// ---------------------------------------------------------------------------
void drive_view(FVector* loc, FRotator* rot, uint64_t now) {
    if (!loc || !rot) return;

    bvr::vr::HeadPose hp{};
    bool driveHead = false;
    bool liveHead = false;
    const char* lane = "off";
    if (recorder::playing()) {
        if (recorder::replay_head(hp)) {
            driveHead = true;
            lane = "replay";
        }
    } else if (now < g_simHead.deadline) {
        float q[4];
        xr_local_trim_quat(g_simHead.pitchDeg / kRadToDeg, g_simHead.yawDeg / kRadToDeg,
                           g_simHead.rollDeg / kRadToDeg, q);
        hp = {};
        hp.px = g_simHead.px;
        hp.py = g_simHead.py;
        hp.pz = g_simHead.pz;
        hp.qx = q[0];
        hp.qy = q[1];
        hp.qz = q[2];
        hp.qw = q[3];
        driveHead = true; // sim lane stays ungated for flat tests
        lane = "sim";
    } else if (g_driveEnabled.load(std::memory_order_relaxed) && bvr::vr::get_head_pose(hp)) {
        // Live lane. Gated on the adapter's own flag plus a valid located
        // pose - NOT on vr_camera_mode(): camera mode flips core's submission
        // from the quad to a projection layer, and I4 is the MonoTracked rung
        // (head-driven camera UNDER the quad). set_camera_mode is never
        // called anywhere in this adapter until I5.
        driveHead = true;
        liveHead = true;
        lane = "live";
    }

    if (driveHead) {
        const UeAngles a = ue_angles_from_xr_quat(hp.qx, hp.qy, hp.qz, hp.qw);
        if (g_recenterRequested.exchange(false, std::memory_order_relaxed) || !g_haveRecenter) {
            g_recenterPose = hp;
            g_recenterYawUnits = static_cast<int32_t>(lroundf(a.yawRad * kRotUnitsPerRadian));
            g_haveRecenter = true;
            BVR_LOG("[bsi] vr camera recentered (yaw %.1f deg)", a.yawRad * kRadToDeg);
        }

        // Rotation: yaw ADDITIVE (the game's own yaw plus the head-look
        // residual, so stick/mouse turning and game scripting keep working);
        // pitch and roll ABSOLUTE from the head - on the out-param only, the
        // engine's own values stay engine-owned underneath.
        const int32_t gameYawUnits = rot->yaw;
        const int32_t headYawUnits =
            static_cast<int32_t>(lroundf(a.yawRad * kRotUnitsPerRadian));
        const int32_t residualUnits = wrap_rot(headYawUnits - g_recenterYawUnits);
        const float gameYawRad = static_cast<float>(gameYawUnits) / kRotUnitsPerRadian;
        {
            const int32_t headPitchUnits =
                static_cast<int32_t>(lroundf(a.pitchRad * kRotUnitsPerRadian));
            g_pitchErrDeg =
                static_cast<float>(wrap_rot(headPitchUnits - rot->pitch)) / kRotUnitsPerDegree;
        }
        rot->pitch = static_cast<int32_t>(a.pitchRad * kRotUnitsPerRadian);
        rot->roll = static_cast<int32_t>(a.rollRad * kRotUnitsPerRadian);
        rot->yaw = gameYawUnits + residualUnits;

        // Position: recenter-relative head offset, mapped XR->UE, rotated
        // into the recenter-local frame and back out by the game yaw (which
        // is where recenter-forward points now, since our yaw is purely
        // additive), scaled UU-per-meter, ADDED to the game's own location.
        // Z is world-up, unrotated.
        const float dxr[3] = {hp.px - g_recenterPose.px, hp.py - g_recenterPose.py,
                              hp.pz - g_recenterPose.pz};
        float d[3];
        xr_to_ue(dxr, d);
        const float scale = g_worldScale.load(std::memory_order_relaxed);
        const float recenterYawRad = recenter_yaw_rad();
        const float c = cosf(-recenterYawRad), s = sinf(-recenterYawRad);
        const float lx = d[0] * c - d[1] * s;
        const float ly = d[0] * s + d[1] * c;
        const float cg = cosf(gameYawRad), sg = sinf(gameYawRad);
        const float ox = (lx * cg - ly * sg) * scale;
        const float oy = (lx * sg + ly * cg) * scale;
        const float oz = d[2] * scale;
        loc->x += ox;
        loc->y += oy;
        loc->z += oz;
        g_headOffX.store(ox, std::memory_order_relaxed);
        g_headOffY.store(oy, std::memory_order_relaxed);
        g_headOffZ.store(oz, std::memory_order_relaxed);
    } else {
        g_pitchErrDeg = 0.0f;
    }

    g_driveLane = lane;
    g_finalLoc = *loc;
    g_finalRot = *rot;
    g_finalValid = true;
    g_vrDriving.store(driveHead, std::memory_order_relaxed);

    // The vrrec tap, once per rendered frame (present-count edge - the seam
    // itself fires many times per frame). Both record and replay advance on
    // the same edge, so the cadences match by construction.
    const uint64_t present = bvr::d3d11_hook::present_count();
    if (present != g_lastPresentSeen) {
        g_lastPresentSeen = present;
        recorder::on_tick(driveHead ? hp : bvr::vr::HeadPose{}, driveHead, liveHead, *loc,
                          *rot);
    }
}

// --- vrpreset: minimal per-game persistence (worldScale only, I4) -----------
std::wstring vr_preset_path() {
    std::wstring p = bvr::log::data_dir();
    p += L"\\vrpreset.ini";
    return p;
}

void save_vr_preset() {
    FILE* f = nullptr;
    if (_wfopen_s(&f, vr_preset_path().c_str(), L"w") != 0 || !f) {
        BVR_LOG("[bsi] vrpreset save FAILED (open)");
        return;
    }
    fprintf(f, "worldScale=%.1f\n", g_worldScale.load(std::memory_order_relaxed));
    fprintf(f, "claimTanV=%.4f\n", g_claimTanV.load(std::memory_order_relaxed));
    fclose(f);
    BVR_LOG("[bsi] vrpreset saved (worldScale=%.1f claimTanV=%.4f)",
            g_worldScale.load(std::memory_order_relaxed),
            g_claimTanV.load(std::memory_order_relaxed));
}

// ---------------------------------------------------------------------------
// The projection claim, published on every detour call (BS2 publishes per
// CalcView - same seam role here; the math is one atan). Two core inputs:
//  - set_rendered_hfov: the horizontal FOV the game actually renders, from
//    the law hfov = 2*atan(tanV x aspect). hfovSrc=0, the honest claim - the
//    projection layer is tagged with it, and a mismatch reads as fisheye.
//  - publish_gameplay_view(true): core's cinematic fallback (g_cineEnabled
//    defaults ON) drops projection to the quad whenever this publish goes
//    STALE, so it must tick every dispatch. Strict stays constant true for
//    I5 - Infinite has no cinematic classifier until I9, and the stale leg
//    already quads load screens for free (this seam goes silent there).
// ---------------------------------------------------------------------------
void publish_projection_claim() {
    unsigned w = 0, h = 0;
    float aspect = 16.0f / 9.0f; // pre-first-Present fallback, matches the law's 16:9 row
    if (bvr::hud::backbuffer_dims(&w, &h) && w > 0 && h > 0)
        aspect = static_cast<float>(w) / static_cast<float>(h);
    const float tanV = g_claimTanV.load(std::memory_order_relaxed);
    const float hfovDeg = 2.0f * atanf(tanV * aspect) * kRadToDeg;
    bvr::vr::set_rendered_hfov(hfovDeg);
    bvr::vr::publish_gameplay_view(true);
    g_lastClaimHfovDeg.store(hfovDeg, std::memory_order_relaxed);
    g_lastClaimAspect.store(aspect, std::memory_order_relaxed);
}

// The vrstereo one-toggle, rung 1: quad <-> projection. Runs on the GAME
// thread only (posted from the overlay via g_vrstereoPending). NO 1t rung on
// this game by design - DR-I5 measured a threaded ring-buffered substrate
// (BS2's shape, not BS1's kick-and-wait). Rung 3 extends ON with pair pacing
// and the doubled scene build; the arming order is already BS2's proven one.
// OFF returns to the MONO QUAD (session stays live), symmetric across every
// stereo backend so an off can never strand one armed (BS2's asymmetric-off
// trap).
void apply_vrstereo(bool on) {
    if (on) {
        bvr::vr::set_enabled(true);
        bvr::vr::set_camera_mode(true);
        g_stereoArmed.store(true, std::memory_order_relaxed);
        BVR_LOG("[bsi] VRSTEREO ON (I5 rung 1): camera mode requested - core flips quad -> "
                "projection once views locate. Claim tanV=%.4f hfov=%.1f deg aspect=%.4f. "
                "Mono projection: both eyes the same image until AER/SR arm.",
                g_claimTanV.load(std::memory_order_relaxed),
                g_lastClaimHfovDeg.load(std::memory_order_relaxed),
                g_lastClaimAspect.load(std::memory_order_relaxed));
    } else {
        bvr::vr::set_alternate_eye(false);
        bvr::vr::set_camera_mode(false);
        g_stereoArmed.store(false, std::memory_order_relaxed);
        BVR_LOG("[bsi] vrstereo off - back to the mono quad (session stays live)");
    }
}

void apply_pending_vrstereo() {
    const int pending = g_vrstereoPending.exchange(-1, std::memory_order_relaxed);
    if (pending >= 0) apply_vrstereo(pending != 0);
}

void throttled(void* self, uint64_t now) {
    const uint32_t count = g_callCount.load(std::memory_order_relaxed);

    const Probe p = probe_self(self);
    if (!p.ok) {
        g_pathUnknown.fetch_add(1, std::memory_order_relaxed);
    } else if (p.cachedFlag) {
        g_pathCached.fetch_add(1, std::memory_order_relaxed);
    } else if (p.cameraNonNull) {
        g_pathCamera.fetch_add(1, std::memory_order_relaxed);
    } else {
        // Paths 3 and 4 are ONE honest bucket. Separating them means calling
        // vtable slot +0x2C0 (GetViewTarget), and a virtual call out of a
        // detour can lazily create engine objects - the guard would be worse
        // than the thing it measures.
        g_pathTarget.fetch_add(1, std::memory_order_relaxed);
    }
    if (p.ok) {
        // Bind the comparison to the path that ACTUALLY RAN. Getting this wrong
        // is not a cosmetic bug: it is the difference between "the returned
        // view is a raw copy" and "the returned view is transformed", which is
        // what decides where I4 injects an HMD pose.
        if (p.cachedFlag) {
            g_last.sourceLoc = p.cachedLoc;
            g_last.sourceName = "cached POV [this+0x24C]";
            g_last.sourceValid = true;
        } else if (p.cameraNonNull && p.camLocRead) {
            g_last.sourceLoc = p.camLoc;
            g_last.sourceName = "camera POV [cam+0x3B8]";
            g_last.sourceValid = true;
        } else {
            // Paths 3 and 4 read off the view target, which we refuse to
            // resolve from inside a detour. No source, so no claim.
            g_last.sourceValid = false;
            g_last.sourceName = "view target (not resolved - no claim made)";
        }
        log_matrix_once(p);
    }

    // Thread split, once. Feeds DR-I5: if UE3's game thread and render thread
    // are the same here, the whole substrate question changes shape.
    const uint32_t presentTid = bvr::d3d11_hook::last_present_tid();
    if (presentTid != 0 && !g_loggedThreadSplit.exchange(true)) {
        const uint32_t camTid = g_cameraTid.load(std::memory_order_relaxed);
        BVR_LOG("[bsi] camera: thread split - camera tid %u, present tid %u -> %s", camTid,
                presentTid,
                camTid == presentTid ? "SAME THREAD (game and render are one)"
                                     : "separate game and render threads");
    }

    if (!g_heartbeat.load(std::memory_order_relaxed) || g_beatsLeft <= 0) {
        g_lastBeatMs = 0;
        return;
    }
    if (g_lastBeatMs == 0) {
        g_lastBeatMs = now;
        g_beatBaseCount = count;
        return;
    }
    if (now - g_lastBeatMs < 1000) return;

    const uint32_t perSec =
        static_cast<uint32_t>((count - g_beatBaseCount) * 1000ull / (now - g_lastBeatMs));
    // Rotator components as %d ALWAYS, with degrees alongside. Never %f.
    BVR_LOG("[bsi] camera: loc=(%.1f %.1f %.1f) rot=(%d %d %d) = (%.1f %.1f %.1f)deg "
            "(%u calls/s, %u total)",
            g_last.loc.x, g_last.loc.y, g_last.loc.z, g_last.rot.pitch, g_last.rot.yaw,
            g_last.rot.roll, g_last.rot.pitch * kRotToDeg, g_last.rot.yaw * kRotToDeg,
            g_last.rot.roll * kRotToDeg, perSec, count);
    if (g_last.sourceValid) {
        // The delta between the source the ACTIVE path read and the value
        // actually handed back. Non-zero means the documented 4x4 transform is
        // really applied on the way out - measured, not assumed, and now
        // measured against the right field.
        const float dx = g_last.loc.x - g_last.sourceLoc.x;
        const float dy = g_last.loc.y - g_last.sourceLoc.y;
        const float dz = g_last.loc.z - g_last.sourceLoc.z;
        BVR_LOG("[bsi] camera: returned-minus-source d=(%.3f %.3f %.3f) vs %s%s", dx, dy, dz,
                g_last.sourceName,
                (dx == 0.0f && dy == 0.0f && dz == 0.0f)
                    ? "  <- identical, this path hands back its source unchanged"
                    : "  <- TRANSFORMED on the way out");
    } else {
        BVR_LOG("[bsi] camera: returned-minus-source SKIPPED - %s", g_last.sourceName);
    }
    if (g_finalValid) {
        // The FINAL camera handed back to the game - drive and offsets and
        // all - which is what the flat 6DoF checks measure (simhead -> exact
        // degrees; sim position -> headOff in UU). engineRot is the pre-drive
        // snapshot: the engine's own view, which must keep moving under
        // mouse/pad while the drive is on - a frozen engineRot next to a
        // moving final rot is the BS1 pitch-freeze bug showing itself.
        // xr=<state> because a running session that never reached FOCUSED
        // still paces the game at the runtime's not-visible cadence.
        BVR_LOG("[bsi] drive: lane=%s final loc=(%.1f %.1f %.1f) rot=(%d %d %d) = "
                "(%.1f %.1f %.1f)deg engineRot=(%d %d %d) pitchErr=%.1f "
                "headOff=(%.1f %.1f %.1f) scale=%.0f xr=%s",
                g_driveLane, g_finalLoc.x, g_finalLoc.y, g_finalLoc.z, g_finalRot.pitch,
                g_finalRot.yaw, g_finalRot.roll, g_finalRot.pitch * kRotToDeg,
                g_finalRot.yaw * kRotToDeg, g_finalRot.roll * kRotToDeg, g_last.rot.pitch,
                g_last.rot.yaw, g_last.rot.roll, g_pitchErrDeg,
                g_headOffX.load(std::memory_order_relaxed),
                g_headOffY.load(std::memory_order_relaxed),
                g_headOffZ.load(std::memory_order_relaxed),
                g_worldScale.load(std::memory_order_relaxed), bvr::vr::session_state_name());
    }
    if (g_stereoArmed.load(std::memory_order_relaxed)) {
        // The I5 stereo heartbeat: what the flat battery asserts on. camMode
        // is core's composite (requested AND session AND projection-ready);
        // the audit tangents are what the last projection layer was actually
        // TAGGED with (src 0 = our claim - anything else means core fell back
        // and the claim is not being consumed).
        float auditTanH = 0.0f, auditTanV = 0.0f;
        int auditSrc = -1;
        unsigned swapW = 0, swapH = 0;
        bvr::vr::fov_audit(&auditTanH, &auditTanV, &auditSrc, &swapW, &swapH);
        BVR_LOG("[bsi] stereo: armed=1 camMode=%d claim tanV=%.4f aspect=%.4f hfov=%.1f | "
                "audit tanH=%.4f tanV=%.4f src=%d swap=%ux%u",
                bvr::vr::vr_camera_mode() ? 1 : 0,
                g_claimTanV.load(std::memory_order_relaxed),
                g_lastClaimAspect.load(std::memory_order_relaxed),
                g_lastClaimHfovDeg.load(std::memory_order_relaxed), auditTanH, auditTanV,
                auditSrc, swapW, swapH);
    }
    g_lastBeatMs = now;
    g_beatBaseCount = count;
    --g_beatsLeft;
}

// ---------------------------------------------------------------------------
// The detour. The probe/observation machinery is read-only (the snapshot is
// copied into const locals BEFORE the drive, so the heartbeat's
// returned-minus-source instrument keeps measuring the ORIGINAL's output);
// the ONLY writes through loc/rot in this translation unit are in drive_view,
// which substitutes the out-params after the original has filled them and
// never touches engine memory.
// ---------------------------------------------------------------------------
void __fastcall GetViewPointDetour(void* self, void* edx, FVector* loc, FRotator* rot) {
    // Original FIRST. It writes both out-params on all four internal paths, so
    // after this call their writability is proven rather than assumed.
    g_original(self, edx, loc, rot);

    if (!g_enabled.load(std::memory_order_relaxed)) return;

    const uint32_t count = g_callCount.fetch_add(1, std::memory_order_relaxed) + 1;
    const uint64_t now = GetTickCount64();
    const uint64_t prevCallMs = g_lastCallMs.exchange(now, std::memory_order_relaxed);
    g_fired.store(true, std::memory_order_relaxed);

    const uint32_t tid = GetCurrentThreadId();
    uint32_t expectedTid = 0;
    if (!g_cameraTid.compare_exchange_strong(expectedTid, tid, std::memory_order_relaxed) &&
        expectedTid != tid) {
        // A second thread dispatching this is DR-I5 evidence, not a bug - but
        // the throttled block is not thread-safe, so foreign threads are
        // counted and then leave.
        g_foreignTidCalls.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    // Snapshot for the heartbeat. const locals: nothing here can write back.
    const FVector outLoc = loc ? *loc : FVector{};
    const FRotator outRot = rot ? *rot : FRotator{};
    g_last.loc = outLoc;
    g_last.rot = outRot;
    g_last.valid = true;
    g_lastSelf.store(self, std::memory_order_relaxed);

    if (!g_loggedFirstFire.exchange(true)) {
        BVR_LOG("[bsi] camera: FIRST FIRE - GetPlayerViewPoint detour live. this=%p tid=%u "
                "loc=(%.1f %.1f %.1f) rot=(%d %d %d)",
                self, tid, outLoc.x, outLoc.y, outLoc.z, outRot.pitch, outRot.yaw, outRot.roll);
    }

    // THE HANDOVER. This is the whole reason the command seam was built to be
    // pump-agnostic: from here commands run on the GAME thread, where anything
    // that touches the engine belongs. Safe to call every frame - it is 1 Hz
    // internally - and it latches on the first call, so the takeover happens
    // the moment the hook is proven live rather than on a timer.
    bvr::command::poll_from_game_thread(now);

    // A gap that just ended. During it the game-thread command pump was silent
    // too, so this line is what turns "the mod stopped responding" into a
    // timestamped fact. Level loads and cinematics are the expected causes.
    if (prevCallMs != 0 && now - prevCallMs >= kSilenceReportMs) {
        BVR_LOG("[bsi] camera: RESUMED after %llu ms silent - the game-thread command pump was "
                "silent with it for that whole window",
                static_cast<unsigned long long>(now - prevCallMs));
        g_lastBeatMs = 0; // reseed the base rather than report a fake calls/s spike
    }

    // I5: consume a posted vrstereo toggle (game thread, before the drive so
    // it takes effect this call), then publish the projection claim - the
    // fov the layer is tagged with plus the gameplay-view liveness that keeps
    // core's cinematic fallback from quadding a live gameplay projection.
    apply_pending_vrstereo();
    publish_projection_claim();

    // The I4 drive, AFTER the snapshot (so the observation instruments keep
    // measuring the original) and after the command poll (so a just-dispatched
    // simhead/recenter takes effect on this very call).
    drive_view(loc, rot, now);

    static uint64_t s_lastThrottle = 0;
    if (now - s_lastThrottle >= 1000) {
        s_lastThrottle = now;
        throttled(self, now);
    }
}

const char* path_summary(char* buf, size_t n) {
    _snprintf_s(buf, n, _TRUNCATE, "cached=%u camera=%u viewtarget-or-self=%u unreadable=%u",
                g_pathCached.load(), g_pathCamera.load(), g_pathTarget.load(),
                g_pathUnknown.load());
    return buf;
}

void log_status() {
    char paths[160];
    BVR_LOG("[bsi] camera: hook=%s enabled=%s fired=%s calls=%u silent=%llu ms tid=%u "
            "foreign-tid-calls=%u",
            g_hookLive.load() ? "installed" : "NOT installed",
            g_enabled.load() ? "yes" : "no", g_fired.load() ? "YES" : "no",
            g_callCount.load(), static_cast<unsigned long long>(silent_ms()),
            g_cameraTid.load(), g_foreignTidCalls.load());
    BVR_LOG("[bsi] camera: paths %s", path_summary(paths, sizeof paths));
}

} // namespace

bool install(const bvr::pattern_scan::ProcessImage& image) {
    (void)image;
    if (g_hookLive.load()) return true;

    if (!patterns::rva_trusted()) {
        BVR_LOG("[bsi] camera: hook NOT installed - build gate closed. The game runs flat.");
        return false;
    }

    const uint8_t* target =
        patterns::rva_to_address(patterns::kGetPlayerViewPointRva, 64);
    if (!target) {
        BVR_LOG("[bsi] camera: hook NOT installed - RVA 0x%X is not readable",
                patterns::kGetPlayerViewPointRva);
        return false;
    }

    // Prologue gate. A hardcoded RVA on the wrong build does not fail to work,
    // it detours whatever happens to live there - which is how a mod corrupts a
    // game rather than merely not helping. REFUSE on any mismatch.
    if (memcmp(target, patterns::kGetPlayerViewPointPrologue,
               sizeof patterns::kGetPlayerViewPointPrologue) != 0) {
        BVR_LOG("[bsi] camera: prologue MISMATCH at RVA 0x%X (got %02X %02X %02X %02X ...) - "
                "build changed? REFUSING hook, the game runs flat",
                patterns::kGetPlayerViewPointRva, target[0], target[1], target[2], target[3]);
        return false;
    }

    // Independently confirm the argument count the typedef declares. `ret 8`
    // is C2 08 00; if it is not in the body, the 2-stack-arg assumption is
    // wrong and hooking would pop the RTC dialog that writes no crash dump.
    constexpr size_t kRetScanBytes = 0x400;
    const uint8_t* body = patterns::rva_to_address(patterns::kGetPlayerViewPointRva,
                                                   kRetScanBytes);
    bool retFound = false;
    if (body) {
        for (size_t i = 0; i + 3 <= kRetScanBytes; ++i) {
            if (body[i] == 0xC2 && body[i + 1] == patterns::kGetPlayerViewPointRetImm &&
                body[i + 2] == 0x00) {
                retFound = true;
                break;
            }
        }
    }
    if (!retFound) {
        BVR_LOG("[bsi] camera: no `ret %u` (C2 %02X 00) found in the first 0x400 bytes of "
                "RVA 0x%X - the 2-stack-arg assumption is UNCONFIRMED. REFUSING hook rather "
                "than risking a misaligned return (the RTC dialog writes no crash dump).",
                patterns::kGetPlayerViewPointRetImm, patterns::kGetPlayerViewPointRetImm,
                patterns::kGetPlayerViewPointRva);
        return false;
    }

    void* addr = const_cast<uint8_t*>(target);
    MH_STATUS status = MH_CreateHook(addr, reinterpret_cast<void*>(&GetViewPointDetour),
                                     reinterpret_cast<void**>(&g_original));
    if (status != MH_OK) {
        BVR_LOG("[bsi] camera: MH_CreateHook failed: %s", MH_StatusToString(status));
        return false;
    }
    // Self-enabling so this hook's activation never rides on another module's
    // MH_EnableHook(MH_ALL_HOOKS).
    status = MH_EnableHook(addr);
    if (status != MH_OK) {
        BVR_LOG("[bsi] camera: MH_EnableHook failed: %s", MH_StatusToString(status));
        MH_RemoveHook(addr);
        g_original = nullptr;
        return false;
    }

    g_target = addr;
    g_hookLive.store(true, std::memory_order_relaxed);
    BVR_LOG("[bsi] camera: hook installed on GetPlayerViewPoint (target %p, RVA 0x%X, "
            "prologue and `ret %u` both verified). I4 drive present but OFF by default - "
            "the write target is the detour's out-params ONLY, never engine memory; with "
            "the drive off the hook observes only.",
            addr, patterns::kGetPlayerViewPointRva, patterns::kGetPlayerViewPointRetImm);
    return true;
}

bool has_fired() {
    return g_fired.load(std::memory_order_relaxed);
}

bool hook_live() {
    return g_hookLive.load(std::memory_order_relaxed);
}

void* last_player_controller() {
    return g_lastSelf.load(std::memory_order_relaxed);
}

uint32_t camera_tid() {
    return g_cameraTid.load(std::memory_order_relaxed);
}

uint64_t silent_ms() {
    const uint64_t last = g_lastCallMs.load(std::memory_order_relaxed);
    if (last == 0) return 0;
    const uint64_t now = GetTickCount64();
    return now > last ? now - last : 0;
}

void get_recenter_state(bvr::vr::HeadPose* pose, int32_t* yawUnits, float* worldScale) {
    if (pose) *pose = g_recenterPose;
    if (yawUnits) *yawUnits = g_recenterYawUnits;
    if (worldScale) *worldScale = g_worldScale.load(std::memory_order_relaxed);
}

void set_recenter_state(const bvr::vr::HeadPose& pose, int32_t yawUnits, float worldScale) {
    g_recenterPose = pose;
    g_recenterYawUnits = yawUnits;
    g_worldScale.store(worldScale, std::memory_order_relaxed);
    g_haveRecenter = true;
    // A pending auto-recenter would re-reference the mapping onto the first
    // replayed head pose, throwing away the state just restored.
    g_recenterRequested.store(false, std::memory_order_relaxed);
    BVR_LOG("[bsi] recenter state SET (yaw %d units, worldScale %.1f) - vrrec play restore",
            yawUnits, worldScale);
}

void load_vr_preset() {
    FILE* f = _wfsopen(vr_preset_path().c_str(), L"r", _SH_DENYNO);
    if (!f) return; // no preset yet - defaults stand
    char line[128];
    while (fgets(line, sizeof line, f)) {
        float v = 0.0f;
        if (sscanf_s(line, "worldScale=%f", &v) == 1 && v > 0.0f)
            g_worldScale.store(v, std::memory_order_relaxed);
        else if (sscanf_s(line, "claimTanV=%f", &v) == 1 && v > 0.0f)
            g_claimTanV.store(v, std::memory_order_relaxed);
    }
    fclose(f);
    BVR_LOG("[bsi] vrpreset loaded (worldScale=%.1f claimTanV=%.4f)",
            g_worldScale.load(std::memory_order_relaxed),
            g_claimTanV.load(std::memory_order_relaxed));
}

bool handle_drive_verb(const char* cmd, const char* args) {
    if (!args) args = "";
    while (*args == ' ') ++args;

    if (strcmp(cmd, "recenter") == 0) {
        g_recenterRequested.store(true, std::memory_order_relaxed);
        BVR_LOG("[bsi] command: recenter");
        return true;
    }
    if (strcmp(cmd, "worldscale") == 0) {
        float v = 0.0f;
        if (sscanf_s(args, "%f", &v) == 1 && v > 0.0f) {
            g_worldScale.store(v, std::memory_order_relaxed);
            BVR_LOG("[bsi] command: worldscale %.1f", v);
        } else {
            BVR_LOG("[bsi] usage: worldscale <uuPerMeter> (current %.1f)",
                    g_worldScale.load(std::memory_order_relaxed));
        }
        return true;
    }
    if (strcmp(cmd, "vrpreset") == 0) {
        if (strncmp(args, "save", 4) == 0) {
            save_vr_preset();
        } else {
            load_vr_preset();
            BVR_LOG("[bsi] vrpreset: worldScale=%.1f (vrpreset save persists the current "
                    "values)",
                    g_worldScale.load(std::memory_order_relaxed));
        }
        return true;
    }
    if (strcmp(cmd, "vrstereo") == 0) {
        // Seam commands already run on the game thread (the pump handover), so
        // this applies directly; the overlay checkbox posts instead.
        if (strncmp(args, "on", 2) == 0) {
            apply_vrstereo(true);
        } else if (strncmp(args, "off", 3) == 0) {
            apply_vrstereo(false);
        } else {
            BVR_LOG("[bsi] vrstereo: armed=%d camMode=%d session=%s | usage: vrstereo on|off",
                    g_stereoArmed.load(std::memory_order_relaxed) ? 1 : 0,
                    bvr::vr::vr_camera_mode() ? 1 : 0, bvr::vr::session_state_name());
        }
        return true;
    }
    if (strcmp(cmd, "bsifov") == 0) {
        if (strncmp(args, "tanv", 4) == 0) {
            float v = 0.0f;
            if (sscanf_s(args + 4, "%f", &v) == 1 && v > 0.1f && v < 2.0f) {
                g_claimTanV.store(v, std::memory_order_relaxed);
                BVR_LOG("[bsi] fov claim: tanV=%.4f (law: slider min %.4f .. max %.4f; verify "
                        "against a `dumpframe cb` decode)",
                        v, patterns::kTanVSliderMin, patterns::kTanVSliderMax);
            } else {
                BVR_LOG("[bsi] usage: bsifov tanv <0.1..2.0> (current %.4f)",
                        g_claimTanV.load(std::memory_order_relaxed));
            }
        } else {
            float auditTanH = 0.0f, auditTanV = 0.0f;
            int auditSrc = -1;
            unsigned swapW = 0, swapH = 0;
            bvr::vr::fov_audit(&auditTanH, &auditTanV, &auditSrc, &swapW, &swapH);
            BVR_LOG("[bsi] fov claim: tanV=%.4f aspect=%.4f hfov=%.1f deg | audit tanH=%.4f "
                    "tanV=%.4f src=%d swap=%ux%u | usage: bsifov [tanv <v>]",
                    g_claimTanV.load(std::memory_order_relaxed),
                    g_lastClaimAspect.load(std::memory_order_relaxed),
                    g_lastClaimHfovDeg.load(std::memory_order_relaxed), auditTanH, auditTanV,
                    auditSrc, swapW, swapH);
        }
        return true;
    }
    if (strcmp(cmd, "simhead") == 0) {
        if (strncmp(args, "off", 3) == 0) {
            g_simHead.deadline = 0;
            BVR_LOG("[bsi] command: simhead off");
        } else {
            // 3 args = angles; 4 = angles + holdMs (BS1-compatible); 6 =
            // angles + position; 7 = angles + position + holdMs.
            float v[7] = {};
            int n = sscanf_s(args, "%f %f %f %f %f %f %f", &v[0], &v[1], &v[2], &v[3], &v[4],
                             &v[5], &v[6]);
            if (n == 3 || n == 4 || n == 6 || n == 7) {
                const bool wasIdle = GetTickCount64() >= g_simHead.deadline;
                g_simHead.yawDeg = v[0];
                g_simHead.pitchDeg = v[1];
                g_simHead.rollDeg = v[2];
                g_simHead.px = n >= 6 ? v[3] : 0.0f;
                g_simHead.py = n >= 6 ? v[4] : 0.0f;
                g_simHead.pz = n >= 6 ? v[5] : 0.0f;
                int hold = n == 4   ? static_cast<int>(v[3])
                           : n == 7 ? static_cast<int>(v[6])
                                    : 0;
                if (hold <= 0) hold = 120000;
                g_simHead.deadline = GetTickCount64() + static_cast<uint64_t>(hold);
                if (wasIdle) g_recenterRequested.store(true, std::memory_order_relaxed);
                BVR_LOG("[bsi] command: simhead yaw %.1f pitch %.1f roll %.1f "
                        "pos (%.2f %.2f %.2f) for %d ms%s",
                        v[0], v[1], v[2], g_simHead.px, g_simHead.py, g_simHead.pz, hold,
                        wasIdle ? " (recentering onto first sim pose)" : "");
            } else {
                BVR_LOG("[bsi] usage: simhead <yaw> <pitch> <roll> [px py pz] [holdMs] | "
                        "simhead off");
            }
        }
        return true;
    }
    return false;
}

bool handle_command(const char* args) {
    if (!args) args = "";
    while (*args == ' ') ++args;

    if (strncmp(args, "status", 6) == 0 || *args == '\0') {
        log_status();
        return true;
    }
    if (strncmp(args, "paths", 5) == 0) {
        char buf[160];
        BVR_LOG("[bsi] camera: path census %s", path_summary(buf, sizeof buf));
        BVR_LOG("[bsi] camera: 'cached' = [this+0x248] bit 0 set (fast path, +0x24C/+0x258); "
                "'camera' = clear with [this+0x240] non-null; the third bucket is the view "
                "target and the controller's own fields, deliberately NOT separated because "
                "that needs a virtual call out of a detour.");
        return true;
    }
    if (strncmp(args, "tid", 3) == 0) {
        BVR_LOG("[bsi] camera: camera tid=%u present tid=%u foreign-tid calls=%u",
                g_cameraTid.load(), bvr::d3d11_hook::last_present_tid(),
                g_foreignTidCalls.load());
        return true;
    }
    if (strncmp(args, "matrix", 6) == 0) {
        g_loggedMatrix.store(false, std::memory_order_relaxed);
        BVR_LOG("[bsi] camera: re-arming the one-shot [this+0x430] dump for the next beat");
        return true;
    }
    if (strncmp(args, "heartbeat", 9) == 0) {
        const char* v = args + 9;
        while (*v == ' ') ++v;
        const bool on = strncmp(v, "off", 3) != 0;
        g_heartbeat.store(on, std::memory_order_relaxed);
        if (on) g_beatsLeft = 10;
        BVR_LOG("[bsi] camera: heartbeat %s%s", on ? "ON" : "off",
                on ? " (10-beat burst)" : "");
        return true;
    }
    if (strncmp(args, "drive", 5) == 0) {
        const char* v = args + 5;
        while (*v == ' ') ++v;
        if (strncmp(v, "on", 2) == 0) {
            g_driveEnabled.store(true, std::memory_order_relaxed);
            BVR_LOG("[bsi] camera: DRIVE ON (live lane; needs a located head pose - simhead "
                    "and vrrec replay drive regardless). Out-param substitution only; the "
                    "quad stays the submit path.");
        } else if (strncmp(v, "off", 3) == 0) {
            g_driveEnabled.store(false, std::memory_order_relaxed);
            BVR_LOG("[bsi] camera: drive off (passthrough)");
        } else {
            BVR_LOG("[bsi] camera: drive=%s lane=%s driving=%d scale=%.1f recenter=%s "
                    "(bsicam drive on|off)",
                    g_driveEnabled.load(std::memory_order_relaxed) ? "ON" : "off", g_driveLane,
                    g_vrDriving.load(std::memory_order_relaxed) ? 1 : 0,
                    g_worldScale.load(std::memory_order_relaxed),
                    g_haveRecenter ? "captured" : "pending");
        }
        return true;
    }
    if (strncmp(args, "off", 3) == 0) {
        // Deliberately does NOT uninstall. This stops the detour's body, which
        // stops the game-thread command pump, which is exactly the positive
        // control for the pump lease in core/framework/command: four seconds
        // later `vrcmd` must report the Present pump has resumed degraded.
        g_enabled.store(false, std::memory_order_relaxed);
        BVR_LOG("[bsi] camera: observation DISABLED. The game-thread command pump goes silent "
                "with it - the Present pump should take back over within the lease window, "
                "degraded. `bsicam on` restores.");
        return true;
    }
    if (strncmp(args, "on", 2) == 0) {
        g_enabled.store(true, std::memory_order_relaxed);
        g_lastBeatMs = 0;
        BVR_LOG("[bsi] camera: observation enabled");
        return true;
    }
    return false;
}

void draw_debug_ui() {
    // The I4 in-headset surface FIRST and default-open: anything judged by
    // eye gets a control here, never a typed command (alt-tabbing to type
    // destabilises the XR session). Sliders and buttons write atomics only;
    // the game thread consumes them at the next detour call.
    if (ImGui::CollapsingHeader("VR camera (I4)", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Text(g_vrDriving.load(std::memory_order_relaxed)
                        ? "camera: driven by HMD pose (lane on the drive heartbeat)"
                        : "camera: game (enable the drive; live lane needs an XR session)");
        {
            bool drive = g_driveEnabled.load(std::memory_order_relaxed);
            if (ImGui::Checkbox("Drive camera from HMD (quad stays the screen)", &drive))
                g_driveEnabled.store(drive, std::memory_order_relaxed);
        }
        if (ImGui::Button("Recenter (seated pose + view yaw)"))
            g_recenterRequested.store(true, std::memory_order_relaxed);
        {
            float ws = g_worldScale.load(std::memory_order_relaxed);
            if (ImGui::SliderFloat("World scale (UU per m)", &ws, 10.0f, 200.0f, "%.0f"))
                g_worldScale.store(ws, std::memory_order_relaxed);
        }
        ImGui::Text("head offset: (%.1f %.1f %.1f) UU",
                    g_headOffX.load(std::memory_order_relaxed),
                    g_headOffY.load(std::memory_order_relaxed),
                    g_headOffZ.load(std::memory_order_relaxed));
        ImGui::TextDisabled("persist tuning: vrpreset save (worldScale)");
    }

    // I5 stereo. The checkbox POSTS - the game thread applies at the next
    // detour call (rung 3 makes the toggle install a hook, which must never
    // happen on the render thread this UI draws on).
    if (ImGui::CollapsingHeader("VR stereo (I5)", ImGuiTreeNodeFlags_DefaultOpen)) {
        {
            bool armed = g_stereoArmed.load(std::memory_order_relaxed);
            if (ImGui::Checkbox("VR stereo (projection layer)", &armed))
                g_vrstereoPending.store(armed ? 1 : 0, std::memory_order_relaxed);
        }
        ImGui::Text("camera mode: %s (core: requested AND session AND projection-ready)",
                    bvr::vr::vr_camera_mode() ? "LIVE" : "off");
        ImGui::Text("claim: tanV %.4f  aspect %.4f  hfov %.1f deg",
                    g_claimTanV.load(std::memory_order_relaxed),
                    g_lastClaimAspect.load(std::memory_order_relaxed),
                    g_lastClaimHfovDeg.load(std::memory_order_relaxed));
        ImGui::TextDisabled("claim assumes the in-game FOV slider at MINIMUM; if moved, fix "
                            "with bsifov tanv (desktop)");
    }

    if (!ImGui::CollapsingHeader("Camera seam (DR-I2 observation)")) return;
    ImGui::Text("hook: %s   fired: %s", hook_live() ? "installed" : "not installed",
                has_fired() ? "YES" : "no");
    ImGui::Text("calls: %u   silent: %llu ms", g_callCount.load(),
                static_cast<unsigned long long>(silent_ms()));
    ImGui::Text("tid: camera %u / present %u", g_cameraTid.load(),
                bvr::d3d11_hook::last_present_tid());
    ImGui::Text("paths: cached %u | camera %u | target %u | unreadable %u", g_pathCached.load(),
                g_pathCamera.load(), g_pathTarget.load(), g_pathUnknown.load());
    if (g_last.valid) {
        ImGui::Text("engine loc (%.1f %.1f %.1f)", g_last.loc.x, g_last.loc.y, g_last.loc.z);
        ImGui::Text("engine rot (%d %d %d) = (%.1f %.1f %.1f) deg", g_last.rot.pitch,
                    g_last.rot.yaw, g_last.rot.roll, g_last.rot.pitch * kRotToDeg,
                    g_last.rot.yaw * kRotToDeg, g_last.rot.roll * kRotToDeg);
    }
    ImGui::TextDisabled("snapshot is pre-drive: this is the engine's own view");
}

} // namespace bvr::bsi::camera
