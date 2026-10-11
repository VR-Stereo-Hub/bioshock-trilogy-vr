// gpu-test-sources: src/game/bioshock1r/temporal_gpu.cpp src/core/gfx/dlss_client.cpp
// gpu-test-needs: dlss-host
// The DLSS transport end to end on this PC's GPU, with no game: the production 32-bit
// client (core/gfx/dlss_client) starts the real x64 helper (src/tools/dlss_host, NGX
// 310.7), and the production guides (game/bioshock1r/temporal_gpu) feed it a ray-traced
// camera move. Proves: the 32-to-64 handshake, DLAA and Super Resolution features, the
// vector SIGN and scale convention (the output must follow the truth better with the
// production vectors than with none or with negated ones), the reset flag, and a clean
// stop. Needs an NVIDIA RTX GPU; on any other GPU the suite says so and fails.
#include "gpu_test_util.h"
#include "core/gfx/dlss_client.h"
#include "game/bioshock1r/temporal_gpu.h"

#include <string>

using namespace gt;
namespace tg = bvr::b1r::tgpu;

namespace {

const double kNear = 10.0, kFar = 65536.0;

void logfn(int level, const char* line) { printf("      [%s] %s\n", level == 0 ? "info" : level == 1 ? "warn" : "error", line); }

Scene make_scene() {
    Scene s;
    s.solids.push_back({{100, -30, -100}, {160, 30, 200}, 1});
    s.solids.push_back({{-60, 40, 0}, {-20, 120, 80}, 2});
    s.solids.push_back({{-10, -140, -40}, {60, -90, 30}, 4});
    return s;
}

Cam cam_at(int frame, uint32_t w, uint32_t h, double yawPerFrame, double stepPerFrame) {
    Cam c;
    c.loc[0] = -150 + stepPerFrame * frame; c.loc[1] = 20; c.loc[2] = 60;
    c.rot = {0, (int32_t)(yawPerFrame * frame * bvr::ue::kRotUnitsPerDegree), 0};
    c.tanH = std::tan(50.0 * 3.14159265 / 180.0);
    c.tanV = c.tanH * h / w;
    return c;
}

tg::Camera to_tcam(const Cam& c) {
    tg::Camera t;
    double f[3], r[3], u[3];
    c.basis(f, r, u);
    for (int i = 0; i < 3; ++i) { t.loc[i] = (float)c.loc[i]; t.fwd[i] = (float)f[i]; t.right[i] = (float)r[i]; t.up[i] = (float)u[i]; }
    t.tanH = (float)c.tanH; t.tanV = (float)c.tanV;
    return t;
}

// Mean absolute error (0..1 per channel, RGB) of an RGBA8 readback against a render.
double image_error(const std::vector<float>& got, const Render& truth, uint32_t w, uint32_t h, uint32_t border) {
    double s = 0; size_t n = 0;
    for (uint32_t y = border; y < h - border; ++y)
        for (uint32_t x = border; x < w - border; ++x) {
            const uint32_t t = truth.rgba[(size_t)y * w + x];
            for (int c = 0; c < 3; ++c) { s += std::fabs(got[((size_t)y * w + x) * 4 + c] - ((t >> (8 * c)) & 255) / 255.0); ++n; }
        }
    return n ? s / n : 1.0;
}

float halton(uint32_t i, uint32_t b) {
    float f = 1.0f, r = 0.0f;
    while (i) { f /= (float)b; r += f * (float)(i % b); i /= b; }
    return r;
}

struct Seq {
    int vecMode = 1;          // 1 production vectors, 0 none, -1 negated
    bool jitter = false;      // Halton(2,3) sample offsets, 8 phases
    float jitterSign = -1.0f; // what DLSS receives per unit of sample offset (the Dishonored mod proved -1)
    double detail = 0.0;
};
double g_lastBaseline = 0;    // the mean error of the raw inputs themselves over the scored frames

// Runs `frames` images of a camera move through one eye of the client and returns the mean
// error of the last `scored` outputs against supersampled truth at the output size.
double run_sequence(bvr::dlss::Client& cl, int eye, uint32_t w, uint32_t h, uint32_t ow, uint32_t oh, const Seq& q,
                    int frames, int scored, double yawPerFrame, double stepPerFrame, bool* ok, std::string* fail) {
    *ok = false;
    char why[256] = "";
    if (!cl.built(eye, w, h, ow, oh, DXGI_FORMAT_R8G8B8A8_UNORM) &&
        !cl.build(eye, w, h, ow, oh, DXGI_FORMAT_R8G8B8A8_UNORM, 0, why, sizeof(why))) {
        *fail = std::string("build: ") + why; return 1.0;
    }
    Scene s = make_scene();
    s.detail = q.detail;
    tg::TemporalGpu t;
    double err = 0, base = 0; int n = 0;
    const int vecMode = q.vecMode;
    ID3D11Texture2D* zeroMv = make_tex(w, h, DXGI_FORMAT_R16G16_FLOAT, D3D11_BIND_SHADER_RESOURCE);
    for (int f = 0; f < frames; ++f) {
        const Cam cur = cam_at(f, w, h, yawPerFrame, stepPerFrame), prev = cam_at(f > 0 ? f - 1 : 0, w, h, yawPerFrame, stepPerFrame);
        const double jx = q.jitter ? halton((uint32_t)(f % 8) + 1, 2) - 0.5 : 0.0;
        const double jy = q.jitter ? halton((uint32_t)(f % 8) + 1, 3) - 0.5 : 0.0;
        const Render r = render(s, cur, w, h, kNear, kFar, 1, jx, jy);
        ID3D11Texture2D* color = make_tex(w, h, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_SHADER_RESOURCE, r.rgba.data(), w * 4);
        ID3D11Texture2D* raw = make_tex(w, h, DXGI_FORMAT_R32_FLOAT, D3D11_BIND_SHADER_RESOURCE, r.raw.data(), w * 4);
        ID3D11ShaderResourceView* rawSrv = srv_of(raw);
        tg::Params p;
        p.w = w; p.h = h; p.nearUu = (float)kNear; p.farUu = (float)kFar;
        p.historyValid = f > 0; p.cur = to_tcam(cur); p.prev = to_tcam(prev);
        if (vecMode < 0) {   // negated: swap the cameras (the vector of the reverse move)
            p.prev = to_tcam(cam_at(f + 1, w, h, yawPerFrame, stepPerFrame));
        }
        if (!t.run(dev, ctx, rawSrv, nullptr, p, why, sizeof(why))) { *fail = std::string("guides: ") + why; rel(rawSrv); rel(raw); rel(color); break; }
        bvr::dlss::EyeInputs in;
        in.color = color; in.depth = t.dlss_depth(); in.motion = vecMode == 0 ? zeroMv : t.motion();
        in.reset = f == 0;
        in.jitterX = (float)(q.jitterSign * jx);
        in.jitterY = (float)(q.jitterSign * jy);
        const bool good = cl.evaluate(ctx, eye, in, why, sizeof(why));
        if (!good) { *fail = std::string("evaluate: ") + why; rel(rawSrv); rel(raw); rel(color); break; }
        if (f >= frames - scored) {
            Cam co = cur; co.tanV = co.tanH * oh / ow;
            const Render truth = render(s, co, ow, oh, kNear, kFar, 4);
            err += image_error(readback(cl.output_texture(eye)), truth, ow, oh, ow / 16);
            if (ow == w) {
                // The unjittered single-sample render is what the game would show without DLAA.
                const Render plain = render(s, cur, w, h, kNear, kFar, 1);
                std::vector<float> in8((size_t)w * h * 4);
                for (size_t i = 0; i < plain.rgba.size(); ++i)
                    for (int c = 0; c < 4; ++c) in8[i * 4 + c] = ((plain.rgba[i] >> (8 * c)) & 255) / 255.0f;
                base += image_error(in8, truth, w, h, w / 16);
            }
            ++n;
        }
        rel(rawSrv); rel(raw); rel(color);
        if (f == frames - 1) *ok = true;
    }
    rel(zeroMv);
    t.shutdown();
    g_lastBaseline = n ? base / n : 0;
    return n ? err / n : 1.0;
}

} // namespace

