// See muzzle.h.
#include "game/bioshock1r/muzzle.h"

#include "core/util/log.h"
#include "game/bioshock1r/patterns.h"

#include <windows.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <limits>

namespace bvr::b1r::muzzle {
namespace {

// patterns.h kActorGetBoneCoordsRva: FCoords* __thiscall (FCoords* out, FName name, UBOOL bSocket),
// the FName passed by value as its two dwords.
using GetBoneCoordsFn = float*(__thiscall*)(void* actor, float* out, int32_t nameIdx, int32_t nameNum,
                                            int32_t bSocket);
GetBoneCoordsFn g_fn = nullptr;

struct Cache {
    void* inst = nullptr;
    int32_t name[2] = {};
    int socket = -1, count = 0;
};
Cache g_cache;
uint64_t g_calls = 0, g_hits = 0, g_noSocket = 0, g_unwritten = 0, g_faults = 0;

bool read_n(const void* src, void* dst, size_t n) {
    __try {
        memcpy(dst, src, n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// No C++ objects in an SEH frame.
bool call_guarded(void* actor, float* out, int32_t idx, int32_t num, int32_t bSocket) {
    __try {
        g_fn(actor, out, idx, num, bSocket);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The "muzzle" entry of a mesh instance's socket table, searched once per instance. A weapon
// without one logs its socket names, so a run says what to look for instead.
bool muzzle_name(void* inst, int32_t out[2]) {
    if (inst != g_cache.inst) {
        g_cache = Cache{};
        g_cache.inst = inst;
        const int32_t* names = nullptr;
        int32_t count = 0;
        if (read_n(static_cast<uint8_t*>(inst) + patterns::kMeshInstSocketNamesOffset, &names, sizeof names) &&
            read_n(static_cast<uint8_t*>(inst) + patterns::kMeshInstSocketCountOffset, &count, sizeof count) &&
            names && count > 0 && count <= 256) {
            g_cache.count = count;
            char list[512] = "";
            for (int i = 0; i < count; ++i) {
                int32_t nm[2] = {};
                if (!read_n(names + 2 * i, nm, sizeof nm)) break;
                const wchar_t* t = patterns::fname_text(nm[0]);
                if (t && !_wcsicmp(t, L"muzzle") && g_cache.socket < 0) {
                    memcpy(g_cache.name, nm, sizeof nm);
                    g_cache.socket = i;
                }
                const size_t used = strlen(list);
                if (used + 40 < sizeof list) _snprintf_s(list + used, sizeof list - used, _TRUNCATE, "%s%ls", i ? ", " : "", t ? t : L"?");
            }
            if (g_cache.socket >= 0)
                BVR_LOG("[muzzle] weapon mesh %p: 'muzzle' is socket %d of %d", inst, g_cache.socket, count);
            else
                BVR_LOG("[muzzle] weapon mesh %p has no 'muzzle' socket (%d: %s)", inst, count, list);
        } else {
            BVR_LOG("[muzzle] weapon mesh %p: no socket table", inst);
        }
    }
    if (g_cache.socket < 0) return false;
    memcpy(out, g_cache.name, sizeof g_cache.name);
    return true;
}

} // namespace

void init(void* getBoneCoords) {
    g_fn = reinterpret_cast<GetBoneCoordsFn>(getBoneCoords);
}

namespace {
// The three axes of an FCoords result (after its origin), normalised; all zero if any is not.
void take_axes(const float c[12], float axes[3][3]) {
    bool ok = true;
    for (int a = 0; a < 3 && ok; ++a) {
        const float* v = c + 3 + 3 * a;
        const float len = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        ok = std::isfinite(len) && len > 1e-4f;
        for (int i = 0; i < 3 && ok; ++i) axes[a][i] = v[i] / len;
    }
    if (!ok) memset(axes, 0, sizeof(float) * 9);
}
} // namespace

bool bone_coords(void* actor, int32_t nameIndex, float origin[3], float axes[3][3]) {
    if (!g_fn || !actor || nameIndex < 0) return false;
    float c[12];
    for (float& v : c) v = std::numeric_limits<float>::quiet_NaN();
    if (!call_guarded(actor, c, nameIndex, 0, 0)) return false;
    for (int i = 0; i < 3; ++i)
        if (!std::isfinite(c[i]) || std::fabs(c[i]) > 1.0e7f) return false;
    for (int i = 0; i < 3; ++i) origin[i] = c[i];
    take_axes(c, axes);
    return axes[0][0] == axes[0][0] && (axes[0][0] != 0.0f || axes[0][1] != 0.0f || axes[0][2] != 0.0f);
}

bool locate(void* weapon, float origin[3], float socketAxes[3][3], float boneAxes[3][3]) {
    if (!g_fn || !weapon) return false;
    void* inst = nullptr;
    if (!read_n(static_cast<uint8_t*>(weapon) + patterns::kActorMeshInstOffset, &inst, sizeof inst) || !inst)
        return false;
    int32_t name[2];
    if (!muzzle_name(inst, name)) {
        ++g_noSocket;
        return false;
    }
    ++g_calls;
    // Its early returns leave the output untouched (no mesh instance, a failed LOD check):
    // pre-filled with NaN, an unwritten result cannot pass for a position.
    float c[12];
    for (float& v : c) v = std::numeric_limits<float>::quiet_NaN();
    if (!call_guarded(weapon, c, name[0], name[1], 1)) {
        if (++g_faults == 1) BVR_LOG("[muzzle] GetBoneCoords FAULTED on weapon %p (caught)", weapon);
        return false;
    }
    for (int i = 0; i < 3; ++i)
        if (!std::isfinite(c[i]) || std::fabs(c[i]) > 1.0e6f) {
            ++g_unwritten;
            return false;
        }
    ++g_hits;
    for (int i = 0; i < 3; ++i) origin[i] = c[i];
    take_axes(c, socketAxes);
    // The same name without the socket flag: the lookup swaps it for the bone it hangs on (the
    // gun's body), and returns that bone's frame - the mesh's own axes, without the socket's
    // authored rotation.
    float bc[12];
    for (float& v : bc) v = std::numeric_limits<float>::quiet_NaN();
    if (call_guarded(weapon, bc, name[0], name[1], 0)) take_axes(bc, boneAxes);
    else memset(boneAxes, 0, sizeof(float) * 9);
    return true;
}

void log_status() {
    BVR_LOG("[muzzle] GetBoneCoords %s | calls %llu hits %llu | no socket %llu, unwritten %llu, faults %llu | "
            "last mesh %p socket %d of %d",
            g_fn ? "resolved" : "NOT resolved", static_cast<unsigned long long>(g_calls),
            static_cast<unsigned long long>(g_hits), static_cast<unsigned long long>(g_noSocket),
            static_cast<unsigned long long>(g_unwritten), static_cast<unsigned long long>(g_faults), g_cache.inst,
            g_cache.socket, g_cache.count);
}

} // namespace bvr::b1r::muzzle
