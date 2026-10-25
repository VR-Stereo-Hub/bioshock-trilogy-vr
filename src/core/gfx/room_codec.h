// Room files, compressed (2026-10-24): a room's six colour faces as JPEG (quality 0.92) and its six distance faces as
// 16-bit greyscale PNG (millimetres, 0 = no distance, lossless), through WIC. A raw room was 48 MB at 1024 a face;
// this is a few MB. The caller owns the file layout; these turn one face to bytes and back.

#pragma once

#include <cstdint>
#include <vector>

namespace bvr::room_codec {

// One colour face: `rgba` w*h*4 bytes (alpha ignored) -> JPEG bytes.
bool encode_colour(const uint8_t* rgba, uint32_t w, uint32_t h, std::vector<uint8_t>& out);
// JPEG bytes -> w*h*4 RGBA (alpha 255). False unless the image is exactly w x h.
bool decode_colour(const uint8_t* bytes, size_t n, uint32_t w, uint32_t h, std::vector<uint8_t>& rgba);
// One distance face: `metres` w*h floats -> 16-bit PNG bytes.
bool encode_depth(const float* metres, uint32_t w, uint32_t h, std::vector<uint8_t>& out);
// PNG bytes -> w*h floats (metres).
bool decode_depth(const uint8_t* bytes, size_t n, uint32_t w, uint32_t h, std::vector<float>& metres);

} // namespace bvr::room_codec
