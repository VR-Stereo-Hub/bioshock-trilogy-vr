#include "core/gfx/cube_snapshot.h"

#include "core/util/log.h"

#include <d3dcompiler.h>

#include <cstring>

namespace bvr::cube_snapshot {
namespace {

const char* kShader = R"(
Texture2D eyeTex : register(t0);
SamplerState samp0 : register(s0);
cbuffer SplatCB : register(b0) {
    float4 qInv;   // world -> eye rotation (x y z w)
    float4 tans;   // eye fov tangents: left, right, up, down
    float4 params; // x = face index, y = inset (fraction of the fov kept)
};
struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
VSOut vs_main(uint id : SV_VertexID) {
    VSOut o;
    float2 uv = float2((id << 1) & 2, id & 2);
    o.pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
    o.uv = uv;
    return o;
}
float3 rotate_by(float4 q, float3 v) {
    float3 t = 2.0 * cross(q.xyz, v);
    return v + q.w * t + cross(q.xyz, t);
}
// The D3D cube-map face axes: texel (u,v) of face f is the direction below, u
// and v running right and DOWN across the face.
float3 face_dir(int f, float s, float t) {
    if (f == 0) return float3( 1, -t, -s);
    if (f == 1) return float3(-1, -t,  s);
    if (f == 2) return float3( s,  1,  t);
    if (f == 3) return float3( s, -1, -t);
    if (f == 4) return float3( s, -t,  1);
    return float3(-s, -t, -1);
}
float4 ps_main(VSOut i) : SV_Target {
    float3 d = normalize(face_dir((int)params.x, i.uv.x * 2 - 1, i.uv.y * 2 - 1));
    float3 e = rotate_by(qInv, d);           // the direction in the eye's frame
    if (e.z >= -1e-4) discard;               // behind the eye
    float2 t = e.xy / -e.z;                  // on the eye's tangent plane
    float k = params.y;
    if (t.x < tans.x * k || t.x > tans.y * k || t.y > tans.z * k || t.y < tans.w * k) discard;
    float2 uv = float2((t.x - tans.x) / (tans.y - tans.x), (tans.z - t.y) / (tans.z - tans.w));
    return float4(eyeTex.Sample(samp0, uv).rgb, 1);
}
// The screen over the room: black keyed out, premultiplied.
float4 ps_key(VSOut i) : SV_Target {
    float3 c = eyeTex.Sample(samp0, i.uv).rgb;
    float lum = dot(c, float3(0.299, 0.587, 0.114));
    float a = saturate((lum - 0.012) / 0.05);
    return float4(c * a, a);
}
)";

struct SplatCB {
    float qInv[4];
    float tans[4];
    float params[4];
};

ID3D11VertexShader* g_vs = nullptr;
ID3D11PixelShader* g_ps = nullptr;
ID3D11PixelShader* g_psKey = nullptr;
struct ImageRtv { ID3D11Texture2D* tex; ID3D11RenderTargetView* rtv; };
ImageRtv g_imageRtvs[8] = {}; // the eye swapchain images the screen pass wrote (a handful)
int g_imageRtvCount = 0;
ID3D11Buffer* g_cb = nullptr;
ID3D11BlendState* g_blend = nullptr;
ID3D11SamplerState* g_sampler = nullptr;
ID3D11RasterizerState* g_raster = nullptr;
ID3D11DepthStencilState* g_depth = nullptr;
ID3D11Texture2D* g_cube = nullptr;
ID3D11RenderTargetView* g_faceRtv[6] = {};
DXGI_FORMAT g_cubeFmt = DXGI_FORMAT_UNKNOWN;
ID3D11Texture2D* g_src = nullptr; // a sampleable copy of the eye image
ID3D11ShaderResourceView* g_srcSrv = nullptr;
uint32_t g_srcW = 0, g_srcH = 0;
DXGI_FORMAT g_srcFmt = DXGI_FORMAT_UNKNOWN;
uint32_t g_splats = 0;
bool g_failed = false;

void release_cube() {
    for (auto& r : g_faceRtv) { if (r) r->Release(); r = nullptr; }
    if (g_cube) g_cube->Release();
    g_cube = nullptr;
    g_cubeFmt = DXGI_FORMAT_UNKNOWN;
    g_splats = 0;
}

void release_src() {
    if (g_srcSrv) g_srcSrv->Release();
    if (g_src) g_src->Release();
    g_srcSrv = nullptr;
    g_src = nullptr;
    g_srcW = g_srcH = 0;
    g_srcFmt = DXGI_FORMAT_UNKNOWN;
}

bool ensure_pipeline(ID3D11Device* dev) {
    if (g_vs) return true;
    if (g_failed) return false;
    ID3DBlob* vsb = nullptr;
    ID3DBlob* psb = nullptr;
    ID3DBlob* pkb = nullptr;
    ID3DBlob* err = nullptr;
    bool ok = SUCCEEDED(D3DCompile(kShader, strlen(kShader), nullptr, nullptr, nullptr, "vs_main",
                                   "vs_4_0", 0, 0, &vsb, &err)) &&
              SUCCEEDED(D3DCompile(kShader, strlen(kShader), nullptr, nullptr, nullptr, "ps_main",
                                   "ps_4_0", 0, 0, &psb, &err)) &&
              SUCCEEDED(D3DCompile(kShader, strlen(kShader), nullptr, nullptr, nullptr, "ps_key",
                                   "ps_4_0", 0, 0, &pkb, &err));
    if (ok)
        ok = SUCCEEDED(dev->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(),
                                               nullptr, &g_vs)) &&
             SUCCEEDED(dev->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(),
                                              nullptr, &g_ps)) &&
             SUCCEEDED(dev->CreatePixelShader(pkb->GetBufferPointer(), pkb->GetBufferSize(),
                                              nullptr, &g_psKey));
    if (vsb) vsb->Release();
    if (psb) psb->Release();
    if (pkb) pkb->Release();
    if (err) {
        BVR_LOG("[cube] shader compile: %s", static_cast<const char*>(err->GetBufferPointer()));
        err->Release();
    }
    if (ok) {
        D3D11_BUFFER_DESC cbd{};
        cbd.ByteWidth = sizeof(SplatCB);
        cbd.Usage = D3D11_USAGE_DYNAMIC;
        cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        D3D11_BLEND_DESC bd{}; // blending off: a discarded texel keeps what it had
        bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        D3D11_SAMPLER_DESC sd{};
        sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        D3D11_RASTERIZER_DESC rd{};
        rd.FillMode = D3D11_FILL_SOLID;
        rd.CullMode = D3D11_CULL_NONE;
        rd.DepthClipEnable = TRUE;
        D3D11_DEPTH_STENCIL_DESC dd{};
        ok = SUCCEEDED(dev->CreateBuffer(&cbd, nullptr, &g_cb)) &&
             SUCCEEDED(dev->CreateBlendState(&bd, &g_blend)) &&
             SUCCEEDED(dev->CreateSamplerState(&sd, &g_sampler)) &&
             SUCCEEDED(dev->CreateRasterizerState(&rd, &g_raster)) &&
             SUCCEEDED(dev->CreateDepthStencilState(&dd, &g_depth));
    }
    if (!ok) {
        release();
        g_failed = true;
        BVR_LOG("[cube] pipeline creation FAILED - room snapshot disabled");
    }
    return ok;
}

