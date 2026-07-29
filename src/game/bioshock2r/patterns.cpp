#include "game/bioshock2r/patterns.h"

#include "core/util/log.h"

namespace bvr::b2r::patterns {

bool resolve(const bvr::pattern_scan::ProcessImage& image, Symbols& out) {
    using namespace bvr::pattern_scan;

    BVR_LOG("[b2r] scanning main module: base %p size 0x%zX", image.base, image.size);

    // The same FName-chain scan that resolves BS1 (core/hooks/pattern_scan.h
    // is game-agnostic by design). M3 needs no other scan: this is an image
    // scan, and the gameplay predicate is a vtable identity check on a
    // pointer the detour is handed - no heap walk anywhere.
    EventScanResult scan{};
    bool ok = find_event_function(image, "PlayerCalcView", scan);

    BVR_LOG("[b2r] \"PlayerCalcView\": %zu wide-string match(es), %zu string xref(s)",
            scan.stringMatches, scan.stringXrefs);
    if (scan.fnameIndexGlobal) {
        BVR_LOG("[b2r] fname index global: %p (%zu xref(s), %zu candidate(s) after init-site filter)",
                scan.fnameIndexGlobal, scan.globalXrefs, scan.candidates);
    }
    if (scan.fnameCtor) {
        // The FName constructor - the entry point to the engine's name system
        // when the GNames derivation is re-run for this exe (BS1 session 20).
        BVR_LOG("[b2r] fname ctor = %p (RVA 0x%X)", scan.fnameCtor,
                static_cast<unsigned>(scan.fnameCtor - image.base));
    }

    if (!ok) {
        const char* stage = scan.stringMatches == 0     ? "wide string not found"
                            : scan.stringXrefs == 0     ? "no string xrefs"
                            : !scan.fnameIndexGlobal    ? "fname index global not found"
                            : scan.candidates == 0      ? "no candidates past init-site filter"
                                                        : "no valid prologue found";
        BVR_LOG("[b2r] scan FAILED (%s) - camera features disabled, game runs flat", stage);
        return false;
    }

    out.eventPlayerCalcView = scan.function;
    BVR_LOG("[b2r] eventPlayerCalcView = %p (RVA 0x%X)", scan.function,
            static_cast<unsigned>(reinterpret_cast<uintptr_t>(scan.function) -
                                  reinterpret_cast<uintptr_t>(image.base)));
    return true;
}

} // namespace bvr::b2r::patterns
