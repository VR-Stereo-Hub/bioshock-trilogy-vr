#include "game/bioshockinf/reflect.h"

#include "core/util/log.h"
#include "game/bioshockinf/camera.h"
#include "game/bioshockinf/patterns.h"

#include <imgui.h>
#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace bvr::bsi::reflect {
namespace {

using bvr::pattern_scan::NativeScanResult;
using bvr::pattern_scan::NativeTableBounds;

bvr::pattern_scan::ProcessImage g_image{};
NativeTableBounds g_table{};   // the APlayerController block, for status only
bool g_tableTried = false;

// Native registration is segmented into ONE BLOCK PER CLASS, separated by
// {0,0} sentinels (established live, session 36). So a table-walk cross-check
// must be seeded from the SAME class it is about to look up - a block seeded
// elsewhere simply cannot contain the name.
bool bounds_for(const char* cls, const char* fn, NativeTableBounds& out) {
    out = {};
    NativeScanResult seed{};
    if (!bvr::pattern_scan::find_native_function_ex(g_image, bvr::pattern_scan::kNativeTableUE3,
                                                    cls, fn, seed))
        return false;
    return bvr::pattern_scan::native_table_bounds(g_image, bvr::pattern_scan::kNativeTableUE3,
                                                  static_cast<const uint8_t*>(seed.tableEntry),
                                                  out);
}

uint32_t rva_of(const void* p) {
    const uint8_t* base = patterns::image_base();
    if (!base || !p) return 0;
    return static_cast<uint32_t>(static_cast<const uint8_t*>(p) - base);
}

// Seeds the table bounds from one known-good entry, so the O(count) walk
// instrument becomes available. Uses GetPlayerViewPoint because its expected
// answer is known independently (the thunk RVA from an offline disassembly),
// which makes the seed itself a checked step rather than an assumption.
bool ensure_table() {
    if (g_tableTried) return g_table.base != nullptr;
    g_tableTried = true;
    if (!g_image.base) return false;
    NativeScanResult seed{};
    if (!bvr::pattern_scan::find_native_function_ex(g_image, bvr::pattern_scan::kNativeTableUE3,
                                                    "APlayerController", "GetPlayerViewPoint",
                                                    seed)) {
        BVR_LOG("[bsi] reflect: cannot seed the native table - the UE3 shape did not resolve "
                "APlayerController::GetPlayerViewPoint (strings=%zu refs=%zu termRej=%zu "
                "nbrRej=%zu implRej=%zu)",
                seed.stringMatches, seed.tableRefs, seed.terminatorRejects,
                seed.neighbourRejects, seed.implRejects);
        return false;
    }
    if (!bvr::pattern_scan::native_table_bounds(g_image, bvr::pattern_scan::kNativeTableUE3,
                                                static_cast<const uint8_t*>(seed.tableEntry),
                                                g_table)) {
        BVR_LOG("[bsi] reflect: native table bounds walk failed from a verified entry");
        return false;
    }
    // 46 is APlayerController's native count from the offline per-class census,
    // and the block is bounded by {0,0} sentinels rather than running into the
    // next class. The image's 2647 is the TOTAL across all classes.
    BVR_LOG("[bsi] reflect: APlayerController native block at RVA 0x%X, %zu entries (seed index "
            "%zu). Offline per-class census said 46 - %s",
            rva_of(g_table.base), g_table.count, g_table.seedIndex,
            g_table.count == 46 ? "MATCH" : "MISMATCH, the 8-byte shape is suspect");
    return true;
}

// Resolves through BOTH instruments and reports whether they agree. Two
// independent methods answering the same question is the evidence; when they
// disagree, both are suspect and neither result should be used.
void cmd_native(const char* args) {
    char cls[96] = {};
    char fn[96] = {};
    if (sscanf_s(args, "%95s %95s", cls, static_cast<unsigned>(sizeof cls), fn,
                 static_cast<unsigned>(sizeof fn)) != 2) {
        BVR_LOG("[bsi] reflect: usage - bsinative <Class> <Func>   e.g. "
                "bsinative APlayerController GetPlayerViewPoint");
        return;
    }
    NativeScanResult scan{};
    const bool okScan = bvr::pattern_scan::find_native_function_ex(
        g_image, bvr::pattern_scan::kNativeTableUE3, cls, fn, scan);
    BVR_LOG("[bsi] reflect: scan  %sexec%s -> %s (rva 0x%X) | strings=%zu refs=%zu "
            "termRej=%zu nbrRej=%zu implRej=%zu",
            cls, fn, okScan ? "FOUND" : "not found", rva_of(scan.function), scan.stringMatches,
            scan.tableRefs, scan.terminatorRejects, scan.neighbourRejects, scan.implRejects);

    NativeTableBounds blk{};
    if (!bounds_for(cls, fn, blk)) return;
    NativeScanResult walk{};
    const bool okWalk = bvr::pattern_scan::find_native_in_table(
        g_image, bvr::pattern_scan::kNativeTableUE3, blk, cls, fn, walk);
    BVR_LOG("[bsi] reflect: walk  %sexec%s -> %s (rva 0x%X)", cls, fn,
            okWalk ? "FOUND" : "not found", rva_of(walk.function));
    if (okScan != okWalk || scan.function != walk.function) {
        BVR_LOG("[bsi] reflect: !! THE TWO INSTRUMENTS DISAGREE - treat both answers as "
                "unusable until the shape is re-derived");
    }
}

// Session 42 (the cheat-loadout lane): one-shot FULL GNames dump to the data
// dir, so candidate exec names (the give-family, cheats, weapon and Vigor
// class names) are greppable OFFLINE instead of being guessed 64 entries at a
// time through the pump. The file is game-derived content: it lives under
// %LOCALAPPDATA% and is never committed - the frame-dump rule. Game thread
// only (command-driven by construction); one linear pass, buffered stdio, so
// the cost is one long-ish tick rather than a per-second cadence (the
// fname_* one-shot rule).
void cmd_names_dump() {
    const int32_t num = patterns::fname_count();
    if (num <= 0) {
        BVR_LOG("[bsi] reflect: GNames is not populated yet (Num=%d) - nothing to dump", num);
        return;
    }
    wchar_t path[MAX_PATH];
    const wchar_t* base = bvr::log::data_dir();
    if (!base || !base[0]) {
        BVR_LOG("[bsi] reflect: no data dir - cannot dump GNames");
        return;
    }
    swprintf_s(path, L"%s\\gnames.txt", base);
    FILE* f = _wfsopen(path, L"w", _SH_DENYWR);
    if (!f) {
        BVR_LOG("[bsi] reflect: could not open gnames.txt for writing");
        return;
    }
    char buf[256];
    int written = 0, unreadable = 0;
    for (int i = 0; i < num; ++i) {
        if (patterns::fname_text(i, buf, sizeof buf)) {
            fprintf(f, "%d\t%s\n", i, buf);
            ++written;
        } else {
            ++unreadable;
        }
    }
    fclose(f);
    BVR_LOG("[bsi] reflect: GNames dumped - %d names (%d unreadable) of Num=%d -> "
            "%%LOCALAPPDATA%%\\BioshockVR\\bsi\\gnames.txt (game-derived: never commit)",
            written, unreadable, num);
}

void cmd_names(const char* args) {
    if (strncmp(args, "dump", 4) == 0) {
        cmd_names_dump();
        return;
    }
    int start = 0;
    int count = 16;
    sscanf_s(args, "%d %d", &start, &count);
    if (count < 1) count = 1;
    if (count > 64) count = 64;
    const int32_t num = patterns::fname_count();
    if (num <= 0) {
        BVR_LOG("[bsi] reflect: GNames is not populated yet (Num=%d). It is empty until the "
                "engine's static initializers have run - this is normal early, and a command "
                "issued from a running game should never see it.",
                num);
        return;
    }
    BVR_LOG("[bsi] reflect: GNames Num=%d Max=%d, showing [%d, %d)", num, patterns::fname_max(),
            start, start + count);
    char buf[256];
    for (int i = start; i < start + count && i < num; ++i) {
        bool wide = false;
        patterns::fname_is_wide(i, wide);
        if (patterns::fname_text(i, buf, sizeof buf))
            BVR_LOG("[bsi] reflect:   [%5d] %s%s", i, buf, wide ? "  (UTF-16)" : "");
        else
            BVR_LOG("[bsi] reflect:   [%5d] <unreadable or self-index mismatch>", i);
    }
}

void cmd_name_find(const char* args) {
    char want[128] = {};
    if (sscanf_s(args, "%127s", want, static_cast<unsigned>(sizeof want)) != 1) {
        BVR_LOG("[bsi] reflect: usage - bsiname <text>");
        return;
    }
    const int32_t idx = patterns::fname_find(want);
    BVR_LOG("[bsi] reflect: fname_find(\"%s\") = %d%s", want, idx,
            idx < 0 ? "  (not in the pool)" : "");
}

// Defined further down with the object-identity helpers; used here only to
// label which object a vtable window belongs to.
bool object_class_name(const void* obj, char* out, size_t outSize);

// Session 44: generalized to an EXPLICIT object and an explicit START SLOT.
// Both were needed the moment a seam turned out to be a virtual on the PAWN
// rather than on the PC: the old form could only read the latched PC, and only
// its first 96 slots, while the I7 aim seam lives at +0x2E8 (slot 186).
// Usage: bsivtable [count] | bsivtable <hexObjAddr> [count] [startSlotHexOff]
void cmd_vtable(const char* args) {
    int count = 40;
    unsigned addr = 0, startOff = 0;
    // An explicit object always starts with 0x; otherwise the first token is
    // the count, which keeps every existing bsivtable invocation working.
    if (args && (args[0] == '0') && (args[1] == 'x' || args[1] == 'X')) {
        if (sscanf_s(args + 2, "%x %d %x", &addr, &count, &startOff) < 1) return;
    } else if (args) {
        sscanf_s(args, "%d", &count);
    }
    if (count < 1) count = 1;
    if (count > 96) count = 96;
    startOff &= ~3u; // slots are pointer-aligned

    void* obj = addr ? reinterpret_cast<void*>(static_cast<uintptr_t>(addr))
                     : camera::last_player_controller();
    if (!obj) {
        BVR_LOG("[bsi] reflect: no live object yet - the camera hook has not fired, and this "
                "lane deliberately takes objects from hook parameters rather than scanning "
                "(GObjObjects is not used).");
        return;
    }
    if (!bvr::pattern_scan::is_memory_valid(obj, 4)) {
        BVR_LOG("[bsi] reflect: object %p is not readable", obj);
        return;
    }
    const uint8_t* const* vtBase = *reinterpret_cast<const uint8_t* const* const*>(obj);
    const uint8_t* const* vt = vtBase ? vtBase + startOff / sizeof(void*) : nullptr;
    if (!vt || !bvr::pattern_scan::is_memory_valid(vt, count * sizeof(void*))) {
        BVR_LOG("[bsi] reflect: vtable pointer %p is not readable for %d slots from +0x%X",
                (void*)vtBase, count, startOff);
        return;
    }
    char clsName[128] = "";
    object_class_name(obj, clsName, sizeof clsName);
    BVR_LOG("[bsi] reflect: %s %p vtable %p (rva 0x%X), %d slots from +0x%X:",
            clsName[0] ? clsName : "<latched PC>", obj, (const void*)vtBase, rva_of(vtBase),
            count, startOff);
    for (int i = 0; i < count; ++i) {
        const uint8_t* fn = vt[i];
        if (!fn) continue;
        const uint32_t r = rva_of(fn);
        // Offsets are ABSOLUTE in the vtable, so a windowed read still names
        // the slot the exec thunk's `call [reg+disp]` was talking about.
        const uint32_t off = startOff + static_cast<uint32_t>(i) * 4;
        const bool isPe = (off == patterns::kProcessEventVtableOffset);
        const bool isFf = (off == patterns::kFindFunctionVtableOffset);
        const bool isAim = (off == patterns::kPawnGetBaseAimRotationVtblOffset);
        BVR_LOG("[bsi] reflect:   +0x%03X [%3d] rva 0x%-8X%s", off, off / 4, r,
                isPe   ? "  <== ProcessEvent (offline: slot +0x7C)"
                : isFf ? "  <== FindFunction (offline: slot +0x54)"
                : isAim
                    ? "  <== GetBaseAimRotation on a Pawn (offline: exec thunk 0x12BF30 "
                      "dispatches through this slot)"
                    : "");
    }
    // The live cross-check that costs nothing: our offline derivation says slot
    // +0x7C holds ProcessEvent. On an APlayerController - an AActor subclass -
    // the expected occupant is AActor::ProcessEvent, NOT the UObject base.
    const uint32_t idx = patterns::kProcessEventVtableOffset / 4 - startOff / 4;
    if (startOff <= patterns::kProcessEventVtableOffset && static_cast<int>(idx) < count &&
        vt[idx]) {
        const uint32_t r = rva_of(vt[idx]);
        const char* verdict = "UNEXPECTED - neither derived address";
        if (r == patterns::kActorProcessEventRva)
            verdict = "AActor::ProcessEvent, exactly as predicted for an AActor subclass";
        else if (r == patterns::kProcessEventRva)
            verdict = "UObject::ProcessEvent (the base) - note this is NOT what an AActor "
                      "subclass was predicted to carry";
        BVR_LOG("[bsi] reflect: slot +0x7C on this object = rva 0x%X -> %s", r, verdict);
    }
}

struct Check {
    int pass = 0;
    int fail = 0;
    void note(bool ok, const char* what, const char* detail) {
        if (ok) ++pass; else ++fail;
        BVR_LOG("[bsi] selftest: %s  %-52s %s", ok ? "PASS" : "FAIL", what, detail);
    }
};

// The whole point: every instrument gets a control that would VISIBLY FAIL if
// the instrument were broken. An instrument that cannot fail its own hypothesis
// is not evidence.
void cmd_selftest() {
    Check c;
    char detail[256];

    if (!g_image.base) {
        BVR_LOG("[bsi] selftest: no process image - cannot run");
        return;
    }

    // --- positives: known answers derived offline by a different method ------
    struct Known {
        const char* cls;
        const char* fn;
        uint32_t rva;
    };
    static const Known kKnown[] = {
        {"APlayerController", "GetPlayerViewPoint", 0x129280},
        {"APlayerController", "XGetPlayerFloatingViewPoint", 0x1292C0},
        {"APawn", "GetBaseAimRotation", 0x12BF30},
        {"AXPlayerController", "CalcFOV", 0x4FC060},
    };
    for (const Known& k : kKnown) {
        NativeScanResult r{};
        const bool ok = bvr::pattern_scan::find_native_function_ex(
            g_image, bvr::pattern_scan::kNativeTableUE3, k.cls, k.fn, r);
        const uint32_t got = rva_of(r.function);
        _snprintf_s(detail, sizeof detail, _TRUNCATE, "%sexec%s -> 0x%X (expected 0x%X)", k.cls,
                    k.fn, got, k.rva);
        c.note(ok && got == k.rva, "positive: native resolves to its offline RVA", detail);
    }

    // --- negative, PREFIX: one character short of a real name ----------------
    // "GetPlayerViewPoin" exists in the image as a prefix of the real string.
    // Only requiring OUR terminator at OUR end rejects it. If this returns the
    // real function, the terminator guard is dead and EVERY result above is
    // suspect rather than merely this one.
    {
        NativeScanResult r{};
        const bool found = bvr::pattern_scan::find_native_function_ex(
            g_image, bvr::pattern_scan::kNativeTableUE3, "APlayerController",
            "GetPlayerViewPoin", r);
        _snprintf_s(detail, sizeof detail, _TRUNCATE,
                    "found=%d strings=%zu terminatorRejects=%zu", found ? 1 : 0,
                    r.stringMatches, r.terminatorRejects);
        c.note(!found && r.stringMatches >= 1 && r.terminatorRejects >= 1,
               "negative: prefix rejected by the terminator guard", detail);
    }

    // --- negative, SUFFIX: the linker pools literals by suffix ---------------
    // "PlayerControllerexecGetPlayerViewPoint" is stored INSIDE
    // "APlayerControllerexecGetPlayerViewPoint" and shares its terminator, so
    // the terminator guard cannot save us here. The reference step must: a
    // pooled suffix has its own address and no table entry points at it.
    {
        NativeScanResult r{};
        const bool found = bvr::pattern_scan::find_native_function_ex(
            g_image, bvr::pattern_scan::kNativeTableUE3, "PlayerController",
            "GetPlayerViewPoint", r);
        _snprintf_s(detail, sizeof detail, _TRUNCATE, "found=%d strings=%zu tableRefs=%zu",
                    found ? 1 : 0, r.stringMatches, r.tableRefs);
        c.note(!found && r.stringMatches >= 1 && r.tableRefs == 0,
               "negative: pooled suffix rejected by the reference step", detail);
    }

    // --- negative, ABSENT ----------------------------------------------------
    {
        NativeScanResult r{};
        const bool found = bvr::pattern_scan::find_native_function_ex(
            g_image, bvr::pattern_scan::kNativeTableUE3, "APlayerController",
            "ThisFunctionDoesNotExist", r);
        _snprintf_s(detail, sizeof detail, _TRUNCATE, "found=%d strings=%zu", found ? 1 : 0,
                    r.stringMatches);
        c.note(!found && r.stringMatches == 0, "negative: absent name finds nothing", detail);
    }

    // --- shape: a falsifiable check on the 8-byte stride itself --------------
    // Registration is segmented per class by {0,0} sentinels, so a walk seeded
    // from an APlayerController native must yield exactly that class's 46.
    {
        const bool ok = ensure_table();
        _snprintf_s(detail, sizeof detail, _TRUNCATE,
                    "APlayerController block = %zu entries (per-class census says 46)",
                    g_table.count);
        c.note(ok && g_table.count == 46, "shape: per-class native block walks to its census",
               detail);
    }

    // --- cross-instrument agreement -----------------------------------------
    // Each lookup seeds its OWN class's block; a block from another class
    // cannot contain the name, which is the layout talking, not a failure.
    for (const Known& k : kKnown) {
        NativeTableBounds blk{};
        NativeScanResult a{}, b{};
        bvr::pattern_scan::find_native_function_ex(g_image, bvr::pattern_scan::kNativeTableUE3,
                                                   k.cls, k.fn, a);
        if (bounds_for(k.cls, k.fn, blk))
            bvr::pattern_scan::find_native_in_table(g_image, bvr::pattern_scan::kNativeTableUE3,
                                                    blk, k.cls, k.fn, b);
        _snprintf_s(detail, sizeof detail, _TRUNCATE, "%s::%s scan=0x%X walk=0x%X (block %zu)",
                    k.cls, k.fn, rva_of(a.function), rva_of(b.function), blk.count);
        c.note(a.function != nullptr && a.function == b.function,
               "cross-check: scan and table-walk agree", detail);
    }

    // --- GNames: the UE3 invariant ------------------------------------------
    {
        const int32_t num = patterns::fname_count();
        char buf[256] = {};
        const bool got = num > 0 && patterns::fname_text(0, buf, sizeof buf);
        _snprintf_s(detail, sizeof detail, _TRUNCATE, "Num=%d GNames[0]=\"%s\"", num, buf);
        c.note(got && strcmp(buf, "None") == 0, "GNames[0] is \"None\" (UE3 invariant)", detail);

        const int32_t idx = num > 0 ? patterns::fname_find("None") : -1;
        _snprintf_s(detail, sizeof detail, _TRUNCATE, "fname_find(\"None\")=%d", idx);
        c.note(idx == 0, "reverse lookup finds \"None\" at index 0", detail);

        // A pool with zero wide entries means the wide path was never
        // exercised. Report that as UNTESTED rather than counting it as a pass.
        int wideSeen = 0;
        const int32_t scanTo = num < 4096 ? num : 4096;
        for (int32_t i = 0; i < scanTo; ++i) {
            bool w = false;
            if (patterns::fname_is_wide(i, w) && w) ++wideSeen;
        }
        BVR_LOG("[bsi] selftest: INFO  %-52s %d wide of the first %d entries%s",
                "FNameEntry encoding flag", wideSeen, scanTo,
                wideSeen == 0 ? "  <- the UTF-16 path is UNTESTED, not proven working"
                              : "");
    }

    // --- live object: the UClass fixpoint -----------------------------------
    // In UE3 the class of UClass is UClass, so obj->Class->Class ==
    // obj->Class->Class->Class. A self-referential identity that needs no names
    // at all - the cheapest possible validation of both a live object pointer
    // and the +0x20 Class offset.
    {
        void* obj = camera::last_player_controller();
        bool ok = false;
        detail[0] = '\0';
        if (!obj) {
            _snprintf_s(detail, sizeof detail, _TRUNCATE,
                        "no live object yet (camera hook has not fired)");
        } else {
            const uint32_t off = patterns::kUObjectClassOffset;
            if (bvr::pattern_scan::is_memory_valid(obj, off + 4)) {
                const uint8_t* cls = *reinterpret_cast<const uint8_t* const*>(
                    static_cast<const uint8_t*>(obj) + off);
                if (cls && bvr::pattern_scan::is_memory_valid(cls, off + 4)) {
                    const uint8_t* meta =
                        *reinterpret_cast<const uint8_t* const*>(cls + off);
                    if (meta && bvr::pattern_scan::is_memory_valid(meta, off + 4)) {
                        const uint8_t* metaMeta =
                            *reinterpret_cast<const uint8_t* const*>(meta + off);
                        ok = (meta == metaMeta);
                        _snprintf_s(detail, sizeof detail, _TRUNCATE,
                                    "obj=%p Class=%p Class->Class=%p ->Class=%p", obj,
                                    (const void*)cls, (const void*)meta,
                                    (const void*)metaMeta);
                    }
                }
            }
            if (!detail[0])
                _snprintf_s(detail, sizeof detail, _TRUNCATE, "chain unreadable from %p", obj);
        }
        c.note(ok, "live object: UClass fixpoint (Class->Class is its own class)", detail);
    }

    BVR_LOG("[bsi] selftest: %d passed, %d FAILED. A failure here invalidates every result the "
            "failing instrument has produced, not just this run.",
            c.pass, c.fail);
}

// ---- bsicall: call a UFunction BY NAME on the latched APlayerController ----
// (DR-I6, session 37.) Goes through the engine's OWN reflection - FindFunction
// at vtable slot +0x54, then ProcessEvent at +0x7C - never the exec thunks,
// which have 0 static callers and would need a hand-built FFrame. This is
// BS2's script-setter precedent (ProcessEvent by name, SEH-isolated,
// effect-verified) rebuilt on UE3's shapes; no BS2 number or code is reused.
//
// UObject::FindFunction(FName InName, UBOOL Global=0): FName is the UE3
// {Index, Number} pair, so thiscall + 3 stack dwords - the same measured
// `ret 0xC` shape as FindFunctionChecked (patterns.h). ProcessEvent takes
// (UFunction*, void* Parms, void* Result), the derived 3-arg `ret 0xC`.
// __fastcall with a dummy edx is this codebase's standing thiscall idiom.
using FindFunctionFn = void* (__fastcall*)(void* self, void* edx, int32_t nameIndex,
                                           int32_t nameNumber, int32_t global);
using ProcessEventFn = void(__fastcall*)(void* self, void* edx, void* func, void* parms,
                                         void* result);

// SEH-isolated dispatch. In its own frame with no C++ objects (SEH and C++
// unwinding do not mix). Returns 0 on success, 1 when FindFunction returned
// null, and 2 on a fault, with the code in outExcept - the game keeps running
// either way, which is the whole point of the isolation.
int call_by_name_seh(void* obj, const uint8_t* const* vt, int32_t nameIndex, void* parms,
                     void** outFunc, uint32_t* outExcept) {
    __try {
        FindFunctionFn findFn = reinterpret_cast<FindFunctionFn>(
            vt[patterns::kFindFunctionVtableOffset / 4]);
        void* fn = findFn(obj, nullptr, nameIndex, 0, 0);
        *outFunc = fn;
        if (!fn) return 1;
        ProcessEventFn pe =
            reinterpret_cast<ProcessEventFn>(vt[patterns::kProcessEventVtableOffset / 4]);
        pe(obj, nullptr, fn, parms, nullptr);
        return 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *outExcept = GetExceptionCode();
        return 2;
    }
}

// The shared gate stack for anything that dispatches into the engine. Every
// refusal is named after the check that made it. Returns false unless: the
// build gate is open, we are on the camera (game) thread, GNames is populated,
// the latched APlayerController and its vtable are readable, and BOTH dispatch
// slots still hold the derived implementations (a re-linked build would pass
// the readability checks and then call through slots that mean something else
// entirely - this is the gate that fails it softly instead).
bool resolve_dispatch_target(const char* tag, void*& outObj, const uint8_t* const*& outVt) {
    if (!patterns::rva_trusted()) {
        BVR_LOG("[bsi] %s: REFUSED - build gate closed", tag);
        return false;
    }
    // Thread interlock: this must be the camera (game) thread. Under the lease
    // a silent game thread hands the pump to the Present thread in degraded
    // mode, and an engine call from there is a cross-thread dispatch the
    // engine itself was measured never to take (foreign-tid-calls=0).
    const uint32_t camTid = camera::camera_tid();
    if (camTid == 0 || GetCurrentThreadId() != camTid) {
        BVR_LOG("[bsi] %s: REFUSED - not on the game thread (this tid=%u camera tid=%u). "
                "The camera hook must own the pump (pump=game).",
                tag, GetCurrentThreadId(), camTid);
        return false;
    }
    if (patterns::fname_count() <= 0) {
        BVR_LOG("[bsi] %s: REFUSED - GNames not populated yet", tag);
        return false;
    }
    void* obj = camera::last_player_controller();
    if (!obj || !bvr::pattern_scan::is_memory_valid(obj, sizeof(void*))) {
        BVR_LOG("[bsi] %s: REFUSED - no readable latched APlayerController (%p)", tag, obj);
        return false;
    }
    const uint8_t* const* vt = *reinterpret_cast<const uint8_t* const* const*>(obj);
    const size_t slotsNeeded = patterns::kProcessEventVtableOffset / 4 + 1;
    if (!vt || !bvr::pattern_scan::is_memory_valid(vt, slotsNeeded * sizeof(void*))) {
        BVR_LOG("[bsi] %s: REFUSED - vtable %p unreadable through slot +0x%X", tag, (void*)vt,
                patterns::kProcessEventVtableOffset);
        return false;
    }
    const uint32_t ffRva = rva_of(vt[patterns::kFindFunctionVtableOffset / 4]);
    const uint32_t peRva = rva_of(vt[patterns::kProcessEventVtableOffset / 4]);
    if (ffRva != patterns::kFindFunctionRva ||
        (peRva != patterns::kActorProcessEventRva && peRva != patterns::kProcessEventRva)) {
        BVR_LOG("[bsi] %s: REFUSED - live vtable disagrees with the derivation "
                "(+0x54=0x%X expected 0x%X, +0x7C=0x%X expected 0x%X/0x%X)",
                tag, ffRva, patterns::kFindFunctionRva, peRva, patterns::kActorProcessEventRva,
                patterns::kProcessEventRva);
        return false;
    }
    outObj = obj;
    outVt = vt;
    return true;
}

// ---- Session 42: object-walking, for the cheat-loadout lane -----------------
// The measured facts that force this design: ConsoleCommand dispatches (s37,
// C++ handlers proven by effect) but SCRIPT execs do not - god, AllWeapons,
// behindview and viewmode all produced zero effect in a gameplay save, and
// FindFunction on the PC chain has no EnableCheats/BehindView. The script-side
// console exec bridge is dead in this retail build, exactly like the key-bind
// lane (s34). The loadout must therefore dispatch by ProcessEvent ON THE
// OBJECT that owns the function (pawn, CheatManager) - which needs a way to
// FIND those objects. Per the recorded design rule, objects come from hook
// parameters, never scans: these helpers walk the LATCHED PC's own pointer
// fields and identify UObjects by class name.
//
// UObject::Class is +0x20 (ENGINE_NOTES struct layouts, from execIsA).
// UObject::Name's offset is DERIVED once per run: candidate dwords on the
// latched PC's class object, accepted only when the FName text they select
// contains "PlayerController" - the one name the class object must carry.
int g_objNameOff = -1;

const void* object_class(const void* obj) {
    if (!bvr::pattern_scan::is_memory_valid(obj, 0x24)) return nullptr;
    const void* cls =
        *reinterpret_cast<const void* const*>(static_cast<const uint8_t*>(obj) + 0x20);
    if (!bvr::pattern_scan::is_memory_valid(cls, 0x50)) return nullptr;
    return cls;
}

bool derive_obj_name_off() {
    if (g_objNameOff >= 0) return true;
    const void* pc = camera::last_player_controller();
    const void* cls = pc ? object_class(pc) : nullptr;
    if (!cls) return false;
    // Live s42 derivation: the PC's class object carried its name index at
    // +0x18 (read 8167 = 'XPlayerController' in that boot's pool), so the
    // candidate walk starts low enough to reach it. Kept as a walk rather
    // than a constant: the text check is the interlock either way.
    const int32_t num = patterns::fname_count();
    for (int cand = 0x0C; cand <= 0x48; cand += 4) {
        const int32_t idx =
            *reinterpret_cast<const int32_t*>(static_cast<const uint8_t*>(cls) + cand);
        if (idx <= 0 || idx >= num) continue;
        char txt[128];
        if (!patterns::fname_text(idx, txt, sizeof txt)) continue;
        if (strstr(txt, "PlayerController")) {
            g_objNameOff = cand;
            BVR_LOG("[bsi] reflect: UObject::Name derived at +0x%X (the PC's class object "
                    "reads '%s')",
                    cand, txt);
            return true;
        }
    }
    return false;
}

// Class name of an arbitrary pointer IF it reads as a UObject; empty string
// otherwise. The gates (readable object, readable class, name index in range,
// name text readable) are what makes walking raw fields safe.
bool object_class_name(const void* obj, char* out, size_t outSize) {
    if (outSize) out[0] = '\0';
    if (g_objNameOff < 0) return false;
    const void* cls = object_class(obj);
    if (!cls) return false;
    // s45b hardening: require the UClass FIXPOINT. A genuine instance's +0x20
    // is a UClass whose own class names "Class"; a raw struct whose +0x20
    // happens to hold an INSTANCE pointer passes every readability gate and
    // then prints that instance's NAME as a "class" - measured live on the
    // pawn's +0x0D8 loadout-cache struct, which walked as a convincing fake
    // XWeaponModelFirstPerson until this check. One extra indirection buys
    // out the whole false-positive class.
    const void* clsOfCls = object_class(cls);
    if (!clsOfCls) return false;
    const int32_t metaIdx = *reinterpret_cast<const int32_t*>(
        static_cast<const uint8_t*>(clsOfCls) + g_objNameOff);
    if (metaIdx <= 0 || metaIdx >= patterns::fname_count()) return false;
    char metaName[patterns::kFNameTextBufMin]; // fname_text refuses smaller buffers
    if (!patterns::fname_text(metaIdx, metaName, sizeof metaName)) return false;
    if (strcmp(metaName, "Class") != 0) return false;
    const int32_t idx =
        *reinterpret_cast<const int32_t*>(static_cast<const uint8_t*>(cls) + g_objNameOff);
    if (idx <= 0 || idx >= patterns::fname_count()) return false;
    return patterns::fname_text(idx, out, outSize);
}

// bsifields [startHex] [dwords]: walk the latched PC's pointer fields and name
// every UObject they reach. This is how the pawn and the CheatManager instance
// (if any) are FOUND, with their PC-relative offsets, without GObjObjects.
void cmd_fields(const char* args) {
    unsigned start = 0;
    unsigned count = 0x180;
    unsigned addr = 0;
    // Session 44: the explicit-object generalization s43 recorded as owed (the
    // grant lane needed it to walk the PAWN's inventory list, and the I7 aim
    // lane needs the pawn too). `0x` prefix = an explicit object, exactly the
    // convention bsicallat already uses; everything else is the old form.
    if (args && args[0] == '0' && (args[1] == 'x' || args[1] == 'X')) {
        if (sscanf_s(args + 2, "%x %x %u", &addr, &start, &count) < 1) return;
    } else if (args) {
        sscanf_s(args, "%x %u", &start, &count);
    }
    if (count > 0x400) count = 0x400;
    void* obj = nullptr;
    const uint8_t* const* vt = nullptr;
    if (!resolve_dispatch_target("fields", obj, vt)) return;
    if (addr) {
        // The gate stack above still runs on the PC (it is what proves the
        // reflection lane is live and on the right thread); only the object
        // being WALKED changes, and it is read-only either way.
        obj = reinterpret_cast<void*>(static_cast<uintptr_t>(addr));
        if (!bvr::pattern_scan::is_memory_valid(obj, 4)) {
            BVR_LOG("[bsi] fields: REFUSED - explicit object %p is not readable", obj);
            return;
        }
    }
    if (!derive_obj_name_off()) {
        BVR_LOG("[bsi] fields: REFUSED - UObject::Name offset did not derive on the PC's "
                "class object (no candidate dword selected a '*PlayerController' name)");
        return;
    }
    char clsName[128];
    object_class_name(obj, clsName, sizeof clsName);
    BVR_LOG("[bsi] fields: walking %s %p from +0x%X, %u dwords - every pointer whose "
            "target reads as a UObject, with its class name",
            clsName[0] ? clsName : "<pc>", obj, start, count);
    int shown = 0;
    for (unsigned i = 0; i < count && shown < 80; ++i) {
        const uint32_t off = start + i * 4;
        const uint8_t* slot = static_cast<const uint8_t*>(obj) + off;
        if (!bvr::pattern_scan::is_memory_valid(slot, sizeof(void*))) break;
        const void* p = *reinterpret_cast<const void* const*>(slot);
        if (!p || (reinterpret_cast<uintptr_t>(p) & 3)) continue;
        char nm[128];
        if (!object_class_name(p, nm, sizeof nm) || !nm[0]) continue;
        BVR_LOG("[bsi] fields:   +0x%03X -> %p  class %s", off, p, nm);
        ++shown;
    }
    BVR_LOG("[bsi] fields: done (%d object fields shown%s)", shown,
            shown >= 80 ? " - CAPPED, narrow with [startHex]" : "");
}

// ---- Session 45b: the two readers the I8 derivation was missing -------------
// bsifields names a UObject a POINTER FIELD reaches, but a UE3 Components list
// is a TArray {Data, Num, Max} - the Data buffer is not a UObject, so the s44
// walker skips it silently. bsiarray walks the triple; bsidump is the typed
// raw view for deriving non-object layout (bone counts, transform banks).
// Read-only, command-driven, same gate stack as bsifields.

// bsiarray 0x<obj> <offHex> [n]: interpret obj+off as TArray<void*> and name
// every element that reads as a UObject.
void cmd_array(const char* args) {
    unsigned addr = 0, off = 0, n = 32;
    if (!args || sscanf_s(args, "%x %x %u", &addr, &off, &n) < 2 || !addr) {
        BVR_LOG("[bsi] array: usage - bsiarray 0x<obj> <offHex> [n]. Reads the TArray "
                "{Data,Num,Max} triple at obj+off and names each element's class. The "
                "offset comes from a bsidump/bsifields pass (e.g. a pawn's Components).");
        return;
    }
    void* pcObj = nullptr;
    const uint8_t* const* pcVt = nullptr;
    if (!resolve_dispatch_target("array", pcObj, pcVt)) return;
    if (!derive_obj_name_off()) {
        BVR_LOG("[bsi] array: REFUSED - UObject::Name offset did not derive");
        return;
    }
    const uint8_t* obj = reinterpret_cast<const uint8_t*>(static_cast<uintptr_t>(addr));
    if (!bvr::pattern_scan::is_memory_valid(obj + off, 12)) {
        BVR_LOG("[bsi] array: REFUSED - %p+0x%X is not readable as a TArray triple",
                (const void*)obj, off);
        return;
    }
    const void* const* data = *reinterpret_cast<const void* const* const*>(obj + off);
    const int32_t num = *reinterpret_cast<const int32_t*>(obj + off + 4);
    const int32_t max = *reinterpret_cast<const int32_t*>(obj + off + 8);
    BVR_LOG("[bsi] array: %p+0x%X = {Data %p, Num %d, Max %d}", (const void*)obj, off,
            (const void*)data, num, max);
    // The triple's own invariants are the acceptance gate: a garbage offset
    // fails them long before an element read could fault.
    if (num < 0 || max < num || num > 0x10000) {
        BVR_LOG("[bsi] array: NOT a live TArray (invariants failed) - wrong offset");
        return;
    }
    if (num == 0) {
        BVR_LOG("[bsi] array: empty (Num 0)");
        return;
    }
    if (!data ||
        !bvr::pattern_scan::is_memory_valid(data, static_cast<size_t>(num) * sizeof(void*))) {
        BVR_LOG("[bsi] array: Data buffer unreadable for %d elements - wrong offset or "
                "not a pointer array",
                num);
        return;
    }
    if (n > 64) n = 64;
    int shown = 0;
    for (int32_t i = 0; i < num && shown < static_cast<int>(n); ++i) {
        const void* e = data[i];
        if (!e) continue;
        char nm[128];
        if (object_class_name(e, nm, sizeof nm) && nm[0]) {
            BVR_LOG("[bsi] array:   [%2d] %p  class %s", i, e, nm);
        } else {
            BVR_LOG("[bsi] array:   [%2d] %p  (not a UObject)", i, e);
        }
        ++shown;
    }
    if (num > static_cast<int32_t>(n))
        BVR_LOG("[bsi] array: ... %d more (raise [n], cap 64)", num - static_cast<int32_t>(n));
}

// bsidump 0x<addr> [dwords] [startHex]: one line per dword - hex, float and
// int32 readings side by side, UObject pointers named. This is the layout
// triage BS2 ran as `vrbones map`, generalized to any address.
void cmd_dump(const char* args) {
    unsigned addr = 0, count = 0x40, start = 0;
    if (!args || sscanf_s(args, "%x %u %x", &addr, &count, &start) < 1 || !addr) {
        BVR_LOG("[bsi] dump: usage - bsidump 0x<addr> [dwords] [startHexOff]. Typed raw "
                "view: hex | float | int per dword, UObject pointers named.");
        return;
    }
    void* pcObj = nullptr;
    const uint8_t* const* pcVt = nullptr;
    if (!resolve_dispatch_target("dump", pcObj, pcVt)) return;
    derive_obj_name_off(); // best-effort: pointer naming degrades gracefully
    if (count > 0x200) count = 0x200;
    const uint8_t* base = reinterpret_cast<const uint8_t*>(static_cast<uintptr_t>(addr));
    BVR_LOG("[bsi] dump: %p from +0x%X, %u dwords", (const void*)base, start, count);
    for (unsigned i = 0; i < count; ++i) {
        const uint32_t off = start + i * 4;
        if (!bvr::pattern_scan::is_memory_valid(base + off, 4)) {
            BVR_LOG("[bsi] dump:   +0x%03X <unreadable - stopped>", off);
            break;
        }
        const uint32_t u = *reinterpret_cast<const uint32_t*>(base + off);
        float f;
        memcpy(&f, &u, 4);
        char nm[128] = {};
        if (u > 0x10000 && (u & 3) == 0)
            object_class_name(reinterpret_cast<const void*>(static_cast<uintptr_t>(u)), nm,
                              sizeof nm);
        BVR_LOG("[bsi] dump:   +0x%03X  0x%08X  f=%- 12.6g i=%-11d%s%s", off, u,
                static_cast<double>(f), static_cast<int32_t>(u), nm[0] ? "  -> class " : "",
                nm);
    }
}

// bsichase 0x<addr> <offHex> <offHex> ...: follow a pointer chain. Each hop
// reads the pointer at cur+off, names the target when it walks as a UObject
// (class + own name), and continues from it; the terminal target gets a
// 16-dword typed dump. Read-only. Built s49 for the anim-tree hunt: a
// Morpheme-side descent is raw C++ pointers that bsifields skips silently,
// and re-walking a proven chain after a rig re-resolve costs ONE command
// instead of N manual bsidump/eyeball/retype hops.
void cmd_chase(const char* args) {
    unsigned addr = 0;
    int pos = 0;
    if (!args || sscanf_s(args, " 0x%x%n", &addr, &pos) < 1 || !addr) {
        BVR_LOG("[bsi] chase: usage - bsichase 0x<addr> <offHex> <offHex> ... Follows "
                "the pointer at cur+off hop by hop, naming every target that reads as a "
                "UObject; the final target gets a 16-dword typed dump. Max 16 hops.");
        return;
    }
    void* pcObj = nullptr;
    const uint8_t* const* pcVt = nullptr;
    if (!resolve_dispatch_target("chase", pcObj, pcVt)) return;
    derive_obj_name_off(); // best-effort: naming degrades gracefully
    const uint8_t* cur = reinterpret_cast<const uint8_t*>(static_cast<uintptr_t>(addr));
    {
        char nm[128] = {};
        object_class_name(cur, nm, sizeof nm);
        BVR_LOG("[bsi] chase: start %p%s%s", (const void*)cur, nm[0] ? "  class " : "", nm);
    }
    const char* p = args + pos;
    int hop = 0;
    while (hop < 16) {
        unsigned off = 0;
        int used = 0;
        if (sscanf_s(p, " %x%n", &off, &used) < 1) break;
        p += used;
        ++hop;
        const uint8_t* slot = cur + off;
        if (!bvr::pattern_scan::is_memory_valid(slot, sizeof(void*))) {
            BVR_LOG("[bsi] chase: hop %d STOPPED - %p+0x%X is not readable", hop,
                    (const void*)cur, off);
            return;
        }
        const uint8_t* next = *reinterpret_cast<const uint8_t* const*>(slot);
        if (!next || (reinterpret_cast<uintptr_t>(next) & 3) ||
            !bvr::pattern_scan::is_memory_valid(next, 4)) {
            BVR_LOG("[bsi] chase: hop %d STOPPED - +0x%X holds %p (null/unaligned/"
                    "unreadable)",
                    hop, off, (const void*)next);
            return;
        }
        char cls[128] = {};
        char own[patterns::kFNameTextBufMin] = {};
        if (object_class_name(next, cls, sizeof cls)) {
            const int32_t nameIdx =
                g_objNameOff >= 0
                    ? *reinterpret_cast<const int32_t*>(next + g_objNameOff)
                    : -1;
            if (nameIdx > 0 && nameIdx < patterns::fname_count())
                patterns::fname_text(nameIdx, own, sizeof own);
        }
        BVR_LOG("[bsi] chase: hop %d  +0x%03X -> %p%s%s%s%s", hop, off, (const void*)next,
                cls[0] ? "  class " : "  (not a UObject)", cls, own[0] ? "  name " : "",
                own);
        cur = next;
    }
    if (hop == 0) {
        BVR_LOG("[bsi] chase: no offsets given - nothing followed");
        return;
    }
    for (unsigned i = 0; i < 16; ++i) {
        const uint32_t off = i * 4;
        if (!bvr::pattern_scan::is_memory_valid(cur + off, 4)) {
            BVR_LOG("[bsi] chase:   +0x%03X <unreadable - stopped>", off);
            break;
        }
        const uint32_t u = *reinterpret_cast<const uint32_t*>(cur + off);
        float f;
        memcpy(&f, &u, 4);
        char nm[128] = {};
        if (u > 0x10000 && (u & 3) == 0)
            object_class_name(reinterpret_cast<const void*>(static_cast<uintptr_t>(u)), nm,
                              sizeof nm);
        BVR_LOG("[bsi] chase:   +0x%03X  0x%08X  f=%- 12.6g i=%-11d%s%s", off, u,
                static_cast<double>(f), static_cast<int32_t>(u), nm[0] ? "  -> class " : "",
                nm);
    }
}

// bsidiff 0x<addr> [dwords]: snapshot-compare (s46). The first call on a range
// snapshots it; each later call on the SAME range prints only the dwords that
// CHANGED since the previous call, then re-snapshots. Built for the UBOOL
// hunt: snapshot the object, flip the game-side switch, diff - the changed
// dword names the offset. (The bsidump before/after alternative is ~80
// interleaved log lines per probe, per candidate class.)
constexpr unsigned kDiffCapDwords = 0x200;
uintptr_t g_diffAddr = 0;
unsigned g_diffCount = 0;
uint32_t g_diffSnap[kDiffCapDwords];

void cmd_diff(const char* args) {
    unsigned addr = 0, count = 0x40;
    if (!args || sscanf_s(args, "%x %u", &addr, &count) < 1 || !addr) {
        BVR_LOG("[bsi] diff: usage - bsidiff 0x<addr> [dwords]. First call snapshots the "
                "range; later calls print only CHANGED dwords, then re-snapshot.");
        return;
    }
    void* pcObj = nullptr;
    const uint8_t* const* pcVt = nullptr;
    if (!resolve_dispatch_target("diff", pcObj, pcVt)) return;
    if (count == 0) count = 1;
    if (count > kDiffCapDwords) count = kDiffCapDwords;
    const uint8_t* base = reinterpret_cast<const uint8_t*>(static_cast<uintptr_t>(addr));
    if (!bvr::pattern_scan::is_memory_valid(base, count * 4)) {
        BVR_LOG("[bsi] diff: %p unreadable for %u dwords - refusing", (const void*)base,
                count);
        return;
    }
    if (g_diffAddr != addr || g_diffCount != count) {
        memcpy(g_diffSnap, base, count * 4);
        g_diffAddr = addr;
        g_diffCount = count;
        BVR_LOG("[bsi] diff: snapshot %u dwords at %p - intervene, then re-run the same "
                "bsidiff",
                count, (const void*)base);
        return;
    }
    unsigned changed = 0, shown = 0;
    for (unsigned i = 0; i < count; ++i) {
        const uint32_t now = *reinterpret_cast<const uint32_t*>(base + i * 4);
        if (now == g_diffSnap[i]) continue;
        ++changed;
        if (shown < 40) {
            float fo, fn;
            memcpy(&fo, &g_diffSnap[i], 4);
            memcpy(&fn, &now, 4);
            BVR_LOG("[bsi] diff:   +0x%03X  0x%08X -> 0x%08X  (f %.6g -> %.6g)", i * 4,
                    g_diffSnap[i], now, static_cast<double>(fo), static_cast<double>(fn));
            ++shown;
        }
        g_diffSnap[i] = now;
    }
    BVR_LOG("[bsi] diff: %u changed dword(s) at %p%s", changed, (const void*)base,
            changed > shown ? " (capped at 40 lines)" : "");
}

// bsicallat <hexaddr> <Func> [float]: cmd_call generalized to an EXPLICIT
// object address (one bsifields just printed). Same gate stack; the vtable
// slot interlock carries over unchanged because FindFunction is UObject-level
// and ProcessEvent must be one of the two derived RVAs on any dispatchable
// object here.
void cmd_call_at(const char* args) {
    unsigned addr = 0;
    char fn[96] = {};
    char valStr[96] = {};
    const int n = sscanf_s(args ? args : "", "%x %95s %95s", &addr, fn,
                           static_cast<unsigned>(sizeof fn), valStr,
                           static_cast<unsigned>(sizeof valStr));
    if (n < 2) {
        BVR_LOG("[bsi] callat: usage - bsicallat <hexaddr> <Func> [0x<ptr>|i:<int>|"
                "n:<Name>|<float>]. The address comes from a bsifields line; dispatch is "
                "FindFunction+ProcessEvent on THAT object. Parms are echoed back after the "
                "call (s45b) so out-params/returns are readable. Acceptance is still the "
                "downstream EFFECT.");
        return;
    }
    const int32_t nameIndex = patterns::fname_find(fn);
    if (nameIndex < 0) {
        BVR_LOG("[bsi] callat: REFUSED - '%s' is not in GNames (%d entries searched)", fn,
                patterns::fname_count());
        return;
    }
    // The shared prefix gates (build/tid/GNames/latched-PC) via the standard
    // resolver - then the checks are re-run against the explicit object.
    void* pcObj = nullptr;
    const uint8_t* const* pcVt = nullptr;
    if (!resolve_dispatch_target("callat", pcObj, pcVt)) return;
    void* obj = reinterpret_cast<void*>(static_cast<uintptr_t>(addr));
    if (!obj || !bvr::pattern_scan::is_memory_valid(obj, sizeof(void*))) {
        BVR_LOG("[bsi] callat: REFUSED - %p is not readable", obj);
        return;
    }
    const uint8_t* const* vt = *reinterpret_cast<const uint8_t* const* const*>(obj);
    const size_t slotsNeeded = patterns::kProcessEventVtableOffset / 4 + 1;
    if (!vt || !bvr::pattern_scan::is_memory_valid(vt, slotsNeeded * sizeof(void*))) {
        BVR_LOG("[bsi] callat: REFUSED - vtable %p unreadable through slot +0x%X",
                (void*)vt, patterns::kProcessEventVtableOffset);
        return;
    }
    const uint32_t ffRva = rva_of(vt[patterns::kFindFunctionVtableOffset / 4]);
    const uint32_t peRva = rva_of(vt[patterns::kProcessEventVtableOffset / 4]);
    if (ffRva != patterns::kFindFunctionRva ||
        (peRva != patterns::kActorProcessEventRva && peRva != patterns::kProcessEventRva)) {
        BVR_LOG("[bsi] callat: REFUSED - vtable disagrees with the derivation (+0x54=0x%X "
                "expected 0x%X, +0x7C=0x%X expected 0x%X/0x%X) - not a dispatchable UObject",
                ffRva, patterns::kFindFunctionRva, peRva, patterns::kActorProcessEventRva,
                patterns::kProcessEventRva);
        return;
    }
    char nm[128] = {};
    derive_obj_name_off();
    object_class_name(obj, nm, sizeof nm);
    alignas(16) uint8_t parms[256] = {};
    const char* parmKind = " with zeroed parms";
    if (n == 3) {
        if (valStr[0] == '0' && (valStr[1] == 'x' || valStr[1] == 'X')) {
            // Session 42b: a raw pointer argument at parms+0 (an object a
            // bsiload/bsifields line produced) - the grant-lane shape
            // (e.g. CreateInventory(class<Inventory>)).
            const uint32_t p = strtoul(valStr + 2, nullptr, 16);
            memcpy(parms, &p, sizeof p);
            parmKind = " with pointer parm";
        } else if (valStr[0] == 'i' && valStr[1] == ':') {
            // s45b: int32 at parms+0 (e.g. GetBoneName(int BoneIndex)).
            const int32_t v = atoi(valStr + 2);
            memcpy(parms, &v, sizeof v);
            parmKind = " with int parm";
        } else if (valStr[0] == 'n' && valStr[1] == ':') {
            // s45b: FName {Index, Number=0} at parms+0, looked up from the
            // pool by TEXT (e.g. MatchRefBone(name Bone)). A missing name is
            // a refusal, not a zero - index 0 is 'None' and would silently
            // ask about the wrong bone.
            const int32_t idx = patterns::fname_find(valStr + 2);
            if (idx < 0) {
                BVR_LOG("[bsi] callat: REFUSED - FName '%s' not in GNames", valStr + 2);
                return;
            }
            memcpy(parms, &idx, sizeof idx);
            parmKind = " with FName parm";
        } else {
            const float v = strtof(valStr, nullptr);
            memcpy(parms, &v, sizeof v);
            parmKind = " with float parm";
        }
    }
    BVR_LOG("[bsi] callat: dispatching '%s' (GNames %d)%s on %p (class %s), tid %u", fn,
            nameIndex, parmKind, obj, nm[0] ? nm : "?", GetCurrentThreadId());
    void* func = nullptr;
    uint32_t code = 0;
    const int r = call_by_name_seh(obj, vt, nameIndex, parms, &func, &code);
    if (r == 1) {
        BVR_LOG("[bsi] callat: FindFunction('%s') returned null on class %s - that object "
                "does not carry the function. Nothing was called.",
                fn, nm[0] ? nm : "?");
    } else if (r == 2) {
        BVR_LOG("[bsi] callat: FAULT 0x%08X inside the dispatch - swallowed by SEH, game "
                "continues. Treat the parameter shape as wrong until re-derived.",
                code);
    } else {
        BVR_LOG("[bsi] callat: '%s' (UFunction %p) returned. NOT acceptance - measure the "
                "downstream effect.",
                fn, func);
        // s45b: echo the parm block back. UE3 writes out-params and the return
        // value into this block, so the first dwords ARE the answer for the
        // reader natives (GetFirstPersonAttachment -> [0] is the object,
        // MatchRefBone -> the int past the FName, GetBoneLocation -> a vec3).
        // Interpretation is the caller's job - this only shows the bytes.
        for (int i = 0; i < 8; ++i) {
            uint32_t u;
            memcpy(&u, parms + i * 4, 4);
            float fv;
            memcpy(&fv, &u, 4);
            char pnm[128] = {};
            if (u > 0x10000 && (u & 3) == 0)
                object_class_name(reinterpret_cast<const void*>(static_cast<uintptr_t>(u)),
                                  pnm, sizeof pnm);
            BVR_LOG("[bsi] callat:   parms[%d] +0x%02X  0x%08X  f=%- 12.6g i=%-11d%s%s", i,
                    i * 4, u, static_cast<double>(fv), static_cast<int32_t>(u),
                    pnm[0] ? "  -> class " : "", pnm);
        }
    }
}

// ---- Session 42b: the object LOADER, for the grant lane ---------------------
// bsiload <Full.Object.Path> - DynamicLoadObject(path, null, MayFail=true)
// dispatched on the latched PC (the function is a static native on Core.Object,
// so any object's FindFunction resolves it). The RETURN pointer is read back
// out of the parms block and logged with its class and name, so the next
// command can feed it onward (bsicallat ... 0x<ptr>). This is how a weapon or
// Vigor CLASS/archetype becomes a pointer an inventory grant can consume.
// s52: the bsiload core as a callable (the arsenal's rung 1). Same gates,
// same trim, same logging; returns the loaded object or null.
void* do_load_object(const char* args) {
    while (args && *args == ' ') ++args;
    if (!args || !*args) return nullptr;
    const int32_t nameIndex = patterns::fname_find("DynamicLoadObject");
    if (nameIndex < 0) {
        BVR_LOG("[bsi] load: REFUSED - 'DynamicLoadObject' not in GNames");
        return nullptr;
    }
    void* obj = nullptr;
    const uint8_t* const* vt = nullptr;
    if (!resolve_dispatch_target("load", obj, vt)) return nullptr;
    static wchar_t s_wide[512]; // game thread only (the tid gate above)
    int written =
        MultiByteToWideChar(CP_UTF8, 0, args, -1, s_wide, static_cast<int>(_countof(s_wide)));
    if (written <= 0) {
        BVR_LOG("[bsi] load: REFUSED - path did not convert to UTF-16");
        return nullptr;
    }
    // Trim trailing CR/LF/space: command.txt lines arrive with the line ending
    // attached. ConsoleCommand's own parser eats it, but an OBJECT PATH match
    // is exact - "XCore.XConsole\n" resolves to nothing (measured, this run).
    while (written >= 2 && (s_wide[written - 2] == L'\r' || s_wide[written - 2] == L'\n' ||
                            s_wide[written - 2] == L' ' || s_wide[written - 2] == L'\t')) {
        s_wide[written - 2] = L'\0';
        --written;
    }
    // UE3: static final function Object DynamicLoadObject(string ObjectName,
    // class ObjectClass, optional bool MayFail). Parms: FString, UClass*,
    // UBOOL, then the return slot. ObjectClass null = no IsA constraint.
    struct LoadParms {
        wchar_t* data;
        int32_t num, max;
        void* objectClass;
        int32_t mayFail;
        void* ret;
    };
    LoadParms parms{};
    parms.data = s_wide;
    parms.num = written;
    parms.max = written;
    parms.objectClass = nullptr;
    parms.mayFail = 1;
    parms.ret = nullptr;
    BVR_LOG("[bsi] load: DynamicLoadObject(\"%s\", null, MayFail=1) on PC %p", args, obj);
    void* func = nullptr;
    uint32_t code = 0;
    const int r = call_by_name_seh(obj, vt, nameIndex, &parms, &func, &code);
    if (r == 1) {
        BVR_LOG("[bsi] load: FindFunction('DynamicLoadObject') returned null - the Object "
                "statics are not reachable through this chain");
        return nullptr;
    }
    if (r == 2) {
        BVR_LOG("[bsi] load: FAULT 0x%08X - swallowed by SEH", code);
        return nullptr;
    }
    return parms.ret;
}

void cmd_load(const char* args) {
    while (args && *args == ' ') ++args;
    if (!args || !*args) {
        BVR_LOG("[bsi] load: usage - bsiload <Full.Object.Path>   e.g. bsiload "
                "XGame.XWeaponMurderOfCrows. Returns the object pointer for bsicallat.");
        return;
    }
    void* ret = do_load_object(args);
    if (!ret) {
        BVR_LOG("[bsi] load: returned NULL - not found (or a refusal logged above)");
    } else {
        char cls[128] = {};
        char nm[128] = {};
        derive_obj_name_off();
        object_class_name(ret, cls, sizeof cls);
        if (g_objNameOff >= 0 && bvr::pattern_scan::is_memory_valid(ret, 0x50)) {
            const int32_t ni = *reinterpret_cast<const int32_t*>(
                static_cast<const uint8_t*>(ret) + g_objNameOff);
            if (ni > 0 && ni < patterns::fname_count()) patterns::fname_text(ni, nm, sizeof nm);
        }
        BVR_LOG("[bsi] load: LOADED %p  class %s  name %s  -> use it as bsicallat's 0x arg",
                ret, cls[0] ? cls : "?", nm[0] ? nm : "?");
    }
}

void cmd_call(const char* args) {
    char fn[96] = {};
    char valStr[32] = {};
    const int n = sscanf_s(args ? args : "", "%95s %31s", fn,
                           static_cast<unsigned>(sizeof fn), valStr,
                           static_cast<unsigned>(sizeof valStr));
    if (n < 1) {
        BVR_LOG("[bsi] call: usage - bsicall <Function> [floatArg]   e.g. bsicall FOV 120. "
                "Dispatches by NAME on the latched APlayerController via FindFunction(+0x54) "
                "+ ProcessEvent(+0x7C). Acceptance is the downstream EFFECT, never this log.");
        return;
    }
    const int32_t nameIndex = patterns::fname_find(fn);
    if (nameIndex < 0) {
        BVR_LOG("[bsi] call: REFUSED - '%s' is not in GNames (%d entries searched). A name "
                "the engine never registered cannot be a UFunction.",
                fn, patterns::fname_count());
        return;
    }
    void* obj = nullptr;
    const uint8_t* const* vt = nullptr;
    if (!resolve_dispatch_target("call", obj, vt)) return;
    // Zeroed 256-byte param block, float arg (if any) at offset 0. Enough for
    // any probe-sized signature; anything the callee writes back (out params,
    // an FString return) lands here and is discarded - this is a probe
    // instrument, not a general dispatcher.
    alignas(16) uint8_t parms[256] = {};
    float v = 0.0f;
    if (n == 2) {
        v = strtof(valStr, nullptr);
        memcpy(parms, &v, sizeof v);
    }
    BVR_LOG("[bsi] call: dispatching '%s' (GNames %d)%s on PC %p, tid %u",
            fn, nameIndex, n == 2 ? " with float parm" : " with zeroed parms", obj,
            GetCurrentThreadId());
    void* func = nullptr;
    uint32_t code = 0;
    const int r = call_by_name_seh(obj, vt, nameIndex, parms, &func, &code);
    if (r == 1) {
        BVR_LOG("[bsi] call: FindFunction('%s') returned null - the controller's class does "
                "not carry that function. Nothing was called.",
                fn);
    } else if (r == 2) {
        BVR_LOG("[bsi] call: FAULT 0x%08X inside the dispatch - swallowed by SEH, game "
                "continues. Treat the shape as wrong until re-derived.",
                code);
    } else {
        BVR_LOG("[bsi] call: '%s' (UFunction %p) returned%s. A completed call is NOT "
                "acceptance - measure the downstream effect (lens, heartbeat, screenshot).",
                fn, func, n == 2 ? "" : " (no parm)");
    }
}

// ---- bsiexec: run a console command through ConsoleCommand, by name ---------
// `ConsoleCommand` is registered as a native on AActor / APlayerController /
// AXPlayerController (ENGINE_NOTES, session 34 census), so the latched PC's
// class chain carries it and FindFunction resolves it without an address.
// Stock UE3 shape:
//   native function string ConsoleCommand(string Command, optional bool
//                                         bWriteToLog);
// Params block: FString Command at +0 ({TCHAR* Data, int Num, int Max} - the
// same TArray triple GNames confirmed on this build), UBOOL bWriteToLog at
// +12, FString ReturnValue at +16. The Command buffer is OURS (static, game
// thread only); the ReturnValue the engine writes is deliberately LEAKED, a
// few bytes per probe, because freeing it means calling the engine allocator
// with a shape this derivation has not touched.
void cmd_exec(const char* args) {
    while (args && *args == ' ') ++args;
    if (!args || !*args) {
        BVR_LOG("[bsi] exec: usage - bsiexec <console command>   e.g. bsiexec shot. Runs it "
                "through AXPlayerController::ConsoleCommand resolved BY NAME. Acceptance is "
                "the downstream EFFECT (a file, the backbuffer, the lens) - never this log.");
        return;
    }
    const int32_t nameIndex = patterns::fname_find("ConsoleCommand");
    if (nameIndex < 0) {
        BVR_LOG("[bsi] exec: REFUSED - 'ConsoleCommand' not in GNames (%d entries)",
                patterns::fname_count());
        return;
    }
    void* obj = nullptr;
    const uint8_t* const* vt = nullptr;
    if (!resolve_dispatch_target("exec", obj, vt)) return;
    // Game thread only (the interlock above guarantees it), so statics are safe.
    static wchar_t s_wide[512];
    const int written =
        MultiByteToWideChar(CP_UTF8, 0, args, -1, s_wide, static_cast<int>(_countof(s_wide)));
    if (written <= 0) {
        BVR_LOG("[bsi] exec: REFUSED - command text did not convert to UTF-16");
        return;
    }
    struct ConsoleCommandParms {
        wchar_t* data;      // FString Command
        int32_t num, max;   //   Num counts the terminator
        int32_t bWriteToLog;
        wchar_t* retData;   // FString ReturnValue, engine-written
        int32_t retNum, retMax;
    };
    ConsoleCommandParms parms{};
    parms.data = s_wide;
    parms.num = written; // MultiByteToWideChar's count includes the NUL
    parms.max = written;
    parms.bWriteToLog = 1;
    BVR_LOG("[bsi] exec: dispatching ConsoleCommand(\"%s\") on PC %p, tid %u", args, obj,
            GetCurrentThreadId());
    void* func = nullptr;
    uint32_t code = 0;
    const int r = call_by_name_seh(obj, vt, nameIndex, &parms, &func, &code);
    if (r == 1) {
        BVR_LOG("[bsi] exec: FindFunction('ConsoleCommand') returned null - unexpected, the "
                "census says the class chain registers it. Nothing was called.");
    } else if (r == 2) {
        BVR_LOG("[bsi] exec: FAULT 0x%08X inside the dispatch - swallowed by SEH, game "
                "continues. Treat the FString shape as wrong until re-derived.",
                code);
    } else {
        char ret[128] = {};
        if (parms.retData && parms.retNum > 0 &&
            bvr::pattern_scan::is_memory_valid(parms.retData,
                                               static_cast<size_t>(parms.retNum) * 2)) {
            WideCharToMultiByte(CP_UTF8, 0, parms.retData, -1, ret, sizeof ret - 1, nullptr,
                                nullptr);
        }
        BVR_LOG("[bsi] exec: ConsoleCommand returned (UFunction %p, ret \"%s\", %d wchars "
                "leaked by design). NOT acceptance - measure the downstream effect.",
                func, ret, parms.retNum);
    }
}

} // namespace