bool ensure_cube(ID3D11Device* dev, DXGI_FORMAT fmt) {
    if (g_cube && g_cubeFmt == fmt) return true;
    release_cube();
    D3D11_TEXTURE2D_DESC td{};
    td.Width = td.Height = kFaceSize;
    td.MipLevels = 1;
    td.ArraySize = 6;
    td.Format = fmt;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    td.MiscFlags = D3D11_RESOURCE_MISC_TEXTURECUBE;
    if (FAILED(dev->CreateTexture2D(&td, nullptr, &g_cube))) {
        BVR_LOG("[cube] cube texture creation failed (format %d)", static_cast<int>(fmt));
        g_cube = nullptr;
        return false;
    }
    for (UINT f = 0; f < 6; ++f) {
        D3D11_RENDER_TARGET_VIEW_DESC rv{};
        rv.Format = fmt;
        rv.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
        rv.Texture2DArray.FirstArraySlice = f;
        rv.Texture2DArray.ArraySize = 1;
        if (FAILED(dev->CreateRenderTargetView(g_cube, &rv, &g_faceRtv[f]))) {
            BVR_LOG("[cube] face %u render target view failed", f);
            release_cube();
            return false;
        }
    }
    g_cubeFmt = fmt;
    return true;
}

bool ensure_src(ID3D11Device* dev, const D3D11_TEXTURE2D_DESC& eye) {
    if (g_src && g_srcW == eye.Width && g_srcH == eye.Height && g_srcFmt == eye.Format) return true;
    release_src();
    D3D11_TEXTURE2D_DESC td{};
    td.Width = eye.Width;
    td.Height = eye.Height;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = eye.Format;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(dev->CreateTexture2D(&td, nullptr, &g_src)) ||
        FAILED(dev->CreateShaderResourceView(g_src, nullptr, &g_srcSrv))) {
        BVR_LOG("[cube] eye copy creation failed (%ux%u format %d)", eye.Width, eye.Height,
                static_cast<int>(eye.Format));
        release_src();
        return false;
    }
    g_srcW = eye.Width;
    g_srcH = eye.Height;
    g_srcFmt = eye.Format;
    return true;
}

