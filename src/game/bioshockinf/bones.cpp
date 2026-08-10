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
#include "game/bioshockinf/fidget.h"
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
char g_names[kMaxBones][64];              // resolve-time copies (fname_text needs >= 64)
int32_t g_nameIdx[kMaxBones];             // s53: RefSkeleton's own stored FName indices

// ---- write state ------------------------------------------------------------
// One written bank, disjoint per-hand masks (a bone can only be in one hand's
// mask by construction - the two grip subtrees are disjoint and the arm sets
// are side-classified).
// ---- s46: the ready-pose glue (the persistent-stance kill) ------------------
// MEASURED MECHANISM (s46 snaps, drive off): the "weapon idle stance" is the
// attachment's SubtleFidget lane (dispatching StartSubtleFidget reproduces it
// on demand) - a discrete second pose the LEFT cluster holds: grip+palm rotate
// RIGIDLY ~101 deg with finger-curl on top, re-entering ~2.5 min after a shot
// and holding until the next one; post-fire is the READY pose by definition,
// and stance-vs-ready-vs-stance closes to the 0.5 deg idle noise floor.
// Source-side kills are dead on this retail build (console `set` nulled even
// the bHidden positive control; script execs already measured dead, s42).
//
// THE GLUE: fold corr = qRef (x) conj(src[anchor]) into the compose, per hand.
// The anchor then writes qtc (x) qRef - the controller carrying the CAPTURED
// ready pose - and every other bone keeps its pose RELATIVE to the anchor:
// a rigid whole-hand engine rotation S multiplies src[anchor] AND src[i] on
// the left, so conj(src[anchor]) cancels it exactly (quats double-cover, and
// the sign cancels in the pair), while articulated animation relative to the
// grip (finger curls, reload, the vigor flourish's articulation) passes
// through untouched. This is what "pin the anchor quat" has to MEAN on a
// name-flat component-space bank: every atom is absolute, so pinning one
// bone's quat alone would shear the mesh at the grip-palm boundary.
//
// qRef auto-captures 1.2 s after every player shot (fire.cpp's seam calls
// note_player_fire; the engine itself resets the stance on fire, so the
// post-fire window IS the ready pose - the mechanism mirrors the game's own
// reset), and a manual capture command exists for pacifist scenes.
// s48: default OFF - the glue was headset-REJECTED as the stance fix (it pins
// the driven bones while the anim still owns the rest of the model). The
// SubtleFidget ROOT KILL (fidget.cpp) replaces it; the glue stays as a bisect
// lever and as the compose correction the animtrans lever shares.
std::atomic<bool> g_stanceKill{false};
float g_readyQuat[2][4] = {};
std::atomic<bool> g_readyValid[2] = {};
std::atomic<uint64_t> g_captureAtMs[2] = {}; // 0 = nothing pending, per hand
uint32_t g_readyCaptures[2] = {};
// s47: ANIMTRANS - authored anchor TRAVEL pass-through. Measured (travel
// instrument, drive off): the reload anim moves the R anchor 21 UU (14 cm)
// and the L anchor 72 UU (48 cm, the cross-over rack); fire moves R 5-7 UU.
// All of it is discarded by the anchor-pin compose (dp is relative to the
// CURRENT anchor - which is exactly the translation analogue of the stance
// glue: whole-hand travel cancels, articulation relative to the grip passes).
// With animtrans ON the dp base becomes the banked READY anchor translation
// (captured with qRef), so the anchor departs from the controller by exactly
// the authored travel. OFF by default, NOT persisted; asymmetries the headset
// must judge: the glue still cancels the anim's whole-hand ROTATION while its
// travel passes, and a stance re-onset leaks its ~50 UU translation until the
// next shot (the glue only cancels the stance's rotation).
std::atomic<bool> g_animTrans{false};
float g_readyTrans[2][3] = {};
// s48: the LOCOMOTION fix. Headset verdict: stick-walking moves the model;
// flat: 9.26 UU (6.2 cm) of rigid component-space translation on both driven
// anchors at walk speed vs 0.81 UU stationary - one frame of walk speed. The
// mechanism: the hand target is built on THIS dispatch's fc.engineLoc while
// the L2W the compose inverts embeds the camera the engine placed the
// attachment with (a frame out of phase under locomotion). camPin composes
// the hand RELATIVE to the camera the drive WROTE this dispatch
// (fc.writtenLoc): target - writtenCam cancels engineLoc exactly, so no
// engine-phase term survives in translation. The lag probe (`bsibones lag`)
// measures c0 = R_l2w^-1 * (writtenCam - L2Wt) - camPin is exact when c0 is
// zero/constant-in-camera-frame - measured BEFORE this default was decided:
// c0 = 0.00 exactly on all axes stationary (7.7k samples); walking, c0 spans
// 11.2 UU (the lag made visible) and camPin cuts the driven-anchor wobble
// 9.26 -> 1.72 UU (the residual is view-bob rotation, second-order). ON.
std::atomic<bool> g_camPin{true};
// s50 (user report, mid-session): "shooting shouldn't affect the left hand".
// Flat-measured: a single right-hand pistol shot swings the LEFT anchor's
// composed rotation 95.58 deg (idle control 0.00; the right hand's own recoil
// 21.25) - the engine's fire anim moves the AUTHORED left grip and the
// adopt-then-compose lane passes whole-hand authored swings through
// (q_i = qtc (x) src[i] has no conj(src[anchor]) factor). The retired s46
// glue implements exactly that missing cancellation and measured 95.58 ->
// 0.14 deg with all else equal - but ALWAYS-on it would also cancel the
// flourish's arm sweep and the cast anims the headset already accepted. So:
// FIRE-SCOPED - the glue correction auto-engages for kFireGlueMs around each
// player shot (note_player_fire) and stays out of every other anim's way.
// `bsibones fireglue on|off` is the A/B; needs a ready capture (self-heals
// on every shot at +1.2 s, so only the first-ever shot of a boot leaks).
std::atomic<bool> g_fireGlue{true};
std::atomic<uint64_t> g_fireGlueUntilMs{0};
constexpr uint64_t kFireGlueMs = 1500;
// s51: the anchor glue was NOT enough. Headset verdict on the s50 build:
// "firing makes the arm/SHOULDERS jump and come back" - and the all-bones
// travel table names the mechanism: with fireglue ON the LEFT ANCHOR reads
// 0.10 deg (the corr works) while the rest of the left DRIVEN chain swings
// 74-133 deg / up to 87.7 cm (Larm21 133.54, Larm22 125.60, Larm1 115.09 at
// 131 UU). corr = readyQuat (x) conj(src[anchor]) cancels only the anchor's
// ABSOLUTE motion; every other bone's ANCHOR-RELATIVE articulation passes
// through by design (that is what lets fingers articulate) - and the fire
// anim articulates the whole arm relative to the grip. The chest (the one
// undriven bone) measured 0.00: the engine-owned-bones theory is falsified.
// THE FIX: during the fire window substitute the WHOLE HAND's source atoms
// (quat AND translation - the arm bones translate ~87 cm) from a ready bank
// captured at the same +1.2 s post-shot moment as readyQuat. corr then
// collapses to identity and the hand renders its banked ready articulation
// rigidly attached to the controller. `bsibones fireglue anchor` bisects
// back to the s50 anchor-only behavior.
std::atomic<bool> g_fireGlueFull{true};   // full-hand ready substitution
float g_readyAtoms[kMaxBones][8];         // per-bone ready bank (hands disjoint)
bool g_readyAtomsHave[kMaxBones] = {};
std::atomic<bool> g_readyAllValid[2] = {};
uint64_t g_lagUntilMs = 0;
float g_lagMin[3], g_lagMax[3];
uint32_t g_lagSamples = 0;
// Past this the basis is broken (authored peaks measure <= 72 UU), or tRef is
// stale garbage - fall back to the anchor-pinned compose for the frame.
constexpr float kAnimTransMaxUu = 120.0f;

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
// s47: the reapply-gate staleness instrument (carried from s45b - "does pass-2
// ever replay a stale write across transitions/loads?"). Counters only; the
// 100 ms gate itself is untouched. A "gap" is a write older than 50 ms (~4-5
// frames at 90 Hz) still being replayed - the class the concern names.
uint64_t g_reapplyMaxAgeMs = 0;       // oldest write actually replayed
uint32_t g_reapplyAfterGapMs50 = 0;   // replays of a write older than 50 ms
uint32_t g_reapplySkippedStale = 0;   // hands the 100 ms gate refused while masked

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
        g_nameIdx[i] = nameIdx;
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
    // Membership by NAME (the live rig is name-flat - every RefSkeleton
    // parent reads 0, so there is no subtree to walk; the names carry the
    // structure instead, measured s45b):
    //   bone 0            PlayerHandsChest           - never driven
    //   1 / 22            L_Grip / R_Grip            - the anchors
    //   *armPalm/*Digit*  the HAND                   - grip cluster (rigid
    //                       with the controller; fingers must survive hide)
    //   *arm1/21/22, *_ArmParent  the ARM CHAIN      - follow/hide set
    // Side comes from the letter before "arm" ("...Larm...", "...RArm...",
    // "L_Arm...", case-insensitive).
    for (int i = 0; i < num; ++i) {
        if (i == g_grip[0]) {
            g_cluster[0][i] = true;
            continue;
        }
        if (i == g_grip[1]) {
            g_cluster[1][i] = true;
            continue;
        }
        char low[64];
        low[sizeof low - 1] = '\0';
        for (size_t k = 0; k < sizeof low - 1; ++k) {
            low[k] = static_cast<char>(tolower(static_cast<unsigned char>(g_names[i][k])));
            if (!g_names[i][k]) break;
        }
        const char* arm = strstr(low, "arm");
        if (!arm || arm == low) continue;
        const char side = (*(arm - 1) == '_' && arm - 1 > low) ? *(arm - 2) : *(arm - 1);
        const int h = side == 'l' ? 0 : (side == 'r' ? 1 : -1);
        if (h < 0) continue;
        if (strstr(low, "palm") || strstr(low, "digit")) {
            g_cluster[h][i] = true;
        } else {
            g_armSet[h][i] = true;
        }
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

// ---- the s46 stance instrument: bank snapshots ------------------------------
// Four slots of raw SpaceBases atoms, captured SYNCHRONOUSLY in the command
// handler (the drive may be OFF during a measurement - engine truth - so no
// deferral to a drive tick that would never run). Gated on the game thread
// (the pump the camera hook owns) and the rig identity gates; a diff refuses
// across rig generations (a level transition between snaps would silently
// compare different skeleton instances).
struct SnapSlot {
    bool valid = false;
    bool fromWritten = false; // s46: our written bank instead of SpaceBases
    uint8_t* bank = nullptr;
    int boneCount = 0;
    uint32_t resolves = 0;        // rig generation
    uint32_t midDrawRestamps = 0; // restamp-cadence context for the diff
    uint64_t stampMs = 0;
    float atoms[kMaxBones][8];
    bool torn[kMaxBones];
    bool have[kMaxBones]; // written-bank snaps: only masked bones are real
};
SnapSlot g_snap[4];

// Geodesic angle between two bone quats, degrees. abs() of the 4-D dot:
// Morpheme is free to restamp q as -q, and without it a sign flip reads as
// ~360 deg of phantom stance. Normalized first (the torn-read class exists).
float quat_angle_deg(const float* a, const float* b) {
    const float na = sqrtf(a[0] * a[0] + a[1] * a[1] + a[2] * a[2] + a[3] * a[3]);
    const float nb = sqrtf(b[0] * b[0] + b[1] * b[1] + b[2] * b[2] + b[3] * b[3]);
    if (na < 1e-6f || nb < 1e-6f) return 0.0f;
    float d = fabsf((a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3]) / (na * nb));
    if (d > 1.0f) d = 1.0f;
    return 2.0f * acosf(d) * 57.29578f;
}

