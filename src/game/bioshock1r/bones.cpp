// M7-v2 bone drive. See bones.h for the contract and ENGINE_NOTES "Skeleton /
// bone internals" for every offset's derivation.
//
// Write protocol, decided by the live probes (2026-07-26):
//  - The bone array is re-evaluated lazily behind a dirty flag. Our write runs
//    in the CalcView detour (after the engine tick placed everything), then
//    CLEARS the dirty flag so a render-side evaluate-if-dirty cannot rebuild
//    the pose over our values in the same frame.
//  - Before writing we compare the anchor bone against what WE last wrote: if
//    it changed, the engine re-evaluated since last frame and the array holds
//    a fresh animated pose - recapture it as the reference. If it did not
//    change, the engine skipped evaluation and the reference stays. Either
//    way the drive composes from an ENGINE pose, never from its own output
//    (no feedback accumulation).
//  - Disabling sets the dirty flag so the engine's next evaluation restores
//    its own pose - no restore bookkeeping of ours can go stale.

#include "game/bioshock1r/bones.h"

#include "core/gfx/frame_inspector.h"
#include "core/gfx/hud_capture.h" // backbuffer_dims: the lens laws are aspect-parameterised
#include "core/util/log.h"
#include "core/vr/openxr_runtime.h" // cine_drive/name for the vrbones status residue line
#include "game/bioshock1r/camera.h"
#include "game/bioshock1r/hands.h"
#include "game/bioshock1r/hands_state.h"
#include "game/bioshock1r/patterns.h"

#include <windows.h>

