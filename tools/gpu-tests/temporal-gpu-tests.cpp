// gpu-test-sources: src/game/bioshock1r/temporal_gpu.cpp
// The BS1 temporal guides (game/bioshock1r/temporal_gpu) against a ray-traced scene with
// known geometry: the vectors must match the true reprojection of every pixel's world point,
// the depth must survive the game's real D24 format, and the audit must rank the vectors
// below "no motion" and pick the true depth scale. Negative controls: rotation-only vectors
// under translation, and a wrong near plane.
#include "gpu_test_util.h"
#include "game/bioshock1r/temporal_gpu.h"

#include <algorithm>

using namespace gt;
namespace tg = bvr::b1r::tgpu;

namespace {

const uint32_t W = 256, H = 240;
const double kNear = 10.0, kFar = 65536.0;

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
    s.solids.push_back({{100, -30, -100}, {160, 30, 200}, 1});    // a pillar
    s.solids.push_back({{-60, 40, 0}, {-20, 120, 80}, 2});        // a crate
    // A "hand": 45-53 uu in front of the first camera, 31-39 uu after the walk (beyond the
    // 10 uu near plane in both - a real render clips anything nearer).
    s.solids.push_back({{-105, 14, 52}, {-97, 30, 64}, 3});
    return s;
}

Cam base_cam() {
    Cam c;
    c.loc[0] = -150; c.loc[1] = 20; c.loc[2] = 60;
    c.rot = {0, 0, 0};
    c.tanH = std::tan(52.0 * 3.14159265 / 180.0);
    c.tanV = c.tanH * H / W;
    return c;
}

struct Errors { double mean = 0, p95 = 0, max = 0; size_t n = 0; };

// GPU vectors vs the true reprojection of each pixel's world point, in pixels.
Errors vector_error(const std::vector<float>& mv, const Render& cur, const Cam& prev, double minZ = 0, double maxZ = 1e30) {
    std::vector<double> e;
    for (uint32_t y = 0; y < H; ++y)
        for (uint32_t x = 0; x < W; ++x) {
            const size_t i = (size_t)y * W + x;
            const double z = cur.viewZ[i];
            if (z < minZ || z > maxZ) continue;
            double pu, pv;
            if (!project(prev, &cur.world[i * 3], &pu, &pv)) continue;
            const double tu = pu - (x + 0.5) / W, tv = pv - (y + 0.5) / H;
            const double dx = (mv[i * 2] - tu) * W, dy = (mv[i * 2 + 1] - tv) * H;
            e.push_back(std::sqrt(dx * dx + dy * dy));
        }
    Errors r;
    if (e.empty()) return r;
    r.n = e.size();
    double s = 0; for (double v : e) s += v;
    r.mean = s / e.size();
    std::sort(e.begin(), e.end());
    r.p95 = e[(size_t)(e.size() * 0.95)];
    r.max = e.back();
    return r;
}

struct Gpu {
    tg::TemporalGpu t;
    ID3D11Texture2D* raw = nullptr;
    ID3D11ShaderResourceView* rawSrv = nullptr;
    ~Gpu() { rel(rawSrv); rel(raw); t.shutdown(); }
    bool run(const Render& cur, const tg::Params& p) {
        rel(rawSrv); rel(raw);
        raw = make_tex(W, H, DXGI_FORMAT_R32_FLOAT, D3D11_BIND_SHADER_RESOURCE, cur.raw.data(), W * 4);
        rawSrv = srv_of(raw);
        char why[256] = "";
        if (!t.run(dev, ctx, rawSrv, nullptr, p, why, sizeof(why))) { printf("    run failed: %s\n", why); return false; }
        return true;
    }
};

tg::Params params(const Cam& cur, const Cam& prev, bool hist = true, double bodyUu = 0) {
    tg::Params p;
    p.w = W; p.h = H; p.nearUu = (float)kNear; p.farUu = (float)kFar;
    p.historyValid = hist; p.bodyUu = (float)bodyUu;
    p.cur = to_tcam(cur); p.prev = to_tcam(prev);
    return p;
}

