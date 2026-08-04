// Drive behavior (call the original, then adjust the writable out-values;
// publish state through atomics) follows
// itsloopyo/bioshock-remastered-headtracking (MIT), src/engine_hook.rs,
// via the bioshock1r camera module this file is the M3 subset of. The seam
// itself differs from BS1: ProcessEvent filtered to the PlayerCalcView
// UFunction (see patterns.h - BS2 inlined the event dispatch, the thunk is
// dead code).

#include "game/bioshock2r/camera.h"

#include "core/debug/value_scan.h"
#include "core/gfx/frame_inspector.h"
#include "core/gfx/hud_capture.h"
#include "core/input/xinput_bridge.h"
#include "core/ui/overlay.h"
#include "core/util/crash.h"
#include "core/util/log.h"
#include "core/vr/openxr_runtime.h"
#include "game/bioshock2r/aim.h"
#include "game/bioshock2r/bones.h"
#include "game/bioshock2r/frame_context.h"
#include "game/bioshock2r/game_ini.h"
#include "game/bioshock2r/hands.h"
#include "game/bioshock2r/input_drive.h"
#include "game/bioshock2r/scenedraw.h"
#include "game/shared/ue_math.h"

#include <windows.h>
#include <MinHook.h>

#include <imgui.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <share.h>

