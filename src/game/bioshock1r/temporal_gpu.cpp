// See temporal_gpu.h.
#include "game/bioshock1r/temporal_gpu.h"

#include <d3d11.h>
#include <d3dcompiler.h>

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>

namespace bvr::b1r::tgpu {
namespace {

template <class T> void rel(T*& p) { if (p) { p->Release(); p = nullptr; } }
void put(char* why, size_t cap, const char* fmt, ...) {
    if (!why || !cap) return;
    va_list a; va_start(a, fmt);
    _vsnprintf_s(why, cap, _TRUNCATE, fmt, a);
    va_end(a);
}

// Shared by both shaders: the constant buffer and the reprojection, so the audit's
// "scale 1" curve and the production vectors cannot disagree on the algebra.
const char* kCommon = R"(
cbuffer P : register(b0) {
    float4 curLocNear;      // xyz loc, w near (uu)
    float4 curFwdTanH;
    float4 curRightTanV;
    float4 curUpHist;       // w: history valid
    float4 prevLocFar;      // w: far (uu, 0 = infinite)
    float4 prevFwdTanH;
    float4 prevRightTanV;
    float4 prevUpBody;      // w: body band (uu)
    float4 sizeFlags;       // w, h, depth reversed, foreground depth bound
};

float view_z(float raw) {
    if (sizeFlags.z > 0.5f) raw = 1.0f - raw;
    raw = saturate(raw);
    float n = max(curLocNear.w, 0.0001f), f = prevLocFar.w;
    if (raw >= 1.0f) return 0.0f;   // cleared: sky / nothing drawn
    if (f > n) return n * f / max(f - raw * (f - n), 0.0001f);
    return n / max(1.0f - raw, 0.0000001f);
}

float3 cur_dir(float2 uv) {
    float x = uv.x * 2.0f - 1.0f, y = 1.0f - uv.y * 2.0f;
    return curFwdTanH.xyz + curRightTanV.xyz * (x * curFwdTanH.w) + curUpHist.xyz * (y * curRightTanV.w);
}

// v: the point relative to the previous camera (or a direction for rotation-only).
bool prev_uv(float3 v, out float2 puv) {
    float z = dot(v, prevFwdTanH.xyz);
    puv = float2(0, 0);
    if (z <= 1e-4f) return false;
    float x = dot(v, prevRightTanV.xyz) / (z * prevFwdTanH.w);
    float y = dot(v, prevUpBody.xyz) / (z * prevRightTanV.w);
    puv = float2(x * 0.5f + 0.5f, 0.5f - y * 0.5f);
    return true;
}

// The vector of one pixel, scale = depth multiplier (1 = production).
float2 vector_at(float2 uv, float vz, float scale, bool bodyRule) {
    float3 d = cur_dir(uv);
    float2 puv;
    bool rotOnly = vz <= 0.0f || (bodyRule && vz < prevUpBody.w);
    if (rotOnly) {
        if (!prev_uv(d, puv)) return float2(0, 0);
    } else {
        float3 world = curLocNear.xyz + d * (vz * scale);
        if (!prev_uv(world - prevLocFar.xyz, puv)) {
            if (!prev_uv(d, puv)) return float2(0, 0);
        }
    }
    return puv - uv;
}
)";

const char* kMain = R"(
Texture2D<float> rawDepth : register(t0);
Texture2D<float> fgDepth : register(t1);
RWTexture2D<float> dlssDepth : register(u0);
RWTexture2D<float> linearZ : register(u1);
RWTexture2D<float2> motion : register(u2);
RWTexture2D<unorm float> hands : register(u3);

[numthreads(8, 8, 1)]
void csmain(uint3 id : SV_DispatchThreadID) {
    uint w = (uint)sizeFlags.x, h = (uint)sizeFlags.y;
    if (id.x >= w || id.y >= h) return;
    float raw = rawDepth.Load(int3(id.xy, 0));
    // The foreground pass (arms and weapon): where it left depth, that depth is the
    // surface the eye sees, and the pixel belongs to the hands.
    float hand = 0.0f;
    if (sizeFlags.w > 0.5f) {
        float fr = fgDepth.Load(int3(id.xy, 0));
        float fc = sizeFlags.z > 0.5f ? 1.0f - fr : fr;
        if (fc < 1.0f) { raw = fr; hand = 1.0f; }
    }
    hands[id.xy] = hand;
    float conv = sizeFlags.z > 0.5f ? 1.0f - raw : raw;
    dlssDepth[id.xy] = 1.0f - saturate(conv);
    float vz = view_z(raw);
    linearZ[id.xy] = vz;
    if (curUpHist.w < 0.5f) { motion[id.xy] = float2(0, 0); return; }
    float2 uv = (float2(id.xy) + 0.5f) / float2(w, h);
    motion[id.xy] = vector_at(uv, vz, 1.0f, true);
}
)";

