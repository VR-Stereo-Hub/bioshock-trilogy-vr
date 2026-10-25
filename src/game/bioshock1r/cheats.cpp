#include "cheats.h"
#include "menu_theme.h"
#include "patterns.h"
#include "script_call.h"
#include "core/util/log.h"

#include <windows.h>
#include <imgui.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <cwchar>

namespace bvr::b1r::cheats {
namespace {

// ShockCheatManager's own lists (its defaultproperties WeaponName / ItemName), less the film, the
// hypos and the money: the weapons and what they fire.
const wchar_t* const kWeapons[] = {L"Wrench",         L"Pistol",          L"Shotgun",        L"Crossbow",
                                   L"GrenadeLauncher", L"MachineGun",     L"ChemicalThrower", L"ResearchCamera"};
const wchar_t* const kAmmo[] = {
    L"ShockGame.Pistol_Bullet",           L"ShockGame.Pistol_AntiPersonnel",
    L"ShockGame.Pistol_ArmorPiercing",    L"ShockGame.Shotgun_00Buck",
    L"ShockGame.Shotgun_IonicBuck",       L"ShockGame.Shotgun_HighExplosiveBuck",
    L"ShockGame.Crossbow_Bolt",           L"ShockGame.Crossbow_SuperHeatedBolt",
    L"ShockGame.Crossbow_TrapBolt",       L"ShockGame.ChemicalThrower_Kerosene",
    L"ShockGame.ChemicalThrower_IonicGel", L"ShockGame.ChemicalThrower_LiquidNitrogen",
    L"ShockGame.GrenadeLauncher_FragGrenade", L"ShockGame.GrenadeLauncher_StickyGrenade",
    L"ShockGame.GrenadeLauncher_RPG",     L"ShockGame.Film",
    L"ShockGame.MachineGun_Bullet",       L"ShockGame.MachineGun_FrozenBullet",
    L"ShockGame.MachineGun_ArmorPiercingBullet"};
constexpr int kAmmoAmount = 999; // GiveAll's: the inventory caps each type at its own maximum
constexpr int kSlots = 40;       // GiveAll's UnlockInventorySlots

std::atomic<int> g_request{0}; // 0 none, 1 all weapons, 2 all weapons upgraded
std::atomic<int> g_lastGiven{-1}, g_lastUpgraded{-1};
char g_status[160] = "";

// UE2's FString as a parameter: the engine copies it into the call's frame and only reads ours.
struct FStr {
    const wchar_t* data;
    int32_t num, max; // characters including the terminator
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

bool is_player(void* pawn) {
    void* vt = nullptr;
    const auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    return pawn && rd(pawn, &vt) && reinterpret_cast<uintptr_t>(vt) == base + patterns::kShockPlayerVtableRva;
}

void* holdable_by_class(void* pawn, const wchar_t* cls) {
    char narrow[64] = {};
    for (int i = 0; i < 63 && cls[i]; ++i) narrow[i] = static_cast<char>(cls[i]);
    const int32_t nm = script::name_index(narrow);
    if (nm < 0) return nullptr;
    struct {
        int32_t name[2];
        void* ret;
        int32_t pad[5];
    } p{{nm, 0}, nullptr, {}};
    return script::call(pawn, "GetHoldableByClassName", &p) ? p.ret : nullptr;
}

// PlayerWeapon.PostBeginPlay's New Game Plus branch, for one weapon: every upgrade stat allocated,
// and its attachment shown. Returns how many stats it upgraded (0: already, or none to upgrade).
int upgrade(void* weapon) {
    const int32_t statsOff = script::property_offset(weapon, L"UpgradeStats");
    if (statsOff < 0) return 0;
    struct {
        void** data;
        int32_t num, max;
    } arr{};
    if (!rd(static_cast<uint8_t*>(weapon) + statsOff, &arr) || !arr.data || arr.num <= 0 || arr.num > 8) return 0;
    int done = 0;
    for (int i = 0; i < arr.num; ++i) {
        void* stat = nullptr;
        if (!rd(arr.data + i, &stat) || !stat) continue;
        const int32_t ptsOff = script::property_offset(stat, L"PointsAllocated");
        int32_t pts = 0;
        if (ptsOff < 0 || !rd(static_cast<uint8_t*>(stat) + ptsOff, &pts) || pts >= 1) continue;
        if (!wr(static_cast<uint8_t*>(stat) + ptsOff, int32_t{1})) continue;
        uint8_t none[32] = {};
        script::call(weapon, i == 0 ? "GiveAltFireMod" : "GiveStrictlySuperiorMod", none);
        ++done;
    }
    return done;
}

void run(void* pawn, bool upgraded) {
    struct {
        int32_t num;
        int32_t pad[7];
    } slots{kSlots, {}};
    script::call(pawn, "UnlockInventorySlots", &slots);
    int given = 0, have = 0;
    for (const wchar_t* w : kWeapons) {
        if (holdable_by_class(pawn, w)) {
            ++have;
            continue;
        }
        wchar_t path[64];
        swprintf_s(path, L"ShockGame.%s", w);
        struct {
            FStr name;
            int32_t pad[5];
        } p{fstr(path), {}};
        if (script::call(pawn, "GiveWeapon", &p) && holdable_by_class(pawn, w)) {
            ++given;
            BVR_LOG("[cheats] gave the %S", w);
        } else {
            BVR_LOG("[cheats] could not give the %S", w);
        }
    }
    for (const wchar_t* a : kAmmo) {
        struct {
            int32_t amount;
            FStr name;
            int32_t pad[4];
        } p{kAmmoAmount, fstr(a), {}};
        script::call(pawn, "GiveItem", &p);
    }
    int ups = 0, upWeapons = 0;
    if (upgraded) {
        if (!script::properties_ready(pawn)) {
            BVR_LOG("[cheats] upgrades skipped: script properties by name are unavailable on this build");
        } else {
            for (const wchar_t* w : kWeapons) {
                void* weapon = holdable_by_class(pawn, w);
                const int n = weapon ? upgrade(weapon) : 0;
                if (n) {
                    ups += n;
                    ++upWeapons;
                    BVR_LOG("[cheats] upgraded the %S (%d upgrade(s))", w, n);
                }
            }
        }
    }
    g_lastGiven.store(given, std::memory_order_relaxed);
    g_lastUpgraded.store(ups, std::memory_order_relaxed);
    snprintf(g_status, sizeof g_status, "Gave %d weapon(s) (%d already held) and full ammunition%s", given, have,
             upgraded ? (ups ? "; every upgrade applied." : "; nothing left to upgrade.") : ".");
    BVR_LOG("[cheats] %s (%d upgrade(s) on %d weapon(s))", g_status, ups, upWeapons);
}

} // namespace

void request_all_weapons(bool upgraded) {
    g_request.store(upgraded ? 2 : 1, std::memory_order_relaxed);
    snprintf(g_status, sizeof g_status, "Waiting for a weapon in your hand...");
}

void tick(void* holdable) {
    const int req = g_request.load(std::memory_order_relaxed);
    if (!req || !holdable || !script::ready()) return;
    void* pawn = nullptr;
    if (!rd(static_cast<uint8_t*>(holdable) + patterns::kWeaponOwnerOffset, &pawn) || !is_player(pawn)) return;
    g_request.store(0, std::memory_order_relaxed);
    run(pawn, req == 2);
}

void draw_panel() {
    namespace theme = menu::theme;
    if (!theme::section("Weapons")) return;
    if (theme::button("Give all weapons")) request_all_weapons(false);
    ImGui::SameLine();
    if (theme::button("Give all weapons, upgraded")) request_all_weapons(true);
    theme::note("Every weapon you do not have yet, with a full load of every ammunition type, through the game's "
                "own developer functions. Upgraded: both upgrades on every weapon, as New Game Plus starts them. "
                "Runs as soon as a weapon is in your hand. Saving keeps them.");
    if (*g_status) ImGui::TextWrapped("%s", g_status);
}

} // namespace bvr::b1r::cheats
