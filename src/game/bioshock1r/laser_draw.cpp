// See laser_draw.h.
#include "game/bioshock1r/laser_draw.h"

#include "core/util/log.h"
#include "game/bioshock1r/temporal_gpu.h"

#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace bvr::b1r::laser_draw {
namespace {

template <class T> void rel(T*& p) { if (p) { p->Release(); p = nullptr; } }

constexpr int kMaxDots = 16;

// One quad per dot from SV_VertexID; the pixel shader draws a soft disc - the laser's dots and
// the reticle alike, as the XR quads draw both from one texture. Premultiplied output.
const char* kShader = R"(
cbuffer C : register(b0) {
    float4 dots[16];   // xy centre (NDC), zw radius (NDC) - for a reticle, the radius WITH its rim
    float4 style[16];  // x: 0 a laser dot, 1 a reticle; y: the coloured disc's share of the radius
    float4 color;      // the laser: rgb, a = opacity
    float4 reticle;    // the reticle's fill: rgb, a = opacity
    float4 rim;        // the reticle's rim: rgb, a = opacity
};
struct V { float4 pos : SV_Position; float2 uv : TEXCOORD0; nointerpolation float2 st : TEXCOORD1; };
V vs(uint id : SV_VertexID) {
    const float2 corner[6] = {float2(-1, -1), float2(-1, 1), float2(1, 1),
                              float2(-1, -1), float2(1, 1), float2(1, -1)};
    uint d = id / 6;
    float2 k = corner[id % 6];
    V o;
    o.pos = float4(dots[d].xy + k * dots[d].zw, 0, 1);
    o.uv = k;
    o.st = style[d].xy;
    return o;
}
float4 ps(V i) : SV_Target {
    float r = length(i.uv);
    if (i.st.x > 0.5f) {
        // 2026-10-25, the reticle as BO3 Zombies VR's aim aid draws it: a solid disc on a dark rim, edges
        // softened over a few percent so it does not shimmer.
        float fill = 1.0f - smoothstep(i.st.y - 0.06f, i.st.y, r);
        float ring = 1.0f - smoothstep(0.94f, 1.0f, r);
        float a = lerp(ring * rim.a, reticle.a, fill);
        float3 c = lerp(rim.rgb, reticle.rgb, fill);
        return float4(c * a, a);
    }
    // The XR laser's own dot (openxr_runtime.cpp create_laser): a solid core to a quarter of
    // the radius, then (1 - r)^1.5, so the two lasers read the same size.
    float a = r >= 1.0f ? 0.0f : (r < 0.25f ? 1.0f : pow(1.0f - r, 1.5f));
    a *= color.a;
    return float4(color.rgb * a, a);
}
)";

struct alignas(16) CB {
    float dots[kMaxDots][4];
    float style[kMaxDots][4];
    float color[4];
    float reticle[4];
    float rim[4];
};

// ---- 2026-10-25: the dynamic reticle distance (measure) ---------------------------------------------------------
// One thread per reticle marches its ray through the eye's linear view depth: geometric steps from the near end to
// the far end, the first step at or behind the scene's surface refined by bisection. The viewmodel (hands mask) is
// looked through. Out: the distance along the ray (uu), 0 nothing hit within the range, -1 not measurable here
// (the ray left this image, or no ray).
const char* kMeasureShader = R"(
cbuffer M : register(b0) {
    float4 camLoc;   // xyz, w tanH
    float4 camFwd;   // xyz, w tanV
    float4 camRight; // xyz, w image width
    float4 camUp;    // xyz, w image height
    float4 o0, d0;   // reticle 0: origin (w 1 = valid), direction
    float4 o1, d1;   // reticle 1
    float4 range;    // x near (uu), y far (uu), z steps
};
Texture2D<float> linZ : register(t0);
Texture2D<float> handsMask : register(t1);
RWTexture2D<float> outD : register(u0);

