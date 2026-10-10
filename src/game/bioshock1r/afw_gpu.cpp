// See afw_gpu.h.
#include "game/bioshock1r/afw_gpu.h"

#include <d3d11.h>
#include <d3dcompiler.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace bvr::b1r::afw {
namespace {

template <class T> void rel(T*& p) { if (p) { p->Release(); p = nullptr; } }
void put(char* why, size_t cap, const char* fmt, ...) {
    if (!why || !cap) return;
    va_list a; va_start(a, fmt);
    _vsnprintf_s(why, cap, _TRUNCATE, fmt, a);
    va_end(a);
}

const char* kSrc = R"(
cbuffer W : register(b0) {
    float4 srcLocSky;       // xyz source camera, w sky distance (uu)
    float4 srcFwdTanH;
    float4 srcRightTanV;
    float4 srcUpGrid;       // w mesh cell (texels)
    float4 tgtLocFar;       // w z-buffer far (uu)
    float4 tgtFwdTanH;
    float4 tgtRightTanV;
    float4 tgtUpEdgeRel;    // w discontinuity ratio
    float4 othLocStaleRel;  // the other source, for the stale test; w stale ratio
    float4 othFwdTanH;
    float4 othRightTanV;
    float4 othUpStaleAbs;   // w stale absolute (uu)
    float4 dims;            // source depth w, h, cells x, cells y
    float4 othDims;         // other depth w, h, stale test on, discontinuity absolute (uu)
    float4 compose;         // nearer ratio, tint, use A, use B
    float4 outDims;         // w, h
};

Texture2D<float> srcZ : register(t0);
Texture2D<float4> srcC : register(t1);
Texture2D<float> othZ : register(t2);
SamplerState lin : register(s0);
SamplerState pnt : register(s1);

struct V {
    float4 pos : SV_Position;
    float2 suv : TEXCOORD0;
    float3 world : TEXCOORD1;
    float tz : TEXCOORD2;
    float stretched : TEXCOORD3;   // 1: this cell spans a depth discontinuity
};

float zfix(float z) { return z > 0.0f ? z : srcLocSky.w; }
float zat(int2 p) {
    p = clamp(p, int2(0, 0), int2(dims.xy) - 1);
    return zfix(srcZ.Load(int3(p, 0)));
}

V vsmesh(uint vid : SV_VertexID) {
    const int2 C[6] = { int2(0, 0), int2(1, 0), int2(0, 1), int2(1, 0), int2(1, 1), int2(0, 1) };
    uint cell = vid / 6;
    uint k = vid - cell * 6;
    uint cw = (uint)dims.z;
    int2 c = int2(cell % cw, cell / cw);
    int g = (int)srcUpGrid.w;
    int2 p0 = c * g;
    V o;
    o.pos = float4(0, 0, -1, 1);   // culled (z < 0) unless set below
    o.suv = float2(0, 0); o.world = float3(0, 0, 0); o.tz = 0; o.stretched = 0;
    float z00 = zat(p0), z10 = zat(p0 + int2(g, 0)), z01 = zat(p0 + int2(0, g)), z11 = zat(p0 + int2(g, g));
    float zmin = min(min(z00, z10), min(z01, z11)), zmax = max(max(z00, z10), max(z01, z11));
    // A cell across a depth discontinuity is drawn, but only as a last resort: at the far
    // end of the z-buffer (it never hides a real surface of this source) and flagged, so
    // the other source's real surface wins over it in the compose. Dropping it instead
    // leaves a hole along every silhouette that only a neighbour search can fill.
    const bool edge = zmax > zmin * (1.0f + tgtUpEdgeRel.w) + othDims.w;
    int2 node = min(p0 + C[k] * g, int2(dims.xy) - 1);
    float z = zat(node);
    float2 uv = (float2(node) + 0.5f) / dims.xy;
    float x = uv.x * 2.0f - 1.0f, y = 1.0f - uv.y * 2.0f;
    float3 d = srcFwdTanH.xyz + srcRightTanV.xyz * (x * srcFwdTanH.w) + srcUpGrid.xyz * (y * srcRightTanV.w);
    float3 w = srcLocSky.xyz + d * z;
    float3 q = w - tgtLocFar.xyz;
    float tz = dot(q, tgtFwdTanH.xyz);
    if (tz < 0.5f) return o;
    o.pos = float4(dot(q, tgtRightTanV.xyz) / tgtFwdTanH.w, dot(q, tgtUpEdgeRel.xyz) / tgtRightTanV.w,
                   edge ? tz * 0.9999f : tz * tz / tgtLocFar.w, tz);
    o.suv = uv;
    o.world = d * z;   // relative to the source camera: precision, and the PS reprojects it

    o.tz = tz;
    o.stretched = edge ? 1.0f : 0.0f;
    return o;
}

struct PO { float4 c : SV_Target0; float z : SV_Target1; };

PO psmesh(V i) {
    // The source texel this pixel shows: the interpolated surface point projected back
    // into the source camera. Interpolating the source UV itself is wrong - the
    // rasterizer's perspective-correct interpolation is linear in WORLD space, the UV is
    // not - and across a depth step that skews the colour onto the wrong side of the edge.
    float2 suv = i.suv;
    float sz = dot(i.world, srcFwdTanH.xyz);
    if (sz > 1e-3f)
        suv = float2(dot(i.world, srcRightTanV.xyz) / (sz * srcFwdTanH.w) * 0.5f + 0.5f,
                     0.5f - dot(i.world, srcUpGrid.xyz) / (sz * srcRightTanV.w) * 0.5f);
    float3 world = srcLocSky.xyz + i.world;
    if (othDims.z > 0.5f) {
        // The stale test: does the other (fresh) eye see THROUGH this point now?
        float3 q = world - othLocStaleRel.xyz;
        float oz = dot(q, othFwdTanH.xyz);
        if (oz > 0.5f) {
            float2 ouv = float2(dot(q, othRightTanV.xyz) / (oz * othFwdTanH.w) * 0.5f + 0.5f,
                                0.5f - dot(q, othUpStaleAbs.xyz) / (oz * othRightTanV.w) * 0.5f);
            if (all(ouv > 0.002f) && all(ouv < 0.998f)) {
                float4 g4 = othZ.GatherRed(pnt, ouv);
                g4 = g4 > 0.0f ? g4 : srcLocSky.w;
                // The NEAREST of the four: a silhouette texel never condemns a point.
                float zo = min(min(g4.x, g4.y), min(g4.z, g4.w));
                if (zo > oz * (1.0f + othLocStaleRel.w) + othUpStaleAbs.w) discard;
            }
        }
    }
    PO o;
    o.c = srcC.SampleLevel(lin, suv, 0);
    o.c.a = 1.0f;
    // Negative = a stretched (discontinuity) pixel: valid, but the last choice.
    o.z = i.stretched > 0.5f ? -i.tz : i.tz;
    return o;
}

Texture2D<float4> colA : register(t0);
Texture2D<float> lzA : register(t1);
Texture2D<float4> colB : register(t2);
Texture2D<float> lzB : register(t3);

float4 vsfull(uint id : SV_VertexID) : SV_Position {
    float2 p = float2((id << 1) & 2, id & 2);
    return float4(p * float2(2, -2) + float2(-1, 1), 0, 1);
}

float4 pscompose(float4 pos : SV_Position) : SV_Target {
    int2 p = int2(pos.xy);
    int2 hi = int2(outDims.xy) - 1;
    bool useA = compose.z > 0.5f, useB = compose.w > 0.5f;
    float za = useA ? lzA.Load(int3(p, 0)) : 0.0f;
    float zb = useB ? lzB.Load(int3(p, 0)) : 0.0f;
    float4 c = float4(0, 0, 0, 1);
    int cls = 0;
    const bool goodA = za > 0.0f, goodB = zb > 0.0f;   // real surfaces (negative = stretched)
    if (goodA && goodB) {
        if (zb < za * (1.0f - compose.x)) { c = colB.Load(int3(p, 0)); cls = 1; }
        else { c = colA.Load(int3(p, 0)); cls = 0; }
    } else if (goodA) {
        c = colA.Load(int3(p, 0)); cls = 0;
    } else if (goodB) {
        c = colB.Load(int3(p, 0)); cls = 1;
    } else if (za < 0.0f) {
        c = colA.Load(int3(p, 0)); cls = 0;
    } else if (zb < 0.0f) {
        c = colB.Load(int3(p, 0)); cls = 1;
    } else {
        // Neither source has this pixel (a disocclusion both missed, or the frame edge):
        // the FARTHEST valid neighbour, nearest radius first - holes open onto background.
        const int2 D[8] = { int2(1, 0), int2(-1, 0), int2(0, 1), int2(0, -1),
                            int2(1, 1), int2(-1, 1), int2(1, -1), int2(-1, -1) };
        float best = 0.0f;
        cls = 2;
        [loop] for (int r = 1; r <= 64 && best <= 0.0f; r *= 2) {
            [unroll] for (int k = 0; k < 8; ++k) {
                int2 q = clamp(p + D[k] * r, int2(0, 0), hi);
                float qa = useA ? abs(lzA.Load(int3(q, 0))) : 0.0f;
                float qb = useB ? abs(lzB.Load(int3(q, 0))) : 0.0f;
                if (qa > best) { best = qa; c = colA.Load(int3(q, 0)); }
                if (qb > best) { best = qb; c = colB.Load(int3(q, 0)); }
            }
        }
        if (best <= 0.0f) cls = 3;
    }
    if (compose.y > 0.5f) {
        const float3 T[4] = { float3(1, 1, 1), float3(0.6, 0.7, 1.4), float3(1.4, 0.5, 1.4), float3(1.5, 0.4, 0.4) };
        c.rgb *= T[cls];
    }
    c.a = 1.0f;
    return c;
}
)";