bool cmd_snap(const char* rest) {
    int slot = -1;
    char which[16] = {};
    const int n = sscanf_s(rest, "%d %15s", &slot, which,
                           static_cast<unsigned>(sizeof which));
    if (n < 1 || slot < 0 || slot > 3) {
        BVR_LOG("[bsi] bones: usage - bsibones snap <0-3> [written] (default: the raw "
                "SpaceBases bank = engine truth at the poll point; `written` = OUR last "
                "composed writes, driven bones only)");
        return true;
    }
    const bool fromWritten = n == 2 && strncmp(which, "written", 7) == 0;
    const uint32_t camTid = camera::camera_tid();
    if (camTid == 0 || GetCurrentThreadId() != camTid) {
        BVR_LOG("[bsi] bones: snap REFUSED - not on the game thread (tid=%u camera=%u); "
                "the camera hook must own the pump",
                GetCurrentThreadId(), camTid);
        return true;
    }
    if (!g_comp || !rig_intact()) {
        BVR_LOG("[bsi] bones: snap REFUSED - rig not resolved/intact");
        return true;
    }
    SnapSlot& s = g_snap[slot];
    int torn = 0, have = 0;
    for (int i = 0; i < g_boneCount; ++i) {
        if (fromWritten) {
            s.have[i] = g_writtenMask[0][i] || g_writtenMask[1][i];
            memcpy(s.atoms[i], g_written[i], 32);
        } else {
            s.have[i] = true;
            memcpy(s.atoms[i], g_bank + static_cast<size_t>(i) * kAtom, 32);
        }
        have += s.have[i];
        const float* q = s.atoms[i];
        const float n2 = q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3];
        s.torn[i] = s.have[i] && !(n2 > 0.5f && n2 < 2.0f);
        torn += s.torn[i];
    }
    s.valid = true;
    s.fromWritten = fromWritten;
    s.bank = g_bank;
    s.boneCount = g_boneCount;
    s.resolves = g_resolves;
    s.midDrawRestamps = g_midDrawRestamps;
    s.stampMs = GetTickCount64();
    BVR_LOG("[bsi] bones: snap %d - %d bones from %s (gen %u, %d torn, %d covered)", slot,
            g_boneCount, fromWritten ? "OUR WRITTEN bank" : "the raw SpaceBases bank",
            g_resolves, torn, have);
    return true;
}

