// ============================================================================
//  ovrshim_render.cpp - per-eye compositor: OpenXR layers -> OpenVR Submit.
//
//  Adapted from BioVRDev/Bioshock-Remastered-VR OpenXRShim/src/shim_render.cpp
//  with the author's permission (see THIRD_PARTY_NOTICES.md). Delta vs donor:
//  the eye targets are re-created whenever g_st.rtW/rtH outgrew them (the BS2
//  mid-session resolution change re-creates the mod's eye swapchains at a new
//  size; the donor sized once and then silently dropped mismatched copies,
//  which presented as a black headset with a clean log).
//
//  Every layer is drawn as a textured quad into a per-eye render target that
//  uses the HMD's real frustum (GetProjectionRaw) viewed from the current
//  WaitGetPoses eye pose. Clip-space corner positions are computed on the CPU,
//  so the shaders are trivial pass-throughs and the whole pipeline state fits
//  in a dozen objects. Runs on the game's immediate context inside the mod's
//  Present hook, so full state save/restore around our draws.
// ============================================================================
#include "ovrshim.h"
#include <cmath>
#include <cstring>

// D3DCompile, loaded dynamically so we link against nothing extra.
typedef HRESULT(WINAPI* PFN_D3DCompile)(
    LPCVOID, SIZE_T, LPCSTR, const void*, void*, LPCSTR, LPCSTR,
    UINT, UINT, void**, void**);

// Minimal ID3DBlob shape (avoids pulling in d3dcompiler headers).
struct MiniBlob : public IUnknown
{
    virtual LPVOID STDMETHODCALLTYPE GetBufferPointer() = 0;
    virtual SIZE_T STDMETHODCALLTYPE GetBufferSize() = 0;
};

static const char* kHlsl = R"(
struct VSIn  { float4 pos : POSITION; float2 uv : TEXCOORD0; };
struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
VSOut vsmain(VSIn i) { VSOut o; o.pos = i.pos; o.uv = i.uv; return o; }
Texture2D    tex0 : register(t0);
SamplerState smp0 : register(s0);
float4 psmain(VSOut i) : SV_Target { return tex0.Sample(smp0, i.uv); }
)";

struct Vtx { float px, py, pz, pw, u, v; };

static bool                     g_rInit = false;
static ID3D11VertexShader*      g_vs = nullptr;
static ID3D11PixelShader*       g_ps = nullptr;
static ID3D11InputLayout*       g_il = nullptr;
static ID3D11Buffer*            g_vb = nullptr;
static ID3D11SamplerState*      g_samp = nullptr;
static ID3D11BlendState*        g_blendOff = nullptr;
static ID3D11BlendState*        g_blendAlpha = nullptr;
static ID3D11BlendState*        g_blendPremult = nullptr;
static ID3D11RasterizerState*   g_rast = nullptr;
static ID3D11DepthStencilState* g_dss = nullptr;
static ID3D11Texture2D*         g_eyeTex[2][2] = {};   // [parity][eye]
static ID3D11RenderTargetView*  g_eyeRtv[2][2] = {};
static uint32_t                 g_eyeW = 0, g_eyeH = 0; // size the targets were built at

static bool CreateEyeTargets()
{
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = g_st.rtW;
    td.Height = g_st.rtH;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    for (int p = 0; p < 2; ++p)
        for (int e = 0; e < 2; ++e)
        {
            if (FAILED(g_st.dev->CreateTexture2D(&td, nullptr, &g_eyeTex[p][e])) ||
                FAILED(g_st.dev->CreateRenderTargetView(g_eyeTex[p][e], nullptr, &g_eyeRtv[p][e])))
            {
                SLOG("!!! render: eye RT create failed (%ux%u)", g_st.rtW, g_st.rtH);
                return false;
            }
        }
    g_eyeW = g_st.rtW;
    g_eyeH = g_st.rtH;
    return true;
}

static void ReleaseEyeTargets()
{
    for (int p = 0; p < 2; ++p)
        for (int e = 0; e < 2; ++e)
        {
            if (g_eyeRtv[p][e]) { g_eyeRtv[p][e]->Release(); g_eyeRtv[p][e] = nullptr; }
            if (g_eyeTex[p][e]) { g_eyeTex[p][e]->Release(); g_eyeTex[p][e] = nullptr; }
        }
    g_eyeW = g_eyeH = 0;
}

