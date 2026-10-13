// gpu-test-sources: src/game/bioshock1r/afw_gpu.cpp
// The alternate frame warp's rebuild under the headset's load, for as long as the game ran
// before the 2026-10-10 driver crashes (0xC0000005 in nvwgf2um.dll+0x48153A on a driver
// worker thread, 5-11 s after AFW came on, both runs): 2750 x 2850 eyes - the tester's size
// - with varied depth (a far wall, mid boxes, a near "hand" block, depth steps everywhere),
// both sources, the stale test on, the camera moving every present, ~130 rebuilds a second
// for 30 s, each result copied out the way the core copies it into the swapchain image.
// Same 32-bit process type and driver as the game. Passes if the driver survives and every
// rebuild succeeds. `BVR_SOAK_SECONDS` overrides the duration.
#include "gpu_test_util.h"

#include "game/bioshock1r/afw_gpu.h"

#include <cstdlib>

using namespace gt;
namespace af = bvr::b1r::afw;

namespace {

bvr::b1r::tgpu::Camera cam_at(double t, int eye, uint32_t w, uint32_t h) {
    bvr::b1r::tgpu::Camera c;
    const double yaw = 0.6 * std::sin(t * 1.3), pitch = 0.15 * std::sin(t * 0.7);
    const float f[3] = {(float)(std::cos(yaw) * std::cos(pitch)), (float)(std::sin(yaw) * std::cos(pitch)), (float)std::sin(pitch)};
    const float r[3] = {(float)-std::sin(yaw), (float)std::cos(yaw), 0.0f};
    const float u[3] = {f[1] * r[2] - f[2] * r[1], f[2] * r[0] - f[0] * r[2], f[0] * r[1] - f[1] * r[0]};
    for (int i = 0; i < 3; ++i) { c.fwd[i] = f[i]; c.right[i] = r[i]; c.up[i] = -u[i]; }
    c.loc[0] = (float)(40.0 * t); c.loc[1] = (float)(10.0 * std::sin(t)); c.loc[2] = 60.0f;
    for (int i = 0; i < 3; ++i) c.loc[i] += c.right[i] * (eye == 0 ? -3.2f : 3.2f);
    c.tanH = 1.191754f;
    c.tanV = c.tanH * h / w;
    return c;
}

} // namespace

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    printf("afw-soak: the rebuild at 2750x2850 under load, as long as the game crashed after\n");
    if (!make_device()) return 1;
    const uint32_t W = 2750, H = 2850;
    double seconds = 30.0;
    if (const char* s = getenv("BVR_SOAK_SECONDS")) seconds = atof(s);

    // Two eyes' images and depths: a busy colour field, and depth with steps at every scale.
    std::vector<uint32_t> col[2];
    std::vector<float> dep[2];
    ID3D11Texture2D *ct[2] = {}, *dt[2] = {};
    ID3D11ShaderResourceView *cs[2] = {}, *ds[2] = {};
    for (int e = 0; e < 2; ++e) {
        col[e].resize((size_t)W * H);
        dep[e].resize((size_t)W * H);
        for (uint32_t y = 0; y < H; ++y)
            for (uint32_t x = 0; x < W; ++x) {
                const size_t i = (size_t)y * W + x;
                col[e][i] = 0xFF000000u | ((x * 7 + e * 40) & 255) | (((y * 5) & 255) << 8) | ((((x ^ y) * 3) & 255) << 16);
                float z = 2000.0f + 500.0f * std::sin(x * 0.003f) * std::cos(y * 0.002f);   // a far wall
                if (((x / 97) + (y / 131)) % 3 == 0) z = 300.0f + (float)((x * 13 + y * 7) % 400);   // boxes, steps
                if (x > W / 2 && y > H * 2 / 3 && x < W * 4 / 5) z = 25.0f + 0.01f * x;   // a near "hand"
                if (((x * 31 + y * 17) % 997) == 0) z = 0.0f;   // sky texels
                dep[e][i] = z;
            }
        ct[e] = make_tex(W, H, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_SHADER_RESOURCE, col[e].data(), W * 4);
        dt[e] = make_tex(W, H, DXGI_FORMAT_R32_FLOAT, D3D11_BIND_SHADER_RESOURCE, dep[e].data(), W * 4);
        cs[e] = srv_of(ct[e]);
        ds[e] = srv_of(dt[e]);
    }
    // The swapchain image the core copies into (typed sRGB, like the runtime's).
    ID3D11Texture2D* dst = make_tex(W, H, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);

    af::WarpGpu warp;
    char why[256] = "";
    LARGE_INTEGER f0, t0, t1;
    QueryPerformanceFrequency(&f0);
    QueryPerformanceCounter(&t0);
    uint64_t runs = 0, failed = 0;
    for (;;) {
        QueryPerformanceCounter(&t1);
        const double t = (double)(t1.QuadPart - t0.QuadPart) / f0.QuadPart;
        if (t > seconds) break;
        const int fresh = (int)(runs & 1), held = fresh ^ 1;
        af::Source B, A;
        B.valid = true; B.color = cs[fresh]; B.depth = ds[fresh]; B.depthW = W; B.depthH = H;
        B.cam = cam_at(t, fresh, W, H);
        A.valid = true; A.color = cs[held]; A.depth = ds[held]; A.depthW = W; A.depthH = H;
        A.cam = cam_at(t - 0.0077, held, W, H);
        af::Params p;
        p.w = W; p.h = H;
        p.target = cam_at(t, held, W, H);
        p.staleTest = true;
        p.tint = (runs / 500) & 1;
        if (warp.run(dev, ctx, A, B, p, why, sizeof(why))) ctx->CopyResource(dst, warp.out());
        else ++failed;
        ++runs;
        ctx->Flush();
        if (runs % 650 == 0) printf("    %.0f s: %llu rebuilds\n", t, (unsigned long long)runs);
        Sleep(runs % 2 ? 7 : 8);   // ~130 a second, the game's rate
    }
    // A readback forces everything queued to have executed.
    const auto px = readback(dst);
    check(failed == 0 && runs > 100, "AFW rebuild soak: the driver survives and every rebuild succeeds",
          "%llu rebuilds in %.0f s, %llu failed%s%s, first pixel %.3f", (unsigned long long)runs, seconds,
          (unsigned long long)failed, failed ? ": " : "", failed ? why : "", px.empty() ? -1.0 : px[0]);
    warp.shutdown();
    for (int e = 0; e < 2; ++e) { rel(cs[e]); rel(ds[e]); rel(ct[e]); rel(dt[e]); }
    rel(dst);
    return finish("afw-soak");
}