void init(const bvr::pattern_scan::ProcessImage& image) {
    g_image = image;
}

void exec_console(const char* cmd) {
    cmd_exec(cmd);
}

bool class_name_of(const void* obj, char* out, size_t outSize) {
    // Best-effort derive first, the internal callers' own pattern: a no-op
    // once the offset is known, a clean false before a PC exists.
    derive_obj_name_off();
    return object_class_name(obj, out, outSize);
}

int32_t uobject_name_offset() {
    derive_obj_name_off();
    return g_objNameOff;
}

int32_t object_name_index(const void* obj) {
    if (g_objNameOff < 0 || !obj) return -1;
    if (!bvr::pattern_scan::is_memory_valid(obj, static_cast<size_t>(g_objNameOff) + 4))
        return -1;
    const int32_t idx = *reinterpret_cast<const int32_t*>(
        static_cast<const uint8_t*>(obj) + g_objNameOff);
    return (idx > 0 && idx < patterns::fname_count()) ? idx : -1;
}

// ---- s48: the UProperty-chain walker (bsiprop / bsipropbit) -----------------
// The stance root kill needs the BYTE OFFSET of bDisableSubtleFidget on the
// live attachment (console `set` is dead on this build, s46; the ProcessEvent
// filter proved the anim starts NATIVELY, s48 - so the engine's own UBOOL gate
// is the remaining root). The chain: UClass -> Children (first UField) ->
// Next -> ... Each link offset is DERIVED live with the UClass-fixpoint
// walker rather than assumed: a candidate dword is a link iff it points to a
// UObject whose class names a field type (*Property/Function/Struct/Enum/
// Const) - and NOT "Class" (that is the +0x20 class slot). Read-only; the
// separate bsipropbit does the one-bit write once a human has read the dump.

