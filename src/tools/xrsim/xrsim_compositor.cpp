// bvr_xrsim32: the compositor and the capture path.
//
// This is what makes the simulator worth building. A window screenshot shows the
// game's backbuffer; it can never show an XR quad layer, so the aim laser and
// the HUD panel have been unverifiable outside a headset for this project's
// whole life (docs/bioshock1/TESTING.md records exactly that limitation). Here
// the submitted layer stack is composited the way a compositor would - each eye
// gets the projection layer reprojected into ITS optics, then every quad layer
// placed by its pose - and written out as PNGs plus a JSON sidecar of numbers an
// agent can assert on.
//
// The projection pass is a real reprojection, not a blit, because the layer's
// tagged pose and tagged fov both differ from the eye's. That is the highest
// value part: a claimed-fov mismatch becomes visible magnification in a picture.
// Session 28's yaw warp (a 1.84x under-claim) would have been one look at a PNG
// instead of three sessions of inference.
//
// Cost discipline: compositing is OFF except on capture frames (`compose
// oncapture`, the default), so having the sim attached costs nothing per frame.
// This tool must never become the pacing bug it was built to find.

#include "xrsim_internal.h"

#include <d3dcompiler.h>
#include <wincodec.h>

#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <thread>
#include <vector>

namespace xrsim {
namespace {

// --- shaders ---------------------------------------------------------------
// One VS drives both passes: vertex id 0..2 makes a fullscreen triangle for the
// projection pass, 0..3 makes a quad in clip space for the quad pass.

const char* kShaderSource = R"HLSL(
cbuffer Constants : register(b0) {
    float4 eyeTan;      // l, r, u, d  (tangents of the EYE's fov)
    float4 layTan;      // l, r, u, d  (tangents of the LAYER's fov)
    float4 rectMin;     // xy = uv min of the submitted subimage
    float4 rectMax;     // xy = uv max
    float4 qRel;        // rotation from eye space into layer space
    float4x4 mvp;       // quad pass only
    float4 quadParams;  // x = pass (0 projection, 1 quad), y = flipY
};

struct VSOut {
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
};

Texture2D    srcTex : register(t0);
SamplerState srcSmp : register(s0);

float3 rotate_by(float4 q, float3 v) {
    float3 u = q.xyz;
    float3 t = 2.0 * cross(u, v);
    return v + q.w * t + cross(u, t);
}

VSOut VSMain(uint vid : SV_VertexID) {
    VSOut o;
    if (quadParams.x < 0.5) {
        // Fullscreen triangle.
        float2 uv = float2((vid << 1) & 2, vid & 2);
        o.uv = uv;
        o.pos = float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 0.0, 1.0);
    } else {
        // Unit quad in the layer's local plane, transformed by mvp.
        float2 c = float2((vid == 1 || vid == 3) ? 1.0 : -1.0,
                          (vid >= 2) ? -1.0 : 1.0);
        o.uv = float2(c.x * 0.5 + 0.5, 0.5 - c.y * 0.5);
        o.pos = mul(float4(c.x, c.y, 0.0, 1.0), mvp);
    }
    return o;
}

// Projection layer: for each output pixel, build the eye ray, rotate it into the
// layer's frame, project onto the layer's tangent plane, and sample there.
float4 PSProjection(VSOut i) : SV_TARGET {
    float tanX = lerp(eyeTan.x, eyeTan.y, i.uv.x);
    float tanY = lerp(eyeTan.z, eyeTan.w, i.uv.y);
    float3 dEye = normalize(float3(tanX, tanY, -1.0));
    float3 dLay = rotate_by(qRel, dEye);
    if (dLay.z >= -0.0001) return float4(0, 0, 0, 1);   // behind the layer plane

    float2 t = dLay.xy / -dLay.z;
    float2 lay = float2((t.x - layTan.x) / (layTan.y - layTan.x),
                        (t.y - layTan.z) / (layTan.w - layTan.z));
    if (lay.x < 0.0 || lay.x > 1.0 || lay.y < 0.0 || lay.y > 1.0)
        return float4(0, 0, 0, 1);                       // outside what was submitted

    float2 uv = lerp(rectMin.xy, rectMax.xy, lay);
    return float4(srcTex.Sample(srcSmp, uv).rgb, 1.0);
}

float4 PSQuad(VSOut i) : SV_TARGET {
    float2 uv = lerp(rectMin.xy, rectMax.xy, i.uv);
    return srcTex.Sample(srcSmp, uv);
}
)HLSL";

struct Constants {
    float eyeTan[4];
    float layTan[4];
    float rectMin[4];
    float rectMax[4];
    float qRel[4];
    float mvp[16];
    float quadParams[4];
};

ID3D11Device* g_device = nullptr;
ID3D11DeviceContext* g_ctx = nullptr;
ID3D11VertexShader* g_vs = nullptr;
ID3D11PixelShader* g_psProj = nullptr;
ID3D11PixelShader* g_psQuad = nullptr;
ID3D11Buffer* g_cb = nullptr;
ID3D11SamplerState* g_sampler = nullptr;
ID3D11BlendState* g_blendOff = nullptr;
ID3D11BlendState* g_blendPremul = nullptr;
ID3D11BlendState* g_blendAlpha = nullptr;
ID3D11DepthStencilState* g_depthOff = nullptr;
ID3D11RasterizerState* g_raster = nullptr;

ID3D11Texture2D* g_eyeRt[2] = {};
ID3D11RenderTargetView* g_eyeRtv[2] = {};
ID3D11Texture2D* g_staging[2] = {};
uint32_t g_rtW = 0, g_rtH = 0;
bool g_ready = false;
bool g_disabled = false;

std::atomic<uint32_t> g_lastLayerCount{0};
std::atomic<uint32_t> g_lastProjViews{0};

// --- the encode queue ------------------------------------------------------
struct EncodeJob {
    std::vector<uint8_t> pixels[2]; // BGRA, tightly packed
    uint32_t width = 0, height = 0;
    std::string baseName;
    std::string json;
};

std::mutex g_qMutex;
std::condition_variable g_qCv;
std::deque<EncodeJob> g_queue;
std::thread g_encodeThread;
std::atomic<bool> g_encodeRunning{false};