namespace bvr::b2r::camera {
namespace {

// FVector/FRotator, the rotation-unit constants and the XR->UE conversion all
// live in game/shared/ue_math.h - the engine conventions are identical across
// the two remasters (same build session).

// The inlined dispatch sites build exactly this block and read it back after
// ProcessEvent returns (offline disasm, patterns.h) - so mutating loc/rot
// here AFTER calling the original is equivalent to BS1's out-param writes.
struct CalcViewParams {
    void* viewActor;
    FVector loc;
    FRotator rot;
};
static_assert(sizeof(CalcViewParams) == 0x1C, "param block is 0x1C bytes on this engine");

// Controls: overlay thread writes, game thread reads. All relaxed - x86
// lock-free, and a field arriving one frame late is fine for debug sliders.
std::atomic<float> g_offsetX{0.0f}, g_offsetY{0.0f}, g_offsetZ{0.0f};
// Heartbeat on by default during bring-up - 1 line/s proves per-frame firing
// in every session log. Toggle off with `camlog off` / the overlay.
std::atomic<bool> g_logCamera{true};

// FOV (session 25). The readback claims whatever the game renders (honest
// projection = no fisheye/world-drag). `vrfov` asks for the headset-suggested
// hfov in strict gameplay while the HMD drives; `gfov` is the manual test lever.
//
// SESSION 34 - `vrfov` NOW SHIPS DEFAULT ON, and that is a deliberate override
// of the every-lever-off rule, at the user's explicit instruction ("I want the
// visual space to be the whole screen/FOV"). The measurement behind it: the
// Quest 3 eye is 108 x 110 deg (log: `headset fov half-angles h=54.0 v=55.0`)
// while the game at option 100 renders 100 x 67.7 - so 38% of the eye's height
// was black, top and bottom, and THAT is the "black bars" report. It is not a
// letterbox: every session-33 dump shows the viewport at full backbuffer height
// and `letterbox 1.0000`.
//
// On BS2 the FOV option is the ONLY lever that adds vertical view, because the
// law fixes tanV against a 16:9 reference regardless of aspect - a squarer
// backbuffer only narrows the horizontal. That is the exact opposite of BS1,
// where a square backbuffer was the answer and no FOV write was needed.
std::atomic<int32_t> g_lastOptionFov{0}; // telemetry: 0 = object not located
std::atomic<bool> g_forceHeadsetFov{true};
std::atomic<bool> g_gameFovWrite{false};
// Manual-lever default 130 = BS1 parity (user request, session 25 in-headset
// pass: the headset-derived vrfov wrote 131 on the Quest 3 / VD rig and was
// judged good, so the manual lever defaults to match that neighborhood).
std::atomic<float> g_gameFovDeg{130.0f};

// Resolution write requested by the F10 picker (session 37). The overlay runs
// on the RENDER thread and game_ini::write_viewport does file I/O plus a
// read-back, so the request goes through a pending atomic and is performed on
// the game thread - consumed in ProcessEventDetour's poll gate, which (unlike
// BS1's CalcView-detour consumer) also ticks at the main menu. Packed as one
// 64-bit value (w << 32 | h) so the pair cannot tear.
std::atomic<uint64_t> g_resWritePending{0};
// F10 "APPLY PRESET" button (session 40 round 2): arming touches engine state
// (stereo hook installs, SetUseController) so it is posted here and consumed
// on the poller lane - game thread, outside hooked calls - never on the
// render thread the overlay draws from.
std::atomic<int> g_vrPresetPending{0};

// M4 rung 1 (AlternateEye) + the SR passes share this half-IPD shift. The
// AER eye sign comes from core (vr::current_eye_sign(), 0 while the AER
// checkbox is off), suppressed while SequentialReentry stereo is active.
std::atomic<float> g_ipdMm{63.0f};
// AER flat measure (game thread only): last eyed camera loc per sign, so the
// heartbeat can print the live inter-eye delta - the G0 stereo quantity
// (ipd/1000 x worldScale UU between consecutive frames' cameras).
FVector g_aerLoc[2] = {};
uint64_t g_aerStampMs[2] = {};

// M4 rung 2 (SequentialReentry): the pass-1 dispatch caches the fully-driven
// camera here (post head drive + debug offsets, PRE eye offset) so pass 2
// replays the exact same base with the opposite eye. Game thread only; pass
// 2 always immediately follows its pass 1 inside the same doubled Draw.
// The stamp kills the stale-base hazard (scripted scenes silence CalcView);
// the eyed latch carries pass-1's strict-gameplay decision so a non-gameplay
// pair renders both eyes IDENTICAL (quad-screen content must not jitter).
bool g_srBaseValid = false;
FVector g_srBaseLoc{};
FRotator g_srBaseRot{};
uint64_t g_srBaseStampMs = 0;
bool g_srBaseEyed = false;
// SR flat measure: final per-eye cameras (post eye offset) for the 1 Hz log.
FVector g_srEyeLoc[2] = {};
uint64_t g_srEyeStampMs[2] = {};

// M3 VR camera drive. worldScale default follows BS1's in-headset calibration
// (100 UU/m, session 16) as the starting point - BS2 gets its own verdict
// from the user before anything is persisted.
std::atomic<float> g_worldScale{100.0f}; // Unreal units per meter
std::atomic<float> g_headOffUpUu{0.0f};
std::atomic<float> g_headOffFwdUu{0.0f};
std::atomic<bool> g_recenterRequested{true}; // auto-recenter on first drive
std::atomic<bool> g_vrDriving{false};        // telemetry for the UI
// Head-offset telemetry: the recenter-relative offset applied to loc this
// frame, in UU - makes the world-scale value's effect a number in the log,
// which is what the flat 6DOF check measures.
std::atomic<float> g_headOffX{0.0f}, g_headOffY{0.0f}, g_headOffZ{0.0f};

// Telemetry: game thread writes, overlay thread reads.
std::atomic<uint32_t> g_callCount{0};
std::atomic<float> g_lastLocX{0.0f}, g_lastLocY{0.0f}, g_lastLocZ{0.0f};
std::atomic<int32_t> g_lastPitch{0}, g_lastYaw{0}, g_lastRoll{0};
// Display only - the overlay never dereferences these.
std::atomic<void*> g_playerController{nullptr};
std::atomic<void*> g_lastViewActor{nullptr};
std::atomic<uint32_t> g_lastVtblRva{0}; // observed view-actor vtable RVA
std::atomic<void*> g_calcViewFn{nullptr}; // learned PlayerCalcView UFunction*

// ProcessEvent(UFunction*, void* parms, void* result): __thiscall, ret 0xC.
// __fastcall with a dummy EDX slot is register/stack/cleanup-identical.
using ProcessEventFn = void(__fastcall*)(void* self, void* edx, void* fn, void* parms,
                                         void* result);
// FindFunctionChecked(FName{index,number}, UBOOL global): __thiscall, ret 0xC.
using FindFuncFn = void*(__fastcall*)(void* self, void* edx, uint32_t nameIndex,
                                      uint32_t nameNumber, uint32_t global);
ProcessEventFn g_originalPE = nullptr;
FindFuncFn g_originalFF = nullptr;
void* g_peTarget = nullptr;
const uint8_t* g_fnameIndexGlobal = nullptr;
std::atomic<bool> g_hookLive{false};
std::atomic<bool> g_loggedFirstFire{false};

// Game-thread-only bookkeeping (never touched by the overlay).
uintptr_t g_imageBase = 0;
size_t g_imageSize = 0;
uint64_t g_lastHeartbeatMs = 0;
uint32_t g_heartbeatBaseCount = 0;
// FOV write latch (game thread only): one-shot save of the user's option on
// the ON edge, restored on the OFF edge - the option ini value is the user's
// property and must survive every path out of gameplay.
bool g_wasWritingGameFov = false;
int32_t g_savedGameFov = 0;

// ---- The foreground (viewmodel) lens match, session 33 ---------------------
// BS2 renders TWO lenses. The world tracks the FOV option; the foreground is
// pinned at 60 deg and ignores it, so the viewmodel is displayed at an angular
// gain of tan(option/2)/tan(30) - 2.06x at option 100, and the user's "the
// weapon moves with my head" report. Confirmed in-headset twice by numeric
// prediction (worse at 130, gone at option 60 where the gain is exactly 1).
//
// The lever raises the FOREGROUND to the world's value rather than dropping the
// world to 60: a 60-deg world is unusable in VR, and one projection layer
// carries one FOV claim, so matched lenses is the only state where the wide
// world AND the weapon are both right.
//
// THE ADDRESS IS SETTABLE AT RUNTIME (`fgfov addr <hex>`) because the field is
// still being derived. That is deliberate: it makes the whole live hunt - scan,
// nominate, watch fovaudit's second cluster collapse - a zero-rebuild loop. Once
// the object+offset is derived it becomes a patterns.h constant resolved the
// same way hfov_option_ptr() resolves the settings object, and this manual lane
// stays as the A/B.
//
// It is a LIVE EQUALITY, never a stored tangent: the value written is derived
// from the option READ THIS FRAME. A tangent measured at one aspect or one
// option is exactly the constant that stopped porting on BS1.
std::atomic<uintptr_t> g_fgFovAddr{0};   // nominated candidate field
// DEFAULT ON since 2026-07-31: accepted in-headset, user's words - "I tested
// the match viewmodel lens to the world and it worked, the weapon was not
// moving anymore". The lever stays, and the overlay checkbox is the A/B.
// OPEN, and tracked separately: with the lens matched the first-person rig
// (the Big Daddy helmet) takes much more of the view. That is the fg rig's
// apparent size being coupled to this FOV value, not to its lens alone - a
// distinct defect from the swimming this fixes.
std::atomic<bool> g_fgFovMatch{true};
std::atomic<float> g_fgFovManual{0.0f};  // >0 = write this instead of the option
float g_fgFovSaved = 0.0f;
bool g_wasWritingFgFov = false;
std::atomic<float> g_fgFovLastWritten{0.0f};
// ---- The fg lens LAW GAIN, self-identified (session 37) --------------------
// The fg lens does NOT follow the world's law off 16:9. Measured at aspect
// 0.9348 (2064x2208, three probes 60/100/138): fg tanV = tan(d/2) * 0.99488,
// a constant gain in tan space - while session 33's one-cluster acceptance
// pins the gain at 16:9 to exactly 9/16 (writing the option matched the world
// there). No natural closed form fits both points, so the gain G is
// IDENTIFIED live instead of assumed: every fresh fg sample from the fov
// watch, paired with the value this code last wrote, yields
// G = tanV_fg / tan(dLast/2), and the match writes the inverse,
// d = 2*atan(tanV_world / G). At 16:9 G converges to 9/16 and the write
// reduces to d == option - bit-compatible with the accepted s33 behavior.
// Convergence is one sample (G_meas is independent of the d in effect), and
// the estimator FREEZES exactly when the lenses merge (fov_watch_fg returns
// false once there is no second cluster) - it only measures while there is an
// error signal, and re-identifies by itself after an aspect change.
// Atomic because the overlay displays it; only the game thread writes it.
std::atomic<float> g_fgLawG{9.0f / 16.0f}; // 16:9 identity at init
uint64_t g_fgLastWriteChangeMs = 0;        // game thread only; pairing guard
uint64_t g_lastCalcViewMs = 0;
// Session 32: the ENGINE's own view pitch, sampled BEFORE the drive overwrites
// it, and the error handed to the core pitch servo. These exist because the
// heartbeat below runs LAST and prints the FINAL rot by design - so it reports
// the head's pitch, never the engine's, and could not have shown the engine's
// pitch frozen. Game thread only.
int32_t g_enginePitchUnits = 0;
float g_pitchErrDeg = 0.0f;
bool g_haveRecenter = false;
bvr::vr::HeadPose g_recenterPose{};
// The seated frame's yaw zero, in ROTATOR UNITS (65536/turn), integer for the
// same exactness reasons as BS1 (see bioshock1r/camera.cpp; the M7.5 body
// transfer that motivated it will want the same invariant here eventually).
int32_t g_recenterYawUnits = 0;
float recenter_yaw_rad() { return g_recenterYawUnits / kRotUnitsPerRadian; }

// Synthetic HMD lane, extended over BS1's: `simhead <yaw> <pitch> <roll>
// [px py pz] [holdMs]` feeds a scripted head pose - ROTATION AND POSITION -
// through the real drive, so the full 6DOF xr-to-ue mapping is provable flat
// from the log (BS1's lane was rotation-only). Self-expiring.
struct SimHead {
    float yawDeg = 0.0f, pitchDeg = 0.0f, rollDeg = 0.0f;
    float px = 0.0f, py = 0.0f, pz = 0.0f; // meters, XR local space
    uint64_t deadline = 0;
};
SimHead g_simHead;

uint64_t g_lastCmdPollMs = 0;
FILETIME g_lastCmdWrite{};

// --- gameplay-view predicate -------------------------------------------------
// Strict form (BS1's body.cpp predicate): the view actor's vtable must be
// AShockPlayer's. Deliberately no `viewActor == pc` escape hatch (that hatch
// exists in BS1 only for the aim ray, which does not exist here yet).

bool read_ptr(void* addr, void** out) {
    if (!bvr::pattern_scan::is_memory_valid(addr, sizeof(void*))) return false;
    *out = *static_cast<void**>(addr);
    return true;
}

// Observed vtable RVA of the view actor, 0 if unreadable/foreign. This is the
// runtime verdict on the offline RTTI candidates: logged on every change, so
// a wrong candidate RVA names its own correction from any session log.
uint32_t observed_vtable_rva(void* viewActor) {
    void* vtbl = nullptr;
    if (!viewActor || !read_ptr(viewActor, &vtbl)) return 0;
    uintptr_t v = reinterpret_cast<uintptr_t>(vtbl);
    if (!g_imageBase || v < g_imageBase || v >= g_imageBase + g_imageSize) return 0;
    return static_cast<uint32_t>(v - g_imageBase);
}

bool is_gameplay_view_rva(uint32_t vtblRva) {
    return vtblRva == patterns::kShockPlayerVtableRva;
}

// Write the world's FOV into the nominated foreground-lens field, once per
// CalcView. Save-once / restore-on-disable exactly like the option write above,
// and the same strict-gameplay gate: a scripted camera must never be left with
// our value, which is what restore_game_fov_if_stale covers.
//
// `want` is a FOV IN DEGREES read live, not a tangent. If the two lenses ever
// turn out to use different aspect conventions (BS1's situation, and BS2's
// square-backbuffer dump hints at it), the aspect term is one more factor on
// this line rather than a redesign.
// Where the field lives: the DERIVED offset off the live PlayerController by
// default (patterns::kPcForegroundFovOffset), or a manually nominated address
// when one is set. The manual lane is what derived the offset in the first
// place and stays as the re-derivation path for another build - but nobody has
// to type an address to use the feature.
uintptr_t fg_fov_addr() {
    uintptr_t manual = g_fgFovAddr.load(std::memory_order_relaxed);
    if (manual) return manual;
    void* pc = g_playerController.load(std::memory_order_relaxed);
    if (!pc) return 0;
    return reinterpret_cast<uintptr_t>(pc) + patterns::kPcForegroundFovOffset;
}

void apply_fg_fov_match(const int32_t* optionFov, bool strictGameplay) {
    uintptr_t addr = fg_fov_addr();
    if (!addr) return;
    float* fg = reinterpret_cast<float*>(addr);
    float manual = g_fgFovManual.load(std::memory_order_relaxed);
    bool want = strictGameplay && g_fgFovMatch.load(std::memory_order_relaxed) &&
                (manual > 0.0f || (optionFov && *optionFov > 0));
    if (want) {
        // Probe before trusting the pointer: a nominated address from a scan
        // can be freed between rounds, and a stale one would be a wild write.
        float probe = 0.0f;
        if (!bvr::value_scan::safe_read_f32(addr, &probe)) {
            g_fgFovAddr.store(0, std::memory_order_relaxed);
            g_fgFovMatch.store(false, std::memory_order_relaxed);
            g_wasWritingFgFov = false;
            BVR_LOG("[b2r] fgfov: address 0x%08X went unreadable - disarming",
                    static_cast<unsigned>(addr));
            return;
        }
        // Only ARM on something that already looks like a FOV. If the offset is
        // wrong on some other build this refuses instead of writing into
        // unrelated memory every frame; once armed the check is skipped, since
        // by then the field holds OUR value and would fail its own gate.
        if (!g_wasWritingFgFov &&
            (probe < patterns::kFgFovMinDeg || probe > patterns::kFgFovMaxDeg)) {
            static bool s_warned = false; // game thread only
            if (!s_warned) {
                s_warned = true;
                BVR_LOG("[b2r] fgfov: 0x%08X reads %.3f, not a plausible FOV - REFUSING "
                        "to write. The offset (0x%X off the PlayerController) may not "
                        "hold on this build; re-derive with pcinfo + `fgfov addr`.",
                        static_cast<unsigned>(addr), probe,
                        patterns::kPcForegroundFovOffset);
            }
            return;
        }
        if (!g_wasWritingFgFov) {
            g_fgFovSaved = probe;
            g_wasWritingFgFov = true;
            BVR_LOG("[b2r] fgfov write ON at 0x%08X (saved %.3f)",
                    static_cast<unsigned>(addr), g_fgFovSaved);
        }
        // Identify the fg lens gain from what the watch measured against what
        // we last wrote (see g_fgLawG). The pairing guard skips samples that
        // may predate the last CHANGE of the written value (a 1-2 frame skew
        // during transitions would briefly corrupt G); in steady state the
        // written value is constant and every sample qualifies.
        uint64_t nowMs = GetTickCount64();
        float lastW = g_fgFovLastWritten.load(std::memory_order_relaxed);
        float fgTh = 0.0f, fgTv = 0.0f;
        unsigned long long fgAge = 0;
        float lawG = g_fgLawG.load(std::memory_order_relaxed);
        if (lastW > 10.0f && bvr::hud::fov_watch_fg(&fgTh, &fgTv, &fgAge, 400) &&
            nowMs - fgAge > g_fgLastWriteChangeMs + 100) {
            float meas = fgTv / tanf(lastW * 0.5f / kRadToDeg);
            if (meas > 0.1f && meas < 4.0f && fabsf(meas - lawG) > 0.0005f) {
                if (fabsf(meas - lawG) / lawG > 0.01f)
                    BVR_LOG("[b2r] fgfov: lens gain G %.5f -> %.5f (identified from "
                            "fg tanV %.5f at written %.1f)",
                            lawG, meas, fgTv, lastW);
                lawG = meas;
                g_fgLawG.store(meas, std::memory_order_relaxed);
            }
        }
        float value;
        if (manual > 0.0f) {
            value = manual; // calibration lane: raw degrees, never corrected
        } else {
            // The equality the match exists for: fg tanV == world tanV. The
            // world's is the law (tan(option/2) * 9/16, aspect-invariant);
            // the fg renders tan(d/2) * G, so write the inverse.
            float tanVWorld =
                tanf(static_cast<float>(*optionFov) * 0.5f / kRadToDeg) * (9.0f / 16.0f);
            value = 2.0f * atanf(tanVWorld / lawG) * kRadToDeg;
        }
        if (*fg != value) *fg = value;
        if (value != lastW) g_fgLastWriteChangeMs = nowMs;
        g_fgFovLastWritten.store(value, std::memory_order_relaxed);
    } else if (g_wasWritingFgFov) {
        *fg = g_fgFovSaved;
        g_wasWritingFgFov = false;
        g_fgFovLastWritten.store(0.0f, std::memory_order_relaxed);
        BVR_LOG("[b2r] fgfov write OFF (restored %.3f)", g_fgFovSaved);
    }
}

// BS2's WORLD lens law -> the horizontal half-angle the game is ACTUALLY
// rendering, in degrees. Settled session 33 from frame dumps at two backbuffer
// aspects x two FOV options (derivation in docs/bioshock2/ENGINE_NOTES.md):
//
//     tanV = tan(option/2) * 9/16        <- aspect-INVARIANT, the fixed axis
//     tanH = tanV * (bbW / bbH)          <- horizontal follows the BACKBUFFER
//
// i.e. the option is a 16:9-REFERENCED horizontal, not a true one. Session 32
// measured `tanH = tan(option/2)` and correctly refused to promote it, because
// the two laws coincide exactly at 16:9; the square dumps separate them (Law A
// predicts tanV 1.1918 at 2048x2048, the dump says 0.6704 = tan(50)*9/16, and
// again at option 130: 1.2063 = tan(65)*9/16).
//
// NOTE the horizontal follows the BACKBUFFER aspect even though the scene is
// rendered into a LETTERBOXED viewport - that mismatch is what stretches a
// non-16:9 BS2 render, and it is an engine defect, not something this claim
// should try to correct. We claim what is rendered.
//
// BS1's law is the opposite (a true horizontal). Same engine tree, different
// link: never copy one game's law to the other.
float rendered_hfov_for_option(int32_t optionDeg) {
    if (optionDeg <= 0) return 0.0f;
    unsigned bw = 0, bh = 0;
    if (!bvr::hud::backbuffer_dims(&bw, &bh) || !bw || !bh)
        return static_cast<float>(optionDeg); // pre-first-present: the 16:9 answer
    float tanV = tanf(static_cast<float>(optionDeg) * 0.5f / kRadToDeg) * (9.0f / 16.0f);
    float tanH = tanV * (static_cast<float>(bw) / static_cast<float>(bh));
    return 2.0f * atanf(tanH) * kRadToDeg;
}

// The law RUN BACKWARDS: the option value that makes the game render a given
// true horizontal fov. Needed because the option is NOT a true horizontal on
// BS2 - core asks for a horizontal (suggested_hfov_deg), and writing that
// number straight into the option is only correct at 16:9. Identity at 16:9, so
// nothing shipping moves; correct the moment a second aspect ships. Same shape
// of correction as session 33 made to the CLAIM, for the same reason.
float option_for_rendered_hfov(float hfovDeg) {
    if (hfovDeg <= 0.0f) return 0.0f;
    unsigned bw = 0, bh = 0;
    if (!bvr::hud::backbuffer_dims(&bw, &bh) || !bw || !bh) return hfovDeg;
    float tanH = tanf(hfovDeg * 0.5f / kRadToDeg);
    float tanV = tanH * (static_cast<float>(bh) / static_cast<float>(bw));
    return 2.0f * atanf(tanV * (16.0f / 9.0f)) * kRadToDeg;
}

// The VERTICAL the game renders for an option value. Aspect-INVARIANT on BS2 -
// which is exactly why a squarer backbuffer cannot buy vertical view here (it
// only narrows the horizontal), and why the FOV option is the only lever that
// fills a square headset eye. The opposite of BS1, where aspect was the lever.
float rendered_vfov_for_option(int32_t optionDeg) {
    if (optionDeg <= 0) return 0.0f;
    float tanV = tanf(static_cast<float>(optionDeg) * 0.5f / kRadToDeg) * (9.0f / 16.0f);
    return 2.0f * atanf(tanV) * kRadToDeg;
}

// The option the automatic FOV will write at a GIVEN backbuffer size - the
// picker's preview number for a selected-but-not-yet-applied resolution. This
// deliberately does NOT reuse option_for_rendered_hfov: that helper (and
// suggested_hfov_deg feeding it) reads the LIVE backbuffer, so it would show
// the wrong option for a selection that only lands on the next launch. Same
// circumscription core computes (openxr_runtime.cpp): the symmetric horizontal
// that covers the eye's vertical at this aspect, capped at 160, then the law
// run backwards. Returns 0 without headset geometry.
int32_t auto_option_for_dims(int w, int h) {
    float halfH = 0.0f, halfV = 0.0f;
    if (!bvr::vr::headset_half_fov_deg(&halfH, &halfV) || w <= 0 || h <= 0) return 0;
    float aspect = static_cast<float>(w) / static_cast<float>(h);
    float halfHNeed = fmaxf(halfH / kRadToDeg,
                            atanf(tanf(halfV / kRadToDeg) * aspect));
    float sugDeg = fminf(halfHNeed * 2.0f * kRadToDeg, 160.0f);
    float tanH = tanf(sugDeg * 0.5f / kRadToDeg);
    float tanV = tanH * (static_cast<float>(h) / static_cast<float>(w));
    float opt = 2.0f * atanf(tanV * (16.0f / 9.0f)) * kRadToDeg;
    return static_cast<int32_t>(opt + 0.5f);
}

// ---- Resolution presets (session 37) ---------------------------------------
// One table drives BOTH the `vrres <name>` command and the F10 picker, BS1's
// dropdown shape with BS2's numbers. The entries are STATIC, measured-in
// values - never derived from the headset at runtime - so a preset means the
// same pixels on every rig and every boot. The 16:9 ladder is the shipped
// baseline; squarer rungs are added only as the session-37 aspect bisection
// proves BS2 renders them full-height (the engine letterboxes somewhere
// between 16:9 and square: 2048x2048 renders a 2048x1421 scene).
struct ResMode {
    const char* cmdName; // `vrres <cmdName>`; nullptr = overlay-only entry
    const char* label;   // overlay combo text
    int w, h;
};
const ResMode kResModes[] = {
    {"flat", "1920 x 1080  (16:9, 2.1 MPx, flat play)", 1920, 1080},
    {"perf", "1648 x 1768  (2.9 MPx, performance)", 1648, 1768},
    {"native", "2064 x 2208  (4.6 MPx, Quest 3 native)", 2064, 2208},
    {"sharp", "2480 x 2648  (6.6 MPx, sharper)", 2480, 2648},
    {"max", "2888 x 3088  (8.9 MPx, very demanding)", 2888, 3088},
};

// ---- Live window enforcement (session 37) ----------------------------------
// THE LETTERBOX ROOT CAUSE, measured live this session: the engine sizes its
// scene viewport to the window CLIENT area while the backbuffer holds the ini
// size, and its own window carries chrome plus a size clamp - on a 1440p
// desktop the client tops out at 1421 rows, so every taller configuration
// rendered letterboxed and anamorphic. 2048x2048 -> a 2048x1421 scene:
// sessions 32-33's mystery ratio 1.4413 was never an engine law, it was
// window arithmetic (2048 wide / 1421 visible rows).
//
// The fix is a window the engine cannot lose rows to: strip the chrome
// (borderless popup) and size the client EXACTLY to the render size - beyond
// the desktop if need be, which a popup is allowed to do. The engine follows
// a client resize with its own ResizeBuffers (backbuffer == client == scene
// viewport, letterbox 1.0000, square pixels at every aspect tried: 1.778,
// 1.6, 0.9348, 0.9321), the XR swapchain rebuilds, and the auto FOV and the
// claim recompute per the law. So resolution on BS2 is LIVE - the picker
// resizes the window and the engine does the rest, no relaunch. (BS1 cannot
// do this; its lane stays ini + relaunch. Do not port either way.)
HWND g_gameWindow = nullptr;      // game thread only; revalidated before use
LONG g_savedWindowStyle = 0;      // original chrome, for `vrres restore`
RECT g_savedWindowRect = {};      // original rect, for `vrres restore`
bool g_windowSaved = false;       // game thread only
std::atomic<bool> g_windowRestorePending{false}; // overlay -> game thread
// After an apply: re-verify the ini once the engine settles, because the
// engine PERSISTS ITS LIVE SIZE INTO Shared.ini ON RESIZE, one step behind
// (measured: it recorded the previous size mid-transition). Game thread only.
uint64_t g_resConfirmVal = 0;
uint64_t g_resConfirmDueMs = 0;
// The self-heal must not fight a just-applied resize while the engine's
// ResizeBuffers is still in flight. Game thread only.
uint64_t g_resHealHoldUntilMs = 0;

BOOL CALLBACK find_game_window_cb(HWND h, LPARAM param) {
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid != GetCurrentProcessId() || !IsWindowVisible(h)) return TRUE;
    RECT c{};
    if (!GetClientRect(h, &c) || c.right < 320 || c.bottom < 200) return TRUE;
    *reinterpret_cast<HWND*>(param) = h;
    return FALSE;
}

HWND game_window() {
    if (g_gameWindow && IsWindow(g_gameWindow)) return g_gameWindow;
    g_gameWindow = nullptr;
    EnumWindows(find_game_window_cb, reinterpret_cast<LPARAM>(&g_gameWindow));
    return g_gameWindow;
}

// Make the window's CLIENT area exactly w x h, chrome-free, top-left pinned
// so the F10 overlay stays on the visible desktop while any excess hangs off
// the bottom. Saves the original style/rect once for `vrres restore`.
bool enforce_client_size(uint32_t w, uint32_t h) {
    HWND wnd = game_window();
    if (!wnd) {
        BVR_LOG("[b2r] resolution: game window not found - cannot resize");
        return false;
    }
    if (!g_windowSaved) {
        g_savedWindowStyle = GetWindowLongW(wnd, GWL_STYLE);
        GetWindowRect(wnd, &g_savedWindowRect);
        g_windowSaved = true;
    }
    SetWindowLongW(wnd, GWL_STYLE,
                   WS_POPUP | WS_VISIBLE | WS_CLIPSIBLINGS | WS_CLIPCHILDREN);
    if (!SetWindowPos(wnd, nullptr, 0, 0, static_cast<int>(w), static_cast<int>(h),
                      SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED)) {
        BVR_LOG("[b2r] resolution: SetWindowPos %ux%u failed (err %lu)", w, h,
                GetLastError());
        return false;
    }
    RECT c{};
    GetClientRect(wnd, &c);
    BVR_LOG("[b2r] resolution: window client now %ldx%ld (asked %ux%u, borderless) - "
            "the engine follows with its own ResizeBuffers",
            c.right, c.bottom, w, h);
    return true;
}

void restore_game_window() {
    HWND wnd = game_window();
    if (!wnd || !g_windowSaved) return;
    // Restore the CHROME, but size the client for the CURRENT backbuffer
    // rather than the remembered rect: the remembered rect encodes whatever
    // clamped size the boot happened to have, and re-applying it drags the
    // engine's follow-the-client resize to a stale resolution (found live -
    // restore after `vrres flat` left the render at 2064x1421). Only the
    // saved top-left position is reused.
    SetWindowLongW(wnd, GWL_STYLE, g_savedWindowStyle);
    unsigned bw = 0, bh = 0;
    RECT r{0, 0, 1920, 1080};
    if (bvr::hud::backbuffer_dims(&bw, &bh) && bw && bh) {
        r.right = static_cast<LONG>(bw);
        r.bottom = static_cast<LONG>(bh);
    }
    AdjustWindowRect(&r, static_cast<DWORD>(g_savedWindowStyle), FALSE);
    SetWindowPos(wnd, nullptr, g_savedWindowRect.left, g_savedWindowRect.top,
                 r.right - r.left, r.bottom - r.top,
                 SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    BVR_LOG("[b2r] resolution: window chrome restored (client sized for %ux%u)", bw,
            bh);
}

// One resolution-apply path for the command, the picker and the self-heal:
// resize FIRST (the engine follows and then writes its own lagging value into
// Shared.ini), persist SECOND, re-verify THIRD after the dust settles.
void apply_resolution(uint32_t w, uint32_t h) {
    enforce_client_size(w, h);
    game_ini::write_viewport(w, h);
    g_resConfirmVal = (static_cast<uint64_t>(w) << 32) | h;
    g_resConfirmDueMs = GetTickCount64() + 4000;
    g_resHealHoldUntilMs = GetTickCount64() + 6000;
}

// Half-IPD shift along view-right of `rot`, sign -1 = left eye. Shared by the
// AER path now and both SequentialReentry passes later - ONE implementation,
// exactly like BS1 (its session-22 lesson: a yaw-only right axis keeps the
// virtual eyes horizontal while real eyes stack vertically under head roll;
// ue_rot_basis's full-rotation right row fixes that, and at roll 0 it reduces
// bit-identically to the yaw-only formula).
void apply_eye_offset(FVector* loc, const FRotator& rot, int sign) {
    float fwd[3], right[3], up[3];
    ue_rot_basis(rot, fwd, right, up);
    float halfIpdUu = static_cast<float>(sign) *
                      (g_ipdMm.load(std::memory_order_relaxed) / 2000.0f) *
                      g_worldScale.load(std::memory_order_relaxed);
    loc->x += right[0] * halfIpdUu;
    loc->y += right[1] * halfIpdUu;
    loc->z += right[2] * halfIpdUu;
}

// --- command seam ------------------------------------------------------------
// <data_dir>\command.txt (= %LOCALAPPDATA%\BioshockVR\bs2\command.txt) polled
// at 1 Hz on the game thread, same contract as BS1's seam - but driven from
// the ProcessEvent detour, so it works at the main menu too (BS2's menu never
// runs PlayerCalcView, unlike BS1's attract scene). M3 vocabulary:
//   recenter                     re-reference the seated pose
//   offset <x> <y> <z>           debug camera offset in UU (0 0 0 clears)
//   worldscale <v>               UU per meter
//   headoff <up> <fwd>           head-anchor offset in UU
//   simhead <yaw> <pitch> <roll> [px py pz] [holdMs] | simhead off
//   vrcam on|off                 VR enable + camera mode (core funnels)
//   camlog on|off                1 Hz heartbeat
//   vroverlay on|off             core overlay visibility (bring-up A/B)
//   vrcine <args>                core cinematic-fallback A/B (vrcine status..)
//   vrinput <args>               core input surface (session 32): synthetic
//                                gamepad `test stick|trig|press|clear`,
//                                `pitchkill`, `pitchservo on|off|invert|status`,
//                                `swing ...` - all core, all previously
//                                unreachable on BS2 for want of this branch
// FOV (session 25; both write levers DEFAULT OFF):
//   vrfov on|off|status          forced headset FOV write (strict gameplay +
//                                HMD driving only; save/restore of the option)
//   gfov <deg>|off               manual game FOV write (flat test lever)
//   fovaudit                     option vs submitted claim vs rendered fov,
//                                plus both lenses, the letterbox factor and
//                                the square-pixels/stretched verdict
//   fgfov addr <hex>|on|off|<deg>|status
//                                session 33: the FOREGROUND (viewmodel) lens
//                                match. BS2 pins the fg lens at 60 deg while
//                                the world tracks the option, so the weapon is
//                                displayed at tan(option/2)/tan(30) angular
//                                gain. `on` writes the LIVE option into the
//                                nominated field every CalcView; `<deg>` is the
//                                manual calibration lane. DEFAULT OFF. The
//                                address is settable because the field is being
//                                derived - that makes the hunt zero-rebuild.
//   pcinfo                       live PC / view actor / settings pointers, and
//                                a 55-65 float sweep of both objects - the
//                                first move of the fg-field hunt
// Core features that b2r simply never dispatched to (session 32 found the
// first, session 33 the rest - core growing a feature does NOT give an adapter
// access to it when the adapter owns the command table):
//   vrpace <args>                M8 stall guard
//   vrmirror <args>              M8 desktop mirror
//   vrhud on|off|status          HUD capture control + the lens/letterbox state
//   vrpreset [save]              arm the full VR configuration / persist the
//                                tuned sliders to this game's own
//                                vrpreset.ini. b2r had NO persistence at all
//                                before session 33, so every in-headset
//                                verdict had to be re-tuned by hand after a
//                                relaunch - which makes a verdict unrepeatable
// Resolution (session 32; the eye render IS the backbuffer):
//   vrres <w>x<h> | <w> <h>      write the game's own viewport keys (next
//                                launch); bare `vrres` reports current vs live
// Discovery commands (route to core/debug/value_scan; game thread only),
// ported from BS1's dispatcher for the session-25 FOV derivation - the
// duplicate-now seam policy applies (see the ARCHITECTURE decision log):
//   memscan <f>  memrescan <f>  memlist [n]  memread <idx>
//   memscani <u>  memrescani <u>   (integer-typed variants)
//   mempoke <idx> <f>  mempoke <lo>-<hi> <f>  mempokei ...same with <u>
//   memrestore  memptr <idx> [maxDeltaHex]
//   pokeaddr <hex> <f>  pokeaddri <hex> <u>  hexdump <hex> <len>
//   fsweep <hexaddr> <len> <lo> <hi>  strscan <text>  membases
//   dumpframe [full] [n]
//   vtscan <hexRva> [needBytesHex]  (b2r-first: one-shot heap scan for live
//   objects whose dword0 == base + RVA - the candidate-vtable verifier)
// Stereo / render substrate (session 26; scenedraw.h has the grammar):
//   vrstereo on|off              ONE toggle: camera mode + stereo (threaded
//                                substrate - BS2's primary bet, no 1t rung)
//   reentry vrstereo|stereo|pulse|on|off|yaw|reset|hook [draw|stream]|
//           unhook|dump <n>|kick on|off|kick2 on|off|calcstack|status
// Aim (session 39; aim.h has the grammar):
//   vraim status | on|off | seam weapon|ability on|off |
//         test r <yaw> <pitch> [ms]|off | probe on|off|clear|dump
//                                the GetPerfectFireStart impl seam (both
//                                variants hooked, telemetry always; `on` arms
//                                substitution, `test` feeds a synthetic ray
//                                as an offset from the live view rot - the
//                                decal-proof lane) plus the dispatch probe
//                                (fire-watch + GNames census) that settled
//                                the by-name-vs-impl question.
//   vrhands on|off|status|trim <p> <y> <r>|offset <f> <r> <u>|pose aim|grip
//                                the rig rides the RIGHT controller through
//                                the same frame context the ray uses
//   vrbones status|cluster <lo> <hi> <anchor>|refcap|release
//                                the bone-drive mechanism's own levers

void save_vr_preset();
void apply_vr_preset();

void apply_command(const char* cmd, const char* args) {
    float v = 0.0f, x = 0.0f, y = 0.0f, z = 0.0f;
    unsigned lo = 0, hi = 0, n = 0;
    unsigned addr = 0, len = 0;

    if (strcmp(cmd, "recenter") == 0) {
        g_recenterRequested.store(true, std::memory_order_relaxed);
        BVR_LOG("[b2r] command: recenter");
    } else if (strcmp(cmd, "offset") == 0) {
        if (sscanf_s(args, "%f %f %f", &x, &y, &z) == 3) {
            g_offsetX.store(x, std::memory_order_relaxed);
            g_offsetY.store(y, std::memory_order_relaxed);
            g_offsetZ.store(z, std::memory_order_relaxed);
            BVR_LOG("[b2r] command: offset %.1f %.1f %.1f", x, y, z);
        }
    } else if (strcmp(cmd, "worldscale") == 0) {
        if (sscanf_s(args, "%f", &x) == 1 && x > 0.0f) {
            g_worldScale.store(x, std::memory_order_relaxed);
            BVR_LOG("[b2r] command: worldscale %.1f", x);
        }
    } else if (strcmp(cmd, "headoff") == 0) {
        if (sscanf_s(args, "%f %f", &x, &y) == 2) {
            g_headOffUpUu.store(x, std::memory_order_relaxed);
            g_headOffFwdUu.store(y, std::memory_order_relaxed);
            BVR_LOG("[b2r] command: headoff up %.1f fwd %.1f", x, y);
        }
    } else if (strcmp(cmd, "simhead") == 0) {
        if (strncmp(args, "off", 3) == 0) {
            g_simHead.deadline = 0;
            BVR_LOG("[b2r] command: simhead off");
        } else {
            // 3 args = angles; 4 = angles + holdMs (BS1-compatible); 6 =
            // angles + position; 7 = angles + position + holdMs.
            float v[7] = {};
            int n = sscanf_s(args, "%f %f %f %f %f %f %f", &v[0], &v[1], &v[2], &v[3],
                             &v[4], &v[5], &v[6]);
            if (n == 3 || n == 4 || n == 6 || n == 7) {
                bool wasIdle = GetTickCount64() >= g_simHead.deadline;
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
                BVR_LOG("[b2r] command: simhead yaw %.1f pitch %.1f roll %.1f "
                        "pos (%.2f %.2f %.2f) for %d ms%s",
                        v[0], v[1], v[2], g_simHead.px, g_simHead.py, g_simHead.pz, hold,
                        wasIdle ? " (recentering onto first sim pose)" : "");
            } else {
                BVR_LOG("[b2r] usage: simhead <yaw> <pitch> <roll> [px py pz] [holdMs] | "
                        "simhead off");
            }
        }
    } else if (strcmp(cmd, "vrcam") == 0) {
        // SESSION 33: `off` now disables VR as well as the camera mode. The
        // asymmetry was a trap with no escape hatch: `on` calls set_enabled(),
        // `off` did not, so once a session was running the game stayed PACED BY
        // THE HEADSET forever. With the headset idle the runtime paces its
        // not-visible cadence - measured ~10 fps, with the pace thread showing
        // lastWait 0 ms and timeouts 0, i.e. not blocked, just throttled - and
        // nothing in the command surface could undo it. That is what read as
        // "the game hangs a few seconds after turning on VR stereo".
        bool on = strncmp(args, "off", 3) != 0;
        bvr::vr::set_enabled(on);
        bvr::vr::set_camera_mode(on);
        BVR_LOG("[b2r] command: vrcam %s", on ? "on (VR enabled + camera mode)"
                                              : "off (VR DISABLED + camera mode off - "
                                                "the game stops being paced by the headset)");
    } else if (strcmp(cmd, "camlog") == 0) {
        g_logCamera.store(strncmp(args, "off", 3) != 0, std::memory_order_relaxed);
        BVR_LOG("[b2r] command: camlog %s", strncmp(args, "off", 3) != 0 ? "on" : "off");
    } else if (strcmp(cmd, "vroverlay") == 0) {
        bvr::overlay::set_visible(strncmp(args, "off", 3) != 0);
        BVR_LOG("[b2r] command: vroverlay %s", strncmp(args, "off", 3) != 0 ? "on" : "off");
    } else if (strcmp(cmd, "vrcine") == 0) {
        bvr::vr::handle_cine_command(args); // core detector A/B on a new game
    } else if (strcmp(cmd, "vrfov") == 0) {
        // Forced headset FOV, DEFAULT OFF: in strict gameplay while the HMD
        // drives, write the headset-suggested hfov into the game option so
        // the image fills the headset. Off restores on the next CalcView.
        if (strncmp(args, "status", 6) == 0) {
            BVR_LOG("[b2r] vrfov status: force=%s suggested=%.1f option=%d "
                    "writing=%d savedOption=%d",
                    g_forceHeadsetFov.load(std::memory_order_relaxed) ? "on" : "off",
                    bvr::vr::suggested_hfov_deg(),
                    g_lastOptionFov.load(std::memory_order_relaxed),
                    g_wasWritingGameFov ? 1 : 0, g_savedGameFov);
        } else {
            bool on = strncmp(args, "off", 3) != 0;
            g_forceHeadsetFov.store(on, std::memory_order_relaxed);
            BVR_LOG("[b2r] command: vrfov %s (suggested headset hfov %.1f deg; engages "
                    "in strict gameplay while the HMD drives)",
                    on ? "on" : "off", bvr::vr::suggested_hfov_deg());
        }
    } else if (strcmp(cmd, "gfov") == 0) {
        // Manual game-option FOV write (flat test lever + clamp probe).
        if (strncmp(args, "off", 3) == 0) {
            g_gameFovWrite.store(false, std::memory_order_relaxed);
            BVR_LOG("[b2r] command: gfov off");
        } else if (sscanf_s(args, "%f", &v) == 1 && v > 0.0f) {
            g_gameFovDeg.store(v, std::memory_order_relaxed);
            g_gameFovWrite.store(true, std::memory_order_relaxed);
            BVR_LOG("[b2r] command: gfov %.1f (writes in strict gameplay)", v);
        } else {
            BVR_LOG("[b2r] usage: gfov <deg> | gfov off");
        }
    } else if (strcmp(cmd, "fgfov") == 0) {
        // The foreground/viewmodel lens match (session 33). The field address
        // is settable while it is being derived, so the whole live hunt is a
        // zero-rebuild loop: nominate a candidate, arm, and watch whether
        // fovaudit's second cluster collapses onto the world's.
        unsigned addr = 0;
        if (strncmp(args, "addr", 4) == 0) {
            if (sscanf_s(args + 4, "%x", &addr) == 1) {
                // Restore the OLD address HERE, synchronously, before
                // repointing. Deferring it to the next CalcView (which is what
                // clearing the match flag would do) writes the saved value to
                // the NEW address instead - so the previous candidate keeps our
                // value forever. That defect made a six-candidate sweep report
                // a hit on ALL SIX: the first write was never undone, so every
                // later reading was measuring candidate 1 still being held.
                if (g_wasWritingFgFov) {
                    uintptr_t old = g_fgFovAddr.load(std::memory_order_relaxed);
                    float probe = 0.0f;
                    if (old && bvr::value_scan::safe_read_f32(old, &probe)) {
                        *reinterpret_cast<float*>(old) = g_fgFovSaved;
                        BVR_LOG("[b2r] fgfov: restored 0x%08X to %.3f before repointing",
                                static_cast<unsigned>(old), g_fgFovSaved);
                    }
                    g_wasWritingFgFov = false;
                    g_fgFovMatch.store(false, std::memory_order_relaxed);
                }
                g_fgFovAddr.store(addr, std::memory_order_relaxed);
                float cur = 0.0f;
                bool ok = bvr::value_scan::safe_read_f32(addr, &cur);
                BVR_LOG("[b2r] command: fgfov addr 0x%08X (currently %s%.4f) - "
                        "arm with `fgfov on`",
                        addr, ok ? "" : "UNREADABLE ", ok ? cur : 0.0f);
            } else {
                BVR_LOG("[b2r] usage: fgfov addr <hexaddr>");
            }
        } else if (strncmp(args, "status", 6) == 0) {
            uintptr_t a = fg_fov_addr();
            float cur = 0.0f;
            bool ok = a && bvr::value_scan::safe_read_f32(a, &cur);
            BVR_LOG("[b2r] fgfov status: match=%s addr=0x%08X current=%s%.4f "
                    "manual=%.1f writing=%d saved=%.4f lastWritten=%.1f option=%d "
                    "lawG=%.5f",
                    g_fgFovMatch.load(std::memory_order_relaxed) ? "on" : "off",
                    static_cast<unsigned>(a), ok ? "" : "UNREADABLE ", ok ? cur : 0.0f,
                    g_fgFovManual.load(std::memory_order_relaxed),
                    g_wasWritingFgFov ? 1 : 0, g_fgFovSaved,
                    g_fgFovLastWritten.load(std::memory_order_relaxed),
                    g_lastOptionFov.load(std::memory_order_relaxed),
                    g_fgLawG.load(std::memory_order_relaxed));
        } else if (strncmp(args, "off", 3) == 0) {
            g_fgFovMatch.store(false, std::memory_order_relaxed);
            BVR_LOG("[b2r] command: fgfov off (restores on the next CalcView)");
        } else if (strncmp(args, "on", 2) == 0) {
            g_fgFovManual.store(0.0f, std::memory_order_relaxed);
            g_fgFovMatch.store(true, std::memory_order_relaxed);
            BVR_LOG("[b2r] command: fgfov on - matching the LIVE world option "
                    "(addr 0x%08X, %s)",
                    static_cast<unsigned>(fg_fov_addr()),
                    g_fgFovAddr.load(std::memory_order_relaxed) ? "manual"
                                                                : "derived off the PC");
        } else if (sscanf_s(args, "%f", &v) == 1 && v > 0.0f) {
            // Manual degrees: the calibration lane. Poke 2-3 values and read
            // the resulting fg tangent out of `fovaudit` to MEASURE the
            // field's law instead of assuming it is degrees-in-degrees-out.
            g_fgFovManual.store(v, std::memory_order_relaxed);
            g_fgFovMatch.store(true, std::memory_order_relaxed);
            BVR_LOG("[b2r] command: fgfov %.1f (manual - overrides the option match)", v);
        } else {
            BVR_LOG("[b2r] usage: fgfov addr <hex> | on | off | <deg> | status");
        }
    } else if (strcmp(cmd, "pcinfo") == 0) {
        // The anchors a live field hunt needs, without digging a pointer out
        // of a one-shot log line from an hour ago.
        void* pc = g_playerController.load(std::memory_order_relaxed);
        void* va = g_lastViewActor.load(std::memory_order_relaxed);
        BVR_LOG("[b2r] pcinfo: playerController=%p viewActor=%p vtblRva=0x%X "
                "settings=%p option=%d | fsweep those for a 60.0 float, then "
                "`fgfov addr <hit>` + `fgfov on` + `fovaudit`",
                pc, va, g_lastVtblRva.load(std::memory_order_relaxed),
                patterns::hfov_option_ptr(),
                g_lastOptionFov.load(std::memory_order_relaxed));
        if (pc) bvr::value_scan::float_sweep(reinterpret_cast<uintptr_t>(pc), 0x1000,
                                             55.0f, 65.0f);
        if (va && va != pc)
            bvr::value_scan::float_sweep(reinterpret_cast<uintptr_t>(va), 0x1000, 55.0f,
                                         65.0f);
    } else if (strcmp(cmd, "fovaudit") == 0) {
        // The three fov truths side by side (BS1 session-21 instrument, minus
        // the pose/eyes stereo sub-forms BS2 does not have yet): the engine
        // option, what the runtime last tagged the projection layer with, and
        // the option-derived expectation at the swap aspect. The RENDERED
        // side comes from the core fov watch when it decodes, else from
        // `dumpframe full 2` + tools/decode-framedump.ps1.
        int32_t* opt = patterns::hfov_option_ptr();
        float tanH = 0.0f, tanV = 0.0f;
        int src = -1;
        unsigned sw = 0, sh = 0;
        bvr::vr::fov_audit(&tanH, &tanV, &src, &sw, &sh);
        // Option-derived expectation. Session 32: the old fallback assumed 16:9
        // whenever there was no XR session - i.e. always, flat, which is where
        // the measuring happens, and wrong at every aspect but one. Use the
        // real backbuffer (published every present, headset or not) and fall
        // back to the swap dims, not to a constant. BS1 made the same fix in
        // session 28 for the same reason.
        float optTanH = 0.0f, optTanV = 0.0f;
        unsigned bw = 0, bh = 0;
        bvr::hud::backbuffer_dims(&bw, &bh);
        if (!bw || !bh) {
            bw = sw;
            bh = sh;
        }
        if (opt) {
            // BS2's world law, SETTLED session 33 against two aspects x two FOV
            // options (see ENGINE_NOTES): the option fixes the VERTICAL through
            // a 16:9 reference, and the horizontal follows the BACKBUFFER
            // aspect. Session 32 could not tell this from "tanH = tan(opt/2)"
            // because the two laws coincide exactly at 16:9; the square dumps
            // separate them (Law A predicts tanV 1.1918 there, measured 0.6704).
            //   tanV = tan(opt/2) * 9/16          <- aspect-INVARIANT
            //   tanH = tanV * (bbW / bbH)
            // NOTE this is the OPPOSITE of BS1's law, which is a true horizontal
            // - same engine tree, different link. Never copy the other game's.
            optTanV = tanf(static_cast<float>(*opt) * 0.5f / kRadToDeg) * (9.0f / 16.0f);
            optTanH = optTanV * ((bw && bh) ? (static_cast<float>(bw) / static_cast<float>(bh))
                                            : 0.0f);
        }
        BVR_LOG("[b2r] fovaudit: option=%d gfovWrite=%s(%.1f) vrfov=%s | submitted "
                "tanH=%.6f tanV=%.6f src=%s swap=%ux%u | option-derived tanH=%.6f "
                "tanV=%.6f",
                opt ? *opt : -1,
                g_gameFovWrite.load(std::memory_order_relaxed) ? "on" : "off",
                g_gameFovDeg.load(std::memory_order_relaxed),
                g_forceHeadsetFov.load(std::memory_order_relaxed) ? "on" : "off", tanH,
                tanV,
                src == 0   ? "readback"
                : src == 1 ? "fallback"
                : src == 2 ? "manual"
                : src == 3 ? "live"
                           : "none",
                sw, sh, optTanH, optTanV);
        // Session 28 (core change, shared): both lenses, age labelled in words.
        // Session 33 adds the LETTERBOX factor and the two aspects it compares:
        // the frustum's own (tanH/tanV) against the scene VIEWPORT's. When the
        // engine letterboxes, those two disagree and the render is stretched -
        // which is a different defect from the black band and the one that
        // decides whether an aspect is usable at all. `lenses=1` is the
        // viewmodel-match gate; `stretch` is the resolution gate.
        float liveTanH = 0.0f, liveTanV = 0.0f;
        unsigned long long liveAge = 0;
        if (bvr::hud::fov_watch(&liveTanH, &liveTanV, &liveAge, 0)) {
            float fgH = 0.0f, fgV = 0.0f;
            unsigned long long fgAge = 0;
            bool haveFg = bvr::hud::fov_watch_fg(&fgH, &fgV, &fgAge, 0);
            float lb = bvr::hud::fov_vp_ratio();
            // The scene viewport is the backbuffer scaled down by the letterbox
            // factor vertically - so its aspect is bbW/(bbH/lb).
            float vpAr = (bw && bh) ? (static_cast<float>(bw) * lb / static_cast<float>(bh))
                                    : 0.0f;
            float frustumAr = liveTanV > 0.0f ? liveTanH / liveTanV : 0.0f;
            BVR_LOG("[b2r] fovaudit live: WORLD tanH=%.6f tanV=%.6f (%.2f deg) "
                    "age=%llums %s | 2nd-lens tanH=%.6f tanV=%.6f age=%llums | "
                    "lenses=%d mismatch=%d cineActive=%d | letterbox=%.4f "
                    "vpAspect=%.4f frustumAspect=%.4f -> %s",
                    liveTanH, liveTanV, 2.0f * atanf(liveTanH) * kRadToDeg, liveAge,
                    liveAge <= 500 ? "FRESH" : "STALE - DO NOT CONCLUDE",
                    haveFg ? fgH : 0.0f, haveFg ? fgV : 0.0f, fgAge,
                    bvr::hud::fov_lens_count(),
                    bvr::hud::fov_mismatch() ? 1 : 0,
                    bvr::vr::cinematic_active() ? 1 : 0, lb, vpAr, frustumAr,
                    fabsf(frustumAr - vpAr) < 0.005f ? "square pixels"
                                                     : "STRETCHED (anamorphic)");
        } else {
            BVR_LOG("[b2r] fovaudit live: no decoded scene tangents yet");
        }
        // The raw slots, always - this is the line that says whether the fg
        // lens was SAMPLED, which `lenses` cannot. The fg pass is a short
        // contiguous run at the head of the pass (~19 buffers of 600+), so
        // before the head-slot reservation a round could easily miss it and
        // report lenses=1 with nothing having changed.
        {
            float sh[16] = {}, sv[16] = {};
            int n = bvr::hud::fov_slots(sh, sv, 16);
            char buf[256];
            int off = 0;
            for (int i = 0; i < n && off < 200; ++i)
                off += sprintf_s(buf + off, sizeof(buf) - off, "%s%.4f", i ? " " : "", sh[i]);
            if (!n) sprintf_s(buf, "(none)");
            BVR_LOG("[b2r] fovaudit slots: %d decoded | tanH: %s", n, buf);
        }
    } else if (strcmp(cmd, "fsweep") == 0) {
        float flo = 0.0f, fhi = 0.0f;
        if (sscanf_s(args, "%x %u %f %f", &addr, &len, &flo, &fhi) == 4)
            bvr::value_scan::float_sweep(addr, len, flo, fhi);
        else
            BVR_LOG("[b2r] usage: fsweep <hexaddr> <len> <lo> <hi>");
    } else if (strcmp(cmd, "memscan") == 0) {
        if (sscanf_s(args, "%f", &v) == 1) bvr::value_scan::scan_f32(v);
    } else if (strcmp(cmd, "memrescan") == 0) {
        if (sscanf_s(args, "%f", &v) == 1) bvr::value_scan::rescan_f32(v);
    } else if (strcmp(cmd, "memscani") == 0) {
        if (sscanf_s(args, "%u", &n) == 1) bvr::value_scan::scan_u32(n);
    } else if (strcmp(cmd, "memrescani") == 0) {
        if (sscanf_s(args, "%u", &n) == 1) bvr::value_scan::rescan_u32(n);
    } else if (strcmp(cmd, "memlist") == 0) {
        bvr::value_scan::list(sscanf_s(args, "%u", &n) == 1 ? n : 32);
    } else if (strcmp(cmd, "memread") == 0) {
        if (sscanf_s(args, "%u", &n) == 1) bvr::value_scan::read_at(n);
    } else if (strcmp(cmd, "mempoke") == 0) {
        if (sscanf_s(args, "%u-%u %f", &lo, &hi, &v) == 3)
            bvr::value_scan::poke_range(lo, hi, v);
        else if (sscanf_s(args, "%u %f", &n, &v) == 2)
            bvr::value_scan::poke(n, v);
    } else if (strcmp(cmd, "mempokei") == 0) {
        unsigned iv = 0;
        if (sscanf_s(args, "%u-%u %u", &lo, &hi, &iv) == 3)
            bvr::value_scan::poke_range_u32(lo, hi, iv);
        else if (sscanf_s(args, "%u %u", &n, &iv) == 2)
            bvr::value_scan::poke_u32(n, iv);
    } else if (strcmp(cmd, "memrestore") == 0) {
        bvr::value_scan::restore_all();
    } else if (strcmp(cmd, "memptr") == 0) {
        unsigned maxDelta = 0x400;
        if (sscanf_s(args, "%u %x", &n, &maxDelta) >= 1)
            bvr::value_scan::ptr_scan(n, maxDelta);
    } else if (strcmp(cmd, "pokeaddr") == 0) {
        if (sscanf_s(args, "%x %f", &addr, &v) == 2)
            bvr::value_scan::poke_addr(addr, v);
    } else if (strcmp(cmd, "pokeaddri") == 0) {
        unsigned iv = 0;
        if (sscanf_s(args, "%x %u", &addr, &iv) == 2)
            bvr::value_scan::poke_addr_u32(addr, iv);
    } else if (strcmp(cmd, "hexdump") == 0) {
        if (sscanf_s(args, "%x %u", &addr, &len) >= 1)
            bvr::value_scan::hexdump(addr, len ? len : 64);
    } else if (strcmp(cmd, "strscan") == 0) {
        char text[96];
        if (sscanf_s(args, "%95s", text, static_cast<unsigned>(sizeof text)) == 1)
            bvr::value_scan::log_string_scan(text);
    } else if (strcmp(cmd, "membases") == 0) {
        bvr::value_scan::log_module_bases();
    } else if (strcmp(cmd, "dumpframe") == 0) {
        // dumpframe [full] [n] - n > 1 records consecutive present windows
        // (files suffixed _qN). Same core frame inspector as BS1; the dump
        // lands in this game's data dir via log::data_dir().
        bool full = strncmp(args, "full", 4) == 0;
        int count = 1;
        sscanf_s(full ? args + 4 : args, " %d", &count);
        bvr::frame_inspector::arm(full ? 2 : 1, count);
    } else if (strcmp(cmd, "vtscan") == 0) {
        // vtscan <hexRva> [needBytesHex] - one-shot candidate-vtable verifier:
        // logs every live object whose dword0 == base + RVA. The accept
        // callback never chooses, so the census covers ALL matches; the
        // summary's chosen=00000000 is expected. EXPENSIVE (full 4 GB walk) -
        // probe use only, never wire onto a cadence.
        unsigned needBytes = 0x100;
        if (sscanf_s(args, "%x %x", &addr, &needBytes) >= 1) {
            uint32_t rva = addr;
            patterns::scan_for_vtable_object(
                rva, needBytes,
                [](void* obj, void* user) -> bool {
                    BVR_LOG("[b2r] vtscan 0x%X match @ %p",
                            *static_cast<const uint32_t*>(user), obj);
                    return false;
                },
                &rva, "vtscan", nullptr);
        } else {
            BVR_LOG("[b2r] usage: vtscan <hexRva> [needBytesHex]");
        }
    } else if (strcmp(cmd, "vrres") == 0) {
        // The eye render IS the game's backbuffer, so the game's resolution is
        // the VR resolution - and a headset eye is roughly square, which is why
        // this lane comes BEFORE any FOV or viewmodel tuning (BS1's settled
        // policy, sessions 27-28). Session 37 made it LIVE: the apply path
        // resizes the window (borderless, client == render size), the engine
        // follows with its own ResizeBuffers, and the ini write is only the
        // persistence for the next launch. Deliberately explicit, never
        // automatic - this resizes the user's game and writes their config.
        //
        // Named presets from kResModes (the same table the F10 picker shows),
        // `list` to print them, `restore` to bring the window chrome back,
        // raw WxH still accepted.
        // NOTE args comes from fgets and still carries the trailing newline,
        // so a whole-string strcmp can never match - token-match instead
        // (found the hard way: `vrres list` fell through to the status line).
        auto argIs = [args](const char* name) {
            size_t n = strlen(name);
            return strncmp(args, name, n) == 0 &&
                   (args[n] == '\0' || args[n] == '\n' || args[n] == '\r' ||
                    args[n] == ' ');
        };
        const ResMode* named = nullptr;
        for (const ResMode& m : kResModes)
            if (m.cmdName && argIs(m.cmdName)) named = &m;
        unsigned rw = 0, rh = 0;
        if (named) {
            rw = static_cast<unsigned>(named->w);
            rh = static_cast<unsigned>(named->h);
        }
        if (rw && rh) {
            apply_resolution(rw, rh); // already on the game thread, poll lane
        } else if (argIs("list")) {
            for (const ResMode& m : kResModes)
                BVR_LOG("[b2r] vrres %-10s -> %s", m.cmdName ? m.cmdName : "(ui)",
                        m.label);
        } else if (argIs("restore")) {
            restore_game_window();
        } else if (sscanf_s(args, "%ux%u", &rw, &rh) == 2 && rw && rh) {
            apply_resolution(rw, rh);
        } else if (sscanf_s(args, "%u %u", &rw, &rh) == 2 && rw && rh) {
            apply_resolution(rw, rh);
        } else {
            // The REAL backbuffer, not the XR swapchain: flat there is no
            // session, so fov_audit's swap dims are 0x0 and the status line
            // could never do its one job (compare the config against what is
            // actually being rendered). backbuffer_dims is published every
            // present, headset or not.
            unsigned bw = 0, bh = 0;
            bvr::hud::backbuffer_dims(&bw, &bh);
            game_ini::log_status(bw, bh);
        }
    } else if (strcmp(cmd, "vrinput") == 0) {
        // Session 32: the whole core input surface was unreachable on BS2 -
        // b2r never dispatched here, so the synthetic gamepad, the pitch servo
        // (`vrinput pitchservo on|off|invert|status`, which is how step 0's
        // sign gets checked) and the swing detector's entire flat test suite
        // had no way in. One line, all of it core, none of it BS1-specific.
        bvr::input::handle_command(args); // logs its own echoes
    } else if (strcmp(cmd, "vrpace") == 0) {
        // Session 33 audit, same class of gap as vrinput was: core owns the M8
        // stall guard and BS1 dispatches to it; b2r never did, so a fully built
        // feature was unreachable on this game. Core growing a feature does not
        // give an adapter access to it when the adapter owns the command table.
        bvr::vr::handle_pace_command(args);
    } else if (strcmp(cmd, "vrmirror") == 0) {
        bvr::vr::handle_mirror_command(args); // same gap, M8 desktop mirror
    } else if (strcmp(cmd, "vrhud") == 0) {
        // Same gap again: the HUD capture module is core and fully implemented
        // (b2r's own fovaudit and vrres already read from it), but with no
        // control surface it could not be toggled or A/B'd on BS2 at all.
        if (strncmp(args, "fovwatch", 8) == 0) {
            bvr::hud::set_fov_watch(strstr(args, "off") == nullptr);
        } else if (strncmp(args, "status", 6) == 0) {
            unsigned bw = 0, bh = 0;
            bvr::hud::backbuffer_dims(&bw, &bh);
            BVR_LOG("[b2r] vrhud status: backbuffer %ux%u screenOnly=%d letterbox=%d "
                    "cineHold=%d lenses=%d vpRatio=%.4f rayOffset=%d",
                    bw, bh, bvr::hud::screen_only() ? 1 : 0,
                    bvr::hud::letterbox(nullptr, nullptr) ? 1 : 0,
                    bvr::hud::cinematic_hold() ? 1 : 0, bvr::hud::fov_lens_count(),
                    bvr::hud::fov_vp_ratio(), bvr::hud::ray_block_offset());
            BVR_LOG("[b2r] vrhud status: fovWatch=%s",
                    bvr::hud::fov_watch_enabled() ? "on" : "OFF");
        } else {
            bvr::hud::set_enabled(strncmp(args, "off", 3) != 0);
            BVR_LOG("[b2r] command: vrhud %s", strncmp(args, "off", 3) != 0 ? "on" : "off");
        }
    } else if (strcmp(cmd, "vrpreset") == 0) {
        if (strncmp(args, "save", 4) == 0) save_vr_preset();
        else apply_vr_preset();
    } else if (strcmp(cmd, "reentry") == 0) {
        scenedraw::handle_command(args);
    } else if (strcmp(cmd, "vraer") == 0) {
        // Camera mode + AlternateEye: stereo WITHOUT draw re-entrancy, which is
        // the freeze. One toggle so it can be soaked the same way vrstereo is.
        bool on = strncmp(args, "on", 2) == 0;
        if (on) {
            bvr::vr::set_enabled(true);
            bvr::vr::set_camera_mode(true);
        }
        bvr::vr::set_alternate_eye(on);
        BVR_LOG("[b2r] command: vraer %s", on ? "on" : "off");
    } else if (strcmp(cmd, "vrstereo") == 0) {
        // Top-level alias for the one-toggle (BS1 parity). The poller only
        // ticks outside hooked calls, so applying directly here is safe.
        scenedraw::handle_command(strncmp(args, "on", 2) == 0 ? "vrstereo on"
                                                              : "vrstereo off");
    } else if (strcmp(cmd, "vraim") == 0) {
        aim::handle_command(args); // session 39: dispatch probe first, seam after
    } else if (strcmp(cmd, "vrhands") == 0) {
        hands::handle_command(args); // session 39: rig rides the right controller
    } else if (strcmp(cmd, "vrbones") == 0) {
        bones::handle_command(args); // session 39: the mechanism's own levers
    } else {
        BVR_LOG("[b2r] unknown command: %s (see the vocabulary comment in camera.cpp; "
                "BS1-only levers like exec are not ported)",
                cmd);
    }
}

// ---- VR preset (ported from b1r, session 33) -------------------------------
// b2r had NO persistence at all: every worldscale / headoff / ipd the user
// tuned had to be re-issued after each launch, which makes an in-headset
// verdict unrepeatable - "worldscale 100 was accepted in session 26" could
// never be re-checked against the same numbers. The file lives in THIS game's
// data dir (%LOCALAPPDATA%\BioshockVR\bs2\), so the two games can never read
// each other's calibration.
//
// Deliberately much smaller than b1r's: b2r has no aim/hands/bones/body
// modules, so only the sliders that exist here are persisted. Toggles are
// implied ON by `vrpreset` apply, except fgFovMatch, which is persisted as a
// VALUE because it is the thing currently under test.

void vr_preset_path(wchar_t* out, size_t count) {
    swprintf_s(out, count, L"%s\\vrpreset.ini", bvr::log::data_dir());
}

void save_vr_preset() {
    wchar_t path[MAX_PATH];
    vr_preset_path(path, MAX_PATH);
    FILE* f = nullptr;
    if (_wfopen_s(&f, path, L"w") != 0 || !f) {
        BVR_LOG("[b2r] could not write vrpreset.ini");
        return;
    }
    fprintf(f, "# BioShock 2 VR - tuned slider values (toggles are implied ON)\n");
    fprintf(f, "worldScale=%.1f\n", g_worldScale.load(std::memory_order_relaxed));
    fprintf(f, "headUpUu=%.1f\n", g_headOffUpUu.load(std::memory_order_relaxed));
    fprintf(f, "headFwdUu=%.1f\n", g_headOffFwdUu.load(std::memory_order_relaxed));
    fprintf(f, "ipdMm=%.1f\n", g_ipdMm.load(std::memory_order_relaxed));
    fprintf(f, "gameFovDeg=%.1f\n", g_gameFovDeg.load(std::memory_order_relaxed));
    fprintf(f, "fgFovMatch=%d\n", g_fgFovMatch.load(std::memory_order_relaxed) ? 1 : 0);
    // Session 34: persisted as a VALUE for the same reason fgFovMatch is - it is
    // under judgement, and a verdict that cannot be re-checked against the same
    // numbers next launch is not a verdict.
    fprintf(f, "fillHeadsetFov=%d\n", g_forceHeadsetFov.load(std::memory_order_relaxed) ? 1 : 0);
    fprintf(f, "fgFovManual=%.1f\n", g_fgFovManual.load(std::memory_order_relaxed));
    // Session 40: the per-hand model + aim calibration. These are exactly the
    // "tuned slider values" the doctrine says to persist - losing them on
    // relaunch would mean re-tuning both hands in the headset every session.
    for (int h = 0; h < 2; ++h) {
        const char* s = h ? "R" : "L";
        fprintf(f, "handTrimPitch%s=%.2f\n", s, hands::trim_pitch(h));
        fprintf(f, "handTrimYaw%s=%.2f\n", s, hands::trim_yaw(h));
        fprintf(f, "handTrimRoll%s=%.2f\n", s, hands::trim_roll(h));
        fprintf(f, "handOffFwd%s=%.2f\n", s, hands::off_fwd_cm(h));
        fprintf(f, "handOffRight%s=%.2f\n", s, hands::off_right_cm(h));
        fprintf(f, "handOffUp%s=%.2f\n", s, hands::off_up_cm(h));
        fprintf(f, "handScale%s=%.3f\n", s, bones::scale_of(h));
        // Session 40 round 2: the aim ray's own calibration, per hand.
        fprintf(f, "aimTrimPitch%s=%.2f\n", s, aim::trim_pitch(h));
        fprintf(f, "aimTrimYaw%s=%.2f\n", s, aim::trim_yaw(h));
        fprintf(f, "aimPosFwd%s=%.2f\n", s, aim::pos_fwd_cm(h));
        fprintf(f, "aimPosRight%s=%.2f\n", s, aim::pos_right_cm(h));
        fprintf(f, "aimPosUp%s=%.2f\n", s, aim::pos_up_cm(h));
    }
    // Round-2 judged toggles + input levers (user request: the preset owns
    // EVERYTHING; values here, arming implied by apply).
    fprintf(f, "originOn=%d\n", aim::origin_on() ? 1 : 0);
    fprintf(f, "dotDistM=%.2f\n", aim::dot_dist_m());
    fprintf(f, "armsMode=%d\n", bones::arms_mode());
    fprintf(f, "scaleWeapon=%d\n", bones::scale_attach() ? 1 : 0);
    fprintf(f, "animMode=%d\n", bones::anim_mode() ? 1 : 0);
    fprintf(f, "animTrans=%.2f\n", bones::anim_trans());
    fprintf(f, "turnScale=%.2f\n", bvr::input::turn_scale());
    fprintf(f, "snapOn=%d\n", bvr::input::snap_turn() ? 1 : 0);
    fprintf(f, "snapAngle=%.1f\n", bvr::input::snap_angle_deg());
    fprintf(f, "ammoMod=%d\n", static_cast<int>(bvr::input::ammo_mod()));
    fclose(f);
    BVR_LOG("[b2r] VR preset values saved to %ls", path);
}

void load_vr_preset_values() {
    wchar_t path[MAX_PATH];
    vr_preset_path(path, MAX_PATH);
    FILE* f = nullptr;
    if (_wfopen_s(&f, path, L"r") != 0 || !f) return; // no file = shipped defaults
    char line[128];
    int n = 0;
    // Session-40 hand values are staged and applied AFTER the parse loop, per
    // BS1's rule for coupled sets: a partial ini must never half-apply one.
    float hTrim[2][3] = {{hands::trim_pitch(0), hands::trim_yaw(0), hands::trim_roll(0)},
                         {hands::trim_pitch(1), hands::trim_yaw(1), hands::trim_roll(1)}};
    float hOff[2][3] = {
        {hands::off_fwd_cm(0), hands::off_right_cm(0), hands::off_up_cm(0)},
        {hands::off_fwd_cm(1), hands::off_right_cm(1), hands::off_up_cm(1)}};
    float hScale[2] = {bones::scale_of(0), bones::scale_of(1)};
    float aTrim[2][2] = {{aim::trim_pitch(0), aim::trim_yaw(0)},
                         {aim::trim_pitch(1), aim::trim_yaw(1)}};
    float aPos[2][3] = {{aim::pos_fwd_cm(0), aim::pos_right_cm(0), aim::pos_up_cm(0)},
                        {aim::pos_fwd_cm(1), aim::pos_right_cm(1), aim::pos_up_cm(1)}};
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '#' || line[0] == '\n') continue;
        char key[64] = {};
        float v = 0.0f;
        if (sscanf_s(line, "%63[^=]=%f", key, static_cast<unsigned>(sizeof(key)), &v) != 2)
            continue;
        ++n;
        // Per-hand keys: <name>L / <name>R.
        size_t klen = strlen(key);
        if (klen > 4 && (strncmp(key, "hand", 4) == 0 || strncmp(key, "aim", 3) == 0) &&
            (key[klen - 1] == 'L' || key[klen - 1] == 'R')) {
            int h = (key[klen - 1] == 'R') ? 1 : 0;
            char base[64];
            strncpy_s(base, key, klen - 1);
            base[klen - 1] = '\0';
            if (strcmp(base, "handTrimPitch") == 0) hTrim[h][0] = v;
            else if (strcmp(base, "handTrimYaw") == 0) hTrim[h][1] = v;
            else if (strcmp(base, "handTrimRoll") == 0) hTrim[h][2] = v;
            else if (strcmp(base, "handOffFwd") == 0) hOff[h][0] = v;
            else if (strcmp(base, "handOffRight") == 0) hOff[h][1] = v;
            else if (strcmp(base, "handOffUp") == 0) hOff[h][2] = v;
            else if (strcmp(base, "handScale") == 0 && v > 0.05f && v < 20.0f) hScale[h] = v;
            else if (strcmp(base, "aimTrimPitch") == 0) aTrim[h][0] = v;
            else if (strcmp(base, "aimTrimYaw") == 0) aTrim[h][1] = v;
            else if (strcmp(base, "aimPosFwd") == 0) aPos[h][0] = v;
            else if (strcmp(base, "aimPosRight") == 0) aPos[h][1] = v;
            else if (strcmp(base, "aimPosUp") == 0) aPos[h][2] = v;
            else --n;
            continue;
        }
        if (strcmp(key, "worldScale") == 0 && v > 0.0f)
            g_worldScale.store(v, std::memory_order_relaxed);
        else if (strcmp(key, "headUpUu") == 0)
            g_headOffUpUu.store(v, std::memory_order_relaxed);
        else if (strcmp(key, "headFwdUu") == 0)
            g_headOffFwdUu.store(v, std::memory_order_relaxed);
        else if (strcmp(key, "ipdMm") == 0 && v > 0.0f)
            g_ipdMm.store(v, std::memory_order_relaxed);
        else if (strcmp(key, "gameFovDeg") == 0 && v > 0.0f)
            g_gameFovDeg.store(v, std::memory_order_relaxed);
        else if (strcmp(key, "fgFovMatch") == 0)
            g_fgFovMatch.store(v != 0.0f, std::memory_order_relaxed);
        else if (strcmp(key, "fillHeadsetFov") == 0)
            g_forceHeadsetFov.store(v != 0.0f, std::memory_order_relaxed);
        else if (strcmp(key, "fgFovManual") == 0 && v >= 0.0f)
            g_fgFovManual.store(v, std::memory_order_relaxed);
        else if (strcmp(key, "originOn") == 0)
            aim::set_origin(v != 0.0f);
        else if (strcmp(key, "dotDistM") == 0)
            aim::set_dot_dist_m(v);
        else if (strcmp(key, "armsMode") == 0)
            bones::set_arms_mode(static_cast<int>(v));
        else if (strcmp(key, "scaleWeapon") == 0)
            bones::set_scale_attach(v != 0.0f);
        else if (strcmp(key, "animMode") == 0)
            bones::set_anim_mode(v != 0.0f);
        else if (strcmp(key, "animTrans") == 0)
            bones::set_anim_trans(v);
        else if (strcmp(key, "turnScale") == 0 && v > 0.0f)
            bvr::input::set_turn_scale(v);
        else if (strcmp(key, "snapOn") == 0)
            bvr::input::set_snap_turn(v != 0.0f);
        else if (strcmp(key, "snapAngle") == 0 && v > 0.0f)
            bvr::input::set_snap_angle_deg(v);
        else if (strcmp(key, "ammoMod") == 0 && v >= 0.0f && v <= 2.0f)
            bvr::input::set_ammo_mod(static_cast<bvr::input::AmmoMod>(static_cast<int>(v)));
        else
            --n;
    }
    fclose(f);
    for (int h = 0; h < 2; ++h) {
        hands::set_trim(h, hTrim[h][0], hTrim[h][1], hTrim[h][2]);
        hands::set_offset(h, hOff[h][0], hOff[h][1], hOff[h][2]);
        bones::set_scale(h, hScale[h]);
        aim::set_trim(h, aTrim[h][0], aTrim[h][1]);
        aim::set_pos(h, aPos[h][0], aPos[h][1], aPos[h][2]);
    }
    if (n) BVR_LOG("[b2r] VR preset: %d value(s) loaded from vrpreset.ini", n);
}

