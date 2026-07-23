// eventPlayerCalcView derivation: FName-chain scan, technique ported from
// itsloopyo/bioshock-remastered-headtracking (MIT), src/memory.rs - the
// generic implementation lives in core/hooks/pattern_scan.cpp. The logged RVA
// is the cross-check value against the Rust mod on the same exe build
// (docs/ENGINE_NOTES.md).

#include "game/bioshock1r/patterns.h"

#include "core/util/log.h"

namespace bvr::b1r::patterns {

bool resolve(const bvr::pattern_scan::ProcessImage& image, Symbols& out) {
    using namespace bvr::pattern_scan;

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
    return true;
}

} // namespace bvr::b1r::patterns