// D3D11 state save/restore. Same slice core/gfx/blit.cpp already proves out
// against this game's renderer; anything the compositor touches must come back
// exactly as it was or the flat window corrupts.
struct SavedState {
    ID3D11RenderTargetView* rtv[8] = {};
    ID3D11DepthStencilView* dsv = nullptr;
    ID3D11BlendState* blend = nullptr;
    FLOAT blendFactor[4] = {};
    UINT sampleMask = 0;
    ID3D11DepthStencilState* depth = nullptr;
    UINT stencilRef = 0;
    ID3D11RasterizerState* raster = nullptr;
    D3D11_VIEWPORT viewports[8] = {};
    UINT viewportCount = 8;
    ID3D11VertexShader* vs = nullptr;
    ID3D11PixelShader* ps = nullptr;
    ID3D11Buffer* vsCb = nullptr;
    ID3D11Buffer* psCb = nullptr;
    ID3D11ShaderResourceView* srv = nullptr;
    ID3D11SamplerState* samp = nullptr;
    ID3D11InputLayout* layout = nullptr;
    D3D11_PRIMITIVE_TOPOLOGY topology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
};

void save_state(SavedState& s) {
    g_ctx->OMGetRenderTargets(8, s.rtv, &s.dsv);
    g_ctx->OMGetBlendState(&s.blend, s.blendFactor, &s.sampleMask);
    g_ctx->OMGetDepthStencilState(&s.depth, &s.stencilRef);
    g_ctx->RSGetState(&s.raster);
    g_ctx->RSGetViewports(&s.viewportCount, s.viewports);
    g_ctx->VSGetShader(&s.vs, nullptr, nullptr);
    g_ctx->PSGetShader(&s.ps, nullptr, nullptr);
    g_ctx->VSGetConstantBuffers(0, 1, &s.vsCb);
    g_ctx->PSGetConstantBuffers(0, 1, &s.psCb);
    g_ctx->PSGetShaderResources(0, 1, &s.srv);
    g_ctx->PSGetSamplers(0, 1, &s.samp);
    g_ctx->IAGetInputLayout(&s.layout);
    g_ctx->IAGetPrimitiveTopology(&s.topology);
}

void restore_state(SavedState& s) {
    g_ctx->OMSetRenderTargets(8, s.rtv, s.dsv);
    g_ctx->OMSetBlendState(s.blend, s.blendFactor, s.sampleMask);
    g_ctx->OMSetDepthStencilState(s.depth, s.stencilRef);
    g_ctx->RSSetState(s.raster);
    if (s.viewportCount) g_ctx->RSSetViewports(s.viewportCount, s.viewports);
    g_ctx->VSSetShader(s.vs, nullptr, 0);
    g_ctx->PSSetShader(s.ps, nullptr, 0);
    g_ctx->VSSetConstantBuffers(0, 1, &s.vsCb);
    g_ctx->PSSetConstantBuffers(0, 1, &s.psCb);
    g_ctx->PSSetShaderResources(0, 1, &s.srv);
    g_ctx->PSSetSamplers(0, 1, &s.samp);
    g_ctx->IASetInputLayout(s.layout);
    g_ctx->IASetPrimitiveTopology(s.topology);

    for (auto*& r : s.rtv) if (r) { r->Release(); r = nullptr; }
    if (s.dsv) s.dsv->Release();
    if (s.blend) s.blend->Release();
    if (s.depth) s.depth->Release();
    if (s.raster) s.raster->Release();
    if (s.vs) s.vs->Release();
    if (s.ps) s.ps->Release();
    if (s.vsCb) s.vsCb->Release();
    if (s.psCb) s.psCb->Release();
    if (s.srv) s.srv->Release();
    if (s.samp) s.samp->Release();
    if (s.layout) s.layout->Release();
}

bool ensure_targets(uint32_t w, uint32_t h) {
    if (g_eyeRt[0] && g_rtW == w && g_rtH == h) return true;
    for (int i = 0; i < 2; ++i) {
        if (g_eyeRtv[i]) { g_eyeRtv[i]->Release(); g_eyeRtv[i] = nullptr; }
        if (g_eyeRt[i]) { g_eyeRt[i]->Release(); g_eyeRt[i] = nullptr; }
        if (g_staging[i]) { g_staging[i]->Release(); g_staging[i] = nullptr; }
    }

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = w;
    desc.Height = h;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    // _SRGB, so the hardware RE-ENCODES on write. The app's swapchain images are
    // R8G8B8A8_UNORM_SRGB (core's format pick), so the SRV decodes their texels
    // to linear when compositing - a UNORM target then stores those LINEAR
    // values, which crushes every dark scene ~13x and made an Infinite quad
    // capture read meanLuma 0.05 while the window read ~10 (session 38). With an
    // SRGB target the texel bytes round-trip decode->blend->encode, matching
    // what a real compositor shows the eye and what the PNG format expects.
    // NOTE: pixel-stat baselines captured before this change (VERIFICATION 2.9)
    // are systematically darker and not comparable.
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

    D3D11_TEXTURE2D_DESC sd = desc;
    // Staging keeps plain UNORM - same B8G8R8A8_TYPELESS family, so CopyResource
    // is legal and Map hands back the sRGB-encoded bytes WIC wants.
    sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.Usage = D3D11_USAGE_STAGING;
    sd.BindFlags = 0;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

    for (int i = 0; i < 2; ++i) {
        if (FAILED(g_device->CreateTexture2D(&desc, nullptr, &g_eyeRt[i]))) return false;
        if (FAILED(g_device->CreateRenderTargetView(g_eyeRt[i], nullptr, &g_eyeRtv[i]))) return false;
        if (FAILED(g_device->CreateTexture2D(&sd, nullptr, &g_staging[i]))) return false;
    }
    g_rtW = w;
    g_rtH = h;
    return true;
}

// --- WIC PNG encode --------------------------------------------------------
bool write_png(const wchar_t* path, const uint8_t* bgra, uint32_t w, uint32_t h) {
    IWICImagingFactory* factory = nullptr;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&factory))))
        return false;

    bool ok = false;
    IWICStream* stream = nullptr;
    IWICBitmapEncoder* encoder = nullptr;
    IWICBitmapFrameEncode* frame = nullptr;
    IPropertyBag2* props = nullptr;

    if (SUCCEEDED(factory->CreateStream(&stream)) &&
        SUCCEEDED(stream->InitializeFromFilename(path, GENERIC_WRITE)) &&
        SUCCEEDED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) &&
        SUCCEEDED(encoder->Initialize(stream, WICBitmapEncoderNoCache)) &&
        SUCCEEDED(encoder->CreateNewFrame(&frame, &props)) && SUCCEEDED(frame->Initialize(props))) {
        WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
        if (SUCCEEDED(frame->SetSize(w, h)) && SUCCEEDED(frame->SetPixelFormat(&fmt)) &&
            SUCCEEDED(frame->WritePixels(h, w * 4, w * h * 4, const_cast<BYTE*>(bgra))) &&
            SUCCEEDED(frame->Commit()) && SUCCEEDED(encoder->Commit())) {
            ok = true;
        }
    }

    if (props) props->Release();
    if (frame) frame->Release();
    if (encoder) encoder->Release();
    if (stream) stream->Release();
    factory->Release();
    return ok;
}

