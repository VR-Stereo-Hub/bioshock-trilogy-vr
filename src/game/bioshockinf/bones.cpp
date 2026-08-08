#include "game/bioshockinf/bones.h"

#include <windows.h>

#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "core/hooks/pattern_scan.h"
#include "core/util/log.h"
#include "game/bioshockinf/camera.h"
#include "game/bioshockinf/patterns.h"
#include "game/bioshockinf/reflect.h"

#include "imgui.h"

namespace bvr::bsi::bones {
namespace {

using bvr::pattern_scan::is_memory_valid;
using bvr::xrmath::quat_conj;
using bvr::xrmath::quat_mul;

constexpr int kMaxBones = 128;
constexpr uint32_t kAtom = patterns::kBoneAtomStride; // 0x20

// ---- resolved rig ----------------------------------------------------------
void* g_pawn = nullptr;        // XHuman the rig was resolved on
void* g_attach = nullptr;      // XFirstPersonAttachment
uint8_t* g_comp = nullptr;     // XSkeletalMeshComponent
uint8_t* g_bank = nullptr;     // SpaceBases.Data
int g_boneCount = 0;
uint64_t g_lastResolveMs = 0;
uint32_t g_resolves = 0, g_resolveFails = 0;

// Rig structure, re-read from RefSkeleton at every resolve (mesh facts, never
// baked): parent map, grip roots, per-hand cluster masks (grip subtree), and
// per-side arm-chain masks (name-classified).
int g_parent[kMaxBones];
int g_grip[2] = {-1, -1};                 // 0 = L_Grip, 1 = R_Grip
bool g_cluster[2][kMaxBones] = {};        // grip subtree, incl. the grip itself
bool g_armSet[2][kMaxBones] = {};         // PlayerHands[LR]*arm* chains
char g_names[kMaxBones][40];              // resolve-time copies, diagnostics only

// ---- write state ------------------------------------------------------------
// One written bank, disjoint per-hand masks (a bone can only be in one hand's
// mask by construction - the two grip subtrees are disjoint and the arm sets
// are side-classified).
float g_written[kMaxBones][8];            // quat[4], trans[3], scale
bool g_writtenMask[2][kMaxBones] = {};
uint8_t g_writeKind[2][kMaxBones] = {};   // 0 full, 2 hidden/collapsed
float g_anim[kMaxBones][8];               // the engine pose, adopted per drive
bool g_animValid[kMaxBones] = {};
float g_ref[kMaxBones][8];                // rigid-mode snapshot (anim off)
bool g_refValid = false;
uint64_t g_writeStampMs[2] = {};
float g_lastWriteLoc[2][3] = {};
int32_t g_lastWriteRot[2][3] = {};
uint32_t g_drives[2] = {}, g_adopts[2] = {}, g_reapplies = 0, g_gateRefusals = 0;
uint32_t g_midDrawRestamps = 0; // pass-1 sentinel found the bank != our write

// ---- helpers ----------------------------------------------------------------

uint32_t rva_of(const void* p) {
    const uint8_t* base = reinterpret_cast<const uint8_t*>(GetModuleHandleW(nullptr));
    const uint8_t* b = static_cast<const uint8_t*>(p);
    return (b && b > base) ? static_cast<uint32_t>(b - base) : 0;
}

// Quaternion (xyzw) from a row-vector rotation matrix R (world = local * R):
// quat_rotate's column convention needs M = R^T, so this reads R transposed.
// Rows are normalized defensively (a scaled component would poison the quat).
void quat_from_l2w_rows(const float r0[3], const float r1[3], const float r2[3],
                        float out[4]) {
    float m[3][3]; // m[i][j] = column-convention matrix = R[j][i]
    const float* rows[3] = {r0, r1, r2};
    float a[3][3];
    for (int i = 0; i < 3; ++i) {
        float n = sqrtf(rows[i][0] * rows[i][0] + rows[i][1] * rows[i][1] +
                        rows[i][2] * rows[i][2]);
        if (n < 1e-4f) n = 1.0f;
        a[i][0] = rows[i][0] / n;
        a[i][1] = rows[i][1] / n;
        a[i][2] = rows[i][2] / n;
    }
    // column-convention M[i][j] = a[j][i]
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) m[i][j] = a[j][i];
    const float tr = m[0][0] + m[1][1] + m[2][2];
    if (tr > 0.0f) {
        float s = sqrtf(tr + 1.0f) * 2.0f;
        out[3] = 0.25f * s;
        out[0] = (m[2][1] - m[1][2]) / s;
        out[1] = (m[0][2] - m[2][0]) / s;
        out[2] = (m[1][0] - m[0][1]) / s;
    } else if (m[0][0] > m[1][1] && m[0][0] > m[2][2]) {
        float s = sqrtf(1.0f + m[0][0] - m[1][1] - m[2][2]) * 2.0f;
        out[3] = (m[2][1] - m[1][2]) / s;
        out[0] = 0.25f * s;
        out[1] = (m[0][1] + m[1][0]) / s;
        out[2] = (m[0][2] + m[2][0]) / s;
    } else if (m[1][1] > m[2][2]) {
        float s = sqrtf(1.0f + m[1][1] - m[0][0] - m[2][2]) * 2.0f;
        out[3] = (m[0][2] - m[2][0]) / s;
        out[0] = (m[0][1] + m[1][0]) / s;
        out[1] = 0.25f * s;
        out[2] = (m[1][2] + m[2][1]) / s;
    } else {
        float s = sqrtf(1.0f + m[2][2] - m[0][0] - m[1][1]) * 2.0f;
        out[3] = (m[1][0] - m[0][1]) / s;
        out[0] = (m[0][2] + m[2][0]) / s;
        out[1] = (m[1][2] + m[2][1]) / s;
        out[2] = 0.25f * s;
    }
}

// UE-frame quaternion of an FRotator via the measured basis (ue_rot_basis is
// the s39-falsified frame; building the quat FROM that basis means the
// skeleton's convention and the rotator's agree by construction).
void quat_from_rotator(const FRotator& r, float out[4]) {
    float fwd[3], right[3], up[3];
    ue_rot_basis(r, fwd, right, up);
    // Rows of the row-vector matrix: local X image = fwd, Y = right, Z = up.
    quat_from_l2w_rows(fwd, right, up, out);
}

bool rig_intact() {
    if (!g_comp || !g_bank || g_boneCount <= 0) return false;
    if (!is_memory_valid(g_comp, 0x2A8)) return false;
    if (rva_of(*reinterpret_cast<void**>(g_comp)) != patterns::kSkelCompVtableRva)
        return false;
    if (*reinterpret_cast<void**>(g_comp + patterns::kSkelCompOuterOffset) != g_attach)
        return false;
    const uint8_t* data =
        *reinterpret_cast<uint8_t**>(g_comp + patterns::kSkelCompSpaceBasesOffset);
    const int32_t num = *reinterpret_cast<int32_t*>(g_comp +
                                                    patterns::kSkelCompSpaceBasesOffset + 4);
    if (data != g_bank || num != g_boneCount) return false;
    if (!is_memory_valid(g_bank, static_cast<size_t>(g_boneCount) * kAtom)) return false;
    return true;
}

void drop(const char* why) {
    if (g_comp) BVR_LOG("[bsi] bones: rig dropped (%s)", why);
    g_pawn = nullptr;
    g_attach = nullptr;
    g_comp = nullptr;
    g_bank = nullptr;
    g_boneCount = 0;
    g_refValid = false;
    memset(g_writtenMask, 0, sizeof g_writtenMask);
    memset(g_animValid, 0, sizeof g_animValid);
}

// Read RefSkeleton off the component's mesh: names, parents, grip subtrees,
// arm sets. All feasibility gates fail soft into a drop.
bool read_ref_skeleton() {
    const uint8_t* mesh =
        *reinterpret_cast<const uint8_t* const*>(g_comp + patterns::kSkelCompSkelMeshOffset);
    if (!is_memory_valid(mesh, patterns::kSkelMeshRefSkeletonOffset + 12)) return false;
    const uint8_t* data = *reinterpret_cast<const uint8_t* const*>(
        mesh + patterns::kSkelMeshRefSkeletonOffset);
    const int32_t num = *reinterpret_cast<const int32_t*>(
        mesh + patterns::kSkelMeshRefSkeletonOffset + 4);
    if (num != g_boneCount || num <= 0 || num > kMaxBones) return false;
    if (!is_memory_valid(data, static_cast<size_t>(num) * patterns::kMeshBoneStride))
        return false;

    g_grip[0] = g_grip[1] = -1;
    memset(g_cluster, 0, sizeof g_cluster);
    memset(g_armSet, 0, sizeof g_armSet);
    for (int i = 0; i < num; ++i) {
        const uint8_t* e = data + static_cast<size_t>(i) * patterns::kMeshBoneStride;
        const int32_t nameIdx =
            *reinterpret_cast<const int32_t*>(e + patterns::kMeshBoneNameOffset);
        g_parent[i] = *reinterpret_cast<const int32_t*>(e + patterns::kMeshBoneParentOffset);
        if (!patterns::fname_text(nameIdx, g_names[i], sizeof g_names[i]))
            g_names[i][0] = '\0';
        if (strcmp(g_names[i], "L_Grip") == 0) g_grip[0] = i;
        if (strcmp(g_names[i], "R_Grip") == 0) g_grip[1] = i;
    }
    if (g_grip[0] < 0 || g_grip[1] < 0) {
        BVR_LOG("[bsi] bones: RefSkeleton has no L_Grip/R_Grip (%d bones) - not the "
                "player-hands rig, refusing",
                num);
        return false;
    }
    // Grip subtrees: walk each bone's parent chain to a grip. Roots included.
    for (int i = 0; i < num; ++i) {
        int p = i;
        for (int guard = 0; guard < num; ++guard) {
            if (p == g_grip[0]) {
                g_cluster[0][i] = true;
                break;
            }
            if (p == g_grip[1]) {
                g_cluster[1][i] = true;
                break;
            }
            if (p == 0) break;
            p = g_parent[p];
        }
    }
    // Arm chains, classified by name: "...L_Arm...", "...Larm..." and the R
    // twins ("PlayerHandsL_ArmParent", "PlayerHandsLarm1", "PlayerHandsLarm21"
    // live). Case-insensitive; a bone already in a grip cluster stays there.
    for (int i = 0; i < num; ++i) {
        if (g_cluster[0][i] || g_cluster[1][i]) continue;
        char low[40];
        for (size_t k = 0; k < sizeof low; ++k) {
            low[k] = static_cast<char>(tolower(static_cast<unsigned char>(g_names[i][k])));
            if (!g_names[i][k]) break;
        }
        const char* arm = strstr(low, "arm");
        if (!arm || arm == low) continue;
        char side = *(arm - 1) == '_' && arm - 1 > low ? *(arm - 2) : *(arm - 1);
        if (side == 'l') g_armSet[0][i] = true;
        if (side == 'r') g_armSet[1][i] = true;
    }
    int cl = 0, cr = 0, al = 0, ar = 0;
    for (int i = 0; i < num; ++i) {
        cl += g_cluster[0][i];
        cr += g_cluster[1][i];
        al += g_armSet[0][i];
        ar += g_armSet[1][i];
    }
    BVR_LOG("[bsi] bones: RefSkeleton read - %d bones, L_Grip %d (subtree %d) R_Grip %d "
            "(subtree %d), arm chains L %d R %d",
            num, g_grip[0], cl, g_grip[1], cr, al, ar);
    return true;
}

void capture_reference() {
    for (int i = 0; i < g_boneCount; ++i) {
        memcpy(g_ref[i], g_bank + static_cast<size_t>(i) * kAtom, 32);
        memcpy(g_anim[i], g_ref[i], 32);
        g_animValid[i] = true;
    }
    g_refValid = true;
}

// Adopt the engine's freshly-restamped atom UNLESS the bank still holds our
// own last write (memcmp against g_written - the rule that stops us adopting
// ourselves and compounding). Whole 32-byte atoms: this engine restamps scale
// too (s45b oracle), so there is no pinned-scale row here.
void adopt_one(int hand, int i) {
    if (i < 0 || i >= g_boneCount) return;
    if (g_writtenMask[1 - hand][i]) return; // other hand owns it
    const uint8_t* bank = g_bank + static_cast<size_t>(i) * kAtom;
    if (g_writtenMask[hand][i]) {
        if (memcmp(bank, g_written[i], 32) == 0) return; // still ours
        const float* q = reinterpret_cast<const float*>(bank);
        const float n2 = q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3];
        if (!(n2 > 0.5f && n2 < 2.0f)) return; // torn read - refuse
        ++g_adopts[hand];
        ++g_midDrawRestamps; // it was restamped since OUR write - the cadence counter
    }
    memcpy(g_anim[i], bank, 32);
    g_animValid[i] = true;
}

} // namespace

