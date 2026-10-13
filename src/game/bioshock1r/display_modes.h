#pragma once
// Display ordering is separate from the existing saved DLSS quality values and seam words.
#include <cmath>
#include <cstdint>
namespace bvr::b1r::display {
// Tester-specified 100% baseline (2026-10-11), independent of runtime scaling.
inline constexpr uint32_t kReferenceW=2750, kReferenceH=2850;
inline constexpr int kQualityOrder[] = {-1, 1, 2, 3, 4, 0}; // -1 = DLAA
inline constexpr const char* kModeNames[] = {
    "Full resolution (DLAA)", "Quality (67% per axis)", "Balanced (58%)",
    "Performance (50%)", "Ultra Performance (33%)", "Ultra Quality (77%)"};
inline constexpr const char* kModeTips[] = {
    "Full resolution: the sharpest and most expensive.",
    "Renders at 67% per axis (45% of the pixels) and rebuilds the rest.",
    "Renders at 58% per axis (34% of the pixels).",
    "Renders at 50% per axis (25% of the pixels).",
    "Renders at 33% per axis (11% of the pixels): the softest.",
    "Renders at 77% per axis (59% of the pixels): nearly DLAA, a little cheaper."};
inline int mode_index(int upscaler, int quality) {
    if (upscaler != 2) return 0;
    for (int i = 1; i < 6; ++i) if (kQualityOrder[i] == quality) return i;
    return 1;
}
inline void pixel_size(uint32_t referenceW, uint32_t referenceH, float percent,
                       uint32_t* w, uint32_t* h) {
    if (!std::isfinite(percent) || percent <= 0) { *w = *h = 0; return; }
    const double scale = std::sqrt(percent * 0.01);
    *w = static_cast<uint32_t>(referenceW * scale + 0.5) & ~1u;
    *h = static_cast<uint32_t>(referenceH * scale + 0.5) & ~1u;
}
} // namespace bvr::b1r::display
