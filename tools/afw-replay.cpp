// AFW replay: runs the PRODUCTION held-eye rebuild (src/game/bioshock1r/afw_gpu.cpp) on a capture
// written by `afw dump` / the F10 "AFW capture" button, with any lever changed, writes viewable
// BMPs of every input and output, prints what the capture says about the hands' depth and the
// cameras, and scores each rebuild against the NEXT present's native image of the same eye (the
// frame the rebuild alternates with in the headset). The Dishonored VR mod's tools/afw-replay.cpp,
// ported. Never touches the game; reads only the local capture folder, which is game output and
// stays local, as does everything this writes.
//
// Build and run: tools\afw-replay.ps1 -Capture <data dir>\dumps\afw-... [-Mask 0] [-Gain 1.12]
//                [-Stale 0] [-Grid 2] [-Tint] [-NoHeld] [-NoFresh] [-NoPictures]
#include "game/bioshock1r/afw_gpu.h"

#define NOMINMAX
#include <windows.h>
#include <d3d11.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace af = bvr::b1r::afw;
namespace tg = bvr::b1r::tgpu;

namespace {

ID3D11Device* g_dev = nullptr;
ID3D11DeviceContext* g_ctx = nullptr;
bool g_pictureWriteFailed = false;

typedef std::map<std::string, std::string> Meta;
bool load_meta(const std::string& path, Meta& m) {
    FILE* f = nullptr;
    if (fopen_s(&f, path.c_str(), "r") || !f) return false;
    char line[4096];
    while (fgets(line, sizeof(line), f)) {
        char* e = strchr(line, '=');
        if (!e) continue;
        *e = 0;
        std::string v = e + 1;
        while (!v.empty() && (v.back() == '\n' || v.back() == '\r')) v.pop_back();
        m[line] = v;
    }
    fclose(f);
    return true;
}
bool read_file(const std::string& path, std::vector<uint8_t>& out) {
    FILE* f = nullptr;
    if (fopen_s(&f, path.c_str(), "rb") || !f) return false;
    fseek(f, 0, SEEK_END);
    const long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    out.resize(n);
    const size_t r = fread(out.data(), 1, n, f);
    fclose(f);
    return r == (size_t)n;
}
// "key=ok WxH fmt F bpp B"
struct TexInfo { bool ok = false; uint32_t w = 0, h = 0; unsigned fmt = 0, bpp = 0; };
TexInfo tex_info(const std::string& s) {
    TexInfo t;
    t.ok = sscanf_s(s.c_str(), "ok %ux%u fmt %u bpp %u", &t.w, &t.h, &t.fmt, &t.bpp) == 4;
    return t;
}
tg::Camera camera_of(const std::string& s) {
    tg::Camera c;
    std::string t = s;
    for (char& ch : t) if (ch == '|') ch = ' ';
    float v[14] = {};
    const char* p = t.c_str();
    for (int i = 0; i < 14; ++i) v[i] = strtof(p, (char**)&p);
    for (int i = 0; i < 3; ++i) { c.loc[i] = v[i]; c.fwd[i] = v[3 + i]; c.right[i] = v[6 + i]; c.up[i] = v[9 + i]; }
    c.tanH = v[12]; c.tanV = v[13];
    return c;
}
ID3D11Texture2D* make_tex(uint32_t w, uint32_t h, DXGI_FORMAT f, UINT bind, const void* data, uint32_t pitch, D3D11_USAGE use = D3D11_USAGE_DEFAULT, UINT cpu = 0) {
    D3D11_TEXTURE2D_DESC d = {};
    d.Width = w; d.Height = h; d.MipLevels = 1; d.ArraySize = 1; d.Format = f; d.SampleDesc.Count = 1;
    d.Usage = use; d.BindFlags = bind; d.CPUAccessFlags = cpu;
    D3D11_SUBRESOURCE_DATA sd = {data, pitch, 0};
    ID3D11Texture2D* t = nullptr;
    g_dev->CreateTexture2D(&d, data ? &sd : nullptr, &t);
    return t;
}
ID3D11ShaderResourceView* srv_of(ID3D11Texture2D* t) {
    ID3D11ShaderResourceView* s = nullptr;
    if (t) g_dev->CreateShaderResourceView(t, nullptr, &s);
    return s;
}
template <class T> void rel(T*& p) { if (p) { p->Release(); p = nullptr; } }

// RGBA8 or BGRA8 rows as the capture stored them -> RGBA8 (what the warp samples; channel order
// only matters for the pictures, so a BGRA capture is swapped here).
DXGI_FORMAT colour_format(unsigned fmt) {
    switch (fmt) {
        case 87: case 90: case 91: return DXGI_FORMAT_B8G8R8A8_UNORM;   // B8G8R8A8 typeless/unorm/srgb
        default: return DXGI_FORMAT_R8G8B8A8_UNORM;
    }
}
bool is_bgra(unsigned fmt) { return fmt == 87 || fmt == 90 || fmt == 91; }

std::vector<uint8_t> readback_rgba(ID3D11Texture2D* t, uint32_t w, uint32_t h) {
    std::vector<uint8_t> out((size_t)w * h * 4, 0);
    ID3D11Texture2D* st = make_tex(w, h, DXGI_FORMAT_R8G8B8A8_UNORM, 0, nullptr, 0, D3D11_USAGE_STAGING, D3D11_CPU_ACCESS_READ);
    if (!st) return {};
    g_ctx->CopyResource(st, t);
    D3D11_MAPPED_SUBRESOURCE m = {};
    if (SUCCEEDED(g_ctx->Map(st, 0, D3D11_MAP_READ, 0, &m))) {
        for (uint32_t y = 0; y < h; ++y) memcpy(&out[(size_t)y * w * 4], (const uint8_t*)m.pData + (size_t)y * m.RowPitch, (size_t)w * 4);
        g_ctx->Unmap(st, 0);
    } else out.clear();
    st->Release();
    return out;
}

// 24-bit BMP from RGBA8 rows (top-down in, bottom-up out).
bool write_bmp(const std::string& path, const uint8_t* rgba, uint32_t w, uint32_t h, bool bgra = false) {
    FILE* f = nullptr;
    if (fopen_s(&f, path.c_str(), "wb") || !f) { g_pictureWriteFailed = true; return false; }
    const uint32_t rowBytes = (w * 3 + 3) & ~3u, size = 54 + rowBytes * h;
    uint8_t hdr[54] = {'B', 'M'};
    auto put32 = [&](int at, uint32_t v) { memcpy(hdr + at, &v, 4); };
    auto put16 = [&](int at, uint16_t v) { memcpy(hdr + at, &v, 2); };
    put32(2, size); put32(10, 54); put32(14, 40); put32(18, w); put32(22, h); put16(26, 1); put16(28, 24); put32(34, rowBytes * h);
    bool ok = fwrite(hdr, 1, 54, f) == 54;
    std::vector<uint8_t> row(rowBytes, 0);
    for (int y = (int)h - 1; y >= 0; --y) {
        const uint8_t* s = rgba + (size_t)y * w * 4;
        for (uint32_t x = 0; x < w; ++x) {
            row[x * 3 + 0] = bgra ? s[x * 4 + 0] : s[x * 4 + 2];
            row[x * 3 + 1] = s[x * 4 + 1];
            row[x * 3 + 2] = bgra ? s[x * 4 + 2] : s[x * 4 + 0];
        }
        ok = (fwrite(row.data(), 1, rowBytes, f) == rowBytes) && ok;
    }
    ok = (fclose(f) == 0) && ok;
    if (!ok) g_pictureWriteFailed = true;
    return ok;
}
// Linear depth (uu) as a picture: log scale from 10 uu (white) to 65536 uu (black), sky black.
void depth_picture(const std::vector<uint8_t>& raw, uint32_t w, uint32_t h, const std::string& path) {
    const float* z = (const float*)raw.data();
    std::vector<uint8_t> px((size_t)w * h * 4, 255);
    for (size_t i = 0; i < (size_t)w * h; ++i) {
        const float v = z[i];
        uint8_t g = 0;
        if (v > 0.0f) g = (uint8_t)(255.0f * (1.0f - std::min(1.0f, std::max(0.0f, std::log(v / 10.0f) / std::log(6553.6f)))));
        px[i * 4] = px[i * 4 + 1] = px[i * 4 + 2] = g;
    }
    write_bmp(path, px.data(), w, h);
}
void mask_picture(const std::vector<uint8_t>& raw, uint32_t w, uint32_t h, const std::string& path) {
    std::vector<uint8_t> px((size_t)w * h * 4, 255);
    for (size_t i = 0; i < (size_t)w * h; ++i) px[i * 4] = px[i * 4 + 1] = px[i * 4 + 2] = raw[i] > 127 ? 255 : 0;
    write_bmp(path, px.data(), w, h);
}

struct HandStats { size_t n = 0; float median = 0, lo = 0, hi = 0; };
HandStats hand_depth(const std::vector<uint8_t>& depth, const std::vector<uint8_t>& mask, uint32_t w, uint32_t h) {
    HandStats s;
    if (depth.size() < (size_t)w * h * 4 || mask.size() < (size_t)w * h) return s;
    std::vector<float> zs;
    const float* z = (const float*)depth.data();
    for (size_t i = 0; i < (size_t)w * h; ++i)
        if (mask[i] > 127 && z[i] > 0.0f) zs.push_back(z[i]);
    if (zs.empty()) return s;
    std::sort(zs.begin(), zs.end());
    s.n = zs.size(); s.median = zs[zs.size() / 2]; s.lo = zs[zs.size() / 50]; s.hi = zs[zs.size() * 49 / 50];
    return s;
}
float lum(const uint8_t* p, bool bgra) { return bgra ? 0.114f * p[0] + 0.587f * p[1] + 0.299f * p[2] : 0.299f * p[0] + 0.587f * p[1] + 0.114f * p[2]; }
struct Score { double mean = 0, hand = 0, differ = 0; size_t handN = 0; };
// got: RGBA8 (the replay's or the game's rebuild), truth: the capture's next fresh image (its own channel order).
Score score(const std::vector<uint8_t>& got, bool gotBgra, const std::vector<uint8_t>& truth, bool truthBgra,
            const std::vector<uint8_t>& handMask, uint32_t mw, uint32_t mh, uint32_t w, uint32_t h) {
    Score s;
    size_t n = 0, nd = 0;
    const uint32_t b = w / 40;
    for (uint32_t y = b; y < h - b; ++y)
        for (uint32_t x = b; x < w - b; ++x) {
            const size_t i = (size_t)y * w + x;
            const uint8_t* g = &got[i * 4]; const uint8_t* t = &truth[i * 4];
            const float gl = lum(g, gotBgra), tl = lum(t, truthBgra);
            double e = 0;
            for (int c = 0; c < 3; ++c) {
                const int gc = gotBgra ? 2 - c : c, tc = truthBgra ? 2 - c : c;
                e += std::fabs((double)g[gc] - t[tc]) / 255.0;
            }
            e /= 3;
            s.mean += e; ++n;
            if (std::fabs(gl - tl) > 40.0f) ++nd;
            if (!handMask.empty()) {
                const uint32_t mx = x * mw / w, my = y * mh / h;
                if (handMask[(size_t)my * mw + mx] > 127) { s.hand += e; ++s.handN; }
            }
        }
    s.mean /= std::max<size_t>(n, 1); s.differ = (double)nd / std::max<size_t>(n, 1);
    if (s.handN) s.hand /= s.handN;
    return s;
}


// Dishonored's "measure first" (guide 9.5), offline: present p's fresh image and the next present's
// (the other eye, ~7 ms later - a near-native pair when the head is still) are block-matched
// along the row at sampled pixels. The ratio measured / predicted disparity (predicted from the
// stored depth with the WORLD lens) is 1 where the depth and lens are right; for hand pixels its
// inverse can indicate a lens gain ONLY for stationary, rigid geometry and independently
// verified depth. Eye-dependent IK changes the shape between eyes (run eleven) and biases
// the same ratio. World pixels are the control; a ratio alone is not a calibration.
struct ProbeOut {
    double handRatio = 0, worldRatio = 0; size_t hands = 0, world = 0;
    // The projection that explains the measured depths: 1/z is linear in device depth for a
    // standard projection (1/z = 1/N - raw * (F - N) / (F * N)), so a line fit of 1/z_true
    // against the stored device depth gives the near and far plane the hands were drawn with.
    double handNear = 0, handFar = 0, handFitRms = 0, worldNear = 0, worldFar = 0, worldFitRms = 0;
};
static double raw_of(double z, double n, double f) { return f * (z - n) / (z * (f - n)); }
static void fit_planes(const std::vector<std::pair<double, double>>& pts, double* nearOut, double* farOut, double* rms) {
    *nearOut = *farOut = *rms = 0;
    if (pts.size() < 20) return;
    double sx = 0, sy = 0, sxx = 0, sxy = 0;
    for (auto& p : pts) { sx += p.first; sy += p.second; sxx += p.first * p.first; sxy += p.first * p.second; }
    const double n = (double)pts.size();
    const double den = n * sxx - sx * sx;
    if (std::fabs(den) < 1e-12) return;
    const double slope = (n * sxy - sx * sy) / den, A = (sy - slope * sx) / n, B = -slope;
    double e2 = 0;
    for (auto& p : pts) { const double e = p.second - (A - B * p.first); e2 += e * e; }
    *rms = std::sqrt(e2 / n);
    if (A > 0) { *nearOut = 1.0 / A; *farOut = (A - B) > 1e-9 ? 1.0 / (A - B) : 1e9; }
}
ProbeOut probe_pair(const std::vector<uint8_t>& L, bool lBgra, const std::vector<uint8_t>& R, bool rBgra, const std::vector<uint8_t>& depth,
                    const std::vector<uint8_t>& mask, uint32_t w, uint32_t h, uint32_t dw, uint32_t dh, float sepUu, float tanH,
                    int sign, double nearUu = 10.0, double farUu = 65536.0) {
    // sign: +1 when L is the left eye and R the right (the right eye sees a point further LEFT).
    ProbeOut o;
    std::vector<std::pair<double, double>> hpts, wpts;
    std::vector<float> hr, wr;
    const float* z = (const float*)depth.data();
    const int B = 10, step = 20;
    auto lum = [&](const std::vector<uint8_t>& img, bool bgra, int x, int y) {
        const uint8_t* q = &img[((size_t)y * w + x) * 4];
        return bgra ? 0.114f * q[0] + 0.587f * q[1] + 0.299f * q[2] : 0.299f * q[0] + 0.587f * q[1] + 0.114f * q[2];
    };
    for (int y = B + 40; y < (int)h - B - 40; y += step)
        for (int x = B + 40; x < (int)w - B - 40; x += step) {
            const uint32_t mx = x * dw / w, my = y * dh / h;
            const float zz = z[(size_t)my * dw + mx];
            if (!(zz > 0.0f)) continue;
            const bool hand = !mask.empty() && mask[(size_t)my * dw + mx] > 127;
            if (!hand && (zz < 150.0f || zz > 1500.0f)) continue;
            const float dpred = (sepUu / zz) / tanH * (w * 0.5f);   // px, world lens
            if (dpred < 1.5f) continue;
            // Texture: skip flat blocks (no match possible).
            float mn = 1e9f, mxv = -1e9f;
            for (int j = -B; j <= B; j += 2) for (int i = -B; i <= B; i += 2) { const float v = lum(L, lBgra, x + i, y + j); mn = std::min(mn, v); mxv = std::max(mxv, v); }
            if (mxv - mn < 24.0f) continue;
            const int lo = (int)(x - sign * (dpred * 2.2f + 24.0f)), hi = (int)(x - sign * (dpred * 0.3f - 24.0f));
            int best = x; float bestSad = 1e30f;
            for (int xs = std::min(lo, hi); xs <= std::max(lo, hi); ++xs) {
                if (xs < B || xs >= (int)w - B) continue;
                float sad = 0;
                for (int j = -B; j <= B; j += 2) for (int i = -B; i <= B; i += 2) sad += std::fabs(lum(L, lBgra, x + i, y + j) - lum(R, rBgra, xs + i, y + j));
                if (sad < bestSad) { bestSad = sad; best = xs; }
            }
            const float meas = (float)(sign * (x - best));
            if (meas <= 0.5f) continue;
            if (!mask.empty() && hand) { const uint32_t rmx = best * dw / w; if (mask[(size_t)my * dw + rmx] <= 127) continue; }
            (hand ? hr : wr).push_back(meas / dpred);
            const double ztrue = sepUu / (meas / (w * 0.5f) * tanH);   // what the disparity says
            (hand ? hpts : wpts).push_back({raw_of(zz, nearUu, farUu), 1.0 / ztrue});
        }
    fit_planes(hpts, &o.handNear, &o.handFar, &o.handFitRms);
    fit_planes(wpts, &o.worldNear, &o.worldFar, &o.worldFitRms);
    auto median = [](std::vector<float>& v) { if (v.empty()) return 0.0; std::sort(v.begin(), v.end()); return (double)v[v.size() / 2]; };
    o.hands = hr.size(); o.world = wr.size();
    o.handRatio = median(hr); o.worldRatio = median(wr);
    return o;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        printf("usage: afw-replay <capture dir> <out dir> [mask 0|1] [gain] [stale 0|1] [grid] [tint 0|1] [held 0|1] [fresh 0|1] [pictures 0|1]\n");
        return 2;
    }
    setvbuf(stdout, nullptr, _IONBF, 0);
    const std::string dir = argv[1], out = argv[2];
    const int maskArg = argc > 3 ? atoi(argv[3]) : -1;
    const float gainArg = argc > 4 ? strtof(argv[4], nullptr) : 0.0f;
    const int staleArg = argc > 5 ? atoi(argv[5]) : -1;
    const int gridArg = argc > 6 ? atoi(argv[6]) : 0;
    const bool tint = argc > 7 && atoi(argv[7]) != 0;
    const bool useHeld = argc > 8 ? atoi(argv[8]) != 0 : true, useFresh = argc > 9 ? atoi(argv[9]) != 0 : true;
    const bool pictures = argc > 10 ? atoi(argv[10]) != 0 : true;
    const float fgTanArg = argc > 11 ? strtof(argv[11], nullptr) : 0.0f;   // the hands' lens over the world's; 0 = the capture's
    const int refineArg = argc > 12 ? atoi(argv[12]) : -1;
    CreateDirectoryA(out.c_str(), nullptr);
    const D3D_FEATURE_LEVEL fl[] = {D3D_FEATURE_LEVEL_11_0};
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, fl, 1, D3D11_SDK_VERSION, &g_dev, nullptr, &g_ctx))) {
        printf("no D3D11 device\n");
        return 2;
    }
    int n = 0;
    while (true) {
        char p[512];
        snprintf(p, sizeof(p), "%s\\p%02d.txt", dir.c_str(), n);
        if (GetFileAttributesA(p) == INVALID_FILE_ATTRIBUTES) break;
        ++n;
    }
    printf("capture %s: %d presents\n", dir.c_str(), n);
    if (!n) return 1;
    af::WarpGpu warp;
    char why[256] = "";
    double sumReplay = 0, sumGame = 0, sumHandReplay = 0, sumHandGame = 0, sumDiffer = 0;
    int scored = 0, replayed = 0;
    for (int p = 0; p < n; ++p) {
        char buf[512];
        Meta m, mn;
        snprintf(buf, sizeof(buf), "%s\\p%02d.txt", dir.c_str(), p);
        if (!load_meta(buf, m)) continue;
        const bool haveNext = p + 1 < n && (snprintf(buf, sizeof(buf), "%s\\p%02d.txt", dir.c_str(), p + 1), load_meta(buf, mn));
        auto path = [&](int q, const char* s) { char b[512]; snprintf(b, sizeof(b), "%s\\p%02d_%s.raw", dir.c_str(), q, s); return std::string(b); };
        auto outp = [&](int q, const char* s) { char b[512]; snprintf(b, sizeof(b), "%s\\p%02d_%s.bmp", out.c_str(), q, s); return std::string(b); };
        const TexInfo fc = tex_info(m["fresh.color"]), fd = tex_info(m["fresh.depth"]), fm = tex_info(m["fresh.mask"]);
        const TexInfo hc = tex_info(m["held.color"]), hd = tex_info(m["held.depth"]), hm = tex_info(m["held.mask"]);
        const TexInfo rb = tex_info(m["rebuilt"]);
        const int held = atoi(m["held"].c_str()), fresh = atoi(m["fresh"].c_str());
        uint32_t w = 0, h = 0;
        sscanf_s(m["output"].c_str(), "%ux%u", &w, &h);
        printf("\n-- p%02d: fresh eye %d (%s), held eye %d, output %ux%u, render %ux%u, how %s\n", p, fresh, fresh == 0 ? "left" : "right",
               held, w, h, fd.w, fd.h, m["how"].c_str());
        std::vector<uint8_t> fcol, fdep, fmsk, hcol, hdep, hmsk, rebuilt;
        if (!fc.ok || !read_file(path(p, "fresh"), fcol) || !fd.ok || !read_file(path(p, "fresh_depth"), fdep) ||
            fcol.size() != (size_t)fc.w * fc.h * 4 || fdep.size() != (size_t)fd.w * fd.h * 4) {
            printf("   fresh colour or depth missing - skipped\n");
            continue;
        }
        const bool haveFm = fm.ok && read_file(path(p, "fresh_mask"), fmsk);
        const bool haveH = hc.ok && hd.ok && read_file(path(p, "held"), hcol) && read_file(path(p, "held_depth"), hdep);
        const bool haveHm = haveH && hm.ok && read_file(path(p, "held_mask"), hmsk);
        const bool haveRb = rb.ok && read_file(path(p, "rebuilt"), rebuilt);
        if (m["renderer"] == "native-foreground-hdr")
            printf("   native foreground/HDR capture: saved rebuilt image is the displayed result; this replay is only the legacy AFW comparison\n");
        // What the capture says before any rebuild: the cameras and the hands' depth.
        const tg::Camera camF = camera_of(m["fresh.cam"]), camH = camera_of(m["held.cam"]), camT = camera_of(m["target.cam"]);
        {
            const float dx = camF.loc[0] - camH.loc[0], dy = camF.loc[1] - camH.loc[1], dz = camF.loc[2] - camH.loc[2];
            const float yawF = std::atan2(camF.fwd[1], camF.fwd[0]) * 57.2958f, yawH = std::atan2(camH.fwd[1], camH.fwd[0]) * 57.2958f;
            const float tx = camT.loc[0] - camF.loc[0], ty = camT.loc[1] - camF.loc[1], tz = camT.loc[2] - camF.loc[2];
            printf("   cameras: fresh -> held moved %.2f uu (%.2f %.2f %.2f), yaw %.3f deg; target is %.2f uu from the fresh eye "
                   "(the eye separation, expected %.2f); tangents fresh %.4f/%.4f target %.4f/%.4f\n",
                   std::sqrt(dx * dx + dy * dy + dz * dz), dx, dy, dz, yawF - yawH, std::sqrt(tx * tx + ty * ty + tz * tz),
                   2.0f * strtof(m["eyeHalfSepUu"].c_str(), nullptr), camF.tanH, camF.tanV, camT.tanH, camT.tanV);
            printf("   levers in the capture: mask %s, gain %s under %s uu, stale %s, grid %s, squeeze [%s), near %s far %s\n",
                   m["handsFromFresh"].c_str(), m["fgGain"].c_str(), m["fgBandUu"].c_str(), m["staleTest"].c_str(), m["grid"].c_str(),
                   m["squeeze"].c_str(), m["nearUu"].c_str(), m["farUu"].c_str());
            if (haveFm) {
                const HandStats s = hand_depth(fdep, fmsk, fd.w, fd.h);
                printf("   fresh hands mask: %zu px (%.1f%% of the image); their depth median %.1f uu, 2%%..98%% %.1f..%.1f uu "
                       "(includes the sleeves: near depth alone does not establish a projection error)\n",
                       s.n, 100.0 * s.n / ((double)fd.w * fd.h), s.median, s.lo, s.hi);
            } else {
                printf("   fresh hands mask: none in the capture\n");
            }
            if (haveHm) {
                const HandStats s = hand_depth(hdep, hmsk, hd.w, hd.h);
                printf("   held hands mask: %zu px; depth median %.1f uu, 2%%..98%% %.1f..%.1f uu\n", s.n, s.median, s.lo, s.hi);
            }
        }
        if (haveNext && atoi(mn["fresh"].c_str()) != fresh) {
            std::vector<uint8_t> next;
            const TexInfo nc = tex_info(mn["fresh.color"]);
            if (nc.ok && nc.w == fc.w && nc.h == fc.h && read_file(path(p + 1, "fresh"), next)) {
                const float sep = 2.0f * strtof(m["eyeHalfSepUu"].c_str(), nullptr);
                const int sign = fresh == 0 ? 1 : -1;   // this fresh eye is the left: the next (right) sees points further left
                const ProbeOut po = probe_pair(fcol, is_bgra(fc.fmt), next, is_bgra(nc.fmt), fdep, haveFm ? fmsk : std::vector<uint8_t>(),
                                               fc.w, fc.h, fd.w, fd.h, sep, camF.tanH, sign, strtod(m["nearUu"].c_str(), nullptr),
                                               strtod(m["farUu"].c_str(), nullptr));
                printf("   disparity probe against the next present (the other eye, a near-native pair when still): hands measured/"
                       "predicted %.3f over %zu blocks (inverse %.3f; requires stationary rigid geometry) | world (150-1500 uu, the control) %.3f over %zu blocks\n",
                       po.handRatio, po.hands, po.handRatio > 0 ? 1.0 / po.handRatio : 0.0, po.worldRatio, po.world);
                printf("   projection fit (1/z linear in device depth): hands near %.2f uu far %.0f uu (rms %.5f) | world near %.2f far %.0f (rms %.5f; the control should read the capture's %s / %s)\n",
                       po.handNear, po.handFar, po.handFitRms, po.worldNear, po.worldFar, po.worldFitRms, m["nearUu"].c_str(), m["farUu"].c_str());
            }
        }
        if (pictures) {
            write_bmp(outp(p, "fresh"), fcol.data(), fc.w, fc.h, is_bgra(fc.fmt));
            depth_picture(fdep, fd.w, fd.h, outp(p, "fresh_depth"));
            if (haveFm) mask_picture(fmsk, fm.w, fm.h, outp(p, "fresh_mask"));
            if (haveH) { write_bmp(outp(p, "held"), hcol.data(), hc.w, hc.h, is_bgra(hc.fmt)); depth_picture(hdep, hd.w, hd.h, outp(p, "held_depth")); }
            if (haveHm) mask_picture(hmsk, hm.w, hm.h, outp(p, "held_mask"));
            if (haveRb) write_bmp(outp(p, "rebuilt_ingame"), rebuilt.data(), rb.w, rb.h, false);
        }
        // The rebuild, re-run with the levers as asked.
        ID3D11Texture2D* tfc = make_tex(fc.w, fc.h, colour_format(fc.fmt), D3D11_BIND_SHADER_RESOURCE, fcol.data(), fc.w * 4);
        ID3D11Texture2D* tfd = make_tex(fd.w, fd.h, DXGI_FORMAT_R32_FLOAT, D3D11_BIND_SHADER_RESOURCE, fdep.data(), fd.w * 4);
        ID3D11Texture2D* tfm = haveFm ? make_tex(fm.w, fm.h, DXGI_FORMAT_R8_UNORM, D3D11_BIND_SHADER_RESOURCE, fmsk.data(), fm.w) : nullptr;
        ID3D11Texture2D* thc = haveH ? make_tex(hc.w, hc.h, colour_format(hc.fmt), D3D11_BIND_SHADER_RESOURCE, hcol.data(), hc.w * 4) : nullptr;
        ID3D11Texture2D* thd = haveH ? make_tex(hd.w, hd.h, DXGI_FORMAT_R32_FLOAT, D3D11_BIND_SHADER_RESOURCE, hdep.data(), hd.w * 4) : nullptr;
        ID3D11Texture2D* thm = haveHm ? make_tex(hm.w, hm.h, DXGI_FORMAT_R8_UNORM, D3D11_BIND_SHADER_RESOURCE, hmsk.data(), hm.w) : nullptr;
        ID3D11ShaderResourceView *sfc = srv_of(tfc), *sfd = srv_of(tfd), *sfm = srv_of(tfm), *shc = srv_of(thc), *shd = srv_of(thd), *shm = srv_of(thm);
        af::Source A, B;
        B.valid = sfc && sfd; B.color = sfc; B.depth = sfd; B.mask = sfm; B.depthW = fd.w; B.depthH = fd.h; B.cam = camF;
        A.valid = haveH && shc && shd && m["held.valid"] == "1"; A.color = shc; A.depth = shd; A.mask = shm; A.depthW = hd.w; A.depthH = hd.h; A.cam = camH;
        A.foregroundCameraValid = m["held.foregroundCameraValid"] == "1";
        if (A.foregroundCameraValid) A.foregroundCam = camera_of(m["held.foregroundCam"]);
        af::Params q;
        q.w = w; q.h = h; q.target = camT;
        q.refine = refineArg >= 0 ? refineArg != 0 : m["refine"] == "1";
        q.grid = gridArg > 0 ? gridArg : atoi(m["grid"].c_str());
        q.staleTest = staleArg >= 0 ? staleArg != 0 : m["staleTest"] == "1";
        q.edgeRel = strtof(m["edgeRel"].c_str(), nullptr); q.edgeAbsUu = strtof(m["edgeAbsUu"].c_str(), nullptr);
        q.staleRel = strtof(m["staleRel"].c_str(), nullptr); q.staleAbsUu = strtof(m["staleAbsUu"].c_str(), nullptr);
        q.nearerRel = strtof(m["nearerRel"].c_str(), nullptr); q.skyUu = strtof(m["skyUu"].c_str(), nullptr);
        q.fgGain = gainArg > 0.0f ? gainArg : strtof(m["fgGain"].c_str(), nullptr); q.fgBandUu = strtof(m["fgBandUu"].c_str(), nullptr);
        q.handsFromFresh = maskArg >= 0 ? maskArg != 0 : m["handsFromFresh"] == "1";
        q.fgTanGain = fgTanArg > 0.0f ? fgTanArg : (m.count("fgTanGain") ? strtof(m["fgTanGain"].c_str(), nullptr) : 1.0f);
        q.tint = tint; q.useHeld = useHeld; q.useFresh = useFresh;
        if (q.grid < 1) q.grid = 2;
        if (q.skyUu <= 0.0f) q.skyUu = 100000.0f;
        printf("   replay levers: refine %d, mask %d, gain %.3f, lens %.3f, stale %d, grid %d, tint %d, held %d, fresh %d\n", (int)q.refine, (int)q.handsFromFresh, q.fgGain, q.fgTanGain,
               (int)q.staleTest, q.grid, (int)q.tint, (int)q.useHeld, (int)q.useFresh);
        std::vector<uint8_t> replay;
        if (!warp.run(g_dev, g_ctx, A, B, q, why, sizeof(why))) {
            printf("   rebuild refused: %s\n", why);
        } else {
            replay = readback_rgba(warp.out(), w, h);
            if (!replay.empty()) {
                ++replayed;
                if (pictures) write_bmp(outp(p, "replay"), replay.data(), w, h, false);
            }
        }
        // Score against the next present's native image of this held eye.
        if (!replay.empty() && haveNext && atoi(mn["fresh"].c_str()) == held) {
            std::vector<uint8_t> next, nextMask;
            const TexInfo nc = tex_info(mn["fresh.color"]), nm = tex_info(mn["fresh.mask"]);
            if (nc.ok && nc.w == w && nc.h == h && read_file(path(p + 1, "fresh"), next)) {
                const bool haveNm = nm.ok && read_file(path(p + 1, "fresh_mask"), nextMask);
                const Score sr = score(replay, false, next, is_bgra(nc.fmt), haveNm ? nextMask : std::vector<uint8_t>(), nm.w, nm.h, w, h);
                printf("   vs the next native frame of eye %d: replay mean error %.4f, hand region %.4f (%zu px), %.2f%% of pixels differ by > 40/255",
                       held, sr.mean, sr.hand, sr.handN, sr.differ * 100);
                sumReplay += sr.mean; sumHandReplay += sr.hand; sumDiffer += sr.differ;
                if (haveRb && rb.w == w && rb.h == h) {
                    const Score sg = score(rebuilt, false, next, is_bgra(nc.fmt), haveNm ? nextMask : std::vector<uint8_t>(), nm.w, nm.h, w, h);
                    printf(" | the game's own rebuild: mean %.4f, hand region %.4f", sg.mean, sg.hand);
                    sumGame += sg.mean; sumHandGame += sg.hand;
                }
                printf("\n   (only STILL frames compare cleanly: head motion between the two presents is real difference, not error)\n");
                ++scored;
            }
        } else if (!replay.empty()) {
            printf("   no next native frame of eye %d to score against\n", held);
        }
        rel(sfc); rel(sfd); rel(sfm); rel(shc); rel(shd); rel(shm);
        rel(tfc); rel(tfd); rel(tfm); rel(thc); rel(thd); rel(thm);
    }
    if (scored)
        printf("\nover %d scored presents: replay mean %.4f hand %.4f differ %.2f%% | the game's rebuild mean %.4f hand %.4f\n", scored,
               sumReplay / scored, sumHandReplay / scored, 100 * sumDiffer / scored, sumGame / scored, sumHandGame / scored);
    printf("pictures in %s (BMP: *_fresh, *_held, *_rebuilt_ingame, *_replay, *_depth, *_mask)\n", out.c_str());
    warp.shutdown();
    rel(g_ctx); rel(g_dev);
    // A zero exit now means the whole replay succeeded, so -Delete cannot erase a failed run.
    const bool complete = replayed == n && !g_pictureWriteFailed;
    printf("replayed %d/%d presents; picture writes %s\n", replayed, n, g_pictureWriteFailed ? "FAILED" : "ok");
    return complete ? 0 : 1;
}