bool field_like(const void* p) {
    char cls[64];
    if (!class_name_of(p, cls, sizeof cls) || !cls[0]) return false;
    const size_t n = strlen(cls);
    if (strcmp(cls, "Function") == 0 || strcmp(cls, "ScriptStruct") == 0 ||
        strcmp(cls, "Struct") == 0 || strcmp(cls, "Enum") == 0 || strcmp(cls, "Const") == 0 ||
        strcmp(cls, "State") == 0)
        return true;
    return n >= 8 && strcmp(cls + n - 8, "Property") == 0;
}

int find_link_offset(const void* obj, int lo, int hi) {
    for (int off = lo; off <= hi; off += 4) {
        if (!bvr::pattern_scan::is_memory_valid(
                static_cast<const uint8_t*>(obj) + off, 4))
            return -1;
        const void* p = *reinterpret_cast<const void* const*>(
            static_cast<const uint8_t*>(obj) + off);
        if (p && field_like(p)) return off;
    }
    return -1;
}

void cmd_prop(const char* args) {
    unsigned addr = 0;
    char want[64] = {};
    if (!args || sscanf_s(args, "%x %63s", &addr, want,
                          static_cast<unsigned>(sizeof want)) < 1 || !addr) {
        BVR_LOG("[bsi] prop: usage - bsiprop <hexObj> [propName|*] (walks the object's "
                "class property chain; * dumps every entry's candidate dwords)");
        return;
    }
    void* pcObj = nullptr;
    const uint8_t* const* pcVt = nullptr;
    if (!resolve_dispatch_target("prop", pcObj, pcVt)) return; // game-thread gate
    if (!derive_obj_name_off()) return;
    const void* obj = reinterpret_cast<const void*>(static_cast<uintptr_t>(addr));
    const void* cls = object_class(obj);
    char clsName[64] = {};
    if (!cls || !class_name_of(obj, clsName, sizeof clsName)) {
        BVR_LOG("[bsi] prop: 0x%08X does not walk as a UObject", addr);
        return;
    }
    // s48b RE-derivation, anchored on the derived Name offset. Measured on
    // this build (typed dumps of the class, its first child Function and the
    // +0x34 super, cross-checked by NAME semantics - the super names
    // XFirstPersonMeshActorBase): UObject = {..., HashNext +0x0C (the trap
    // the first scan fell into - a hash-bucket chain of unrelated objects),
    // Outer +0x14, Name +0x18/+0x1C, Class +0x20, ObjectArchetype +0x24
    // (Class-classed on classes and SHARED between two classes, which is what
    // falsified +0x24 as a link - two nodes cannot share a Next)}. So
    // UField::Next = Name+0x10 = +0x28 (the first child's +0x28 points 0x40
    // bytes away - adjacent allocation, a true sibling) and UStruct's
    // SuperStruct/Children pair sits at +0x34/+0x38. The scans below still
    // DERIVE (fixpoint-gated) but start PAST the archetype slot.
    const int nameOff = uobject_name_offset();
    const int childOff = find_link_offset(cls, nameOff + 0x1C, 0xC0);
    if (childOff < 0) {
        BVR_LOG("[bsi] prop: no Children link found on class '%s' (%p)", clsName, cls);
        return;
    }
    const void* first = *reinterpret_cast<const void* const*>(
        static_cast<const uint8_t*>(cls) + childOff);
    const int nextOff = find_link_offset(first, nameOff + 0x10, nameOff + 0x10);
    if (nextOff < 0) {
        BVR_LOG("[bsi] prop: first field %p carries no Next at name+0x10 - layout "
                "drifted from the s48 derivation, refusing (re-derive by bsidump)",
                first);
        return;
    }
    // SuperStruct: the first Class-classed pointer past the archetype slot
    // that is not the metaclass, not named "Class", and not Children.
    int superOff = -1;
    for (int off = nameOff + 0x14; off <= 0xC0 && superOff < 0; off += 4) {
        if (off == childOff) continue;
        const void* p = *reinterpret_cast<const void* const*>(
            static_cast<const uint8_t*>(cls) + off);
        if (!p || p == cls) continue;
        char c2[64] = {};
        if (!class_name_of(p, c2, sizeof c2) || strcmp(c2, "Class") != 0) continue;
        char n2[64] = {};
        const int32_t ni2 = object_name_index(p);
        if (ni2 > 0) patterns::fname_text(ni2, n2, sizeof n2);
        if (strcmp(n2, "Class") == 0) continue; // the metaclass, not a super
        superOff = off;
    }
    BVR_LOG("[bsi] prop: object 0x%08X class '%s' (%p) - Children +0x%X, Next +0x%X, "
            "Super %s0x%X; walking the class chain:",
            addr, clsName, cls, childOff, nextOff, superOff < 0 ? "NOT FOUND " : "+",
            superOff < 0 ? 0 : superOff);
    const bool dumpAll = want[0] == '*';
    int guard = 0;
    const void* walkCls = cls;
    for (int level = 0; level < 12 && walkCls; ++level) {
        char wname[64] = {};
        const int32_t wni = object_name_index(walkCls);
        if (wni > 0) patterns::fname_text(wni, wname, sizeof wname);
        const void* cur = *reinterpret_cast<const void* const*>(
            static_cast<const uint8_t*>(walkCls) + childOff);
        int fieldsHere = 0;
        while (cur && field_like(cur) && guard++ < 2048) {
            ++fieldsHere;
            char nm[64] = {};
            const int32_t ni = object_name_index(cur);
            if (ni > 0) patterns::fname_text(ni, nm, sizeof nm);
            char pcls[64] = {};
            class_name_of(cur, pcls, sizeof pcls);
            const bool match = want[0] && !dumpAll && _stricmp(nm, want) == 0;
            if ((dumpAll || match) && bvr::pattern_scan::is_memory_valid(cur, 0x58)) {
                const uint32_t* d = static_cast<const uint32_t*>(cur);
                // Raw metadata dwords past UField (+0x28) - the ascending
                // column across a class's fields identifies Offset
                // empirically; for a BoolProperty the power-of-two dword
                // nearby is the BitMask.
                BVR_LOG("[bsi] prop:   [%s] %p %-14s %-28s d28=%u d2C=%u d30=%u d34=%u "
                        "d38=0x%X d3C=0x%X d40=%u d44=%u d48=0x%X d4C=0x%X d50=0x%X",
                        wname, cur, pcls, nm[0] ? nm : "(unnamed)", d[0x28 / 4],
                        d[0x2C / 4], d[0x30 / 4], d[0x34 / 4], d[0x38 / 4], d[0x3C / 4],
                        d[0x40 / 4], d[0x44 / 4], d[0x48 / 4], d[0x4C / 4], d[0x50 / 4]);
                if (match)
                    BVR_LOG("[bsi] prop:   ^^ MATCH on class '%s' - eyeball Offset, then "
                            "bsipropbit",
                            wname);
            }
            if (!bvr::pattern_scan::is_memory_valid(
                    static_cast<const uint8_t*>(cur) + nextOff, 4))
                break;
            cur = *reinterpret_cast<const void* const*>(static_cast<const uint8_t*>(cur) +
                                                        nextOff);
        }
        if (!want[0])
            BVR_LOG("[bsi] prop:   class '%s' (%p): %d fields", wname, walkCls, fieldsHere);
        if (superOff < 0) break;
        walkCls = *reinterpret_cast<const void* const*>(
            static_cast<const uint8_t*>(walkCls) + superOff);
    }
    BVR_LOG("[bsi] prop: walk done (%d fields total)", guard);
}

