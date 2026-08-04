#include "game/bioshock2r/patterns.h"

#include "core/util/log.h"

#include <windows.h>

#include <cstring>

namespace bvr::b2r::patterns {
namespace {

// Captured by resolve() so the heap scanner can form vtable addresses for
// this session's ASLR base.
const uint8_t* g_imageBase = nullptr;
size_t g_imageSize = 0;

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

// Decode a direct call (E8 rel32) at a known call site. Returns null when the
// byte there is not E8 - the caller treats that as a build mismatch.
const uint8_t* follow_call_rel32(const uint8_t* p) {
    if (!bvr::pattern_scan::is_memory_valid(p, 5) || p[0] != 0xE8) return nullptr;
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

void resolve_gpfs_impls(const bvr::pattern_scan::ProcessImage& image, GpfsImpls& out) {
    using namespace bvr::pattern_scan;
    out = {};

    // Weapon: image[APlayerWeapon vtbl + 0x374] -> one stub hop -> body.
    const uint8_t* slot = image.base + kPlayerWeaponVtableRva + kWeaponGpfsVtblByteOffset;
    if (is_memory_valid(slot, sizeof(void*))) {
        const uint8_t* stub = *reinterpret_cast<const uint8_t* const*>(slot);
        const uint8_t* body = follow_jmp_stub(stub);
        if (body != image.base + kWeaponGpfsImplRva) {
            BVR_LOG("[b2r] weapon GetPerfectFireStart via vtbl+0x%X = %p, expected RVA "
                    "0x%X - build differs, weapon seam refused",
                    kWeaponGpfsVtblByteOffset, body, kWeaponGpfsImplRva);
        } else if (prologue_matches(body, kWeaponGpfsPrologue, sizeof(kWeaponGpfsPrologue),
                                    "weapon GetPerfectFireStart")) {
            out.weapon = const_cast<uint8_t*>(body);
            BVR_LOG("[b2r] weapon GetPerfectFireStart impl VERIFIED: vtbl 0x%X+0x%X -> "
                    "stub %p -> body %p (RVA 0x%X)",
                    kPlayerWeaponVtableRva, kWeaponGpfsVtblByteOffset, stub, body,
                    kWeaponGpfsImplRva);
        }
    } else {
        BVR_LOG("[b2r] APlayerWeapon vtable slot unreadable - weapon seam refused");
    }

    // Ability: static-called body, prologue gate only.
    const uint8_t* abody = image.base + kAbilityGpfsImplRva;
    if (prologue_matches(abody, kAbilityGpfsPrologue, sizeof(kAbilityGpfsPrologue),
                         "ability GetPerfectFireStart")) {
        out.ability = const_cast<uint8_t*>(const_cast<const uint8_t*>(abody));
        BVR_LOG("[b2r] ability GetPerfectFireStart impl VERIFIED: body %p (RVA 0x%X)",
                abody, kAbilityGpfsImplRva);
    }
}

bool fname_text(uint32_t index, char* out, size_t outCap) {
    using namespace bvr::pattern_scan;
    if (!g_imageBase || !out || outCap < 2) return false;
    out[0] = '\0';

    const uint8_t* arr = g_imageBase + kGNamesArrayRva;
    if (!is_memory_valid(arr, 8)) return false;
    const uint8_t* const* data = *reinterpret_cast<const uint8_t* const* const*>(arr);
    int32_t count = *reinterpret_cast<const int32_t*>(arr + 4);
    // Count band: BS1's live table held ~54k names; a table outside a broad
    // band means the RVA is wrong for this build - refuse everything.
    if (!data || count <= 0 || count > 2000000) return false;
    if (index >= static_cast<uint32_t>(count)) return false;
    if (!is_memory_valid(data + index, sizeof(void*))) return false;

    const uint8_t* entry = data[index];
    // Freed indices leave null slots (the worker keeps a free-index stack).
    if (!entry) return false;
    // Validate the header plus a bounded text window in ONE call; per-char
    // VirtualQuery would put ~2000 syscalls behind a census dump.
    const size_t kTextWindow = 64; // 31 UTF-16 chars + terminator
    if (!is_memory_valid(entry, kFNameEntryTextOffset + kTextWindow)) return false;
    if (*reinterpret_cast<const uint32_t*>(entry + kFNameEntryIndexOffset) != index)
        return false; // self-index check - the entry vouches for itself

    const wchar_t* w = reinterpret_cast<const wchar_t*>(entry + kFNameEntryTextOffset);
    size_t i = 0;
    size_t cap = outCap - 1;
    if (cap > kTextWindow / 2 - 1) cap = kTextWindow / 2 - 1;
    for (; i < cap; ++i) {
        wchar_t c = w[i];
        if (!c) break;
        out[i] = (c >= 32 && c < 127) ? static_cast<char>(c) : '?';
    }
    out[i] = '\0';
    return i > 0;
}

void probe_object_identity(const void* objPtr, const char* label) {
    using namespace bvr::pattern_scan;
    const uint8_t* o = static_cast<const uint8_t*>(objPtr);
    if (!label) label = "?";
    if (!g_imageBase || !o || !is_memory_valid(o, 0x48)) {
        BVR_LOG("[b2r] oclass %s: object %p unreadable", label, objPtr);
        return;
    }
    // Raw header first - the derivation evidence even when nothing resolves.
    const uint32_t* d = reinterpret_cast<const uint32_t*>(o);
    BVR_LOG("[b2r] oclass %s %p hdr: %08X %08X %08X %08X | %08X %08X %08X %08X | "
            "%08X %08X %08X %08X | %08X %08X %08X %08X",
            label, o, d[0], d[1], d[2], d[3], d[4], d[5], d[6], d[7], d[8], d[9], d[10],
            d[11], d[12], d[13], d[14], d[15]);
    // Candidate own-name FName index: any int32 in +0x20..+0x3C that GNames
    // resolves (BS1's was +0x28; derive fresh - never copy).
    char text[64];
    for (uint32_t off = 0x20; off <= 0x3C; off += 4) {
        int32_t idx = *reinterpret_cast<const int32_t*>(o + off);
        if (idx > 0 && idx < 2000000 && fname_text(static_cast<uint32_t>(idx), text,
                                                   sizeof text))
            BVR_LOG("[b2r]   name candidate +0x%02X: idx %d -> '%s'", off, idx, text);
    }
    // Candidate UClass pointer: any pointer in +0x24..+0x44 to a heap object
    // whose dword0 is an IN-IMAGE vtable and whose +0x28 FName resolves (the
    // canonical class name is FName number 0 on BS1's layout). The UClass
    // vtable RVA printed here is the constant to bank once it is identical
    // across >= 3 distinct classes.
    for (uint32_t off = 0x24; off <= 0x44; off += 4) {
        const uint8_t* cls = *reinterpret_cast<const uint8_t* const*>(o + off);
        if (!cls || cls == o || !is_memory_valid(cls, 0x2C + 4)) continue;
        const uint8_t* vt = *reinterpret_cast<const uint8_t* const*>(cls);
        if (vt < g_imageBase || vt >= g_imageBase + g_imageSize) continue;
        int32_t nidx = *reinterpret_cast<const int32_t*>(cls + 0x28);
        if (nidx <= 0 || !fname_text(static_cast<uint32_t>(nidx), text, sizeof text))
            continue;
        BVR_LOG("[b2r]   class candidate +0x%02X: cls %p vtbl RVA 0x%X, cls+0x28 "
                "name '%s'",
                off, cls, static_cast<unsigned>(vt - g_imageBase), text);
    }
}

void resolve_fire_names(const bvr::pattern_scan::ProcessImage& image, FireNames& out) {
    using namespace bvr::pattern_scan;
    // The dispatch names (no `exec` prefix - those are the thunk registration
    // strings, a different region). StopFiring has no wide string in this exe
    // at all (script-side name only) and is deliberately absent.
    static const char* kNames[] = {"GetPerfectFireStart", "BeginFiring", "UseAbility",
                                   "InitiateDamage",      "ApplyAimError"};
    out.count = 0;
    for (const char* name : kNames) {
        if (out.count >= FireNames::kMax) break;
        const uint8_t* global = nullptr;
        auto strs = find_wide_string(image, name);
        size_t xrefs = 0;
        for (const uint8_t* s : strs) {
            // Suffix-pooling trap (BS1 session 10, live in THIS list:
            // "UseAbility" is the tail of "AnimNotify_UseAbility"): only an
            // occurrence with its own terminator is the real string.
            const uint8_t* term = s + 2 * strlen(name);
            if (!is_memory_valid(term, 2) || term[0] != 0 || term[1] != 0) continue;
            auto refs = find_references(image, s);
            xrefs += refs.size();
            for (const uint8_t* r : refs) {
                global = find_fname_index_global(image, r);
                if (global) break;
            }
            if (global) break;
        }
        out.name[out.count] = name;
        out.indexGlobal[out.count] = global;
        ++out.count;
        BVR_LOG("[b2r] fire-name \"%s\": %zu wide string(s), %zu xref(s), index global %s"
                " (RVA 0x%X)",
                name, strs.size(), xrefs, global ? "RESOLVED" : "none",
                global ? static_cast<unsigned>(global - image.base) : 0u);
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

bool verify_flush_chain(const bvr::pattern_scan::ProcessImage& image) {
    using namespace bvr::pattern_scan;
    const uint8_t* site = image.base + kFlushCallSiteRva;
    const uint8_t* thunk = follow_call_rel32(site);
    if (!thunk) {
        BVR_LOG("[b2r] flush call site 0x%X is not an E8 call - build differs, refusing",
                kFlushCallSiteRva);
        return false;
    }
    if (thunk != image.base + kFlushThunkRva) {
        BVR_LOG("[b2r] flush call at 0x%X lands at %p, expected thunk RVA 0x%X - build "
                "differs, refusing",
                kFlushCallSiteRva, thunk, kFlushThunkRva);
        return false;
    }
    const uint8_t* body = follow_jmp_stub(thunk);
    if (body != image.base + kFlushPointRva) {
        BVR_LOG("[b2r] flush thunk 0x%X lands at %p, expected body RVA 0x%X - build "
                "differs, refusing",
                kFlushThunkRva, body, kFlushPointRva);
        return false;
    }
    if (!prologue_matches(body, kFlushPointPrologue, sizeof(kFlushPointPrologue),
                          "render flush point"))
        return false;
    BVR_LOG("[b2r] render flush chain VERIFIED: Draw tail 0x%X -> thunk 0x%X -> body %p "
            "(RVA 0x%X)",
            kFlushCallSiteRva, kFlushThunkRva, body, kFlushPointRva);
    return true;
}

bool resolve(const bvr::pattern_scan::ProcessImage& image, Symbols& out) {
    using namespace bvr::pattern_scan;

    g_imageBase = image.base;
    g_imageSize = image.size;
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
