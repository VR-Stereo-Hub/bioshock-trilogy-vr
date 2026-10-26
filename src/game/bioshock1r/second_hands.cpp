#include "game/bioshock1r/second_hands.h"

#include "core/util/log.h"
#include "game/bioshock1r/hands_state.h"
#include "game/bioshock1r/own_ability.h"
#include "game/bioshock1r/patterns.h"
#include "game/bioshock1r/script_call.h"

#include <windows.h>
#include <intrin.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cwchar>
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
// Second hands whose Destroy did not take (destroy): still collapsed in the skin gather, destroyed again each tick.
constexpr int kMaxZombies = 8;
void* volatile g_zombie[kMaxZombies] = {};
volatile int g_zombieN = 0;
void* g_main = nullptr;             // the main hands it was made beside
std::atomic<bool> g_rightDown{false};
bool g_rightWas = false;
bool g_armed[2] = {false, false};   // a fire state entered and its cast not yet seen: [0] the game's hands, [1] ours
bool g_wasFiring[2] = {false, false};
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

void drop_object(void* ability);
void* prop_ptr(void* obj, const wchar_t* name);

// 2026-10-25: Actor.Destroy refused the second hands every time (Destroy returned 0, bDeleteMe clear). Its C++,
// ULevel::DestroyActor (patterns.h), refuses an actor whose Role is not ROLE_Authority unless the call forces it,
// and the script's Destroy never does. This is the same engine call with bNetForce set - the game is single-player,
// so there is no network copy to answer to. False with `role`/`flags` read back when it still refuses.
using DestroyActorFn = int(__thiscall*)(void* level, void* actor, int netForce, int);
bool destroy_actor(void* a, int* role, uint32_t* flags) {
    uint8_t r = 0;
    uint32_t f = 0;
    rd(static_cast<uint8_t*>(a) + patterns::kActorRoleOffset, &r);
    rd(static_cast<uint8_t*>(a) + patterns::kActorDestroyFlagsOffset, &f);
    *role = r;
    *flags = f;
    void* level = nullptr;
    void** vt = nullptr;
    DestroyActorFn fn = nullptr;
    const auto* image = reinterpret_cast<const uint8_t*>(GetModuleHandleW(nullptr));
    if (!rd(static_cast<uint8_t*>(a) + patterns::kActorXLevelOffset, &level) || !level || !rd(level, &vt) || !vt ||
        !rd(reinterpret_cast<uint8_t*>(vt) + patterns::kLevelDestroyActorVtbl, &fn) ||
        reinterpret_cast<const uint8_t*>(fn) != image + patterns::kLevelDestroyActorRva)
        return false; // not the level this build's table describes: nothing is called
    __try {
        if (!fn(level, a, 1, 0)) return false;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    bool deleted = false;
    return script::get_bool(a, L"bDeleteMe", &deleted) && deleted;
}

// The plasmid's effect on `rig` taken away (Hands.SetAbilityEffects: the 'SelectedAbility' event and its persistent
// context, keyed by the ability's class name), as the script's own ability change does - before the hands go, so no
// idle effect is left behind (the 16:03 run's right-hand electro glow with a gun in it).
void take_ability_effects(void* rig) {
    void* const ab = prop_ptr(rig, L"CurrentAbility");
    void* const cls = class_of(ab);
    int32_t cn = -1;
    if (!cls || !rd(static_cast<uint8_t*>(cls) + patterns::kUObjectNameIndexOffset, &cn) || cn <= 0) return;
    const int32_t ev = script::name_index("SelectedAbility");
    struct {
        int32_t ev[2];
        int32_t tag[2];
        int32_t pad[4];
    } ut{{ev, 0}, {cn, 0}, {}};
    const bool u = ev >= 0 && script::call(rig, "UnTriggerEffectEvent", &ut);
    // The persistent context is the effects system's, keyed by class name alone: while the game's hands hold the same
    // plasmid in hand, it is theirs too, and stays.
    void* const mainAb = g_main ? prop_ptr(g_main, L"CurrentAbility") : nullptr;
    bool weaponMode = true;
    const bool leftHasIt = mainAb && class_of(mainAb) == cls && g_main &&
                           script::get_bool(g_main, L"bIsInWeaponMode", &weaponMode) && !weaponMode;
    bool r = false;
    if (!leftHasIt) {
        struct {
            int32_t ctx[2];
            int32_t pad[6];
        } rc{{cn, 0}, {}};
        r = script::call(rig, "RemovePersistentContext", &rc);
    }
    // Its own flag goes off with it (an object of its own, or the Telekinesis object it took - never the left's).
    if (ab != mainAb) script::set_bool(ab, L"AbilityEffectsTriggered", false);
    BVR_LOG("[hands2] the second hands' plasmid effect taken away: %s, %s", u ? "untriggered" : "NOT untriggered",
            leftHasIt ? "its context kept (the left hand holds the same plasmid)" : r ? "its context removed"
                                                                                  : "context NOT removed");
}

void destroy(const char* why) {
    void* rig = g_rig.exchange(nullptr, std::memory_order_relaxed);
    if (!rig) return;
    drop_object(prop_ptr(rig, L"CurrentAbility")); // a right-hand Telekinesis hold lets go first
    take_ability_effects(rig);
    int role = 0;
    uint32_t flags = 0;
    // Its PostBeginPlay spawned a gatherer tool of its own (Hands.PostBeginPlay): that goes with it.
    if (void* tool = prop_ptr(rig, L"GathererTool")) destroy_actor(tool, &role, &flags);
    // One that still does not go stays on the list below: its draw stays collapsed (is_leftover) and the destroy is
    // tried again.
    const bool gone = destroy_actor(rig, &role, &flags);
    BVR_LOG("[hands2] the second hands destroyed (%s): %s (Role %d, flags 0x%X) | casts %u, releases %u, plasmid sets %u",
            why, gone ? "done" : "REFUSED - kept collapsed, tried again every 2 s", role, flags, g_casts, g_releases,
            g_equips);
    if (!gone) {
        bool known = false;
        for (int i = 0; i < g_zombieN; ++i) known = known || g_zombie[i] == rig;
        if (!known && g_zombieN < kMaxZombies) g_zombie[g_zombieN++] = rig;
    }
    g_main = nullptr;
    g_rightWas = false;
}

// The destroys that did not take, tried again (game thread, tick) - every 2 s, not every frame: per-frame retries cost
// the 16:03 run 20-30 fps while a gun was out.
void retry_zombies() {
    static uint64_t s_ms = 0;
    const uint64_t now = GetTickCount64();
    if (now - s_ms < 2000) return;
    s_ms = now;
    for (int i = 0; i < g_zombieN;) {
        void* z = g_zombie[i];
        bool deleted = false;
        int role = 0;
        uint32_t flags = 0;
        if (!script::get_bool(z, L"bDeleteMe", &deleted) || deleted || destroy_actor(z, &role, &flags)) {
            BVR_LOG("[hands2] a second hands actor that had refused to go is gone now");
            g_zombie[i] = g_zombie[--g_zombieN];
            continue;
        }
        ++i;
    }
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
    if (script::call(mainHands, "Spawn", &sp) && sp.ret) return sp.ret;
    // Actor.Spawn by name returns nothing in this build (the 23:24 simulator run; dropped_part.cpp met the same):
    // the hands' own script spawn instead - Hands.ApplyScriptedHandAttachment spawns the class in the game's
    // bytecode - then taken back off the hands: ScriptedAttachment cleared so they never destroy it, detached and
    // un-based so its own UpdateLocation moves it.
    struct {
        void* cls;
        int32_t bone[2];
        int32_t pad[5];
    } ap{cls, {0, 0}, {}};
    if (!script::call(mainHands, "ApplyScriptedHandAttachment", &ap)) return nullptr;
    const int32_t off = script::property_offset(mainHands, L"ScriptedAttachment");
    void* a = nullptr;
    if (off < 0 || !rd(static_cast<uint8_t*>(mainHands) + off, &a) || !a) return nullptr;
    wr(static_cast<uint8_t*>(mainHands) + off, static_cast<void*>(nullptr));
    struct {
        void* actor;
        uint32_t ret;
        int32_t pad[4];
    } dp{a, 0, {}};
    script::call(mainHands, "DetachFromBone", &dp);
    struct {
        void* base;
        float floor[3];
        int32_t pad[4];
    } sb{nullptr, {0, 0, 0}, {}};
    script::call(a, "SetBase", &sb);
    struct {
        void* owner;
        int32_t pad[7];
    } so{pawn, {}};
    script::call(a, "SetOwner", &so);
    BVR_LOG("[hands2] spawned through the hands' own script spawn (Actor.Spawn by name gave nothing)");
    return a;
}

// A class in one of the player's TArray<Class<Ability>> properties (AvailableAbilities: the ones they own;
// PossibleAbilities: every one), the best by `score(name)` (0 = no).
template <typename Score>
void* class_in(void* pawn, const wchar_t* prop, Score score) {
    const int32_t off = script::property_offset(pawn, prop);
    struct {
        void** data;
        int32_t num, max;
    } arr{};
    if (off < 0 || !rd(static_cast<uint8_t*>(pawn) + off, &arr) || !arr.data || arr.num <= 0 || arr.num > 256)
        return nullptr;
    void* best = nullptr;
    int bestScore = 0;
    for (int32_t i = 0; i < arr.num; ++i) {
        void* c = nullptr;
        int32_t ni = -1;
        if (!rd(arr.data + i, &c) || !c || !rd(static_cast<uint8_t*>(c) + patterns::kUObjectNameIndexOffset, &ni))
            continue;
        const wchar_t* n = patterns::fname_text(ni);
        const int s = n ? score(n) : 0;
        if (s > bestScore) {
            bestScore = s;
            best = c;
        }
    }
    return best;
}

// The RightPlasmid row's families, by class-name prefix (ShockPlayer.PossibleAbilities: <prefix>[Zero|Two|Three]
// Ability, one class per upgrade level).
constexpr int kFamilyCount = 12;
const wchar_t* const kFamilies[kFamilyCount] = {
    nullptr,          L"Incineration", L"ElectricBolt",   L"IcicleAssault",  L"Telekinesis",    L"AirBlast",
    L"InsectSwarm",   L"BerserkRage",  L"SecurityBeacon", L"DecoyHuman",     L"SpringBoardTrap", L"SummonProtector"};
std::atomic<int> g_rightFamily{0};

// The player's owned class of a family, the highest level first (Three, Two, the base, Zero).
void* family_class(void* pawn, const wchar_t* prefix) {
    const size_t pl = wcslen(prefix);
    return class_in(pawn, L"AvailableAbilities", [prefix, pl](const wchar_t* n) {
        if (wcsncmp(n, prefix, pl)) return 0;
        const wchar_t* rest = n + pl;
        return !wcscmp(rest, L"ThreeAbility") ? 4
               : !wcscmp(rest, L"TwoAbility") ? 3
               : !wcscmp(rest, L"Ability")    ? 2
               : !wcscmp(rest, L"ZeroAbility") ? 1
                                               : 0;
    });
}

// ShockPlayer.GetAbilityFromClass: the player's one ability object of a class (made on first use).
void* ability_for(void* pawn, void* cls) {
    struct {
        void* cls;
        void* ret;
        int32_t pad[6];
    } g{cls, nullptr, {}};
    return script::call(pawn, "GetAbilityFromClass", &g) ? g.ret : nullptr;
}

bool is_telekinesis(void* ability) {
    const wchar_t* cn = ability ? patterns::object_class_name(ability) : nullptr;
    return cn && !wcsncmp(cn, L"Telekinesis", 11);
}

// A Telekinesis object's hold let go (TelekinesisAbility.DropObject, native).
void drop_object(void* ability) {
    if (!is_telekinesis(ability)) return;
    struct {
        uint32_t ret;
        int32_t pad[7];
    } p{0, {}};
    script::call(ability, "DropObject", &p);
}

// The second Telekinesis object is not the player's ACTIVE ability, so the player's own tick never runs its state
// (TELEKINESIS_RE.md section 4): it is ticked here, at the game's own 30 Hz step, with its own accumulator.
float g_tkAcc = 0.0f;
uint64_t g_tkMs = 0;
uint32_t g_tkTicks = 0;
void tick_telekinesis(void* ability, uint64_t now) {
    const float dt = g_tkMs ? (now - g_tkMs) / 1000.0f : 0.0f;
    g_tkMs = now;
    if (!is_telekinesis(ability)) {
        g_tkAcc = 0.0f;
        return;
    }
    void* target = nullptr;
    void* state = nullptr;
    if (!rd(static_cast<uint8_t*>(ability) + patterns::kTkTargetOffset, &target) ||
        !rd(static_cast<uint8_t*>(ability) + patterns::kTkStateOffset, &state) || !target || !state) {
        g_tkAcc = 0.0f;
        return;
    }
    g_tkAcc += dt > 0.25f ? 0.25f : dt;
    if (g_tkAcc < 1.0f / 30.0f) return;
    void** vt = nullptr;
    void* slot1 = nullptr;
    if (!rd(state, &vt) || !vt || !rd(vt + 1, &slot1) || !slot1) return;
    using TickFn = void(__thiscall*)(void*, void*, float);
    __try {
        reinterpret_cast<TickFn>(slot1)(state, ability, g_tkAcc);
        ++g_tkTicks;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        BVR_LOG("[hands2] the right hand's Telekinesis tick FAULTED");
    }
    g_tkAcc = 0.0f;
}

const wchar_t* name_of(const void* obj) {
    int32_t ni = -1;
    return obj && rd(static_cast<const uint8_t*>(obj) + patterns::kUObjectNameIndexOffset, &ni) ? patterns::fname_text(ni)
                                                                                                 : nullptr;
}

// WHAT THE RIGHT HAND HOLDS: a plasmid class - the F10 RightPlasmid row's family (the player's highest owned level),
// else the class the left hand holds - and, for Telekinesis only, the player's object it takes instead of one of its
// own. Telekinesis keeps a native state object beside the script one (TELEKINESIS_RE.md), proven only on the player's
// objects: so the right hand takes the player's object of a Telekinesis class the left hand does not hold
// (TelekinesisTwoAbility when the left holds TelekinesisAbility - config only, the same native code). Every other
// plasmid gets an object of its own (own_ability.h).
struct RightChoice {
    void* cls = nullptr;
    void* shared = nullptr;
};
RightChoice right_choice(void* pawn, void* mainAb) {
    RightChoice c;
    if (!mainAb) return c;
    void* const mainCls = class_of(mainAb);
    c.cls = mainCls;
    const int fam = g_rightFamily.load(std::memory_order_relaxed);
    if (fam > 0 && fam < kFamilyCount) {
        void* cls = family_class(pawn, kFamilies[fam]);
        static void* s_logged = reinterpret_cast<void*>(1);
        if (s_logged != cls) {
            s_logged = cls;
            const wchar_t* cn = name_of(cls);
            if (cls) BVR_LOG("[hands2] the right hand's plasmid: %ls (the %ls family)", cn ? cn : L"?", kFamilies[fam]);
            else
                BVR_LOG("[hands2] the right hand's plasmid: the player owns nothing of the %ls family - it takes the "
                        "left's", kFamilies[fam]);
        }
        if (cls) c.cls = cls;
    }
    const wchar_t* cn = name_of(c.cls);
    if (cn && !wcsncmp(cn, L"Telekinesis", 11)) {
        const bool sameAsLeft = c.cls == mainCls;
        const wchar_t* take = !sameAsLeft ? cn : !wcscmp(cn, L"TelekinesisAbility") ? L"TelekinesisTwoAbility"
                                                                                       : L"TelekinesisAbility";
        void* cls = sameAsLeft ? class_in(pawn, L"PossibleAbilities", [take](const wchar_t* n) { return !wcscmp(n, take) ? 1 : 0; })
                               : c.cls;
        void* ab = cls ? ability_for(pawn, cls) : nullptr;
        static void* s_logged = reinterpret_cast<void*>(1);
        if (s_logged != ab) {
            s_logged = ab;
            BVR_LOG("[telekinesis] the right hand's object: the player's %ls -> %p%s", take, ab,
                    ab ? "" : " - not found: the right hand gets one of its own");
        }
        if (ab && ab != mainAb) {
            c.cls = cls;
            c.shared = ab;
        }
    }
    return c;
}

// The second hands at the game's hands' DrawScale. The game's player hands stand at 0.8 (ENGINE_NOTES: the foreground
// rig path scales their bone translations and their draw by it); a script Spawn of the same class gives 1.0. Left
// unmatched, the right arm and every effect on the second hands' bones came out at a different size from the left
// (the 2026-10-25 builds compensated in the draw instead). SetDrawScale is the engine's own change, dirty flags and
// all; logged when it changes something.
void match_scale(void* rig, void* mainHands) {
    float want = 0.0f, have = 0.0f;
    if (!rd(static_cast<uint8_t*>(mainHands) + patterns::kActorDrawScaleOffset, &want) ||
        !rd(static_cast<uint8_t*>(rig) + patterns::kActorDrawScaleOffset, &have) || !(want > 0.01f && want < 100.0f) ||
        fabsf(have - want) < 1e-4f)
        return;
    struct {
        float s;
        int32_t pad[3];
    } ds{want, {}};
    const bool called = script::call(rig, "SetDrawScale", &ds);
    float now = 0.0f;
    rd(static_cast<uint8_t*>(rig) + patterns::kActorDrawScaleOffset, &now);
    static uint64_t s_logMs = 0;
    const uint64_t t = GetTickCount64();
    if (fabsf(now - want) < 1e-4f || t - s_logMs >= 5000) s_logMs = t;
    else return; // a change that does not take is logged every 5 s, not every frame
    BVR_LOG("[hands2] the second hands' DrawScale %.3f -> %.3f, the game's hands' (%s)", have, now,
            called && fabsf(now - want) < 1e-4f ? "matched" : called ? "the call ran, NOT matched" : "the call FAILED");
}

// Its hand effect is checked this long after it settles into idling with a new plasmid.
constexpr uint64_t kEffectsSettleMs = 400;
uint64_t g_effectsCheckMs = 0;

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

bool is_leftover(const void* owner) {
    if (!owner) return false;
    for (int i = 0; i < g_zombieN && i < kMaxZombies; ++i)
        if (g_zombie[i] == owner) return true;
    return false;
}

void set_right_plasmid(int family) {
    if (family < 0 || family >= kFamilyCount) family = 0;
    if (g_rightFamily.exchange(family, std::memory_order_relaxed) != family)
        BVR_LOG("[hands2] the right hand's plasmid: %S", family ? kFamilies[family] : L"the same as the left's");
}

int right_plasmid() { return g_rightFamily.load(std::memory_order_relaxed); }

bool is_right_ability(const void* ability) {
    void* rig = g_rig.load(std::memory_order_relaxed);
    if (!rig || !ability || !g_main) return false;
    void* rab = prop_ptr(rig, L"CurrentAbility");
    return rab == ability && rab != prop_ptr(g_main, L"CurrentAbility");
}

void set_right_trigger(bool down) { g_rightDown.store(down, std::memory_order_relaxed); }

void tick(void* mainHands, bool wanted) {
    // Re-entered from inside one of its own script calls (they evaluate the hands, which runs the drive): nothing.
    static bool s_in = false;
    if (s_in) return;
    struct Guard {
        bool& f;
        explicit Guard(bool& b) : f(b) { f = true; }
        ~Guard() { f = false; }
    } guard(s_in);
    if (g_zombieN) retry_zombies();
    void* rig = g_rig.load(std::memory_order_relaxed);
    if (rig && (!wanted || mainHands != g_main)) {
        // A new main hands is a new level (the old actors are the level's to free): forget, never touch.
        if (mainHands != g_main) {
            g_rig.store(nullptr, std::memory_order_relaxed);
            own_ability::forget_all();
            BVR_LOG("[hands2] the main hands changed (a level change): the second hands forgotten");
        } else {
            destroy("dual plasmids off, no plasmid in hand, or not in gameplay");
        }
        rig = nullptr;
    }
    // 2026-10-25 (the 18:29 run: after a while the plasmid wheel would no longer switch from a gun to the plasmids):
    // ShockPlayer.SwitchHandModes switches to the plasmids only while LastAbility is one the player has
    // (GetAbilityClassIndex >= 0), and OnAbilityEquippingFinished sets LastAbility to the ability whose equip just
    // finished - called by the SECOND hands too, the player being their owner. So the right hand's equip could leave
    // LastAbility on its own class (with Telekinesis, the player's TelekinesisTwoAbility, which the player does not
    // have) and the wheel then only re-equipped the gun. Only the second hands' own writes are undone: a change of
    // LastAbility to the class the SECOND hands hold, while the game's hands hold another, goes back to the value before
    // it. (The 20:24 run's first guard held LastAbility on the game's hands' class every frame, and lagged a wheel pick
    // by a frame - each pick was put back to the plasmid before it.)
    if (mainHands && script::ready()) {
        static void* s_accepted = nullptr;
        void* pw = prop_ptr(mainHands, L"PawnOwner");
        void* la = pw ? prop_ptr(pw, L"LastAbility") : nullptr;
        void* const live = g_rig.load(std::memory_order_relaxed);
        void* const rigCls = live ? class_of(prop_ptr(live, L"CurrentAbility")) : nullptr;
        void* const mainCls = class_of(prop_ptr(mainHands, L"CurrentAbility"));
        if (pw && la != s_accepted) {
            if (la && la == rigCls && la != mainCls && s_accepted && set_prop_ptr(pw, L"LastAbility", s_accepted)) {
                const wchar_t* was = patterns::fname_text([&] {
                    int32_t ni = -1;
                    rd(static_cast<uint8_t*>(la) + patterns::kUObjectNameIndexOffset, &ni);
                    return ni;
                }());
                const wchar_t* back = patterns::fname_text([&] {
                    int32_t ni = -1;
                    rd(static_cast<uint8_t*>(s_accepted) + patterns::kUObjectNameIndexOffset, &ni);
                    return ni;
                }());
                BVR_LOG("[hands2] the right hand's equip set the player's LastAbility to %ls - put back to %ls, so the "
                        "wheel switches to the left hand's plasmid",
                        was ? was : L"?", back ? back : L"?");
            } else {
                s_accepted = la;
            }
        }
    }
    if (!wanted || !mainHands || !script::ready()) return;
    void* pawn = prop_ptr(mainHands, L"PawnOwner");
    if (!pawn) return;
    const uint64_t now = GetTickCount64();
    if (!rig) {
        if (now - g_spawnFailMs < 2000) return;
        // One that refused to be destroyed is taken back rather than a new one made beside it (in gameplay; it is
        // still owned by the player).
        if (g_zombieN > 0) {
            rig = g_zombie[--g_zombieN];
            bool deleted = false;
            if (script::get_bool(rig, L"bDeleteMe", &deleted) && !deleted) {
                BVR_LOG("[hands2] the second hands that refused to go are taken back: %p", rig);
            } else {
                rig = nullptr;
            }
        }
        if (!rig) rig = spawn(mainHands, pawn);
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
    match_scale(rig, mainHands);
    // Its plasmid: the main hands', unless the F10 RightPlasmid row picks another family for the right hand (the
    // mix-and-match step): then the player's own ability object of their class in that family, from the game's
    // GetAbilityFromClass - its own state, cast and effects. SetActiveAbility takes it only while no change is in
    // flight (OldAbility none).
    void* const mainAb = prop_ptr(mainHands, L"CurrentAbility");
    void* const have = prop_ptr(rig, L"CurrentAbility");
    const RightChoice c = right_choice(pawn, mainAb);
    // The held object is kept while it is the one this choice makes: of the wanted class AND the right hand's own
    // (made for it, or the Telekinesis object it takes) - never the player's object the game's hands hold.
    const bool keep = have && class_of(have) == c.cls && (c.shared ? have == c.shared : own_ability::is_own(have));
    if (c.cls && !keep && !prop_ptr(rig, L"OldAbility")) {
        void* obj = c.shared;
        if (!obj) obj = own_ability::make(c.cls, pawn);
        if (!obj) { // the constructor is not there: the player's object, shared as before
            obj = ability_for(pawn, c.cls);
            BVR_LOG("[hands2] the right hand SHARES the player's %ls object (no object of its own could be made)",
                    name_of(c.cls) ? name_of(c.cls) : L"?");
        }
        if (obj && obj != have) {
            if (have) drop_object(have); // a hold the old object had goes first
            // A fresh hold: its hand effect is off until this hands actor's own script turns it on. (Never on an
            // object the game's hands may hold: their flag is theirs.)
            if (own_ability::is_own(obj) || obj == c.shared) script::set_bool(obj, L"AbilityEffectsTriggered", false);
            struct {
                void* ability;
                int32_t pad[7];
            } p{obj, {}};
            if (script::call(rig, "SetActiveAbility", &p)) {
                ++g_equips;
                g_effectsCheckMs = now;
                const wchar_t* cn = patterns::object_class_name(obj);
                BVR_LOG("[hands2] the second hands take the plasmid %ls (object %p, %s)", cn ? cn : L"?", obj,
                        own_ability::is_own(obj) ? "the right hand's own" : "the player's");
            }
        }
    }
    // Its hand effect, checked once it has settled into idling: an own object whose effect never came on is turned on
    // by the hands' own SetAbilityEffects (the script's path: the 'SelectedAbility' event, its persistent context).
    {
        void* rab = prop_ptr(rig, L"CurrentAbility");
        const wchar_t* st = hands_state::current_name(rig);
        const bool idling = st && !wcsncmp(st, L"AbilityIdling", 13);
        bool on = true;
        if (!idling || !rab || prop_ptr(rig, L"OldAbility")) {
            g_effectsCheckMs = now;
        } else if (now >= g_effectsCheckMs + kEffectsSettleMs &&
                   script::get_bool(rab, L"AbilityEffectsTriggered", &on) && !on) {
            uint8_t none[32] = {};
            const bool called = script::call(rig, "SetAbilityEffects", none);
            script::get_bool(rab, L"AbilityEffectsTriggered", &on);
            BVR_LOG("[hands2] the right hand's plasmid effect was still off after it settled - turned on: %s",
                    called && on ? "done" : called ? "the call ran, the effect is still off" : "the call FAILED");
            g_effectsCheckMs = now + 5000; // then not again for 5 s
        }
    }
    // The right hand's own Telekinesis object, ticked as the player ticks the active one.
    void* rab = prop_ptr(rig, L"CurrentAbility");
    if (rab && rab != prop_ptr(mainHands, L"CurrentAbility")) tick_telekinesis(rab, now);
    // Each hands entering its fire state arms one cast (on_stack).
    {
        void* both[2] = {mainHands, rig};
        for (int i = 0; i < 2; ++i) {
            const wchar_t* st = hands_state::current_name(both[i]);
            const bool firing = st && !wcsncmp(st, L"AbilityFiring", 13);
            if (firing && !g_wasFiring[i]) g_armed[i] = true;
            g_wasFiring[i] = firing;
        }
    }
    // Diagnostics: EVE as it changes, with each hands' state at that moment (so a test reads which cast cost what),
    // and every 2 s the second hands' state and plasmid.
    {
        struct {
            float ret;
            int32_t pad[7];
        } eve{};
        if (script::call(pawn, "GetBioAmmo", &eve)) {
            static float s_eve = -1.0f;
            if (s_eve >= 0.0f && eve.ret != s_eve) {
                const wchar_t* sm = hands_state::current_name(mainHands);
                const wchar_t* sr = hands_state::current_name(rig);
                BVR_LOG("[hands2] EVE %.2f -> %.2f (%+.2f) | left hands %S | right hands %S", s_eve, eve.ret,
                        eve.ret - s_eve, sm ? sm : L"?", sr ? sr : L"?");
            }
            s_eve = eve.ret;
        }
        static uint64_t s_ms = 0;
        if (now - s_ms >= 2000) {
            s_ms = now;
            const wchar_t* sr = hands_state::current_name(rig);
            void* ab = prop_ptr(rig, L"CurrentAbility");
            const wchar_t* cn = ab ? patterns::object_class_name(ab) : nullptr;
            BVR_LOG("[hands2] state %S, plasmid %S, right trigger %s | casts %u, releases %u", sr ? sr : L"?",
                    cn ? cn : L"none", g_rightDown.load() ? "DOWN" : "up", g_casts, g_releases);
            // Telekinesis: what each hand's object holds (its Target), and the right one's own ticks.
            void* lab = prop_ptr(mainHands, L"CurrentAbility");
            if (is_telekinesis(ab) || is_telekinesis(lab)) {
                void* lt = nullptr;
                void* rt = nullptr;
                if (is_telekinesis(lab)) rd(static_cast<uint8_t*>(lab) + patterns::kTkTargetOffset, &lt);
                if (is_telekinesis(ab) && ab != lab) rd(static_cast<uint8_t*>(ab) + patterns::kTkTargetOffset, &rt);
                const wchar_t* ln = lt ? patterns::object_class_name(lt) : nullptr;
                const wchar_t* rn = rt ? patterns::object_class_name(rt) : nullptr;
                float lp[3] = {}, rp[3] = {};
                if (lt) rd(static_cast<uint8_t*>(lt) + patterns::kActorLocOffset, &lp);
                if (rt) rd(static_cast<uint8_t*>(rt) + patterns::kActorLocOffset, &rp);
                // The state's kind byte (TELEKINESIS_RE.md: state+4; 0 idle, 1 pulling, 2 holding, 3 throwing, 4
                // dropping).
                auto kind = [](void* a) -> const char* {
                    static const char* const kNames[5] = {"idle", "PULLING", "HOLDING", "THROWING", "dropping"};
                    void* st = nullptr;
                    uint8_t k = 0xFF;
                    if (!a || !rd(static_cast<uint8_t*>(a) + patterns::kTkStateOffset, &st) || !st ||
                        !rd(static_cast<uint8_t*>(st) + 4, &k) || k > 4)
                        return "-";
                    return kNames[k];
                };
                BVR_LOG("[telekinesis] LEFT %s %p (%S) at %.0f %.0f %.0f %s| RIGHT %s %p (%S) at %.0f %.0f %.0f %s"
                        "| right ticks %u",
                        is_telekinesis(lab) ? kind(lab) : "-", lt, ln ? ln : L"nothing", lp[0], lp[1], lp[2],
                        ab == lab ? "(SHARED with the right) " : "", ab != lab ? kind(ab) : "-", rt,
                        rn ? rn : L"nothing", rp[0], rp[1], rp[2], lt && lt == rt ? "SAME TARGET " : "", g_tkTicks);
            }
        }
    }
    // The right trigger: its own press and release, as the game's input gives the main hands theirs.
    const bool down = g_rightDown.load(std::memory_order_relaxed);
    if (down != g_rightWas) {
        // The edge is taken BEFORE the call: UseActiveAbility makes the game evaluate the hands, which runs the drive
        // and this tick again - the 23:36 simulator run cast 598 times on one pull, every nested tick a new "press",
        // until the EVE was gone.
        g_rightWas = down;
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
    }
}

bool ptr_on_stack(const void* p) {
    const uintptr_t want = reinterpret_cast<uintptr_t>(p);
    if (!want) return false;
    const NT_TIB* tib = reinterpret_cast<const NT_TIB*>(NtCurrentTeb());
    const uintptr_t* sp = reinterpret_cast<const uintptr_t*>(_AddressOfReturnAddress());
    const uintptr_t* top = reinterpret_cast<const uintptr_t*>(tib->StackBase);
    for (int i = 0; i < 16384 && sp + i < top; ++i)
        if (sp[i] == want) return true;
    return false;
}

bool nearest_is_rig();

bool on_stack(const void* ability) {
    // WHICH HANDS IS CASTING. With a different ability object in each hand (two plasmids, or Telekinesis in both),
    // the casting ability says it: the right hand's own object is the right's cast. Telekinesis in the left keeps
    // the game's hands in their fire state for the whole hold, so their armed flag alone would claim the right's
    // bolt (the 01:02 simulator run). Otherwise each hands actor entering its fire state arms one cast (tick,
    // below); a cast takes the armed one. Only when both are armed does the stack decide (the nearer of the two
    // pointers). The stack alone fails: both pointers sit above every cast, and their order is not the caller's
    // (the 23:51 and 23:54 simulator runs - a two-handed cast read as two right-hand casts, then a right-hand cast
    // as the left's).
    void* rig = g_rig.load(std::memory_order_relaxed);
    if (!rig) return false;
    void* rab = prop_ptr(rig, L"CurrentAbility");
    void* lab = g_main ? prop_ptr(g_main, L"CurrentAbility") : nullptr;
    if (ability && rab && lab && rab != lab && (ability == rab || ability == lab)) {
        const bool right = ability == rab;
        g_armed[right ? 1 : 0] = false;
        return right;
    }
    const bool r = g_armed[1], l = g_armed[0];
    bool right;
    if (r && !l) right = true;
    else if (l && !r) right = false;
    else right = nearest_is_rig();
    g_armed[right ? 1 : 0] = false;
    return right;
}

bool nearest_is_rig() {
    const uintptr_t rig = reinterpret_cast<uintptr_t>(g_rig.load(std::memory_order_relaxed));
    if (!rig) return false;
    const uintptr_t main = reinterpret_cast<uintptr_t>(g_main);
    const NT_TIB* tib = reinterpret_cast<const NT_TIB*>(NtCurrentTeb());
    const uintptr_t* sp = reinterpret_cast<const uintptr_t*>(_AddressOfReturnAddress());
    const uintptr_t* top = reinterpret_cast<const uintptr_t*>(tib->StackBase);
    for (int i = 0; i < 16384 && sp + i < top; ++i) {
        if (sp[i] == rig) return true;
        if (main && sp[i] == main) return false;
    }
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