void apply_vr_preset() {
    BVR_LOG("[b2r] VR PRESET: arming the full VR configuration");
    bvr::vr::set_enabled(true);
    bvr::vr::set_camera_mode(true);
    // The one-toggle arms full-rate SR stereo ON 1t (SR-only knobs like pair
    // pacing live in apply_vrstereo). Session 26's "no 1t rung" premise was
    // refuted in session 35 - Draw's tail DOES have a submit handshake - and
    // the session-36 1t port is what makes the doubled draw safe.
    scenedraw::handle_command("vrstereo on");
    // Session 40 round 2 (user request): the CONTROLLER is part of the
    // preset - one apply arms the whole stack. Toggles are implied ON by the
    // apply, per the doctrine; the drive arms on the next pump tick.
    bvr::input::handle_command("on");
    BVR_LOG("[b2r] VR PRESET: worldscale %.1f headoff up %.1f fwd %.1f ipd %.1f "
            "fgfov %s",
            g_worldScale.load(std::memory_order_relaxed),
            g_headOffUpUu.load(std::memory_order_relaxed),
            g_headOffFwdUu.load(std::memory_order_relaxed),
            g_ipdMm.load(std::memory_order_relaxed),
            g_fgFovMatch.load(std::memory_order_relaxed) ? "on" : "off");
}

