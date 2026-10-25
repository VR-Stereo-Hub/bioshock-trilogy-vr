#include "game/bioshock1r/second_hands.h"

#include "core/util/log.h"
#include "game/bioshock1r/patterns.h"
#include "game/bioshock1r/script_call.h"

#include <windows.h>
#include <intrin.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>

namespace bvr::b1r::second_hands {
namespace {

template <class T>
bool rd(const void* p, T* out) {
    __try {
        memcpy(out, p, sizeof(T));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
template <class T>
bool wr(void* p, const T& v) {
    __try {
        memcpy(p, &v, sizeof(T));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

std::atomic<void*> g_rig{nullptr}; // the second hands (game thread writes)
void* g_main = nullptr;             // the main hands it was made beside
std::atomic<bool> g_rightDown{false};
bool g_rightWas = false;
uint64_t g_spawnFailMs = 0;
uint32_t g_casts = 0, g_releases = 0, g_equips = 0;

constexpr int kRigBones = 47; // patterns::kHandsRigBoneCount
std::mutex g_skinMutex;
float g_skins[kRigBones * 16] = {};
uint64_t g_skinMs = 0;
float g_skinsOut[kRigBones * 16] = {};

void* class_of(void* obj) {
    void* c = nullptr;
    return obj && rd(static_cast<uint8_t*>(obj) + patterns::kUObjectClassOffset, &c) ? c : nullptr;
}

void* prop_ptr(void* obj, const wchar_t* name) {
    const int32_t off = obj ? script::property_offset(obj, name) : -1;
    void* v = nullptr;
    return off >= 0 && rd(static_cast<uint8_t*>(obj) + off, &v) ? v : nullptr;
}
bool set_prop_ptr(void* obj, const wchar_t* name, void* v) {
    const int32_t off = obj ? script::property_offset(obj, name) : -1;
    return off >= 0 && wr(static_cast<uint8_t*>(obj) + off, v);
}

void destroy(const char* why) {
    void* rig = g_rig.exchange(nullptr, std::memory_order_relaxed);
    if (!rig) return;
    struct {
        uint32_t ret;
        int32_t pad[7];
    } d{0, {}};
    // Its PostBeginPlay spawned a gatherer tool of its own (Hands.PostBeginPlay): that goes with it.
    if (void* tool = prop_ptr(rig, L"GathererTool")) {
        decltype(d) td{0, {}};
        script::call(tool, "Destroy", &td);
    }
    const bool ok = script::call(rig, "Destroy", &d);
    BVR_LOG("[hands2] the second hands destroyed (%s): %s | casts %u, releases %u, plasmid sets %u", why,
            ok ? "done" : "Destroy refused", g_casts, g_releases, g_equips);
    g_main = nullptr;
    g_rightWas = false;
}

void* spawn(void* mainHands, void* pawn) {
    void* cls = class_of(mainHands);
    if (!cls) return nullptr;
    struct {
        void* cls;
        void* owner;
        int32_t tag[2];
        float loc[3];
        int32_t rot[3];
        uint32_t noCollisionFail;
        int32_t label[2];
        void* ret;
        int32_t pad[4];
    } sp{cls, pawn, {0, 0}, {}, {}, 1, {0, 0}, nullptr, {}};
    rd(static_cast<uint8_t*>(mainHands) + patterns::kActorLocOffset, &sp.loc);
    rd(static_cast<uint8_t*>(mainHands) + patterns::kActorLocOffset + 12, &sp.rot);
    return script::call(mainHands, "Spawn", &sp) ? sp.ret : nullptr;
}

bool call_name(void* obj, const char* fn, const char* nameArg) {
    const int32_t nm = script::name_index(nameArg);
    if (nm < 0) return false;
    struct {
        int32_t name[2];
        int32_t pad[6];
    } p{{nm, 0}, {}};
    return script::call(obj, fn, &p);
}

} // namespace

void* actor() { return g_rig.load(std::memory_order_relaxed); }

void set_right_trigger(bool down) { g_rightDown.store(down, std::memory_order_relaxed); }

void tick(void* mainHands, bool wanted) {
    void* rig = g_rig.load(std::memory_order_relaxed);
    if (rig && (!wanted || mainHands != g_main)) {
        // A new main hands is a new level (the old actors are the level's to free): forget, never touch.
        if (mainHands != g_main) {
            g_rig.store(nullptr, std::memory_order_relaxed);
            BVR_LOG("[hands2] the main hands changed (a level change): the second hands forgotten");
        } else {
            destroy("dual plasmids off, no plasmid in hand, or not in gameplay");
        }
        rig = nullptr;
    }
    if (!wanted || !mainHands || !script::ready()) return;
    void* pawn = prop_ptr(mainHands, L"PawnOwner");
    if (!pawn) return;
    const uint64_t now = GetTickCount64();
    if (!rig) {
        if (now - g_spawnFailMs < 2000) return;
        rig = spawn(mainHands, pawn);
        if (!rig) {
            g_spawnFailMs = now;
            BVR_LOG("[hands2] Spawn of the second hands returned nothing (retrying in 2 s)");
            return;
        }
        if (!prop_ptr(rig, L"PawnOwner")) set_prop_ptr(rig, L"PawnOwner", pawn);
        call_name(rig, "SetHandsMode", "Ability");
        g_rig.store(rig, std::memory_order_relaxed);
        g_main = mainHands;
        g_casts = g_releases = g_equips = 0;
        BVR_LOG("[hands2] the second hands spawned: %p (the main %p, owner %p, PawnOwner %s)", rig, mainHands, pawn,
                prop_ptr(rig, L"PawnOwner") ? "set" : "NOT set");
    }
    // Placed as the game places its hands (Hands.UpdateLocation: the pawn, plus its view offset - which bones.cpp
    // sets so its left palm lands on the right hand), called here too in case the engine only ticks Pawn.Hands.
    {
        uint8_t none[32] = {};
        script::call(rig, "UpdateLocation", none);
    }
    // Its plasmid: the main hands' (SetActiveAbility takes it only while no change is in flight - OldAbility none).
    void* want = prop_ptr(mainHands, L"CurrentAbility");
    void* have = prop_ptr(rig, L"CurrentAbility");
    if (want && want != have && !prop_ptr(rig, L"OldAbility")) {
        struct {
            void* ability;
            int32_t pad[7];
        } p{want, {}};
        if (script::call(rig, "SetActiveAbility", &p)) {
            ++g_equips;
            const wchar_t* cn = patterns::object_class_name(want);
            BVR_LOG("[hands2] the second hands take the plasmid %S", cn ? cn : L"?");
        }
    }
    // The right trigger: its own press and release, as the game's input gives the main hands theirs.
    const bool down = g_rightDown.load(std::memory_order_relaxed);
    if (down != g_rightWas) {
        if (down) {
            struct {
                uint32_t keepPending;
                int32_t pad[7];
            } p{0, {}};
            if (script::call(rig, "UseActiveAbility", &p)) ++g_casts;
        } else {
            uint8_t none[32] = {};
            if (script::call(rig, "UseActiveAbilityRelease", none)) ++g_releases;
        }
        g_rightWas = down;
    }
}

bool on_stack() {
    const uintptr_t rig = reinterpret_cast<uintptr_t>(g_rig.load(std::memory_order_relaxed));
    if (!rig) return false;
    const NT_TIB* tib = reinterpret_cast<const NT_TIB*>(NtCurrentTeb());
    const uintptr_t* sp = reinterpret_cast<const uintptr_t*>(_AddressOfReturnAddress());
    const uintptr_t* top = reinterpret_cast<const uintptr_t*>(tib->StackBase);
    for (int i = 0; i < 16384 && sp + i < top; ++i)
        if (sp[i] == rig) return true;
    return false;
}

void capture_skins(const float* engine, int count) {
    if (!engine || count != kRigBones) return;
    std::lock_guard<std::mutex> lock(g_skinMutex);
    memcpy(g_skins, engine, sizeof g_skins);
    g_skinMs = GetTickCount64();
}

const float* skins() {
    std::lock_guard<std::mutex> lock(g_skinMutex);
    if (!g_skinMs || GetTickCount64() - g_skinMs > 250) return nullptr;
    memcpy(g_skinsOut, g_skins, sizeof g_skinsOut);
    return g_skinsOut;
}

} // namespace bvr::b1r::second_hands
