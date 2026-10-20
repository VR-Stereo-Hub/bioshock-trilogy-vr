#include "dropped_part.h"
#include "patterns.h"
#include "script_call.h"
#include "core/util/log.h"

#include <windows.h>

#include <cstdint>
#include <cstring>
#include <cwchar>

namespace bvr::b1r::dropped_part {
namespace {

constexpr float kLifeSpanS = 30.0f;   // the prop removes itself after this
constexpr float kFallUu = 60.0f;      // a short downward start, world units per second
constexpr wchar_t kPropClass[] = L"ShockGame.PhysicalReactiveActor";

struct FStr {
    const wchar_t* data;
    int32_t num, max;
};
FStr fstr(const wchar_t* s) {
    const int32_t n = static_cast<int32_t>(wcslen(s)) + 1;
    return {s, n, n};
}

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

struct Captured {
    bool valid = false;
    void* mesh = nullptr;
    float loc[3] = {};
    int32_t rot[3] = {};
    float scale = 1.0f;
} g_cap;

void* class_of(void* obj) {
    void* c = nullptr;
    return obj && rd(static_cast<uint8_t*>(obj) + patterns::kUObjectClassOffset, &c) ? c : nullptr;
}

// Object.DynamicLoadObject(string, Class, optional bool MayFail) -> Object. The class of classes is
// any class's own class.
void* load_class(void* any, const wchar_t* path) {
    void* classClass = class_of(class_of(any));
    if (!classClass) return nullptr;
    struct {
        FStr name;
        void* cls;
        uint32_t mayFail;
        void* ret;
        int32_t pad[4];
    } p{fstr(path), classClass, 1, nullptr, {}};
    return script::call(any, "DynamicLoadObject", &p) ? p.ret : nullptr;
}

bool set_physics(void* a, uint8_t phys) {
    struct {
        uint8_t phys;
        uint8_t pad[15];
    } p{phys, {}};
    return script::call(a, "SetPhysics", &p);
}

bool set_vector(void* a, const char* fn, const float v[3]) {
    struct {
        float v[3];
        int32_t bone[2]; // optional name BoneName: none
        int32_t pad[3];
    } p{{v[0], v[1], v[2]}, {0, 0}, {}};
    return script::call(a, fn, &p);
}

} // namespace

bool capture(void* weapon) {
    g_cap = Captured{};
    if (!weapon || !script::properties_ready(weapon)) return false;
    const int32_t amOff = script::property_offset(weapon, L"AmmoModel");
    void* am = nullptr;
    if (amOff < 0 || !rd(static_cast<uint8_t*>(weapon) + amOff, &am) || !am) {
        BVR_LOG("[drop] no visible magazine on this weapon (AmmoModel %s)", amOff < 0 ? "not a property" : "empty");
        return false;
    }
    const int32_t meshOff = script::property_offset(am, L"StaticMesh");
    if (meshOff < 0 || !rd(static_cast<uint8_t*>(am) + meshOff, &g_cap.mesh) || !g_cap.mesh) {
        BVR_LOG("[drop] the visible magazine has no static mesh");
        return false;
    }
    if (!rd(static_cast<uint8_t*>(am) + patterns::kActorLocOffset, &g_cap.loc) ||
        !rd(static_cast<uint8_t*>(am) + patterns::kActorLocOffset + 12, &g_cap.rot) ||
        !rd(static_cast<uint8_t*>(am) + patterns::kActorDrawScaleOffset, &g_cap.scale))
        return false;
    if (!(g_cap.scale > 0.01f)) g_cap.scale = 1.0f;
    g_cap.valid = true;
    int32_t nameIdx = -1;
    const wchar_t* mn = rd(static_cast<uint8_t*>(g_cap.mesh) + patterns::kUObjectNameIndexOffset, &nameIdx)
                            ? patterns::fname_text(nameIdx)
                            : nullptr;
    BVR_LOG("[drop] magazine noted: mesh %S at %.1f %.1f %.1f, size %.3f", mn ? mn : L"?", g_cap.loc[0], g_cap.loc[1],
            g_cap.loc[2], g_cap.scale);
    return true;
}

bool drop(void* weapon, const float velocity[3]) {
    if (!g_cap.valid || !weapon) return false;
    const Captured c = g_cap;
    g_cap.valid = false;
    void* cls = load_class(weapon, kPropClass);
    if (!cls) {
        BVR_LOG("[drop] REFUSED: %S did not load", kPropClass);
        return false;
    }
    // Actor.Spawn(class, owner, tag, location, rotation, bNoCollisionFail, label) -> Actor.
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
    } sp{cls, nullptr, {0, 0}, {c.loc[0], c.loc[1], c.loc[2]}, {c.rot[0], c.rot[1], c.rot[2]}, 1, {0, 0}, nullptr, {}};
    if (!script::call(weapon, "Spawn", &sp) || !sp.ret) {
        BVR_LOG("[drop] REFUSED: Spawn returned nothing");
        return false;
    }
    void* a = sp.ret;
    // Its physics is built for the mesh it has, so: off, the magazine's mesh and size, then Havok.
    set_physics(a, 0);
    struct {
        void* mesh;
        int32_t pad[3];
    } sm{c.mesh, {}};
    const bool meshOk = script::call(a, "SetStaticMesh", &sm);
    struct {
        float s;
        int32_t pad[3];
    } ds{c.scale, {}};
    script::call(a, "SetDrawScale", &ds);
    const bool havokOk = set_physics(a, 1); // PHYS_Havok
    const float v[3] = {velocity[0], velocity[1], velocity[2] - kFallUu};
    set_vector(a, "HavokSetLinearVelocity", v);
    const float spin[3] = {1.5f, -2.0f, 0.8f}; // radians per second: a tumble, not a slide
    set_vector(a, "HavokSetAngularVelocity", spin);
    const int32_t lifeOff = script::property_offset(a, L"LifeSpan");
    if (lifeOff >= 0) wr(static_cast<uint8_t*>(a) + lifeOff, kLifeSpanS);
    struct {
        uint32_t ret;
        int32_t pad[3];
    } act{};
    const bool active = script::call(a, "HavokIsActive", &act) && act.ret != 0;
    BVR_LOG("[drop] the magazine falls: %p (mesh %s, Havok %s, %s), gone in %.0f s", a, meshOk ? "set" : "NOT set",
            havokOk ? "on" : "NOT on", active ? "simulating" : "NOT simulating", kLifeSpanS);
    if (!meshOk || !active) {
        // A prop that would hang in the air (or show the class's cube) is worse than none: remove it,
        // and the caller lets the gun's own part fall instead.
        uint8_t none[32] = {};
        script::call(a, "Destroy", none);
        BVR_LOG("[drop] the copy is removed - the gun's own part falls instead");
        return false;
    }
    return true;
}

} // namespace bvr::b1r::dropped_part
