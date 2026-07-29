#include "game/bioshock2r/patterns.h"

#include "core/util/log.h"

#include <windows.h>

#include <cstring>

namespace bvr::b2r::patterns {
namespace {

// Captured by resolve() so the heap scanner can form vtable addresses for
// this session's ASLR base.
const uint8_t* g_imageBase = nullptr;

// Cached UShockUserSettings instance (revalidated by vtable every call).
// Like BS1 there is no known static pointer to it; unlike BS1's scanner this
// one goes DORMANT after 3 straight misses (scan-hygiene rule from the BS1
// session-22 weapon-resolver lesson: backoff alone still reads as "the game
// freezes every couple of seconds" on saves where the object never appears).
void* g_userSettings = nullptr;
uint64_t g_lastScanMs = 0;
int g_scanMisses = 0;
bool g_scanDormant = false;

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

// Follow one incremental-link jmp stub (E9 rel32). Returns the input when it
// is not a stub. BS2's vtables and inlined call sites route through a stub
// table (RVAs ~0x10000-0x40000); one hop reaches the real body.
const uint8_t* follow_jmp_stub(const uint8_t* p) {
    if (!bvr::pattern_scan::is_memory_valid(p, 5) || p[0] != 0xE9) return p;
    int32_t rel = 0;
    memcpy(&rel, p + 1, sizeof(rel));
    return p + 5 + rel;
}

bool prologue_matches(const uint8_t* fn, const uint8_t* expect, size_t n, const char* what) {
    if (!bvr::pattern_scan::is_memory_valid(fn, n)) {
        BVR_LOG("[b2r] %s prologue unreadable at %p", what, fn);
        return false;
    }
    if (memcmp(fn, expect, n) != 0) {
        BVR_LOG("[b2r] %s prologue MISMATCH at %p: %02X %02X %02X %02X %02X - build "
                "differs, refusing to hook",
                what, fn, fn[0], fn[1], fn[2], fn[3], fn[4]);
        return false;
    }
    return true;
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
    // Walk the FULL 4 GB range: the game is Large-Address-Aware and allocates
    // actors above 2 GB after a while (BS1 session-18 lesson, baked into this
    // game's notes up front). VirtualQuery fails past the top on non-LAA
    // processes, so the loop still terminates there.
    while (p < 0xFFFE0000u &&
           VirtualQuery(reinterpret_cast<void*>(p), &mbi, sizeof(mbi)) == sizeof(mbi)) {
        uintptr_t base = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
        uintptr_t end = base + mbi.RegionSize;
        if (end <= base) break;
        if (region_scannable(mbi))
            scan_region(base, end, wantVtable, needBytes, accept, user, &chosen, &matches);
        p = end;
    }
    BVR_LOG("[b2r] %s scan: %d vtable match(es), chosen=%p", what, matches, chosen);
    if (outMatches) *outMatches = matches;
    return chosen;
}

namespace {

// Accept the first UShockUserSettings whose HorizontalFOV reads as a
// plausible degree value, logging every candidate so a wrong pick (stack
// slot, class default object) is diagnosable from the session log.
bool accept_user_settings(void* obj, void*) {
    int32_t fov = *reinterpret_cast<const int32_t*>(static_cast<uint8_t*>(obj) +
                                                    kUserSettingsHfovOffset);
    BVR_LOG("[b2r] UShockUserSettings vtable match @ %p HorizontalFOV=%d", obj, fov);
    return fov >= 40 && fov <= 170;
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
        hfov_scan_rearm("cached object went stale");
    }

    if (g_scanDormant) return nullptr;

    // Rate-limit rescans so a not-yet-created object doesn't scan every frame.
    uint64_t now = GetTickCount64();
    if (now - g_lastScanMs < 2000) return nullptr;
    g_lastScanMs = now;

