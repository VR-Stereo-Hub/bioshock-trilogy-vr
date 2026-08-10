#include "game/bioshockinf/arsenal.h"

#include "core/hooks/pattern_scan.h"
#include "core/util/log.h"
#include "game/bioshockinf/camera.h"
#include "game/bioshockinf/patterns.h"
#include "game/bioshockinf/reflect.h"

#include <cstdio>
#include <cstring>

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
        BVR_LOG("[bsi] give: usage - bsigive <ArchetypeName|Full.Path> [ammo] | bsigive list. "
                "Bare names get the PreCoalescedItemAssets. prefix (PistolFounder, "
                "ShotgunFounder, Plasmid_DevilsKiss, ...)");
        return true;
    }

    // Rung 1: the archetype. %s already stripped the newline from `name`.
    char path[160];
    if (strchr(name, '.'))
        snprintf(path, sizeof path, "%s", name);
    else
        snprintf(path, sizeof path, "PreCoalescedItemAssets.%s", name);
    void* archetype = reflect::load_object(path);
    if (!archetype) {
        BVR_LOG("[bsi] give: FAILED at load - '%s' did not resolve", path);
        return true;
    }

    // Rung 2: acquire, unless an instance already exists (re-give = re-equip).
    void* instance = find_instance(mgr, archetype);
    if (!instance) {
        uint8_t parms[64] = {};
        memcpy(parms, &archetype, sizeof archetype);
        if (!reflect::call_on_object(pawn, "AcquireWeapon", parms)) {
            BVR_LOG("[bsi] give: FAILED at AcquireWeapon dispatch");
            return true;
        }
        instance = find_instance(mgr, archetype);
        if (!instance) {
            BVR_LOG("[bsi] give: AcquireWeapon dispatched but NO instance appeared in the "
                    "carried list - archetype %p is not grantable this way", archetype);
            return true;
        }
    }

    // Rung 3: equip (manager-side - the pawn's own EquipWeapon is a no-op,
    // measured s52).
    {
        uint8_t parms[64] = {};
        memcpy(parms, &instance, sizeof instance);
        if (!reflect::call_on_object(mgr, "EquipWeapon", parms))
            BVR_LOG("[bsi] give: EquipWeapon dispatch failed (weapon still granted)");
    }

    // Rung 4: ammo (vigors ignore it harmlessly - salts are a pawn resource).
    if (ammo > 0) {
        uint8_t parms[64] = {};
        memcpy(parms, &ammo, sizeof ammo);
        reflect::call_on_object(instance, "AddAmmo", parms);
    }

    // The measured acceptance: what does the pawn say is equipped now?
    {
        uint8_t parms[64] = {};
        reflect::call_on_object(pawn, "GetEquippedWeapon", parms);
        void* equipped = nullptr;
        memcpy(&equipped, parms + 4, sizeof equipped); // return slot after the selector
        char nm[96] = {};
        if (void* a = archetype_of(equipped)) object_name(a, nm, sizeof nm);
        BVR_LOG("[bsi] give: %s -> instance %p, equipped now = %p (%s), ammo +%d",
                name, instance, equipped, nm[0] ? nm : "?", ammo);
    }
    return true;
}

} // namespace bvr::bsi::arsenal
