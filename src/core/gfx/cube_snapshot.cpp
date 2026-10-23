#include "core/gfx/cube_snapshot.h"

#include "core/util/log.h"

#include <d3dcompiler.h>

#include <cstring>
#include <vector>

namespace bvr::cube_snapshot {
namespace {

const char* kShader = R"(
Texture2D eyeTex : register(t0);
SamplerState samp0 : register(s0);
cbuffer SplatCB : register(b0) {
    float4 qInv;   // world -> eye rotation (x y z w)
    float4 tans;   // eye fov tangents: left, right, up, down
    float4 params; // x = face index, y = inset (fraction of the fov kept); key: z = circle radius, w = aspect
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
// The eye-image texel a cube texel sees (discarding outside the eye's fov), and its
// direction's tangent-plane position.
void splat_at(float2 cubeUv, out float2 uv, out float2 t) {
    float3 d = normalize(face_dir((int)params.x, cubeUv.x * 2 - 1, cubeUv.y * 2 - 1));
    float3 e = rotate_by(qInv, d);           // the direction in the eye's frame
    if (e.z >= -1e-4) discard;               // behind the eye
    t = e.xy / -e.z;                         // on the eye's tangent plane
    float k = params.y;
    if (t.x < tans.x * k || t.x > tans.y * k || t.y > tans.z * k || t.y < tans.w * k) discard;
    uv = float2((t.x - tans.x) / (tans.y - tans.x), (tans.z - t.y) / (tans.z - tans.w));
}
float4 ps_main(VSOut i) : SV_Target {
    float2 uv, t;
    splat_at(i.uv, uv, t);
    return float4(eyeTex.Sample(samp0, uv).rgb, 1);
}
// 2026-10-23 (the tester: "mono painted around you"): with the eye image's linear depth
// (uu along the view axis) the cube keeps each texel's DISTANCE too, in metres (params.z
// is uu per metre), so the backdrop can give each eye its own parallax.
Texture2D depthTex : register(t2);
struct SplatOut { float4 c : SV_Target0; float d : SV_Target1; };
SplatOut ps_splat_depth(VSOut i) {
    float2 uv, t;
    splat_at(i.uv, uv, t);
    SplatOut o;
    o.c = float4(eyeTex.Sample(samp0, uv).rgb, 1);
    o.d = depthTex.SampleLevel(samp0, uv, 0).r * sqrt(1 + dot(t, t)) / params.z;
    return o;
}
// The screen over the room: black keyed out, premultiplied, and when asked a
// circle (radius in fractions of the frame's height, centred) with a soft rim.
float4 ps_key(VSOut i) : SV_Target {
    float3 c = eyeTex.Sample(samp0, i.uv).rgb;
    float lum = dot(c, float3(0.299, 0.587, 0.114));
    float a = saturate((lum - 0.012) / 0.05);
    if (params.z > 0) {
        float2 d = (i.uv - 0.5) * float2(params.w, 1);
        a *= 1 - smoothstep(params.z - 0.006, params.z + 0.006, length(d));
    }
    return float4(c * a, a);
}
// The room as a projection-layer image (runtimes without the cube layer): each
// pixel's eye ray, from the eye's fov tangents, rotated into the world by `qInv`
// (here the eye-to-world rotation), looks the cube up.
// With the room's distances (params.x > 0), the eye stands params.yzw metres off the
// cube's centre: the point its ray meets is found where |o + d t| equals the distance
// the cube holds in that point's direction (a fixed point, a few steps from the centre's
// own distance - the eyes are centimetres off and the room metres away), and the colour
// is looked up in that point's direction. A texel with no distance is at infinity.
TextureCube roomTex : register(t1);
TextureCube roomDepth : register(t3);
float4 ps_backdrop(VSOut i) : SV_Target {
    float tx = lerp(tans.x, tans.y, i.uv.x);
    float ty = lerp(tans.z, tans.w, i.uv.y);
    float3 d = rotate_by(qInv, normalize(float3(tx, ty, -1)));
    float3 look = d;
    if (params.x > 0.5) {
        float3 o = params.yzw;
        float r = roomDepth.SampleLevel(samp0, d, 0).r;
        if (r > 0.01) {
            float od = dot(o, d), oo = dot(o, o);
            float t = r;
            [unroll] for (int k = 0; k < 4; ++k) {
                float3 p = o + d * t;
                float rp = roomDepth.SampleLevel(samp0, normalize(p), 0).r;
                if (rp <= 0.01) break;
                t = -od + sqrt(max(od * od - oo + rp * rp, 0));
            }
            look = normalize(o + d * t);
        }
    }
    return float4(roomTex.Sample(samp0, look).rgb, 1);
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
ID3D11PixelShader* g_psBackdrop = nullptr;
ID3D11PixelShader* g_psSplatDepth = nullptr;
ID3D11Texture2D* g_cubeDepth = nullptr;      // the room's distances (R32F, metres; 0 = none)
ID3D11RenderTargetView* g_depthRtv[6] = {};
uint32_t g_depthSplats = 0;
ID3D11Texture2D* g_savedDepth = nullptr;     // a saved room's distances
DXGI_FORMAT g_viewFmt = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB; // the typed view of a typeless swapchain image
struct ImageRtv { ID3D11Texture2D* tex; ID3D11RenderTargetView* rtv; };
ImageRtv g_imageRtvs[16] = {}; // the swapchain images the passes wrote (eye and backdrop images)
int g_imageRtvCount = 0;
ID3D11Buffer* g_cb = nullptr;
ID3D11BlendState* g_blend = nullptr;
ID3D11SamplerState* g_sampler = nullptr;
ID3D11RasterizerState* g_raster = nullptr;
ID3D11DepthStencilState* g_depth = nullptr;
ID3D11Texture2D* g_cube = nullptr;
ID3D11RenderTargetView* g_faceRtv[6] = {};
DXGI_FORMAT g_cubeFmt = DXGI_FORMAT_UNKNOWN;
ID3D11Texture2D* g_saved = nullptr; // a room loaded from disk, shown in place of the live cube
float g_circle = 0.42f; // 2026-10-23: 0.16 was "way too small" in the headset; 0.5 touches the frame's top and bottom
ID3D11Texture2D* g_src = nullptr; // a sampleable copy of the eye image
ID3D11ShaderResourceView* g_srcSrv = nullptr;
uint32_t g_srcW = 0, g_srcH = 0;
DXGI_FORMAT g_srcFmt = DXGI_FORMAT_UNKNOWN;
uint32_t g_splats = 0;
bool g_failed = false;

void release_cube() {
    for (auto& r : g_faceRtv) { if (r) r->Release(); r = nullptr; }
    for (auto& r : g_depthRtv) { if (r) r->Release(); r = nullptr; }
    if (g_cube) g_cube->Release();
    if (g_cubeDepth) g_cubeDepth->Release();
    g_cube = nullptr;
    g_cubeDepth = nullptr;
    g_cubeFmt = DXGI_FORMAT_UNKNOWN;
    g_splats = 0;
    g_depthSplats = 0;
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
    ID3DBlob* pbb = nullptr;
    ID3DBlob* err = nullptr;
    bool ok = SUCCEEDED(D3DCompile(kShader, strlen(kShader), nullptr, nullptr, nullptr, "vs_main",
                                   "vs_4_0", 0, 0, &vsb, &err)) &&
              SUCCEEDED(D3DCompile(kShader, strlen(kShader), nullptr, nullptr, nullptr, "ps_main",
                                   "ps_4_0", 0, 0, &psb, &err)) &&
              SUCCEEDED(D3DCompile(kShader, strlen(kShader), nullptr, nullptr, nullptr, "ps_key",
                                   "ps_4_0", 0, 0, &pkb, &err)) &&
              SUCCEEDED(D3DCompile(kShader, strlen(kShader), nullptr, nullptr, nullptr, "ps_backdrop",
                                   "ps_4_0", 0, 0, &pbb, &err));
    ID3DBlob* psd = nullptr;
    if (ok)
        ok = SUCCEEDED(D3DCompile(kShader, strlen(kShader), nullptr, nullptr, nullptr, "ps_splat_depth", "ps_4_0", 0,
                                  0, &psd, &err)) &&
             SUCCEEDED(dev->CreatePixelShader(psd->GetBufferPointer(), psd->GetBufferSize(), nullptr, &g_psSplatDepth));
    if (psd) psd->Release();
    if (ok)
        ok = SUCCEEDED(dev->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(),
                                               nullptr, &g_vs)) &&
             SUCCEEDED(dev->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(),
                                              nullptr, &g_ps)) &&
             SUCCEEDED(dev->CreatePixelShader(pkb->GetBufferPointer(), pkb->GetBufferSize(),
                                              nullptr, &g_psKey)) &&
             SUCCEEDED(dev->CreatePixelShader(pbb->GetBufferPointer(), pbb->GetBufferSize(),
                                              nullptr, &g_psBackdrop));
    if (vsb) vsb->Release();
    if (psb) psb->Release();
    if (pkb) pkb->Release();
    if (pbb) pbb->Release();
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
    // The distances, alongside. A failure leaves the room mono, never missing.
    td.Format = DXGI_FORMAT_R32_FLOAT;
    if (SUCCEEDED(dev->CreateTexture2D(&td, nullptr, &g_cubeDepth))) {
        for (UINT f = 0; f < 6; ++f) {
            D3D11_RENDER_TARGET_VIEW_DESC rv{};
            rv.Format = DXGI_FORMAT_R32_FLOAT;
            rv.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
            rv.Texture2DArray.FirstArraySlice = f;
            rv.Texture2DArray.ArraySize = 1;
            if (FAILED(dev->CreateRenderTargetView(g_cubeDepth, &rv, &g_depthRtv[f]))) {
                for (auto& r : g_depthRtv) { if (r) r->Release(); r = nullptr; }
                g_cubeDepth->Release();
                g_cubeDepth = nullptr;
                BVR_LOG("[cube] the distance cube's views failed - the room stays mono");
                break;
            }
        }
    } else {
        g_cubeDepth = nullptr;
        BVR_LOG("[cube] the distance cube could not be made - the room stays mono");
    }
    g_cubeFmt = fmt;
    return true;
}

// A runtime may hand out TYPELESS swapchain textures (the app picks the view);
// a view of one needs the typed format the swapchain was created with.
bool typeless(DXGI_FORMAT f) {
    return f == DXGI_FORMAT_R8G8B8A8_TYPELESS || f == DXGI_FORMAT_B8G8R8A8_TYPELESS ||
           f == DXGI_FORMAT_R10G10B10A2_TYPELESS || f == DXGI_FORMAT_R16G16B16A16_TYPELESS;
}
DXGI_FORMAT typed(DXGI_FORMAT f) { return typeless(f) ? g_viewFmt : f; }

bool ensure_src(ID3D11Device* dev, const D3D11_TEXTURE2D_DESC& eye) {
    const DXGI_FORMAT fmt = typed(eye.Format);
    if (g_src && g_srcW == eye.Width && g_srcH == eye.Height && g_srcFmt == fmt) return true;
    release_src();
    D3D11_TEXTURE2D_DESC td{};
    td.Width = eye.Width;
    td.Height = eye.Height;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = fmt;
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
    g_srcFmt = fmt;
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
    D3D11_TEXTURE2D_DESC td{};
    image->GetDesc(&td);
    D3D11_RENDER_TARGET_VIEW_DESC rv{};
    rv.Format = typed(td.Format);
    rv.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    const HRESULT hr = dev->CreateRenderTargetView(image, &rv, &rtv);
    dev->Release();
    if (FAILED(hr)) {
        BVR_LOG("[cube] render target view on a swapchain image failed (%08lx, format %d)",
                static_cast<unsigned long>(hr), static_cast<int>(td.Format));
        return nullptr;
    }
    image->AddRef();
    g_imageRtvs[g_imageRtvCount++] = {image, rtv};
    return rtv;
}

} // namespace

bool splat(ID3D11DeviceContext* ctx, ID3D11Texture2D* eyeImage, const float q[4], float tanL,
           float tanR, float tanU, float tanD, DXGI_FORMAT fmt, ID3D11ShaderResourceView* linearZ, float uuPerM) {
    if (!ctx || !eyeImage || !(tanR > tanL) || !(tanU > tanD)) return false;
    if (!prepare(ctx, eyeImage, fmt, true)) return false;
    const bool withDepth = linearZ && uuPerM > 1.0f && g_cubeDepth && g_psSplatDepth;

    SavedState saved(ctx);
    ID3D11ShaderResourceView* oldT2 = nullptr;
    ctx->PSGetShaderResources(2, 1, &oldT2);
    bind_pass(ctx, withDepth ? g_psSplatDepth : g_ps, static_cast<float>(kFaceSize), static_cast<float>(kFaceSize));
    if (withDepth) ctx->PSSetShaderResources(2, 1, &linearZ);

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
    cb.params[2] = uuPerM;
    for (UINT f = 0; f < 6; ++f) {
        cb.params[0] = static_cast<float>(f);
        D3D11_MAPPED_SUBRESOURCE m{};
        if (FAILED(ctx->Map(g_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) break;
        memcpy(m.pData, &cb, sizeof(cb));
        ctx->Unmap(g_cb, 0);
        if (withDepth) {
            ID3D11RenderTargetView* two[2] = {g_faceRtv[f], g_depthRtv[f]};
            ctx->OMSetRenderTargets(2, two, nullptr);
        } else {
            ctx->OMSetRenderTargets(1, &g_faceRtv[f], nullptr);
        }
        ctx->Draw(3, 0);
    }
    ctx->PSSetShaderResources(2, 1, &oldT2);
    if (oldT2) oldT2->Release();
    ++g_splats;
    if (withDepth) ++g_depthSplats;
    return true;
}

bool key_black(ID3D11DeviceContext* ctx, ID3D11Texture2D* image) {
    if (!ctx || !image) return false;
    if (!prepare(ctx, image, DXGI_FORMAT_UNKNOWN, false)) return false;
    ID3D11RenderTargetView* rtv = image_rtv(ctx, image);
    if (!rtv) return false;
    SavedState saved(ctx);
    bind_pass(ctx, g_psKey, static_cast<float>(g_srcW), static_cast<float>(g_srcH));
    SplatCB cb{};
    cb.params[2] = g_circle;
    cb.params[3] = g_srcH ? static_cast<float>(g_srcW) / static_cast<float>(g_srcH) : 1.0f;
    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(ctx->Map(g_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return false;
    memcpy(m.pData, &cb, sizeof(cb));
    ctx->Unmap(g_cb, 0);
    ctx->OMSetRenderTargets(1, &rtv, nullptr);
    ctx->Draw(3, 0);
    return true;
}

void set_view_format(DXGI_FORMAT fmt) {
    if (fmt != DXGI_FORMAT_UNKNOWN && !typeless(fmt)) g_viewFmt = fmt;
}

bool draw_backdrop(ID3D11DeviceContext* ctx, ID3D11Texture2D* dst, ID3D11Texture2D* room, ID3D11Texture2D* roomDepth,
                   const float q[4], float tanL, float tanR, float tanU, float tanD, const float eyeOffsetM[3]) {
    if (!ctx || !dst || !room || !(tanR > tanL) || !(tanU > tanD)) return false;
    ID3D11Device* dev = nullptr;
    ctx->GetDevice(&dev);
    if (!dev) return false;
    const bool pipe = ensure_pipeline(dev);
    D3D11_TEXTURE2D_DESC rd{};
    room->GetDesc(&rd);
    D3D11_SHADER_RESOURCE_VIEW_DESC sv{};
    sv.Format = typed(rd.Format);
    sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURECUBE;
    sv.TextureCube.MipLevels = 1;
    ID3D11ShaderResourceView* srv = nullptr;
    const HRESULT hr = pipe ? dev->CreateShaderResourceView(room, &sv, &srv) : E_FAIL;
    dev->Release();
    if (FAILED(hr) || !srv) {
        static bool logged = false;
        if (!logged) {
            logged = true;
            BVR_LOG("[cube] backdrop: the room's cube view failed (%08lx)", static_cast<unsigned long>(hr));
        }
        return false;
    }
    ID3D11RenderTargetView* rtv = image_rtv(ctx, dst);
    if (!rtv) {
        srv->Release();
        return false;
    }
    D3D11_TEXTURE2D_DESC dd{};
    dst->GetDesc(&dd);
    bool ok = false;
    // The distances, when the room has them: each eye its own parallax.
    ID3D11ShaderResourceView* dsrv = nullptr;
    if (roomDepth && eyeOffsetM) {
        ID3D11Device* d2 = nullptr;
        ctx->GetDevice(&d2);
        D3D11_SHADER_RESOURCE_VIEW_DESC dv{};
        dv.Format = DXGI_FORMAT_R32_FLOAT;
        dv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURECUBE;
        dv.TextureCube.MipLevels = 1;
        if (d2) {
            if (FAILED(d2->CreateShaderResourceView(roomDepth, &dv, &dsrv))) dsrv = nullptr;
            d2->Release();
        }
    }
    {
        SavedState saved(ctx);
        ID3D11ShaderResourceView* oldT1 = nullptr;
        ID3D11ShaderResourceView* oldT3 = nullptr;
        ctx->PSGetShaderResources(1, 1, &oldT1);
        ctx->PSGetShaderResources(3, 1, &oldT3);
        bind_pass(ctx, g_psBackdrop, static_cast<float>(dd.Width), static_cast<float>(dd.Height));
        ctx->PSSetShaderResources(1, 1, &srv);
        ctx->PSSetShaderResources(3, 1, &dsrv);
        SplatCB cb{};
        for (int i = 0; i < 4; ++i) cb.qInv[i] = q[i]; // eye -> world, as the shader wants
        cb.tans[0] = tanL;
        cb.tans[1] = tanR;
        cb.tans[2] = tanU;
        cb.tans[3] = tanD;
        if (dsrv) {
            cb.params[0] = 1.0f;
            cb.params[1] = eyeOffsetM[0];
            cb.params[2] = eyeOffsetM[1];
            cb.params[3] = eyeOffsetM[2];
        }
        D3D11_MAPPED_SUBRESOURCE m{};
        if (SUCCEEDED(ctx->Map(g_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
            memcpy(m.pData, &cb, sizeof(cb));
            ctx->Unmap(g_cb, 0);
            ctx->OMSetRenderTargets(1, &rtv, nullptr);
            ctx->Draw(3, 0);
            ok = true;
        }
        ctx->PSSetShaderResources(1, 1, &oldT1);
        ctx->PSSetShaderResources(3, 1, &oldT3);
        if (oldT1) oldT1->Release();
        if (oldT3) oldT3->Release();
    }
    srv->Release();
    if (dsrv) dsrv->Release();
    return ok;
}

void set_screen_circle(float radiusFrac) { g_circle = radiusFrac < 0.0f ? 0.0f : radiusFrac; }
float screen_circle() { return g_circle; }

static bool download_cube(ID3D11Texture2D* cube, std::vector<uint8_t>& out) {
    out.clear();
    if (!cube) return false;
    ID3D11Device* dev = nullptr;
    cube->GetDevice(&dev);
    if (!dev) return false;
    ID3D11DeviceContext* ctx = nullptr;
    dev->GetImmediateContext(&ctx);
    D3D11_TEXTURE2D_DESC td{};
    cube->GetDesc(&td);
    td.Usage = D3D11_USAGE_STAGING;
    td.BindFlags = 0;
    td.MiscFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D* st = nullptr;
    bool ok = ctx && SUCCEEDED(dev->CreateTexture2D(&td, nullptr, &st)) && st;
    if (ok) {
        ctx->CopyResource(st, cube);
        out.resize(static_cast<size_t>(6) * kFaceSize * kFaceSize * 4);
        for (UINT f = 0; f < 6 && ok; ++f) {
            D3D11_MAPPED_SUBRESOURCE m{};
            ok = SUCCEEDED(ctx->Map(st, f, D3D11_MAP_READ, 0, &m));
            if (!ok) break;
            uint8_t* dst = out.data() + static_cast<size_t>(f) * kFaceSize * kFaceSize * 4;
            for (UINT y = 0; y < kFaceSize; ++y)
                memcpy(dst + static_cast<size_t>(y) * kFaceSize * 4,
                       static_cast<const uint8_t*>(m.pData) + static_cast<size_t>(y) * m.RowPitch, kFaceSize * 4);
            ctx->Unmap(st, f);
        }
    }
    if (st) st->Release();
    if (ctx) ctx->Release();
    dev->Release();
    if (!ok) out.clear();
    return ok;
}

bool download(std::vector<uint8_t>& out) { return download_cube(g_cube, out); }
bool download_depth(std::vector<uint8_t>& out) {
    if (g_depthSplats == 0) {
        out.clear();
        return false;
    }
    return download_cube(g_cubeDepth, out);
}

static bool upload_cube(ID3D11Device* dev, const uint8_t* bytes, DXGI_FORMAT fmt, ID3D11Texture2D** out) {
    D3D11_TEXTURE2D_DESC td{};
    td.Width = td.Height = kFaceSize;
    td.MipLevels = 1;
    td.ArraySize = 6;
    td.Format = fmt;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_IMMUTABLE;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    td.MiscFlags = D3D11_RESOURCE_MISC_TEXTURECUBE;
    D3D11_SUBRESOURCE_DATA init[6] = {};
    for (UINT f = 0; f < 6; ++f) {
        init[f].pSysMem = bytes + static_cast<size_t>(f) * kFaceSize * kFaceSize * 4;
        init[f].SysMemPitch = kFaceSize * 4;
    }
    const HRESULT hr = dev->CreateTexture2D(&td, init, out);
    if (FAILED(hr)) {
        BVR_LOG("[cube] saved room upload failed (%08lx, format %d)", static_cast<unsigned long>(hr), static_cast<int>(fmt));
        *out = nullptr;
        return false;
    }
    return true;
}

bool upload_saved(ID3D11DeviceContext* ctx, const uint8_t* rgba, size_t bytes, DXGI_FORMAT fmt, const uint8_t* depth) {
    clear_saved();
    if (!ctx || !rgba || bytes != static_cast<size_t>(6) * kFaceSize * kFaceSize * 4) return false;
    ID3D11Device* dev = nullptr;
    ctx->GetDevice(&dev);
    if (!dev) return false;
    const bool ok = upload_cube(dev, rgba, fmt, &g_saved);
    if (ok && depth) upload_cube(dev, depth, DXGI_FORMAT_R32_FLOAT, &g_savedDepth); // mono if it fails
    dev->Release();
    return ok;
}

void clear_saved() {
    if (g_saved) g_saved->Release();
    if (g_savedDepth) g_savedDepth->Release();
    g_saved = nullptr;
    g_savedDepth = nullptr;
}
bool has_saved() { return g_saved != nullptr; }
ID3D11Texture2D* shown() { return g_saved ? g_saved : (valid() ? g_cube : nullptr); }
ID3D11Texture2D* shown_depth(bool liveOnly) {
    if (!liveOnly && g_saved) return g_savedDepth;
    return g_depthSplats > 0 ? g_cubeDepth : nullptr;
}
bool has_depth() { return shown_depth(false) != nullptr; }

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
        const float none[4] = {0, 0, 0, 0};
        for (auto* r : g_faceRtv)
            if (r) ctx->ClearRenderTargetView(r, black);
        for (auto* r : g_depthRtv)
            if (r) ctx->ClearRenderTargetView(r, none);
        ctx->Release();
    }
    dev->Release();
    g_splats = 0;
    g_depthSplats = 0;
}

void release() {
    release_cube();
    release_src();
    forget_images();
    clear_saved();
    if (g_cb) g_cb->Release();
    if (g_blend) g_blend->Release();
    if (g_sampler) g_sampler->Release();
    if (g_raster) g_raster->Release();
    if (g_depth) g_depth->Release();
    if (g_psKey) g_psKey->Release();
    if (g_psBackdrop) g_psBackdrop->Release();
    g_psBackdrop = nullptr;
    if (g_psSplatDepth) g_psSplatDepth->Release();
    g_psSplatDepth = nullptr;
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