struct alignas(16) CB {
    float srcLocSky[4], srcFwdTanH[4], srcRightTanV[4], srcUpGrid[4];
    float tgtLocFar[4], tgtFwdTanH[4], tgtRightTanV[4], tgtUpEdgeRel[4];
    float othLocStaleRel[4], othFwdTanH[4], othRightTanV[4], othUpStaleAbs[4];
    float dims[4], othDims[4], compose[4], outDims[4];
};
static_assert(sizeof(CB) == 16 * 16, "CB layout");

void set4(float* d, const float* s, float w) { d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = w; }

const float kFarUu = 400000.0f;   // z-buffer encoding range (the sky sits at skyUu < this)

ID3DBlob* compile_blob(const char* entry, const char* model, char* why, size_t cap) {
    ID3DBlob* code = nullptr; ID3DBlob* err = nullptr;
    HRESULT hr = D3DCompile(kSrc, strlen(kSrc), "b1r_afw_gpu", nullptr, nullptr, entry, model,
                            D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &err);
    if (FAILED(hr)) {
        put(why, cap, "%s compile: %s", entry, err ? (const char*)err->GetBufferPointer() : "?");
        rel(err); rel(code); return nullptr;
    }
    rel(err);
    return code;
}
bool compile(ID3D11Device* dev, const char* entry, const char* model, ID3D11VertexShader** out, char* why, size_t cap) {
    ID3DBlob* code = compile_blob(entry, model, why, cap);
    if (!code) return false;
    const HRESULT hr = dev->CreateVertexShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, out);
    rel(code);
    if (FAILED(hr)) { put(why, cap, "%s create 0x%08lX", entry, (unsigned long)hr); return false; }
    return true;
}
bool compile(ID3D11Device* dev, const char* entry, const char* model, ID3D11PixelShader** out, char* why, size_t cap) {
    ID3DBlob* code = compile_blob(entry, model, why, cap);
    if (!code) return false;
    const HRESULT hr = dev->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, out);
    rel(code);
    if (FAILED(hr)) { put(why, cap, "%s create 0x%08lX", entry, (unsigned long)hr); return false; }
    return true;
}