#include <imgui.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace bvr::b1r::bones {
namespace {

const uint8_t* g_imageBase = nullptr;

// Havok hkQsTransform: the pos/scale w lanes carry engine-owned values (one
// live bone held 35.02 in pos.w) - the drive writes pos.xyz and quat only.
struct Qts {
    float p[4];
    float q[4];
    float s[4];
};
static_assert(sizeof(Qts) == 48, "hkQsTransform is 48 bytes");

constexpr int kMaxBones = 128;

// Cached skeleton, revalidated every use.
void* g_skelInst = nullptr;
Qts* g_bones = nullptr;
int g_boneCount = 0;

// Reference pose (engine-evaluated) + the anchor value we last wrote.
Qts g_ref[kMaxBones];
bool g_refValid = false;
Qts g_lastWrittenAnchor[2]; // per hand
bool g_hasWritten[2] = {false, false};

// s70: THE CANONICAL REST POSE IS GONE, and so is the machinery around it.
//
// s68 captured one snapshot per holdable at the WeaponIdling edge and eased the
// rig back to it whenever an adopted animation ended. It existed because s67's
// per-state animation mask cut adoption at `WeaponFiring`, which Hands.uc leaves
// at the TOP of the recoil - so the reference stuck at the apex and something
// had to put it back.
//
// BRVR has no rest pose, no restore and no blend, because it never acquires the
// defect: its adoption is a size threshold plus a HOLD WINDOW
// (`playing = (now - lastBig) < HandAnimHoldMs`), so it keeps tracking for 1.2 s
// past the animation's last big frame and the reference lands on the SETTLED
// pose by construction. Delete the mask and the reason for all of this goes with
// it. See ENGINE_NOTES "Session 70".
//
// g_lastKnownState survives only as telemetry - nothing gates on it now.
hands_state::State g_lastKnownState = hands_state::State::Unknown; // for the log line

// Everything the last drive() wrote, for reapply() (the stereo second pass).
struct CachedBone {
    int idx;
    float p[3];
    float q[4];
    float s[3];
    bool writeScale;
    // False for the weapon-attach bone when the attach-rotation lane is off:
    // the second pass must replay exactly what the first pass wrote, or the
    // right eye bakes a rotation the left eye never got. CachedSleeve already
    // replays position-without-rotation for the same reason.
    bool writeRot;
};
CachedBone g_cache[kMaxBones];
int g_cacheCount = 0;
struct CachedSleeve {
    int idx;
    float p[3];
    float s[3];
};
CachedSleeve g_cacheSleeve[8];
int g_cacheSleeveCount = 0;

// Session 20 idle-sway kill (default ON; `vrhands swaykill on|off`): freeze
// the drive's reference pose against the idle animation's breathing. A fresh
// engine pose is adopted only when either wrist anchor moved past the
// thresholds - real animations (equip/reload/melee) pass through and
// re-freeze when they settle; the measured idle wobble (+-1.2 deg barrel
// direction, sub-UU positions) stays out. Acts only on the DRIVEN rig; the
// weapon's own skeleton (pump, cylinder) animates untouched.
std::atomic<bool> g_swayKill{true};
// s67: THESE ARE NOW BRVR'S NUMBERS, AND THE OLD ONES WERE THE BUG.
//
// The mechanism here and BRVR's HandAnim are the same shape - a threshold that
// says "this delta is a real animation, adopt it" plus a hold window so the
// freeze lands on the settled pose. Only the constants differed, and they
// differed a long way:
//
//                       BRVR                    here (before)
//   adopt threshold     HandAnimMinDeg  5 deg   12 deg
//   hold window         HandAnimHoldMs  1200ms  600 ms
//
// Session 20 measured the IDLE envelope at dpos 3.01 UU / dang 4.6 deg, then
// set the threshold to 12 for margin. That margin is the defect: it is more
// than double the idle peak, so it rejects REAL animation as well as breathing,
// and the rig goes rigid where BRVR's stays natural. BRVR's 5.0 sits just above
// the same measured 4.6 - the two projects measured the same envelope and only
// this one rounded away from it. Reported by the tester, s67: "turning off the
// arm animation causes the arms to be straight instead of their natural
// position", and "my mod killed sway somehow and left hand animations in".
// That is exactly what a 5 deg threshold does and a 12 deg one cannot.
//
// TUNABLE AT RUNTIME (`vrbones sway`, F10) because the margin above idle is now
// only 0.4 deg: if breathing starts leaking back in, this is the knob, and it
// must not need a rebuild to find out.
//
// The POSITION term has no BRVR counterpart - BRVR thresholds on angle alone.
// It is kept because it can only ADD adoption, which is the direction of this
// change, and dropping it outright is a second variable in one test.
std::atomic<float> g_swayPosThreshUu{6.0f};
std::atomic<float> g_swayAngThreshDeg{5.0f};    // s70: BRVR's HandAnimMinDeg
// s70: 5.0, WHICH IS BRVR'S, AND THE 25 IT REPLACES WAS AN ARTEFACT OF A BAD
// PROBE.
//
// s67 set this to 25 with a specific justification: "on this rig the shotgun's
// idle crosses 5 and re-triggers adoption forever - reported as 'does an
// animation cycle, stops, then does another and repeats'." That observation was
// real, but the cause was not the threshold. The probe was sampling bone 43, the
// WEAPON ATTACH - the one bone the engine keeps animating under freeze, whose
// idle drift BRVR measures at 1-5 deg with PEAKS OF 41-135. An idle that "crosses
// 5" on that bone says nothing about whether the hand is animating.
//
// With the probe moved to the wrists (see kProbe below), the measurement this
// number has to clear is the one both projects took independently and agreed on:
// an idle envelope of 4.6 deg. BRVR sits at 5.0, just above it, and has done so
// in a shipping build the tester calls "pretty much perfect".
//
// 25 was not merely cautious - it is above the per-shot wrist movement of some
// weapons entirely, which is BRVR's own recorded failure ("the tester saw recoil
// on every gun EXCEPT the Tommy gun") and was reported here too.
//
// STILL TUNABLE (`vrbones sway`, F10), and ANIMREJECT reports the largest thing
// rejected in the last 3 s so the number can be set from data rather than
// guessed a fourth time.
std::atomic<unsigned> g_swaySettleMs{1200};     // BRVR HandAnimHoldMs
uint64_t g_lastBigDeltaMs = 0;
uint64_t g_lastBigDeltaHandMs[2] = {0, 0}; // s70c: per hand, as BRVR's lastBig[hand]

// ---- s70d: PIN THE ANCHOR'S ORIENTATION, AND ONLY ITS ORIENTATION ----------
//
// MEASURED, the tester, on the build that deleted the s69 pin: "the animation is
// correct but it points like 90 degrees left when the animation is going", while
// casting, with the idle pose correct.
//
// That is the s69 defect exactly, and s69's own note named the mechanism before
// this session deleted it: the ACTOR's rotation is set from the controller on
// the assumption that the anchor still carries the orientation it was CAPTURED
// with. An adopted animation turns the anchor; the cluster is replayed at that
// turned orientation; the actor knows nothing about it, so the whole hand points
// somewhere else for the length of the animation.
//
// s70 removed the pin as compensation for the per-state mask. Half of that was
// right - the POSITION pin was fighting an anchor on bone 43, which the engine
// animates, and moving the anchor to the wrist fixed that at source. The
// ROTATION pin was not compensation for anything; it answers a problem that
// survives the anchor fix, and the tester had already confirmed it working
// ("position and rotation are correct").
//
// ROTATION ONLY. Position deliberately still follows the animation, because the
// tester's report is that the MOTION is correct - it is only the pointing that
// is wrong. Pinning position too would flatten the cast animation this exists to
// let through.
//
// BRVR has no equivalent, and that is not evidence against it: BRVR's threshold
// is 5.0 against a plasmid animation of 2.6-5.0 deg, so its plasmid hand barely
// adopts at all and never reaches this case. A rigid hand cannot be re-pointed.
float g_pinAnchorQ[2][4] = {{0, 0, 0, 1}, {0, 0, 0, 1}};
bool g_pinValid[2] = {false, false};
std::atomic<bool> g_animPinRot{true};
// s70c: THE LEFT HAND NEEDS ITS OWN THRESHOLD, and 5.0 is why Telekinesis and
// Electrobolt behaved differently from every weapon.
//
// MEASURED, the tester's 2026-08-28 23:48 run: with a plasmid equipped
// ANIMREJECT reported 2.6, 3.2, 3.3, 3.4, 4.5, 4.9 and 5.0 deg. That is a whole
// animation living inside the band BRVR calls idle breathing - so a 5.0
// threshold rejects it outright (Telekinesis: no animation at all) or admits it
// erratically as it flickers across the line (Electrobolt: motion that starts
// and stops).
//
// It is not a tuning accident, it is the rig: the left cluster is bones 6-21 and
// s68 dumped the names - 6 is the palm and 7-21 are NOTHING but finger joints.
// There is no wrist or forearm in it, so a plasmid animation that is mostly
// fingers barely rotates the one bone the threshold measures. BRVR's 41-135 deg
// figures are the RIGHT cluster's wrist during a reload; they do not describe
// this hand and were never meant to.
//
// 2.0 sits under every rejected plasmid reading above and clear of the sub-1 deg
// noise. A FIRST ESTIMATE from one run, not a measurement of the idle floor -
// ANIMREJECT now reports per hand, so the next run can set it properly.
std::atomic<float> g_swayAngThreshLeftDeg{2.0f};

// ---- s70: BRVR's EQUIP SETTLE, ported with its own numbers -----------------
//
// All four are BRVR's, and three of them are answers to defects this tree has
// re-derived the hard way (see ENGINE_NOTES "Session 70").
//
// g_keyDebounceMs   BRVR WeaponKeyDebounceMs 150. The engine parks
//                   CurrentHoldable at NULL for a frame during fire/pump, so an
//                   undebounced key change fires on animation frames. MEASURED
//                   in BRVR: 11 changes in 3 minutes against 2 real switches.
// g_settleStillUu   BRVR kSettleStillUnits 0.05. How still the WRIST POSITION
//                   has to be, per frame, to count as settled. POSITION, not
//                   angle - s68 probed angles against a 25 deg animation
//                   threshold and the test never gated at all.
// g_settleStillMs   BRVR kSettleStillMs 150. How long that stillness must hold.
// g_settleMinMs     BRVR kSettleMinMs 350. A FLOOR before stillness may count,
//                   because many draw animations pause part-way through and 150
//                   ms of quiet inside a pause is not the end of the animation.
// g_settleCeilMs    BRVR WeaponSwitchSettleMs 600. A CEILING, not a duration -
//                   a rig that settles fast is picked up fast, and one that
//                   never settles cannot wedge the window open.
std::atomic<unsigned> g_keyDebounceMs{150};
std::atomic<float> g_settleStillUu{0.05f};
std::atomic<unsigned> g_settleStillMs{150};
std::atomic<unsigned> g_settleMinMs{350};
std::atomic<unsigned> g_settleCeilMs{600};

// The settle window's own state. g_settleN > 0 means a window is open.
uint64_t g_settleStartMs = 0;   // when the switch was seen
uint64_t g_settleLastMoved = 0; // last frame the wrist moved more than the threshold
float g_settlePrevWrist[3] = {0.0f, 0.0f, 0.0f};
bool g_settleHavePrev = false;
// s70b: THERE IS NO POSE AVERAGING HERE, AND THE ATTEMPT AT IT WAS A BUG.
//
// BRVR accumulates ONE POINT over the settle window - the wrist position - and
// uses the mean for its two-hand GRAB POINT (`g_anchorPos[oh]`), a feature this
// tree does not have. Its POSE capture is `CaptureClusterRef`, which takes the
// LIVE FRAME, still or not.
//
// s70 generalised the mean from that one point to the whole cluster. That is
// not a pose: averaging bone positions component-wise does not preserve BONE
// LENGTHS, so the result is a skeleton that never existed. Measured immediately,
// 2026-08-28 23:36:31 - the wrench captured "the MEAN of the window" over 48
// engine evaluations spanning its entire equip animation, and the tester
// reported the hand "super far away and super duper streched like a slenderman
// hand". That is what an averaged skeleton looks like.
//
// The lesson generalises: a mean is only meaningful over a space where the
// average of two valid values is itself valid. Positions of ONE point qualify.
// A skeleton's bone array does not.
int g_settleN = 0;

// Drop every scrap of settle state. Called wherever the reference dies - a new
// bone array, a new world, an explicit release - because a settle window that
// outlives the skeleton it was measuring is a window onto a dead rig.
void settle_reset() {
    g_settleStartMs = 0;
    g_settleLastMoved = 0;
    g_settleHavePrev = false;
    g_settleN = 0;
}
// Telemetry (1 Hz while frozen): the probe deltas the threshold judges, so
// the thresholds are set from measured idle amplitude, not guesses.
uint64_t g_lastSwayTlmMs = 0;

// Session 19: the whole INACTIVE hand collapses too - the drive poses only
// the active hand's cluster, so the other one stays engine-animated at the
// eye anchor and reads as a ghost hand. Cache mirrors g_cacheSleeve so the
// stereo second pass replays the same writes. writeScale is false for the
// weapon-attach bone: it hides by translation, never by scale (see drive()).
std::atomic<bool> g_hideInactive{true};

// Session 67: does the WEAPON-ATTACH bone (43) take our rotation, or only our
// position? Today it takes both, because the rigid write loop treats it as an
// ordinary cluster bone. BRVR does not, and says so twice independently:
//
//   "Bone 43 takes the cluster's POSITION and nothing else. It is the weapon
//    attachment ... rotation is untested and scale is known fatal."
//        - BRVR planning/DECISIONS.md, 2026-08-10 (M6-S1)
//   "WriteCluster already handles it correctly: position only, never rotation,
//    never scale."
//        - BRVR planning/features/skeletal-hand-drive.md, part 4 B
//
// THIS TREE ALREADY LEARNED THE SCALE HALF OF THAT LESSON AND NOT THE ROTATION
// HALF. Bone 43 is excluded from every scale write (scale_selects() and the
// hide path) because session 16 measured that the engine's attachment path
// INVERSE-DECOMPOSES the chain - so a scale we write comes back out of the
// weapon's transform and blows it up near-plane. If the same decomposition
// carries rotation, a quat we write to 43 is applied to the weapon a SECOND
// time, and the rendered gun turns about twice as far as the controller did.
//
// That is the shape of the standing viewmodel defect, and it is the only shape
// that fits all of it: position is written once and TRACKS CORRECTLY (user,
// s67: translation is fine); rotation is the channel that desyncs; the error is
// ZERO at exactly one wrist pose, because there qtc is identity and identity
// applied twice is still identity; and no position offset can cancel an angular
// gain, which is why s11's grip-offset tuning "doesn't matter ... instead it
// creates other problems". BRVR's own note for the same class of bug reads
// "90 twice is 180. The fix is to suppress the second rotation."
//
// ---- TESTED IN A HEADSET, s67. THE DOUBLING THEORY IS DEAD. ----------------
//
// With this OFF (position only, the BRVR shape) THE GUN STOPS ROTATING
// ALTOGETHER. So the engine is NOT re-deriving the weapon's rotation from the
// chain: bone 43's quat IS the weapon's rotation, we are the only writer of it,
// and it is applied exactly once. Rotation does not behave like scale here, and
// BRVR's "rotation is untested" caution turns out to cost the whole feature.
//
// THAT ALSO ANSWERS IT FOR BRVR, which only gets away with position-only
// because its ACTOR carries the rotation - it drives Hands.Rotation and freezes
// the cluster. This tree leaves the actor engine-placed and rotates the cluster
// instead, so the attach bone's quat is load-bearing and must be written.
//
// KEEP THE TOGGLE. It is three lines, it defaults to today's behaviour, and it
// is the only way to re-run the test if anyone proposes the doubling theory
// again - which they will, because it is a good theory that happens to be
// wrong. Do not delete it and do not "fix" the default.
//
// What the same session DID find, by the same route: the viewmodel error is a
// LEVER ARM, not an angular gain. Rolling the controller 360 deg about the
// barrel traces a circle - zero error at 0 deg, PEAK AT 180, back to zero at
// 360 - which is |R(t)v - v| = 2|v_perp|sin(t/2) and nothing else. The fix is
// the per-weapon grip offset in hands.cpp, tuned at the 180 deg pose where the
// error is doubled and therefore easiest to null.
std::atomic<bool> g_attachRot{true};

struct CachedHidden {
    int idx;
    float p[3];
    float s[3];
    bool writeScale;
};
CachedHidden g_cacheHidden[32];
int g_cacheHiddenCount = 0;
int g_hiddenHand = -1; // whose cluster is collapsed right now (game thread)
// Session 29: hoisted out of drive() (it was a function-static there) so
// release() can restore a collapsed sleeve. A latch only drive() could see was
// a latch only drive() could ever clear - which is precisely the shape of bug
// release() exists to fix.
bool g_wasCollapsed = false;
int g_collapsedHand = -1; // whose sleeve g_wasCollapsed refers to

// ---- s64: WHICH CLUSTERS ARE OURS -------------------------------------------
//
// Per hand, set by drive() and cleared by release(). The s64 motion gate reads
// a WRIST to answer "is the engine animating the rig right now", and a wrist we
// are writing reports our own rigid transform - bit-for-bit identical every
// frame while the controller is still, so the delta is not merely small, it is
// exactly zero. BRVR spent 189 and 223 consecutive `raw 0.0000` samples
// learning that (ArmHide.cpp, MotionBone banner) and its rule is the one this
// mirrors: sample the wrist of a cluster we are NOT writing, or say we cannot.
//
// BOTH FLAGS ARE SET BY ONE drive() CALL. The driven hand's cluster is posed,
// and with hide-inactive on the OTHER hand's cluster is pinned too - and unlike
// BRVR's HideBone, which pins each bone at that cluster's own wrist and so
// leaves the wrist write a no-op, ours pins every bone at the DRIVEN target.
// A collapsed hand is therefore not honest here and must count as written.
bool g_clWritten[2] = {false, false};

void* g_cacheSkelInst = nullptr;
uint64_t g_cacheMs = 0;

// Both clusters are baked (patterns.h) after live measurement; the lcluster
// command stays as a runtime override for future rig experiments.
std::atomic<int> g_lFirst{patterns::kBoneLClusterFirst}, g_lLast{patterns::kBoneLClusterLast},
    g_lAnchor{patterns::kBoneLWrist};
std::atomic<int> g_rAnchorOverride{-1};

// Session 61: per-cluster viewmodel scale (1.0 = authored; the BS2-shaped
// lever the s16 dead ends never actually tested). The cluster is still moved
// rigidly, but the anchor-relative translations shrink by s for EVERY cluster
// bone - the weapon-attach (43) and muzzle (44) bones MOVE with the scaled
// hand - while the hkQsTransform .s channel is written only for the bones the
// probe mode selects, and NEVER for 43/44: the s16 test wrote 43's .s at its
// authored value, and BS2's later live bisection localised the identical
// weapon blowup to the attach pivot's own scale CHANNEL being consumed
// (inverse-decomposed) by the attachment math. Leaving the channel engine-
// owned entirely is the untested cell this probe exists to measure.
// Scale rides the per-frame drive - one-shot pokes were BS2-proven not to
// render reliably - and the reference recapture PINS the scale rows of bones
// we scale-wrote (see the adopt block) so g_ref can never re-adopt our own
// write and compound refS * s^n.
// The default was the user's session-61 in-headset calibration (2026-08-14,
// 0.793, "everything looks perfect"). 2026-08-22: BRVR ships HandsScale=0.8 and
// the user wants the two mods to match exactly, so the shipped default is 0.80.
// State plainly what that is worth: 0.793 -> 0.80 is under one percent and is
// almost certainly not what the "hands read tiny" report was about. It is
// adopted for parity; the number that actually moves is the camera height
// (camera.cpp g_headOffUpUu), and `HandsScale` in BioshockVR.ini exists so the
// next headset session can find the real one without a rebuild per guess.
//
// NOTE the mechanism differs from BRVR's even at the same number: BRVR writes
// the AHands ACTOR's DrawScale (+0x2AC), while this cluster scale compresses
// bone translations toward the wrist anchor. Actor DrawScale is deliberately
// NOT ported for the hands - the rig is positioned by writing bone translations
// here, so an actor-level scale would silently scale our own writes with it.
// 1.0 = authored size.
std::atomic<float> g_scale[2] = {0.80f, 0.80f}; // [0] left, [1] right
// Probe modes (vrbones scalemode <n>): which cluster bones get the .s write.
// 0 = all except attach/muzzle (intended ship mode), 1 = fingers only (wrist
// keeps authored .s), 2 = wrist only, 3 = translation-only (no .s anywhere -
// pure skeleton compression).
std::atomic<int> g_scaleMode{0};
// Engine scale-restamp telemetry: BS2's engine never restamps the scale
// channel (pin-to-ref correct), Infinite's does (adopt correct). Counted at
// reference adoption: a scale-written bone whose bank value no longer matches
// what we last wrote was restamped by the engine.
std::atomic<uint32_t> g_scaleRestamps{0};
float g_lastWrittenS[kMaxBones][3];
bool g_scaleWrote[kMaxBones] = {};

// Which bones get the .s channel write in a given probe mode. Right hand:
// attach (43) and muzzle (44) are excluded in EVERY mode - their scale
// channel stays engine-owned (see the g_scale block comment).
bool scale_selects(int mode, int hand, int idx, int first) {
    if (mode == 3) return false;
    if (hand == 1 &&
        (idx == patterns::kBoneWeaponAttach || idx == patterns::kBoneRClusterLast))
        return false;
    if (mode == 1) return idx != first; // fingers only (wrist = cluster first)
    if (mode == 2) return idx == first; // wrist only
    return true;
}

// Session 61: uniform weapon scale - drives the HOLDABLE's own
// SkeletonInstance (BS2 session-41's shipped design, adapted; the weapon has
// carried its own skeleton here since s20, it is how the muzzle bone was
// found). Every bone: translation *= ws (uniform about the component origin,
// which IS the grip - R_Grip sits at (0,0,0)), quat adopted from the engine
// per frame (drum-spin/reload animations keep playing while scaled), scale
// channel = captured reference * ws, pinned exactly like the cluster scale.
// At ws == 1.0 the lane drops the skeleton entirely and restores the
// captured pose - zero interference at the default.
// 0.760 was the s61 in-headset calibration; 0.80 is BRVR's GunScale, adopted
// 2026-08-22 for parity (see the g_scale note above for what a 5% change is
// worth). 1.0 = authored, and at 1.0 the lane drops entirely.
std::atomic<float> g_wScale{0.80f};
void* g_wHoldable = nullptr; // the actor the lane is bound to
void* g_wSkelInst = nullptr;
Qts* g_wBones = nullptr;
int g_wBoneCount = 0;
Qts g_wRef[kMaxBones]; // captured pose (restored on drop)
Qts g_wAnim[kMaxBones]; // adopted engine p/q; scale rows pinned to g_wRef
Qts g_wWritten[kMaxBones]; // what we last wrote (adoption + reapply source)
bool g_wWrittenValid = false;
uint32_t g_wAdopts = 0;
uint32_t g_wDrives = 0;

// 2026-08-22: the same weapon scale, for holdables that have NO skeleton to
// drive. The WRENCH is one (+0x3FC is null - rigid melee mesh), and before this
// the lane simply logged "stays unbound" and left it at authored size forever,
// which is the one place this mod did visibly less than BRVR. BRVR has never
// had the problem because it scales the weapon ACTOR (DrawScale +0x2AC), which
// does not need bones at all.
//
// Semantics match the skeleton lane: g_wScale is a MULTIPLIER on the actor's
// authored DrawScale, captured at bind. (BRVR writes its GunScale absolutely;
// for BS1 weapons the authored value is 1.0, so the two agree - the captured
// ref is logged so a weapon where it is not shows up rather than hides.)
void* g_dsHoldable = nullptr; // the actor the lane is bound to
float g_dsRaw = 1.0f;         // the authored field value, restored on release
float g_dsWrote = 0.0f;       // what we last wrote (0 = nothing written yet)

std::atomic<bool> g_collapse{true}; // hide the driven arm's sleeve
std::atomic<uint32_t> g_writes{0};
std::atomic<uint32_t> g_reapplies{0};
std::atomic<int> g_lastHand{-1};
char g_status[160] = "idle";

// Render-lock (session 13): solve the anchor against the renderer's OWN
// foreground transform (captured per frame from the vm draws' cb0) so the
// rig lands on the world-correct pixel. See patterns.h "Foreground scene".
// DEFAULT OFF since session 21 part 2 - the user's in-headset verdict:
// "lock off is exactly what I was looking for, the aim is in tune with the
// model" - the lock's own correction WAS the +-90-flipping laser-vs-gun
// drift reported in session 20 (its model was calibrated against the old
// fg composition and miscorrects laterally at large hand yaws).
// `vrbones lock abs` remains the live A/B back to the old behavior.
std::atomic<bool> g_gunXform{false};
float g_gunXfLast[3] = {0.0f, 0.0f, 0.0f};
bool g_gunXfWrote = false;
std::atomic<int> g_renderLock{0};         // 0 off, 1 abs (true position), 2 diff
                                          // (head-split cancel only)
// Correction gains. Session 13 measured "gain 0.5 lands, 1.0 doubles" and
// blamed a rigid-path rebake; session 14 decomposed that 1.79x overshoot as
// (model depth-scale error 1.63, from the section-contaminated eye) x (true
// rebake ~1.1). With the model's depth scale now physically calibrated, the
// remaining rebake factor is ~1.1 on BOTH axes, so both gains default to
// ~1/1.1. Separate knobs kept - the axes are measured by different
// instruments (simhead sweeps vs size/parallax A/Bs).
std::atomic<float> g_lockGain{0.9f};      // lateral
std::atomic<float> g_lockDepthGain{0.9f}; // along the fg forward
// Fg-eye pull-back behind the camera at the MATCHED lens. The DRIVEN rigid
// path renders a much smaller pull than the vanilla path's fov-coupled ~65
// (session 16 flat calibration at option 117: +11.5 UU physical, agreed by
// offset-parallax and size A/Bs within ~1 UU, and close to the stock-lens
// 13 - the driven path's pull is NOT fov-coupled). The default is the knob
// value that lands 11.5 through the 0.9 depth gain (11.5/0.9 = 12.8); if
// lockdgain changes, the effective pull moves with it.
std::atomic<float> g_lockPull{12.8f};
std::atomic<float> g_lockDeltaMag{0.0f};  // telemetry: |delta| UU last frame
std::atomic<uint32_t> g_lockSolves{0};
std::atomic<uint32_t> g_lockSkips{0};
// Measure-only lane (session 66). The lock's guard refuses its own answer at
// `lat 30.0`, and a refusal returns before anything is published - so with the
// lock ARMED the only evidence of the correction is a rate-limited "refusing"
// line, and with it OFF there is no evidence at all. `vrbones lockprobe on`
// runs the solve for its NUMBERS with the lock still off: the delta is
// computed, logged and thrown away, and nothing is ever added to ptc unless
// `lock` itself is non-zero. That is what makes the s65 A/B repeatable without
// arming a correction nobody has shown to be correct.
std::atomic<bool> g_lockProbe{false};

// In-headset telemetry (vrbones log on): ~5 Hz shared sample window. The
// headset cannot be watched from outside, so the log carries every frame of
// the chain - raw XR poses (camera.cpp / hands.cpp lines), the camera, the
// actor, the world target, and what the bone array held before the write.
std::atomic<bool> g_telemetry{false};
uint64_t g_lastTlmMs = 0;
bool g_tlmWindowOpen = false;

// ---- guarded memory (no C++ objects inside SEH frames) ----------------------

bool read_n(const void* src, void* dst, size_t n) {
    __try {
        memcpy(dst, src, n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool write_n(void* dst, const void* src, size_t n) {
    __try {
        memcpy(dst, src, n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

uint32_t to_rva(const void* p) {
    if (!p || !g_imageBase) return 0;
    return static_cast<uint32_t>(static_cast<const uint8_t*>(p) - g_imageBase);
}

// actor -> SkeletonInstance, fully revalidated (vtable, plausible count,
// readable array). The instance is recreated on mesh relinks, so never trust
// yesterday's pointer.
//
// Session 20: the resolution itself is BY VALUE and slot-free (`resolve_skel`)
// so any actor's skeleton can be inspected - the weapon carries its own, which
// is what the muzzle probe needs. `locate()` keeps the module's single cached
// slot for the drive, which only ever poses the AHands rig.
struct Skel {
    void* inst = nullptr;
    Qts* bones = nullptr;
    int count = 0;
};

bool resolve_skel(void* actor, Skel& out) {
    out = {};
    if (!actor) return false;
    void* si = nullptr;
    if (!read_n(static_cast<uint8_t*>(actor) + patterns::kActorSkelInstOffset, &si, sizeof si) ||
        !si)
        return false;
    void* vtbl = nullptr;
    if (!read_n(si, &vtbl, sizeof vtbl) || to_rva(vtbl) != patterns::kSkeletonInstanceVtableRva)
        return false;
    struct {
        Qts* bones;
        int count;
    } a{};
    if (!read_n(static_cast<uint8_t*>(si) + patterns::kSkelInstBonesOffset, &a, sizeof a) ||
        !a.bones || a.count < 1 || a.count > kMaxBones)
        return false;
    // The count bound alone is not enough (session 27): the drive memcpys up to
    // count * sizeof(Qts) bytes THROUGH a.bones, so a plausible count paired
    // with an array pointer that does not actually have that many readable
    // bytes behind it is a multi-kilobyte write into whatever follows. The SEH
    // guard on write_n turns that into a survivable fault, not a correct one -
    // so require the whole span to be committed before trusting the pair.
    if (!bvr::pattern_scan::is_memory_valid(a.bones, static_cast<size_t>(a.count) * sizeof(Qts)))
        return false;
    out.inst = si;
    out.bones = a.bones;
    out.count = a.count;
    return true;
}

// Bone index -> name, via the SharedSkeletonData FName->index map walked in
// reverse (patterns.h "Session 20: bone NAMES"). Returns how many names were
// filled; entries stay null when a bone has no map entry.
int resolve_bone_names(const Skel& sk, const wchar_t** names, int cap) {
    for (int i = 0; i < cap; ++i) names[i] = nullptr;
    if (!sk.inst) return 0;
    void* shared = nullptr;
    if (!read_n(static_cast<uint8_t*>(sk.inst) + patterns::kSkelInstSharedOffset, &shared,
                sizeof shared) ||
        !shared)
        return 0;
    const uint8_t* map = static_cast<const uint8_t*>(shared) + patterns::kSharedBoneNameMapOffset;
    struct {
        const uint8_t* pairs;
    } p{};
    const int32_t* buckets = nullptr;
    int32_t bucketCount = 0;
    if (!read_n(map + patterns::kNameMapPairsOffset, &p, sizeof p) ||
        !read_n(map + patterns::kNameMapBucketsOffset, &buckets, sizeof buckets) ||
        !read_n(map + patterns::kNameMapBucketCountOffset, &bucketCount, sizeof bucketCount))
        return 0;
    if (!p.pairs || !buckets || bucketCount <= 0 || bucketCount > 65536) return 0;

    int filled = 0;
    for (int b = 0; b < bucketCount; ++b) {
        int32_t idx = -1;
        if (!read_n(buckets + b, &idx, sizeof idx)) break;
        // Chain walk, bounded by the table size so a corrupt link cannot spin.
        for (int guard = 0; idx >= 0 && guard <= cap * 4; ++guard) {
            struct {
                int32_t next, nameIdx, nameNum, value;
            } pair{};
            if (!read_n(p.pairs + static_cast<size_t>(idx) * patterns::kNameMapPairStride, &pair,
                        sizeof pair))
                break;
            if (pair.value >= 0 && pair.value < cap && !names[pair.value]) {
                const wchar_t* t = patterns::fname_text(pair.nameIdx);
                if (t) {
                    names[pair.value] = t;
                    ++filled;
                }
            }
            idx = pair.next;
        }
    }
    return filled;
}

bool locate(void* handsActor) {
    Skel sk{};
    if (!resolve_skel(handsActor, sk) || sk.count < 8) {
        g_skelInst = nullptr;
        return false;
    }
    if (sk.inst != g_skelInst || sk.bones != g_bones || sk.count != g_boneCount) {
        BVR_LOG("[bones] skeleton: inst=%p bones=%p count=%d%s", sk.inst,
                static_cast<void*>(sk.bones), sk.count,
                sk.count == patterns::kHandsRigBoneCount ? "" : " (UNEXPECTED count)");
        // s69c: NAME THE LEFT CLUSTER, once per skeleton, always on.
        //
        // The two clusters anchor asymmetrically and nobody chose it: the RIGHT
        // pins bone 43 (kBoneWeaponAttach), which sits at the gun's grip deep in
        // the hand, so a recoil animation rotates about the thing you are looking
        // at and the gun stays put. The LEFT pins bone 6 (kBoneLWrist), the FIRST
        // bone of its range, with the palm and fingers hanging off it - so any
        // animation that moves the palm relative to the wrist translates the
        // visible hand away from the controller. That is the plasmid firing
        // animation "moving away from my hand position" (screenshots, 2026-08-27:
        // the fist sits centred at idle and the open hand is well left and low
        // during the bolt).
        //
        // Picking a better left anchor needs the NAMES, and this is how they get
        // into a log without the tester typing a command in the headset. Try one
        // live with `vrbones lcluster 6 21 <anchor>`.
        {
            const wchar_t* nm[kMaxBones];
            const int named = resolve_bone_names(sk, nm, sk.count);
            const int lo = patterns::kBoneLClusterFirst, hi = patterns::kBoneLClusterLast;
            for (int i = lo; i <= hi && i < sk.count; ++i)
                BVR_LOG("[bones] LEFTBONE %2d = '%ls'%s", i,
                        (named && nm[i]) ? nm[i] : L"?",
                        i == patterns::kBoneLWrist ? "   <-- anchor today" : "");
        }
        g_refValid = false; // new array = new reference
        g_hasWritten[0] = g_hasWritten[1] = false;
        settle_reset(); // s70: and a new array is a new settle window
    }
    g_skelInst = sk.inst;
    g_bones = sk.bones;
    g_boneCount = sk.count;
    return true;
}

void set_dirty(uint8_t v) {
    if (!g_skelInst) return;
    write_n(static_cast<uint8_t*>(g_skelInst) + patterns::kSkelInstDirtyOffset, &v, 1);
}

// ---- quat helpers over the Qts layout ---------------------------------------

void qts_rotate(const float q[4], const float v[3], float out[3]) {
    quat_rotate(q[0], q[1], q[2], q[3], v, out);
}

// ---- render lock -------------------------------------------------------------

// Build the foreground-view model matrix (rows x, y, w of [R | -R*E], scaled
// by the fixed projection) for a given component-frame rotation quat and eye
// position. The eye is a PARAMETER because it rides the camera (session 13
// part 3): eye = camera position in component space + the measured pull-back
// expressed in the fg view's own frame.
void build_fg_model(const float qd[4], const float E[3], float invTanH, float invTanV,
                    float M[12]) {
    float fgF[3], fgR[3], fgU[3];
    static const float kX[3] = {1.0f, 0.0f, 0.0f}, kY[3] = {0.0f, 1.0f, 0.0f},
                       kZ[3] = {0.0f, 0.0f, 1.0f};
    quat_rotate(qd[0], qd[1], qd[2], qd[3], kX, fgF);
    quat_rotate(qd[0], qd[1], qd[2], qd[3], kY, fgR);
    quat_rotate(qd[0], qd[1], qd[2], qd[3], kZ, fgU);
    M[0] = invTanH * fgR[0];
    M[1] = invTanH * fgR[1];
    M[2] = invTanH * fgR[2];
    M[3] = -(M[0] * E[0] + M[1] * E[1] + M[2] * E[2]);
    M[4] = invTanV * fgU[0];
    M[5] = invTanV * fgU[1];
    M[6] = invTanV * fgU[2];
    M[7] = -(M[4] * E[0] + M[5] * E[1] + M[6] * E[2]);
    M[8] = fgF[0];
    M[9] = fgF[1];
    M[10] = fgF[2];
    M[11] = -(M[8] * E[0] + M[9] * E[1] + M[10] * E[2]);
}

// Natural foreground depth of a component point through the model (row w).
float fg_natural_w(const float M[12], const float ptc[3]) {
    return M[8] * ptc[0] + M[9] * ptc[1] + M[10] * ptc[2] + M[11];
}

// Solve M (rows x, y, w) for the component point that renders at (ndcX, ndcY)
// with foreground depth w. The caller picks w: the world-equivalent k*d for
// the depth-corrected solves, or the natural depth for the diff-mode anchor.
bool solve_fg(const float M[12], float ndcX, float ndcY, float w, float outP[3]) {
    if (w < 1.0f) return false;
    float A[3][3] = {{M[0], M[1], M[2]}, {M[4], M[5], M[6]}, {M[8], M[9], M[10]}};
    float b[3] = {ndcX * w - M[3], ndcY * w - M[7], w - M[11]};
    float det = A[0][0] * (A[1][1] * A[2][2] - A[1][2] * A[2][1]) -
                A[0][1] * (A[1][0] * A[2][2] - A[1][2] * A[2][0]) +
                A[0][2] * (A[1][0] * A[2][1] - A[1][1] * A[2][0]);
    if (det > -1e-4f && det < 1e-4f) return false;
    float inv = 1.0f / det;
    outP[0] = inv * (b[0] * (A[1][1] * A[2][2] - A[1][2] * A[2][1]) -
                     A[0][1] * (b[1] * A[2][2] - A[1][2] * b[2]) +
                     A[0][2] * (b[1] * A[2][1] - A[1][1] * b[2]));
    outP[1] = inv * (A[0][0] * (b[1] * A[2][2] - A[1][2] * b[2]) -
                     b[0] * (A[1][0] * A[2][2] - A[1][2] * A[2][0]) +
                     A[0][2] * (A[1][0] * b[2] - b[1] * A[2][0]));
    outP[2] = inv * (A[0][0] * (A[1][1] * b[2] - b[1] * A[2][1]) -
                     A[0][1] * (A[1][0] * b[2] - b[1] * A[2][0]) +
                     b[0] * (A[1][0] * A[2][1] - A[1][1] * A[2][0]));
    return true;
}

// NDC of the world target through a pinhole at the camera position with the
// given rotation and the world option FOV. outDf = the target's forward
// distance from the camera - the "true distance" the depth constraint uses.
// Session 28: h/w of the live backbuffer, defaulting to 16:9. The world lens is
// tanV = tanH*(h/w) (dump-measured, ENGINE_NOTES "Session 28"), so every model
// of it here has to read the real aspect. It was hardcoded 9/16, which is right
// only at 16:9 - and the resolutions people actually run in VR are square-ish.
float live_inv_aspect() {
    unsigned w = 0, h = 0;
    if (bvr::hud::backbuffer_dims(&w, &h) && w && h)
        return static_cast<float>(h) / static_cast<float>(w);
    return 9.0f / 16.0f;
}

bool world_ndc(const FrameContext& ctx, const GamePose& gp, const FRotator& rot, float tanH,
               float* outX, float* outY, float* outDf) {
    float fwd[3], right[3], up[3];
    ue_rot_basis(rot, fwd, right, up);
    float d[3] = {gp.loc.x - ctx.camX, gp.loc.y - ctx.camY, gp.loc.z - ctx.camZ};
    float df = d[0] * fwd[0] + d[1] * fwd[1] + d[2] * fwd[2];
    if (df < 4.0f) return false; // target at/behind the eye: no stable pixel
    float dr = d[0] * right[0] + d[1] * right[1] + d[2] * right[2];
    float du = d[0] * up[0] + d[1] * up[1] + d[2] * up[2];
    float tanV = tanH * live_inv_aspect(); // world lens: vertical follows the window
    *outX = dr / (df * tanH);
    *outY = du / (df * tanV);
    *outDf = df;
    return true;
}

// Compute the component-space nudge that counters the foreground pipeline's
// camera-coupled displacement of the rig. The pipeline (fixed 60-deg 4:3
// projection, eye pulled ~32 UU behind the RENDER CAMERA - it rides the
// camera, translation included; session 13 part 3 - plus hand sway) is
// self-consistent at view center but slides the rig laterally under head-
// split AND pins its stereo depth at ~(d+32)/k. The transform is MODELED
// analytically (patterns.h kFgEyeComp block) - live captures embed the
// engine's per-frame re-derivations of section transforms from the very
// bones this module writes (feedback), so they cannot be used.
// Depth constraint (session 14): the rig must RENDER at fg depth k*df,
// k = tan(worldFov/2)/tan(fgFov/2) - apparent size, stereo disparity, and
// translation parallax are all the same (1/w)*k geometry, so that one
// constraint makes all three world-correct at once. It renders today at
// df + kFgEyeFwdBehindCam (parallax-calibrated), so the solve pushes the
// anchor deeper by the DIFFERENCE, expressed against the model's own
// natural depth (the model's absolute depth scale is section-relative and
// cannot be trusted; its lateral geometry is g5-validated and kept).
// Modes: abs = solve the anchor onto its TRUE world pixel at w* (fixes the
// raised/too-close authored composition as well; inherits the sway wobble).
// diff = subtract the zero-split solve taken at ptc's NATURAL depth, so the
// delta carries the full depth correction plus only the head-split lateral
// cancel (authored lateral composition preserved).
// The correction returns SPLIT into a lateral part and a depth part (along
// the fg forward) because the rigid-section rebake gain was measured on the
// lateral axis only - the depth axis gets its own gain (vrbones lockdgain).
// qaInv/actorRot/actorLoc come from the drive (already computed there).
bool render_lock_delta(const FrameContext& ctx, const GamePose& gp, const float qaInv[4],
                       const FRotator& actorRot, const float actorLoc[3], const float ptc[3],
                       float outLat[3], float outDepth[3]) {
    int mode = g_renderLock.load(std::memory_order_relaxed);
    int32_t* opt = patterns::hfov_option_ptr();
    float hfov = opt && *opt > 0 ? static_cast<float>(*opt) : 0.0f;
    if (hfov <= 0.0f) return false;
    float tanH = tanf(hfov * 0.5f / kRadToDeg);

    FRotator camRot{ctx.camPitch, ctx.camYaw, ctx.camRoll};
    float ndcX, ndcY, df;
    if (!world_ndc(ctx, gp, camRot, tanH, &ndcX, &ndcY, &df)) return false;

    // Fg lens: with the session-15 lens match armed (vrfgfov, default on)
    // the rig renders through the WORLD lens - invTan scales come from the
    // live world tanH and the lens ratio k collapses to 1, so the depth
    // constraint w* = k*df becomes simply the true distance. Without the
    // match, the legacy 60-deg constants apply (kept for A/B).
    // Session 28: the vertical model reads the LIVE aspect. This comment's claim
    // that "the rig renders through the WORLD lens, so k collapses to 1" was
    // TRUE only at 16:9 - the shipped match constant (0.75) left the fg lens
    // 1.7778/aspect narrower than the world everywhere else, so at a square
    // backbuffer k was really 1.7778 while this code assumed 1. That mis-scaled
    // the depth constraint AND the head-split lateral cancel, which is what
    // "the hand and gun move when the headset moves" looks like. With the
    // aspect-correct fg write in camera.cpp the lenses genuinely match and k
    // genuinely collapses to 1 - the assumption is now earned, not asserted.
    bool matched = camera::fg_fov_match_active();
    float invTanHFg = matched ? 1.0f / tanH : patterns::kFgInvTanH;
    float invTanVFg =
        matched ? 1.0f / (tanH * live_inv_aspect()) : patterns::kFgInvTanV;
    float k = tanH * invTanHFg;
    float wStar = k * df;
    if (wStar < 4.0f) return false; // hand at/behind the face: no stable solve

    float qcam[4], qdRaw[4], qd[4];
    ue_rot_to_quat(camRot, qcam);
    quat_mul(qaInv, qcam, qdRaw);
    // Composition bias (patterns.h): the fg view sits a hair up-right of the
    // camera delta; constant, measured as the mean of the dump set.
    static float s_qBias[4] = {2.0f, 0.0f, 0.0f, 0.0f}; // sentinel: build once
    if (s_qBias[0] > 1.5f) {
        FRotator biasRot{
            static_cast<int32_t>(patterns::kFgViewPitchBiasDeg * kRotUnitsPerDegree),
            static_cast<int32_t>(patterns::kFgViewYawBiasDeg * kRotUnitsPerDegree), 0};
        ue_rot_to_quat(biasRot, s_qBias);
    }
    quat_mul(qdRaw, s_qBias, qd);

    // Effective fg eye: camera position in component space + the TRUE
    // pull-back (forward from the parallax calibration - kFgEyeFwdBehindCam;
    // laterals from the dump mean, statics the abs solve absorbs) rotated
    // into the fg view's frame. The eye RIDES THE CAMERA - dump-proven
    // (offset 0 30 0 moved the recovered eye 29.7 UU).
    float dCam[3] = {ctx.camX - actorLoc[0], ctx.camY - actorLoc[1], ctx.camZ - actorLoc[2]};
    float camComp[3], ePulled[3], eyeEff[3];
    qts_rotate(qaInv, dCam, camComp);
    float pull = matched ? g_lockPull.load(std::memory_order_relaxed)
                         : patterns::kFgEyeFwdBehindCam;
    const float eTrue[3] = {-pull, patterns::kFgEyeComp[1], patterns::kFgEyeComp[2]};
    // Pull frame (session-16 simhead discriminator): at the matched lens the
    // renderer's eye offset does NOT swing with the camera-vs-actor head
    // split - the gun over-shifted the world by exactly pull*sin(split)*gain
    // on both axes when eTrue rode qd - so it rotates by the constant view
    // bias only (identical at zero split, where the A/Bs calibrated it). The
    // unmatched session-14 path keeps the qd rotation it was verified with.
    if (matched)
        quat_rotate(s_qBias[0], s_qBias[1], s_qBias[2], s_qBias[3], eTrue, ePulled);
    else
        quat_rotate(qd[0], qd[1], qd[2], qd[3], eTrue, ePulled);
    eyeEff[0] = camComp[0] + ePulled[0];
    eyeEff[1] = camComp[1] + ePulled[1];
    eyeEff[2] = camComp[2] + ePulled[2];

    float M1[12];
    build_fg_model(qd, eyeEff, invTanHFg, invTanVFg, M1);
    float wNat = fg_natural_w(M1, ptc);
    float p1[3];
    if (!solve_fg(M1, ndcX, ndcY, wStar, p1)) return false;

    float delta[3];
    if (mode == 2) {
        // Differential: subtract the zero-split solution AT NATURAL DEPTH so
        // the lateral part vanishes when the camera sits on the actor while
        // the depth correction survives the subtraction.
        float ndcX0, ndcY0, df0;
        if (!world_ndc(ctx, gp, actorRot, tanH, &ndcX0, &ndcY0, &df0)) return false;
        float e0[3];
        quat_rotate(s_qBias[0], s_qBias[1], s_qBias[2], s_qBias[3], eTrue, e0);
        float M0[12], p0[3];
        build_fg_model(s_qBias, e0, invTanHFg, invTanVFg, M0); // bias kept at zero split
        if (!solve_fg(M0, ndcX0, ndcY0, fg_natural_w(M0, ptc), p0)) return false;
        delta[0] = p1[0] - p0[0];
        delta[1] = p1[1] - p0[1];
        delta[2] = p1[2] - p0[2];
    } else {
        delta[0] = p1[0] - ptc[0];
        delta[1] = p1[1] - ptc[1];
        delta[2] = p1[2] - ptc[2];
    }

    // Split along the fg forward (M row w is the unit forward vector).
    float dDepth = delta[0] * M1[8] + delta[1] * M1[9] + delta[2] * M1[10];
    outDepth[0] = dDepth * M1[8];
    outDepth[1] = dDepth * M1[9];
    outDepth[2] = dDepth * M1[10];
    outLat[0] = delta[0] - outDepth[0];
    outLat[1] = delta[1] - outDepth[1];
    outLat[2] = delta[2] - outDepth[2];
    float latMag = sqrtf(outLat[0] * outLat[0] + outLat[1] * outLat[1] + outLat[2] * outLat[2]);

    // Session 66 diagnostic terms. `latMag` is the component of the FULL delta
    // perpendicular to the fg forward, so it conflates two unrelated things:
    // a genuine lateral disagreement, and the perpendicular shadow that any
    // depth move casts when the anchor is off axis (a point at NDC n sits
    // n*tan(fov/2)*w off the axis, so changing w by dDepth moves it sideways
    // by n*tan(fov/2)*dDepth for free). At the observed dDepth of -15 and a
    // half-screen anchor that shadow alone is ~9 UU. The terms below separate
    // them, and none of them feed the guard or the correction.
    //
    //   nat   - where the fg model says ptc renders TODAY, in NDC.
    //   dndc  - nat vs the world's answer. Dimensionless, so it survives every
    //           depth and UU scale factor, and its two axes name the suspect:
    //           X -> tanH / the right basis, Y -> live_inv_aspect / invTanVFg.
    //   latP  - the lateral correction solved at CONSTANT depth (wNat instead
    //           of wStar). This is the honest lateral term. Taken against M1 in
    //           both modes so an ABS/DIFF comparison compares like with like.
    //   split - the camera-vs-actor yaw the lock exists to cancel. A correction
    //           that does not scale with this is not doing its job, whatever
    //           its magnitude.
    // latP is -1 when it could not be solved, so a reader can tell that apart
    // from a genuine zero (the two mean opposite things).
    float natX = 0.0f, natY = 0.0f, latPure = -1.0f;
    if (wNat > 1.0f) {
        natX = (M1[0] * ptc[0] + M1[1] * ptc[1] + M1[2] * ptc[2] + M1[3]) / wNat;
        natY = (M1[4] * ptc[0] + M1[5] * ptc[1] + M1[6] * ptc[2] + M1[7]) / wNat;
        float pSame[3];
        if (solve_fg(M1, ndcX, ndcY, wNat, pSame)) {
            float dx = pSame[0] - ptc[0], dy = pSame[1] - ptc[1], dz = pSame[2] - ptc[2];
            latPure = sqrtf(dx * dx + dy * dy + dz * dz);
        }
    }
    // Wrapped into [-180, 180). Both yaws are FREE-RUNNING rotator units, so
    // they are folded onto one turn BEFORE subtracting - a raw int32 subtract
    // of two far-apart values is signed overflow, and the difference is the
    // only part that means anything anyway.
    const int32_t kTurn = 65536;
    int32_t splitUnits = (camRot.yaw % kTurn) - (actorRot.yaw % kTurn);
    float splitDeg = static_cast<float>(splitUnits) / kRotUnitsPerDegree;
    splitDeg -= floorf((splitDeg + 180.0f) / 360.0f) * 360.0f;

    if (g_tlmWindowOpen) {
        BVR_LOG("[tlm] lock mode=%d%s tgt=(%.3f %.3f) nat=(%.3f %.3f) dndc=(%+.3f %+.3f) "
                "df=%.1f k=%.2f wNat=%.1f w*=%.1f lat=%.2f latP=%.2f depth=%+.2f "
                "split=%+.1f",
                mode,
                !g_lockProbe.load(std::memory_order_relaxed) ? ""
                : mode == 0                                  ? " PROBE(solving abs)"
                                                             : " PROBE",
                ndcX,
                ndcY, natX, natY, ndcX - natX, ndcY - natY, df, k, wNat, wStar, latMag,
                latPure, dDepth, splitDeg);
    }
    // Per-axis refusal: the depth correction is LARGE by design (~25-70 UU),
    // the lateral one is not - a big lateral delta still means a model bug.
    if (latMag > 30.0f || dDepth < -120.0f || dDepth > 120.0f) {
        static uint64_t lastDump = 0;
        uint64_t now = GetTickCount64();
        if (now - lastDump > 2000) {
            lastDump = now;
            BVR_LOG("[bones] lock: refusing outsized delta (lat %.1f depth %+.1f)", latMag,
                    dDepth);
        }
        return false;
    }
    g_lockDeltaMag.store(
        sqrtf(delta[0] * delta[0] + delta[1] * delta[1] + delta[2] * delta[2]),
        std::memory_order_relaxed);
    return true;
}

// ---- the drive ---------------------------------------------------------------

// s67: the RIGHT cluster's range is overridable now, not just its anchor.
//
// The tester's observation, and it is a structural one: driving 27..44 moves
// the wrist, every finger AND the weapon-attach bone as ONE rigid body, so the
// gun and the hand cannot be controlled separately - "we are moving both the
// gun and the arms when we should have control of both separately".
//
// `vrbones rcluster 43 44 43` drives ONLY the weapon bones. The gun goes to the
// controller; the hand, fingers and arm stay entirely engine-animated. As a
// DIAGNOSTIC that isolates the gun from the rig: if the gun alone tracks
// cleanly, the desync lives in the cluster drive's interaction with the hand,
// not in the gun's own transform.
//
// Expect the hand NOT to hold the gun in that mode. That is the point, not a
// regression - it is the control condition.
// ---- s67: FREEZE-ONLY, the BRVR shape --------------------------------------
//
// Read straight off BRVR's own log, this machine, tonight:
//
//   >>> WEAPONHAND: freezing the RIGHT cluster, bones 27-44.
//   >>> WEAPONHAND: pose settled after 359 ms (rig went still) - freezing from here.
//   >>> B43: attach rotation drift now 1.29 deg (CLUSTER FROZEN; THIS BONE IS
//            STILL THE ENGINE'S)
//   >>> HANDANIM: cluster 1 rejected - largest movement 1.8 deg, threshold 5
//
// BRVR does NOT retarget the cluster. It REPLAYS the captured pose verbatim so
// the rig is rigid, leaves bone 43 entirely to the engine, and lets the ACTOR
// transform carry the whole assembly to the controller. The rig's internal pose
// is therefore always exactly as authored - nothing inside it is ever re-posed
// relative to anything else, so the weapon's attachment sees precisely the
// geometry the artist shipped.
//
// THIS TREE DOES THE INVERSE and that is the architectural difference: it
// leaves the actor where the engine pins it (Hands.UpdateLocation puts it on
// the camera every frame) and retargets bones 27-44 - bone 43 included - about
// a reference point that is itself a frozen snapshot of an animated pose.
//
// Freeze-only turns this module into BRVR's half: reference capture, sway
// rejection and the stereo re-apply all still run, but the rigid retarget is
// replaced by a verbatim replay and bone 43 is skipped. hands.cpp mode 3 drives
// the actor, exactly as BRVR does.
std::atomic<bool> g_freezeOnly{false};
// s70: the s69 anchor pin is GONE. It held the cluster's anchor on the captured
// rest while an adopted animation played, so the animation changed the pose
// without dragging the hand off the controller. It was a correction for two
// things that are both fixed at their source now: the freeze was anchored on
// bone 43, which the engine animates (see cluster_of), and the state mask left
// the reference stuck at the recoil apex. BRVR pins nothing.
// ---- s67: CAPTURE ONCE PER HOLDABLE ----------------------------------------
//
// The reference pose must be a property of WHAT YOU ARE HOLDING, captured once
// when it settles and then left alone. Re-latching on every large animation is
// what made the crosshair wander: recoil clears the adopt threshold, the drive
// tracks it, and then re-freezes on wherever the animation happened to SETTLE -
// a slightly different pose after every shot. The aim ray does not move, so the
// gun moves under it, and the dot lands right-and-low after one shot and left
// after the next. Reported s67, and it is BRVR's own recorded failure mode:
// "the grip started correct, then moved to the middle of the barrel, then up
// the barrel, then back."
//
// It is also what put an equip animation on screen before every freeze.
//
// So: unlatch on a holdable CHANGE, let the settle logic find the still pose,
// latch, and then never adopt again until the holdable changes.
// s67: PER-WEAPON ANIMATION GATE, ported from BRVR's CameraHook:
//     ArmHide_SetAnimAllowed(handAnim != 0 && handAnimSlot[wslot] != 0)
// with its comment - "so ArmHide's adoption policy never has to know about
// weapon slots". hands.cpp publishes it on the weapon switch; this module just
// obeys it.
//
// The hard latch that briefly lived here (capture once per holdable, never
// adopt again) is GONE. It did stop the reference drifting, but it also froze
// out recoil and reload, which is worse: BRVR keeps adopting real animation and
// only the WRENCH is gated off, because a swing animation fights manual melee.
std::atomic<bool> g_animAllowed{true};

// s70: THE PER-STATE ANIMATION MASK IS GONE, and it was the root of s67-s69.
//
// It adopted only Firing/PostFiring and refused everything else. Two costs, both
// paid in full:
//
//   - Hands.uc leaves `WeaponFiring` at the TOP of the recoil, so adoption was
//     cut at the apex and the reference stuck there. Everything s68 and s69 then
//     built - the canonical rest, the eased restore, the anchor pin, the
//     quaternion normalisation for that pin - exists to undo this one line.
//   - It refused reload, equip and melee outright, so the rig went rigid through
//     every animation that is not a shot.
//
// BRVR separates idle from real animation BY SIZE, not by state: idle breathing
// measures 1-5 deg at the wrist and a reload or a switch peaks at 41-135, so a
// threshold in that gap rejects breathing and admits everything else, and the
// hold window carries adoption to the animation's settled end. That mechanism is
// already in this file (g_swayAngThreshDeg / g_swaySettleMs) - the mask was
// layered on top of it and overrode it.

// ---- s68 REST REFERENCE: what happens when an adopted animation ENDS -------
//
// THE DEFECT s67 SHIPPED. The state mask above says which animations reach the
// rig, and when the state leaves the mask the drive simply stops adopting. But
// "stop adopting" is not "go back" - it leaves g_ref holding the last frame it
// took, and the state machine leaves WeaponFiring at the TOP of the recoil, not
// after the gun has come back down. So the pistol froze at its recoil apex and
// stayed there until a weapon switch. The same shape, reported the same run:
// the pistol freezing when it runs out of ammo (Firing -> Reloading) and the
// shotgun sticking on its first reload.
//
// WHY BRVR NEVER HAD IT, and why we cannot just copy the answer. BRVR gates on
// a movement threshold plus a HOLD window - `adopt = (now - lastBigDelta) <
// holdMs` - so it keeps tracking for ~1.2 s past the animation's last big frame
// and its reference re-freezes on the SETTLED pose. That property is still in
// the threshold fallback below, and the state branch is exactly what threw it
// away. Reinstating a settle window on the state branch would work, but it
// would re-freeze onto whatever the idle animation had drifted to - and
// GetIdlingHandsAnim() is weighted-random per loop (Holdable.uc:32), which is
// the "crosshair moves randomly between shots" report we just fixed.
//
// SO: a CANONICAL rest, captured ONCE per holdable during WeaponIdling and
// restored verbatim. Deterministic, per weapon, and it cannot drift - which is
// what the per-weapon crosshair tuning needs to stay true between shots.
//
// The restore is BLENDED rather than snapped, because the pose we are leaving
// is the apex of a recoil: cutting straight to rest reads as a jerk, whereas
// easing over ~120 ms is the recovery the recoil animation would have played.
// 0 ms = snap, and the F10 slider makes that an A/B in the headset.
// s70: the Idling-edge capture (g_capAtIdling), the canonical-rest restore and
// its blend are all gone with the mask that needed them. What replaces the
// capture is BRVR's settle - see the settle block in drive().

std::atomic<int> g_rFirst{-1}; // -1 = the authored kBoneRClusterFirst
std::atomic<int> g_rLast{-1};

// s70: THE RIGHT CLUSTER'S ANCHOR DEPENDS ON WHICH SHAPE IS DRIVING IT, and
// getting that wrong is why a "frozen" cluster still moved.
//
// FREEZE (mode 3, BRVR's shape) ANCHORS ON THE WRIST, 27. BRVR's ClusterSpec is
// {first, last, wrist} = {27, 44, 27} and every replay is anchored on that wrist
// (ArmHide.cpp SpecFor/WriteCluster). Bone 43 is the WEAPON ATTACH, and it is
// precisely the one bone BRVR deliberately leaves the engine still animating -
// its own log says so: "cluster frozen; this bone is still the engine's", with
// 1-5 deg of measured idle drift and peaks of 41-135.
//
// So anchoring the freeze on 43 anchors it on a MOVING bone: every other bone is
// written relative to a point the engine is animating, and the whole cluster is
// dragged along by it. That is the hand walking off the controller during an
// animation, and it is what the s69 anchor pin was built to cancel - a
// correction for a reference point that should never have been moving.
//
// s68 dumped the bone names and recorded the left/right asymmetry (left anchors
// on 6, the palm; right on 43) as "not a defect". Against BRVR it is one: BRVR
// anchors BOTH clusters on their wrist, and the left cluster here already does.
//
// RETARGET (mode 2) KEEPS 43, and that is not an oversight. Retarget maps the
// anchor onto the controller, so the point it maps must be where the weapon
// hangs; that path is this tree's own and 43 is right for it. BRVR has no
// retarget at all, so there is nothing to be faithful to there.
void cluster_of(int hand, int* first, int* last, int* anchor) {
    if (hand == 1) {
        int rf = g_rFirst.load(std::memory_order_relaxed);
        int rl = g_rLast.load(std::memory_order_relaxed);
        *first = rf >= 0 ? rf : patterns::kBoneRClusterFirst;
        *last = rl >= 0 ? rl : patterns::kBoneRClusterLast;
        int ov = g_rAnchorOverride.load(std::memory_order_relaxed);
        const int dflt = g_freezeOnly.load(std::memory_order_relaxed)
                             ? patterns::kBoneRWrist
                             : patterns::kBoneWeaponAttach;
        *anchor = ov >= 0 ? ov : dflt;
    } else {
        *first = g_lFirst.load(std::memory_order_relaxed);
        *last = g_lLast.load(std::memory_order_relaxed);
        *anchor = g_lAnchor.load(std::memory_order_relaxed);
    }
}

// s68: normalised lerp with hemisphere correction. Good enough here by a wide
// margin - the two poses either side of a recoil are tens of degrees apart, and
// nlerp only parts company with slerp near 180.
void quat_nlerp(const float a[4], const float b[4], float t, float out[4]) {
    float dot = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
    const float sgn = dot < 0.0f ? -1.0f : 1.0f; // shortest arc
    for (int k = 0; k < 4; ++k) out[k] = a[k] * (1.0f - t) + b[k] * sgn * t;
    float len = sqrtf(out[0] * out[0] + out[1] * out[1] + out[2] * out[2] + out[3] * out[3]);
    if (len < 1e-6f) { // degenerate: the two are opposite, take the target
        out[0] = b[0]; out[1] = b[1]; out[2] = b[2]; out[3] = b[3];
        return;
    }
    for (int k = 0; k < 4; ++k) out[k] /= len;
}

// Angle between two quats, degrees. Shared by the rest-restore telemetry.
//
// NORMALISE BOTH INPUTS, and it is the same rule 825ced6 applied to the drive
// path: every caller passes g_ref/g_rest, and that bank does NOT hold unit quats
// while an animation is blending. The dot of two non-unit quats is
// |a||b|cos(theta/2), so an unnormalised readout is wrong in BOTH directions -
// magnitudes above 1 inflate the dot, it clamps at 1, and the instrument reports
// 0 deg while the bones genuinely differ; magnitudes below 1 deflate it and the
// instrument INVENTS an angle out of a pure magnitude difference. That second
// case is the ROLLCHECK lie ("drifted 16.64 deg" with a +0.00/+0.00/-0.00
// component split) wearing a different hat, and it cost session 68 two rounds of
// diagnosis and one commit fixing a non-problem.
//
// It matters more here than in an ordinary readout: the ANIMPIN telemetry GATES
// on this value (angOff > 2.0), so a lying angle decides whether the line prints
// at all. An instrument that chooses its own visibility cannot be checked
// against its own silence.
float quat_angle_deg(const float a[4], const float b[4]) {
    const float na = sqrtf(a[0] * a[0] + a[1] * a[1] + a[2] * a[2] + a[3] * a[3]);
    const float nb = sqrtf(b[0] * b[0] + b[1] * b[1] + b[2] * b[2] + b[3] * b[3]);
    if (na < 1e-4f || nb < 1e-4f) return 0.0f; // degenerate: no angle to report
    float dot = (a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3]) / (na * nb);
    if (dot < 0.0f) dot = -dot;
    if (dot > 1.0f) dot = 1.0f;
    return 2.0f * acosf(dot) * kRadToDeg;
}

// Restore a hidden hand's cluster + sleeve from the reference pose. g_ref is
// a safe source: an engine re-evaluation rewrites the whole array (scales
// included) and triggers the reference refresh in drive(), so the reference
// can never hold our zeroed scales.
void restore_hidden(int hand) {
    if (hand < 0 || !g_bones || !g_refValid) return;
    int first = 0, last = 0, anchor = 0;
    cluster_of(hand, &first, &last, &anchor);
    for (int i = first; i <= last && i < g_boneCount; ++i) {
        if (i < 0) continue;
        write_n(g_bones[i].p, g_ref[i].p, 12);
        write_n(g_bones[i].q, g_ref[i].q, 16);
        write_n(g_bones[i].s, g_ref[i].s, 12);
        g_scaleWrote[i] = false; // the bank holds the authored scale again
    }
    const int* sleeve = hand == 1 ? patterns::kBoneRSleeve : patterns::kBoneLSleeve;
    const size_t sleeveCount = hand == 1 ? _countof(patterns::kBoneRSleeve)
                                         : _countof(patterns::kBoneLSleeve);
    for (size_t k = 0; k < sleeveCount; ++k) {
        int idx = sleeve[k];
        if (idx >= g_boneCount) continue;
        write_n(g_bones[idx].p, g_ref[idx].p, 12);
        write_n(g_bones[idx].s, g_ref[idx].s, 12);
    }
}

// ---- rigid-holdable DrawScale lane (2026-08-22) -----------------------------
// The offsets are this repo's own (patterns.h session 12: AActor::SetDrawScale
// 0x375830 disassembled, field poked live), NOT carried across from BRVR - and
// patterns.h also records the honest earlier miss, where +0x168/+0x16C looked
// like DrawScale and were not. The DIRTY PROTOCOL is the part that makes the
// difference: patterns.h states outright that a raw field poke without it is
// invisible, which is the likeliest explanation of that earlier miss.
//
// THE WEAPON ACTOR RENDERS ITS DrawScale. HEADSET-CONFIRMED 2026-08-22 (the
// wrench visibly changed size), which SETTLES the question session 16 left open.
//
// That matters because session 16 measured DrawScale on the RIG actor and found
// the geometry INERT through the foreground path - gun width 240 -> 234 px at
// s=0.5, a 2% change where 50% was asked - and concluded "the fg rig path
// consumes actor DrawScale for bone translations but NOT for skin/attached-mesh
// size". It named the WEAPON actor as the case it could not isolate: "it could
// at best scale the gun, never the hand." That guess was right, and the split
// is real: the rig actor's DrawScale does not size geometry, the weapon
// actor's does.

bool ds_read(void* actor, float* out) {
    float v = 0.0f;
    if (!read_n(static_cast<uint8_t*>(actor) + patterns::kActorDrawScaleOffset, &v, sizeof v))
        return false;
    // Zero is Unreal's "unset, treat as 1" and session 12 saw it on the AHands
    // actor, so it is a legal authored value, not a failed read. Everything
    // else outside a plausible scale range fails closed: a wrong pointer reads
    // as garbage far more often than as a plausible scale, and this is the only
    // cheap check between us and writing into an unrelated object's field.
    if (v != 0.0f && !(v > 0.001f && v < 1000.0f)) return false;
    *out = v;
    return true;
}

bool ds_write(void* actor, float v) {
    if (!write_n(static_cast<uint8_t*>(actor) + patterns::kActorDrawScaleOffset, &v, sizeof v))
        return false;
    // The protocol SetDrawScale itself runs. Read-modify-write each field, and
    // treat a failed read as "leave that one alone" rather than as a reason to
    // abandon the write - the scale is already in.
    uint8_t* a = static_cast<uint8_t*>(actor);
    uint32_t flags = 0;
    if (read_n(a + patterns::kActorDirtyFlagsOffset, &flags, sizeof flags)) {
        flags |= 0x10u;
        write_n(a + patterns::kActorDirtyFlagsOffset, &flags, sizeof flags);
    }
    uint32_t rev = 0;
    if (read_n(a + patterns::kActorRenderRevOffset, &rev, sizeof rev)) {
        ++rev;
        write_n(a + patterns::kActorRenderRevOffset, &rev, sizeof rev);
    }
    uint8_t zero = 0;
    write_n(a + patterns::kActorDirtyByteOffset, &zero, 1);
    return true;
}

// ---- the AUTHORED DrawScale memo, and why the lane cannot re-read it -------
//
// ds_drop refuses to restore whenever the actor may be gone (see its comment),
// so a weapon switch LEAVES OUR WRITTEN VALUE on the wrench. Re-equip it and
// ds_read hands back 0.868 - our own write - and the lane records that as the
// "authored" scale. The log says so out loud: the first bind of a session reads
// `authored DrawScale 0.800`, every later one reads `authored DrawScale 0.868`.
//
// THAT POISONING IS WHY THIS LANE WRITES ABSOLUTELY TODAY, and it is worth
// knowing before anyone reverts the fractional change below. s63's first cut
// multiplied - `authored * ws` - which is correct arithmetic on a correct
// authored value and compounding nonsense on a poisoned one: 0.800 -> 0.640 ->
// 0.512 -> ... one step smaller per equip. That was reported as "it read too
// small", diagnosed as multiplication being wrong, and fixed by writing the
// knob absolutely. The multiplication was never the bug. The memo was missing.
//
// Eight slots because a BioShock loadout is a handful of holdables and the
// wrench is only ever one of them; the oldest entry is evicted, and a miss
// simply falls back to the live read, which is exactly today's behaviour.
struct DsAuthored {
    void* actor;
    float authored;
};
constexpr int kDsMemoSlots = 8;
DsAuthored g_dsMemo[kDsMemoSlots]{};
int g_dsMemoNext = 0;

// The authored DrawScale for `actor`: remembered if we have seen it before our
// first write, otherwise `live` (and remembered for next time).
float ds_authored(void* actor, float live) {
    for (const DsAuthored& e : g_dsMemo)
        if (e.actor == actor) return e.authored;
    g_dsMemo[g_dsMemoNext] = {actor, live};
    g_dsMemoNext = (g_dsMemoNext + 1) % kDsMemoSlots;
    return live;
}

// Hand the actor back. `restore` is FALSE whenever the actor may already be
// gone: BRVR's ArmHide_Reset carries the same rule for the same reason - by the
// time a change is noticed the old actor can be destroyed and its address
// reused, and write_n's SEH guard does not save you from a VALID write into
// somebody else's object. Restoring is only safe while the actor is still the
// one the rig says it is holding.
void ds_drop(const char* why, bool restore) {
    if (!g_dsHoldable) return;
    void* actor = g_dsHoldable;
    bool wrote = false;
    if (restore && g_dsWrote != 0.0f) wrote = ds_write(actor, g_dsRaw);
    BVR_LOG("[bones] wscale rigid: released %p (%s) - DrawScale %s %.3f", actor,
            why ? why : "?", wrote ? "restored to" : "left at",
            wrote ? g_dsRaw : g_dsWrote);
    g_dsHoldable = nullptr;
    g_dsRaw = 1.0f;
    g_dsWrote = 0.0f;
}

// Drive the lane for a skeleton-less holdable. `hold` is the CURRENT holdable
// as the rig reports it this frame, so nothing here ever touches a stale actor.
void ds_drive(void* hold, float ws) {
    if (hold != g_dsHoldable) {
        // Do NOT restore the outgoing actor - see ds_drop's comment.
        ds_drop("holdable changed", false);
        float raw = 0.0f;
        if (!ds_read(hold, &raw)) {
            // One line per holdable rather than one per frame: wskel_resolve's
            // negative cache has already gated us to that rate.
            BVR_LOG("[bones] wscale rigid: %p DrawScale (+0x%X) did not read as a "
                    "scale - lane stays unbound",
                    hold, patterns::kActorDrawScaleOffset);
            return;
        }
        const float authored = ds_authored(hold, raw);
        g_dsHoldable = hold;
        g_dsRaw = authored;
        g_dsWrote = 0.0f;
        BVR_LOG("[bones] wscale rigid: bound %p, authored DrawScale %.3f%s - no skeleton, "
                "scaling the ACTOR instead",
                hold, authored,
                fabsf(authored - raw) < 0.0005f ? "" : " (remembered; the live read is our "
                                                       "own earlier write)");
    }
    // A FRACTION OF THE AUTHORED VALUE, matching the skeleton lane. Reverses
    // s63, and the reversal is the wrench-scaling fix.
    //
    // wskel_compose writes `g_wRef[i].s * ws` - authored bone scale TIMES the
    // knob - so every skeletal weapon lands at ws of the size its artist
    // authored. Writing `ws` absolutely here made the knob mean something else
    // entirely on this lane, and the wrench is authored at 0.800: at ws 0.868
    // it rendered at 108.5% of authored while every other weapon sat at 86.8%,
    // which is the wrench reading ~25% large next to them. Reported from a
    // headset 2026-08-24, and the log carries the whole story in one line -
    // `wscale rigid: DrawScale 0.800 -> 0.868`.
    //
    // WHY IT SURVIVED A HEADSET RUN: s63 dialled wScale to exactly 0.800, and
    // 0.800 absolute on a weapon authored 0.800 leaves it at authored size. It
    // looked right because the wrench was being left alone. The defect only
    // appeared when the knob moved off that one value - same PR, no code change.
    //
    // The memo above is what makes this safe; without it this multiplies a
    // poisoned "authored" and shrinks the wrench once per equip.
    const float want = g_dsRaw * ws;
    // Re-assert rather than write once: the engine restamps DrawScale on equip
    // and on some animation transitions, and a one-shot poke was BS2-proven not
    // to render reliably. Reading first keeps this to zero writes on the frames
    // where nothing has moved it.
    float now = 0.0f;
    if (ds_read(g_dsHoldable, &now) && fabsf(now - want) < 0.0005f) {
        // Already there - usually because WE put it there before a switch and
        // ds_drop declined to restore. Record it as ours, or the restore on the
        // next release is gated off by g_dsWrote == 0 and the actor keeps our
        // value for the rest of the session.
        g_dsWrote = want;
        return;
    }
    if (!ds_write(g_dsHoldable, want)) {
        ds_drop("write faulted", false);
        return;
    }
    if (g_dsWrote == 0.0f)
        BVR_LOG("[bones] wscale rigid: DrawScale %.3f -> %.3f", g_dsRaw, want);
    g_dsWrote = want;
}

// ---- weapon-skeleton scale lane (session 61) --------------------------------

void wskel_set_dirty(uint8_t v) {
    if (!g_wSkelInst) return;
    write_n(static_cast<uint8_t*>(g_wSkelInst) + patterns::kSkelInstDirtyOffset, &v, 1);
}

// The bound skeleton, revalidated by value - never write through yesterday's
// pointers (the same session-29 lesson release() carries for the rig).
bool wskel_intact() {
    if (!g_wHoldable || !g_wSkelInst || !g_wBones) return false;
    Skel sk{};
    if (!resolve_skel(g_wHoldable, sk)) return false;
    return sk.inst == g_wSkelInst && sk.bones == g_wBones && sk.count == g_wBoneCount;
}

// Hand the weapon skeleton back: restore the captured pose over every bone
// (the engine does not restamp the scale channel, so merely not driving
// would leave the gun scaled for good - the sleeve-collapse lesson), then
// set its dirty flag so the engine rebuilds from its own animation.
void wskel_drop(const char* why) {
    if (wskel_intact()) {
        for (int i = 0; i < g_wBoneCount; ++i) {
            write_n(g_wBones[i].p, g_wRef[i].p, 12);
            write_n(g_wBones[i].q, g_wRef[i].q, 16);
            write_n(g_wBones[i].s, g_wRef[i].s, 12);
        }
        wskel_set_dirty(1);
        BVR_LOG("[bones] wskel: released to the engine (%s) - authored pose restored, "
                "%u drives %u adopts",
                why ? why : "?", g_wDrives, g_wAdopts);
    }
    g_wHoldable = nullptr;
    g_wSkelInst = nullptr;
    g_wBones = nullptr;
    g_wBoneCount = 0;
    g_wWrittenValid = false;
    g_wAdopts = 0;
    g_wDrives = 0;
}

// Bind (or re-bind) the lane to the current holdable. The holdable read is
// deliberately CLASS-AGNOSTIC (hands::current_holdable) - the MachineGun and
// GrenadeLauncher carry a different vtable and a gated read pins a stale
// weapon (the session-21 part-3 defect; BS2 relearned it independently).
bool wskel_resolve() {
    // A holdable without a resolvable skeleton (the WRENCH: +0x3FC is null -
    // rigid melee mesh, flat-proven 2026-08-14) would otherwise fail here
    // every frame: negative-cache it per holdable with a 1 s retry, so a
    // TRANSIENT mid-equip failure on a skeletal weapon still self-heals,
    // and log the verdict once per holdable instead of forever.
    static void* g_wDumpedHoldable = nullptr;
    static void* s_failedHold = nullptr;
    static uint64_t s_nextRetryMs = 0;
    void* hold = nullptr;
    if (!hands::current_holdable(&hold) || !hold) {
        if (g_wHoldable) wskel_drop("holdable gone");
        return false;
    }
    if (hold == g_wHoldable && wskel_intact()) return true;
    if (g_wHoldable) wskel_drop("holdable changed");
    if (hold == s_failedHold && GetTickCount64() < s_nextRetryMs) return false;
    Skel sk{};
    if (!resolve_skel(hold, sk)) {
        if (hold != s_failedHold)
            BVR_LOG("[bones] wskel: holdable %p has no resolvable SkeletonInstance "
                    "(+0x3FC) - no bones to scale (rigid mesh?), lane stays unbound",
                    hold);
        s_failedHold = hold;
        s_nextRetryMs = GetTickCount64() + 1000;
        return false;
    }
    s_failedHold = nullptr;
    Qts bank[kMaxBones];
    if (!read_n(sk.bones, bank, sizeof(Qts) * static_cast<size_t>(sk.count))) return false;
    // ---- WHERE IS THE GRIP? Dump this weapon's own bones, once. ------------
    //
    // The viewmodel is placed as `controller + R * e`, so it pivots about the
    // CONTROLLER. For it to look right, e has to be the vector that puts the
    // weapon's GRIP on the controller. Get e wrong and the grip traces a circle
    // of radius |e - e*| as the wrist turns - which is why a wrong offset still
    // lines up perfectly at ONE rotation (where the wrong circle crosses the
    // right point) and nowhere else. Every wrong value has such a rotation, so
    // "it lines up at this angle" proves nothing, and tuning by eye at a single
    // angle only moves which angle is perfect.
    //
    // e* IS NOT A MATTER OF TASTE. It is a property of the mesh, and the mesh
    // is carrying it: these bones are in the weapon's own space, so whichever
    // one is the handle IS the origin-to-grip vector, to be READ rather than
    // dialled. If one is there, per-weapon position tuning stops being tuning
    // and becomes a derivation - which is the outcome worth having, because a
    // weapon nobody has tuned would then be right on sight.
    //
    // `vrbones skel weapon` has always been able to print this. It needed
    // typing, which is impossible with both hands on the controllers, so it is
    // now automatic: once per holdable, at bind, in ordinary play.
    if (sk.count > 0 && sk.count <= 24 && hold != g_wDumpedHoldable) {
        g_wDumpedHoldable = hold;
        const wchar_t* wnames[kMaxBones];
        const int wnamed = resolve_bone_names(sk, wnames, sk.count);
        BVR_LOG("[bones] wskel GRIP HUNT: %d bone(s), %d named - the handle bone's pos IS "
                "the grip offset this weapon wants (read it, do not dial it)",
                sk.count, wnamed);
        for (int i = 0; i < sk.count; ++i) {
            const float* p = bank[i].p;
            BVR_LOG("[bones]   %2d %-22S pos(%8.2f %8.2f %8.2f) |pos|=%7.2f UU", i,
                    wnames[i] ? wnames[i] : L"<unnamed>", p[0], p[1], p[2],
                    sqrtf(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]));
        }
    }
    g_wHoldable = hold;
    g_wSkelInst = sk.inst;
    g_wBones = sk.bones;
    g_wBoneCount = sk.count;
    memcpy(g_wRef, bank, sizeof(Qts) * static_cast<size_t>(sk.count));
    memcpy(g_wAnim, bank, sizeof(Qts) * static_cast<size_t>(sk.count));
    g_wWrittenValid = false;
    g_wAdopts = 0;
    g_wDrives = 0;
    BVR_LOG("[bones] wskel: bound holdable=%p inst=%p count=%d (reference captured)", hold,
            sk.inst, sk.count);
    return true;
}

// Per-frame compose: adopt the engine's p/q where it wrote since our last
// write (animations keep playing), then write p*ws / q verbatim / s=ref*ws.
// The scale rows are NEVER adopted - the engine does not restamp scale, so
// adopting would feed our own write back as refS * ws^n (the same structural
// rule as the cluster's pinned reference).
bool wskel_compose(float ws) {
    for (int i = 0; i < g_wBoneCount; ++i) {
        Qts cur{};
        if (!read_n(&g_wBones[i], &cur, sizeof cur)) {
            g_wSkelInst = nullptr; // faulted: rebind next frame
            return false;
        }
        bool engineWrote = !g_wWrittenValid || memcmp(cur.p, g_wWritten[i].p, 12) != 0 ||
                           memcmp(cur.q, g_wWritten[i].q, 16) != 0;
        if (engineWrote) {
            memcpy(g_wAnim[i].p, cur.p, 12);
            memcpy(g_wAnim[i].q, cur.q, 16);
            ++g_wAdopts;
        }
        float p[3] = {g_wAnim[i].p[0] * ws, g_wAnim[i].p[1] * ws, g_wAnim[i].p[2] * ws};
        float sv[3] = {g_wRef[i].s[0] * ws, g_wRef[i].s[1] * ws, g_wRef[i].s[2] * ws};
        if (!write_n(g_wBones[i].p, p, 12) || !write_n(g_wBones[i].q, g_wAnim[i].q, 16) ||
            !write_n(g_wBones[i].s, sv, 12)) {
            g_wSkelInst = nullptr;
            return false;
        }
        memcpy(g_wWritten[i].p, p, 12);
        memcpy(g_wWritten[i].q, g_wAnim[i].q, 16);
        memcpy(g_wWritten[i].s, sv, 12);
    }
    g_wWrittenValid = true;
    ++g_wDrives;
    wskel_set_dirty(0); // render-side evaluate-if-dirty must not rebuild over us
    return true;
}

// s64 arm hide: is the ENGINE animating the rig right now? Mechanism only -
// threshold, hold and the fail-safe direction are scripted.cpp's.
//
// Sample the wrist of a cluster we are NOT writing. A wrist we write reports our
// own rigid transform, bit-for-bit identical every frame while the controller is
// still, so its delta is not small - it is exactly zero. When both are ours,
// say -1 rather than hand back a guaranteed zero.
//
// Full derivation, and the two BRVR constraints that do NOT apply here (its
// release-before-measure, and its hide-by-bone latch), in ENGINE_NOTES,
// "The arms during a scene: MOTION answers what the flag cannot".

float g_motPrevP[3] = {};
float g_motPrevQ[4] = {};
bool g_motHave = false;
int g_motBone = -1;   // which bone g_motPrev* describes
float g_motSmoothed = 0.0f;

void motion_reset() {
    g_motHave = false;
    g_motBone = -1;
    g_motSmoothed = 0.0f;
}

int motion_bone_idx() {
    if (!g_clWritten[1]) return patterns::kBoneRWrist;
    if (!g_clWritten[0]) return patterns::kBoneLWrist;
    return -1; // both clusters are ours - no honest bone exists
}

} // namespace

void init(const bvr::pattern_scan::ProcessImage& image) {
    g_imageBase = image.base;
    BVR_LOG("[bones] init (SkeletonInstance vtable 0x%X, right cluster %d-%d anchor %d)",
            patterns::kSkeletonInstanceVtableRva, patterns::kBoneRClusterFirst,
            patterns::kBoneRClusterLast, patterns::kBoneWeaponAttach);
}

void on_world_change() {
    if (g_skelInst) BVR_LOG("[bones] world changed - skeleton cache cleared");
    g_skelInst = nullptr;
    g_bones = nullptr;
    g_boneCount = 0;
    g_refValid = false;
    g_hasWritten[0] = g_hasWritten[1] = false;
    settle_reset(); // s70
    g_cacheSkelInst = nullptr;
    g_cacheMs = 0;
    g_hiddenHand = -1; // the collapsed bones died with the old world
    g_cacheHiddenCount = 0;
    g_clWritten[0] = g_clWritten[1] = false;
    motion_reset(); // a new rig is a new bone history, not a frame of motion
    // Session 29: the sleeve latch dies with the world too. It was hoisted out
    // of drive() so release() could reach it, which also means it now has to be
    // cleared here - a stale `true` would make release() write a dead world's
    // sleeve bones back from a dead world's reference.
    g_wasCollapsed = false;
    g_collapsedHand = -1;
    memset(g_scaleWrote, 0, sizeof g_scaleWrote); // scale writes died with it
    // The weapon skeleton died with the world too - drop WITHOUT writing
    // (wskel_intact re-resolves through the dead actor and fails, so
    // wskel_drop degrades to a pointer clear, which is exactly right here).
    wskel_drop("world change");
    // Same rule, and here it is not a judgement call: the actor is definitely
    // gone, so forget it without writing.
    ds_drop("world change", false);
}

void release(const char* why) {
    // NEVER write without a live skeleton. on_world_change() nulls these the
    // moment a level swap is seen, so this is the interlock that makes the
    // call safe from any site: without it, a release on the world-change frame
    // writes ~1.8 KB through the PREVIOUS level's bone array. write_n is
    // SEH-guarded, so that does not fault - it silently corrupts whatever the
    // engine has since allocated over those pages (session 29: a save load
    // hung the game exactly this way).
    if (!g_skelInst || !g_bones || g_boneCount <= 0) {
        g_hiddenHand = -1;
        g_wasCollapsed = false;
        g_collapsedHand = -1;
        g_cacheSkelInst = nullptr;
        g_cacheMs = 0;
        g_cacheCount = g_cacheSleeveCount = g_cacheHiddenCount = 0;
        g_refValid = false;
        g_hasWritten[0] = g_hasWritten[1] = false;
        settle_reset(); // s70
        g_clWritten[0] = g_clWritten[1] = false;
        memset(g_scaleWrote, 0, sizeof g_scaleWrote); // the bank died with the world
        wskel_drop("rig released (no rig skeleton)"); // intact-gated, safe here
        return;
    }

    // Nothing driven means nothing to hand back. Cheap and idempotent, so the
    // edge detector that calls this can fire freely. The weapon lane is
    // dropped FIRST - it can be live while the rig cache is already clean.
    wskel_drop("rig released");
    if (g_hiddenHand < 0 && !g_wasCollapsed && !g_cacheSkelInst && !g_refValid) return;

    // ORDER MATTERS: both restores read g_ref, so g_refValid must still be
    // true here. Clearing it first would silently turn restore_hidden() into a
    // no-op and leave the hand collapsed - the exact failure this fixes.
    int hidden = g_hiddenHand;
    if (hidden >= 0) restore_hidden(hidden);
    // Scale writes persist in the bank across engine evaluations (the engine
    // does not restamp the scale channel - the same fact the sleeve restore
    // rests on), so a scaled cluster must be handed back explicitly too.
    if (g_refValid) {
        for (int i = 0; i < g_boneCount; ++i) {
            if (!g_scaleWrote[i]) continue;
            write_n(g_bones[i].s, g_ref[i].s, 12);
            g_scaleWrote[i] = false;
        }
    } else {
        memset(g_scaleWrote, 0, sizeof g_scaleWrote);
    }
    if (g_wasCollapsed && g_collapsedHand >= 0 && g_bones && g_refValid) {
        const int* sleeve =
            g_collapsedHand == 1 ? patterns::kBoneRSleeve : patterns::kBoneLSleeve;
        const size_t sleeveCount = g_collapsedHand == 1 ? _countof(patterns::kBoneRSleeve)
                                                        : _countof(patterns::kBoneLSleeve);
        for (size_t k = 0; k < sleeveCount; ++k) {
            int idx = sleeve[k];
            if (idx >= g_boneCount) continue;
            write_n(g_bones[idx].p, g_ref[idx].p, 12);
            write_n(g_bones[idx].s, g_ref[idx].s, 12);
        }
    }
    g_hiddenHand = -1;
    g_wasCollapsed = false;
    g_collapsedHand = -1;
    // Both clusters are the engine's again, which is exactly the condition the
    // s64 motion gate needs: a scripted scene releases here, so the wrist it
    // samples carries the authored animation and not our rigid transform.
    g_clWritten[0] = g_clWritten[1] = false;

    // Stop reapply() dead. Its only brakes are the instance check and a 100 ms
    // cache age, so without this it keeps repainting - and re-clearing the
    // dirty flag - for ~6 frames into the cutscene.
    g_cacheSkelInst = nullptr;
    g_cacheMs = 0;
    g_cacheCount = 0;
    g_cacheSleeveCount = 0;
    g_cacheHiddenCount = 0;

    // Hand the skeleton back actively rather than waiting for the engine to
    // notice, then drop the frozen reference so the next drive re-adopts a
    // FRESH engine pose (same reasoning as set_sway_kill(false)) - otherwise
    // the first post-cutscene frame rebuilds the rig, and the muzzle axis the
    // laser and aim dot ride, from a pre-cutscene pose.
    if (g_skelInst) set_dirty(1);
    g_refValid = false;
    g_hasWritten[0] = g_hasWritten[1] = false;
    settle_reset(); // s70: the next switch opens a fresh settle window
    g_lastBigDeltaMs = 0;

    BVR_LOG("[bones] released to the engine (%s): hidden hand %d restored, reapply cache "
            "cleared, dirty flag handed back", why ? why : "?", hidden);
}

void debug_state(int* hiddenHand, unsigned long long* cacheAgeMs, bool* refValid) {
    if (hiddenHand) *hiddenHand = g_hiddenHand;
    if (cacheAgeMs)
        *cacheAgeMs = g_cacheMs ? static_cast<unsigned long long>(GetTickCount64() - g_cacheMs)
                                : 0ULL;
    if (refValid) *refValid = g_refValid;
}

// s70: set_anim_state_mask / anim_state_mask / set_rest_restore / rest_restore /
// set_rest_blend_ms / rest_blend_ms / rest_status / drop_rest all removed with
// the machinery they controlled. See the s70 banners above.

void set_anim_allowed(bool on) {
    const bool was = g_animAllowed.exchange(on, std::memory_order_relaxed);
    if (was != on)
        BVR_LOG("[bones] engine animation %s for this weapon", on ? "ADOPTED" : "gated OFF");
}
bool anim_allowed() { return g_animAllowed.load(std::memory_order_relaxed); }

void set_freeze_only(bool on) {
    g_freezeOnly.store(on, std::memory_order_relaxed);
    set_dirty(1); // let the engine re-evaluate once on the way in or out
}
bool freeze_only() { return g_freezeOnly.load(std::memory_order_relaxed); }

void set_sway_kill(bool on) {
    bool was = g_swayKill.exchange(on, std::memory_order_relaxed);
    if (was && !on) g_refValid = false; // release the frozen pose immediately
    BVR_LOG("[bones] idle-sway kill %s (%s)", on ? "ON" : "off",
            on ? "reference frozen against idle breathing; real animations pass the "
                 "threshold and re-freeze when settled"
               : "reference tracks every engine evaluation - sway visible again");
}

bool sway_kill() {
    return g_swayKill.load(std::memory_order_relaxed);
}

void set_scale(int hand, float s) {
    if (s < 0.05f) s = 0.05f;
    if (s > 20.0f) s = 20.0f;
    if (hand != 1) g_scale[0].store(s, std::memory_order_relaxed);
    if (hand != 0) g_scale[1].store(s, std::memory_order_relaxed);
    // No bank write here: the per-frame drive applies it (and hands the
    // authored scale back on the 1.0 off edge) - commands may run when no
    // skeleton is live.
}

float scale(int hand) {
    return g_scale[hand == 1 ? 1 : 0].load(std::memory_order_relaxed);
}

void set_weapon_scale(float ws) {
    if (ws < 0.05f) ws = 0.05f;
    if (ws > 20.0f) ws = 20.0f;
    g_wScale.store(ws, std::memory_order_relaxed);
}

float weapon_scale() {
    return g_wScale.load(std::memory_order_relaxed);
}

void wskel_drive() {
    float ws = g_wScale.load(std::memory_order_relaxed);
    if (ws == 1.0f) {
        // 1.0 is a total drop on BOTH lanes - no adoption, no writes, no cached
        // pointers. A lane only ever exists while the knob is off 1.0. This is
        // also the one moment the rigid lane can safely restore: the holdable
        // has not changed, so the actor is still the live one.
        if (g_wHoldable) wskel_drop("scale back to 1.0");
        if (g_dsHoldable) ds_drop("scale back to 1.0", true);
        return;
    }
    if (wskel_resolve()) {
        // A skeletal weapon must never carry both lanes - they would compound.
        // Restore ONLY if the rigid lane is bound to the actor still in hand.
        // The first headset run took this branch on a weapon SWITCH (log:
        // "released ... holdable has a skeleton after all"), which meant
        // writing through the outgoing actor - the exact stale-pointer case
        // ds_drop's `restore` flag exists to refuse.
        if (g_dsHoldable) {
            void* live = nullptr;
            const bool same = hands::current_holdable(&live) && live == g_dsHoldable;
            ds_drop("holdable has a skeleton after all", same);
        }
        wskel_compose(ws);
        return;
    }
    // No skeleton to drive: either there is no holdable at all, or it is a
    // rigid mesh (the wrench), in which case scale the actor instead.
    void* hold = nullptr;
    if (!hands::current_holdable(&hold) || !hold) {
        ds_drop("holdable gone", false);
        return;
    }
    ds_drive(hold, ws);
}

void wskel_release(const char* why) {
    if (g_wHoldable) wskel_drop(why);
    // The rigid lane can restore here: release() is called from live sites that
    // still hold the actor, not from the world-change path (which drops it
    // itself, without writing).
    if (g_dsHoldable) ds_drop(why, true);
}

// ---- KEEP THE ENGINE EVALUATING WHILE THE RIG IS HIDDEN --------------------
//
// The arm hide reads the bone array to decide when to come back, and the array
// stopped changing the moment the rig hid: measured over two scenes, 172 and 112
// samples inside a single hidden window, ONE distinct wrist position each. A
// gate that can hide but never un-hide is a bistable latch, and it is why the
// Little Sister bottle catch never brought the arms back.
//
// BRVR's DECISIONS.md names the mechanism without naming this consequence:
// "the engine evaluates once early in the frame, and the dirty byte controls
// whether the RENDER PASS rebuilds again before drawing." In BRVR six passes
// write that byte every frame - sleeve collapse, inactive-hand hide and cluster
// write pushing 0; the three restores pushing 1 - so it is being re-armed
// constantly. Here, release() is idempotent by design: it sets the byte to 1
// ONCE when the scene starts and then early-returns for the rest of the scene,
// so after the render pass consumes that one evaluation nothing ever asks again.
//
// So ask again, every frame, for as long as something is reading the array.
// Cheap (one byte), safe (1 is the engine's own default - it means "you own
// this"), and it cannot fight the drive, which is not running during a scene.
void keep_evaluating(void* handsActor) {
    if (!handsActor || !locate(handsActor)) return;
    set_dirty(1);
}


// ---- DRAW NOTHING WITHOUT LEAVING THE RENDER SET ---------------------------
//
// MEASURED 2026-08-23, and it is the fact this whole lane turns on: with the
// actor at DrawScale3D 0.0001 the bone array is FROZEN - 293 consecutive probe
// samples, 0 of 47 bones moving, every one. Shown, the same probe reports 21-47
// bones moving. Hiding the actor takes it out of whatever the engine animates.
//
// So during a re-check the actor goes back to full scale - the engine animates
// it again and the motion signal becomes honest - and the GEOMETRY is hidden
// here instead, by collapsing the bones that carry it. The player never sees the
// arms; the engine thinks it is drawing them.
//
// WRITE-ONLY, AND THAT IS DELIBERATE. There is no restore path and none is
// needed: the engine re-evaluates the whole array early in each frame, so the
// moment this stops being called the authored pose is back by itself. That also
// means it cannot strand the rig if a scene ends mid-collapse.
//
// THE WEAPON-ATTACH BONE HIDES BY TRANSLATION, NEVER SCALE. The engine's attach
// path inverse-decomposes chain scale (session 16), so a zero there is a divide
// by zero and the gun fills the screen. Same exception, same reason, as the
// session-19 inactive-hand hide this mirrors.
// Our own collapse marker. The sampler reads the array far faster than the
// engine re-evaluates it (CalcView runs 118-240/s against an animation tick well
// below that), so on plenty of calls the bone still holds what WE wrote. Reading
// that back as a 5000-unit delta is not motion; it is no new information, and
// treating it as motion pinned the arms up permanently. hand_motion() compares
// against this and reports "stale" instead.
const float kCollapsePos[3] = {0.0f, 0.0f, -5000.0f};
bool g_collapseArmed = false;

void collapse_rig(void* handsActor) {
    if (!handsActor || !locate(handsActor) || !g_bones) return;

    static const float kZero[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    const float* const kFarBelow = kCollapsePos;
    g_collapseArmed = true;

    // POSITION AS WELL AS SCALE, and the first cut got that wrong. Writing scale
    // alone leaves the collapsed geometry sitting exactly where the arm was, and
    // a zero-scale bone still renders as a degenerate polygon there - reported
    // from a headset as "a strange looking small black polygon where both the
    // right and left arm are supposed to be", once every re-check. The
    // session-19 inactive-hand hide has always written both for this reason; the
    // only thing it does differently is pin at the driven target, which does not
    // exist here, so everything goes far below the actor instead.
    auto hideBone = [&](int idx) {
        if (idx < 0 || idx >= g_boneCount) return;
        write_n(g_bones[idx].p, kFarBelow, 12);
        // The attach bone hides by TRANSLATION ONLY - see the banner above.
        if (idx != patterns::kBoneWeaponAttach) write_n(g_bones[idx].s, kZero, 12);
    };

    // EVERY bone, not just the clusters and sleeves. The first cut collapsed 44
    // of 47 and left the root and spine (0-2) at full scale, which is another way
    // for residual geometry to survive. The engine restores the whole array next
    // frame regardless, so there is nothing to be gained by being selective.
    for (int i = 0; i < g_boneCount; ++i) hideBone(i);

    // The render pass must not rebuild over this before drawing. The engine's
    // own early evaluation next frame is untouched by it - that is exactly the
    // distinction DECISIONS.md draws, and it is what makes this safe.
    set_dirty(0);
}

void end_collapse(void* handsActor) {
    g_collapseArmed = false;
    if (!handsActor || !locate(handsActor)) return;
    set_dirty(1); // let the render pass rebuild the authored pose again
}

int motion_bone() { return motion_bone_idx(); }

bool hand_motion(void* handsActor, float* outSmoothed, float* outRaw, float outPos[3],
                 bool* outStale) {
    if (outStale) *outStale = false;
    const int bone = motion_bone_idx();
    if (bone < 0) return false;

    // Re-resolve rather than trust the cache. This runs on scripted frames,
    // which are exactly the frames drive() does NOT run - so g_bones may be
    // hundreds of frames stale, and locate() is the same check drive() makes
    // before every write.
    if (!handsActor || !locate(handsActor)) return false;
    if (!g_bones || bone >= g_boneCount) return false;

    Qts cur{};
    if (!read_n(&g_bones[bone], &cur, sizeof cur)) return false;

    // OUR OWN WRITE IS NOT A READING. If the bone still carries the collapse we
    // wrote, the engine has not re-evaluated since - so there is no new pose to
    // difference, and differencing anyway produces a 5000-unit spike that reads
    // as violent motion. Report it as stale and let the caller leave the motion
    // state exactly as it was; the next call the engine HAS refreshed is a real
    // sample. Bit-for-bit, because we wrote the value ourselves.
    if (g_collapseArmed && cur.p[0] == kCollapsePos[0] && cur.p[1] == kCollapsePos[1] &&
        cur.p[2] == kCollapsePos[2]) {
        if (outStale) *outStale = true;
        return false;
    }

    // A hand switch changes WHICH bone this is, and the distance between two
    // different bones is not motion. Drop the history instead of measuring
    // across the change.
    if (bone != g_motBone) {
        g_motBone = bone;
        g_motHave = false;
        g_motSmoothed = 0.0f;
    }

    // NO HISTORY IS NOT ZERO MOTION, and reporting it as zero is what hid the
    // arms on the first frame of the very first scene (measured 03:21:20.583:
    // first sample, raw 0.0000, rig hidden the same millisecond). A difference
    // needs two samples. Take this one, then say we cannot answer yet - which
    // the caller turns into arms VISIBLE.
    if (!g_motHave) {
        memcpy(g_motPrevP, cur.p, sizeof g_motPrevP);
        memcpy(g_motPrevQ, cur.q, sizeof g_motPrevQ);
        g_motHave = true;
        return false;
    }

    float raw = 0.0f;
    {
        const float dx = cur.p[0] - g_motPrevP[0];
        const float dy = cur.p[1] - g_motPrevP[1];
        const float dz = cur.p[2] - g_motPrevP[2];
        const float dPos = sqrtf(dx * dx + dy * dy + dz * dz);

        // Quaternion difference: 1 - |dot| is 0 for an identical orientation and
        // grows with the angle between them. A wrist can rotate in place without
        // its position moving at all, so position alone would miss it.
        float dot = 0.0f;
        for (int i = 0; i < 4; ++i) dot += cur.q[i] * g_motPrevQ[i];
        if (dot < 0.0f) dot = -dot;
        if (dot > 1.0f) dot = 1.0f;

        // The 50 is BRVR's, and it is arbitrary by construction - it only makes
        // a small rotation comparable to a small translation. The LOGGED raw and
        // smoothed values are what calibrate the threshold against it; the
        // constant itself is not a measurement and must not be treated as one.
        raw = dPos + (1.0f - dot) * 50.0f;
    }

    memcpy(g_motPrevP, cur.p, sizeof g_motPrevP);
    memcpy(g_motPrevQ, cur.q, sizeof g_motPrevQ);
    g_motHave = true;

    // Peak-hold with decay: a single frame of motion must not be lost between
    // samples, and an animation that eases in and out must not chatter.
    g_motSmoothed *= 0.90f;
    if (raw > g_motSmoothed) g_motSmoothed = raw;

    if (outSmoothed) *outSmoothed = g_motSmoothed;
    if (outRaw) *outRaw = raw;
    // The VALUE, not just the delta. A delta of zero has two completely
    // different causes - the engine holding a pose, or the array no longer being
    // evaluated at all - and only the position tells them apart across a
    // hidden/shown transition. See the ENGINE_NOTES warning this exists to test.
    if (outPos) memcpy(outPos, cur.p, sizeof(float) * 3);
    return true;
}

void set_hide_inactive(bool on) {
    bool was = g_hideInactive.exchange(on, std::memory_order_relaxed);
    if (was != on)
        BVR_LOG("[bones] hideinactive %s (inactive hand %s)", on ? "ON" : "off",
                on ? "collapses while the other drives"
                   : "restores on the next driven frame");
}

bool hide_inactive() { return g_hideInactive.load(std::memory_order_relaxed); }

bool telemetry_on() {
    return g_telemetry.load(std::memory_order_relaxed);
}

float lock_delta_mag() {
    return g_lockDeltaMag.load(std::memory_order_relaxed);
}

bool barrel_ref_axis(float d0[3]) {
    // The rendered barrel axis in the DRIVE TARGET's local frame (UE
    // fwd/right/up). Derivation: drive() writes every cluster quat as
    // qtc (x) q_ref with qtc = inv(q_actor) (x) q_target and positions as a
    // rigid rotation of the reference about the anchor, so the rendered
    // world direction of (bone44ref - bone43ref) is q_target (x) d0 - the
    // actor frame cancels. d0 tracks the reference pose, which IS the
    // per-weapon animation (bone 44 = "muzzle-ish tip", patterns.h), so the
    // muzzle ray is per-weapon automatic and follows any authored sway the
    // engine still plays.
    if (!g_refValid || g_boneCount <= patterns::kBoneRClusterLast) return false;
    const float* pa = g_ref[patterns::kBoneWeaponAttach].p;
    const float* pm = g_ref[patterns::kBoneRClusterLast].p; // bone 44
    float d[3] = {pm[0] - pa[0], pm[1] - pa[1], pm[2] - pa[2]};
    float len = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if (len < 1.0f) return false; // degenerate reference (collapsed rig)
    d0[0] = d[0] / len;
    d0[1] = d[1] / len;
    d0[2] = d[2] / len;
    return true;
}

bool drive(const FrameContext& ctx, void* handsActor, const GamePose& gp, int hand) {
    // Telemetry window: opened here (the once-per-frame pass-1 path) so every
    // module's lines for one sample land together in the log.
    if (g_telemetry.load(std::memory_order_relaxed)) {
        uint64_t now = GetTickCount64();
        g_tlmWindowOpen = (now - g_lastTlmMs >= 200);
        if (g_tlmWindowOpen) g_lastTlmMs = now;
    } else {
        g_tlmWindowOpen = false;
    }

    if (!handsActor || !locate(handsActor)) return false;
    // One-shot derivation of the engine Hands state machine. Backs off on its
    // own and never becomes a per-frame scan once located.
    if (!hands_state::located()) hands_state::locate(handsActor);

    int first = 0, last = 0, anchor = 0;
    cluster_of(hand, &first, &last, &anchor);
    if (first < 0 || last >= g_boneCount || anchor < first || anchor > last) {
        static bool warned = false;
        if (!warned) {
            warned = true;
            BVR_LOG("[bones] %s cluster not configured (first %d last %d anchor %d) - "
                    "use vrbones lcluster",
                    hand == 1 ? "RIGHT" : "LEFT", first, last, anchor);
        }
        return false;
    }

    // Reference refresh: if the anchor no longer holds what we last wrote,
    // the engine re-evaluated and the array is a fresh animated pose.
    Qts cur{};
    if (!read_n(&g_bones[anchor], &cur, sizeof cur)) return false;
    bool engineEvaluated =
        !g_hasWritten[hand] || memcmp(&cur, &g_lastWrittenAnchor[hand], sizeof cur) != 0;

    // ---- s67 ROTATION-SURVIVAL READBACK (BRVR S59, ported) ------------------
    // BRVR measured this on the ACTOR rotator and it is the finding its whole
    // viewmodel fix rests on:
    //
    //   pitch drift 0.0-0.3 deg, yaw drift 0.0-1.2 deg  -> our writes HOLD
    //   roll  drift 5-102 deg, SCALING WITH WRIST TWIST -> the game ERASES roll
    //
    // and then, crucially: "Writing it anyway was actively harmful: the grip
    // correction rotated the offset by a roll the mesh never rendered with,
    // which swung the hand through an arc that grew with the twist."
    //
    // "An arc that grew with the twist" is the s67 symptom exactly - a pure
    // roll sweep of the controller traces an oval that is zero at 0 deg, peaks
    // at 180 and closes at 360. THIS TREE HAS NEVER MEASURED ROLL SURVIVAL on
    // any lane. The equivalent question here is whether the bone quat we wrote
    // last frame is still there this frame, and whether the discrepancy grows
    // with the target's roll.
    //
    // Read-only, throttled, and it prints only while telemetry is on.
    // ALWAYS ON, deliberately. The tester cannot type with both hands on the
    // controllers, so a probe that needs `vrbones telemetry on` is a probe that
    // does not run. One line every 500 ms while the drive is live.
    if (g_hasWritten[hand]) {
        static uint64_t s_lastRollTlm = 0;
        const uint64_t nowRb = GetTickCount64();
        if (nowRb - s_lastRollTlm >= 500) {
            s_lastRollTlm = nowRb;
            // Local-frame delta between OUR last write and what is there now.
            float qwInv[4], d[4];
            quat_conj(g_lastWrittenAnchor[hand].q, qwInv);
            quat_mul(qwInv, cur.q, d);
            // NORMALISE THE DELTA before reading an angle out of it. Both
            // operands come from the bone bank, which does not guarantee unit
            // quats mid-blend, and conj() is not the inverse of a non-unit quat
            // (825ced6). Un-normalised, d[3] is |qw||cur|cos(theta/2) and the
            // components carry the same factor - which is EXACTLY how this probe
            // came to report "drifted 16.64 deg (local x +0.00 y +0.00 z -0.00)"
            // in session 68. Those two readings cannot both be true of a
            // rotation; the rotation was unchanged and only the magnitude
            // differed. It was believed twice and produced 137a44b, a fix for a
            // problem that did not exist.
            const float dn = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2] + d[3] * d[3]);
            if (dn > 1e-4f) {
                d[0] /= dn;
                d[1] /= dn;
                d[2] /= dn;
                d[3] /= dn;
            }
            float w = d[3] < 0.0f ? -d[3] : d[3];
            if (w > 1.0f) w = 1.0f;
            const float driftDeg = 2.0f * acosf(w) * kRadToDeg;
            // Component split in the bone's own frame: for a small delta the
            // imaginary parts are half-angles about the local axes, so this
            // says WHICH channel is being taken away, not just how much.
            const float sgn = d[3] < 0.0f ? -1.0f : 1.0f;
            const float cx = 2.0f * d[0] * sgn * kRadToDeg;
            const float cy = 2.0f * d[1] * sgn * kRadToDeg;
            const float cz = 2.0f * d[2] * sgn * kRadToDeg;
            // The target roll is the independent variable BRVR's finding is a
            // function of. If drift tracks it, the engine is erasing roll.
            const float tgtRollDeg =
                static_cast<float>(static_cast<int16_t>(gp.rot.roll & 0xFFFF)) /
                kRotUnitsPerDegree;
            BVR_LOG("[b1r] ROLLCHECK: target roll %+7.1f deg | our bone write drifted "
                    "%.2f deg (local x %+.2f y %+.2f z %+.2f) | engineEval=%d  %s",
                    tgtRollDeg, driftDeg, cx, cy, cz, engineEvaluated ? 1 : 0,
                    driftDeg > 3.0f ? "<-- OUR WRITE IS BEING CHANGED" : "(write held)");
        }
    }
    // s70: READ AND LOGGED, NEVER GATED ON. The state machine is still the best
    // description of what the rig is doing and it stays in the telemetry, but it
    // no longer decides what reaches the rig - the size threshold and hold window
    // do, exactly as BRVR does it. See the s70 banner on the deleted mask.
    g_lastKnownState = hands_state::current(handsActor);

    if (engineEvaluated || !g_refValid) {
        // Session 20 sway kill: the idle breathing is the authored idle
        // ANIMATION (channel 0/1 - no script parameter to zero, measured),
        // and it reaches the VR rig only through this recapture. With the
        // kill armed, a fresh engine pose replaces the reference ONLY when
        // it differs by a REAL animation's magnitude (equip/reload/melee -
        // they track live and re-freeze when they settle); idle wobble
        // (measured +-1.2 deg / sub-UU on the anchor) stays frozen out.
        Qts fresh[kMaxBones];
        if (!read_n(g_bones, fresh, sizeof(Qts) * static_cast<size_t>(g_boneCount)))
            return false;
        // A WEAPON CHANGE ALWAYS RE-CAPTURES, whatever the animation gate says.
        // s67 bug: with animOn=0 (the wrench) adopt was false unconditionally,
        // so switching to it kept the OUTGOING weapon's reference pose and the
        // wrench was posed as a pistol - reported as "super off with a massive
        // pivot". The gate is about whether to follow animation, not about
        // whether this weapon gets its own pose at all.
        static void* s_lastHoldable = nullptr;
        static void* s_lastAbility = nullptr;
        static bool s_forceCapture = false;
        {
            // s69: IDENTITY IS THE PAIR. CurrentHoldable is NULL for every
            // plasmid, so watching it alone makes every plasmid look like every
            // other one - plasmid-to-plasmid is null != null == false and no
            // recapture happens. The reference then belongs to whichever plasmid
            // was equipped last, and the per-plasmid offsets it is tuned against
            // sit on the wrong pose.
            //
            // The tester's BRVR config is why this matters: with PerPlasmidTuning
            // its rotations differ by tens of degrees between plasmids
            // (PlasmidRot0 -111,-64,22 vs PlasmidRot1 -35,-20,22), because the
            // authored poses genuinely differ. Per-plasmid offsets only hold still
            // if each plasmid also keeps its own captured reference.
            void* liveHold = nullptr;
            if (!hands::current_holdable(&liveHold)) liveHold = nullptr;
            void* liveAbil = nullptr;
            if (!hands::current_ability(&liveAbil)) liveAbil = nullptr;

            // s70: THE PAIR HAS TO HOLD STILL BEFORE IT COUNTS AS A SWITCH.
            //
            // Ported from BRVR (ArmHide.cpp, ArmHide_FreezeWeaponHand), where it
            // is backed by a measurement this tree never took: "what you are
            // holding changed" fired ELEVEN times in a three-minute session in
            // which the player switched weapon TWICE. The engine does not hold
            // CurrentHoldable steady - a shotgun's fire and pump animations park
            // it at NULL for a frame - so every blip is TWO changes,
            // hold -> null -> hold, and each one throws away the reference and
            // recaptures from whatever the rig looked like mid-animation.
            //
            // Watching the (holdable, ability) PAIR does not help here: a NULL
            // blip on the holdable while the ability is also NULL is still a
            // change of the pair. Debouncing is what makes it one.
            //
            // A real switch still lands - the incoming item is a stable non-null
            // key, so it simply commits one debounce later, which is nothing
            // beside the settle window that follows it.
            static void* s_pendHold = nullptr;
            static void* s_pendAbil = nullptr;
            static uint64_t s_pendSince = 0;
            static bool s_pendInit = false;
            const uint64_t nowKey = GetTickCount64();
            if (!s_pendInit) {
                s_pendInit = true;
                s_pendHold = liveHold;
                s_pendAbil = liveAbil;
                s_pendSince = nowKey;
            }
            if (liveHold != s_pendHold || liveAbil != s_pendAbil) {
                s_pendHold = liveHold;
                s_pendAbil = liveAbil;
                s_pendSince = nowKey;
            }
            const bool keyHeld =
                (nowKey - s_pendSince) >= g_keyDebounceMs.load(std::memory_order_relaxed);

            if (keyHeld && (liveHold != s_lastHoldable || liveAbil != s_lastAbility)) {
                s_lastHoldable = liveHold;
                s_lastAbility = liveAbil;
                s_forceCapture = true;
                // s70: OPEN A FRESH SETTLE WINDOW. BRVR releases the cluster
                // here so the equip can play, and stamps the window at the same
                // moment; the settle block above then decides when it is over.
                settle_reset();
                g_settleStartMs = GetTickCount64();
                g_lastBigDeltaMs = g_settleStartMs;
                BVR_LOG("[bones] holding changed (holdable %p, ability %p%s) - forcing a "
                        "fresh reference capture",
                        liveHold, liveAbil, liveAbil ? ", PLASMID" : "");
            }
        }

        // s70: BRVR'S POLICY, AND ONLY BRVR'S. Two branches, not five:
        //   - a switch just happened -> settle, then capture once;
        //   - otherwise -> size threshold + hold window.
        // The per-state mask that used to sit between them is gone; see the s70
        // banner where it was declared.
        bool adopt = true;
        if (s_forceCapture) {
            // ---- BRVR's EQUIP SETTLE (ArmHide.cpp, the g_wpSettling block) ---
            //
            // Release, let the equip animation play, and capture once the rig
            // has STOPPED MOVING - not after a fixed number of milliseconds.
            //
            // The probe is the ANCHOR'S POSITION, frame to frame. s68 spent nine
            // builds probing ANGLES against g_swayAngThreshDeg - the 25 deg
            // animation-adoption threshold - so any two evaluations of a smooth
            // ease read as "still" and the test never gated at all. BRVR probes
            // position against a threshold of its own, and 0.05 model units is
            // three orders of magnitude below the adoption threshold's scale.
            //
            // Three guards, each answering a defect BRVR measured:
            //   FLOOR   (g_settleMinMs) - many draw animations PAUSE part-way
            //           through, and 150 ms of quiet inside a pause is not the
            //           end of the animation.
            //   CEILING (g_settleCeilMs) - a rig that never stills cannot wedge
            //           the window open forever.
            // The capture is the LIVE FRAME either way, exactly as BRVR's
            // CaptureClusterRef takes it. BRVR's mean-of-the-window applies only
            // to its two-hand GRAB POINT - one point, where an average is
            // meaningful - and never to a pose. See the s70b note on the settle
            // state for what happened when that was generalised.
            const uint64_t nowS = GetTickCount64();
            if (anchor < g_boneCount) {
                if (g_settleHavePrev) {
                    const float dx = fresh[anchor].p[0] - g_settlePrevWrist[0];
                    const float dy = fresh[anchor].p[1] - g_settlePrevWrist[1];
                    const float dz = fresh[anchor].p[2] - g_settlePrevWrist[2];
                    if (sqrtf(dx * dx + dy * dy + dz * dz) >
                        g_settleStillUu.load(std::memory_order_relaxed))
                        g_settleLastMoved = nowS;
                }
                memcpy(g_settlePrevWrist, fresh[anchor].p, sizeof g_settlePrevWrist);
                g_settleHavePrev = true;
            }
            ++g_settleN; // sample count, for the log only


            const bool longEnough =
                (nowS - g_settleStartMs) >= g_settleMinMs.load(std::memory_order_relaxed);
            const bool still =
                longEnough && g_settleLastMoved &&
                (nowS - g_settleLastMoved) >= g_settleStillMs.load(std::memory_order_relaxed);
            const bool timedOut =
                (nowS - g_settleStartMs) >= g_settleCeilMs.load(std::memory_order_relaxed);

            if (!still && !timedOut) {
                adopt = true; // still equipping - track it
            } else {
                adopt = true;         // this frame writes g_ref, then we are done
                s_forceCapture = false;
                // The sample count is the measurement that says whether these
                // constants transfer: this block runs only when the engine
                // RE-EVALUATED the bone array. The first run answered it - 24 to
                // 48 evaluations per window, not the 2 or 3 that s68's "1 frame
                // in 19" implied - so the window has plenty of data.
                BVR_LOG("[bones] equip settled after %llu ms over %d engine evaluation(s) "
                        "(%s) - capturing this holdable's live pose as its reference.",
                        static_cast<unsigned long long>(nowS - g_settleStartMs), g_settleN,
                        still ? "rig went still"
                              : "ceiling reached - the rig never stopped moving, so this "
                                "pose is one frame of a loop and may sit off");
                // LATCH THE ORIENTATION THE ACTOR IS ABOUT TO BE ALIGNED TO.
                // Everything after this frame is measured against it, so it has
                // to be taken from the pose we are capturing, not from g_ref -
                // adoption overwrites g_ref, which is the whole point.
                if (anchor < g_boneCount) {
                    const float* aq = fresh[anchor].q;
                    const float n = sqrtf(aq[0] * aq[0] + aq[1] * aq[1] + aq[2] * aq[2] +
                                          aq[3] * aq[3]);
                    if (n > 1e-4f) {
                        for (int k = 0; k < 4; ++k) g_pinAnchorQ[hand][k] = aq[k] / n;
                        g_pinValid[hand] = true;
                    } else {
                        g_pinValid[hand] = false;
                    }
                }
                g_settleN = 0;
                g_settleHavePrev = false;
            }
        } else if (g_refValid && !g_animAllowed.load(std::memory_order_relaxed)) {
            // This weapon does not take engine animation at all (the wrench).
            // Its reference is whatever it settled into and it stays there.
            adopt = false;
        } else if (g_refValid && g_swayKill.load(std::memory_order_relaxed)) {
            adopt = false;
            uint64_t nowMs = GetTickCount64();
            float maxPos = 0.0f, maxAng = 0.0f;
            float dAnchor[3] = {0.0f, 0.0f, 0.0f}; // s70d: signed, for ANIMDIR
            // s70c: PROBE THE DRIVEN HAND'S ANCHOR, AND ONLY IT.
            //
            // This took the MAX over both wrists, so one number stood for two
            // hands: the right hand's movement could trigger adoption for a
            // plasmid on the left, and ANIMREJECT reported a figure that named
            // neither. A plasmid's whole animation measures 2.6-5.0 deg at the
            // palm - inside the weapon hand's idle band - so mixing the two
            // makes the one measurement that matters unreadable.
            //
            // BRVR is per hand throughout: CaptureClusterRef(c, hand) measures
            // `c.wrist` of the cluster it is capturing, with lastBig[hand] and
            // rejPeak[hand] kept separately. drive() already knows its hand, so
            // this is just using it.
            const int kProbe[1] = {anchor};
            for (int k = 0; k < 1; ++k) {
                int b = kProbe[k];
                if (b >= g_boneCount) continue;
                float dp[3] = {fresh[b].p[0] - g_ref[b].p[0], fresh[b].p[1] - g_ref[b].p[1],
                               fresh[b].p[2] - g_ref[b].p[2]};
                float dot = fresh[b].q[0] * g_ref[b].q[0] + fresh[b].q[1] * g_ref[b].q[1] +
                            fresh[b].q[2] * g_ref[b].q[2] + fresh[b].q[3] * g_ref[b].q[3];
                if (dot < 0.0f) dot = -dot;
                if (dot > 1.0f) dot = 1.0f;
                float angDeg = 2.0f * acosf(dot) * kRadToDeg;
                float posUu = sqrtf(dp[0] * dp[0] + dp[1] * dp[1] + dp[2] * dp[2]);
                if (posUu > maxPos) maxPos = posUu;
                if (angDeg > maxAng) maxAng = angDeg;
                memcpy(dAnchor, dp, sizeof dAnchor);
            }
            // Per-hand threshold. The left hand carries the PLASMIDS, and a
            // plasmid's animation is 2.6-5.0 deg at the palm where a weapon
            // reload is 41-135 at the wrist - BRVR's "separate them by size"
            // premise holds within a hand, not across them. One number cannot
            // serve both, and the left cluster is palm + fingers only (6-21,
            // measured s68), so there is no wrist chain to amplify anything.
            const float thresh = (hand == 0)
                                     ? g_swayAngThreshLeftDeg.load(std::memory_order_relaxed)
                                     : g_swayAngThreshDeg.load(std::memory_order_relaxed);
            if (maxPos > g_swayPosThreshUu.load(std::memory_order_relaxed) || maxAng > thresh)
                g_lastBigDeltaHandMs[hand] = nowMs;
            adopt = (nowMs - g_lastBigDeltaHandMs[hand]) <
                    g_swaySettleMs.load(std::memory_order_relaxed);

            // ---- s70d ANIMDIR: WHAT DIRECTION IS THE ANIMATION ACTUALLY GOING?
            //
            // "The electrobolt animates the completely wrong direction" has now
            // survived a change to the freeze anchor, the adoption policy, the
            // drive hand and a 48 deg swing of the model trim yaw. A symptom
            // that does not move when four subsystems do is not in any of them,
            // and this is the measurement that was never taken: the SIGNED
            // model-space displacement of the anchor while the animation plays.
            //
            // Read it as UE component axes - x forward, y right, z up - against
            // the hand the log names. If the reported motion matches what the
            // animation looks like in the headset, the rig is being replayed
            // correctly and the fault is in the frame it is CARRIED by. If it
            // does not match, the fault is in this file.
            //
            // ALWAYS ON and throttled to 1 s, because the tester cannot type
            // with both hands on the controllers.
            if (adopt) {
                static uint64_t s_dirLog[2] = {0, 0};
                if (nowMs - s_dirLog[hand] >= 1000) {
                    s_dirLog[hand] = nowMs;
                    BVR_LOG("[bones] ANIMDIR: %s hand adopting - anchor bone %d moved "
                            "fwd %+.2f right %+.2f up %+.2f UU (|d|=%.2f, %.1f deg) in "
                            "MODEL space this step.",
                            hand == 0 ? "LEFT/plasmid" : "RIGHT/weapon", anchor, dAnchor[0],
                            dAnchor[1], dAnchor[2], maxPos, maxAng);
                }
            }

            // BRVR's rejection-peak report, ported and ALWAYS ON. Its note:
            // "the tester saw recoil on every gun EXCEPT the Tommy gun -- whose
            // per-shot wrist movement simply never reached 12 deg. A threshold
            // picked from one distribution cannot be checked against another it
            // never logs, so this reports the largest thing it REJECTED."
            // s67 has the same symptom ("some tommy gun shots have recoil and
            // others don't"), so this is how the threshold gets chosen from data
            // instead of guessed a third time.
            if (!adopt) {
                static float s_rejPeak[2] = {0.0f, 0.0f};
                static uint64_t s_rejLog[2] = {0, 0};
                if (maxAng > s_rejPeak[hand]) s_rejPeak[hand] = maxAng;
                if (nowMs - s_rejLog[hand] >= 3000) {
                    s_rejLog[hand] = nowMs;
                    BVR_LOG("[bones] ANIMREJECT: %s hand, largest movement REJECTED in the "
                            "last 3 s was %.1f deg at bone %d (threshold %.1f). If an "
                            "animation is missing, set that hand's threshold under this.",
                            hand == 0 ? "LEFT/plasmid" : "RIGHT/weapon", s_rejPeak[hand],
                            anchor, thresh);
                    s_rejPeak[hand] = 0.0f;
                }
            }
            if (g_telemetry.load(std::memory_order_relaxed) &&
                nowMs - g_lastSwayTlmMs >= 1000) {
                g_lastSwayTlmMs = nowMs;
                BVR_LOG("[tlm] sway probe: dpos=%.2f UU dang=%.2f deg (thresh %.1f/%.1f) %s",
                        maxPos, maxAng, g_swayPosThreshUu.load(std::memory_order_relaxed),
                        g_swayAngThreshDeg.load(std::memory_order_relaxed),
                        adopt ? "TRACKING" : "frozen");
            }
        }
        if (adopt || !g_refValid) {
            // Session 61: pin the scale rows of bones OUR scale writes own.
            // If the bank still holds exactly what we last wrote, the engine
            // did not restamp scale (the BS2 behaviour) - adopting it into
            // g_ref would compound refS * s^n on the next compose, so the
            // previous reference row is kept. If the bank differs, the
            // engine genuinely restamped scale (the Infinite behaviour) -
            // adopt it and count it; the vrbones status readout decides
            // pin-vs-adopt is the right architecture from that number.
            if (g_refValid) {
                for (int i = 0; i < g_boneCount; ++i) {
                    if (!g_scaleWrote[i]) continue;
                    if (memcmp(fresh[i].s, g_lastWrittenS[i], 12) == 0) {
                        memcpy(fresh[i].s, g_ref[i].s, 12);
                    } else {
                        g_scaleRestamps.fetch_add(1, std::memory_order_relaxed);
                        g_scaleWrote[i] = false; // the bank is the engine's again
                    }
                }
            }
            memcpy(g_ref, fresh, sizeof(Qts) * static_cast<size_t>(g_boneCount));
            g_refValid = true;

        }
    }

    // s70: the s68 rest-restore block stood here - the adopted-state edge, the
    // canonical-rest snapshot and the smoothstep blend back to it. All of it was
    // downstream of the per-state mask cutting adoption at the recoil apex. With
    // the mask gone the hold window carries adoption to the animation's settled
    // end, so there is nothing to restore FROM and nothing to restore TO.

    // Hide-inactive bookkeeping, BEFORE the rigid write: if the hand about to
    // be driven is the one currently collapsed (hand switch), or the feature
    // just turned off, restore it from the reference first - the rigid write
    // below sets p/q but never touches .s, so a zero scale left behind would
    // keep the incoming hand invisible.
    bool hideInactive = g_hideInactive.load(std::memory_order_relaxed);
    if (g_hiddenHand >= 0 && (!hideInactive || g_hiddenHand == hand)) {
        restore_hidden(g_hiddenHand);
        g_hiddenHand = -1;
    }

    // World target -> component space, composed against the ACTOR transform.
    // THE FRAME MATTERS, and the in-headset telemetry settled it (2026-07-26,
    // session 12 part 3): during an +-80 deg head-yaw sweep the actor rotation
    // held constant (stick-only, no head-look) while the world target stayed
    // solid - and the user saw the gun move REVERSED, which is exactly
    // actor-frame rendering composed against the camera frame. The renderer
    // orients the rig by the ACTOR fields; composing against them makes the
    // bone values head-independent end to end (the head enters only the view
    // matrix, as it should). The one-day detour through "compose against
    // fc.cam" came from misreading a flat screenshot - see STATUS session 12.
    float actorLoc[3];
    int32_t actorRotRaw[3];
    if (!read_n(static_cast<uint8_t*>(handsActor) + patterns::kActorLocOffset, actorLoc, 12) ||
        !read_n(static_cast<uint8_t*>(handsActor) + patterns::kActorViewDirOffset, actorRotRaw,
                12))
        return false;
    FRotator actorRot{actorRotRaw[0], actorRotRaw[1], actorRotRaw[2]};

    float qa[4], qt[4], qaInv[4], qtc[4];
    ue_rot_to_quat(actorRot, qa);
    ue_rot_to_quat(gp.rot, qt);
    quat_conj(qa, qaInv);
    quat_mul(qaInv, qt, qtc); // target rotation, component space

    float dWorld[3] = {gp.loc.x - actorLoc[0], gp.loc.y - actorLoc[1], gp.loc.z - actorLoc[2]};
    float ptc[3];
    qts_rotate(qaInv, dWorld, ptc); // target anchor position, component space

    // Sanity: a target further than ~10 m from the actor is a mapping bug,
    // not a pose - refuse to smear the mesh across the map.
    const bool freezeOnly = g_freezeOnly.load(std::memory_order_relaxed);
    if (!freezeOnly &&
        ptc[0] * ptc[0] + ptc[1] * ptc[1] + ptc[2] * ptc[2] > 500.0f * 500.0f) {
        static bool warned = false;
        if (!warned) {
            warned = true;
            BVR_LOG("[bones] target %.0f UU from actor - refusing (mapping bug?)",
                    sqrtf(ptc[0] * ptc[0] + ptc[1] * ptc[1] + ptc[2] * ptc[2]));
        }
        return false;
    }

    // Render lock: nudge the whole cluster so the renderer's foreground
    // transform lands the anchor on the world-correct pixel (fails soft to
    // the uncorrected pose when no fresh capture exists - e.g. rig off
    // screen, menu, or the watch not hitting).
    // `lockprobe` runs the solve for its telemetry with the lock still off -
    // the APPLY stays gated on the lock mode alone, so a probe can never move
    // a bone.
    int lockMode = g_renderLock.load(std::memory_order_relaxed);
    if (lockMode != 0 || g_lockProbe.load(std::memory_order_relaxed)) {
        float dLat[3], dDepth[3];
        if (render_lock_delta(ctx, gp, qaInv, actorRot, actorLoc, ptc, dLat, dDepth)) {
            if (lockMode != 0) {
                float gl = g_lockGain.load(std::memory_order_relaxed);
                float gd = g_lockDepthGain.load(std::memory_order_relaxed);
                ptc[0] += dLat[0] * gl + dDepth[0] * gd;
                ptc[1] += dLat[1] * gl + dDepth[1] * gd;
                ptc[2] += dLat[2] * gl + dDepth[2] * gd;
            }
            g_lockSolves.fetch_add(1, std::memory_order_relaxed);
        } else {
            g_lockSkips.fetch_add(1, std::memory_order_relaxed);
        }
    }

    if (g_tlmWindowOpen) {
        BVR_LOG("[tlm] cam loc=(%.1f %.1f %.1f) rot=(%d %d %d) base=(%.1f %.1f %.1f) "
                "dyaw=%.2f",
                ctx.camX, ctx.camY, ctx.camZ, ctx.camPitch, ctx.camYaw, ctx.camRoll, ctx.baseX,
                ctx.baseY, ctx.baseZ, ctx.driveYawOffsetRad * kRadToDeg);
        BVR_LOG("[tlm] actor loc=(%.1f %.1f %.1f) rot=(%d %d %d) | target loc=(%.1f %.1f "
                "%.1f) rot=(%d %d %d) hand=%d",
                actorLoc[0], actorLoc[1], actorLoc[2], actorRotRaw[0], actorRotRaw[1],
                actorRotRaw[2], gp.loc.x, gp.loc.y, gp.loc.z, gp.rot.pitch, gp.rot.yaw,
                gp.rot.roll, hand);
        BVR_LOG("[tlm] comp p=(%.2f %.2f %.2f) q=(%.3f %.3f %.3f %.3f) | anchorBefore "
                "p=(%.2f %.2f %.2f) engineEval=%d reapplies=%u lock=%.2f solves=%u skips=%u",
                ptc[0], ptc[1], ptc[2], qtc[0], qtc[1], qtc[2], qtc[3], cur.p[0], cur.p[1],
                cur.p[2], engineEvaluated ? 1 : 0,
                g_reapplies.load(std::memory_order_relaxed),
                g_lockDeltaMag.load(std::memory_order_relaxed),
                g_lockSolves.load(std::memory_order_relaxed),
                g_lockSkips.load(std::memory_order_relaxed));
    }

    // Rigid move: rotate the reference cluster by qtc about the reference
    // anchor point, then put the anchor point at the target. Every write is
    // also cached for reapply() - the stereo second pass must be able to
    // restore this exact set after the engine re-evaluates over it.
    g_cacheCount = 0;
    g_cacheSleeveCount = 0;
    const float* pa = g_ref[anchor].p;
    // s70: THE s69 ANCHOR PIN STOOD HERE, and it is gone.
    //
    // It translated and rotated the whole cluster back onto a captured rest
    // anchor whenever an adopted animation moved it, so the hand could not walk
    // off the controller. Three commits of it (e337532, ac33342, 825ced6, the
    // last one normalising quaternions the pin itself had denormalised).
    //
    // It was compensation for two separate faults, both fixed at source now:
    //
    //   1. The freeze was anchored on bone 43, the WEAPON ATTACH - the one bone
    //      the engine keeps animating even under freeze. Every other bone was
    //      written relative to a moving point, so the cluster was dragged by it
    //      whether or not anything was adopted. cluster_of now anchors the
    //      freeze on the WRIST, as BRVR does.
    //   2. The per-state mask cut adoption at the recoil apex and left the
    //      reference stuck there, so the "animation" the pin was fighting was
    //      often a pose that should have finished. The hold window ends that.
    //
    // BRVR pins nothing. Its WriteCluster is fed the reference's OWN wrist, so
    // the delta is identity and the cluster replays verbatim; when it adopts, it
    // adopts the whole live pose and replays THAT verbatim. An animation moving
    // the hand is the animation doing its job - recoil is supposed to be visible.
    //
    // s70d: EXCEPT FOR ITS HEADING. See the pin banner up top - an adopted
    // animation that turns the anchor re-points the whole hand, because the
    // actor was aligned to the orientation captured at settle. qFix undoes only
    // that turn.
    float qFix[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    bool pinRot = false;
    if (g_freezeOnly.load(std::memory_order_relaxed) &&
        g_animPinRot.load(std::memory_order_relaxed) && g_pinValid[hand] &&
        anchor < g_boneCount) {
        float qInv[4];
        quat_conj(g_ref[anchor].q, qInv);
        quat_mul(g_pinAnchorQ[hand], qInv, qFix);
        const float fn = sqrtf(qFix[0] * qFix[0] + qFix[1] * qFix[1] + qFix[2] * qFix[2] +
                               qFix[3] * qFix[3]);
        if (fn > 1e-4f) {
            for (int k = 0; k < 4; ++k) qFix[k] /= fn;
            pinRot = true;
            // ALWAYS ON, throttled: how far the animation had turned the anchor
            // IS the size of the defect this cancels, so it gets measured.
            static uint64_t s_pinLog[2] = {0, 0};
            const uint64_t nowPin = GetTickCount64();
            const float turn = quat_angle_deg(g_ref[anchor].q, g_pinAnchorQ[hand]);
            if (turn > 3.0f && nowPin - s_pinLog[hand] >= 1000) {
                s_pinLog[hand] = nowPin;
                BVR_LOG("[bones] ANIMPIN: %s hand - the animation had turned anchor bone "
                        "%d by %.1f deg off the captured heading; pinned back. The "
                        "animation keeps its motion, the hand keeps its heading.",
                        hand == 0 ? "LEFT/plasmid" : "RIGHT/weapon", anchor, turn);
            }
        }
    }
    // Viewmodel scale (session 61, see the g_scale block comment): the
    // anchor-relative translations shrink by s for every cluster bone - the
    // cluster scales ABOUT THE ANCHOR, so the anchor write-loc is unchanged
    // by s (the proof metric) - and the .s channel is written only for
    // mode-selected bones, from the PINNED reference, never adopted back.
    // On the off edge (s back to 1.0, mode change) the authored scale is
    // written back explicitly - the engine cannot be relied on to
    // re-evaluate while the drive keeps clearing the dirty flag (the sleeve
    // collapse learned the same lesson).
    const float s = g_scale[hand].load(std::memory_order_relaxed);
    const bool scaling = s != 1.0f;
    const int sMode = g_scaleMode.load(std::memory_order_relaxed);
    for (int i = first; i <= last; ++i) {
        float p[3], q[4];
        if (freezeOnly) {
            // BRVR: the attach bone stays the ENGINE'S. Skipping it is what
            // keeps the weapon's own attachment geometry untouched.
            // BRVR's WriteCluster rule for the attach bone is "position only,
            // never rotation, never scale" - NOT "skip it". s67 shipped it as a
            // skip, which left the engine animating bone 43's POSITION while the
            // rest of the cluster was frozen: the weapon rode that animation and
            // phased through a hand that was standing still. Its rotation still
            // belongs to the engine, which is what BRVR's own log means by
            // "cluster frozen; this bone is still the engine's".
            const bool attachBone = (i == patterns::kBoneWeaponAttach);
            // SCALE STILL APPLIES. Freeze mode drops the RETARGET, not the
            // viewmodel scale: s67 shipped it writing g_ref verbatim and the
            // rig snapped back to authored size ("the hand and weapon scale is
            // too big"). The cluster scales about the anchor exactly as the
            // retarget path does, so the authored pose is preserved in shape
            // and only its size changes.
            float off[3] = {(g_ref[i].p[0] - pa[0]) * s, (g_ref[i].p[1] - pa[1]) * s,
                            (g_ref[i].p[2] - pa[2]) * s};
            // VERBATIM, unless the anchor has been turned by an adopted
            // animation - see the s70d pin banner. qFix maps the animated anchor
            // back onto the orientation the actor was aligned to, and is applied
            // to the whole cluster so the hand keeps its heading while the
            // animation plays inside it. Position is NOT pinned: the motion is
            // correct and only the pointing was wrong.
            //
            // NORMALISED, both qFix and the per-bone product. The bone bank does
            // not hold unit quats while an animation blends, conj() is not the
            // inverse of a non-unit quat, and qts_rotate() would then scale every
            // offset by |q|^2 - which is a vertex explosion, not a rotation
            // (825ced6, and ENGINE_NOTES).
            if (pinRot) {
                float rot[3];
                qts_rotate(qFix, off, rot);
                off[0] = rot[0];
                off[1] = rot[1];
                off[2] = rot[2];
                quat_mul(qFix, g_ref[i].q, q);
                const float bn = sqrtf(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
                if (bn > 1e-4f) {
                    q[0] /= bn;
                    q[1] /= bn;
                    q[2] /= bn;
                    q[3] /= bn;
                } else {
                    memcpy(q, g_ref[i].q, 16);
                }
            } else {
                memcpy(q, g_ref[i].q, 16);
            }
            p[0] = pa[0] + off[0];
            p[1] = pa[1] + off[1];
            p[2] = pa[2] + off[2];
            if (attachBone) {
                // Position always; rotation only if the attach-rotation toggle
                // is on. BRVR ships position-only here and its own log shows
                // the resulting 1.3-1.7 deg of engine drift on this bone, which
                // it lives with. If that drift reads as the weapon swaying
                // against a frozen hand, freezing the rotation too is the fix -
                // BRVR calls the same thing WeaponHandBone43Rot and marks it an
                // untested path. Reachable from the existing F10 checkbox.
                if (g_attachRot.load(std::memory_order_relaxed)) {
                    if (!write_n(g_bones[i].q, q, 16)) {
                        g_skelInst = nullptr;
                        g_cacheMs = 0;
                        return false;
                    }
                }
                if (!write_n(g_bones[i].p, p, 12)) {
                    g_skelInst = nullptr;
                    g_cacheMs = 0;
                    return false;
                }
                CachedBone& cba = g_cache[g_cacheCount++];
                cba.idx = i;
                memcpy(cba.p, p, 12);
                cba.writeScale = false;
                cba.writeRot = g_attachRot.load(std::memory_order_relaxed);
                if (cba.writeRot) memcpy(cba.q, q, 16);
                continue;
            }
        } else {
            float rel[3] = {(g_ref[i].p[0] - pa[0]) * s, (g_ref[i].p[1] - pa[1]) * s,
                            (g_ref[i].p[2] - pa[2]) * s};
            float rot[3];
            qts_rotate(qtc, rel, rot);
            p[0] = ptc[0] + rot[0];
            p[1] = ptc[1] + rot[1];
            p[2] = ptc[2] + rot[2];
            quat_mul(qtc, g_ref[i].q, q);
        }
        // Session 67: the weapon-attach bone can be driven POSITION-ONLY (the
        // BRVR shape - see the g_attachRot banner). Its quat is then left to
        // the engine, which is the whole point: if the attachment path is
        // re-deriving that rotation, writing it too applies it twice.
        const bool wantRot =
            (i != patterns::kBoneWeaponAttach) || g_attachRot.load(std::memory_order_relaxed);
        if (!write_n(g_bones[i].p, p, 12) || (wantRot && !write_n(g_bones[i].q, q, 16))) {
            g_skelInst = nullptr; // faulted mid-write: revalidate next frame
            g_cacheMs = 0;
            return false;
        }
        bool wantS = scaling && scale_selects(sMode, hand, i, first);
        float sv[3];
        if (wantS) {
            sv[0] = g_ref[i].s[0] * s;
            sv[1] = g_ref[i].s[1] * s;
            sv[2] = g_ref[i].s[2] * s;
            if (write_n(g_bones[i].s, sv, 12)) {
                memcpy(g_lastWrittenS[i], sv, 12);
                g_scaleWrote[i] = true;
            } else {
                wantS = false;
            }
        } else if (g_scaleWrote[i]) {
            write_n(g_bones[i].s, g_ref[i].s, 12); // off edge: authored back
            g_scaleWrote[i] = false;
        }
        CachedBone& cb = g_cache[g_cacheCount++];
        cb.idx = i;
        memcpy(cb.p, p, 12);
        memcpy(cb.q, q, 16);
        cb.writeScale = wantS;
        cb.writeRot = wantRot;
        if (wantS) memcpy(cb.s, sv, 12);
    }


    // ---- HANDOFF_9 6.4: DRIVE THE WEAPON ACTOR ITSELF ----------------------
    //
    // SUPERSEDED BEFORE IT WAS EVER RUN - keep it OFF. HANDOFF_11 4.3(c) says
    // direct actor positioning was already tried and found insufficient, because
    // the rendered weapon follows the skeletal attachment matrix rather than the
    // actor's top-level transform. The real answer is the RENDER LOCK above
    // (HANDOFF_11 4.2), which this repo implements and ships switched off. This
    // block stays only because it is the cheap way to confirm 4.3(c) on this
    // build if anyone doubts it: arm it and watch the gun not settle.
    //
    // "gun+0x1D8 / +0x1E4 are real fields the attach system rewrites from the
    // bone every frame ... They are therefore writable in principle ... If that
    // write survives, the gun stops caring what the arms do - firing, reload,
    // empty-idle, equip settle and the shot origin all resolve at once, and the
    // large grip offsets stop being necessary. NEVER TESTED. This is the
    // highest-value unknown in the project."
    //
    // It explains the attach probe above, which I misread. I measured the gun
    // sitting 10-16 UU off the bone, not rotating with it, and concluded the
    // field was inert bookkeeping. BRVR measured the same wander (>20 cm) and
    // read it correctly: the field is LIVE and ANIMATION-driven. The gun does
    // not follow the bone we write - it follows where the fidget/fire animation
    // put the attach socket. That is the desync, and no grip offset or rotation
    // trim can cancel an animation.
    //
    // FIRST QUESTION IS ONLY "DOES THE WRITE SURVIVE". So this writes the
    // controller pose straight onto the weapon actor, no offsets, exactly the
    // dumb test HANDOFF_9 asks for, and reads it back on the NEXT frame before
    // writing again. If the readback matches, the engine let it stand and the
    // real version is worth building. If it drifts, the write is too early and
    // it needs the post-tick path bones::reapply() already uses.
    //
    // OFF BY DEFAULT. It writes an engine actor's transform every frame; that
    // is not something to arm without asking.
    if (g_gunXform.load(std::memory_order_relaxed)) {
        void* hold = nullptr;
        if (hands::current_holdable(&hold) && hold) {
            uint8_t* g = static_cast<uint8_t*>(hold);
            // Readback of LAST frame's write, before this one lands.
            float back[3];
            if (g_gunXfWrote && read_n(g + patterns::kActorLocOffset, back, 12)) {
                const float d[3] = {back[0] - g_gunXfLast[0], back[1] - g_gunXfLast[1],
                                    back[2] - g_gunXfLast[2]};
                const float mag = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
                static uint64_t s_lastMs = 0;
                const uint64_t nowMs = GetTickCount64();
                if (nowMs - s_lastMs >= 1000) {
                    s_lastMs = nowMs;
                    BVR_LOG("[bones] gunxf: wrote (%.1f %.1f %.1f) read back (%.1f %.1f %.1f) "
                            "drift %.2f UU - %s",
                            g_gunXfLast[0], g_gunXfLast[1], g_gunXfLast[2], back[0], back[1],
                            back[2], mag,
                            mag < 0.5f ? "THE WRITE SURVIVED"
                                       : "the engine overwrote it (needs the post-tick path)");
                }
            }
            const float wl[3] = {gp.loc.x, gp.loc.y, gp.loc.z};
            const int32_t wr[3] = {gp.rot.pitch, gp.rot.yaw, gp.rot.roll};
            if (write_n(g + patterns::kActorLocOffset, wl, 12)) {
                write_n(g + patterns::kActorViewDirOffset, wr, 12);
                memcpy(g_gunXfLast, wl, 12);
                g_gunXfWrote = true;
            }
        }
    }

    // ---- ATTACH PROBE: is the weapon actually AT the bone we pivot about? ---
    //
    // Everything upstream of this point measured clean while the viewmodel
    // still desynced as the wrist turned: the weapon's own bone 0 is R_Grip at
    // its model origin, rig bone 43 is R_Grip too, the cluster anchor defaults
    // to that bone, every offset and trim reads 0.00, the render lock is off,
    // and a four-pose rotation sweep at a fixed hand position wrote a
    // bit-identical target every time. A zero offset behaving perfectly is what
    // that sweep proves, so the displacement enters AFTER our write.
    //
    // The one link never measured is the attachment itself. The weapon is a
    // SEPARATE ACTOR attached to the rig bone, so its world transform is
    // `bone * weaponRelative`. A non-zero relative translation swings it about
    // the bone as the bone turns - which is "rotating from hand position rather
    // than gun position" exactly, and is invisible to every check above because
    // all of them live upstream of it.
    //
    // So compare the two directly: where the weapon actor says it is, against
    // where the bone we just wrote actually is in world space. The delta IS the
    // lever arm, if there is one, and it is a property of the attachment rather
    // than of taste - so it can be compensated once for every weapon instead of
    // dialled per weapon.
    {
        static uint64_t s_lastAttachMs = 0;
        const uint64_t nowAtt = GetTickCount64();
        if (nowAtt - s_lastAttachMs >= 1000) {
            s_lastAttachMs = nowAtt;
            void* hold = nullptr;
            float wl[3];
            Qts ab{};
            if (hands::current_holdable(&hold) && hold &&
                read_n(static_cast<uint8_t*>(hold) + patterns::kActorLocOffset, wl, 12) &&
                read_n(&g_bones[anchor], &ab, sizeof ab)) {
                // The bone we just wrote is in COMPONENT space; lift it to world
                // through the actor transform we already decomposed above.
                float bw[3];
                qts_rotate(qa, ab.p, bw);
                bw[0] += actorLoc[0];
                bw[1] += actorLoc[1];
                bw[2] += actorLoc[2];
                const float d[3] = {wl[0] - bw[0], wl[1] - bw[1], wl[2] - bw[2]};
                const float mag = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
                BVR_LOG("[bones] attach probe: weapon loc=(%.1f %.1f %.1f) bone%d "
                        "world=(%.1f %.1f %.1f) delta=(%.2f %.2f %.2f) |d|=%.2f UU",
                        wl[0], wl[1], wl[2], anchor, bw[0], bw[1], bw[2], d[0], d[1], d[2],
                        mag);
            }
        }
    }

    // Sleeve collapse: zero scale hides the geometry; pinning the position at
    // the target keeps any residual skin inside the fist instead of smeared
    // toward the shoulder. On the off-transition the reference values are
    // written back explicitly - the engine cannot be relied on to re-evaluate
    // while the drive keeps clearing the dirty flag.
    bool collapse = g_collapse.load(std::memory_order_relaxed);
    const int* sleeve = hand == 1 ? patterns::kBoneRSleeve : patterns::kBoneLSleeve;
    const size_t sleeveCount = hand == 1 ? _countof(patterns::kBoneRSleeve)
                                         : _countof(patterns::kBoneLSleeve);
    if (collapse) {
        static const float kZero[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        // WHERE THE SLEEVE COLLAPSES TO IS MODE-DEPENDENT, and getting it wrong
        // is a spike of skin out of the palm - the s67 screenshot. The retarget
        // puts the cluster at ptc, so the sleeve belongs there too. FREEZE mode
        // leaves the cluster at its reference (scaled about the anchor), so ptc
        // is nowhere near the hand and collapsing to it smears the arm across
        // the room. BRVR collapses to the WRIST it actually wrote
        // (CollapseArm(kRightSleeve, 5, kRightWrist)); do the same.
        float sleeveTo[3];
        if (freezeOnly) {
            const int wrist = hand == 1 ? patterns::kBoneRWrist : patterns::kBoneLWrist;
            if (wrist < g_boneCount) {
                for (int c = 0; c < 3; ++c)
                    sleeveTo[c] = pa[c] + (g_ref[wrist].p[c] - pa[c]) * s;
            } else {
                memcpy(sleeveTo, pa, 12);
            }
        } else {
            memcpy(sleeveTo, ptc, 12);
        }
        for (size_t k = 0; k < sleeveCount; ++k) {
            int idx = sleeve[k];
            if (idx >= g_boneCount) continue;
            write_n(g_bones[idx].p, sleeveTo, 12);
            write_n(g_bones[idx].s, kZero, 12);
            if (g_cacheSleeveCount < static_cast<int>(_countof(g_cacheSleeve))) {
                CachedSleeve& cs = g_cacheSleeve[g_cacheSleeveCount++];
                cs.idx = idx;
                memcpy(cs.p, sleeveTo, 12);
                memcpy(cs.s, kZero, 12);
            }
        }
    } else if (g_wasCollapsed) {
        for (size_t k = 0; k < sleeveCount; ++k) {
            int idx = sleeve[k];
            if (idx >= g_boneCount) continue;
            write_n(g_bones[idx].p, g_ref[idx].p, 12);
            write_n(g_bones[idx].s, g_ref[idx].s, 12);
        }
    }
    g_wasCollapsed = collapse;
    g_collapsedHand = collapse ? hand : -1;

    // Collapse the whole INACTIVE hand: cluster + its sleeve (session 19).
    // Zero scale hides the skin exactly like the sleeve collapse; positions
    // pin at the driven target so residual geometry stays inside the fist.
    // EXCEPTION - the weapon-attach bone hides by TRANSLATION, scale
    // untouched: the engine's attach path inverse-decomposes chain scale
    // (session 16: any wrist-chain scale blows the attached weapon up
    // near-plane, and zero would be 1/0), so the equipped gun is parked far
    // below the actor in component space and frustum-culled instead.
    g_cacheHiddenCount = 0;
    if (hideInactive) {
        const int ih = 1 - hand;
        int hFirst = 0, hLast = 0, hAnchor = 0;
        cluster_of(ih, &hFirst, &hLast, &hAnchor);
        static const float kZero[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        static const float kFarBelow[3] = {0.0f, 0.0f, -5000.0f};
        const int* hSleeve = ih == 1 ? patterns::kBoneRSleeve : patterns::kBoneLSleeve;
        const size_t hSleeveCount = ih == 1 ? _countof(patterns::kBoneRSleeve)
                                            : _countof(patterns::kBoneLSleeve);
        auto hideBone = [&](int idx) {
            if (idx < 0 || idx >= g_boneCount) return;
            bool isAttach = ih == 1 && idx == patterns::kBoneWeaponAttach;
            const float* p = isAttach ? kFarBelow : ptc;
            write_n(g_bones[idx].p, p, 12);
            if (!isAttach) write_n(g_bones[idx].s, kZero, 12);
            g_scaleWrote[idx] = false; // hide owns the .s channel now
            if (g_cacheHiddenCount < static_cast<int>(_countof(g_cacheHidden))) {
                CachedHidden& ch = g_cacheHidden[g_cacheHiddenCount++];
                ch.idx = idx;
                memcpy(ch.p, p, 12);
                memcpy(ch.s, kZero, 12);
                ch.writeScale = !isAttach;
            }
        };
        for (int i = hFirst; i <= hLast; ++i) hideBone(i);
        for (size_t k = 0; k < hSleeveCount; ++k) hideBone(hSleeve[k]);
        g_hiddenHand = ih;
    }

    // s64 motion gate: record which clusters this frame's write owns, so
    // motion_bone() can pick a wrist that is still the ENGINE's. g_hiddenHand
    // is authoritative for the inactive side - it is restored and cleared above
    // when the feature turns off or the driven hand switches.
    g_clWritten[hand] = true;
    g_clWritten[1 - hand] = (g_hiddenHand == 1 - hand);

    if (!read_n(&g_bones[anchor], &g_lastWrittenAnchor[hand], sizeof(Qts))) return false;
    g_hasWritten[hand] = true;
    set_dirty(0); // render-side evaluate-if-dirty must not rebuild over us
    g_cacheSkelInst = g_skelInst;
    g_cacheMs = GetTickCount64();
    g_writes.fetch_add(1, std::memory_order_relaxed);
    g_lastHand.store(hand, std::memory_order_relaxed);
    return true;
}

void reapply() {
    // Only replay a FRESH write (the paired first pass of this frame). A stale
    // cache must never keep painting an old pose after the drive stops.
    if (!g_cacheSkelInst || g_cacheSkelInst != g_skelInst || !g_bones) return;
    if (GetTickCount64() - g_cacheMs > 100) return;
    g_reapplies.fetch_add(1, std::memory_order_relaxed);
    for (int k = 0; k < g_cacheCount; ++k) {
        const CachedBone& cb = g_cache[k];
        if (cb.idx >= g_boneCount) continue;
        if (!write_n(g_bones[cb.idx].p, cb.p, 12) ||
            (cb.writeRot && !write_n(g_bones[cb.idx].q, cb.q, 16))) {
            g_skelInst = nullptr;
            return;
        }
        if (cb.writeScale) write_n(g_bones[cb.idx].s, cb.s, 12);
    }
    for (int k = 0; k < g_cacheSleeveCount; ++k) {
        const CachedSleeve& cs = g_cacheSleeve[k];
        if (cs.idx >= g_boneCount) continue;
        write_n(g_bones[cs.idx].p, cs.p, 12);
        write_n(g_bones[cs.idx].s, cs.s, 12);
    }
    for (int k = 0; k < g_cacheHiddenCount; ++k) {
        const CachedHidden& ch = g_cacheHidden[k];
        if (ch.idx >= g_boneCount) continue;
        write_n(g_bones[ch.idx].p, ch.p, 12);
        if (ch.writeScale) write_n(g_bones[ch.idx].s, ch.s, 12);
    }
    set_dirty(0);
    // The weapon's own skeleton gets the same second-pass replay - without it
    // the engine's second CalcView re-evaluates the weapon pose over our
    // write and the eyes render different gun sizes.
    if (g_wHoldable && g_wWrittenValid && g_wBones) {
        for (int i = 0; i < g_wBoneCount; ++i) {
            if (!write_n(g_wBones[i].p, g_wWritten[i].p, 12) ||
                !write_n(g_wBones[i].q, g_wWritten[i].q, 16) ||
                !write_n(g_wBones[i].s, g_wWritten[i].s, 12)) {
                g_wSkelInst = nullptr;
                return;
            }
        }
        wskel_set_dirty(0);
    }
}

void handle_command(const char* args) {
    char verb[16] = {};
    int consumed = 0;
    if (sscanf_s(args, "%15s%n", verb, static_cast<unsigned>(sizeof verb), &consumed) != 1) {
        BVR_LOG("[bones] usage: vrbones status|list [n]|poke <idx> <dUU>|freeze on|off|"
                "collapse on|off|attachrot on|off|ref|anchor <idx>|"
                "rest on|off|rest ms <n>|rest drop|"
                "lcluster <lo> <hi> <anchor>");
        return;
    }
    const char* rest = args + consumed;
    while (*rest == ' ' || *rest == '\t') ++rest;

    if (strcmp(verb, "status") == 0) {
        BVR_LOG("[bones] inst=%p bones=%p count=%d writes=%u lastHand=%d refValid=%d "
                "collapse=%d hideinactive=%d hiddenHand=%d",
                g_skelInst, static_cast<void*>(g_bones), g_boneCount,
                g_writes.load(std::memory_order_relaxed),
                g_lastHand.load(std::memory_order_relaxed), g_refValid ? 1 : 0,
                g_collapse.load(std::memory_order_relaxed) ? 1 : 0,
                g_hideInactive.load(std::memory_order_relaxed) ? 1 : 0, g_hiddenHand);
        BVR_LOG("[bones] s70 settle: window=%s floor=%ums still=%ums/%.3fUU ceiling=%ums "
                "debounce=%ums | adopt above %.1f deg, hold %ums | hands state %s",
                g_settleN > 0 ? "OPEN" : "closed",
                g_settleMinMs.load(std::memory_order_relaxed),
                g_settleStillMs.load(std::memory_order_relaxed),
                g_settleStillUu.load(std::memory_order_relaxed),
                g_settleCeilMs.load(std::memory_order_relaxed),
                g_keyDebounceMs.load(std::memory_order_relaxed),
                g_swayAngThreshDeg.load(std::memory_order_relaxed),
                g_swaySettleMs.load(std::memory_order_relaxed),
                hands_state::to_string(g_lastKnownState));
        // Session 30: the drive-residue state, on demand. Until now cacheAge
        // existed only on the `[b1r] cine edge` line and the sleeve latch was
        // printed nowhere at all, so a hands regression check had to provoke a
        // cutscene edge to read the state it wanted to verify. These are the
        // three things release() clears, plus the gate that decides whether it
        // runs, so one command answers "did the hands get handed back".
        BVR_LOG("[bones] drive residue: cacheAge=%llums wasCollapsed=%d collapsedHand=%d "
                "reapplies=%u | cineHold=%d cineDrive=%s",
                static_cast<unsigned long long>(g_cacheMs ? GetTickCount64() - g_cacheMs : 0),
                g_wasCollapsed ? 1 : 0, g_collapsedHand,
                g_reapplies.load(std::memory_order_relaxed),
                bvr::hud::cinematic_hold() ? 1 : 0,
                bvr::vr::cine_drive_name(bvr::vr::cine_drive()));
        int lockMode = g_renderLock.load(std::memory_order_relaxed);
        BVR_LOG("[bones] render lock: %s%s |delta|=%.2f UU gain=%.2f dgain=%.2f solves=%u "
                "skips=%u",
                lockMode == 0 ? "off" : lockMode == 2 ? "DIFF" : "ABS",
                g_lockProbe.load(std::memory_order_relaxed) ? " +PROBE (measure only)" : "",
                g_lockDeltaMag.load(std::memory_order_relaxed),
                g_lockGain.load(std::memory_order_relaxed),
                g_lockDepthGain.load(std::memory_order_relaxed),
                g_lockSolves.load(std::memory_order_relaxed),
                g_lockSkips.load(std::memory_order_relaxed));
        BVR_LOG("[bones] right cluster %d-%d anchor %d | left %d-%d anchor %d",
                patterns::kBoneRClusterFirst, patterns::kBoneRClusterLast,
                g_rAnchorOverride.load() >= 0 ? g_rAnchorOverride.load()
                                              : patterns::kBoneWeaponAttach,
                g_lFirst.load(), g_lLast.load(), g_lAnchor.load());
        BVR_LOG("[bones] weapon-attach bone %d rotation write: %s",
                patterns::kBoneWeaponAttach,
                g_attachRot.load(std::memory_order_relaxed)
                    ? "ON - position + rotation (today's behaviour)"
                    : "OFF - position only (the BRVR shape; s67 desync A/B)");
        int sw = 0;
        for (int i = 0; i < g_boneCount; ++i)
            if (g_scaleWrote[i]) ++sw;
        BVR_LOG("[bones] scale: L=%.3f R=%.3f mode=%d (0 cluster-sans-43/44, 1 fingers, "
                "2 wrist, 3 trans-only) scaleWrote=%d engine scale-restamps=%u",
                g_scale[0].load(std::memory_order_relaxed),
                g_scale[1].load(std::memory_order_relaxed),
                g_scaleMode.load(std::memory_order_relaxed), sw,
                g_scaleRestamps.load(std::memory_order_relaxed));
        BVR_LOG("[bones] wskel: ws=%.3f %s holdable=%p count=%d drives=%u adopts=%u",
                g_wScale.load(std::memory_order_relaxed),
                g_wHoldable ? "BOUND" : "dropped", g_wHoldable, g_wBoneCount, g_wDrives,
                g_wAdopts);
    } else if (strcmp(verb, "list") == 0) {
        if (!g_bones) {
            BVR_LOG("[bones] no skeleton located yet (enable the drive or poke once)");
            return;
        }
        int n = g_boneCount;
        sscanf_s(rest, "%d", &n);
        if (n > g_boneCount) n = g_boneCount;
        for (int i = 0; i < n; ++i) {
            Qts b{};
            if (!read_n(&g_bones[i], &b, sizeof b)) break;
            BVR_LOG("[bones] %2d pos(%7.2f %7.2f %7.2f) quat(%6.3f %6.3f %6.3f %6.3f) "
                    "scale(%.2f %.2f %.2f)",
                    i, b.p[0], b.p[1], b.p[2], b.q[0], b.q[1], b.q[2], b.q[3], b.s[0], b.s[1],
                    b.s[2]);
        }
    } else if (strcmp(verb, "skel") == 0) {
        // Session 20 muzzle probe: dump ANY actor's skeleton WITH bone names.
        // "skel hands" (default) = the AHands rig; "skel weapon" = the
        // equipped weapon's own skeleton, where the muzzle bone lives.
        bool wantWeapon = strncmp(rest, "weapon", 6) == 0;
        void* actor = wantWeapon ? hands::weapon_actor() : hands::hands_actor();
        if (!actor) {
            BVR_LOG("[bones] skel: no live %s actor (arm the drive; fire once for the "
                    "weapon)",
                    wantWeapon ? "weapon" : "hands");
            return;
        }
        Skel sk{};
        if (!resolve_skel(actor, sk)) {
            BVR_LOG("[bones] skel: %s actor %p has no SkeletonInstance at +0x%X (or the "
                    "vtable did not validate)",
                    wantWeapon ? "weapon" : "hands", actor, patterns::kActorSkelInstOffset);
            return;
        }
        const wchar_t* names[kMaxBones];
        int named = resolve_bone_names(sk, names, sk.count);
        BVR_LOG("[bones] skel %s: actor=%p inst=%p bones=%p count=%d (%d named)",
                wantWeapon ? "WEAPON" : "HANDS", actor, sk.inst, static_cast<void*>(sk.bones),
                sk.count, named);
        for (int i = 0; i < sk.count; ++i) {
            Qts b{};
            if (!read_n(&sk.bones[i], &b, sizeof b)) break;
            BVR_LOG("[bones]  %2d %-24S pos(%8.2f %8.2f %8.2f) quat(%6.3f %6.3f %6.3f %6.3f)",
                    i, names[i] ? names[i] : L"<unnamed>", b.p[0], b.p[1], b.p[2], b.q[0],
                    b.q[1], b.q[2], b.q[3]);
        }
    } else if (strcmp(verb, "gunxf") == 0) {
        const bool on = (strncmp(rest, "on", 2) == 0);
        g_gunXform.store(on, std::memory_order_relaxed);
        g_gunXfWrote = false;
        BVR_LOG("[bones] gunxf %s - writing the controller pose onto the WEAPON ACTOR "
                "(HANDOFF_9 6.4). Watch for 'gunxf:' lines.",
                on ? "ON" : "off");
    } else if (strcmp(verb, "poke") == 0) {
        int idx = -1;
        float d = 30.0f;
        if (sscanf_s(rest, "%d %f", &idx, &d) < 1 || !g_bones || idx < 0 ||
            idx >= g_boneCount) {
            BVR_LOG("[bones] usage: vrbones poke <idx> [dUU] (skeleton must be located; "
                    "freeze first or the engine re-evaluates over it)");
            return;
        }
        float z = 0.0f;
        if (read_n(&g_bones[idx].p[2], &z, 4)) {
            z += d;
            if (write_n(&g_bones[idx].p[2], &z, 4)) {
                set_dirty(0);
                BVR_LOG("[bones] poked bone %d z %+0.1f UU -> %.2f", idx, d, z);
            }
        }
    } else if (strcmp(verb, "freeze") == 0) {
        if (!g_skelInst) {
            BVR_LOG("[bones] no skeleton located yet");
            return;
        }
        int v = strncmp(rest, "on", 2) == 0 ? 1 : 0;
        write_n(static_cast<uint8_t*>(g_skelInst) + patterns::kSkelInstFreezeOffset, &v, 4);
        if (!v) set_dirty(1); // unfreeze: let the engine rebuild its own pose
        BVR_LOG("[bones] skeleton freeze %s", v ? "ON" : "off");
    } else if (strcmp(verb, "sway") == 0) {
        // "sway <adoptDeg> [holdMs] [posUu]" - BRVR parity knobs (s67).
        float d = 0.0f, pu = 0.0f;
        unsigned hold = 0;
        int n = sscanf_s(rest, "%f %u %f", &d, &hold, &pu);
        if (n >= 1 && d > 0.0f) {
            g_swayAngThreshDeg.store(d, std::memory_order_relaxed);
            if (n >= 2 && hold > 0) g_swaySettleMs.store(hold, std::memory_order_relaxed);
            if (n >= 3 && pu > 0.0f) g_swayPosThreshUu.store(pu, std::memory_order_relaxed);
        }
        BVR_LOG("[bones] sway: adopt above %.1f deg / %.1f UU, hold %u ms "
                "(BRVR ships 5.0 deg / 1200 ms; measured idle envelope is 4.6 deg / 3.0 UU. "
                "RAISE the degrees if breathing leaks back in, LOWER if animations look "
                "frozen)",
                g_swayAngThreshDeg.load(std::memory_order_relaxed),
                g_swayPosThreshUu.load(std::memory_order_relaxed),
                g_swaySettleMs.load(std::memory_order_relaxed));
    } else if (strcmp(verb, "settle") == 0) {
        // s70: replaces `rest`, `capidle` and `animpin`, which controlled the
        // three pieces of machinery this session deleted.
        // "settle [minMs stillMs ceilMs stillUu debounceMs]"
        unsigned mn = 0, st = 0, ce = 0, db = 0;
        float uu = 0.0f;
        const int n = sscanf_s(rest, "%u %u %u %f %u", &mn, &st, &ce, &uu, &db);
        if (n >= 1 && mn > 0) g_settleMinMs.store(mn, std::memory_order_relaxed);
        if (n >= 2 && st > 0) g_settleStillMs.store(st, std::memory_order_relaxed);
        if (n >= 3 && ce > 0) g_settleCeilMs.store(ce, std::memory_order_relaxed);
        if (n >= 4 && uu > 0.0f) g_settleStillUu.store(uu, std::memory_order_relaxed);
        if (n >= 5) g_keyDebounceMs.store(db, std::memory_order_relaxed);
        BVR_LOG("[bones] settle: floor %u ms, still for %u ms under %.3f UU/frame, "
                "ceiling %u ms, key debounce %u ms (BRVR ships 350 / 150 / 0.05 / 600 "
                "/ 150). Window %s.",
                g_settleMinMs.load(std::memory_order_relaxed),
                g_settleStillMs.load(std::memory_order_relaxed),
                g_settleStillUu.load(std::memory_order_relaxed),
                g_settleCeilMs.load(std::memory_order_relaxed),
                g_keyDebounceMs.load(std::memory_order_relaxed),
                g_settleN > 0 ? "OPEN" : "closed");
    } else if (strcmp(verb, "attachrot") == 0) {
        bool on = strncmp(rest, "on", 2) == 0;
        g_attachRot.store(on, std::memory_order_relaxed);
        // ON is today's behaviour; off hands bone 43's quat back, so ask the
        // engine to re-evaluate once rather than leaving our last write frozen
        // in the array with nothing about to overwrite it.
        set_dirty(1);
        BVR_LOG("[bones] weapon-attach bone %d rotation write %s - the gun should %s",
                patterns::kBoneWeaponAttach, on ? "ON (position + rotation, today)"
                                                : "OFF (position only, the BRVR shape)",
                on ? "out-turn the wrist again if it was doing so"
                   : "still rotate, but stop out-turning the wrist");
    } else if (strcmp(verb, "collapse") == 0) {
        bool on = strncmp(rest, "on", 2) == 0;
        g_collapse.store(on, std::memory_order_relaxed);
        if (!on) set_dirty(1); // engine re-evaluation restores sleeve scales
        BVR_LOG("[bones] sleeve collapse %s", on ? "ON" : "off");
    } else if (strcmp(verb, "ref") == 0) {
        g_refValid = false;
        g_hasWritten[0] = g_hasWritten[1] = false;
        settle_reset();
        set_dirty(1);
        BVR_LOG("[bones] reference pose recapture queued (next engine evaluation), "
                "settle window dropped");
    } else if (strcmp(verb, "anchor") == 0) {
        int idx = -1;
        sscanf_s(rest, "%d", &idx);
        g_rAnchorOverride.store(idx, std::memory_order_relaxed);
        BVR_LOG("[bones] right anchor override = %d (-1 = default %d)", idx,
                patterns::kBoneWeaponAttach);
    } else if (strcmp(verb, "lock") == 0) {
        int mode = strncmp(rest, "off", 3) == 0    ? 0
                   : strncmp(rest, "diff", 4) == 0 ? 2
                                                   : 1; // on/abs
        g_renderLock.store(mode, std::memory_order_relaxed);
        BVR_LOG("[bones] render lock %s", mode == 0   ? "OFF"
                                          : mode == 2 ? "DIFF (head-split cancel only)"
                                                      : "ABS (anchor to true world pixel)");
    } else if (strcmp(verb, "lockprobe") == 0) {
        bool on = strncmp(rest, "on", 2) == 0;
        g_lockProbe.store(on, std::memory_order_relaxed);
        BVR_LOG("[bones] render lock probe %s%s", on ? "ON (measure only, applies nothing)"
                                                     : "off",
                on && !g_telemetry.load(std::memory_order_relaxed)
                    ? " - `vrbones telemetry on` for the [tlm] lock lines"
                    : "");
    } else if (strcmp(verb, "lockgain") == 0) {
        float g = 0.5f;
        if (sscanf_s(rest, "%f", &g) == 1 && g >= 0.0f && g <= 2.0f) {
            g_lockGain.store(g, std::memory_order_relaxed);
            BVR_LOG("[bones] render lock lateral gain = %.2f", g);
        } else {
            BVR_LOG("[bones] usage: vrbones lockgain <0..2> (current %.2f)",
                    g_lockGain.load(std::memory_order_relaxed));
        }
    } else if (strcmp(verb, "lockpull") == 0) {
        float p = 65.0f;
        if (sscanf_s(rest, "%f", &p) == 1 && p >= 0.0f && p <= 200.0f) {
            g_lockPull.store(p, std::memory_order_relaxed);
            BVR_LOG("[bones] fg eye pull-back (matched lens) = %.1f UU", p);
        } else {
            BVR_LOG("[bones] usage: vrbones lockpull <0..200> (current %.1f)",
                    g_lockPull.load(std::memory_order_relaxed));
        }
    } else if (strcmp(verb, "lockdgain") == 0) {
        float g = 0.5f;
        if (sscanf_s(rest, "%f", &g) == 1 && g >= 0.0f && g <= 2.0f) {
            g_lockDepthGain.store(g, std::memory_order_relaxed);
            BVR_LOG("[bones] render lock depth gain = %.2f", g);
        } else {
            BVR_LOG("[bones] usage: vrbones lockdgain <0..2> (current %.2f)",
                    g_lockDepthGain.load(std::memory_order_relaxed));
        }
    } else if (strcmp(verb, "log") == 0) {
        bool on = strncmp(rest, "on", 2) == 0;
        g_telemetry.store(on, std::memory_order_relaxed);
        BVR_LOG("[bones] telemetry %s%s", on ? "ON" : "off",
                on ? " - [tlm] lines at ~5 Hz (head/ctrl/cam/actor/target/bones)" : "");
    } else if (strcmp(verb, "scalemode") == 0) {
        int m = -1;
        if (sscanf_s(rest, "%d", &m) == 1 && m >= 0 && m <= 3) {
            g_scaleMode.store(m, std::memory_order_relaxed);
            // Bones the outgoing mode scaled and the incoming one does not get
            // their authored .s back on the next drive frame (the off-edge
            // branch in the write loop) - no bank write needed here.
            BVR_LOG("[bones] scale mode = %d (%s)", m,
                    m == 0   ? "cluster minus attach/muzzle .s - intended ship mode"
                    : m == 1 ? "fingers-only .s (wrist keeps authored)"
                    : m == 2 ? "wrist-only .s"
                             : "translation-only (no .s writes)");
        } else {
            BVR_LOG("[bones] usage: vrbones scalemode <0..3> (current %d)",
                    g_scaleMode.load(std::memory_order_relaxed));
        }
    } else if (strcmp(verb, "rcluster") == 0) {
        int lo = -1, hi = -1, an = -1;
        int n = sscanf_s(rest, "%d %d %d", &lo, &hi, &an);
        if (n >= 2) {
            g_rFirst.store(lo, std::memory_order_relaxed);
            g_rLast.store(hi, std::memory_order_relaxed);
            if (n >= 3) g_rAnchorOverride.store(an, std::memory_order_relaxed);
            int liveAn = g_rAnchorOverride.load(std::memory_order_relaxed);
            BVR_LOG("[bones] right cluster = %d-%d anchor %d%s", lo, hi,
                    liveAn >= 0 ? liveAn : patterns::kBoneWeaponAttach,
                    (lo == patterns::kBoneWeaponAttach)
                        ? "  - GUN ONLY: hand and arm stay engine-animated"
                        : "");
        } else {
            g_rFirst.store(-1, std::memory_order_relaxed);
            g_rLast.store(-1, std::memory_order_relaxed);
            BVR_LOG("[bones] right cluster reset to the authored %d-%d "
                    "(usage: vrbones rcluster <lo> <hi> [anchor]; "
                    "try 43 44 43 to drive the GUN ALONE)",
                    patterns::kBoneRClusterFirst, patterns::kBoneRClusterLast);
        }
    } else if (strcmp(verb, "lcluster") == 0) {
        int lo = -1, hi = -1, an = -1;
        if (sscanf_s(rest, "%d %d %d", &lo, &hi, &an) == 3) {
            g_lFirst.store(lo, std::memory_order_relaxed);
            g_lLast.store(hi, std::memory_order_relaxed);
            g_lAnchor.store(an, std::memory_order_relaxed);
            BVR_LOG("[bones] left cluster = %d-%d anchor %d", lo, hi, an);
        } else {
            BVR_LOG("[bones] usage: vrbones lcluster <lo> <hi> <anchor>");
        }
    } else {
        BVR_LOG("[bones] unknown command '%s' (status|list|poke|freeze|collapse|ref|anchor|"
                "rcluster|lcluster|scalemode|lock|lockprobe|lockgain|lockdgain|lockpull|log)",
                verb);
    }
}

void draw_debug_ui() {
    ImGui::Text("Bones: count %d writes %u hand %d", g_boneCount,
                g_writes.load(std::memory_order_relaxed),
                g_lastHand.load(std::memory_order_relaxed));
    {
        bool gx = g_gunXform.load(std::memory_order_relaxed);
        if (ImGui::Checkbox("EXPERIMENT: drive the WEAPON ACTOR directly", &gx))
            g_gunXform.store(gx, std::memory_order_relaxed);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(
                "HANDOFF_9 section 6.4, never tested until now. The weapon is a\n"
                "SEPARATE ACTOR whose transform the attach system rewrites from the\n"
                "animated bone every frame - so the gun follows the FIDGET, not your\n"
                "controller, and no grip offset or rotation trim can cancel an\n"
                "animation.\n\n"
                "This writes the controller pose straight onto that actor, no\n"
                "offsets. If the write survives, the gun stops caring what the arms\n"
                "do and the per-weapon offsets stop being necessary.\n\n"
                "Watch the log for \"gunxf:\" - it says whether the write survived.");
    }
    {
        // s67 A/B for the viewmodel rotation desync. Ticked = today. Unticked =
        // the BRVR shape. Judged by eye, so it lives here rather than behind a
        // console verb the tester cannot reach with both hands on the pads.
        bool ar = g_attachRot.load(std::memory_order_relaxed);
        if (ImGui::Checkbox("Write ROTATION to the weapon-attach bone (untick = BRVR)", &ar)) {
            g_attachRot.store(ar, std::memory_order_relaxed);
            set_dirty(1);
            BVR_LOG("[bones] weapon-attach bone %d rotation write %s (F10)",
                    patterns::kBoneWeaponAttach, ar ? "ON (today)" : "OFF (position only)");
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(
                "THE TEST FOR \"the model goes faster than the aim\".\n\n"
                "The weapon is a separate actor attached to bone 43. This tree\n"
                "writes that bone's POSITION and ROTATION; BRVR writes position\n"
                "only, and records why: the attachment path INVERSE-DECOMPOSES the\n"
                "chain. We already know that is true of scale (session 16 - a scale\n"
                "written here blows the gun up near-plane, which is why bone 43 is\n"
                "excluded from every scale write). If it is true of rotation too,\n"
                "our quat is applied to the weapon a SECOND time and the gun turns\n"
                "about twice as far as your wrist did.\n\n"
                "That fits the whole symptom: position tracks fine, only rotation\n"
                "desyncs, and it is correct at exactly ONE wrist pose - the one\n"
                "where the rotation is identity, and identity twice is identity.\n\n"
                "UNTICK IT, hold your arm still and twist your wrist.\n"
                "  - gun still rotates but stops out-turning you -> that was it\n"
                "  - gun stops rotating entirely      -> theory dead, clean result\n"
                "Either answer is worth the run. Retick to compare.");
    }
    {
        // s67: what the drive actually owns. The tester's point - the gun and
        // the arm are one rigid body today and should be separable.
        int rf = g_rFirst.load(std::memory_order_relaxed);
        int sel = (rf == patterns::kBoneWeaponAttach) ? 1 : 0;
        ImGui::TextDisabled("WHAT THE DRIVE MOVES");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(
                "Hand + gun : bones 27-44 move as ONE rigid body - wrist, every\n"
                "             finger and the weapon-attach bone together. The\n"
                "             sleeve (24-26) is NOT moved, so with arms visible\n"
                "             the skin stretches between the two.\n"
                "Gun only   : bones 43-44 only. The gun goes to your controller;\n"
                "             the hand, fingers and arm stay engine-animated.\n\n"
                "GUN ONLY IS A CONTROL CONDITION, not a shippable look - the hand\n"
                "will not appear to hold the gun. What it answers is whether the\n"
                "GUN ALONE tracks your controller cleanly. If it does, the desync\n"
                "is in the cluster drive's interaction with the rig, not in the\n"
                "gun's own transform.");
        if (ImGui::RadioButton("hand + gun (27-44)", &sel, 0)) {
            g_rFirst.store(-1, std::memory_order_relaxed);
            g_rLast.store(-1, std::memory_order_relaxed);
            BVR_LOG("[bones] right cluster = authored %d-%d (F10)",
                    patterns::kBoneRClusterFirst, patterns::kBoneRClusterLast);
        }
        ImGui::SameLine();
        if (ImGui::RadioButton("gun only (43-44)", &sel, 1)) {
            g_rFirst.store(patterns::kBoneWeaponAttach, std::memory_order_relaxed);
            g_rLast.store(patterns::kBoneRClusterLast, std::memory_order_relaxed);
            g_rAnchorOverride.store(patterns::kBoneWeaponAttach, std::memory_order_relaxed);
            BVR_LOG("[bones] right cluster = %d-%d GUN ONLY (F10) - hand/arm left to "
                    "the engine",
                    patterns::kBoneWeaponAttach, patterns::kBoneRClusterLast);
        }
    }

    {
        // s70: WHICH BONE THE FROZEN CLUSTER IS ANCHORED ON. The one control
        // that needs no rebuild to answer the biggest open question, and it is a
        // RADIO rather than a console verb on purpose - the tester cannot type
        // with both hands on the controllers.
        ImGui::TextDisabled("FREEZE ANCHOR - the bone everything else hangs off");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(
                "Wrist 27 is BRVR's. Its cluster spec is {27, 44, wrist 27} and\n"
                "every replay is anchored on that wrist.\n\n"
                "Attach 43 is what this tree shipped, and it is the WEAPON ATTACH\n"
                "bone - the one bone BRVR deliberately leaves the engine still\n"
                "animating. Its own log: cluster frozen, this bone is still the\n"
                "engine's, with 1-5 deg of idle drift and peaks of 41-135.\n\n"
                "Anchoring the freeze on a bone the engine animates writes every\n"
                "other bone relative to a MOVING point, so the whole cluster is\n"
                "dragged along by it. If the hand walks off the controller during\n"
                "recoil or a reload, this is the first thing to try.\n\n"
                "EXPECT THE GUN TO SIT SLIGHTLY DIFFERENTLY. The cluster is scaled\n"
                "to 0.80 ABOUT THIS ANCHOR, so moving it shifts the whole hand by\n"
                "0.2 x the distance between the two bones. That is a placement\n"
                "re-tune, not a fault - judge this switch on whether the hand\n"
                "STAYS PUT during animation, not on where it sits.");
        const int curAnchor = g_rAnchorOverride.load(std::memory_order_relaxed);
        int asel = (curAnchor == patterns::kBoneWeaponAttach) ? 1
                   : (curAnchor == patterns::kBoneRWrist)     ? 0
                                                              : 0; // -1 = wrist in freeze
        if (ImGui::RadioButton("wrist 27 (BRVR)", &asel, 0)) {
            g_rAnchorOverride.store(-1, std::memory_order_relaxed);
            set_dirty(1);
            BVR_LOG("[bones] freeze anchor = WRIST %d (F10, BRVR's own) - the cluster "
                    "now hangs off a bone we write, not one the engine animates",
                    patterns::kBoneRWrist);
        }
        ImGui::SameLine();
        if (ImGui::RadioButton("attach 43 (old)", &asel, 1)) {
            g_rAnchorOverride.store(patterns::kBoneWeaponAttach, std::memory_order_relaxed);
            set_dirty(1);
            BVR_LOG("[bones] freeze anchor = WEAPON ATTACH %d (F10) - the pre-s70 "
                    "behaviour; this bone is still engine-animated",
                    patterns::kBoneWeaponAttach);
        }
    }

    {
        // s67: the animation-adoption threshold, live. See the g_swayAngThreshDeg
        // banner - these are BRVR's HandAnimMinDeg / HandAnimHoldMs, and the old
        // hardcoded 12 deg / 600 ms is what froze real animation out.
        ImGui::TextDisabled("ANIMATION - what reaches the rig");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(
                "A delta bigger than the threshold is treated as a REAL animation\n"
                "(reload, equip, melee) and adopted; anything smaller is idle\n"
                "breathing and frozen out.\n\n"
                "Measured idle envelope: 4.6 deg. BRVR uses 5.0, just above it.\n"
                "This tree used 12.0, which is more than double idle and rejects\n"
                "real animation too - the rig goes rigid and the arm sits straight\n"
                "instead of in its natural pose.\n\n"
                "Arms look frozen or straight -> LOWER the degrees.\n"
                "Breathing wobble is back  -> RAISE the degrees.");
        float sd = g_swayAngThreshDeg.load(std::memory_order_relaxed);
        if (ImGui::SliderFloat("adopt above (deg)", &sd, 1.0f, 20.0f))
            g_swayAngThreshDeg.store(sd, std::memory_order_relaxed);
        int sh = static_cast<int>(g_swaySettleMs.load(std::memory_order_relaxed));
        if (ImGui::SliderInt("hold (ms)", &sh, 100, 3000))
            g_swaySettleMs.store(static_cast<unsigned>(sh < 1 ? 1 : sh),
                                 std::memory_order_relaxed);
        bool sk = g_swayKill.load(std::memory_order_relaxed);
        if (ImGui::Checkbox("Freeze idle sway (untick = adopt everything)", &sk))
            g_swayKill.store(sk, std::memory_order_relaxed);

        // s70: BRVR's settle window, live. Replaces the animpin / capidle /
        // rest-restore controls that stood here - all three drove machinery this
        // session deleted. These are the numbers BRVR ships and the ones the
        // headset run needs to be able to move.
        bool apr = g_animPinRot.load(std::memory_order_relaxed);
        if (ImGui::Checkbox("Animations cannot re-point the hand", &apr))
            g_animPinRot.store(apr, std::memory_order_relaxed);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(
                "The rig is carried to your controller by the ACTOR, which is aligned\n"
                "to the orientation the hand had when its pose was captured. An\n"
                "adopted animation TURNS that anchor, and the actor knows nothing\n"
                "about it - so the whole hand points somewhere else for as long as\n"
                "the animation plays.\n\n"
                "Reported on the plasmids: \"the animation is correct but it points\n"
                "like 90 degrees left when the animation is going\".\n\n"
                "ON  : the animation keeps its MOTION and the hand keeps its heading.\n"
                "off : the s70 behaviour, for comparison.\n\n"
                "Position is never pinned - only the pointing was ever wrong.");

        ImGui::TextDisabled("EQUIP SETTLE - when the pose is captured after a switch");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(
                "On a weapon or plasmid switch the rig follows the equip animation,\n"
                "and the pose is captured once it stops moving.\n\n"
                "floor    : ignore stillness before this - many draws PAUSE part-way\n"
                "           through, and quiet inside a pause is not the end.\n"
                "still    : how long the anchor must stay under the threshold.\n"
                "ceiling  : give up waiting, and capture the live pose anyway. A\n"
                "           looping idle never goes still, so a capture that hits the\n"
                "           ceiling is one frame of that loop and may sit off.\n"
                "debounce : the engine parks CurrentHoldable at NULL for a frame\n"
                "           during fire and pump animations. Without this, those\n"
                "           blips read as weapon switches.");
        int smn = static_cast<int>(g_settleMinMs.load(std::memory_order_relaxed));
        if (ImGui::SliderInt("floor (ms)", &smn, 0, 1500))
            g_settleMinMs.store(static_cast<unsigned>(smn < 0 ? 0 : smn),
                                std::memory_order_relaxed);
        int sst = static_cast<int>(g_settleStillMs.load(std::memory_order_relaxed));
        if (ImGui::SliderInt("still for (ms)", &sst, 0, 1000))
            g_settleStillMs.store(static_cast<unsigned>(sst < 0 ? 0 : sst),
                                  std::memory_order_relaxed);
        int sce = static_cast<int>(g_settleCeilMs.load(std::memory_order_relaxed));
        if (ImGui::SliderInt("ceiling (ms)", &sce, 100, 4000))
            g_settleCeilMs.store(static_cast<unsigned>(sce < 1 ? 1 : sce),
                                 std::memory_order_relaxed);
        float suu = g_settleStillUu.load(std::memory_order_relaxed);
        if (ImGui::SliderFloat("still under (UU/frame)", &suu, 0.01f, 2.0f, "%.3f"))
            g_settleStillUu.store(suu, std::memory_order_relaxed);
        int sdb = static_cast<int>(g_keyDebounceMs.load(std::memory_order_relaxed));
        if (ImGui::SliderInt("switch debounce (ms)", &sdb, 0, 1000))
            g_keyDebounceMs.store(static_cast<unsigned>(sdb < 0 ? 0 : sdb),
                                  std::memory_order_relaxed);
        ImGui::Text("settle window: %s | state %s", g_settleN > 0 ? "OPEN" : "closed",
                    hands_state::to_string(g_lastKnownState));
    }

    bool col = g_collapse.load(std::memory_order_relaxed);
    if (ImGui::Checkbox("Hide the driven arm (collapse sleeve bones)", &col))
        g_collapse.store(col, std::memory_order_relaxed);
    bool hide = g_hideInactive.load(std::memory_order_relaxed);
    if (ImGui::Checkbox("Hide the inactive hand", &hide))
        set_hide_inactive(hide);
    int lockMode = g_renderLock.load(std::memory_order_relaxed);
    if (ImGui::RadioButton("lock off", &lockMode, 0) ||
        ImGui::RadioButton("lock ABS (true position)", &lockMode, 1) ||
        ImGui::RadioButton("lock DIFF (head-split cancel only)", &lockMode, 2))
        g_renderLock.store(lockMode, std::memory_order_relaxed);
    float gl = g_lockGain.load(std::memory_order_relaxed);
    if (ImGui::SliderFloat("lock lateral gain", &gl, 0.0f, 2.0f, "%.2f"))
        g_lockGain.store(gl, std::memory_order_relaxed);
    float gd = g_lockDepthGain.load(std::memory_order_relaxed);
    if (ImGui::SliderFloat("lock depth gain", &gd, 0.0f, 2.0f, "%.2f"))
        g_lockDepthGain.store(gd, std::memory_order_relaxed);
    ImGui::Text("lock |delta| %.1f UU, solves %u, skips %u",
                g_lockDeltaMag.load(std::memory_order_relaxed),
                g_lockSolves.load(std::memory_order_relaxed),
                g_lockSkips.load(std::memory_order_relaxed));
    (void)g_status;
}

} // namespace bvr::b1r::bones
