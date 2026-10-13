// gpu-test-sources: src/core/gfx/shader_bytecode.cpp src/game/bioshock1r/native_audit.cpp
// gpu-test-libs: build/third_party/minhook/RelWithDebInfo/minhook.x32.lib
// Real D3D11/MinHook metadata hooks plus the production read-only observer.
// No game: synthetic shader bytes and reused buffers make freshness measurable.
#include "gpu_test_util.h"
#include "core/gfx/frame_inspector.h"
#include "core/gfx/shader_bytecode.h"
#include "game/bioshock1r/native_audit.h"
#include <MinHook.h>
#include "game/bioshock1r/native_audit_com.h"
#include <fstream>
#include <iterator>
#include <string>
using bvr::b1r::native_audit::ComPtr;
namespace fi=bvr::frame_inspector;
namespace sb=bvr::shader_bytecode;
namespace na=bvr::b1r::native_audit;
const fi::DrawObserver* observer=nullptr;
std::wstring dataDir;
namespace bvr::frame_inspector { void set_draw_observer(const DrawObserver* v) { observer=v; } }
namespace bvr::log {
const wchar_t* data_dir() { return dataDir.c_str(); }
void write(const char* fmt,...) { va_list a; va_start(a,fmt); vprintf(fmt,a); printf("\n"); va_end(a); }
}
static ComPtr<ID3DBlob> compile(const char* code,const char* target) {
    ComPtr<ID3DBlob> blob,error;
    const HRESULT hr=D3DCompile(code,strlen(code),nullptr,nullptr,nullptr,"main",target,0,0,blob.GetAddressOf(),error.GetAddressOf());
    if(FAILED(hr)) printf("compile: %s\n",error ? static_cast<const char*>(error->GetBufferPointer()) : "failed");
    return blob;
}
static std::string read_text(const std::wstring& p) {
    std::ifstream f(p,std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f),std::istreambuf_iterator<char>());
}
int main() {
    if(!gt::make_device()) return 1;
    const auto vsCode=compile("float4 main(uint i:SV_VertexID):SV_Position { float2 p=float2((i<<1)&2,i&2); return float4(p*float2(2,-2)+float2(-1,1),0.5,1); }","vs_5_0");
    const auto psCode=compile("cbuffer C:register(b0) { float4 colour; }; float4 main():SV_Target { return colour; }","ps_5_0");
    const auto ilCode=compile("float4 main(float3 p:POSITION):SV_Position { return float4(p,1); }","vs_5_0");
    if(!vsCode || !psCode || !ilCode) return 1;
    ComPtr<ID3D11PixelShader> before;
    gt::dev->CreatePixelShader(psCode->GetBufferPointer(),psCode->GetBufferSize(),nullptr,before.GetAddressOf());
    std::vector<uint8_t> bytes;
    gt::check(!sb::read(before.Get(),bytes),"default off","unrequested objects carry no metadata");
    const auto* base=reinterpret_cast<const uint8_t*>(GetModuleHandleW(nullptr));
    const auto* dos=reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt=reinterpret_cast<const IMAGE_NT_HEADERS*>(base+dos->e_lfanew);
    // Match startup: discover creation entries on a disposable FL11_0 device,
    // destroy it, then create the real device. A per-device thunk would fail here.
    before.Reset(); gt::rel(gt::ctx); gt::rel(gt::dev);
    ComPtr<ID3D11Device> bootstrap; ComPtr<ID3D11DeviceContext> bootstrapCtx;
    const D3D_FEATURE_LEVEL bootstrapLevel=D3D_FEATURE_LEVEL_11_0;
    if(FAILED(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,0,&bootstrapLevel,1,
                              D3D11_SDK_VERSION,bootstrap.GetAddressOf(),nullptr,bootstrapCtx.GetAddressOf()))) return 1;
    sb::configure(base,nt->OptionalHeader.SizeOfImage);
    if(MH_Initialize()!=MH_OK || !sb::install(bootstrap.Get())) return 1;
    bootstrapCtx.Reset(); bootstrap.Reset();
    if(!gt::make_device()) return 1;
    sb::bind_device(gt::dev);
    ComPtr<ID3D11VertexShader> vs; ComPtr<ID3D11PixelShader> ps; ComPtr<ID3D11InputLayout> il;
    gt::dev->CreateVertexShader(vsCode->GetBufferPointer(),vsCode->GetBufferSize(),nullptr,vs.GetAddressOf());
    gt::dev->CreatePixelShader(psCode->GetBufferPointer(),psCode->GetBufferSize(),nullptr,ps.GetAddressOf());
    const D3D11_INPUT_ELEMENT_DESC element={"POSITION",0,DXGI_FORMAT_R32G32B32_FLOAT,0,0,D3D11_INPUT_PER_VERTEX_DATA,0};
    gt::dev->CreateInputLayout(&element,1,ilCode->GetBufferPointer(),ilCode->GetBufferSize(),il.GetAddressOf());
    gt::check(sb::read(vs.Get(),bytes) && bytes.size()==vsCode->GetBufferSize() && memcmp(bytes.data(),vsCode->GetBufferPointer(),bytes.size())==0,"vertex bytecode","creation-time bytes exact, after bootstrap device destruction");
    gt::check(sb::read(ps.Get(),bytes) && bytes.size()==psCode->GetBufferSize() && memcmp(bytes.data(),psCode->GetBufferPointer(),bytes.size())==0,"pixel bytecode","creation-time bytes exact, after bootstrap device destruction");
    const bool layoutOk=sb::read(il.Get(),bytes) && bytes.size()==sizeof(sb::InputElement);
    sb::InputElement got{}; if(layoutOk) memcpy(&got,bytes.data(),sizeof(got));
    gt::check(layoutOk && strcmp(got.semantic,"POSITION")==0 && got.format==DXGI_FORMAT_R32G32B32_FLOAT,"input layout","semantic and typed descriptor preserved");
    ComPtr<ID3D11Device> foreign; ComPtr<ID3D11DeviceContext> foreignCtx;
    D3D_FEATURE_LEVEL fl=D3D_FEATURE_LEVEL_11_0;
    D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,0,&fl,1,D3D11_SDK_VERSION,foreign.GetAddressOf(),nullptr,foreignCtx.GetAddressOf());
    ComPtr<ID3D11PixelShader> foreignPs;
    if(foreign) foreign->CreatePixelShader(psCode->GetBufferPointer(),psCode->GetBufferSize(),nullptr,foreignPs.GetAddressOf());
    gt::check(foreignPs && !sb::read(foreignPs.Get(),bytes),"foreign device","no metadata on the other device, even from the host caller");
    ComPtr<ID3D11Texture2D> target; target.Attach(gt::make_tex(64,64,DXGI_FORMAT_R8G8B8A8_UNORM,D3D11_BIND_RENDER_TARGET));
    ComPtr<ID3D11RenderTargetView> rt; gt::dev->CreateRenderTargetView(target.Get(),nullptr,rt.GetAddressOf());
    const float red[4]={1,0,0,1},blue[4]={0,0,1,1};
    D3D11_BUFFER_DESC bd{}; bd.ByteWidth=16; bd.Usage=D3D11_USAGE_DYNAMIC; bd.BindFlags=D3D11_BIND_CONSTANT_BUFFER; bd.CPUAccessFlags=D3D11_CPU_ACCESS_WRITE;
    ComPtr<ID3D11Buffer> cb; gt::dev->CreateBuffer(&bd,nullptr,cb.GetAddressOf());
    auto upload=[&](const float* data) { D3D11_MAPPED_SUBRESOURCE m{}; if(FAILED(gt::ctx->Map(cb.Get(),0,D3D11_MAP_WRITE_DISCARD,0,&m))) return false; memcpy(m.pData,data,16); gt::ctx->Unmap(cb.Get(),0); return true; };
    D3D11_DEPTH_STENCIL_DESC dd{}; dd.DepthEnable=FALSE; dd.StencilEnable=FALSE;
    ComPtr<ID3D11DepthStencilState> ds; gt::dev->CreateDepthStencilState(&dd,ds.GetAddressOf());
    D3D11_RASTERIZER_DESC rd{}; rd.FillMode=D3D11_FILL_SOLID; rd.CullMode=D3D11_CULL_NONE; rd.DepthClipEnable=TRUE;
    ComPtr<ID3D11RasterizerState> rs; gt::dev->CreateRasterizerState(&rd,rs.GetAddressOf());
    const D3D11_VIEWPORT viewport={0,0,64,64,0,1};
    gt::ctx->RSSetViewports(1,&viewport); gt::ctx->RSSetState(rs.Get());
    gt::ctx->OMSetRenderTargets(1,rt.GetAddressOf(),nullptr); gt::ctx->OMSetDepthStencilState(ds.Get(),23);
    gt::ctx->VSSetShader(vs.Get(),nullptr,0); gt::ctx->PSSetShader(ps.Get(),nullptr,0);
    gt::ctx->PSSetConstantBuffers(0,1,cb.GetAddressOf()); gt::ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    gt::ctx->IASetInputLayout(nullptr);
    wchar_t cwd[MAX_PATH]; GetCurrentDirectoryW(MAX_PATH,cwd); dataDir=cwd;
    wchar_t suffix[80]; swprintf_s(suffix,L"\\native-audit-fixture-%u-%llu",GetCurrentProcessId(),GetTickCount64());
    const std::wstring folder=dataDir+suffix;
    na::request(folder.c_str());
    const auto* callbacks=observer;
    gt::check(callbacks && !na::stats().draws,"pending until boundary","no partial frame starts at request");
    callbacks->on_present(gt::ctx);
    const fi::DrawArgs draw{fi::DrawKind::Direct,3,0,0};
    if(foreignCtx) callbacks->on_draw(foreignCtx.Get(),draw);
    gt::check(na::stats().draws==0,"foreign context","observer refuses another context before any Get or Copy");
    if(!upload(red)) return 1;
    callbacks->on_draw(gt::ctx,draw); gt::ctx->Draw(3,0);
    if(!upload(blue)) return 1;
    callbacks->on_draw(gt::ctx,draw); gt::ctx->Draw(3,0);
    // The audit has read state and issued only copies into its own buffers.
    ComPtr<ID3D11VertexShader> afterVs; ComPtr<ID3D11PixelShader> afterPs; ComPtr<ID3D11Buffer> afterCb;
    ComPtr<ID3D11RenderTargetView> afterRt; ComPtr<ID3D11RasterizerState> afterRs; ComPtr<ID3D11DepthStencilState> afterDs; UINT ref=0;
    gt::ctx->VSGetShader(afterVs.GetAddressOf(),nullptr,nullptr); gt::ctx->PSGetShader(afterPs.GetAddressOf(),nullptr,nullptr);
    gt::ctx->PSGetConstantBuffers(0,1,afterCb.GetAddressOf()); gt::ctx->OMGetRenderTargets(1,afterRt.GetAddressOf(),nullptr);
    gt::ctx->RSGetState(afterRs.GetAddressOf()); gt::ctx->OMGetDepthStencilState(afterDs.GetAddressOf(),&ref);
    gt::check(afterVs.Get()==vs.Get() && afterPs.Get()==ps.Get() && afterCb.Get()==cb.Get() && afterRt.Get()==rt.Get() && afterRs.Get()==rs.Get() && afterDs.Get()==ds.Get() && ref==23,"pipeline preserved","VS/PS, buffer, target, raster and depth/stencil ref unchanged");
    const auto withAudit=gt::readback(target.Get());
    callbacks->on_present(gt::ctx);
    gt::ctx->Draw(3,0);
    const auto withoutAudit=gt::readback(target.Get());
    gt::check(!withAudit.empty() && withAudit==withoutAudit,"render equality","audited synthetic draw is byte-equal to an unobserved draw");
    const auto result=na::stats();
    const std::string cbBytes=read_text(folder+L"\\constants.bin");
    gt::check(cbBytes.size()==32 && memcmp(cbBytes.data(),red,16)==0 && memcmp(cbBytes.data()+16,blue,16)==0,"reused buffer versions","the same WRITE_DISCARD buffer retains red then blue at its two draws");
    const std::string report=read_text(folder+L"\\audit.txt");
    gt::check(result.finished && !result.failed && result.draws==2 && result.constants==2 && result.missingMetadata==0 && observer==nullptr,"one bounded window","draws=%u constants=%u missing=%u failed=%u",result.draws,result.constants,result.missingMetadata,result.failed);
    gt::check(report.find("nativeReplayReady=0")!=std::string::npos && report.find("indirectDraws=NOT_OBSERVED")!=std::string::npos,"honest scope","descriptor-only resources and unobserved indirect draws explicitly marked");
    gt::check(!read_text(folder+L"\\object_0000.dxbc.asm").empty(),"shader disassembly","real captured DXBC can be disassembled offline");
    callbacks->on_draw(gt::ctx,draw); callbacks->on_present(gt::ctx);
    gt::check(na::stats().draws==2,"automatic stop","late callbacks cannot extend the completed audit");
    ComPtr<ID3D11PixelShader> afterStop;
    gt::dev->CreatePixelShader(psCode->GetBufferPointer(),psCode->GetBufferSize(),nullptr,afterStop.GetAddressOf());
    gt::check(!sb::read(afterStop.Get(),bytes) && sb::read(ps.Get(),bytes),"metadata lifecycle","collection stops; original live object still owns its exact bytes");
    printf("fixture: %ls\nnative audit: %d pass, %d fail\n",folder.c_str(),gt::g_pass,gt::g_fail);
    return gt::g_fail ? 1 : 0;
}
