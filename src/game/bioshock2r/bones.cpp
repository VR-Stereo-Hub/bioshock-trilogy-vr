#include "game/bioshock2r/bones.h"

#include "core/hooks/pattern_scan.h"
#include "core/util/log.h"
#include "core/util/xr_math.h"
#include "game/bioshock2r/aim.h"
#include "game/bioshock2r/patterns.h"

#include <windows.h>

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
std::atomic<float> g_scale[2] = {{1.0f}, {1.0f}};
std::atomic<bool> g_scaleAttach{true}; // round-1 behavior kept; F10 escape hatch
std::atomic<int> g_armsMode{1};        // 0 game, 1 follow (user test default), 2 hide
int g_armsApplied[2] = {-1, -1};       // game thread; drives the transition restore
std::atomic<uint32_t> g_peRepaints{0}; // restamps caught mid-draw (flicker fix)

// Last write per hand, for reapply/repaint/release + telemetry. A bone MASK
// rather than ranges (session 40 round 2): the written set is no longer
// contiguous once the arm bones ride along.
float g_written[kMaxBones][12];
bool g_writtenMask[2][kMaxBones] = {};
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
    const float* refA = g_ref[anchor];
    const float* delta = qtc;
    float scale = g_scale[hand].load(std::memory_order_relaxed);
    if (!(scale > 0.05f && scale < 20.0f)) scale = 1.0f;

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

    memset(g_writtenMask[hand], 0, sizeof g_writtenMask[hand]);
    auto write_bone = [&](int i, bool scaleChannel) {
        const float* r = g_ref[i];
        float* w = g_written[i];
        // Anchor-relative offset, scaled so the cluster grows about its
        // anchor instead of only thickening bones in place.
        float dp[3] = {(r[0] - refA[0]) * scale, (r[1] - refA[1]) * scale,
                       (r[2] - refA[2]) * scale};
        float rp[3];
        bvr::xrmath::quat_rotate(delta[0], delta[1], delta[2], delta[3], dp, rp);
        w[0] = ptc[0] + rp[0];
        w[1] = ptc[1] + rp[1];
        w[2] = ptc[2] + rp[2];
        w[3] = r[3];
        bvr::xrmath::quat_mul(delta, &r[4], &w[4]);
        // Scale channel: poke-proven to render, and animation never restamps
        // it (patterns.h "the AHands rig"). scaleChannel=false keeps the
        // authored scale (the weapon-attach escape hatch - see scale_attach).
        w[8] = scaleChannel ? r[8] * scale : r[8];
        w[9] = scaleChannel ? r[9] * scale : r[9];
        w[10] = scaleChannel ? r[10] * scale : r[10];
        w[11] = r[11];
        memcpy(g_pose + i * patterns::kSkelPoseStride, w, 48);
        g_writtenMask[hand][i] = true;
    };
    // Hidden bone: authored transform, zero scale. Kept in the mask so
    // reapply/repaint hold it hidden and release() restores it.
    auto hide_bone = [&](int i) {
        float* w = g_written[i];
        memcpy(w, g_ref[i], 48);
        w[8] = 0.0f;
        w[9] = 0.0f;
        w[10] = 0.0f;
        memcpy(g_pose + i * patterns::kSkelPoseStride, w, 48);
        g_writtenMask[hand][i] = true;
    };

    for (int i = lo; i <= hi; ++i) write_bone(i, true);
    if (extra >= 0) write_bone(extra, g_scaleAttach.load(std::memory_order_relaxed));
    if (armsMode == 1) {
        for (int a = 0; a < patterns::kBoneArmCount; ++a)
            if (armIdx[a] < g_boneCount) write_bone(armIdx[a], true);
    } else if (armsMode == 2) {
        for (int a = 0; a < patterns::kBoneArmCount; ++a)
            if (armIdx[a] < g_boneCount) hide_bone(armIdx[a]);
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
}

void release(const char* why, int hand) {
    // Per-cluster restore: a left-hand release must not disturb a live right
    // hand, so only the released hand's own written bones (cluster + pivot +
    // arms, whatever the mask holds) go back to reference.
    bool any = false;
    for (int h = 0; h < 2; ++h) {
        if (hand >= 0 && h != hand) continue;
        if (g_refValid && g_pose) {
            for (int i = 0; i < g_boneCount; ++i)
                if (g_writtenMask[h][i]) {
                    memcpy(g_pose + i * patterns::kSkelPoseStride, g_ref[i], 48);
                    any = true;
                }
        }
        memset(g_writtenMask[h], 0, sizeof g_writtenMask[h]);
        g_armsApplied[h] = -1;
    }
    if (any)
        BVR_LOG("[b2r] bones: released %s (%s) - reference restored",
                hand < 0 ? "both clusters" : (hand ? "right" : "left"), why);
    // Only a full release drops the reference: recapturing it while the other
    // cluster is still driven would capture OUR pose as the new authored one.
    if (hand < 0) g_refValid = false;
}

void pe_repaint() {
    if (!g_pose || !g_refValid) return;
    uint64_t now = GetTickCount64();
    for (int h = 0; h < 2; ++h) {
        if (now - g_writeStampMs[h] > 100) continue; // not driving, nothing to defend
        // Sentinel: the first masked bone. Animation restamps translation and
        // rotation, so a 48-byte compare catches it the moment it lands.
        int s = -1;
        for (int i = 0; i < g_boneCount; ++i)
            if (g_writtenMask[h][i]) {
                s = i;
                break;
            }
        if (s < 0) continue;
        if (memcmp(g_pose + s * patterns::kSkelPoseStride, g_written[s], 48) == 0)
            continue;
        if (!is_memory_valid(g_pose, g_boneCount * patterns::kSkelPoseStride)) return;
        for (int i = 0; i < g_boneCount; ++i)
            if (g_writtenMask[h][i])
                memcpy(g_pose + i * patterns::kSkelPoseStride, g_written[i], 48);
        g_peRepaints.fetch_add(1, std::memory_order_relaxed);
    }
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
    g_scanDormant = false;
    g_scanMisses = 0;
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
                "scaleattach %s, pe repaints %u",
                g_hands ? "RESOLVED" : "-", g_boneCount, g_refValid ? "captured" : "-",
                g_namesValid ? "mapped" : "-",
                armNames[g_armsMode.load(std::memory_order_relaxed)],
                g_scaleAttach.load(std::memory_order_relaxed) ? "on" : "off",
                g_peRepaints.load(std::memory_order_relaxed));
        for (int h = 0; h < 2; ++h)
            BVR_LOG("[b2r]   %s cluster %d..%d +%d anchor %d scale %.2f, drives %u, "
                    "last write (%.1f %.1f %.1f)",
                    h ? "R" : "L", g_cluster[h].lo, g_cluster[h].hi, g_cluster[h].extra,
                    g_cluster[h].anchor, g_scale[h].load(std::memory_order_relaxed),
                    g_driveCount[h], g_lastWriteLoc[h][0], g_lastWriteLoc[h][1],
                    g_lastWriteLoc[h][2]);
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
            "release | names | map | axes [idx]");
    return true;
}

} // namespace bvr::b2r::bones