void cmd_propbit(const char* args) {
    unsigned addr = 0, off = 0, mask = 0;
    int setTo = -1;
    if (!args ||
        sscanf_s(args, "%x %x %x %d", &addr, &off, &mask, &setTo) < 3 || !addr || !mask) {
        BVR_LOG("[bsi] prop: usage - bsipropbit <hexObj> <hexByteOff> <hexMask> [0|1] "
                "(no 4th arg = read only)");
        return;
    }
    void* pcObj = nullptr;
    const uint8_t* const* pcVt = nullptr;
    if (!resolve_dispatch_target("propbit", pcObj, pcVt)) return;
    uint8_t* obj = reinterpret_cast<uint8_t*>(static_cast<uintptr_t>(addr));
    if (!bvr::pattern_scan::is_memory_valid(obj + off, 4)) {
        BVR_LOG("[bsi] prop: 0x%08X+0x%X not readable", addr, off);
        return;
    }
    uint32_t* dw = reinterpret_cast<uint32_t*>(obj + off);
    const uint32_t before = *dw;
    if (setTo == 0) *dw = before & ~mask;
    else if (setTo == 1) *dw = before | mask;
    BVR_LOG("[bsi] prop: 0x%08X+0x%X mask 0x%X: %s%s (dword 0x%08X -> 0x%08X)", addr, off,
            mask, (before & mask) ? "was SET" : "was clear",
            setTo < 0 ? "" : (setTo ? ", now SET" : ", now CLEAR"), before, *dw);
}