void write_sbs(const wchar_t* path, const EncodeJob& job) {
    const uint32_t w = job.width, h = job.height;
    std::vector<uint8_t> sbs(static_cast<size_t>(w) * 2 * h * 4);
    for (uint32_t y = 0; y < h; ++y) {
        memcpy(&sbs[(static_cast<size_t>(y) * w * 2) * 4], &job.pixels[0][static_cast<size_t>(y) * w * 4], w * 4);
        memcpy(&sbs[(static_cast<size_t>(y) * w * 2 + w) * 4], &job.pixels[1][static_cast<size_t>(y) * w * 4], w * 4);
    }
    write_png(path, sbs.data(), w * 2, h);
}

void encode_thread_proc() {
    // The capture thread owns its OWN apartment. Calling CoInitializeEx on the
    // present thread would change the game's COM state, which is not ours to
    // touch.
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    while (g_encodeRunning.load()) {
        EncodeJob job;
        {
            std::unique_lock<std::mutex> lock(g_qMutex);
            g_qCv.wait_for(lock, std::chrono::milliseconds(200),
                           [] { return !g_queue.empty() || !g_encodeRunning.load(); });
            if (g_queue.empty()) continue;
            job = std::move(g_queue.front());
            g_queue.pop_front();
        }

        wchar_t base[MAX_PATH];
        swprintf_s(base, L"%s\\capture\\%hs", log::dir(), job.baseName.c_str());

        wchar_t p[MAX_PATH];
        swprintf_s(p, L"%s_left.png", base);
        write_png(p, job.pixels[0].data(), job.width, job.height);
        swprintf_s(p, L"%s_right.png", base);
        write_png(p, job.pixels[1].data(), job.width, job.height);
        swprintf_s(p, L"%s_sbs.png", base);
        write_sbs(p, job);

        swprintf_s(p, L"%s.json", base);
        FILE* f = _wfsopen(p, L"w", _SH_DENYNO);
        if (f) {
            fputs(job.json.c_str(), f);
            fclose(f);
        }

        char utf8[MAX_PATH];
        WideCharToMultiByte(CP_UTF8, 0, base, -1, utf8, MAX_PATH, nullptr, nullptr);
        strncpy_s(g.lastCapturePath, utf8, _TRUNCATE);
        g.captureSeq.fetch_add(1);
        XRSIM_LOG("xrsim: capture written %hs (%ux%u)", job.baseName.c_str(), job.width, job.height);
    }
    CoUninitialize();
}

Fov fov_from_xr(const XrFovf& f) { return Fov{f.angleLeft, f.angleRight, f.angleUp, f.angleDown}; }

void tangents(const Fov& f, float* out) {
    out[0] = tanf(f.angleLeft);
    out[1] = tanf(f.angleRight);
    out[2] = tanf(f.angleUp);
    out[3] = tanf(f.angleDown);
}

} // namespace

uint32_t compositor_last_layer_count() { return g_lastLayerCount.load(); }
uint32_t compositor_last_projection_views() { return g_lastProjViews.load(); }

void compositor_init(ID3D11Device* device, ID3D11DeviceContext* context) {
    if (g_ready || g_disabled) return;
    g_device = device;
    g_ctx = context;

    ID3DBlob* vsb = nullptr;
    ID3DBlob* psb1 = nullptr;
    ID3DBlob* psb2 = nullptr;
    ID3DBlob* err = nullptr;
    const UINT flags = D3DCOMPILE_OPTIMIZATION_LEVEL1;

    auto fail = [&](const char* what) {
        XRSIM_LOG("xrsim: compositor DISABLED - %s%s%s", what, err ? ": " : "",
                  err ? static_cast<const char*>(err->GetBufferPointer()) : "");
        g_disabled = true;
        if (vsb) vsb->Release();
        if (psb1) psb1->Release();
        if (psb2) psb2->Release();
        if (err) err->Release();
    };

    if (FAILED(D3DCompile(kShaderSource, strlen(kShaderSource), "xrsim", nullptr, nullptr, "VSMain",
                          "vs_4_0", flags, 0, &vsb, &err))) { fail("VS compile failed"); return; }
    if (FAILED(D3DCompile(kShaderSource, strlen(kShaderSource), "xrsim", nullptr, nullptr,
                          "PSProjection", "ps_4_0", flags, 0, &psb1, &err))) {
        fail("PSProjection compile failed"); return;
    }
    if (FAILED(D3DCompile(kShaderSource, strlen(kShaderSource), "xrsim", nullptr, nullptr, "PSQuad",
                          "ps_4_0", flags, 0, &psb2, &err))) { fail("PSQuad compile failed"); return; }

    device->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &g_vs);
    device->CreatePixelShader(psb1->GetBufferPointer(), psb1->GetBufferSize(), nullptr, &g_psProj);
    device->CreatePixelShader(psb2->GetBufferPointer(), psb2->GetBufferSize(), nullptr, &g_psQuad);
    vsb->Release();
    psb1->Release();
    psb2->Release();
    if (err) err->Release();

    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = sizeof(Constants);
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    device->CreateBuffer(&bd, nullptr, &g_cb);

    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    device->CreateSamplerState(&sd, &g_sampler);

    D3D11_BLEND_DESC bl{};
    bl.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    device->CreateBlendState(&bl, &g_blendOff);

    // Premultiplied: what XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT
    // means without the UNPREMULTIPLIED bit. Same state blit.cpp uses.
    bl.RenderTarget[0].BlendEnable = TRUE;
    bl.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
    bl.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    bl.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    bl.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    bl.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    bl.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    device->CreateBlendState(&bl, &g_blendPremul);

    bl.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    device->CreateBlendState(&bl, &g_blendAlpha);

    D3D11_DEPTH_STENCIL_DESC dsd{};
    dsd.DepthEnable = FALSE;
    device->CreateDepthStencilState(&dsd, &g_depthOff);

    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE; // a quad may face away and must still show
    device->CreateRasterizerState(&rd, &g_raster);

    g_encodeRunning.store(true);
    g_encodeThread = std::thread(encode_thread_proc);
    g_ready = true;
    XRSIM_LOG("xrsim: compositor ready");
}