// -1 off the image or in front of the camera; 0 nothing to compare (sky, the viewmodel); else 1 = at or behind
// the surface, 2 = in front of it.
int test(float3 p) {
    float3 v = p - camLoc.xyz;
    float z = dot(v, camFwd.xyz);
    if (z < 1.0f) return -1;
    float x = dot(v, camRight.xyz) / (z * camLoc.w), y = dot(v, camUp.xyz) / (z * camFwd.w);
    if (abs(x) > 1.0f || abs(y) > 1.0f) return -1;
    int2 px = int2(min((x * 0.5f + 0.5f) * camRight.w, camRight.w - 1.0f), min((0.5f - y * 0.5f) * camUp.w, camUp.w - 1.0f));
    float sz = linZ.Load(int3(px, 0));
    if (sz <= 0.0f || handsMask.Load(int3(px, 0)) > 0.5f) return 0;
    return z >= sz ? 1 : 2;
}
float march(float3 o, float3 d) {
    float n = range.x, f = range.y;
    int steps = (int)range.z;
    float prev = n;
    bool seen = false;
    for (int k = 0; k < steps; ++k) {
        float s = n * pow(f / n, k / (float)(steps - 1));
        int t = test(o + d * s);
        if (t == -1) return seen ? 0.0f : -1.0f;
        seen = true;
        if (t == 1) {
            float a = prev, b = s;
            for (int i = 0; i < 6; ++i) {
                float m = 0.5f * (a + b);
                if (test(o + d * m) == 1) b = m; else a = m;
            }
            return b;
        }
        prev = s;
    }
    return 0.0f;
}
[numthreads(2, 1, 1)]
void cs(uint3 id : SV_DispatchThreadID) {
    float4 o = id.x == 0 ? o0 : o1;
    float4 d = id.x == 0 ? d0 : d1;
    outD[uint2(id.x, 0)] = o.w > 0.5f ? march(o.xyz, d.xyz) : -1.0f;
}
)";

struct alignas(16) MCB {
    float camLoc[4], camFwd[4], camRight[4], camUp[4];
    float o0[4], d0[4], o1[4], d1[4];
    float range[4];
};
constexpr float kMarchNearUu = 30.0f;   // 0.3 m: nearer than the muzzle's own reach is the gun
constexpr float kMarchFarUu = 6000.0f;  // 60 m; nothing hit within it = the far end (Darkness II: 40 m, BO3: 200 m)
constexpr int kMarchSteps = 96;
constexpr int kStages = 3;
ID3D11ComputeShader* g_mcs = nullptr;
ID3D11Buffer* g_mcb = nullptr;
ID3D11Texture2D* g_mtex = nullptr;
ID3D11UnorderedAccessView* g_muav = nullptr;
ID3D11Texture2D* g_mstage[kStages] = {};
bool g_mpending[kStages] = {};
int g_mnext = 0;
bool g_mfailed = false;
std::atomic<float> g_hitUu[2] = {-1.0f, -1.0f};
std::atomic<uint64_t> g_hitMs[2] = {0, 0};
uint32_t g_mHits = 0, g_mMisses = 0, g_mOff = 0;
uint64_t g_mLogMs = 0;

