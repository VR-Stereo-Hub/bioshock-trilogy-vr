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
// Correction gain. With the drive live the engine switches rig sections to a
// rigid path whose matrices REBUILD from the very bones we move, so part of
// the correction is applied twice; 0.5 measured closest to unity end to end.
std::atomic<float> g_lockGain{0.5f};
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
bool locate(void* handsActor) {
    void* si = nullptr;
    if (!read_n(static_cast<uint8_t*>(handsActor) + patterns::kActorSkelInstOffset, &si,
                sizeof si) ||
        !si) {
        g_skelInst = nullptr;
        return false;
    }
    void* vtbl = nullptr;
    if (!read_n(si, &vtbl, sizeof vtbl) || to_rva(vtbl) != patterns::kSkeletonInstanceVtableRva) {
        g_skelInst = nullptr;
        return false;
    }
    struct {
        Qts* bones;
        int count;
    } a{};
    if (!read_n(static_cast<uint8_t*>(si) + patterns::kSkelInstBonesOffset, &a, sizeof a) ||
        !a.bones || a.count < 8 || a.count > kMaxBones) {
        g_skelInst = nullptr;
        return false;
    }
    if (si != g_skelInst || a.bones != g_bones || a.count != g_boneCount) {
        BVR_LOG("[bones] skeleton: inst=%p bones=%p count=%d%s", si,
                static_cast<void*>(a.bones), a.count,
                a.count == patterns::kHandsRigBoneCount ? "" : " (UNEXPECTED count)");
        g_refValid = false; // new array = new reference
        g_hasWritten[0] = g_hasWritten[1] = false;
    }
    g_skelInst = si;
    g_bones = a.bones;
    g_boneCount = a.count;
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
// by the fixed projection) for a given component-frame rotation quat.
void build_fg_model(const float qd[4], float M[12]) {
    float fgF[3], fgR[3], fgU[3];
    static const float kX[3] = {1.0f, 0.0f, 0.0f}, kY[3] = {0.0f, 1.0f, 0.0f},
                       kZ[3] = {0.0f, 0.0f, 1.0f};
    quat_rotate(qd[0], qd[1], qd[2], qd[3], kX, fgF);
    quat_rotate(qd[0], qd[1], qd[2], qd[3], kY, fgR);
    quat_rotate(qd[0], qd[1], qd[2], qd[3], kZ, fgU);
    const float* E = patterns::kFgEyeComp;
    M[0] = patterns::kFgInvTanH * fgR[0];
    M[1] = patterns::kFgInvTanH * fgR[1];
    M[2] = patterns::kFgInvTanH * fgR[2];
    M[3] = -(M[0] * E[0] + M[1] * E[1] + M[2] * E[2]);
    M[4] = patterns::kFgInvTanV * fgU[0];
    M[5] = patterns::kFgInvTanV * fgU[1];
    M[6] = patterns::kFgInvTanV * fgU[2];
    M[7] = -(M[4] * E[0] + M[5] * E[1] + M[6] * E[2]);
    M[8] = fgF[0];
    M[9] = fgF[1];
    M[10] = fgF[2];
    M[11] = -(M[8] * E[0] + M[9] * E[1] + M[10] * E[2]);
}

// Solve M (rows x, y, w) for the component point that renders at (ndcX, ndcY)
// while keeping ptc's natural foreground depth (size/perspective unchanged).
bool solve_fg(const float M[12], float ndcX, float ndcY, const float ptc[3], float outP[3]) {
    float wNat = M[8] * ptc[0] + M[9] * ptc[1] + M[10] * ptc[2] + M[11];
    if (wNat < 1.0f) return false;
    float A[3][3] = {{M[0], M[1], M[2]}, {M[4], M[5], M[6]}, {M[8], M[9], M[10]}};
    float b[3] = {ndcX * wNat - M[3], ndcY * wNat - M[7], wNat - M[11]};
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
// given rotation and the world option FOV.
bool world_ndc(const FrameContext& ctx, const GamePose& gp, const FRotator& rot, float tanH,
               float* outX, float* outY) {
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
    return true;
}

// Compute the component-space nudge that counters the foreground pipeline's
// camera-coupled displacement of the rig. The pipeline (fixed 60-deg 4:3
// projection, eye parked ~32 UU behind the rig origin in ACTOR space, hand
// sway) is self-consistent at view center but slides the rig laterally once
// the camera splits from the actor rotation (HMD head-look). The transform is
// MODELED analytically (patterns.h kFgEyeComp block) - live captures embed
// the engine's per-frame re-derivations of section transforms from the very
// bones this module writes (feedback), so they cannot be used.
// Modes: abs = solve the anchor onto its TRUE world pixel (fixes the raised/
// too-close authored composition as well; inherits the vanilla sway wobble).
// diff = cancel only the head-split term (authored composition preserved,
// zero correction when camera == actor).
// qaInv/actorRot come from the drive (already computed there).
bool render_lock_delta(const FrameContext& ctx, const GamePose& gp, const float qaInv[4],
                       const FRotator& actorRot, const float ptc[3], float outDelta[3]) {
    int mode = g_renderLock.load(std::memory_order_relaxed);
    int32_t* opt = patterns::hfov_option_ptr();
    float hfov = opt && *opt > 0 ? static_cast<float>(*opt) : 0.0f;
    if (hfov <= 0.0f) return false;
    float tanH = tanf(hfov * 0.5f / kRadToDeg);

    FRotator camRot{ctx.camPitch, ctx.camYaw, ctx.camRoll};
    float ndcX, ndcY;
    if (!world_ndc(ctx, gp, camRot, tanH, &ndcX, &ndcY)) return false;

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
    float M1[12];
    build_fg_model(qd, M1);
    float p1[3];
    if (!solve_fg(M1, ndcX, ndcY, ptc, p1)) return false;

    if (mode == 2) {
        // Differential: subtract the zero-split solution so the correction
        // vanishes when the camera sits on the actor rotation.
        float ndcX0, ndcY0;
        if (!world_ndc(ctx, gp, actorRot, tanH, &ndcX0, &ndcY0)) return false;
        float M0[12], p0[3];
        build_fg_model(s_qBias, M0); // zero split still carries the composition bias
        if (!solve_fg(M0, ndcX0, ndcY0, ptc, p0)) return false;
        outDelta[0] = p1[0] - p0[0];
        outDelta[1] = p1[1] - p0[1];
        outDelta[2] = p1[2] - p0[2];
    } else {
        outDelta[0] = p1[0] - ptc[0];
        outDelta[1] = p1[1] - ptc[1];
        outDelta[2] = p1[2] - ptc[2];
    }

    if (g_tlmWindowOpen) {
        BVR_LOG("[tlm] lock mode=%d tgt=(%.3f %.3f) d=(%.2f %.2f %.2f)", mode, ndcX, ndcY,
                outDelta[0], outDelta[1], outDelta[2]);
    }
    float mag2 = outDelta[0] * outDelta[0] + outDelta[1] * outDelta[1] +
                 outDelta[2] * outDelta[2];
    if (mag2 > 30.0f * 30.0f) {
        static uint64_t lastDump = 0;
        uint64_t now = GetTickCount64();
        if (now - lastDump > 2000) {
            lastDump = now;
            BVR_LOG("[bones] lock: refusing outsized delta (%.1f %.1f %.1f)", outDelta[0],
                    outDelta[1], outDelta[2]);
        }
        return false;
    }
    g_lockDeltaMag.store(sqrtf(mag2), std::memory_order_relaxed);
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
}

bool telemetry_on() {
    return g_telemetry.load(std::memory_order_relaxed);
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
        if (!read_n(g_bones, g_ref, sizeof(Qts) * static_cast<size_t>(g_boneCount)))
            return false;
        g_refValid = true;
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
        float delta[3];
        if (render_lock_delta(ctx, gp, qaInv, actorRot, ptc, delta)) {
            float g = g_lockGain.load(std::memory_order_relaxed);
            ptc[0] += delta[0] * g;
            ptc[1] += delta[1] * g;
            ptc[2] += delta[2] * g;
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
                "collapse=%d",
                g_skelInst, static_cast<void*>(g_bones), g_boneCount,
                g_writes.load(std::memory_order_relaxed),
                g_lastHand.load(std::memory_order_relaxed), g_refValid ? 1 : 0,
                g_collapse.load(std::memory_order_relaxed) ? 1 : 0);
        int lockMode = g_renderLock.load(std::memory_order_relaxed);
        BVR_LOG("[bones] render lock: %s |delta|=%.2f UU solves=%u skips=%u",
                lockMode == 0 ? "off" : lockMode == 2 ? "DIFF" : "ABS",
                g_lockDeltaMag.load(std::memory_order_relaxed),
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
            BVR_LOG("[bones] render lock gain = %.2f", g);
        } else {
            BVR_LOG("[bones] usage: vrbones lockgain <0..2> (current %.2f)",
                    g_lockGain.load(std::memory_order_relaxed));
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
                "lcluster|lock|log)",
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
    int lockMode = g_renderLock.load(std::memory_order_relaxed);
    if (ImGui::RadioButton("lock off", &lockMode, 0) ||
        ImGui::RadioButton("lock ABS (true position)", &lockMode, 1) ||
        ImGui::RadioButton("lock DIFF (head-split cancel only)", &lockMode, 2))
        g_renderLock.store(lockMode, std::memory_order_relaxed);
    ImGui::Text("lock |delta| %.1f UU, solves %u, skips %u",
                g_lockDeltaMag.load(std::memory_order_relaxed),
                g_lockSolves.load(std::memory_order_relaxed),
                g_lockSkips.load(std::memory_order_relaxed));
    (void)g_status;
}

} // namespace bvr::b1r::bones