void poll_command_file(uint64_t now) {
    if (now - g_lastCmdPollMs < 1000) return;
    g_lastCmdPollMs = now;
    static wchar_t path[MAX_PATH];
    if (!path[0]) {
        const wchar_t* dir = bvr::log::data_dir();
        if (!dir[0]) return; // log::init failed - no data dir to poll
        swprintf_s(path, L"%s\\command.txt", dir);
    }
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (!GetFileAttributesExW(path, GetFileExInfoStandard, &fad)) return;
    if (CompareFileTime(&fad.ftLastWriteTime, &g_lastCmdWrite) == 0) return;
    g_lastCmdWrite = fad.ftLastWriteTime;
    FILE* f = _wfsopen(path, L"rt", _SH_DENYNO);
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        char cmd[32];
        int consumed = 0;
        if (sscanf_s(line, "%31s%n", cmd, static_cast<unsigned>(sizeof cmd), &consumed) != 1)
            continue;
        const char* args = line + consumed;
        while (*args == ' ' || *args == '\t') ++args;
        apply_command(cmd, args);
    }
    fclose(f);
}

// --- the camera tail ---------------------------------------------------------
// Runs after the original ProcessEvent returned for a PlayerCalcView dispatch:
// the script has produced the camera, the inlined caller has not yet read the
// block back. Everything here is the BS1 CalcView detour body reshaped onto
// the param block.
void calcview_tail(void* self, CalcViewParams* p) {
    g_playerController.store(self, std::memory_order_relaxed);
    uint32_t count = g_callCount.fetch_add(1, std::memory_order_relaxed) + 1;

    FVector* loc = &p->loc;
    FRotator* rot = &p->rot;
    g_lastLocX.store(loc->x, std::memory_order_relaxed);
    g_lastLocY.store(loc->y, std::memory_order_relaxed);
    g_lastLocZ.store(loc->z, std::memory_order_relaxed);
    g_lastPitch.store(rot->pitch, std::memory_order_relaxed);
    g_lastYaw.store(rot->yaw, std::memory_order_relaxed);
    g_lastRoll.store(rot->roll, std::memory_order_relaxed);

    if (!g_loggedFirstFire.exchange(true)) {
        BVR_LOG("[b2r] calcview first fire: pc=%p viewactor=%p loc=(%.1f %.1f %.1f) "
                "rot=(%d %d %d)",
                self, p->viewActor, loc->x, loc->y, loc->z, rot->pitch, rot->yaw, rot->roll);
    }

    uint64_t now = GetTickCount64();
    g_lastCalcViewMs = now;
    scenedraw::note_calcview(); // in/out attribution + one-shot instruments

    // FOV readback (session 25): claim what the game actually renders, every
    // call, menus included - BS1 shape. Null option object -> claim 0, which
    // core treats exactly like "no readback yet" (falls back to the headset
    // target, src=fallback), so nothing regresses before the first scan
    // lands. While the write block below holds the option, the readback
    // echoes the written value - correct, the renderer really renders it.
    int32_t* optionFov = patterns::hfov_option_ptr();
    g_lastOptionFov.store(optionFov ? *optionFov : 0, std::memory_order_relaxed);
    // Session 33: the claim is the RENDERED horizontal, and on BS2 that is NOT
    // the option except at 16:9. The settled law (two aspects x two options,
    // ENGINE_NOTES) fixes the vertical through a 16:9 reference and lets the
    // horizontal follow the backbuffer:
    //     tanV = tan(opt/2) * 9/16 ;  tanH = tanV * (bbW/bbH)
    // At 16:9 that collapses to hfov == opt, so this changes nothing about the
    // shipping configuration - it stops the claim being wrong the moment a
    // non-16:9 aspect ships. A claim that disagrees with the render is BS1's
    // yaw-warp bug: the compositor mis-reprojects every head rotation.
    // No backbuffer yet (before the first present) -> claim the option, which
    // is the 16:9 answer and the old behaviour.
    bvr::vr::set_rendered_hfov(optionFov ? rendered_hfov_for_option(*optionFov) : 0.0f);

    // Gameplay verdict + candidate-RVA self-diagnosis. Published every call:
    // core's cinematic fallback keys on this verdict AND its staleness, so a
    // silent adapter would park the headset on the quad screen permanently.
    void* va = p->viewActor;
    g_lastViewActor.store(va, std::memory_order_relaxed);
    uint32_t vtblRva = observed_vtable_rva(va);
    g_lastVtblRva.store(vtblRva, std::memory_order_relaxed);
    bool strictGameplay = is_gameplay_view_rva(vtblRva);
    bvr::vr::publish_gameplay_view(strictGameplay);

    {
        static uint32_t s_lastLoggedRva = 0xFFFFFFFFu; // game thread only
        if (vtblRva != s_lastLoggedRva) {
            s_lastLoggedRva = vtblRva;
            const char* verdict =
                vtblRva == patterns::kShockPlayerVtableRva ? "AShockPlayer candidate VERIFIED"
                : vtblRva == patterns::kShockPlayerControllerVtableRva
                    ? "AShockPlayerController candidate VERIFIED (menu shape)"
                    : "unrecognized - candidates may be wrong";
            BVR_LOG("[b2r] view actor vtable RVA 0x%X - %s (candidates: player 0x%X, pc 0x%X)",
                    vtblRva, verdict, patterns::kShockPlayerVtableRva,
                    patterns::kShockPlayerControllerVtableRva);
        }
    }

    // Transition line - same phrase as BS1 so the harness's generic "save is
    // loaded" detector transfers to this game unchanged.
    {
        static int s_lastViewState = -1; // game thread only
        int viewState = strictGameplay ? 1 : 0;
        if (viewState != s_lastViewState) {
            s_lastViewState = viewState;
            BVR_LOG("[b2r] view state: %s",
                    strictGameplay ? "GAMEPLAY (ShockPlayer view)" : "menu/cutscene");
            // A view-state change is the cheap signal that the object world
            // changed under us - the moment to retry a dormant settings scan,
            // and to drop the bone rig's cached pointers (session 39).
            patterns::hfov_scan_rearm("view state change");
            bones::on_world_change("view state change");
        }
    }

    // M3: drive the camera from the HMD pose. Pitch/roll are absolute (head
    // owns them); yaw is additive on the game's yaw so stick/mouse turning
    // still works; position adds the recenter-relative head offset, rotated
    // into the game yaw frame and scaled UU-per-meter. Identical math to
    // BS1's shipped drive (bioshock1r/camera.cpp) - a deliberate duplication,
    // recorded as a seam leak for a later game/shared unification.
    bool vrDrove = false;
    bvr::vr::HeadPose hp{};
    bool driveHead = false;
    // The frame context the aim ray and the hand models both read (session 39).
    // Filled as the drive runs; published once, after the drive and BEFORE the
    // eye offset, so both consumers place a controller with EXACTLY the
    // transform this frame's camera used.
    FrameContext fc{};
    fc.worldScale = g_worldScale.load(std::memory_order_relaxed);
    fc.viewActor = p->viewActor;
    fc.pc = self;
    if (now < g_simHead.deadline) {
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
    } else if (strictGameplay && !bvr::vr::cinematic_active() &&
               !bvr::hud::letterbox(nullptr, nullptr) && bvr::vr::vr_camera_mode() &&
               bvr::vr::get_head_pose(hp)) {
        // Live lane gated exactly like BS1: the HMD must not steer scripted
        // or menu cameras (their content lands on the quad screen).
        driveHead = true;
    }
    if (driveHead) {
        UeAngles a = ue_angles_from_xr_quat(hp.qx, hp.qy, hp.qz, hp.qw);
        if (g_recenterRequested.exchange(false, std::memory_order_relaxed) || !g_haveRecenter) {
            g_recenterPose = hp;
            g_recenterYawUnits = static_cast<int32_t>(lroundf(a.yawRad * kRotUnitsPerRadian));
            g_haveRecenter = true;
            BVR_LOG("[b2r] vr camera recentered (yaw %.1f deg)", a.yawRad * kRadToDeg);
        }

        // Snap turn (session 40, BS1 shape): shift the recenter yaw by the
        // queued bridge steps - raises the residual exactly like a physical
        // head turn. Drained BEFORE the residual math so this frame's yaw,
        // frame context and lasers all agree on the new recenter.
        if (g_haveRecenter) {
            if (int steps = bvr::input::take_snap_steps()) {
                int32_t units = static_cast<int32_t>(
                    lroundf(bvr::input::snap_angle_deg() * kRotUnitsPerDegree * steps));
                g_recenterYawUnits = wrap_rot(g_recenterYawUnits - units);
                BVR_LOG("[b2r] snap turn %+d step(s) (%.0f deg each)", steps,
                        bvr::input::snap_angle_deg());
            }
        }

        // Integer all the way through (see BS1's invariant note): the
        // head-look residual is the ONLY thing added to the game's own yaw.
        int32_t gameYawUnits = rot->yaw;
        int32_t headYawUnits = static_cast<int32_t>(lroundf(a.yawRad * kRotUnitsPerRadian));
        int32_t residualUnits = wrap_rot(headYawUnits - g_recenterYawUnits);
        float gameYawRad = static_cast<float>(gameYawUnits) / kRotUnitsPerRadian;
        // Session 32 (BS1 session 30, same defect in shared code): publish how
        // far the ENGINE's own pitch is from the head's BEFORE we overwrite it,
        // so the core pitch servo can steer the engine's value back through the
        // pad. This read has to happen here and ONLY here - one line below,
        // rot->pitch is the head's value and the error is identically zero,
        // which is exactly why nobody notices the engine's pitch is frozen.
        // Yaw needs no equivalent: it is written RELATIVE just below (the
        // engine's own yaw plus a residual), so the engine's yaw stays real.
        // Without this, publish_vr_gameplay below arms the shared pitch kill,
        // the engine's view pitch never changes again for the session, and
        // melee - BS2's DRILL - aims with it.
        {
            int32_t headPitchUnits =
                static_cast<int32_t>(lroundf(a.pitchRad * kRotUnitsPerRadian));
            int32_t errUnits = wrap_rot(headPitchUnits - rot->pitch);
            g_enginePitchUnits = rot->pitch;
            g_pitchErrDeg = static_cast<float>(errUnits) / kRotUnitsPerDegree;
            bvr::input::publish_pitch_error(g_pitchErrDeg);
        }
        rot->pitch = static_cast<int32_t>(a.pitchRad * kRotUnitsPerRadian);
        rot->roll = static_cast<int32_t>(a.rollRad * kRotUnitsPerRadian);
        rot->yaw = gameYawUnits + residualUnits;

        // Everything the aim/hands mapping needs from the drive, captured at
        // the one point where it is all true at once: the camera loc here is
        // still PRE head-offset (the base the controller maps onto), and
        // residualUnits is exactly the yaw the head drive added.
        fc.baseX = loc->x;
        fc.baseY = loc->y;
        fc.baseZ = loc->z;
        fc.driveYawOffsetRad = static_cast<float>(residualUnits) / kRotUnitsPerRadian;
        fc.recenterYawRad = recenter_yaw_rad();
        fc.recenterPx = g_recenterPose.px;
        fc.recenterPy = g_recenterPose.py;
        fc.recenterPz = g_recenterPose.pz;

        float dxr[3] = {hp.px - g_recenterPose.px, hp.py - g_recenterPose.py,
                        hp.pz - g_recenterPose.pz};
        float d[3];
        xr_to_ue(dxr, d);
        float scale = g_worldScale.load(std::memory_order_relaxed);
        // Into the recenter-local frame, then out by the game yaw (which is
        // where recenter-forward points now, since our yaw is purely additive).
        float recenterYawRad = recenter_yaw_rad();
        float c = cosf(-recenterYawRad), s = sinf(-recenterYawRad);
        float lx = d[0] * c - d[1] * s;
        float ly = d[0] * s + d[1] * c;
        float cg = cosf(gameYawRad), sg = sinf(gameYawRad);
        float ox = (lx * cg - ly * sg) * scale;
        float oy = (lx * sg + ly * cg) * scale;
        float oz = d[2] * scale;
        loc->x += ox;
        loc->y += oy;
        loc->z += oz;
        g_headOffX.store(ox, std::memory_order_relaxed);
        g_headOffY.store(oy, std::memory_order_relaxed);
        g_headOffZ.store(oz, std::memory_order_relaxed);

        // User head-anchor offset. Vertical is world-up; forward rides the
        // final view yaw, horizontal only.
        float hoUp = g_headOffUpUu.load(std::memory_order_relaxed);
        float hoFwd = g_headOffFwdUu.load(std::memory_order_relaxed);
        if (hoUp != 0.0f || hoFwd != 0.0f) {
            float vyaw = static_cast<float>(rot->yaw) / kRotUnitsPerRadian;
            loc->x += cosf(vyaw) * hoFwd;
            loc->y += sinf(vyaw) * hoFwd;
            loc->z += hoUp;
        }

        // AlternateEye (M4 rung 1): shift half an IPD along view-right; core
        // flips the sign after each submitted frame so successive game frames
        // render alternating eyes. Suppressed under SequentialReentry stereo
        // (rung 2), which applies both eye offsets itself. Same wiring as
        // BS1's shipped AER path.
        // The context is complete here: post head-anchor, PRE eye offset (an
        // eyed base would put the ray half an IPD off, and under SR stereo
        // pass 2 replays from this same pre-eye base).
        fc.vrDriving = true;
        fc.camX = loc->x;
        fc.camY = loc->y;
        fc.camZ = loc->z;
        fc.camPitch = rot->pitch;
        fc.camYaw = rot->yaw;
        fc.camRoll = rot->roll;

        int eyeSign = scenedraw::stereo_active() ? 0 : bvr::vr::current_eye_sign();
        if (eyeSign != 0) {
            apply_eye_offset(loc, *rot, eyeSign);
            int e = eyeSign < 0 ? 0 : 1;
            g_aerLoc[e] = *loc;
            g_aerStampMs[e] = now;
        }
        vrDrove = true;
    }
    if (!fc.vrDriving) {
        // Not driving: publish the engine's own camera so the ray still has a
        // coherent frame (the aim gate keys on vrDriving, but the seam's
        // freshness stamp and world-change detection keep running).
        fc.camX = loc->x;
        fc.camY = loc->y;
        fc.camZ = loc->z;
        fc.baseX = loc->x;
        fc.baseY = loc->y;
        fc.baseZ = loc->z;
        fc.camPitch = rot->pitch;
        fc.camYaw = rot->yaw;
        fc.camRoll = rot->roll;
    }
    g_vrDriving.store(vrDrove, std::memory_order_relaxed);
    if (!vrDrove) {
        g_headOffX.store(0.0f, std::memory_order_relaxed);
        g_headOffY.store(0.0f, std::memory_order_relaxed);
        g_headOffZ.store(0.0f, std::memory_order_relaxed);
        // Not driving means the pitch kill is not armed either, so the engine
        // owns its pitch outright and the error is zero by definition. Kept
        // fresh rather than stale so the heartbeat never shows a leftover.
        g_enginePitchUnits = rot->pitch;
        g_pitchErrDeg = 0.0f;
    }
    // Stick-pitch-kill gate for the core input bridge, same funnel BS1 feeds.
    bvr::input::publish_vr_gameplay(vrDrove && strictGameplay);
    // The aim seam's frame (pass 1 only by construction - pass 2 routes through
    // second_pass_replay). on_calcview builds this frame's hand rays and
    // publishes the laser + aim dot; the test ray and the hand ray both
    // substitute against fc's transform. hands runs LAST (BS1 ordering): the
    // model write must never precede the ray build it has to agree with.
    aim::on_calcview(fc, strictGameplay);
    hands::on_calcview(fc, strictGameplay);

    // FOV write (session 25, BS1 write-block shape): strict gameplay only,
    // VR wants it only while the HMD actually drives, manual lever for flat
    // tests. One-shot save/restore of the user's option value around the
    // whole written span; leaving gameplay restores immediately, and the
    // stale-restore in the ProcessEvent detour covers CalcView-silent
    // scripted scenes. No clamp needed: derivation showed the renderer
    // consumes at least up to 150 unclamped, and suggested_hfov_deg caps at
    // 160 on its own.
    if (optionFov) {
        // Session 34: core asks for a true HORIZONTAL fov; the option is not
        // one on BS2, so run the law backwards to get the option that renders
        // it. Identity at 16:9 - nothing shipping moves.
        float vrFov = g_forceHeadsetFov.load(std::memory_order_relaxed)
                          ? option_for_rendered_hfov(bvr::vr::suggested_hfov_deg())
                          : 0.0f;
        // Session 38: while the window tears down, stop wanting the write.
        // This runs inside a LIVE CalcView, so the existing OFF-edge restore
        // below goes through engine-provided live pointers - the safe restore
        // path; a teardown-time restore from outside CalcView would write
        // through possibly-freed objects.
        bool alive = !bvr::crash::teardown_seen();
        bool wantVr = alive && strictGameplay && vrDrove && vrFov > 0.0f;
        bool wantManual =
            alive && strictGameplay && g_gameFovWrite.load(std::memory_order_relaxed);
        if (wantVr || wantManual) {
            if (!g_wasWritingGameFov) {
                g_savedGameFov = *optionFov;
                g_wasWritingGameFov = true;
                BVR_LOG("[b2r] game fov write ON (saved option %d)", g_savedGameFov);
            }
            float want = wantVr ? vrFov : g_gameFovDeg.load(std::memory_order_relaxed);
            int32_t wantInt = static_cast<int32_t>(want + 0.5f);
            if (*optionFov != wantInt) *optionFov = wantInt;
        } else if (g_wasWritingGameFov) {
            *optionFov = g_savedGameFov;
            g_wasWritingGameFov = false;
            BVR_LOG("[b2r] game fov write OFF (restored option %d)", g_savedGameFov);
        }
    }

    // Foreground lens match. Runs AFTER the option write above, so when gfov or
    // vrfov is holding the option we match the value the world is actually
    // rendering rather than the user's saved one. Teardown gate as above: a
    // false want makes its OFF-edge restore run through this live CalcView.
    apply_fg_fov_match(optionFov,
                       strictGameplay && !bvr::crash::teardown_seen());

    // Debug camera offset - the cheapest "the block is writable on this game
    // too" proof, log-measurable via the heartbeat.
    loc->x += g_offsetX.load(std::memory_order_relaxed);
    loc->y += g_offsetY.load(std::memory_order_relaxed);
    loc->z += g_offsetZ.load(std::memory_order_relaxed);

    // SR pass 1 (LEFT eye): cache the fully-driven base camera LAST - after
    // everything above that mutates it - then shift this pass's render half
    // an IPD left. Pass 2 replays the base with the opposite eye. The eyed
    // latch carries the strict-gameplay decision (non-gameplay pairs render
    // both eyes identical for the quad screen).
    if (scenedraw::stereo_active()) {
        g_srBaseLoc = *loc;
        g_srBaseRot = *rot;
        g_srBaseStampMs = now;
        g_srBaseValid = true;
        g_srBaseEyed = strictGameplay && !bvr::vr::cinematic_active();
        if (g_srBaseEyed) {
            apply_eye_offset(loc, *rot, -1);
            g_srEyeLoc[0] = *loc;
            g_srEyeStampMs[0] = now;
        }
    } else {
        g_srBaseValid = false;
    }

    // Heartbeat LAST so it reports the FINAL camera handed back to the game -
    // drive, offsets and all. This is what the flat 6DOF checks measure
    // (offset -> exact UU delta; simhead -> exact rotator units; sim position
    // -> headOff), so it must not read the pre-drive values.
    if (g_logCamera.load(std::memory_order_relaxed)) {
        if (g_lastHeartbeatMs == 0) {
            g_lastHeartbeatMs = now;
            g_heartbeatBaseCount = count;
        } else if (now - g_lastHeartbeatMs >= 1000) {
            // enginePitch is the value the engine believed BEFORE the drive
            // overwrote it, so a frozen engine pitch shows up here as a number
            // that never moves while `rot` does. pitchErr is what the servo is
            // being fed; it should shrink as the servo converges.
            // xr=<state> is here because "why is it suddenly 10 fps" cost an
            // hour: a running session that never reached FOCUSED still PACES
            // the game at the runtime's not-visible cadence. One field answers
            // it at a glance.
            BVR_LOG("[b2r] camera: loc=(%.1f %.1f %.1f) rot=(%d %d %d) fov=%d "
                    "headOff=(%.1f %.1f %.1f) drive=%d enginePitch=%d "
                    "pitchErr=%.1f deg xr=%s%s (%u calls/s)",
                    loc->x, loc->y, loc->z, rot->pitch, rot->yaw, rot->roll,
                    g_lastOptionFov.load(std::memory_order_relaxed),
                    g_headOffX.load(std::memory_order_relaxed),
                    g_headOffY.load(std::memory_order_relaxed),
                    g_headOffZ.load(std::memory_order_relaxed), vrDrove ? 1 : 0,
                    g_enginePitchUnits, g_pitchErrDeg, bvr::vr::session_state_name(),
                    bvr::vr::ever_focused() ? "" : "/neverFocused",
                    count - g_heartbeatBaseCount);
            // SR flat measure (G6): live inter-eye camera delta from the two
            // passes of the current pair. Expect |d| == ipdMm/1000 x
            // worldScale UU (6.3 at defaults).
            if (g_srEyeStampMs[0] && g_srEyeStampMs[1] && now - g_srEyeStampMs[0] < 500 &&
                now - g_srEyeStampMs[1] < 500) {
                float dx = g_srEyeLoc[1].x - g_srEyeLoc[0].x;
                float dy = g_srEyeLoc[1].y - g_srEyeLoc[0].y;
                float dz = g_srEyeLoc[1].z - g_srEyeLoc[0].z;
                BVR_LOG("[b2r] sr eyes: L=(%.2f %.2f %.2f) R=(%.2f %.2f %.2f) "
                        "|d|=%.2f UU (expect %.2f)",
                        g_srEyeLoc[0].x, g_srEyeLoc[0].y, g_srEyeLoc[0].z,
                        g_srEyeLoc[1].x, g_srEyeLoc[1].y, g_srEyeLoc[1].z,
                        sqrtf(dx * dx + dy * dy + dz * dz),
                        g_ipdMm.load(std::memory_order_relaxed) / 1000.0f *
                            g_worldScale.load(std::memory_order_relaxed));
            }
            // AER flat measure (G0): live inter-eye camera delta. Expect
            // |d| == ipdMm/1000 x worldScale UU (6.3 at defaults) with the
            // head held still (simhead).
            if (g_aerStampMs[0] && g_aerStampMs[1] && now - g_aerStampMs[0] < 500 &&
                now - g_aerStampMs[1] < 500) {
                float dx = g_aerLoc[1].x - g_aerLoc[0].x;
                float dy = g_aerLoc[1].y - g_aerLoc[0].y;
                float dz = g_aerLoc[1].z - g_aerLoc[0].z;
                BVR_LOG("[b2r] aer: eye delta=(%.2f %.2f %.2f) |d|=%.2f UU "
                        "(expect %.2f)",
                        dx, dy, dz, sqrtf(dx * dx + dy * dy + dz * dz),
                        g_ipdMm.load(std::memory_order_relaxed) / 1000.0f *
                            g_worldScale.load(std::memory_order_relaxed));
            }
            g_lastHeartbeatMs = now;
            g_heartbeatBaseCount = count;
        }
    } else {
        g_lastHeartbeatMs = 0;
    }
}

