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
// 2026-10-22 (MANUAL_RELOAD_PLAN.md step 1; the 18:00 run: the crossbow's reticle sat left of where the bolt
// landed): the crossbow's mesh has no "muzzle" socket (XB_UpgradeA, XB_UpgradeB, AmmoDummy), so it had no barrel
// origin and its bolt left from the game's own fire start while the reticle sat on the controller ray - a
// parallax of the whole hand-to-eye offset. Without a muzzle, the socket the round SITS in (AmmunitionModelSocket:
// the crossbow's AmmoDummy on X_BoltSocket, the bolt's seat) is where it leaves from.
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
            int seat = -1;
            int32_t seatName[2] = {};
            for (int i = 0; i < count; ++i) {
                int32_t nm[2] = {};
                if (!read_n(names + 2 * i, nm, sizeof nm)) break;
                const wchar_t* t = patterns::fname_text(nm[0]);
                if (t && !_wcsicmp(t, L"muzzle") && g_cache.socket < 0) {
                    memcpy(g_cache.name, nm, sizeof nm);
                    g_cache.socket = i;
                }
                if (t && !_wcsicmp(t, L"AmmoDummy") && seat < 0) {
                    memcpy(seatName, nm, sizeof nm);
                    seat = i;
                }
                const size_t used = strlen(list);
                if (used + 40 < sizeof list) _snprintf_s(list + used, sizeof list - used, _TRUNCATE, "%s%ls", i ? ", " : "", t ? t : L"?");
            }
            if (g_cache.socket >= 0) {
                BVR_LOG("[muzzle] weapon mesh %p: 'muzzle' is socket %d of %d", inst, g_cache.socket, count);
            } else if (seat >= 0) {
                memcpy(g_cache.name, seatName, sizeof seatName);
                g_cache.socket = seat;
                BVR_LOG("[muzzle] weapon mesh %p has no 'muzzle' socket: the round's seat 'AmmoDummy' (socket %d of %d) is the "
                        "barrel origin", inst, seat, count);
            } else {
                BVR_LOG("[muzzle] weapon mesh %p has no 'muzzle' socket (%d: %s)", inst, count, list);
            }
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

