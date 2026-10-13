// s91: the attachment census - see attach_census.h.
#include "game/bioshock1r/attach_census.h"

#include "core/hooks/pattern_scan.h"
#include "core/util/log.h"
#include "game/bioshock1r/arm_ik.h"
#include "game/bioshock1r/bones.h"
#include "game/bioshock1r/hands.h"
#include "game/bioshock1r/palette.h"
#include "game/bioshock1r/patterns.h"

#include <windows.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <cwchar>

namespace bvr::b1r::attach_census {
namespace {

// AActor -> XLevel (screens.cpp, ENGINE_NOTES "XLevel"); the Actors array inside ULevel is found
// by content, not assumed: the first TArray<AActor*> in the level's head that holds the hands.
constexpr uint32_t kActorXLevelOffset = 0xFC;
constexpr uint32_t kActorLocationOffset = 0x1D8;
constexpr uint32_t kActorScanBytes = 0x460; // where a link to the parent is looked for
constexpr int kMaxLinked = 64;

bool rd(const void* p, void* out, size_t n) {
    if (!p || !bvr::pattern_scan::is_memory_valid(p, n)) return false;
    __try {
        memcpy(out, p, n);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return true;
}

template <typename T>
bool rdv(const void* base, uint32_t off, T* out) {
    return rd(static_cast<const uint8_t*>(base) + off, out, sizeof(T));
}

int32_t g_actorsOffset = -1;

bool level_actors(void* hands, void*** data, int* num) {
    void* level = nullptr;
    if (!rdv(hands, kActorXLevelOffset, &level) || !level) return false;
    auto try_at = [&](int32_t off) {
        struct {
            void** data;
            int32_t num, max;
        } a{};
        if (!rdv(level, static_cast<uint32_t>(off), &a) || !a.data || a.num < 16 || a.num > 500000 ||
            a.max < a.num || !bvr::pattern_scan::is_memory_valid(a.data, static_cast<size_t>(a.num) * 4))
            return false;
        for (int i = 0; i < a.num; ++i)
            if (a.data[i] == hands) {
                *data = a.data;
                *num = a.num;
                return true;
            }
        return false;
    };
    if (g_actorsOffset >= 0 && try_at(g_actorsOffset)) return true;
    for (int32_t off = 0x20; off < 0x200; off += 4)
        if (try_at(off)) {
            if (g_actorsOffset != off) BVR_LOG("[attach] the level's Actors array is at XLevel+0x%X", off);
            g_actorsOffset = off;
            return true;
        }
    return false;
}

void sockets(void* hands) {
    void* inst = nullptr;
    if (!rdv(hands, patterns::kActorMeshInstOffset, &inst) || !inst) {
        BVR_LOG("[attach] socket table: no instance at hands+0x%X", patterns::kActorMeshInstOffset);
        return;
    }
    // +0x3FA1E0 (tools\ida\rs14): instance +0xE8/+0xEC socket names (8 bytes each), +0xF4 the bone each
    // aliases (8 bytes), +0x100 a 48-byte offset per socket.
    struct {
        const int32_t* names;
        int32_t num;
    } s{};
    const int32_t* bonesTo = nullptr;
    const float* coords = nullptr;
    if (!rdv(inst, 0xE8, &s) || !rdv(inst, 0xF4, &bonesTo) || !rdv(inst, 0x100, &coords)) return;
    BVR_LOG("[attach] socket table on the hands instance %p: %d entries", inst, s.num);
    for (int i = 0; i < s.num && i < 32; ++i) {
        int32_t nm[2] = {}, bn[2] = {};
        float c[12] = {};
        if (!rd(s.names + 2 * i, nm, 8)) break;
        const bool haveBone = bonesTo && rd(bonesTo + 2 * i, bn, 8);
        const bool haveC = coords && rd(coords + 12 * i, c, 48);
        const wchar_t* sn = patterns::fname_text(nm[0]);
        const wchar_t* bt = haveBone ? patterns::fname_text(bn[0]) : nullptr;
        BVR_LOG("[attach]   socket '%ls' -> bone '%ls' (index %d) offset %.2f %.2f %.2f", sn ? sn : L"?",
                bt ? bt : L"?", haveBone ? bones::bone_index_for_name(hands, bn[0], bn[1]) : -1,
                haveC ? c[0] : 0.0f, haveC ? c[1] : 0.0f, haveC ? c[2] : 0.0f);
    }
}

} // namespace

void run() {
    void* hands = hands::hands_actor();
    if (!hands) {
        BVR_LOG("[attach] no hands actor yet - in gameplay with the hands up?");
        return;
    }
    void* weapon = nullptr;
    hands::current_holdable(&weapon);
    palette::request_weight_census();
    sockets(hands);
    void** actors = nullptr;
    int num = 0;
    if (!level_actors(hands, &actors, &num)) {
        BVR_LOG("[attach] the level's Actors array was not found from XLevel (+0x%X)", kActorXLevelOffset);
        return;
    }
    float handsLoc[3] = {};
    rdv(hands, kActorLocationOffset, &handsLoc);
    void* parents[kMaxLinked] = {hands, weapon};
    int np = weapon ? 2 : 1;
    if (weapon) {
        int32_t nm[2] = {};
        rdv(weapon, patterns::kActorAttachBoneNameOffset, &nm);
        const wchar_t* wn = nm[0] > 0 ? patterns::fname_text(nm[0]) : nullptr;
        BVR_LOG("[attach] the weapon attaches to the hands at '%ls' = hands bone %d", wn ? wn : L"?",
                wn ? bones::bone_index_for_name(hands, nm[0], nm[1]) : -1);
    }
    BVR_LOG("[attach] census: %d actors in the level; parents: hands %p (%ls), weapon %p (%ls)", num, hands,
            patterns::object_class_name(hands) ? patterns::object_class_name(hands) : L"?", weapon,
            weapon && patterns::object_class_name(weapon) ? patterns::object_class_name(weapon) : L"-");
    // Two passes, so a child of the weapon's child is found as well.
    for (int pass = 0; pass < 2; ++pass) {
        for (int i = 0; i < num; ++i) {
            void* a = nullptr;
            if (!rd(actors + i, &a, 4) || !a || a == hands) continue;
            bool known = false;
            for (int p = 0; p < np; ++p) known = known || parents[p] == a;
            if (known) continue;
            uint8_t buf[kActorScanBytes];
            if (!rd(a, buf, sizeof buf)) continue;
            int which = -1;
            uint32_t at = 0;
            for (uint32_t off = 0x34; off < kActorScanBytes && which < 0; off += 4) {
                void* v = *reinterpret_cast<void**>(buf + off);
                for (int p = 0; p < np; ++p)
                    if (v == parents[p]) {
                        which = p;
                        at = off;
                    }
            }
            if (which < 0) continue;
            int32_t nm[2];
            memcpy(nm, buf + patterns::kActorAttachBoneNameOffset, 8);
            const wchar_t* attachName = nm[0] > 0 ? patterns::fname_text(nm[0]) : nullptr;
            const wchar_t* cls = patterns::object_class_name(a);
            if (!attachName && !(cls && (wcsstr(cls, L"Emitter") || wcsstr(cls, L"Effect") || wcsstr(cls, L"Light"))))
                continue; // a plain reference (an owner, an instigator), not an attachment
            const int boneIdx = attachName ? bones::bone_index_for_name(parents[which], nm[0], nm[1]) : -1;
            float loc[3];
            memcpy(loc, buf + kActorLocationOffset, 12);
            const wchar_t* pcls = patterns::object_class_name(parents[which]);
            BVR_LOG("[attach]   %ls %p -> %ls %p via +0x%X | attach name '%ls' = %s bone %d | location %.1f %.1f "
                    "%.1f (%.1f UU from the hands origin)",
                    cls ? cls : L"?", a, pcls ? pcls : L"?", parents[which], at, attachName ? attachName : L"",
                    which == 0 ? "hands" : "parent", boneIdx, loc[0], loc[1], loc[2],
                    sqrtf((loc[0] - handsLoc[0]) * (loc[0] - handsLoc[0]) +
                          (loc[1] - handsLoc[1]) * (loc[1] - handsLoc[1]) +
                          (loc[2] - handsLoc[2]) * (loc[2] - handsLoc[2])));
            // s91: where does the actor keep the relative it was attached with? For an actor on a
            // socket, look for three floats equal to that socket's offset (as recorded, x0.8, x1.25)
            // within its first 0x460 bytes - the field the engine follows every frame (renaming an
            // effect to another socket moved it to that socket's BONE with its old zero relative).
            if (which == 0 && attachName && boneIdx < 0) {
                void* inst = nullptr;
                struct {
                    const int32_t* names;
                    int32_t count;
                } tn{};
                const float* coords = nullptr;
                if (rdv(hands, patterns::kActorMeshInstOffset, &inst) && inst && rdv(inst, 0xE8, &tn) &&
                    rdv(inst, 0x100, &coords) && tn.names && coords && tn.count > 0 && tn.count <= 32) {
                    for (int s = 0; s < tn.count; ++s) {
                        int32_t sn[2];
                        float rec[3];
                        if (!rd(tn.names + 2 * s, sn, 8) || sn[0] != nm[0] || !rd(coords + 12 * s, rec, 12)) continue;
                        char hits[200] = {};
                        int at2 = 0;
                        const float scales[3] = {1.0f, 0.8f, 1.25f};
                        for (uint32_t off = 0x34; off + 12 <= kActorScanBytes && at2 < 180; off += 4) {
                            const float* f = reinterpret_cast<const float*>(buf + off);
                            for (float sc : scales) {
                                if (fabsf(f[0] - rec[0] * sc) < 1.5f && fabsf(f[1] - rec[1] * sc) < 1.5f &&
                                    fabsf(f[2] - rec[2] * sc) < 1.5f && (fabsf(rec[0]) + fabsf(rec[1]) + fabsf(rec[2])) > 1.0f)
                                    at2 += _snprintf_s(hits + at2, sizeof hits - at2, _TRUNCATE, " +0x%X(x%.2f: %.2f %.2f %.2f)",
                                                       off, sc, f[0], f[1], f[2]);
                            }
                        }
                        BVR_LOG("[attach]     relative search: socket '%ls' offset now %.2f %.2f %.2f -> matches in the actor:%s",
                                attachName, rec[0], rec[1], rec[2], at2 ? hits : " NONE");
                    }
                }
            }
            // The engine-vs-bone-array gap for an actor attached straight to a hands bone: the
            // socket follow's attach origin (bones.cpp) should be this vector for every bone.
            if (which == 0 && boneIdx >= 0) {
                float lc[3], bp[3], bq[4];
                if (bones::world_to_component(hands, loc, lc) && bones::bone_component(hands, boneIdx, bp, bq)) {
                    const arm_ik::Vec gap = arm_ik::vec(lc) - arm_ik::vec(bp);
                    const arm_ik::Vec inBone = arm_ik::rotate(arm_ik::conj(arm_ik::normalized(arm_ik::quat(bq))), gap);
                    BVR_LOG("[attach]     gap to bone %d: component %.2f %.2f %.2f | in the bone's frame %.2f %.2f %.2f",
                            boneIdx, gap.x, gap.y, gap.z, inBone.x, inBone.y, inBone.z);
                }
            }
            if (np < kMaxLinked) parents[np++] = a;
        }
    }
    BVR_LOG("[attach] census done: %d linked actors (the hands, the weapon and %d attached)", np, np - (weapon ? 2 : 1));
}

namespace {
constexpr int kSnapMax = 32768;
void* g_snap[kSnapMax];
int g_snapN = 0;
uint64_t g_watchUntil = 0, g_watchStart = 0;
void* g_seen[256];
int g_seenN = 0;

bool in_snap(void* a) {
    int lo = 0, hi = g_snapN - 1;
    while (lo <= hi) {
        const int mid = (lo + hi) / 2;
        if (g_snap[mid] == a) return true;
        if (reinterpret_cast<uintptr_t>(g_snap[mid]) < reinterpret_cast<uintptr_t>(a)) lo = mid + 1;
        else hi = mid - 1;
    }
    return false;
}
} // namespace

namespace {
// One guarded pass; a fault mid-list ends it (the list is re-read next call).
int collect_raw(void** actors, int num, void* hands, void** out, int cap) {
    int n = 0;
    __try {
        for (int i = 0; i < num && n < cap; ++i) {
            uint8_t* a = static_cast<uint8_t*>(actors[i]);
            if (!a || a == hands) continue;
            if (*reinterpret_cast<void**>(a + 0xAC) != hands && *reinterpret_cast<void**>(a + 0xB0) != hands) continue;
            if (*reinterpret_cast<int32_t*>(a + patterns::kActorAttachBoneNameOffset) <= 0) continue;
            out[n++] = a;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return n;
}
} // namespace

int collect_linked(void* hands, void** out, int cap) {
    void** actors = nullptr;
    int num = 0;
    if (!hands || !level_actors(hands, &actors, &num)) return 0;
    return collect_raw(actors, num, hands, out, cap);
}

void find(const char* text) {
    void* hands = hands::hands_actor();
    void** actors = nullptr;
    int num = 0;
    if (!hands || !level_actors(hands, &actors, &num) || !text || !*text) {
        BVR_LOG("[attach] find: no hands actor, no level actor list, or no text");
        return;
    }
    wchar_t wt[64] = {};
    for (int i = 0; i < 63 && text[i] && text[i] != ' ' && text[i] != '\r' && text[i] != '\n'; ++i)
        wt[i] = static_cast<wchar_t>(text[i]);
    void* pawn = nullptr;
    rdv(hands, patterns::kHandsBaseOffset, &pawn);
    float handsLoc[3] = {};
    rdv(hands, kActorLocationOffset, &handsLoc);
    int hits = 0;
    for (int i = 0; i < num && hits < 40; ++i) {
        void* a = nullptr;
        if (!rd(actors + i, &a, 4) || !a) continue;
        const wchar_t* cls = patterns::object_class_name(a);
        if (!cls || !wcsstr(cls, wt)) continue;
        uint8_t buf[kActorScanBytes];
        if (!rd(a, buf, sizeof buf)) continue;
        char links[160] = {};
        int at = 0;
        for (uint32_t off = 0x34; off < kActorScanBytes && at < 140; off += 4) {
            void* v = *reinterpret_cast<void**>(buf + off);
            if (v && (v == hands || v == pawn))
                at += _snprintf_s(links + at, sizeof links - at, _TRUNCATE, " %s@+0x%X", v == hands ? "hands" : "pawn", off);
        }
        float loc[3];
        memcpy(loc, buf + kActorLocationOffset, 12);
        ++hits;
        BVR_LOG("[attach] FIND %ls %p | location %.1f %.1f %.1f (%.1f UU from the hands origin) | links%s", cls, a,
                loc[0], loc[1], loc[2],
                sqrtf((loc[0] - handsLoc[0]) * (loc[0] - handsLoc[0]) + (loc[1] - handsLoc[1]) * (loc[1] - handsLoc[1]) +
                      (loc[2] - handsLoc[2]) * (loc[2] - handsLoc[2])),
                at ? links : " none");
    }
    BVR_LOG("[attach] find '%ls': %d match(es)", wt, hits);
}

void watch(float seconds) {
    void* hands = hands::hands_actor();
    void** actors = nullptr;
    int num = 0;
    if (!hands || !level_actors(hands, &actors, &num)) {
        BVR_LOG("[attach] watch: no hands actor or no level actor list");
        return;
    }
    g_snapN = 0;
    for (int i = 0; i < num && g_snapN < kSnapMax; ++i) {
        void* a = nullptr;
        if (rd(actors + i, &a, 4) && a) g_snap[g_snapN++] = a;
    }
    // insertion sort is too slow for 9k; a simple shell sort keeps this dependency-free
    for (int gap = g_snapN / 2; gap > 0; gap /= 2)
        for (int i = gap; i < g_snapN; ++i) {
            void* t = g_snap[i];
            int j = i;
            for (; j >= gap && reinterpret_cast<uintptr_t>(g_snap[j - gap]) > reinterpret_cast<uintptr_t>(t); j -= gap)
                g_snap[j] = g_snap[j - gap];
            g_snap[j] = t;
        }
    g_seenN = 0;
    g_watchStart = GetTickCount64();
    g_watchUntil = g_watchStart + static_cast<uint64_t>((seconds > 0.0f ? seconds : 1.0f) * 1000.0f);
    BVR_LOG("[attach] watch armed for %.1f s over %d actors", seconds, g_snapN);
}

void tick() {
    if (!g_watchUntil) return;
    const uint64_t now = GetTickCount64();
    if (now > g_watchUntil) {
        BVR_LOG("[attach] watch done: %d new actor(s)", g_seenN);
        g_watchUntil = 0;
        return;
    }
    void* hands = hands::hands_actor();
    void** actors = nullptr;
    int num = 0;
    if (!hands || !level_actors(hands, &actors, &num)) return;
    void* pawn = nullptr;
    rdv(hands, patterns::kHandsBaseOffset, &pawn);
    void* weapon = nullptr;
    hands::current_holdable(&weapon);
    float handsLoc[3] = {};
    rdv(hands, kActorLocationOffset, &handsLoc);
    for (int i = 0; i < num; ++i) {
        void* a = nullptr;
        if (!rd(actors + i, &a, 4) || !a || in_snap(a)) continue;
        bool seen = false;
        for (int k = 0; k < g_seenN; ++k) seen = seen || g_seen[k] == a;
        if (seen || g_seenN >= 256) continue;
        g_seen[g_seenN++] = a;
        uint8_t buf[kActorScanBytes];
        if (!rd(a, buf, sizeof buf)) continue;
        char links[160] = {};
        int at = 0;
        for (uint32_t off = 0x34; off < kActorScanBytes && at < 140; off += 4) {
            void* v = *reinterpret_cast<void**>(buf + off);
            if (!v) continue;
            const char* who = v == hands ? "hands" : v == pawn ? "pawn" : (weapon && v == weapon) ? "weapon" : nullptr;
            if (who) at += _snprintf_s(links + at, sizeof links - at, _TRUNCATE, " %s@+0x%X", who, off);
        }
        int32_t nm[2];
        memcpy(nm, buf + patterns::kActorAttachBoneNameOffset, 8);
        const wchar_t* attachName = nm[0] > 0 ? patterns::fname_text(nm[0]) : nullptr;
        float loc[3];
        memcpy(loc, buf + kActorLocationOffset, 12);
        const wchar_t* cls = patterns::object_class_name(a);
        BVR_LOG("[attach] NEW +%llums %ls %p | location %.1f %.1f %.1f (%.1f UU from the hands origin) | attach "
                "name '%ls' | links%s",
                static_cast<unsigned long long>(now - g_watchStart), cls ? cls : L"?", a, loc[0], loc[1], loc[2],
                sqrtf((loc[0] - handsLoc[0]) * (loc[0] - handsLoc[0]) + (loc[1] - handsLoc[1]) * (loc[1] - handsLoc[1]) +
                      (loc[2] - handsLoc[2]) * (loc[2] - handsLoc[2])),
                attachName ? attachName : L"", at ? links : " none");
    }
}

} // namespace bvr::b1r::attach_census