bool Render_Init()
{
    if (g_rInit) return true;
    if (!g_st.dev || !g_st.rtW) return false;

    HMODULE dc = LoadLibraryA("d3dcompiler_47.dll");
    if (!dc) dc = LoadLibraryA("d3dcompiler_43.dll");
    if (!dc) { SLOG("!!! render: no d3dcompiler dll"); return false; }
    PFN_D3DCompile compile = (PFN_D3DCompile)GetProcAddress(dc, "D3DCompile");
    if (!compile) { SLOG("!!! render: no D3DCompile export"); return false; }

    MiniBlob* vsb = nullptr; MiniBlob* psb = nullptr; MiniBlob* err = nullptr;
    HRESULT hr = compile(kHlsl, strlen(kHlsl), nullptr, nullptr, nullptr,
                         "vsmain", "vs_4_0", 0, 0, (void**)&vsb, (void**)&err);
    if (FAILED(hr))
    {
        SLOG("!!! render: VS compile 0x%08X %s", (unsigned)hr,
             err ? (const char*)err->GetBufferPointer() : "");
        if (err) err->Release();
        return false;
    }
    if (err) { err->Release(); err = nullptr; }
    hr = compile(kHlsl, strlen(kHlsl), nullptr, nullptr, nullptr,
                 "psmain", "ps_4_0", 0, 0, (void**)&psb, (void**)&err);
    if (FAILED(hr))
    {
        SLOG("!!! render: PS compile 0x%08X %s", (unsigned)hr,
             err ? (const char*)err->GetBufferPointer() : "");
        if (err) err->Release();
        vsb->Release();
        return false;
    }
    if (err) err->Release();

    ID3D11Device* dev = g_st.dev;
    hr = dev->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &g_vs);
    if (FAILED(hr)) { SLOG("!!! render: CreateVertexShader 0x%08X", (unsigned)hr); return false; }
    hr = dev->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &g_ps);
    if (FAILED(hr)) { SLOG("!!! render: CreatePixelShader 0x%08X", (unsigned)hr); return false; }

    D3D11_INPUT_ELEMENT_DESC ied[2] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0,  D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,       0, 16, D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };
    hr = dev->CreateInputLayout(ied, 2, vsb->GetBufferPointer(), vsb->GetBufferSize(), &g_il);
    vsb->Release(); psb->Release();
    if (FAILED(hr)) { SLOG("!!! render: CreateInputLayout 0x%08X", (unsigned)hr); return false; }

    D3D11_BUFFER_DESC bd = {};
    bd.ByteWidth = sizeof(Vtx) * 4;
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(dev->CreateBuffer(&bd, nullptr, &g_vb))) { SLOG("!!! render: vb"); return false; }

    D3D11_SAMPLER_DESC sd = {};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    dev->CreateSamplerState(&sd, &g_samp);

    D3D11_BLEND_DESC bl = {};
    bl.RenderTarget[0].BlendEnable = FALSE;
    bl.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    dev->CreateBlendState(&bl, &g_blendOff);
    bl.RenderTarget[0].BlendEnable = TRUE;
    bl.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    bl.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    bl.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    bl.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    bl.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    bl.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    dev->CreateBlendState(&bl, &g_blendAlpha);
    bl.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;      // premultiplied variant
    dev->CreateBlendState(&bl, &g_blendPremult);

    D3D11_RASTERIZER_DESC rd = {};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    dev->CreateRasterizerState(&rd, &g_rast);

    D3D11_DEPTH_STENCIL_DESC dd = {};
    dd.DepthEnable = FALSE;
    dd.StencilEnable = FALSE;
    dev->CreateDepthStencilState(&dd, &g_dss);

    if (!CreateEyeTargets()) return false;

    g_rInit = true;
    SLOG("render: init ok, eye targets %ux%u", g_st.rtW, g_st.rtH);
    return true;
}

void Render_Shutdown()
{
    // Process is going away with the game; leak-free teardown is not worth the
    // crash risk of releasing objects on a dying device. Intentionally minimal.
    g_rInit = false;
}

// ---------------------------------------------------------------- state save
struct SavedState
{
    ID3D11RenderTargetView* rtv; ID3D11DepthStencilView* dsv;
    D3D11_VIEWPORT vp; UINT nvp;
    ID3D11BlendState* blend; FLOAT bf[4]; UINT bmask;
    ID3D11DepthStencilState* dss; UINT sref;
    ID3D11RasterizerState* rast;
    ID3D11VertexShader* vs; ID3D11PixelShader* ps;
    ID3D11GeometryShader* gs; ID3D11HullShader* hs; ID3D11DomainShader* ds;
    ID3D11InputLayout* il;
    D3D11_PRIMITIVE_TOPOLOGY topo;
    ID3D11Buffer* vb; UINT vbStride, vbOff;
    ID3D11ShaderResourceView* srv0;
    ID3D11SamplerState* samp0;
};

