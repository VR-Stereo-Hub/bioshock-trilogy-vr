// eventPlayerCalcView derivation: FName-chain scan, technique ported from
// itsloopyo/bioshock-remastered-headtracking (MIT), src/memory.rs - the
// generic implementation lives in core/hooks/pattern_scan.cpp. The logged RVA
// is the cross-check value against the Rust mod on the same exe build
// (docs/ENGINE_NOTES.md).

#include "game/bioshock1r/patterns.h"

#include "core/util/log.h"

#include <windows.h>

#include <cstring>

namespace bvr::b1r::patterns {
namespace {

// Captured by resolve() so the lazy hfov_option_ptr() can form the vtable
// address for this session's ASLR base.
const uint8_t* g_imageBase = nullptr;

// Cached UShockUserSettings instance (revalidated by vtable every call). There
// is no static pointer to it, so we find it by its fixed-RVA vtable and cache
// the heap address for the session.
void* g_userSettings = nullptr;
uint64_t g_lastScanMs = 0;

bool region_scannable(const MEMORY_BASIC_INFORMATION& mbi) {
    if (mbi.State != MEM_COMMIT) return false;
    if (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) return false;
    if (mbi.Type != MEM_PRIVATE) return false; // the object lives on the heap
    switch (mbi.Protect & 0xFF) {
        case PAGE_READWRITE:
        case PAGE_EXECUTE_READWRITE:
            return true;
        default:
            return false;
    }
}

// SEH-guarded scan of one region for the vtable dword. A region can decommit
// between VirtualQuery and the read (the game heap churns on other threads),
// so the raw walk must not fault the game. No C++ objects in this frame.
void scan_region(uintptr_t base, uintptr_t end, uintptr_t wantVtable, uint32_t needBytes,
                 ObjectAccept accept, void* user, void** outChosen, int* outMatches) {
    __try {
        for (uintptr_t a = base; a + needBytes <= end; a += 4) {
            if (*reinterpret_cast<const uintptr_t*>(a) != wantVtable) continue;
            ++*outMatches;
            void* obj = reinterpret_cast<void*>(a);
            if (!*outChosen && accept(obj, user)) *outChosen = obj;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

// Accept the first UShockUserSettings whose HorizontalFOV reads as a plausible
// degree value, logging every candidate so a wrong pick (e.g. a class default
// object) is diagnosable.
bool accept_user_settings(void* obj, void*) {
    int32_t fov = *reinterpret_cast<const int32_t*>(static_cast<uint8_t*>(obj) +
                                                    kUserSettingsHfovOffset);
    BVR_LOG("[b1r] UShockUserSettings vtable match @ %p HorizontalFOV=%d", obj, fov);
    return fov >= 40 && fov <= 170;
}

// One-shot per session in practice - the object exists by the time CalcView
// first fires.
void* scan_for_user_settings() {
    int matches = 0;
    return scan_for_vtable_object(kUserSettingsVtableRva,
                                  kUserSettingsHfovOffset + sizeof(int32_t),
                                  &accept_user_settings, nullptr, "UShockUserSettings",
                                  &matches);
}

} // namespace

void* scan_for_vtable_object(uint32_t vtableRva, uint32_t needBytes, ObjectAccept accept,
                             void* user, const char* what, int* outMatches) {
    if (!g_imageBase || !accept) return nullptr;
    const uintptr_t wantVtable = reinterpret_cast<uintptr_t>(g_imageBase) + vtableRva;
    void* chosen = nullptr;
    int matches = 0;

    uintptr_t p = 0x10000;
    MEMORY_BASIC_INFORMATION mbi{};
    while (p < 0x7FFF0000u &&
           VirtualQuery(reinterpret_cast<void*>(p), &mbi, sizeof(mbi)) == sizeof(mbi)) {
        uintptr_t base = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
        uintptr_t end = base + mbi.RegionSize;
        if (end <= base) break;
        if (region_scannable(mbi))
            scan_region(base, end, wantVtable, needBytes, accept, user, &chosen, &matches);
        p = end;
    }
    BVR_LOG("[b1r] %s scan: %d vtable match(es), chosen=%p", what, matches, chosen);
    if (outMatches) *outMatches = matches;
    return chosen;
}

int32_t* hfov_option_ptr() {
    using namespace bvr::pattern_scan;
    if (!g_imageBase) return nullptr;

    const void* wantVtable = g_imageBase + kUserSettingsVtableRva;

    // Revalidate the cache: object freed/moved -> vtable no longer matches.
    if (g_userSettings) {
        if (is_memory_valid(g_userSettings, kUserSettingsHfovOffset + sizeof(int32_t)) &&
            *reinterpret_cast<const void* const*>(g_userSettings) == wantVtable) {
            return reinterpret_cast<int32_t*>(static_cast<uint8_t*>(g_userSettings) +
                                              kUserSettingsHfovOffset);
        }
        g_userSettings = nullptr; // stale, fall through to a rescan
    }

    // Rate-limit rescans so a not-yet-created object doesn't scan every frame.
    uint64_t now = GetTickCount64();
    if (now - g_lastScanMs < 2000) return nullptr;
    g_lastScanMs = now;

    g_userSettings = scan_for_user_settings();
    if (!g_userSettings) return nullptr;
    return reinterpret_cast<int32_t*>(static_cast<uint8_t*>(g_userSettings) +
                                      kUserSettingsHfovOffset);
}

bool resolve(const bvr::pattern_scan::ProcessImage& image, Symbols& out) {
    using namespace bvr::pattern_scan;

    g_imageBase = image.base;
    BVR_LOG("[b1r] scanning main module: base %p size 0x%zX", image.base, image.size);

    EventScanResult scan{};
    bool ok = find_event_function(image, "PlayerCalcView", scan);

    BVR_LOG("[b1r] \"PlayerCalcView\": %zu wide-string match(es), %zu string xref(s)",
            scan.stringMatches, scan.stringXrefs);
    if (scan.fnameIndexGlobal) {
        BVR_LOG("[b1r] fname index global: %p (%zu xref(s), %zu candidate(s) after init-site filter)",
                scan.fnameIndexGlobal, scan.globalXrefs, scan.candidates);
    }

    if (!ok) {
        const char* stage = scan.stringMatches == 0     ? "wide string not found"
                            : scan.stringXrefs == 0     ? "no string xrefs"
                            : !scan.fnameIndexGlobal    ? "fname index global not found"
                            : scan.candidates == 0      ? "no candidates past init-site filter"
                                                        : "no valid prologue found";
        BVR_LOG("[b1r] scan FAILED (%s) - camera features disabled, game runs flat", stage);
        return false;
    }

    out.eventPlayerCalcView = scan.function;
    BVR_LOG("[b1r] eventPlayerCalcView = %p (RVA 0x%X)", scan.function,
            static_cast<unsigned>(reinterpret_cast<uintptr_t>(scan.function) -
                                  reinterpret_cast<uintptr_t>(image.base)));

    resolve_aim_natives(image, out); // fail-soft, logs each
    return true;
}

void resolve_aim_natives(const bvr::pattern_scan::ProcessImage& image, Symbols& out) {
    using namespace bvr::pattern_scan;

    // Weapon side: read the implementation straight out of the class vtable
    // (RTTI-derived vtable RVA + the slot the InitiateDamage call site uses).
    // A vtable read survives a rebuild that moves the function; the expected
    // RVA is only a cross-check.
    const void** weaponVtbl =
        reinterpret_cast<const void**>(const_cast<uint8_t*>(image.base) + kPlayerWeaponVtableRva);
    if (is_memory_valid(weaponVtbl, kWeaponFireStartVtblOffset + sizeof(void*))) {
        const uint8_t* impl = *reinterpret_cast<const uint8_t* const*>(
            reinterpret_cast<const uint8_t*>(weaponVtbl) + kWeaponFireStartVtblOffset);
        if (impl >= image.base && impl < image.base + image.size) {
            out.weaponFireStart = const_cast<uint8_t*>(impl);
            uint32_t rva = static_cast<uint32_t>(impl - image.base);
            BVR_LOG("[b1r] AWeapon::GetPerfectFireStart impl = %p (RVA 0x%X%s)", impl, rva,
                    rva == kExpectedWeaponFireStartImplRva
                        ? ""
                        : " - DIFFERS from the documented build");
        }
    }
    if (!out.weaponFireStart)
        BVR_LOG("[b1r] AWeapon::GetPerfectFireStart impl NOT resolved (vtable read failed)");

    // Same two functions one level up, for the probe's "a shot happened" line.
    if (is_memory_valid(weaponVtbl, kWeaponInitDamageVtblOffset + sizeof(void*))) {
        const uint8_t* impl = *reinterpret_cast<const uint8_t* const*>(
            reinterpret_cast<const uint8_t*>(weaponVtbl) + kWeaponInitDamageVtblOffset);
        if (impl >= image.base && impl < image.base + image.size) {
            out.weaponInitDamage = const_cast<uint8_t*>(impl);
            uint32_t rva = static_cast<uint32_t>(impl - image.base);
            BVR_LOG("[b1r] AWeapon::InitiateDamage impl = %p (RVA 0x%X%s)", impl, rva,
                    rva == kExpectedWeaponInitDamageRva ? "" : " - DIFFERS from the documented build");
        }
    }
    const uint8_t* abilityInit = image.base + kAbilityInitDamageRva;
    if (is_memory_valid(abilityInit, sizeof kAbilityInitDamagePrologue) &&
        memcmp(abilityInit, kAbilityInitDamagePrologue, sizeof kAbilityInitDamagePrologue) == 0) {
        out.abilityInitDamage = const_cast<uint8_t*>(abilityInit);
        BVR_LOG("[b1r] UAttackAbility::InitiateDamage impl = %p (RVA 0x%X)", abilityInit,
                kAbilityInitDamageRva);
    }

    // Ability side: the InitiateDamage call site calls it directly, so there is
    // no slot to read - use the documented RVA, gated on a prologue match so a
    // patched build refuses rather than hooking a stranger.
    const uint8_t* ability = image.base + kAbilityFireStartImplRva;
    if (is_memory_valid(ability, sizeof kAbilityFireStartPrologue) &&
        memcmp(ability, kAbilityFireStartPrologue, sizeof kAbilityFireStartPrologue) == 0) {
        out.abilityFireStart = const_cast<uint8_t*>(ability);
        BVR_LOG("[b1r] UAttackAbility::GetPerfectFireStart impl = %p (RVA 0x%X)", ability,
                kAbilityFireStartImplRva);
    } else {
        BVR_LOG("[b1r] UAttackAbility::GetPerfectFireStart impl prologue mismatch at RVA 0x%X"
                " - plasmid aim unavailable", kAbilityFireStartImplRva);
    }
}

} // namespace bvr::b1r::patterns
