#include "game/bioshock2r/bones.h"

#include "core/gfx/hud_capture.h" // session 42: cinematic_hold (residue line)
#include "core/hooks/pattern_scan.h"
#include "core/util/log.h"
#include "core/util/xr_math.h"
#include "core/vr/openxr_runtime.h" // session 42: cine_drive (residue line)
#include "game/bioshock2r/aim.h"
#include "game/bioshock2r/patterns.h"
#include "game/bioshock2r/scenedraw.h"

#include <windows.h>
#include <MinHook.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace bvr::b2r::bones {
namespace {

using bvr::pattern_scan::is_memory_valid;

constexpr int kMaxBones = 128;

// Resolved rig (game thread only; revalidated every use, never trusted stale).
uint8_t* g_hands = nullptr;     // AHands actor
uint8_t* g_skel = nullptr;      // its SkeletonInstance
uint8_t* g_pose = nullptr;      // bone transform bank (+0x44 data)
int g_boneCount = 0;
uint64_t g_lastScanMs = 0;
int g_scanMisses = 0;
bool g_scanDormant = false;

// Reference pose: the engine's own component-space transforms, captured on
// the first drive after (re)resolve. The drive rigidly re-places THIS, so
// idle sway frozen at capture time stays frozen (BS1's sway_kill, inherent
// here rather than optional).
float g_ref[kMaxBones][12];
bool g_refValid = false;

// Engine-adopted pose (session 41, the animation-preserving drive): the
// trans+quat rows track the engine's own freshly-evaluated animation - a bone
// is adopted only when the bank differs from OUR last write, so the drive's
// own output can never feed back. The SCALE rows stay PINNED to g_ref
// forever: the engine never restamps scale (poke-proven, patterns.h), so the
// bank's scale bytes are always ours and adopting them would compound
// g_scale geometrically (and could adopt arms-hide's zero).
float g_anim[kMaxBones][12];
std::atomic<bool> g_animMode{true};   // vrhands anim on|off (off = rigid ref drive)
std::atomic<float> g_animTrans{0.0f}; // wrist authored-travel re-add, 0..1
std::atomic<uint32_t> g_adopts[2] = {{0}, {0}}; // engine restamps absorbed

// Per-frame compose inputs, cached so the absorb-repaint can RECOMPOSE a
// mid-draw restamp instead of restoring a stale write (game thread only).
struct ComposeCache {
    float qtc[4];
    float ptc[3];
    float ptcExtra[3]; // the attach pivot's base: ptc + the weapon offset
    float scale;
    float animT;
    int anchor;
    int extra;
    bool animMode;
    bool valid;
};
ComposeCache g_compose[2] = {};

// Weapon offset (session 41 round 2, user ask): move the WEAPON relative to
// the hand - scaling revealed the gun sitting ahead of the hand model. The
// lever is the attach pivot (bone 63): offsetting only its position moves
// what renders FROM it (the weapon) while fingers/wrist stay put, and the
// aim ray is untouched (it never reads the model). cm in the hand's trimmed
// basis - the same frame as the model offset sliders; per-weapon profile
// field. hands.cpp converts to a game-space UU vector each frame.
// fwd, right, up; default = user calibration (s41 r3 bake)
std::atomic<float> g_wOffCm[3] = {{-6.30f}, {0.0f}, {0.0f}};
float g_wOffGame[3] = {}; // this frame's converted vector (game thread)

// --- the weapon's OWN skeleton: uniform scale (session 41) ------------------
// The AHands pivot-63 scale channel is inverse-decomposed by attachment math
// (the ammo-canister proof, session 40) - uniform weapon scaling is only
// reachable through the holdable's own SkeletonInstance
// (kWeaponSkelInstOffset). Scale s multiplies BOTH the bone scale channels
// and the bone translations (uniform about the component origin, so parts
// keep their relative layout); trans+quat come from a per-bone adopted bank
// (the Priority-2 adoption shape, duplicated - own banks, never grows the
// AHands g_written) so weapon animations (drill spin, pump) keep playing
// while scaled; the SCALE channel composes from the captured reference only,
// never adopted - the engine does not restamp scale, adopting it would
// re-absorb our own writes and compound.
uint8_t* g_wHold = nullptr;
uint8_t* g_wSkel = nullptr;
uint8_t* g_wPose = nullptr;
int g_wBoneCount = 0;
float g_wRef[kMaxBones][12];
float g_wAnim[kMaxBones][12];
float g_wWritten[kMaxBones][12];
bool g_wWrittenValid[kMaxBones] = {};
std::atomic<float> g_wScale{0.770f}; // user-calibration default (s41 r3 bake)
uint64_t g_wStampMs = 0;
std::atomic<uint32_t> g_wAdopts{0};
uint32_t g_wDrives = 0;

// Bone names (diagnostic; see the name-map section below).
char g_boneNames[kMaxBones][48];
bool g_namesValid = false;
int g_nameMapOffset = -1; // -1 = not yet detected

// Clusters (session 40): per hand, a contiguous [lo, hi] range PLUS one extra
// bone - the hand-pivot target the weapon/plasmid actually renders from, which
// sits at the end of the rig rather than beside the fingers. Derived from the
// live bone-name map (ENGINE_NOTES session 40): left = wrist 7 + fingers 8..28
// + pivot 62, right = wrist 36 + fingers 37..57 + pivot 63. The anchor is the
// bone the attachment renders from (BS1's rule, re-derived here by driving
// bone 63 alone and watching the weapon move).
struct Cluster {
    int lo, hi, extra, anchor;
};
Cluster g_cluster[2] = {
    {7, 28, 62, 7},   // left / plasmid hand
    {36, 57, 63, 63}, // right / weapon hand
};
// Default = the user's calibration (session 41 round 3 bake; preset overrides).
std::atomic<float> g_scale[2] = {{0.771f}, {0.760f}};
// Session 41: default OFF - the uniform weapon scale (wskel lane) supersedes
// scaling the attach pivot, whose scale attachments inverse-decompose (the
// canister proof). Kept as the F10 fallback toggle; preset key overrides.
std::atomic<bool> g_scaleAttach{false};
std::atomic<int> g_armsMode{1};        // 0 game, 1 follow (user test default), 2 hide
int g_armsApplied[2] = {-1, -1};       // game thread; drives the transition restore
std::atomic<uint32_t> g_peRepaints{0}; // restamps caught mid-draw (flicker fix)

// --- Session 42: flicker DIAGNOSIS instrumentation (counters only) ----------
// The surviving left-eye flicker (~10 min onset per the user) outlived both
// repaint rungs; before the next fix we need to know WHERE in the pass
// timeline the surviving restamps land and WHEN the cadence changes. Phase =
// where a restamp was DISCOVERED: 0/1 = PE-lane repaint pass 1/2, 2/3 =
// flush-point repaint pass 1/2. kind: 0 = hands bank, 1 = weapon skeleton.
// IMPORTANT for the readout: with anim mode ON the drive ADOPTS a fresh
// engine pose nearly every frame - g_driveAdoptEvents is the restamp-cadence
// BASELINE, not a survivor count. The survivor signal is the late-window
// catches (fl*) and a large write->catch latency (g_catchDeltaMaxMs): a
// restamp that sat unrepainted for most of the pass plausibly rendered.
// Session 74 widens the phase table: 4/5 = the pass-ENTRY repaint (site 2),
// the fix candidate armed by `vrbones p1fix|p2fix`.
// 6/7 = the post-WRITER repaint (site 3, the s74 fix: right after the engine's
// pose writer returns inside pass 1).
std::atomic<uint32_t> g_catch[8][2] = {};
std::atomic<uint32_t> g_catchDeltaMaxMs[8] = {}; // window max, drained on snapshot
std::atomic<uint32_t> g_driveAdoptEvents[2] = {}; // [0] hand drives, [1] wskel drives
std::atomic<uint32_t> g_worldChanges{0};
std::atomic<uint32_t> g_wRescans{0};
std::atomic<bool> g_flickLog{true}; // [flick] minute line (counters always count)

// --- Session 74: the pose-RACE probe ----------------------------------------
// The per-eye captures proved the RIGHT eye renders the engine's authored pose
// every tick while fl2 stayed 0 - so the bank is ours at pass-2 flush yet the
// image is not. This probe samples the same sentinel at six points of the
// pair (pass 1 entry / flush / post-drain, pass 2 entry / flush / post-drain)
// and counts "foreign at this point", which says WHERE the bank flips instead
// of arguing it from counters that only see the repaint sites.
std::atomic<uint32_t> g_probeCalls[6] = {};
std::atomic<uint32_t> g_probeForeign[6][2] = {}; // [point][0 hands, 1 weapon]
std::atomic<int> g_probeFirstDiff[6] = {};       // last first-differing bone index seen
std::atomic<bool> g_entryFix[2] = {}; // repaint at pass 1 / pass 2 ENTRY
// The s41 sentinel compares ONE bone (the first masked) - a restamp that
// leaves that bone alone and moves the elbow/wrist/gun bones is invisible to
// it. fullmask makes the repaint decision (and the probe, always) scan every
// masked bone: `vrbones fullmask on|off`.
std::atomic<bool> g_fullMask{false};
// The race probe is a diagnostic (its answer is in ENGINE_NOTES s74); it costs
// syscalls at six points per pair, so it is OFF unless armed (`vrbones race
// on`, or diag31).
std::atomic<bool> g_raceProbe{false};
// The writer hook installs from the game thread's poll lane (outside hooked
// calls), never from inside a Draw or the overlay: rig resolve and the F10
// checkbox only POST the request here.
std::atomic<bool> g_wfixPending{false};

void note_catch(int phase, int kind, uint32_t latMs) {
    if (phase < 0 || phase > 7) return;
    g_catch[phase][kind].fetch_add(1, std::memory_order_relaxed);
    // Torn max under the dev-only threaded substrate is an accepted diagnostic
    // hazard (matches pe_repaint's existing exposure); shipping config is 1t.
    if (latMs > g_catchDeltaMaxMs[phase].load(std::memory_order_relaxed))
        g_catchDeltaMaxMs[phase].store(latMs, std::memory_order_relaxed);
}

// Last write per hand, for reapply/repaint/release + telemetry. A bone MASK
// rather than ranges (session 40 round 2): the written set is no longer
// contiguous once the arm bones ride along.
float g_written[kMaxBones][12];
bool g_writtenMask[2][kMaxBones] = {};
// How each masked bone was written (0 = full, 1 = no scale channel, 2 =
// hidden/collapsed) so the absorb-repaint can recompose it identically.
uint8_t g_writeKind[2][kMaxBones] = {};
uint64_t g_writeStampMs[2] = {};
float g_lastWriteLoc[2][3] = {};
uint32_t g_driveCount[2] = {};

const uint8_t* g_imageBase = nullptr;

bool accept_hands(void* obj, void*) {
    // Two-factor identity: the +0x430 pointer must be a SkeletonInstance
    // (vtable dword) whose owner backpointer is this very actor.
    uint8_t* hands = static_cast<uint8_t*>(obj);
    if (!is_memory_valid(hands + patterns::kAHandsSkelInstOffset, sizeof(void*)))
        return false;
    uint8_t* skel = *reinterpret_cast<uint8_t**>(hands + patterns::kAHandsSkelInstOffset);
    if (!skel || !is_memory_valid(skel, 0x60)) return false;
    if (*reinterpret_cast<const uint8_t* const*>(skel) !=
        g_imageBase + patterns::kSkeletonInstanceVtableRva)
        return false;
    if (*reinterpret_cast<uint8_t**>(skel + patterns::kSkelOwnerOffset) != hands)
        return false;
    return true;
}

void drop(const char* why) {
    if (g_hands || g_refValid)
        BVR_LOG("[b2r] bones: cache dropped (%s)", why);
    g_hands = nullptr;
    g_skel = nullptr;
    g_pose = nullptr;
    g_boneCount = 0;
    g_refValid = false;
    memset(g_writtenMask, 0, sizeof g_writtenMask);
    g_armsApplied[0] = g_armsApplied[1] = -1;
    g_namesValid = false;
    g_compose[0].valid = g_compose[1].valid = false;
}

// Revalidate the cached rig or (rate-limited, dormancy-guarded) rescan.
bool resolve_rig() {
    if (!g_imageBase) g_imageBase = nullptr; // set below on first scan
    if (g_hands) {
        // Cheap revalidation: same vtable, same skeleton, same pose bank.
        if (is_memory_valid(g_hands, sizeof(void*)) &&
            *reinterpret_cast<const uint8_t* const*>(g_hands) ==
                g_imageBase + patterns::kAHandsVtableRva &&
            accept_hands(g_hands, nullptr)) {
            return true;
        }
        drop("revalidation failed");
    }

    if (g_scanDormant) return false;
    uint64_t now = GetTickCount64();
    if (now - g_lastScanMs < 3000) return false;
    g_lastScanMs = now;

    bvr::pattern_scan::ProcessImage img{};
    if (!bvr::pattern_scan::capture_main_module(img)) return false;
    g_imageBase = img.base;

    int matches = 0;
    void* found = patterns::scan_for_vtable_object(
        patterns::kAHandsVtableRva, patterns::kAHandsSkelInstOffset + sizeof(void*),
        &accept_hands, nullptr, "AHands", &matches);
    if (!found) {
        if (++g_scanMisses >= 3) {
            g_scanDormant = true;
            BVR_LOG("[b2r] bones: AHands scan DORMANT after %d misses (view-state "
                    "change re-arms)",
                    g_scanMisses);
        }
        return false;
    }
    g_scanMisses = 0;
    g_hands = static_cast<uint8_t*>(found);
    g_skel = *reinterpret_cast<uint8_t**>(g_hands + patterns::kAHandsSkelInstOffset);
    uint8_t* arr = g_skel + patterns::kSkelPoseArrayOffset;
    g_pose = *reinterpret_cast<uint8_t**>(arr);
    g_boneCount = *reinterpret_cast<int32_t*>(arr + 4);
    if (!g_pose || g_boneCount <= 0 || g_boneCount > kMaxBones ||
        !is_memory_valid(g_pose, g_boneCount * patterns::kSkelPoseStride)) {
        drop("pose bank implausible");
        return false;
    }
    g_refValid = false;
    BVR_LOG("[b2r] bones: rig resolved - AHands %p skel %p pose %p x%d bones", g_hands,
            g_skel, g_pose, g_boneCount);
    // Session 74: the left-eye flicker fix rides the rig. The engine's
    // dirty-flagged skeleton update re-evaluates the hands INSIDE pass 1 (never
    // pass 2), after every repaint site and before the mesh is drawn; hooking
    // it here means the driven pose is repainted the moment it returns. The
    // hook is a static vtable slot, so installing once is enough; the
    // this-filter in the detour follows g_skel/g_wSkel across re-resolves.
    // POSTED, not installed: this runs inside the CalcView dispatch of a
    // hooked Draw, and hooks install only from the poll lane (apply_pending_wfix).
    g_wfixPending.store(true, std::memory_order_relaxed);
    return true;
}

bool capture_reference() {
    if (!g_pose) return false;
    for (int i = 0; i < g_boneCount; ++i)
        memcpy(g_ref[i], g_pose + i * patterns::kSkelPoseStride, 48);
    // The adopted-pose bank starts as the reference (scale rows included -
    // they are pinned to g_ref and never adopted afterwards).
    memcpy(g_anim, g_ref, sizeof g_anim);
    g_refValid = true;
    BVR_LOG("[b2r] bones: reference pose captured (%d bones)", g_boneCount);
    return true;
}

// --- engine-pose adoption (session 41) --------------------------------------
// For every bone this hand is about to drive, decide whether the bank holds a
// FRESH engine evaluation (adopt its trans+quat into g_anim) or still holds
// our own last write (keep g_anim). Rules, in order:
// - a bone the OTHER hand has masked is skipped outright (the bank holds that
//   hand's composed write - adopting it would compose-on-composed);
// - a bone NOT in this hand's mask is adopted UNCONDITIONALLY (it is entering
//   the driven set - fresh drive after release, arms-mode change, cluster
//   reconfig - and g_written[i] is stale garbage there, so the compare would
//   be meaningless; the bank is engine truth for undriven bones);
// - otherwise adopt only when the first 32 bytes differ from our last write.
// NEVER the scale bytes (see g_anim above). Runs even with anim OFF so the
// toggle switches to an up-to-date bank glitch-free.
void adopt_one(int hand, int i) {
    if (i < 0 || i >= g_boneCount) return;
    if (g_writtenMask[1 - hand][i]) return;
    const uint8_t* bank = g_pose + i * patterns::kSkelPoseStride;
    if (g_writtenMask[hand][i]) {
        if (memcmp(bank, g_written[i], 32) == 0) return; // still our write
        // Quat-norm sanity: a torn read on the dev-only threaded substrate
        // must not enter the compose (structural 1t makes this a no-op in
        // the shipping config).
        const float* q = reinterpret_cast<const float*>(bank + 16);
        float n2 = q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3];
        if (!(n2 > 0.5f && n2 < 2.0f)) return;
        g_adopts[hand].fetch_add(1, std::memory_order_relaxed);
    }
    memcpy(g_anim[i], bank, 32);
}

