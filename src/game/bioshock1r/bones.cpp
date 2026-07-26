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

#include "core/util/log.h"
#include "game/bioshock1r/patterns.h"

#include <windows.h>

#include <imgui.h>

#include <atomic>
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

// Both clusters are baked (patterns.h) after live measurement; the lcluster
// command stays as a runtime override for future rig experiments.
std::atomic<int> g_lFirst{patterns::kBoneLClusterFirst}, g_lLast{patterns::kBoneLClusterLast},
    g_lAnchor{patterns::kBoneLWrist};
std::atomic<int> g_rAnchorOverride{-1};

std::atomic<bool> g_collapse{true}; // hide the driven arm's sleeve
std::atomic<uint32_t> g_writes{0};
std::atomic<int> g_lastHand{-1};
char g_status[160] = "idle";

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
}

bool drive(const FrameContext& ctx, void* handsActor, const GamePose& gp, int hand) {
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

    // World target -> component space. THE FRAME MATTERS (in-headset lesson,
    // 2026-07-26): the renderer orients the first-person rig by the RENDER
    // CAMERA's rotation, not by the actor's rotation field - flat they are the
    // same value, so only a camera-vs-actor rotation split (the injected-pitch
    // A/B, or the HMD head-look) exposes it. Compose against the FINAL camera
    // rotation this frame produced (ctx), anchored at the actor's LOCATION
    // field (which the renderer does use - camera-position moves proved that).
    float actorLoc[3];
    if (!read_n(static_cast<uint8_t*>(handsActor) + patterns::kActorLocOffset, actorLoc, 12))
        return false;
    FRotator camRot{ctx.camPitch, ctx.camYaw, ctx.camRoll};

    float qa[4], qt[4], qaInv[4], qtc[4];
    ue_rot_to_quat(camRot, qa);
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

    // Rigid move: rotate the reference cluster by qtc about the reference
    // anchor point, then put the anchor point at the target.
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
            return false;
        }
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
    g_writes.fetch_add(1, std::memory_order_relaxed);
    g_lastHand.store(hand, std::memory_order_relaxed);
    return true;
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
                "lcluster)",
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
    (void)g_status;
}

} // namespace bvr::b1r::bones