void tick_resolve(uint64_t nowMs) {
    if (g_comp) {
        if (rig_intact()) return;
        drop("identity gate failed");
    }
    if (nowMs - g_lastResolveMs < 3000) return;
    g_lastResolveMs = nowMs;

    void* pc = camera::last_player_controller();
    if (!pc || !is_memory_valid(pc, patterns::kPcPawnOffset + 4)) return;
    void* pawn = *reinterpret_cast<void**>(static_cast<uint8_t*>(pc) +
                                           patterns::kPcPawnOffset);
    if (!pawn || !is_memory_valid(pawn, 0x40)) return;

    // The named front door: pawn.GetFirstPersonAttachment() -> the attachment
    // actor. Dispatch-by-name with the full bsicallat gate stack; a miss is a
    // wait, not a failure (menu, loading, no pawn possession yet).
    alignas(16) uint8_t parms[64] = {};
    if (!reflect::call_on_object(pawn, "GetFirstPersonAttachment", parms)) {
        ++g_resolveFails;
        return;
    }
    void* attach = *reinterpret_cast<void**>(parms);
    if (!attach || !is_memory_valid(attach, patterns::kFpAttachMeshCompOffset + 4)) {
        ++g_resolveFails;
        return;
    }
    uint8_t* comp = *reinterpret_cast<uint8_t**>(static_cast<uint8_t*>(attach) +
                                                 patterns::kFpAttachMeshCompOffset);
    if (!comp || !is_memory_valid(comp, 0x2A8)) {
        ++g_resolveFails;
        return;
    }
    if (rva_of(*reinterpret_cast<void**>(comp)) != patterns::kSkelCompVtableRva) {
        BVR_LOG("[bsi] bones: component vtable 0x%X != derived 0x%X - refusing",
                rva_of(*reinterpret_cast<void**>(comp)), patterns::kSkelCompVtableRva);
        ++g_resolveFails;
        return;
    }
    uint8_t* bank = *reinterpret_cast<uint8_t**>(comp + patterns::kSkelCompSpaceBasesOffset);
    const int32_t num =
        *reinterpret_cast<int32_t*>(comp + patterns::kSkelCompSpaceBasesOffset + 4);
    const int32_t max =
        *reinterpret_cast<int32_t*>(comp + patterns::kSkelCompSpaceBasesOffset + 8);
    if (!bank || num <= 0 || num > kMaxBones || num != max ||
        !is_memory_valid(bank, static_cast<size_t>(num) * kAtom)) {
        ++g_resolveFails;
        return;
    }
    g_pawn = pawn;
    g_attach = attach;
    g_comp = comp;
    g_bank = bank;
    g_boneCount = num;
    if (!read_ref_skeleton()) {
        drop("RefSkeleton refused");
        return;
    }
    capture_reference();
    memset(g_writtenMask, 0, sizeof g_writtenMask);
    ++g_resolves;
    BVR_LOG("[bsi] bones: rig RESOLVED - pawn %p attachment %p component %p, %d bones, "
            "SpaceBases %p",
            pawn, attach, comp, num, bank);
}