bool cmd_snap_diff(const char* rest) {
    int a = -1, b = -1;
    if (sscanf_s(rest, "%d %d", &a, &b) != 2 || a < 0 || a > 3 || b < 0 || b > 3) {
        BVR_LOG("[bsi] bones: usage - bsibones diff <slotA> <slotB>");
        return true;
    }
    const SnapSlot& A = g_snap[a];
    const SnapSlot& B = g_snap[b];
    if (!A.valid || !B.valid) {
        BVR_LOG("[bsi] bones: diff REFUSED - slot %d %s, slot %d %s", a,
                A.valid ? "ok" : "empty", b, B.valid ? "ok" : "empty");
        return true;
    }
    if (A.bank != B.bank || A.boneCount != B.boneCount || A.resolves != B.resolves) {
        BVR_LOG("[bsi] bones: diff REFUSED - different rig generations (bank %p/%p, "
                "bones %d/%d, gen %u/%u)",
                (void*)A.bank, (void*)B.bank, A.boneCount, B.boneCount, A.resolves,
                B.resolves);
        return true;
    }
    float ang[kMaxBones], dist[kMaxBones];
    int idx[kMaxBones];
    for (int i = 0; i < A.boneCount; ++i) {
        if (!A.have[i] || !B.have[i]) { // written-bank snaps: undriven bones
            ang[i] = 0.0f;
            dist[i] = 0.0f;
            idx[i] = i;
            continue;
        }
        ang[i] = quat_angle_deg(A.atoms[i], B.atoms[i]);
        const float dx = B.atoms[i][4] - A.atoms[i][4];
        const float dy = B.atoms[i][5] - A.atoms[i][5];
        const float dz = B.atoms[i][6] - A.atoms[i][6];
        dist[i] = sqrtf(dx * dx + dy * dy + dz * dz);
        idx[i] = i;
    }
    // insertion sort by angle, descending (43 bones)
    for (int i = 1; i < A.boneCount; ++i) {
        const int v = idx[i];
        int j = i - 1;
        while (j >= 0 && ang[idx[j]] < ang[v]) {
            idx[j + 1] = idx[j];
            --j;
        }
        idx[j + 1] = v;
    }
    int moved = 0;
    for (int i = 0; i < A.boneCount; ++i)
        if (ang[i] > 0.5f) ++moved;
    BVR_LOG("[bsi] bones: diff %d->%d dt=%llums restamps+%u | %d/%d bones moved >0.5deg "
            "(worst 15 below)",
            a, b, static_cast<unsigned long long>(B.stampMs - A.stampMs),
            B.midDrawRestamps - A.midDrawRestamps, moved, A.boneCount);
    for (int k = 0; k < 15 && k < A.boneCount; ++k) {
        const int i = idx[k];
        if (ang[i] <= 0.01f && dist[i] <= 0.01f) break; // noise floor - stop early
        BVR_LOG("[bsi] bones:   #%2d %-22s %7.2f deg  d=%6.2f UU  scale %.3f->%.3f%s%s%s%s%s",
                i, g_names[i], ang[i], dist[i], A.atoms[i][7], B.atoms[i][7],
                g_cluster[0][i] ? "  [L cluster]" : "",
                g_cluster[1][i] ? "  [R cluster]" : "",
                g_armSet[0][i] ? "  [L arm]" : "", g_armSet[1][i] ? "  [R arm]" : "",
                (A.torn[i] || B.torn[i]) ? "  [TORN]" : "");
    }
    return true;
}

// ---- s47: the animtrans evidence instrument ---------------------------------
// Peak ANCHOR (grip) travel over a timed window, sampled per pass-1 camera
// dispatch (~90 Hz - a game-cmd-timed snap would miss a sub-second reload or
// recoil peak). Raw-bank reads: the protocol runs with the drive OFF so the
// bank is engine truth; a driven hand is FLAGGED in the report rather than
// refused (a driven anchor is pinned to the controller and reads ~0 travel by
// construction, which would be our write, not the anim).
uint64_t g_travelUntilMs = 0; // 0 = idle
bool g_travelStartValid[2] = {};
float g_travelStartT[2][3];
float g_travelStartQ[2][4];
float g_travelPeakUu[2];
float g_travelPeakDeg[2];
bool g_travelSawDriven[2];
uint32_t g_travelSamples = 0;
// s51: the ALL-BONES variant (`bsibones travel all [secs]`). The s50 headset
// verdict said the fire jump survives with the DRIVEN anchors provably glued
// (A-B-A 95.58 -> 0.00), so the guilty motion lives on bones the drive does
// not own - this samples the whole bank through one fire window and names
// them. Anchors-only travel stays untouched: it is the established signature.
bool g_travelAll = false; // the armed window samples all bones, not the anchors
bool g_travelAllStartValid[kMaxBones];
float g_travelAllStartT[kMaxBones][3];
float g_travelAllStartQ[kMaxBones][4];
float g_travelAllPeakUu[kMaxBones];
float g_travelAllPeakDeg[kMaxBones];
bool g_travelAllSawDriven[kMaxBones];

