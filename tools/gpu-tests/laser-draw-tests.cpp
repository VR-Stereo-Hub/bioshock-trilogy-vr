// gpu-test-sources: src/game/bioshock1r/laser_draw.cpp
// The in-image laser (game/bioshock1r/laser_draw): drawn into an eye image through the image's
// own camera, its dots must land exactly where the fire ray projects - computed here from the
// camera independently of the module - the reticle must be a ring at the reticle's distance,
// and the device state the game left must come back. Controls: an invalid segment, a ray
// behind the camera and an image that cannot be rendered into draw nothing.
#include "gpu_test_util.h"
#include "game/bioshock1r/laser_draw.h"
#include "game/bioshock1r/temporal_gpu.h"

#include <cstdarg>
#include <cmath>

using namespace gt;
namespace ld = bvr::b1r::laser_draw;
namespace tg = bvr::b1r::tgpu;

namespace bvr::log {
void write(const char* fmt, ...) {
    char line[512];
    va_list a; va_start(a, fmt);
    _vsnprintf_s(line, sizeof(line), _TRUNCATE, fmt, a);
    va_end(a);
    printf("      | %s\n", line);
}
} // namespace bvr::log

namespace {
const uint32_t W = 256, H = 240;

tg::Camera camera() {
    tg::Camera c;
    c.loc[0] = 100; c.loc[1] = -20; c.loc[2] = 50;
    const float yaw = 0.3f;
    c.fwd[0] = std::cos(yaw); c.fwd[1] = std::sin(yaw); c.fwd[2] = 0;
    c.right[0] = -std::sin(yaw); c.right[1] = std::cos(yaw); c.right[2] = 0;
    c.up[0] = 0; c.up[1] = 0; c.up[2] = 1;
    c.tanH = 1.0f; c.tanV = 0.9375f;
    return c;
}

// The ray's point at `d` uu, as a pixel through `c` - the test's own projection.
bool pixel_of(const tg::Camera& c, const ld::Segment& s, float d, double* px, double* py) {
    double v[3];
    for (int i = 0; i < 3; ++i) v[i] = s.origin[i] + s.dir[i] * d - c.loc[i];
    const double z = v[0] * c.fwd[0] + v[1] * c.fwd[1] + v[2] * c.fwd[2];
    if (z < 1) return false;
    const double x = (v[0] * c.right[0] + v[1] * c.right[1] + v[2] * c.right[2]) / (z * c.tanH);
    const double y = (v[0] * c.up[0] + v[1] * c.up[1] + v[2] * c.up[2]) / (z * c.tanV);
    *px = (x * 0.5 + 0.5) * W;
    *py = (0.5 - y * 0.5) * H;
    return true;
}

float red_at(const std::vector<float>& img, double px, double py) {
    const int x = (int)std::floor(px), y = (int)std::floor(py);
    if (x < 0 || y < 0 || x >= (int)W || y >= (int)H) return -1;
    return img[((size_t)y * W + x) * 4];
}
} // namespace