// Every piece of pipeline state these passes set, saved and put back.
struct SavedState {
    ID3D11DeviceContext* ctx;
    ID3D11RenderTargetView* rtv[2] = {};
    ID3D11DepthStencilView* dsv = nullptr;
    ID3D11BlendState* blend = nullptr; FLOAT bf[4] = {}; UINT mask = 0;
    ID3D11DepthStencilState* ds = nullptr; UINT sref = 0;
    ID3D11RasterizerState* rs = nullptr;
    D3D11_VIEWPORT vp[1] = {}; UINT nvp = 1;
    ID3D11VertexShader* vs = nullptr; ID3D11PixelShader* ps = nullptr;
    ID3D11GeometryShader* gs = nullptr; ID3D11HullShader* hs = nullptr; ID3D11DomainShader* dsh = nullptr;
    ID3D11Buffer* vcb = nullptr; ID3D11Buffer* pcb = nullptr;
    ID3D11ShaderResourceView* vsrv[3] = {}; ID3D11ShaderResourceView* psrv[4] = {};
    ID3D11SamplerState* vsmp[2] = {}; ID3D11SamplerState* psmp[2] = {};
    ID3D11InputLayout* il = nullptr; D3D11_PRIMITIVE_TOPOLOGY topo{};
    explicit SavedState(ID3D11DeviceContext* c) : ctx(c) {
        ctx->OMGetRenderTargets(2, rtv, &dsv);
        ctx->OMGetBlendState(&blend, bf, &mask);
        ctx->OMGetDepthStencilState(&ds, &sref);
        ctx->RSGetState(&rs);
        ctx->RSGetViewports(&nvp, vp);
        ctx->VSGetShader(&vs, nullptr, nullptr); ctx->PSGetShader(&ps, nullptr, nullptr);
        ctx->GSGetShader(&gs, nullptr, nullptr); ctx->HSGetShader(&hs, nullptr, nullptr); ctx->DSGetShader(&dsh, nullptr, nullptr);
        ctx->VSGetConstantBuffers(0, 1, &vcb); ctx->PSGetConstantBuffers(0, 1, &pcb);
        ctx->VSGetShaderResources(0, 3, vsrv); ctx->PSGetShaderResources(0, 4, psrv);
        ctx->VSGetSamplers(0, 2, vsmp); ctx->PSGetSamplers(0, 2, psmp);
        ctx->IAGetInputLayout(&il); ctx->IAGetPrimitiveTopology(&topo);
    }
    ~SavedState() {
        ID3D11ShaderResourceView* none[4] = {};
        ctx->VSSetShaderResources(0, 3, none); ctx->PSSetShaderResources(0, 4, none);
        ctx->OMSetRenderTargets(2, rtv, dsv);
        ctx->OMSetBlendState(blend, bf, mask);
        ctx->OMSetDepthStencilState(ds, sref);
        ctx->RSSetState(rs);
        if (nvp) ctx->RSSetViewports(nvp, vp);
        ctx->VSSetShader(vs, nullptr, 0); ctx->PSSetShader(ps, nullptr, 0);
        ctx->GSSetShader(gs, nullptr, 0); ctx->HSSetShader(hs, nullptr, 0); ctx->DSSetShader(dsh, nullptr, 0);
        ctx->VSSetConstantBuffers(0, 1, &vcb); ctx->PSSetConstantBuffers(0, 1, &pcb);
        ctx->VSSetShaderResources(0, 3, vsrv); ctx->PSSetShaderResources(0, 4, psrv);
        ctx->VSSetSamplers(0, 2, vsmp); ctx->PSSetSamplers(0, 2, psmp);
        ctx->IASetInputLayout(il); ctx->IASetPrimitiveTopology(topo);
        for (auto*& r : rtv) rel(r);
        rel(dsv); rel(blend); rel(ds); rel(rs); rel(vs); rel(ps); rel(gs); rel(hs); rel(dsh);
        rel(vcb); rel(pcb); rel(il);
        for (auto*& s : vsrv) rel(s);
        for (auto*& s : psrv) rel(s);
        for (auto*& s : vsmp) rel(s);
        for (auto*& s : psmp) rel(s);
    }
};

} // namespace