// Renders a D24 depth of `raw` through SV_Depth into a real D24_UNORM_S8_UINT target, then
// copies it the way temporal.cpp copies the game's (CopyResource into an R24G8_TYPELESS
// capture with DEPTH_STENCIL|SHADER_RESOURCE) and returns the capture's R24 view.
ID3D11ShaderResourceView* d24_roundtrip(const std::vector<float>& raw, ID3D11Texture2D** keepA, ID3D11Texture2D** keepB) {
    const char* src = R"(
Texture2D<float> d : register(t0);
float4 vs(uint id : SV_VertexID) : SV_Position {
    float2 p = float2((id << 1) & 2, id & 2);
    return float4(p * float2(2, -2) + float2(-1, 1), 0, 1);
}
float ps(float4 pos : SV_Position) : SV_Depth { return d.Load(int3(pos.xy, 0)); }
)";
    ID3DBlob *vb = nullptr, *pb = nullptr, *err = nullptr;
    D3DCompile(src, strlen(src), "d24", nullptr, nullptr, "vs", "vs_5_0", 0, 0, &vb, &err); rel(err);
    D3DCompile(src, strlen(src), "d24", nullptr, nullptr, "ps", "ps_5_0", 0, 0, &pb, &err); rel(err);
    ID3D11VertexShader* vsh = nullptr; ID3D11PixelShader* psh = nullptr;
    dev->CreateVertexShader(vb->GetBufferPointer(), vb->GetBufferSize(), nullptr, &vsh);
    dev->CreatePixelShader(pb->GetBufferPointer(), pb->GetBufferSize(), nullptr, &psh);
    rel(vb); rel(pb);
    ID3D11Texture2D* f32 = make_tex(W, H, DXGI_FORMAT_R32_FLOAT, D3D11_BIND_SHADER_RESOURCE, raw.data(), W * 4);
    ID3D11ShaderResourceView* f32s = srv_of(f32);
    ID3D11Texture2D* a = make_tex(W, H, DXGI_FORMAT_R24G8_TYPELESS, D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE);
    ID3D11Texture2D* b = make_tex(W, H, DXGI_FORMAT_R24G8_TYPELESS, D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE);
    D3D11_DEPTH_STENCIL_VIEW_DESC dd = {};
    dd.Format = DXGI_FORMAT_D24_UNORM_S8_UINT; dd.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
    ID3D11DepthStencilView* dsv = nullptr;
    dev->CreateDepthStencilView(a, &dd, &dsv);
    D3D11_DEPTH_STENCIL_DESC dsd = {};
    dsd.DepthEnable = TRUE; dsd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL; dsd.DepthFunc = D3D11_COMPARISON_ALWAYS;
    ID3D11DepthStencilState* dss = nullptr;
    dev->CreateDepthStencilState(&dsd, &dss);
    D3D11_VIEWPORT vp = {0, 0, (float)W, (float)H, 0, 1};
    ctx->OMSetRenderTargets(0, nullptr, dsv);
    ctx->OMSetDepthStencilState(dss, 0);
    ctx->RSSetViewports(1, &vp);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->IASetInputLayout(nullptr);
    ctx->VSSetShader(vsh, nullptr, 0);
    ctx->PSSetShader(psh, nullptr, 0);
    ctx->PSSetShaderResources(0, 1, &f32s);
    ctx->Draw(3, 0);
    ID3D11ShaderResourceView* none = nullptr;
    ctx->PSSetShaderResources(0, 1, &none);
    ctx->OMSetRenderTargets(0, nullptr, nullptr);
    ctx->CopyResource(b, a);
    rel(dss); rel(dsv); rel(f32s); rel(f32); rel(vsh); rel(psh);
    *keepA = a; *keepB = b;
    return srv_of(b, DXGI_FORMAT_R24_UNORM_X8_TYPELESS);
}

} // namespace