bool cmd_travel(const char* rest) {
    while (*rest == ' ') ++rest;
    bool all = false;
    if (strncmp(rest, "all", 3) == 0) {
        all = true;
        rest += 3;
    }
    float secs = 5.0f;
    sscanf_s(rest, "%f", &secs);
    if (secs < 0.5f) secs = 0.5f;
    if (secs > 60.0f) secs = 60.0f;
    const uint32_t camTid = camera::camera_tid();
    if (camTid == 0 || GetCurrentThreadId() != camTid) {
        BVR_LOG("[bsi] bones: travel REFUSED - not on the game thread");
        return true;
    }
    if (!g_comp || !rig_intact()) {
        BVR_LOG("[bsi] bones: travel REFUSED - rig not resolved/intact");
        return true;
    }
    memset(g_travelStartValid, 0, sizeof g_travelStartValid);
    memset(g_travelPeakUu, 0, sizeof g_travelPeakUu);
    memset(g_travelPeakDeg, 0, sizeof g_travelPeakDeg);
    memset(g_travelSawDriven, 0, sizeof g_travelSawDriven);
    memset(g_travelAllStartValid, 0, sizeof g_travelAllStartValid);
    memset(g_travelAllPeakUu, 0, sizeof g_travelAllPeakUu);
    memset(g_travelAllPeakDeg, 0, sizeof g_travelAllPeakDeg);
    memset(g_travelAllSawDriven, 0, sizeof g_travelAllSawDriven);
    g_travelAll = all;
    g_travelSamples = 0;
    g_travelUntilMs = GetTickCount64() + static_cast<uint64_t>(secs * 1000.0f);
    BVR_LOG("[bsi] bones: travel window ARMED for %.1f s - sampling %s per dispatch",
            secs, all ? "ALL bones (per-bone peak table at expiry)" : "both anchors");
    return true;
}

} // namespace

void travel_tick() {
    if (g_travelUntilMs == 0) return;
    const uint64_t now = GetTickCount64();
    if (!g_comp || !rig_intact()) return; // dropped rig: hold; report at expiry
    if (now <= g_travelUntilMs) {
        if (g_travelAll) {
            for (int i = 0; i < g_boneCount && i < kMaxBones; ++i) {
                const float* atom = reinterpret_cast<const float*>(
                    g_bank + static_cast<size_t>(i) * kAtom);
                const float n2 = atom[0] * atom[0] + atom[1] * atom[1] +
                                 atom[2] * atom[2] + atom[3] * atom[3];
                if (!(n2 > 0.5f && n2 < 2.0f)) continue; // torn - skip the sample
                if (g_writtenMask[0][i] || g_writtenMask[1][i])
                    g_travelAllSawDriven[i] = true;
                if (!g_travelAllStartValid[i]) {
                    memcpy(g_travelAllStartQ[i], atom, 16);
                    memcpy(g_travelAllStartT[i], atom + 4, 12);
                    g_travelAllStartValid[i] = true;
                    continue;
                }
                const float dx = atom[4] - g_travelAllStartT[i][0];
                const float dy = atom[5] - g_travelAllStartT[i][1];
                const float dz = atom[6] - g_travelAllStartT[i][2];
                const float d = sqrtf(dx * dx + dy * dy + dz * dz);
                if (d > g_travelAllPeakUu[i]) g_travelAllPeakUu[i] = d;
                const float ang = quat_angle_deg(atom, g_travelAllStartQ[i]);
                if (ang > g_travelAllPeakDeg[i]) g_travelAllPeakDeg[i] = ang;
            }
            ++g_travelSamples;
            return;
        }
        for (int h = 0; h < 2; ++h) {
            const int a = g_grip[h];
            if (a < 0 || a >= g_boneCount) continue;
            const float* atom = reinterpret_cast<const float*>(
                g_bank + static_cast<size_t>(a) * kAtom);
            const float n2 = atom[0] * atom[0] + atom[1] * atom[1] + atom[2] * atom[2] +
                             atom[3] * atom[3];
            if (!(n2 > 0.5f && n2 < 2.0f)) continue; // torn - skip the sample
            if (g_writtenMask[h][a]) g_travelSawDriven[h] = true;
            if (!g_travelStartValid[h]) {
                memcpy(g_travelStartQ[h], atom, 16);
                memcpy(g_travelStartT[h], atom + 4, 12);
                g_travelStartValid[h] = true;
                continue;
            }
            const float dx = atom[4] - g_travelStartT[h][0];
            const float dy = atom[5] - g_travelStartT[h][1];
            const float dz = atom[6] - g_travelStartT[h][2];
            const float d = sqrtf(dx * dx + dy * dy + dz * dz);
            if (d > g_travelPeakUu[h]) g_travelPeakUu[h] = d;
            const float ang = quat_angle_deg(atom, g_travelStartQ[h]);
            if (ang > g_travelPeakDeg[h]) g_travelPeakDeg[h] = ang;
        }
        ++g_travelSamples;
        return;
    }
    g_travelUntilMs = 0;
    const FrameContext& fc = camera::frame_context();
    const float uuPerCm = fc.valid ? fc.worldScale * 0.01f : 0.0f;
    if (g_travelAll) {
        g_travelAll = false;
        // Descending by peak rotation, every bone listed - the whole point is
        // to name the movers OUTSIDE the driven mask, so nothing is filtered.
        int order[kMaxBones];
        const int n = g_boneCount < kMaxBones ? g_boneCount : kMaxBones;
        for (int i = 0; i < n; ++i) order[i] = i;
        for (int i = 1; i < n; ++i) {
            const int v = order[i];
            int j = i - 1;
            while (j >= 0 && g_travelAllPeakDeg[order[j]] < g_travelAllPeakDeg[v]) {
                order[j + 1] = order[j];
                --j;
            }
            order[j + 1] = v;
        }
        BVR_LOG("[bsi] bones: travel ALL - per-bone peaks over %u samples "
                "(deg / UU / cm; sorted worst-first)",
                g_travelSamples);
        for (int k = 0; k < n; ++k) {
            const int i = order[k];
            if (!g_travelAllStartValid[i]) {
                BVR_LOG("[bsi] bones:   %2d %-24s  NO SAMPLES", i, g_names[i]);
                continue;
            }
            const bool inSet = g_cluster[0][i] || g_cluster[1][i] || g_armSet[0][i] ||
                               g_armSet[1][i];
            BVR_LOG("[bsi] bones:   %2d %-24s  %7.2f deg  %7.2f UU (%.1f cm)%s%s%s%s%s%s",
                    i, g_names[i], g_travelAllPeakDeg[i], g_travelAllPeakUu[i],
                    uuPerCm > 0.0f ? g_travelAllPeakUu[i] / uuPerCm : -1.0f,
                    g_travelAllSawDriven[i] ? "  [driven]" : "",
                    g_cluster[0][i] ? "  [L cluster]" : "",
                    g_cluster[1][i] ? "  [R cluster]" : "",
                    g_armSet[0][i] ? "  [L arm]" : "", g_armSet[1][i] ? "  [R arm]" : "",
                    inSet ? "" : "  [UNDRIVEN]");
        }
        return;
    }
    for (int h = 0; h < 2; ++h) {
        if (!g_travelStartValid[h]) {
            BVR_LOG("[bsi] bones: travel %c - no samples", h ? 'R' : 'L');
            continue;
        }
        BVR_LOG("[bsi] bones: travel %c peak %.2f UU (%.1f cm) / %.2f deg over %u samples%s",
                h ? 'R' : 'L', g_travelPeakUu[h],
                uuPerCm > 0.0f ? g_travelPeakUu[h] / uuPerCm : -1.0f, g_travelPeakDeg[h],
                g_travelSamples,
                g_travelSawDriven[h] ? "  [DRIVEN - sampled OUR writes, not the anim]" : "");
    }
}

