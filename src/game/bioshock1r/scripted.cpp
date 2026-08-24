#include "game/bioshock1r/scripted.h"

#include "core/hooks/pattern_scan.h"
#include "core/util/log.h"
#include "core/vr/openxr_runtime.h"
#include "game/bioshock1r/body.h"
#include "game/bioshock1r/hands.h"
#include "game/bioshock1r/patterns.h"
#include "game/shared/ue_math.h"

#include <windows.h>

#include <imgui.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cwchar>

namespace bvr::b1r::scripted {
namespace {

// ============================================================================
//  THE OFFSETS, AND WHY EACH ONE IS BELIEVED
//
//  All four came from BRVR (docs/brvr-reference/BioshockVR/Game/GameState.cpp,
//  its M7-S1 / S4 / S5 / S6 banners) and every one is re-checked here before
//  use. The full derivations live in docs/bioshock1/ENGINE_NOTES.md; the short
//  version is in the comment above each constant.
// ============================================================================

// THE ENGINE OFFSETS THIS MODULE READS LIVE IN patterns.h, section
// "M7 scripted-event signals" - kHandsScriptedBits/kScriptedBit, kAnchorSlots,
// kHandsBaseOffset, kPawnFlagsBOffset/kCannotFallBit/kHavokCapsuleBit and
// kCtlForcedMoveOffset, each with the derivation that produced it. They were
// declared here until VOID's review of PR 51; the repo rule is that every
// engine address lives in patterns.cpp/.h and is documented in ENGINE_NOTES,
// and nothing about this module was an exception to it.

// ---- a clock that can actually see a frame ---------------------------------
//
// GetTickCount64 HAS ~15.6 ms RESOLUTION AND CalcView RUNS AT ~118/s, so most
// frames are 8.5 ms apart and the tick counter does not move between them. The
// first version of the freeze below computed dt from it and treated dt == 0 as
// "I was not called last frame, re-arm the reference" - which fired on roughly
// every other frame, re-armed constantly, and therefore never accumulated a
// single unit of the offset it exists to accumulate. It shipped looking
// plausible and did exactly nothing.
//
// QPC has the resolution the question needs. dt is clamped to 0 across a real
// gap so a discontinuity still re-arms; the point is that an ordinary frame is
// no longer indistinguishable from one.
LARGE_INTEGER g_qpcFreq{};
long long g_qpcLast = 0;

double tick_seconds() {
    if (!g_qpcFreq.QuadPart) QueryPerformanceFrequency(&g_qpcFreq);
    LARGE_INTEGER n;
    QueryPerformanceCounter(&n);
    double dt = 0.0;
    if (g_qpcLast) dt = static_cast<double>(n.QuadPart - g_qpcLast) / g_qpcFreq.QuadPart;
    g_qpcLast = n.QuadPart;
    if (dt < 0.0 || dt > 0.25) dt = 0.0; // a real gap: the caller re-arms
    return dt;
}

// ---- guarded memory helpers (no C++ objects in an SEH frame) ---------------

// SEH ONLY, DELIBERATELY - AND THIS IS A PERFORMANCE FIX, NOT A STYLE ONE.
//
// These used to call bvr::pattern_scan::is_memory_valid() first, which sounds
// like cheap belt-and-braces and is not: it is a VirtualQuery, a syscall that
// takes the process address-space lock. observe() does four of these per
// CalcView at ~118 CalcView/s, so it was ~470 VirtualQuery calls a second on the
// GAME THREAD, every frame, forever. This tree already has a rule against
// exactly that shape ("never add a per-frame memory scan") and this walked into
// it sideways, because each individual call looks like a bounds check.
//
// The __try/__except IS the protection and always was - it is the idiom the rest
// of this adapter uses (body.cpp's read_rot has no VirtualQuery either). A bad
// pointer faults, the handler catches it, the read reports false. The pointers
// themselves are validated once per change in anchor_check, which is where a
// one-shot check belongs.
bool read_u32(const void* obj, uint32_t off, uint32_t* out) {
    if (!obj) return false;
    const uint8_t* p = static_cast<const uint8_t*>(obj) + off;
    __try {
        *out = *reinterpret_cast<const uint32_t*>(p);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool read_f32(const void* obj, uint32_t off, float* out) {
    if (!obj) return false;
    const uint8_t* p = static_cast<const uint8_t*>(obj) + off;
    __try {
        *out = *reinterpret_cast<const float*>(p);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool write_f32(void* obj, uint32_t off, float v) {
    if (!obj) return false;
    uint8_t* p = static_cast<uint8_t*>(obj) + off;
    __try {
        *reinterpret_cast<float*>(p) = v;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool read_ptr_at(const void* obj, uint32_t off, void** out) {
    if (!obj) return false;
    const uint8_t* p = static_cast<const uint8_t*>(obj) + off;
    __try {
        *out = *reinterpret_cast<void* const*>(p);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// ---- published state (game thread writes, overlay thread reads) ------------
// The world FOV guard. Game thread owns g_worldFovGameplay; the switch and the
// counter are read by the F10 overlay thread.
std::atomic<int> g_worldFovGuard{1};
std::atomic<uint32_t> g_worldFovSnaps{0};
float g_worldFovGameplay = 0.0f;


// Master switch. Exists so the entire module can be taken out of the frame in
// the headset without a rebuild - which is the cheapest possible answer to "did
// this cause it?" and beats bisecting builds by a whole session per question.
std::atomic<int> g_enabled{1};

std::atomic<int> g_anim{0};
std::atomic<int> g_forced{0};
std::atomic<int> g_bathy{0};
std::atomic<int> g_window{0};
std::atomic<int> g_anchorOk{0};
std::atomic<int> g_holdMs{250}; // BRVR's measured value; covers every gap it saw

// Last raw dwords, for the F10 readout - a wrong offset is far easier to spot
// as a number that never moves than as a signal that never fires.
std::atomic<unsigned> g_rawHands{0};
std::atomic<unsigned> g_rawPawn{0};
std::atomic<unsigned> g_rawCtl{0};

// Edge bookkeeping for the readout.
std::atomic<unsigned long long> g_lastAnimEdgeMs{0};
std::atomic<int> g_lastAnimEdgeWas{0};
std::atomic<unsigned> g_animEdges{0};
std::atomic<unsigned> g_forcedEdges{0};
std::atomic<unsigned> g_bathyEdges{0};
std::atomic<unsigned> g_bridged{0};    // times the hold covered a gap
std::atomic<unsigned> g_shapeFails{0}; // ctl+0x9E0 read something that is not a bool

// Anchor state. Re-evaluated only when the hands actor pointer changes.
const void* g_anchorOwner = nullptr;
int g_anchorNamesOk = 0;

// Window hold. File scope rather than a function-local static SO THAT IT CAN BE
// RESET: a carried-over hold is exactly the "new world, old state" trap - a
// level or save load inside the 250 ms would otherwise report a scene still
// running in a world that has only just started.
unsigned long long g_windowLastOnMs = 0;

// Yaw the body transfer handed to the pawn since the last freeze tick.
int g_bodyYawPending = 0;

// ---- the rotation-follow policy -------------------------------------------
//
// Default Both, which is bit-for-bit today's behaviour: the policy is a no-op
// until the player picks otherwise in F10.
// Settled in a headset 2026-08-23: horizontal only. The user ran a full session
// on it and reported "vertical injection in cutscenes is gone".
std::atomic<int> g_rotFollow{static_cast<int>(RotFollow::HorizontalOnly)};

// The reference latched on the frame the game took the camera. Held absolutely
// rather than differenced, so an authored pitch slew is removed entirely and
// the horizon stays where it was when the shot began.
bool g_rotRefValid = false;
int g_rotRefPitch = 0;
int g_rotRefYaw = 0;
int g_rotRefRoll = 0;
std::atomic<unsigned> g_rotHolds{0}; // shots this policy has levelled
std::atomic<int> g_gameOwnsCam{0};   // live, for the panel to explain itself
// Default settled in a headset 2026-08-23.
std::atomic<int> g_freezeHands{1};
std::atomic<int> g_hideRig{1};
std::atomic<int> g_panelUp{0};
std::atomic<int> g_panelSuppressed{0};

// ---- the arm hide runs on measured rig MOTION, and the HOLD is the whole job -
//
// RETRACTED, round 11. Round 10 claimed hiding the rig freezes the bone array
// and moved the gate to forced_move() on that basis. **The claim was wrong and
// the evidence was confounded**: in the round-9 build `hidden` was CAUSED by
// motion reading zero, so identical bone values while hidden were guaranteed by
// construction and proved nothing about causation. Measuring a variable you are
// controlling is not a measurement.
//
// The round-10 build decoupled them - the hide ran off forced_move(), motion ran
// free - and the unconfounded answer is the opposite: **3 samples taken while
// hidden, 3 DISTINCT bone positions.** The array stays live behind a hidden
// actor, exactly as BRVR's ArmHide.h always said. DrawScale3D is a safe hide.
//
// WHAT THE SAME RUN DID ESTABLISH, and it is the real finding: of 336 samples
// inside scripted windows, **229 read raw exactly 0.0000**. CalcView fires at
// 118-240/s while the engine's animation ticks far slower, so most consecutive
// reads simply see the same pose. That is a SAMPLING ARTEFACT, not stillness -
// and it is why the peak-hold and, above all, the HOLD carry this feature.
//
// Measured distribution over that run: raw p75 0.0021, p90 0.0271, max 77.66;
// smoothed p75 0.0402, p90 0.2403. So 0.02 is a sound threshold - but 300 ms is
// far too sharp a hold against a signal that reads zero two thirds of the time.
// **BRVR ships 300 and ran 4000 in a headset**; the distribution above says its
// live value was the right one, so that is the default here.
// ---- the scene turns you, so hand your own turning back ---------------------
//
// Ported from BRVR's ScriptedRecentre. Without it the scene's authored rotation
// lands ON TOP of however far you turned yourself, so a scene that means to
// point you at a doorway points you at the doorway PLUS your own offset - and
// the more you looked around, the more wrong the framing.
//
// With it, your own turning is handed back as the scene turns: the authored
// direction wins and you still had free look on the way there.
//
//   0  off  - scene rotation lands on top of your own turning
//   1  wash - spend |d| of your offset for every d the scene turns (BRVR's ship)
//   2  drop - the whole offset goes the moment the scene first turns you
//
// Mode 1 is the default because it is proportional: a scene that turns you a
// long way takes all of it back, and one that nudges you takes a nudge.
std::atomic<int> g_scriptedRecentre{1};
std::atomic<unsigned> g_recentreEvents{0};
float g_recentreCancelled = 0.0f; // for the log line, game thread

// ---- HOW THE RIG IS HIDDEN, AND WHY IT IS NOT THE ACTOR SCALE ---------------
//
// By collapsing the BONES, with the actor left at full DrawScale3D.
//
// Scaling the actor to 0.0001 takes it out of whatever the engine animates, so
// the bone array FREEZES - measured 2026-08-23 as 293 consecutive probe samples
// with 0 of 47 bones moving, against 21-47 moving whenever the rig was shown.
// The gate reads that array to decide when the arms come back, so hiding that
// way is a one-way door: the arms can go and can never return. Three separate
// attempts to work around it (re-flagging the dirty byte every frame, a timed
// re-check, then discarding readings taken across the frozen gap) all failed,
// each in a new direction, because none of them addressed the freeze itself.
//
// Collapsing bones instead keeps the actor in the render set, so the engine
// animates it every frame and the reading is always honest. The caller takes
// its sample BEFORE writing the collapse, so it never measures our own write -
// which is INVARIANTS.md's 'you cannot hide by bone and measure by bone',
// answered by sequencing rather than by choosing a different bone.

std::atomic<float> g_armMotionThresh{0.02f};
std::atomic<int> g_armHoldMs{4000};
// Latched by note_hand_motion() on the game thread, read everywhere else.
std::atomic<int> g_armMoving{1};   // fail-safe default: arms VISIBLE
std::atomic<int> g_armBlind{1};    // no honest wrist to measure this frame
std::atomic<float> g_armRaw{0.0f}; // live values for the F10 readout
std::atomic<float> g_armSmoothed{0.0f};
std::atomic<int> g_armBone{-1};
unsigned long long g_armLastMovingMs = 0; // game thread only

// ---- the gameplay rotation freeze ------------------------------------------
//
// RETRACTION, and the reason this exists. s64 part 1 measured that the head
// drive overwrites rot->pitch and rot->roll ABSOLUTELY and concluded BRVR's
// FreezeGameplayRotation was redundant here. **That was wrong.** Yaw is not
// overwritten - camera.cpp composes `gameYawUnits + residualUnits` - so the
// engine's own yaw reaches the view untouched, and screenshake and the auto-pan
// onto enemy groups both arrive through it. Reported from a headset, 2026-08-22:
// "I am getting screenshake with world events and its making me look at groups
// of enemies that the game normally turns you to".
//
// The filter absorbs the game's yaw DELTA into an offset rather than clamping
// the value, so nothing ever snaps: the view simply declines to be turned.
std::atomic<int> g_freezeOn{1}; // headset-confirmed 2026-08-23
std::atomic<float> g_freezeBleedDegPerSec{0.0f}; // 0 = hold indefinitely
std::atomic<int> g_freezeHolding{0};
std::atomic<unsigned> g_freezeEvents{0};
bool g_freezeHave = false;
int g_freezePrevYaw = 0;
float g_freezeOffsetUnits = 0.0f; // float so the bleed can be sub-unit per frame
unsigned long long g_freezeLastMs = 0;

// A bound, because an unbounded offset is an unbounded divergence between where
// you are looking and where the pawn is facing. 60 deg is far past anything a
// shake or an auto-pan produces, so hitting it means something else is wrong -
// and it says so once rather than silently drifting.
constexpr float kFreezeMaxUnits = 60.0f * 65536.0f / 360.0f;

// ---- the player's own turn during a scripted scene -------------------------
//
// A scripted sequence pushes NullInput, so the game DISCARDS stick input and our
// turn - which goes through the game - does nothing. This is BRVR's
// ScriptedManualYaw: a mod-side accumulator applied to the CAMERA ONLY.
//
// It is never written into Controller.Rotation. That is BRVR's invariant 1 and
// it was bought expensively: three balcony falls entered far right, straight on
// and far left and all landed on the same spot with NO write, and with a heading
// substituted in both straight-on runs landed badly wrong. The write itself is
// the damage.
std::atomic<int> g_scriptedTurnOn{1}; // headset-settled 2026-08-23: on
std::atomic<float> g_scriptedTurnRate{90.0f}; // deg/s at full stick
float g_scriptedTurnUnits = 0.0f;
unsigned long long g_scriptedTurnLastMs = 0;

// Dropped on BOTH edges of the window, and this is not defensive coding - it is
// a bug BRVR shipped and had to chase. A latch set on the first scripted frame
// and never cleared means the SECOND scene of a session differences against a
// value left over from the END of the first. Reported as "both runs had the
// balcony fall land in different spots - first almost perfect, second way off".
void reset_scene_accumulators() {
    g_scriptedTurnUnits = 0.0f;
    g_scriptedTurnLastMs = 0;
    g_freezeHave = false; // a scene moves the camera on purpose; do not difference across it
}

// ============================================================================
//  THE ANCHOR CHECK - stronger here than in BRVR, because this repo has names
//
//  BRVR could only ask "does this field SHAPE like a name". This tree resolves
//  FName indices to text (patterns.h, fname_text via GNames), so the check is:
//
//    1. Is this object actually AHands? Its UObject name must read
//       'PlayerHands'. patterns.h already records that cross-check.
//    2. Does the field walk that produced +0x594 land on real fields? All four
//       computed FName slots must resolve to non-null text.
//
//  Both must pass. A PREDICTION IS NEVER ENOUGH TO TRUST - the same discipline
//  the rest of this tree applies to every derived offset.
// ============================================================================

bool anchor_check(const void* hands) {
    if (hands == g_anchorOwner) return g_anchorNamesOk >= 4;
    g_anchorOwner = hands;
    g_anchorNamesOk = 0;

    uint32_t nameIdx = 0;
    if (!read_u32(hands, patterns::kUObjectNameIndexOffset, &nameIdx)) {
        BVR_LOG("[b1r] scripted: ANCHOR FAILED - hands actor +0x%02X unreadable, every "
                "signal held false",
                patterns::kUObjectNameIndexOffset);
        return false;
    }
    const wchar_t* cls = patterns::fname_text(static_cast<int32_t>(nameIdx));
    if (!cls || wcscmp(cls, L"PlayerHands") != 0) {
        BVR_LOG("[b1r] scripted: ANCHOR FAILED - hands actor names itself '%ls' (index %u), "
                "not 'PlayerHands'. Wrong object; every signal held false.",
                cls ? cls : L"<unresolved>", nameIdx);
        return false;
    }

    // The four computed slots. Each is an FName {index, number}; only the index
    // needs to resolve for the walk to be standing on a real field.
    const wchar_t* got[4] = {nullptr, nullptr, nullptr, nullptr};
    for (int i = 0; i < 4; ++i) {
        uint32_t idx = 0;
        if (!read_u32(hands, patterns::kAnchorSlots[i], &idx)) continue;
        got[i] = patterns::fname_text(static_cast<int32_t>(idx));
        if (got[i]) ++g_anchorNamesOk;
    }

    const bool ok = g_anchorNamesOk >= 4;
    BVR_LOG("[b1r] scripted: anchor %s - PlayerHands confirmed, %d/4 name slots resolve "
            "(+0x498 '%ls', +0x4B8 '%ls', +0x4D8 '%ls', +0x558 '%ls')",
            ok ? "ok" : "FAILED", g_anchorNamesOk, got[0] ? got[0] : L"<none>",
            got[1] ? got[1] : L"<none>", got[2] ? got[2] : L"<none>",
            got[3] ? got[3] : L"<none>");
    if (!ok)
        BVR_LOG("[b1r] scripted: ANCHOR FAILED - the field walk behind hands+0x%03X does "
                "not land on real fields on this build. Every signal held false; RE-DERIVE "
                "before trusting the offset (ENGINE_NOTES, scripted events).",
                patterns::kHandsScriptedBits);
    return ok;
}

// ---- the three raw signals -------------------------------------------------

void anim_tick(const void* hands) {
    uint32_t bits = 0;
    if (!read_u32(hands, patterns::kHandsScriptedBits, &bits)) return;
    g_rawHands.store(bits, std::memory_order_relaxed);

    const int want = (bits & patterns::kScriptedBit) ? 1 : 0;
    if (want == g_anim.load(std::memory_order_relaxed)) return;

    g_anim.store(want, std::memory_order_relaxed);
    g_lastAnimEdgeMs.store(GetTickCount64(), std::memory_order_relaxed);
    g_lastAnimEdgeWas.store(want, std::memory_order_relaxed);
    g_animEdges.fetch_add(1, std::memory_order_relaxed);
    BVR_LOG("[b1r] scripted: %s  (hands+0x%03X = %08X)",
            want ? "*** SCRIPTED ANIMATION BEGAN ***" : "--- scripted animation ended ---",
            patterns::kHandsScriptedBits, bits);
}

void bathysphere_tick(const void* hands) {
    void* pawn = nullptr;
    if (!read_ptr_at(hands, patterns::kHandsBaseOffset, &pawn) || !pawn) return;
    // Hands.Base must be the player pawn, and this tree already has that test:
    // body::is_gameplay_view compares the actor's vtable to kShockPlayerVtableRva.
    if (!body::is_gameplay_view(pawn)) return;

    uint32_t bits = 0;
    if (!read_u32(pawn, patterns::kPawnFlagsBOffset, &bits)) return;
    g_rawPawn.store(bits, std::memory_order_relaxed);

    const int want = (bits & patterns::kCannotFallBit) ? 1 : 0;
    if (want == g_bathy.load(std::memory_order_relaxed)) return;

    g_bathy.store(want, std::memory_order_relaxed);
    g_bathyEdges.fetch_add(1, std::memory_order_relaxed);
    // THE ORACLE, logged on every edge: entering a ride must raise bit 1 while
    // LOWERING bit 2 in the same write. If the log ever shows them moving
    // together, this offset is not what the derivation says it is.
    const int havok = (bits & patterns::kHavokCapsuleBit) ? 1 : 0;
    BVR_LOG("[b1r] scripted: bathysphere %s  (pawn+0x%03X = %08X, bCannotFall=%d "
            "havokCapsule=%d - oracle %s)",
            want ? "ON" : "off", patterns::kPawnFlagsBOffset, bits, want, havok,
            want != havok ? "HOLDS (bits oppose)"
                          : "BROKEN - both bits agree, suspect the offset");
}

void forced_move_tick(const void* controller) {
    uint32_t v = 0;
    if (!read_u32(controller, patterns::kCtlForcedMoveOffset, &v)) return;
    g_rawCtl.store(v, std::memory_order_relaxed);

    // Shape check, every read. A lone bool is exactly 0 or 1.
    if (v > 1) {
        if (g_shapeFails.fetch_add(1, std::memory_order_relaxed) == 0)
            BVR_LOG("[b1r] scripted: SHAPE CHECK FAILED - ctl+0x%03X reads %08X, which is "
                    "not a bool. Stale controller or wrong offset; forced-move held false.",
                    patterns::kCtlForcedMoveOffset, v);
        if (g_forced.exchange(0, std::memory_order_relaxed))
            BVR_LOG("[b1r] scripted: --- forced move done --- (dropped by the shape check)");
        return;
    }

    const int want = static_cast<int>(v);
    if (want == g_forced.load(std::memory_order_relaxed)) return;

    g_forced.store(want, std::memory_order_relaxed);
    g_forcedEdges.fetch_add(1, std::memory_order_relaxed);
    BVR_LOG("[b1r] scripted: %s  (ctl+0x%03X = %u)",
            want ? "forced move BEGAN" : "--- forced move done ---", patterns::kCtlForcedMoveOffset, v);
}

// ---- ONE SCENE, ONE WINDOW - the held pair ---------------------------------
//
// A scene raises the two signals IN SEQUENCE: the forced move walks you into
// place, then the scripted animation plays. They normally overlap - BRVR
// measured the forced flag dropping 0.09 s AFTER the animation begins - so
// nothing downstream ever sees a gap.
//
// THE ORDER IS NOT GUARANTEED. BRVR measured the Little Sister crawl going
// forced-move-done -> (one frame) -> animation-began, at 231 CalcView/s. Its
// camera hook released the aim in that frame, re-armed its base from a field
// that happened to read (0,0), and the scene ran 18.6 deg off for 58 seconds.
//
// So the pair is HELD: rises instantly, falls only after g_holdMs with neither
// signal set. Held here rather than in a consumer because consumers layer
// different policies on top and each gets signed off separately in a headset.

void note_bridge(bool rawNow, bool rawPrev) {
    // The hold is doing work exactly when the raw pair goes down and comes back
    // up inside the window. That gap is the defect this code exists to cover,
    // and it is the only case worth a line - a log that only ever says the
    // obvious gets skimmed past.
    if (rawPrev || !rawNow || !g_windowLastOnMs) return;
    if (!g_window.load(std::memory_order_relaxed)) return; // held across, or it is a new scene
    const unsigned long long gap = GetTickCount64() - g_windowLastOnMs;
    if (gap == 0) return;
    g_bridged.fetch_add(1, std::memory_order_relaxed);
    BVR_LOG("[b1r] scripted: window bridged a %llu ms gap between the two signals - one "
            "scene, one window (this is the Little Sister crawl fix working)",
            gap);
}

void window_tick() {
    const bool raw = g_anim.load(std::memory_order_relaxed) != 0 ||
                     g_forced.load(std::memory_order_relaxed) != 0;
    const unsigned long long now = GetTickCount64();
    const int hold = g_holdMs.load(std::memory_order_relaxed);
    bool held = raw;

    if (raw)
        g_windowLastOnMs = now;
    else if (g_windowLastOnMs && hold > 0 &&
             (now - g_windowLastOnMs) < static_cast<unsigned long long>(hold))
        held = true;
    else
        g_windowLastOnMs = 0;

    const int want = held ? 1 : 0;
    if (want != g_window.load(std::memory_order_relaxed)) {
        g_window.store(want, std::memory_order_relaxed);
        // BOTH edges. See reset_scene_accumulators.
        reset_scene_accumulators();
    }
}

} // namespace

// ---- public ----------------------------------------------------------------

bool scripted_anim() { return g_anim.load(std::memory_order_relaxed) != 0; }
bool forced_move() { return g_forced.load(std::memory_order_relaxed) != 0; }
bool bathysphere() { return g_bathy.load(std::memory_order_relaxed) != 0; }
bool scripted_window() { return g_window.load(std::memory_order_relaxed) != 0; }
bool anchor_ok() { return g_anchorOk.load(std::memory_order_relaxed) != 0; }

void reset() {
    g_windowLastOnMs = 0;
    g_freezeOffsetUnits = 0.0f;
    g_freezeHave = false;
    g_scriptedTurnUnits = 0.0f;
    g_bodyYawPending = 0;
    g_anim.store(0, std::memory_order_relaxed);
    g_forced.store(0, std::memory_order_relaxed);
    g_bathy.store(0, std::memory_order_relaxed);
    g_window.store(0, std::memory_order_relaxed);
    // The arm latch resets to VISIBLE, never to hidden: a new world with a stale
    // "still" would collapse the rig before a single sample was taken.
    g_armMoving.store(1, std::memory_order_relaxed);
    g_armBlind.store(1, std::memory_order_relaxed);
    g_armRaw.store(0.0f, std::memory_order_relaxed);
    g_armSmoothed.store(0.0f, std::memory_order_relaxed);
    g_armBone.store(-1, std::memory_order_relaxed);
    g_armLastMovingMs = 0;
}

// Defined below with their banners; declared here because observe() drives them.
void update_freeze(int gameDelta, float turnStickX, double dt);
int game_yaw_delta(int gameYawUnits, double dt);
void update_scripted_recentre(int gameDelta);
void update_scripted_turn(float turnStickX, double dt);

bool enabled() { return g_enabled.load(std::memory_order_relaxed) != 0; }

void set_enabled(bool on) {
    if (g_enabled.exchange(on ? 1 : 0, std::memory_order_relaxed) == (on ? 1 : 0)) return;
    if (!on) reset();
    BVR_LOG("[b1r] scripted: module %s - %s", on ? "ON" : "OFF (nothing read, nothing applied)",
            on ? "signals live" : "use this to A/B whether this module causes a symptom");
}

// ---- THE WORLD FOV GUARD ---------------------------------------------------
//
// A scripted camera narrows the world lens and CalcView never mentions it.
// MEASURED 2026-08-23, boarding a bathysphere: CalcView reports fov=100.0 for
// the whole ride, and on the frame the forced move begins the renderer switches
// to 80 deg (`rendered tanH 0.8391 = 80.0 deg vs option 100.0`). The mod does
// the honest thing with that - it re-claims the layer at the measured 80 - and
// the honest thing is what the player sees as a bug: the picture is correct and
// still full-resolution, but it only fills 80 deg of a wider headset, so it sits
// in a small box with black all round.
//
// So the fix is not on the claim side. It is to stop the narrowing.
//
// PORTED FROM BRVR's ClampWorldFov, with its shape and NOT its numbers: two
// fields written together, snapped only when they move the wrong way, never a
// fixed per-frame write. BRVR measured 75 -> 60; this build reads 100 -> 80.
//
// SELF-CALIBRATING, because there is no good constant to hardcode. The value to
// restore is whatever the lens read during ordinary gameplay, which already
// accounts for the user's FOV option, so it is sampled continuously while no
// scene owns the camera and only replayed while one does.
//
// GATED, and the gate is the load-bearing part. BRVR's own CONSOLIDATION.md
// names the hazard: this mod's cutscene detector has a leg that fires when the
// game renders a different fov than it claims, and clamping deletes exactly
// that evidence. Restricting the guard to the boarding-plus-ride window keeps
// the leg intact everywhere else, and it is also why weapon zoom is safe -
// Hands::FadeFOV drives the same field down when you scope, and you cannot
// scope on a bathysphere.
//
// The window is `forced_move() || bathysphere()` and not `bathysphere()` alone:
// the narrowing lands 1.2 s BEFORE the ride flag, on the frame the forced move
// begins, which is the moment the player presses A.
constexpr float kWorldFovEpsilon = 0.5f;

void clamp_world_fov(void* pc) {
    if (!pc || !g_worldFovGuard.load(std::memory_order_relaxed)) return;

    float live = 0.0f;
    if (!read_f32(pc, patterns::kPcWorldFovOffset, &live)) return;
    if (!(live > 1.0f) || !(live < 170.0f)) return; // not a lens; leave it alone

    const bool sceneOwnsLens = forced_move() || bathysphere();

    if (!sceneOwnsLens) {
        // Ordinary gameplay: this IS the value to restore. Remembered rather
        // than derived, so the user's own FOV option needs no separate read.
        g_worldFovGameplay = live;
        return;
    }
    const float want = g_worldFovGameplay;
    if (!(want > 1.0f)) return; // never seen gameplay yet - nothing to restore

    // ONLY THE NARROW SIDE. A scene that opens the lens WIDER than gameplay is
    // not the defect being fixed here, and snapping that back would be an
    // unrequested content change.
    if (live >= want - kWorldFovEpsilon) return;

    const bool a = write_f32(pc, patterns::kPcWorldFovOffset, want);
    const bool b = write_f32(pc, patterns::kPcWorldFovMirrorOffset, want);
    g_worldFovSnaps.fetch_add(1, std::memory_order_relaxed);

    static unsigned long long lastLog = 0;
    const unsigned long long now = GetTickCount64();
    if (now - lastLog >= 2000) {
        lastLog = now;
        BVR_LOG("[b1r] worldfov: scene narrowed the lens to %.1f - snapping both fields "
                "back to %.1f (+0x%03X %s, +0x%03X %s, %s)",
                live, want, patterns::kPcWorldFovOffset, a ? "ok" : "FAILED",
                patterns::kPcWorldFovMirrorOffset, b ? "ok" : "FAILED",
                bathysphere() ? "bathysphere ride" : "forced move");
    }
}

void observe(void* playerController, int gameYawUnits, float turnStickX) {
    if (!enabled()) {
        // Say so ONCE. The switch exists to take this module out of the frame
        // for an A/B, and it is easy to leave off and then wonder why the arms
        // never hide - which is exactly what happened on 2026-08-23.
        static bool s_said = false;
        if (!s_said) {
            s_said = true;
            BVR_LOG("[b1r] scripted: module is OFF - no signals, no arm hide, no hands "
                    "gate, no camera adjustment. Tick it back on in F10 to test any of "
                    "those.");
        }
        return;
    }
    // The lens guard runs before anything can early-return: a scripted camera
    // narrows it on the same frame the forced move begins, and a frame missed
    // there is a frame the player sees boxed.
    clamp_world_fov(playerController);

    // The comfort accumulators advance HERE, before any early return, because
    // observe() is the one thing in this module that runs on every single
    // CalcView. The first version drove them from inside the head-drive block
    // in camera.cpp, which meant they stopped the moment the game took the
    // camera - i.e. during exactly the scenes they exist to handle. That is why
    // the right stick did nothing during a scripted scene.
    const double dt = tick_seconds();
    const int gameDelta = game_yaw_delta(gameYawUnits, dt);
    update_freeze(gameDelta, turnStickX, dt);
    update_scripted_turn(turnStickX, dt);
    // AFTER the turn accumulator advances, so a frame in which you turn and the
    // scene turns nets out the same way round every time.
    update_scripted_recentre(gameDelta);

    void* hands = hands::hands_actor();
    if (!hands) {
        // Hands actor gone (level load, save reload). FAIL CLOSED: a stale
        // "scripted" would leave consumers frozen with nothing on screen to
        // explain why.
        g_anchorOwner = nullptr;
        g_anchorNamesOk = 0;
        g_anchorOk.store(0, std::memory_order_relaxed);
        reset();
        return;
    }

    const bool ok = anchor_check(hands);
    g_anchorOk.store(ok ? 1 : 0, std::memory_order_relaxed);
    if (!ok) {
        reset();
        return;
    }

    const bool rawPrev = g_anim.load(std::memory_order_relaxed) != 0 ||
                         g_forced.load(std::memory_order_relaxed) != 0;

    anim_tick(hands);
    bathysphere_tick(hands);
    if (playerController) forced_move_tick(playerController);

    const bool rawNow = g_anim.load(std::memory_order_relaxed) != 0 ||
                        g_forced.load(std::memory_order_relaxed) != 0;
    note_bridge(rawNow, rawPrev);
    window_tick();
}

bool freeze_game_rot() { return g_freezeOn.load(std::memory_order_relaxed) != 0; }
bool freeze_holding() { return g_freezeHolding.load(std::memory_order_relaxed) != 0; }

bool wants_turn_axis() {
    return freeze_game_rot() || (scripted_turn() && scripted_window());
}

void note_body_yaw(int movedUnits) {
    if (movedUnits) g_bodyYawPending += movedUnits;
}

void set_freeze_game_rot(bool on) {
    if (g_freezeOn.exchange(on ? 1 : 0, std::memory_order_relaxed) == (on ? 1 : 0)) return;
    g_freezeHave = false;
    g_freezeOffsetUnits = 0.0f;
    BVR_LOG("[b1r] scripted: freeze the game's own rotation during play = %s",
            on ? "ON (shake and the auto-pan stop reaching the view)" : "off");
}

float freeze_bleed_deg_per_sec() {
    return g_freezeBleedDegPerSec.load(std::memory_order_relaxed);
}

void set_freeze_bleed_deg_per_sec(float d) {
    if (d < 0.0f) d = 0.0f;
    if (d > 90.0f) d = 90.0f;
    g_freezeBleedDegPerSec.store(d, std::memory_order_relaxed);
}

// Called from the VR gameplay path with the engine's own yaw, BEFORE the head
// residual is added. Returns the yaw the camera should use.
//
// `turnStickX` is the player's own turning and it must always survive - that is
// what lets the freeze work at all without touching the stick, and it keeps
// BRVR's grave 12 (zeroing the stick freezes Controller.Rotation, and forced
// moves steer by it) out of this entirely.
// Bookkeeping only. Called from observe(), which runs on EVERY CalcView -
// unlike the first version, which lived inside the head-drive block and so
// stopped running during exactly the scenes it needed to track across.
// HOW FAR THE GAME MOVED ITS OWN YAW THIS FRAME, with our body transfer taken
// back out. Computed UNCONDITIONALLY and shared, because two consumers need it
// and neither may be able to blind the other by being switched off - which is
// what would happen if this stayed inside the freeze, since the freeze ships
// OFF and the scripted recentre ships ON.
int game_yaw_delta(int gameYawUnits, double dt) {
    // dt == 0 means a real gap (QPC clamps it there), so re-arm rather than
    // difference across it. See the tick_seconds banner for why this used to
    // fire on nearly every frame and silently disable the whole feature.
    if (!g_freezeHave || dt <= 0.0) {
        g_freezeHave = true;
        g_freezePrevYaw = gameYawUnits;
        g_bodyYawPending = 0; // never carried across a gap
        return 0;
    }
    int delta = wrap_rot(gameYawUnits - g_freezePrevYaw);
    g_freezePrevYaw = gameYawUnits;

    // Subtract what OUR OWN body transfer put there. body::on_calcview steers
    // the pawn to follow the head, and that shows up here one frame later as an
    // engine-yaw change with the stick centred - indistinguishable from the
    // game turning you, and absorbing it inverted the head look. Exact integers
    // both ways, so nothing is left over.
    if (g_bodyYawPending) {
        delta = wrap_rot(delta - g_bodyYawPending);
        g_bodyYawPending = 0;
    }
    return delta;
}

// THE SCENE TURNS YOU, SO HAND YOUR OWN TURNING BACK. See the banner on
// g_scriptedRecentre. Only ever REDUCES the player's accumulator toward zero, so
// it can neither add rotation nor overshoot past centre.
void update_scripted_recentre(int gameDelta) {
    const int mode = g_scriptedRecentre.load(std::memory_order_relaxed);
    if (!mode || gameDelta == 0) return;
    if (!scripted_turn() || !scripted_window()) return;
    if (g_scriptedTurnUnits == 0.0f) return;

    const float have = fabsf(g_scriptedTurnUnits);
    const float budget = (mode >= 2) ? have : fabsf(static_cast<float>(gameDelta));
    const float take = budget < have ? budget : have;
    const float cancel = g_scriptedTurnUnits < 0.0f ? -take : take;

    g_scriptedTurnUnits -= cancel;
    g_recentreCancelled += take;

    if (g_scriptedTurnUnits == 0.0f && g_recentreCancelled > 0.0f) {
        BVR_LOG("[b1r] scripted: recentred - %.1f deg of your own turning handed back "
                "to the scene (mode %d)",
                g_recentreCancelled / kRotUnitsPerDegree, mode);
        g_recentreCancelled = 0.0f;
        g_recentreEvents.fetch_add(1, std::memory_order_relaxed);
    }
}

void update_freeze(int delta, float turnStickX, double dt) {
    if (!freeze_game_rot()) {
        g_freezeOffsetUnits = 0.0f;
        g_freezeHolding.store(0, std::memory_order_relaxed);
        return;
    }

    // The exclusions are the whole reason the signals had to land first.
    //   - a scripted scene turns you ON PURPOSE and must not be resisted
    //   - a bathysphere ride is not a scripted animation, so without its own
    //     signal the freeze applies there too and the camera stops following
    //     the sphere. BRVR shipped exactly that bug for as long as the signal
    //     was missing, and could not fix it by level name because the mod does
    //     not know what map it is on.
    //   - your own turn always wins
    if (dt <= 0.0) return; // the shared delta already re-armed on the gap
    const bool excluded = scripted_window() || bathysphere() || fabsf(turnStickX) > 0.02f;

    if (!excluded && delta != 0) {
        // Absorb the delta rather than clamping the value, so the view declines
        // to be turned instead of snapping back.
        g_freezeOffsetUnits += static_cast<float>(delta);
        if (!g_freezeHolding.exchange(1, std::memory_order_relaxed))
            g_freezeEvents.fetch_add(1, std::memory_order_relaxed);
    } else {
        g_freezeHolding.store(0, std::memory_order_relaxed);
    }

    // Optional bleed-back. At 0 the offset is held indefinitely, which is a true
    // freeze; above 0 the view returns to the game's heading at that rate.
    const float bleed = g_freezeBleedDegPerSec.load(std::memory_order_relaxed);
    if (bleed > 0.0f && g_freezeOffsetUnits != 0.0f) {
        const float step = bleed * kRotUnitsPerDegree * static_cast<float>(dt);
        if (fabsf(g_freezeOffsetUnits) <= step)
            g_freezeOffsetUnits = 0.0f;
        else
            g_freezeOffsetUnits -= (g_freezeOffsetUnits > 0.0f ? step : -step);
    }

    if (fabsf(g_freezeOffsetUnits) > kFreezeMaxUnits) {
        g_freezeOffsetUnits = g_freezeOffsetUnits > 0.0f ? kFreezeMaxUnits : -kFreezeMaxUnits;
        static unsigned long long s_lastWarn = 0;
        const unsigned long long now = GetTickCount64();
        if (now - s_lastWarn > 5000) {
            s_lastWarn = now;
            BVR_LOG("[b1r] scripted: freeze offset hit the 60 deg bound - the view and the "
                    "pawn are as far apart as this is allowed to take them. Something is "
                    "turning the player continuously; check the exclusions.");
        }
    }
}

void update_scripted_turn(float turnStickX, double dt) {
    if (!scripted_turn() || !scripted_window()) return;
    if (dt <= 0.0 || fabsf(turnStickX) <= 0.02f) return;
    g_scriptedTurnUnits += turnStickX *
                           g_scriptedTurnRate.load(std::memory_order_relaxed) *
                           kRotUnitsPerDegree * static_cast<float>(dt);
}

bool scripted_turn() { return g_scriptedTurnOn.load(std::memory_order_relaxed) != 0; }

void set_scripted_turn(bool on) {
    if (g_scriptedTurnOn.exchange(on ? 1 : 0, std::memory_order_relaxed) == (on ? 1 : 0))
        return;
    g_scriptedTurnUnits = 0.0f;
    BVR_LOG("[b1r] scripted: turn yourself during a scripted scene = %s", on ? "ON" : "off");
}

float scripted_turn_deg_per_sec() {
    return g_scriptedTurnRate.load(std::memory_order_relaxed);
}

void set_scripted_turn_deg_per_sec(float d) {
    if (d < 0.0f) d = 0.0f;
    if (d > 360.0f) d = 360.0f;
    g_scriptedTurnRate.store(d, std::memory_order_relaxed);
}

// Pure reads. All the advancing happens in observe().
int yaw_adjust_units() {
    // With the module off nothing advances these, so a stale offset would stay
    // applied forever and the switch would not actually take it out of the
    // frame - which is the one thing it exists to do.
    if (!enabled()) return 0;
    int adj = -static_cast<int>(g_freezeOffsetUnits);
    if (scripted_turn() && scripted_window()) adj += static_cast<int>(g_scriptedTurnUnits);
    return adj;
}

float freeze_offset_deg() { return g_freezeOffsetUnits / kRotUnitsPerDegree; }
float scripted_turn_deg() { return g_scriptedTurnUnits / kRotUnitsPerDegree; }

bool world_fov_guard() { return g_worldFovGuard.load(std::memory_order_relaxed) != 0; }

void set_world_fov_guard(bool on) {
    if (g_worldFovGuard.exchange(on ? 1 : 0, std::memory_order_relaxed) != (on ? 1 : 0))
        BVR_LOG("[b1r] worldfov guard %s (a scripted camera %s narrow the world lens "
                "below its gameplay value; snaps so far %u)",
                on ? "ON" : "off", on ? "may not" : "may",
                g_worldFovSnaps.load(std::memory_order_relaxed));
}

void world_fov_readout(float* gameplay, unsigned* snaps) {
    if (gameplay) *gameplay = g_worldFovGameplay;
    if (snaps) *snaps = g_worldFovSnaps.load(std::memory_order_relaxed);
}

bool hide_rig_in_scenes() { return g_hideRig.load(std::memory_order_relaxed) != 0; }

void set_hide_rig_in_scenes(bool on) {
    if (g_hideRig.exchange(on ? 1 : 0, std::memory_order_relaxed) == (on ? 1 : 0)) return;
    BVR_LOG("[b1r] scripted: hide the rig when a scene has nothing for your hands = %s",
            on ? "ON" : "off");
}

float arm_motion_threshold() { return g_armMotionThresh.load(std::memory_order_relaxed); }

void set_arm_motion_threshold(float t) {
    if (t < 0.0001f) t = 0.0001f;
    if (t > 1.0f) t = 1.0f;
    g_armMotionThresh.store(t, std::memory_order_relaxed);
}

int arm_hold_ms() { return g_armHoldMs.load(std::memory_order_relaxed); }

void set_arm_hold_ms(int ms) {
    if (ms < 0) ms = 0;
    if (ms > 10000) ms = 10000;
    g_armHoldMs.store(ms, std::memory_order_relaxed);
}

int scripted_recentre_mode() { return g_scriptedRecentre.load(std::memory_order_relaxed); }

void set_scripted_recentre_mode(int m) {
    if (m < 0) m = 0;
    if (m > 2) m = 2;
    if (g_scriptedRecentre.exchange(m, std::memory_order_relaxed) == m) return;
    BVR_LOG("[b1r] scripted: hand your turning back when the scene turns = %s",
            m == 0   ? "off (scene rotation lands on top of your own turning)"
            : m == 1 ? "wash out as the scene turns"
                     : "drop it all the moment the scene turns");
}

bool hands_moving() { return g_armMoving.load(std::memory_order_relaxed) != 0; }

void arm_motion_readout(float* raw, float* smoothed, int* bone, bool* blind) {
    if (raw) *raw = g_armRaw.load(std::memory_order_relaxed);
    if (smoothed) *smoothed = g_armSmoothed.load(std::memory_order_relaxed);
    if (bone) *bone = g_armBone.load(std::memory_order_relaxed);
    if (blind) *blind = g_armBlind.load(std::memory_order_relaxed) != 0;
}

void note_hand_motion(bool have, float smoothed, float raw, int bone) {
    g_armBone.store(bone, std::memory_order_relaxed);
    g_armBlind.store(have ? 0 : 1, std::memory_order_relaxed);

    // Cannot answer means VISIBLE. The only number available would be a
    // guaranteed zero, which reads as "still" and hides the arms for the whole
    // scene - the failure on record. The scripted release should make this
    // unreachable; it is here because "should" is how that bug happened once.
    if (!have) {
        g_armRaw.store(0.0f, std::memory_order_relaxed);
        g_armSmoothed.store(0.0f, std::memory_order_relaxed);
        g_armMoving.store(1, std::memory_order_relaxed);
        // Say so: this state is invisible from outside - the arms simply never
        // hide. The bone separates the two causes. Throttled, window only.
        static unsigned long long lastBlind = 0;
        const unsigned long long nowBlind = GetTickCount64();
        if (scripted_window() && nowBlind - lastBlind >= 2000) {
            lastBlind = nowBlind;
            BVR_LOG("[b1r] scripted: motion CANNOT BE MEASURED (bone %d) - arms stay "
                    "visible. %s",
                    bone,
                    bone < 0 ? "Both hand clusters are ours; the scripted release is "
                               "not standing the drive down."
                             : "The skeleton could not be reached this frame.");
        }
        return;
    }

    g_armRaw.store(raw, std::memory_order_relaxed);
    g_armSmoothed.store(smoothed, std::memory_order_relaxed);

    // GetTickCount64's ~15.6 ms resolution is a rounding error for a
    // hundreds-of-ms hold. It would NOT be for a per-frame dt - that was s64's
    // round-2 bug, in the freeze accumulator.
    const unsigned long long nowMs = GetTickCount64();
    const bool moving = smoothed > g_armMotionThresh.load(std::memory_order_relaxed);
    if (moving) g_armLastMovingMs = nowMs;

    const int hold = g_armHoldMs.load(std::memory_order_relaxed);
    const bool held = g_armLastMovingMs != 0 &&
                      (nowMs - g_armLastMovingMs) < static_cast<unsigned long long>(hold);
    g_armMoving.store((moving || held) ? 1 : 0, std::memory_order_relaxed);

}

bool want_rig_hidden() {
    // MOTION. scripted_anim() is deliberately not here: round 7 measured it
    // already true on the first frame of the window, so it cannot separate the
    // still parts of a scene from the animated ones. forced_move() is not here
    // either - it covered 0.4 to 1.0 s of scenes that ran 60 to 90 s.
    //
    // A PURE PREDICATE again. It carried a re-check state machine while the hide
    // was by actor scale; the F10 panel calls this from the render thread, so
    // that had to be latched on the game thread and was a standing hazard. The
    // bone collapse removed the need for it entirely.
    return enabled() && hide_rig_in_scenes() && scripted_window() && !hands_moving();
}


bool freeze_hands_in_scenes() {
    return g_freezeHands.load(std::memory_order_relaxed) != 0;
}

void set_freeze_hands_in_scenes(bool on) {
    if (g_freezeHands.exchange(on ? 1 : 0, std::memory_order_relaxed) == (on ? 1 : 0)) return;
    BVR_LOG("[b1r] scripted: the engine owns your hands during a scene = %s",
            on ? "ON" : "off");
}

int hold_ms() { return g_holdMs.load(std::memory_order_relaxed); }

void set_hold_ms(int ms) {
    if (ms < 0) ms = 0;
    if (ms > 5000) ms = 5000;
    g_holdMs.store(ms, std::memory_order_relaxed);
}

const char* rot_follow_name(RotFollow m) {
    switch (m) {
        case RotFollow::HorizontalOnly: return "horizontal only";
        case RotFollow::Neither: return "neither axis";
        default: return "both axes";
    }
}

RotFollow rot_follow() {
    return static_cast<RotFollow>(g_rotFollow.load(std::memory_order_relaxed));
}

void set_rot_follow(RotFollow m) {
    g_rotFollow.store(static_cast<int>(m), std::memory_order_relaxed);
    BVR_LOG("[b1r] scripted: when the game takes the camera, follow its rotation = %s",
            rot_follow_name(m));
}

void publish_panel_state(bool panelUp, bool suppressed) {
    g_panelUp.store(panelUp ? 1 : 0, std::memory_order_relaxed);
    g_panelSuppressed.store(suppressed ? 1 : 0, std::memory_order_relaxed);
}

void apply_rotation_policy(bool gameOwnsCamera, bool sceneActive, int* pitch, int* yaw,
                           int* roll) {
    g_gameOwnsCam.store(gameOwnsCamera && sceneActive ? 1 : 0, std::memory_order_relaxed);
    const RotFollow mode = rot_follow();
    if (!gameOwnsCamera || !sceneActive || mode == RotFollow::Both || !pitch || !yaw ||
        !roll) {
        // The head is driving again (or the player wants the authored motion):
        // drop the reference so the NEXT shot latches its own opening framing
        // rather than inheriting the last one's.
        g_rotRefValid = false;
        return;
    }

    if (!g_rotRefValid) {
        g_rotRefValid = true;
        g_rotRefPitch = *pitch;
        g_rotRefYaw = *yaw;
        g_rotRefRoll = *roll;
        g_rotHolds.fetch_add(1, std::memory_order_relaxed);
        BVR_LOG("[b1r] scripted: game took the camera - holding %s at the opening framing "
                "(pitch %d roll %d%s, window %d)",
                mode == RotFollow::Neither ? "pitch, roll and yaw" : "pitch and roll",
                g_rotRefPitch, g_rotRefRoll,
                mode == RotFollow::Neither ? "" : ", yaw free", scripted_window() ? 1 : 0);
    }

    // Pitch is the comfort hazard in a camera you are not steering, and roll
    // goes with it - a horizon that rolls under a head that did not is the
    // other half of the same complaint. Yaw is what makes looking around during
    // a scene feel alive, so it survives unless the player asks for neither.
    *pitch = g_rotRefPitch;
    *roll = g_rotRefRoll;
    if (mode == RotFollow::Neither) *yaw = g_rotRefYaw;
}

void draw_debug_ui() {
    if (!ImGui::CollapsingHeader("Scripted events (M7 window)")) return;

    bool on = enabled();
    if (ImGui::Checkbox("Scripted-event module ENABLED", &on)) set_enabled(on);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(
            "Untick to take this whole module out of the frame - no engine reads,\n"
            "no camera adjustment, nothing. It is here so a symptom can be blamed\n"
            "on or cleared of this code in one session, instead of a bisect across\n"
            "builds."
            );
    if (!on) {
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.2f, 1.0f),
                           "     MODULE OFF - the arm hide and the hands gate are off too");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(
                "This switch is for A/B testing a symptom against this module, and it\n"
                "takes EVERYTHING here with it - including hiding the arms during a\n"
                "scene and handing your hands back to the engine.\n\n"
                "If you are testing whether this code causes something, that is what\n"
                "you want. If you are testing the arms, tick it back on first."
                );
        return;
    }


    ImGui::Text("signals   anim %d   forcedMove %d   bathysphere %d   window %d",
                scripted_anim() ? 1 : 0, forced_move() ? 1 : 0, bathysphere() ? 1 : 0,
                scripted_window() ? 1 : 0);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(
            "anim         a scripted hand animation is playing (hands+0x594 bit 2)\n"
            "forcedMove   the game is walking you into position (ctl+0x9E0)\n"
            "bathysphere  you are on a ride (pawn+0x464 bit 1) - NOT a scripted scene\n"
            "window       the held union of anim and forcedMove. Ask THIS one.");

    if (anchor_ok()) {
        ImGui::Text("anchor    ok  (hands %08X | pawn %08X | ctl %u)",
                    g_rawHands.load(std::memory_order_relaxed),
                    g_rawPawn.load(std::memory_order_relaxed),
                    g_rawCtl.load(std::memory_order_relaxed));
    } else {
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.3f, 1.0f),
                           "anchor    FAILED - offsets not verified on this build, every "
                           "signal held false");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("These offsets came from the BRVR mod and are re-checked here "
                              "before use.\nThis says the check did not pass - the log names "
                              "which half failed.\nNothing is being driven by them.");
    }

    if (g_panelUp.load(std::memory_order_relaxed))
        ImGui::Text("panel     PausePC/interface screen up, treated as a UI pause: %s",
                    g_panelSuppressed.load(std::memory_order_relaxed) ? "NO (a scene owns it)"
                                                                      : "yes");

    const unsigned long long edge = g_lastAnimEdgeMs.load(std::memory_order_relaxed);
    if (edge) {
        const double ago = static_cast<double>(GetTickCount64() - edge) / 1000.0;
        ImGui::Text("last      scripted animation %s %.1f s ago",
                    g_lastAnimEdgeWas.load(std::memory_order_relaxed) ? "began" : "ended", ago);
    } else {
        ImGui::TextDisabled("last      no scripted animation seen yet this session");
    }

    ImGui::Text("edges     anim %u   forced %u   bathysphere %u   bridged %u",
                g_animEdges.load(std::memory_order_relaxed),
                g_forcedEdges.load(std::memory_order_relaxed),
                g_bathyEdges.load(std::memory_order_relaxed),
                g_bridged.load(std::memory_order_relaxed));
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("BRVR's reference run: ONE anim edge pair in six minutes covering "
                          "combat, plasmid fire,\nfour gene-machine opens, a Little Sister "
                          "rescue and walking. Zero false positives.\nChatter here means the "
                          "offset is wrong, not that this build sees more.");

    const unsigned shapeFails = g_shapeFails.load(std::memory_order_relaxed);
    if (shapeFails)
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.3f, 1.0f),
                           "ctl+0x9E0 failed the bool shape check %u times", shapeFails);

    ImGui::Separator();
    ImGui::TextDisabled("DURING A SCRIPTED SCENE");

    bool fovGuard = world_fov_guard();
    if (ImGui::Checkbox("Keep the world lens at its gameplay width", &fovGuard))
        set_world_fov_guard(fovGuard);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(
            "A scripted camera can narrow the world FOV without CalcView ever\n"
            "reporting it - measured on the bathysphere as 100 -> 80 degrees on the\n"
            "frame you press A. The mod then honestly re-claims the layer at the\n"
            "narrower value, and you see the picture in a small box with black all\n"
            "round it: correct image, full resolution, just not filling the headset.\n\n"
            "This snaps the lens back instead. It only ever WIDENS a narrowed lens\n"
            "back to what gameplay was using, and only while boarding or riding, so\n"
            "weapon zoom is untouched."
            );
    {
        float gameplayFov = 0.0f;
        unsigned snaps = 0;
        world_fov_readout(&gameplayFov, &snaps);
        if (gameplayFov > 1.0f)
            ImGui::Text("lens      gameplay %.1f deg   snaps %u", gameplayFov, snaps);
        else
            ImGui::TextDisabled("lens      no gameplay sample yet");
    }

    bool freezeHands = freeze_hands_in_scenes();
    if (ImGui::Checkbox("The engine owns your hands", &freezeHands))
        set_freeze_hands_in_scenes(freezeHands);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(
            "The authored animation plays instead of your controller, so you cannot\n"
            "drag the rig around in the middle of a scene. The skeleton is handed\n"
            "back to the engine properly, not just left alone."
            );

    bool hideRig = hide_rig_in_scenes();
    if (ImGui::Checkbox("Hide arms while your hands have nothing to do", &hideRig))
        set_hide_rig_in_scenes(hideRig);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(
            "While the game is only walking you into position there is nothing for\n"
            "your hands to do, so arms, hands and weapon are hidden. They come back\n"
            "the moment the rig actually moves.\n\n"
            "Gated on MEASURED movement, not on the game's sequence flag - that\n"
            "flag is already true on the first frame of a scene and stays true\n"
            "through the parts where the hands sit perfectly still.\n\n"
            "If the arms vanish mid-animation, raise the hold under Advanced."
            );
    if (hideRig && scripted_window()) {
        float raw = 0.0f, smoothed = 0.0f;
        int bone = -1;
        bool blind = true;
        arm_motion_readout(&raw, &smoothed, &bone, &blind);
        ImGui::TextDisabled("     rig %s | motion %.4f vs %.4f | bone %d%s",
                            want_rig_hidden() ? "HIDDEN" : "shown", smoothed,
                            arm_motion_threshold(), bone,
                            blind ? "  <- CANNOT MEASURE, showing arms" : "");
        ImGui::TextDisabled("     raw %.4f (two thirds of samples read 0 - the view "
                            "updates faster than the animation)", raw);
    }


    {
        int rc = scripted_recentre_mode();
        const char* kNames[] = {"off - it adds to your turning", "wash it out as the "
                                "scene turns", "drop it the moment the scene turns"};
        ImGui::TextDisabled("When the scene turns you to face something:");
        if (ImGui::Combo("##scriptedrecentre", &rc, kNames, 3))
            set_scripted_recentre_mode(rc);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(
                "A scene that means to point you at a doorway otherwise points you\n"
                "at the doorway PLUS however far you turned yourself - so the more\n"
                "you looked around, the more wrong the framing.\n\n"
                "Wash out is proportional: a scene that turns you a long way takes\n"
                "all of your offset back, one that nudges you takes a nudge. Your\n"
                "free look on the way there is untouched either way.");
        ImGui::TextDisabled("     you have turned %+.0f deg of your own so far",
                            scripted_turn_deg());
    }

    ImGui::Separator();
    ImGui::TextDisabled("COMFORT");

    // ---- the gameplay rotation freeze --------------------------------------
    bool freeze = freeze_game_rot();
    if (ImGui::Checkbox("Ignore screenshake and auto-turn during play", &freeze))
        set_freeze_game_rot(freeze);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(
            "The game turns your view for you: shake from world events, weapon kick,\n"
            "and the pan that swings you onto a group of enemies. This declines it.\n\n"
            "Your own turning always wins - it only holds while your stick is centred.\n"
            "Scripted scenes and bathysphere rides are excluded automatically.\n\n"
            "TRADE: your aim still turns with the game, so the crosshair can sit off\n"
            "centre after a big shake. That is the thing to judge in the headset."
            );
    if (freeze)
        ImGui::TextDisabled("     %s - holding %.0f deg, %u event%s so far",
                            freeze_holding() ? "ACTIVE NOW" : "watching",
                            freeze_offset_deg(),
                            g_freezeEvents.load(std::memory_order_relaxed),
                            g_freezeEvents.load(std::memory_order_relaxed) == 1 ? "" : "s");

    // ---- turning yourself during a scene ------------------------------------
    bool turn = scripted_turn();
    if (ImGui::Checkbox("Right stick can turn you during a scripted scene", &turn))
        set_scripted_turn(turn);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(
            "A scripted scene tells the game to ignore the controller, so a normal turn\n"
            "does nothing during one. This turns the camera directly instead.\n\n"
            "It is never written into the game own aim field - doing that during a\n"
            "scene is what threw the reference mod balcony landing metres off."
            );
    if (turn && scripted_window())
        ImGui::TextDisabled("     turned %.0f deg in this scene", scripted_turn_deg());

    ImGui::Separator();

    static const char* const kFollowItems[] = {
        "Both axes (as authored)",
        "Horizontal only",
        "Neither axis",
    };
    const bool ownsCam = g_gameOwnsCam.load(std::memory_order_relaxed) != 0;
    ImGui::Text("the game owns the camera right now: %s", ownsCam ? "YES" : "no");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("The control below only does anything while this says YES.\n\n"
                          "While your head is driving - which is most scripted scenes now -\n"
                          "pitch and roll come absolutely from you and the game cannot inject\n"
                          "any vertical rotation at all, so there is nothing left to block.");

    int follow = static_cast<int>(rot_follow());
    if (ImGui::Combo("While the game owns it, follow its rotation", &follow, kFollowItems, 3))
        set_rot_follow(static_cast<RotFollow>(follow));
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(
            "COMFORT. Only applies while the GAME is rotating your view instead of your "
            "head - a scripted or cinematic camera. During ordinary play your head\n"
            "already owns pitch and roll, so shake and kick never reach you there and\n"
            "this changes nothing.\n\n"
            "Both axes    the shot plays exactly as authored (default, unchanged)\n"
            "Horizontal   the horizon is held level and only turning reaches you.\n"
            "             Pitch is the hazard in a camera you are not steering;\n"
            "             yaw is what keeps looking around during a scene alive.\n"
            "Neither      the view holds completely still and you turn yourself.");
    ImGui::Text("          held %u shot%s so far this session",
                g_rotHolds.load(std::memory_order_relaxed),
                g_rotHolds.load(std::memory_order_relaxed) == 1 ? "" : "s");

    // The cinematic drive policy. It has existed since session 29 and has been
    // preset-saved as `cineDrive` the whole time, but it has only ever been
    // reachable from the `vrcine drive` console command - there has never been
    // a panel control for it. It belongs with the rest of the authored-camera
    // comfort settings, so it is surfaced here.
    static const char* const kDriveItems[] = {
        "keeps steering the camera",        // CineDrive::Off
        "stands down, shot fully authored",  // CineDrive::Authored
        "adds a look-around delta on top",   // CineDrive::AuthoredLook
    };
    int drive = static_cast<int>(bvr::vr::cine_drive());
    if (ImGui::Combo("During a cutscene, your head", &drive, kDriveItems, 3))
        bvr::vr::set_cine_drive(static_cast<bvr::vr::CineDrive>(drive));
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(
            "keeps steering   pre-session-29: the HMD drives the camera even through"
            " an authored shot, which wobbles the framing."
            "stands down      the shot plays exactly as authored, in stereo. Default."
            "adds a delta     the shot is framed as intended on its first frame and"
            " your own head motion accumulates from there, so you can look"
            " around inside it. No positional term, so the camera can never"
            " leave the shot or walk into geometry.");

    int hold = g_holdMs.load(std::memory_order_relaxed);
    float thr = arm_motion_threshold();
    if (ImGui::SliderFloat("Advanced: arm motion threshold", &thr, 0.001f, 0.2f, "%.4f"))
        set_arm_motion_threshold(thr);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(
            "How much the rig has to move before the arms count as animating.\n\n"
            "Measured over one session: smoothed sits at 0.040 for three quarters\n"
            "of samples and 0.240 for nine tenths, peaking at 77. 0.02 is well\n"
            "clear of the noise floor.");

    int armHold = arm_hold_ms();
    if (ImGui::SliderInt("Advanced: arms stay up (ms)", &armHold, 0, 10000))
        set_arm_hold_ms(armHold);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(
            "How long the arms stay up after the rig last moved.\n\n"
            "THIS IS THE SETTING THAT MATTERS. Two thirds of samples read exactly\n"
            "zero because the view updates far faster than the animation does, so\n"
            "a short hold makes a moving rig look still. 4000 is what BRVR ran in\n"
            "a headset.\n\n"
            "Lower it if the arms linger through long dead stretches; raise it if\n"
            "they flicker out mid-animation.");

    if (ImGui::SliderInt("Advanced: scene hold (ms)", &hold, 0, 1000))
        set_hold_ms(hold);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("A bug fix, not a comfort setting. One scene announces itself "
                          "TWICE - once while\nthe game walks you into position, once while "
                          "the animation plays - and when those\ntwo announcements fail to "
                          "overlap for a single frame, anything reading the pair\nsees the "
                          "scene end and restart. 250 covers every gap BRVR measured.\n"
                          "0 restores the unheld behaviour for comparison.");
}

} // namespace bvr::b1r::scripted