int main(int argc, char** argv) {
    printf("dlss-host: the DLSS transport and NGX on this GPU\n");
    if (argc < 2) { printf("usage: dlss-host-tests <repo>\n"); return 2; }
    if (!make_device()) return 1;
    const std::string repo = argv[1];
    const std::wstring wrepo(repo.begin(), repo.end());
    const std::wstring exe = wrepo + L"\\build\\dlss_host\\bvr_dlss_host64.exe";
    const std::wstring data = wrepo + L"\\build\\gpu-tests\\dlss-data";
    CreateDirectoryW(data.c_str(), nullptr);

    bvr::dlss::Client cl;
    bvr::dlss::StartParams sp;
    sp.hostExe = exe.c_str(); sp.dataDir = data.c_str(); sp.log = logfn;
    char why[256] = "";
    LARGE_INTEGER f0, t0, t1; QueryPerformanceFrequency(&f0); QueryPerformanceCounter(&t0);
    const bool started = cl.start(dev, sp, why, sizeof(why));
    QueryPerformanceCounter(&t1);
    check(started, "helper start + NGX init", "%s (%.0f ms) %s", started ? cl.adapter() : "FAILED",
          (t1.QuadPart - t0.QuadPart) * 1000.0 / f0.QuadPart, why);
    if (!started) return finish("dlss-host");

    // Negative control first: a size the feature does not have must be refused, not crash.
    {
        bvr::dlss::EyeInputs in;
        const bool r = cl.evaluate(ctx, 1, in, why, sizeof(why));
        check(!r && cl.running(), "control: evaluate on an unbuilt eye is refused", "%s", why);
    }

    const uint32_t W = 512, H = 480;
    // DLAA on a detailed scene with projection jitter, during a slow turn and walk.
    // Production vectors against none, negated, and the opposite jitter sign.
    bool ok = false; std::string fail;
    const int kFrames = 24, kScored = 8;
    const double kYaw = 0.3, kStep = 1.0;
    Seq q; q.jitter = true; q.detail = 0.25;
    const double good = run_sequence(cl, 0, W, H, W, H, q, kFrames, kScored, kYaw, kStep, &ok, &fail);
    const double baseline = g_lastBaseline;
    check(ok, "DLAA sequence runs", "%s", fail.c_str());
    Seq qn = q; qn.vecMode = 0;
    const double none = run_sequence(cl, 0, W, H, W, H, qn, kFrames, kScored, kYaw, kStep, &ok, &fail);
    Seq qg = q; qg.vecMode = -1;
    const double neg = run_sequence(cl, 0, W, H, W, H, qg, kFrames, kScored, kYaw, kStep, &ok, &fail);
    Seq qj = q; qj.jitterSign = 1.0f;
    const double jitWrong = run_sequence(cl, 0, W, H, W, H, qj, kFrames, kScored, kYaw, kStep, &ok, &fail);
    check(good < baseline * 0.9, "DLAA antialiases (closer to supersampled truth than the raw render)",
          "DLAA %.5f, raw single sample %.5f", good, baseline);
    check(good < none * 0.9 && good < neg * 0.8, "DLAA follows the production vectors (sign and scale)",
          "error with vectors %.5f, none %.5f, negated %.5f", good, none, neg);
    check(good < jitWrong, "jitter sign: DLSS reads the negated sample offset", "-1: %.5f, +1: %.5f", good, jitWrong);
    {
        // Without projection jitter (what the game gives today): informational.
        Seq q0 = q; q0.jitter = false;
        const double nj = run_sequence(cl, 0, W, H, W, H, q0, kFrames, kScored, kYaw, kStep, &ok, &fail);
        Seq q0n = q0; q0n.vecMode = 0;
        const double njNone = run_sequence(cl, 0, W, H, W, H, q0n, kFrames, kScored, kYaw, kStep, &ok, &fail);
        printf("    no jitter: DLAA %.5f (none %.5f) vs raw %.5f\n", nj, njNone, g_lastBaseline);
    }

    // Super Resolution, Quality (1.5x per axis) on the other eye: its own feature and history.
    const uint32_t OW = 768, OH = 720;
    const double sr = run_sequence(cl, 1, W, H, OW, OH, q, kFrames, kScored, kYaw, kStep, &ok, &fail);
    check(ok, "DLSS SR sequence runs (512x480 -> 768x720)", "%s", fail.c_str());
    {
        // Reference: the bilinear upscale of the same (unjittered) input.
        Scene s = make_scene();
        s.detail = q.detail;
        const Cam c = cam_at(kFrames - 1, W, H, kYaw, kStep);
        Cam co = c; co.tanV = co.tanH * OH / OW;
        const Render lo = render(s, c, W, H, kNear, kFar, 1);
        const Render truth = render(s, co, OW, OH, kNear, kFar, 4);
        std::vector<float> up((size_t)OW * OH * 4);
        for (uint32_t y = 0; y < OH; ++y)
            for (uint32_t x = 0; x < OW; ++x) {
                const double sx = (x + 0.5) * W / OW - 0.5, sy = (y + 0.5) * H / OH - 0.5;
                const int x0 = (int)std::floor(sx), y0 = (int)std::floor(sy);
                const double fx = sx - x0, fy = sy - y0;
                for (int ch = 0; ch < 4; ++ch) {
                    auto at = [&](int xx, int yy) {
                        xx = xx < 0 ? 0 : xx >= (int)W ? W - 1 : xx; yy = yy < 0 ? 0 : yy >= (int)H ? H - 1 : yy;
                        return ((lo.rgba[(size_t)yy * W + xx] >> (8 * ch)) & 255) / 255.0;
                    };
                    up[((size_t)y * OW + x) * 4 + ch] = (float)((at(x0, y0) * (1 - fx) + at(x0 + 1, y0) * fx) * (1 - fy) +
                                                                (at(x0, y0 + 1) * (1 - fx) + at(x0 + 1, y0 + 1) * fx) * fy);
                }
            }
        const double bil = image_error(up, truth, OW, OH, OW / 16);
        check(sr < bil * 1.15, "DLSS SR is at least as faithful as a bilinear stretch", "SR %.4f, bilinear %.4f", sr, bil);
    }
    // Reset: an unrelated image with reset must not carry the previous scene.
    {
        const Scene s = make_scene();
        Cam other = cam_at(0, W, H, 0, 0);
        other.rot.yaw = (int32_t)(150 * bvr::ue::kRotUnitsPerDegree);
        const Render r = render(s, other, W, H, kNear, kFar, 1);
        ID3D11Texture2D* color = make_tex(W, H, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_SHADER_RESOURCE, r.rgba.data(), W * 4);
        ID3D11Texture2D* raw = make_tex(W, H, DXGI_FORMAT_R32_FLOAT, D3D11_BIND_SHADER_RESOURCE, r.raw.data(), W * 4);
        ID3D11ShaderResourceView* rawSrv = srv_of(raw);
        tg::TemporalGpu t;
        tg::Params p; p.w = W; p.h = H; p.historyValid = false; p.cur = p.prev = to_tcam(other);
        t.run(dev, ctx, rawSrv, nullptr, p, why, sizeof(why));
        double errReset = 1, errCarry = 1;
        for (int pass = 0; pass < 2; ++pass) {
            // pass 0: reset set; pass 1: re-run the prior sequence, then the cut without reset.
            if (pass == 1) run_sequence(cl, 0, W, H, W, H, Seq{}, 8, 1, 0.8, 3.0, &ok, &fail);
            bvr::dlss::EyeInputs in;
            in.color = color; in.depth = t.dlss_depth(); in.motion = t.motion(); in.reset = pass == 0;
            cl.evaluate(ctx, 0, in, why, sizeof(why));
            const double e = image_error(readback(cl.output_texture(0)), r, W, H, W / 16);
            (pass == 0 ? errReset : errCarry) = e;
        }
        check(errReset < 0.03, "reset: a cut is shown as it is", "error %.4f", errReset);
        check(errCarry > errReset, "control: the same cut without reset carries history", "with reset %.4f, without %.4f", errReset, errCarry);
        t.shutdown(); rel(rawSrv); rel(raw); rel(color);
    }
    const auto& st = cl.stats;
    printf("    helper evaluate GPU avg: eye0 %.3f ms, eye1 %.3f ms; round trip avg eye0 %.3f ms\n",
           st.gpuN[0] ? st.gpuMsSum[0] / st.gpuN[0] : -1.0, st.gpuN[1] ? st.gpuMsSum[1] / st.gpuN[1] : -1.0,
           st.frames[0] ? st.cpuMsSum[0] / (double)st.frames[0] : -1.0);
    cl.stop();
    check(!cl.running(), "clean stop", "helper gone");
    return finish("dlss-host");
}