bool drive(const FrameContext& fc, const GamePose& target, int hand, float scale,
           int armsMode, bool animMode) {
    if (hand < 0 || hand > 1) return false;
    if (!g_comp) return false;
    if (!rig_intact()) {
        drop("identity gate failed at drive");
        ++g_gateRefusals;
        return false;
    }
    if (!(scale > 0.05f && scale < 20.0f)) scale = 1.0f;
    const int anchor = g_grip[hand];

    // Component frame this instant: L2W rotation rows + world translation.
    const float* m = reinterpret_cast<const float*>(g_comp +
                                                    patterns::kSkelCompLocalToWorldOffset);
    const float* r0 = m + 0;
    const float* r1 = m + 4;
    const float* r2 = m + 8;
    const float* t = m + 12;
    float qa[4], qaInv[4];
    quat_from_l2w_rows(r0, r1, r2, qa);
    quat_conj(qa, qaInv);

    // World target -> component space. Row-vector inverse: l_j = dot(row_j, w).
    const float w[3] = {target.loc.x - t[0], target.loc.y - t[1], target.loc.z - t[2]};
    float ptc[3] = {r0[0] * w[0] + r0[1] * w[1] + r0[2] * w[2],
                    r1[0] * w[0] + r1[1] * w[1] + r1[2] * w[2],
                    r2[0] * w[0] + r2[1] * w[1] + r2[2] * w[2]};
    // Sanity refusal: a target absurdly far from the component means the basis
    // or the context is broken - refuse rather than teleport the mesh. 2000 UU
    // is ~13 m at the user's calibrated 150 UU/m.
    if (ptc[0] * ptc[0] + ptc[1] * ptc[1] + ptc[2] * ptc[2] > 2000.0f * 2000.0f) {
        ++g_gateRefusals;
        return false;
    }
    float qt[4], qtc[4];
    quat_from_rotator(target.rot, qt);
    quat_mul(qaInv, qt, qtc); // controller rotation, expressed in component space

    // Adopt the engine pose for everything we are about to write.
    const bool useAnim = animMode;
    for (int i = 0; i < g_boneCount; ++i) {
        if (!g_cluster[hand][i] && !(armsMode != 0 && g_armSet[hand][i])) continue;
        adopt_one(hand, i);
    }
    const float(*src)[8] = useAnim ? g_anim : g_ref;
    if (!useAnim && !g_refValid) return false;

    // Compose: q_i = qtc (x) srcQ_i ; p_i = ptc + qtc*(srcT_i - srcT_anchor)*scale.
    const float* aT = &src[anchor][4];
    for (int i = 0; i < g_boneCount; ++i) {
        const bool inCluster = g_cluster[hand][i];
        const bool inArms = armsMode != 0 && g_armSet[hand][i];
        if (!inCluster && !inArms) continue;
        const uint8_t kind = (inArms && armsMode == 2) ? 2 : 0;
        float wq[8];
        if (kind == 2) {
            // hide: collapse ONTO the driven grip and zero the scale - the
            // collapse is what degenerates the cross-boundary skin blend to
            // the wrist instead of stretching a web (BS2 s41, shape only).
            memcpy(wq, src[i], 32);
            wq[4] = ptc[0];
            wq[5] = ptc[1];
            wq[6] = ptc[2];
            wq[7] = 0.0f;
        } else {
            float dp[3] = {(src[i][4] - aT[0]) * scale, (src[i][5] - aT[1]) * scale,
                           (src[i][6] - aT[2]) * scale};
            float rp[3];
            bvr::xrmath::quat_rotate(qtc[0], qtc[1], qtc[2], qtc[3], dp, rp);
            quat_mul(qtc, src[i], wq); // src[i][0..3] is the quat
            wq[4] = ptc[0] + rp[0];
            wq[5] = ptc[1] + rp[1];
            wq[6] = ptc[2] + rp[2];
            wq[7] = src[i][7] * scale; // engine-adopted scale x hand scale
        }
        memcpy(g_written[i], wq, 32);
        memcpy(g_bank + static_cast<size_t>(i) * kAtom, wq, 32);
        g_writtenMask[hand][i] = true;
        g_writeKind[hand][i] = kind;
    }
    // Arms-mode shrink: bones written under a previous wider mode but not this
    // one drop out of the mask (engine restamp restores them next tick).
    for (int i = 0; i < g_boneCount; ++i) {
        if (!g_writtenMask[hand][i]) continue;
        if (!g_cluster[hand][i] && !(armsMode != 0 && g_armSet[hand][i]))
            g_writtenMask[hand][i] = false;
    }
    g_writeStampMs[hand] = GetTickCount64();
    g_lastWriteLoc[hand][0] = target.loc.x;
    g_lastWriteLoc[hand][1] = target.loc.y;
    g_lastWriteLoc[hand][2] = target.loc.z;
    g_lastWriteRot[hand][0] = target.rot.pitch;
    g_lastWriteRot[hand][1] = target.rot.yaw;
    g_lastWriteRot[hand][2] = target.rot.roll;
    ++g_drives[hand];
    return true;
}