// The CalcView-silent hole (BS1 session-22 lesson, bathysphere descent):
// scripted cameras can stop PlayerCalcView entirely, so the write block's
// OFF edge never runs and the user's option would stay overwritten. BS1
// restores from its scene-build detour; BS2 has no scenedraw hook, but
// ProcessEvent keeps firing for every script event, so the restore ticks
// from the detour below. Game thread only.
void restore_game_fov_if_stale(uint64_t staleMs) {
    if (!g_wasWritingGameFov) return; // steady-state cost: one bool read
    uint64_t now = GetTickCount64();
    if (g_lastCalcViewMs == 0 || now - g_lastCalcViewMs < staleMs) return;
    int32_t* optionFov = patterns::hfov_option_ptr();
    if (!optionFov) return;
    *optionFov = g_savedGameFov;
    g_wasWritingGameFov = false;
    BVR_LOG("[b2r] game fov write OFF (restored option %d - calcview silent %llu ms)",
            g_savedGameFov,
            static_cast<unsigned long long>(now - g_lastCalcViewMs));
}

// The SECOND-pass dispatch of a doubled Draw (SequentialReentry pass 2).
// Runs INSTEAD of calcview_tail: replay the cached pass-1 base with the
// RIGHT eye, or (non-stereo probe mode) add the flat yaw delta. Everything
// else is deliberately skipped - no drive, no telemetry, no FOV write, no
// heartbeat, and NO g_lastCalcViewMs update (staleness must keep meaning
// "the NORMAL pass went silent"). Writes are absolute, so multiple
// dispatches inside one pass-2 window are idempotent.
// NOTE for the future body/bones port: BS1's pass 2 also re-applies the
// bone drive here (bioshock1r/camera.cpp:821-824 - the engine re-evaluates
// the skeleton during pass 2). BS2 has no bones module yet; revisit this
// exact spot when it grows one.
void second_pass_replay(CalcViewParams* p, float yawDeg) {
    uint64_t now = GetTickCount64();
    if (scenedraw::stereo_active()) {
        if (!g_srBaseValid || now - g_srBaseStampMs > 100) return; // stale base: leave as-is
        p->loc = g_srBaseLoc;
        p->rot = g_srBaseRot;
        if (g_srBaseEyed) {
            apply_eye_offset(&p->loc, p->rot, +1);
            g_srEyeLoc[1] = p->loc;
            g_srEyeStampMs[1] = now;
        }
    } else {
        // Flat double-render probe: a visibly yawed second frame is the
        // proof the engine renders a second full scene per tick.
        p->rot.yaw += static_cast<int32_t>(yawDeg * kRotUnitsPerDegree);
    }
}