const char* kAudit = R"(
Texture2D<float4> cur : register(t0);
Texture2D<float4> prev : register(t1);
Texture2D<float> linZ : register(t2);
Texture2D<float2> mv : register(t3);
SamplerState lin : register(s0);
RWByteAddressBuffer acc : register(u0);   // [count, zero, rot, vec, s0..s4]

static const float kScales[5] = {0.25f, 0.5f, 1.0f, 2.0f, 4.0f};

float lum(float3 c) { return dot(c, float3(0.299f, 0.587f, 0.114f)); }
uint fx(float e) { return (uint)(saturate(e) * 65535.0f + 0.5f); }

[numthreads(8, 8, 1)]
void csaudit(uint3 id : SV_DispatchThreadID) {
    // A 64 x 48 grid inside a 6% border.
    if (id.x >= 64 || id.y >= 48) return;
    float2 uv = float2(0.06f + 0.88f * (id.x + 0.5f) / 64.0f, 0.06f + 0.88f * (id.y + 0.5f) / 48.0f);
    uint w = (uint)sizeFlags.x, h = (uint)sizeFlags.y;
    int2 px = int2(uv * float2(w, h));
    float vz = linZ.Load(int3(px, 0));
    float2 v = mv.Load(int3(px, 0));
    float2 puv = uv + v;
    if (any(puv < 0.01f) || any(puv > 0.99f)) return;
    float lc = lum(cur.Load(int3(px, 0)).rgb);
    float zero = abs(lc - lum(prev.SampleLevel(lin, uv, 0).rgb));
    float rot = abs(lc - lum(prev.SampleLevel(lin, uv + vector_at(uv, 0.0f, 1.0f, false), 0).rgb));
    float vec = abs(lc - lum(prev.SampleLevel(lin, puv, 0).rgb));
    acc.InterlockedAdd(0, 1);
    acc.InterlockedAdd(4, fx(zero));
    acc.InterlockedAdd(8, fx(rot));
    acc.InterlockedAdd(12, fx(vec));
    [unroll] for (int s = 0; s < 5; ++s) {
        float2 suv = uv + vector_at(uv, vz, kScales[s], false);
        acc.InterlockedAdd(16 + 4 * s, fx(abs(lc - lum(prev.SampleLevel(lin, suv, 0).rgb))));
    }
}
)";

const char* kTaa = R"(
Texture2D<float4> cur : register(t0);
Texture2D<float4> hist : register(t1);
Texture2D<float2> mv : register(t2);
Texture2D<float> hands : register(t3);
SamplerState lin : register(s0);
RWTexture2D<unorm float4> outp : register(u0);
cbuffer T : register(b0) {
    float4 p;   // w, h, history valid, current weight
    float4 q;   // hands bound
};
float3 ycocg(float3 c) { return float3(0.25f * c.r + 0.5f * c.g + 0.25f * c.b, 0.5f * c.r - 0.5f * c.b,
                                       -0.25f * c.r + 0.5f * c.g - 0.25f * c.b); }
float3 rgb(float3 y) { float t = y.x - y.z; return float3(t + y.y, y.x + y.z, t - y.y); }

[numthreads(8, 8, 1)]
void cstaa(uint3 id : SV_DispatchThreadID) {
    int2 sz = int2(p.xy);
    if (id.x >= (uint)sz.x || id.y >= (uint)sz.y) return;
    int2 px = int2(id.xy);
    float4 c = cur.Load(int3(px, 0));
    float2 uv = (float2(px) + 0.5f) / float2(sz);
    float2 puv = uv + mv.Load(int3(px, 0));
    bool useHist = p.z > 0.5f && all(puv >= 0.0f) && all(puv <= 1.0f);
    if (useHist && q.x > 0.5f && hands.Load(int3(px, 0)) > 0.5f) useHist = false;
    if (!useHist) { outp[px] = float4(c.rgb, 1.0f); return; }
    float3 mn = float3(1e9f, 1e9f, 1e9f), mx = -mn;
    [unroll] for (int dy = -1; dy <= 1; ++dy)
        [unroll] for (int dx = -1; dx <= 1; ++dx) {
            float3 s = ycocg(cur.Load(int3(clamp(px + int2(dx, dy), int2(0, 0), sz - 1), 0)).rgb);
            mn = min(mn, s); mx = max(mx, s);
        }
    float3 h = clamp(ycocg(hist.SampleLevel(lin, puv, 0).rgb), mn, mx);
    outp[px] = float4(saturate(rgb(lerp(h, ycocg(c.rgb), p.w))), 1.0f);
}
)";