static void SaveState(ID3D11DeviceContext* c, SavedState& s)
{
    c->OMGetRenderTargets(1, &s.rtv, &s.dsv);
    s.nvp = 1; c->RSGetViewports(&s.nvp, &s.vp);
    c->OMGetBlendState(&s.blend, s.bf, &s.bmask);
    c->OMGetDepthStencilState(&s.dss, &s.sref);
    c->RSGetState(&s.rast);
    c->VSGetShader(&s.vs, nullptr, nullptr);
    c->PSGetShader(&s.ps, nullptr, nullptr);
    c->GSGetShader(&s.gs, nullptr, nullptr);
    c->HSGetShader(&s.hs, nullptr, nullptr);
    c->DSGetShader(&s.ds, nullptr, nullptr);
    c->IAGetInputLayout(&s.il);
    c->IAGetPrimitiveTopology(&s.topo);
    c->IAGetVertexBuffers(0, 1, &s.vb, &s.vbStride, &s.vbOff);
    c->PSGetShaderResources(0, 1, &s.srv0);
    c->PSGetSamplers(0, 1, &s.samp0);
}

static void RestoreState(ID3D11DeviceContext* c, SavedState& s)
{
    c->OMSetRenderTargets(1, &s.rtv, s.dsv);
    if (s.nvp) c->RSSetViewports(1, &s.vp);
    c->OMSetBlendState(s.blend, s.bf, s.bmask);
    c->OMSetDepthStencilState(s.dss, s.sref);
    c->RSSetState(s.rast);
    c->VSSetShader(s.vs, nullptr, 0);
    c->PSSetShader(s.ps, nullptr, 0);
    c->GSSetShader(s.gs, nullptr, 0);
    c->HSSetShader(s.hs, nullptr, 0);
    c->DSSetShader(s.ds, nullptr, 0);
    c->IASetInputLayout(s.il);
    c->IASetPrimitiveTopology(s.topo);
    c->IASetVertexBuffers(0, 1, &s.vb, &s.vbStride, &s.vbOff);
    c->PSSetShaderResources(0, 1, &s.srv0);
    c->PSSetSamplers(0, 1, &s.samp0);
    // release the references Get* added
    if (s.rtv) s.rtv->Release();   if (s.dsv) s.dsv->Release();
    if (s.blend) s.blend->Release(); if (s.dss) s.dss->Release();
    if (s.rast) s.rast->Release();
    if (s.vs) s.vs->Release();     if (s.ps) s.ps->Release();
    if (s.gs) s.gs->Release();     if (s.hs) s.hs->Release();
    if (s.ds) s.ds->Release();
    if (s.il) s.il->Release();     if (s.vb) s.vb->Release();
    if (s.srv0) s.srv0->Release(); if (s.samp0) s.samp0->Release();
}

// ---------------------------------------------------------------- draw one quad
// V = inverse of the current eye pose (origin space). corners = 4 world-space
// points, TL TR BL BR. Projects through the eye's raw frustum into clip space.
static void DrawQuad(ID3D11DeviceContext* c, int eye, const M34& V,
                     const float corners[4][3], const float uvs[4][2],
                     ID3D11ShaderResourceView* srv)
{
    const float l = g_st.rawL[eye], r = g_st.rawR[eye];
    const float U = g_st.rawU[eye], D = g_st.rawD[eye];

    D3D11_MAPPED_SUBRESOURCE map;
    if (FAILED(c->Map(g_vb, 0, D3D11_MAP_WRITE_DISCARD, 0, &map))) return;
    Vtx* v = (Vtx*)map.pData;
    for (int i = 0; i < 4; ++i)
    {
        float cam[3];
        M34_XformPoint(V, corners[i], cam);
        const float x = cam[0], y = cam[1], z = cam[2];
        // off-center projection from tangents, w = -z
        v[i].px = (2.f * x + (l + r) * z) / (r - l);
        v[i].py = (2.f * y + (D + U) * z) / (U - D);
        v[i].pz = 0.5f * (-z);
        v[i].pw = -z;
        v[i].u = uvs[i][0];
        v[i].v = uvs[i][1];
    }
    c->Unmap(g_vb, 0);
    c->PSSetShaderResources(0, 1, &srv);
    c->Draw(4, 0);
}

static void PoseToM34(const float pose[7], M34* out)
{
    *out = M34_FromQuatPos(pose, pose + 4);
}