// --- the detours -------------------------------------------------------------

// FindFunctionChecked: learn the PlayerCalcView UFunction pointer with zero
// UObject-layout assumptions - the inlined camera sites resolve the name on
// EVERY dispatch, so the cache stays fresh from the first gameplay frame.
void* __fastcall FindFuncDetour(void* self, void* edx, uint32_t nameIndex,
                                uint32_t nameNumber, uint32_t global) {
    void* fn = g_originalFF(self, edx, nameIndex, nameNumber, global);
    if (g_fnameIndexGlobal && fn &&
        nameIndex == *reinterpret_cast<const uint32_t*>(g_fnameIndexGlobal)) {
        void* prev = g_calcViewFn.exchange(fn, std::memory_order_relaxed);
        if (prev != fn)
            BVR_LOG("[b2r] PlayerCalcView UFunction learned: %p (was %p, this=%p)", fn,
                    prev, self);
    }
    // Session-39 dispatch probe: one relaxed load when disarmed.
    if (aim::g_probeArmed.load(std::memory_order_relaxed))
        aim::probe_findfunc(nameIndex, nameNumber, fn);
    return fn;
}

// ProcessEvent: EVERY script event in the game passes through here - the
// pre-filter work must stay tiny. Camera work happens only on a pointer match
// with the learned UFunction; the 1 Hz command poll ticks through a cheap
// call counter so the seam works at the menu too (no CalcView there on BS2).
void __fastcall ProcessEventDetour(void* self, void* edx, void* fn, void* parms,
                                   void* result) {
    g_originalPE(self, edx, fn, parms, result);

    // Session-39 dispatch probe: one relaxed load when disarmed.
    if (aim::g_probeArmed.load(std::memory_order_relaxed)) aim::probe_process_event(fn);

    // Left-eye flicker fix (session 40 round 2): pass 1 has no second-pass
    // reapply, and the engine's animation restamps the pose bank mid-draw on
    // SOME frames - after pass 1's CalcView write, before the mesh batching.
    // Repaint the driven bones the moment a restamp is seen inside a hooked
    // draw. Cheap when idle: pe_repaint self-gates on the 100 ms write stamp
    // and does one 48-byte sentinel compare per driven hand.
    if (scenedraw::inside_hooked_call()) bones::pe_repaint();

    // The poll and the stale-restore defer while this thread is inside a
    // hooked render call (session 26): a command that installs or disables
    // hooks must never execute mid-build. Total ProcessEvent traffic is high,
    // so a deferred tick lands again within milliseconds. (BS2 improvement
    // over BS1, whose poller runs inside the build via the CalcView detour.)
    static uint32_t s_pollGate = 0; // game thread only
    // Session 40: one re-entrancy latch over the poller lane and the input
    // pump below - UpdateInput dispatches input events that re-enter this
    // detour, and a nested command poll could flip vrinput or install hooks
    // mid-pump. Game thread only, like the gate counter.
    static bool s_inTailLane = false;
    ++s_pollGate;
    if (((s_pollGate & 0xFF) == 0 || (s_pollGate & 0x3F) == 0) && !s_inTailLane) {
        if (!scenedraw::inside_hooked_call()) {
            s_inTailLane = true;
            if ((s_pollGate & 0xFF) == 0) poll_command_file(GetTickCount64());
            if ((s_pollGate & 0x3F) == 0) {
                restore_game_fov_if_stale(400);
                aim::poll_tick(GetTickCount64()); // 1 Hz probe summary while armed
                // Overlay-posted vrstereo request: hook installs must never
                // run mid-Draw or from the render thread; this lane is the
                // game thread outside hooked calls. Also the MENU-arming
                // path - BS2's menu never runs PlayerCalcView.
                scenedraw::apply_pending_vrstereo();
                // Overlay/command-posted resolution apply (session 37): live
                // window resize + ini persistence, on the game thread, and it
                // works from the main menu for the same reason vrstereo
                // arming does.
                if (uint64_t req = g_resWritePending.exchange(0, std::memory_order_relaxed))
                    apply_resolution(static_cast<uint32_t>(req >> 32),
                                     static_cast<uint32_t>(req & 0xFFFFFFFFu));
                // F10 "APPLY PRESET" (session 40 round 2): full arming on the
                // game thread outside hooked calls, same lane as vrstereo.
                if (g_vrPresetPending.exchange(0, std::memory_order_relaxed))
                    apply_vr_preset();
                if (g_windowRestorePending.exchange(false, std::memory_order_relaxed))
                    restore_game_window();
                // Deferred ini re-verify: the engine persists its live size
                // into Shared.ini on resize, ONE STEP BEHIND - measured this
                // session (it recorded the previous size mid-transition). One
                // rewrite wins because ours comes last.
                if (g_resConfirmVal && GetTickCount64() >= g_resConfirmDueMs) {
                    uint32_t w = static_cast<uint32_t>(g_resConfirmVal >> 32);
                    uint32_t h = static_cast<uint32_t>(g_resConfirmVal & 0xFFFFFFFFu);
                    g_resConfirmVal = 0;
                    game_ini::Viewport vp = game_ini::read_viewport();
                    if (vp.valid && (vp.w != w || vp.h != h)) {
                        BVR_LOG("[b2r] resolution: engine's resize-persist overwrote "
                                "Shared.ini (%ux%u) - rewriting %ux%u",
                                vp.w, vp.h, w, h);
                        game_ini::write_viewport(w, h);
                    }
                }
                // VR letterbox self-heal: a fresh boot comes up with the
                // game's own chromed window, whose client loses rows to the
                // desktop clamp - THE letterbox. While stereo is armed and
                // the client is smaller than the backbuffer, re-apply the
                // borderless enforcement at the backbuffer size. Never fires
                // flat, never fires in fullscreen, holds off while an apply
                // is still settling, and never touches a closing window
                // (session 38 teardown gate).
                if (scenedraw::stereo_active() && !bvr::crash::teardown_seen() &&
                    GetTickCount64() >= g_resHealHoldUntilMs) {
                    static int s_healFullscreen = -1; // -1 unknown, cache once
                    if (s_healFullscreen < 0)
                        s_healFullscreen =
                            game_ini::read_viewport().startupFullscreen ? 1 : 0;
                    unsigned bw = 0, bh = 0;
                    RECT c{};
                    HWND wnd = game_window();
                    if (s_healFullscreen == 0 && wnd &&
                        bvr::hud::backbuffer_dims(&bw, &bh) && bw && bh &&
                        GetClientRect(wnd, &c) && c.right > 0 && c.bottom > 0 &&
                        (static_cast<unsigned>(c.right) < bw ||
                         static_cast<unsigned>(c.bottom) < bh)) {
                        BVR_LOG("[b2r] resolution: client %ldx%ld < backbuffer %ux%u "
                                "with stereo armed - re-applying the borderless fix",
                                c.right, c.bottom, bw, bh);
                        enforce_client_size(bw, bh);
                        g_resHealHoldUntilMs = GetTickCount64() + 6000;
                    }
                }
            }
            s_inTailLane = false;
        }
    }

    // Input pump (session 40): per-event check, self-throttled to one
    // UpdateInput per present inside on_frame (the poller's 0x3F mask fires
    // ~40/s at gameplay dispatch rates - too coarse for a per-present pump).
    // Runs at the menu too (this lane exists precisely because BS2's menu
    // has no CalcView); never inside a hooked render call, never during
    // teardown, never nested under its own event dispatches.
    if (!s_inTailLane && !scenedraw::inside_hooked_call() &&
        !bvr::crash::teardown_seen()) {
        s_inTailLane = true;
        input_drive::on_frame(GetTickCount64());
        s_inTailLane = false;
    }

    if (fn && parms && fn == g_calcViewFn.load(std::memory_order_relaxed)) {
        auto* p = static_cast<CalcViewParams*>(parms);
        float yawDeg = 0.0f;
        if (scenedraw::second_pass_for_current_thread(&yawDeg)) {
            second_pass_replay(p, yawDeg);
            // The second engine CalcView may re-evaluate the skeleton over the
            // pass-1 bone write; repaint it so both eyes render the driven
            // hands (BS1's live-proven shape; cheap memcpy, no-op when idle).
            bones::reapply();
        } else {
            calcview_tail(self, p);
        }
    }
}

