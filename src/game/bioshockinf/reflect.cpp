#include "game/bioshockinf/reflect.h"

#include "core/util/log.h"
#include "game/bioshockinf/camera.h"
#include "game/bioshockinf/patterns.h"

#include <imgui.h>
#include <windows.h>

#include <cstdio>
#include <cstring>

namespace bvr::bsi::reflect {
namespace {

using bvr::pattern_scan::NativeScanResult;
using bvr::pattern_scan::NativeTableBounds;

bvr::pattern_scan::ProcessImage g_image{};
NativeTableBounds g_table{};
bool g_tableTried = false;

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
    BVR_LOG("[bsi] reflect: native table base RVA 0x%X, %zu entries (seed index %zu). "
            "Offline enumeration said 2647 - %s",
            rva_of(g_table.base), g_table.count, g_table.seedIndex,
            g_table.count == 2647 ? "MATCH" : "MISMATCH, the 8-byte shape is suspect");
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

    if (!ensure_table()) return;
    NativeScanResult walk{};
    const bool okWalk = bvr::pattern_scan::find_native_in_table(
        g_image, bvr::pattern_scan::kNativeTableUE3, g_table, cls, fn, walk);
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
    {
        const bool ok = ensure_table();
        _snprintf_s(detail, sizeof detail, _TRUNCATE, "count=%zu (offline said 2647)",
                    g_table.count);
        c.note(ok && g_table.count == 2647, "shape: native table walks to 2647 entries", detail);
    }

    // --- cross-instrument agreement -----------------------------------------
    if (g_table.base) {
        for (const Known& k : kKnown) {
            NativeScanResult a{}, b{};
            bvr::pattern_scan::find_native_function_ex(g_image,
                                                       bvr::pattern_scan::kNativeTableUE3, k.cls,
                                                       k.fn, a);
            bvr::pattern_scan::find_native_in_table(g_image, bvr::pattern_scan::kNativeTableUE3,
                                                    g_table, k.cls, k.fn, b);
            _snprintf_s(detail, sizeof detail, _TRUNCATE, "%s::%s scan=0x%X walk=0x%X", k.cls,
                        k.fn, rva_of(a.function), rva_of(b.function));
            c.note(a.function != nullptr && a.function == b.function,
                   "cross-check: scan and table-walk agree", detail);
        }
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
    if (strcmp(cmd, "bsireflect") == 0) {
        if (args && strncmp(args, "selftest", 8) == 0) {
            cmd_selftest();
        } else {
            BVR_LOG("[bsi] reflect: GNames Num=%d Max=%d | native table %s | commands: "
                    "bsireflect selftest | bsinative <Class> <Func> | bsinames <start> [n] | "
                    "bsiname <text> | bsivtable [n]",
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