void adopt_hand(int hand) {
    if (!g_pose || !g_refValid) return;
    const Cluster& cl = g_cluster[hand];
    int lo = cl.lo < 0 ? 0 : cl.lo;
    int hi = cl.hi >= g_boneCount ? g_boneCount - 1 : cl.hi;
    for (int i = lo; i <= hi; ++i) adopt_one(hand, i);
    if (cl.extra >= 0 && cl.extra < g_boneCount) adopt_one(hand, cl.extra);
    int armsMode = g_armsMode.load(std::memory_order_relaxed);
    if (armsMode == 1 || armsMode == 2) {
        const int* armIdx = hand ? patterns::kBoneArmR : patterns::kBoneArmL;
        for (int a = 0; a < patterns::kBoneArmCount; ++a)
            adopt_one(hand, armIdx[a]);
    }
}

// Compose one bone from the cached frame inputs and stamp it (g_written +
// bank + mask + kind). kind: 0 = full, 1 = keep authored scale (the
// weapon-attach escape hatch), 2 = hidden - collapsed ONTO the driven wrist
// (session 41: zero scale at the AUTHORED spot stretched a skin web from the
// wrist across the arm; degenerating the blend to the wrist point kills it).
// Scale ALWAYS comes from g_ref (never g_anim) - see the g_anim comment.
void compose_bone(int hand, int i, int kind) {
    const ComposeCache& cc = g_compose[hand];
    const float(*src)[12] = cc.animMode ? g_anim : g_ref;
    const float* r = src[i];
    float* w = g_written[i];
    if (kind == 2) {
        memcpy(w, g_ref[i], 48);
        w[0] = cc.ptc[0];
        w[1] = cc.ptc[1];
        w[2] = cc.ptc[2];
        w[8] = 0.0f;
        w[9] = 0.0f;
        w[10] = 0.0f;
    } else {
        const float* A = src[cc.anchor];
        const float* refA = g_ref[cc.anchor];
        // The attach pivot rides its own base (ptc + weapon offset) so the
        // WEAPON can be moved relative to the hand; everything else is glued
        // to ptc.
        const float* base = (i == cc.extra) ? cc.ptcExtra : cc.ptc;
        // Anchor-relative offset, scaled about the anchor; animT re-adds the
        // wrist's own authored travel (0 = glued to the controller, the
        // default - the write-loc ground truth stays exact).
        float dp[3];
        for (int k = 0; k < 3; ++k)
            dp[k] = (r[k] - A[k]) * cc.scale + cc.animT * (A[k] - refA[k]) * cc.scale;
        float rp[3];
        bvr::xrmath::quat_rotate(cc.qtc[0], cc.qtc[1], cc.qtc[2], cc.qtc[3], dp, rp);
        w[0] = base[0] + rp[0];
        w[1] = base[1] + rp[1];
        w[2] = base[2] + rp[2];
        w[3] = r[3];
        bvr::xrmath::quat_mul(cc.qtc, &r[4], &w[4]);
        bool scaleCh = kind == 0;
        w[8] = scaleCh ? g_ref[i][8] * cc.scale : g_ref[i][8];
        w[9] = scaleCh ? g_ref[i][9] * cc.scale : g_ref[i][9];
        w[10] = scaleCh ? g_ref[i][10] * cc.scale : g_ref[i][10];
        w[11] = g_ref[i][11];
    }
    memcpy(g_pose + i * patterns::kSkelPoseStride, w, 48);
    g_writtenMask[hand][i] = true;
    g_writeKind[hand][i] = static_cast<uint8_t>(kind);
}

// --- the weapon skeleton lane (session 41) ----------------------------------

// Is the cached weapon skeleton still the live, owned, readable one?
bool wskel_intact() {
    if (!g_wHold || !g_wSkel || !g_wPose || g_wBoneCount <= 0) return false;
    if (!is_memory_valid(g_wSkel, 0x60)) return false;
    if (*reinterpret_cast<const uint8_t* const*>(g_wSkel) !=
        g_imageBase + patterns::kSkeletonInstanceVtableRva)
        return false;
    if (*reinterpret_cast<uint8_t**>(g_wSkel + patterns::kSkelOwnerOffset) != g_wHold)
        return false;
    uint8_t* arr = g_wSkel + patterns::kSkelPoseArrayOffset;
    if (*reinterpret_cast<uint8_t**>(arr) != g_wPose ||
        *reinterpret_cast<int32_t*>(arr + 4) != g_wBoneCount)
        return false;
    return is_memory_valid(g_wPose, g_wBoneCount * patterns::kSkelPoseStride);
}

// Hand the weapon skeleton back to the engine: restore authored transforms
// over every bone we wrote (scale never restamps on its own - BS1's
// stranded-collapse class), then forget it. Restore only through an INTACT
// identity - a freed bank must never be written.
void wskel_drop(const char* why) {
    bool any = false;
    if (wskel_intact()) {
        for (int i = 0; i < g_wBoneCount; ++i)
            if (g_wWrittenValid[i]) {
                memcpy(g_wPose + i * patterns::kSkelPoseStride, g_wRef[i], 48);
                any = true;
            }
    }
    if (g_wHold || any)
        BVR_LOG("[b2r] bones: weapon skel dropped (%s)%s", why,
                any ? " - authored pose restored" : "");
    g_wHold = nullptr;
    g_wSkel = nullptr;
    g_wPose = nullptr;
    g_wBoneCount = 0;
    memset(g_wWrittenValid, 0, sizeof g_wWrittenValid);
    g_wStampMs = 0;
}

// Resolve (or revalidate) the CURRENT holdable's own skeleton. On a holdable
// change the old skeleton is restored first, then the new one is captured.
bool wskel_resolve() {
    void* hold = nullptr;
    if (!current_holdable(&hold) || !hold) {
        if (g_wHold) wskel_drop("no holdable");
        return false;
    }
    if (hold == g_wHold) {
        if (wskel_intact()) return true;
        wskel_drop("revalidation failed");
        return false; // re-resolve next frame
    }
    if (g_wHold) wskel_drop("holdable changed");
    uint8_t* h = static_cast<uint8_t*>(hold);
    if (!is_memory_valid(h + patterns::kWeaponSkelInstOffset, sizeof(void*)))
        return false;
    uint8_t* skel = *reinterpret_cast<uint8_t**>(h + patterns::kWeaponSkelInstOffset);
    if (!skel || !is_memory_valid(skel, 0x60)) return false;
    if (*reinterpret_cast<const uint8_t* const*>(skel) !=
        g_imageBase + patterns::kSkeletonInstanceVtableRva)
        return false;
    if (*reinterpret_cast<uint8_t**>(skel + patterns::kSkelOwnerOffset) != h)
        return false;
    uint8_t* arr = skel + patterns::kSkelPoseArrayOffset;
    uint8_t* pose = *reinterpret_cast<uint8_t**>(arr);
    int32_t cnt = *reinterpret_cast<int32_t*>(arr + 4);
    if (!pose || cnt <= 0 || cnt > kMaxBones ||
        !is_memory_valid(pose, cnt * patterns::kSkelPoseStride))
        return false;
    g_wHold = h;
    g_wSkel = skel;
    g_wPose = pose;
    g_wBoneCount = cnt;
    for (int i = 0; i < cnt; ++i)
        memcpy(g_wRef[i], pose + i * patterns::kSkelPoseStride, 48);
    memcpy(g_wAnim, g_wRef, sizeof g_wAnim);
    memset(g_wWrittenValid, 0, sizeof g_wWrittenValid);
    BVR_LOG("[b2r] bones: weapon skel resolved - holdable %p skel %p pose %p x%d bones",
            h, skel, pose, cnt);
    g_wRescans.fetch_add(1, std::memory_order_relaxed); // flicker correlate
    return true;
}

