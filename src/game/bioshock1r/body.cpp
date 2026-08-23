#include "game/bioshock1r/body.h"

#include "core/hooks/d3d11_hook.h"
#include "core/input/xinput_bridge.h"
#include "core/util/log.h"
#include "game/bioshock1r/patterns.h"
#include "game/bioshock1r/scripted.h"
#include "game/shared/ue_math.h"

#include <windows.h>

#include <imgui.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace bvr::b1r::body {
namespace {

const uint8_t* g_imageBase = nullptr;

// ---- knobs (overlay thread writes, game thread reads) ----------------------

// Default ON since session 17: the flat gate passed with it off (reproducing
// the session-16 numbers, which is how the integer-yaw refactor got its own
// regression test) and then passed again with it on - the hand's world pose
// and the camera came out bit-identical, and the render lock's correction went
// from swinging 10.5 UU across a +-30 deg head sweep to flat within 0.5 UU,
// landing on the same 4.57 the calibrated zero-split configuration uses.
// `vrbody off` is the live in-headset A/B against the session-16 build.
std::atomic<bool>  g_armed{true};
// 0 = instant 1:1 (the user's call, session 17): the body snaps to the head
// yaw every frame, so stick-forward is ALWAYS exactly the look direction and
// the camera-vs-body split stays ~0 - the best case for both the viewmodel
// alignment and the bounds cull. Non-zero = exponential follow at that rate.
std::atomic<float> g_ratePerSec{0.0f};
// 23 deg by the user's in-headset calibration (session 17): "just needed that
// deadzone change and it was perfect". Inside the band the body does not steer
// at all, so ordinary glances leave the viewmodel completely world-locked -
// which is what removes the last of the "the gun moves with the camera a bit"
// percept - and only a deliberate turn past the band carries the body along.
// Beyond it the body trails the head by exactly the band width, so head and
// body never diverge by more than the band no matter how far you turn.
// DEFAULT 0 since v0.3.0 (session 21 part 4, the user's call): with the
// render lock retired nothing moves with the camera anymore, so the band
// that hid the lock's swing is not needed - instant 1:1 body follow.
// (The 23-deg calibration was the session-17 answer to a lock-era percept.)
std::atomic<float> g_deadzoneDeg{0.0f};
std::atomic<float> g_maxDegPerSec{180.0f};  // safety slew cap, not feel
// Feedback session (2026-08-13): while the slew cap is absorbing a fast head
// turn, stick-forward walks the INTERPOLATING body yaw - the reported "arc"
// on a quick 180. When on, camera.cpp publishes the not-yet-transferred error
// to the input bridge each CalcView and core rotates the movement stick by it
// (the Infinite s52 lane), so the walk direction is instant while the body
// still catches up under the cap. Zero whenever the body is caught up, so
// steady-state input is untouched.
std::atomic<bool>  g_moveDirInstant{true};
std::atomic<int>   g_field{0};              // 0 pc, 1 pawn, 2 both
std::atomic<bool>  g_probeLog{false};
// One-shot raw write test (`vrbody poke <deg>`), applied on the next frame.
std::atomic<int32_t> g_pokeUnits{0};

// ---- telemetry (game thread writes, overlay reads) -------------------------

std::atomic<int>     g_stateTlm{0};
std::atomic<float>   g_residDegTlm{0.0f};
std::atomic<int32_t> g_committedPerSec{0};
std::atomic<uint32_t> g_skipView{0}, g_skipWrite{0}, g_resets{0};

// ---- game-thread-only state ------------------------------------------------

enum State { kOff, kProbe, kRun, kDisabled };
State g_state = kOff;

void*    g_lastPc = nullptr;
uint32_t g_lastPresent = 0;
bool     g_havePrev = false;
int32_t  g_prevGameYaw = 0;
int32_t  g_expect = 0;      // units commanded last frame (0 = nothing)
int      g_probeTries = 0;
int      g_quietFrames = 0;
LARGE_INTEGER g_qpcFreq{};
LARGE_INTEGER g_lastQpc{};
uint64_t g_lastProbeLogMs = 0;
uint64_t g_secWindowMs = 0;
int32_t  g_secAccum = 0;
// RUN-state health: over a rolling window, commanded vs observed.
int      g_runFrames = 0;
int32_t  g_runCommanded = 0, g_runObserved = 0;
char     g_disableReason[64] = {};

// A single frame's transfer never exceeds this. Purely a blast radius limit:
// the probe handshake below is what actually decides whether the write works.
constexpr int32_t kMaxStepUnits = 4096;      // 22.5 deg
constexpr int32_t kProbeUnits = 200;         // 1.1 deg - invisible if it fails
constexpr int32_t kProbeTolerance = 64;
constexpr int32_t kQuietUnits = 32;          // "the player is not turning"
constexpr int32_t kDiscontinuityUnits = 4096; // teleport / load / scripted slew

// ---- guarded memory helpers (no C++ objects in an SEH frame) ---------------

bool read_rot(const void* obj, uint32_t off, int32_t out[3]) {
    __try {
        memcpy(out, static_cast<const uint8_t*>(obj) + off, 12);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool read_vec(const void* obj, uint32_t off, float out[3]) {
    __try {
        memcpy(out, static_cast<const uint8_t*>(obj) + off, 12);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Yaw is the SECOND int32 of an FRotator {pitch, yaw, roll} - see ue_math.h.
// Pitch is deliberately never touched: the engine's own rotation update
// applies a signed clamp to it, and the pawn keeps pitch 0 by design.
bool write_yaw(void* obj, uint32_t rotOff, int32_t yaw) {
    __try {
        *reinterpret_cast<int32_t*>(static_cast<uint8_t*>(obj) + rotOff + 4) = yaw;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool read_ptr(const void* src, void** out) {
    __try {
        *out = *static_cast<void* const*>(src);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

uint32_t to_rva(const void* p) {
    if (!p || !g_imageBase) return 0;
    return static_cast<uint32_t>(static_cast<const uint8_t*>(p) - g_imageBase);
}

const char* state_name(State s) {
    switch (s) {
        case kOff: return "off";
        case kProbe: return "PROBE";
        case kRun: return "RUN";
        default: return "DISABLED";
    }
}

void set_state(State s) {
    g_state = s;
    g_stateTlm.store(static_cast<int>(s), std::memory_order_relaxed);
}

void disable(const char* why) {
    _snprintf_s(g_disableReason, sizeof g_disableReason, _TRUNCATE, "%s", why);
    set_state(kDisabled);
    BVR_LOG("[vrbody] DISABLED: %s - the body write does not stick on this "
            "build. Camera and hand mapping are untouched (the probe undid "
            "itself); `vrbody status` for details.",
            why);
}

// Commit `units` to the body facing. Returns what actually landed (0 on a
// failed write), which is the ONLY thing the caller may absorb.
int32_t commit(void* pc, void* pawn, int32_t units) {
    if (units == 0) return 0;
    int field = g_field.load(std::memory_order_relaxed);
    bool wantPc = (field == 0 || field == 2);
    bool wantPawn = (field == 1 || field == 2);
    bool ok = false;

    if (wantPc && pc) {
        int32_t rot[3];
        if (read_rot(pc, patterns::kActorViewDirOffset, rot) &&
            write_yaw(pc, patterns::kActorViewDirOffset, (rot[1] + units) & 0xFFFF))
            ok = true;
    }
    if (wantPawn && pawn) {
        int32_t rot[3];
        if (read_rot(pawn, patterns::kActorViewDirOffset, rot) &&
            write_yaw(pawn, patterns::kActorViewDirOffset, (rot[1] + units) & 0xFFFF))
            ok = true;
    }
    if (!ok) {
        g_skipWrite.fetch_add(1, std::memory_order_relaxed);
        return 0;
    }
    return units;
}

// How much of the residual to take this frame, before any clamping.
int32_t wanted_step(int32_t residualUnits, float dt) {
    float dz = g_deadzoneDeg.load(std::memory_order_relaxed) * kRotUnitsPerDegree;
    float r = static_cast<float>(residualUnits);
    if (dz > 0.0f) {
        if (fabsf(r) <= dz) return 0;
        r -= (r > 0.0f ? dz : -dz); // no jump at the band edge
    }
    float rate = g_ratePerSec.load(std::memory_order_relaxed);
    if (rate > 0.0f) r *= (1.0f - expf(-rate * dt));

    float cap = g_maxDegPerSec.load(std::memory_order_relaxed) * kRotUnitsPerDegree * dt;
    if (cap > 0.0f) {
        if (r > cap) r = cap;
        if (r < -cap) r = -cap;
    }
    int32_t u = static_cast<int32_t>(lroundf(r));
    if (u > kMaxStepUnits) u = kMaxStepUnits;
    if (u < -kMaxStepUnits) u = -kMaxStepUnits;
    return u;
}

} // namespace

// The cutscene/vehicle guard, same predicate aim.cpp and hands.cpp use - but
// WITHOUT their `viewActor == pc` escape hatch. That hatch exists so the aim
// ray still works in the main-menu attract scene; a write to the body facing
// emphatically must not fire there. Public since session 19: the stick-pitch
// kill and the harness's "in gameplay" log signal gate on the same strict
// predicate.
// ---- WHICH WAY THE WALK ROTATION GOES --------------------------------------
//
// It was a compile-time +1 in camera.cpp, carried over from Infinite, and its
// own banner said "flip here if the walk tracks the mirror heading instead" and
// that settling it was a SIM derivation rather than an assumption. It was never
// derived for this game, and a rebuild is a terrible way to answer a question a
// headset can answer in five seconds - so it is a live toggle.
//
// It also could not have been FELT until s64: the pre-compensation ran on the
// wrong side of this rotation and collapsed small offsets to zero, so for
// ordinary head movement the walk went straight ahead whichever sign was used.
// Fixing the magnitude is what exposed the direction.
std::atomic<int> g_moveYawSign{1};

// The walk-direction probe, ported from BRVR's WalkDriftProbe. Default OFF.
//
// It measures rather than argues: where the pawn ACTUALLY travelled, against
// where the player was looking, and it evaluates BOTH signs every sample so the
// log picks the answer instead of a derivation picking it. Whichever error
// column sits at ~0 is the correct sign.
std::atomic<bool> g_walkProbe{false};
std::atomic<unsigned> g_scriptedFrames{0}; // frames the transfer stood down for
bool g_stoodDown = false;                  // edge detector, game thread
std::atomic<float> g_lastPubDeg{0.0f};
float g_walkPrev[3] = {};
bool g_walkHave = false;
uint64_t g_walkLastMs = 0;
const void* g_walkPawn = nullptr;

float wrap180(float d) {
    while (d > 180.0f) d -= 360.0f;
    while (d < -180.0f) d += 360.0f;
    return d;
}

// Once a second, and only while actually travelling. `viewActor` is the pawn.
void walk_probe(void* viewActor, int32_t gameYawUnits, int32_t residualUnits,
                const int32_t pawnRot[3], bool havePawn, uint64_t nowMs) {
    if (!g_walkProbe.load(std::memory_order_relaxed) || !viewActor || !havePawn) return;

    float loc[3] = {};
    if (!read_vec(viewActor, patterns::kActorLocOffset, loc)) return;

    // A new pawn is a new coordinate history, not a journey.
    if (viewActor != g_walkPawn) {
        g_walkPawn = viewActor;
        g_walkHave = false;
        g_walkLastMs = 0;
    }
    if (!g_walkHave || nowMs - g_walkLastMs < 1000) {
        if (!g_walkHave) {
            memcpy(g_walkPrev, loc, sizeof g_walkPrev);
            g_walkHave = true;
            g_walkLastMs = nowMs;
        }
        return;
    }

    const float dx = loc[0] - g_walkPrev[0];
    const float dy = loc[1] - g_walkPrev[1];
    memcpy(g_walkPrev, loc, sizeof g_walkPrev);
    g_walkLastMs = nowMs;

    const float dist = sqrtf(dx * dx + dy * dy);
    if (dist < 20.0f) return; // standing still; the heading is noise

    // WHERE THE PAWN ACTUALLY WENT. UE yaw is 0 at +X and grows toward +Y.
    const float actual = atan2f(dy, dx) * 180.0f / 3.14159265f;

    // WHERE THE PLAYER WAS LOOKING, composed exactly as CalcView composed it.
    const float camYaw =
        (gameYawUnits + residualUnits + scripted::yaw_adjust_units()) / kRotUnitsPerDegree;
    const float pawnYaw = pawnRot[1] / kRotUnitsPerDegree;

    // WHAT THE GAME RECEIVED. The composed pad is the last honest word on the
    // stick - reading our own pre-rotation value would assume the very thing
    // being measured.
    int16_t lx = 0, ly = 0;
    bvr::input::last_composed_sticks(&lx, &ly, nullptr, nullptr);
    if (lx == 0 && ly == 0) return;
    const float stickSent = atan2f(static_cast<float>(lx), static_cast<float>(ly)) *
                            180.0f / 3.14159265f;

    const float pub = g_lastPubDeg.load(std::memory_order_relaxed);

    // The game walks at `pawnYaw - stickAngle`, and we want it to walk at
    // `camYaw - (the angle the player actually pushed)`. The composer ADDS pub
    // to the stick, so the player pushed `stickSent - pub`.
    //
    // BOTH SIGNS ARE EVALUATED because a derivation is what got this wrong in
    // the first place. Whichever column is ~0 is the right sign; a reading where
    // one is ~0 and the other is ~2x pub is the mirror signature.
    const float errAsIs = wrap180(actual - camYaw + stickSent - pub);
    const float errFlipped = wrap180(actual - camYaw + stickSent + pub);

    BVR_LOG("[vrbody] walk: moved %.0f uu | actual %+.1f camYaw %+.1f pawnYaw %+.1f | "
            "stickSent %+.1f pub %+.1f | ERR as-is %+.1f  flipped %+.1f | %s",
            dist, actual, camYaw, pawnYaw, stickSent, pub, errAsIs, errFlipped,
            fabsf(pub) < 5.0f
                ? "pub is tiny - turn your head off your body WHILE walking or this "
                  "sample cannot tell the two signs apart"
                : (fabsf(errAsIs) <= fabsf(errFlipped) ? "as-is looks right"
                                                       : "FLIPPED looks right"));
}

bool is_gameplay_view(void* viewActor) {
    if (!viewActor) return false;
    void* vtbl = nullptr;
    if (!read_ptr(viewActor, &vtbl)) return false;
    return to_rva(vtbl) == patterns::kShockPlayerVtableRva;
}

void init(const bvr::pattern_scan::ProcessImage& image) {
    g_imageBase = image.base;
    QueryPerformanceFrequency(&g_qpcFreq);
}

void on_reset(const char* why) {
    g_havePrev = false;
    g_expect = 0;
    g_probeTries = 0;
    g_quietFrames = 0;
    g_runFrames = 0;
    g_runCommanded = g_runObserved = 0;
    if (g_state != kDisabled) set_state(g_armed.load(std::memory_order_relaxed) ? kProbe : kOff);
    g_resets.fetch_add(1, std::memory_order_relaxed);
    if (why) BVR_LOG("[vrbody] state reset (%s) -> %s", why, state_name(g_state));
}

int32_t on_calcview(void* pc, void* viewActor, int32_t gameYawUnits,
                    int32_t residualUnits, bool vrDriving) {
    // Once per rendered frame. CalcView fires far above frame rate (~7800/s at
    // the menu), and an ADDITIVE body write must not be applied several times
    // for one frame's worth of head motion. Same gate input_drive uses.
    uint32_t present = static_cast<uint32_t>(bvr::d3d11_hook::present_count());
    if (present == g_lastPresent) return 0;
    g_lastPresent = present;

    uint64_t nowMs = GetTickCount64();

    if (pc != g_lastPc) {
        g_lastPc = pc;
        on_reset("player controller changed");
    }

    // Read both candidate rotation fields once - the probe telemetry and the
    // transfer share them.
    int32_t pcRot[3] = {0, 0, 0}, pawnRot[3] = {0, 0, 0};
    bool havePc = pc && read_rot(pc, patterns::kActorViewDirOffset, pcRot);
    bool havePawn = viewActor && read_rot(viewActor, patterns::kActorViewDirOffset, pawnRot);

    int32_t dG = g_havePrev ? wrap_rot(gameYawUnits - g_prevGameYaw) : 0;

    if (g_probeLog.load(std::memory_order_relaxed) && nowMs - g_lastProbeLogMs >= 500) {
        g_lastProbeLogMs = nowMs;
        BVR_LOG("[vrbody] pc=%p G=%d (dG=%+d, expect %+d) | PC.rot=(%d %d %d)%s | "
                "pawn=%p rot=(%d %d %d)%s | resid=%+d (%.2f deg) | %s view=%s",
                pc, gameYawUnits, dG, g_expect, pcRot[0], pcRot[1], pcRot[2],
                havePc ? "" : " UNREADABLE", viewActor, pawnRot[0], pawnRot[1], pawnRot[2],
                havePawn ? "" : " UNREADABLE", residualUnits,
                residualUnits / kRotUnitsPerDegree, state_name(g_state),
                is_gameplay_view(viewActor) ? "gameplay" : "OTHER");
    }

    g_residDegTlm.store(residualUnits / kRotUnitsPerDegree, std::memory_order_relaxed);

    walk_probe(viewActor, gameYawUnits, residualUnits, pawnRot, havePawn, nowMs);

    // `vrbody poke <deg>`: one raw additive write through the transfer's own
    // path, deliberately NOT absorbed into the recenter. If the camera swings
    // and STAYS swung, the field is the authority and the engine's rotation
    // update composes on our value - which is the whole precondition for the
    // transfer. If it snaps back, this field is a slave and we go hunting.
    int32_t poke = g_pokeUnits.exchange(0, std::memory_order_relaxed);
    if (poke != 0) {
        int32_t landed = commit(pc, viewActor, poke);
        BVR_LOG("[vrbody] poke %+d units: %s (G was %d, PC.yaw was %d, pawn.yaw was %d) "
                "- watch the next few [vrbody] lines: G should hold at %d",
                poke, landed ? "written" : "WRITE FAILED", gameYawUnits, pcRot[1],
                pawnRot[1], wrap_rot(gameYawUnits + poke) & 0xFFFF);
        g_havePrev = true;
        g_prevGameYaw = gameYawUnits;
        return 0; // never absorbed - the camera swing IS the signal
    }

    // 1 Hz "units committed" telemetry.
    if (nowMs - g_secWindowMs >= 1000) {
        g_secWindowMs = nowMs;
        g_committedPerSec.store(g_secAccum, std::memory_order_relaxed);
        g_secAccum = 0;
    }

    // A discontinuity we did not command (teleport, load, scripted camera
    // slew) must not be read as "our write failed".
    if (g_havePrev && g_expect == 0 && abs(dG) > kDiscontinuityUnits) {
        g_prevGameYaw = gameYawUnits;
        on_reset("yaw discontinuity");
        return 0;
    }

    bool armed = g_armed.load(std::memory_order_relaxed);
    if (!armed) {
        if (g_state != kDisabled && g_state != kOff) set_state(kOff);
        g_havePrev = true;
        g_prevGameYaw = gameYawUnits;
        g_expect = 0;
        return 0;
    }
    if (g_state == kOff) on_reset("armed");
    if (g_state == kDisabled) {
        g_havePrev = true;
        g_prevGameYaw = gameYawUnits;
        return 0;
    }

    if (!vrDriving) {
        g_havePrev = true;
        g_prevGameYaw = gameYawUnits;
        g_expect = 0;
        return 0;
    }
    if (!is_gameplay_view(viewActor)) {
        g_skipView.fetch_add(1, std::memory_order_relaxed);
        g_havePrev = true;
        g_prevGameYaw = gameYawUnits;
        g_expect = 0;
        return 0;
    }

    // ---- NEVER WRITE THE AIM FIELD WHILE A SEQUENCE IS MOVING THE PLAYER ----
    //
    // A forced move steers the player BY Controller.Rotation, which is exactly
    // the field commit() writes to make the body follow the head. So looking
    // around during a scripted event re-aims the interpolation and the player
    // lands somewhere else. Reported 2026-08-23 from a headset and reproduced as
    // a clean pair: looking around with head and right stick landed "way off",
    // holding both completely still landed "in the exact right spot".
    //
    // BRVR graveyard entry 16, in its own words: "A forced move steers by
    // NOTHING of ours -- three balcony falls entered far right, straight on and
    // far left all landed on the same spot. THE WRITE ITSELF IS THE DAMAGE."
    //
    // WHY IT ONLY STARTED MATTERING IN s64. Part 2's PausePC.swf fix made the
    // head drive live during scripted scenes, deliberately, so the player could
    // look around instead of being pinned. That made vrDriving TRUE inside a
    // scene for the first time - which switched this transfer on inside them
    // too. One fix, two consequences, and the second was never considered.
    //
    // THE WINDOW, NOT forced_move(). Entry 16 has a second half: "never let the
    // window break mid-scene - a per-frame 'are you still in control' predicate
    // over the HUD did exactly that and threw one landing 3.7 m." The held
    // window bridged a real 16 ms gap on its first run here; the raw signals
    // would have broken in it.
    //
    // The bathysphere is in for the same reason with a different mover: the
    // sphere carries the pawn, and writing its yaw fights the ride.
    //
    // NOT ACCOMPANIED BY A TURN-AXIS KILL, deliberately. Graveyard entry 12:
    // zeroing sThumbRX freezes Controller.Rotation, which forced-move sequences
    // steer by, and the opening bathysphere walked into the back wall. We never
    // zero it; keep it that way.
    const bool inScriptedNow =
        scripted::enabled() &&
        (scripted::scripted_window() || scripted::bathysphere());
    if (inScriptedNow != g_stoodDown) {
        g_stoodDown = inScriptedNow;
        BVR_LOG("[vrbody] %s", inScriptedNow
                    ? "scripted event - body transfer STANDS DOWN (the game steers a "
                      "forced move by the field this writes)"
                    : "scripted event over - body transfer resumes (it slews to your "
                      "head under the cap; the recenter absorbs it, so the camera "
                      "must not move)");
    }
    if (inScriptedNow) {
        g_scriptedFrames.fetch_add(1, std::memory_order_relaxed);
        // Same shape as the early-outs above: carry the reference forward so the
        // scene's own yaw motion is never read as a failed write of ours, and so
        // the transfer resumes cleanly instead of on a stale expectation.
        g_havePrev = true;
        g_prevGameYaw = gameYawUnits;
        g_expect = 0;
        return 0;
    }

    float dt = 0.016f;
    LARGE_INTEGER qpc{};
    QueryPerformanceCounter(&qpc);
    if (g_lastQpc.QuadPart && g_qpcFreq.QuadPart)
        dt = static_cast<float>(qpc.QuadPart - g_lastQpc.QuadPart) /
             static_cast<float>(g_qpcFreq.QuadPart);
    g_lastQpc = qpc;
    if (dt < 0.001f) dt = 0.001f;
    if (dt > 0.1f) dt = 0.1f;

    int32_t committed = 0;

    if (g_state == kProbe) {
        if (g_expect != 0) {
            // Verdict frame: did last frame's 1.1 deg actually move the body?
            if (abs(dG - g_expect) <= kProbeTolerance) {
                set_state(kRun);
                g_probeTries = 0;
                BVR_LOG("[vrbody] probe CONFIRMED (asked %+d units, body moved %+d) - "
                        "the write is the authority and composes incrementally. "
                        "Transfer live.",
                        g_expect, dG);
            } else if (++g_probeTries >= 3) {
                // Undo: hand back exactly what we took, so a failed probe
                // leaves the shipped behaviour bit-identical.
                committed = -g_expect;
                g_expect = 0;
                g_havePrev = true;
                g_prevGameYaw = gameYawUnits;
                g_secAccum += committed;
                disable("body yaw write did not stick (3 probes)");
                return committed;
            } else {
                committed = -g_expect; // undo this attempt, retry after quiet
                BVR_LOG("[vrbody] probe %d/3 failed (asked %+d, body moved %+d) - retrying",
                        g_probeTries, g_expect, dG);
                g_quietFrames = 0;
            }
            g_expect = 0;
        } else {
            // Wait for the player to stop turning, so the engine's own turn
            // delta cannot be mistaken for our probe landing.
            if (abs(dG) < kQuietUnits) ++g_quietFrames; else g_quietFrames = 0;
            if (g_quietFrames >= 2 && abs(residualUnits) > kProbeUnits) {
                int32_t step = residualUnits > 0 ? kProbeUnits : -kProbeUnits;
                committed = commit(pc, viewActor, step);
                g_expect = committed;
            }
        }
    } else if (g_state == kRun) {
        int32_t step = wanted_step(residualUnits, dt);
        committed = commit(pc, viewActor, step);
        g_expect = committed;

        // Rolling health check: if we keep asking and the body keeps not
        // moving, something else took ownership - drop back to PROBE rather
        // than silently transferring into a void.
        g_runCommanded += abs(g_expect);
        g_runObserved += abs(dG);
        if (++g_runFrames >= 30) {
            if (g_runCommanded > 500 && g_runObserved * 4 < g_runCommanded) {
                BVR_LOG("[vrbody] commanded %d units over 30 frames but the body moved "
                        "%d - re-probing",
                        g_runCommanded, g_runObserved);
                on_reset("body stopped responding");
            }
            g_runFrames = 0;
            g_runCommanded = g_runObserved = 0;
        }
    }

    g_havePrev = true;
    g_prevGameYaw = gameYawUnits;
    g_secAccum += committed;
    return committed;
}

bool enabled() { return g_armed.load(std::memory_order_relaxed); }

float rate_per_sec() { return g_ratePerSec.load(std::memory_order_relaxed); }
float deadzone_deg() { return g_deadzoneDeg.load(std::memory_order_relaxed); }

int move_yaw_sign() { return g_moveYawSign.load(std::memory_order_relaxed); }

void set_move_yaw_sign(int s) {
    const int v = s < 0 ? -1 : 1;
    if (g_moveYawSign.exchange(v, std::memory_order_relaxed) == v) return;
    BVR_LOG("[vrbody] walk rotation sign = %+d (%s)", v,
            v > 0 ? "as shipped" : "MIRRORED - stick rotates the other way");
}

void note_published_move_yaw(float deg) {
    g_lastPubDeg.store(deg, std::memory_order_relaxed);
}

bool walk_probe_on() { return g_walkProbe.load(std::memory_order_relaxed); }

void set_walk_probe(bool on) {
    if (g_walkProbe.exchange(on, std::memory_order_relaxed) == on) return;
    g_walkHave = false;
    BVR_LOG("[vrbody] walk-direction probe %s%s", on ? "ON" : "off",
            on ? " - walk in a straight line with your head turned off your body, "
                 "then read the ERR columns"
               : "");
}

bool move_dir_instant() { return g_moveDirInstant.load(std::memory_order_relaxed); }
void set_move_dir_instant(bool on) { g_moveDirInstant.store(on, std::memory_order_relaxed); }

void set_tuning(float ratePerSec, float deadzoneDeg) {
    g_ratePerSec.store(ratePerSec < 0.0f ? 0.0f : ratePerSec, std::memory_order_relaxed);
    g_deadzoneDeg.store(deadzoneDeg < 0.0f ? 0.0f : deadzoneDeg, std::memory_order_relaxed);
}

void handle_command(const char* args) {
    float v = 0.0f;
    char word[16] = {};

    if (strncmp(args, "status", 6) == 0) {
        BVR_LOG("[vrbody] %s%s | rate=%.2f/s (%s) deadzone=%.1f deg max=%.0f deg/s | "
                "field=%s | resid=%.2f deg | committed=%d units/s | "
                "skips: view=%u write=%u resets=%u",
                state_name(g_state),
                g_state == kDisabled ? g_disableReason : "",
                g_ratePerSec.load(std::memory_order_relaxed),
                g_ratePerSec.load(std::memory_order_relaxed) <= 0.0f ? "instant 1:1"
                                                                    : "smoothed",
                g_deadzoneDeg.load(std::memory_order_relaxed),
                g_maxDegPerSec.load(std::memory_order_relaxed),
                g_field.load(std::memory_order_relaxed) == 0   ? "pc"
                : g_field.load(std::memory_order_relaxed) == 1 ? "pawn"
                                                              : "both",
                g_residDegTlm.load(std::memory_order_relaxed),
                g_committedPerSec.load(std::memory_order_relaxed),
                g_skipView.load(std::memory_order_relaxed),
                g_skipWrite.load(std::memory_order_relaxed),
                g_resets.load(std::memory_order_relaxed));
    } else if (strncmp(args, "probe", 5) == 0) {
        bool on = strstr(args, "off") == nullptr;
        g_probeLog.store(on, std::memory_order_relaxed);
        BVR_LOG("[vrbody] probe telemetry %s", on ? "ON (2 Hz)" : "off");
    } else if (strncmp(args, "rate", 4) == 0) {
        if (sscanf_s(args + 4, "%f", &v) == 1) {
            if (v < 0.0f) v = 0.0f;
            g_ratePerSec.store(v, std::memory_order_relaxed);
            BVR_LOG("[vrbody] rate %.2f/s (%s)", v, v <= 0.0f ? "instant 1:1" : "smoothed");
        }
    } else if (strncmp(args, "deadzone", 8) == 0) {
        if (sscanf_s(args + 8, "%f", &v) == 1) {
            if (v < 0.0f) v = 0.0f;
            g_deadzoneDeg.store(v, std::memory_order_relaxed);
            BVR_LOG("[vrbody] deadzone %.1f deg", v);
        }
    } else if (strncmp(args, "max", 3) == 0) {
        if (sscanf_s(args + 3, "%f", &v) == 1) {
            if (v < 1.0f) v = 1.0f;
            g_maxDegPerSec.store(v, std::memory_order_relaxed);
            BVR_LOG("[vrbody] slew cap %.0f deg/s", v);
        }
    } else if (strncmp(args, "field", 5) == 0) {
        if (sscanf_s(args + 5, "%15s", word, static_cast<unsigned>(sizeof word)) == 1) {
            int f = strcmp(word, "pawn") == 0 ? 1 : (strcmp(word, "both") == 0 ? 2 : 0);
            g_field.store(f, std::memory_order_relaxed);
            on_reset("field changed");
            BVR_LOG("[vrbody] write field = %s", f == 0 ? "pc" : (f == 1 ? "pawn" : "both"));
        }
    } else if (strncmp(args, "movesign", 8) == 0) {
        if (sscanf_s(args + 8, "%f", &v) == 1) set_move_yaw_sign(v < 0.0f ? -1 : 1);
    } else if (strncmp(args, "walkprobe", 9) == 0) {
        set_walk_probe(strstr(args, "off") == nullptr);
    } else if (strncmp(args, "movedir", 7) == 0) {
        bool on = strstr(args, "off") == nullptr;
        g_moveDirInstant.store(on, std::memory_order_relaxed);
        BVR_LOG("[vrbody] instant move direction %s", on ? "ON" : "off");
    } else if (strncmp(args, "poke", 4) == 0) {
        // One-shot additive body-yaw write through the SAME path the transfer
        // uses - the discriminator that answers "does an additive write to
        // this field survive the engine's own per-tick rotation update?"
        // without hand-computing a hex address. Does NOT touch the recenter
        // reference, so the camera WILL swing by this amount: that is the
        // point of the test.
        if (sscanf_s(args + 4, "%f", &v) == 1) {
            BVR_LOG("[vrbody] poke %+.1f deg queued (camera WILL swing - this is the "
                    "raw write test, the recenter is deliberately not advanced)",
                    v);
            g_pokeUnits.store(static_cast<int32_t>(lroundf(v * kRotUnitsPerDegree)),
                              std::memory_order_relaxed);
        }
    } else {
        bool on = strncmp(args, "off", 3) != 0;
        g_armed.store(on, std::memory_order_relaxed);
        on_reset(on ? "armed" : "disarmed");
        BVR_LOG("[vrbody] body-follows-head yaw transfer %s", on ? "ON" : "off");
    }
}

void draw_debug_ui() {
    if (!ImGui::CollapsingHeader("Body / locomotion (M7.5)")) return;

    bool on = g_armed.load(std::memory_order_relaxed);
    if (ImGui::Checkbox("Body follows head (stick-forward = look direction)", &on)) {
        g_armed.store(on, std::memory_order_relaxed);
        handle_command(on ? "on" : "off");
    }

    float rate = g_ratePerSec.load(std::memory_order_relaxed);
    if (ImGui::SliderFloat("Follow rate (/s, 0 = instant)", &rate, 0.0f, 10.0f, "%.2f"))
        g_ratePerSec.store(rate, std::memory_order_relaxed);
    float dz = g_deadzoneDeg.load(std::memory_order_relaxed);
    if (ImGui::SliderFloat("Deadzone (deg)", &dz, 0.0f, 60.0f, "%.1f"))
        g_deadzoneDeg.store(dz, std::memory_order_relaxed);

    bool mdi = g_moveDirInstant.load(std::memory_order_relaxed);
    if (ImGui::Checkbox("Instant move direction (walk tracks the head during fast turns)",
                        &mdi))
        g_moveDirInstant.store(mdi, std::memory_order_relaxed);

    bool mirrored = g_moveYawSign.load(std::memory_order_relaxed) < 0;
    if (ImGui::Checkbox("Walk goes the WRONG way - mirror it", &mirrored))
        set_move_yaw_sign(mirrored ? -1 : 1);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(
            "Which way the walk rotation turns. Tick this if walking forward with\n"
            "your head turned sends you off to the OTHER side.\n\n"
            "It was a compile-time constant carried over from BioShock Infinite and\n"
            "never derived for this game. Until s64 it could not be felt: the stick\n"
            "pre-compensation ran on the wrong side of this rotation and flattened\n"
            "small offsets to nothing, so the walk went straight ahead either way.");

    bool wp = g_walkProbe.load(std::memory_order_relaxed);
    if (ImGui::Checkbox("Log walk direction to the file", &wp)) set_walk_probe(wp);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(
            "Once a second while you are actually moving, writes where the pawn\n"
            "TRAVELLED against where you were LOOKING - and scores both signs.\n\n"
            "Walk a straight line with your head turned off your body, then read the\n"
            "two ERR columns: whichever sits near 0 is the correct setting above.");

    {
        const bool inScene = scripted::enabled() && (scripted::scripted_window() ||
                                                     scripted::bathysphere());
        ImGui::TextDisabled("scripted events: transfer %s (%u frames stood down)",
                            inScene ? "STANDING DOWN NOW" : "free",
                            g_scriptedFrames.load(std::memory_order_relaxed));
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(
                "The game steers a scripted forced move by the same rotation field\n"
                "this transfer writes, so looking around during one used to land you\n"
                "somewhere else. It now stands down for the whole scene and slews\n"
                "back afterwards.\n\n"
                "Not optional and not a comfort setting - it is a correctness gate.");
    }

    ImGui::Text("state %s   residual %.1f deg   %d units/s",
                state_name(static_cast<State>(g_stateTlm.load(std::memory_order_relaxed))),
                g_residDegTlm.load(std::memory_order_relaxed),
                g_committedPerSec.load(std::memory_order_relaxed));
}

} // namespace bvr::b1r::body
