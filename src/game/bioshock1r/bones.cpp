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
#include "core/util/log.h"
#include "game/bioshock1r/camera.h"
#include "game/bioshock1r/hands.h"
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

// Everything the last drive() wrote, for reapply() (the stereo second pass).
struct CachedBone {
    int idx;
    float p[3];
    float q[4];
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
// 2x the MEASURED idle envelope vs a frozen snapshot (session 20, 1 Hz sway
// telemetry: dpos peaks 3.01 UU, dang peaks 4.6 deg) - real animations move
// the anchors tens of UU / tens of degrees, so the gap is wide on both sides.
constexpr float kSwayPosThreshUu = 6.0f;
constexpr float kSwayAngThreshDeg = 12.0f;
// After a real animation, keep tracking this long past the LAST threshold
// crossing so the freeze lands on the SETTLED pose, not the last big frame.
constexpr uint64_t kSwaySettleMs = 600;
uint64_t g_lastBigDeltaMs = 0;
// Telemetry (1 Hz while frozen): the probe deltas the threshold judges, so
// the thresholds are set from measured idle amplitude, not guesses.
uint64_t g_lastSwayTlmMs = 0;

// Session 19: the whole INACTIVE hand collapses too - the drive poses only
// the active hand's cluster, so the other one stays engine-animated at the
// eye anchor and reads as a ghost hand. Cache mirrors g_cacheSleeve so the
// stereo second pass replays the same writes. writeScale is false for the
// weapon-attach bone: it hides by translation, never by scale (see drive()).
std::atomic<bool> g_hideInactive{true};
struct CachedHidden {
    int idx;
    float p[3];
    float s[3];
    bool writeScale;
};
CachedHidden g_cacheHidden[32];
int g_cacheHiddenCount = 0;
int g_hiddenHand = -1; // whose cluster is collapsed right now (game thread)

void* g_cacheSkelInst = nullptr;
uint64_t g_cacheMs = 0;

// Both clusters are baked (patterns.h) after live measurement; the lcluster
// command stays as a runtime override for future rig experiments.
std::atomic<int> g_lFirst{patterns::kBoneLClusterFirst}, g_lLast{patterns::kBoneLClusterLast},
    g_lAnchor{patterns::kBoneLWrist};
std::atomic<int> g_rAnchorOverride{-1};

std::atomic<bool> g_collapse{true}; // hide the driven arm's sleeve
std::atomic<uint32_t> g_writes{0};
std::atomic<uint32_t> g_reapplies{0};
std::atomic<int> g_lastHand{-1};
char g_status[160] = "idle";

// Render-lock (session 13): solve the anchor against the renderer's OWN
// foreground transform (captured per frame from the vm draws' cb0) so the
// rig lands on the world-correct pixel. See patterns.h "Foreground scene".
std::atomic<int> g_renderLock{1};         // 0 off, 1 abs (true position), 2 diff
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
        g_refValid = false; // new array = new reference
        g_hasWritten[0] = g_hasWritten[1] = false;
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
bool world_ndc(const FrameContext& ctx, const GamePose& gp, const FRotator& rot, float tanH,
               float* outX, float* outY, float* outDf) {
    float fwd[3], right[3], up[3];
    ue_rot_basis(rot, fwd, right, up);
    float d[3] = {gp.loc.x - ctx.camX, gp.loc.y - ctx.camY, gp.loc.z - ctx.camZ};
    float df = d[0] * fwd[0] + d[1] * fwd[1] + d[2] * fwd[2];
    if (df < 4.0f) return false; // target at/behind the eye: no stable pixel
    float dr = d[0] * right[0] + d[1] * right[1] + d[2] * right[2];
    float du = d[0] * up[0] + d[1] * up[1] + d[2] * up[2];
    float tanV = tanH * (9.0f / 16.0f); // 16:9 window (matches the option's meaning)
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
    bool matched = camera::fg_fov_match_active();
    float invTanHFg = matched ? 1.0f / tanH : patterns::kFgInvTanH;
    float invTanVFg = matched ? 1.0f / (tanH * (9.0f / 16.0f)) : patterns::kFgInvTanV;
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

    if (g_tlmWindowOpen) {
        BVR_LOG("[tlm] lock mode=%d tgt=(%.3f %.3f) df=%.1f k=%.2f wNat=%.1f w*=%.1f "
                "lat=%.2f depth=%+.2f",
                mode, ndcX, ndcY, df, k, wNat, wStar, latMag, dDepth);
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

void cluster_of(int hand, int* first, int* last, int* anchor) {
    if (hand == 1) {
        *first = patterns::kBoneRClusterFirst;
        *last = patterns::kBoneRClusterLast;
        int ov = g_rAnchorOverride.load(std::memory_order_relaxed);
        *anchor = ov >= 0 ? ov : patterns::kBoneWeaponAttach;
    } else {
        *first = g_lFirst.load(std::memory_order_relaxed);
        *last = g_lLast.load(std::memory_order_relaxed);
        *anchor = g_lAnchor.load(std::memory_order_relaxed);
    }
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
    g_cacheSkelInst = nullptr;
    g_cacheMs = 0;
    g_hiddenHand = -1; // the collapsed bones died with the old world
    g_cacheHiddenCount = 0;
}

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
        bool adopt = true;
        if (g_refValid && g_swayKill.load(std::memory_order_relaxed)) {
            adopt = false;
            uint64_t nowMs = GetTickCount64();
            float maxPos = 0.0f, maxAng = 0.0f;
            static const int kProbe[2] = {patterns::kBoneLWrist, patterns::kBoneWeaponAttach};
            for (int k = 0; k < 2; ++k) {
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
            }
            if (maxPos > kSwayPosThreshUu || maxAng > kSwayAngThreshDeg)
                g_lastBigDeltaMs = nowMs;
            // Track through the animation AND a settle window past its last
            // big frame, so the eventual freeze holds the SETTLED pose.
            adopt = (nowMs - g_lastBigDeltaMs) < kSwaySettleMs;
            if (g_telemetry.load(std::memory_order_relaxed) &&
                nowMs - g_lastSwayTlmMs >= 1000) {
                g_lastSwayTlmMs = nowMs;
                BVR_LOG("[tlm] sway probe: dpos=%.2f UU dang=%.2f deg (thresh %.1f/%.1f) %s",
                        maxPos, maxAng, kSwayPosThreshUu, kSwayAngThreshDeg,
                        adopt ? "TRACKING" : "frozen");
            }
        }
        if (adopt || !g_refValid) {
            memcpy(g_ref, fresh, sizeof(Qts) * static_cast<size_t>(g_boneCount));
            g_refValid = true;
        }
    }

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
    if (ptc[0] * ptc[0] + ptc[1] * ptc[1] + ptc[2] * ptc[2] > 500.0f * 500.0f) {
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
    if (g_renderLock.load(std::memory_order_relaxed) != 0) {
        float dLat[3], dDepth[3];
        if (render_lock_delta(ctx, gp, qaInv, actorRot, actorLoc, ptc, dLat, dDepth)) {
            float gl = g_lockGain.load(std::memory_order_relaxed);
            float gd = g_lockDepthGain.load(std::memory_order_relaxed);
            ptc[0] += dLat[0] * gl + dDepth[0] * gd;
            ptc[1] += dLat[1] * gl + dDepth[1] * gd;
            ptc[2] += dLat[2] * gl + dDepth[2] * gd;
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
    // NOTE viewmodel scale has NO working lever yet - three flat-proven dead
    // ends on 2026-07-27 (ENGINE_NOTES session 16 part 2): cluster bone .s
    // scales the skin but ANY scale in the wrist chain makes the engine's
    // attach path blow the weapon up near-plane (inverse-scale decompose;
    // excluding the attach helper's own .s changed nothing), and the rig
    // actor's DrawScale is geometry-inert on the fg path (bone positions
    // round-trip through it, skin and gun render unscaled). The factor needs
    // the render-path work (attach-matrix / fg section bake disasm, or the
    // vm_draw replay lane) - until then no scale is applied here.
    for (int i = first; i <= last; ++i) {
        float rel[3] = {g_ref[i].p[0] - pa[0], g_ref[i].p[1] - pa[1], g_ref[i].p[2] - pa[2]};
        float rot[3];
        qts_rotate(qtc, rel, rot);
        float p[3] = {ptc[0] + rot[0], ptc[1] + rot[1], ptc[2] + rot[2]};
        float q[4];
        quat_mul(qtc, g_ref[i].q, q);
        if (!write_n(g_bones[i].p, p, 12) || !write_n(g_bones[i].q, q, 16)) {
            g_skelInst = nullptr; // faulted mid-write: revalidate next frame
            g_cacheMs = 0;
            return false;
        }
        CachedBone& cb = g_cache[g_cacheCount++];
        cb.idx = i;
        memcpy(cb.p, p, 12);
        memcpy(cb.q, q, 16);
    }

    // Sleeve collapse: zero scale hides the geometry; pinning the position at
    // the target keeps any residual skin inside the fist instead of smeared
    // toward the shoulder. On the off-transition the reference values are
    // written back explicitly - the engine cannot be relied on to re-evaluate
    // while the drive keeps clearing the dirty flag.
    bool collapse = g_collapse.load(std::memory_order_relaxed);
    static bool s_wasCollapsed = false;
    const int* sleeve = hand == 1 ? patterns::kBoneRSleeve : patterns::kBoneLSleeve;
    const size_t sleeveCount = hand == 1 ? _countof(patterns::kBoneRSleeve)
                                         : _countof(patterns::kBoneLSleeve);
    if (collapse) {
        static const float kZero[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        for (size_t k = 0; k < sleeveCount; ++k) {
            int idx = sleeve[k];
            if (idx >= g_boneCount) continue;
            write_n(g_bones[idx].p, ptc, 12);
            write_n(g_bones[idx].s, kZero, 12);
            if (g_cacheSleeveCount < static_cast<int>(_countof(g_cacheSleeve))) {
                CachedSleeve& cs = g_cacheSleeve[g_cacheSleeveCount++];
                cs.idx = idx;
                memcpy(cs.p, ptc, 12);
                memcpy(cs.s, kZero, 12);
            }
        }
    } else if (s_wasCollapsed) {
        for (size_t k = 0; k < sleeveCount; ++k) {
            int idx = sleeve[k];
            if (idx >= g_boneCount) continue;
            write_n(g_bones[idx].p, g_ref[idx].p, 12);
            write_n(g_bones[idx].s, g_ref[idx].s, 12);
        }
    }
    s_wasCollapsed = collapse;

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
        if (!write_n(g_bones[cb.idx].p, cb.p, 12) || !write_n(g_bones[cb.idx].q, cb.q, 16)) {
            g_skelInst = nullptr;
            return;
        }
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
}

void handle_command(const char* args) {
    char verb[16] = {};
    int consumed = 0;
    if (sscanf_s(args, "%15s%n", verb, static_cast<unsigned>(sizeof verb), &consumed) != 1) {
        BVR_LOG("[bones] usage: vrbones status|list [n]|poke <idx> <dUU>|freeze on|off|"
                "collapse on|off|ref|anchor <idx>|lcluster <lo> <hi> <anchor>");
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
        int lockMode = g_renderLock.load(std::memory_order_relaxed);
        BVR_LOG("[bones] render lock: %s |delta|=%.2f UU gain=%.2f dgain=%.2f solves=%u "
                "skips=%u",
                lockMode == 0 ? "off" : lockMode == 2 ? "DIFF" : "ABS",
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
    } else if (strcmp(verb, "collapse") == 0) {
        bool on = strncmp(rest, "on", 2) == 0;
        g_collapse.store(on, std::memory_order_relaxed);
        if (!on) set_dirty(1); // engine re-evaluation restores sleeve scales
        BVR_LOG("[bones] sleeve collapse %s", on ? "ON" : "off");
    } else if (strcmp(verb, "ref") == 0) {
        g_refValid = false;
        g_hasWritten[0] = g_hasWritten[1] = false;
        set_dirty(1);
        BVR_LOG("[bones] reference pose recapture queued (next engine evaluation)");
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
                "lcluster|lock|lockgain|lockdgain|lockpull|log)",
                verb);
    }
}

void draw_debug_ui() {
    ImGui::Text("Bones: count %d writes %u hand %d", g_boneCount,
                g_writes.load(std::memory_order_relaxed),
                g_lastHand.load(std::memory_order_relaxed));
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