bool WarpGpu::init(ID3D11Device* dev, char* why, size_t cap) {
    if (ready_) return true;
    bool ok = compile(dev, "vsmesh", "vs_5_0", &vsMesh_, why, cap) &&
              compile(dev, "psmesh", "ps_5_0", &psMesh_, why, cap) &&
              compile(dev, "vsfull", "vs_5_0", &vsFull_, why, cap) &&
              compile(dev, "pscompose", "ps_5_0", &psCompose_, why, cap);
    if (!ok) { shutdown(); return false; }
    D3D11_BUFFER_DESC bd = {};
    bd.ByteWidth = sizeof(CB); bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER; bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    D3D11_SAMPLER_DESC sl = {};
    sl.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sl.AddressU = sl.AddressV = sl.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sl.MaxLOD = D3D11_FLOAT32_MAX;
    D3D11_SAMPLER_DESC sp = sl;
    sp.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    D3D11_RASTERIZER_DESC rd = {};
    rd.FillMode = D3D11_FILL_SOLID; rd.CullMode = D3D11_CULL_NONE; rd.DepthClipEnable = TRUE;
    D3D11_BLEND_DESC bl = {};
    bl.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    bl.RenderTarget[1].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    bl.IndependentBlendEnable = FALSE;
    D3D11_DEPTH_STENCIL_DESC dt = {};
    dt.DepthEnable = TRUE; dt.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL; dt.DepthFunc = D3D11_COMPARISON_LESS;
    D3D11_DEPTH_STENCIL_DESC dn = {};
    ok = SUCCEEDED(dev->CreateBuffer(&bd, nullptr, &cb_)) &&
         SUCCEEDED(dev->CreateSamplerState(&sl, &linear_)) &&
         SUCCEEDED(dev->CreateSamplerState(&sp, &point_)) &&
         SUCCEEDED(dev->CreateRasterizerState(&rd, &raster_)) &&
         SUCCEEDED(dev->CreateBlendState(&bl, &blend_)) &&
         SUCCEEDED(dev->CreateDepthStencilState(&dt, &dsTest_)) &&
         SUCCEEDED(dev->CreateDepthStencilState(&dn, &dsOff_));
    if (!ok) { put(why, cap, "pipeline state"); shutdown(); return false; }
    ready_ = true;
    return true;
}

