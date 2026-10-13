#pragma once
// Shared by the GPU host suites: a D3D11 device, texture helpers and a ray-traced
// synthetic scene in BioShock's own conventions (UE2.5 axes: X forward, Y right, Z up,
// uu), so a suite can render the exact images, depth and per-pixel truth the production
// passes are supposed to reproduce. The camera basis is the production one
// (game/shared/ue_math.h ue_rot_basis), so a convention error there is caught too.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <d3d11.h>
#include <d3dcompiler.h>

#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "game/shared/ue_math.h"

namespace gt {

inline int g_fail = 0, g_pass = 0;
inline void check(bool ok, const char* name, const char* fmt, ...) {
    char detail[512];
    va_list a; va_start(a, fmt);
    _vsnprintf_s(detail, sizeof(detail), _TRUNCATE, fmt, a);
    va_end(a);
    printf("  [%s] %s: %s\n", ok ? "PASS" : "FAIL", name, detail);
    if (ok) ++g_pass; else ++g_fail;
}

inline ID3D11Device* dev = nullptr;
inline ID3D11DeviceContext* ctx = nullptr;

inline bool make_device() {
    D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_1;
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &fl, 1, D3D11_SDK_VERSION, &dev, nullptr, &ctx);
    if (FAILED(hr)) {
        fl = D3D_FEATURE_LEVEL_11_0;
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &fl, 1, D3D11_SDK_VERSION, &dev, nullptr, &ctx);
    }
    if (FAILED(hr)) { printf("no D3D11 hardware device (0x%08lX)\n", (unsigned long)hr); return false; }
    return true;
}

template <class T> void rel(T*& p) { if (p) { p->Release(); p = nullptr; } }

inline ID3D11Texture2D* make_tex(uint32_t w, uint32_t h, DXGI_FORMAT f, UINT bind, const void* data = nullptr, uint32_t pitch = 0) {
    D3D11_TEXTURE2D_DESC d = {};
    d.Width = w; d.Height = h; d.MipLevels = 1; d.ArraySize = 1; d.Format = f;
    d.SampleDesc.Count = 1; d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = bind;
    D3D11_SUBRESOURCE_DATA sd = {data, pitch, 0};
    ID3D11Texture2D* t = nullptr;
    if (FAILED(dev->CreateTexture2D(&d, data ? &sd : nullptr, &t))) return nullptr;
    return t;
}
inline ID3D11ShaderResourceView* srv_of(ID3D11Texture2D* t, DXGI_FORMAT f = DXGI_FORMAT_UNKNOWN) {
    ID3D11ShaderResourceView* s = nullptr;
    if (f == DXGI_FORMAT_UNKNOWN) { dev->CreateShaderResourceView(t, nullptr, &s); return s; }
    D3D11_SHADER_RESOURCE_VIEW_DESC d = {};
    d.Format = f; d.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; d.Texture2D.MipLevels = 1;
    dev->CreateShaderResourceView(t, &d, &s);
    return s;
}

inline float half_to_float(uint16_t h) {
    const uint32_t s = (h >> 15) & 1, e = (h >> 10) & 31, m = h & 1023;
    float v;
    if (e == 0) v = std::ldexp((float)m, -24);
    else if (e == 31) v = m ? NAN : INFINITY;
    else v = std::ldexp((float)(m | 1024), (int)e - 25);
    return s ? -v : v;
}

// Readback of a texture as floats (R32F: 1 per texel, R16G16F: 2, RGBA8: 4 in 0..1).
inline std::vector<float> readback(ID3D11Texture2D* src) {
    D3D11_TEXTURE2D_DESC d = {};
    src->GetDesc(&d);
    D3D11_TEXTURE2D_DESC s = d;
    s.Usage = D3D11_USAGE_STAGING; s.BindFlags = 0; s.CPUAccessFlags = D3D11_CPU_ACCESS_READ; s.MiscFlags = 0;
    ID3D11Texture2D* st = nullptr;
    std::vector<float> out;
    if (FAILED(dev->CreateTexture2D(&s, nullptr, &st))) return out;
    ctx->CopyResource(st, src);
    D3D11_MAPPED_SUBRESOURCE m = {};
    if (FAILED(ctx->Map(st, 0, D3D11_MAP_READ, 0, &m))) { st->Release(); return out; }
    const int ch = d.Format == DXGI_FORMAT_R16G16_FLOAT ? 2
                 : (d.Format == DXGI_FORMAT_R32_FLOAT || d.Format == DXGI_FORMAT_R8_UNORM) ? 1 : 4;
    out.resize((size_t)d.Width * d.Height * ch);
    for (uint32_t y = 0; y < d.Height; ++y) {
        const uint8_t* row = (const uint8_t*)m.pData + (size_t)y * m.RowPitch;
        for (uint32_t x = 0; x < d.Width; ++x) {
            float* o = &out[((size_t)y * d.Width + x) * ch];
            if (d.Format == DXGI_FORMAT_R32_FLOAT) o[0] = ((const float*)row)[x];
            else if (d.Format == DXGI_FORMAT_R8_UNORM) o[0] = row[x] / 255.0f;
            else if (d.Format == DXGI_FORMAT_R16G16_FLOAT) {
                o[0] = half_to_float(((const uint16_t*)row)[x * 2]);
                o[1] = half_to_float(((const uint16_t*)row)[x * 2 + 1]);
            } else {
                for (int c = 0; c < 4; ++c) o[c] = row[x * 4 + c] / 255.0f;
            }
        }
    }
    ctx->Unmap(st, 0);
    st->Release();
    return out;
}

