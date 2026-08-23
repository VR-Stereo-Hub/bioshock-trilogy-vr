#include "game/bioshock1r/scripted.h"

#include "core/hooks/pattern_scan.h"
#include "core/util/log.h"
#include "core/vr/openxr_runtime.h"
#include "game/bioshock1r/body.h"
#include "game/bioshock1r/hands.h"
#include "game/bioshock1r/patterns.h"

#include <windows.h>

#include <imgui.h>

#include <atomic>
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

// Hands.uc declares three consecutive bools at lines 80-82, and UE2 packs
// consecutive bools into one DWORD:
//     bit 0  bFinishedStateAnimations
//     bit 1  AbilityHasBeenReleased
//     bit 2  CurrentlyExecutingScriptedHandAnimationSequence   <-- ours
// BRVR computed the DWORD's address by walking the field list from its proven
// +0x494/+0x498 anchor. THE WALK IS THE WEAK LINK, not the bit, which is why
// anchor_check below re-validates the walk at four points along its length.
constexpr uint32_t kHandsScriptedBits = 0x594;
constexpr uint32_t kScriptedBit = 1u << 2;

// FALSIFIED IN BRVR, M7-S3, recorded here so it is not re-proposed: bit 0 is
// NOT "an animation is playing". It is bFinishedStateAnimations, it tracks the
// Hands state machine's own animations, and gating on it produced OPPOSITE
// failures in two scenes (arms hidden through the whole Little Sister crawl;
// arms stuck visible and frozen on the plasmid balcony). The state
// PlayingScriptedHandAnimation has an EMPTY BODY and never touches the flag.
// DO NOT gate anything on bit 0.

// The anchor slots. Four FNames on the hands actor, computed from the same
// field walk that produces +0x594 - so if all four resolve to real names, the
// walk is validated at four points along its length and +0x594 is standing on
// something.
//   +0x498  HandsOffscreenAnimationName        (BRVR's proven anchor)
//   +0x4B8  InjectingEveAnimationName          (the plasmid injection)
//   +0x4D8  ExorcisingGathererAnimationName    (the rescue)
//   +0x558  CurrentScriptedAnimationName       (what is playing NOW)
constexpr uint32_t kAnchorSlots[4] = {0x498, 0x4B8, 0x4D8, 0x558};

// MEASURED AND NOT KEPT, BRVR M7-S2: +0x558 read 'None' (index 0) for an entire
// run INCLUDING throughout a scripted sequence. So animation-level naming does
// not come from that field and the index-comparison idea it was going to enable
// is unproven. It is an anchor slot here and nothing more.

// Hands.Base. Derived live in this repo already - see patterns.h's
// kHandsCurrentHoldableOffset comment: "hands+0x450 held the pawn = Hands.Base;
// the weapon's own +0x450 holds the hands actor: the attach chain
// weapon -> hands -> pawn is self-consistent".
constexpr uint32_t kHandsBaseOffset = 0x450;

// Pawn.uc line 46 bCannotFall. Pawn's own fields start at the AActor base
// 0x450; lines 13..44 are EXACTLY 32 bools, one full DWORD at +0x460, so the
// next three start a fresh one at +0x464:
//     bit 0  ShouldNotTakeDamageOnNextLanding
//     bit 1  bCannotFall                            <-- ours
//     bit 2  bUseHavokRigidBodyCapsuleCollisions
// THE ORACLE: ShockPlayer defaults bit 2 TRUE, and
// ActionEnableBathysphereModeForPlayer clears it in the same call that sets
// bit 1. Entering a ride must therefore flip BIT 1 UP AND BIT 2 DOWN IN THE
// SAME WRITE - two bits moving in opposite directions at once is not something
// a wrong offset produces by chance. Every edge logs both bits so the oracle
// can be read straight out of the log.
constexpr uint32_t kPawnFlagsBOffset = 0x464;
constexpr uint32_t kCannotFallBit = 1u << 1;
constexpr uint32_t kHavokCapsuleBit = 1u << 2;

// ShockPlayerController.bIsForcingPlayerMove. BRVR could NOT compute this one:
// six interface-typed fields of unknown size sit between the class base and the
// flag. Found by differential probe (M7-S5) and correlated across three events
// whose durations differ by 24x - 1.0 s ("went straight in"), 0.24 s (instant),
// 5.75 s ("the slewing") - each matching an independent tester report.
//
// A LONE BOOL IS EXACTLY 0 OR 1. Anything else means a stale pointer or a wrong
// offset, which is not hypothetical: BRVR caught its own bathysphere read doing
// precisely that. The shape check in forced_move_tick refuses anything wider.
constexpr uint32_t kCtlForcedMoveOffset = 0x9E0;

// ---- guarded memory helpers (no C++ objects in an SEH frame) ---------------