// The silent chain walk itself: the UProperty object for propName on obj's
// class chain, or nullptr. Shared by the bool and generic accessors below.
const void* find_property_object(const void* obj, const char* propName) {
    if (!obj || !propName) return nullptr;
    if (!derive_obj_name_off()) return nullptr;
    const int nameOff = g_objNameOff;
    const void* cls = object_class(obj);
    if (!cls) return nullptr;
    // The s48-derived link layout, re-scanned (not assumed) each call - the
    // same anchored windows cmd_prop uses.
    const int childOff = find_link_offset(cls, nameOff + 0x1C, 0xC0);
    if (childOff < 0) return nullptr;
    const void* first = *reinterpret_cast<const void* const*>(
        static_cast<const uint8_t*>(cls) + childOff);
    const int nextOff = find_link_offset(first, nameOff + 0x10, nameOff + 0x10);
    if (nextOff < 0) return nullptr;
    int superOff = -1;
    for (int off = nameOff + 0x14; off <= 0xC0 && superOff < 0; off += 4) {
        if (off == childOff) continue;
        const void* p = *reinterpret_cast<const void* const*>(
            static_cast<const uint8_t*>(cls) + off);
        if (!p || p == cls) continue;
        char c2[64] = {};
        if (!class_name_of(p, c2, sizeof c2) || strcmp(c2, "Class") != 0) continue;
        char n2[64] = {};
        const int32_t ni2 = object_name_index(p);
        if (ni2 > 0) patterns::fname_text(ni2, n2, sizeof n2);
        if (strcmp(n2, "Class") == 0) continue;
        superOff = off;
    }
    int guard = 0;
    const void* walkCls = cls;
    for (int level = 0; level < 12 && walkCls; ++level) {
        const void* cur = *reinterpret_cast<const void* const*>(
            static_cast<const uint8_t*>(walkCls) + childOff);
        while (cur && field_like(cur) && guard++ < 4096) {
            char nm[64] = {};
            const int32_t ni = object_name_index(cur);
            if (ni > 0) patterns::fname_text(ni, nm, sizeof nm);
            if (_stricmp(nm, propName) == 0) return cur;
            if (!bvr::pattern_scan::is_memory_valid(
                    static_cast<const uint8_t*>(cur) + nextOff, 4))
                break;
            cur = *reinterpret_cast<const void* const*>(static_cast<const uint8_t*>(cur) +
                                                        nextOff);
        }
        if (superOff < 0) break;
        walkCls = *reinterpret_cast<const void* const*>(
            static_cast<const uint8_t*>(walkCls) + superOff);
    }
    return nullptr;
}

