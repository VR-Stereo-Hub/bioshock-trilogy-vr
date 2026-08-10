#include "game/bioshockinf/arsenal.h"

#include "core/hooks/pattern_scan.h"
#include "core/util/log.h"
#include "game/bioshockinf/camera.h"
#include "game/bioshockinf/patterns.h"
#include "game/bioshockinf/reflect.h"

#include <imgui.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <iterator>

namespace bvr::bsi::arsenal {
namespace {

// The latched PC's pawn and its inventory manager, both validity-gated. The
// fire.cpp player-gate idiom, pointed one hop deeper (kPawnInventoryMgrOffset,
// derivation in patterns.h).
void* live_pawn() {
    void* pc = camera::last_player_controller();
    if (!pc || !bvr::pattern_scan::is_memory_valid(pc, patterns::kPcPawnOffset + 4))
        return nullptr;
    void* pawn = *reinterpret_cast<void* const*>(static_cast<const uint8_t*>(pc) +
                                                 patterns::kPcPawnOffset);
    if (!pawn ||
        !bvr::pattern_scan::is_memory_valid(pawn, patterns::kPawnInventoryMgrOffset + 4))
        return nullptr;
    return pawn;
}

void* manager_of(void* pawn) {
    void* mgr = *reinterpret_cast<void* const*>(static_cast<const uint8_t*>(pawn) +
                                                patterns::kPawnInventoryMgrOffset);
    if (!mgr ||
        !bvr::pattern_scan::is_memory_valid(
            mgr, patterns::kMgrWeaponSlotsOffset + patterns::kMgrWeaponSlotMax * 4))
        return nullptr;
    return mgr;
}

// A carried entry's archetype (UObject+0x24), null on any invalid read.
void* archetype_of(void* weapon) {
    if (!weapon ||
        !bvr::pattern_scan::is_memory_valid(weapon, patterns::kUObjectArchetypeOffset + 4))
        return nullptr;
    return *reinterpret_cast<void* const*>(static_cast<const uint8_t*>(weapon) +
                                           patterns::kUObjectArchetypeOffset);
}

bool object_name(void* obj, char* out, size_t outSize) {
    out[0] = '\0';
    const int32_t ni = reflect::object_name_index(obj);
    if (ni <= 0 || ni >= patterns::fname_count()) return false;
    return patterns::fname_text(ni, out, outSize);
}

// Scan the carried list for the instance whose archetype matches.
void* find_instance(void* mgr, void* archetype) {
    for (uint32_t i = 0; i < patterns::kMgrWeaponSlotMax; ++i) {
        void* w = *reinterpret_cast<void* const*>(static_cast<const uint8_t*>(mgr) +
                                                  patterns::kMgrWeaponSlotsOffset + i * 4);
        if (!w) break; // the list is dense; first null ends it
        if (archetype_of(w) == archetype) return w;
    }
    return nullptr;
}

// The base-game roster (archetype names, ENGINE_NOTES s52 part 2). The F10
// buttons index into this; `bsigive all` walks it.
struct RosterEntry {
    const char* name;
    int ammo;
};
constexpr RosterEntry kRoster[] = {
    {"PistolFounder", 100},
    {"MachineGunFounder", 200},
    {"ShotgunFounder", 24},
    {"CarbineFounder", 90},
    {"HandCannonFounder", 30},
    {"SniperRifleFounder", 20},
    {"RPGFounder", 8},
    {"Plasmid_DevilsKiss", 0},
    {"Plasmid_EnrageFounder", 0},
    {"Plasmid_MurderOfCrowsFounder", 0},
    {"Plasmid_BuckingBroncoFounder", 0},
    {"Plasmid_UndertowFounder", 0},
    {"Plasmid_VoltSwarmFounder", 0},
    {"Plasmid_Charge", 0},
};

// F10 -> game-thread grant post: -2 idle, -1 give-all, >= 0 a roster index.
std::atomic<int> g_pendGive{-2};

// Function indices, resolved once per boot (fname_find is a whole-pool
// linear scan - the give-all path makes ~50 dispatches in one tick and
// by-name lookups would hitch seconds; the s52 stutter lesson).
int32_t fn_idx(const char* name, int32_t& slot) {
    if (slot < 0) slot = reflect::find_function_index(name);
    return slot;
}
int32_t g_idxAcquire = -1, g_idxEquip = -1, g_idxAddAmmo = -1, g_idxGetEquipped = -1;

void cmd_list(void* mgr);

bool give_one(void* pawn, void* mgr, const char* name, int ammo) {
    // Rung 1: the archetype (callers strip whitespace/newlines already).
    char path[160];
    if (strchr(name, '.'))
        snprintf(path, sizeof path, "%s", name);
    else
        snprintf(path, sizeof path, "PreCoalescedItemAssets.%s", name);
    void* archetype = reflect::load_object(path);
    if (!archetype) {
        // s52 round 2 (measured): the PreCoalescedItemAssets container is
        // populated per level; in the pre-raffle INTRO it is empty and
        // DynamicLoadObject cannot demand-load the cooked CoalescedItems
        // package either (probed: bare package, dotted path - both null).
        // Everywhere from the fair onward the loads resolve.
        BVR_LOG("[bsi] give: FAILED at load - '%s' did not resolve (early-game level "
                "with no item assets? give works from any save at/after the fair)",
                path);
        return false;
    }

    // Rung 2: acquire, unless an instance already exists (re-give = re-equip).
    void* instance = find_instance(mgr, archetype);
    if (!instance) {
        uint8_t parms[64] = {};
        memcpy(parms, &archetype, sizeof archetype);
        if (!reflect::call_on_object_by_index(pawn, fn_idx("AcquireWeapon", g_idxAcquire),
                                              parms)) {
            BVR_LOG("[bsi] give: FAILED at AcquireWeapon dispatch");
            return false;
        }
        instance = find_instance(mgr, archetype);
        if (!instance) {
            BVR_LOG("[bsi] give: AcquireWeapon dispatched but NO instance appeared in the "
                    "carried list - archetype %p is not grantable this way", archetype);
            return false;
        }
    }

    // Rung 3: equip (manager-side - the pawn's own EquipWeapon is a no-op,
    // measured s52).
    {
        uint8_t parms[64] = {};
        memcpy(parms, &instance, sizeof instance);
        if (!reflect::call_on_object_by_index(mgr, fn_idx("EquipWeapon", g_idxEquip), parms))
            BVR_LOG("[bsi] give: EquipWeapon dispatch failed (weapon still granted)");
    }

    // Rung 4: ammo (vigors ignore it harmlessly - salts are a pawn resource).
    if (ammo > 0) {
        uint8_t parms[64] = {};
        memcpy(parms, &ammo, sizeof ammo);
        reflect::call_on_object_by_index(instance, fn_idx("AddAmmo", g_idxAddAmmo), parms);
    }

    // The measured acceptance: what does the pawn say is equipped now?
    {
        uint8_t parms[64] = {};
        reflect::call_on_object_by_index(pawn, fn_idx("GetEquippedWeapon", g_idxGetEquipped),
                                         parms);
        void* equipped = nullptr;
        memcpy(&equipped, parms + 4, sizeof equipped); // return slot after the selector
        char nm[96] = {};
        if (void* a = archetype_of(equipped)) object_name(a, nm, sizeof nm);
        BVR_LOG("[bsi] give: %s -> instance %p, equipped now = %p (%s), ammo +%d", name,
                instance, equipped, nm[0] ? nm : "?", ammo);
    }
    return true;
}

void give_all(void* pawn, void* mgr) {
    int ok = 0;
    for (const RosterEntry& e : kRoster)
        if (give_one(pawn, mgr, e.name, e.ammo)) ++ok;
    BVR_LOG("[bsi] give: ALL done - %d of %d granted (grips cycle; the LAST one is "
            "equipped)", ok, static_cast<int>(std::size(kRoster)));
    cmd_list(mgr);
}

void cmd_list(void* mgr) {
    char nm[96] = {};
    void* melee = *reinterpret_cast<void* const*>(static_cast<const uint8_t*>(mgr) +
                                                  patterns::kMgrMeleeSlotOffset);
    if (void* a = archetype_of(melee)) {
        object_name(a, nm, sizeof nm);
        BVR_LOG("[bsi] give: melee  %p  %s", melee, nm[0] ? nm : "?");
    }
    for (uint32_t i = 0; i < patterns::kMgrWeaponSlotMax; ++i) {
        void* w = *reinterpret_cast<void* const*>(static_cast<const uint8_t*>(mgr) +
                                                  patterns::kMgrWeaponSlotsOffset + i * 4);
        if (!w) break;
        void* a = archetype_of(w);
        nm[0] = '\0';
        if (a) object_name(a, nm, sizeof nm);
        BVR_LOG("[bsi] give: slot %u  %p  %s", i, w, nm[0] ? nm : "?");
    }
}

} // namespace

bool handle_command(const char* cmd, const char* args) {
    if (strcmp(cmd, "bsigive") != 0) return false;
    char name[96] = {};
    int ammo = 50;
    const int n = sscanf_s(args ? args : "", "%95s %d", name,
                           static_cast<unsigned>(sizeof name), &ammo);
    void* pawn = live_pawn();
    void* mgr = pawn ? manager_of(pawn) : nullptr;
    if (!pawn || !mgr) {
        BVR_LOG("[bsi] give: REFUSED - no pawn/manager (load a save first)");
        return true;
    }
    if (n < 1 || strcmp(name, "list") == 0) {
        cmd_list(mgr);
        BVR_LOG("[bsi] give: usage - bsigive <ArchetypeName|Full.Path> [ammo] | bsigive all "
                "| bsigive list. Bare names get the PreCoalescedItemAssets. prefix "
                "(PistolFounder, ShotgunFounder, Plasmid_DevilsKiss, ...)");
        return true;
    }
    if (strcmp(name, "all") == 0) {
        give_all(pawn, mgr);
        return true;
    }
    give_one(pawn, mgr, name, ammo);
    return true;
}

void tick() {
    // s52 round 2: the F10 GIVE buttons (render thread) post here; the grant
    // itself must run on the game thread behind the reflection gates.
    const int req = g_pendGive.exchange(-2, std::memory_order_relaxed);
    if (req == -2) return;
    void* pawn = live_pawn();
    void* mgr = pawn ? manager_of(pawn) : nullptr;
    if (!pawn || !mgr) {
        BVR_LOG("[bsi] give: button REFUSED - no pawn/manager (load a save first)");
        return;
    }
    if (req == -1) {
        give_all(pawn, mgr);
    } else if (req >= 0 && req < static_cast<int>(std::size(kRoster))) {
        give_one(pawn, mgr, kRoster[req].name, kRoster[req].ammo);
    }
}

void draw_debug_ui() {
    if (!ImGui::CollapsingHeader("ARSENAL (I9 cheat)")) return;
    ImGui::TextDisabled("grants run on the next game tick; giving a gun DROPS the");
    ImGui::TextDisabled("replaced carried gun (the game's carry-2 rule)");
    if (ImGui::Button("GIVE ALL BASE WEAPONS + VIGORS"))
        g_pendGive.store(-1, std::memory_order_relaxed);
    for (int i = 0; i < static_cast<int>(std::size(kRoster)); ++i) {
        if (i % 2 == 1) ImGui::SameLine(220);
        if (ImGui::Button(kRoster[i].name)) g_pendGive.store(i, std::memory_order_relaxed);
    }
}

} // namespace bvr::bsi::arsenal
