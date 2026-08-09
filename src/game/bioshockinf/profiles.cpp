#include "game/bioshockinf/profiles.h"

#include <cstring>

#include "core/util/log.h"
#include "game/bioshockinf/reflect.h"

namespace bvr::bsi::profiles {
namespace {

// The table. Empty by design this session (see the header); I9 fills it from
// headset-derived numbers against the arsenal save.
constexpr Profile kTable[] = {
    {nullptr},
};

void* g_lastWeapon = reinterpret_cast<void*>(~0u); // never-seen sentinel
char g_lastClass[64] = "(no shot yet)";            // fname_text needs >= 64
uint32_t g_latches = 0;

} // namespace

const Profile* find(const char* className) {
    if (!className || !className[0]) return nullptr;
    for (const Profile& p : kTable)
        if (p.className && strcmp(p.className, className) == 0) return &p;
    return nullptr;
}

void note_weapon_object(void* weaponObj) {
    if (weaponObj == g_lastWeapon) return;
    g_lastWeapon = weaponObj;
    ++g_latches;
    if (!weaponObj) {
        strcpy_s(g_lastClass, "(none - seam param null)");
        return;
    }
    if (!reflect::class_name_of(weaponObj, g_lastClass, sizeof g_lastClass) ||
        !g_lastClass[0])
        strcpy_s(g_lastClass, "(unreadable)");
}

bool handle_command(const char* cmd, const char* args) {
    (void)args;
    if (strcmp(cmd, "bsiprofiles") != 0) return false;
    size_t entries = 0;
    for (const Profile& p : kTable)
        if (p.className) ++entries;
    BVR_LOG("[bsi] profiles: SCAFFOLD - %u entries (values arrive with the I9 arsenal "
            "save) | last weapon class '%s' (%u pointer latches) -> %s",
            static_cast<unsigned>(entries), g_lastClass, g_latches,
            find(g_lastClass) ? "profile found" : "no override");
    return true;
}

} // namespace bvr::bsi::profiles
