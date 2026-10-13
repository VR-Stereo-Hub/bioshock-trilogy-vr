#include "game/bioshock1r/native_audit.h"
#include "core/gfx/frame_inspector.h"
#include "core/gfx/shader_bytecode.h"
#include "core/hooks/pattern_scan.h"
#include "core/util/log.h"
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include "game/bioshock1r/native_audit_com.h"
#include <algorithm>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace bvr::b1r::native_audit {
namespace {
using bvr::b1r::native_audit::ComPtr;
using bvr::frame_inspector::DrawArgs;
using bvr::frame_inspector::DrawKind;
constexpr uint32_t kMaxDraws=4096;
constexpr uint64_t kMaxConstants=64*1024*1024, kMaxReport=32*1024*1024;
struct Constant { ComPtr<ID3D11Buffer> copy; UINT bytes; uint64_t offset; };
struct Child {
    ComPtr<ID3D11DeviceChild> object; // pins identity only until the audit finishes
    std::vector<uint8_t> bytes;
    std::string filename;
};
ComPtr<ID3D11DeviceContext> g_context;
ComPtr<ID3D11Device> g_device;
std::vector<Constant> g_constants;
std::vector<Child> g_children;
std::map<ID3D11DeviceChild*,size_t> g_childIndex;
std::wstring g_directory;
FILE* g_report=nullptr;
Stats g_stats{};
bool g_auto=false, g_pending=false, g_collect=false;
bool g_frameFg=false,g_frameWorld=false;
uint64_t g_stableSince=0,g_reportBytes=0;

void record(const char* fmt,...) {
    if(!g_report || g_reportBytes>=kMaxReport) { g_stats.failed=true; return; }
    va_list args; va_start(args,fmt); const int n=vfprintf(g_report,fmt,args); va_end(args);
    if(n<0) g_stats.failed=true;
    else g_reportBytes+=static_cast<uint64_t>(n);
}
std::wstring path(const char* name) {
    std::wstring wide; while(*name) wide.push_back(static_cast<unsigned char>(*name++));
    return g_directory+L"\\"+wide;
}
bool write_file(const std::wstring& name,const void* bytes,size_t count) {
    FILE* f=nullptr;
    if(_wfopen_s(&f,name.c_str(),L"wb") || !f) return false;
    const bool ok=fwrite(bytes,1,count,f)==count;
    return fclose(f)==0 && ok;
}
template<class T> void raw_desc(const T& desc) {
    // Lossless SDK descriptor, printed in hex so all union fields remain available.
    const uint8_t* p=reinterpret_cast<const uint8_t*>(&desc);
    record(" desc=");
    for(size_t i=0;i<sizeof(T);++i) record("%02x",p[i]);
}
void resource(ID3D11Resource* r) {
    if(!r) { record(" resource=null"); return; }
    D3D11_RESOURCE_DIMENSION dim{}; r->GetType(&dim);
    record(" resource=%p dimension=%u",static_cast<void*>(r),dim);
    ComPtr<ID3D11Texture2D> t;
    if(SUCCEEDED(r->QueryInterface(IID_PPV_ARGS(t.GetAddressOf())))) {
        D3D11_TEXTURE2D_DESC d{}; t->GetDesc(&d);
        record(" texture2d=%ux%u format=%u samples=%u mips=%u array=%u usage=%u bind=%x cpu=%x misc=%x",
               d.Width,d.Height,d.Format,d.SampleDesc.Count,d.MipLevels,d.ArraySize,d.Usage,d.BindFlags,d.CPUAccessFlags,d.MiscFlags);
    }
    ComPtr<ID3D11Buffer> b;
    if(SUCCEEDED(r->QueryInterface(IID_PPV_ARGS(b.GetAddressOf())))) {
        D3D11_BUFFER_DESC d{}; b->GetDesc(&d);
        record(" bufferBytes=%u usage=%u bind=%x cpu=%x misc=%x stride=%u",
               d.ByteWidth,d.Usage,d.BindFlags,d.CPUAccessFlags,d.MiscFlags,d.StructureByteStride);
    }
}
const Child* child(ID3D11DeviceChild* object,const char* kind) {
    if(!object) return nullptr;
    const auto old=g_childIndex.find(object);
    if(old!=g_childIndex.end()) return &g_children[old->second];
    if(g_children.size()>=2048) { g_stats.failed=true; return nullptr; }
    Child c; c.object=object;
    const bool found=bvr::shader_bytecode::read(object,c.bytes);
    char name[64]; sprintf_s(name,"object_%04u.%s",static_cast<unsigned>(g_children.size()),kind);
    c.filename=found ? name : "MISSING";
    if(!found) ++g_stats.missingMetadata;
    const size_t index=g_children.size();
    g_children.push_back(std::move(c)); g_childIndex.emplace(object,index);
    return &g_children[index];
}
void shader(ID3D11DeviceChild* object,const char* stage,UINT classes) {
    record(" %s shader=%p classes=%u",stage,static_cast<void*>(object),classes);
    if(object) {
        const Child* c=child(object,"dxbc");
        record(" bytecode=%s",c ? c->filename.c_str() : "LIMIT");
    }
    record("\n");
    if(classes) ++g_stats.unsupported;
}
void constants(ID3D11DeviceContext* ctx,bool pixel) {
    ID3D11Buffer* buffers[D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT]={};
    if(pixel) ctx->PSGetConstantBuffers(0,_countof(buffers),buffers);
    else ctx->VSGetConstantBuffers(0,_countof(buffers),buffers);
    for(UINT i=0;i<_countof(buffers);++i) {
        ComPtr<ID3D11Buffer> source; source.Attach(buffers[i]);
        if(!source) continue;
        D3D11_BUFFER_DESC d{}; source->GetDesc(&d);
        record(" %s_cb slot=%u",pixel?"ps":"vs",i); resource(source.Get());
        if(!d.ByteWidth || d.ByteWidth>65536 || g_stats.constantBytes+d.ByteWidth>kMaxConstants) {
            record(" snapshot=LIMIT\n"); g_stats.failed=true; continue;
        }
        D3D11_BUFFER_DESC read=d;
        read.Usage=D3D11_USAGE_STAGING; read.BindFlags=0;
        read.CPUAccessFlags=D3D11_CPU_ACCESS_READ; read.MiscFlags=0; read.StructureByteStride=0;
        Constant c; c.bytes=d.ByteWidth; c.offset=g_stats.constantBytes;
        if(FAILED(g_device->CreateBuffer(&read,nullptr,c.copy.GetAddressOf()))) {
            record(" snapshot=CREATE_FAILED\n"); g_stats.failed=true; continue;
        }
        // Copy NOW, before the game's draw or a later WRITE_DISCARD. Map only at
        // the following Present. Never deduplicate by a reused buffer pointer.
        ctx->CopyResource(c.copy.Get(),source.Get());
        record(" snapshot=constants.bin offset=%llu bytes=%u\n",
               static_cast<unsigned long long>(c.offset),c.bytes);
        g_constants.push_back(std::move(c));
        g_stats.constantBytes+=d.ByteWidth; ++g_stats.constants;
    }
}
void bindings(ID3D11DeviceContext* ctx,bool pixel) {
    ID3D11ShaderResourceView* views[D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT]={};
    if(pixel) ctx->PSGetShaderResources(0,_countof(views),views);
    else ctx->VSGetShaderResources(0,_countof(views),views);
    for(UINT i=0;i<_countof(views);++i) {
        ComPtr<ID3D11ShaderResourceView> v; v.Attach(views[i]); if(!v) continue;
        D3D11_SHADER_RESOURCE_VIEW_DESC d{}; v->GetDesc(&d);
        ComPtr<ID3D11Resource> r; v->GetResource(r.GetAddressOf());
        record(" %s_srv slot=%u view=%p format=%u viewDimension=%u",pixel?"ps":"vs",i,static_cast<void*>(v.Get()),d.Format,d.ViewDimension);
        resource(r.Get()); raw_desc(d); record("\n");
    }
    ID3D11SamplerState* samplers[D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT]={};
    if(pixel) ctx->PSGetSamplers(0,_countof(samplers),samplers);
    else ctx->VSGetSamplers(0,_countof(samplers),samplers);
    for(UINT i=0;i<_countof(samplers);++i) {
        ComPtr<ID3D11SamplerState> s; s.Attach(samplers[i]); if(!s) continue;
        D3D11_SAMPLER_DESC d{}; s->GetDesc(&d);
        record(" %s_sampler slot=%u filter=%u address=%u,%u,%u lod=%.9g,%.9g bias=%.9g anisotropy=%u comparison=%u border=%.9g,%.9g,%.9g,%.9g\n",
               pixel?"ps":"vs",i,d.Filter,d.AddressU,d.AddressV,d.AddressW,d.MinLOD,d.MaxLOD,d.MipLODBias,d.MaxAnisotropy,d.ComparisonFunc,
               d.BorderColor[0],d.BorderColor[1],d.BorderColor[2],d.BorderColor[3]);
    }
}
void capture(ID3D11DeviceContext* ctx,const DrawArgs& a,const D3D11_VIEWPORT* vp,UINT nv,bool foreground) {
    if(g_stats.draws>=kMaxDraws) { g_stats.failed=true; return; }
    const unsigned draw=g_stats.draws++;
    if(foreground) ++g_stats.foreground;
    record("draw=%u kind=%u count=%u start=%u baseVertex=%d instances=%u startInstance=%u foreground=%u viewports=%u\n",
           draw,static_cast<unsigned>(a.kind),a.count,a.start,a.baseVertex,a.instances,a.startInstance,foreground?1:0,nv);
    for(UINT i=0;i<nv;++i) record(" viewport=%u x=%.9g y=%.9g width=%.9g height=%.9g depth=%.9g,%.9g\n",i,vp[i].TopLeftX,vp[i].TopLeftY,vp[i].Width,vp[i].Height,vp[i].MinDepth,vp[i].MaxDepth);
    ID3D11RenderTargetView* targets[8]={};
    ComPtr<ID3D11DepthStencilView> depth;
    ctx->OMGetRenderTargets(8,targets,depth.GetAddressOf());
    for(UINT i=0;i<8;++i) {
        ComPtr<ID3D11RenderTargetView> v; v.Attach(targets[i]); if(!v) continue;
        ComPtr<ID3D11Resource> r; v->GetResource(r.GetAddressOf());
        D3D11_RENDER_TARGET_VIEW_DESC d{}; v->GetDesc(&d);
        record(" rt slot=%u view=%p",i,static_cast<void*>(v.Get())); resource(r.Get()); raw_desc(d); record("\n");
    }
    if(depth) {
        ComPtr<ID3D11Resource> r; depth->GetResource(r.GetAddressOf());
        D3D11_DEPTH_STENCIL_VIEW_DESC d{}; depth->GetDesc(&d);
        record(" dsv view=%p",static_cast<void*>(depth.Get())); resource(r.Get()); raw_desc(d); record("\n");
    }
    UINT stencil=0; ComPtr<ID3D11DepthStencilState> ds; ctx->OMGetDepthStencilState(ds.GetAddressOf(),&stencil);
    if(ds) {
        D3D11_DEPTH_STENCIL_DESC d{}; ds->GetDesc(&d);
        record(" depthState enable=%u write=%u func=%u stencil=%u ref=%u readMask=%x writeMask=%x front=%u,%u,%u,%u back=%u,%u,%u,%u\n",
               d.DepthEnable,d.DepthWriteMask,d.DepthFunc,d.StencilEnable,stencil,d.StencilReadMask,d.StencilWriteMask,
               d.FrontFace.StencilFailOp,d.FrontFace.StencilDepthFailOp,d.FrontFace.StencilPassOp,d.FrontFace.StencilFunc,
               d.BackFace.StencilFailOp,d.BackFace.StencilDepthFailOp,d.BackFace.StencilPassOp,d.BackFace.StencilFunc);
    } else record(" depthState=DEFAULT ref=%u\n",stencil);
    FLOAT factor[4]={}; UINT mask=0; ComPtr<ID3D11BlendState> blend; ctx->OMGetBlendState(blend.GetAddressOf(),factor,&mask);
    record(" blend factor=%.9g,%.9g,%.9g,%.9g sampleMask=%x",factor[0],factor[1],factor[2],factor[3],mask);
    if(blend) { D3D11_BLEND_DESC d{}; blend->GetDesc(&d); raw_desc(d); }
    else record(" state=DEFAULT"); record("\n");
    ComPtr<ID3D11RasterizerState> rs; ctx->RSGetState(rs.GetAddressOf());
    record(" raster");
    if(rs) { D3D11_RASTERIZER_DESC d{}; rs->GetDesc(&d); raw_desc(d); }
    else record(" state=DEFAULT"); record("\n");
    D3D11_RECT rects[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]={}; UINT nr=_countof(rects);
    ctx->RSGetScissorRects(&nr,rects);
    for(UINT i=0;i<nr;++i) record(" scissor=%u rect=%ld,%ld,%ld,%ld\n",i,rects[i].left,rects[i].top,rects[i].right,rects[i].bottom);
    D3D11_PRIMITIVE_TOPOLOGY topology{}; ctx->IAGetPrimitiveTopology(&topology); record(" topology=%u\n",topology);
    ComPtr<ID3D11InputLayout> il; ctx->IAGetInputLayout(il.GetAddressOf());
    if(il) {
        const Child* c=child(il.Get(),"layout"); record(" layout=%s\n",c?c->filename.c_str():"LIMIT");
        if(c && !c->bytes.empty() && c->bytes.size()%sizeof(shader_bytecode::InputElement)==0) {
            for(size_t i=0;i<c->bytes.size()/sizeof(shader_bytecode::InputElement);++i) {
                shader_bytecode::InputElement e; memcpy(&e,c->bytes.data()+i*sizeof(e),sizeof(e));
                record(" element=%u semantic=%.64s index=%u format=%u slot=%u offset=%u classification=%u step=%u\n",
                       static_cast<unsigned>(i),e.semantic,e.index,e.format,e.slot,e.offset,e.classification,e.step);
            }
        }
    } else record(" layout=null\n");
    ID3D11Buffer* vb[D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT]={}; UINT strides[_countof(vb)]={},offsets[_countof(vb)]={};
    ctx->IAGetVertexBuffers(0,_countof(vb),vb,strides,offsets);
    for(UINT i=0;i<_countof(vb);++i) {
        ComPtr<ID3D11Buffer> b; b.Attach(vb[i]); if(!b) continue;
        record(" vb slot=%u stride=%u offset=%u",i,strides[i],offsets[i]); resource(b.Get()); record(" bytes=NOT_SNAPSHOTTED\n");
    }
    ComPtr<ID3D11Buffer> ib; DXGI_FORMAT format{}; UINT offset=0; ctx->IAGetIndexBuffer(ib.GetAddressOf(),&format,&offset);
    if(ib) { record(" ib format=%u offset=%u",format,offset); resource(ib.Get()); record(" bytes=NOT_SNAPSHOTTED\n"); }
    ComPtr<ID3D11VertexShader> vs; ComPtr<ID3D11PixelShader> ps; UINT vc=0,pc=0;
    ctx->VSGetShader(vs.GetAddressOf(),nullptr,&vc); ctx->PSGetShader(ps.GetAddressOf(),nullptr,&pc);
    shader(vs.Get(),"vs",vc); shader(ps.Get(),"ps",pc);
    ComPtr<ID3D11GeometryShader> gs; ComPtr<ID3D11HullShader> hs; ComPtr<ID3D11DomainShader> dsShader;
    ctx->GSGetShader(gs.GetAddressOf(),nullptr,nullptr); ctx->HSGetShader(hs.GetAddressOf(),nullptr,nullptr); ctx->DSGetShader(dsShader.GetAddressOf(),nullptr,nullptr);
    if(gs || hs || dsShader) { ++g_stats.unsupported; record(" unsupportedStages gs=%p hs=%p ds=%p\n",static_cast<void*>(gs.Get()),static_cast<void*>(hs.Get()),static_cast<void*>(dsShader.Get())); }
    constants(ctx,false); constants(ctx,true); bindings(ctx,false); bindings(ctx,true);
}
void observe(ID3D11DeviceContext* ctx,const DrawArgs& a) noexcept {
    if(!g_context || ctx!=g_context.Get() || g_stats.finished) return;
    try {
        if(!g_collect && (!g_auto || a.kind!=DrawKind::Indexed || a.count<100)) return;
        D3D11_VIEWPORT vp[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]={}; UINT nv=_countof(vp);
        ctx->RSGetViewports(&nv,vp);
        ComPtr<ID3D11RenderTargetView> rt; ComPtr<ID3D11DepthStencilView> depth;
        ctx->OMGetRenderTargets(1,rt.GetAddressOf(),depth.GetAddressOf());
        D3D11_RENDER_TARGET_VIEW_DESC rd{}; if(rt) rt->GetDesc(&rd);
        const bool scene=nv==1 && vp[0].Width>=64 && vp[0].Height>=64 && depth && rd.Format==DXGI_FORMAT_R11G11B10_FLOAT;
        const bool fg=scene && vp[0].MinDepth==0 && vp[0].MaxDepth>0 && vp[0].MaxDepth<=0.011f;
        g_frameFg |= fg; g_frameWorld |= scene && vp[0].MaxDepth>0.9f;
        if(g_collect) capture(ctx,a,vp,nv,fg);
    } catch(...) { g_stats.failed=true; g_collect=false; BVR_LOG("[native-audit] allocation failed; audit stopped, original draw retained"); }
}
void other(ID3D11DeviceContext* ctx,const char* kind) noexcept {
    if(ctx==g_context.Get() && g_collect) { ++g_stats.unsupported; record("unsupportedCommand=%s\n",kind); }
}
void finish(ID3D11DeviceContext* ctx) {
    FILE* constantsFile=nullptr;
    if(_wfopen_s(&constantsFile,path("constants.bin").c_str(),L"wb") || !constantsFile) g_stats.failed=true;
    for(auto& c:g_constants) {
        D3D11_MAPPED_SUBRESOURCE map{};
        if(SUCCEEDED(ctx->Map(c.copy.Get(),0,D3D11_MAP_READ,0,&map))) {
            if(constantsFile && fwrite(map.pData,1,c.bytes,constantsFile)!=c.bytes) g_stats.failed=true;
            ctx->Unmap(c.copy.Get(),0);
        } else {
            g_stats.failed=true; record("constantReadbackFailed offset=%llu bytes=%u\n",static_cast<unsigned long long>(c.offset),c.bytes);
            // Keep later offsets truthful even if this readback fails.
            const uint8_t zeros[65536]={}; if(constantsFile) fwrite(zeros,1,c.bytes,constantsFile);
        }
        c.copy.Reset();
    }
    if(constantsFile && fclose(constantsFile)!=0) g_stats.failed=true;
    for(const auto& c:g_children) {
        if(c.bytes.empty()) continue;
        if(!write_file(path(c.filename.c_str()),c.bytes.data(),c.bytes.size())) g_stats.failed=true;
        if(c.filename.find(".dxbc")!=std::string::npos) {
            ComPtr<ID3DBlob> text;
            if(SUCCEEDED(D3DDisassemble(c.bytes.data(),c.bytes.size(),0,nullptr,text.GetAddressOf()))) {
                if(!write_file(path((c.filename+".asm").c_str()),text->GetBufferPointer(),text->GetBufferSize())) g_stats.failed=true;
            } else { g_stats.failed=true; record("disassembleFailed=%s\n",c.filename.c_str()); }
        }
    }
    const auto s=shader_bytecode::stats();
    if(g_stats.missingMetadata || !s.hooks) g_stats.failed=true;
    record("summary draws=%u foreground=%u constants=%u constantBytes=%llu missingMetadata=%u unsupported=%u failed=%u\n",
           g_stats.draws,g_stats.foreground,g_stats.constants,static_cast<unsigned long long>(g_stats.constantBytes),g_stats.missingMetadata,g_stats.unsupported,g_stats.failed?1:0);
    record("creationHooks=%u stored=%u refused=%u failed=%u retainedBytes=%llu\n",s.hooks?1:0,s.stored,s.refused,s.failed,static_cast<unsigned long long>(s.bytes));
    record("nativeReplayReady=0 geometryAndTextureContents=NOT_CAPTURED indirectDraws=NOT_OBSERVED\n");
    if(g_report && fclose(g_report)!=0) g_stats.failed=true; g_report=nullptr;
    g_stats.finished=true; g_collect=false; g_auto=false; g_pending=false;
    BVR_LOG("[native-audit] %s: draws=%u foreground=%u constants=%u missingMetadata=%u unsupported=%u folder=%ls (state audit only; no native repair enabled)",
            g_stats.failed?"INCOMPLETE":"saved",g_stats.draws,g_stats.foreground,g_stats.constants,g_stats.missingMetadata,g_stats.unsupported,g_directory.c_str());
    g_constants.clear(); g_children.clear(); g_childIndex.clear();
    shader_bytecode::stop();
    frame_inspector::set_draw_observer(nullptr);
    g_context.Reset(); g_device.Reset();
}
bool begin() {
    if(g_directory.empty()) {
        const std::wstring parent=std::wstring(log::data_dir())+L"\\dumps";
        CreateDirectoryW(parent.c_str(),nullptr);
        SYSTEMTIME t{}; GetLocalTime(&t); wchar_t name[100];
        swprintf_s(name,L"\\native-audit-%04u%02u%02u-%02u%02u%02u-%u",t.wYear,t.wMonth,t.wDay,t.wHour,t.wMinute,t.wSecond,GetCurrentProcessId());
        g_directory=parent+name;
    }
    if(!CreateDirectoryW(g_directory.c_str(),nullptr) || _wfopen_s(&g_report,path("audit.txt").c_str(),L"wb") || !g_report) {
        g_stats.failed=true; g_stats.finished=true; g_pending=false; g_auto=false;
        BVR_LOG("[native-audit] output directory refused: %ls",g_directory.c_str());
        shader_bytecode::stop(); frame_inspector::set_draw_observer(nullptr); return false;
    }
    record("nativeAuditVersion=1\nlimits draws=4096 constants=64MiB report=32MiB\n");
    record("scope=VS/PS bytecode, input layouts, per-draw VS/PS constant bytes, bindings and raster/OM state\n");
    record("texturesAndGeometry=descriptorsOnly noEngineWrites=1 noPipelineChanges=1 nativeReplayReady=0\n");
    g_collect=true; g_pending=false;
    BVR_LOG("[native-audit] recording one Present window; a one-time capture hitch is expected");
    return true;
}
void present(ID3D11DeviceContext* ctx) noexcept {
    try {
        if(g_stats.finished) return;
        if(!g_context) { g_context=ctx; ctx->GetDevice(g_device.GetAddressOf()); }
        if(ctx!=g_context.Get()) return;
        if(g_report) { finish(ctx); return; }
        const uint64_t now=GetTickCount64();
        if(g_auto) {
            if(!g_frameFg || !g_frameWorld) g_stableSince=0;
            else if(!g_stableSince) g_stableSince=now;
            if(g_stableSince && now-g_stableSince>=5000) g_pending=true;
        }
        g_frameFg=false; g_frameWorld=false;
        if(g_pending) begin();
    } catch(...) {
        g_stats.failed=true; g_stats.finished=true; g_collect=false;
        if(g_report) { fclose(g_report); g_report=nullptr; }
        shader_bytecode::stop(); frame_inspector::set_draw_observer(nullptr);
        BVR_LOG("[native-audit] diagnostic failed; original rendering continues");
    }
}
const frame_inspector::DrawObserver kObserver={observe,present,other};
} // namespace
void init(const bvr::pattern_scan::ProcessImage& image) {
    HMODULE self=nullptr; wchar_t ini[MAX_PATH]={};
    if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          reinterpret_cast<LPCWSTR>(&init),&self) || !GetModuleFileNameW(self,ini,MAX_PATH)) return;
    wchar_t* slash=wcsrchr(ini,L'\\'); if(!slash) return;
    const size_t left=_countof(ini)-static_cast<size_t>(slash+1-ini);
    if(wcscpy_s(slash+1,left,L"BioshockVR.ini")) return;
    if(GetPrivateProfileIntW(L"Bioshock1R",L"NativeRepairAudit",0,ini)!=1) return;
    shader_bytecode::configure(image.base,image.size);
    g_auto=true; frame_inspector::set_draw_observer(&kObserver);
    BVR_LOG("[native-audit] enabled from %ls; wait for 5 s of world + foreground, then one read-only frame",ini);
}
void request(const wchar_t* directory) {
    if(!directory || !directory[0] || g_collect || g_stats.finished) return;
    g_directory=directory; g_pending=true; frame_inspector::set_draw_observer(&kObserver);
}
Stats stats() { return g_stats; }
} // namespace bvr::b1r::native_audit