void atomic_slider(const char* label, std::atomic<float>& value, float lo, float hi) {
    float v = value.load(std::memory_order_relaxed);
    if (ImGui::SliderFloat(label, &v, lo, hi)) value.store(v, std::memory_order_relaxed);
}

} // namespace

void init_image(const bvr::pattern_scan::ProcessImage& image) {
    g_imageBase = reinterpret_cast<uintptr_t>(image.base);
    g_imageSize = image.size;
}

bool install(const patterns::Symbols& symbols) {
    if (!symbols.processEvent || !symbols.findFuncChecked || !symbols.fnameIndexGlobal)
        return false;
    g_fnameIndexGlobal = symbols.fnameIndexGlobal;

    MH_STATUS status = MH_CreateHook(symbols.findFuncChecked,
                                     reinterpret_cast<void*>(&FindFuncDetour),
                                     reinterpret_cast<void**>(&g_originalFF));
    if (status != MH_OK) {
        BVR_LOG("[b2r] MH_CreateHook(findfunc) failed: %s", MH_StatusToString(status));
        return false;
    }
    status = MH_CreateHook(symbols.processEvent,
                           reinterpret_cast<void*>(&ProcessEventDetour),
                           reinterpret_cast<void**>(&g_originalPE));
    if (status != MH_OK) {
        BVR_LOG("[b2r] MH_CreateHook(processevent) failed: %s", MH_StatusToString(status));
        MH_RemoveHook(symbols.findFuncChecked);
        return false;
    }
    // Self-enabling so these hooks' activation never rides on another
    // module's MH_EnableHook(MH_ALL_HOOKS). FindFunctionChecked goes live
    // FIRST: ProcessEvent's filter no-ops until the learner has run, so this
    // order can never dispatch on a stale null.
    status = MH_EnableHook(symbols.findFuncChecked);
    if (status == MH_OK) status = MH_EnableHook(symbols.processEvent);
    if (status != MH_OK) {
        BVR_LOG("[b2r] MH_EnableHook(calcview seam) failed: %s", MH_StatusToString(status));
        MH_RemoveHook(symbols.findFuncChecked);
        MH_RemoveHook(symbols.processEvent);
        return false;
    }

    load_vr_preset_values(); // tuned sliders, before anything reads them
    // Session 41 (user ask): the pad must work WITHOUT pressing APPLY PRESET.
    // BS2-local default-ON - core's shared enabled flag keeps its false
    // default so no BS1 path changes; `vrinput off` still disarms, and the
    // drive itself arms lazily on the pump lane once a viewport exists.
    bvr::input::handle_command("on");
    g_peTarget = symbols.processEvent;
    g_hookLive.store(true, std::memory_order_relaxed);
    BVR_LOG("[b2r] calcview seam installed (ProcessEvent %p + FindFunctionChecked %p)",
            symbols.processEvent, symbols.findFuncChecked);
    return true;
}

bool hook_live() {
    return g_hookLive.load(std::memory_order_relaxed);
}

void set_fov_override(float hfovDeg) {
    if (hfovDeg > 0.0f) {
        g_gameFovDeg.store(hfovDeg, std::memory_order_relaxed);
        g_gameFovWrite.store(true, std::memory_order_relaxed);
    } else {
        g_gameFovWrite.store(false, std::memory_order_relaxed);
    }
}

bool calcview_silent(uint64_t maxAgeMs) {
    if (g_lastCalcViewMs == 0) return true;
    uint64_t now = GetTickCount64();
    return now - g_lastCalcViewMs > maxAgeMs;
}