bool init_measure(ID3D11Device* dev) {
    if (g_mcs) return true;
    if (g_mfailed) return false;
    ID3DBlob *cb = nullptr, *err = nullptr;
    bool ok = SUCCEEDED(D3DCompile(kMeasureShader, strlen(kMeasureShader), "b1r_reticle_march", nullptr, nullptr, "cs",
                                   "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &cb, &err));
    if (!ok) BVR_LOG("[reticle] march compile: %s", err ? (const char*)err->GetBufferPointer() : "?");
    rel(err);
    ok = ok && SUCCEEDED(dev->CreateComputeShader(cb->GetBufferPointer(), cb->GetBufferSize(), nullptr, &g_mcs));
    rel(cb);
    D3D11_BUFFER_DESC bd = {};
    bd.ByteWidth = sizeof(MCB);
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    ok = ok && SUCCEEDED(dev->CreateBuffer(&bd, nullptr, &g_mcb));
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = 2;
    td.Height = 1;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R32_FLOAT;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
    ok = ok && SUCCEEDED(dev->CreateTexture2D(&td, nullptr, &g_mtex)) &&
         SUCCEEDED(dev->CreateUnorderedAccessView(g_mtex, nullptr, &g_muav));
    td.Usage = D3D11_USAGE_STAGING;
    td.BindFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    for (auto*& s : g_mstage) ok = ok && SUCCEEDED(dev->CreateTexture2D(&td, nullptr, &s));
    if (!ok) {
        g_mfailed = true;
        BVR_LOG("[reticle] dynamic distance unavailable: the depth march could not be created - the reticle "
                "stays at its set distance");
        rel(g_mcs); rel(g_mcb); rel(g_muav); rel(g_mtex);
        for (auto*& s : g_mstage) rel(s);
        return false;
    }
    BVR_LOG("[reticle] dynamic distance: the reticle sits where its ray meets the scene (depth march %d steps, "
            "%.1f to %.0f m)", kMarchSteps, kMarchNearUu / 100.0f, kMarchFarUu / 100.0f);
    return true;
}

ID3D11Device* g_dev = nullptr;
ID3D11VertexShader* g_vs = nullptr;
ID3D11PixelShader* g_ps = nullptr;
ID3D11Buffer* g_cb = nullptr;
ID3D11BlendState* g_blend = nullptr;
ID3D11RasterizerState* g_rs = nullptr;
ID3D11DepthStencilState* g_ds = nullptr;
bool g_failed = false;

struct RtvSlot { ID3D11Texture2D* tex = nullptr; ID3D11RenderTargetView* rtv = nullptr; };
RtvSlot g_rtv[8];
int g_rtvNext = 0;

bool init(ID3D11Device* dev) {
    if (g_vs && g_dev == dev) return true;
    if (g_failed) return false;
    release();
    g_dev = dev;
    ID3DBlob *vb = nullptr, *pb = nullptr, *err = nullptr;
    bool ok = SUCCEEDED(D3DCompile(kShader, strlen(kShader), "b1r_laser", nullptr, nullptr, "vs", "vs_5_0",
                                   D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &vb, &err));
    if (!ok) BVR_LOG("[laser] vs compile: %s", err ? (const char*)err->GetBufferPointer() : "?");
    rel(err);
    ok = ok && SUCCEEDED(D3DCompile(kShader, strlen(kShader), "b1r_laser", nullptr, nullptr, "ps", "ps_5_0",
                                    D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &pb, &err));
    if (!ok && err) BVR_LOG("[laser] ps compile: %s", (const char*)err->GetBufferPointer());
    rel(err);
    ok = ok && SUCCEEDED(dev->CreateVertexShader(vb->GetBufferPointer(), vb->GetBufferSize(), nullptr, &g_vs)) &&
         SUCCEEDED(dev->CreatePixelShader(pb->GetBufferPointer(), pb->GetBufferSize(), nullptr, &g_ps));
    rel(vb); rel(pb);
    D3D11_BUFFER_DESC bd = {};
    bd.ByteWidth = sizeof(CB);
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    ok = ok && SUCCEEDED(dev->CreateBuffer(&bd, nullptr, &g_cb));
    D3D11_BLEND_DESC bl = {};
    bl.RenderTarget[0].BlendEnable = TRUE;
    bl.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
    bl.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    bl.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    bl.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ZERO;
    bl.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
    bl.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    bl.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    ok = ok && SUCCEEDED(dev->CreateBlendState(&bl, &g_blend));
    D3D11_RASTERIZER_DESC rd = {};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = FALSE;
    ok = ok && SUCCEEDED(dev->CreateRasterizerState(&rd, &g_rs));
    D3D11_DEPTH_STENCIL_DESC dd = {};
    dd.DepthEnable = FALSE;
    dd.StencilEnable = FALSE;
    ok = ok && SUCCEEDED(dev->CreateDepthStencilState(&dd, &g_ds));
    if (!ok) {
        g_failed = true;
        BVR_LOG("[laser] in-image laser unavailable: pipeline creation failed");
        release();
        return false;
    }
    return true;
}

DXGI_FORMAT typed(DXGI_FORMAT f) {
    switch (f) {
        case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
        case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
        case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
        case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
        default: return f;
    }
}

// The eye swapchains are a small ring of images: one view each, made once.
ID3D11RenderTargetView* rtv_for(ID3D11Texture2D* tex) {
    for (const RtvSlot& s : g_rtv)
        if (s.tex == tex) return s.rtv;
    D3D11_TEXTURE2D_DESC d = {};
    tex->GetDesc(&d);
    if (!(d.BindFlags & D3D11_BIND_RENDER_TARGET) || d.SampleDesc.Count != 1) {
        static bool logged = false;
        if (!logged) {
            logged = true;
            BVR_LOG("[laser] eye image cannot be rendered into (bind 0x%X, samples %u) - the XR laser stays",
                    d.BindFlags, d.SampleDesc.Count);
        }
        return nullptr;
    }
    D3D11_RENDER_TARGET_VIEW_DESC v = {};
    v.Format = typed(d.Format);
    v.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    ID3D11RenderTargetView* rtv = nullptr;
    if (FAILED(g_dev->CreateRenderTargetView(tex, &v, &rtv))) return nullptr;
    RtvSlot& s = g_rtv[g_rtvNext];
    g_rtvNext = (g_rtvNext + 1) % 8;
    rel(s.rtv);
    s.tex = tex;
    s.rtv = rtv;
    return rtv;
}

// World point -> NDC through `c`; false behind (or too close to) the camera.
bool project(const tgpu::Camera& c, const float p[3], float* x, float* y, float* z) {
    const float v[3] = {p[0] - c.loc[0], p[1] - c.loc[1], p[2] - c.loc[2]};
    *z = v[0] * c.fwd[0] + v[1] * c.fwd[1] + v[2] * c.fwd[2];
    if (*z < 1.0f) return false;
    *x = (v[0] * c.right[0] + v[1] * c.right[1] + v[2] * c.right[2]) / (*z * c.tanH);
    *y = (v[0] * c.up[0] + v[1] * c.up[1] + v[2] * c.up[2]) / (*z * c.tanV);
    return true;
}

// Saves and restores exactly the pipeline state the draw touches.
struct Saved {
    ID3D11RenderTargetView* rtv = nullptr;
    ID3D11DepthStencilView* dsv = nullptr;
    ID3D11BlendState* blend = nullptr;
    float blendFactor[4] = {};
    UINT sampleMask = 0;
    ID3D11DepthStencilState* ds = nullptr;
    UINT stencilRef = 0;
    ID3D11RasterizerState* rs = nullptr;
    D3D11_VIEWPORT vp[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE] = {};
    UINT vpCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    D3D11_PRIMITIVE_TOPOLOGY topo = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
    ID3D11InputLayout* il = nullptr;
    ID3D11VertexShader* vs = nullptr;
    ID3D11PixelShader* ps = nullptr;
    ID3D11GeometryShader* gs = nullptr;
    ID3D11Buffer* vcb = nullptr;
    ID3D11Buffer* pcb = nullptr;
    explicit Saved(ID3D11DeviceContext* c) : ctx(c) {
        ctx->OMGetRenderTargets(1, &rtv, &dsv);
        ctx->OMGetBlendState(&blend, blendFactor, &sampleMask);
        ctx->OMGetDepthStencilState(&ds, &stencilRef);
        ctx->RSGetState(&rs);
        ctx->RSGetViewports(&vpCount, vp);
        ctx->IAGetPrimitiveTopology(&topo);
        ctx->IAGetInputLayout(&il);
        ctx->VSGetShader(&vs, nullptr, nullptr);
        ctx->PSGetShader(&ps, nullptr, nullptr);
        ctx->GSGetShader(&gs, nullptr, nullptr);
        ctx->VSGetConstantBuffers(0, 1, &vcb);
        ctx->PSGetConstantBuffers(0, 1, &pcb);
    }
    ~Saved() {
        ctx->OMSetRenderTargets(1, &rtv, dsv);
        ctx->OMSetBlendState(blend, blendFactor, sampleMask);
        ctx->OMSetDepthStencilState(ds, stencilRef);
        ctx->RSSetState(rs);
        ctx->RSSetViewports(vpCount, vp);
        ctx->IASetPrimitiveTopology(topo);
        ctx->IASetInputLayout(il);
        ctx->VSSetShader(vs, nullptr, 0);
        ctx->PSSetShader(ps, nullptr, 0);
        ctx->GSSetShader(gs, nullptr, 0);
        ctx->VSSetConstantBuffers(0, 1, &vcb);
        ctx->PSSetConstantBuffers(0, 1, &pcb);
        rel(rtv); rel(dsv); rel(blend); rel(ds); rel(rs); rel(il); rel(vs); rel(ps); rel(gs); rel(vcb); rel(pcb);
    }
    ID3D11DeviceContext* ctx;
};

// The readback: the oldest staging copy that is ready, without waiting.
void collect(ID3D11DeviceContext* ctx) {
    const uint64_t now = GetTickCount64();
    for (int k = 0; k < kStages; ++k) {
        const int i = (g_mnext + k) % kStages; // oldest first
        if (!g_mpending[i]) continue;
        D3D11_MAPPED_SUBRESOURCE m;
        if (ctx->Map(g_mstage[i], 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m) != S_OK) continue;
        float d[2];
        memcpy(d, m.pData, sizeof d);
        ctx->Unmap(g_mstage[i], 0);
        g_mpending[i] = false;
        for (int r = 0; r < 2; ++r) {
            if (d[r] < 0.0f) { ++g_mOff; continue; } // not measurable in this image: keep the last
            if (d[r] > 0.0f) ++g_mHits; else ++g_mMisses;
            g_hitUu[r].store(d[r] > 0.0f ? d[r] : kMarchFarUu, std::memory_order_relaxed);
            g_hitMs[r].store(now, std::memory_order_relaxed);
        }
    }
    if (now - g_mLogMs >= 5000) {
        g_mLogMs = now;
        if (g_mHits || g_mMisses)
            BVR_LOG("[reticle] 5 s: %u measured hits, %u with nothing within %.0f m, %u off the image; now %.2f m",
                    g_mHits, g_mMisses, kMarchFarUu / 100.0f, g_mOff, g_hitUu[0].load() / 100.0f);
        g_mHits = g_mMisses = g_mOff = 0;
    }
}

} // namespace

void measure(ID3D11DeviceContext* ctx, ID3D11ShaderResourceView* linearZ, ID3D11ShaderResourceView* handsMask,
             uint32_t w, uint32_t h, const tgpu::Camera& cam, const Segment& s) {
    if (!ctx || !linearZ || !w || !h || !s.valid || !s.dynamic || (!s.reticle && !s.reticle2)) return;
    ID3D11Device* dev = nullptr;
    ctx->GetDevice(&dev);
    if (!dev) return;
    const bool ok = init_measure(dev);
    dev->Release();
    if (!ok) return;
    collect(ctx);
    if (g_mpending[g_mnext]) return; // every copy still in flight: skip this image
    MCB cb = {};
    auto put = [](float* d, const float* v, float w4) { d[0] = v[0]; d[1] = v[1]; d[2] = v[2]; d[3] = w4; };
    put(cb.camLoc, cam.loc, cam.tanH);
    put(cb.camFwd, cam.fwd, cam.tanV);
    put(cb.camRight, cam.right, static_cast<float>(w));
    put(cb.camUp, cam.up, static_cast<float>(h));
    put(cb.o0, s.origin, s.reticle ? 1.0f : 0.0f);
    put(cb.d0, s.dir, 0.0f);
    put(cb.o1, s.origin2, s.reticle2 ? 1.0f : 0.0f);
    put(cb.d1, s.dir2, 0.0f);
    cb.range[0] = kMarchNearUu;
    cb.range[1] = kMarchFarUu;
    cb.range[2] = static_cast<float>(kMarchSteps);
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(ctx->Map(g_mcb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return;
    memcpy(m.pData, &cb, sizeof cb);
    ctx->Unmap(g_mcb, 0);
    // Save and restore the compute bindings this touches (nothing else in the frame is compute-bound here, but the
    // temporal passes bind their own and expect them unchanged across this call).
    ID3D11ComputeShader* oldCs = nullptr;
    ctx->CSGetShader(&oldCs, nullptr, nullptr);
    ID3D11Buffer* oldCb = nullptr;
    ctx->CSGetConstantBuffers(0, 1, &oldCb);
    ID3D11ShaderResourceView* oldSrv[2] = {};
    ctx->CSGetShaderResources(0, 2, oldSrv);
    ID3D11UnorderedAccessView* oldUav = nullptr;
    ctx->CSGetUnorderedAccessViews(0, 1, &oldUav);
    ID3D11ShaderResourceView* srvs[2] = {linearZ, handsMask};
    ctx->CSSetShader(g_mcs, nullptr, 0);
    ctx->CSSetConstantBuffers(0, 1, &g_mcb);
    ctx->CSSetShaderResources(0, 2, srvs);
    ctx->CSSetUnorderedAccessViews(0, 1, &g_muav, nullptr);
    ctx->Dispatch(1, 1, 1);
    ctx->CSSetShader(oldCs, nullptr, 0);
    ctx->CSSetConstantBuffers(0, 1, &oldCb);
    ctx->CSSetShaderResources(0, 2, oldSrv);
    ctx->CSSetUnorderedAccessViews(0, 1, &oldUav, nullptr);
    rel(oldCs); rel(oldCb); rel(oldSrv[0]); rel(oldSrv[1]); rel(oldUav);
    ctx->CopyResource(g_mstage[g_mnext], g_mtex);
    g_mpending[g_mnext] = true;
    g_mnext = (g_mnext + 1) % kStages;
}

float measured_uu(int i) {
    const int r = i ? 1 : 0;
    if (GetTickCount64() - g_hitMs[r].load(std::memory_order_relaxed) > 250) return -1.0f;
    return g_hitUu[r].load(std::memory_order_relaxed);
}

bool draw(ID3D11DeviceContext* ctx, ID3D11Texture2D* dst, uint32_t w, uint32_t h, const tgpu::Camera& cam,
          const Segment& s) {
    if (!ctx || !dst || !w || !h || !s.valid || (!s.laser && !s.reticle && !s.reticle2)) return false;
    ID3D11Device* dev = nullptr;
    ctx->GetDevice(&dev);
    if (!dev) return false;
    const bool ok = init(dev);
    dev->Release();
    if (!ok) return false;
    ID3D11RenderTargetView* rtv = rtv_for(dst);
    if (!rtv) return false;

    CB cb = {};
    int n = 0;
    auto add_at = [&](const float* o, const float* d, float distUu, float diamDeg, bool isReticle) {
        if (n >= kMaxDots) return -1;
        const float p[3] = {o[0] + d[0] * distUu, o[1] + d[1] * distUu, o[2] + d[2] * distUu};
        float x, y, z;
        if (!project(cam, p, &x, &y, &z)) return -1;
        const float t = tanf(diamDeg * 0.5f * 3.14159265f / 180.0f);
        float rx = t / cam.tanH, ry = t / cam.tanV, share = 1.0f;
        if (isReticle) {
            // BO3's rim: 1.2 px past the disc (0.8 px on a disc under 3 px), so it reads on any background.
            const float rPx = rx * 0.5f * static_cast<float>(w);
            const float rimPx = rPx < 3.0f ? 0.8f : 1.2f;
            const float outer = (rPx < 1.2f ? 1.2f : rPx) + rimPx;
            share = (outer - rimPx) / outer;
            rx = outer * 2.0f / static_cast<float>(w);
            ry = outer * 2.0f / static_cast<float>(h);
        }
        cb.dots[n][0] = x;
        cb.dots[n][1] = y;
        cb.dots[n][2] = rx;
        cb.dots[n][3] = ry;
        cb.style[n][0] = isReticle ? 1.0f : 0.0f;
        cb.style[n][1] = share;
        return n++;
    };
    auto add = [&](float distUu, float diamDeg) { return add_at(s.origin, s.dir, distUu, diamDeg, false); };
    if (s.laser) {
        const int dots = s.dots < 1 ? 1 : s.dots > kMaxDots - 1 ? kMaxDots - 1 : s.dots;
        const float nearUu = s.nearUu > 0.1f ? s.nearUu : 0.1f;
        const float farUu = s.farUu > nearUu * 1.01f ? s.farUu : nearUu * 1.01f;
        // Geometric spacing, as the XR laser: a beam receding into the distance.
        for (int i = 0; i < dots; ++i) {
            const float t = dots == 1 ? 0.0f : static_cast<float>(i) / static_cast<float>(dots - 1);
            add(nearUu * powf(farUu / nearUu, t), s.dotDeg);
        }
    }
    // The reticle is a dot of the slider's diameter, as the XR aim dot (build_aim_dot_layer) is.
    // 2026-10-14: it was a ring at twice that size, which read as a circle, not the dot.
    // 2026-10-25: at the measured hit (measure()) while one is recent, else the set distance.
    if (s.reticle) {
        const float m = s.dynamic ? measured_uu(0) : -1.0f;
        add_at(s.origin, s.dir, m > 0.0f ? m : s.reticleUu, s.reticleDeg, true);
    }
    if (s.reticle2) {
        const float m = s.dynamic ? measured_uu(1) : -1.0f;
        add_at(s.origin2, s.dir2, m > 0.0f ? m : s.reticle2Uu, s.reticleDeg, true);
    }
    if (n == 0) return false;
    cb.color[0] = 1.0f; cb.color[1] = 60.0f / 255.0f; cb.color[2] = 40.0f / 255.0f; cb.color[3] = 1.0f;
    // 2026-10-25: the reticle as BO3 Zombies VR ships its aim aid - white at 90 %, on a dark rim (12, 0, 0) at
    // 150/255 of that.
    cb.reticle[0] = 1.0f; cb.reticle[1] = 1.0f; cb.reticle[2] = 1.0f; cb.reticle[3] = 0.9f;
    cb.rim[0] = 12.0f / 255.0f; cb.rim[1] = 0.0f; cb.rim[2] = 0.0f; cb.rim[3] = 0.9f * 150.0f / 255.0f;

    Saved saved(ctx);
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(ctx->Map(g_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return false;
    memcpy(m.pData, &cb, sizeof cb);
    ctx->Unmap(g_cb, 0);
    D3D11_VIEWPORT vp = {0.0f, 0.0f, static_cast<float>(w), static_cast<float>(h), 0.0f, 1.0f};
    ctx->OMSetRenderTargets(1, &rtv, nullptr);
    const float bf[4] = {};
    ctx->OMSetBlendState(g_blend, bf, 0xFFFFFFFF);
    ctx->OMSetDepthStencilState(g_ds, 0);
    ctx->RSSetState(g_rs);
    ctx->RSSetViewports(1, &vp);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->IASetInputLayout(nullptr);
    ctx->VSSetShader(g_vs, nullptr, 0);
    ctx->PSSetShader(g_ps, nullptr, 0);
    ctx->GSSetShader(nullptr, nullptr, 0);
    ctx->VSSetConstantBuffers(0, 1, &g_cb);
    ctx->PSSetConstantBuffers(0, 1, &g_cb);
    ctx->Draw(static_cast<UINT>(n) * 6, 0);
    return true;
}

void release() {
    for (RtvSlot& s : g_rtv) { rel(s.rtv); s.tex = nullptr; }
    rel(g_vs); rel(g_ps); rel(g_cb); rel(g_blend); rel(g_rs); rel(g_ds);
    rel(g_mcs); rel(g_mcb); rel(g_muav); rel(g_mtex);
    for (int i = 0; i < kStages; ++i) { rel(g_mstage[i]); g_mpending[i] = false; }
    g_dev = nullptr;
}

} // namespace bvr::b1r::laser_draw
