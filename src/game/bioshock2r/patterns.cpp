#include "game/bioshock2r/patterns.h"

#include "core/util/log.h"

#include <cstring>

namespace bvr::b2r::patterns {
namespace {

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

bool resolve(const bvr::pattern_scan::ProcessImage& image, Symbols& out) {
    using namespace bvr::pattern_scan;

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
