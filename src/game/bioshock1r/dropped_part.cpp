#include "dropped_part.h"
#include "patterns.h"
#include "script_call.h"
#include "core/util/log.h"

#include <windows.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <cwchar>

namespace bvr::b1r::dropped_part {
namespace {

constexpr float kLifeSpanS = 90.0f;   // the prop removes itself after this (long enough to pile up)
constexpr float kFallUu = 60.0f;      // a short downward start, world units per second
const wchar_t* const kPropClasses[] = {L"ShockGame.PhysicalReactiveActor", L"ShockGame.NonPhysicalReactiveActor"};

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
    bool byName = false; // the mesh is not the ammo model's own (the pistol's rounds)
} g_cap;
bool g_lastRounds = false;

void* class_of(void* obj) {
    void* c = nullptr;
    return obj && rd(static_cast<uint8_t*>(obj) + patterns::kUObjectClassOffset, &c) ? c : nullptr;
}

// Object.DynamicLoadObject(string, Class, optional bool MayFail) -> Object.
void* load_object(void* any, const wchar_t* path, void* ofClass) {
    if (!ofClass) return nullptr;
    struct {
        FStr name;
        void* cls;
        uint32_t mayFail;
        void* ret;
        int32_t pad[4];
    } p{fstr(path), ofClass, 1, nullptr, {}};
    return script::call(any, "DynamicLoadObject", &p) ? p.ret : nullptr;
}
// A class by name: the class of classes is any class's own class.
void* load_class(void* any, const wchar_t* path) { return load_object(any, path, class_of(class_of(any))); }

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
    if (meshOff >= 0) rd(static_cast<uint8_t*>(am) + meshOff, &g_cap.mesh);
    g_cap.byName = false;
    if (!g_cap.mesh) {
        // The pistol's AmmoModel carries no mesh while it is hidden; its rounds are PI_AmmoType
        // (ShockGame.U, the six cartridges) - they fall instead, where the ammo model sits.
        const wchar_t* cls = patterns::object_class_name(weapon);
        if (cls && !_wcsicmp(cls, L"Pistol")) {
            // The current ammunition's own VisualAmmoModel, from its class's defaults (Weapon.uc
            // ShowAmmoModel reads `PendingAmmoSelection.default.VisualAmmoModel`). By name it would not
            // load (14:09 run: DynamicLoadObject("ShockGame.WP_Pistol.PI_AmmoType") gave nothing).
            void* ammoCls = nullptr;
            const int32_t sel = script::property_offset(weapon, L"PendingAmmoSelection");
            if (sel >= 0) rd(static_cast<uint8_t*>(weapon) + sel, &ammoCls);
            void* defs = script::class_defaults(ammoCls);
            const int32_t vam = script::class_property_offset(ammoCls, L"VisualAmmoModel");
            if (defs && vam >= 0) rd(static_cast<uint8_t*>(defs) + vam, &g_cap.mesh);
            g_cap.byName = g_cap.mesh != nullptr;
            BVR_LOG("[drop] the pistol's rounds: ammo class %s, its VisualAmmoModel %s", ammoCls ? "read" : "NOT read",
                    g_cap.mesh ? "read" : "NOT read");
        }
        if (!g_cap.mesh) {
            BVR_LOG("[drop] the visible magazine has no static mesh");
            return false;
        }
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

// Actor.Spawn(class, owner, tag, location, rotation, bNoCollisionFail, label) -> Actor, on `on`.
static void* spawn(void* on, void* cls, const Captured& c) {
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
    return script::call(on, "Spawn", &sp) ? sp.ret : nullptr;
}

// The hands' own script spawn (Hands.ApplyScriptedHandAttachment: the game's bytecode calls Spawn
// itself), then the actor taken back off the hands: Hands.ScriptedAttachment cleared so the hands
// never destroy it, detached, put where the magazine was and out of the foreground (DrawPriority 0).
static void* spawn_through_hands(void* hands, void* cls, const Captured& c) {
    if (!hands) return nullptr;
    struct {
        void* cls;
        int32_t bone[2];
        int32_t pad[5];
    } ap{cls, {0, 0}, {}};
    if (!script::call(hands, "ApplyScriptedHandAttachment", &ap)) return nullptr;
    const int32_t off = script::property_offset(hands, L"ScriptedAttachment");
    void* a = nullptr;
    if (off < 0 || !rd(static_cast<uint8_t*>(hands) + off, &a) || !a) return nullptr;
    wr(static_cast<uint8_t*>(hands) + off, static_cast<void*>(nullptr));
    struct {
        void* actor;
        uint32_t ret;
        int32_t pad[4];
    } dp{a, 0, {}};
    script::call(hands, "DetachFromBone", &dp);
    // And off the hands as its base: still based on them, every SetLocation was refused (01:29 run: each prop sat
    // at the hands actor's own location, "now -37846.4 500.5 7735.2" for every spawn), and a held spare moved by
    // its fields alone stayed drawn where it was made.
    struct {
        void* base;
        float floor[3];
        int32_t pad[4];
    } sb{nullptr, {0, 0, 0}, {}};
    const bool unbased = script::call(a, "SetBase", &sb);
    void* base = nullptr;
    const int32_t baseOff = script::property_offset(a, L"Base");
    if (baseOff >= 0) rd(static_cast<uint8_t*>(a) + baseOff, &base);
    BVR_LOG("[drop] taken off the hands: SetBase %s, base now %p", unbased ? "called" : "FAILED", base);
    const int32_t prio = script::property_offset(a, L"DrawPriority");
    if (prio >= 0) wr(static_cast<uint8_t*>(a) + prio, uint8_t{0}); // 1 = the hands' foreground layer
    return a;
}

// Where the magazine was. With its physics OFF: a Havok body keeps the place it was made at (the
// 12:49 run: spawned through the hands, the drum fell from the hands actor, head high).
static bool place_at(void* a, const Captured& c) {
    set_physics(a, 0);
    struct {
        float loc[3];
        uint32_t noTest;
        uint32_t ret;
        int32_t pad[3];
    } sl{{c.loc[0], c.loc[1], c.loc[2]}, 1, 0, {}};
    const bool ok = script::call(a, "SetLocation", &sl) && sl.ret != 0;
    struct {
        int32_t rot[3];
        uint32_t ret;
        int32_t pad[3];
    } sr{{c.rot[0], c.rot[1], c.rot[2]}, 0, {}};
    script::call(a, "SetRotation", &sr);
    float now[3] = {};
    rd(static_cast<uint8_t*>(a) + patterns::kActorLocOffset, &now);
    if (!ok || std::fabs(now[2] - c.loc[2]) > 1.0f) {
        // SetLocation refused (or moved it elsewhere): write the field, which Havok reads as it starts.
        wr(static_cast<uint8_t*>(a) + patterns::kActorLocOffset, c.loc);
        wr(static_cast<uint8_t*>(a) + patterns::kActorLocOffset + 12, c.rot);
    }
    BVR_LOG("[drop] placed at the magazine: SetLocation %s, now %.1f %.1f %.1f (wanted %.1f %.1f %.1f)",
            ok ? "ok" : "REFUSED", now[0], now[1], now[2], c.loc[0], c.loc[1], c.loc[2]);
    return ok;
}

bool dropped_rounds_only() { return g_lastRounds; }

Prop g_lastDrop;
Prop last_dropped() { return g_lastDrop; }

static void* level_of(void* a) {
    const int32_t off = a ? script::property_offset(a, L"Level") : -1;
    void* l = nullptr;
    return off >= 0 && rd(static_cast<uint8_t*>(a) + off, &l) ? l : nullptr;
}
static Prop identify(void* a) {
    Prop p;
    p.actor = a;
    p.cls = class_of(a);
    rd(static_cast<uint8_t*>(a) + patterns::kUObjectNameIndexOffset, &p.name);
    p.level = level_of(a);
    return p;
}

// A prop of the captured magazine where it was: spawned, its physics off, the magazine's mesh and size.
static void* make_prop(void* weapon, void* hands, const Captured& c, bool* meshOk) {
    // The 12:21 headset run: Spawn(PhysicalReactiveActor) returned nothing every time. The game spawns
    // NonPhysicalReactiveActor at runtime itself (SecurityCamera.OnKilled) - the same prop with its
    // physics off, turned on below - and the hands' script spawn is the last resort.
    void* a = nullptr;
    const char* how = nullptr;
    for (const wchar_t* path : kPropClasses) {
        void* cls = load_class(weapon, path);
        if (!cls) {
            BVR_LOG("[drop] %S did not load", path);
            continue;
        }
        if ((a = spawn(weapon, cls, c))) { how = "Spawn"; }
        else if ((a = spawn_through_hands(hands, cls, c))) { how = "the hands' script spawn"; }
        BVR_LOG("[drop] %S: %s", path, a ? how : "Spawn and the hands' script spawn both returned nothing");
        if (a) break;
    }
    if (!a) return nullptr;
    // Its physics is built for the mesh it has and where it is, so: off, moved to the magazine, the
    // magazine's mesh and size (then Havok, when it is let go).
    place_at(a, c);
    struct {
        void* mesh;
        int32_t pad[3];
    } sm{c.mesh, {}};
    *meshOk = script::call(a, "SetStaticMesh", &sm);
    struct {
        float s;
        int32_t pad[3];
    } ds{c.scale, {}};
    script::call(a, "SetDrawScale", &ds);
    return a;
}

// Havok on, moving at `velocity` plus a short fall, tumbling; gone after LifeSpan. False (and removed) when
// it would hang in the air.
static bool launch(void* a, bool meshOk, const float velocity[3]) {
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

bool drop(void* weapon, void* hands, const float velocity[3]) {
    g_lastDrop = Prop{};
    if (!g_cap.valid || !weapon) return false;
    const Captured c = g_cap;
    g_cap.valid = false;
    g_lastRounds = c.byName;
    bool meshOk = false;
    void* a = make_prop(weapon, hands, c, &meshOk);
    if (!a) return false;
    const Prop id = identify(a);
    if (!launch(a, meshOk, velocity)) return false;
    g_lastDrop = id;
    return true;
}

bool ammo_model_pose(void* weapon, float loc[3], int32_t rot[3], float* scale) {
    if (!weapon || !script::properties_ready(weapon)) return false;
    const int32_t amOff = script::property_offset(weapon, L"AmmoModel");
    void* am = nullptr;
    if (amOff < 0 || !rd(static_cast<uint8_t*>(weapon) + amOff, &am) || !am) return false;
    const int32_t meshOff = script::property_offset(am, L"StaticMesh");
    void* mesh = nullptr;
    if (meshOff < 0 || !rd(static_cast<uint8_t*>(am) + meshOff, &mesh) || !mesh) return false;
    float s = 1.0f;
    if (!rd(static_cast<uint8_t*>(am) + patterns::kActorLocOffset, reinterpret_cast<float(*)[3]>(loc)) ||
        !rd(static_cast<uint8_t*>(am) + patterns::kActorLocOffset + 12, reinterpret_cast<int32_t(*)[3]>(rot)) ||
        !rd(static_cast<uint8_t*>(am) + patterns::kActorDrawScaleOffset, &s))
        return false;
    *scale = s > 0.01f ? s : 1.0f;
    return true;
}

Prop make_held(void* weapon, void* hands) {
    if (!capture(weapon) || g_cap.byName) { // a mesh of its own only (the Tommy Gun's drum)
        g_cap.valid = false;
        return Prop{};
    }
    const Captured c = g_cap;
    g_cap.valid = false;
    bool meshOk = false;
    void* a = make_prop(weapon, hands, c, &meshOk);
    if (!a) return Prop{};
    if (!meshOk) {
        uint8_t none[32] = {};
        script::call(a, "Destroy", none);
        BVR_LOG("[drop] the spare took no mesh - removed");
        return Prop{};
    }
    // In the WORLD's draw layer, like every drop that shows (00:44 run: in the hands' foreground layer, copied
    // from the gun's magazine, the spare in the hand was never drawn), and with no collision while it is held -
    // so the game's own SetLocation moves it (its zone and render bounds with it) instead of refusing.
    const int32_t prio = script::property_offset(a, L"DrawPriority");
    if (prio >= 0) wr(static_cast<uint8_t*>(a) + prio, uint8_t{0});
    struct {
        uint32_t colActors, blockActors, blockPlayers;
        int32_t pad[3];
    } sc{0, 0, 0, {}};
    const bool collOff = script::call(a, "SetCollision", &sc);
    const Prop p = identify(a);
    BVR_LOG("[drop] a spare in the hand: %p (world layer, collision %s)", a, collOff ? "off" : "NOT changed");
    return p;
}

bool alive(const Prop& p, void* weapon) {
    if (!p.actor || !weapon) return false;
    void* cls = nullptr;
    int32_t name = -1;
    if (!rd(static_cast<uint8_t*>(p.actor) + patterns::kUObjectClassOffset, &cls) || cls != p.cls ||
        !rd(static_cast<uint8_t*>(p.actor) + patterns::kUObjectNameIndexOffset, &name) || name != p.name)
        return false;
    // The same level as the gun in hand: after a level change both are new, and the old one is freed memory.
    return p.level && level_of(p.actor) == p.level && level_of(weapon) == p.level;
}

void move(const Prop& p, const float loc[3], const int32_t rot[3]) {
    // The game's own move (its zone and render bounds follow), the fields when it refuses.
    struct {
        float loc[3];
        uint32_t noTest;
        uint32_t ret;
        int32_t pad[3];
    } sl{{loc[0], loc[1], loc[2]}, 1, 0, {}};
    if (!script::call(p.actor, "SetLocation", &sl) || !sl.ret)
        wr(static_cast<uint8_t*>(p.actor) + patterns::kActorLocOffset, *reinterpret_cast<const float(*)[3]>(loc));
    struct {
        int32_t rot[3];
        uint32_t ret;
        int32_t pad[3];
    } sr{{rot[0], rot[1], rot[2]}, 0, {}};
    if (!script::call(p.actor, "SetRotation", &sr) || !sr.ret)
        wr(static_cast<uint8_t*>(p.actor) + patterns::kActorLocOffset + 12, *reinterpret_cast<const int32_t(*)[3]>(rot));
    // Kept alive while it is moved, and gone on its own soon after it no longer is (a spare we lost track of).
    const int32_t lifeOff = script::property_offset(p.actor, L"LifeSpan");
    if (lifeOff >= 0) wr(static_cast<uint8_t*>(p.actor) + lifeOff, 20.0f);
}

bool location(const Prop& p, float loc[3], int32_t rot[3]) {
    return rd(static_cast<uint8_t*>(p.actor) + patterns::kActorLocOffset, reinterpret_cast<float(*)[3]>(loc)) &&
           rd(static_cast<uint8_t*>(p.actor) + patterns::kActorLocOffset + 12, reinterpret_cast<int32_t(*)[3]>(rot));
}

// 2026-10-20 (22:55 run: a let-go spare fell "from the position of the mag already in the gun"): the held
// prop's Havok body was built where it was spawned - at the gun's magazine - and moving its fields never
// moved that body, so turning Havok on dropped it from there. Let go, the held prop goes and a NEW drop is
// made where it is now (drop()'s own order: placed, then its mesh, then Havok), which falls from the hand.
bool release(const Prop& p, void* weapon, void* hands, const float velocity[3]) {
    Captured c;
    if (!location(p, c.loc, c.rot)) return false;
    const int32_t meshOff = script::property_offset(p.actor, L"StaticMesh");
    if (meshOff < 0 || !rd(static_cast<uint8_t*>(p.actor) + meshOff, &c.mesh) || !c.mesh) return false;
    if (!rd(static_cast<uint8_t*>(p.actor) + patterns::kActorDrawScaleOffset, &c.scale) || !(c.scale > 0.01f))
        c.scale = 1.0f;
    destroy(p);
    bool meshOk = false;
    void* a = make_prop(weapon, hands, c, &meshOk);
    g_lastDrop = Prop{};
    if (!a) return false;
    const Prop id = identify(a);
    if (!launch(a, meshOk, velocity)) return false;
    g_lastDrop = id;
    return true;
}

// Gone for certain (the 22:55 run: a caught drum went on falling from the well - Destroy alone left it):
// physics off, out of sight far below, then Destroy, and LifeSpan as the backstop.
void destroy(const Prop& p) {
    set_physics(p.actor, 0);
    float loc[3];
    if (rd(static_cast<uint8_t*>(p.actor) + patterns::kActorLocOffset, &loc)) {
        loc[2] -= 100000.0f;
        wr(static_cast<uint8_t*>(p.actor) + patterns::kActorLocOffset, loc);
    }
    const int32_t lifeOff = script::property_offset(p.actor, L"LifeSpan");
    if (lifeOff >= 0) wr(static_cast<uint8_t*>(p.actor) + lifeOff, 0.1f);
    struct {
        uint32_t ret;
        int32_t pad[3];
    } d{};
    const bool ok = script::call(p.actor, "Destroy", &d) && d.ret != 0;
    BVR_LOG("[drop] prop %p removed (Destroy %s)", p.actor, ok ? "ok" : "REFUSED - out of sight, gone on its LifeSpan");
}

} // namespace bvr::b1r::dropped_part
