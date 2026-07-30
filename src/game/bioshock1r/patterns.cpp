// eventPlayerCalcView derivation: FName-chain scan, technique ported from
// itsloopyo/bioshock-remastered-headtracking (MIT), src/memory.rs - the
// generic implementation lives in core/hooks/pattern_scan.cpp. The logged RVA
// is the cross-check value against the Rust mod on the same exe build
// (docs/bioshock1/ENGINE_NOTES.md).

#include "game/bioshock1r/patterns.h"

#include "core/util/log.h"
#include "core/util/module_id.h"

#include <windows.h>

#include <cstring>

namespace bvr::b1r::patterns {
namespace {

// Captured by resolve() so the lazy hfov_option_ptr() can form the vtable
// address for this session's ASLR base.
const uint8_t* g_imageBase = nullptr;

// Set once by resolve(): does the running exe match the build every RVA in
// patterns.h came from? See the header's build-identity block.
bool g_rvaTrusted = false;
// Test override for the stand-down path - see handle_buildgate_command.
bool g_buildGateForcedOff = false;

// ---- live-object search (session 27 rewrite) --------------------------------
// The generic machinery - thread-stack/TEB exclusion, the live-heap fast path,
// the budgeted sliced fallback - lives in core/hooks/heap_scan.h. This file
// supplies only the per-class accept filters and the dormancy policy, because
// those are the parts that know what a BioShock object looks like.
//
// What this replaced swept the whole 4 GB address space at a 4-byte stride in
// one blocking call, accepting any MEM_PRIVATE + PAGE_READWRITE region - which
// is also the signature of a thread stack. Field consequences: multi-second
// game-thread stalls, and a candidate that could be a stack slot or a freed
// block while the FOV writer poked it every frame. Full post-mortem in
// heap_scan.h and ENGINE_NOTES session 27.

// Cached UShockUserSettings instance (revalidated every call). There is no
// static pointer to it, so we find it by its fixed-RVA vtable and cache the
// address for the session.
void* g_userSettings = nullptr;
uint64_t g_lastSettingsAttemptMs = 0;
int g_settingsMisses = 0;
bool g_settingsDormant = false;
ObjectScan g_settingsScan;

// Probe slots for the candidate log line: [0] HorizontalFOV, [1] 1 if the
// UObject class chain resolved, [2] the class name's FName index.
bool accept_user_settings(void* obj, void*, int32_t probe[bvr::heap_scan::kProbeSlots]) {
    const uint8_t* p = static_cast<const uint8_t*>(obj);
    int32_t fov = *reinterpret_cast<const int32_t*>(p + kUserSettingsHfovOffset);
    probe[0] = fov;

    // CORROBORATION (session 27). The vtable dword is one predicate, and it is
    // satisfied by any 4 bytes that happen to hold that value: a stack slot
    // spilling the pointer, or a freed object whose header the allocator has
    // not reused yet. The consumer WRITES to whatever comes back, so one
    // predicate is not enough. object_class_name walks obj -> +0x30 UClass* ->
    // UClass vtable identity -> +0x28 FName index -> GNames with every step
    // validated, so a non-null result is strong evidence of a live UObject.
    //
    // Deliberately NOT compared against an expected name: the live instance
    // could be a subclass and rejecting it would silently disable FOV control.
    // The resolved name is logged instead, so tightening this to an exact match
    // later is a one-line change backed by real data.
    const wchar_t* cls = object_class_name(obj);
    probe[1] = cls ? 1 : 0;
    if (!cls) return false;
    const uint8_t* clsObj = *reinterpret_cast<const uint8_t* const*>(p + kUObjectClassOffset);
    probe[2] = *reinterpret_cast<const int32_t*>(clsObj + kUObjectNameIndexOffset);

    // The engine's own options UI clamps to 75..130. The 40..170 window this
    // used to accept let far more debris through.
    return fov >= kUserSettingsHfovMin && fov <= kUserSettingsHfovMax;
}

} // namespace

void log_scan_result(const char* what, const ObjectScan& scan, const ScanResult& r,
                     bool candidates) {
    // A refusal already said so once, in run_object_scan. Repeating a summary for
    // a search that never ran turns the retry cadence into log spam - which the
    // wrong-build test produced, one line every 2 s for the whole session.
    if (r.disabled) return;
    const bvr::heap_scan::Sweep& s = scan.sweep;
    if (candidates) {
        for (int i = 0; i < s.recorded; ++i) {
            const wchar_t* cls = object_class_name(s.addr[i]);
            BVR_LOG("[b1r] %s candidate @ %p probe=%d,%d,%d,%d class='%ls' -> %s", what, s.addr[i],
                    s.probe[i][0], s.probe[i][1], s.probe[i][2], s.probe[i][3],
                    cls ? cls : L"<unresolved>", s.ok[i] ? "ACCEPTED" : "rejected");
        }
        if (s.matches > s.recorded)
            BVR_LOG("[b1r] %s: %d further match(es) not recorded (cap %d)", what,
                    s.matches - s.recorded, bvr::heap_scan::kMaxRecorded);
    }
    BVR_LOG("[b1r] %s scan via %s: %d match(es), %d accepted, chosen=%p "
            "(%.1f ms, %u slice(s), %u heap(s)/%u block(s), %d exclude span(s)%s)",
            what, r.path, r.matches, r.accepted, r.object,
            static_cast<double>(s.elapsedUs) / 1000.0, s.slices, s.heaps, s.blocks,
            s.excludeSpans,
            s.excludes.truncated  ? " - EXCLUSION TABLE FULL, SOME STACKS NOT EXCLUDED"
            : s.excludeMissed > 0 ? " - SOME THREAD STACKS NOT EXCLUDED"
            : s.excludeMissed < 0 ? " - THREAD ENUMERATION FAILED"
                                  : "");
}

bool run_object_scan(ObjectScan& scan, uint32_t vtableRva, uint32_t needBytes,
                     ObjectAccept accept, void* user, ScanResult& out) {
    out = ScanResult{};
    if (!g_imageBase || !accept) return true; // nothing to search: complete and empty
    // On an unrecognised build the vtable RVA is an arbitrary constant, and the
    // callers of this function WRITE to what it returns. Searching memory for a
    // meaningless value and then poking the winner is the one failure mode worth
    // refusing outright (session 27).
    if (!g_rvaTrusted) {
        static bool once = false;
        if (!once) {
            once = true;
            BVR_LOG("[b1r] object scans DISABLED - the host build is not the one these vtable "
                    "addresses came from (see the build block above)");
        }
        out.path = "disabled (unverified build)";
        out.disabled = true;
        return true;
    }
    const void* wantVtable = g_imageBase + vtableRva;

    // Fast path first: only live, allocated heap blocks, tens of milliseconds,
    // and it cannot hand back a freed block. When it succeeds the sliced sweep
    // never runs at all.
    if (!scan.sweeping) {
        if (bvr::heap_scan::heap_blocks(scan.sweep, wantVtable, needBytes, accept, user)) {
            out.object = scan.sweep.first;
            out.matches = scan.sweep.matches;
            out.accepted = scan.sweep.accepted;
            out.path = "heap";
            return true;
        }
        // Not in any Win32 heap block: either the object does not exist yet, or
        // this engine allocates it from its own pool. Fall back to the region
        // sweep, sliced so it can never stall a frame.
        scan.sweeping = true;
        scan.sweep.cursor = 0;
    }

    auto outcome = bvr::heap_scan::sweep(scan.sweep, wantVtable, needBytes, accept, user,
                                         kScanSliceBudgetUs);
    out.path = "sweep (heap path found nothing)";
    if (outcome == bvr::heap_scan::Outcome::Working) return false;
    scan.sweeping = false;
    out.object = scan.sweep.first;
    out.matches = scan.sweep.matches;
    out.accepted = scan.sweep.accepted;
    return true;
}

int32_t* hfov_option_ptr() {
    using namespace bvr::pattern_scan;
    if (!g_imageBase) return nullptr;

    const void* wantVtable = g_imageBase + kUserSettingsVtableRva;

    // Revalidate the cache on TWO predicates, not one: the vtable dword AND the
    // UObject class chain, so a freed-and-not-yet-reused block cannot keep
    // passing as the settings object while the FOV writer pokes it every frame.
    if (g_userSettings) {
        if (is_memory_valid(g_userSettings, kUserSettingsHfovOffset + sizeof(int32_t)) &&
            *reinterpret_cast<const void* const*>(g_userSettings) == wantVtable &&
            object_class_name(g_userSettings) != nullptr) {
            return reinterpret_cast<int32_t*>(static_cast<uint8_t*>(g_userSettings) +
                                              kUserSettingsHfovOffset);
        }
        g_userSettings = nullptr;
        hfov_scan_rearm("cached settings object went stale");
    }

    // DORMANCY (backported from bioshock2r; the session-22 weapon-resolver
    // lesson). A rate limit alone means a save where the object never appears
    // rescans forever, which reads as "the game freezes every couple of
    // seconds". Only an explicit re-arm clears this.
    if (g_settingsDormant) return nullptr;

    uint64_t now = GetTickCount64();
    if (!g_settingsScan.sweeping && now - g_lastSettingsAttemptMs < kScanRetryMs) return nullptr;
    g_lastSettingsAttemptMs = now;

    ScanResult r{};
    if (!run_object_scan(g_settingsScan, kUserSettingsVtableRva,
                         kUserSettingsHfovOffset + sizeof(int32_t), &accept_user_settings, nullptr,
                         r))
        return nullptr; // sweep still in progress - no stall, try again next frame

    log_scan_result("UShockUserSettings", g_settingsScan, r, true);

    // FAIL CLOSED on ambiguity: more than one object passed the filter, so we
    // cannot say which one the renderer reads. Writing to the wrong one is
    // worse than not writing at all.
    if (r.accepted > 1) {
        BVR_LOG("[b1r] UShockUserSettings AMBIGUOUS (%d accepted) - refusing to bind, "
                "FOV control stays off",
                r.accepted);
        g_settingsDormant = true;
        return nullptr;
    }

    g_userSettings = r.object;
    if (!g_userSettings) {
        if (++g_settingsMisses >= kScanMissesBeforeDormant) {
            g_settingsDormant = true;
            BVR_LOG("[b1r] UShockUserSettings scan DORMANT after %d miss(es) - a view-state "
                    "change re-arms it",
                    g_settingsMisses);
        }
        return nullptr;
    }
    g_settingsMisses = 0;
    return reinterpret_cast<int32_t*>(static_cast<uint8_t*>(g_userSettings) +
                                      kUserSettingsHfovOffset);
}

void hfov_scan_rearm(const char* why) {
    g_settingsMisses = 0;
    if (g_settingsDormant) {
        g_settingsDormant = false;
        BVR_LOG("[b1r] UShockUserSettings scan re-armed (%s)", why);
    }
}

bool settings_bound() { return g_userSettings != nullptr; }

bool rva_trusted() { return g_rvaTrusted && !g_buildGateForcedOff; }

void handle_buildgate_command(const char* args) {
    if (args && strncmp(args, "off", 3) == 0) {
        g_buildGateForcedOff = true;
        BVR_LOG("[b1r] buildgate FORCED OFF - address-dependent features now behave exactly as "
                "they would on an unrecognised build (object scans refuse, so no FOV control "
                "and no viewmodel drive). 'buildgate on' restores.");
    } else if (args && strncmp(args, "on", 2) == 0) {
        g_buildGateForcedOff = false;
        // The scanners latched dormant while refusing; wake them.
        hfov_scan_rearm("buildgate restored");
        BVR_LOG("[b1r] buildgate restored (host build itself is %s)",
                g_rvaTrusted ? "VERIFIED" : "STILL UNRECOGNISED");
    } else {
        BVR_LOG("[b1r] buildgate: host build %s, forced-off %s -> rva_trusted=%s",
                g_rvaTrusted ? "VERIFIED" : "NOT RECOGNISED",
                g_buildGateForcedOff ? "yes" : "no", rva_trusted() ? "yes" : "no");
    }
}

bool resolve(const bvr::pattern_scan::ProcessImage& image, Symbols& out) {
    using namespace bvr::pattern_scan;

    g_imageBase = image.base;
    BVR_LOG("[b1r] scanning main module: base %p size 0x%zX", image.base, image.size);

    // Build gate FIRST, so everything logged below is read in the right light.
    const bvr::module_id::Fingerprint host = bvr::module_id::host_exe();
    g_rvaTrusted =
        bvr::module_id::matches(host, kHostTimeDateStamp, kHostSizeOfImage, kHostFileBytes);
    if (g_rvaTrusted) {
        BVR_LOG("[b1r] host build VERIFIED (pe-timestamp 0x%08X) - hardcoded addresses trusted",
                host.timeDateStamp);
    } else {
        BVR_LOG("[b1r] ============================================================");
        BVR_LOG("[b1r] HOST BUILD NOT RECOGNISED - address-dependent features OFF");
        BVR_LOG("[b1r]   running:  pe-timestamp 0x%08X size-of-image 0x%08X bytes %llu",
                host.timeDateStamp, host.sizeOfImage,
                static_cast<unsigned long long>(host.fileBytes));
        BVR_LOG("[b1r]   expected: pe-timestamp 0x%08X size-of-image 0x%08X bytes %llu",
                kHostTimeDateStamp, kHostSizeOfImage,
                static_cast<unsigned long long>(kHostFileBytes));
        BVR_LOG("[b1r] Every offset this mod hardcodes was derived from the 2022-04-13 Steam");
        BVR_LOG("[b1r] build. Applying them to a different binary is how a mod corrupts a");
        BVR_LOG("[b1r] game rather than failing to work, so they stand down and the game");
        BVR_LOG("[b1r] stays fully playable. Head tracking still uses a pattern scan and is");
        BVR_LOG("[b1r] unaffected. If you are on Epic, GOG or a newer patch, please send this");
        BVR_LOG("[b1r] log - the three numbers above are what a per-build offset table needs.");
        BVR_LOG("[b1r] ============================================================");
    }

    EventScanResult scan{};
    bool ok = find_event_function(image, "PlayerCalcView", scan);

    BVR_LOG("[b1r] \"PlayerCalcView\": %zu wide-string match(es), %zu string xref(s)",
            scan.stringMatches, scan.stringXrefs);
    if (scan.fnameIndexGlobal) {
        BVR_LOG("[b1r] fname index global: %p (%zu xref(s), %zu candidate(s) after init-site filter)",
                scan.fnameIndexGlobal, scan.globalXrefs, scan.candidates);
    }
    if (scan.fnameCtor) {
        // Session 20: the FName constructor, kept for the name-system work
        // (GNames derivation in ENGINE_NOTES; resolver below).
        BVR_LOG("[b1r] fname ctor = %p (RVA 0x%X)", scan.fnameCtor,
                static_cast<unsigned>(scan.fnameCtor - image.base));
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

const wchar_t* fname_text(int32_t index) {
    using bvr::pattern_scan::is_memory_valid;
    if (!g_imageBase || index < 0) return nullptr;
    const uint8_t* arrayBase = g_imageBase + kGNamesDataRva;
    if (!is_memory_valid(arrayBase, 8)) return nullptr;
    const uint8_t* const* data = *reinterpret_cast<const uint8_t* const* const*>(arrayBase);
    int32_t count = *reinterpret_cast<const int32_t*>(arrayBase + 4);
    if (!data || index >= count) return nullptr;
    if (!is_memory_valid(data + index, sizeof(void*))) return nullptr;
    const uint8_t* entry = data[index];
    if (!entry || !is_memory_valid(entry, kFNameEntryTextOffset + 2)) return nullptr;
    // Entries self-identify: +0 is the entry's own index. A mismatch means
    // the layout assumption broke (or the slot is recycled garbage) - refuse.
    if (*reinterpret_cast<const int32_t*>(entry) != index) return nullptr;
    return reinterpret_cast<const wchar_t*>(entry + kFNameEntryTextOffset);
}

int32_t fname_count() {
    using bvr::pattern_scan::is_memory_valid;
    if (!g_imageBase) return 0;
    const uint8_t* arrayBase = g_imageBase + kGNamesDataRva;
    if (!is_memory_valid(arrayBase, 8)) return 0;
    return *reinterpret_cast<const int32_t*>(arrayBase + 4);
}

const wchar_t* object_class_name(const void* obj) {
    using bvr::pattern_scan::is_memory_valid;
    if (!g_imageBase || !obj) return nullptr;
    const uint8_t* o = static_cast<const uint8_t*>(obj);
    if (!is_memory_valid(o, kUObjectClassOffset + sizeof(void*))) return nullptr;
    const uint8_t* cls =
        *reinterpret_cast<const uint8_t* const*>(o + kUObjectClassOffset);
    if (!cls || !is_memory_valid(cls, kUObjectNameIndexOffset + sizeof(int32_t)))
        return nullptr;
    // The +0x30 target must BE a UClass - vtable-gated so a stranger layout
    // (or a freed object) can never produce a bogus profile key.
    if (*reinterpret_cast<const void* const*>(cls) != g_imageBase + kUClassVtableRva)
        return nullptr;
    return fname_text(
        *reinterpret_cast<const int32_t*>(cls + kUObjectNameIndexOffset));
}

} // namespace bvr::b1r::patterns