void release(const char* why, int hand) {
    const int lo = hand < 0 ? 0 : hand;
    const int hi = hand < 0 ? 1 : hand;
    int cleared = 0;
    for (int h = lo; h <= hi; ++h)
        for (int i = 0; i < g_boneCount; ++i) {
            cleared += g_writtenMask[h][i];
            g_writtenMask[h][i] = false;
        }
    // No restore write: the engine restamps whole atoms (scale included) on
    // the next tick - measured s45b - and writing through a possibly-stale
    // pointer is the BS1 save-load hang. Stopping is always safe.
    if (cleared) BVR_LOG("[bsi] bones: release %s (%s) - %d bones handed back to the "
                         "engine's restamp",
                         hand < 0 ? "BOTH" : (hand ? "R" : "L"), why, cleared);
    if (hand < 0) g_refValid = false;
}

void reapply() {
    if (!g_comp || !g_bank) return;
    const uint64_t now = GetTickCount64();
    if (!rig_intact()) return;
    bool any = false;
    for (int h = 0; h < 2; ++h) {
        if (now - g_writeStampMs[h] > 100) continue; // stale - leave the engine alone
        for (int i = 0; i < g_boneCount; ++i) {
            if (!g_writtenMask[h][i]) continue;
            memcpy(g_bank + static_cast<size_t>(i) * kAtom, g_written[i], 32);
            any = true;
        }
    }
    if (any) ++g_reapplies;
}

