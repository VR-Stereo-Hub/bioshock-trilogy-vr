#pragma once
// Bounded, read-only diagnostic for sparse native stereo repair. Does not render,
// modify constants, retain engine objects or change AFW output.
#include <cstdint>
namespace bvr::pattern_scan { struct ProcessImage; }
namespace bvr::b1r::native_audit {
// [Bioshock1R] NativeRepairAudit=1 in the DLL-adjacent BioshockVR.ini.
// Default off. After five seconds of world + foreground draws, records one frame.
void init(const bvr::pattern_scan::ProcessImage& image);
// Before the first Present only: the NEXT Present starts a complete window.
// The directory must not exist. Also used by the offline GPU fixture.
void request(const wchar_t* directory);
struct Stats {
    bool finished, failed;
    uint32_t draws, foreground, constants, missingMetadata, unsupported;
    uint64_t constantBytes;
};
Stats stats(); // render thread, or after the offline fixture has stopped recording
} // namespace bvr::b1r::native_audit