// ---- the scene --------------------------------------------------------------
struct Box { double lo[3], hi[3]; int id; };

// A cam in production terms: location + FRotator, and the symmetric tangents.
struct Cam {
    double loc[3] = {0, 0, 0};
    bvr::ue::FRotator rot{0, 0, 0};
    double tanH = 1.0, tanV = 1.0;
    void basis(double f[3], double r[3], double u[3]) const {
        float ff[3], rr[3], uu[3];
        bvr::ue::ue_rot_basis(rot, ff, rr, uu);
        for (int i = 0; i < 3; ++i) { f[i] = ff[i]; r[i] = rr[i]; u[i] = uu[i]; }
    }
};

struct Hit { bool ok = false; double t = 0; double p[3] = {}; int id = -1; };

struct Scene {
    Box room{{-400, -400, -100}, {400, 400, 200}, 0};   // seen from inside
    std::vector<Box> solids;                            // seen from outside
    // A moving object: solids[movingIndex] is shifted by `objShift` uu (0 = none).
    int movingIndex = -1;
    double objShift[3] = {0, 0, 0};
    // Fine stripes (period ~3 uu) on top of the smooth pattern: a few pixels at the test
    // sizes, so a single sample per pixel aliases and temporal accumulation has work to do.
    double detail = 0.0;

    Hit cast(const double o[3], const double d[3]) const {
        Hit h;
        // Room: exit distance.
        double tExit = 1e30;
        for (int a = 0; a < 3; ++a) {
            if (std::fabs(d[a]) < 1e-12) continue;
            const double t = ((d[a] > 0 ? room.hi[a] : room.lo[a]) - o[a]) / d[a];
            if (t > 0 && t < tExit) tExit = t;
        }
        h.ok = tExit < 1e29; h.t = tExit; h.id = 0;
        for (size_t i = 0; i < solids.size(); ++i) {
            Box b = solids[i];
            if ((int)i == movingIndex)
                for (int a = 0; a < 3; ++a) { b.lo[a] += objShift[a]; b.hi[a] += objShift[a]; }
            double t0 = -1e30, t1 = 1e30;
            bool miss = false;
            for (int a = 0; a < 3; ++a) {
                if (std::fabs(d[a]) < 1e-12) {
                    if (o[a] < b.lo[a] || o[a] > b.hi[a]) { miss = true; break; }
                    continue;
                }
                double ta = (b.lo[a] - o[a]) / d[a], tb = (b.hi[a] - o[a]) / d[a];
                if (ta > tb) { const double x = ta; ta = tb; tb = x; }
                if (ta > t0) t0 = ta;
                if (tb < t1) t1 = tb;
            }
            if (miss || t0 > t1 || t0 <= 1e-6) continue;
            if (t0 < h.t) { h.t = t0; h.id = b.id; }
        }
        for (int a = 0; a < 3; ++a) h.p[a] = o[a] + d[a] * h.t;
        return h;
    }
};

// A smooth but detailed surface pattern keyed on world position (and surface id), so two
// views of one point have the same colour and a bilinear sample is meaningful.
inline void shade(const Hit& h, const Scene& s, float rgb[3]) {
    double p[3] = {h.p[0], h.p[1], h.p[2]};
    if (h.id >= 0 && s.movingIndex >= 0 && h.id == s.solids[s.movingIndex].id)
        for (int a = 0; a < 3; ++a) p[a] -= s.objShift[a];   // the pattern moves with the object
    const double k = 0.06;
    const double a = std::sin(p[0] * k * 1.3 + 0.7 * h.id) * std::sin(p[1] * k * 1.1) ;
    const double b = std::sin(p[2] * k * 1.7 + p[0] * k * 0.4);
    const double c = std::sin((p[0] + p[1] + p[2]) * k * 0.5 + h.id);
    const double fine = s.detail != 0.0 ? s.detail * std::sin(p[0] * 2.1 + p[1] * 1.7) * std::sin(p[2] * 1.9 - p[1] * 0.8) : 0.0;
    rgb[0] = (float)(0.5 + 0.22 * a + 0.18 * b + fine);
    rgb[1] = (float)(0.5 + 0.2 * b + 0.15 * c + fine);
    rgb[2] = (float)(0.5 + 0.25 * c - 0.1 * a + fine);
}

