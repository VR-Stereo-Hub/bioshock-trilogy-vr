// eventPlayerCalcView derivation: FName-chain scan, technique ported from
// itsloopyo/bioshock-remastered-headtracking (MIT), src/memory.rs - the
// generic implementation lives in core/hooks/pattern_scan.cpp. The logged RVA
// is the cross-check value against the Rust mod on the same exe build
// (docs/ENGINE_NOTES.md).

#include "game/bioshock1r/patterns.h"

#include "core/util/log.h"

#include <windows.h>

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
void scan_region(uintptr_t base, uintptr_t end, uintptr_t wantVtable, uint32_t hfovOffset,
                 void** outChosen, int* outMatches) {
    __try {
        for (uintptr_t a = base; a + hfovOffset + 4 <= end; a += 4) {
            if (*reinterpret_cast<const uintptr_t*>(a) != wantVtable) continue;
            ++*outMatches;
            int32_t fov = *reinterpret_cast<const int32_t*>(a + hfovOffset);
            BVR_LOG("[b1r] UShockUserSettings vtable match @ 0x%08X HorizontalFOV=%d",
                    static_cast<unsigned>(a), fov);
            if (!*outChosen && fov >= 40 && fov <= 170) *outChosen = reinterpret_cast<void*>(a);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

// Scan committed private memory for an object whose first dword is the
// UShockUserSettings vtable. Returns the first instance whose HorizontalFOV
// reads as a plausible degree value; logs every vtable match so a wrong pick
// (e.g. a class default object) is diagnosable. One-shot per session in
// practice - the object exists by the time CalcView first fires.
void* scan_for_user_settings() {
    const uintptr_t wantVtable =
        reinterpret_cast<uintptr_t>(g_imageBase) + kUserSettingsVtableRva;
    void* firstPlausible = nullptr;
    int matches = 0;

    uintptr_t p = 0x10000;
    MEMORY_BASIC_INFORMATION mbi{};
    while (p < 0x7FFF0000u &&
           VirtualQuery(reinterpret_cast<void*>(p), &mbi, sizeof(mbi)) == sizeof(mbi)) {
        uintptr_t base = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
        uintptr_t end = base + mbi.RegionSize;
        if (end <= base) break;
        if (region_scannable(mbi))
            scan_region(base, end, wantVtable, kUserSettingsHfovOffset, &firstPlausible, &matches);
        p = end;
    }
    BVR_LOG("[b1r] UShockUserSettings scan: %d vtable match(es), chosen=%p", matches,
            firstPlausible);
    return firstPlausible;
}

} // namespace

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

    struct Want {
        const char* cls;
        const char* fn;
        void** slot;
        uint32_t expectedRva;
    } wants[] = {
        {"AWeapon", "GetPerfectFireStart", &out.execWeaponFireStart, kExpectedWeaponFireStartRva},
        {"AWeapon", "ApplyAimError", &out.execWeaponAimError, kExpectedWeaponAimErrorRva},
        {"APawn", "GetViewPoint", &out.execPawnViewPoint, kExpectedPawnViewPointRva},
        {"APawn", "GetViewDirection", &out.execPawnViewDir, kExpectedPawnViewDirRva},
        {"AActor", "Trace", &out.execActorTrace, kExpectedActorTraceRva},
    };

    for (const Want& w : wants) {
        NativeScanResult scan{};
        if (!find_native_function(image, w.cls, w.fn, scan)) {
            BVR_LOG("[b1r] native %s::%s NOT resolved (%zu string match(es), %zu table ref(s))",
                    w.cls, w.fn, scan.stringMatches, scan.tableRefs);
            continue;
        }
        *w.slot = scan.function;
        uint32_t rva = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(scan.function) -
                                            reinterpret_cast<uintptr_t>(image.base));
        BVR_LOG("[b1r] native %s::%s = %p (RVA 0x%X%s)", w.cls, w.fn, scan.function, rva,
                rva == w.expectedRva ? "" : " - DIFFERS from the documented build");
    }
}

} // namespace bvr::b1r::patterns