struct alignas(16) CB {
    float curLocNear[4], curFwdTanH[4], curRightTanV[4], curUpHist[4];
    float prevLocFar[4], prevFwdTanH[4], prevRightTanV[4], prevUpBody[4];
    float sizeFlags[4];
};
static_assert(sizeof(CB) == 9 * 16, "CB layout");

void fill(CB& cb, const Params& p) {
    auto set = [](float* d, const float* s, float w) { d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = w; };
    set(cb.curLocNear, p.cur.loc, p.nearUu);
    set(cb.curFwdTanH, p.cur.fwd, p.cur.tanH);
    set(cb.curRightTanV, p.cur.right, p.cur.tanV);
    set(cb.curUpHist, p.cur.up, p.historyValid ? 1.0f : 0.0f);
    set(cb.prevLocFar, p.prev.loc, p.farUu);
    set(cb.prevFwdTanH, p.prev.fwd, p.prev.tanH);
    set(cb.prevRightTanV, p.prev.right, p.prev.tanV);
    set(cb.prevUpBody, p.prev.up, p.bodyUu);
    cb.sizeFlags[0] = (float)p.w; cb.sizeFlags[1] = (float)p.h;
    cb.sizeFlags[2] = p.depthReversed ? 1.0f : 0.0f; cb.sizeFlags[3] = p.haveForeground ? 1.0f : 0.0f;
}

bool compile(ID3D11Device* dev, const char* body, const char* entry, ID3D11ComputeShader** out, char* why, size_t cap,
             bool withCommon = true) {
    std::string src = withCommon ? std::string(kCommon) + body : std::string(body);
    ID3DBlob* code = nullptr; ID3DBlob* err = nullptr;
    HRESULT hr = D3DCompile(src.data(), src.size(), "b1r_temporal_gpu", nullptr, nullptr, entry, "cs_5_0",
                            D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &err);
    if (FAILED(hr)) {
        put(why, cap, "%s compile: %s", entry, err ? (const char*)err->GetBufferPointer() : "?");
        rel(err); rel(code); return false;
    }
    rel(err);
    hr = dev->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, out);
    rel(code);
    if (FAILED(hr)) { put(why, cap, "%s create 0x%08lX", entry, (unsigned long)hr); return false; }
    return true;
}