void on_world_change(const char* why) {
    release(why, -1);
    drop(why);
}

bool last_write(int hand, FVector& loc, FRotator& rot, uint32_t& drives, uint32_t& adopts) {
    if (hand < 0 || hand > 1 || !g_drives[hand]) return false;
    loc.x = g_lastWriteLoc[hand][0];
    loc.y = g_lastWriteLoc[hand][1];
    loc.z = g_lastWriteLoc[hand][2];
    rot.pitch = g_lastWriteRot[hand][0];
    rot.yaw = g_lastWriteRot[hand][1];
    rot.roll = g_lastWriteRot[hand][2];
    drives = g_drives[hand];
    adopts = g_adopts[hand];
    return true;
}

bool handle_command(const char* cmd, const char* args) {
    if (strcmp(cmd, "bsibones") != 0) return false;
    if (args && strncmp(args, "names", 5) == 0) {
        if (!g_comp) {
            BVR_LOG("[bsi] bones: rig not resolved");
            return true;
        }
        for (int i = 0; i < g_boneCount; ++i)
            BVR_LOG("[bsi] bones: %2d parent %2d  %s%s%s%s", i, g_parent[i], g_names[i],
                    g_cluster[0][i] ? "  [L cluster]" : "",
                    g_cluster[1][i] ? "  [R cluster]" : "",
                    g_armSet[0][i] ? "  [L arm]" : (g_armSet[1][i] ? "  [R arm]" : ""));
        return true;
    }
    if (args && strncmp(args, "release", 7) == 0) {
        release("command", -1);
        return true;
    }
    // status (default)
    BVR_LOG("[bsi] bones: rig %s comp=%p bones=%d resolves=%u fails=%u | drives L=%u "
            "R=%u adopts L=%u R=%u midDrawRestamps=%u reapplies=%u gateRefusals=%u",
            g_comp ? "RESOLVED" : "not resolved", (void*)g_comp, g_boneCount, g_resolves,
            g_resolveFails, g_drives[0], g_drives[1], g_adopts[0], g_adopts[1],
            g_midDrawRestamps, g_reapplies, g_gateRefusals);
    for (int h = 0; h < 2; ++h)
        if (g_drives[h])
            BVR_LOG("[bsi] bones:   %c last write loc=(%.1f %.1f %.1f) rot=(%d %d %d)",
                    h ? 'R' : 'L', g_lastWriteLoc[h][0], g_lastWriteLoc[h][1],
                    g_lastWriteLoc[h][2], g_lastWriteRot[h][0], g_lastWriteRot[h][1],
                    g_lastWriteRot[h][2]);
    return true;
}

void draw_debug_ui() {
    ImGui::Text("rig: %s (%d bones, resolves %u)", g_comp ? "resolved" : "searching",
                g_boneCount, g_resolves);
    ImGui::Text("drives L %u R %u | adopts L %u R %u | reapplies %u", g_drives[0],
                g_drives[1], g_adopts[0], g_adopts[1], g_reapplies);
}

} // namespace bvr::bsi::bones