int main() {
    printf("temporal-gpu: the BS1 depth and camera motion vectors\n");
    if (!make_device()) return 1;
    const Scene scene = make_scene();
    const Cam c0 = base_cam();

    // view_z: the shader's formula (CPU twin) against the truth, finite and infinite far.
    {
        tg::Params p; p.nearUu = (float)kNear; p.farUu = (float)kFar;
        double worst = 0;
        for (double z : {12.0, 40.0, 300.0, 2000.0, 20000.0}) {
            const double back = tg::view_z(p, device_depth(z, kNear, kFar));
            worst = std::max(worst, std::fabs(back - z) / z);
        }
        check(worst < 2e-3, "view_z finite far", "worst relative error %.2e", worst);
        p.farUu = 0;
        worst = 0;
        for (double z : {12.0, 40.0, 300.0, 2000.0}) {
            const double back = tg::view_z(p, device_depth(z, kNear, 0));
            worst = std::max(worst, std::fabs(back - z) / z);
        }
        check(worst < 2e-3, "view_z infinite far", "worst relative error %.2e", worst);
    }

    Gpu g;
    // 1. A still camera: zero vectors everywhere.
    {
        const Render r = render(scene, c0, W, H, kNear, kFar, 1);
        if (!g.run(r, params(c0, c0))) return 1;
        const auto mv = readback(g.t.motion());
        double worst = 0;
        for (float v : mv) worst = std::max(worst, (double)std::fabs(v));
        check(worst * W < 0.01, "still camera", "max |vector| %.4f px", worst * W);
        // linearZ matches the truth.
        const auto lz = readback(g.t.linear_z());
        double lw = 0;
        for (size_t i = 0; i < lz.size(); ++i) lw = std::max(lw, (double)std::fabs(lz[i] - r.viewZ[i]) / r.viewZ[i]);
        check(lw < 2e-3, "linear depth", "worst relative error %.2e", lw);
        // dlssDepth = 1 - raw (reversed).
        const auto dd = readback(g.t.dlss_depth());
        double dw = 0;
        for (size_t i = 0; i < dd.size(); ++i) dw = std::max(dw, std::fabs(dd[i] - (1.0 - r.raw[i])));
        check(dw < 1e-6, "DLSS depth is reversed", "worst |dlss - (1 - raw)| %.2e", dw);
    }

    // 2. Pure head turn (yaw 4 deg, pitch -2 deg): exact for every pixel.
    Cam turned = c0;
    turned.rot.yaw = (int32_t)(4.0 * bvr::ue::kRotUnitsPerDegree);
    turned.rot.pitch = (int32_t)(-2.0 * bvr::ue::kRotUnitsPerDegree);
    {
        const Render r = render(scene, turned, W, H, kNear, kFar, 1);
        g.run(r, params(turned, c0));
        const Errors e = vector_error(readback(g.t.motion()), r, c0);
        check(e.n > 1000 && e.p95 < 0.05 && e.max < 0.2, "head turn", "mean %.4f p95 %.4f max %.4f px over %zu", e.mean, e.p95, e.max, e.n);
    }

    // 3. Walking + turn + roll: translation parallax must be right at every depth.
    Cam walked = c0;
    walked.loc[0] += 14; walked.loc[1] -= 9; walked.loc[2] += 2;
    walked.rot.yaw = (int32_t)(-3.0 * bvr::ue::kRotUnitsPerDegree);
    walked.rot.roll = (int32_t)(5.0 * bvr::ue::kRotUnitsPerDegree);
    const Render rw = render(scene, walked, W, H, kNear, kFar, 1);
    {
        g.run(rw, params(walked, c0));
        const auto mv = readback(g.t.motion());
        const Errors en = vector_error(mv, rw, c0, 0, 60), ef = vector_error(mv, rw, c0, 60, 1e30);
        check(ef.n > 1000 && ef.p95 < 0.05 && ef.max < 0.3, "walk + turn + roll (world)",
              "mean %.4f p95 %.4f max %.4f px over %zu", ef.mean, ef.p95, ef.max, ef.n);
        check(en.n > 20 && en.p95 < 0.1, "walk + turn + roll (near object)",
              "mean %.4f p95 %.4f max %.4f px over %zu", en.mean, en.p95, en.max, en.n);
    }
    // Negative control: rotation-only vectors (the body band over everything) must fail.
    {
        g.run(rw, params(walked, c0, true, 1e9));
        const Errors e = vector_error(readback(g.t.motion()), rw, c0, 0, 400);
        check(e.mean > 1.0, "control: rotation-only under translation is wrong", "mean %.3f px (must be > 1)", e.mean);
    }
    // Negative control: a wrong near plane (20 for 10) must fail under translation.
    {
        tg::Params p = params(walked, c0);
        p.nearUu = 20.0f;
        g.run(rw, p);
        const Errors e = vector_error(readback(g.t.motion()), rw, c0, 0, 400);
        check(e.mean > 0.5, "control: a wrong near plane is wrong", "mean %.3f px (must be > 0.5)", e.mean);
    }
    // 4. The body band: nearer than bodyUu is rotation-only, farther unchanged.
    {
        g.run(rw, params(walked, c0, true, 40));
        const auto mv = readback(g.t.motion());
        tg::Params rot = params(walked, c0);
        double worstNear = 0, worstFar = 0;
        size_t nNear = 0;
        for (uint32_t y = 0; y < H; ++y)
            for (uint32_t x = 0; x < W; ++x) {
                const size_t i = (size_t)y * W + x;
                const double u = (x + 0.5) / W, v = (y + 0.5) / H;
                if (rw.viewZ[i] < 40) {
                    // Expected: the direction reprojected (a point at a huge distance).
                    float pu, pv;
                    tg::Params still = rot;
                    for (int k = 0; k < 3; ++k) still.cur.loc[k] = still.prev.loc[k];
                    tg::reproject(still, (float)u, (float)v, 1000.0f, &pu, &pv);
                    worstNear = std::max(worstNear, std::hypot((mv[i * 2] - (pu - u)) * W, (mv[i * 2 + 1] - (pv - v)) * H));
                    ++nNear;
                } else {
                    double pu, pv;
                    project(c0, &rw.world[i * 3], &pu, &pv);
                    worstFar = std::max(worstFar, std::hypot((mv[i * 2] - (pu - u)) * W, (mv[i * 2 + 1] - (pv - v)) * H));
                }
            }
        check(nNear > 20 && worstNear < 0.05, "body band is rotation-only", "%zu px, worst %.4f px", nNear, worstNear);
        check(worstFar < 0.5, "body band leaves the world alone", "worst %.4f px", worstFar);
    }
    // 5. No history: zero vectors.
    {
        g.run(rw, params(walked, c0, false));
        const auto mv = readback(g.t.motion());
        double worst = 0;
        for (float v : mv) worst = std::max(worst, (double)std::fabs(v));
        check(worst == 0.0, "no history -> zero", "max %.6f", worst);
    }
    // 6. The game's real format: D24 through SV_Depth and a copy, same vectors.
    {
        ID3D11Texture2D *a = nullptr, *b = nullptr;
        ID3D11ShaderResourceView* s = d24_roundtrip(rw.raw, &a, &b);
        char why[256] = "";
        tg::TemporalGpu t2;
        const bool ok = s && t2.run(dev, ctx, s, nullptr, params(walked, c0), why, sizeof(why));
        const Errors e = ok ? vector_error(readback(t2.motion()), rw, c0) : Errors{};
        check(ok && e.n > 1000 && e.p95 < 0.06, "D24 round trip", "%s mean %.4f p95 %.4f max %.4f px", ok ? "" : why, e.mean, e.p95, e.max);
        t2.shutdown(); rel(s); rel(a); rel(b);
    }
    // 6b. The foreground pass: where it left depth, that depth and the hands mask; elsewhere
    //     the world untouched. Control: without the foreground the mask is empty.
    {
        std::vector<float> fg((size_t)W * H, 1.0f);
        size_t nHand = 0;
        for (uint32_t y = H / 2; y < H / 2 + 40; ++y)
            for (uint32_t x = W / 2; x < W / 2 + 50; ++x) { fg[(size_t)y * W + x] = device_depth(30.0, kNear, kFar); ++nHand; }
        ID3D11Texture2D* ft = make_tex(W, H, DXGI_FORMAT_R32_FLOAT, D3D11_BIND_SHADER_RESOURCE, fg.data(), W * 4);
        ID3D11Texture2D* wt = make_tex(W, H, DXGI_FORMAT_R32_FLOAT, D3D11_BIND_SHADER_RESOURCE, rw.raw.data(), W * 4);
        ID3D11ShaderResourceView *fs = srv_of(ft), *ws = srv_of(wt);
        tg::TemporalGpu t3;
        char why[256] = "";
        tg::Params p = params(walked, c0);
        p.haveForeground = true;
        const bool ok = t3.run(dev, ctx, ws, fs, p, why, sizeof(why));
        const auto hm = ok ? readback(t3.hands()) : std::vector<float>();
        const auto lz = ok ? readback(t3.linear_z()) : std::vector<float>();
        size_t wrong = 0; double worstHand = 0, worstWorld = 0;
        for (size_t i = 0; ok && i < fg.size(); ++i) {
            const bool hand = fg[i] < 1.0f;
            if ((hm[i] > 0.5f) != hand) ++wrong;
            if (hand) worstHand = std::max(worstHand, std::fabs(lz[i] - 30.0) / 30.0);
            else worstWorld = std::max(worstWorld, (double)std::fabs(lz[i] - rw.viewZ[i]) / rw.viewZ[i]);
        }
        check(ok && wrong == 0 && worstHand < 1e-3 && worstWorld < 2e-3, "foreground: hands mask and depth, world untouched",
              "%s %zu hand px, %zu wrong, worst hand %.1e, worst world %.1e", why, nHand, wrong, worstHand, worstWorld);
        p.haveForeground = false;
        t3.run(dev, ctx, ws, nullptr, p, why, sizeof(why));
        const auto hm0 = readback(t3.hands());
        double any = 0; for (float v : hm0) any = std::max(any, (double)v);
        check(any == 0.0, "control: no foreground, no hands", "max mask %.2f", any);
        t3.shutdown(); rel(fs); rel(ws); rel(ft); rel(wt);
    }
    // 6c. TAA over a walk on a detailed scene: closer to supersampled truth than the raw
    //     render, and the vectors matter (control: zero vectors ghost).
    {
        Scene ds = scene;
        ds.detail = 0.25;
        auto cam_at = [&](int f) {
            Cam c = c0;
            c.loc[0] += 1.2 * f; c.loc[1] -= 0.4 * f;
            c.rot.yaw = (int32_t)(0.35 * f * bvr::ue::kRotUnitsPerDegree);
            return c;
        };
        ID3D11Texture2D* zeroMv = make_tex(W, H, DXGI_FORMAT_R16G16_FLOAT, D3D11_BIND_SHADER_RESOURCE);
        ID3D11ShaderResourceView* zeroSrv = srv_of(zeroMv);
        double err[2] = {}, raw = 0;
        int n = 0;
        for (int variant = 0; variant < 2; ++variant) {
            tg::TemporalGpu tgv;
            tg::TaaGpu taa;
            char why[256] = "";
            for (int f = 0; f < 24; ++f) {
                const Cam c = cam_at(f), pc = cam_at(f > 0 ? f - 1 : 0);
                const Render r = render(ds, c, W, H, kNear, kFar, 1);
                ID3D11Texture2D* ct = make_tex(W, H, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_SHADER_RESOURCE, r.rgba.data(), W * 4);
                ID3D11Texture2D* dt = make_tex(W, H, DXGI_FORMAT_R32_FLOAT, D3D11_BIND_SHADER_RESOURCE, r.raw.data(), W * 4);
                ID3D11ShaderResourceView *cs = srv_of(ct), *dsv = srv_of(dt);
                tgv.run(dev, ctx, dsv, nullptr, params(c, pc, f > 0), why, sizeof(why));
                taa.run(dev, ctx, 0, cs, variant == 0 ? tgv.motion_srv() : zeroSrv, nullptr, W, H, f > 0, 0.12f, why, sizeof(why));
                if (f >= 16) {
                    const Render truth = render(ds, c, W, H, kNear, kFar, 4);
                    const auto got = readback(taa.out(0));
                    double e = 0, er = 0; size_t m = 0;
                    for (uint32_t y = H / 16; y < H - H / 16; ++y)
                        for (uint32_t x = W / 16; x < W - W / 16; ++x) {
                            const size_t i = (size_t)y * W + x;
                            for (int ch = 0; ch < 3; ++ch) {
                                const double t = ((truth.rgba[i] >> (8 * ch)) & 255) / 255.0;
                                e += std::fabs(got[i * 4 + ch] - t);
                                er += std::fabs(((r.rgba[i] >> (8 * ch)) & 255) / 255.0 - t);
                                ++m;
                            }
                        }
                    err[variant] += e / m;
                    if (variant == 0) { raw += er / m; ++n; }
                }
                rel(cs); rel(dsv); rel(ct); rel(dt);
            }
            taa.shutdown(); tgv.shutdown();
        }
        check(n && err[0] / n < raw / n, "TAA is closer to supersampled truth than the raw render",
              "TAA %.5f vs raw %.5f", err[0] / n, raw / n);
        check(n && err[0] < err[1], "control: TAA with zero vectors is worse (the vectors matter)",
              "vectors %.5f vs zero %.5f", err[0] / n, err[1] / n);
        rel(zeroSrv); rel(zeroMv);
    }
    // 7. The audit on real images: vectors beat no-motion and rotation-only, and the depth
    //    scale curve bottoms out at 1.
    {
        const Render cur = render(scene, walked, W, H, kNear, kFar, 2);
        const Render prev = render(scene, c0, W, H, kNear, kFar, 2);
        ID3D11Texture2D* ct = make_tex(W, H, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_SHADER_RESOURCE, cur.rgba.data(), W * 4);
        ID3D11Texture2D* pt = make_tex(W, H, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_SHADER_RESOURCE, prev.rgba.data(), W * 4);
        ID3D11ShaderResourceView *cs = srv_of(ct), *ps = srv_of(pt);
        tg::Params p = params(walked, c0);
        g.run(cur, p);
        g.t.audit.clear();
        for (int i = 0; i < 12; ++i) {
            g.t.audit_frame(dev, ctx, cs, ps, p);
            ctx->Flush();
            Sleep(5);
        }
        const auto& a = g.t.audit;
        const double n = a.frames ? (double)a.samples : 1;
        int best = 0;
        for (int s = 1; s < tg::kAuditScales; ++s) if (a.scaleErr[s] < a.scaleErr[best]) best = s;
        check(a.frames >= 3, "audit reads back", "%llu frames, %llu samples", (unsigned long long)a.frames, (unsigned long long)a.samples);
        check(a.vecErr < 0.35 * a.zeroErr && a.vecErr < 0.6 * a.rotErr, "audit: vectors explain the motion",
              "vec %.4f rot %.4f zero %.4f (mean per sample)", a.vecErr / n, a.rotErr / n, a.zeroErr / n);
        check(tg::kAuditScaleValues[best] == 1.0f, "audit: depth scale 1 is best",
              "errors 0.25:%.4f 0.5:%.4f 1:%.4f 2:%.4f 4:%.4f", a.scaleErr[0] / n, a.scaleErr[1] / n, a.scaleErr[2] / n,
              a.scaleErr[3] / n, a.scaleErr[4] / n);
        rel(cs); rel(ps); rel(ct); rel(pt);
    }
    // 8. The hand-depth probe on a native stereo pair (both eyes at one instant). With true
    //    depth both bands read 1.00; with the hand's depth 12% too far (Dishonored's measured
    //    foreground shape) the hand band reads 1.12 and the world still 1.00. Larger images
    //    so the world's disparity clears the probe's 6 px floor.
    {
        const uint32_t PW = 768, PH = 720;
        Scene ps = make_scene();
        ps.detail = 0.25;
        Cam head = base_cam();
        head.tanV = head.tanH * PH / PW;
        Cam l = head, r = head;
        double f[3], rt[3], u[3];
        head.basis(f, rt, u);
        for (int i = 0; i < 3; ++i) { l.loc[i] -= rt[i] * 3.2; r.loc[i] += rt[i] * 3.2; }
        const Render rl = render(ps, l, PW, PH, kNear, kFar, 2), rr = render(ps, r, PW, PH, kNear, kFar, 2);
        ID3D11Texture2D* lt = make_tex(PW, PH, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_SHADER_RESOURCE, rl.rgba.data(), PW * 4);
        ID3D11Texture2D* rtx = make_tex(PW, PH, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_SHADER_RESOURCE, rr.rgba.data(), PW * 4);
        ID3D11ShaderResourceView *ls = srv_of(lt), *rs = srv_of(rtx);
        auto probe = [&](float handScale, double* hand, double* world, uint64_t* nh, uint64_t* nw) {
            std::vector<float> z = rl.viewZ;
            for (float& v : z) if (v > 0 && v < 80) v *= handScale;
            ID3D11Texture2D* zt = make_tex(PW, PH, DXGI_FORMAT_R32_FLOAT, D3D11_BIND_SHADER_RESOURCE, z.data(), PW * 4);
            ID3D11ShaderResourceView* zs = srv_of(zt);
            tg::DepthProbe pr;
            char why[256] = "";
            for (int i = 0; i < 8; ++i) {
                if (!pr.run(dev, ctx, ls, zs, rs, to_tcam(l), to_tcam(r), PW, PH, 80.0f, why, sizeof(why))) {
                    printf("    probe failed: %s\n", why);
                    break;
                }
                ctx->Flush();
                Sleep(5);
            }
            *hand = pr.result.median(0, nh);
            *world = pr.result.median(1, nw);
            pr.shutdown();
            rel(zs); rel(zt);
        };
        double h1, w1, h2, w2;
        uint64_t nh1, nw1, nh2, nw2;
        probe(1.0f, &h1, &w1, &nh1, &nw1);
        probe(1.12f, &h2, &w2, &nh2, &nw2);
        check(nh1 > 20 && nw1 > 20 && std::fabs(h1 - 1.0) <= 0.02 && std::fabs(w1 - 1.0) <= 0.02,
              "hand-depth probe: true depth reads 1.00 in both bands", "hands %.2f (%llu), world %.2f (%llu)", h1,
              (unsigned long long)nh1, w1, (unsigned long long)nw1);
        check(nh2 > 20 && std::fabs(h2 - 1.12) <= 0.02 && std::fabs(w2 - 1.0) <= 0.02,
              "hand-depth probe: hands 12% too deep read 1.12, the world stays 1.00", "hands %.2f (%llu), world %.2f (%llu)",
              h2, (unsigned long long)nh2, w2, (unsigned long long)nw2);
        rel(ls); rel(rs); rel(lt); rel(rtx);
    }
    return finish("temporal-gpu");
}
