#include "core/gfx/blit.h"

#include "core/util/log.h"

#include <d3dcompiler.h>

namespace bvr::blit {
namespace {

const char* kShader = R"(
Texture2D tex0 : register(t0);
SamplerState samp0 : register(s0);
cbuffer StretchCB : register(b0) { float4 uvRemap; } // x = vOffset, y = vScale
struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
VSOut vs_main(uint id : SV_VertexID) {
    // Fullscreen triangle, no vertex buffer.
    VSOut o;
    float2 uv = float2((id << 1) & 2, id & 2);
    o.pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
    o.uv = uv;
    return o;
}
// Session 22: sample only a horizontal BAND of the source, stretched across
// the full destination (the engine-cinematic letterbox unsqueeze).
VSOut vs_stretch(uint id : SV_VertexID) {
    VSOut o;
    float2 uv = float2((id << 1) & 2, id & 2);
    o.pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
    o.uv = float2(uv.x, uvRemap.x + uv.y * uvRemap.y);
    return o;
}
float4 ps_main(VSOut i) : SV_Target {
    return tex0.Sample(samp0, i.uv);
}
// Alpha safety floor: coverage now accumulates correctly (the redirect swaps
// gameswf blend states for alpha-corrected variants), so the luminance term
// is only a low floor against any state that slips through - bright pixels
// can never be fully invisible.
float4 ps_process(VSOut i) : SV_Target {
    float4 c = tex0.Sample(samp0, i.uv);
    float lum = dot(c.rgb, float3(0.299, 0.587, 0.114));
    c.a = max(c.a, saturate(lum * 0.35));
    return c;
}
)";

ID3D11VertexShader* g_vs = nullptr;
ID3D11VertexShader* g_vsStretch = nullptr;
ID3D11Buffer* g_stretchCb = nullptr;
ID3D11PixelShader* g_ps = nullptr;
ID3D11PixelShader* g_psProcess = nullptr;
ID3D11BlendState* g_blend = nullptr;
ID3D11SamplerState* g_sampler = nullptr;
ID3D11RasterizerState* g_raster = nullptr;
ID3D11DepthStencilState* g_depth = nullptr;
bool g_failed = false;

bool ensure_pipeline(ID3D11DeviceContext* ctx) {
    if (g_vs) return true;
    if (g_failed) return false;
    ID3D11Device* dev = nullptr;
    ctx->GetDevice(&dev);
    if (!dev) return false;

    ID3DBlob* vsb = nullptr;
    ID3DBlob* vstb = nullptr;
    ID3DBlob* psb = nullptr;
    ID3DBlob* ppb = nullptr;
    ID3DBlob* err = nullptr;
    bool ok = SUCCEEDED(D3DCompile(kShader, strlen(kShader), nullptr, nullptr, nullptr,
                                   "vs_main", "vs_4_0", 0, 0, &vsb, &err)) &&
              SUCCEEDED(D3DCompile(kShader, strlen(kShader), nullptr, nullptr, nullptr,
                                   "vs_stretch", "vs_4_0", 0, 0, &vstb, &err)) &&
              SUCCEEDED(D3DCompile(kShader, strlen(kShader), nullptr, nullptr, nullptr,
                                   "ps_main", "ps_4_0", 0, 0, &psb, &err)) &&
              SUCCEEDED(D3DCompile(kShader, strlen(kShader), nullptr, nullptr, nullptr,
                                   "ps_process", "ps_4_0", 0, 0, &ppb, &err));
    if (ok)
        ok = SUCCEEDED(dev->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(),
                                               nullptr, &g_vs)) &&
             SUCCEEDED(dev->CreateVertexShader(vstb->GetBufferPointer(), vstb->GetBufferSize(),
                                               nullptr, &g_vsStretch)) &&
             SUCCEEDED(dev->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(),
                                              nullptr, &g_ps)) &&
             SUCCEEDED(dev->CreatePixelShader(ppb->GetBufferPointer(), ppb->GetBufferSize(),
                                              nullptr, &g_psProcess));
    if (ok) {
        D3D11_BUFFER_DESC cbd{};
        cbd.ByteWidth = 16;
        cbd.Usage = D3D11_USAGE_DYNAMIC;
        cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        ok = SUCCEEDED(dev->CreateBuffer(&cbd, nullptr, &g_stretchCb));
    }
    if (vsb) vsb->Release();
    if (vstb) vstb->Release();
    if (psb) psb->Release();
    if (ppb) ppb->Release();
    if (err) {
        BVR_LOG("[blit] shader compile: %s", static_cast<const char*>(err->GetBufferPointer()));
        err->Release();
    }

    if (ok) {
        D3D11_BLEND_DESC bd{};
        bd.RenderTarget[0].BlendEnable = TRUE;
        // Premultiplied: the source rgb is already scaled by coverage.
        bd.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
        bd.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
        bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
        bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
        bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        D3D11_SAMPLER_DESC sd{};
        sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        D3D11_RASTERIZER_DESC rd{};
        rd.FillMode = D3D11_FILL_SOLID;
        rd.CullMode = D3D11_CULL_NONE;
        rd.DepthClipEnable = TRUE;
        D3D11_DEPTH_STENCIL_DESC dd{};
        dd.DepthEnable = FALSE;
        dd.StencilEnable = FALSE;
        ok = SUCCEEDED(dev->CreateBlendState(&bd, &g_blend)) &&
             SUCCEEDED(dev->CreateSamplerState(&sd, &g_sampler)) &&
             SUCCEEDED(dev->CreateRasterizerState(&rd, &g_raster)) &&
             SUCCEEDED(dev->CreateDepthStencilState(&dd, &g_depth));
    }
    dev->Release();
    if (!ok) {
        release();
        g_failed = true; // log once, stay quiet after
        BVR_LOG("[blit] pipeline creation FAILED - window HUD composite disabled");
    }
    return ok;
}

} // namespace