double dot3(const float* a, const double* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

// Saves and restores exactly the compute state these passes touch.
struct SavedCs {
    ID3D11DeviceContext* ctx;
    ID3D11ComputeShader* cs = nullptr;
    ID3D11Buffer* cb = nullptr;
    ID3D11ShaderResourceView* srv[4] = {};
    ID3D11UnorderedAccessView* uav[4] = {};
    ID3D11SamplerState* smp = nullptr;
    explicit SavedCs(ID3D11DeviceContext* c) : ctx(c) {
        ctx->CSGetShader(&cs, nullptr, nullptr);
        ctx->CSGetConstantBuffers(0, 1, &cb);
        ctx->CSGetShaderResources(0, 4, srv);
        ctx->CSGetUnorderedAccessViews(0, 4, uav);
        ctx->CSGetSamplers(0, 1, &smp);
    }
    ~SavedCs() {
        ID3D11ShaderResourceView* nsrv[4] = {};
        ID3D11UnorderedAccessView* nuav[4] = {};
        ctx->CSSetShaderResources(0, 4, nsrv);
        ctx->CSSetUnorderedAccessViews(0, 4, nuav, nullptr);
        ctx->CSSetShader(cs, nullptr, 0);
        ctx->CSSetConstantBuffers(0, 1, &cb);
        ctx->CSSetShaderResources(0, 4, srv);
        ctx->CSSetUnorderedAccessViews(0, 4, uav, nullptr);
        ctx->CSSetSamplers(0, 1, &smp);
        rel(cs); rel(cb); rel(smp);
        for (auto*& s : srv) rel(s);
        for (auto*& u : uav) rel(u);
    }
};

// The hand-depth probe. One thread per sample (every 6th pixel each way): the left pixel's
// world point, its predicted place in the right image, then a 3x3-tap (5x5 px) luminance block
// match over ratios 0.70..1.30 of the predicted disparity. The best ratio is binned for the
// pixel's band, unless it is at the end of the range (no minimum inside) or the left patch is
// flat (no texture to match).
const char* kProbe = R"(
cbuffer P : register(b0) {
    float4 lLocTanH;
    float4 lFwdTanV;
    float4 lRight;
    float4 lUp;
    float4 rLocTanH;
    float4 rFwdTanV;
    float4 rRight;
    float4 rUp;
    float4 dims;    // w, h, stride, hand band (uu)
    float4 band;    // world band min, max (uu), ratio min, ratio step
};
Texture2D<float4> colL : register(t0);
Texture2D<float> lzL : register(t1);
Texture2D<float4> colR : register(t2);
SamplerState lin : register(s0);
RWByteAddressBuffer hist : register(u0);

float lum(float3 c) { return dot(c, float3(0.299f, 0.587f, 0.114f)); }

[numthreads(8, 8, 1)]
void csprobe(uint3 id : SV_DispatchThreadID) {
    const int s = (int)dims.z;
    const int2 p = int2(id.xy) * s + s / 2;
    if (p.x < 8 || p.y < 8 || p.x >= (int)dims.x - 8 || p.y >= (int)dims.y - 8) return;
    const float z = lzL.Load(int3(p, 0));
    const int b = (z > 0.0f && z < dims.w) ? 0 : (z > band.x && z < band.y) ? 1 : -1;
    if (b < 0) return;
    if (b == 1 && ((id.x + id.y) & 3) != 0) return;   // the control needs fewer samples
    const float2 uv = (float2(p) + 0.5f) / dims.xy;
    const float x = uv.x * 2.0f - 1.0f, y = 1.0f - uv.y * 2.0f;
    const float3 d = lFwdTanV.xyz + lRight.xyz * (x * lLocTanH.w) + lUp.xyz * (y * lFwdTanV.w);
    const float3 q = lLocTanH.xyz + d * z - rLocTanH.xyz;
    const float rz = dot(q, rFwdTanV.xyz);
    if (rz < 1.0f) return;
    const float2 ruv = float2(dot(q, rRight.xyz) / (rz * rLocTanH.w) * 0.5f + 0.5f,
                              0.5f - dot(q, rUp.xyz) / (rz * rFwdTanV.w) * 0.5f);
    const float2 dpx = (ruv - uv) * dims.xy;   // the predicted disparity, pixels
    if (abs(dpx.x) < 6.0f) return;
    float lp[9];
    float mean = 0.0f;
    [unroll] for (int t = 0; t < 9; ++t) {
        lp[t] = lum(colL.Load(int3(p + int2(t % 3 - 1, t / 3 - 1) * 2, 0)).rgb);
        mean += lp[t];
    }
    mean /= 9.0f;
    float var = 0.0f;
    [unroll] for (int v = 0; v < 9; ++v) var += (lp[v] - mean) * (lp[v] - mean);
    if (var < 9.0f * 0.0004f) return;   // flat: no texture to match
    float best = 1e9f;
    int bi = -1;
    [loop] for (int k = 0; k < 61; ++k) {
        const float2 c = float2(p) + 0.5f + dpx * (band.z + band.w * k);
        float sad = 0.0f;
        [unroll] for (int t2 = 0; t2 < 9; ++t2) {
            const float2 o = float2(t2 % 3 - 1, t2 / 3 - 1) * 2.0f;
            sad += abs(lum(colR.SampleLevel(lin, (c + o) / dims.xy, 0).rgb) - lp[t2]);
        }
        if (sad < best) { best = sad; bi = k; }
    }
    if (bi <= 0 || bi >= 60) return;
    hist.InterlockedAdd((uint)(b * 61 + bi) * 4u, 1u);
}
)";

struct alignas(16) ProbeCB {
    float lLocTanH[4], lFwdTanV[4], lRight[4], lUp[4];
    float rLocTanH[4], rFwdTanV[4], rRight[4], rUp[4];
    float dims[4], band[4];
};

} // namespace

double ProbeResult::median(int b, uint64_t* n) const {
    uint64_t total = 0;
    for (int i = 0; i < kProbeBins; ++i) total += hist[b & 1][i];
    if (n) *n = total;
    if (!total) return 0.0;
    uint64_t acc = 0;
    for (int i = 0; i < kProbeBins; ++i) {
        acc += hist[b & 1][i];
        if (acc * 2 >= total) return kProbeMin + kProbeStep * i;
    }
    return 0.0;
}

void DepthProbe::collect(ID3D11DeviceContext* ctx) {
    for (int i = 0; i < 3; ++i) {
        if (!pending_[i]) continue;
        D3D11_MAPPED_SUBRESOURCE m = {};
        if (ctx->Map(stage_[i], 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m) != S_OK) continue;
        const uint32_t* h = (const uint32_t*)m.pData;
        for (int b = 0; b < 2; ++b)
            for (int k = 0; k < kProbeBins; ++k) {
                result.hist[b][k] += h[b * kProbeBins + k];
                lifetime.hist[b][k] += h[b * kProbeBins + k];
            }
        ++result.pairs;
        ++lifetime.pairs;
        ctx->Unmap(stage_[i], 0);
        pending_[i] = false;
    }
}