inline void pixel_dir(const Cam& c, uint32_t w, uint32_t h, double px, double py, double d[3]) {
    double f[3], r[3], u[3];
    c.basis(f, r, u);
    const double x = (px / w) * 2.0 - 1.0, y = 1.0 - (py / h) * 2.0;
    for (int i = 0; i < 3; ++i) d[i] = f[i] + r[i] * x * c.tanH + u[i] * y * c.tanV;
}

// Conventional D3D device depth (0 near, 1 far) for a view depth; far 0 = infinite.
inline float device_depth(double viewZ, double n, double f) {
    if (f > n) return (float)((f / (f - n)) * (1.0 - n / viewZ));
    return (float)(1.0 - n / viewZ);
}

struct Render {
    std::vector<uint32_t> rgba;    // RGBA8
    std::vector<float> raw;        // device depth
    std::vector<float> viewZ;      // truth
    std::vector<double> world;     // 3 per pixel
};

// Supersampled colour (ss x ss per pixel, box filter) so edges are antialiased like a real
// render's; depth and world from the pixel centre.
inline Render render(const Scene& s, const Cam& c, uint32_t w, uint32_t h, double n, double f, int ss = 2,
                     double jx = 0.0, double jy = 0.0) {
    Render r;
    r.rgba.resize((size_t)w * h); r.raw.resize((size_t)w * h); r.viewZ.resize((size_t)w * h); r.world.resize((size_t)w * h * 3);
    for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w; ++x) {
            float acc[3] = {0, 0, 0};
            for (int sy = 0; sy < ss; ++sy)
                for (int sx = 0; sx < ss; ++sx) {
                    double d[3];
                    pixel_dir(c, w, h, x + (sx + 0.5) / ss + jx, y + (sy + 0.5) / ss + jy, d);
                    const Hit hit = s.cast(c.loc, d);
                    float rgb[3]; shade(hit, s, rgb);
                    for (int i = 0; i < 3; ++i) acc[i] += rgb[i];
                }
            uint32_t px = 0;
            for (int i = 0; i < 3; ++i) {
                const float v = acc[i] / (ss * ss);
                px |= (uint32_t)(std::fmin(std::fmax(v, 0.0f), 1.0f) * 255.0f + 0.5f) << (8 * i);
            }
            r.rgba[(size_t)y * w + x] = px | 0xFF000000u;
            double d[3];
            pixel_dir(c, w, h, x + 0.5 + jx, y + 0.5 + jy, d);
            const Hit hit = s.cast(c.loc, d);
            r.viewZ[(size_t)y * w + x] = (float)hit.t;
            r.raw[(size_t)y * w + x] = device_depth(hit.t, n, f);
            for (int i = 0; i < 3; ++i) r.world[((size_t)y * w + x) * 3 + i] = hit.p[i];
        }
    return r;
}

// Where a world point lands in a camera's image (UV); false behind it.
inline bool project(const Cam& c, const double p[3], double* u, double* v, double* z = nullptr) {
    double f[3], r[3], up[3];
    c.basis(f, r, up);
    double q[3] = {p[0] - c.loc[0], p[1] - c.loc[1], p[2] - c.loc[2]};
    const double zz = q[0] * f[0] + q[1] * f[1] + q[2] * f[2];
    if (zz <= 1e-6) return false;
    *u = 0.5 + 0.5 * (q[0] * r[0] + q[1] * r[1] + q[2] * r[2]) / (zz * c.tanH);
    *v = 0.5 - 0.5 * (q[0] * up[0] + q[1] * up[1] + q[2] * up[2]) / (zz * c.tanV);
    if (z) *z = zz;
    return true;
}

inline double lum(uint32_t px) {
    return (0.299 * (px & 255) + 0.587 * ((px >> 8) & 255) + 0.114 * ((px >> 16) & 255)) / 255.0;
}

inline int finish(const char* suite) {
    printf("%s: %d passed, %d failed\n", suite, g_pass, g_fail);
    return g_fail ? 1 : 0;
}

} // namespace gt