int main() {
    printf("laser-draw: the aim laser drawn into the eye image through its camera\n");
    if (!make_device()) return 1;
    const tg::Camera cam = camera();

    ld::Segment s;
    s.valid = s.laser = s.reticle = true;
    // A muzzle 30 uu ahead, a little right and below the eye, the ray turned slightly.
    for (int i = 0; i < 3; ++i) s.origin[i] = cam.loc[i] + cam.fwd[i] * 30 + cam.right[i] * 6 - cam.up[i] * 8;
    float d[3] = {cam.fwd[0] + 0.05f * cam.right[0], cam.fwd[1] + 0.05f * cam.right[1], 0.04f};
    const float n = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    for (int i = 0; i < 3; ++i) s.dir[i] = d[i] / n;
    s.dots = 6; s.nearUu = 5; s.farUu = 500; s.reticleUu = 500;
    s.dotDeg = 4.0f; s.reticleDeg = 2.0f;

    // An eye swapchain image: typeless, render target + sampled, cleared black.
    std::vector<uint8_t> black((size_t)W * H * 4, 0);
    ID3D11Texture2D* img = make_tex(W, H, DXGI_FORMAT_R8G8B8A8_TYPELESS,
                                    D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, black.data(), W * 4);
    // The state the game left behind, which must come back.
    D3D11_VIEWPORT gameVp = {3, 4, 10, 11, 0, 1};
    ctx->RSSetViewports(1, &gameVp);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP);

    const bool drew = ld::draw(ctx, img, W, H, cam, s);
    D3D11_VIEWPORT after = {};
    UINT nvp = 1;
    ctx->RSGetViewports(&nvp, &after);
    D3D11_PRIMITIVE_TOPOLOGY topo;
    ctx->IAGetPrimitiveTopology(&topo);
    check(drew && nvp == 1 && after.TopLeftX == 3 && after.Width == 10 && topo == D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP,
          "draws, and the game's pipeline state comes back", "drew %d, viewport %.0f+%.0f, topology %d", drew,
          after.TopLeftX, after.Width, (int)topo);

    const std::vector<float> out = readback(img);
    // Every laser dot centre: red, where the camera puts the ray at that distance.
    int hits = 0, tested = 0;
    float worst = 1;
    for (int i = 0; i < s.dots; ++i) {
        const float t = (float)i / (s.dots - 1);
        const float dist = s.nearUu * std::pow(s.farUu / s.nearUu, t);
        double px, py;
        if (!pixel_of(cam, s, dist, &px, &py) || i == s.dots - 1) continue; // the last sits under the ring
        ++tested;
        const float r = red_at(out, px, py);
        worst = std::min(worst, r);
        if (r > 0.5f) ++hits;
    }
    check(tested >= 4 && hits == tested, "every laser dot lands on the projected fire ray",
          "%d of %d dot centres red (weakest %.2f)", hits, tested, worst);

    // The reticle: a ring at its distance - empty centre, red band.
    double rx, ry;
    pixel_of(cam, s, s.reticleUu, &rx, &ry);
    const float ringPx = std::tan(s.reticleDeg * 3.14159265f / 180.0f) / cam.tanH * W * 0.5f;
    const float centre = red_at(out, rx, ry);
    const float band = std::max({red_at(out, rx + 0.8 * ringPx, ry), red_at(out, rx - 0.8 * ringPx, ry),
                                 red_at(out, rx, ry + 0.8 * ringPx), red_at(out, rx, ry - 0.8 * ringPx)});
    // The last laser dot also sits at the reticle distance; the ring is the one to see there.
    check(band > 0.3f, "the reticle ring sits at the reticle's point on the ray", "band %.2f at %.1f px, centre %.2f",
          band, 0.8 * ringPx, centre);

    // Away from the ray: untouched.
    float stray = 0;
    for (uint32_t y = 0; y < 20; ++y)
        for (uint32_t x = 0; x < 20; ++x) stray = std::max(stray, out[((size_t)y * W + x) * 4 + 0]);
    check(stray == 0, "nothing is drawn away from the ray", "max red %.3f in the top-left corner", stray);

    // Controls.
    ld::Segment off = s;
    off.valid = false;
    check(!ld::draw(ctx, img, W, H, cam, off), "control: an invalid segment draws nothing", "");
    ld::Segment behind = s;
    for (int i = 0; i < 3; ++i) { behind.origin[i] = cam.loc[i] - cam.fwd[i] * 50; behind.dir[i] = -cam.fwd[i]; }
    check(!ld::draw(ctx, img, W, H, cam, behind), "control: a ray behind the camera draws nothing", "");
    ID3D11Texture2D* sampledOnly = make_tex(W, H, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_SHADER_RESOURCE);
    check(!ld::draw(ctx, sampledOnly, W, H, cam, s), "control: an image that cannot be rendered into is refused", "");
    rel(sampledOnly);
    rel(img);
    ld::release();
    return finish("laser-draw");
}