void WarpGpu::release_targets() {
    for (int i = 0; i < 2; ++i) {
        rel(colRtv_[i]); rel(colSrv_[i]); rel(col_[i]);
        rel(lzRtv_[i]); rel(lzSrv_[i]); rel(lz_[i]);
    }
    rel(zbDsv_); rel(zb_);
    rel(outRtv_); rel(out_);
    w_ = h_ = 0;
}

void WarpGpu::shutdown() {
    release_targets();
    rel(vsMesh_); rel(psMesh_); rel(vsFull_); rel(psCompose_);
    rel(cb_); rel(linear_); rel(point_); rel(raster_); rel(blend_); rel(dsTest_); rel(dsOff_);
    ready_ = false;
}

bool WarpGpu::ensure(ID3D11Device* dev, uint32_t w, uint32_t h, char* why, size_t cap) {
    if (w == w_ && h == h_ && out_) return true;
    release_targets();
    D3D11_TEXTURE2D_DESC d = {};
    d.Width = w; d.Height = h; d.MipLevels = 1; d.ArraySize = 1;
    d.SampleDesc.Count = 1; d.Usage = D3D11_USAGE_DEFAULT;
    bool ok = true;
    for (int i = 0; ok && i < 2; ++i) {
        d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        ok = SUCCEEDED(dev->CreateTexture2D(&d, nullptr, &col_[i])) &&
             SUCCEEDED(dev->CreateRenderTargetView(col_[i], nullptr, &colRtv_[i])) &&
             SUCCEEDED(dev->CreateShaderResourceView(col_[i], nullptr, &colSrv_[i]));
        d.Format = DXGI_FORMAT_R32_FLOAT;
        ok = ok && SUCCEEDED(dev->CreateTexture2D(&d, nullptr, &lz_[i])) &&
             SUCCEEDED(dev->CreateRenderTargetView(lz_[i], nullptr, &lzRtv_[i])) &&
             SUCCEEDED(dev->CreateShaderResourceView(lz_[i], nullptr, &lzSrv_[i]));
    }
    d.Format = DXGI_FORMAT_D32_FLOAT;
    d.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    ok = ok && SUCCEEDED(dev->CreateTexture2D(&d, nullptr, &zb_)) &&
         SUCCEEDED(dev->CreateDepthStencilView(zb_, nullptr, &zbDsv_));
    d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    ok = ok && SUCCEEDED(dev->CreateTexture2D(&d, nullptr, &out_)) &&
         SUCCEEDED(dev->CreateRenderTargetView(out_, nullptr, &outRtv_));
    if (!ok) { put(why, cap, "warp targets %ux%u", w, h); release_targets(); return false; }
    w_ = w; h_ = h;
    return true;
}