// The slices of state the passes touch, backed up and restored around them.
struct SavedState {
    ID3D11DeviceContext* ctx;
    ID3D11RenderTargetView* rtv = nullptr;
    ID3D11DepthStencilView* dsv = nullptr;
    ID3D11BlendState* blend = nullptr;
    FLOAT bf[4] = {};
    UINT mask = 0;
    ID3D11DepthStencilState* depth = nullptr;
    UINT stencilRef = 0;
    ID3D11RasterizerState* raster = nullptr;
    UINT nVp = 1;
    D3D11_VIEWPORT vp{};
    ID3D11VertexShader* vs = nullptr;
    ID3D11PixelShader* ps = nullptr;
    ID3D11Buffer* psCb = nullptr;
    ID3D11ShaderResourceView* srv = nullptr;
    ID3D11SamplerState* samp = nullptr;
    ID3D11InputLayout* layout = nullptr;
    D3D11_PRIMITIVE_TOPOLOGY topo{};
    explicit SavedState(ID3D11DeviceContext* c) : ctx(c) {
        ctx->OMGetRenderTargets(1, &rtv, &dsv);
        ctx->OMGetBlendState(&blend, bf, &mask);
        ctx->OMGetDepthStencilState(&depth, &stencilRef);
        ctx->RSGetState(&raster);
        ctx->RSGetViewports(&nVp, &vp);
        ctx->VSGetShader(&vs, nullptr, nullptr);
        ctx->PSGetShader(&ps, nullptr, nullptr);
        ctx->PSGetConstantBuffers(0, 1, &psCb);
        ctx->PSGetShaderResources(0, 1, &srv);
        ctx->PSGetSamplers(0, 1, &samp);
        ctx->IAGetInputLayout(&layout);
        ctx->IAGetPrimitiveTopology(&topo);
    }
    ~SavedState() {
        ctx->OMSetRenderTargets(1, &rtv, dsv);
        ctx->OMSetBlendState(blend, bf, mask);
        ctx->OMSetDepthStencilState(depth, stencilRef);
        ctx->RSSetState(raster);
        if (nVp) ctx->RSSetViewports(1, &vp);
        ctx->VSSetShader(vs, nullptr, 0);
        ctx->PSSetShader(ps, nullptr, 0);
        ctx->PSSetConstantBuffers(0, 1, &psCb);
        ctx->PSSetShaderResources(0, 1, &srv);
        ctx->PSSetSamplers(0, 1, &samp);
        ctx->IASetInputLayout(layout);
        ctx->IASetPrimitiveTopology(topo);
        if (rtv) rtv->Release();
        if (dsv) dsv->Release();
        if (blend) blend->Release();
        if (depth) depth->Release();
        if (raster) raster->Release();
        if (vs) vs->Release();
        if (ps) ps->Release();
        if (psCb) psCb->Release();
        if (srv) srv->Release();
        if (samp) samp->Release();
        if (layout) layout->Release();
    }
};

// The common binding of both passes: a fullscreen triangle from g_src.
void bind_pass(ID3D11DeviceContext* ctx, ID3D11PixelShader* ps, float w, float h) {
    D3D11_VIEWPORT vp{};
    vp.Width = w;
    vp.Height = h;
    vp.MaxDepth = 1.0f;
    ctx->OMSetBlendState(g_blend, nullptr, 0xFFFFFFFF);
    ctx->OMSetDepthStencilState(g_depth, 0);
    ctx->RSSetState(g_raster);
    ctx->RSSetViewports(1, &vp);
    ctx->IASetInputLayout(nullptr);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(g_vs, nullptr, 0);
    ctx->PSSetShader(ps, nullptr, 0);
    ctx->PSSetShaderResources(0, 1, &g_srcSrv);
    ctx->PSSetSamplers(0, 1, &g_sampler);
    ctx->PSSetConstantBuffers(0, 1, &g_cb);
}

// Pipeline, cube and a copy of `image` in g_src, ready to sample.
bool prepare(ID3D11DeviceContext* ctx, ID3D11Texture2D* image, DXGI_FORMAT cubeFmt, bool wantCube) {
    ID3D11Device* dev = nullptr;
    ctx->GetDevice(&dev);
    if (!dev) return false;
    D3D11_TEXTURE2D_DESC ed{};
    image->GetDesc(&ed);
    const bool ok = ensure_pipeline(dev) && (!wantCube || ensure_cube(dev, cubeFmt)) && ensure_src(dev, ed);
    dev->Release();
    if (!ok) return false;
    // A typed, image-sized copy of our own, sampleable whatever the swapchain
    // image allows, and read after the image is released.
    ctx->CopyResource(g_src, image);
    return true;
}

