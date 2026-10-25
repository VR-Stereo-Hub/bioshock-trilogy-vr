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

// 2026-10-25: the active plasmids (every ShockGame class extending ActivePlasmid), by the class name the game's
// own GEP / TestAddAvailablePlasmid take.
const char* const kPlasmids[] = {"ElectricBolt",   "Incineration",   "IcicleAssault",      "Telekinesis",
                                 "InsectSwarmPlasmid", "AirBlast",    "BerserkRage",        "DecoyHuman",
                                 "SecurityBeacon", "SpringboardTrap", "SummonProtector"};
constexpr int kActiveTrack = 1;   // Plasmid.ePlasmidTrack TRACK_Active
constexpr int kMaxTrackSlots = 6; // GEP's own cap

std::atomic<int> g_request{0}; // 0 none, 1 all weapons, 2 all weapons upgraded
std::atomic<bool> g_plasmidRequest{false};
std::atomic<bool> g_infiniteEve{false};
uint64_t g_eveMs = 0;
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

// 2026-10-25: every active plasmid, as the game's own GEP command gives one (ShockCheatManager.GEP): made available
// through ShockPlayer.TestAddAvailablePlasmid (which quiets the inventory warnings itself), the active track's
// slots unlocked up to its six, and the first six equipped through TestEquipPlasmid. The rest wait in the Gene Bank.
void give_plasmids(void* pawn) {
    int added = 0, equipped = 0;
    struct NameParm {
        int32_t name[2];
        int32_t pad[6];
    };
    for (const char* pl : kPlasmids) {
        const int32_t nm = script::name_index(pl);
        if (nm < 0) {
            BVR_LOG("[cheats] plasmid name '%s' is not in this build's names", pl);
            continue;
        }
        NameParm p{{nm, 0}, {}};
        script::call(pawn, "TestAddAvailablePlasmid", &p);
        // The Test functions are refused in the censored build: then the plain one the Test function wraps.
        struct {
            int32_t name[2];
            uint32_t ret;
            int32_t pad[5];
        } q{{nm, 0}, 0, {}};
        if (script::call(pawn, "IsPlasmidAvailable", &q) && !q.ret) {
            NameParm p2{{nm, 0}, {}};
            script::call(pawn, "AddAvailablePlasmid", &p2);
            q.ret = 0;
            script::call(pawn, "IsPlasmidAvailable", &q);
        }
        if (q.ret) ++added;
        else BVR_LOG("[cheats] %s did not become available", pl);
    }
    for (int i = 0; i < kMaxTrackSlots; ++i) {
        struct {
            uint8_t track;
            uint8_t pad[3];
            int32_t ret;
            int32_t more[6];
        } q{static_cast<uint8_t>(kActiveTrack), {}, 0, {}};
        if (!script::call(pawn, "GetNumTrackSlots", &q) || q.ret >= kMaxTrackSlots) break;
        struct {
            uint8_t track;
            uint8_t pad[3];
            int32_t more[7];
        } u{static_cast<uint8_t>(kActiveTrack), {}, {}};
        script::call(pawn, "UnlockTrackSlot", &u);
    }
    for (int s = 0; s < kMaxTrackSlots && s < static_cast<int>(sizeof kPlasmids / sizeof kPlasmids[0]); ++s) {
        const int32_t nm = script::name_index(kPlasmids[s]);
        if (nm < 0) continue;
        struct {
            int32_t name[2];
            int32_t slot;
            int32_t pad[5];
        } e{{nm, 0}, s, {}};
        script::call(pawn, "TestEquipPlasmid", &e);
        struct {
            int32_t name[2];
            uint32_t ret;
            int32_t pad[5];
        } q{{nm, 0}, 0, {}};
        if (script::call(pawn, "IsPlasmidEquipped", &q) && !q.ret) {
            decltype(e) e2{{nm, 0}, s, {}};
            script::call(pawn, "EquipPlasmid", &e2);
            q.ret = 0;
            script::call(pawn, "IsPlasmidEquipped", &q);
        }
        if (q.ret) ++equipped;
    }
    snprintf(g_status, sizeof g_status, "Plasmids: %d made available, %d equipped (the active track holds six; the "
             "rest are in the Gene Bank).", added, equipped);
    BVR_LOG("[cheats] %s", g_status);
}

// 2026-10-25: infinite EVE - once a second, EVE topped up to its maximum through the game's own AddBioAmmo, so the
// HUD's bar and every EVE check see it.
void top_up_eve(void* pawn) {
    struct {
        float ret;
        int32_t pad[7];
    } cur{}, mx{};
    if (!script::call(pawn, "GetBioAmmo", &cur) || !script::call(pawn, "GetMaxBioAmmo", &mx)) return;
    if (!(mx.ret > 0.0f) || cur.ret >= mx.ret - 0.01f) return;
    struct {
        float amount;
        int32_t pad[7];
    } a{mx.ret - cur.ret, {}};
    script::call(pawn, "AddBioAmmo", &a);
}

} // namespace

void request_all_plasmids() {
    g_plasmidRequest.store(true, std::memory_order_relaxed);
    snprintf(g_status, sizeof g_status, "Giving every plasmid...");
}
void set_infinite_eve(bool on) {
    if (g_infiniteEve.exchange(on, std::memory_order_relaxed) != on)
        BVR_LOG("[cheats] infinite EVE %s", on ? "ON" : "off");
}
bool infinite_eve() { return g_infiniteEve.load(std::memory_order_relaxed); }

void tick_hands(void* handsActor) {
    if (!handsActor || !script::ready()) return;
    const bool plasmids = g_plasmidRequest.load(std::memory_order_relaxed);
    const bool eve = g_infiniteEve.load(std::memory_order_relaxed);
    if (!plasmids && !eve) return;
    const uint64_t now = GetTickCount64();
    if (!plasmids && now - g_eveMs < 1000) return;
    const int32_t off = script::property_offset(handsActor, L"PawnOwner");
    void* pawn = nullptr;
    if (off < 0 || !rd(static_cast<uint8_t*>(handsActor) + off, &pawn) || !is_player(pawn)) return;
    if (plasmids) {
        g_plasmidRequest.store(false, std::memory_order_relaxed);
        give_plasmids(pawn);
    }
    if (eve) {
        g_eveMs = now;
        top_up_eve(pawn);
    }
}

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
    if (!theme::section("Plasmids and EVE")) return;
    if (theme::button("Give all plasmids")) request_all_plasmids();
    theme::note("Every active plasmid made available, as the game's own developer command gives one, and the first six "
                "equipped (the most the plasmid track holds). The rest are in the Gene Bank. Saving keeps them.");
    bool eve = infinite_eve();
    if (ImGui::Checkbox("Infinite EVE", &eve)) set_infinite_eve(eve);
    theme::note("EVE is filled back to full every second. Not saved: off again after a restart.");
}

} // namespace bvr::b1r::cheats
