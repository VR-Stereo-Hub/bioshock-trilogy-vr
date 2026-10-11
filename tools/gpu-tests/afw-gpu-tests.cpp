// gpu-test-sources: src/game/bioshock1r/afw_gpu.cpp
// The alternate frame warp's held-eye rebuild (game/bioshock1r/afw_gpu) against a
// ray-traced truth. One AlternateEye step: the LEFT eye's image is a frame old, the RIGHT
// eye was just rendered; between them the player walked, turned and the hand moved.
// The rebuilt left eye must match a true render of the left eye NOW far better than what
// the compositor would show (the old image, rotated), and every part of the method must
// earn its place against a control that switches it off.
#include "gpu_test_util.h"
#include "game/bioshock1r/afw_gpu.h"

#include <algorithm>

using namespace gt;
namespace af = bvr::b1r::afw;
namespace tg = bvr::b1r::tgpu;

namespace {

const double kNear = 10.0, kFar = 65536.0;
const double kHalfIpd = 3.2;   // uu: 64 mm at the shipped 100 uu per metre

tg::Camera to_tcam(const Cam& c) {
    tg::Camera t;
    double f[3], r[3], u[3];
    c.basis(f, r, u);
    for (int i = 0; i < 3; ++i) { t.loc[i] = (float)c.loc[i]; t.fwd[i] = (float)f[i]; t.right[i] = (float)r[i]; t.up[i] = (float)u[i]; }
    t.tanH = (float)c.tanH; t.tanV = (float)c.tanV;
    return t;
}

Scene make_scene() {
    Scene s;
    s.solids.push_back({{100, -30, -100}, {160, 30, 200}, 1});   // pillar
    s.solids.push_back({{-60, 40, 0}, {-20, 120, 80}, 2});       // crate
    s.solids.push_back({{-10, -140, -40}, {60, -90, 30}, 4});
    s.solids.push_back({{-108, 4, 40}, {-98, 22, 56}, 3});       // the hand, ~45 uu ahead
    s.movingIndex = 3;
    return s;
}

Cam head(double x, double y, double z, double yawDeg, double pitchDeg, uint32_t w, uint32_t h) {
    Cam c;
    c.loc[0] = x; c.loc[1] = y; c.loc[2] = z;
    c.rot = {(int32_t)(pitchDeg * bvr::ue::kRotUnitsPerDegree), (int32_t)(yawDeg * bvr::ue::kRotUnitsPerDegree), 0};
    c.tanH = std::tan(52.0 * 3.14159265 / 180.0);
    c.tanV = c.tanH * h / w;
    return c;
}
Cam eye_of(const Cam& headCam, int sign) {
    Cam e = headCam;
    double f[3], r[3], u[3];
    headCam.basis(f, r, u);
    for (int i = 0; i < 3; ++i) e.loc[i] += r[i] * sign * kHalfIpd;
    return e;
}

struct Gpu {
    ID3D11Texture2D* color = nullptr; ID3D11ShaderResourceView* colorSrv = nullptr;
    ID3D11Texture2D* depth = nullptr; ID3D11ShaderResourceView* depthSrv = nullptr;
    ~Gpu() { rel(colorSrv); rel(color); rel(depthSrv); rel(depth); }
};
af::Source source_of(Gpu& g, const Render& col, uint32_t cw, uint32_t ch, const Render& dep, uint32_t dw, uint32_t dh, const Cam& cam) {
    g.color = make_tex(cw, ch, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_SHADER_RESOURCE, col.rgba.data(), cw * 4);
    g.colorSrv = srv_of(g.color);
    g.depth = make_tex(dw, dh, DXGI_FORMAT_R32_FLOAT, D3D11_BIND_SHADER_RESOURCE, dep.viewZ.data(), dw * 4);
    g.depthSrv = srv_of(g.depth);
    af::Source s;
    s.valid = true; s.color = g.colorSrv; s.depth = g.depthSrv; s.depthW = dw; s.depthH = dh; s.cam = to_tcam(cam);
    return s;
}

struct Score { double mean = 0; double bad = 0; double handRegion = 0; };
// bad: fraction of pixels off by more than 0.12 (a visible error); handRegion: the mean
// error over the pixels where the hand was in the OLD image's reprojection or is now.
Score score(const std::vector<float>& got, const Render& truth, const std::vector<uint8_t>& handMask, uint32_t w, uint32_t h) {
    Score s;
    size_t n = 0, nb = 0, nh = 0;
    double hs = 0;
    const uint32_t b = w / 40;
    for (uint32_t y = b; y < h - b; ++y)
        for (uint32_t x = b; x < w - b; ++x) {
            const size_t i = (size_t)y * w + x;
            double e = 0;
            for (int c = 0; c < 3; ++c) e += std::fabs(got[i * 4 + c] - ((truth.rgba[i] >> (8 * c)) & 255) / 255.0);
            e /= 3;
            s.mean += e; ++n;
            if (e > 0.12) ++nb;
            if (handMask[i]) { hs += e; ++nh; }
        }
    s.mean /= n; s.bad = (double)nb / n; s.handRegion = nh ? hs / nh : 0;
    return s;
}

// What the compositor shows without the warp: the held image rotated to the new head
// orientation (rotation only, no depth).
std::vector<float> rotation_only(const Render& a, const Cam& camA, const Cam& target, uint32_t w, uint32_t h) {
    std::vector<float> out((size_t)w * h * 4, 0.0f);
    for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w; ++x) {
            double d[3];
            pixel_dir(target, w, h, x + 0.5, y + 0.5, d);
            double p[3] = {camA.loc[0] + d[0] * 1e6, camA.loc[1] + d[1] * 1e6, camA.loc[2] + d[2] * 1e6};
            double u, v;
            if (!project(camA, p, &u, &v)) continue;
            const int sx = std::min(std::max((int)(u * w), 0), (int)w - 1), sy = std::min(std::max((int)(v * h), 0), (int)h - 1);
            const uint32_t px = a.rgba[(size_t)sy * w + sx];
            for (int c = 0; c < 4; ++c) out[((size_t)y * w + x) * 4 + c] = ((px >> (8 * c)) & 255) / 255.0f;
        }
    return out;
}

} // namespace