ID3D11RenderTargetView* image_rtv(ID3D11DeviceContext* ctx, ID3D11Texture2D* image) {
    for (int i = 0; i < g_imageRtvCount; ++i)
        if (g_imageRtvs[i].tex == image) return g_imageRtvs[i].rtv;
    if (g_imageRtvCount == static_cast<int>(sizeof(g_imageRtvs) / sizeof(g_imageRtvs[0]))) forget_images();
    ID3D11Device* dev = nullptr;
    ctx->GetDevice(&dev);
    if (!dev) return nullptr;
    ID3D11RenderTargetView* rtv = nullptr;
    const HRESULT hr = dev->CreateRenderTargetView(image, nullptr, &rtv);
    dev->Release();
    if (FAILED(hr)) {
        BVR_LOG("[cube] render target view on an eye image failed (%08lx)", static_cast<unsigned long>(hr));
        return nullptr;
    }
    image->AddRef();
    g_imageRtvs[g_imageRtvCount++] = {image, rtv};
    return rtv;
}

} // namespace

bool splat(ID3D11DeviceContext* ctx, ID3D11Texture2D* eyeImage, const float q[4], float tanL,
           float tanR, float tanU, float tanD, DXGI_FORMAT fmt) {
    if (!ctx || !eyeImage || !(tanR > tanL) || !(tanU > tanD)) return false;
    if (!prepare(ctx, eyeImage, fmt, true)) return false;

    SavedState saved(ctx);
    bind_pass(ctx, g_ps, static_cast<float>(kFaceSize), static_cast<float>(kFaceSize));

    SplatCB cb{};
    // The inverse rotation: world directions into the eye's frame.
    cb.qInv[0] = -q[0];
    cb.qInv[1] = -q[1];
    cb.qInv[2] = -q[2];
    cb.qInv[3] = q[3];
    cb.tans[0] = tanL;
    cb.tans[1] = tanR;
    cb.tans[2] = tanU;
    cb.tans[3] = tanD;
    cb.params[1] = 0.96f; // keep clear of the image's outermost rim
    for (UINT f = 0; f < 6; ++f) {
        cb.params[0] = static_cast<float>(f);
        D3D11_MAPPED_SUBRESOURCE m{};
        if (FAILED(ctx->Map(g_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) break;
        memcpy(m.pData, &cb, sizeof(cb));
        ctx->Unmap(g_cb, 0);
        ctx->OMSetRenderTargets(1, &g_faceRtv[f], nullptr);
        ctx->Draw(3, 0);
    }
    ++g_splats;
    return true;
}

bool key_black(ID3D11DeviceContext* ctx, ID3D11Texture2D* image) {
    if (!ctx || !image) return false;
    if (!prepare(ctx, image, DXGI_FORMAT_UNKNOWN, false)) return false;
    ID3D11RenderTargetView* rtv = image_rtv(ctx, image);
    if (!rtv) return false;
    SavedState saved(ctx);
    bind_pass(ctx, g_psKey, static_cast<float>(g_srcW), static_cast<float>(g_srcH));
    ctx->OMSetRenderTargets(1, &rtv, nullptr);
    ctx->Draw(3, 0);
    return true;
}

void forget_images() {
    for (int i = 0; i < g_imageRtvCount; ++i) {
        g_imageRtvs[i].rtv->Release();
        g_imageRtvs[i].tex->Release();
    }
    g_imageRtvCount = 0;
}

ID3D11Texture2D* texture() { return g_cube; }
bool valid() { return g_cube && g_splats > 0; }
uint32_t splats() { return g_splats; }

void reset() {
    if (!g_cube) return;
    ID3D11Device* dev = nullptr;
    g_cube->GetDevice(&dev);
    if (!dev) return;
    ID3D11DeviceContext* ctx = nullptr;
    dev->GetImmediateContext(&ctx);
    if (ctx) {
        const float black[4] = {0, 0, 0, 1};
        for (auto* r : g_faceRtv)
            if (r) ctx->ClearRenderTargetView(r, black);
        ctx->Release();
    }
    dev->Release();
    g_splats = 0;
}

void release() {
    release_cube();
    release_src();
    forget_images();
    if (g_cb) g_cb->Release();
    if (g_blend) g_blend->Release();
    if (g_sampler) g_sampler->Release();
    if (g_raster) g_raster->Release();
    if (g_depth) g_depth->Release();
    if (g_psKey) g_psKey->Release();
    if (g_ps) g_ps->Release();
    if (g_vs) g_vs->Release();
    g_cb = nullptr;
    g_blend = nullptr;
    g_sampler = nullptr;
    g_raster = nullptr;
    g_depth = nullptr;
    g_psKey = nullptr;
    g_ps = nullptr;
    g_vs = nullptr;
}

} // namespace bvr::cube_snapshot