namespace {

bool draw_internal(ID3D11DeviceContext* ctx, ID3D11RenderTargetView* dst,
                   ID3D11ShaderResourceView* src, UINT dstW, UINT dstH,
                   ID3D11PixelShader* ps, ID3D11BlendState* blend,
                   ID3D11VertexShader* vs = nullptr) {
    if (!ctx || !dst || !src || !dstW || !dstH) return false;
    if (!ensure_pipeline(ctx)) return false;
    if (!vs) vs = g_vs;

    // Backup the slices of state we touch (the game re-binds most of this per
    // frame, but the present chain must be transparent regardless).
    ID3D11RenderTargetView* oldRtv = nullptr;
    ID3D11DepthStencilView* oldDsv = nullptr;
    ctx->OMGetRenderTargets(1, &oldRtv, &oldDsv);
    ID3D11BlendState* oldBlend = nullptr;
    FLOAT oldBf[4];
    UINT oldMask = 0;
    ctx->OMGetBlendState(&oldBlend, oldBf, &oldMask);
    ID3D11DepthStencilState* oldDepth = nullptr;
    UINT oldStencilRef = 0;
    ctx->OMGetDepthStencilState(&oldDepth, &oldStencilRef);
    ID3D11RasterizerState* oldRaster = nullptr;
    ctx->RSGetState(&oldRaster);
    UINT nVp = 1;
    D3D11_VIEWPORT oldVp{};
    ctx->RSGetViewports(&nVp, &oldVp);
    ID3D11VertexShader* oldVs = nullptr;
    ID3D11PixelShader* oldPs = nullptr;
    ctx->VSGetShader(&oldVs, nullptr, nullptr);
    ctx->PSGetShader(&oldPs, nullptr, nullptr);
    ID3D11Buffer* oldVsCb = nullptr;
    ctx->VSGetConstantBuffers(0, 1, &oldVsCb); // the engine lives in VS b0
    ID3D11ShaderResourceView* oldSrv = nullptr;
    ctx->PSGetShaderResources(0, 1, &oldSrv);
    ID3D11SamplerState* oldSamp = nullptr;
    ctx->PSGetSamplers(0, 1, &oldSamp);
    ID3D11InputLayout* oldLayout = nullptr;
    ctx->IAGetInputLayout(&oldLayout);
    D3D11_PRIMITIVE_TOPOLOGY oldTopo{};
    ctx->IAGetPrimitiveTopology(&oldTopo);

    D3D11_VIEWPORT vp{};
    vp.Width = static_cast<FLOAT>(dstW);
    vp.Height = static_cast<FLOAT>(dstH);
    vp.MaxDepth = 1.0f;
    ctx->OMSetRenderTargets(1, &dst, nullptr);
    ctx->OMSetBlendState(blend, nullptr, 0xFFFFFFFF);
    ctx->OMSetDepthStencilState(g_depth, 0);
    ctx->RSSetState(g_raster);
    ctx->RSSetViewports(1, &vp);
    ctx->IASetInputLayout(nullptr);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(vs, nullptr, 0);
    if (vs == g_vsStretch) ctx->VSSetConstantBuffers(0, 1, &g_stretchCb);
    ctx->PSSetShader(ps, nullptr, 0);
    ctx->PSSetShaderResources(0, 1, &src);
    ctx->PSSetSamplers(0, 1, &g_sampler);
    ctx->Draw(3, 0);

    ctx->OMSetRenderTargets(1, &oldRtv, oldDsv);
    ctx->OMSetBlendState(oldBlend, oldBf, oldMask);
    ctx->OMSetDepthStencilState(oldDepth, oldStencilRef);
    ctx->RSSetState(oldRaster);
    if (nVp) ctx->RSSetViewports(1, &oldVp);
    ctx->VSSetShader(oldVs, nullptr, 0);
    ctx->VSSetConstantBuffers(0, 1, &oldVsCb);
    ctx->PSSetShader(oldPs, nullptr, 0);
    ctx->PSSetShaderResources(0, 1, &oldSrv);
    ctx->PSSetSamplers(0, 1, &oldSamp);
    ctx->IASetInputLayout(oldLayout);
    ctx->IASetPrimitiveTopology(oldTopo);
    if (oldRtv) oldRtv->Release();
    if (oldDsv) oldDsv->Release();
    if (oldVsCb) oldVsCb->Release();
    if (oldBlend) oldBlend->Release();
    if (oldDepth) oldDepth->Release();
    if (oldRaster) oldRaster->Release();
    if (oldVs) oldVs->Release();
    if (oldPs) oldPs->Release();
    if (oldSrv) oldSrv->Release();
    if (oldSamp) oldSamp->Release();
    if (oldLayout) oldLayout->Release();
    return true;
}

} // namespace