void Render_CompositeAndSubmit(const ProjDrawView* proj, const QuadDraw* quads, int quadCount)
{
    if (!g_vr.ok || !g_st.ctx || !g_st.haveOrigin) return;
    if (!Render_Init()) return;

    // The mod's swapchains may have been re-created LARGER after a mid-session
    // resolution change (BS2). Rebuild the eye targets to match, or every copy
    // into the compositor would be a downscale of a partial rect.
    if (g_eyeW != g_st.rtW || g_eyeH != g_st.rtH)
    {
        SLOG("render: eye targets %ux%u -> %ux%u (app swapchain changed)",
             g_eyeW, g_eyeH, g_st.rtW, g_st.rtH);
        ReleaseEyeTargets();
        if (!CreateEyeTargets()) { g_rInit = false; return; }
    }

    ID3D11DeviceContext* c = g_st.ctx;
    const int parity = (int)(g_st.frameIndex & 1);

    SavedState saved = {};
    SaveState(c, saved);

    // common state
    c->IASetInputLayout(g_il);
    c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    UINT stride = sizeof(Vtx), off = 0;
    c->IASetVertexBuffers(0, 1, &g_vb, &stride, &off);
    c->VSSetShader(g_vs, nullptr, 0);
    c->PSSetShader(g_ps, nullptr, 0);
    c->GSSetShader(nullptr, nullptr, 0);
    c->HSSetShader(nullptr, nullptr, 0);
    c->DSSetShader(nullptr, nullptr, 0);
    c->PSSetSamplers(0, 1, &g_samp);
    c->RSSetState(g_rast);
    c->OMSetDepthStencilState(g_dss, 0);

    for (int e = 0; e < 2; ++e)
    {
        ID3D11RenderTargetView* rtv = g_eyeRtv[parity][e];
        const float black[4] = { 0, 0, 0, 1 };
        c->OMSetRenderTargets(1, &rtv, nullptr);
        c->ClearRenderTargetView(rtv, black);

        D3D11_VIEWPORT vp = {};
        vp.Width = (float)g_st.rtW;
        vp.Height = (float)g_st.rtH;
        vp.MaxDepth = 1.f;
        c->RSSetViewports(1, &vp);

        // current eye pose & inverse (origin space)
        const M34 eyePose = M34_Mul(g_st.hmd, g_st.eyeToHead[e]);
        const M34 V = M34_InvRigid(eyePose);

        // ---- projection layer: quad at 50 m through the submitted frustum --
        // Dq = 50 m is a donor invariant: 1000 ("plane at infinity") was tried
        // and rejected. Do not retune without headset evidence.
        if (proj)
        {
            const ProjDrawView& P = proj[e];
            M34 pm; PoseToM34(P.pose, &pm);
            const float Dq = 50.f;
            const float x0 = P.tanL * Dq, x1 = P.tanR * Dq;
            const float y0 = P.tanU * Dq, y1 = P.tanD * Dq;   // y0 top, y1 bottom
            float local[4][3] = {
                { x0, y0, -Dq }, { x1, y0, -Dq },
                { x0, y1, -Dq }, { x1, y1, -Dq },
            };
            float world[4][3];
            for (int i = 0; i < 4; ++i) M34_XformPoint(pm, local[i], world[i]);
            const float uvs[4][2] = { {0,0},{1,0},{0,1},{1,1} };
            c->OMSetBlendState(g_blendOff, nullptr, 0xFFFFFFFF);
            DrawQuad(c, e, V, world, uvs, P.srv);
        }

        // ---- quad layers, in submission order ------------------------------
        for (int qi = 0; qi < quadCount; ++qi)
        {
            const QuadDraw& q = quads[qi];
            M34 qm; PoseToM34(q.pose, &qm);
            if (q.viewSpace)
                qm = M34_Mul(g_st.hmd, qm);      // head-locked -> origin space

            const float hx = q.sx * 0.5f, hy = q.sy * 0.5f;
            float local[4][3] = {
                { -hx,  hy, 0 }, {  hx,  hy, 0 },
                { -hx, -hy, 0 }, {  hx, -hy, 0 },
            };
            float world[4][3];
            for (int i = 0; i < 4; ++i) M34_XformPoint(qm, local[i], world[i]);
            const float uvs[4][2] = { {0,0},{1,0},{0,1},{1,1} };
            ID3D11BlendState* bs = (q.blend == 1) ? g_blendAlpha
                                 : (q.blend == 2) ? g_blendPremult : g_blendOff;
            c->OMSetBlendState(bs, nullptr, 0xFFFFFFFF);
            DrawQuad(c, e, V, world, uvs, q.srv);
        }
    }

    // unbind our RT before Submit (compositor reads the texture)
    ID3D11RenderTargetView* nullRtv = nullptr;
    c->OMSetRenderTargets(1, &nullRtv, nullptr);

    for (int e = 0; e < 2; ++e)
    {
        Texture_t tex;
        tex.handle = g_eyeTex[parity][e];
        tex.eType = ETextureType_TextureType_DirectX;
        tex.eColorSpace = EColorSpace_ColorSpace_Gamma;
        const EVRCompositorError ce =
            g_vr.comp->Submit((EVREye)e, &tex, nullptr, EVRSubmitFlags_Submit_Default);
        static EVRCompositorError lastErr = EVRCompositorError_VRCompositorError_None;
        if (ce != EVRCompositorError_VRCompositorError_None && ce != lastErr)
        {
            SLOG("!!! Submit eye %d -> compositor error %d", e, (int)ce);
            lastErr = ce;
        }
    }

    RestoreState(c, saved);
}
