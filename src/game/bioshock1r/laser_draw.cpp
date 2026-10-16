// See laser_draw.h.
#include "game/bioshock1r/laser_draw.h"

#include "core/util/log.h"
#include "game/bioshock1r/temporal_gpu.h"

#include <d3d11.h>
#include <d3dcompiler.h>

#include <cmath>
#include <cstring>

namespace bvr::b1r::laser_draw {
namespace {

template <class T> void rel(T*& p) { if (p) { p->Release(); p = nullptr; } }

constexpr int kMaxDots = 16;

// One quad per dot from SV_VertexID; the pixel shader draws a soft disc - the laser's dots and
// the reticle alike, as the XR quads draw both from one texture. Premultiplied output.
const char* kShader = R"(
cbuffer C : register(b0) {
    float4 dots[16];   // xy centre (NDC), zw radius (NDC)
    float4 color;      // rgb, a = opacity
};
struct V { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
V vs(uint id : SV_VertexID) {
    const float2 corner[6] = {float2(-1, -1), float2(-1, 1), float2(1, 1),
                              float2(-1, -1), float2(1, 1), float2(1, -1)};
    uint d = id / 6;
    float2 k = corner[id % 6];
    V o;
    o.pos = float4(dots[d].xy + k * dots[d].zw, 0, 1);
    o.uv = k;
    return o;
}
float4 ps(V i) : SV_Target {
    // The XR laser's own dot (openxr_runtime.cpp create_laser): a solid core to a quarter of
    // the radius, then (1 - r)^1.5, so the two lasers read the same size.
    float r = length(i.uv);
    float a = r >= 1.0f ? 0.0f : (r < 0.25f ? 1.0f : pow(1.0f - r, 1.5f));
    a *= color.a;
    return float4(color.rgb * a, a);
}
)";

struct alignas(16) CB {
    float dots[kMaxDots][4];
    float color[4];
};

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

} // namespace

bool draw(ID3D11DeviceContext* ctx, ID3D11Texture2D* dst, uint32_t w, uint32_t h, const tgpu::Camera& cam,
          const Segment& s) {
    if (!ctx || !dst || !w || !h || !s.valid || (!s.laser && !s.reticle)) return false;
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
    auto add = [&](float distUu, float diamDeg) {
        if (n >= kMaxDots) return -1;
        const float p[3] = {s.origin[0] + s.dir[0] * distUu, s.origin[1] + s.dir[1] * distUu,
                            s.origin[2] + s.dir[2] * distUu};
        float x, y, z;
        if (!project(cam, p, &x, &y, &z)) return -1;
        const float t = tanf(diamDeg * 0.5f * 3.14159265f / 180.0f);
        cb.dots[n][0] = x;
        cb.dots[n][1] = y;
        cb.dots[n][2] = t / cam.tanH;
        cb.dots[n][3] = t / cam.tanV;
        return n++;
    };
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
    if (s.reticle) add(s.reticleUu, s.reticleDeg);
    if (n == 0) return false;
    cb.color[0] = 1.0f; cb.color[1] = 60.0f / 255.0f; cb.color[2] = 40.0f / 255.0f; cb.color[3] = 1.0f;

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
    g_dev = nullptr;
}

} // namespace bvr::b1r::laser_draw