bool alpha_premul(ID3D11DeviceContext* ctx, ID3D11RenderTargetView* dst,
                  ID3D11ShaderResourceView* src, UINT dstW, UINT dstH) {
    return draw_internal(ctx, dst, src, dstW, dstH, g_ps, g_blend);
}

bool process(ID3D11DeviceContext* ctx, ID3D11RenderTargetView* dst,
             ID3D11ShaderResourceView* src, UINT dstW, UINT dstH) {
    // Blend OFF (null state = replace): rgb passes through, alpha repaired.
    return draw_internal(ctx, dst, src, dstW, dstH, g_psProcess, nullptr);
}

bool stretch_band(ID3D11DeviceContext* ctx, ID3D11RenderTargetView* dst,
                  ID3D11ShaderResourceView* src, UINT dstW, UINT dstH,
                  float topFrac, float heightFrac) {
    if (!ensure_pipeline(ctx)) return false;
    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(ctx->Map(g_stretchCb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return false;
    float* f = static_cast<float*>(m.pData);
    f[0] = topFrac;    // vOffset
    f[1] = heightFrac; // vScale
    f[2] = 0.0f;
    f[3] = 0.0f;
    ctx->Unmap(g_stretchCb, 0);
    // Blend OFF (replace): the band overwrites the whole destination.
    return draw_internal(ctx, dst, src, dstW, dstH, g_ps, nullptr, g_vsStretch);
}

void release() {
    if (g_vs) { g_vs->Release(); g_vs = nullptr; }
    if (g_vsStretch) { g_vsStretch->Release(); g_vsStretch = nullptr; }
    if (g_stretchCb) { g_stretchCb->Release(); g_stretchCb = nullptr; }
    if (g_ps) { g_ps->Release(); g_ps = nullptr; }
    if (g_psProcess) { g_psProcess->Release(); g_psProcess = nullptr; }
    if (g_blend) { g_blend->Release(); g_blend = nullptr; }
    if (g_sampler) { g_sampler->Release(); g_sampler = nullptr; }
    if (g_raster) { g_raster->Release(); g_raster = nullptr; }
    if (g_depth) { g_depth->Release(); g_depth = nullptr; }
}

} // namespace bvr::blit