// Compose + stamp every weapon bone from the adopted bank at scale ws.
void wskel_compose(float ws) {
    for (int i = 0; i < g_wBoneCount; ++i) {
        uint8_t* bank = g_wPose + i * patterns::kSkelPoseStride;
        if (!g_wWrittenValid[i]) {
            memcpy(g_wAnim[i], bank, 32); // entering: bank is engine truth
        } else if (memcmp(bank, g_wWritten[i], 32) != 0) {
            const float* q = reinterpret_cast<const float*>(bank + 16);
            float n2 = q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3];
            if (n2 > 0.5f && n2 < 2.0f) {
                memcpy(g_wAnim[i], bank, 32);
                g_wAdopts.fetch_add(1, std::memory_order_relaxed);
            }
        }
        float* w = g_wWritten[i];
        w[0] = g_wAnim[i][0] * ws;
        w[1] = g_wAnim[i][1] * ws;
        w[2] = g_wAnim[i][2] * ws;
        w[3] = g_wAnim[i][3];
        memcpy(&w[4], &g_wAnim[i][4], 16);
        w[8] = g_wRef[i][8] * ws;
        w[9] = g_wRef[i][9] * ws;
        w[10] = g_wRef[i][10] * ws;
        w[11] = g_wRef[i][11];
        memcpy(bank, w, 48);
        g_wWrittenValid[i] = true;
    }
}

// --- bone-name map (session 40) ---------------------------------------------
// SharedSkeletonData (skel+0x08) carries an FName->boneIndex hash map at an
// offset auto-detected live; BS1's map LAYOUT shape transfers (pairs at map
// +0x00, buckets int32* at +0xC, power-of-two count at +0x10, 16-byte pairs
// {next, fnameIdx, fnameNum, boneIndex}), its offset does not (patterns.h).
// Diagnostic only - the drive never reads names (BS1 rule: clusters are baked
// index ranges; the map exists to DERIVE them). State declared at the top.