void* attachment() { return g_attach; }
void* component() { return g_comp; }

int side_bones(int hand, int32_t* outFname, const char** outName, int maxCount) {
    // s53: the hide module's per-side enumeration - the grip first (its
    // subtree carries the hand AND the holdable, s45b), then the arm-chain
    // bones. FName indices come from RefSkeleton's own entries - no pool
    // scans, so this is cadence-safe. Pointers into g_names stay valid until
    // the next drop; callers consume them immediately on the game thread.
    if (hand < 0 || hand > 1 || !g_comp || g_boneCount <= 0) return 0;
    int n = 0;
    if (g_grip[hand] >= 0 && n < maxCount) {
        outFname[n] = g_nameIdx[g_grip[hand]];
        if (outName) outName[n] = g_names[g_grip[hand]];
        ++n;
    }
    for (int i = 0; i < g_boneCount && n < maxCount; ++i) {
        if (!g_armSet[hand][i]) continue;
        outFname[n] = g_nameIdx[i];
        if (outName) outName[n] = g_names[i];
        ++n;
    }
    return n;
}

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
           int armsMode, bool animMode, float capDepthCm, const float wristDeg[3]) {
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
    // s48 camPin: the reference origin is the camera the drive WROTE this
    // dispatch, not the L2W translation - see the block comment at g_camPin.
    // (The L2W ROTATION is still the frame; rotational phase error at turn
    // speed is second-order and was not the reported defect.)
    const bool camPin = g_camPin.load(std::memory_order_relaxed) && fc.valid;
    const float w[3] = {target.loc.x - (camPin ? fc.writtenLocX : t[0]),
                        target.loc.y - (camPin ? fc.writtenLocY : t[1]),
                        target.loc.z - (camPin ? fc.writtenLocZ : t[2])};
    float ptc[3] = {r0[0] * w[0] + r0[1] * w[1] + r0[2] * w[2],
                    r1[0] * w[0] + r1[1] * w[1] + r1[2] * w[2],
                    r2[0] * w[0] + r2[1] * w[1] + r2[2] * w[2]};

    // s48: the lag probe - c0 = R^-1*(writtenCam - L2Wt), min/max per axis.
    if (g_lagUntilMs != 0 && fc.valid && hand == 1) {
        const uint64_t lagNow = GetTickCount64();
        if (lagNow > g_lagUntilMs) {
            g_lagUntilMs = 0;
            BVR_LOG("[bsi] bones: lag c0 over %u samples - x[%.2f..%.2f] y[%.2f..%.2f] "
                    "z[%.2f..%.2f] UU (span %.2f %.2f %.2f)",
                    g_lagSamples, g_lagMin[0], g_lagMax[0], g_lagMin[1], g_lagMax[1],
                    g_lagMin[2], g_lagMax[2], g_lagMax[0] - g_lagMin[0],
                    g_lagMax[1] - g_lagMin[1], g_lagMax[2] - g_lagMin[2]);
        } else {
            const float e[3] = {fc.writtenLocX - t[0], fc.writtenLocY - t[1],
                                fc.writtenLocZ - t[2]};
            const float el[3] = {r0[0] * e[0] + r0[1] * e[1] + r0[2] * e[2],
                                 r1[0] * e[0] + r1[1] * e[1] + r1[2] * e[2],
                                 r2[0] * e[0] + r2[1] * e[1] + r2[2] * e[2]};
            for (int k = 0; k < 3; ++k) {
                if (g_lagSamples == 0 || el[k] < g_lagMin[k]) g_lagMin[k] = el[k];
                if (g_lagSamples == 0 || el[k] > g_lagMax[k]) g_lagMax[k] = el[k];
            }
            ++g_lagSamples;
        }
    }
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

    // s48 rework (headset verdict: "the sliders move the ARM, not the wrist"):
    // the extra quat W now rides the HAND CLUSTER's compose - the hand (and
    // its holdable) tilts about the grip while the forearm keeps the plain
    // controller rotation, which is what reads as BENDING THE WRIST. (The s46
    // version put W on the arm chain instead: the same relative wrist angle,
    // but the elbow's long lever arm made it read as sweeping the whole arm.)
    // W is built about the identity ROTATOR-frame axes (X fwd, Y right, Z up -
    // the ue_rot_basis frame quat_from_rotator encodes), NOT xr_local_trim_quat
    // (XR axes - wrong frame here). Signs: +pitch up, +yaw right, +roll
    // clockwise; roll innermost, matching the trim-slider conventions. Purely
    // visual: aim, laser and the fire origin never see it.
    float qtcHand[4];
    const bool wristActive = wristDeg && (wristDeg[0] != 0.0f || wristDeg[1] != 0.0f ||
                                          wristDeg[2] != 0.0f);
    if (wristActive) {
        constexpr float kDegToRad = 3.14159265f / 180.0f;
        float qy[4], qp[4], qr[4], t[4], w[4];
        bvr::xrmath::quat_axis_angle(0.0f, 0.0f, 1.0f, wristDeg[1] * kDegToRad, qy);
        bvr::xrmath::quat_axis_angle(0.0f, -1.0f, 0.0f, wristDeg[0] * kDegToRad, qp);
        bvr::xrmath::quat_axis_angle(-1.0f, 0.0f, 0.0f, wristDeg[2] * kDegToRad, qr);
        quat_mul(qp, qr, t);
        quat_mul(qy, t, w);
        quat_mul(qtc, w, qtcHand);
    } else {
        memcpy(qtcHand, qtc, sizeof qtcHand);
    }

    // Adopt the engine pose for everything we are about to write.
    const bool useAnim = animMode;
    for (int i = 0; i < g_boneCount; ++i) {
        if (!g_cluster[hand][i] && !(armsMode != 0 && g_armSet[hand][i])) continue;
        adopt_one(hand, i);
    }
    const float(*src)[8] = useAnim ? g_anim : g_ref;
    if (!useAnim && !g_refValid) return false;

    // s46: a pending post-fire ready capture, per hand. The adopted anchor
    // quat IS the engine's ready pose right now (the shot reset the stance);
    // normalize and bank it. Re-captured on every shot - self-heals across
    // weapon switches. The window EXPIRES after 3 s: a hand that was not
    // driving through the window must not capture a later pose (the stance
    // re-onsets in minutes, and banking IT as "ready" would invert the glue).
    const uint64_t captureAt = g_captureAtMs[hand].load(std::memory_order_relaxed);
    if (captureAt != 0) {
        const uint64_t tick = GetTickCount64();
        if (tick >= captureAt + 3000) {
            g_captureAtMs[hand].store(0, std::memory_order_relaxed);
        } else if (tick >= captureAt && g_animValid[anchor]) {
            const float* q = g_anim[anchor];
            const float n2 = q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3];
            if (n2 > 0.5f && n2 < 2.0f) {
                const float inv = 1.0f / sqrtf(n2);
                for (int k = 0; k < 4; ++k) g_readyQuat[hand][k] = q[k] * inv;
                // s47: bank the ready anchor TRANSLATION too - the animtrans
                // pass-through's dp base (same capture, same self-healing).
                for (int k = 0; k < 3; ++k) g_readyTrans[hand][k] = q[4 + k];
                const bool first =
                    !g_readyValid[hand].exchange(true, std::memory_order_relaxed);
                // s51: bank the whole hand's ready articulation in the same
                // instant (both masks unconditionally - a later armsMode widen
                // must still find its bones banked when the anim was adopted).
                for (int i = 0; i < g_boneCount; ++i) {
                    if (!g_cluster[hand][i] && !g_armSet[hand][i]) continue;
                    if (!g_animValid[i]) continue;
                    const float* a = g_anim[i];
                    const float an2 =
                        a[0] * a[0] + a[1] * a[1] + a[2] * a[2] + a[3] * a[3];
                    if (!(an2 > 0.5f && an2 < 2.0f)) continue;
                    memcpy(g_readyAtoms[i], a, 32);
                    g_readyAtomsHave[i] = true;
                }
                g_readyAllValid[hand].store(g_readyAtomsHave[anchor],
                                            std::memory_order_relaxed);
                ++g_readyCaptures[hand];
                g_captureAtMs[hand].store(0, std::memory_order_relaxed);
                if (first)
                    BVR_LOG("[bsi] bones: ready pose captured for %c (post-fire anchor "
                            "quat %.3f %.3f %.3f %.3f) - the stance glue is live for "
                            "this hand",
                            hand ? 'R' : 'L', g_readyQuat[hand][0], g_readyQuat[hand][1],
                            g_readyQuat[hand][2], g_readyQuat[hand][3]);
            }
        }
    }

    // s46: the ready-pose glue - corr cancels the rigid stance component (see
    // the block comment at the top). Identity when off or before a capture.
    // s50: ALSO engaged fire-scoped (kFireGlueMs after a player shot) - the
    // fire anim's authored left-grip swing measured 95.58 deg through the
    // compose; the glue cancels it to 0.14 with the window keeping every
    // other anim (flourish, casts) untouched.
    const bool fireGlueActive =
        g_fireGlue.load(std::memory_order_relaxed) &&
        GetTickCount64() < g_fireGlueUntilMs.load(std::memory_order_relaxed);
    // s51: full-hand substitution (see the block comment at g_fireGlueFull).
    // corr and aT must come from the SAME source lane as the bones, or the
    // hand would wobble with the live anchor while its bones sit banked.
    const bool fireGlueFull = fireGlueActive &&
                              g_fireGlueFull.load(std::memory_order_relaxed) &&
                              g_readyAllValid[hand].load(std::memory_order_relaxed);
    float corr[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    if ((g_stanceKill.load(std::memory_order_relaxed) || fireGlueActive) &&
        g_readyValid[hand].load(std::memory_order_relaxed)) {
        const float* qa2 = fireGlueFull ? g_readyAtoms[anchor] : src[anchor];
        const float n2 = qa2[0] * qa2[0] + qa2[1] * qa2[1] + qa2[2] * qa2[2] + qa2[3] * qa2[3];
        if (n2 > 0.5f && n2 < 2.0f) {
            float cj[4];
            quat_conj(qa2, cj);
            const float inv = 1.0f / sqrtf(n2);
            for (int k = 0; k < 4; ++k) cj[k] *= inv;
            quat_mul(g_readyQuat[hand], cj, corr);
        }
    }
    // Fold the glue in as the INNERMOST factor (it corrects src before
    // anything else sees it): wq = qtc (x) [wrist] (x) corr (x) src, and the
    // same factor rotates the dp term, so the whole hand stays one rigid map.
    if (corr[3] != 1.0f || corr[0] != 0.0f || corr[1] != 0.0f || corr[2] != 0.0f) {
        float t2[4];
        quat_mul(qtc, corr, t2);
        memcpy(qtc, t2, sizeof t2);
        quat_mul(qtcHand, corr, t2);
        memcpy(qtcHand, t2, sizeof t2);
    }

    // Compose: q_i = qtc (x) srcQ_i ; p_i = ptc + qtc*(srcT_i - aT)*scale.
    // aT (the dp base) is the CURRENT anchor translation - which pins the
    // anchor to the controller and cancels all whole-hand travel. s47
    // animtrans: with the lever ON (and a ready capture to reference), the
    // base becomes the banked READY anchor translation instead, so authored
    // anchor travel (recoil kick, the reload rack) reaches the written pose
    // as a controller-relative offset. Anim mode only: the rigid snapshot has
    // no authored travel, and its resolve-time ref (the boot stance) against
    // a post-fire tRef would leak a permanent stance offset.
    const float* aT = &src[anchor][4];
    if (fireGlueFull) {
        aT = &g_readyAtoms[anchor][4]; // the banked lane's own anchor base
    } else if (useAnim && g_animTrans.load(std::memory_order_relaxed) &&
               g_readyValid[hand].load(std::memory_order_relaxed)) {
        const float tx = src[anchor][4] - g_readyTrans[hand][0];
        const float ty = src[anchor][5] - g_readyTrans[hand][1];
        const float tz = src[anchor][6] - g_readyTrans[hand][2];
        if (tx * tx + ty * ty + tz * tz <= kAnimTransMaxUu * kAnimTransMaxUu)
            aT = g_readyTrans[hand];
    }
    for (int i = 0; i < g_boneCount; ++i) {
        const bool inCluster = g_cluster[hand][i];
        const bool inArms = armsMode != 0 && g_armSet[hand][i];
        if (!inCluster && !inArms) continue;
        // s48: ONE hide mode (the headset A/B rejected the alternatives) -
        // collapse every arm bone to a point capDepthCm BEHIND the grip along
        // the controller's -forward, zero scale. Depth 0 = the old collapse-
        // at-grip; the depth is an F10 slider so the residual stretch is
        // tunable in the headset instead of baked here.
        const bool hideThis = inArms && armsMode == 2;
        const uint8_t kind = hideThis ? 2 : 0;
        // s51: the banked ready atom replaces the live source for the whole
        // hand during the fire window (unbanked bones keep the live lane
        // until the next capture re-banks - one-capture staleness at most).
        const float* sI =
            (fireGlueFull && g_readyAtomsHave[i]) ? g_readyAtoms[i] : src[i];
        float wq[8];
        if (kind == 2) {
            // The collapse is what degenerates the cross-boundary skin blend
            // to the wrist instead of stretching a web (BS2 s41, shape only).
            memcpy(wq, sI, 32);
            float cp[3] = {ptc[0], ptc[1], ptc[2]};
            if (capDepthCm > 0.01f) {
                const float back[3] = {-capDepthCm * fc.worldScale * 0.01f, 0.0f, 0.0f};
                float rb[3];
                bvr::xrmath::quat_rotate(qtc[0], qtc[1], qtc[2], qtc[3], back, rb);
                cp[0] += rb[0];
                cp[1] += rb[1];
                cp[2] += rb[2];
            }
            wq[4] = cp[0];
            wq[5] = cp[1];
            wq[6] = cp[2];
            wq[7] = 0.0f;
        } else {
            // s48: the CLUSTER takes qtcHand (the wrist-bend compose) in BOTH
            // the quat and the dp rotation - one rigid rotation about the
            // grip; using it in only one of the two would shear the hand
            // apart. The arm chain keeps the plain controller rotation.
            const float* q = inArms ? qtc : qtcHand;
            float dp[3] = {(sI[4] - aT[0]) * scale, (sI[5] - aT[1]) * scale,
                           (sI[6] - aT[2]) * scale};
            float rp[3];
            bvr::xrmath::quat_rotate(q[0], q[1], q[2], q[3], dp, rp);
            quat_mul(q, sI, wq); // sI[0..3] is the quat
            wq[4] = ptc[0] + rp[0];
            wq[5] = ptc[1] + rp[1];
            wq[6] = ptc[2] + rp[2];
            wq[7] = sI[7] * scale; // engine-adopted scale x hand scale
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

int reapply() {
    if (!g_comp || !g_bank) return 0;
    const uint64_t now = GetTickCount64();
    if (!rig_intact()) return 0;
    bool any = false;
    int differed = 0;
    for (int h = 0; h < 2; ++h) {
        const uint64_t age = now - g_writeStampMs[h];
        if (age > 100) { // stale - leave the engine alone
            // s47 instrument: was there anything the gate actually refused?
            for (int i = 0; i < g_boneCount; ++i)
                if (g_writtenMask[h][i]) {
                    ++g_reapplySkippedStale;
                    break;
                }
            continue;
        }
        bool wrote = false;
        for (int i = 0; i < g_boneCount; ++i) {
            if (!g_writtenMask[h][i]) continue;
            uint8_t* dst = g_bank + static_cast<size_t>(i) * kAtom;
            if (memcmp(dst, g_written[i], 32) != 0) ++differed;
            memcpy(dst, g_written[i], 32);
            wrote = true;
        }
        if (wrote) {
            any = true;
            if (age > g_reapplyMaxAgeMs) g_reapplyMaxAgeMs = age;
            if (age > 50) ++g_reapplyAfterGapMs50;
        }
    }
    if (any) ++g_reapplies;
    return differed;
}

void on_world_change(const char* why) {
    release(why, -1);
    drop(why);
}

void note_player_fire() {
    // Game thread (the fire seam's detour). The engine resets the stance on a
    // shot; 1.2 s later the pose has settled at READY (recoil is ~0.3-0.5 s)
    // and the stance re-onset is minutes away - the capture window.
    // s50: the same event opens the fire-glue window (see g_fireGlue).
    g_fireGlueUntilMs.store(GetTickCount64() + kFireGlueMs, std::memory_order_relaxed);
    const uint64_t at = GetTickCount64() + 1200;
    g_captureAtMs[0].store(at, std::memory_order_relaxed);
    g_captureAtMs[1].store(at, std::memory_order_relaxed);
}

bool stance_kill() { return g_stanceKill.load(std::memory_order_relaxed); }
void set_stance_kill(bool on) { g_stanceKill.store(on, std::memory_order_relaxed); }
bool anim_trans() { return g_animTrans.load(std::memory_order_relaxed); }
void set_anim_trans(bool on) { g_animTrans.store(on, std::memory_order_relaxed); }

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
    if (args && strncmp(args, "snap", 4) == 0) return cmd_snap(args + 4);
    if (args && strncmp(args, "diff", 4) == 0) return cmd_snap_diff(args + 4);
    if (args && strncmp(args, "travel", 6) == 0) return cmd_travel(args + 6);
    if (args && strncmp(args, "lag", 3) == 0) {
        float secs = 5.0f;
        sscanf_s(args + 3, "%f", &secs);
        if (secs < 1.0f) secs = 1.0f;
        if (secs > 30.0f) secs = 30.0f;
        g_lagSamples = 0;
        g_lagUntilMs = GetTickCount64() + static_cast<uint64_t>(secs * 1000.0f);
        BVR_LOG("[bsi] bones: lag probe ARMED for %.1f s (needs the drive ON; reports "
                "writtenCam-vs-L2W offset in the component frame)",
                secs);
        return true;
    }
    if (args && strncmp(args, "fireglue", 8) == 0) {
        const char* rest = args + 8;
        while (*rest == ' ') ++rest;
        if (strncmp(rest, "on", 2) == 0) g_fireGlue.store(true, std::memory_order_relaxed);
        else if (strncmp(rest, "off", 3) == 0)
            g_fireGlue.store(false, std::memory_order_relaxed);
        else if (strncmp(rest, "full", 4) == 0) {
            g_fireGlue.store(true, std::memory_order_relaxed);
            g_fireGlueFull.store(true, std::memory_order_relaxed);
        } else if (strncmp(rest, "anchor", 6) == 0) {
            // the s50 anchor-only behavior - the in-headset bisect for s51
            g_fireGlue.store(true, std::memory_order_relaxed);
            g_fireGlueFull.store(false, std::memory_order_relaxed);
        }
        BVR_LOG("[bsi] bones: fireglue %s mode=%s (window %llu ms per player shot; "
                "full = whole-hand ready substitution, anchor = s50 corr-only; "
                "ready captures L=%s R=%s, full banks L=%s R=%s)",
                g_fireGlue.load() ? "ON" : "off",
                g_fireGlueFull.load() ? "FULL" : "anchor",
                static_cast<unsigned long long>(kFireGlueMs),
                g_readyValid[0].load() ? "ok" : "-", g_readyValid[1].load() ? "ok" : "-",
                g_readyAllValid[0].load() ? "ok" : "-",
                g_readyAllValid[1].load() ? "ok" : "-");
        return true;
    }
    if (args && strncmp(args, "campin", 6) == 0) {
        const char* rest = args + 6;
        while (*rest == ' ') ++rest;
        if (strncmp(rest, "on", 2) == 0) g_camPin.store(true, std::memory_order_relaxed);
        else if (strncmp(rest, "off", 3) == 0) g_camPin.store(false, std::memory_order_relaxed);
        BVR_LOG("[bsi] bones: campin %s (hand composed relative to the written camera - "
                "the locomotion fix)",
                g_camPin.load() ? "ON" : "off");
        return true;
    }
    if (args && strncmp(args, "glue", 4) == 0) {
        const char* rest = args + 4;
        while (*rest == ' ') ++rest;
        if (strncmp(rest, "on", 2) == 0) set_stance_kill(true);
        else if (strncmp(rest, "off", 3) == 0) set_stance_kill(false);
        else if (strncmp(rest, "capture", 7) == 0) {
            // Manual capture for pacifist scenes: treat "now" as post-fire.
            const uint64_t at = GetTickCount64();
            g_captureAtMs[0].store(at, std::memory_order_relaxed);
            g_captureAtMs[1].store(at, std::memory_order_relaxed);
            BVR_LOG("[bsi] bones: glue capture requested - the CURRENT pose becomes the "
                    "ready reference (make sure the stance is not held right now; firing "
                    "once is the reliable reset)");
            return true;
        }
        BVR_LOG("[bsi] bones: glue %s | ready L=%s(%u) R=%s(%u) | bsibones glue "
                "on|off|capture",
                g_stanceKill.load() ? "ON" : "off",
                g_readyValid[0].load() ? "captured" : "-", g_readyCaptures[0],
                g_readyValid[1].load() ? "captured" : "-", g_readyCaptures[1]);
        return true;
    }
    // status (default)
    BVR_LOG("[bsi] bones: rig %s comp=%p bones=%d resolves=%u fails=%u | drives L=%u "
            "R=%u adopts L=%u R=%u midDrawRestamps=%u reapplies=%u gateRefusals=%u | "
            "reapply maxAge=%llums afterGap50=%u skippedStale=%u",
            g_comp ? "RESOLVED" : "not resolved", (void*)g_comp, g_boneCount, g_resolves,
            g_resolveFails, g_drives[0], g_drives[1], g_adopts[0], g_adopts[1],
            g_midDrawRestamps, g_reapplies, g_gateRefusals,
            static_cast<unsigned long long>(g_reapplyMaxAgeMs), g_reapplyAfterGapMs50,
            g_reapplySkippedStale);
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
    // s48: the stance fix is the ROOT KILL (block the trigger); the s46 glue
    // below is demoted to a bisect lever, default off.
    fidget::draw_debug_ui();
    bool glue = g_stanceKill.load(std::memory_order_relaxed);
    if (ImGui::Checkbox("stance glue (s46 fallback, off - superseded by the root kill)",
                        &glue))
        set_stance_kill(glue);
    ImGui::SameLine();
    ImGui::Text("ready L:%s R:%s", g_readyValid[0].load(std::memory_order_relaxed) ? "ok" : "-",
                g_readyValid[1].load(std::memory_order_relaxed) ? "ok" : "-");
    if (ImGui::Button("capture ready pose NOW (fire a shot first)")) {
        const uint64_t at = GetTickCount64();
        g_captureAtMs[0].store(at, std::memory_order_relaxed);
        g_captureAtMs[1].store(at, std::memory_order_relaxed);
    }
    // s47: the animtrans A/B lever (measured travel: reload moves R 14 cm,
    // L 48 cm - all discarded when off). Needs the ready capture; unpersisted.
    bool at2 = g_animTrans.load(std::memory_order_relaxed);
    if (ImGui::Checkbox("authored anchor travel (animtrans, off = hand pinned)", &at2))
        set_anim_trans(at2);
    // s51: the fire-swing levers, in-headset A/B without alt-tabbing.
    bool fg = g_fireGlue.load(std::memory_order_relaxed);
    if (ImGui::Checkbox("FIRE GLUE (window around each shot)", &fg))
        g_fireGlue.store(fg, std::memory_order_relaxed);
    ImGui::SameLine();
    bool fgFull = g_fireGlueFull.load(std::memory_order_relaxed);
    if (ImGui::Checkbox("full-hand (off = s50 anchor-only)", &fgFull))
        g_fireGlueFull.store(fgFull, std::memory_order_relaxed);
}

} // namespace bvr::bsi::bones
