#pragma once
// BS1: the weapon and plasmid wheels as the HUD movie sees them, rebuilt from the
// HUD movie's own calls (wheel_state.cpp hooks the one routine every call goes
// through, tools\ida\fw1/fw2). PURE: no engine, no globals - shared with tools/tests.
//
// It mirrors HUDRadial.swf's ActionScript (ENGINE_NOTES "The weapon and plasmid
// wheels"):
//   AddWeapon / AddAbility (classID, ...)  a new class takes the first empty index;
//                                          a known one is only updated
//   RemoveWeapon / RemoveAbility (classID) spliced out, the rest close up
//   EmptyWeapon / EmptyAbility             everything goes
//   OpenWeaponMenu  (RB pressed)           the weapon wheel shows when the plasmid
//                                          wheel is not up and 2+ weapons are held;
//                                          with 1 the bumper just equips it, with 0
//                                          nothing happens
//   RBReleased                             it closes (the weapon commits)
//   OpenAbilityMenu / LBReleased           the same for plasmids
//   InitializeMovie                        a new HUD movie: everything resets
//   SelectWeapon / SelectAbility (classID) the equipped item (for the log)

#include <cstring>

namespace bvr::b1r::wheel {

constexpr int kSlots = 8; // MaxWeapons = MaxAbilities = 8
enum Kind { kWeapon = 0, kPlasmid = 1 };

inline void copy_id(char* dst, size_t n, const char* src) {
    size_t i = 0;
    for (; i + 1 < n && src[i]; ++i) dst[i] = src[i];
    dst[i] = 0;
}

struct List {
    char item[kSlots][48] = {};
    int count = 0;
    char selected[48] = {};
    int find(const char* id) const {
        for (int i = 0; i < count; ++i)
            if (std::strcmp(item[i], id) == 0) return i;
        return -1;
    }
    bool add(const char* id) {
        if (!id || !*id || find(id) >= 0 || count >= kSlots) return false;
        copy_id(item[count], sizeof item[0], id);
        ++count;
        return true;
    }
    bool remove(const char* id) {
        const int at = find(id);
        if (at < 0) return false;
        for (int i = at; i + 1 < count; ++i) std::memcpy(item[i], item[i + 1], sizeof item[0]);
        --count;
        item[count][0] = 0;
        return true;
    }
    void clear() { *this = List{}; }
};

struct Mirror {
    List list[2];
    int open = -1;       // the wheel on screen: kWeapon, kPlasmid, or -1
    bool known = false;  // a HUD call has been seen since the movie started
    unsigned generation = 0; // bumps on every change, for the log and the readers

    // One HUD call. `arg0` is the first string argument when the call has one.
    // Returns true when the state changed.
    bool on_call(const char* method, const char* arg0) {
        if (!method) return false;
        const char* a = arg0 ? arg0 : "";
        bool changed = false;
        if (!std::strcmp(method, "InitializeMovie")) {
            list[0].clear(); list[1].clear(); open = -1; changed = true;
        } else if (!std::strcmp(method, "AddWeapon")) {
            changed = list[kWeapon].add(a);
        } else if (!std::strcmp(method, "AddAbility")) {
            changed = list[kPlasmid].add(a);
        } else if (!std::strcmp(method, "RemoveWeapon")) {
            changed = list[kWeapon].remove(a);
        } else if (!std::strcmp(method, "RemoveAbility")) {
            changed = list[kPlasmid].remove(a);
        } else if (!std::strcmp(method, "EmptyWeapon")) {
            changed = list[kWeapon].count != 0; list[kWeapon].clear();
        } else if (!std::strcmp(method, "EmptyAbility")) {
            changed = list[kPlasmid].count != 0; list[kPlasmid].clear();
        } else if (!std::strcmp(method, "OpenWeaponMenu")) {
            if (open != kPlasmid && list[kWeapon].count >= 2 && open != kWeapon) { open = kWeapon; changed = true; }
        } else if (!std::strcmp(method, "OpenAbilityMenu")) {
            if (open != kWeapon && list[kPlasmid].count >= 2 && open != kPlasmid) { open = kPlasmid; changed = true; }
        } else if (!std::strcmp(method, "RBReleased")) {
            if (open == kWeapon) { open = -1; changed = true; }
        } else if (!std::strcmp(method, "LBReleased")) {
            if (open == kPlasmid) { open = -1; changed = true; }
        } else if (!std::strcmp(method, "SelectWeapon") || !std::strcmp(method, "SelectAbility")) {
            List& l = list[method[6] == 'W' ? kWeapon : kPlasmid];
            if (std::strcmp(l.selected, a) != 0) {
                copy_id(l.selected, sizeof l.selected, a);
                changed = true;
            }
        } else {
            return false;
        }
        known = true;
        if (changed) ++generation;
        return changed;
    }
};

// True for the methods on_call reads: the hook's cheap first filter.
inline bool interesting(const char* m) {
    if (!m) return false;
    switch (m[0]) {
        case 'A': return !std::strcmp(m, "AddWeapon") || !std::strcmp(m, "AddAbility");
        case 'R': return !std::strcmp(m, "RemoveWeapon") || !std::strcmp(m, "RemoveAbility") ||
                         !std::strcmp(m, "RBReleased");
        case 'E': return !std::strcmp(m, "EmptyWeapon") || !std::strcmp(m, "EmptyAbility");
        case 'O': return !std::strcmp(m, "OpenWeaponMenu") || !std::strcmp(m, "OpenAbilityMenu");
        case 'L': return !std::strcmp(m, "LBReleased");
        case 'I': return !std::strcmp(m, "InitializeMovie");
        case 'S': return !std::strcmp(m, "SelectWeapon") || !std::strcmp(m, "SelectAbility");
        default: return false;
    }
}

} // namespace bvr::b1r::wheel
