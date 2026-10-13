// gpu-test-sources: src/game/bioshock1r/afw_gpu.cpp
// The alternate frame warp's held-eye rebuild (game/bioshock1r/afw_gpu) against a
// ray-traced truth. One AlternateEye step: the LEFT eye's image is a frame old, the RIGHT
// eye was just rendered; between them the player walked, turned and the hand moved.
// The rebuilt left eye must match a true render of the left eye NOW far better than what
// the compositor would show (the old image, rotated), and every part of the method must
// earn its place against a control that switches it off.
#include "gpu_test_util.h"
#include "game/bioshock1r/afw_gpu.h"
#include "game/bioshock1r/afw_motion.h"

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
    // The viewmodel: a hand that moves WITH the camera (the game's hands follow the head and
    // the controllers). One frame of walking with the sticks at the headset's rate: 1.5 uu
    // forward and 0.4 deg of turn (the integration test's stride). Carried as world geometry
    // by A, the old hand lands where a world object would - a frame behind and 3% nearer -
    // and that is UNDER the stale test's and the compose's tolerances (3% + 2 uu, tuned for
    // the world), so the old code keeps it: the doubled, trailing hands of headset run 4.
    // (The case above moves the camera 4 uu, which is over the tolerance, and the stale test
    // alone cleans it - that is why the ghost never showed offline.) With the hands mask the
    // rebuild drops A's hand and takes B's alone; the control is the mask switched off.
    {
        const uint32_t W = 640, H = 600;
        Scene vb = make_scene(), vn = make_scene();
        // Textured: on the smooth pattern a copy of the hand a few px to the side differs only
        // at its silhouette; the game's hands are textured, and the trailing copy shows.
        vb.detail = vn.detail = 0.25;
        const Cam h0 = head(-150, 20, 60, 0.0, -4.0, W, H);
        const Cam h1 = head(-148.5, 20.3, 60, 0.4, -4.0, W, H);
        {
            // The hand box fixed in the head's frame: where h0's frame put it, h1's puts it now.
            double f0[3], r0[3], u0[3], f1[3], r1[3], u1[3];
            h0.basis(f0, r0, u0); h1.basis(f1, r1, u1);
            const Box& hb = vb.solids[3];
            double c[3], rel[3];
            for (int i = 0; i < 3; ++i) { c[i] = (hb.lo[i] + hb.hi[i]) * 0.5; rel[i] = c[i] - h0.loc[i]; }
            const double fa = rel[0] * f0[0] + rel[1] * f0[1] + rel[2] * f0[2];
            const double ra = rel[0] * r0[0] + rel[1] * r0[1] + rel[2] * r0[2];
            const double ua = rel[0] * u0[0] + rel[1] * u0[1] + rel[2] * u0[2];
            for (int i = 0; i < 3; ++i) vn.objShift[i] = h1.loc[i] + f1[i] * fa + r1[i] * ra + u1[i] * ua - c[i];
        }
        const Cam a = eye_of(h0, -1), b = eye_of(h1, +1), t = eye_of(h1, -1);
        const Render vac = render(vb, a, W, H, kNear, kFar, 2), vbc = render(vn, b, W, H, kNear, kFar, 2);
        const Render vaz = render(vb, a, W, H, kNear, kFar, 1), vbz = render(vn, b, W, H, kNear, kFar, 1);
        const Render vtruth = render(vn, t, W, H, kNear, kFar, 2);
        // Each source's hands mask (what temporal_gpu marks from the squeezed depth range), and
        // the hand region of the target: the hand now, or the old hand carried along.
        std::vector<uint8_t> ma((size_t)W * H, 0), mb((size_t)W * H, 0), region((size_t)W * H, 0);
        size_t na = 0, nb = 0, nr = 0;
        for (uint32_t y = 0; y < H; ++y)
            for (uint32_t x = 0; x < W; ++x) {
                double d[3];
                pixel_dir(a, W, H, x + 0.5, y + 0.5, d);
                if (vb.cast(a.loc, d).id == 3) { ma[(size_t)y * W + x] = 255; ++na; }
                pixel_dir(b, W, H, x + 0.5, y + 0.5, d);
                if (vn.cast(b.loc, d).id == 3) { mb[(size_t)y * W + x] = 255; ++nb; }
                pixel_dir(t, W, H, x + 0.5, y + 0.5, d);
                if (vn.cast(t.loc, d).id == 3 || vb.cast(t.loc, d).id == 3) { region[(size_t)y * W + x] = 1; ++nr; }
            }
        Gpu gva, gvb;
        af::Source VA = source_of(gva, vac, W, H, vaz, W, H, a), VB = source_of(gvb, vbc, W, H, vbz, W, H, b);
        ID3D11Texture2D* mat = make_tex(W, H, DXGI_FORMAT_R8_UNORM, D3D11_BIND_SHADER_RESOURCE, ma.data(), W);
        ID3D11Texture2D* mbt = make_tex(W, H, DXGI_FORMAT_R8_UNORM, D3D11_BIND_SHADER_RESOURCE, mb.data(), W);
        ID3D11ShaderResourceView *mas = srv_of(mat), *mbs = srv_of(mbt);
        VA.mask = mas; VB.mask = mbs;
        af::Params p;
        p.w = W; p.h = H;
        p.target = to_tcam(t);
        auto runV = [&](bool mask) -> Score {
            af::Params q = p; q.handsFromFresh = mask;
            if (!warp.run(dev, ctx, VA, VB, q, why, sizeof(why))) { printf("    run failed: %s\n", why); return Score{1, 1, 1}; }
            return score(readback(warp.out()), vtruth, region, W, H);
        };
        const Score withMask = runV(true), control = runV(false);
        printf(" -- viewmodel, one frame of walking (mean / bad px / hand region): hands from the fresh eye %.4f / %.2f%% / %.4f | "
               "control (old code) %.4f / %.2f%% / %.4f | masks %zu + %zu px, region %zu px\n",
               withMask.mean, withMask.bad * 100, withMask.handRegion, control.mean, control.bad * 100, control.handRegion,
               na, nb, nr);
        check(na > 100 && nb > 100 && withMask.handRegion < 0.6 * control.handRegion,
              "a viewmodel that moves with the camera: the hands from the fresh eye alone beat carrying the old ones",
              "hand region %.4f with the mask vs %.4f without", withMask.handRegion, control.handRegion);
        check(withMask.mean <= control.mean * 1.02 && withMask.bad <= control.bad * 1.02,
              "the hands mask leaves the world no worse", "mean %.4f vs %.4f, bad %.2f%% vs %.2f%%", withMask.mean,
              control.mean, withMask.bad * 100, control.bad * 100);
        // Masks absent = the lever off: the old code exactly.
        VA.mask = VB.mask = nullptr;
        const Score noMask = runV(true);
        check(std::fabs(noMask.mean - control.mean) < 1e-6 && std::fabs(noMask.handRegion - control.handRegion) < 1e-6,
              "without masks the rebuild is the old code exactly", "mean %.6f vs %.6f", noMask.mean, control.mean);
        rel(mas); rel(mbs); rel(mat); rel(mbt);
    }

    // A one-depth-texel red rail before sky, plus a diagonal wall silhouette.
    // Analytic ray/plane intersections supply both input depth and supersampled truth.
    // The held eye sees the rail on an even mesh vertex; the fresh eye sees it between
    // vertices, so the legacy compose prefers its valid sky over the held sheet.
    {
        const uint32_t W=320,H=300;
        Cam a=head(0,-2.5,0,0,0,W,H),b=head(0,6.4,0,0,0,W,H),t=head(0,0,0,0,0,W,H);
        a.tanH=b.tanH=t.tanH=1; a.tanV=b.tanV=t.tanV=(double)H/W;
        const double barX=200,barY=50.625,barWidth=2*barX*t.tanH/W;
        auto trace=[&](const Cam& c,double px,double py,float* z)->uint32_t {
            double d[3];pixel_dir(c,W,H,px,py,d);
            const double tb=(barX-c.loc[0])/d[0];
            const double by=c.loc[1]+tb*d[1],bz=c.loc[2]+tb*d[2];
            if(std::fabs(by-barY)<barWidth*.5 && std::fabs(bz)<140) {
                *z=(float)tb;return 0xFF0F23FAu; // red rail
            }
            const double tw=(350-c.loc[0])/d[0];
            const double wy=c.loc[1]+tw*d[1],wz=c.loc[2]+tw*d[2];
            if(wy<.47*wz-45) {*z=(float)tw;return 0xFF506050u;}
            *z=0;return 0xFFD25A20u; // sky
        };
        auto draw=[&](const Cam& c)->Render {
            Render r;r.rgba.resize((size_t)W*H);r.viewZ.resize((size_t)W*H);
            for(uint32_t y=0;y<H;++y) for(uint32_t x=0;x<W;++x) {
                unsigned sum[3]={};float z=0;
                for(int j=0;j<4;++j) for(int i=0;i<4;++i) {
                    const auto color=trace(c,x+(i+.5)/4,y+(j+.5)/4,&z);
                    for(int k=0;k<3;++k) sum[k]+=(color>>(8*k))&255;
                }
                uint32_t px=0xFF000000u;
                for(int k=0;k<3;++k) px|=((sum[k]+8)/16)<<(8*k);
                const size_t index=(size_t)y*W+x;
                r.rgba[index]=px;trace(c,x+.5,y+.5,&r.viewZ[index]);
            }
            return r;
        };
        const Render ra=draw(a),rb=draw(b),truth=draw(t);
        Gpu ga,gb;af::Source A=source_of(ga,ra,W,H,ra,W,H,a),B=source_of(gb,rb,W,H,rb,W,H,b);
        af::Params p;p.w=W;p.h=H;p.target=to_tcam(t);
        p.refine=false;
        check(warp.run(dev,ctx,A,B,p,why,sizeof(why)),"thin silhouettes: mesh control renders","%s",why);
        const auto mesh=readback(warp.out());
        p.refine=true;
        check(warp.run(dev,ctx,A,B,p,why,sizeof(why)),"thin silhouettes: refined path renders","%s",why);
        const auto refined=readback(warp.out());
        auto metrics=[&](const std::vector<float>& got,double* edge,int* bars)->double {
            double error=0;*edge=0;*bars=0;
            for(uint32_t y=8;y<H-8;++y) for(uint32_t x=8;x<W-8;++x) {
                const size_t i=(size_t)y*W+x;
                for(int k=0;k<3;++k) {
                    const double e=std::fabs(got[i*4+k]-((truth.rgba[i]>>(8*k))&255)/255.);
                    error+=e;
                    if(x<W/2+10) *edge+=e;
                }
                if(x>W/2+10 && got[i*4]>.6f && got[i*4+1]<.4f) ++*bars;
            }
            return error;
        };
        double oldEdge=0,newEdge=0;int oldBar=0,newBar=0,truthBar=0;
        const double oldError=metrics(mesh,&oldEdge,&oldBar),newError=metrics(refined,&newEdge,&newBar);
        for(uint32_t y=8;y<H-8;++y) for(uint32_t x=W/2+11;x<W-8;++x) {
            const uint32_t px=truth.rgba[(size_t)y*W+x];
            if((px&255)>153 && ((px>>8)&255)<102) ++truthBar;
        }
        check(newError<oldError*.5,"refinement more than halves silhouette error against ray-traced truth",
              "refine %.3f mesh %.3f (ratio %.3f)",newError,oldError,newError/oldError);
        check(newEdge<oldEdge*.5,"the diagonal edge error is below half the mesh control",
              "refine %.3f mesh %.3f",newEdge,oldEdge);
        check(truthBar>100 && std::abs(newBar-truthBar)<=truthBar*.2 && oldBar<truthBar*.8,
              "one-texel rail survives within 20 percent where the mesh loses it",
              "truth %d refined %d mesh %d",truthBar,newBar,oldBar);
        p.refine=false;warp.run(dev,ctx,A,B,p,why,sizeof(why));
        check(readback(warp.out())==mesh,"refine off restores the exact mesh image after a mode change","");
    }

    // Curved foreground silhouettes against a bright wall. Input depth is
    // 320x300 and colour is 480x450: at a cylinder edge the fresh-eye inverse
    // solve can miss even though its depth confirms A's own foreground pixel.
    // Dropping A merely because the point is visible in B paints white holes.
    {
        const uint32_t W=480,H=450,DW=320,DH=300;
        struct Foreground { Render image; std::vector<uint8_t> mask; };
        auto draw=[&](const Cam& c,uint32_t w,uint32_t h,double cy,double radius,int ss) {
            Foreground a;
            a.image.rgba.resize((size_t)w*h);a.image.viewZ.resize((size_t)w*h);
            a.mask.resize((size_t)w*h);
            auto z_at=[&](double px,double py)->float {
                double d[3];pixel_dir(c,w,h,px,py,d);
                const double ox=c.loc[0]-45,oy=c.loc[1]-cy;
                const double qa=d[0]*d[0]+d[1]*d[1],qb=2*(d[0]*ox+d[1]*oy);
                const double qc=ox*ox+oy*oy-radius*radius;
                const double disc=qb*qb-4*qa*qc;
                if(disc<=0) return 1000;
                const double z=(-qb-std::sqrt(disc))/(2*qa);
                return z>10 && std::fabs(c.loc[2]+z*d[2])<15 ? (float)z:1000.0f;
            };
            for(uint32_t y=0;y<h;++y) for(uint32_t x=0;x<w;++x) {
                double sum=0;
                for(int j=0;j<ss;++j) for(int i=0;i<ss;++i)
                    sum+=z_at(x+(i+.5)/ss,y+(j+.5)/ss)<100?32:240;
                const unsigned v=(unsigned)(sum/(ss*ss)+.5);
                const size_t n=(size_t)y*w+x;
                a.image.rgba[n]=0xFF000000u|v|(v<<8)|(v<<16);
                a.image.viewZ[n]=z_at(x+.5,y+.5);
                a.mask[n]=a.image.viewZ[n]<100?255:0;
            }
            return a;
        };
        Cam a,b;
        a.loc[1]=-kHalfIpd;b.loc[1]=kHalfIpd;
        a.tanH=b.tanH=1.19;a.tanV=b.tanV=1.19*H/W;
        size_t leaks=0,area=0;
        for(double radius:{2.0,4.0,8.0}) for(double cy:{-10.0,-.6,0.0,.6,10.0}) {
            const auto ca=draw(a,W,H,cy,radius,4),cb=draw(b,W,H,cy,radius,4);
            const auto za=draw(a,DW,DH,cy,radius,1),zb=draw(b,DW,DH,cy,radius,1);
            Gpu ga,gb;
            af::Source A=source_of(ga,ca.image,W,H,za.image,DW,DH,a);
            af::Source B=source_of(gb,cb.image,W,H,zb.image,DW,DH,b);
            auto* mat=make_tex(DW,DH,DXGI_FORMAT_R8_UNORM,D3D11_BIND_SHADER_RESOURCE,za.mask.data(),DW);
            auto* mbt=make_tex(DW,DH,DXGI_FORMAT_R8_UNORM,D3D11_BIND_SHADER_RESOURCE,zb.mask.data(),DW);
            auto* mas=srv_of(mat);auto* mbs=srv_of(mbt);
            A.mask=mas;B.mask=mbs;
            af::Params p;p.w=W;p.h=H;p.target=to_tcam(a);
            if(!warp.run(dev,ctx,A,B,p,why,sizeof(why))) return 1;
            const auto got=readback(warp.out());
            size_t caseLeaks=0;
            for(size_t i=0;i<(size_t)W*H;++i) if((ca.image.rgba[i]&255)<40) {
                ++area;if(got[i*4]>.35f) { ++leaks; ++caseLeaks; }
            }
            if((radius==2 && cy==-.6) || (radius==8 && cy==-10)) {
                // A projects just beyond B's point-sampled mask while the
                // gathered neighbour's depth still confirms the same surface.
                // A mismatched point-mask/gather-depth test cuts a bright line
                // down the whole object: 136 / 146 interior pixels before fix.
                check(caseLeaks<8,"gathered foreground depth uses the matching mask footprint",
                      "radius %.1f y %.1f: %zu bright interior pixels",radius,cy,caseLeaks);
            }
            if(radius==4 && cy==0) {
                // The cylinder moves completely away. Depth confirmation must
                // fail at its old position: preserving every old hand is wrong.
                const auto movedC=draw(b,W,H,15,radius,4),movedZ=draw(b,DW,DH,15,radius,1);
                const auto truth=draw(a,W,H,15,radius,4);
                Gpu gm;af::Source M=source_of(gm,movedC.image,W,H,movedZ.image,DW,DH,b);
                auto* mt=make_tex(DW,DH,DXGI_FORMAT_R8_UNORM,D3D11_BIND_SHADER_RESOURCE,movedZ.mask.data(),DW);
                auto* ms=srv_of(mt);M.mask=ms;
                auto ghosts=[&](bool haveFresh) {
                    af::Params q=p;q.useFresh=haveFresh;
                    if(!warp.run(dev,ctx,A,M,q,why,sizeof(why))) return (size_t)W*H;
                    const auto out=readback(warp.out());size_t n=0;
                    for(size_t i=0;i<(size_t)W*H;++i)
                        if((ca.image.rgba[i]&255)<40 && (truth.image.rgba[i]&255)>230 && out[i*4]<.5f) ++n;
                    return n;
                };
                const size_t freshGhosts=ghosts(true),heldGhosts=ghosts(false);
                check(heldGhosts>4000 && freshGhosts<heldGhosts*.02,
                      "confirmed-foreground reuse still discards an object that moved away",
                      "ghosts %zu; held-only control %zu",freshGhosts,heldGhosts);
                // The player walks/turns with the cylinder. Its relative eye
                // images stay the same, but the world-space cameras change.
                // The far side is visible only to A, so B alone cannot fill it.
                af::TrackingFrame from,to;from.valid=to.valid=true;
                for(float yaw:{0.0f,.18f}) {
                    to.origin[0]=3;to.origin[1]=2;to.yaw=yaw;
                    af::Source walkingB=B,walkingA=A;
                    af::Params q=p;
                    af::transport_foreground_camera(A.cam,from,to,q.target);
                    af::transport_foreground_camera(B.cam,from,to,walkingB.cam);
                    walkingA.foregroundCam=q.target;
                    auto holes=[&](bool transport) {
                        walkingA.foregroundCameraValid=transport;
                        if(!warp.run(dev,ctx,walkingA,walkingB,q,why,sizeof(why))) return (size_t)W*H;
                        const auto out=readback(warp.out());size_t n=0;
                        for(size_t i=0;i<(size_t)W*H;++i)
                            if((ca.image.rgba[i]&255)<40 && out[i*4]>.35f) ++n;
                        return n;
                    };
                    const size_t oldHoles=holes(false),newHoles=holes(true);
                    check(oldHoles>50 && newHoles<oldHoles*.25,
                          "locomotion carries hidden foreground without losing its pixels",
                          "yaw %.2f: %zu bright holes, world-fixed control %zu",yaw,newHoles,oldHoles);
                    // Moving the controller independently must still retire A.
                    af::Source walkingMoved=M;walkingMoved.cam=walkingB.cam;
                    if(!warp.run(dev,ctx,walkingA,walkingMoved,q,why,sizeof(why))) return 1;
                    const auto out=readback(warp.out());size_t n=0;
                    for(size_t i=0;i<(size_t)W*H;++i)
                        if((ca.image.rgba[i]&255)<40 && (truth.image.rgba[i]&255)>230 && out[i*4]<.5f) ++n;
                    check(n<heldGhosts*.02,"locomotion correction still rejects independently moved foreground",
                          "yaw %.2f: %zu ghost pixels",yaw,n);
                    walkingA.mask=nullptr;walkingB.mask=nullptr;
                    walkingA.foregroundCameraValid=false;
                    if(!warp.run(dev,ctx,walkingA,walkingB,q,why,sizeof(why))) return 1;
                    const auto worldControl=readback(warp.out());
                    walkingA.foregroundCameraValid=true;
                    if(!warp.run(dev,ctx,walkingA,walkingB,q,why,sizeof(why))) return 1;
                    check(worldControl==readback(warp.out()),"foreground transport leaves world reprojection unchanged","yaw %.2f",yaw);
                }
                rel(ms);rel(mt);
            }
            rel(mas);rel(mbs);rel(mat);rel(mbt);
        }
        check(area>90000 && leaks<area*.015,
              "curved foreground retains confirmed pixels instead of bright background holes",
              "%zu of %zu interior pixels bright (old shader 3870)",leaks,area);
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
        ID3D11Query* dj = nullptr;
        bool queriesOk = SUCCEEDED(dev->CreateQuery(&qd, &dj));
        qd.Query = D3D11_QUERY_TIMESTAMP;
        ID3D11Query *q0 = nullptr, *q1 = nullptr;
        queriesOk = queriesOk && SUCCEEDED(dev->CreateQuery(&qd, &q0)) &&
                    SUCCEEDED(dev->CreateQuery(&qd, &q1));
        auto ready = [&](ID3D11Query* query, void* data, UINT bytes) {
            const ULONGLONG deadline = GetTickCount64() + 5000;
            HRESULT hr;
            while ((hr = ctx->GetData(query, data, bytes, 0)) == S_FALSE) {
                if (GetTickCount64() >= deadline) return false;
                Sleep(1);
            }
            return hr == S_OK;
        };
        auto measure = [&](const char* label) {
            if (!queriesOk) return -1.0;
            // The first batch can include a large driver/scheduling transient:
            // measured 13.973 ms, then seven batches at 1.432-1.659 ms with the
            // same renderer. Report every sample and use the steady-state median;
            // keep the 4 ms budget and fail on ANY invalid/disjoint measurement.
            double samples[5] = {};
            for (int batch = 0; batch < 5; ++batch) {
                for (int i = 0; i < 5; ++i)
                    if (!warp.run(dev, ctx, s, s, p, why, sizeof(why))) return -1.0;
                UINT64 t0 = 0, t1 = 0;
                ctx->End(q1); // complete warmup before starting the timed batch
                if (!ready(q1, &t1, sizeof(t1))) return -1.0;
                ctx->Begin(dj); ctx->End(q0);
                const int kRuns = 10;
                bool rendered = true;
                for (int i = 0; i < kRuns; ++i)
                    if (!warp.run(dev, ctx, s, s, p, why, sizeof(why))) rendered = false;
                ctx->End(q1); ctx->End(dj);
                D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dd = {};
                if (!rendered || !ready(dj, &dd, sizeof(dd)) ||
                    !ready(q0, &t0, sizeof(t0)) || !ready(q1, &t1, sizeof(t1)) ||
                    dd.Disjoint || !dd.Frequency || t1 <= t0) return -1.0;
                samples[batch] = double(t1 - t0) * 1000.0 / dd.Frequency / kRuns;
            }
            printf("    %s batches (ms): %.3f %.3f %.3f %.3f %.3f\n", label,
                   samples[0], samples[1], samples[2], samples[3], samples[4]);
            std::sort(samples, samples + 5);
            return samples[2];
        };
        const double ms = measure("world");
        check(ms > 0 && ms < 4.0, "cost per rebuild at 2064x2208",
              "median %.3f ms (grid %d, 5 x 10 rebuilds)", ms, p.grid);
        // Exercise the added foreground hint at its worst coverage: every
        // pixel is foreground and carries a valid player-motion transform.
        std::vector<uint8_t> mask((size_t)W*H,255);
        auto* mt=make_tex(W,H,DXGI_FORMAT_R8_UNORM,D3D11_BIND_SHADER_RESOURCE,mask.data(),W);
        auto* msrv=srv_of(mt);s.mask=msrv;s.foregroundCameraValid=true;s.foregroundCam=s.cam;
        const double fgMs = measure("foreground");
        check(fgMs > 0 && fgMs < 4.0, "cost with foreground motion covering the entire eye",
              "median %.3f ms (5 x 10 rebuilds)", fgMs);
        rel(msrv);rel(mt);
        rel(dj); rel(q0); rel(q1); rel(cs); rel(ds); rel(ct); rel(dt);
    }
    warp.shutdown();
    return finish("afw-gpu");
}