void draw_debug_ui() {
    if (!hook_live()) {
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f),
                           "CalcView seam: scan FAILED - running flat");
        return;
    }

    ImGui::Text("ProcessEvent hook: LIVE @ %p", g_peTarget);
    void* fn = g_calcViewFn.load(std::memory_order_relaxed);
    if (fn)
        ImGui::Text("PlayerCalcView UFunction: %p", fn);
    else
        ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.4f, 1.0f),
                           "PlayerCalcView UFunction: not seen yet (load a save)");

    static uint64_t lastSample = 0;
    static uint32_t lastCount = 0;
    static uint32_t callsPerSec = 0;
    uint64_t now = GetTickCount64();
    uint32_t total = g_callCount.load(std::memory_order_relaxed);
    if (lastSample == 0) {
        lastSample = now;
        lastCount = total;
    } else if (now - lastSample >= 1000) {
        callsPerSec =
            static_cast<uint32_t>((total - lastCount) * 1000ull / (now - lastSample));
        lastSample = now;
        lastCount = total;
    }

    int32_t pitch = g_lastPitch.load(std::memory_order_relaxed);
    int32_t yaw = g_lastYaw.load(std::memory_order_relaxed);
    int32_t roll = g_lastRoll.load(std::memory_order_relaxed);
    ImGui::Text("calcview calls: %u total, %u/s", total, callsPerSec);
    ImGui::Text("pc: %p  view actor: %p (vtbl RVA 0x%X)",
                g_playerController.load(std::memory_order_relaxed),
                g_lastViewActor.load(std::memory_order_relaxed),
                g_lastVtblRva.load(std::memory_order_relaxed));
    ImGui::Text("loc: %.1f %.1f %.1f", g_lastLocX.load(std::memory_order_relaxed),
                g_lastLocY.load(std::memory_order_relaxed),
                g_lastLocZ.load(std::memory_order_relaxed));
    ImGui::Text("rot: %d %d %d (%.1f %.1f %.1f deg)", pitch, yaw, roll,
                pitch / kRotUnitsPerDegree, yaw / kRotUnitsPerDegree,
                roll / kRotUnitsPerDegree);

    // ---- PRESET: always visible, at the TOP (session 41, user's round-2
    // ask - buried at a section bottom these read as sub-options). One
    // obvious place: APPLY arms the whole VR stack, SAVE persists every
    // tuned value on this panel (camera, lens, hands, aim, arms, input).
    ImGui::Separator();
    ImGui::TextColored(ImVec4(0.55f, 0.9f, 0.55f, 1.0f),
                       "PRESET - applies / saves ALL settings and values");
    // Engine-state arming must run on the game thread, outside hooked
    // calls - posted to the poller lane, never applied here.
    if (ImGui::Button("APPLY PRESET (stereo + camera + INPUT)"))
        g_vrPresetPending.store(1, std::memory_order_relaxed);
    ImGui::SameLine();
    if (ImGui::Button("SAVE all settings (survives a relaunch)"))
        save_vr_preset();
    ImGui::Separator();

    // ---- VIEWMODEL LENS: the thing currently under test -------------------
    // FIRST and open by default, because this is driven IN THE HEADSET. The
    // overlay renders into the game's backbuffer, which IS the eye image, so
    // everything here is reachable with F10 without taking the headset off.
    // Typing seam commands for an in-headset A/B does not work: reaching a
    // keyboard means alt-tabbing, and alt-tab drops the XR session out of
    // FOCUSED - which is the transition that preceded a hard freeze in session
    // 33. Anything the user has to judge by eye belongs here, not in a command.
    if (ImGui::CollapsingHeader("VIEWMODEL LENS  <-- TEST THIS",
                                ImGuiTreeNodeFlags_DefaultOpen)) {
        bool match = g_fgFovMatch.load(std::memory_order_relaxed);
        if (ImGui::Checkbox("Match viewmodel lens to the world  (the fix)", &match))
            g_fgFovMatch.store(match, std::memory_order_relaxed);
        ImGui::TextWrapped(
            "OFF = the game's own behaviour: the world uses your FOV option but "
            "the weapon is drawn through a fixed 60 deg lens, so it swings about "
            "twice as far as your head does. ON = both match.");

        float manual = g_fgFovManual.load(std::memory_order_relaxed);
        bool useManual = manual > 0.0f;
        if (ImGui::Checkbox("Use a manual value instead of following the FOV option",
                            &useManual))
            g_fgFovManual.store(useManual ? 90.0f : 0.0f, std::memory_order_relaxed);
        if (useManual) {
            float v = manual;
            if (ImGui::SliderFloat("Viewmodel FOV (deg)", &v, 40.0f, 140.0f, "%.0f"))
                g_fgFovManual.store(v, std::memory_order_relaxed);
            ImGui::TextWrapped("Drag until the weapon looks right, then note the "
                               "number - that IS the measurement.");
        }

        uintptr_t addr = fg_fov_addr();
        float cur = 0.0f;
        bool ok = addr && bvr::value_scan::safe_read_f32(addr, &cur);
        ImGui::Text("field @0x%08X = %s%.1f deg   |   world FOV option = %d",
                    static_cast<unsigned>(addr), ok ? "" : "?", ok ? cur : 0.0f,
                    g_lastOptionFov.load(std::memory_order_relaxed));
        // The written value is no longer the option verbatim: the fg lens's
        // gain differs from the world's law off 16:9 and is identified live
        // (g_fgLawG). Show both so a mismatch report carries its numbers.
        ImGui::Text("written %.1f deg (lens gain G=%.4f, 0.5625 = 16:9 identity)",
                    g_fgFovLastWritten.load(std::memory_order_relaxed),
                    g_fgLawG.load(std::memory_order_relaxed));

        float wH = 0.0f, wV = 0.0f, fH = 0.0f, fV = 0.0f;
        unsigned long long age = 0;
        if (bvr::hud::fov_watch(&wH, &wV, &age, 0)) {
            bool haveFg = bvr::hud::fov_watch_fg(&fH, &fV, &age, 0);
            int lenses = bvr::hud::fov_lens_count();
            ImGui::Text("rendered: world %.1f deg | viewmodel %.1f deg | lenses=%d",
                        2.0f * atanf(wH) * kRadToDeg,
                        haveFg ? 2.0f * atanf(fH) * kRadToDeg : 0.0f, lenses);
            // Say what the number is worth: lenses==1 is NOT proof (the watch
            // samples ~12 of 500 buffers and the viewmodel pass is ~17 of them).
            if (lenses == 1)
                ImGui::TextDisabled("(lenses=1 is a hint, not proof - the dump is "
                                    "the evidence)");
        }
    }

    // ---- FILL THE VIEW: the black bands, with the measurement on screen -----
    // The user's report was "there's black bars at the bottom". The cause is not
    // a letterbox - it is that a 16:9 render does not reach the top and bottom
    // of an essentially square headset eye. The numbers below ARE the diagnosis,
    // so the A/B explains itself in the headset instead of needing a log read.
    if (ImGui::CollapsingHeader("FILL THE VIEW  <-- the black bands",
                                ImGuiTreeNodeFlags_DefaultOpen)) {
        bool fill = g_forceHeadsetFov.load(std::memory_order_relaxed);
        if (ImGui::Checkbox("Automatic FOV (computed from your headset, never manual)",
                            &fill))
            g_forceHeadsetFov.store(fill, std::memory_order_relaxed);

        int32_t opt = g_lastOptionFov.load(std::memory_order_relaxed);
        float rh = rendered_hfov_for_option(opt), rv = rendered_vfov_for_option(opt);
        ImGui::Text("FOV option: %d %s", opt,
                    fill ? "(auto - the mod computes it per frame)" : "(the game's own)");
        float halfH = 0.0f, halfV = 0.0f;
        if (bvr::vr::headset_half_fov_deg(&halfH, &halfV)) {
            ImGui::Text("rendered %.0f x %.0f deg   |   your eye %.0f x %.0f deg", rh, rv,
                        halfH * 2.0f, halfV * 2.0f);
            float shortV = halfV * 2.0f - rv;
            if (shortV > 1.0f)
                ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f),
                                   "%.0f deg of vertical view missing - %.0f%% of the "
                                   "eye's height is black",
                                   shortV, 100.0f * shortV / (halfV * 2.0f));
            else
                ImGui::TextColored(ImVec4(0.5f, 1.0f, 0.5f, 1.0f),
                                   "vertical view fills the eye");
        } else {
            ImGui::Text("rendered %.0f x %.0f deg (no headset geometry yet)", rh, rv);
        }
        ImGui::TextWrapped(
            "The FOV option is the COVERAGE lever on this game: its vertical is "
            "fixed against a 16:9 reference at every aspect, so this toggle is "
            "what fills the eye (a squarer buffer alone adds no view). The "
            "RENDER RESOLUTION below is the SHARPNESS lever: more pixels over "
            "the same degrees.");

        ImGui::Separator();
        // The helmet, right here rather than in a rig section of its own,
        // because it is the OTHER HALF OF THE SAME TRADE: filling the eye
        // widens the weapon's lens too, and that is what brings the porthole
        // into frame. Measured, not guessed - the foreground eye does not move
        // with the fov on BS2 (BS1's zoom-pull does not exist here), so a wider
        // lens simply reveals a mesh that was always inches away. There is no
        // "push it to the edge" position at that distance; not drawing it is
        // the only lever that returns the view.
        bool hideRig = scenedraw::rig_hidden();
        if (ImGui::Checkbox("Hide the Big Daddy helmet (gives the view back)", &hideRig))
            scenedraw::set_rig_hidden(hideRig);
        ImGui::TextWrapped(
            "At a wide FOV the helmet's porthole ring surrounds the view and "
            "takes most of it. Untick to put it back - it costs you the "
            "periphery, but it is the authored look.");
    }

    // ---- RENDER RESOLUTION (session 37): the sharpness lever ----------------
    // BS1's picker SHAPE (dropdown of named modes + Custom + an apply button),
    // BS2's facts: the apply is LIVE (borderless window resize -> the engine's
    // own ResizeBuffers follows; see the enforcement block near kResModes),
    // Shared.ini [SharedOptions] is only the persistence for the next launch
    // (never BS1's WinDrv pair - the engine ignores those here). The work is
    // posted to the game thread via g_resWritePending: window calls and file
    // I/O do not belong on the render thread.
    if (ImGui::CollapsingHeader("RENDER RESOLUTION (applies live)")) {
        // read_viewport opens two files, so re-read at ~1 Hz, not every frame
        // (BS1 re-reads every overlay frame; do not port that).
        static game_ini::Viewport s_vp{};
        static uint64_t s_vpReadMs = 0;
        uint64_t nowMs = GetTickCount64();
        if (nowMs - s_vpReadMs > 1000 || s_vpReadMs == 0) {
            s_vpReadMs = nowMs;
            s_vp = game_ini::read_viewport();
        }
        unsigned liveW = 0, liveH = 0;
        bvr::hud::backbuffer_dims(&liveW, &liveH);
        if (!s_vp.valid) {
            ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f),
                               "Shared.ini not found - cannot set the resolution");
        } else {
            ImGui::Text("ini: %ux%u   live backbuffer: %ux%u", s_vp.w, s_vp.h, liveW,
                        liveH);
            if (liveW && liveH && (s_vp.w != liveW || s_vp.h != liveH))
                ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f),
                                   "ini and live render disagree - Apply below "
                                   "makes them match");
            if (s_vp.startupFullscreen)
                ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f),
                                   "StartupFullscreen=True: odd sizes may be quantized "
                                   "by the display path; windowed is the tested lane");

            const int kCustom = static_cast<int>(std::size(kResModes));
            static int s_sel = -1;
            static int s_w = 0, s_h = 0;
            if (s_sel < 0) {
                // Preselect what the ini already says, so the dropdown opens
                // showing the truth rather than a default.
                s_w = static_cast<int>(s_vp.w);
                s_h = static_cast<int>(s_vp.h);
                s_sel = kCustom;
                for (int i = 0; i < kCustom; ++i)
                    if (kResModes[i].w == s_w && kResModes[i].h == s_h) s_sel = i;
            }
            const char* preview =
                (s_sel == kCustom) ? "Custom..." : kResModes[s_sel].label;
            if (ImGui::BeginCombo("Resolution", preview)) {
                for (int i = 0; i <= kCustom; ++i) {
                    const char* label = (i == kCustom) ? "Custom..." : kResModes[i].label;
                    if (ImGui::Selectable(label, s_sel == i)) {
                        s_sel = i;
                        if (i != kCustom) {
                            s_w = kResModes[i].w;
                            s_h = kResModes[i].h;
                        }
                    }
                }
                ImGui::EndCombo();
            }
            if (s_sel == kCustom) {
                ImGui::InputInt("width", &s_w, 64, 256);
                ImGui::InputInt("height", &s_h, 64, 256);
                if (s_w < 1024) s_w = 1024;
                if (s_h < 1024) s_h = 1024;
                if (s_w > 8192) s_w = 8192;
                if (s_h > 8192) s_h = 8192;
            }
            ImGui::Text("selected: %d x %d, %.1f MPx", s_w, s_h,
                        static_cast<double>(s_w) * s_h / 1.0e6);
            // A headset eye wants ~0.93 (the Quest 3 render target's shape);
            // far from it the auto FOV must over-render one axis to cover the
            // eye's other axis, and those pixels fall outside the lenses.
            float selAspect = static_cast<float>(s_w) / static_cast<float>(s_h);
            if (selAspect > 1.25f || selAspect < 0.8f)
                ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f),
                                   "aspect %.3f is far from the eye's ~0.93 - much "
                                   "of this render falls outside the lenses",
                                   selAspect);
            // The auto-FOV preview for the SELECTED size - what the automatic
            // FOV will write once this size is live, from the law run
            // backwards at these dims (never option_for_rendered_hfov, which
            // reads the LIVE backbuffer and would preview the wrong number).
            int32_t prevOpt = auto_option_for_dims(s_w, s_h);
            if (prevOpt > 0) {
                float tanV =
                    tanf(static_cast<float>(prevOpt) * 0.5f / kRadToDeg) * (9.0f / 16.0f);
                float prevV = 2.0f * atanf(tanV) * kRadToDeg;
                float prevH = 2.0f * atanf(tanV * selAspect) * kRadToDeg;
                ImGui::Text("auto FOV here: option %d (renders %.0f x %.0f deg)",
                            prevOpt, prevH, prevV);
            } else {
                ImGui::TextDisabled("auto FOV preview needs a headset session");
            }
            if (ImGui::Button("Apply now (resizes the render)")) {
                g_resWritePending.store((static_cast<uint64_t>(s_w) << 32) |
                                            static_cast<uint32_t>(s_h),
                                        std::memory_order_relaxed);
            }
            ImGui::SameLine();
            if (ImGui::Button("Restore window chrome"))
                g_windowRestorePending.store(true, std::memory_order_relaxed);
            ImGui::TextDisabled("applies live (borderless window; taller than the "
                                "desktop hangs off the bottom) and persists to "
                                "Shared.ini for the next launch");
        }
    }

    if (ImGui::CollapsingHeader("VR camera (M3)", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Text(g_vrDriving.load(std::memory_order_relaxed)
                        ? "camera: driven by HMD pose"
                        : "camera: game (press VR camera ON, needs gameplay view)");
        input_drive::draw_debug_ui();
        ImGui::Text("head offset: (%.1f %.1f %.1f) UU",
                    g_headOffX.load(std::memory_order_relaxed),
                    g_headOffY.load(std::memory_order_relaxed),
                    g_headOffZ.load(std::memory_order_relaxed));
        if (ImGui::Button("VR camera ON (enable + camera mode)")) {
            bvr::vr::set_enabled(true);
            bvr::vr::set_camera_mode(true);
        }
        ImGui::SameLine();
        if (ImGui::Button("VR camera OFF")) bvr::vr::set_camera_mode(false);
        if (ImGui::Button("Recenter (seated pose + view yaw)"))
            g_recenterRequested.store(true, std::memory_order_relaxed);
        atomic_slider("World scale (UU per m)", g_worldScale, 10.0f, 200.0f);
        atomic_slider("Head offset up (UU)", g_headOffUpUu, -150.0f, 150.0f);
        atomic_slider("Head offset fwd (UU)", g_headOffFwdUu, -80.0f, 80.0f);
        atomic_slider("IPD (mm, AER + stereo)", g_ipdMm, 50.0f, 75.0f);
        // The headset-FOV write moved to its own "FILL THE VIEW" section above,
        // where the numbers that justify it are on screen next to it. One flag,
        // one control - two checkboxes for the same atomic is how a user ends up
        // unable to say which one they were judging.
    }

    // Session 40: the in-headset calibration surface. Standing rule - anything
    // judged BY EYE gets a control here, never a console command: alt-tabbing
    // to type destabilises the XR session, which is how these knobs were
    // unusable in the headset before. BS1's conventions: one slider set plus a
    // tuning-hand radio (not twelve sliders), sliders write atomics directly,
    // anything touching engine state goes through a pending lane.
    if (ImGui::CollapsingHeader("HANDS + AIM (per hand)  <-- CALIBRATE HERE",
                                ImGuiTreeNodeFlags_DefaultOpen)) {
        static int tuneHand = 1; // start on the weapon hand
        ImGui::RadioButton("L (plasmid)", &tuneHand, 0);
        ImGui::SameLine();
        ImGui::RadioButton("R (weapon)", &tuneHand, 1);

        float p = hands::trim_pitch(tuneHand), y = hands::trim_yaw(tuneHand),
              r = hands::trim_roll(tuneHand);
        bool trimChanged = false;
        trimChanged |= ImGui::SliderFloat("model trim pitch (deg)", &p, -180.0f, 180.0f);
        trimChanged |= ImGui::SliderFloat("model trim yaw (deg)", &y, -180.0f, 180.0f);
        trimChanged |= ImGui::SliderFloat("model trim roll (deg)", &r, -180.0f, 180.0f);
        if (trimChanged) hands::set_trim(tuneHand, p, y, r);

        float of = hands::off_fwd_cm(tuneHand), orr = hands::off_right_cm(tuneHand),
              ou = hands::off_up_cm(tuneHand);
        bool offChanged = false;
        offChanged |= ImGui::SliderFloat("model offset fwd (cm)", &of, -60.0f, 60.0f);
        offChanged |= ImGui::SliderFloat("model offset right (cm)", &orr, -60.0f, 60.0f);
        offChanged |= ImGui::SliderFloat("model offset up (cm)", &ou, -60.0f, 60.0f);
        if (offChanged) hands::set_offset(tuneHand, of, orr, ou);

        // Scale is deliberately NOT tied to world scale (user requirement): the
        // rig can be the wrong size while the world is right.
        float sc = bones::scale_of(tuneHand);
        if (ImGui::SliderFloat("model SCALE (x, independent of worldscale)", &sc, 0.2f,
                               4.0f))
            bones::set_scale(tuneHand, sc);
        if (ImGui::Button("scale both hands to this")) {
            bones::set_scale(0, sc);
            bones::set_scale(1, sc);
        }
        // Some weapon attachments inverse-decompose the pivot bone's scale
        // (the rifle's ammo drum GROWS as the hand shrinks - BS1 session-30
        // class). Off = hands scale, weapon keeps its authored size.
        bool sw = bones::scale_attach();
        if (ImGui::Checkbox("scale the WEAPON too (off if parts grow)", &sw))
            bones::set_scale_attach(sw);

        ImGui::Separator();
        ImGui::TextUnformatted("AIM ray (this hand) - where the laser/bullets go:");
        float ap = aim::trim_pitch(tuneHand), ay = aim::trim_yaw(tuneHand);
        bool aimChanged = false;
        aimChanged |= ImGui::SliderFloat("aim trim pitch (deg)", &ap, -30.0f, 30.0f);
        aimChanged |= ImGui::SliderFloat("aim trim yaw (deg)", &ay, -30.0f, 30.0f);
        if (aimChanged) aim::set_trim(tuneHand, ap, ay);
        float rf = aim::pos_fwd_cm(tuneHand), rr = aim::pos_right_cm(tuneHand),
              ru = aim::pos_up_cm(tuneHand);
        bool posChanged = false;
        posChanged |= ImGui::SliderFloat("ray origin fwd (cm)", &rf, -60.0f, 60.0f);
        posChanged |= ImGui::SliderFloat("ray origin right (cm)", &rr, -60.0f, 60.0f);
        posChanged |= ImGui::SliderFloat("ray origin up (cm)", &ru, -60.0f, 60.0f);
        if (posChanged) aim::set_pos(tuneHand, rf, rr, ru);
        bool orig = aim::origin_on();
        if (ImGui::Checkbox("bullets from the HAND (origin substitution)", &orig))
            aim::set_origin(orig);
        float dd = aim::dot_dist_m();
        if (ImGui::SliderFloat("aim dot / beam length (m)", &dd, 0.5f, 8.0f))
            aim::set_dot_dist_m(dd);

        ImGui::Separator();
        int arms = bones::arms_mode();
        ImGui::TextUnformatted("Arms:");
        ImGui::SameLine();
        if (ImGui::RadioButton("follow the hands", &arms, 1)) bones::set_arms_mode(1);
        ImGui::SameLine();
        if (ImGui::RadioButton("hide", &arms, 2)) bones::set_arms_mode(2);
        ImGui::SameLine();
        if (ImGui::RadioButton("game (frozen)", &arms, 0)) bones::set_arms_mode(0);

        ImGui::Separator();
        bool anim = bones::anim_mode();
        if (ImGui::Checkbox("engine animations on driven hands (melee/reload)", &anim))
            bones::set_anim_mode(anim);
        if (anim) {
            float at = bones::anim_trans();
            if (ImGui::SliderFloat("anim wrist travel (0 = glued)", &at, 0.0f, 1.0f))
                bones::set_anim_trans(at);
        }
    }

    if (ImGui::CollapsingHeader("Camera debug")) {
        atomic_slider("Offset X (UU)", g_offsetX, -500.0f, 500.0f);
        atomic_slider("Offset Y (UU)", g_offsetY, -500.0f, 500.0f);
        atomic_slider("Offset Z (UU)", g_offsetZ, -500.0f, 500.0f);
        bool logCam = g_logCamera.load(std::memory_order_relaxed);
        if (ImGui::Checkbox("Log camera (1 Hz to file)", &logCam))
            g_logCamera.store(logCam, std::memory_order_relaxed);
        int32_t optFov = g_lastOptionFov.load(std::memory_order_relaxed);
        if (optFov > 0)
            ImGui::Text("fov option: %d (readback claims it)", optFov);
        else
            ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.4f, 1.0f),
                               "fov option: settings object not located yet");
        bool gfovOn = g_gameFovWrite.load(std::memory_order_relaxed);
        if (ImGui::Checkbox("Game FOV write (manual)", &gfovOn))
            g_gameFovWrite.store(gfovOn, std::memory_order_relaxed);
        atomic_slider("Game FOV (deg)", g_gameFovDeg, 60.0f, 150.0f);
    }
}

} // namespace bvr::b2r::camera
