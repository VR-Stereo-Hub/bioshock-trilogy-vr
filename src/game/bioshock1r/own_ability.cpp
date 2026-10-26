// 2026-10-25: an Ability object that belongs to one hand - see own_ability.h.
#include "game/bioshock1r/own_ability.h"

#include "core/util/log.h"
#include "game/bioshock1r/patterns.h"

#include <windows.h>

#include <cstdint>
#include <cstring>

namespace bvr::b1r::own_ability {
namespace {

// StaticConstructObject (patterns.h): cdecl, ten dwords.
using ConstructFn = void*(__cdecl*)(void* cls, void* outer, int32_t nameIndex, int32_t nameNumber, uint32_t flags,
                                    void*, void*, void* error, void*, void*);

// The error device the constructor reports to (exec New passes its script frame). Only Serialize is ever called
// (Logf formats, then Serialize); what it says goes to our log.
struct QuietDevice {
    virtual void Serialize(const wchar_t* text, int /*event*/) {
        BVR_LOG("[ownability] the engine's constructor reported: %ls", text ? text : L"(nothing)");
    }
    virtual void Flush() {}
    virtual void TearDown() {}
    int32_t suppressEventTag = 0;
    int32_t autoEmitLineTerminator = 1;
};
QuietDevice g_device;

ConstructFn g_construct = nullptr;
int g_resolved = 0; // 0 not yet, 1 found, -1 refused

bool resolve() {
    if (g_resolved) return g_resolved > 0;
    const auto* image = reinterpret_cast<const uint8_t*>(GetModuleHandleW(nullptr));
    const uint8_t* at = image + patterns::kStaticConstructObjectRva;
    uint8_t head[16] = {};
    bool read = false;
    __try {
        memcpy(head, at, sizeof head);
        read = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    if (!read || memcmp(head, patterns::kStaticConstructObjectPrologue, sizeof head)) {
        g_resolved = -1;
        BVR_LOG("[ownability] StaticConstructObject NOT found at RVA 0x%X (its first bytes differ on this build): the "
                "hands share the player's ability objects",
                patterns::kStaticConstructObjectRva);
        return false;
    }
    g_construct = reinterpret_cast<ConstructFn>(const_cast<uint8_t*>(at));
    g_resolved = 1;
    BVR_LOG("[ownability] StaticConstructObject found (RVA 0x%X)", patterns::kStaticConstructObjectRva);
    return true;
}

constexpr int kMaxMade = 32;
const void* g_made[kMaxMade] = {};
int g_madeAt = 0;
uint32_t g_madeCount = 0;

} // namespace

void* make(void* abilityClass, void* outer) {
    if (!abilityClass || !resolve()) return nullptr;
    void* obj = nullptr;
    __try {
        obj = g_construct(abilityClass, outer, 0, 0, 0, nullptr, nullptr, &g_device, nullptr, nullptr);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        obj = nullptr;
        BVR_LOG("[ownability] StaticConstructObject FAULTED");
    }
    if (!obj) return nullptr;
    g_made[g_madeAt] = obj;
    g_madeAt = (g_madeAt + 1) % kMaxMade;
    ++g_madeCount;
    const wchar_t* cn = patterns::object_class_name(obj);
    BVR_LOG("[ownability] a hand's own %ls made: %p (%u made since the level began)", cn ? cn : L"?", obj, g_madeCount);
    return obj;
}

bool is_own(const void* ability) {
    if (!ability) return false;
    for (const void* m : g_made)
        if (m == ability) return true;
    return false;
}

void forget_all() {
    memset(g_made, 0, sizeof g_made);
    g_madeAt = 0;
    g_madeCount = 0;
}

} // namespace bvr::b1r::own_ability