bool find_bool_property_bit(const void* obj, const char* propName, uint32_t* outByteOff,
                            uint32_t* outMask) {
    if (!outByteOff || !outMask) return false;
    const void* p = find_property_object(obj, propName);
    if (!p) return false;
    char pcls[64] = {};
    class_name_of(p, pcls, sizeof pcls);
    if (strcmp(pcls, "BoolProperty") != 0) return false;
    if (!bvr::pattern_scan::is_memory_valid(p, 0x5C)) return false;
    const uint32_t* d = static_cast<const uint32_t*>(p);
    // UProperty::Offset at +0x48, UBoolProperty::BitMask at +0x58 (the s48
    // typed dump); gated for plausibility - one bit, small offset.
    const uint32_t off = d[0x48 / 4];
    const uint32_t mask = d[0x58 / 4];
    if (off == 0 || off > 0x4000) return false;
    if (mask == 0 || (mask & (mask - 1)) != 0) return false;
    *outByteOff = off;
    *outMask = mask;
    return true;
}

bool find_property_offset(const void* obj, const char* propName, const char* expectClass,
                          uint32_t* outByteOff) {
    if (!outByteOff) return false;
    const void* p = find_property_object(obj, propName);
    if (!p) return false;
    char pcls[64] = {};
    class_name_of(p, pcls, sizeof pcls);
    if (expectClass && strcmp(pcls, expectClass) != 0) return false;
    if (!bvr::pattern_scan::is_memory_valid(p, 0x4C)) return false;
    const uint32_t off = static_cast<const uint32_t*>(p)[0x48 / 4];
    if (off == 0 || off > 0x4000) return false;
    *outByteOff = off;
    return true;
}

