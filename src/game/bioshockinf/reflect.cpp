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

void cmd_names(const char* args) {
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

void cmd_vtable(const char* args) {
    int count = 40;
    sscanf_s(args, "%d", &count);
    if (count < 1) count = 1;
    if (count > 96) count = 96;

    void* obj = camera::last_player_controller();
    if (!obj) {
        BVR_LOG("[bsi] reflect: no live object yet - the camera hook has not fired, and this "
                "lane deliberately takes objects from hook parameters rather than scanning "
                "(GObjObjects is not used).");
        return;
    }
    if (!bvr::pattern_scan::is_memory_valid(obj, 4)) {
        BVR_LOG("[bsi] reflect: latched object %p is no longer readable", obj);
        return;
    }
    const uint8_t* const* vt = *reinterpret_cast<const uint8_t* const* const*>(obj);
    if (!vt || !bvr::pattern_scan::is_memory_valid(vt, count * sizeof(void*))) {
        BVR_LOG("[bsi] reflect: vtable pointer %p is not readable for %d slots", (void*)vt,
                count);
        return;
    }
    BVR_LOG("[bsi] reflect: APlayerController %p vtable %p (rva 0x%X), first %d slots:", obj,
            (const void*)vt, rva_of(vt), count);
    for (int i = 0; i < count; ++i) {
        const uint8_t* fn = vt[i];
        if (!fn) continue;
        const uint32_t r = rva_of(fn);
        const bool isPe = (static_cast<uint32_t>(i * 4) == patterns::kProcessEventVtableOffset);
        const bool isFf = (static_cast<uint32_t>(i * 4) == patterns::kFindFunctionVtableOffset);
        BVR_LOG("[bsi] reflect:   +0x%02X [%2d] rva 0x%-8X%s", i * 4, i, r,
                isPe ? "  <== ProcessEvent (offline: slot +0x7C)"
                     : (isFf ? "  <== FindFunction (offline: slot +0x54)" : ""));
    }
    // The live cross-check that costs nothing: our offline derivation says slot
    // +0x7C holds ProcessEvent. On an APlayerController - an AActor subclass -
    // the expected occupant is AActor::ProcessEvent, NOT the UObject base.
    const uint32_t idx = patterns::kProcessEventVtableOffset / 4;
    if (static_cast<int>(idx) < count && vt[idx]) {
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
                    "bsiexec <console cmd>",
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
