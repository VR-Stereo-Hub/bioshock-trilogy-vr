// s87: the hands through the skin palette - see palette.h and patterns.h "THE SKIN PALETTE".
#include "game/bioshock1r/palette.h"

#include "core/util/log.h"
#include "game/bioshock1r/arm_ik.h"
#include "game/bioshock1r/patterns.h"

#include <windows.h>

#include <MinHook.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <mutex>

namespace bvr::b1r::palette {
namespace {

using palette_math::apply_affine;
using palette_math::apply_sim;
using palette_math::collapse;
using palette_math::entry_inverse_point;
using palette_math::entry_point;
using palette_math::sim_inv;
using palette_math::sim_mul;

// x' = palmAt + s R (x - pivot) on one palette entry: the hand's correction (palette_math.h).
void compose(const HandXform& h, float* m) { apply_sim(palette_math::hand_sim(h.q, h.s, h.pivot, h.t), m); }

bool readable(const void* p, size_t n); // below

using GatherFn = int(__thiscall*)(void* self, void* inst, int lod, void* out, int a5);
GatherFn g_orig = nullptr;
bool g_installed = false;
std::atomic<bool> g_enabled{true};

std::mutex g_mutex; // the drive publishes on the game thread; the gather may run on the render thread
struct Published {
    void* instance = nullptr;
    HandXform hand[2];
    WeaponXform weapon;
    bool hideArms = false;
    uint64_t ms = 0;
};
Published g_pub;

std::atomic<uint32_t> g_calls{0}, g_composed{0}, g_refused{0};
std::atomic<void*> g_linkActor{nullptr}, g_linkSkel{nullptr};
std::atomic<uint32_t> g_weaponComposed{0};

// ---- s88: the rigid anchor (VR-183) and the rigid wrist (VR-184) ------------------------
std::atomic<bool> g_rigidAnchor{true}, g_rigidWrist{true};
std::mutex g_anchorMutex;
bool g_palmLocalOk[2] = {false, false};
float g_palmLocal[2][3] = {};

// The hands' skinned vertices with every HAND vertex's forearm influences pointed at the hand
// bone (Dishonored mesh_split.cpp VR-184, in the mod's own copy; the game's array is not
// touched). Built once per (LOD, source array); swapped in for the length of a gather call,
// like the palette. Persistent: a skinning task queued to a worker keeps reading it.
struct WristCopy {
    const void* lod = nullptr;
    const void* src = nullptr;
    int count = 0;
    uint8_t* data = nullptr;
    int repointed = 0, influences = 0;
};
// A small cache, not one buffer: a skinning task the dispatch queued to a worker keeps reading
// the copy it was handed after the gather returns, so a copy is never freed the moment another
// LOD is skinned - only the OLDEST of kWristCopies is reused, frames after its last use.
constexpr int kWristCopies = 8;
WristCopy g_wristCache[kWristCopies];
int g_wristNext = 0;
const WristCopy* find_wrist(const void* lod, const void* src, int count) {
    for (const WristCopy& w : g_wristCache)
        if (w.data && w.lod == lod && w.src == src && w.count == count) return &w;
    return nullptr;
}

// VR-183, measured once per hand: the palm bones' heads in bind space (P_j^-1 of their posed
// heads), their mean carried by the WRIST's palette entry, then into the wrist bone's frame.
void measure_anchor(int h, const HandXform& x, const float* pal, int count) {
    {
        std::lock_guard<std::mutex> lock(g_anchorMutex);
        if (g_palmLocalOk[h]) return;
    }
    const int* pb = h == 1 ? patterns::kBoneRPalmBones : patterns::kBoneLPalmBones;
    const float* entries[5];
    for (int j = 0; j < 5; ++j) {
        if (pb[j] >= count) return;
        entries[j] = pal + pb[j] * 16;
    }
    float out[3];
    if (!palette_math::rigid_palm(entries, x.palmPoseP, pal + x.wristBone * 16, x.poseP, x.poseQ, out)) return;
    const arm_ik::Vec local{out[0], out[1], out[2]};
    std::lock_guard<std::mutex> lock(g_anchorMutex);
    g_palmLocal[h][0] = local.x;
    g_palmLocal[h][1] = local.y;
    g_palmLocal[h][2] = local.z;
    g_palmLocalOk[h] = true;
    BVR_LOG("[palette] anchor: the %s palm, rigid with the hand bone (Dishonored VR-183), at %.2f %.2f %.2f in "
            "the wrist's frame (the bind pose's palm centre)",
            h ? "right" : "left", local.x, local.y, local.z);
}

// VR-184 on the hands' LOD. Skinned vertex: 64 bytes, bone slots (LOD palette indices) at +56,
// weights (/255) at +60 (tools\ida\rs10: the skinner reads them at those offsets).
const WristCopy* build_wrist_copy(const uint8_t* lod, const uint8_t* src, int count) {
    if (const WristCopy* hit = find_wrist(lod, src, count)) return hit;
    const uint16_t* map = *reinterpret_cast<uint16_t* const*>(lod + patterns::kLodBoneMapOffset);
    const int mapN = *reinterpret_cast<const int*>(lod + patterns::kLodBoneMapCountOffset);
    if (!map || mapN <= 0 || mapN > 256 || !readable(map, mapN * 2) || !readable(src, static_cast<size_t>(count) * patterns::kSkinnedVertexStride))
        return nullptr;
    int wristSlot[2] = {-1, -1};
    for (int i = 0; i < mapN; ++i) {
        if (map[i] == patterns::kBoneLWrist) wristSlot[0] = i;
        if (map[i] == patterns::kBoneRWrist) wristSlot[1] = i;
    }
    if (wristSlot[0] < 0 || wristSlot[1] < 0) {
        BVR_LOG("[palette] wrist: the LOD's bone map does not hold both wrists - the rigid wrist is off for it");
        return nullptr;
    }
    auto side_of = [](int bone, bool* sleeve) -> int {
        *sleeve = false;
        for (int h = 0; h < 2; ++h) {
            const int first = h ? patterns::kBoneRClusterFirst : patterns::kBoneLClusterFirst;
            const int last = h ? patterns::kBoneRClusterLast : patterns::kBoneLClusterLast;
            if (bone >= first && bone <= last) return h;
            const int* sl = h ? patterns::kBoneRSleeve : patterns::kBoneLSleeve;
            for (int j = 0; j < 5; ++j)
                if (bone == sl[j]) {
                    *sleeve = true;
                    return h;
                }
        }
        return -1;
    };
    WristCopy& g_wrist = g_wristCache[g_wristNext];
    g_wristNext = (g_wristNext + 1) % kWristCopies;
    delete[] g_wrist.data;
    g_wrist = WristCopy{};
    g_wrist.data = new uint8_t[static_cast<size_t>(count) * patterns::kSkinnedVertexStride];
    memcpy(g_wrist.data, src, static_cast<size_t>(count) * patterns::kSkinnedVertexStride);
    int rep = 0, infl = 0;
    for (int v = 0; v < count; ++v) {
        uint8_t* vx = g_wrist.data + static_cast<size_t>(v) * patterns::kSkinnedVertexStride;
        int handW[2] = {0, 0};
        for (int k = 0; k < 4; ++k) {
            const int slot = vx[patterns::kSkinnedVertexBonesOffset + k];
            if (slot >= mapN || !vx[patterns::kSkinnedVertexWeightsOffset + k]) continue;
            bool sleeve = false;
            const int h = side_of(map[slot], &sleeve);
            if (h >= 0 && !sleeve) handW[h] += vx[patterns::kSkinnedVertexWeightsOffset + k];
        }
        // A HAND vertex: at least half its weight on one hand's cluster (the vertex-level twin
        // of Dishonored's hand-class triangles). Its influences on that side's arm bones move to
        // that side's hand bone, weights kept, finger bones untouched.
        for (int h = 0; h < 2; ++h) {
            if (handW[h] < 128) continue;
            bool any = false;
            for (int k = 0; k < 4; ++k) {
                const int slot = vx[patterns::kSkinnedVertexBonesOffset + k];
                if (slot >= mapN || !vx[patterns::kSkinnedVertexWeightsOffset + k]) continue;
                bool sleeve = false;
                if (side_of(map[slot], &sleeve) == h && sleeve) {
                    vx[patterns::kSkinnedVertexBonesOffset + k] = static_cast<uint8_t>(wristSlot[h]);
                    ++infl;
                    any = true;
                }
            }
            if (any) ++rep;
        }
    }
    g_wrist.lod = lod;
    g_wrist.src = src;
    g_wrist.count = count;
    g_wrist.repointed = rep;
    g_wrist.influences = infl;
    BVR_LOG("[palette] wrist: the hand's cuff is rigid with the hand (Dishonored VR-184): %d of %d skinned "
            "vertices had %d forearm influences moved to the hand bone; the fingers still animate",
            rep, count, infl);
    return &g_wrist;
}
// Census: the distinct owner actors the gather has skinned, with their palette sizes. Recorded
// here (any thread), named on the game thread by log_status.
constexpr int kCensus = 24;
std::atomic<void*> g_censusOwner[kCensus];
std::atomic<int> g_censusBones[kCensus];
std::atomic<int> g_censusN{0};
bool g_censusNamed[kCensus] = {};

void census_note(void* owner, int bones) {
    const int n = g_censusN.load(std::memory_order_relaxed);
    for (int i = 0; i < n; ++i)
        if (g_censusOwner[i].load(std::memory_order_relaxed) == owner) return;
    if (n >= kCensus) return;
    g_censusOwner[n].store(owner, std::memory_order_relaxed);
    g_censusBones[n].store(bones, std::memory_order_relaxed);
    g_censusN.store(n + 1, std::memory_order_release);
}

std::atomic<bool> g_probe{false};

// The rotation whose columns are x, y and x cross y (Shepperd's method).
arm_ik::Quat quat_from_axes(arm_ik::Vec x, arm_ik::Vec y) {
    const arm_ik::Vec z{x.y * y.z - x.z * y.y, x.z * y.x - x.x * y.z, x.x * y.y - x.y * y.x};
    const float m00 = x.x, m10 = x.y, m20 = x.z, m01 = y.x, m11 = y.y, m21 = y.z, m02 = z.x, m12 = z.y, m22 = z.z;
    const float tr = m00 + m11 + m22;
    arm_ik::Quat q;
    if (tr > 0) {
        const float s = sqrtf(tr + 1.0f) * 2.0f;
        q.v[3] = 0.25f * s; q.v[0] = (m21 - m12) / s; q.v[1] = (m02 - m20) / s; q.v[2] = (m10 - m01) / s;
    } else if (m00 > m11 && m00 > m22) {
        const float s = sqrtf(1.0f + m00 - m11 - m22) * 2.0f;
        q.v[3] = (m21 - m12) / s; q.v[0] = 0.25f * s; q.v[1] = (m01 + m10) / s; q.v[2] = (m02 + m20) / s;
    } else if (m11 > m22) {
        const float s = sqrtf(1.0f + m11 - m00 - m22) * 2.0f;
        q.v[3] = (m02 - m20) / s; q.v[0] = (m01 + m10) / s; q.v[1] = 0.25f * s; q.v[2] = (m12 + m21) / s;
    } else {
        const float s = sqrtf(1.0f + m22 - m00 - m11) * 2.0f;
        q.v[3] = (m10 - m01) / s; q.v[0] = (m02 + m20) / s; q.v[1] = (m12 + m21) / s; q.v[2] = 0.25f * s;
    }
    return arm_ik::normalized(q);
}
struct ProbeState {
    bool have = false;
    float t0[3] = {};
    arm_ik::Quat q0;
    float maxT = 0, maxDeg = 0;
};
ProbeState g_probeState[2];
uint64_t g_probeLog = 0;

// Column form of a row-vector palette entry: R (3x3, column c = row c of the entry), t.
// Then IB = inverse(pose) * M: R_IB = R_pose^T R_M, t_IB = R_pose^T (t_M - pose.p).
// Rotation part reduced to a quaternion through its columns (the rig's palette is a scaled
// rotation; the scale is divided out by the column length).
void probe_wrist(int h, const HandXform& x, const float* m) {
    const arm_ik::Quat pq = arm_ik::normalized(arm_ik::quat(x.poseQ));
    const arm_ik::Quat pinv = arm_ik::conj(pq);
    const arm_ik::Vec d{m[12] - x.poseP[0], m[13] - x.poseP[1], m[14] - x.poseP[2]};
    const arm_ik::Vec t = arm_ik::rotate(pinv, d);
    // the palette's own rotation, from its first two rows (images of bind X and Y)
    arm_ik::Vec ax{m[0], m[1], m[2]}, ay{m[4], m[5], m[6]};
    const float sx = arm_ik::length(ax);
    if (!(sx > 1e-6f)) return;
    ax = ax * (1.0f / sx);
    ay = ay * (1.0f / arm_ik::length(ay));
    const arm_ik::Vec ibx = arm_ik::rotate(pinv, ax), iby = arm_ik::rotate(pinv, ay);
    ProbeState& ps = g_probeState[h];
    if (!ps.have) {
        ps = ProbeState{};
        ps.have = true;
        ps.t0[0] = t.x;
        ps.t0[1] = t.y;
        ps.t0[2] = t.z;
        ps.q0 = quat_from_axes(ibx, iby);
        return;
    }
    const float dt = sqrtf((t.x - ps.t0[0]) * (t.x - ps.t0[0]) + (t.y - ps.t0[1]) * (t.y - ps.t0[1]) +
                           (t.z - ps.t0[2]) * (t.z - ps.t0[2]));
    const arm_ik::Quat qn = quat_from_axes(ibx, iby);
    const float c = fabsf(qn.v[0] * ps.q0.v[0] + qn.v[1] * ps.q0.v[1] + qn.v[2] * ps.q0.v[2] + qn.v[3] * ps.q0.v[3]);
    const float deg = 2.0f * acosf(c > 1.0f ? 1.0f : c) * (180.0f / 3.14159265f);
    if (dt > ps.maxT) ps.maxT = dt;
    if (deg > ps.maxDeg) ps.maxDeg = deg;
    const uint64_t now = GetTickCount64();
    if (h == 1 && now - g_probeLog >= 2000) {
        g_probeLog = now;
        BVR_LOG("[palette] PROBE inverse(pose)*palette at the wrists: L t=(%.2f %.2f %.2f) drift %.3f UU / %.2f deg, "
                "R t=(%.2f %.2f %.2f) drift %.3f UU / %.2f deg, palette scale %.3f - constant means the palette "
                "is in the pose's component space",
                g_probeState[0].t0[0], g_probeState[0].t0[1], g_probeState[0].t0[2], g_probeState[0].maxT,
                g_probeState[0].maxDeg, ps.t0[0], ps.t0[1], ps.t0[2], ps.maxT, ps.maxDeg, sx);
    }
}
std::atomic<int> g_lastCount{0};
const char* g_lastWhy = "never called";

// 47 bones x 64 bytes; aligned for the engine's float loads.
alignas(16) float g_copy[256 * 16];

bool readable(const void* p, size_t n) {
    __try {
        volatile const uint8_t* b = static_cast<const uint8_t*>(p);
        for (size_t i = 0; i < n; i += 4096) (void)b[i];
        (void)b[n - 1];
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The original with the copy in place, restored however the call ends. Its own function:
// MSVC allows no __try beside objects that need unwinding (the detour holds a lock_guard).
int call_with_copy(void* self, void* inst, int lod, void* out, int a5, float** slot, float* engine,
                   void** vslot = nullptr, void* vengine = nullptr, void* vcopy = nullptr) {
    *slot = g_copy;
    if (vslot) *vslot = vcopy;
    int r = 0;
    __try {
        r = g_orig(self, inst, lod, out, a5);
    } __finally {
        *slot = engine;
        if (vslot) *vslot = vengine;
    }
    return r;
}

int __fastcall gather_detour(void* self, void* /*edx*/, void* inst, int lod, void* out, int a5) {
    g_calls.fetch_add(1, std::memory_order_relaxed);
    Published p;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        p = g_pub;
    }
    // Diagnostic (s87, first run: 0 matches in 14,400 calls): every 2 s, one instance the
    // gather saw that holds a 47-bone palette, beside the one the drive published.
    if (inst && (!readable(static_cast<uint8_t*>(inst) + patterns::kMeshInstOwnerOffset, 4) ||
                 *reinterpret_cast<void**>(static_cast<uint8_t*>(inst) + patterns::kMeshInstOwnerOffset) != p.instance) &&
        readable(static_cast<uint8_t*>(inst) + patterns::kMeshInstPaletteCountOffset, 4) &&
        *reinterpret_cast<int*>(static_cast<uint8_t*>(inst) + patterns::kMeshInstPaletteCountOffset) ==
            patterns::kHandsRigBoneCount) {
        static uint64_t s_diag = 0;
        const uint64_t now = GetTickCount64();
        if (now - s_diag >= 2000) {
            s_diag = now;
            void* vt = nullptr;
            void* w4 = nullptr;
            if (readable(inst, 8)) {
                vt = *reinterpret_cast<void**>(inst);
                w4 = *reinterpret_cast<void**>(static_cast<uint8_t*>(inst) + 4);
            }
            char hits[256] = {};
            int at = 0;
            void* la = g_linkActor.load(std::memory_order_relaxed);
            void* ls = g_linkSkel.load(std::memory_order_relaxed);
            if (readable(inst, 0x400))
                for (uint32_t off = 0; off < 0x400 && at < 200; off += 4) {
                    void* v = *reinterpret_cast<void**>(static_cast<uint8_t*>(inst) + off);
                    if (v && v == la) at += _snprintf_s(hits + at, sizeof hits - at, _TRUNCATE, " actor@+0x%X", off);
                    if (v && v == ls) at += _snprintf_s(hits + at, sizeof hits - at, _TRUNCATE, " skel@+0x%X", off);
                }
            BVR_LOG("[palette] DIAG: the gather skinned a 47-bone instance %p (vtable %p, +4 %p) that is not "
                    "the published %p; fields pointing at the hands actor %p / its SkeletonInstance %p:%s",
                    inst, vt, w4, p.instance, la, ls, at ? hits : " NONE");
        }
    }
    void* owner = nullptr;
    if (inst && readable(static_cast<uint8_t*>(inst) + patterns::kMeshInstOwnerOffset, 4))
        owner = *reinterpret_cast<void**>(static_cast<uint8_t*>(inst) + patterns::kMeshInstOwnerOffset);
    if (owner && readable(static_cast<uint8_t*>(inst) + patterns::kMeshInstPaletteCountOffset, 4))
        census_note(owner, *reinterpret_cast<int*>(static_cast<uint8_t*>(inst) + patterns::kMeshInstPaletteCountOffset));

    // ---- the held weapon (Dishonored 1.5): the hand's world correction, in the weapon's space
    if (g_enabled.load(std::memory_order_relaxed) && owner && p.weapon.valid && owner == p.weapon.actor &&
        GetTickCount64() - p.ms <= 250) {
        uint8_t* b = static_cast<uint8_t*>(inst);
        if (readable(b + patterns::kMeshInstPaletteOffset, 8)) {
            float** wslot = reinterpret_cast<float**>(b + patterns::kMeshInstPaletteOffset);
            float* wengine = *wslot;
            const int wn = *reinterpret_cast<int*>(b + patterns::kMeshInstPaletteCountOffset);
            if (wengine && wn > 0 && wn <= 256 && readable(wengine, static_cast<size_t>(wn) * 64)) {
                const Sim g = sim_mul(sim_inv(p.weapon.lWeapon), sim_mul(p.weapon.dWorld, p.weapon.lWeapon));
                memcpy(g_copy, wengine, static_cast<size_t>(wn) * 64);
                for (int i = 0; i < wn; ++i) apply_sim(g, g_copy + i * 16);
                const int r = call_with_copy(self, inst, lod, out, a5, wslot, wengine);
                g_weaponComposed.fetch_add(1, std::memory_order_relaxed);
                return r;
            }
        }
    }
    if (!g_enabled.load(std::memory_order_relaxed) || !inst || !owner || owner != p.instance ||
        (!p.hand[0].valid && !p.hand[1].valid) || GetTickCount64() - p.ms > 250)
        return g_orig(self, inst, lod, out, a5);

    uint8_t* base = static_cast<uint8_t*>(inst);
    float** slot = reinterpret_cast<float**>(base + patterns::kMeshInstPaletteOffset);
    int count = 0;
    float* engine = nullptr;
    if (!readable(base + patterns::kMeshInstPaletteOffset, 8)) {
        g_lastWhy = "the instance's palette fields are unreadable";
        g_refused.fetch_add(1, std::memory_order_relaxed);
        return g_orig(self, inst, lod, out, a5);
    }
    engine = *slot;
    count = *reinterpret_cast<int*>(base + patterns::kMeshInstPaletteCountOffset);
    g_lastCount.store(count, std::memory_order_relaxed);
    // The palette is indexed by SKELETON bone (the gather maps LOD slots through the bone map
    // into it), so the rig's indices apply only if it holds exactly the rig's bones.
    if (count != patterns::kHandsRigBoneCount || !engine ||
        !readable(engine, static_cast<size_t>(count) * patterns::kSkinnedVertexStride)) {
        g_lastWhy = count != patterns::kHandsRigBoneCount
                        ? "the palette does not hold the 47 hand bones - the rig indices would be wrong"
                        : "the palette array is unreadable";
        g_refused.fetch_add(1, std::memory_order_relaxed);
        return g_orig(self, inst, lod, out, a5);
    }
    memcpy(g_copy, engine, static_cast<size_t>(count) * patterns::kSkinnedVertexStride);
    for (int h = 0; h < 2; ++h) {
        const HandXform& x = p.hand[h];
        if (!x.valid) continue;
        if (g_probe.load(std::memory_order_relaxed) && x.wristBone >= 0 && x.wristBone < count)
            probe_wrist(h, x, g_copy + x.wristBone * 16);
        if (x.wristBone >= 0 && x.wristBone < count) measure_anchor(h, x, g_copy, count);
        const int first = h == 1 ? patterns::kBoneRClusterFirst : patterns::kBoneLClusterFirst;
        const int last = h == 1 ? patterns::kBoneRClusterLast : patterns::kBoneLClusterLast;
        for (int i = first; i <= last && i < count; ++i) compose(x, g_copy + i * 16);
        const int* sl = h == 1 ? patterns::kBoneRSleeve : patterns::kBoneLSleeve;
        if (x.armValid) {
            // s88: the IK arm (Dishonored arm_ik_draw.inc): each sleeve matrix carried from the
            // game's bone to the solved one. No hierarchy: each is set on its own.
            for (int j = 0; j < 5; ++j)
                if (sl[j] < count) apply_affine(x.arm[j], g_copy + sl[j] * 16);
        } else if (p.hideArms) {
            for (int j = 0; j < 5; ++j)
                if (sl[j] < count) collapse(x.wrist, g_copy + sl[j] * 16);
        }
    }
    // s88 (VR-184): the rigid wrist, on the LOD this call skins.
    void** vslot = nullptr;
    void* vengine = nullptr;
    void* vcopy = nullptr;
    if (g_rigidWrist.load(std::memory_order_relaxed)) {
        uint8_t* mesh = nullptr;
        if (readable(base + patterns::kMeshInstMeshOffset, 4))
            mesh = *reinterpret_cast<uint8_t**>(base + patterns::kMeshInstMeshOffset);
        if (mesh && readable(mesh + patterns::kMeshLodsOffset, 8)) {
            uint8_t* lods = *reinterpret_cast<uint8_t**>(mesh + patterns::kMeshLodsOffset);
            const int nl = *reinterpret_cast<int*>(mesh + patterns::kMeshLodCountOffset);
            if (lods && lod >= 0 && lod < nl && readable(lods + patterns::kLodStride * lod, patterns::kLodStride)) {
                uint8_t* L = lods + patterns::kLodStride * lod;
                void* src = *reinterpret_cast<void**>(L + patterns::kLodSkinnedVertsOffset);
                const int sc = *reinterpret_cast<int*>(L + patterns::kLodSkinnedCountOffset);
                if (src && sc > 0 && sc < 200000) {
                    if (const WristCopy* wc = build_wrist_copy(L, static_cast<const uint8_t*>(src), sc)) {
                        vslot = reinterpret_cast<void**>(L + patterns::kLodSkinnedVertsOffset);
                        vengine = src;
                        vcopy = wc->data;
                    }
                }
            }
        }
    }
    // The skinner reads the array through the instance for the length of this one call; it
    // gets the copy, and the engine's pointer is back before anything else can look.
    const int r = call_with_copy(self, inst, lod, out, a5, slot, engine, vslot, vengine, vcopy);
    g_lastWhy = "composing";
    g_composed.fetch_add(1, std::memory_order_relaxed);
    return r;
}

// ---- the held weapon's draw (Dishonored 1.5 on BS1's rigid path) ------------------------
using MeshDrawFn = int(__thiscall*)(void* mesh, void* rec, void* renderer, int a4);
MeshDrawFn g_meshDrawOrig = nullptr;
bool g_meshDrawInstalled = false;
std::atomic<int> g_recOwnerOffset{-1}; // discovered: the record field that holds the weapon actor
std::atomic<uint32_t> g_weaponDraws{0};
uint64_t g_recLog = 0;

// p * A * B for row-vector 4x4s.
void mat_mul(const float* a, const float* b, float* o) {
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
            o[r * 4 + c] = a[r * 4 + 0] * b[0 * 4 + c] + a[r * 4 + 1] * b[1 * 4 + c] + a[r * 4 + 2] * b[2 * 4 + c] +
                           a[r * 4 + 3] * b[3 * 4 + c];
}
// A similarity as a row-vector 4x4: rows 0-2 the images of the axes, row 3 the translation.
void sim_rows(const Sim& g, float* m) {
    for (int i = 0; i < 16; ++i) m[i] = (i % 5 == 0) ? 1.0f : 0.0f;
    apply_sim(g, m);
}

int call_mesh_draw(void* mesh, void* rec, void* renderer, int a4, float* l2w, float* w2l, const float* nl,
                   const float* nw) {
    float savedL[16], savedW[16];
    memcpy(savedL, l2w, 64);
    memcpy(savedW, w2l, 64);
    memcpy(l2w, nl, 64);
    memcpy(w2l, nw, 64);
    int r = 0;
    __try {
        r = g_meshDrawOrig(mesh, rec, renderer, a4);
    } __finally {
        memcpy(l2w, savedL, 64);
        memcpy(w2l, savedW, 64);
    }
    return r;
}

int __fastcall mesh_draw_detour(void* mesh, void* /*edx*/, void* rec, void* renderer, int a4) {
    Published p;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        p = g_pub;
    }
    if (!g_enabled.load(std::memory_order_relaxed) || !p.weapon.valid || !p.weapon.actor || !rec ||
        GetTickCount64() - p.ms > 250 || !readable(rec, 0x120))
        return g_meshDrawOrig(mesh, rec, renderer, a4);
    uint8_t* b = static_cast<uint8_t*>(rec);
    int off = g_recOwnerOffset.load(std::memory_order_relaxed);
    if (off < 0) {
        // Discovery: the first record that carries the held weapon actor names the field.
        for (int o = 0; o < 0x120; o += 4)
            if (*reinterpret_cast<void**>(b + o) == p.weapon.actor) {
                off = o;
                g_recOwnerOffset.store(o, std::memory_order_relaxed);
                BVR_LOG("[palette] weapon: the static-mesh draw record holds its owner actor at +0x%X", o);
                break;
            }
        if (off < 0) return g_meshDrawOrig(mesh, rec, renderer, a4);
    }
    if (*reinterpret_cast<void**>(b + off) != p.weapon.actor) return g_meshDrawOrig(mesh, rec, renderer, a4);

    float* l2w = reinterpret_cast<float*>(b + patterns::kDrawRecLocalToWorldOffset);
    float* w2l = reinterpret_cast<float*>(b + patterns::kDrawRecWorldToLocalOffset);
    // L' = D_world * L ; W' = W * inverse(D_world), both row-vector.
    float nl[16], nw[16], dinv[16];
    memcpy(nl, l2w, 64);
    apply_sim(p.weapon.dWorld, nl);
    sim_rows(sim_inv(p.weapon.dWorld), dinv);
    mat_mul(dinv, w2l, nw);
    const uint64_t now = GetTickCount64();
    if (now - g_recLog >= 2000) {
        g_recLog = now;
        BVR_LOG("[palette] weapon draw: record LocalToWorld translation %.1f %.1f %.1f (the actor's location "
                "%.1f %.1f %.1f) -> %.1f %.1f %.1f",
                l2w[12], l2w[13], l2w[14], p.weapon.lWeapon.t[0], p.weapon.lWeapon.t[1], p.weapon.lWeapon.t[2],
                nl[12], nl[13], nl[14]);
    }
    g_weaponDraws.fetch_add(1, std::memory_order_relaxed);
    return call_mesh_draw(mesh, rec, renderer, a4, l2w, w2l, nl, nw);
}

bool install_mesh_draw(const void* imageBase) {
    uint8_t* target = const_cast<uint8_t*>(static_cast<const uint8_t*>(imageBase)) + patterns::kStaticMeshDrawRva;
    if (!readable(target, sizeof patterns::kStaticMeshDrawPrologue) ||
        memcmp(target, patterns::kStaticMeshDrawPrologue, sizeof patterns::kStaticMeshDrawPrologue) != 0) {
        BVR_LOG("[palette] weapon: REFUSED - the static-mesh draw at RVA 0x%X does not carry the derived prologue",
                patterns::kStaticMeshDrawRva);
        return false;
    }
    MH_STATUS st =
        MH_CreateHook(target, reinterpret_cast<void*>(&mesh_draw_detour), reinterpret_cast<void**>(&g_meshDrawOrig));
    if (st != MH_OK && st != MH_ERROR_ALREADY_CREATED) return false;
    st = MH_EnableHook(target);
    if (st != MH_OK && st != MH_ERROR_ENABLED) return false;
    g_meshDrawInstalled = true;
    BVR_LOG("[palette] weapon: the static-mesh draw is HOOKED at RVA 0x%X (the held weapon's LocalToWorld gets "
            "the hand's correction, Dishonored 1.5)",
            patterns::kStaticMeshDrawRva);
    return true;
}

} // namespace

bool install(const void* imageBase) {
    if (g_installed) return true;
    if (!imageBase) return false;
    uint8_t* target = const_cast<uint8_t*>(static_cast<const uint8_t*>(imageBase)) + patterns::kSkinGatherRva;
    if (!readable(target, sizeof patterns::kSkinGatherPrologue) ||
        memcmp(target, patterns::kSkinGatherPrologue, sizeof patterns::kSkinGatherPrologue) != 0) {
        BVR_LOG("[palette] REFUSED: the skin gather at RVA 0x%X does not carry the derived prologue "
                "(another build? re-run tools\\ida\\rs11_palette_hooks.py) - the hands stay on the bone drive",
                patterns::kSkinGatherRva);
        return false;
    }
    MH_STATUS st = MH_CreateHook(target, reinterpret_cast<void*>(&gather_detour), reinterpret_cast<void**>(&g_orig));
    if (st != MH_OK && st != MH_ERROR_ALREADY_CREATED) {
        BVR_LOG("[palette] MH_CreateHook failed: %s", MH_StatusToString(st));
        return false;
    }
    st = MH_EnableHook(target);
    if (st != MH_OK && st != MH_ERROR_ENABLED) {
        BVR_LOG("[palette] MH_EnableHook failed: %s", MH_StatusToString(st));
        return false;
    }
    g_installed = true;
    install_mesh_draw(imageBase);
    BVR_LOG("[palette] the skin gather is HOOKED at RVA 0x%X: the hands are composed on the skin palette "
            "(Dishonored's route), the skeleton is the game's",
            patterns::kSkinGatherRva);
    return true;
}

void set_enabled(bool on) {
    g_enabled.store(on, std::memory_order_relaxed);
    BVR_LOG("[palette] %s", on ? "ON - the hands are composed on the skin palette"
                               : "OFF - the hands go back to the bone drive");
}
bool enabled() { return g_enabled.load(std::memory_order_relaxed); }
bool active() { return g_installed && enabled(); }

void publish(void* meshInstance, const HandXform hands[2], bool hideArms, const WeaponXform& weapon) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_pub.instance = meshInstance;
    g_pub.weapon = weapon;
    g_pub.hand[0] = hands[0];
    g_pub.hand[1] = hands[1];
    g_pub.hideArms = hideArms;
    g_pub.ms = GetTickCount64();
}