// SEH-guarded read: the map candidates walk unproven heap pointers.
bool read_n(const void* src, void* dst, size_t n) {
    if (!is_memory_valid(src, n)) return false;
    __try {
        memcpy(dst, src, n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Walk one candidate map offset. Returns bones named (>=0) or -1 on a
// structural violation; fills g_boneNames only when commit.
int walk_name_map(const uint8_t* shared, uint32_t off, bool commit) {
    const uint8_t* pairs = nullptr;
    const uint8_t* buckets = nullptr;
    int32_t bucketCount = 0;
    if (!read_n(shared + off, &pairs, 4) || !read_n(shared + off + 0xC, &buckets, 4) ||
        !read_n(shared + off + 0x10, &bucketCount, 4))
        return -1;
    if (!pairs || !buckets) return -1;
    if (bucketCount < 4 || bucketCount > 65536 ||
        (bucketCount & (bucketCount - 1)) != 0)
        return -1; // power-of-two sanity (BS1's shape)
    bool seen[kMaxBones] = {};
    int named = 0;
    for (int b = 0; b < bucketCount; ++b) {
        int32_t idx = -1;
        if (!read_n(buckets + b * 4, &idx, 4)) return -1;
        // Chain walk bounded by 4x the rig size so a corrupt link cannot spin.
        for (int guard = 0; idx >= 0 && guard <= g_boneCount * 4; ++guard) {
            if (idx > 4096) return -1;
            struct {
                int32_t next, nameIdx, nameNum, value;
            } pair{};
            if (!read_n(pairs + static_cast<size_t>(idx) * 16, &pair, sizeof pair))
                return -1;
            if (pair.value < 0 || pair.value >= g_boneCount) return -1;
            char text[48];
            if (!patterns::fname_text(static_cast<uint32_t>(pair.nameIdx), text,
                                      sizeof text))
                return -1;
            if (!seen[pair.value]) {
                seen[pair.value] = true;
                ++named;
                if (commit) {
                    strncpy_s(g_boneNames[pair.value], text, _TRUNCATE);
                    if (pair.nameNum != 0) {
                        // FName number suffix (Name_2 style) - append it.
                        size_t len = strlen(g_boneNames[pair.value]);
                        _snprintf_s(g_boneNames[pair.value] + len,
                                    sizeof g_boneNames[0] - len, _TRUNCATE, "_%d",
                                    pair.nameNum - 1);
                    }
                }
            }
            idx = pair.next;
        }
    }
    return named;
}

bool resolve_bone_names() {
    if (g_namesValid) return true;
    if (!g_skel || g_boneCount <= 0) return false;
    const uint8_t* shared = nullptr;
    if (!read_n(g_skel + patterns::kSkelSharedDataOffset, &shared, 4) || !shared) {
        BVR_LOG("[b2r] bones: SharedSkeletonData unreadable");
        return false;
    }
    if (g_nameMapOffset < 0) {
        int winners = 0;
        uint32_t winOff = 0;
        int winNamed = 0;
        for (uint32_t off = 0; off <= 0x140; off += 4) {
            int named = walk_name_map(shared, off, false);
            if (named >= g_boneCount / 2) {
                ++winners;
                winOff = off;
                winNamed = named;
                BVR_LOG("[b2r] bones: name-map candidate shared+0x%X names %d/%d",
                        off, named, g_boneCount);
            }
        }
        if (winners != 1) {
            BVR_LOG("[b2r] bones: name-map auto-detect FAILED (%d candidates) - "
                    "use `vrbones map` and derive by hand",
                    winners);
            return false;
        }
        g_nameMapOffset = static_cast<int>(winOff);
        BVR_LOG("[b2r] bones: name map at shared+0x%X (%d/%d bones named) - bank "
                "this offset",
                winOff, winNamed, g_boneCount);
    }
    memset(g_boneNames, 0, sizeof g_boneNames);
    if (walk_name_map(shared, static_cast<uint32_t>(g_nameMapOffset), true) < 0)
        return false;
    g_namesValid = true;
    return true;
}

// The axes instrument (session 40): the flat mesh-orientation read that
// aimRayMaxDevDeg never was. World-space basis of a bone's CURRENT and
// REFERENCE rotation vs the right hand ray, plus every raw quat the offline
// bake derivation needs.
void log_axes(int idx) {
    if (!g_pose || idx < 0 || idx >= g_boneCount) {
        BVR_LOG("[b2r] vrbones axes: no rig / bad index %d", idx);
        return;
    }
    const int32_t* aRot =
        reinterpret_cast<const int32_t*>(g_hands + patterns::kAHandsActorRotOffset);
    FRotator actorRot{aRot[0], aRot[1], aRot[2]};
    float qa[4];
    ue_rot_to_quat(actorRot, qa);
    float qc[4], qr[4];
    memcpy(qc, g_pose + idx * patterns::kSkelPoseStride + patterns::kSkelPoseQuatOffset,
           16);
    memcpy(qr, &g_ref[idx][4], 16);
    float qwc[4], qwr[4];
    bvr::xrmath::quat_mul(qa, qc, qwc);
    bvr::xrmath::quat_mul(qa, qr, qwr);

    FVector rayO{};
    FRotator rayR{};
    bool haveRay = aim::last_ray(1, &rayO, &rayR);
    float rayDir[3] = {};
    if (haveRay) ue_rot_to_dir(rayR, rayDir);

    const char* name = (g_namesValid && g_boneNames[idx][0]) ? g_boneNames[idx] : "?";
    BVR_LOG("[b2r] vrbones axes: bone %d '%s' actorRot (%d %d %d) qa (%.4f %.4f "
            "%.4f %.4f)",
            idx, name, actorRot.pitch, actorRot.yaw, actorRot.roll, qa[0], qa[1],
            qa[2], qa[3]);
    // Session 41: 'cur' above samples the BANK, which mid-frame races between
    // the engine's restamp and our recompose (a boot-A reading flipped 79 deg
    // between the two families at one pose). These two are race-free: what WE
    // wrote last, and the engine pose the drive adopted.
    if (g_writtenMask[0][idx] || g_writtenMask[1][idx])
        BVR_LOG("[b2r]   written q (%.4f %.4f %.4f %.4f) anim q (%.4f %.4f %.4f "
                "%.4f)",
                g_written[idx][4], g_written[idx][5], g_written[idx][6],
                g_written[idx][7], g_anim[idx][4], g_anim[idx][5], g_anim[idx][6],
                g_anim[idx][7]);
    BVR_LOG("[b2r]   cur q comp (%.4f %.4f %.4f %.4f) world (%.4f %.4f %.4f %.4f)",
            qc[0], qc[1], qc[2], qc[3], qwc[0], qwc[1], qwc[2], qwc[3]);
    BVR_LOG("[b2r]   ref q comp (%.4f %.4f %.4f %.4f) world (%.4f %.4f %.4f %.4f)",
            qr[0], qr[1], qr[2], qr[3], qwr[0], qwr[1], qwr[2], qwr[3]);
    const float axes[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    const char* axName[3] = {"X", "Y", "Z"};
    for (int pass = 0; pass < 2; ++pass) {
        const float* q = pass == 0 ? qwc : qwr;
        for (int a = 0; a < 3; ++a) {
            float d[3];
            bvr::xrmath::quat_rotate(q[0], q[1], q[2], q[3], axes[a], d);
            if (haveRay) {
                float dot = d[0] * rayDir[0] + d[1] * rayDir[1] + d[2] * rayDir[2];
                if (dot > 1.0f) dot = 1.0f;
                if (dot < -1.0f) dot = -1.0f;
                BVR_LOG("[b2r]   %s %s = (%.3f %.3f %.3f)  angle to ray %.1f deg",
                        pass == 0 ? "cur" : "ref", axName[a], d[0], d[1], d[2],
                        acosf(dot) * 57.29578f);
            } else {
                BVR_LOG("[b2r]   %s %s = (%.3f %.3f %.3f)  (ray invalid)",
                        pass == 0 ? "cur" : "ref", axName[a], d[0], d[1], d[2]);
            }
        }
    }
    if (haveRay)
        BVR_LOG("[b2r]   ray rot (%d %d %d) dir (%.3f %.3f %.3f)", rayR.pitch,
                rayR.yaw, rayR.roll, rayDir[0], rayDir[1], rayDir[2]);
}

// UE rotator -> quat via the shared helper; conjugate/multiply/rotate via
// core's xr_math (plain quaternion algebra - space-agnostic).
void rot_to_quat(const FRotator& r, float q[4]) { ue_rot_to_quat(r, q); }

} // namespace

bool drive(const FrameContext& ctx, const GamePose& target, int hand) {
    if (hand < 0 || hand > 1) return false;
    if (!resolve_rig()) return false;
    if (!g_refValid && !capture_reference()) return false;
    const Cluster& cl = g_cluster[hand];
    int lo = cl.lo, hi = cl.hi, extra = cl.extra, anchor = cl.anchor;
    if (lo < 0) lo = 0;
    if (hi >= g_boneCount) hi = g_boneCount - 1;
    if (extra >= g_boneCount) extra = -1;
    if (anchor < 0 || anchor >= g_boneCount) anchor = lo;

    // Actor transform (the bones are component-space, relative to the actor).
    if (!is_memory_valid(g_hands + patterns::kAHandsActorLocOffset, 0x18)) {
        drop("actor loc unreadable");
        return false;
    }
    const float* aLoc =
        reinterpret_cast<const float*>(g_hands + patterns::kAHandsActorLocOffset);
    const int32_t* aRot =
        reinterpret_cast<const int32_t*>(g_hands + patterns::kAHandsActorRotOffset);
    FRotator actorRot{aRot[0], aRot[1], aRot[2]};
    float qa[4], qaInv[4], qt[4];
    rot_to_quat(actorRot, qa);
    bvr::xrmath::quat_conj(qa, qaInv);
    rot_to_quat(target.rot, qt);

    // Component-space target: qtc = qaInv * qt; ptc = qaInv * (T - A).
    float qtc[4];
    bvr::xrmath::quat_mul(qaInv, qt, qtc);
    float rel[3] = {target.loc.x - aLoc[0], target.loc.y - aLoc[1],
                    target.loc.z - aLoc[2]};
    float ptc[3];
    bvr::xrmath::quat_rotate(qaInv[0], qaInv[1], qaInv[2], qaInv[3], rel, ptc);

    // Sanity refusal (BS1's >500 UU guard): a target that far from the actor
    // means the context and the rig disagree - never smear bones across the
    // map.
    float d2 = rel[0] * rel[0] + rel[1] * rel[1] + rel[2] * rel[2];
    if (d2 > 500.0f * 500.0f) return false;

    // THE COMPOSITION (session 40 - this is the ~90 deg fix).
    //
    // delta = qtc, NOT qtc * conj(refQ_anchor). The old form made the anchor's
    // rotation become the controller's outright, discarding the mesh's authored
    // frame - on this rig the anchor's authored rotation is ~81.6 deg off the
    // view frame, which IS the constant offset the first look reported. Keeping
    // the authored rotations and rotating them by the controller's rotation
    // RELATIVE TO THE ACTOR (which carries the view rotation) means a controller
    // aiming where the view aims reproduces the engine's own pose exactly, and
    // any controller rotation away from it turns the cluster by that much.
    // Equivalent to a per-cluster bake of exactly refQ_anchor, but self-deriving:
    // nothing to re-bank when a weapon, animation or rig changes.
    // (hkQsTransform: translation at [0..3], quat at [4..7], scale [8..11].)
    //
    // Session 41: with anim mode ON the source is g_anim - the engine's own
    // freshly-evaluated pose, adopted below - so q_i = qtc * animQ_i and the
    // engine's animation plays inside the controller-driven frame. Since
    // adoption filters our own writes out, this is algebraically the brief's
    // "compose the animation DELTA on top of the controller frame"
    // (qtc * (animQ * conj(refQ)) * refQ == qtc * animQ), without ever
    // forming the delta explicitly.
    float scale = g_scale[hand].load(std::memory_order_relaxed);
    if (!(scale > 0.05f && scale < 20.0f)) scale = 1.0f;
    bool animMode = g_animMode.load(std::memory_order_relaxed);
    float animT = animMode ? g_animTrans.load(std::memory_order_relaxed) : 0.0f;
    if (!(animT >= 0.0f && animT <= 1.0f)) animT = 0.0f;

    // Absorb the engine's evaluation BEFORE this frame's writes: any driven
    // bone whose bank bytes are no longer ours was restamped by animation and
    // becomes this frame's pose source. Runs before the arms-transition
    // restore below (that restore writes ref to the bank without updating
    // g_written, and must not be mistaken for an engine restamp).
    // Session 42: the snapshot-around counts drive calls where adoption fired
    // at all - the restamp-cadence baseline for the flicker readout.
    uint32_t adoptsBefore = g_adopts[hand].load(std::memory_order_relaxed);
    adopt_hand(hand);
    if (g_adopts[hand].load(std::memory_order_relaxed) != adoptsBefore)
        g_driveAdoptEvents[0].fetch_add(1, std::memory_order_relaxed);

    // Arms-mode transition (game thread, here on purpose): leaving follow or
    // hide must hand the arm bones back to the engine explicitly - animation
    // never restamps the SCALE channel, so a stale zero-scale would strand
    // hidden arms invisible forever (BS1 session-29's stranded-collapse bug).
    int armsMode = g_armsMode.load(std::memory_order_relaxed);
    const int* armIdx = hand ? patterns::kBoneArmR : patterns::kBoneArmL;
    if (armsMode != g_armsApplied[hand]) {
        if (g_armsApplied[hand] == 1 || g_armsApplied[hand] == 2)
            for (int a = 0; a < patterns::kBoneArmCount; ++a)
                if (armIdx[a] < g_boneCount)
                    memcpy(g_pose + armIdx[a] * patterns::kSkelPoseStride,
                           g_ref[armIdx[a]], 48);
        g_armsApplied[hand] = armsMode;
    }

    ComposeCache& cc = g_compose[hand];
    memcpy(cc.qtc, qtc, sizeof cc.qtc);
    memcpy(cc.ptc, ptc, sizeof cc.ptc);
    // The weapon offset (right hand only): the hands policy converted the cm
    // sliders into a game-space vector this frame; rotate it into component
    // space and give the attach pivot its own base.
    memcpy(cc.ptcExtra, ptc, sizeof cc.ptcExtra);
    if (hand == 1) {
        float ro[3];
        bvr::xrmath::quat_rotate(qaInv[0], qaInv[1], qaInv[2], qaInv[3], g_wOffGame,
                                 ro);
        cc.ptcExtra[0] += ro[0];
        cc.ptcExtra[1] += ro[1];
        cc.ptcExtra[2] += ro[2];
    }
    cc.scale = scale;
    cc.animT = animT;
    cc.anchor = anchor;
    cc.extra = extra;
    cc.animMode = animMode;
    cc.valid = true;

    memset(g_writtenMask[hand], 0, sizeof g_writtenMask[hand]);
    for (int i = lo; i <= hi; ++i) compose_bone(hand, i, 0);
    if (extra >= 0)
        compose_bone(hand, extra,
                     g_scaleAttach.load(std::memory_order_relaxed) ? 0 : 1);
    if (armsMode == 1) {
        for (int a = 0; a < patterns::kBoneArmCount; ++a)
            if (armIdx[a] < g_boneCount) compose_bone(hand, armIdx[a], 0);
    } else if (armsMode == 2) {
        for (int a = 0; a < patterns::kBoneArmCount; ++a)
            if (armIdx[a] < g_boneCount) compose_bone(hand, armIdx[a], 2);
    }

    g_writeStampMs[hand] = GetTickCount64();
    g_lastWriteLoc[hand][0] = target.loc.x;
    g_lastWriteLoc[hand][1] = target.loc.y;
    g_lastWriteLoc[hand][2] = target.loc.z;
    ++g_driveCount[hand];
    return true;
}

void reapply() {
    if (!g_pose) return;
    uint64_t now = GetTickCount64();
    for (int h = 0; h < 2; ++h) {
        if (now - g_writeStampMs[h] > 100) continue; // stale write, leave it
        for (int i = 0; i < g_boneCount; ++i)
            if (g_writtenMask[h][i])
                memcpy(g_pose + i * patterns::kSkelPoseStride, g_written[i], 48);
    }
    // Weapon skeleton (session 41): the same pass-2 discipline - verbatim
    // restamp, both eyes must see the same frame.
    if (g_wPose && g_wStampMs && now - g_wStampMs <= 100 && wskel_intact())
        for (int i = 0; i < g_wBoneCount; ++i)
            if (g_wWrittenValid[i])
                memcpy(g_wPose + i * patterns::kSkelPoseStride, g_wWritten[i], 48);
}

void release(const char* why, int hand) {
    // Per-cluster restore: a left-hand release must not disturb a live right
    // hand, so only the released hand's own written bones (cluster + pivot +
    // arms, whatever the mask holds) go back to reference.
    //
    // Session 42, the missing s29 interlock leg: "stop driving" is always
    // safe, "hand state back" WRITES - and a save load can free and reuse the
    // skeleton pages while g_pose still points at them (SEH is useless: the
    // pages stay mapped, owned by the new level). Same predicate pe_repaint
    // already trusts. On refusal the masks still clear below - stopping is
    // the part that must always happen.
    bool bankLive = g_pose && g_refValid &&
                    is_memory_valid(g_pose, g_boneCount * patterns::kSkelPoseStride);
    if (g_pose && g_refValid && !bankLive)
        BVR_LOG("[b2r] bones: release(%s) REFUSED the restore - pose bank not "
                "live (world changed under us); masks cleared, no writes", why);
    bool any = false;
    for (int h = 0; h < 2; ++h) {
        if (hand >= 0 && h != hand) continue;
        if (bankLive) {
            for (int i = 0; i < g_boneCount; ++i)
                if (g_writtenMask[h][i]) {
                    memcpy(g_pose + i * patterns::kSkelPoseStride, g_ref[i], 48);
                    any = true;
                }
        }
        memset(g_writtenMask[h], 0, sizeof g_writtenMask[h]);
        g_armsApplied[h] = -1;
        g_compose[h].valid = false;
    }
    if (any)
        BVR_LOG("[b2r] bones: released %s (%s) - reference restored",
                hand < 0 ? "both clusters" : (hand ? "right" : "left"), why);
    // Only a full release drops the reference: recapturing it while the other
    // cluster is still driven would capture OUR pose as the new authored one.
    if (hand < 0) {
        g_refValid = false;
        // A full hand-back also returns the weapon's own skeleton.
        if (g_wHold) wskel_drop(why);
    }
}

// First masked bone of hand h whose 48-byte pose differs from our last write,
// or -1. scanAll=false reproduces the s41 sentinel (first masked bone only);
// true scans the whole mask (session 74: the sentinel missed restamps that
// leave the first bone alone and move the elbow/wrist/gun bones).
int first_foreign_bone(int h, bool scanAll) {
    for (int i = 0; i < g_boneCount; ++i) {
        if (!g_writtenMask[h][i]) continue;
        if (memcmp(g_pose + i * patterns::kSkelPoseStride, g_written[i], 48) != 0)
            return i;
        if (!scanAll) return -1;
    }
    return -1;
}
int first_foreign_wbone(bool scanAll) {
    for (int i = 0; i < g_wBoneCount; ++i) {
        if (!g_wWrittenValid[i]) continue;
        if (memcmp(g_wPose + i * patterns::kSkelPoseStride, g_wWritten[i], 48) != 0)
            return i;
        if (!scanAll) return -1;
    }
    return -1;
}

void pe_repaint(int site) {
    if (!g_pose || !g_refValid) return;
    uint64_t now = GetTickCount64();
    // Session 42: catch phase for the flicker instrumentation (site 0 = the
    // PE lane, 1 = the flush point; odd = pass 2).
    int phase = site * 2 + (scenedraw::in_second_draw() ? 1 : 0);
    const bool scanAll = g_fullMask.load(std::memory_order_relaxed);
    for (int h = 0; h < 2; ++h) {
        if (now - g_writeStampMs[h] > 100) continue; // not driving, nothing to defend
        // Sentinel (s41): the first masked bone; fullmask (s74): any masked bone.
        // Compare FIRST: this runs on every ProcessEvent dispatch, and the
        // VirtualQuery below is only worth paying once a restamp is in hand.
        if (first_foreign_bone(h, scanAll) < 0) continue;
        if (!is_memory_valid(g_pose, g_boneCount * patterns::kSkelPoseStride)) return;
        // Session 41, absorb-then-recompose: the restamp is INPUT, not an
        // enemy - adopt it into g_anim first, then rewrite this frame
        // composed on the fresh pose. Pass 1 only: the second pass must
        // render the identical frame pass 1 did, so it gets the verbatim
        // restamp (as does a repaint arriving before any compose is cached).
        adopt_hand(h);
        if (g_compose[h].valid && !scenedraw::in_second_draw()) {
            for (int i = 0; i < g_boneCount; ++i)
                if (g_writtenMask[h][i]) compose_bone(h, i, g_writeKind[h][i]);
        } else {
            for (int i = 0; i < g_boneCount; ++i)
                if (g_writtenMask[h][i])
                    memcpy(g_pose + i * patterns::kSkelPoseStride, g_written[i], 48);
        }
        g_peRepaints.fetch_add(1, std::memory_order_relaxed);
        note_catch(phase, 0, static_cast<uint32_t>(now - g_writeStampMs[h]));
    }
    // Weapon skeleton (session 41): same sentinel, same absorb-then-recompose
    // on pass 1 / verbatim on pass 2.
    if (g_wPose && g_wStampMs && now - g_wStampMs <= 100 && wskel_intact()) {
        if (first_foreign_wbone(scanAll) >= 0) {
            if (!scenedraw::in_second_draw()) {
                float ws = g_wScale.load(std::memory_order_relaxed);
                if (!(ws > 0.05f && ws < 20.0f)) ws = 1.0f;
                wskel_compose(ws); // adopts the restamp, recomposes
            } else {
                for (int i = 0; i < g_wBoneCount; ++i)
                    if (g_wWrittenValid[i])
                        memcpy(g_wPose + i * patterns::kSkelPoseStride, g_wWritten[i],
                               48);
            }
            g_peRepaints.fetch_add(1, std::memory_order_relaxed);
            note_catch(phase, 1, static_cast<uint32_t>(now - g_wStampMs));
        }
    }
}

void probe_point(int p) {
    if (!g_raceProbe.load(std::memory_order_relaxed)) return;
    if (p < 0 || p > 5 || !g_pose || !g_refValid) return;
    g_probeCalls[p].fetch_add(1, std::memory_order_relaxed);
    uint64_t now = GetTickCount64();
    if (!is_memory_valid(g_pose, g_boneCount * patterns::kSkelPoseStride)) return;
    // The probe always scans the FULL mask - it exists to see what the
    // sentinel cannot.
    int firstDiff = -1;
    for (int h = 0; h < 2; ++h) {
        if (now - g_writeStampMs[h] > 100) continue;
        int b = first_foreign_bone(h, true);
        if (b >= 0 && (firstDiff < 0 || b < firstDiff)) firstDiff = b;
    }
    if (firstDiff >= 0) {
        g_probeForeign[p][0].fetch_add(1, std::memory_order_relaxed);
        g_probeFirstDiff[p].store(firstDiff, std::memory_order_relaxed);
    }
    if (g_wPose && g_wStampMs && now - g_wStampMs <= 100 && wskel_intact()) {
        if (first_foreign_wbone(true) >= 0)
            g_probeForeign[p][1].fetch_add(1, std::memory_order_relaxed);
    }
}

void set_full_mask(bool on) { g_fullMask.store(on, std::memory_order_relaxed); }
bool full_mask() { return g_fullMask.load(std::memory_order_relaxed); }
void set_race_probe(bool on) { g_raceProbe.store(on, std::memory_order_relaxed); }
bool race_probe() { return g_raceProbe.load(std::memory_order_relaxed); }

// --- Session 74: hardware write-watch on the sentinel bone -----------------
// Every probe says the bank is ours at every point we can look, yet pass 1's
// image carries the restamp. So: stop inferring the writer, CATCH it. Three
// debug registers cover the first masked weapon-hand bone (bytes 0-3, 16-19,
// 32-35 of its 48-byte pose), a vectored handler records the writer's EIP
// plus a short EBP return chain and WHERE in the pair it fired (tick / pass 1
// / pass 2 / flush), and our own module's writes are filtered out. Armed on
// the game thread only (`vrbones watch on`); the registers are cleared on
// `off`. Debug-only instrument - never ships armed.
namespace {
struct WatchHit {
    uint32_t eip = 0;      // RVA in the game exe (or raw address if foreign)
    uint32_t ret[3] = {};  // EBP-chain return RVAs
    uint8_t ctx = 0;       // 0 tick, 1 pass1, 2 pass2, +4 = inside flush
    uint32_t count = 0;
};
std::atomic<bool> g_watchOn{false};
std::atomic<uint32_t> g_watchOurs{0}, g_watchEngine{0}, g_watchForeign{0};
PVOID g_watchVeh = nullptr;
uint8_t* g_watchAddr = nullptr;
uint32_t g_watchTid = 0;
WatchHit g_watchHits[12];
std::atomic<int> g_watchHitN{0};
uintptr_t g_selfBase = 0, g_selfEnd = 0, g_gameBase = 0, g_gameEnd = 0;

void module_range(void* addrInModule, uintptr_t* base, uintptr_t* end) {
    HMODULE m = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       static_cast<LPCWSTR>(addrInModule), &m);
    if (!m) { *base = *end = 0; return; }
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(m);
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(reinterpret_cast<uint8_t*>(m) + dos->e_lfanew);
    *base = reinterpret_cast<uintptr_t>(m);
    *end = *base + nt->OptionalHeader.SizeOfImage;
}

LONG CALLBACK watch_veh(EXCEPTION_POINTERS* ep) {
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP)
        return EXCEPTION_CONTINUE_SEARCH;
    CONTEXT* c = ep->ContextRecord;
    if ((c->Dr6 & 0x7) == 0) return EXCEPTION_CONTINUE_SEARCH; // not our DR0-2
    c->Dr6 = 0;
    const uintptr_t eip = c->Eip;
    if (eip >= g_selfBase && eip < g_selfEnd) {
        g_watchOurs.fetch_add(1, std::memory_order_relaxed);
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    uint8_t ctx = 0;
    if (scenedraw::draw_depth() > 0) ctx = scenedraw::in_second_draw() ? 2 : 1;
    if (scenedraw::in_flush()) ctx |= 4;
    uint32_t rva = (eip >= g_gameBase && eip < g_gameEnd)
                       ? static_cast<uint32_t>(eip - g_gameBase) : static_cast<uint32_t>(eip);
    if (eip >= g_gameBase && eip < g_gameEnd)
        g_watchEngine.fetch_add(1, std::memory_order_relaxed);
    else
        g_watchForeign.fetch_add(1, std::memory_order_relaxed);
    // Dedupe on (eip, ctx); keep the first 12 distinct sites.
    int n = g_watchHitN.load(std::memory_order_relaxed);
    for (int i = 0; i < n; ++i) {
        if (g_watchHits[i].eip == rva && g_watchHits[i].ctx == ctx) {
            ++g_watchHits[i].count;
            return EXCEPTION_CONTINUE_EXECUTION;
        }
    }
    if (n < 12) {
        WatchHit& h = g_watchHits[n];
        h.eip = rva;
        h.ctx = ctx;
        h.count = 1;
        uintptr_t ebp = c->Ebp;
        for (int k = 0; k < 3; ++k) {
            if (!is_memory_valid(reinterpret_cast<void*>(ebp), 8)) break;
            uintptr_t ret = *reinterpret_cast<uintptr_t*>(ebp + 4);
            h.ret[k] = (ret >= g_gameBase && ret < g_gameEnd)
                           ? static_cast<uint32_t>(ret - g_gameBase) : static_cast<uint32_t>(ret);
            ebp = *reinterpret_cast<uintptr_t*>(ebp);
        }
        g_watchHitN.store(n + 1, std::memory_order_relaxed);
    }
    return EXCEPTION_CONTINUE_EXECUTION;
}

// Debug registers are per thread and must be set from OUTSIDE the target:
// a helper thread suspends the game thread, edits DR0-2/DR7, resumes.
void watch_apply(uint32_t tid, uint8_t* addr, bool arm) {
    HANDLE th = OpenThread(THREAD_ALL_ACCESS, FALSE, tid);
    if (!th) {
        BVR_LOG("[b2r] watch: OpenThread(%u) failed (%lu)", tid, GetLastError());
        return;
    }
    if (SuspendThread(th) == static_cast<DWORD>(-1)) {
        BVR_LOG("[b2r] watch: SuspendThread failed (%lu)", GetLastError());
        CloseHandle(th);
        return;
    }
    CONTEXT ctx{};
    ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
    if (GetThreadContext(th, &ctx)) {
        if (arm) {
            ctx.Dr0 = reinterpret_cast<uintptr_t>(addr);
            ctx.Dr1 = reinterpret_cast<uintptr_t>(addr + 16);
            ctx.Dr2 = reinterpret_cast<uintptr_t>(addr + 32);
            // L0-L2 enable; RW=01 (write), LEN=11 (4 bytes) for each.
            ctx.Dr7 = 0x1 | 0x4 | 0x10 | (0x1u << 16) | (0x3u << 18) | (0x1u << 20) |
                      (0x3u << 22) | (0x1u << 24) | (0x3u << 26);
        } else {
            ctx.Dr0 = ctx.Dr1 = ctx.Dr2 = 0;
            ctx.Dr7 = 0;
        }
        ctx.Dr6 = 0;
        if (!SetThreadContext(th, &ctx))
            BVR_LOG("[b2r] watch: SetThreadContext failed (%lu)", GetLastError());
    } else {
        BVR_LOG("[b2r] watch: GetThreadContext failed (%lu)", GetLastError());
    }
    ResumeThread(th);
    CloseHandle(th);
}
} // namespace

void watch_set(bool on) {
    if (on) {
        if (g_watchOn.load(std::memory_order_relaxed)) return;
        if (!g_pose || !g_refValid) {
            BVR_LOG("[b2r] watch: no live pose bank - resolve the rig first");
            return;
        }
        int s = -1;
        for (int i = 0; i < g_boneCount && s < 0; ++i)
            if (g_writtenMask[1][i]) s = i;
        if (s < 0)
            for (int i = 0; i < g_boneCount && s < 0; ++i)
                if (g_writtenMask[0][i]) s = i;
        if (s < 0) {
            BVR_LOG("[b2r] watch: no driven bone yet (vrhands on and wait a tick)");
            return;
        }
        uint32_t tid = scenedraw::draw_tid();
        if (!tid) {
            BVR_LOG("[b2r] watch: no draw thread seen yet");
            return;
        }
        module_range(reinterpret_cast<void*>(&watch_set), &g_selfBase, &g_selfEnd);
        module_range(GetModuleHandleW(nullptr), &g_gameBase, &g_gameEnd);
        g_watchAddr = g_pose + s * patterns::kSkelPoseStride;
        g_watchTid = tid;
        g_watchHitN.store(0, std::memory_order_relaxed);
        g_watchOurs.store(0); g_watchEngine.store(0); g_watchForeign.store(0);
        if (!g_watchVeh) g_watchVeh = AddVectoredExceptionHandler(1, watch_veh);
        uint32_t t = tid;
        uint8_t* a = g_watchAddr;
        HANDLE h = CreateThread(nullptr, 0,
                                [](LPVOID p) -> DWORD {
                                    auto* args = static_cast<uintptr_t*>(p);
                                    watch_apply(static_cast<uint32_t>(args[0]),
                                                reinterpret_cast<uint8_t*>(args[1]), true);
                                    delete[] args;
                                    return 0;
                                },
                                new uintptr_t[2]{t, reinterpret_cast<uintptr_t>(a)}, 0, nullptr);
        if (h) CloseHandle(h);
        g_watchOn.store(true, std::memory_order_relaxed);
        BVR_LOG("[b2r] watch ARMED on bone %d @ %p (thread %u) - engine writers will be "
                "logged by `vrbones watch status`", s, g_watchAddr, tid);
    } else {
        if (!g_watchOn.load(std::memory_order_relaxed)) return;
        uint32_t t = g_watchTid;
        HANDLE h = CreateThread(nullptr, 0,
                                [](LPVOID p) -> DWORD {
                                    watch_apply(static_cast<uint32_t>(reinterpret_cast<uintptr_t>(p)),
                                                nullptr, false);
                                    return 0;
                                },
                                reinterpret_cast<LPVOID>(static_cast<uintptr_t>(t)), 0, nullptr);
        if (h) { WaitForSingleObject(h, 2000); CloseHandle(h); }
        g_watchOn.store(false, std::memory_order_relaxed);
        BVR_LOG("[b2r] watch off");
    }
}

// --- Session 74: the post-writer repaint (the fix) ---------------------------
// The write-watch named ONE engine routine as the pose writer, reached from
// the game tick (handled by the PE-lane repaint) and, ~2x per shot, from a
// render-side path INSIDE pass 1 only - after every repaint site and before
// the hands mesh is drawn, so the LEFT eye renders the raw restamp. The fix is
// to repaint the driven pose the moment that writer RETURNS while pass 1 is
// in flight. The routine's signature is unknown, so the detour is naked and
// convention-agnostic: on entry it swaps the caller's return address for a
// stub and jumps to the original untouched; the stub runs the repaint with
// every register (and the x87 state) preserved and returns to the real
// caller. Game thread only (TEB tid check); nested calls pass through.
namespace {
void* g_wTarget = nullptr;
void* g_wOrig = nullptr;
uintptr_t g_wRetSaved = 0; // the hooked call's real return address (game thread)
uint32_t g_wTid = 0;
bool g_wCreated = false;
std::atomic<bool> g_wPostFix{true}; // the fix ships ON; `vrbones wfix off` is the A/B
std::atomic<uint32_t> g_wPost{0}, g_wPostPass1{0}, g_wPostPass2{0}, g_wPostTick{0};

void __cdecl w_post() {
    g_wPost.fetch_add(1, std::memory_order_relaxed);
    if (scenedraw::draw_depth() <= 0) {
        g_wPostTick.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (scenedraw::in_second_draw()) {
        g_wPostPass2.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    g_wPostPass1.fetch_add(1, std::memory_order_relaxed);
    if (g_wPostFix.load(std::memory_order_relaxed)) pe_repaint(3);
}

__declspec(naked) void w_ret_stub() {
    __asm {
        pushad
        pushfd
        sub esp, 108
        fnsave [esp]
        call w_post
        frstor [esp]
        add esp, 108
        popfd
        popad
        push dword ptr [g_wRetSaved]
        mov dword ptr [g_wRetSaved], 0
        ret
    }
}

__declspec(naked) void w_detour() {
    __asm {
        push eax
        ; only OUR two skeleton instances (ecx = this): every other skeletal
        ; mesh in the level passes straight through untouched
        cmp ecx, [g_skel]
        je ours
        cmp ecx, [g_wSkel]
        jne passthrough
      ours:
        mov eax, fs:[0x24]           ; current thread id (TEB)
        cmp eax, [g_wTid]
        jne passthrough
        cmp dword ptr [g_wRetSaved], 0
        jne passthrough              ; nested call - leave it alone
        mov eax, [esp + 4]           ; the caller's return address
        mov [g_wRetSaved], eax
        mov eax, offset w_ret_stub
        mov [esp + 4], eax
      passthrough:
        pop eax
        jmp dword ptr [g_wOrig]
    }
}
} // namespace

// The writer's call site (patterns::kSkelInstUpdateCallSiteRva, named by the
// watch's EBP chain) is `mov eax,[esi]; call [eax+0xA4]` guarded by
// `cmp byte [esi+0xA0],0` - a dirty-flagged virtual update on the skeleton
// instance (patterns::kSkelInstUpdateSlot of the SkeletonInstance vtable the
// rig scan already identifies), which is also why pass 2 never restamps: the
// flag is clear by then.

bool wfix_install() {
    if (g_wCreated) return true;
    if (!g_imageBase) {
        BVR_LOG("[b2r] wfix: image base unknown - resolve the rig first");
        return false;
    }
    if (!g_gameBase) module_range(GetModuleHandleW(nullptr), &g_gameBase, &g_gameEnd);
    const uint8_t* vt = g_imageBase + patterns::kSkeletonInstanceVtableRva;
    if (!is_memory_valid(vt + patterns::kSkelInstUpdateSlot, sizeof(void*))) {
        BVR_LOG("[b2r] wfix: SkeletonInstance vtable slot unreadable");
        return false;
    }
    uint8_t* target = *reinterpret_cast<uint8_t* const*>(vt + patterns::kSkelInstUpdateSlot);
    if (!is_memory_valid(target, 16)) {
        BVR_LOG("[b2r] wfix: resolved target %p is not readable", target);
        return false;
    }
    // The tid is refreshed at every Draw entry (wfix_on_draw_entry); this is
    // only the value until the next Draw.
    g_wTid = scenedraw::draw_tid();
    g_wTarget = target;
    MH_STATUS st = MH_CreateHook(g_wTarget, reinterpret_cast<void*>(&w_detour), &g_wOrig);
    // A hook that was created but failed to enable last time is still there:
    // retry the enable rather than failing forever on ALREADY_CREATED.
    if (st != MH_OK && st != MH_ERROR_ALREADY_CREATED) {
        BVR_LOG("[b2r] wfix: MH_CreateHook(0x%X) failed: %s",
                static_cast<uint32_t>(reinterpret_cast<uintptr_t>(target) - g_gameBase),
                MH_StatusToString(st));
        return false;
    }
    st = MH_EnableHook(g_wTarget);
    if (st != MH_OK && st != MH_ERROR_ENABLED) {
        BVR_LOG("[b2r] wfix: MH_EnableHook failed: %s", MH_StatusToString(st));
        return false;
    }
    g_wCreated = true;
    BVR_LOG("[b2r] wfix: writer HOOKED at RVA 0x%X (slot 0x%X, call site 0x%X; bytes %02X "
            "%02X %02X %02X %02X %02X %02X %02X) - `vrbones wfix off` is the A/B",
            static_cast<uint32_t>(reinterpret_cast<uintptr_t>(target) - g_gameBase),
            patterns::kSkelInstUpdateSlot, patterns::kSkelInstUpdateCallSiteRva, target[0],
            target[1], target[2], target[3], target[4], target[5], target[6], target[7]);
    return true;
}

void apply_pending_wfix() {
    if (!g_wfixPending.load(std::memory_order_relaxed)) return;
    if (g_wCreated || wfix_install()) g_wfixPending.store(false, std::memory_order_relaxed);
}

void wfix_on_draw_entry(uint32_t tid) {
    // Every Draw entry on the game thread: refresh the thread the detour
    // accepts (it was 0 if the rig resolved before the first hooked Draw) and
    // clear the saved-return slot - nothing of ours is on the stack here, so a
    // slot left set by an unwind that skipped the ret stub is stale.
    g_wTid = tid;
    g_wRetSaved = 0;
}

void wfix_set(bool on) {
    if (on && !g_wCreated) g_wfixPending.store(true, std::memory_order_relaxed); // poll lane installs
    g_wPostFix.store(on, std::memory_order_relaxed);
    BVR_LOG("[b2r] command: vrbones wfix %s (post-writer repaint in pass 1%s)", on ? "on" : "off",
            (on && !g_wCreated) ? "; hook install posted to the poll lane" : "");
}

bool wfix_enabled() { return g_wPostFix.load(std::memory_order_relaxed); }
bool wfix_hooked() { return g_wCreated; }

void wfix_status() {
    BVR_LOG("[b2r] wfix %s hooked=%d: writer returns seen %u (tick %u, pass1 %u, pass2 %u); "
            "post-writer catches hands/weapon = %u/%u",
            g_wPostFix.load() ? "ON" : "off", g_wCreated ? 1 : 0, g_wPost.load(),
            g_wPostTick.load(), g_wPostPass1.load(), g_wPostPass2.load(),
            g_catch[6][0].load(), g_catch[6][1].load());
}

void watch_status() {
    BVR_LOG("[b2r] watch %s: hits ours=%u engine=%u foreign=%u, %d distinct sites",
            g_watchOn.load() ? "ON" : "off", g_watchOurs.load(), g_watchEngine.load(),
            g_watchForeign.load(), g_watchHitN.load());
    int n = g_watchHitN.load(std::memory_order_relaxed);
    static const char* kCtx[8] = {"tick", "pass1", "pass2", "?", "tick+flush",
                                  "pass1+flush", "pass2+flush", "?"};
    for (int i = 0; i < n; ++i) {
        const WatchHit& h = g_watchHits[i];
        BVR_LOG("[b2r]   writer RVA 0x%X x%u in %s <- 0x%X <- 0x%X <- 0x%X", h.eip, h.count,
                kCtx[h.ctx & 7], h.ret[0], h.ret[1], h.ret[2]);
    }
}

void race_snapshot(RaceStats* out) {
    if (!out) return;
    for (int p = 0; p < 6; ++p) {
        out->calls[p] = g_probeCalls[p].load(std::memory_order_relaxed);
        for (int k = 0; k < 2; ++k)
            out->foreign[p][k] = g_probeForeign[p][k].load(std::memory_order_relaxed);
    }
    for (int k = 0; k < 2; ++k) {
        out->entryCatch[0][k] = g_catch[4][k].load(std::memory_order_relaxed);
        out->entryCatch[1][k] = g_catch[5][k].load(std::memory_order_relaxed);
    }
    for (int p = 0; p < 6; ++p)
        out->firstDiff[p] = g_probeFirstDiff[p].load(std::memory_order_relaxed);
    out->entryFix[0] = g_entryFix[0].load(std::memory_order_relaxed);
    out->entryFix[1] = g_entryFix[1].load(std::memory_order_relaxed);
    out->fullMask = g_fullMask.load(std::memory_order_relaxed);
}

void set_entry_fix(int pass, bool on) {
    if (pass < 0 || pass > 1) return;
    g_entryFix[pass].store(on, std::memory_order_relaxed);
}

bool entry_fix(int pass) {
    return (pass < 0 || pass > 1) ? false : g_entryFix[pass].load(std::memory_order_relaxed);
}

void set_anim_mode(bool on) {
    g_animMode.store(on, std::memory_order_relaxed);
}

bool anim_mode() {
    return g_animMode.load(std::memory_order_relaxed);
}

void set_anim_trans(float t) {
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    g_animTrans.store(t, std::memory_order_relaxed);
}

float anim_trans() {
    return g_animTrans.load(std::memory_order_relaxed);
}

uint32_t adopt_count(int hand) {
    return (hand < 0 || hand > 1) ? 0
                                  : g_adopts[hand].load(std::memory_order_relaxed);
}

void flicker_snapshot(FlickerStats* out) {
    if (!out) return;
    for (int p = 0; p < 4; ++p) {
        for (int k = 0; k < 2; ++k)
            out->catches[p][k] = g_catch[p][k].load(std::memory_order_relaxed);
        // Drain the window max: exchange gives the caller-defined window
        // semantics without a second clear pass racing a concurrent catch.
        out->dmaxMs[p] = g_catchDeltaMaxMs[p].exchange(0, std::memory_order_relaxed);
    }
    // Phases 4-7 (s74 entry / post-writer sites) are read by the [race] and
    // wfix reports; drain their window max here too so it does not pin.
    for (int p = 4; p < 8; ++p) g_catchDeltaMaxMs[p].exchange(0, std::memory_order_relaxed);
    out->driveAdoptEvents[0] = g_driveAdoptEvents[0].load(std::memory_order_relaxed);
    out->driveAdoptEvents[1] = g_driveAdoptEvents[1].load(std::memory_order_relaxed);
    out->adopts[0] = g_adopts[0].load(std::memory_order_relaxed);
    out->adopts[1] = g_adopts[1].load(std::memory_order_relaxed);
    out->wAdopts = g_wAdopts.load(std::memory_order_relaxed);
    out->peRepaints = g_peRepaints.load(std::memory_order_relaxed);
    out->worldChanges = g_worldChanges.load(std::memory_order_relaxed);
    out->wRescans = g_wRescans.load(std::memory_order_relaxed);
}

bool flicker_log() {
    return g_flickLog.load(std::memory_order_relaxed);
}

void set_flicker_log(bool on) {
    g_flickLog.store(on, std::memory_order_relaxed);
}

void set_scale(int hand, float scale) {
    if (hand < 0 || hand > 1) return;
    g_scale[hand].store(scale, std::memory_order_relaxed);
}

float scale_of(int hand) {
    return (hand < 0 || hand > 1) ? 1.0f : g_scale[hand].load(std::memory_order_relaxed);
}

void set_scale_attach(bool on) {
    g_scaleAttach.store(on, std::memory_order_relaxed);
}

bool scale_attach() {
    return g_scaleAttach.load(std::memory_order_relaxed);
}

void set_arms_mode(int mode) {
    if (mode < 0 || mode > 2) return;
    g_armsMode.store(mode, std::memory_order_relaxed);
}

int arms_mode() {
    return g_armsMode.load(std::memory_order_relaxed);
}

void on_world_change(const char* why) {
    drop(why);
    // The holdable and its skeleton died with the world - forget them without
    // touching memory (wskel_drop's restore self-gates on wskel_intact).
    if (g_wHold) wskel_drop(why);
    g_scanDormant = false;
    g_scanMisses = 0;
    g_worldChanges.fetch_add(1, std::memory_order_relaxed); // flicker correlate
    g_wRetSaved = 0; // a world change can unwind past the writer ret-stub
}

void* hands_actor() {
    return g_hands;
}

bool current_holdable(void** out) {
    if (!out) return false;
    *out = nullptr;
    // Rig-gated RAW read (session-21 rules b/c): the rig's own vtable is the
    // only gate; the holdable is NEVER vtable-gated - BS1's MachineGun and
    // GrenadeLauncher carried a different native vtable than the rest of the
    // family and a gated read pinned the stale weapon for a whole headset
    // run. Class validation happens downstream via object_class_name.
    // false = rig unknown; true + null = rig known, nothing equipped.
    if (!g_hands || !g_imageBase) return false;
    if (!is_memory_valid(g_hands, sizeof(void*)) ||
        *reinterpret_cast<const uint8_t* const*>(g_hands) !=
            g_imageBase + patterns::kAHandsVtableRva)
        return false;
    if (!is_memory_valid(g_hands + patterns::kHandsCurrentHoldableOffset,
                         sizeof(void*)))
        return false;
    *out = *reinterpret_cast<void**>(g_hands + patterns::kHandsCurrentHoldableOffset);
    return true;
}

void wskel_drive() {
    float ws = g_wScale.load(std::memory_order_relaxed);
    if (!(ws > 0.05f && ws < 20.0f)) ws = 1.0f;
    if (ws == 1.0f) {
        // Authored size = zero interference: restore anything we touched and
        // stay entirely off the weapon's bank.
        if (g_wHold) wskel_drop("wscale 1.0");
        return;
    }
    if (!g_imageBase) return;
    if (!wskel_resolve()) return;
    uint32_t adoptsBefore = g_wAdopts.load(std::memory_order_relaxed);
    wskel_compose(ws);
    if (g_wAdopts.load(std::memory_order_relaxed) != adoptsBefore)
        g_driveAdoptEvents[1].fetch_add(1, std::memory_order_relaxed);
    g_wStampMs = GetTickCount64();
    ++g_wDrives;
}

void wskel_release(const char* why) {
    if (g_wHold) wskel_drop(why);
}

void set_weapon_scale(float s) {
    if (s > 0.05f && s < 20.0f) g_wScale.store(s, std::memory_order_relaxed);
}

float weapon_scale() {
    return g_wScale.load(std::memory_order_relaxed);
}

void set_weapon_offset(float fwdCm, float rightCm, float upCm) {
    g_wOffCm[0].store(fwdCm, std::memory_order_relaxed);
    g_wOffCm[1].store(rightCm, std::memory_order_relaxed);
    g_wOffCm[2].store(upCm, std::memory_order_relaxed);
}

float weapon_off_fwd_cm() { return g_wOffCm[0].load(std::memory_order_relaxed); }
float weapon_off_right_cm() { return g_wOffCm[1].load(std::memory_order_relaxed); }
float weapon_off_up_cm() { return g_wOffCm[2].load(std::memory_order_relaxed); }

void set_weapon_offset_game(float x, float y, float z) {
    g_wOffGame[0] = x;
    g_wOffGame[1] = y;
    g_wOffGame[2] = z;
}

bool last_write(int hand, float* x, float* y, float* z, uint64_t* ageMs) {
    if (hand < 0 || hand > 1 || g_writeStampMs[hand] == 0) return false;
    if (x) *x = g_lastWriteLoc[hand][0];
    if (y) *y = g_lastWriteLoc[hand][1];
    if (z) *z = g_lastWriteLoc[hand][2];
    if (ageMs) *ageMs = GetTickCount64() - g_writeStampMs[hand];
    return true;
}

bool handle_command(const char* args) {
    int lo = 0, hi = 0, anchor = 0, extra = -1;
    char side[8] = {};
    if (strncmp(args, "status", 6) == 0 || *args == '\0') {
        const char* armNames[] = {"game", "follow", "hide"};
        BVR_LOG("[b2r] vrbones status: rig %s (%d bones), ref %s, names %s, arms %s, "
                "scaleattach %s, pe repaints %u, anim %s trans %.2f adopts L %u R %u",
                g_hands ? "RESOLVED" : "-", g_boneCount, g_refValid ? "captured" : "-",
                g_namesValid ? "mapped" : "-",
                armNames[g_armsMode.load(std::memory_order_relaxed)],
                g_scaleAttach.load(std::memory_order_relaxed) ? "on" : "off",
                g_peRepaints.load(std::memory_order_relaxed),
                g_animMode.load(std::memory_order_relaxed) ? "on" : "off",
                g_animTrans.load(std::memory_order_relaxed),
                g_adopts[0].load(std::memory_order_relaxed),
                g_adopts[1].load(std::memory_order_relaxed));
        for (int h = 0; h < 2; ++h)
            BVR_LOG("[b2r]   %s cluster %d..%d +%d anchor %d scale %.2f, drives %u, "
                    "last write (%.1f %.1f %.1f)",
                    h ? "R" : "L", g_cluster[h].lo, g_cluster[h].hi, g_cluster[h].extra,
                    g_cluster[h].anchor, g_scale[h].load(std::memory_order_relaxed),
                    g_driveCount[h], g_lastWriteLoc[h][0], g_lastWriteLoc[h][1],
                    g_lastWriteLoc[h][2]);
        BVR_LOG("[b2r]   weapon skel: %s (holdable %p, %d bones), wscale %.2f, "
                "drives %u, adopts %u",
                g_wPose ? "RESOLVED" : "-", g_wHold, g_wBoneCount,
                g_wScale.load(std::memory_order_relaxed), g_wDrives,
                g_wAdopts.load(std::memory_order_relaxed));
        {
            // Session 42: the residue line - "did the hands get handed back"
            // answerable without provoking an edge.
            int wm[2] = {0, 0};
            for (int h = 0; h < 2; ++h)
                for (int i = 0; i < g_boneCount; ++i)
                    if (g_writtenMask[h][i]) ++wm[h];
            bvr::vr::CineDrive cd = bvr::vr::cine_drive();
            BVR_LOG("[b2r]   residue: masked L %d R %d wskel %s | cineHold=%d "
                    "drive=%s",
                    wm[0], wm[1], g_wHold ? "HELD" : "-",
                    bvr::hud::cinematic_hold() ? 1 : 0,
                    cd == bvr::vr::CineDrive::Off        ? "off"
                    : cd == bvr::vr::CineDrive::Authored ? "authored"
                                                         : "authored+look");
        }
        {
            // Session 42: cumulative flicker catches + the printed invariant.
            uint32_t sum = 0;
            for (int p = 0; p < 8; ++p) // s74: phases 4-7 (entry / post-writer) count too
                for (int k = 0; k < 2; ++k)
                    sum += g_catch[p][k].load(std::memory_order_relaxed);
            uint32_t pe = g_peRepaints.load(std::memory_order_relaxed);
            BVR_LOG("[b2r]   flick: log %s | catches h/w pe1=%u/%u pe2=%u/%u "
                    "fl1=%u/%u fl2=%u/%u (sum %u %s peRepaints %u) | drvAdopt "
                    "h=%u w=%u | world %u rescans %u",
                    g_flickLog.load(std::memory_order_relaxed) ? "on" : "off",
                    g_catch[0][0].load(std::memory_order_relaxed),
                    g_catch[0][1].load(std::memory_order_relaxed),
                    g_catch[1][0].load(std::memory_order_relaxed),
                    g_catch[1][1].load(std::memory_order_relaxed),
                    g_catch[2][0].load(std::memory_order_relaxed),
                    g_catch[2][1].load(std::memory_order_relaxed),
                    g_catch[3][0].load(std::memory_order_relaxed),
                    g_catch[3][1].load(std::memory_order_relaxed),
                    sum, sum == pe ? "==" : "!= (INVARIANT BROKEN)", pe,
                    g_driveAdoptEvents[0].load(std::memory_order_relaxed),
                    g_driveAdoptEvents[1].load(std::memory_order_relaxed),
                    g_worldChanges.load(std::memory_order_relaxed),
                    g_wRescans.load(std::memory_order_relaxed));
        }
        return true;
    }
    if (strncmp(args, "flick", 5) == 0) {
        // flick on|off|status - gates the [flick] minute line only.
        if (strstr(args + 5, "on"))
            g_flickLog.store(true, std::memory_order_relaxed);
        else if (strstr(args + 5, "off"))
            g_flickLog.store(false, std::memory_order_relaxed);
        BVR_LOG("[b2r] command: vrbones flick %s (counters always count)",
                g_flickLog.load(std::memory_order_relaxed) ? "on" : "off");
        return true;
    }
    if (strncmp(args, "p1fix", 5) == 0 || strncmp(args, "p2fix", 5) == 0) {
        // p1fix|p2fix on|off - repaint the driven pose at that pass's ENTRY
        // (the session-74 fix candidate; the [race] line grades it).
        int pass = args[1] == '2' ? 1 : 0;
        if (strstr(args + 5, "on")) set_entry_fix(pass, true);
        else if (strstr(args + 5, "off")) set_entry_fix(pass, false);
        BVR_LOG("[b2r] command: vrbones p%dfix %s (entry repaint pass 1=%d pass 2=%d)",
                pass + 1, entry_fix(pass) ? "on" : "off", entry_fix(0) ? 1 : 0,
                entry_fix(1) ? 1 : 0);
        return true;
    }
    if (strncmp(args, "race", 4) == 0) {
        // race [on|off] - arm/disarm the six-point probe (off by default: it
        // costs syscalls per pair), then print the totals either way.
        if (strstr(args + 4, "on")) set_race_probe(true);
        else if (strstr(args + 4, "off")) set_race_probe(false);
        RaceStats rs;
        race_snapshot(&rs);
        BVR_LOG("[b2r] race totals: p1 entry=%u/%u flush=%u/%u drained=%u/%u | "
                "p2 entry=%u/%u flush=%u/%u drained=%u/%u | calls %u/%u/%u/%u/%u/%u "
                "| firstDiff %d/%d/%d/%d/%d/%d | entry catches p1=%u/%u p2=%u/%u | "
                "fix p1=%d p2=%d fullmask=%d (hands/weapon)",
                rs.foreign[0][0], rs.foreign[0][1], rs.foreign[1][0], rs.foreign[1][1],
                rs.foreign[2][0], rs.foreign[2][1], rs.foreign[3][0], rs.foreign[3][1],
                rs.foreign[4][0], rs.foreign[4][1], rs.foreign[5][0], rs.foreign[5][1],
                rs.calls[0], rs.calls[1], rs.calls[2], rs.calls[3], rs.calls[4],
                rs.calls[5], rs.firstDiff[0], rs.firstDiff[1], rs.firstDiff[2],
                rs.firstDiff[3], rs.firstDiff[4], rs.firstDiff[5],
                rs.entryCatch[0][0], rs.entryCatch[0][1], rs.entryCatch[1][0],
                rs.entryCatch[1][1], rs.entryFix[0] ? 1 : 0, rs.entryFix[1] ? 1 : 0,
                rs.fullMask ? 1 : 0);
        return true;
    }
    if (strncmp(args, "wfix", 4) == 0) {
        if (strstr(args + 4, "on")) wfix_set(true);
        else if (strstr(args + 4, "off")) wfix_set(false);
        else if (strstr(args + 4, "install")) wfix_install();
        wfix_status();
        return true;
    }
    if (strncmp(args, "watch", 5) == 0) {
        if (strstr(args + 5, "on")) watch_set(true);
        else if (strstr(args + 5, "off")) watch_set(false);
        watch_status();
        return true;
    }
    if (strncmp(args, "fullmask", 8) == 0) {
        if (strstr(args + 8, "on")) set_full_mask(true);
        else if (strstr(args + 8, "off")) set_full_mask(false);
        BVR_LOG("[b2r] command: vrbones fullmask %s (repaint decision scans %s)",
                full_mask() ? "on" : "off",
                full_mask() ? "every masked bone" : "the first masked bone only");
        return true;
    }
    if (strncmp(args, "diag31", 6) == 0) {
        // diag31 on|off|status - the issue-#31 umbrella: minute lines
        // ([flick]+[pair]) plus the event-edge witnesses ([pairEdge] pairing
        // breaks in core, [hudgate] one-eye HUD-burn intervals). One verb so a
        // tester arms everything the hunt needs in one line.
        if (strstr(args + 6, "on")) {
            g_flickLog.store(true, std::memory_order_relaxed);
            bvr::vr::set_pair_edge_log(true);
            bvr::hud::set_gate_log(true);
            set_race_probe(true);
        } else if (strstr(args + 6, "off")) {
            bvr::vr::set_pair_edge_log(false);
            bvr::hud::set_gate_log(false);
            set_race_probe(false);
        }
        BVR_LOG("[b2r] command: vrbones diag31 %s (minute lines %s, pairEdge "
                "%s, hudgate %s)",
                bvr::vr::pair_edge_log() ? "on" : "off",
                g_flickLog.load(std::memory_order_relaxed) ? "on" : "off",
                bvr::vr::pair_edge_log() ? "on" : "off",
                bvr::hud::gate_log() ? "on" : "off");
        return true;
    }
    // cluster l|r <lo> <hi> <anchor> [extra]
    if (sscanf_s(args, "cluster %7s %d %d %d %d", side, (unsigned)sizeof side, &lo, &hi,
                 &anchor, &extra) >= 4) {
        int h = (side[0] == 'l' || side[0] == 'L') ? 0 : 1;
        g_cluster[h] = {lo, hi, extra, anchor};
        BVR_LOG("[b2r] command: vrbones cluster %s %d..%d +%d anchor %d",
                h ? "r" : "l", lo, hi, extra, anchor);
        return true;
    }
    if (strncmp(args, "refcap", 6) == 0) {
        if (resolve_rig()) capture_reference();
        return true;
    }
    if (strncmp(args, "release", 7) == 0) {
        release("command");
        return true;
    }
    if (strncmp(args, "names", 5) == 0) {
        if (!resolve_rig()) {
            BVR_LOG("[b2r] vrbones names: rig not resolved (run in gameplay after "
                    "the rig-resolved echo)");
            return true;
        }
        if (!resolve_bone_names()) return true;
        for (int i = 0; i < g_boneCount; ++i)
            BVR_LOG("[b2r]   bone %2d: %s", i,
                    g_boneNames[i][0] ? g_boneNames[i] : "-");
        return true;
    }
    if (strncmp(args, "map", 3) == 0) {
        // Raw fallback: SharedSkeletonData head as dwords, for by-hand layout
        // derivation when the auto-detect refuses.
        if (!resolve_rig()) {
            BVR_LOG("[b2r] vrbones map: rig not resolved");
            return true;
        }
        const uint8_t* shared = nullptr;
        if (!read_n(g_skel + patterns::kSkelSharedDataOffset, &shared, 4) || !shared) {
            BVR_LOG("[b2r] vrbones map: SharedSkeletonData unreadable");
            return true;
        }
        BVR_LOG("[b2r] vrbones map: SharedSkeletonData %p", shared);
        for (uint32_t off = 0; off < 0x140; off += 0x10) {
            uint32_t d[4] = {};
            if (!read_n(shared + off, d, 16)) break;
            BVR_LOG("[b2r]   +0x%03X: %08X %08X %08X %08X", off, d[0], d[1], d[2],
                    d[3]);
        }
        return true;
    }
    // Diagnostic (session 40 round 2, the drum hunt): poke ONE bone's scale
    // channel and leave it - animation never restamps scale, so the poke
    // persists and the renderer answers "which bone does this attachment
    // actually ride, and in which direction". Run with vrhands off, or the
    // drive rewrites the cluster bones next frame.
    {
        int pi = -1;
        float sv = 1.0f;
        if (sscanf_s(args, "scaleone %d %f", &pi, &sv) == 2) {
            if (resolve_rig() && pi >= 0 && pi < g_boneCount) {
                float e[12];
                memcpy(e, g_pose + pi * patterns::kSkelPoseStride, 48);
                e[8] = e[9] = e[10] = sv;
                memcpy(g_pose + pi * patterns::kSkelPoseStride, e, 48);
                BVR_LOG("[b2r] command: vrbones scaleone %d %.2f ('%s')", pi, sv,
                        (g_namesValid && g_boneNames[pi][0]) ? g_boneNames[pi] : "?");
            } else {
                BVR_LOG("[b2r] vrbones scaleone: rig not resolved / bad index %d", pi);
            }
            return true;
        }
    }
    // --- session 41 holdable-offset derivation --------------------------------
    // `holdscan cap` snapshots the AHands actor's first 0x800 bytes;
    // `holdscan diff` lists the dword slots that changed since (run it across
    // a digit-key weapon switch - the slot that changes AND resolves as an
    // object is the holdable); `holdscan find` scans for the pointer the fire
    // seam last saw (the ground-truth method - BS1's derivation found the
    // weapon pointer exactly once in hands+0..0x800). Both methods must agree
    // before the offset is banked.
    if (strncmp(args, "holdscan", 8) == 0) {
        static uint8_t snap[0x800];
        static bool snapValid = false;
        const char* sub = args + 8;
        while (*sub == ' ') ++sub;
        if (!resolve_rig()) {
            BVR_LOG("[b2r] vrbones holdscan: rig not resolved");
            return true;
        }
        if (strncmp(sub, "cap", 3) == 0) {
            if (read_n(g_hands, snap, sizeof snap)) {
                snapValid = true;
                BVR_LOG("[b2r] holdscan: captured hands %p +0x000..0x800", g_hands);
            } else {
                BVR_LOG("[b2r] holdscan: hands read failed");
            }
            return true;
        }
        uint8_t cur[0x800];
        if (!read_n(g_hands, cur, sizeof cur)) {
            BVR_LOG("[b2r] holdscan: hands read failed");
            return true;
        }
        if (strncmp(sub, "diff", 4) == 0) {
            if (!snapValid) {
                BVR_LOG("[b2r] holdscan diff: no snapshot - run `holdscan cap` first");
                return true;
            }
            int shown = 0, probes = 0;
            for (uint32_t off = 0; off < sizeof cur; off += 4) {
                uint32_t a, b;
                memcpy(&a, snap + off, 4);
                memcpy(&b, cur + off, 4);
                if (a == b) continue;
                if (++shown > 24) continue; // count the rest silently
                BVR_LOG("[b2r]   holdscan diff +0x%03X: %08X -> %08X", off, a, b);
                // Object-looking new values get the identity probe (capped).
                if (probes < 6 && b > 0x10000 && (b & 3) == 0 &&
                    is_memory_valid(reinterpret_cast<void*>(static_cast<uintptr_t>(b)),
                                    0x48)) {
                    ++probes;
                    patterns::probe_object_identity(
                        reinterpret_cast<void*>(static_cast<uintptr_t>(b)),
                        "holdscan-diff");
                }
            }
            BVR_LOG("[b2r] holdscan diff: %d slot(s) changed (showing first 24)", shown);
            return true;
        }
        if (strncmp(sub, "find", 4) == 0) {
            void* w = aim::last_weapon_this();
            if (!w) {
                BVR_LOG("[b2r] holdscan find: no weapon seen at the seam yet - fire a "
                        "gun first (vrinput test trig r 255 300)");
                return true;
            }
            int hits = 0;
            for (uint32_t off = 0; off < sizeof cur; off += 4) {
                void* v;
                memcpy(&v, cur + off, 4);
                if (v != w) continue;
                ++hits;
                BVR_LOG("[b2r]   holdscan find: hands+0x%03X holds the fired weapon %p "
                        "<- holdable-offset candidate",
                        off, w);
            }
            BVR_LOG("[b2r] holdscan find: %d hit(s) for %p in hands+0x000..0x800", hits,
                    w);
            return true;
        }
        BVR_LOG("[b2r] vrbones holdscan cap|diff|find");
        return true;
    }
    // `wskel [hexptr]` - session 41: does the holdable carry its OWN
    // SkeletonInstance (vtable + owner backpointer, the AHands two-factor
    // identity)? Scans the object's first 0x800 bytes; the offset it reports
    // is the constant the uniform-weapon-scale lane banks.
    if (strncmp(args, "wskel", 5) == 0) {
        void* hold = nullptr;
        unsigned hv = 0;
        if (sscanf_s(args, "wskel %x", &hv) == 1)
            hold = reinterpret_cast<void*>(static_cast<uintptr_t>(hv));
        else
            hold = aim::last_weapon_this();
        if (!hold) {
            BVR_LOG("[b2r] vrbones wskel: no target - fire a gun first or pass a hex "
                    "pointer");
            return true;
        }
        if (!resolve_rig()) {
            BVR_LOG("[b2r] vrbones wskel: rig not resolved (image base unknown)");
            return true;
        }
        int hits = 0;
        for (uint32_t off = 0; off < 0x800; off += 4) {
            uint8_t* cand = nullptr;
            if (!read_n(static_cast<uint8_t*>(hold) + off, &cand, 4)) break;
            if (!cand || !is_memory_valid(cand, 0x60)) continue;
            if (*reinterpret_cast<const uint8_t* const*>(cand) !=
                g_imageBase + patterns::kSkeletonInstanceVtableRva)
                continue;
            ++hits;
            bool owned = *reinterpret_cast<uint8_t**>(cand + patterns::kSkelOwnerOffset) ==
                         hold;
            uint8_t* arr = cand + patterns::kSkelPoseArrayOffset;
            uint8_t* pose = *reinterpret_cast<uint8_t**>(arr);
            int32_t cnt = *reinterpret_cast<int32_t*>(arr + 4);
            bool poseOk = pose && cnt > 0 && cnt <= kMaxBones &&
                          is_memory_valid(pose, cnt * patterns::kSkelPoseStride);
            BVR_LOG("[b2r]   wskel: %p+0x%03X -> SkeletonInstance %p, owner %s, pose "
                    "%p x%d (%s)",
                    hold, off, cand, owned ? "MATCHES the holdable" : "OTHER", pose, cnt,
                    poseOk ? "valid" : "INVALID");
        }
        BVR_LOG("[b2r] vrbones wskel: %d SkeletonInstance hit(s) in %p+0x000..0x800",
                hits, hold);
        return true;
    }
    if (strncmp(args, "axes", 4) == 0) {
        int idx = g_cluster[1].anchor;
        sscanf_s(args, "axes %d", &idx);
        if (resolve_rig()) {
            if (!g_refValid) capture_reference();
            resolve_bone_names(); // best effort, names in the log line
            log_axes(idx);
        } else {
            BVR_LOG("[b2r] vrbones axes: rig not resolved");
        }
        return true;
    }
    BVR_LOG("[b2r] vrbones: status | cluster l|r <lo> <hi> <anchor> [extra] | refcap | "
            "release | names | map | axes [idx] | holdscan cap|diff|find | "
            "wskel [hex]");
    return true;
}

} // namespace bvr::b2r::bones