    int matches = 0;
    g_userSettings = scan_for_vtable_object(kUserSettingsVtableRva,
                                            kUserSettingsHfovOffset + sizeof(int32_t),
                                            &accept_user_settings, nullptr,
                                            "UShockUserSettings", &matches);
    if (!g_userSettings) {
        if (++g_scanMisses >= 3) {
            g_scanDormant = true;
            BVR_LOG("[b2r] UShockUserSettings scan DORMANT after %d misses (a view-state "
                    "change re-arms it)",
                    g_scanMisses);
        }
        return nullptr;
    }
    g_scanMisses = 0;
    return reinterpret_cast<int32_t*>(static_cast<uint8_t*>(g_userSettings) +
                                      kUserSettingsHfovOffset);
}

void hfov_scan_rearm(const char* why) {
    g_scanMisses = 0;
    if (g_scanDormant) {
        g_scanDormant = false;
        BVR_LOG("[b2r] UShockUserSettings scan re-armed (%s)", why);
    }
}

bool verify_draw_chain(const bvr::pattern_scan::ProcessImage& image) {
    using namespace bvr::pattern_scan;
    const uint8_t* slot = image.base + kGameEngineVtableRva + kDrawVtblByteOffset;
    if (!is_memory_valid(slot, sizeof(void*))) {
        BVR_LOG("[b2r] engine vtable Draw slot unreadable (RVA 0x%X+0x%X)",
                kGameEngineVtableRva, kDrawVtblByteOffset);
        return false;
    }
    const uint8_t* stub = *reinterpret_cast<const uint8_t* const*>(slot);
    const uint8_t* draw = follow_jmp_stub(stub);
    if (draw != image.base + kSceneBuildRva) {
        BVR_LOG("[b2r] Draw via engine vtbl+0x%X = %p (stub %p) does NOT match expected "
                "RVA 0x%X - build differs, refusing",
                kDrawVtblByteOffset, draw, stub, kSceneBuildRva);
        return false;
    }
    if (!prologue_matches(draw, kSceneBuildPrologue, sizeof(kSceneBuildPrologue),
                          "UGameEngine::Draw"))
        return false;
    BVR_LOG("[b2r] UGameEngine::Draw chain VERIFIED: vtbl 0x%X slot +0x%X -> stub %p -> "
            "body %p (RVA 0x%X)",
            kGameEngineVtableRva, kDrawVtblByteOffset, stub, draw, kSceneBuildRva);
    return true;
}

bool resolve(const bvr::pattern_scan::ProcessImage& image, Symbols& out) {
    using namespace bvr::pattern_scan;

    g_imageBase = image.base;
    BVR_LOG("[b2r] scanning main module: base %p size 0x%zX", image.base, image.size);

    // FName-chain scan (game-agnostic, core/hooks/pattern_scan.h). On BS2 its
    // real yield is the cached FName INDEX GLOBAL - the dispatch filter's
    // comparison key. The event thunk it also finds is dead code here (zero
    // static callers; the dispatch glue was inlined) and is logged only.
    EventScanResult scan{};
    bool ok = find_event_function(image, "PlayerCalcView", scan);

    BVR_LOG("[b2r] \"PlayerCalcView\": %zu wide-string match(es), %zu string xref(s)",
            scan.stringMatches, scan.stringXrefs);
    if (scan.fnameIndexGlobal) {
        BVR_LOG("[b2r] fname index global: %p (RVA 0x%X, %zu xref(s))", scan.fnameIndexGlobal,
                static_cast<unsigned>(scan.fnameIndexGlobal - image.base), scan.globalXrefs);
    }

    if (!ok || !scan.fnameIndexGlobal) {
        BVR_LOG("[b2r] scan FAILED (fname global not resolved) - camera features disabled, "
                "game runs flat");
        return false;
    }
    out.fnameIndexGlobal = scan.fnameIndexGlobal;
    out.eventPlayerCalcView = scan.function; // knowledge base only - never hooked
    BVR_LOG("[b2r] event thunk (dead code on this game) = %p (RVA 0x%X)", scan.function,
            static_cast<unsigned>(reinterpret_cast<uintptr_t>(scan.function) -
                                  reinterpret_cast<uintptr_t>(image.base)));

    // ProcessEvent: read the controller vtable's slot 3 from the image and
    // follow its stub - the camera call sites dispatch through exactly this
    // slot, so this derivation cannot pick a copy the camera does not use.
    const uint8_t* slot = image.base + kShockPlayerControllerVtableRva +
                          kProcessEventVtblByteOffset;
    if (!is_memory_valid(slot, sizeof(void*))) {
        BVR_LOG("[b2r] controller vtable slot unreadable (RVA 0x%X) - candidate vtable "
                "wrong? game runs flat",
                kShockPlayerControllerVtableRva);
        return false;
    }
    const uint8_t* peStub = *reinterpret_cast<const uint8_t* const*>(slot);
    const uint8_t* pe = follow_jmp_stub(peStub);
    const uint8_t* peExpected = image.base + kProcessEventRva;
    if (pe != peExpected) {
        BVR_LOG("[b2r] ProcessEvent via vtable = %p (stub %p) does NOT match expected "
                "RVA 0x%X - build differs, game runs flat",
                pe, peStub, kProcessEventRva);
        return false;
    }
    if (!prologue_matches(pe, kProcessEventPrologue, sizeof(kProcessEventPrologue),
                          "ProcessEvent"))
        return false;
    out.processEvent = const_cast<uint8_t*>(pe);
    BVR_LOG("[b2r] ProcessEvent = %p (RVA 0x%X, via controller vtbl+0x%X stub %p)", pe,
            kProcessEventRva, kProcessEventVtblByteOffset, peStub);

    const uint8_t* ff = image.base + kFindFuncCheckedRva;
    if (!prologue_matches(ff, kFindFuncCheckedPrologue, sizeof(kFindFuncCheckedPrologue),
                          "FindFunctionChecked"))
        return false;
    out.findFuncChecked = const_cast<uint8_t*>(const_cast<const uint8_t*>(ff));
    BVR_LOG("[b2r] FindFunctionChecked = %p (RVA 0x%X)", ff, kFindFuncCheckedRva);
    return true;
}

} // namespace bvr::b2r::patterns