Sim sim_world(const Sim& L, const Sim& S) { return palette_math::sim_world(L, S); }

void set_link_probe(void* actor, void* skelInst) {
    g_linkActor.store(actor, std::memory_order_relaxed);
    g_linkSkel.store(skelInst, std::memory_order_relaxed);
}

void clear() {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_pub = Published{};
}

bool rigid_palm_local(int hand, float out[3]) {
    if (!g_rigidAnchor.load(std::memory_order_relaxed)) return false;
    std::lock_guard<std::mutex> lock(g_anchorMutex);
    const int h = hand ? 1 : 0;
    if (!g_palmLocalOk[h]) return false;
    memcpy(out, g_palmLocal[h], 12);
    return true;
}
void set_rigid_anchor(bool on) {
    g_rigidAnchor.store(on, std::memory_order_relaxed);
    BVR_LOG("[palette] rigid anchor (VR-183) %s", on ? "ON" : "off - the live palm centre");
}
bool rigid_anchor() { return g_rigidAnchor.load(std::memory_order_relaxed); }
void set_rigid_wrist(bool on) {
    g_rigidWrist.store(on, std::memory_order_relaxed);
    BVR_LOG("[palette] rigid wrist (VR-184) %s", on ? "ON" : "off - the game's weights");
}

void set_probe(bool on) {
    g_probeState[0] = g_probeState[1] = ProbeState{};
    g_probe.store(on, std::memory_order_relaxed);
    BVR_LOG("[palette] space probe %s", on ? "ON" : "off");
}

void log_status() {
    const int n = g_censusN.load(std::memory_order_acquire);
    for (int i = 0; i < n; ++i) {
        if (g_censusNamed[i]) continue;
        g_censusNamed[i] = true;
        void* o = g_censusOwner[i].load(std::memory_order_relaxed);
        const wchar_t* cls = patterns::object_class_name(o);
        BVR_LOG("[palette] census: the gather skins %ls (actor %p, %d bones)", cls ? cls : L"?", o,
                g_censusBones[i].load(std::memory_order_relaxed));
    }
    void* inst = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        inst = g_pub.instance;
    }
    BVR_LOG("[palette] %s%s | gather calls %u, hands composed %u, weapon composed %u, refused %u (%s) | palette "
            "bones %d | published "
            "instance %p",
            g_installed ? "hooked" : "NOT hooked", enabled() ? ", on" : ", off", g_calls.load(), g_composed.load(),
            g_weaponComposed.load() + g_weaponDraws.load(), g_refused.load(), g_lastWhy, g_lastCount.load(), inst);
}

} // namespace bvr::b1r::palette