// ---- 2026-10-22 late: the socket turned to a line -------------------------------------------------------------
// The chemical thrower's spray (a DamageEmitter attached to the muzzle socket, and whatever effect emitters ride
// it) is drawn from the ATTACH MATRIX - the bone's coords composed with the socket's own FCoords offset in the mesh
// instance's socket table (+0x100, 48 bytes each: origin, then X, Y, Z in the bone's frame) - never from the
// emitter actor's Rotation (patterns.h: "the renderer draws attached actors from the attach matrix, ignoring their
// own Location/Rotation"), which is why Actor.SetRelativeRotation converged on the actor and moved nothing on
// screen (the 20:52 run). So the socket's offset itself is turned: its X to the wanted line, in the bone's frame.
// The original is kept and restored when the weapon leaves the hand. The composition convention (the offset's
// axes as rows in the bone's frame, or the transpose) is decided once by predicting the socket's current world X
// from the offset and the bone and comparing with what the engine returns for the socket.
namespace {
constexpr uint32_t kMeshInstSocketOffsetsOffset = 0x100;
struct SocketSave {
    void* inst = nullptr;
    int socket = -1;
    float coords[12] = {};
    bool valid = false;
    int convention = 0; // 1 rows, 2 transpose, 0 undecided
};
SocketSave g_save;

bool write_n(void* dst, const void* src, size_t n) {
    __try {
        memcpy(dst, src, n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
float* socket_coords(void* inst, int socket, int count) {
    float* data = nullptr;
    if (socket < 0 || socket >= count) return nullptr;
    if (!read_n(static_cast<uint8_t*>(inst) + kMeshInstSocketOffsetsOffset, &data, sizeof data) || !data) return nullptr;
    return data + 12 * socket;
}
bool orthonormal(const float c[12]) {
    for (int a = 0; a < 3; ++a) {
        const float* v = c + 3 + 3 * a;
        const float l = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        if (!std::isfinite(l) || std::fabs(l - 1.0f) > 0.05f) return false;
    }
    for (int a = 0; a < 3; ++a)
        for (int b = a + 1; b < 3; ++b) {
            const float *p = c + 3 + 3 * a, *q = c + 3 + 3 * b;
            if (std::fabs(p[0] * q[0] + p[1] * q[1] + p[2] * q[2]) > 0.05f) return false;
        }
    return true;
}
// rows: world = sum_k local[k] * B[k]; transpose: world[i] = sum_k local[k] * B[k][i]... the two readings of a
// 3x3 against a frame B whose rows are the bone's world axes.
void to_world(const float local[3], const float B[3][3], int convention, float out[3]) {
    for (int i = 0; i < 3; ++i) {
        out[i] = 0.0f;
        for (int k = 0; k < 3; ++k) out[i] += convention == 2 ? local[k] * B[i][k] : local[k] * B[k][i];
    }
}
void to_local(const float world[3], const float B[3][3], int convention, float out[3]) {
    for (int k = 0; k < 3; ++k) {
        out[k] = 0.0f;
        for (int i = 0; i < 3; ++i) out[k] += convention == 2 ? world[i] * B[i][k] : world[i] * B[k][i];
    }
}
} // namespace

bool aim_socket(void* weapon, const float dirWorld[3], float* degBefore, float* degAfter) {
    if (!g_fn || !weapon) return false;
    void* inst = nullptr;
    if (!read_n(static_cast<uint8_t*>(weapon) + patterns::kActorMeshInstOffset, &inst, sizeof inst) || !inst) return false;
    int32_t name[2];
    if (!muzzle_name(inst, name)) return false;
    float* c = socket_coords(inst, g_cache.socket, g_cache.count);
    if (!c) return false;
    if (g_save.inst != inst || g_save.socket != g_cache.socket) {
        g_save = SocketSave{};
        g_save.inst = inst;
        g_save.socket = g_cache.socket;
        if (!read_n(c, g_save.coords, sizeof g_save.coords) || !orthonormal(g_save.coords)) {
            BVR_LOG("[muzzle] socket offset of 'muzzle' on mesh %p is not a frame (origin %.2f %.2f %.2f, X %.2f %.2f %.2f) - not turned",
                    inst, g_save.coords[0], g_save.coords[1], g_save.coords[2], g_save.coords[3], g_save.coords[4], g_save.coords[5]);
            g_save.inst = nullptr;
            return false;
        }
        g_save.valid = true;
        BVR_LOG("[muzzle] socket offset of 'muzzle' on mesh %p: origin %.2f %.2f %.2f, X %.3f %.3f %.3f, Y %.3f %.3f %.3f, Z %.3f %.3f %.3f",
                inst, g_save.coords[0], g_save.coords[1], g_save.coords[2], g_save.coords[3], g_save.coords[4], g_save.coords[5],
                g_save.coords[6], g_save.coords[7], g_save.coords[8], g_save.coords[9], g_save.coords[10], g_save.coords[11]);
    }
    // The bone's frame and the socket's world X as the engine composes them now.
    float bc[12], sc[12];
    for (float& v : bc) v = std::numeric_limits<float>::quiet_NaN();
    for (float& v : sc) v = std::numeric_limits<float>::quiet_NaN();
    if (!call_guarded(weapon, bc, name[0], name[1], 0) || !call_guarded(weapon, sc, name[0], name[1], 1)) return false;
    float B[3][3], S[3][3];
    take_axes(bc, B);
    take_axes(sc, S);
    if (B[0][0] == 0.0f && B[0][1] == 0.0f && B[0][2] == 0.0f) return false;
    if (S[0][0] == 0.0f && S[0][1] == 0.0f && S[0][2] == 0.0f) return false;
    if (!g_save.convention) {
        // Which reading of the offset's X against the bone predicts the socket's X the engine returns?
        float w1[3], w2[3];
        to_world(g_save.coords + 3, B, 1, w1);
        to_world(g_save.coords + 3, B, 2, w2);
        const float d1 = w1[0] * S[0][0] + w1[1] * S[0][1] + w1[2] * S[0][2];
        const float d2 = w2[0] * S[0][0] + w2[1] * S[0][1] + w2[2] * S[0][2];
        g_save.convention = d1 >= d2 ? 1 : 2;
        BVR_LOG("[muzzle] socket offset convention: %s (predicted socket X against the engine's: rows %.3f, transpose %.3f)",
                g_save.convention == 1 ? "rows" : "transpose", d1, d2);
    }
    const float before = S[0][0] * dirWorld[0] + S[0][1] * dirWorld[1] + S[0][2] * dirWorld[2];
    if (degBefore) *degBefore = std::acos(before > 1.0f ? 1.0f : before < -1.0f ? -1.0f : before) * 57.29578f;
    // The wanted X in the bone's frame; Y the original Y squared up to it; Z keeps the original handedness.
    float x[3];
    to_local(dirWorld, B, g_save.convention, x);
    float xl = std::sqrt(x[0] * x[0] + x[1] * x[1] + x[2] * x[2]);
    if (!(xl > 1e-4f)) return false;
    for (float& v : x) v /= xl;
    const float* y0 = g_save.coords + 6;
    const float* x0 = g_save.coords + 3;
    const float* z0 = g_save.coords + 9;
    float y[3];
    const float yd = y0[0] * x[0] + y0[1] * x[1] + y0[2] * x[2];
    for (int i = 0; i < 3; ++i) y[i] = y0[i] - yd * x[i];
    const float yl = std::sqrt(y[0] * y[0] + y[1] * y[1] + y[2] * y[2]);
    if (!(yl > 1e-4f)) return false;
    for (float& v : y) v /= yl;
    float z[3] = {x[1] * y[2] - x[2] * y[1], x[2] * y[0] - x[0] * y[2], x[0] * y[1] - x[1] * y[0]};
    const float zx0[3] = {x0[1] * y0[2] - x0[2] * y0[1], x0[2] * y0[0] - x0[0] * y0[2], x0[0] * y0[1] - x0[1] * y0[0]};
    if (zx0[0] * z0[0] + zx0[1] * z0[1] + zx0[2] * z0[2] < 0.0f)
        for (float& v : z) v = -v;
    float axes[9] = {x[0], x[1], x[2], y[0], y[1], y[2], z[0], z[1], z[2]};
    if (!write_n(c + 3, axes, sizeof axes)) return false;
    if (degAfter) {
        float sc2[12];
        for (float& v : sc2) v = std::numeric_limits<float>::quiet_NaN();
        float S2[3][3];
        if (call_guarded(weapon, sc2, name[0], name[1], 1)) {
            take_axes(sc2, S2);
            const float after = S2[0][0] * dirWorld[0] + S2[0][1] * dirWorld[1] + S2[0][2] * dirWorld[2];
            *degAfter = std::acos(after > 1.0f ? 1.0f : after < -1.0f ? -1.0f : after) * 57.29578f;
        } else {
            *degAfter = -1.0f;
        }
    }
    return true;
}

void restore_socket() {
    if (!g_save.valid || !g_save.inst) return;
    float* c = socket_coords(g_save.inst, g_save.socket, g_cache.inst == g_save.inst ? g_cache.count : g_save.socket + 1);
    if (c) write_n(c, g_save.coords, sizeof g_save.coords);
    BVR_LOG("[muzzle] socket offset of 'muzzle' on mesh %p restored", g_save.inst);
    g_save = SocketSave{};
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
