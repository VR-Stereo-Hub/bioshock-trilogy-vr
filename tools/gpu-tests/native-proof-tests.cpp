// gpu-test-sources: src/core/gfx/frame_inspector.cpp src/core/gfx/shader_bytecode.cpp src/game/bioshock1r/native_proof.cpp src/game/bioshock1r/native_proof_mask.cpp src/game/bioshock1r/native_proof_eye.cpp
// gpu-test-libs: build/third_party/minhook/RelWithDebInfo/minhook.x32.lib
// Real D3D11/MinHook draw detours and native scratch replay, no game or XR runtime.
#include "gpu_test_util.h"
#include "core/gfx/frame_inspector.h"
#include "core/gfx/shader_bytecode.h"
#include "core/gfx/hud_capture.h"
#include "core/gfx/gfx_hud.h"
#include "game/bioshock1r/native_proof.h"
#include "game/bioshock1r/native_proof_mask.h"
#include "game/bioshock1r/native_audit_com.h"
#include <MinHook.h>
#include <imgui.h>
using bvr::b1r::native_audit::ComPtr;
namespace fi = bvr::frame_inspector;
namespace sb = bvr::shader_bytecode;
namespace np = bvr::b1r::native_proof;
namespace bvr::log {
const wchar_t* data_dir() { return L""; }
void write(const char* fmt, ...) { va_list a; va_start(a, fmt); vprintf(fmt, a); printf("\n"); va_end(a); }
}
// UI classifiers are inert in this synthetic fixture. Draw hooks are production.
namespace bvr::hud {
void on_setrt(UINT, ID3D11RenderTargetView* const*, ID3D11DepthStencilView*) {}
void on_draw_indexed(ID3D11DeviceContext*) {}
DrawDecision on_draw(ID3D11DeviceContext*, UINT) { return {}; }
ID3D11DepthStencilView* capture_dsv() { return nullptr; }
void fix_blend_alpha(ID3D11DeviceContext*) {}
}
namespace bvr::gfx_hud {
void on_setrt(unsigned, ID3D11RenderTargetView* const*, ID3D11DepthStencilView*) {}
Decision on_draw_indexed(ID3D11DeviceContext*, unsigned) { return {}; }
Decision on_draw(ID3D11DeviceContext*, unsigned) { return {}; }
}
namespace ImGui {
void Text(const char*, ...) {}
void TextWrapped(const char*, ...) {}
bool Button(const char*, const ImVec2&) { return false; }
void SameLine(float, float) {}
}
static ComPtr<ID3DBlob> compile(const char* code, const char* target) {
    ComPtr<ID3DBlob> blob, error;
    if (FAILED(D3DCompile(code, strlen(code), nullptr, nullptr, nullptr, "main", target, 0, 0,
                         blob.GetAddressOf(), error.GetAddressOf()))) {
        printf("compile: %s\n", error ? static_cast<const char*>(error->GetBufferPointer()) : "failed");
    }
    return blob;
}
static bool upload(ID3D11Buffer* buffer, const void* data, size_t bytes) {
    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(gt::ctx->Map(buffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return false;
    memcpy(m.pData, data, bytes); gt::ctx->Unmap(buffer, 0); return true;
}
static int beforeCount = 0, afterCount = 0;
static bool pair_before(ID3D11DeviceContext*, const fi::DrawArgs&) {
    ++beforeCount; fi::set_draw_replay_probe(nullptr); return true;
}
static void pair_after(ID3D11DeviceContext* ctx, const fi::DrawArgs& a) {
    ++afterCount;
    gt::check(fi::replay_draw(ctx, a), "suppressed replay", "original draw callable inside paired callback");
}
const fi::DrawReplayProbe pairProbe{pair_before, pair_after, nullptr};
static bool false_before(ID3D11DeviceContext*, const fi::DrawArgs&) { ++beforeCount; return false; }
const fi::DrawReplayProbe falseProbe{false_before, pair_after, nullptr};
static bool wait_query(ID3D11Query* q, void* data, UINT bytes) {
    const ULONGLONG start = GetTickCount64();
    for (;;) {
        const HRESULT hr = gt::ctx->GetData(q, data, bytes, 0);
        if (hr == S_OK) return true;
        if (hr != S_FALSE || GetTickCount64() - start > 5000) return false;
        Sleep(1);
    }
}
int main(int argc, char** argv) {
    const bool masked = argc > 1 && (strcmp(argv[1], "--masked") == 0 || strcmp(argv[1], "--maskguards") == 0 || strcmp(argv[1], "--masked-large") == 0 || strcmp(argv[1], "--masked-late") == 0);
    const bool guards = argc > 1 && (strcmp(argv[1], "--guards") == 0 || strcmp(argv[1], "--maskguards") == 0);
    const bool large = argc > 1 && strcmp(argv[1], "--masked-large") == 0;
    const bool late = argc > 1 && strcmp(argv[1], "--masked-late") == 0;
    const UINT width = large ? 2688u : 96u, height = large ? 2880u : 80u;
    if (!gt::make_device()) return 1;
    const auto vsCode = compile("float4 main(float3 p:POSITION):SV_Position { return float4(p,1); }", "vs_5_0");
    const auto psCode = compile("cbuffer C:register(b0){float4 colour;}; float4 main():SV_Target{return colour;}", "ps_5_0");
    if (!vsCode || !psCode) return 1;
    ComPtr<ID3D11PixelShader> noMetadata;
    gt::dev->CreatePixelShader(psCode->GetBufferPointer(), psCode->GetBufferSize(), nullptr, noMetadata.GetAddressOf());
    const auto* base = reinterpret_cast<const uint8_t*>(GetModuleHandleW(nullptr));
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    sb::configure(base, nt->OptionalHeader.SizeOfImage);
    fi::set_draw_replay_probe(&pairProbe); // install opt-in query hooks too
    if (MH_Initialize() != MH_OK || !sb::install(gt::dev) || !fi::install(*reinterpret_cast<void***>(gt::ctx), gt::ctx)) return 1;
    fi::set_draw_replay_probe(nullptr);
    sb::bind_device(gt::dev);
    ComPtr<ID3D11VertexShader> vs; ComPtr<ID3D11PixelShader> ps; ComPtr<ID3D11InputLayout> il;
    gt::dev->CreateVertexShader(vsCode->GetBufferPointer(), vsCode->GetBufferSize(), nullptr, vs.GetAddressOf());
    gt::dev->CreatePixelShader(psCode->GetBufferPointer(), psCode->GetBufferSize(), nullptr, ps.GetAddressOf());
    ComPtr<ID3D11PixelShader> latePs;
    if (late) {
        const auto code=compile("cbuffer C:register(b0){float4 colour;}; struct O{float4 c:SV_Target;float z:SV_Depth;}; O main(float4 p:SV_Position){clip(frac(p.x*.125)-.2);O o;o.c=colour;o.z=.0001;return o;}","ps_5_0");
        if (!code || FAILED(gt::dev->CreatePixelShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,latePs.GetAddressOf()))) return 1;
        ps=latePs;
    }
    const D3D11_INPUT_ELEMENT_DESC e{"POSITION",0,DXGI_FORMAT_R32G32B32_FLOAT,0,0,D3D11_INPUT_PER_VERTEX_DATA,0};
    gt::dev->CreateInputLayout(&e,1,vsCode->GetBufferPointer(),vsCode->GetBufferSize(),il.GetAddressOf());
    const float vertices[] = {-1,1,.5f, 3,1,.5f, -1,-3,.5f};
    const uint16_t indices[] = {0,1,2};
    ComPtr<ID3D11Buffer> vb, ib, cb;
    D3D11_BUFFER_DESC bd{}; bd.ByteWidth = sizeof(vertices); bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER; bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    gt::dev->CreateBuffer(&bd,nullptr,vb.GetAddressOf());
    bd.ByteWidth=sizeof(indices); bd.BindFlags=D3D11_BIND_INDEX_BUFFER;
    gt::dev->CreateBuffer(&bd,nullptr,ib.GetAddressOf());
    bd.ByteWidth=16; bd.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
    gt::dev->CreateBuffer(&bd,nullptr,cb.GetAddressOf());
    if (!upload(vb.Get(),vertices,sizeof(vertices)) || !upload(ib.Get(),indices,sizeof(indices))) return 1;
    ComPtr<ID3D11Texture2D> target, depth, second;
    target.Attach(gt::make_tex(width,height,DXGI_FORMAT_R11G11B10_FLOAT,D3D11_BIND_RENDER_TARGET));
    depth.Attach(gt::make_tex(width,height,DXGI_FORMAT_R24G8_TYPELESS,D3D11_BIND_DEPTH_STENCIL|D3D11_BIND_SHADER_RESOURCE));
    second.Attach(gt::make_tex(width,height,DXGI_FORMAT_R11G11B10_FLOAT,D3D11_BIND_RENDER_TARGET));
    ComPtr<ID3D11RenderTargetView> rt, rt2; ComPtr<ID3D11DepthStencilView> ds, dsRead;
    gt::dev->CreateRenderTargetView(target.Get(),nullptr,rt.GetAddressOf());
    gt::dev->CreateRenderTargetView(second.Get(),nullptr,rt2.GetAddressOf());
    D3D11_DEPTH_STENCIL_VIEW_DESC dv{}; dv.Format=DXGI_FORMAT_D24_UNORM_S8_UINT; dv.ViewDimension=D3D11_DSV_DIMENSION_TEXTURE2D;
    gt::dev->CreateDepthStencilView(depth.Get(),&dv,ds.GetAddressOf());
    dv.Flags=D3D11_DSV_READ_ONLY_DEPTH|D3D11_DSV_READ_ONLY_STENCIL;
    gt::dev->CreateDepthStencilView(depth.Get(),&dv,dsRead.GetAddressOf());
    ComPtr<ID3D11ShaderResourceView> depthSrv; depthSrv.Attach(gt::srv_of(depth.Get(),DXGI_FORMAT_R24_UNORM_X8_TYPELESS));
    D3D11_DEPTH_STENCIL_DESC dd{}; dd.DepthEnable=TRUE; dd.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ALL; dd.DepthFunc=D3D11_COMPARISON_LESS_EQUAL;
    dd.StencilEnable=!masked; dd.StencilReadMask=255; dd.StencilWriteMask=255;
    dd.FrontFace={D3D11_STENCIL_OP_KEEP,D3D11_STENCIL_OP_KEEP,D3D11_STENCIL_OP_REPLACE,D3D11_COMPARISON_ALWAYS}; dd.BackFace=dd.FrontFace;
    ComPtr<ID3D11DepthStencilState> dsState, noDepthWrite, stencilActive;
    gt::dev->CreateDepthStencilState(&dd,dsState.GetAddressOf());
    dd.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ZERO; gt::dev->CreateDepthStencilState(&dd,noDepthWrite.GetAddressOf());
    dd.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ALL; dd.StencilEnable=TRUE;
    gt::dev->CreateDepthStencilState(&dd,stencilActive.GetAddressOf());
    ComPtr<ID3D11DepthStencilView> stencilRead;
    dv.Flags=D3D11_DSV_READ_ONLY_STENCIL; gt::dev->CreateDepthStencilView(depth.Get(),&dv,stencilRead.GetAddressOf());
    D3D11_RASTERIZER_DESC rd{}; rd.FillMode=D3D11_FILL_SOLID; rd.CullMode=D3D11_CULL_NONE; rd.DepthClipEnable=TRUE;
    rd.ScissorEnable=masked;
    ComPtr<ID3D11RasterizerState> raster; gt::dev->CreateRasterizerState(&rd,raster.GetAddressOf());
    ComPtr<ID3D11BlendState> additive, alpha;
    D3D11_BLEND_DESC blend{}; auto& b=blend.RenderTarget[0]; b.BlendEnable=TRUE; b.SrcBlend=D3D11_BLEND_ONE; b.DestBlend=D3D11_BLEND_ONE;
    b.BlendOp=b.BlendOpAlpha=D3D11_BLEND_OP_ADD; b.SrcBlendAlpha=b.DestBlendAlpha=D3D11_BLEND_ONE; b.RenderTargetWriteMask=D3D11_COLOR_WRITE_ENABLE_ALL;
    gt::dev->CreateBlendState(&blend,additive.GetAddressOf());
    b.SrcBlend=D3D11_BLEND_SRC_ALPHA; b.DestBlend=D3D11_BLEND_INV_SRC_ALPHA;
    gt::dev->CreateBlendState(&blend,alpha.GetAddressOf());
    const D3D11_VIEWPORT vp = masked ? D3D11_VIEWPORT{3,2,float(width-8),float(height-6),0,.01f} : D3D11_VIEWPORT{0,0,float(width),float(height),0,.01f};
    const D3D11_RECT scissor{3,5,LONG(width-9),LONG(height-4)}; gt::ctx->RSSetScissorRects(1,&scissor);
    gt::ctx->RSSetViewports(1,&vp); gt::ctx->RSSetState(raster.Get());
    gt::ctx->VSSetShader(vs.Get(),nullptr,0); gt::ctx->PSSetShader(ps.Get(),nullptr,0);
    gt::ctx->IASetInputLayout(il.Get()); gt::ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    const UINT stride=12, offset=0;
    gt::ctx->IASetVertexBuffers(0,1,vb.GetAddressOf(),&stride,&offset); gt::ctx->IASetIndexBuffer(ib.Get(),DXGI_FORMAT_R16_UINT,0);
    gt::ctx->PSSetConstantBuffers(0,1,cb.GetAddressOf()); gt::ctx->OMSetDepthStencilState(dsState.Get(),71);
    gt::ctx->OMSetRenderTargets(1,rt.GetAddressOf(),ds.Get());
    const float clear[4]={.125f,.25f,.5f,1}, colour[4]={.25f,.125f,.0625f,.5f};
    gt::ctx->ClearRenderTargetView(rt.Get(),clear); gt::ctx->ClearDepthStencilView(ds.Get(),D3D11_CLEAR_DEPTH|D3D11_CLEAR_STENCIL,1,9);
    if (!upload(cb.Get(),colour,16)) return 1;
    // Independent exact-readback negative controls, including row-pitch padding.
    for (const auto format : {DXGI_FORMAT_R11G11B10_FLOAT, DXGI_FORMAT_R24G8_TYPELESS}) {
        D3D11_TEXTURE2D_DESC td{}; td.Width=95; td.Height=79; td.ArraySize=td.MipLevels=1;
        td.Format=format; td.SampleDesc.Count=1; td.Usage=D3D11_USAGE_STAGING; td.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        std::vector<uint32_t> pixels(td.Width*td.Height,0x00800400);
        D3D11_SUBRESOURCE_DATA sd{pixels.data(),td.Width*4,0};
        ComPtr<ID3D11Texture2D> first, equal, wrong;
        gt::dev->CreateTexture2D(&td,&sd,first.GetAddressOf()); gt::dev->CreateTexture2D(&td,&sd,equal.GetAddressOf());
        pixels[td.Width+17] ^= format==DXGI_FORMAT_R24G8_TYPELESS ? 0x01000000 : 1;
        gt::dev->CreateTexture2D(&td,&sd,wrong.GetAddressOf());
        uint64_t differences=999;
        gt::check(np::detail::compare_pixels(gt::ctx,first.Get(),equal.Get(),differences) && differences==0,
            "readback equality", "format=%u ignores row padding",format);
        gt::check(np::detail::compare_pixels(gt::ctx,first.Get(),wrong.Get(),differences) && differences==1,
            "readback negative control", "format=%u catches one corrupted colour/stencil pixel, got %llu",format,differences);
        gt::check(!np::detail::compare_pixels(gt::ctx,first.Get(),target.Get(),differences),
            "invalid readback refused", "a non-staging/incompatible surface cannot pass as equal");
    }
    if (masked && !guards && !large && !late) {
        // A deliberately wrong selected colour/depth, outside pixel and stencil
        // bit must each fail the matching oracle. Odd dimensions exercise pitch.
        for (bool isDepth : {false,true}) {
            D3D11_TEXTURE2D_DESC td{}; td.Width=95; td.Height=79; td.ArraySize=td.MipLevels=1;
            td.Format=isDepth ? DXGI_FORMAT_R24G8_TYPELESS : DXGI_FORMAT_R11G11B10_FLOAT;
            td.SampleDesc.Count=1; td.Usage=D3D11_USAGE_STAGING; td.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
            std::vector<uint32_t> before(td.Width*td.Height,isDepth?0x0900007f:0x00010000);
            std::vector<uint32_t> expected(before.size(),isDepth?0x09599999:0x00020000), result(before.size());
            size_t outside=0;
            for(UINT y=0;y<td.Height;++y)for(UINT x=0;x<td.Width;++x) {
                const auto i=size_t(y)*td.Width+x; const bool yes=np::detail::mask_selected(x,y,2);
                result[i]=yes?expected[i]:before[i];
                if(isDepth)result[i]=(result[i]&0x00ffffffu)|(uint32_t(yes)<<24);
                if(!yes)outside=i;
            }
            auto texture=[&](const std::vector<uint32_t>& pixels){
                ComPtr<ID3D11Texture2D> t; D3D11_SUBRESOURCE_DATA data{pixels.data(),td.Width*4,0};
                gt::dev->CreateTexture2D(&td,&data,t.GetAddressOf()); return t;
            };
            auto beforeTex=texture(before),expectedTex=texture(expected),actualTex=texture(result);
            np::detail::ProofSurface surfaces{nullptr,beforeTex.Get(),expectedTex.Get(),actualTex.Get()}; np::MaskStats score{};
            gt::check(np::detail::compare_selected(gt::ctx,surfaces,isDepth,2,score)&&!score.selectedDiff&&!score.outsideDiff&&!score.stencilDiff,
                "mask oracle baseline","depth=%u clean selected and outside data",isDepth);
            result[0]^=1; actualTex=texture(result);surfaces.result=actualTex.Get();score={};
            gt::check(np::detail::compare_selected(gt::ctx,surfaces,isDepth,2,score)&&score.selectedDiff==1&&!score.outsideDiff&&!score.stencilDiff,
                "selected negative control","depth=%u catches one corrupted selected pixel",isDepth);
            result[0]^=1;result[outside]^=1;actualTex=texture(result);surfaces.result=actualTex.Get();score={};
            gt::check(np::detail::compare_selected(gt::ctx,surfaces,isDepth,2,score)&&!score.selectedDiff&&score.outsideDiff==1&&!score.stencilDiff,
                "outside negative control","depth=%u catches one changed unselected pixel",isDepth);
            result[outside]^=1;
            if(isDepth) {
                result[0]^=0x01000000;actualTex=texture(result);surfaces.result=actualTex.Get();score={};
                gt::check(np::detail::compare_selected(gt::ctx,surfaces,true,2,score)&&!score.selectedDiff&&!score.outsideDiff&&score.stencilDiff==1,
                    "stencil negative control","mask marking is checked independently of depth");
            }
            surfaces.result=target.Get();score={};
            gt::check(!np::detail::compare_selected(gt::ctx,surfaces,isDepth,2,score),"invalid mask readback refused","wrong format/usage cannot pass equality");
        }
    }
    const fi::DrawArgs draw{fi::DrawKind::Direct,3,0,0};
    gt::check(!fi::replay_draw(gt::ctx,draw),"unscoped replay refused","cannot call originals outside a suppressed callback");
    fi::set_draw_replay_probe(&pairProbe); gt::ctx->Draw(3,0);
    gt::check(beforeCount==1 && afterCount==1,"callback lifetime","same after callback runs after before unregisters itself; replay does not recurse");
    fi::set_draw_replay_probe(&falseProbe); gt::ctx->Draw(3,0);
    gt::check(beforeCount==2 && afterCount==1,"unprepared draw","false before never receives after");
    fi::set_draw_replay_probe(nullptr);
    ComPtr<ID3D11Device> foreignDev; ComPtr<ID3D11DeviceContext> foreignCtx;
    const D3D_FEATURE_LEVEL fl=D3D_FEATURE_LEVEL_11_0;
    D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,0,&fl,1,D3D11_SDK_VERSION,foreignDev.GetAddressOf(),nullptr,foreignCtx.GetAddressOf());
    fi::set_draw_replay_probe(&falseProbe);
    if (foreignCtx) foreignCtx->Draw(0,0);
    gt::check(foreignCtx && beforeCount==2,"foreign context bypass","shared vtable cannot call the probe for another device");
    fi::set_draw_replay_probe(nullptr);
    gt::ctx->ClearRenderTargetView(rt.Get(),clear); gt::ctx->ClearDepthStencilView(ds.Get(),D3D11_CLEAR_DEPTH|D3D11_CLEAR_STENCIL,1,9);
    np::request(masked);
    gt::ctx->Draw(3,0);
    gt::check(np::stats().tested==0,"pending boundary","request cannot start a partial window");
    fi::on_present(nullptr);
    if (guards) {
        if (masked) {
            gt::ctx->OMSetDepthStencilState(stencilActive.Get(),71); gt::ctx->Draw(3,0);
            gt::ctx->OMSetDepthStencilState(dsState.Get(),71);
            gt::ctx->OMSetRenderTargets(1,rt.GetAddressOf(),stencilRead.Get()); gt::ctx->Draw(3,0);
            gt::ctx->OMSetRenderTargets(1,rt.GetAddressOf(),ds.Get());
            gt::check(np::stats().skipped==2,"native stencil exclusion","active and read-only stencil refuse private mask replay");
        }
        gt::ctx->PSSetShader(noMetadata.Get(),nullptr,0); gt::ctx->Draw(3,0); gt::ctx->PSSetShader(ps.Get(),nullptr,0);
        ID3D11RenderTargetView* mrt[]={rt.Get(),rt2.Get()}; gt::ctx->OMSetRenderTargets(2,mrt,ds.Get()); gt::ctx->Draw(3,0);
        gt::ctx->OMSetRenderTargets(1,rt.GetAddressOf(),dsRead.Get()); gt::ctx->PSSetShaderResources(0,1,depthSrv.GetAddressOf()); gt::ctx->Draw(3,0);
        ComPtr<ID3D11ShaderResourceView> afterAlias; gt::ctx->PSGetShaderResources(0,1,afterAlias.GetAddressOf());
        gt::check(afterAlias.Get()==depthSrv.Get(),"read-only depth alias","unsupported alias stays bound; no scratch rebinding");
        ID3D11ShaderResourceView* none=nullptr; gt::ctx->PSSetShaderResources(0,1,&none); gt::ctx->OMSetRenderTargets(1,rt.GetAddressOf(),ds.Get());
        D3D11_QUERY_DESC qd{D3D11_QUERY_OCCLUSION_PREDICATE,0}; ComPtr<ID3D11Predicate> predicate;
        gt::dev->CreatePredicate(&qd,predicate.GetAddressOf()); gt::ctx->Begin(predicate.Get()); gt::ctx->Draw(3,0); gt::ctx->End(predicate.Get());
        gt::check(np::stats().skipped==unsigned(masked?6:4),"active query excluded","an engine occlusion query cannot count a scratch draw");
        gt::ctx->SetPredication(predicate.Get(),FALSE); gt::ctx->Draw(3,0); gt::ctx->SetPredication(nullptr,FALSE);
        for(int i=0;i<18;++i) gt::ctx->Draw(3,0);
        fi::on_present(nullptr);
        const auto s=np::stats();
        gt::check(s.finished && s.failed && s.skipped>=4 && s.tested<=16,"fail closed","missing metadata/MRT/alias/predication/cap: tested=%u skipped=%u",s.tested,s.skipped);
    } else {
        D3D11_QUERY_DESC qd{D3D11_QUERY_PIPELINE_STATISTICS,0}; ComPtr<ID3D11Query> query;
        gt::dev->CreateQuery(&qd,query.GetAddressOf());
        if (!masked) { fi::ScopedSuppress s; gt::ctx->Begin(query.Get()); } // fixture-owned query, not an engine query
        for (int i=0;i<12;++i) {
            // Reuse/rename the exact geometry and material objects before each
            // draw, alternating opaque/additive/alpha and all four draw slots.
            float changing[9]; memcpy(changing,vertices,sizeof(vertices)); changing[2]=changing[5]=changing[8]=.5f-.025f*i;
            const float material[4]={.025f*(i+1),.0625f,.125f,.5f};
            if (!upload(vb.Get(),changing,sizeof(changing)) || !upload(cb.Get(),material,16)) return 1;
            if (masked) gt::ctx->OMSetDepthStencilState(i<4 ? dsState.Get() : noDepthWrite.Get(),71);
            gt::ctx->OMSetBlendState(i<4 ? nullptr : i<8 ? additive.Get() : alpha.Get(),nullptr,~0u);
            switch(i%4) {
            case 0: gt::ctx->Draw(3,0); break;
            case 1: gt::ctx->DrawIndexed(3,0,0); break;
            case 2: gt::ctx->DrawInstanced(3,1,0,0); break;
            case 3: gt::ctx->DrawIndexedInstanced(3,1,0,0,0); break;
            }
        }
        if (!masked) { fi::ScopedSuppress s; gt::ctx->End(query.Get()); }
        D3D11_QUERY_DATA_PIPELINE_STATISTICS qp{};
        if (!masked) gt::check(wait_query(query.Get(),&qp,sizeof(qp)) && qp.IAVertices==72,"real native execution","12 draws plus 12 scratch replays: IA vertices=%llu (copy-only negative would be 36)",qp.IAVertices);
        ComPtr<ID3D11RenderTargetView> afterRt; ComPtr<ID3D11DepthStencilView> afterDs;
        ComPtr<ID3D11VertexShader> afterVs; ComPtr<ID3D11PixelShader> afterPs; ComPtr<ID3D11Buffer> afterCb,afterVb;
        ComPtr<ID3D11BlendState> afterBlend; FLOAT bf[4]{}; UINT mask=0,vsStride=0,vsOffset=0;
        gt::ctx->OMGetRenderTargets(1,afterRt.GetAddressOf(),afterDs.GetAddressOf());
        gt::ctx->VSGetShader(afterVs.GetAddressOf(),nullptr,nullptr); gt::ctx->PSGetShader(afterPs.GetAddressOf(),nullptr,nullptr);
        gt::ctx->PSGetConstantBuffers(0,1,afterCb.GetAddressOf()); gt::ctx->IAGetVertexBuffers(0,1,afterVb.GetAddressOf(),&vsStride,&vsOffset);
        gt::ctx->OMGetBlendState(afterBlend.GetAddressOf(),bf,&mask);
        gt::check(afterRt.Get()==rt.Get() && afterDs.Get()==ds.Get() && afterVs.Get()==vs.Get() && afterPs.Get()==ps.Get() && afterCb.Get()==cb.Get() && afterVb.Get()==vb.Get() && vsStride==12 && vsOffset==0 && afterBlend.Get()==alpha.Get() && mask==~0u,"pipeline restored","OM, shaders, material/geometry versions and blend state preserved");
        if (masked) {
            ComPtr<ID3D11InputLayout> afterIl; ComPtr<ID3D11RasterizerState> afterRs; ComPtr<ID3D11DepthStencilState> afterState;
            UINT afterRef=0,nv=1,nr=1; D3D11_VIEWPORT afterVp{}; D3D11_RECT afterRect{}; D3D11_PRIMITIVE_TOPOLOGY afterTopology{};
            gt::ctx->IAGetInputLayout(afterIl.GetAddressOf()); gt::ctx->IAGetPrimitiveTopology(&afterTopology);
            gt::ctx->RSGetState(afterRs.GetAddressOf()); gt::ctx->RSGetViewports(&nv,&afterVp); gt::ctx->RSGetScissorRects(&nr,&afterRect);
            gt::ctx->OMGetDepthStencilState(afterState.GetAddressOf(),&afterRef);
            gt::check(afterIl.Get()==il.Get() && afterRs.Get()==raster.Get() && afterState.Get()==noDepthWrite.Get() && afterRef==71 &&
                nv==1 && !memcmp(&afterVp,&vp,sizeof(vp)) && nr==1 && !memcmp(&afterRect,&scissor,sizeof(scissor)) &&
                afterTopology==D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST,"mask state restored","layout, raster, viewport/scissor, topology and depth/stencil reference survive");
        }
        fi::on_present(nullptr);
        const auto s=np::stats();
        gt::check(s.finished && !s.failed && s.tested==12 && s.changed==12 && !s.colourDiff && !s.depthDiff && !s.originalDiff,"native same-eye equality","tested=%u changed=%u colour=%llu depth/stencil=%llu original=%llu",s.tested,s.changed,s.colourDiff,s.depthDiff,s.originalDiff);
        if (masked) {
            for (unsigned i=0;i<5;++i) {
                const auto& m=s.masks[i];
                gt::check(m.tested==12 && !m.selectedDiff && !m.outsideDiff && !m.stencilDiff,
                    "masked native equality","case=%u tested=%u selected=%llu outside=%llu stencil=%llu",i,m.tested,m.selectedDiff,m.outsideDiff,m.stencilDiff);
                gt::check(m.ia==36 && m.vs==36,"real geometry under stencil","case=%u IA=%llu VS=%llu; every draw executed",i,m.ia,m.vs);
                gt::check(m.timed==12,"GPU timestamps","case=%u valid=%u of 12",i,m.timed);
            }
            if (!late) gt::check(s.masks[1].ps==0 && s.masks[0].ps>0 && s.masks[2].ps<s.masks[0].ps/10 && s.masks[3].ps<s.masks[0].ps/5,
                "pixel shading rejected early","PS full=%llu empty=%llu tiles1=%llu tiles5=%llu",s.masks[0].ps,s.masks[1].ps,s.masks[2].ps,s.masks[3].ps);
            gt::check(s.masks[2].selectedChanged>0 && s.masks[3].selectedChanged>0,
                "nonempty sparse proof","selected changed colour/depth channels=%llu / %llu",s.masks[2].selectedChanged,s.masks[3].selectedChanged);
        }
        np::request(masked); gt::ctx->Draw(3,0); fi::on_present(nullptr);
        gt::check(np::stats().tested==12,"one-shot lifecycle","late draw and request cannot extend or re-arm the stopped proof");
        // A separate process exercises refusal/cap failure without a test-only
        // reset/rearm facility in production. No game, window, capture or files.
        if (!masked) {
            for (const auto* flag : {L"--guards",L"--masked",L"--maskguards",L"--masked-large",L"--masked-late"}) {
                wchar_t exe[MAX_PATH]={}; GetModuleFileNameW(nullptr,exe,MAX_PATH);
                wchar_t command[2*MAX_PATH]; swprintf_s(command,L"\"%ls\" %ls",exe,flag);
                STARTUPINFOW si{}; si.cb=sizeof(si); si.dwFlags=STARTF_USESTDHANDLES;
                si.hStdInput=GetStdHandle(STD_INPUT_HANDLE); si.hStdOutput=GetStdHandle(STD_OUTPUT_HANDLE); si.hStdError=GetStdHandle(STD_ERROR_HANDLE);
                PROCESS_INFORMATION pi{}; DWORD code=1;
                if(CreateProcessW(nullptr,command,nullptr,nullptr,TRUE,CREATE_NO_WINDOW,nullptr,nullptr,&si,&pi)) {
                    if(WaitForSingleObject(pi.hProcess,60000)==WAIT_OBJECT_0) GetExitCodeProcess(pi.hProcess,&code);
                    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
                }
                gt::check(code==0,"isolated fixture","%ls exited %lu",flag,code);
            }
        }
    }
    printf("native proof%s%s: %d pass, %d fail\n",masked?" masked":"",guards?" guards":"",gt::g_pass,gt::g_fail);
    return gt::g_fail ? 1 : 0;
}
