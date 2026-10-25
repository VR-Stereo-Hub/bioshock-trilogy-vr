#include "game/bioshock1r/second_hands.h"

#include "core/util/log.h"
#include "game/bioshock1r/hands_state.h"
#include "game/bioshock1r/patterns.h"
#include "game/bioshock1r/script_call.h"

#include <windows.h>
#include <intrin.h>

#include <atomic>
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

void destroy(const char* why) {
    void* rig = g_rig.exchange(nullptr, std::memory_order_relaxed);
    if (!rig) return;
    drop_object(prop_ptr(rig, L"CurrentAbility")); // a right-hand Telekinesis hold lets go first
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
    // Its plasmid: the main hands', unless the F10 RightPlasmid row picks another family for the right hand (the
    // mix-and-match step): then the player's own ability object of their class in that family, from the game's
    // GetAbilityFromClass - its own state, cast and effects. SetActiveAbility takes it only while no change is in
    // flight (OldAbility none).
    void* const mainAb = prop_ptr(mainHands, L"CurrentAbility");
    void* want = mainAb;
    const int fam = g_rightFamily.load(std::memory_order_relaxed);
    if (mainAb && fam > 0 && fam < kFamilyCount) {
        void* cls = family_class(pawn, kFamilies[fam]);
        void* ab = cls ? ability_for(pawn, cls) : nullptr;
        static void* s_logged = reinterpret_cast<void*>(1);
        if (s_logged != ab) {
            s_logged = ab;
            const wchar_t* an = ab ? patterns::object_class_name(ab) : nullptr;
            if (ab)
                BVR_LOG("[hands2] the right hand's plasmid: %S (the %S family, its own object %p)", an ? an : L"?",
                        kFamilies[fam], ab);
            else
                BVR_LOG("[hands2] the right hand's plasmid: the player owns nothing of the %S family - it keeps the "
                        "left's", kFamilies[fam]);
        }
        if (ab) want = ab;
    }
    // 2026-10-25 (TELEKINESIS_RE.md section 3): Telekinesis keeps its target and hold on the ability OBJECT, one per
    // class per player, so two holds need two objects. With Telekinesis in both hands the right hand takes the
    // player's object of the OTHER Telekinesis class (TelekinesisTwoAbility: config only, the same native code).
    // Found by name in the player's own ShockPlayer.PossibleAbilities (loading it by path gives nothing - 00:35 run).
    if (want && want == mainAb && is_telekinesis(want)) {
        const wchar_t* cn = patterns::object_class_name(want);
        const bool two = cn && !wcscmp(cn, L"TelekinesisAbility");
        const wchar_t* other = two ? L"TelekinesisTwoAbility" : L"TelekinesisAbility";
        static void* s_cls[2] = {nullptr, nullptr};
        void*& cls = s_cls[two ? 1 : 0];
        if (!cls)
            cls = class_in(pawn, L"PossibleAbilities", [other](const wchar_t* n) { return !wcscmp(n, other) ? 1 : 0; });
        void* ab = cls ? ability_for(pawn, cls) : nullptr;
        static void* s_logged = reinterpret_cast<void*>(1);
        if (s_logged != ab) {
            s_logged = ab;
            BVR_LOG("[telekinesis] the right hand's own object: %S -> %p%s", other, ab,
                    ab ? "" : " - not found: the right hand shares the left's object");
        }
        if (ab && ab != want) want = ab;
    }
    void* have = prop_ptr(rig, L"CurrentAbility");
    if (want && want != have && !prop_ptr(rig, L"OldAbility")) {
        // A hold the old object had goes first.
        if (have) drop_object(have);
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