bool read_u32(const void* obj, uint32_t off, uint32_t* out) {
    if (!obj) return false;
    const uint8_t* p = static_cast<const uint8_t*>(obj) + off;
    if (!bvr::pattern_scan::is_memory_valid(p, 4)) return false;
    __try {
        *out = *reinterpret_cast<const uint32_t*>(p);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool read_ptr_at(const void* obj, uint32_t off, void** out) {
    if (!obj) return false;
    const uint8_t* p = static_cast<const uint8_t*>(obj) + off;
    if (!bvr::pattern_scan::is_memory_valid(p, sizeof(void*))) return false;
    __try {
        *out = *reinterpret_cast<void* const*>(p);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// ---- published state (game thread writes, overlay thread reads) ------------

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

// ---- the rotation-follow policy -------------------------------------------
//
// Default Both, which is bit-for-bit today's behaviour: the policy is a no-op
// until the player picks otherwise in F10.
std::atomic<int> g_rotFollow{static_cast<int>(RotFollow::Both)};

// The reference latched on the frame the game took the camera. Held absolutely
// rather than differenced, so an authored pitch slew is removed entirely and
// the horizon stays where it was when the shot began.
bool g_rotRefValid = false;
int g_rotRefPitch = 0;
int g_rotRefYaw = 0;
int g_rotRefRoll = 0;
std::atomic<unsigned> g_rotHolds{0}; // shots this policy has levelled

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
        if (!read_u32(hands, kAnchorSlots[i], &idx)) continue;
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
                kHandsScriptedBits);
    return ok;
}

// ---- the three raw signals -------------------------------------------------

void anim_tick(const void* hands) {
    uint32_t bits = 0;
    if (!read_u32(hands, kHandsScriptedBits, &bits)) return;
    g_rawHands.store(bits, std::memory_order_relaxed);

    const int want = (bits & kScriptedBit) ? 1 : 0;
    if (want == g_anim.load(std::memory_order_relaxed)) return;

    g_anim.store(want, std::memory_order_relaxed);
    g_lastAnimEdgeMs.store(GetTickCount64(), std::memory_order_relaxed);
    g_lastAnimEdgeWas.store(want, std::memory_order_relaxed);
    g_animEdges.fetch_add(1, std::memory_order_relaxed);
    BVR_LOG("[b1r] scripted: %s  (hands+0x%03X = %08X)",
            want ? "*** SCRIPTED ANIMATION BEGAN ***" : "--- scripted animation ended ---",
            kHandsScriptedBits, bits);
}

void bathysphere_tick(const void* hands) {
    void* pawn = nullptr;
    if (!read_ptr_at(hands, kHandsBaseOffset, &pawn) || !pawn) return;
    // Hands.Base must be the player pawn, and this tree already has that test:
    // body::is_gameplay_view compares the actor's vtable to kShockPlayerVtableRva.
    if (!body::is_gameplay_view(pawn)) return;

    uint32_t bits = 0;
    if (!read_u32(pawn, kPawnFlagsBOffset, &bits)) return;
    g_rawPawn.store(bits, std::memory_order_relaxed);

    const int want = (bits & kCannotFallBit) ? 1 : 0;
    if (want == g_bathy.load(std::memory_order_relaxed)) return;

    g_bathy.store(want, std::memory_order_relaxed);
    g_bathyEdges.fetch_add(1, std::memory_order_relaxed);
    // THE ORACLE, logged on every edge: entering a ride must raise bit 1 while
    // LOWERING bit 2 in the same write. If the log ever shows them moving
    // together, this offset is not what the derivation says it is.
    const int havok = (bits & kHavokCapsuleBit) ? 1 : 0;
    BVR_LOG("[b1r] scripted: bathysphere %s  (pawn+0x%03X = %08X, bCannotFall=%d "
            "havokCapsule=%d - oracle %s)",
            want ? "ON" : "off", kPawnFlagsBOffset, bits, want, havok,
            want != havok ? "HOLDS (bits oppose)"
                          : "BROKEN - both bits agree, suspect the offset");
}

void forced_move_tick(const void* controller) {
    uint32_t v = 0;
    if (!read_u32(controller, kCtlForcedMoveOffset, &v)) return;
    g_rawCtl.store(v, std::memory_order_relaxed);

    // Shape check, every read. A lone bool is exactly 0 or 1.
    if (v > 1) {
        if (g_shapeFails.fetch_add(1, std::memory_order_relaxed) == 0)
            BVR_LOG("[b1r] scripted: SHAPE CHECK FAILED - ctl+0x%03X reads %08X, which is "
                    "not a bool. Stale controller or wrong offset; forced-move held false.",
                    kCtlForcedMoveOffset, v);
        if (g_forced.exchange(0, std::memory_order_relaxed))
            BVR_LOG("[b1r] scripted: --- forced move done --- (dropped by the shape check)");
        return;
    }

    const int want = static_cast<int>(v);
    if (want == g_forced.load(std::memory_order_relaxed)) return;

    g_forced.store(want, std::memory_order_relaxed);
    g_forcedEdges.fetch_add(1, std::memory_order_relaxed);
    BVR_LOG("[b1r] scripted: %s  (ctl+0x%03X = %u)",
            want ? "forced move BEGAN" : "--- forced move done ---", kCtlForcedMoveOffset, v);
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
    if (want != g_window.load(std::memory_order_relaxed))
        g_window.store(want, std::memory_order_relaxed);
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
    g_anim.store(0, std::memory_order_relaxed);
    g_forced.store(0, std::memory_order_relaxed);
    g_bathy.store(0, std::memory_order_relaxed);
    g_window.store(0, std::memory_order_relaxed);
}

void observe(void* playerController) {
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

void apply_rotation_policy(bool gameOwnsCamera, bool sceneActive, int* pitch, int* yaw,
                           int* roll) {
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

    static const char* const kFollowItems[] = {
        "Both axes (as authored)",
        "Horizontal only",
        "Neither axis",
    };
    int follow = static_cast<int>(rot_follow());
    if (ImGui::Combo("When the game takes the camera, follow its rotation",
                     &follow, kFollowItems, 3))
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
    if (ImGui::SliderInt("Scene hold after both signals drop (ms)", &hold, 0, 1000))
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