int main() {
    printf("afw-gpu: the alternate frame warp's held-eye rebuild\n");
    if (!make_device()) return 1;
    af::WarpGpu warp;
    char why[256] = "";

    for (int sr = 0; sr < 2; ++sr) {
        // DLAA-sized (colour = depth size) and super-resolution-sized (colour 1.5x the depth).
        const uint32_t DW = 320, DH = 300;
        const uint32_t W = sr ? 480 : DW, H = sr ? 450 : DH;
        printf(" -- %s: colour %ux%u, depth %ux%u\n", sr ? "super resolution" : "native", W, H, DW, DH);
        Scene before = make_scene(), now = make_scene();
        now.objShift[1] = 5.0; now.objShift[2] = 2.0;   // the hand moved 5 uu right, 2 up
        const Cam h0 = head(-150, 20, 60, 0.0, -4.0, W, H);
        const Cam h1 = head(-146, 21, 60, 2.5, -3.0, W, H);   // walked 4 uu, turned 2.5 deg
        const Cam a = eye_of(h0, -1), b = eye_of(h1, +1), t = eye_of(h1, -1);
        Cam ad = a, bd = b; ad.tanV = bd.tanV = a.tanH * DH / DW;
        // Colour at W x H, depth at DW x DH (the same cameras).
        const Render ac = render(before, a, W, H, kNear, kFar, 2), bc = render(now, b, W, H, kNear, kFar, 2);
        const Render az = render(before, ad, DW, DH, kNear, kFar, 1), bz = render(now, bd, DW, DH, kNear, kFar, 1);
        const Render truth = render(now, t, W, H, kNear, kFar, 2);
        // The hand region: where the hand is now, or where the old image's hand lands now.
        std::vector<uint8_t> handMask((size_t)W * H, 0);
        {
            Scene oldOnly = before;   // the old hand seen from the target
            const Render oldHand = render(oldOnly, t, W, H, kNear, kFar, 1);
            for (uint32_t y = 0; y < H; ++y)
                for (uint32_t x = 0; x < W; ++x) {
                    double d[3];
                    pixel_dir(t, W, H, x + 0.5, y + 0.5, d);
                    const Hit hn = now.cast(t.loc, d), ho = before.cast(t.loc, d);
                    if (hn.id == 3 || ho.id == 3) handMask[(size_t)y * W + x] = 1;
                }
            (void)oldHand;
        }
        Gpu ga, gb;
        const af::Source A = source_of(ga, ac, W, H, az, DW, DH, ad), B = source_of(gb, bc, W, H, bz, DW, DH, bd);
        af::Params p;
        p.w = W; p.h = H;
        p.target = to_tcam(t);
        auto run = [&](const af::Params& q) -> std::vector<float> {
            if (!warp.run(dev, ctx, A, B, q, why, sizeof(why))) { printf("    run failed: %s\n", why); return {}; }
            return readback(warp.out());
        };
        const auto full = run(p);
        if (full.empty()) return finish("afw-gpu");
        const Score sf = score(full, truth, handMask, W, H);
        const Score sr0 = score(rotation_only(ac, a, t, W, H), truth, handMask, W, H);
        af::Params pa = p; pa.useFresh = false;
        const Score sa = score(run(pa), truth, handMask, W, H);
        af::Params pb = p; pb.useHeld = false;
        const Score sb = score(run(pb), truth, handMask, W, H);
        af::Params ps = p; ps.staleTest = false;
        const Score ss = score(run(ps), truth, handMask, W, H);
        printf("    error (mean / bad px / hand region): AFW %.4f / %.2f%% / %.4f | rotation-only %.4f / %.2f%% / %.4f\n",
               sf.mean, sf.bad * 100, sf.handRegion, sr0.mean, sr0.bad * 100, sr0.handRegion);
        printf("    controls: held only %.4f / %.2f%% / %.4f | fresh only %.4f / %.2f%% / %.4f | no stale test %.4f / %.2f%% / %.4f\n",
               sa.mean, sa.bad * 100, sa.handRegion, sb.mean, sb.bad * 100, sb.handRegion, ss.mean, ss.bad * 100, ss.handRegion);
        check(sf.mean < 0.35 * sr0.mean, "AFW beats the compositor's rotation-only held eye",
              "%.4f vs %.4f", sf.mean, sr0.mean);
        check(sf.bad < 0.02, "few visibly wrong pixels", "%.2f%% of pixels off by > 0.12", sf.bad * 100);
        check(sf.handRegion < 0.6 * sa.handRegion, "the fresh eye carries the moved hand (control: held eye only)",
              "hand region %.4f vs %.4f", sf.handRegion, sa.handRegion);
        check(sf.handRegion < 0.8 * ss.handRegion, "the stale test removes the old hand (control: test off)",
              "hand region %.4f vs %.4f", sf.handRegion, ss.handRegion);
        check(sf.mean <= sb.mean * 1.02, "the held eye's own image is worth keeping (control: fresh eye only)",
              "%.4f vs %.4f", sf.mean, sb.mean);
        if (!sr) {
            // Drawing the mesh in bands changes nothing (SV_VertexID counts from each band's start).
            af::Params pband = p; pband.maxDrawVerts = 6006;
            const auto banded = run(pband);
            double maxd = banded.size() == full.size() ? 0.0 : 1.0;
            for (size_t i = 0; i < banded.size() && i < full.size(); ++i) maxd = std::max(maxd, (double)std::fabs(banded[i] - full[i]));
            check(maxd < 1e-6, "the mesh drawn in 6006-vertex bands equals one draw", "max difference %.2e (%u cells)",
                  maxd, (DW - 1) / 2 * ((DH - 1) / 2));
            // The hand depth gain: the hands' depth reads 12% too far in both sources (the
            // Dishonored foreground-gain shape). Uncorrected the hand is rebuilt at the wrong
            // place; with gain 1.12 on the near band it is where it was with true depth.
            Render azg = az, bzg = bz;
            for (float& z : azg.viewZ) if (z > 0 && z < 80) z *= 1.12f;
            for (float& z : bzg.viewZ) if (z > 0 && z < 80) z *= 1.12f;
            Gpu gag, gbg;
            const af::Source Ag = source_of(gag, ac, W, H, azg, DW, DH, ad), Bg = source_of(gbg, bc, W, H, bzg, DW, DH, bd);
            auto runG = [&](float gain) -> Score {
                af::Params q = p; q.fgGain = gain; q.fgBandUu = 80.0f;
                if (!warp.run(dev, ctx, Ag, Bg, q, why, sizeof(why))) return Score{1, 1, 1};
                return score(readback(warp.out()), truth, handMask, W, H);
            };
            const Score g1 = runG(1.0f), g112 = runG(1.12f);
            check(g112.handRegion <= sf.handRegion * 1.02 && g1.handRegion > g112.handRegion * 1.05,
                  "hand depth gain: a 12% depth error on the hands is undone by gain 1.12",
                  "hand region %.4f with the gain, %.4f without, %.4f with true depth", g112.handRegion, g1.handRegion,
                  sf.handRegion);
        }
        // A still frame: target = A's own camera, nothing moved -> the rebuild is A itself.
        {
            const Render za = render(before, a, W, H, kNear, kFar, 2);
            af::Params pq = p; pq.target = to_tcam(a);
            const af::Source B2 = A;   // fresh = the same view (a degenerate but valid pair)
            if (warp.run(dev, ctx, A, B2, pq, why, sizeof(why))) {
                const auto got = readback(warp.out());
                std::vector<uint8_t> none((size_t)W * H, 0);
                const Score st = score(got, za, none, W, H);
                check(st.mean < 0.01 && st.bad < 0.002, "identity: a still view rebuilds as itself",
                      "mean %.4f, %.3f%% bad", st.mean, st.bad * 100);
                if (st.bad >= 0.002) {
                    int shown = 0;
                    for (uint32_t y = W / 40; y < H - W / 40 && shown < 12; ++y)
                        for (uint32_t x = W / 40; x < W - W / 40 && shown < 12; ++x) {
                            const size_t i = (size_t)y * W + x;
                            double e = 0;
                            for (int c = 0; c < 3; ++c) e += std::fabs(got[i * 4 + c] - ((za.rgba[i] >> (8 * c)) & 255) / 255.0);
                            if (e / 3 > 0.12) {
                                double d[3]; pixel_dir(a, W, H, x + 0.5, y + 0.5, d);
                                const Hit hh = before.cast(a.loc, d);
                                printf("      bad (%u,%u) err %.3f surface %d z %.1f\n", x, y, e / 3, hh.id, hh.t);
                                ++shown;
                            }
                        }
                }
            }
        }
    }
    // Cost at a headset size (2064 x 2208 per eye), timed on the GPU.
    {
        const uint32_t W = 2064, H = 2208;
        std::vector<uint32_t> col((size_t)W * H, 0xFF808080u);
        std::vector<float> dep((size_t)W * H, 300.0f);
        ID3D11Texture2D* ct = make_tex(W, H, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_SHADER_RESOURCE, col.data(), W * 4);
        ID3D11Texture2D* dt = make_tex(W, H, DXGI_FORMAT_R32_FLOAT, D3D11_BIND_SHADER_RESOURCE, dep.data(), W * 4);
        ID3D11ShaderResourceView *cs = srv_of(ct), *ds = srv_of(dt);
        const Cam c = head(0, 0, 0, 0, 0, W, H);
        af::Source s; s.valid = true; s.color = cs; s.depth = ds; s.depthW = W; s.depthH = H; s.cam = to_tcam(c);
        af::Params p; p.w = W; p.h = H; p.target = to_tcam(eye_of(c, -1));
        D3D11_QUERY_DESC qd = {D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
        ID3D11Query* dj = nullptr; dev->CreateQuery(&qd, &dj);
        qd.Query = D3D11_QUERY_TIMESTAMP;
        ID3D11Query *q0 = nullptr, *q1 = nullptr; dev->CreateQuery(&qd, &q0); dev->CreateQuery(&qd, &q1);
        for (int i = 0; i < 5; ++i) warp.run(dev, ctx, s, s, p, why, sizeof(why));   // warm up
        ctx->Begin(dj); ctx->End(q0);
        const int kRuns = 10;
        for (int i = 0; i < kRuns; ++i) warp.run(dev, ctx, s, s, p, why, sizeof(why));
        ctx->End(q1); ctx->End(dj);
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dd = {}; UINT64 t0 = 0, t1 = 0;
        while (ctx->GetData(dj, &dd, sizeof(dd), 0) != S_OK) Sleep(1);
        while (ctx->GetData(q0, &t0, sizeof(t0), 0) != S_OK) Sleep(1);
        while (ctx->GetData(q1, &t1, sizeof(t1), 0) != S_OK) Sleep(1);
        const double ms = dd.Disjoint ? -1.0 : (double)(t1 - t0) * 1000.0 / dd.Frequency / kRuns;
        check(ms > 0 && ms < 4.0, "cost per rebuild at 2064x2208", "%.3f ms (grid %d)", ms, p.grid);
        rel(dj); rel(q0); rel(q1); rel(cs); rel(ds); rel(ct); rel(dt);
    }
    warp.shutdown();
    return finish("afw-gpu");
}
