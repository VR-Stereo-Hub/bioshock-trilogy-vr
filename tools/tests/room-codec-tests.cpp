// room-codec-tests.cpp - host tests for src/core/gfx/room_codec, the compressed room file's faces.
// Run `.\tools\host-test.ps1 room-codec`.
//
// What has to hold:
//   colour - a face survives JPEG within a few levels, and is far smaller than raw
//   depth  - distances survive the 16-bit PNG to the millimetre; none (0) stays none; beyond 65.5 m clamps
//   size   - a decode of the wrong size is refused (a room of another face size never loads as garbage)
#include "core/gfx/room_codec.cpp"

#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")

#include <cmath>
#include <cstdio>
#include <cstdlib>

static int g_checks = 0, g_fails = 0;
static void check(bool ok, const char* what, double got = 0, double want = 0) {
    ++g_checks;
    if (!ok) {
        ++g_fails;
        printf("  FAIL %s: got %.4f want %.4f\n", what, got, want);
    }
}

int main() {
    using namespace bvr::room_codec;
    const uint32_t n = 1024;
    // ---- colour: a smooth room-like face (gradients and a few edges) ----
    {
        std::vector<uint8_t> rgba(static_cast<size_t>(n) * n * 4);
        for (uint32_t y = 0; y < n; ++y)
            for (uint32_t x = 0; x < n; ++x) {
                uint8_t* p = &rgba[(static_cast<size_t>(y) * n + x) * 4];
                p[0] = static_cast<uint8_t>(x / 4);
                p[1] = static_cast<uint8_t>(y / 4);
                p[2] = static_cast<uint8_t>(((x / 128 + y / 128) & 1) ? 200 : 40);
                p[3] = 255;
            }
        std::vector<uint8_t> jpg, back;
        check(encode_colour(rgba.data(), n, n, jpg), "colour encodes");
        check(!jpg.empty() && jpg.size() < rgba.size() / 10, "a JPEG face is under a tenth of raw", jpg.size(),
              rgba.size() / 10.0);
        check(decode_colour(jpg.data(), jpg.size(), n, n, back), "colour decodes");
        double err = 0;
        for (size_t i = 0; i < rgba.size(); ++i)
            if ((i & 3) != 3) err += std::abs(static_cast<int>(rgba[i]) - static_cast<int>(back[i]));
        err /= static_cast<double>(n) * n * 3;
        check(err < 3.0, "mean colour error under 3 levels", err, 3.0);
        check(back[3] == 255 && back[back.size() - 1] == 255, "alpha comes back opaque");
        std::vector<uint8_t> wrong;
        check(!decode_colour(jpg.data(), jpg.size(), 512, 512, wrong), "a decode of the wrong size is refused");
    }
    // ---- depth ----
    {
        std::vector<float> m(static_cast<size_t>(n) * n);
        for (size_t i = 0; i < m.size(); ++i) m[i] = 0.5f + static_cast<float>(i % 7919) * 0.003f; // 0.5 .. 24 m
        m[0] = 0.0f;    // no distance
        m[1] = 80.0f;   // beyond the 16-bit range
        m[2] = 1.2345f; // a millimetre's rounding
        std::vector<uint8_t> png;
        std::vector<float> back;
        check(encode_depth(m.data(), n, n, png), "depth encodes");
        check(decode_depth(png.data(), png.size(), n, n, back), "depth decodes");
        float worst = 0;
        for (size_t i = 3; i < m.size(); ++i) {
            const float e = std::fabs(back[i] - m[i]);
            if (e > worst) worst = e;
        }
        check(worst <= 0.0006f, "every distance within half a millimetre", worst, 0.0005);
        check(back[0] == 0.0f, "no distance stays none");
        check(std::fabs(back[1] - 65.535f) < 1e-3f, "beyond 65.5 m clamps", back[1], 65.535);
        check(std::fabs(back[2] - 1.235f) < 1e-4f, "rounded to the millimetre", back[2], 1.235);
    }
    printf("room-codec: %d checks, %d failed\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