bool call_on_object_by_index(void* obj, int32_t nameIndex, void* parms) {
    if (!obj || !parms || nameIndex < 0) return false;
    void* pcObj = nullptr;
    const uint8_t* const* pcVt = nullptr;
    if (!resolve_dispatch_target("call_on_object", pcObj, pcVt)) return false;
    if (!bvr::pattern_scan::is_memory_valid(obj, sizeof(void*))) return false;
    const uint8_t* const* vt = *reinterpret_cast<const uint8_t* const* const*>(obj);
    const size_t slotsNeeded = patterns::kProcessEventVtableOffset / 4 + 1;
    if (!vt || !bvr::pattern_scan::is_memory_valid(vt, slotsNeeded * sizeof(void*)))
        return false;
    const uint32_t ffRva = rva_of(vt[patterns::kFindFunctionVtableOffset / 4]);
    const uint32_t peRva = rva_of(vt[patterns::kProcessEventVtableOffset / 4]);
    if (ffRva != patterns::kFindFunctionRva ||
        (peRva != patterns::kActorProcessEventRva && peRva != patterns::kProcessEventRva))
        return false;
    void* func = nullptr;
    uint32_t code = 0;
    return call_by_name_seh(obj, vt, nameIndex, parms, &func, &code) == 0;
}

