#pragma once
// Local opt-in proof only. Repeats native draws into scratch targets, compares
// same-eye output, then stops. Mode 3 exercises private stereo constants; no visible repair.
#include <cstdint>
struct ID3D11DeviceContext;
struct ID3D11Texture2D;
namespace bvr::pattern_scan { struct ProcessImage; }
namespace bvr::b1r::native_proof {
void init(const bvr::pattern_scan::ProcessImage& image);
// Offline fixture entry: begin at the next Present, one window, no files.
// Like runtime arming, a completed process cannot re-arm.
void request(bool masked = false);
void request_eye(); // offline fixture: mode 3 at the next Present
// Final per-build camera, on the game thread. A fresh same-thread tag is required;
// no retained game object, cross-thread camera guess or old menu identity is used.
// Copies the position immediately for the precision budget; no pointer is retained.
void note_eye(int sign, float fullSeparationUu, const float worldPosition[3]);
// Five measured workloads: unmasked, empty, tiled 1%, tiled 5%, all.
struct MaskStats {
    uint32_t tested = 0, timed = 0;
    uint64_t selected = 0, selectedChanged = 0, selectedDiff = 0, outsideDiff = 0, stencilDiff = 0;
    uint64_t ia = 0, vs = 0, ps = 0;
    double copyMs = 0, maskMs = 0, drawMs = 0, submitUs = 0;
};
struct EyeStats {
    uint32_t tested = 0, changed = 0, buffers = 0, fields = 0;
    uint64_t constantBytes = 0, cloneColourDiff = 0, cloneDepthDiff = 0;
    uint64_t shiftedColour = 0, shiftedDepth = 0, originalConstantDiff = 0;
    double residual = 0; // largest fraction of the source camera-origin allowance
};
struct Stats {
    bool finished = false, failed = false;
    uint32_t candidates = 0, tested = 0, changed = 0, skipped = 0;
    uint64_t colourDiff = 0, depthDiff = 0, originalDiff = 0;
    MaskStats masks[5];
    EyeStats eyes;
};
Stats stats(); // render thread, or after the stopped offline fixture
namespace detail {
// Shared readback checker; only the two supported 32-bit surface formats.
// False on incompatible descriptors or failed Map, never an equality result.
bool compare_pixels(ID3D11DeviceContext*, ID3D11Texture2D*, ID3D11Texture2D*, uint64_t& different);
}
} // namespace bvr::b1r::native_proof
