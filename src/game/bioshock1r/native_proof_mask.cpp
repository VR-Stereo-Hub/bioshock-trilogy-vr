// Local diagnostic only: exact selected native pixels and measured GPU work.
#include "game/bioshock1r/native_proof_mask.h"
#include "game/bioshock1r/native_audit_com.h"
#include "core/gfx/frame_inspector.h"
#include "core/util/log.h"
#include <d3dcompiler.h>
#include <cstring>
#include <initializer_list>

namespace bvr::b1r::native_proof::detail {
namespace {
using native_audit::ComPtr;
ComPtr<ID3D11Device> g_device;
ComPtr<ID3D11VertexShader> g_vs;
ComPtr<ID3D11PixelShader> g_ps[5];
ComPtr<ID3D11DepthStencilState> g_mark;
ComPtr<ID3D11RasterizerState> g_raster;
ComPtr<ID3D11Query> g_work, g_disjoint, g_time[4];
LARGE_INTEGER g_frequency{};
const char* const kNames[] = {"unmasked", "empty", "tiles1", "tiles5", "all"};
const char* kShader = R"hlsl(
float4 vs(uint id:SV_VertexID):SV_Position {
    float2 uv=float2((id<<1)&2,id&2);
    return float4(uv*float2(2,-2)+float2(-1,1),0,1);
}
float4 ps(float4 p:SV_Position):SV_Target {
    uint2 tile=uint2(p.xy)/8;
    uint h=(tile.x*73856093u)^(tile.y*19349663u); h^=h>>13;
#if MODE == 1
    discard;
#elif MODE == 2
    if(h%1000>=10) discard;
#elif MODE == 3
    if(h%1000>=50) discard;
#endif
    return 0;
}
)hlsl";
ComPtr<ID3DBlob> compile(const char* entry, const char* profile, const char* mode) {
    D3D_SHADER_MACRO defines[] = {{"MODE",mode},{nullptr,nullptr}};
    ComPtr<ID3DBlob> code, errors;
    if (FAILED(D3DCompile(kShader, strlen(kShader), nullptr, defines, nullptr, entry, profile,
                         D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, code.GetAddressOf(), errors.GetAddressOf())))
        code.Reset();
    return code;
}
// Exactly the state touched by mask marking. Eligibility already excludes class
// instances, extra shader stages, UAV/SO effects and target/SRV aliases.
struct State {
    ID3D11DeviceContext* ctx;
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> ps;
    ComPtr<ID3D11InputLayout> layout;
    ComPtr<ID3D11RasterizerState> raster;
    ComPtr<ID3D11DepthStencilState> depth;
    ComPtr<ID3D11BlendState> blend;
    ComPtr<ID3D11RenderTargetView> rt;
    ComPtr<ID3D11DepthStencilView> ds;
    UINT ref = 0, sampleMask = 0, nv = 16;
    FLOAT factor[4]{};
    D3D11_VIEWPORT viewports[16]{};
    D3D11_PRIMITIVE_TOPOLOGY topology{};
    explicit State(ID3D11DeviceContext* c) : ctx(c) {
        ctx->VSGetShader(vs.GetAddressOf(), nullptr, nullptr);
        ctx->PSGetShader(ps.GetAddressOf(), nullptr, nullptr);
        ctx->IAGetInputLayout(layout.GetAddressOf()); ctx->IAGetPrimitiveTopology(&topology);
        ctx->RSGetState(raster.GetAddressOf()); ctx->RSGetViewports(&nv, viewports);
        ctx->OMGetDepthStencilState(depth.GetAddressOf(), &ref);
        ctx->OMGetBlendState(blend.GetAddressOf(), factor, &sampleMask);
        ctx->OMGetRenderTargets(1, rt.GetAddressOf(), ds.GetAddressOf());
    }
    void restore() const {
        ctx->VSSetShader(vs.Get(), nullptr, 0); ctx->PSSetShader(ps.Get(), nullptr, 0);
        ctx->IASetInputLayout(layout.Get()); ctx->IASetPrimitiveTopology(topology);
        ctx->RSSetState(raster.Get()); ctx->RSSetViewports(nv, viewports);
        ctx->OMSetDepthStencilState(depth.Get(), ref);
        ctx->OMSetBlendState(blend.Get(), factor, sampleMask);
        auto* target = rt.Get(); ctx->OMSetRenderTargets(1, &target, ds.Get());
    }
    ~State() { restore(); }
};
D3D11_DEPTH_STENCIL_DESC description(ID3D11DepthStencilState* state) {
    D3D11_DEPTH_STENCIL_DESC d{};
    if (state) state->GetDesc(&d);
    else {
        d.DepthEnable = TRUE; d.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
        d.DepthFunc = D3D11_COMPARISON_LESS; d.StencilReadMask = d.StencilWriteMask = 255;
        d.FrontFace = {D3D11_STENCIL_OP_KEEP,D3D11_STENCIL_OP_KEEP,D3D11_STENCIL_OP_KEEP,D3D11_COMPARISON_ALWAYS};
        d.BackFace = d.FrontFace;
    }
    return d;
}
bool wait_query(ID3D11DeviceContext* ctx, ID3D11Query* query, void* data, UINT size) {
    const auto start = GetTickCount64();
    for (;;) {
        const HRESULT hr = ctx->GetData(query, data, size, 0);
        if (hr == S_OK) return true;
        if (hr != S_FALSE || GetTickCount64() - start > 1000) return false;
        Sleep(1);
    }
}
struct Read {
    ID3D11DeviceContext* ctx;
    ID3D11Texture2D* tex;
    D3D11_MAPPED_SUBRESOURCE mapped{};
    Read(ID3D11DeviceContext* c, ID3D11Texture2D* t) : ctx(c), tex(t) {
        if (FAILED(ctx->Map(tex, 0, D3D11_MAP_READ, 0, &mapped))) mapped.pData = nullptr;
    }
    ~Read() { if (mapped.pData) ctx->Unmap(tex, 0); }
    const uint32_t* row(UINT y) const {
        return reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>(mapped.pData) + y * mapped.RowPitch);
    }
};
} // namespace
bool mask_selected(UINT x, UINT y, UINT mode) {
    if (mode == 0 || mode == 4) return true;
    if (mode == 1) return false;
    UINT h = (x / 8) * 73856093u ^ (y / 8) * 19349663u; h ^= h >> 13;
    return h % 1000 < (mode == 2 ? 10u : 50u);
}
bool masked_eligible(ID3D11DeviceContext* ctx, UINT flags) {
    ComPtr<ID3D11DepthStencilState> state; UINT ref = 0;
    ctx->OMGetDepthStencilState(state.GetAddressOf(), &ref);
    // The copied stencil plane belongs solely to this experiment, only while the
    // native draw does not use it. A native stencil operation is never replaced.
    return !description(state.Get()).StencilEnable && !(flags & D3D11_DSV_READ_ONLY_STENCIL);
}
void masked_stop() {
    g_vs.Reset(); for (auto& ps : g_ps) ps.Reset(); g_mark.Reset(); g_raster.Reset();
    g_work.Reset(); g_disjoint.Reset(); for (auto& time : g_time) time.Reset(); g_device.Reset();
}
bool masked_init(ID3D11Device* device) {
    if (g_device) return g_device.Get() == device;
    if (!QueryPerformanceFrequency(&g_frequency) || g_frequency.QuadPart <= 0) return false;
    auto vs = compile("vs", "vs_5_0", "0");
    if (!vs || FAILED(device->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, g_vs.GetAddressOf()))) return false;
    const char* modes[] = {"0","1","2","3","4"};
    for (UINT i = 1; i < 5; ++i) {
        auto ps = compile("ps", "ps_5_0", modes[i]);
        if (!ps || FAILED(device->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, g_ps[i].GetAddressOf()))) { masked_stop(); return false; }
    }
    D3D11_DEPTH_STENCIL_DESC d{}; d.DepthEnable = FALSE; d.DepthFunc = D3D11_COMPARISON_ALWAYS;
    d.StencilEnable = TRUE; d.StencilReadMask = d.StencilWriteMask = 255;
    d.FrontFace = {D3D11_STENCIL_OP_KEEP,D3D11_STENCIL_OP_KEEP,D3D11_STENCIL_OP_REPLACE,D3D11_COMPARISON_ALWAYS}; d.BackFace = d.FrontFace;
    D3D11_RASTERIZER_DESC r{}; r.FillMode = D3D11_FILL_SOLID; r.CullMode = D3D11_CULL_NONE; r.DepthClipEnable = TRUE;
    bool ok = SUCCEEDED(device->CreateDepthStencilState(&d, g_mark.GetAddressOf())) &&
              SUCCEEDED(device->CreateRasterizerState(&r, g_raster.GetAddressOf()));
    D3D11_QUERY_DESC q{D3D11_QUERY_PIPELINE_STATISTICS,0};
    ok = ok && SUCCEEDED(device->CreateQuery(&q, g_work.GetAddressOf()));
    q.Query = D3D11_QUERY_TIMESTAMP_DISJOINT;
    ok = ok && SUCCEEDED(device->CreateQuery(&q, g_disjoint.GetAddressOf()));
    q.Query = D3D11_QUERY_TIMESTAMP;
    for (auto& time : g_time) ok = ok && SUCCEEDED(device->CreateQuery(&q, time.GetAddressOf()));
    if (!ok) { masked_stop(); return false; }
    g_device = device; return true;
}
bool compare_selected(ID3D11DeviceContext* ctx, const ProofSurface& s, bool depth, UINT mode, MaskStats& out) {
    if (!ctx || !s.before || !s.expected || !s.result || mode > 4 ||
        s.before == s.expected || s.before == s.result || s.expected == s.result) return false;
    D3D11_TEXTURE2D_DESC d{}; s.expected->GetDesc(&d);
    if (depth ? (d.Format != DXGI_FORMAT_R24G8_TYPELESS && d.Format != DXGI_FORMAT_D24_UNORM_S8_UINT) :
                (d.Format != DXGI_FORMAT_R11G11B10_FLOAT)) return false;
    for (auto* texture : {s.before,s.expected,s.result}) {
        D3D11_TEXTURE2D_DESC t{}; texture->GetDesc(&t);
        if (t.Width != d.Width || t.Height != d.Height || t.Format != d.Format || t.ArraySize != 1 ||
            t.MipLevels != 1 || t.SampleDesc.Count != 1 || t.Usage != D3D11_USAGE_STAGING ||
            !(t.CPUAccessFlags & D3D11_CPU_ACCESS_READ)) return false;
    }
    Read before(ctx,s.before), expected(ctx,s.expected), actual(ctx,s.result);
    if (!before.mapped.pData || !expected.mapped.pData || !actual.mapped.pData) return false;
    const uint32_t bits = depth && mode != 0 ? 0x00ffffffu : 0xffffffffu;
    for (UINT y = 0; y < d.Height; ++y) {
        const auto *b = before.row(y), *e = expected.row(y), *a = actual.row(y);
        for (UINT x = 0; x < d.Width; ++x) {
            const bool yes = mask_selected(x,y,mode);
            if (!depth) out.selected += yes;
            out.selectedChanged += yes && (e[x] & bits) != (b[x] & bits);
            if (yes) out.selectedDiff += (a[x] & bits) != (e[x] & bits);
            else out.outsideDiff += (a[x] & bits) != (b[x] & bits);
            if (depth && mode != 0) out.stencilDiff += (a[x] >> 24) != unsigned(yes);
        }
    }
    return true;
}
bool masked_run(ID3D11DeviceContext* ctx, const frame_inspector::DrawArgs& args,
                const ProofSurface& colour, const ProofSurface& depth,
                ID3D11RenderTargetView* rtv, ID3D11DepthStencilView* dsv,
                MaskStats (&totals)[5], uint32_t draw) {
    if (!g_device) return false;
    State native(ctx);
    auto desc = description(native.depth.Get());
    if (desc.StencilEnable) return false;
    desc.StencilEnable = TRUE; desc.StencilReadMask = 255; desc.StencilWriteMask = 0;
    desc.FrontFace = {D3D11_STENCIL_OP_KEEP,D3D11_STENCIL_OP_KEEP,D3D11_STENCIL_OP_KEEP,D3D11_COMPARISON_EQUAL}; desc.BackFace = desc.FrontFace;
    ComPtr<ID3D11DepthStencilState> masked;
    if (FAILED(g_device->CreateDepthStencilState(&desc, masked.GetAddressOf()))) return false;
    D3D11_TEXTURE2D_DESC td{}; colour.scratch->GetDesc(&td);
    const D3D11_VIEWPORT vp{0,0,float(td.Width),float(td.Height),0,1};
    uint64_t referenceIA = 0;
    for (UINT mode = 0; mode < 5; ++mode) {
        MaskStats r{};
        ctx->Begin(g_disjoint.Get()); ctx->End(g_time[0].Get());
        // STAGING -> DEFAULT is an actual resource copy. No resource is mapped
        // here. Copy cost is timed separately from mask marking and native draw.
        ctx->CopyResource(colour.scratch, colour.before); ctx->CopyResource(depth.scratch, depth.before);
        ctx->End(g_time[1].Get());
        if (mode != 0) {
            ctx->OMSetRenderTargets(0, nullptr, dsv);
            ctx->ClearDepthStencilView(dsv, D3D11_CLEAR_STENCIL, 1, 0);
            ctx->OMSetDepthStencilState(g_mark.Get(), 1); ctx->OMSetBlendState(nullptr, nullptr, ~0u);
            ctx->RSSetState(g_raster.Get()); ctx->RSSetViewports(1,&vp);
            ctx->IASetInputLayout(nullptr); ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            ctx->VSSetShader(g_vs.Get(),nullptr,0); ctx->PSSetShader(g_ps[mode].Get(),nullptr,0);
            ctx->Draw(3,0); // caller suppression excludes every observer/tap/query hook
        }
        native.restore();
        ctx->OMSetRenderTargets(1,&rtv,dsv);
        if (mode != 0) ctx->OMSetDepthStencilState(masked.Get(),1);
        ctx->End(g_time[2].Get()); ctx->Begin(g_work.Get());
        LARGE_INTEGER submitted{}, returned{}; QueryPerformanceCounter(&submitted);
        const bool issued = frame_inspector::replay_draw(ctx,args);
        QueryPerformanceCounter(&returned);
        r.submitUs = double(returned.QuadPart-submitted.QuadPart)*1e6/g_frequency.QuadPart;
        ctx->End(g_work.Get()); ctx->End(g_time[3].Get()); ctx->End(g_disjoint.Get());
        native.restore(); // all live state restored before any readback or wait
        ctx->CopyResource(colour.result,colour.scratch); ctx->CopyResource(depth.result,depth.scratch);
        bool ok = issued && compare_selected(ctx,colour,false,mode,r) && compare_selected(ctx,depth,true,mode,r);
        D3D11_QUERY_DATA_PIPELINE_STATISTICS work{}; D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjoint{};
        UINT64 time[4]{};
        ok = ok && wait_query(ctx,g_work.Get(),&work,sizeof(work)) && wait_query(ctx,g_disjoint.Get(),&disjoint,sizeof(disjoint));
        for (UINT i = 0; i < 4; ++i) ok = ok && wait_query(ctx,g_time[i].Get(),&time[i],sizeof(time[i]));
        r.ia = work.IAVertices; r.vs = work.VSInvocations; r.ps = work.PSInvocations;
        if (mode == 0) referenceIA = r.ia;
        // IA counts prove the same submitted geometry. VS invocation counts are
        // measurements, not a correctness precondition: shader/cache work may vary.
        const bool geometry = r.ia == referenceIA && r.ia != 0;
        const bool exact = ok && geometry && !r.selectedDiff && !r.outsideDiff && !r.stencilDiff;
        r.timed = ok && !disjoint.Disjoint && disjoint.Frequency && time[0] <= time[1] && time[1] <= time[2] && time[2] <= time[3];
        if (r.timed) {
            const double ms = 1000.0 / disjoint.Frequency;
            r.copyMs = (time[1]-time[0])*ms; r.maskMs = (time[2]-time[1])*ms; r.drawMs = (time[3]-time[2])*ms;
        }
        BVR_LOG("[native-proof] mask draw=%u case=%s selected=%llu changedChannels=%llu selectedDiff=%llu outsideDiff=%llu stencilDiff=%llu IA=%llu VS=%llu PS=%llu copyMs=%.4f maskMs=%.4f drawMs=%.4f submitUs=%.3f timing=%s check=%s",
            draw,kNames[mode],r.selected,r.selectedChanged,r.selectedDiff,r.outsideDiff,r.stencilDiff,r.ia,r.vs,r.ps,
            r.copyMs,r.maskMs,r.drawMs,r.submitUs,r.timed ? "VALID" : "UNAVAILABLE",exact ? "OK" : "FAILED");
        auto& total = totals[mode]; ++total.tested; total.timed += r.timed;
        total.selected += r.selected; total.selectedChanged += r.selectedChanged;
        total.selectedDiff += r.selectedDiff; total.outsideDiff += r.outsideDiff; total.stencilDiff += r.stencilDiff;
        total.ia += r.ia; total.vs += r.vs; total.ps += r.ps;
        total.copyMs += r.copyMs; total.maskMs += r.maskMs; total.drawMs += r.drawMs; total.submitUs += r.submitUs;
        if (!exact) return false;
    }
    return true;
}
} // namespace bvr::b1r::native_proof::detail