bool DepthProbe::run(ID3D11Device* dev, ID3D11DeviceContext* ctx, ID3D11ShaderResourceView* colLeft,
                     ID3D11ShaderResourceView* lzLeft, ID3D11ShaderResourceView* colRight, const Camera& left,
                     const Camera& right, uint32_t w, uint32_t h, float nearBandUu, char* why, size_t cap) {
    if (!ready_) {
        if (!compile(dev, kProbe, "csprobe", &cs_, why, cap, false)) return false;
        D3D11_BUFFER_DESC bd = {};
        bd.ByteWidth = sizeof(ProbeCB); bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER; bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        D3D11_SAMPLER_DESC sd = {};
        sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.MaxLOD = D3D11_FLOAT32_MAX;
        D3D11_BUFFER_DESC hb = {};
        hb.ByteWidth = 2 * kProbeBins * 4; hb.Usage = D3D11_USAGE_DEFAULT;
        hb.BindFlags = D3D11_BIND_UNORDERED_ACCESS; hb.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
        D3D11_UNORDERED_ACCESS_VIEW_DESC ud = {};
        ud.Format = DXGI_FORMAT_R32_TYPELESS; ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
        ud.Buffer.NumElements = 2 * kProbeBins; ud.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
        D3D11_BUFFER_DESC sb = {};
        sb.ByteWidth = hb.ByteWidth; sb.Usage = D3D11_USAGE_STAGING; sb.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        bool ok = SUCCEEDED(dev->CreateBuffer(&bd, nullptr, &cb_)) && SUCCEEDED(dev->CreateSamplerState(&sd, &linear_)) &&
                  SUCCEEDED(dev->CreateBuffer(&hb, nullptr, &buf_)) && SUCCEEDED(dev->CreateUnorderedAccessView(buf_, &ud, &uav_));
        for (auto*& s : stage_) ok = ok && SUCCEEDED(dev->CreateBuffer(&sb, nullptr, &s));
        if (!ok) { put(why, cap, "probe resources"); shutdown(); return false; }
        ready_ = true;
    }
    collect(ctx);
    const int slot = next_;
    if (pending_[slot]) return true;   // all three in flight: skip this pair
    next_ = (next_ + 1) % 3;
    ProbeCB cb = {};
    auto set = [](float* d, const float* s, float wv) { d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = wv; };
    set(cb.lLocTanH, left.loc, left.tanH); set(cb.lFwdTanV, left.fwd, left.tanV);
    set(cb.lRight, left.right, 0); set(cb.lUp, left.up, 0);
    set(cb.rLocTanH, right.loc, right.tanH); set(cb.rFwdTanV, right.fwd, right.tanV);
    set(cb.rRight, right.right, 0); set(cb.rUp, right.up, 0);
    const uint32_t stride = 6;
    cb.dims[0] = (float)w; cb.dims[1] = (float)h; cb.dims[2] = (float)stride; cb.dims[3] = nearBandUu;
    cb.band[0] = 150.0f; cb.band[1] = 1500.0f; cb.band[2] = kProbeMin; cb.band[3] = kProbeStep;
    D3D11_MAPPED_SUBRESOURCE m = {};
    if (FAILED(ctx->Map(cb_, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) { put(why, cap, "probe map"); return false; }
    memcpy(m.pData, &cb, sizeof(cb));
    ctx->Unmap(cb_, 0);
    {
        SavedCs saved(ctx);
        const UINT zero[4] = {};
        ctx->ClearUnorderedAccessViewUint(uav_, zero);
        ID3D11ShaderResourceView* srv[3] = {colLeft, lzLeft, colRight};
        ctx->CSSetShader(cs_, nullptr, 0);
        ctx->CSSetConstantBuffers(0, 1, &cb_);
        ctx->CSSetShaderResources(0, 3, srv);
        ctx->CSSetUnorderedAccessViews(0, 1, &uav_, nullptr);
        ctx->CSSetSamplers(0, 1, &linear_);
        const uint32_t sx = w / stride, sy = h / stride;
        ctx->Dispatch((sx + 7) / 8, (sy + 7) / 8, 1);
    }
    ctx->CopyResource(stage_[slot], buf_);
    pending_[slot] = true;
    return true;
}

void DepthProbe::shutdown() {
    rel(cs_); rel(cb_); rel(linear_); rel(uav_); rel(buf_);
    for (auto*& s : stage_) rel(s);
    for (bool& b : pending_) b = false;
    ready_ = false;
}

float view_z(const Params& p, float raw) {
    if (p.depthReversed) raw = 1.0f - raw;
    raw = raw < 0 ? 0 : raw > 1 ? 1 : raw;
    const double n = p.nearUu > 0.0001f ? p.nearUu : 0.0001, f = p.farUu;
    if (raw >= 1.0f) return 0.0f;
    if (f > n) return (float)(n * f / (f - raw * (f - n)));
    return (float)(n / (1.0 - raw));
}

bool reproject(const Params& p, float u, float v, float viewZ, float* pu, float* pv) {
    const double x = u * 2.0 - 1.0, y = 1.0 - v * 2.0;
    double d[3], q[3];
    for (int i = 0; i < 3; ++i)
        d[i] = p.cur.fwd[i] + p.cur.right[i] * x * p.cur.tanH + p.cur.up[i] * y * p.cur.tanV;
    for (int i = 0; i < 3; ++i) q[i] = p.cur.loc[i] + d[i] * viewZ - p.prev.loc[i];
    const double z = dot3(p.prev.fwd, q);
    if (z <= 1e-4) return false;
    *pu = (float)(0.5 + 0.5 * dot3(p.prev.right, q) / (z * p.prev.tanH));
    *pv = (float)(0.5 - 0.5 * dot3(p.prev.up, q) / (z * p.prev.tanV));
    return true;
}

bool TemporalGpu::init(ID3D11Device* dev, char* why, size_t cap) {
    if (ready_) return true;
    if (!compile(dev, kMain, "csmain", &cs_, why, cap)) return false;
    if (!compile(dev, kAudit, "csaudit", &csAudit_, why, cap)) { rel(cs_); return false; }
    D3D11_BUFFER_DESC bd = {};
    bd.ByteWidth = sizeof(CB);
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(dev->CreateBuffer(&bd, nullptr, &cb_))) { put(why, cap, "constant buffer"); shutdown(); return false; }
    D3D11_BUFFER_DESC ab = {};
    ab.ByteWidth = 64;
    ab.Usage = D3D11_USAGE_DEFAULT;
    ab.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
    ab.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
    D3D11_UNORDERED_ACCESS_VIEW_DESC ud = {};
    ud.Format = DXGI_FORMAT_R32_TYPELESS;
    ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    ud.Buffer.NumElements = 16;
    ud.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
    D3D11_BUFFER_DESC sb = {};
    sb.ByteWidth = 64;
    sb.Usage = D3D11_USAGE_STAGING;
    sb.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    bool ok = SUCCEEDED(dev->CreateBuffer(&ab, nullptr, &auditBuf_)) &&
              SUCCEEDED(dev->CreateUnorderedAccessView(auditBuf_, &ud, &auditUav_));
    for (auto*& s : auditStage_) ok = ok && SUCCEEDED(dev->CreateBuffer(&sb, nullptr, &s));
    {
        D3D11_SAMPLER_DESC sd = {};
        sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.MaxLOD = D3D11_FLOAT32_MAX;
        ok = ok && SUCCEEDED(dev->CreateSamplerState(&sd, &linear_));
    }
    if (!ok) { put(why, cap, "audit resources"); shutdown(); return false; }
    ready_ = true;
    return true;
}

void TemporalGpu::release_targets() {
    rel(dlssDepthUav_); rel(dlssDepth_);
    rel(linearZUav_); rel(linearZSrv_); rel(linearZ_);
    rel(motionUav_); rel(motionSrv_); rel(motion_);
    rel(handsUav_); rel(handsSrv_); rel(hands_);
    w_ = h_ = 0;
}

void TemporalGpu::shutdown() {
    release_targets();
    rel(cs_); rel(csAudit_); rel(cb_); rel(linear_);
    rel(auditUav_); rel(auditBuf_);
    for (auto*& s : auditStage_) rel(s);
    for (bool& b : auditPending_) b = false;
    ready_ = false;
}

bool TemporalGpu::ensure(ID3D11Device* dev, uint32_t w, uint32_t h, char* why, size_t cap) {
    if (w == w_ && h == h_ && motion_) return true;
    release_targets();
    D3D11_TEXTURE2D_DESC d = {};
    d.Width = w; d.Height = h; d.MipLevels = 1; d.ArraySize = 1;
    d.SampleDesc.Count = 1; d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    d.Format = DXGI_FORMAT_R32_FLOAT;
    bool ok = SUCCEEDED(dev->CreateTexture2D(&d, nullptr, &dlssDepth_)) &&
              SUCCEEDED(dev->CreateUnorderedAccessView(dlssDepth_, nullptr, &dlssDepthUav_)) &&
              SUCCEEDED(dev->CreateTexture2D(&d, nullptr, &linearZ_)) &&
              SUCCEEDED(dev->CreateUnorderedAccessView(linearZ_, nullptr, &linearZUav_)) &&
              SUCCEEDED(dev->CreateShaderResourceView(linearZ_, nullptr, &linearZSrv_));
    d.Format = DXGI_FORMAT_R16G16_FLOAT;
    ok = ok && SUCCEEDED(dev->CreateTexture2D(&d, nullptr, &motion_)) &&
         SUCCEEDED(dev->CreateUnorderedAccessView(motion_, nullptr, &motionUav_)) &&
         SUCCEEDED(dev->CreateShaderResourceView(motion_, nullptr, &motionSrv_));
    d.Format = DXGI_FORMAT_R8_UNORM;
    ok = ok && SUCCEEDED(dev->CreateTexture2D(&d, nullptr, &hands_)) &&
         SUCCEEDED(dev->CreateUnorderedAccessView(hands_, nullptr, &handsUav_)) &&
         SUCCEEDED(dev->CreateShaderResourceView(hands_, nullptr, &handsSrv_));
    if (!ok) { put(why, cap, "guide textures %ux%u", w, h); release_targets(); return false; }
    w_ = w; h_ = h;
    return true;
}

bool TemporalGpu::run(ID3D11Device* dev, ID3D11DeviceContext* ctx, ID3D11ShaderResourceView* rawDepth,
                      ID3D11ShaderResourceView* fgDepth, const Params& p, char* why, size_t cap) {
    if (!ready_ && !init(dev, why, cap)) return false;
    if (!rawDepth || !p.w || !p.h) { put(why, cap, "no depth"); return false; }
    if (!ensure(dev, p.w, p.h, why, cap)) return false;
    CB cb = {};
    fill(cb, p);
    if (!fgDepth) cb.sizeFlags[3] = 0.0f;
    D3D11_MAPPED_SUBRESOURCE m = {};
    if (FAILED(ctx->Map(cb_, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) { put(why, cap, "map"); return false; }
    memcpy(m.pData, &cb, sizeof(cb));
    ctx->Unmap(cb_, 0);
    SavedCs saved(ctx);
    ID3D11UnorderedAccessView* uavs[4] = {dlssDepthUav_, linearZUav_, motionUav_, handsUav_};
    ID3D11ShaderResourceView* srvs[2] = {rawDepth, fgDepth ? fgDepth : rawDepth};
    ctx->CSSetShader(cs_, nullptr, 0);
    ctx->CSSetConstantBuffers(0, 1, &cb_);
    ctx->CSSetShaderResources(0, 2, srvs);
    ctx->CSSetUnorderedAccessViews(0, 4, uavs, nullptr);
    ctx->Dispatch((p.w + 7) / 8, (p.h + 7) / 8, 1);
    return true;
}

void TemporalGpu::audit_frame(ID3D11Device* dev, ID3D11DeviceContext* ctx, ID3D11ShaderResourceView* cur,
                              ID3D11ShaderResourceView* prev, const Params& p) {
    (void)dev;
    if (!ready_ || !cur || !prev || !motionSrv_ || !p.historyValid) return;
    // Fold in the oldest finished readback first (never waits).
    for (int i = 0; i < 3; ++i) {
        if (!auditPending_[i]) continue;
        D3D11_MAPPED_SUBRESOURCE m = {};
        if (ctx->Map(auditStage_[i], 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m) != S_OK) continue;
        const uint32_t* a = (const uint32_t*)m.pData;
        if (a[0]) {
            audit.frames += 1;
            audit.samples += a[0];
            audit.zeroErr += a[1] / 65535.0;
            audit.rotErr += a[2] / 65535.0;
            audit.vecErr += a[3] / 65535.0;
            for (int s = 0; s < kAuditScales; ++s) audit.scaleErr[s] += a[4 + s] / 65535.0;
        }
        ctx->Unmap(auditStage_[i], 0);
        auditPending_[i] = false;
    }
    const int slot = auditNext_;
    if (auditPending_[slot]) return;   // all three in flight: skip this frame
    auditNext_ = (auditNext_ + 1) % 3;
    const UINT zero[4] = {0, 0, 0, 0};
    ctx->ClearUnorderedAccessViewUint(auditUav_, zero);
    {
        SavedCs saved(ctx);
        ID3D11ShaderResourceView* srvs[4] = {cur, prev, linearZSrv_, motionSrv_};
        ctx->CSSetShader(csAudit_, nullptr, 0);
        ctx->CSSetConstantBuffers(0, 1, &cb_);
        ctx->CSSetShaderResources(0, 4, srvs);
        ctx->CSSetUnorderedAccessViews(0, 1, &auditUav_, nullptr);
        ctx->CSSetSamplers(0, 1, &linear_);
        ctx->Dispatch(8, 6, 1);
    }
    ctx->CopyResource(auditStage_[slot], auditBuf_);
    auditPending_[slot] = true;
}

// ---- TAA ------------------------------------------------------------------------------------

bool TaaGpu::init(ID3D11Device* dev, char* why, size_t cap) {
    if (ready_) return true;
    if (!compile(dev, kTaa, "cstaa", &cs_, why, cap, false)) return false;
    D3D11_BUFFER_DESC bd = {};
    bd.ByteWidth = 32; bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER; bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    D3D11_SAMPLER_DESC sd = {};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(dev->CreateBuffer(&bd, nullptr, &cb_)) || FAILED(dev->CreateSamplerState(&sd, &linear_))) {
        put(why, cap, "taa state"); shutdown(); return false;
    }
    ready_ = true;
    return true;
}

void TaaGpu::shutdown() {
    for (int e = 0; e < 2; ++e) {
        for (int k = 0; k < 2; ++k) { rel(histUav_[e][k]); rel(histSrv_[e][k]); rel(hist_[e][k]); }
        valid_[e] = false; w_[e] = h_[e] = 0; cur_[e] = 0;
    }
    rel(cs_); rel(cb_); rel(linear_);
    ready_ = false;
}

bool TaaGpu::run(ID3D11Device* dev, ID3D11DeviceContext* ctx, int eye, ID3D11ShaderResourceView* cur,
                 ID3D11ShaderResourceView* motion, ID3D11ShaderResourceView* hands, uint32_t w, uint32_t h,
                 bool historyValid, float currentWeight, char* why, size_t cap) {
    if (!ready_ && !init(dev, why, cap)) return false;
    eye &= 1;
    if (!cur || !motion || !w || !h) { put(why, cap, "taa input"); return false; }
    if (w != w_[eye] || h != h_[eye] || !hist_[eye][0]) {
        for (int k = 0; k < 2; ++k) { rel(histUav_[eye][k]); rel(histSrv_[eye][k]); rel(hist_[eye][k]); }
        D3D11_TEXTURE2D_DESC d = {};
        d.Width = w; d.Height = h; d.MipLevels = 1; d.ArraySize = 1; d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        d.SampleDesc.Count = 1; d.Usage = D3D11_USAGE_DEFAULT;
        d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
        for (int k = 0; k < 2; ++k) {
            if (FAILED(dev->CreateTexture2D(&d, nullptr, &hist_[eye][k])) ||
                FAILED(dev->CreateShaderResourceView(hist_[eye][k], nullptr, &histSrv_[eye][k])) ||
                FAILED(dev->CreateUnorderedAccessView(hist_[eye][k], nullptr, &histUav_[eye][k]))) {
                put(why, cap, "taa history %ux%u", w, h);
                return false;
            }
        }
        w_[eye] = w; h_[eye] = h; valid_[eye] = false; cur_[eye] = 0;
    }
    const int rd = cur_[eye], wr = rd ^ 1;
    const float c[8] = {(float)w, (float)h, (historyValid && valid_[eye]) ? 1.0f : 0.0f, currentWeight,
                        hands ? 1.0f : 0.0f, 0, 0, 0};
    D3D11_MAPPED_SUBRESOURCE m = {};
    if (FAILED(ctx->Map(cb_, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) { put(why, cap, "map"); return false; }
    memcpy(m.pData, c, sizeof(c));
    ctx->Unmap(cb_, 0);
    {
        SavedCs saved(ctx);
        ID3D11ShaderResourceView* srvs[4] = {cur, histSrv_[eye][rd], motion, hands ? hands : motion};
        ctx->CSSetShader(cs_, nullptr, 0);
        ctx->CSSetConstantBuffers(0, 1, &cb_);
        ctx->CSSetShaderResources(0, 4, srvs);
        ctx->CSSetUnorderedAccessViews(0, 1, &histUav_[eye][wr], nullptr);
        ctx->CSSetSamplers(0, 1, &linear_);
        ctx->Dispatch((w + 7) / 8, (h + 7) / 8, 1);
    }
    cur_[eye] = wr;
    valid_[eye] = true;
    return true;
}

} // namespace bvr::b1r::tgpu