bool call_on_object(void* obj, const char* funcName, void* parms) {
    // fname_find is a LINEAR scan of the whole pool (~70k entries, ~hundreds
    // of ms) - fine command-driven, NEVER on a cadence (the recorded rule;
    // re-learned the hard way s52: two pollers through here stuttered the
    // whole game at 2-3 Hz). Cadenced callers use find_function_index once +
    // call_on_object_by_index.
    return call_on_object_by_index(obj, patterns::fname_find(funcName), parms);
}

int32_t find_function_index(const char* funcName) { return patterns::fname_find(funcName); }

void* load_object(const char* path) { return do_load_object(path); }

bool handle_command(const char* cmd, const char* args) {
    if (strcmp(cmd, "bsinative") == 0) {
        cmd_native(args);
        return true;
    }
    if (strcmp(cmd, "bsinames") == 0) {
        cmd_names(args);
        return true;
    }
    if (strcmp(cmd, "bsiname") == 0) {
        cmd_name_find(args);
        return true;
    }
    if (strcmp(cmd, "bsivtable") == 0) {
        cmd_vtable(args);
        return true;
    }
    if (strcmp(cmd, "bsicall") == 0) {
        cmd_call(args);
        return true;
    }
    if (strcmp(cmd, "bsifields") == 0) {
        cmd_fields(args);
        return true;
    }
    if (strcmp(cmd, "bsicallat") == 0) {
        cmd_call_at(args);
        return true;
    }
    if (strcmp(cmd, "bsiprop") == 0) {
        cmd_prop(args);
        return true;
    }
    if (strcmp(cmd, "bsipropbit") == 0) {
        cmd_propbit(args);
        return true;
    }
    if (strcmp(cmd, "bsiarray") == 0) {
        cmd_array(args);
        return true;
    }
    if (strcmp(cmd, "bsidump") == 0) {
        cmd_dump(args);
        return true;
    }
    if (strcmp(cmd, "bsichase") == 0) {
        cmd_chase(args);
        return true;
    }
    if (strcmp(cmd, "bsidiff") == 0) {
        cmd_diff(args);
        return true;
    }
    if (strcmp(cmd, "bsiload") == 0) {
        cmd_load(args);
        return true;
    }
    if (strcmp(cmd, "bsiexec") == 0) {
        cmd_exec(args);
        return true;
    }
    if (strcmp(cmd, "bsireflect") == 0) {
        if (args && strncmp(args, "selftest", 8) == 0) {
            cmd_selftest();
        } else {
            BVR_LOG("[bsi] reflect: GNames Num=%d Max=%d | native table %s | commands: "
                    "bsireflect selftest | bsinative <Class> <Func> | bsinames <start> [n] | "
                    "bsiname <text> | bsivtable [n] | bsicall <Func> [floatArg] | "
                    "bsiarray 0x<obj> <off> [n] | bsidump 0x<addr> [n] [start] | "
                    "bsichase 0x<addr> <off> <off> ... | bsiexec <console cmd>",
                    patterns::fname_count(), patterns::fname_max(),
                    g_table.base ? "seeded" : "not seeded yet");
        }
        return true;
    }
    return false;
}

void draw_debug_ui() {
    if (!ImGui::CollapsingHeader("UE3 reflection (DR-I1)")) return;
    ImGui::Text("GNames: Num %d  Max %d", patterns::fname_count(), patterns::fname_max());
    ImGui::Text("native table: %s%zu entries", g_table.base ? "" : "(not seeded) ",
                g_table.count);
    ImGui::Text("ProcessEvent rva 0x%X, vtable slot +0x%X", patterns::kProcessEventRva,
                patterns::kProcessEventVtableOffset);
    ImGui::TextDisabled("run `bsireflect selftest` from the command seam");
}

} // namespace bvr::bsi::reflect