bool WarpGpu::mesh(ID3D11DeviceContext* ctx, const Source& src, const Source& other, const Params& p, int which,
                   bool staleTest) {
    const float zero4[4] = {0, 0, 0, 0};
    ctx->ClearRenderTargetView(colRtv_[which], zero4);
    ctx->ClearRenderTargetView(lzRtv_[which], zero4);
    ctx->ClearDepthStencilView(zbDsv_, D3D11_CLEAR_DEPTH, 1.0f, 0);
    if (!src.valid || !src.color || !src.depth || !src.depthW || !src.depthH) return false;
    const int g = p.grid < 1 ? 1 : p.grid > 16 ? 16 : p.grid;
    const uint32_t cx = (src.depthW - 1 + g - 1) / g, cy = (src.depthH - 1 + g - 1) / g;
    CB cb = {};
    set4(cb.srcLocSky, src.cam.loc, p.skyUu);
    set4(cb.srcFwdTanH, src.cam.fwd, src.cam.tanH);
    set4(cb.srcRightTanV, src.cam.right, src.cam.tanV);
    set4(cb.srcUpGrid, src.cam.up, (float)g);
    set4(cb.tgtLocFar, p.target.loc, kFarUu);
    set4(cb.tgtFwdTanH, p.target.fwd, p.target.tanH);
    set4(cb.tgtRightTanV, p.target.right, p.target.tanV);
    set4(cb.tgtUpEdgeRel, p.target.up, p.edgeRel);
    set4(cb.othLocStaleRel, other.cam.loc, p.staleRel);
    set4(cb.othFwdTanH, other.cam.fwd, other.cam.tanH);
    set4(cb.othRightTanV, other.cam.right, other.cam.tanV);
    set4(cb.othUpStaleAbs, other.cam.up, p.staleAbsUu);
    cb.dims[0] = (float)src.depthW; cb.dims[1] = (float)src.depthH; cb.dims[2] = (float)cx; cb.dims[3] = (float)cy;
    cb.othDims[0] = (float)other.depthW; cb.othDims[1] = (float)other.depthH;
    cb.othDims[2] = (staleTest && other.valid && other.depth) ? 1.0f : 0.0f;
    cb.othDims[3] = p.edgeAbsUu;
    cb.outDims[0] = (float)p.w; cb.outDims[1] = (float)p.h;
    D3D11_MAPPED_SUBRESOURCE m = {};
    if (FAILED(ctx->Map(cb_, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return false;
    memcpy(m.pData, &cb, sizeof(cb));
    ctx->Unmap(cb_, 0);
    ID3D11RenderTargetView* rtvs[2] = {colRtv_[which], lzRtv_[which]};
    ctx->OMSetRenderTargets(2, rtvs, zbDsv_);
    ctx->OMSetDepthStencilState(dsTest_, 0);
    ID3D11ShaderResourceView* vsrv[1] = {src.depth};
    ID3D11ShaderResourceView* psrv[3] = {nullptr, src.color, other.depth};
    ID3D11SamplerState* smp[2] = {linear_, point_};
    ctx->VSSetShader(vsMesh_, nullptr, 0);
    ctx->PSSetShader(psMesh_, nullptr, 0);
    ctx->VSSetConstantBuffers(0, 1, &cb_);
    ctx->PSSetConstantBuffers(0, 1, &cb_);
    ctx->VSSetShaderResources(0, 1, vsrv);
    ctx->PSSetShaderResources(0, 3, psrv);
    ctx->PSSetSamplers(0, 2, smp);
    ctx->Draw(cx * cy * 6, 0);
    ID3D11ShaderResourceView* none[3] = {};
    ctx->VSSetShaderResources(0, 1, none);
    ctx->PSSetShaderResources(0, 3, none);
    return true;
}

bool WarpGpu::run(ID3D11Device* dev, ID3D11DeviceContext* ctx, const Source& held, const Source& fresh,
                  const Params& p, char* why, size_t cap) {
    if (!ready_ && !init(dev, why, cap)) return false;
    if (!p.w || !p.h) { put(why, cap, "no size"); return false; }
    if (!held.valid && !fresh.valid) { put(why, cap, "no source"); return false; }
    if (!ensure(dev, p.w, p.h, why, cap)) return false;
    SavedState saved(ctx);
    D3D11_VIEWPORT vp = {0, 0, (float)p.w, (float)p.h, 0, 1};
    ctx->RSSetViewports(1, &vp);
    ctx->RSSetState(raster_);
    ctx->OMSetBlendState(blend_, nullptr, 0xFFFFFFFF);
    ctx->IASetInputLayout(nullptr);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->GSSetShader(nullptr, nullptr, 0);
    ctx->HSSetShader(nullptr, nullptr, 0);
    ctx->DSSetShader(nullptr, nullptr, 0);
    const bool haveA = p.useHeld && held.valid && mesh(ctx, held, fresh, p, 0, p.staleTest);
    if (!haveA && !p.useHeld) mesh(ctx, Source{}, fresh, p, 0, false);   // clears A
    const bool haveB = p.useFresh && fresh.valid && mesh(ctx, fresh, held, p, 1, false);
    if (!haveB && !p.useFresh) mesh(ctx, Source{}, held, p, 1, false);   // clears B
    // Compose into the output.
    CB cb = {};
    cb.compose[0] = p.nearerRel; cb.compose[1] = p.tint ? 1.0f : 0.0f;
    cb.compose[2] = haveA ? 1.0f : 0.0f; cb.compose[3] = haveB ? 1.0f : 0.0f;
    cb.outDims[0] = (float)p.w; cb.outDims[1] = (float)p.h;
    D3D11_MAPPED_SUBRESOURCE m = {};
    if (FAILED(ctx->Map(cb_, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) { put(why, cap, "map"); return false; }
    memcpy(m.pData, &cb, sizeof(cb));
    ctx->Unmap(cb_, 0);
    ctx->OMSetRenderTargets(1, &outRtv_, nullptr);
    ctx->OMSetDepthStencilState(dsOff_, 0);
    ID3D11ShaderResourceView* srv[4] = {colSrv_[0], lzSrv_[0], colSrv_[1], lzSrv_[1]};
    ctx->VSSetShader(vsFull_, nullptr, 0);
    ctx->PSSetShader(psCompose_, nullptr, 0);
    ctx->PSSetConstantBuffers(0, 1, &cb_);
    ctx->PSSetShaderResources(0, 4, srv);
    ctx->Draw(3, 0);
    ++stats.runs;
    return true;
}

} // namespace bvr::b1r::afw
