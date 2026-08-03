#include "game/bioshock2r/bones.h"

#include "core/hooks/pattern_scan.h"
#include "core/util/log.h"
#include "core/util/xr_math.h"
#include "game/bioshock2r/patterns.h"

#include <windows.h>

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

// Cluster: [lo, hi] driven rigidly about the anchor. Default = the whole
// rig from one controller until the per-hand name split lands (session 40).
int g_clusterLo = 0;
int g_clusterHi = 63;
int g_anchor = 0;

// Last write, for reapply + telemetry.
float g_written[kMaxBones][12];
int g_writtenLo = -1, g_writtenHi = -1;
uint64_t g_writeStampMs = 0;
float g_lastWriteLoc[3] = {};
uint32_t g_driveCount = 0;

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
    g_writtenLo = g_writtenHi = -1;
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
    return true;
}

bool capture_reference() {
    if (!g_pose) return false;
    for (int i = 0; i < g_boneCount; ++i)
        memcpy(g_ref[i], g_pose + i * patterns::kSkelPoseStride, 48);
    g_refValid = true;
    BVR_LOG("[b2r] bones: reference pose captured (%d bones)", g_boneCount);
    return true;
}

// UE rotator -> quat via the shared helper; conjugate/multiply/rotate via
// core's xr_math (plain quaternion algebra - space-agnostic).
void rot_to_quat(const FRotator& r, float q[4]) { ue_rot_to_quat(r, q); }

} // namespace

bool drive(const FrameContext& ctx, const GamePose& target) {
    if (!resolve_rig()) return false;
    if (!g_refValid && !capture_reference()) return false;
    int lo = g_clusterLo, hi = g_clusterHi, anchor = g_anchor;
    if (lo < 0) lo = 0;
    if (hi >= g_boneCount) hi = g_boneCount - 1;
    if (anchor < lo || anchor > hi) anchor = lo;

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

    // Rigid map: delta = qtc * conj(refQ_anchor); each cluster bone gets
    // rot_i = delta * refQ_i, pos_i = ptc + delta * (refP_i - refP_anchor).
    // (hkQsTransform: translation at [0..3], quat at [4..7], scale [8..11].)
    const float* refA = g_ref[anchor];
    float refQaInv[4];
    bvr::xrmath::quat_conj(&refA[4], refQaInv);
    float delta[4];
    bvr::xrmath::quat_mul(qtc, refQaInv, delta);

    for (int i = lo; i <= hi; ++i) {
        const float* r = g_ref[i];
        float* w = g_written[i];
        float dp[3] = {r[0] - refA[0], r[1] - refA[1], r[2] - refA[2]};
        float rp[3];
        bvr::xrmath::quat_rotate(delta[0], delta[1], delta[2], delta[3], dp, rp);
        w[0] = ptc[0] + rp[0];
        w[1] = ptc[1] + rp[1];
        w[2] = ptc[2] + rp[2];
        w[3] = r[3];
        bvr::xrmath::quat_mul(delta, &r[4], &w[4]);
        w[8] = r[8];
        w[9] = r[9];
        w[10] = r[10];
        w[11] = r[11];
        memcpy(g_pose + i * patterns::kSkelPoseStride, w, 48);
    }
    g_writtenLo = lo;
    g_writtenHi = hi;
    g_writeStampMs = GetTickCount64();
    g_lastWriteLoc[0] = target.loc.x;
    g_lastWriteLoc[1] = target.loc.y;
    g_lastWriteLoc[2] = target.loc.z;
    ++g_driveCount;
    return true;
}

void reapply() {
    if (g_writtenLo < 0 || !g_pose) return;
    if (GetTickCount64() - g_writeStampMs > 100) return; // stale write, leave it
    for (int i = g_writtenLo; i <= g_writtenHi; ++i)
        memcpy(g_pose + i * patterns::kSkelPoseStride, g_written[i], 48);
}

void release(const char* why) {
    if (g_refValid && g_pose && g_writtenLo >= 0) {
        // Hand the rig back: restore the captured reference once so the
        // engine resumes from its own pose, not from our last write.
        for (int i = 0; i < g_boneCount; ++i)
            memcpy(g_pose + i * patterns::kSkelPoseStride, g_ref[i], 48);
        BVR_LOG("[b2r] bones: released (%s) - reference restored", why);
    }
    g_writtenLo = g_writtenHi = -1;
    g_refValid = false; // recapture on the next drive
}

void on_world_change(const char* why) {
    drop(why);
    g_scanDormant = false;
    g_scanMisses = 0;
}

bool last_write(float* x, float* y, float* z, uint64_t* ageMs) {
    if (g_writtenLo < 0) return false;
    if (x) *x = g_lastWriteLoc[0];
    if (y) *y = g_lastWriteLoc[1];
    if (z) *z = g_lastWriteLoc[2];
    if (ageMs) *ageMs = GetTickCount64() - g_writeStampMs;
    return true;
}

bool handle_command(const char* args) {
    int lo = 0, hi = 0, anchor = 0;
    if (strncmp(args, "status", 6) == 0 || *args == '\0') {
        BVR_LOG("[b2r] vrbones status: rig %s (%d bones), ref %s, cluster %d..%d "
                "anchor %d, drives %u, last write (%.1f %.1f %.1f)",
                g_hands ? "RESOLVED" : "-", g_boneCount, g_refValid ? "captured" : "-",
                g_clusterLo, g_clusterHi, g_anchor, g_driveCount, g_lastWriteLoc[0],
                g_lastWriteLoc[1], g_lastWriteLoc[2]);
        return true;
    }
    if (sscanf_s(args, "cluster %d %d %d", &lo, &hi, &anchor) == 3) {
        g_clusterLo = lo;
        g_clusterHi = hi;
        g_anchor = anchor;
        BVR_LOG("[b2r] command: vrbones cluster %d..%d anchor %d", lo, hi, anchor);
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
    BVR_LOG("[b2r] vrbones: status | cluster <lo> <hi> <anchor> | refcap | release");
    return true;
}

} // namespace bvr::b2r::bones