void compositor_hash_cleanup(); // defined with the hash-log statics below

void compositor_shutdown() {
    compositor_hash_cleanup();
    if (g_encodeRunning.exchange(false)) {
        g_qCv.notify_all();
        if (g_encodeThread.joinable()) g_encodeThread.join();
    }
    for (int i = 0; i < 2; ++i) {
        if (g_eyeRtv[i]) { g_eyeRtv[i]->Release(); g_eyeRtv[i] = nullptr; }
        if (g_eyeRt[i]) { g_eyeRt[i]->Release(); g_eyeRt[i] = nullptr; }
        if (g_staging[i]) { g_staging[i]->Release(); g_staging[i] = nullptr; }
    }
    auto rel = [](auto*& p) { if (p) { p->Release(); p = nullptr; } };
    rel(g_vs); rel(g_psProj); rel(g_psQuad); rel(g_cb); rel(g_sampler);
    rel(g_blendOff); rel(g_blendPremul); rel(g_blendAlpha); rel(g_depthOff); rel(g_raster);
    g_ready = false;
    g_rtW = g_rtH = 0;
    g_device = nullptr;
    g_ctx = nullptr;
}

// ---------------------------------------------------------------------------
// Compose one eye
// ---------------------------------------------------------------------------

namespace {

struct LayerStat {
    const char* kind = "quad";
    uint32_t pixelsCovered[2] = {0, 0};
};

void set_constants(const Constants& c) {
    D3D11_MAPPED_SUBRESOURCE m{};
    if (SUCCEEDED(g_ctx->Map(g_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        memcpy(m.pData, &c, sizeof(c));
        g_ctx->Unmap(g_cb, 0);
    }
    g_ctx->VSSetConstantBuffers(0, 1, &g_cb);
    g_ctx->PSSetConstantBuffers(0, 1, &g_cb);
}

void uv_rect(const XrSwapchainSubImage& sub, uint32_t texW, uint32_t texH, float* mn, float* mx) {
    const float w = (texW > 0) ? static_cast<float>(texW) : 1.0f;
    const float h = (texH > 0) ? static_cast<float>(texH) : 1.0f;
    int x = sub.imageRect.offset.x, y = sub.imageRect.offset.y;
    int ew = sub.imageRect.extent.width, eh = sub.imageRect.extent.height;
    if (ew <= 0) ew = static_cast<int>(texW);
    if (eh <= 0) eh = static_cast<int>(texH);
    mn[0] = x / w;
    mn[1] = y / h;
    mx[0] = (x + ew) / w;
    mx[1] = (y + eh) / h;
}

void compose_eye(int eye, const SimSubmission& sub, std::vector<LayerStat>& stats) {
    const Rig& rig = sub.snap.rig;
    Pose eyeOffset = pose_identity();
    eyeOffset.p.x = (eye == 0 ? -0.5f : 0.5f) * rig.ipdM;
    const Pose eyeWorld = pose_mul(sub.snap.headWorld, eyeOffset);
    const Fov eyeFov = rig.fov[eye];

    const float clear[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    g_ctx->ClearRenderTargetView(g_eyeRtv[eye], clear);
    g_ctx->OMSetRenderTargets(1, &g_eyeRtv[eye], nullptr);

    D3D11_VIEWPORT vp{};
    vp.Width = static_cast<float>(g_rtW);
    vp.Height = static_cast<float>(g_rtH);
    vp.MaxDepth = 1.0f;
    g_ctx->RSSetViewports(1, &vp);
    g_ctx->RSSetState(g_raster);
    g_ctx->OMSetDepthStencilState(g_depthOff, 0);
    g_ctx->VSSetShader(g_vs, nullptr, 0);
    g_ctx->PSSetSamplers(0, 1, &g_sampler);
    g_ctx->IASetInputLayout(nullptr);

    const float bf[4] = {0, 0, 0, 0};

    for (uint32_t li = 0; li < sub.layerCount; ++li) {
        const SimLayer& L = sub.layers[li];
        if (li >= stats.size()) stats.resize(li + 1);

        if (L.type == XR_TYPE_COMPOSITION_LAYER_PROJECTION) {
            stats[li].kind = "projection";
            if (static_cast<uint32_t>(eye) >= L.viewCount) continue;
            const XrCompositionLayerProjectionView& v = L.views[eye];

            uint32_t tw = 0, th = 0;
            ID3D11Texture2D* tex = swapchain_last_image(v.subImage.swapchain, &tw, &th);
            if (!tex) continue;
            ID3D11ShaderResourceView* srv = nullptr;
            if (FAILED(g_device->CreateShaderResourceView(tex, nullptr, &srv))) continue;

            Constants c{};
            tangents(eyeFov, c.eyeTan);
            tangents(fov_from_xr(v.fov), c.layTan);
            uv_rect(v.subImage, tw, th, c.rectMin, c.rectMax);

            // The rotation that carries an eye-space ray into the layer's frame.
            // This is what makes a pose or fov mismatch VISIBLE rather than
            // merely inferable.
            const Pose layerPose = from_xr(v.pose);
            const Quat qRel = quat_norm(quat_mul(quat_conj(layerPose.q), eyeWorld.q));
            c.qRel[0] = qRel.x; c.qRel[1] = qRel.y; c.qRel[2] = qRel.z; c.qRel[3] = qRel.w;
            c.quadParams[0] = 0.0f;
            set_constants(c);

            g_ctx->OMSetBlendState(g_blendOff, bf, 0xFFFFFFFF);
            g_ctx->PSSetShader(g_psProj, nullptr, 0);
            g_ctx->PSSetShaderResources(0, 1, &srv);
            g_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            g_ctx->Draw(3, 0);
            srv->Release();
            stats[li].pixelsCovered[eye] = g_rtW * g_rtH;
            continue;
        }

        // --- quad layer ---
        if (L.eyeVisibility == XR_EYE_VISIBILITY_LEFT && eye != 0) continue;
        if (L.eyeVisibility == XR_EYE_VISIBILITY_RIGHT && eye != 1) continue;

        uint32_t tw = 0, th = 0;
        ID3D11Texture2D* tex = swapchain_last_image(L.sub.swapchain, &tw, &th);
        if (!tex) continue;
        ID3D11ShaderResourceView* srv = nullptr;
        if (FAILED(g_device->CreateShaderResourceView(tex, nullptr, &srv))) continue;

        SimSpace* space = space_get(L.space);
        Pose base = pose_identity();
        bool tracked = true;
        if (space) space_pose(*space, sub.snap, base, tracked);
        const Pose quadWorld = pose_mul(base, from_xr(L.pose));

        // Corners are +/-1 in the shader, so scale by the HALF extent.
        Mat4 model = mat4_identity();
        {
            const Quat q = quadWorld.q;
            const Vec3 rx = quat_rotate(q, Vec3{L.size.width * 0.5f, 0.0f, 0.0f});
            const Vec3 ry = quat_rotate(q, Vec3{0.0f, L.size.height * 0.5f, 0.0f});
            const Vec3 rz = quat_rotate(q, Vec3{0.0f, 0.0f, 1.0f});
            model.m[0][0] = rx.x; model.m[0][1] = rx.y; model.m[0][2] = rx.z;
            model.m[1][0] = ry.x; model.m[1][1] = ry.y; model.m[1][2] = ry.z;
            model.m[2][0] = rz.x; model.m[2][1] = rz.y; model.m[2][2] = rz.z;
            model.m[3][0] = quadWorld.p.x;
            model.m[3][1] = quadWorld.p.y;
            model.m[3][2] = quadWorld.p.z;
        }
        const Mat4 mvp = mat4_mul(mat4_mul(model, mat4_view_from_pose(eyeWorld)),
                                  mat4_projection_fov(eyeFov, 0.02f, 1000.0f));

        Constants c{};
        tangents(eyeFov, c.eyeTan);
        uv_rect(L.sub, tw, th, c.rectMin, c.rectMax);
        memcpy(c.mvp, mvp.m, sizeof(c.mvp));
        c.quadParams[0] = 1.0f;
        set_constants(c);

        ID3D11BlendState* blend = g_blendOff;
        if (L.flags & XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT) {
            blend = (L.flags & XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT) ? g_blendAlpha
                                                                              : g_blendPremul;
        }
        g_ctx->OMSetBlendState(blend, bf, 0xFFFFFFFF);
        g_ctx->PSSetShader(g_psQuad, nullptr, 0);
        g_ctx->PSSetShaderResources(0, 1, &srv);
        g_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
        g_ctx->Draw(4, 0);
        srv->Release();

        // A coarse on-screen estimate. It only has to answer "is this layer in
        // frame at all", which is the question no other tool in the repo can.
        stats[li].pixelsCovered[eye] = 1;
    }
}

std::string build_json(const SimSubmission& sub, const std::vector<LayerStat>& stats,
                       const char* baseName, uint32_t w, uint32_t h,
                       const double* meanLuma, const double* nonBlackPct) {
    char buf[8192];
    std::string out;
    out.reserve(16384);

    const Rig& r = sub.snap.rig;
    Pose eyeW[2];
    for (int e = 0; e < 2; ++e) {
        Pose off = pose_identity();
        off.p.x = (e == 0 ? -0.5f : 0.5f) * r.ipdM;
        eyeW[e] = pose_mul(sub.snap.headWorld, off);
    }
    const float eyeSep = v3_len(v3_sub(eyeW[1].p, eyeW[0].p));

    sprintf_s(buf, "{\n  \"name\": \"%s\",\n  \"frameIndex\": %llu,\n  \"displayTimeNs\": %lld,\n"
                   "  \"sessionState\": \"%s\",\n  \"width\": %u,\n  \"height\": %u,\n",
              baseName, static_cast<unsigned long long>(sub.frameIndex),
              static_cast<long long>(sub.displayTime), session_state_name(sub.snap.state), w, h);
    out += buf;

    sprintf_s(buf, "  \"pacing\": {\"mode\": \"%s\", \"hz\": %.2f, \"creditsLeft\": %u},\n",
              g.pacing.mode == PaceMode::Step ? "step"
                                              : (g.pacing.mode == PaceMode::Turbo ? "turbo" : "free"),
              g.pacing.hz, g.pacing.credits);
    out += buf;

    sprintf_s(buf, "  \"gate\": {\"waited\": %llu, \"begun\": %llu, \"ended\": %llu, "
                   "\"discarded\": %u, \"outOfOrder\": %u},\n",
              static_cast<unsigned long long>(g_gate.waited.load()),
              static_cast<unsigned long long>(g_gate.begun.load()),
              static_cast<unsigned long long>(g_gate.ended.load()), g_gate.discarded.load(),
              g_gate.outOfOrder.load());
    out += buf;

    float hy, hp, hr;
    quat_to_ypr(sub.snap.headWorld.q, hy, hp, hr);
    sprintf_s(buf, "  \"head\": {\"pos\": [%.5f, %.5f, %.5f], \"quat\": [%.5f, %.5f, %.5f, %.5f], "
                   "\"ypr\": [%.3f, %.3f, %.3f]},\n",
              sub.snap.headWorld.p.x, sub.snap.headWorld.p.y, sub.snap.headWorld.p.z,
              sub.snap.headWorld.q.x, sub.snap.headWorld.q.y, sub.snap.headWorld.q.z,
              sub.snap.headWorld.q.w, rad2deg(hy), rad2deg(hp), rad2deg(hr));
    out += buf;

    out += "  \"views\": [\n";
    for (int e = 0; e < 2; ++e) {
        sprintf_s(buf, "    {\"eye\": \"%s\", \"pos\": [%.5f, %.5f, %.5f], "
                       "\"fovDeg\": {\"l\": %.3f, \"r\": %.3f, \"u\": %.3f, \"d\": %.3f}}%s\n",
                  e == 0 ? "left" : "right", eyeW[e].p.x, eyeW[e].p.y, eyeW[e].p.z,
                  rad2deg(r.fov[e].angleLeft), rad2deg(r.fov[e].angleRight),
                  rad2deg(r.fov[e].angleUp), rad2deg(r.fov[e].angleDown), e == 0 ? "," : "");
        out += buf;
    }
    out += "  ],\n";

    for (int h2 = 0; h2 < 2; ++h2) {
        float y, p, rr;
        quat_to_ypr(sub.snap.aimWorld[h2].q, y, p, rr);
        sprintf_s(buf, "  \"hand%s\": {\"grip\": [%.5f, %.5f, %.5f], \"aimYpr\": [%.3f, %.3f, %.3f], "
                       "\"valid\": %s},\n",
                  h2 == 0 ? "L" : "R", sub.snap.gripWorld[h2].p.x, sub.snap.gripWorld[h2].p.y,
                  sub.snap.gripWorld[h2].p.z, rad2deg(y), rad2deg(p), rad2deg(rr),
                  r.handValid[h2] ? "true" : "false");
        out += buf;
    }

    sprintf_s(buf, "  \"controls\": {\"a\": %s, \"b\": %s, \"x\": %s, \"y\": %s, \"menu\": %s, "
                   "\"trigL\": %.3f, \"trigR\": %.3f, \"gripL\": %.3f, \"gripR\": %.3f, "
                   "\"stickL\": [%.3f, %.3f], \"stickR\": [%.3f, %.3f]},\n",
              r.btnA ? "true" : "false", r.btnB ? "true" : "false", r.btnX ? "true" : "false",
              r.btnY ? "true" : "false", r.menu ? "true" : "false", r.trigger[0], r.trigger[1],
              r.squeeze[0], r.squeeze[1], r.stick[0][0], r.stick[0][1], r.stick[1][0], r.stick[1][1]);
    out += buf;

    sprintf_s(buf, "  \"layerCount\": %u,\n  \"layers\": [\n", sub.layerCount);
    out += buf;
    for (uint32_t i = 0; i < sub.layerCount; ++i) {
        const SimLayer& L = sub.layers[i];
        const LayerStat& st = (i < stats.size()) ? stats[i] : LayerStat{};
        const bool isProj = L.type == XR_TYPE_COMPOSITION_LAYER_PROJECTION;
        SimSpace* sp = space_get(L.space);
        const char* spaceName = "unknown";
        if (sp) spaceName = sp->isAction ? "action"
                                         : (sp->refType == XR_REFERENCE_SPACE_TYPE_VIEW ? "view"
                                            : sp->refType == XR_REFERENCE_SPACE_TYPE_STAGE ? "stage"
                                                                                           : "local");
        if (isProj) {
            sprintf_s(buf, "    {\"i\": %u, \"type\": \"projection\", \"space\": \"%s\", "
                           "\"viewCount\": %u, \"fovDeg\": {\"l\": %.3f, \"r\": %.3f, \"u\": %.3f, "
                           "\"d\": %.3f}, \"pixelsCoveredL\": %u, \"pixelsCoveredR\": %u}%s\n",
                      i, spaceName, L.viewCount, rad2deg(L.views[0].fov.angleLeft),
                      rad2deg(L.views[0].fov.angleRight), rad2deg(L.views[0].fov.angleUp),
                      rad2deg(L.views[0].fov.angleDown), st.pixelsCovered[0], st.pixelsCovered[1],
                      (i + 1 < sub.layerCount) ? "," : "");
        } else {
            sprintf_s(buf, "    {\"i\": %u, \"type\": \"quad\", \"space\": \"%s\", "
                           "\"sizeM\": [%.4f, %.4f], \"pose\": [%.4f, %.4f, %.4f], "
                           "\"premultiplied\": %s, \"pixelsCoveredL\": %u, \"pixelsCoveredR\": %u}%s\n",
                      i, spaceName, L.size.width, L.size.height, L.pose.position.x,
                      L.pose.position.y, L.pose.position.z,
                      (L.flags & XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT) ? "true"
                                                                                      : "false",
                      st.pixelsCovered[0], st.pixelsCovered[1],
                      (i + 1 < sub.layerCount) ? "," : "");
        }
        out += buf;
    }
    out += "  ],\n";

    // The derived block: the numbers that turn a whole bug class into an assert.
    // claimRatioH compares what the game CLAIMED it rendered against what the
    // eye actually sees; anything away from 1.0 is magnification in the headset.
    double claimTanH = 0.0;
    for (uint32_t i = 0; i < sub.layerCount; ++i) {
        if (sub.layers[i].type != XR_TYPE_COMPOSITION_LAYER_PROJECTION) continue;
        const XrFovf& f = sub.layers[i].views[0].fov;
        claimTanH = (tan(-f.angleLeft) + tan(f.angleRight)) * 0.5;
        break;
    }
    const double eyeTanH = (tan(-r.fov[0].angleLeft) + tan(r.fov[0].angleRight)) * 0.5;

    // Is the AIM in sync with the MODEL? The laser dots should lie along the ray
    // leaving the right hand in its aim direction. If the weapon model and the
    // aim ray have drifted apart, the dots come off that ray and this angle
    // grows - which is the numeric form of "the gun points somewhere other than
    // where it shoots", and it is otherwise a headset-only judgement.
    double aimDevMax = 0.0, aimDevSum = 0.0;
    int aimDevCount = 0;
    // Per-hand version (BS2 session 41): the legacy number folds EVERY quad
    // against the RIGHT hand's ray, so a second hand's laser reads as 47-75
    // deg of "deviation" that is not one (the dual-beam trap, VERIFICATION
    // 2.8). Here each quad is assigned to the hand whose ray it deviates
    // least from, and the max/count accumulate per hand. The legacy fields
    // keep their old semantics so recorded baselines stay comparable.
    double devMaxH[2] = {0.0, 0.0};
    int devCountH[2] = {0, 0};
    {
        Vec3 aimFwdH[2], gripPosH[2];
        for (int h = 0; h < 2; ++h) {
            aimFwdH[h] =
                v3_norm(quat_rotate(sub.snap.aimWorld[h].q, Vec3{0.0f, 0.0f, -1.0f}));
            gripPosH[h] = sub.snap.gripWorld[h].p;
        }
        for (uint32_t i = 0; i < sub.layerCount; ++i) {
            const SimLayer& L = sub.layers[i];
            if (L.type != XR_TYPE_COMPOSITION_LAYER_QUAD) continue;
            SimSpace* sp = space_get(L.space);
            // Head-locked layers (the HUD) are not on the aim ray by design.
            if (!sp || sp->isAction || sp->refType == XR_REFERENCE_SPACE_TYPE_VIEW) continue;
            Pose base = pose_identity();
            bool tracked = true;
            space_pose(*sp, sub.snap, base, tracked);
            const Pose quadWorld = pose_mul(base, from_xr(L.pose));
            double degH[2] = {-1.0, -1.0};
            for (int h = 0; h < 2; ++h) {
                const Vec3 d = v3_sub(quadWorld.p, gripPosH[h]);
                if (v3_len(d) < 0.05f) continue; // too close to be meaningful
                double c = v3_dot(v3_norm(d), aimFwdH[h]);
                if (c > 1.0) c = 1.0;
                if (c < -1.0) c = -1.0;
                degH[h] = acos(c) * 180.0 / 3.14159265358979323846;
            }
            // Legacy accumulation: right hand, every quad (unchanged).
            if (degH[1] >= 0.0) {
                aimDevSum += degH[1];
                ++aimDevCount;
                if (degH[1] > aimDevMax) aimDevMax = degH[1];
            }
            // Per-hand: the quad belongs to the nearest ray.
            int owner = -1;
            if (degH[0] >= 0.0 && (degH[1] < 0.0 || degH[0] <= degH[1])) owner = 0;
            else if (degH[1] >= 0.0) owner = 1;
            if (owner >= 0) {
                ++devCountH[owner];
                if (degH[owner] > devMaxH[owner]) devMaxH[owner] = degH[owner];
            }
        }
    }

    sprintf_s(buf, "  \"derived\": {\"eyeSeparationM\": %.6f, \"ipdM\": %.6f, "
                   "\"claimTanH\": %.5f, \"eyeTanH\": %.5f, \"claimRatioH\": %.5f, "
                   "\"aimRayDots\": %d, \"aimRayMaxDevDeg\": %.4f, \"aimRayMeanDevDeg\": %.4f, "
                   "\"aimRayDotsL\": %d, \"aimRayMaxDevDegL\": %.4f, "
                   "\"aimRayDotsR\": %d, \"aimRayMaxDevDegR\": %.4f},\n",
              eyeSep, r.ipdM, claimTanH, eyeTanH, (eyeTanH > 0.0) ? claimTanH / eyeTanH : 0.0,
              aimDevCount, aimDevMax, (aimDevCount > 0) ? aimDevSum / aimDevCount : 0.0,
              devCountH[0], devMaxH[0], devCountH[1], devMaxH[1]);
    out += buf;

    sprintf_s(buf, "  \"stats\": {\"meanLumaL\": %.2f, \"meanLumaR\": %.2f, "
                   "\"nonBlackPctL\": %.2f, \"nonBlackPctR\": %.2f}\n}\n",
              meanLuma[0], meanLuma[1], nonBlackPct[0], nonBlackPct[1]);
    out += buf;
    return out;
}

} // namespace

void compositor_note_layers(const SimSubmission& sub) {
    g_lastLayerCount.store(sub.layerCount);
    uint32_t projViews = 0;
    for (uint32_t i = 0; i < sub.layerCount; ++i)
        if (sub.layers[i].type == XR_TYPE_COMPOSITION_LAYER_PROJECTION)
            projViews = sub.layers[i].viewCount;
    g_lastProjViews.store(projViews);
}

void compositor_on_end_frame(const SimSubmission& sub, bool capture) {
    if (!capture || !g_ready || g_disabled) return;
    if (!ensure_targets(g.captureWidth.load(), g.captureHeight.load())) {
        XRSIM_LOG_ONCE("xrsim: capture targets could not be created - captures disabled");
        g_disabled = true;
        return;
    }

    SavedState saved;
    save_state(saved);

    std::vector<LayerStat> stats(sub.layerCount);
    for (int eye = 0; eye < 2; ++eye) compose_eye(eye, sub, stats);

    restore_state(saved);

    // Readback happens inline, on a capture frame only. It stalls the pipeline
    // for a few milliseconds, which is the right trade: a capture that
    // corresponds exactly to the frame the agent asked for is worth more than a
    // faster one that does not.
    EncodeJob job;
    job.width = g_rtW;
    job.height = g_rtH;
    double meanLuma[2] = {0, 0}, nonBlack[2] = {0, 0};

    for (int eye = 0; eye < 2; ++eye) {
        g_ctx->CopyResource(g_staging[eye], g_eyeRt[eye]);
        D3D11_MAPPED_SUBRESOURCE m{};
        if (FAILED(g_ctx->Map(g_staging[eye], 0, D3D11_MAP_READ, 0, &m))) return;
        job.pixels[eye].resize(static_cast<size_t>(g_rtW) * g_rtH * 4);
        const auto* src = static_cast<const uint8_t*>(m.pData);
        uint64_t lumaSum = 0, nonBlackCount = 0;
        for (uint32_t y = 0; y < g_rtH; ++y) {
            const uint8_t* row = src + static_cast<size_t>(y) * m.RowPitch;
            memcpy(&job.pixels[eye][static_cast<size_t>(y) * g_rtW * 4], row, g_rtW * 4);
            for (uint32_t x = 0; x < g_rtW; ++x) {
                const uint32_t b = row[x * 4 + 0], gg = row[x * 4 + 1], rr = row[x * 4 + 2];
                const uint32_t l = (rr * 77 + gg * 151 + b * 28) >> 8;
                lumaSum += l;
                if (l > 8) ++nonBlackCount;
            }
        }
        g_ctx->Unmap(g_staging[eye], 0);
        const double total = static_cast<double>(g_rtW) * g_rtH;
        meanLuma[eye] = lumaSum / total;
        nonBlack[eye] = 100.0 * nonBlackCount / total;
    }

    char baseName[128];
    if (g.captureTag[0])
        sprintf_s(baseName, "%s", g.captureTag);
    else
        sprintf_s(baseName, "f%06llu", static_cast<unsigned long long>(sub.frameIndex));
    job.baseName = baseName;
    job.json = build_json(sub, stats, baseName, g_rtW, g_rtH, meanLuma, nonBlack);

    {
        std::lock_guard<std::mutex> lock(g_qMutex);
        if (g_queue.size() >= 4) g_queue.pop_front(); // bounded: drop rather than stall the game
        g_queue.push_back(std::move(job));
    }
    g_qCv.notify_one();
}

// ---------------------------------------------------------------------------
// Per-eye source-hash log (the BS2 left-eye flicker hunt, issue #31)
// ---------------------------------------------------------------------------
// Fingerprints the PROJECTION layer's two source textures - the exact images
// the game released for each eye - NOT the composited RT: quad layers (laser,
// HUD) move with the head, so a frozen eye image would still change the
// composite and mask itself. A repeated hashL while hashR advances is the
// stale-left signature; hashL matching the previous hashR is the eye-swap
// signature. relAge counts submitted frames since that eye's swapchain was
// last released into - the runtime-side witness of "this eye held an old
// image", independent of the mod's own [pair] bookkeeping.
// Runs on the present thread inside xrEndFrame (same thread as the capture
// path), so the statics below need no lock.

namespace {

ID3D11Texture2D* g_hashStaging[2] = {};
uint32_t g_hashW[2] = {}, g_hashH[2] = {};
DXGI_FORMAT g_hashFmt[2] = {DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN};
FILE* g_hashFile = nullptr;
bool g_hashFileFailed = false;

bool hash_source_tex(int eye, ID3D11Texture2D* tex, uint64_t* outHash, double* outLuma) {
    if (!tex || !g_device || !g_ctx) return false;
    D3D11_TEXTURE2D_DESC d{};
    tex->GetDesc(&d);
    if (g_hashStaging[eye] &&
        (g_hashW[eye] != d.Width || g_hashH[eye] != d.Height || g_hashFmt[eye] != d.Format)) {
        g_hashStaging[eye]->Release();
        g_hashStaging[eye] = nullptr;
    }
    if (!g_hashStaging[eye]) {
        D3D11_TEXTURE2D_DESC sd = d;
        sd.Usage = D3D11_USAGE_STAGING;
        sd.BindFlags = 0;
        sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        sd.MiscFlags = 0;
        sd.MipLevels = 1;
        sd.ArraySize = 1;
        sd.SampleDesc = {1, 0};
        if (FAILED(g_device->CreateTexture2D(&sd, nullptr, &g_hashStaging[eye]))) return false;
        g_hashW[eye] = d.Width;
        g_hashH[eye] = d.Height;
        g_hashFmt[eye] = d.Format;
    }
    g_ctx->CopyResource(g_hashStaging[eye], tex);
    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(g_ctx->Map(g_hashStaging[eye], 0, D3D11_MAP_READ, 0, &m))) return false;
    // FNV-1a over a fixed subsample (every 8th row, every 4th texel): the grid
    // is deterministic, so identical images always agree, and any real frame
    // change lands on it. 2560x2560 -> ~205k texels, well under a millisecond.
    uint64_t hsh = 1469598103934665603ull;
    uint64_t lumaSum = 0, samples = 0;
    const auto* base = static_cast<const uint8_t*>(m.pData);
    for (uint32_t y = 0; y < d.Height; y += 8) {
        const uint8_t* row = base + static_cast<size_t>(y) * m.RowPitch;
        for (uint32_t x = 0; x < d.Width; x += 4) {
            uint32_t px;
            memcpy(&px, row + static_cast<size_t>(x) * 4, 4);
            hsh ^= px;
            hsh *= 1099511628211ull;
            const uint32_t rr = px & 0xFF, gg = (px >> 8) & 0xFF, bb = (px >> 16) & 0xFF;
            lumaSum += (rr * 77 + gg * 151 + bb * 28) >> 8;
            ++samples;
        }
    }
    g_ctx->Unmap(g_hashStaging[eye], 0);
    *outHash = hsh;
    *outLuma = samples ? static_cast<double>(lumaSum) / samples : 0.0;
    return true;
}

} // namespace

void compositor_hash_cleanup() {
    for (int e = 0; e < 2; ++e) {
        if (g_hashStaging[e]) {
            g_hashStaging[e]->Release();
            g_hashStaging[e] = nullptr;
        }
        g_hashW[e] = g_hashH[e] = 0;
        g_hashFmt[e] = DXGI_FORMAT_UNKNOWN;
    }
    if (g_hashFile) {
        fclose(g_hashFile);
        g_hashFile = nullptr;
    }
    g_hashFileFailed = false;
}

void compositor_hash_frame(const SimSubmission& sub) {
    if (!g_device || !g_ctx) return;

    const SimLayer* proj = nullptr;
    for (uint32_t i = 0; i < sub.layerCount; ++i) {
        if (sub.layers[i].type == XR_TYPE_COMPOSITION_LAYER_PROJECTION) {
            proj = &sub.layers[i];
            break;
        }
    }

    uint64_t hsh[2] = {};
    double luma[2] = {};
    uint32_t idx[2] = {}, relFrame[2] = {}, relAge[2] = {};
    if (proj) {
        for (int e = 0; e < 2 && e < static_cast<int>(proj->viewCount); ++e) {
            const XrSwapchain swp = proj->views[e].subImage.swapchain;
            if (!swapchain_release_info(swp, &idx[e], &relFrame[e])) continue;
            relAge[e] = (sub.frameIndex >= relFrame[e])
                            ? static_cast<uint32_t>(sub.frameIndex - relFrame[e])
                            : 0;
            hash_source_tex(e, swapchain_last_image(swp, nullptr, nullptr), &hsh[e], &luma[e]);
        }
    }

    for (int e = 0; e < 2; ++e) {
        g.lastEyeHash[e].store(hsh[e]);
        g.lastEyeRelAge[e].store(relAge[e]);
    }
    g.lastEyeHashFrame.store(sub.frameIndex);

    if (!g_hashFile && !g_hashFileFailed) {
        wchar_t p[MAX_PATH];
        swprintf_s(p, L"%s\\capture\\eyehash.tsv", log::dir());
        g_hashFile = _wfsopen(p, L"w", _SH_DENYNO);
        if (!g_hashFile) {
            g_hashFileFailed = true;
            XRSIM_LOG_ONCE("xrsim: eyehash.tsv could not be opened - hash log disabled");
            return;
        }
        fputs("frame\tdisplayNs\tproj\thashL\thashR\trelAgeL\trelAgeR\timgIdxL\timgIdxR"
              "\tlumaL\tlumaR\tlayers\tstate\n",
              g_hashFile);
    }
    if (g_hashFile) {
        fprintf(g_hashFile,
                "%llu\t%lld\t%d\t%016llx\t%016llx\t%u\t%u\t%u\t%u\t%.2f\t%.2f\t%u\t%s\n",
                static_cast<unsigned long long>(sub.frameIndex),
                static_cast<long long>(sub.displayTime), proj ? 1 : 0,
                static_cast<unsigned long long>(hsh[0]), static_cast<unsigned long long>(hsh[1]),
                relAge[0], relAge[1], idx[0], idx[1], luma[0], luma[1], sub.layerCount,
                session_state_name(sub.snap.state));
        // Flushed per line so a live tail (and a post-crash read) always sees
        // the latest frame; at 80 presents/s the cost is noise next to the
        // staging Map above.
        fflush(g_hashFile);
    }
}

} // namespace xrsim
