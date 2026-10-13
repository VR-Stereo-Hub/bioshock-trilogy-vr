#include "game/bioshock1r/native_repair.h"
#include "game/bioshock1r/native_proof_eye.h"
#include "game/bioshock1r/native_audit_com.h"
#include "core/gfx/frame_inspector.h"
#include "core/gfx/shader_bytecode.h"
#include "core/hooks/pattern_scan.h"
#include "core/util/log.h"
#include <windows.h>
#include <d3d11.h>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <memory>
#include <vector>

namespace bvr::b1r::native_repair {
namespace {
using native_audit::ComPtr;
using frame_inspector::DrawArgs;
namespace eye=native_proof::detail;
constexpr uint64_t kMaxPixels=8ull*1024*1024;
constexpr uint64_t kGraphBytes=256ull*1024*1024;
constexpr unsigned kMaxNodes=32,kMaxForeground=64,kMaxPost=256;
const GUID kConstantSnapshot={0x9d7c49c1,0xf0ac,0x44d6,{0xa9,0x21,0x34,0x59,0x57,0x69,0x23,0xb0}};
std::atomic<bool> g_enabled{false},g_active{false};
Stats g_stats;
ComPtr<ID3D11Device> g_dev;
ComPtr<ID3D11DeviceContext> g_ctx;
struct View {
    uint64_t serial=0,build=0,stamp=0;
    int sign=0;
    float separation=0;
    tgpu::Camera camera;
    tgpu::Params depth;
    afw::Params warp;
};
thread_local View t_view;
thread_local uint64_t t_boundary=0;
View g_view;
bool g_open=false,g_failed=false,g_assembled=false,g_complete=false,g_selected=false;
uint64_t g_generation=0,g_graphBytes=0,g_lastLog=0;
std::atomic<uint64_t> g_snapshotEpoch{1};
std::vector<uint8_t> g_uploadBytes;
unsigned g_fgDraws=0,g_worldDraws=0,g_postDraws=0;
float g_tanH=0,g_tanV=0;
const char* g_reason="no foreground frame";
ComPtr<ID3D11Texture2D> g_scene,g_sceneDepth,g_output;
bool fail(const char* reason) {
    if(!g_failed) {
        ++g_stats.refused;
        g_reason=reason;
        if(g_stats.refused<=8 || GetTickCount64()-g_lastLog>5000)
            BVR_LOG("[native-repair] frame refused: %s (fg=%u world=%u post=%u)",reason,g_fgDraws,g_worldDraws,g_postDraws);
    }
    g_failed=true;g_complete=g_selected=false;return false;
}
bool live_context(ID3D11DeviceContext* c) {
    return g_enabled.load(std::memory_order_relaxed) && g_active.load(std::memory_order_relaxed) && c==g_ctx.Get();
}
struct Surface {
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11RenderTargetView> rtv;
    ComPtr<ID3D11ShaderResourceView> srv;
    ComPtr<ID3D11DepthStencilView> dsv;
    UINT w=0,h=0;DXGI_FORMAT format=DXGI_FORMAT_UNKNOWN;
    bool make(UINT width,UINT height,DXGI_FORMAT fmt,UINT bind) {
        if(texture && w==width && h==height && format==fmt) return true;
        *this={};D3D11_TEXTURE2D_DESC d{};
        d.Width=width;d.Height=height;d.MipLevels=d.ArraySize=d.SampleDesc.Count=1;
        d.Format=fmt;d.Usage=D3D11_USAGE_DEFAULT;d.BindFlags=bind;
        if(FAILED(g_dev->CreateTexture2D(&d,nullptr,texture.GetAddressOf()))) return false;
        if(bind&D3D11_BIND_RENDER_TARGET)
            if(FAILED(g_dev->CreateRenderTargetView(texture.Get(),nullptr,rtv.GetAddressOf()))) return false;
        if(bind&D3D11_BIND_SHADER_RESOURCE) {
            D3D11_SHADER_RESOURCE_VIEW_DESC s{};s.Format=fmt==DXGI_FORMAT_R24G8_TYPELESS?DXGI_FORMAT_R24_UNORM_X8_TYPELESS:fmt;
            s.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE2D;s.Texture2D.MipLevels=1;
            if(FAILED(g_dev->CreateShaderResourceView(texture.Get(),&s,srv.GetAddressOf()))) return false;
        }
        if(bind&D3D11_BIND_DEPTH_STENCIL) {
            D3D11_DEPTH_STENCIL_VIEW_DESC v{};v.Format=DXGI_FORMAT_D24_UNORM_S8_UINT;v.ViewDimension=D3D11_DSV_DIMENSION_TEXTURE2D;
            if(FAILED(g_dev->CreateDepthStencilView(texture.Get(),&v,dsv.GetAddressOf()))) return false;
        }
        w=width;h=height;format=fmt;return true;
    }
};
Surface g_fg,g_trans,g_fgDepth,g_rawDepth,g_sourceCoverage;
ComPtr<ID3D11DepthStencilView> g_sourceReadOnlyDepth;
struct History {Surface hdr,z,mask;tgpu::Camera camera;uint64_t build=0,stamp=0;bool valid=false;};
History g_history[2];
tgpu::TemporalGpu g_guides;
afw::WarpGpu g_worldWarp;
struct ViewMap {
    ComPtr<ID3D11View> original;
    ComPtr<ID3D11ShaderResourceView> srv;
    ComPtr<ID3D11RenderTargetView> rtv;
};
struct Node {
    ComPtr<ID3D11Resource> original;
    ComPtr<ID3D11Texture2D> copy;
    std::vector<ViewMap> views;
    uint64_t generation=0,bytes=0;
};
std::vector<std::unique_ptr<Node>> g_nodes;
Node* find_node(ID3D11Resource* resource,bool current=true) {
    for(auto& n:g_nodes) if(n->original.Get()==resource && (!current || n->generation==g_generation)) return n.get();
    return nullptr;
}
bool texture_desc(ID3D11Resource* res,ComPtr<ID3D11Texture2D>& texture,D3D11_TEXTURE2D_DESC& desc) {
    if(!res || FAILED(res->QueryInterface(__uuidof(ID3D11Texture2D),reinterpret_cast<void**>(texture.GetAddressOf())))) return false;
    texture->GetDesc(&desc);
    return desc.Width && desc.Height && uint64_t(desc.Width)*desc.Height<=kMaxPixels &&
           desc.MipLevels==1 && desc.ArraySize==1 && desc.SampleDesc.Count==1;
}
unsigned format_bytes(DXGI_FORMAT format) {
    switch(format) {
    case DXGI_FORMAT_R11G11B10_FLOAT:case DXGI_FORMAT_R8G8B8A8_UNORM:case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:case DXGI_FORMAT_B8G8R8A8_UNORM:case DXGI_FORMAT_R32_FLOAT:
    case DXGI_FORMAT_R32_TYPELESS:case DXGI_FORMAT_R24G8_TYPELESS:return 4;
    case DXGI_FORMAT_R16G16B16A16_FLOAT:return 8;
    case DXGI_FORMAT_R16_FLOAT:return 2;
    case DXGI_FORMAT_R8_UNORM:return 1;
    default:return 0;
    }
}
Node* ensure_node(ID3D11DeviceContext* ctx,ID3D11Resource* original) {
    Node* node=find_node(original,false);
    if(!node) {
        if(g_nodes.size()>=kMaxNodes) {fail("post resource count cap");return nullptr;}
        ComPtr<ID3D11Texture2D> t;D3D11_TEXTURE2D_DESC d{};
        if(!texture_desc(original,t,d)) {fail("post resource shape");return nullptr;}
        const unsigned bpp=format_bytes(d.Format);const uint64_t bytes=uint64_t(d.Width)*d.Height*bpp;
        if(!bpp || g_graphBytes+bytes>kGraphBytes || !(d.BindFlags&D3D11_BIND_RENDER_TARGET)) {
            fail("post resource format/budget");return nullptr;
        }
        auto n=std::make_unique<Node>();n->original=original;n->bytes=bytes;
        d.Usage=D3D11_USAGE_DEFAULT;d.CPUAccessFlags=d.MiscFlags=0;
        d.BindFlags=D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE;
        if(FAILED(g_dev->CreateTexture2D(&d,nullptr,n->copy.GetAddressOf()))) {fail("post target allocation");return nullptr;}
        node=n.get();g_graphBytes+=bytes;g_nodes.push_back(std::move(n));
    }
    if(node->generation!=g_generation) {
        ctx->CopyResource(node->copy.Get(),original);node->generation=g_generation;
    }
    return node;
}
ID3D11RenderTargetView* node_rtv(Node* n,ID3D11RenderTargetView* original) {
    for(auto& v:n->views) if(v.original.Get()==original) return v.rtv.Get();
    if(n->views.size()>=16)return nullptr;
    D3D11_RENDER_TARGET_VIEW_DESC d{};original->GetDesc(&d);
    if(d.ViewDimension!=D3D11_RTV_DIMENSION_TEXTURE2D || d.Texture2D.MipSlice)return nullptr;
    ViewMap v;v.original=original;
    if(FAILED(g_dev->CreateRenderTargetView(n->copy.Get(),&d,v.rtv.GetAddressOf())))return nullptr;
    auto* result=v.rtv.Get();n->views.push_back(std::move(v));return result;
}
ID3D11ShaderResourceView* node_srv(Node* n,ID3D11ShaderResourceView* original) {
    for(auto& v:n->views) if(v.original.Get()==original) return v.srv.Get();
    if(n->views.size()>=16)return nullptr;
    D3D11_SHADER_RESOURCE_VIEW_DESC d{};original->GetDesc(&d);
    if(d.ViewDimension!=D3D11_SRV_DIMENSION_TEXTURE2D || d.Texture2D.MostDetailedMip || d.Texture2D.MipLevels!=1)return nullptr;
    ViewMap v;v.original=original;
    if(FAILED(g_dev->CreateShaderResourceView(n->copy.Get(),&d,v.srv.GetAddressOf())))return nullptr;
    auto* result=v.srv.Get();n->views.push_back(std::move(v));return result;
}
void begin_frame() {
    if(g_open)return;
    // Old level render targets must not accumulate or keep a stale identity.
    for(auto it=g_nodes.begin();it!=g_nodes.end();) {
        if(g_generation-(*it)->generation>120) {g_graphBytes-=(*it)->bytes;it=g_nodes.erase(it);} else ++it;
    }
    ++g_generation;g_open=true;g_failed=g_assembled=g_complete=g_selected=false;
    g_fgDraws=g_worldDraws=g_postDraws=0;g_tanH=g_tanV=0;g_reason="no foreground frame";
    g_scene.Reset();g_sceneDepth.Reset();g_sourceReadOnlyDepth.Reset();g_output.Reset();g_view={};
}
bool same_tag() {
    return g_view.serial && g_view.serial==t_view.serial && g_view.build==t_view.build &&
           g_view.serial>t_boundary && GetTickCount64()-g_view.stamp<250;
}
bool begin_foreground(ID3D11DeviceContext* ctx,ID3D11RenderTargetView* rt,ID3D11DepthStencilView* ds) {
    if(t_view.serial<=t_boundary || (t_view.sign!=-1 && t_view.sign!=1) || !t_view.build ||
       !std::isfinite(t_view.separation) || t_view.separation<=0 || t_view.separation>128 ||
       GetTickCount64()-t_view.stamp>250) return fail("missing fresh render-eye camera");
    for(float v:t_view.camera.loc)if(!std::isfinite(v))return fail("invalid world camera");
    ComPtr<ID3D11Resource> colour,depth;rt->GetResource(colour.GetAddressOf());ds->GetResource(depth.GetAddressOf());
    D3D11_TEXTURE2D_DESC cd{},dd{};
    if(!texture_desc(colour.Get(),g_scene,cd) || !texture_desc(depth.Get(),g_sceneDepth,dd) ||
       cd.Format!=DXGI_FORMAT_R11G11B10_FLOAT || cd.Width!=dd.Width || cd.Height!=dd.Height ||
       (dd.Format!=DXGI_FORMAT_R24G8_TYPELESS && dd.Format!=DXGI_FORMAT_D24_UNORM_S8_UINT))
        return fail("foreground resource shape");
    if(g_fg.w && (g_fg.w!=cd.Width || g_fg.h!=cd.Height)) {
        g_nodes.clear();g_graphBytes=0;for(auto& h:g_history)h={};
    }
    D3D11_DEPTH_STENCIL_VIEW_DESC sourceView{};ds->GetDesc(&sourceView);
    sourceView.Flags=D3D11_DSV_READ_ONLY_DEPTH|D3D11_DSV_READ_ONLY_STENCIL;
    if(FAILED(g_dev->CreateDepthStencilView(g_sceneDepth.Get(),&sourceView,g_sourceReadOnlyDepth.GetAddressOf())))
        return fail("source read-only depth view");
    const UINT colourBind=D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE;
    if(!g_sourceCoverage.make(cd.Width,cd.Height,DXGI_FORMAT_R8_UNORM,colourBind) ||
       !g_fg.make(cd.Width,cd.Height,DXGI_FORMAT_R11G11B10_FLOAT,colourBind) ||
       !g_trans.make(cd.Width,cd.Height,DXGI_FORMAT_R16_FLOAT,colourBind) ||
       !g_fgDepth.make(cd.Width,cd.Height,DXGI_FORMAT_R24G8_TYPELESS,D3D11_BIND_DEPTH_STENCIL) ||
       !g_rawDepth.make(cd.Width,cd.Height,DXGI_FORMAT_R24G8_TYPELESS,D3D11_BIND_SHADER_RESOURCE))
        return fail("foreground allocation");
    const float black[4]{},one[4]={1,1,1,1};
    ctx->ClearRenderTargetView(g_sourceCoverage.rtv.Get(),one);
    ctx->ClearRenderTargetView(g_fg.rtv.Get(),black);ctx->ClearRenderTargetView(g_trans.rtv.Get(),one);
    ctx->ClearDepthStencilView(g_fgDepth.dsv.Get(),D3D11_CLEAR_DEPTH|D3D11_CLEAR_STENCIL,1,0);
    g_view=t_view;return true;
}
bool eligible_common(ID3D11DeviceContext* ctx) {
    if(!frame_inspector::replay_queries_idle(ctx))return fail("active/untracked query range");
    ComPtr<ID3D11Predicate> predicate;BOOL value=FALSE;ctx->GetPredication(predicate.GetAddressOf(),&value);
    if(predicate)return fail("predicated draw");
    ID3D11Buffer* so[D3D11_SO_BUFFER_SLOT_COUNT]{};ctx->SOGetTargets(_countof(so),so);
    bool unsupported=false;for(auto* b:so)if(b){unsupported=true;b->Release();}
    ID3D11UnorderedAccessView* uavs[64]{};const UINT n=g_dev->GetFeatureLevel()>=D3D_FEATURE_LEVEL_11_1?64:8;
    ctx->OMGetRenderTargetsAndUnorderedAccessViews(0,nullptr,nullptr,0,n,uavs);
    for(auto* u:uavs)if(u){unsupported=true;u->Release();}
    ComPtr<ID3D11GeometryShader> gs;ComPtr<ID3D11HullShader> hs;ComPtr<ID3D11DomainShader> ds;
    ctx->GSGetShader(gs.GetAddressOf(),nullptr,nullptr);ctx->HSGetShader(hs.GetAddressOf(),nullptr,nullptr);
    ctx->DSGetShader(ds.GetAddressOf(),nullptr,nullptr);
    ComPtr<ID3D11VertexShader> vs;ComPtr<ID3D11PixelShader> ps;UINT vc=0,pc=0;
    ctx->VSGetShader(vs.GetAddressOf(),nullptr,&vc);ctx->PSGetShader(ps.GetAddressOf(),nullptr,&pc);
    if(unsupported || gs || hs || ds || vc || pc || !vs || !ps)return fail("shader stages/side effects/class linkage");
    return true;
}
bool constant_read(ID3D11Buffer* buffer,std::vector<uint8_t>& bytes) {
    UINT size=0;
    if(!buffer || FAILED(buffer->GetPrivateData(kConstantSnapshot,&size,nullptr)) || !size || size>65536+sizeof(uint64_t))return false;
    if(size<=sizeof(uint64_t) || size>65536+sizeof(uint64_t))return false;
    bytes.resize(size);
    if(FAILED(buffer->GetPrivateData(kConstantSnapshot,&size,bytes.data())) || size!=bytes.size())return false;
    uint64_t epoch=0;std::memcpy(&epoch,bytes.data(),sizeof(epoch));
    if(epoch!=g_snapshotEpoch.load(std::memory_order_relaxed))return false;
    bytes.erase(bytes.begin(),bytes.begin()+sizeof(epoch));return true;
}
// Only declared resources matter; stale slots from a previous material are not
// dependencies. Used render surfaces in foreground require a target-eye version.
bool foreground_resources(ID3D11DeviceContext* ctx) {
    ComPtr<ID3D11VertexShader> vs;ComPtr<ID3D11PixelShader> ps;
    ctx->VSGetShader(vs.GetAddressOf(),nullptr,nullptr);ctx->PSGetShader(ps.GetAddressOf(),nullptr,nullptr);
    for(unsigned stage=0;stage<2;++stage) {
        eye::ShaderUsage usage;
        if(!eye::stream_shader_usage(stage?static_cast<ID3D11DeviceChild*>(ps.Get()):static_cast<ID3D11DeviceChild*>(vs.Get()),stage,usage))
            return fail("foreground shader metadata/outputs");
        for(unsigned slot:usage.resources) {
            ComPtr<ID3D11ShaderResourceView> srv;
            if(stage)ctx->PSGetShaderResources(slot,1,srv.GetAddressOf());else ctx->VSGetShaderResources(slot,1,srv.GetAddressOf());
            if(!srv)continue;
            ComPtr<ID3D11Resource> res;srv->GetResource(res.GetAddressOf());
            ComPtr<ID3D11Texture2D> texture;D3D11_TEXTURE2D_DESC desc{};
            if(SUCCEEDED(res->QueryInterface(__uuidof(ID3D11Texture2D),reinterpret_cast<void**>(texture.GetAddressOf())))) {
                texture->GetDesc(&desc);
                if(desc.BindFlags&(D3D11_BIND_RENDER_TARGET|D3D11_BIND_DEPTH_STENCIL|D3D11_BIND_UNORDERED_ACCESS))
                    return fail("foreground samples a view-dependent render surface");
            }
        }
    }
    return true;
}
bool assemble_hdr(ID3D11DeviceContext* ctx) {
    if(g_assembled)return true;
    if(!same_tag() || !g_fgDraws || g_worldDraws<8 || !g_scene || !g_sceneDepth || g_tanH<=0 || g_tanV<=0)
        return fail("incomplete foreground/world/camera before post");
    ctx->CopyResource(g_rawDepth.texture.Get(),g_sceneDepth.Get());
    tgpu::Params depth=g_view.depth;depth.w=g_fg.w;depth.h=g_fg.h;
    depth.cur=g_view.camera;depth.cur.tanH=g_tanH;depth.cur.tanV=g_tanV;
    depth.historyValid=false;depth.haveForeground=false;
    char why[256]{};
    if(!g_guides.run(g_dev.Get(),ctx,g_rawDepth.srv.Get(),nullptr,depth,why,sizeof(why))) {
        BVR_LOG("[native-repair] HDR guides: %s",why);return fail("HDR guide generation");
    }
    const unsigned fresh=g_view.sign<0?0:1;
    History& f=g_history[fresh];History& h=g_history[fresh^1];
    const UINT bind=D3D11_BIND_SHADER_RESOURCE;
    if(!f.hdr.make(g_fg.w,g_fg.h,DXGI_FORMAT_R11G11B10_FLOAT,bind) ||
       !f.z.make(g_fg.w,g_fg.h,DXGI_FORMAT_R32_FLOAT,bind) ||
       !f.mask.make(g_fg.w,g_fg.h,DXGI_FORMAT_R8_UNORM,bind))return fail("HDR history allocation");
    ctx->CopyResource(f.hdr.texture.Get(),g_scene.Get());ctx->CopyResource(f.z.texture.Get(),g_guides.linear_z());
    ctx->CopyResource(f.mask.texture.Get(),g_sourceCoverage.texture.Get());
    f.camera=depth.cur;f.build=g_view.build;f.stamp=GetTickCount64();f.valid=true;
    afw::Source a,b;b.valid=true;b.cam=f.camera;b.color=f.hdr.srv.Get();b.depth=f.z.srv.Get();b.mask=f.mask.srv.Get();
    b.depthW=g_fg.w;b.depthH=g_fg.h;b.invertedMask=true;a.invertedMask=true;
    a.valid=h.valid && h.hdr.w==g_fg.w && h.hdr.h==g_fg.h && f.stamp-h.stamp<150 && h.build<f.build &&
        std::fabs(h.camera.tanH-g_tanH)<1e-4f && std::fabs(h.camera.tanV-g_tanV)<1e-4f;
    double distance=0;for(unsigned i=0;i<3;++i)distance+=double(f.camera.loc[i]-h.camera.loc[i])*(f.camera.loc[i]-h.camera.loc[i]);
    if(distance>300.0*300.0)a.valid=false;
    a.cam=h.camera;a.color=h.hdr.srv.Get();a.depth=h.z.srv.Get();a.mask=h.mask.srv.Get();a.depthW=h.z.w;a.depthH=h.z.h;
    afw::Params p=g_view.warp;p.w=g_fg.w;p.h=g_fg.h;p.target=f.camera;
    for(unsigned i=0;i<3;++i)p.target.loc[i]-=g_view.sign*g_view.separation*f.camera.right[i];
    p.hdrWorld=true;p.refine=true;p.handsFromFresh=true;p.tint=false;
    p.nativeForeground=g_fg.srv.Get();p.foregroundTransmittance=g_trans.srv.Get();
    if(!g_worldWarp.run(g_dev.Get(),ctx,a,b,p,why,sizeof(why))) {
        BVR_LOG("[native-repair] HDR warp: %s",why);return fail("HDR world/native foreground compose");
    }
    Node* scene=ensure_node(ctx,g_scene.Get());if(!scene)return false;
    ctx->CopyResource(scene->copy.Get(),g_worldWarp.out());g_assembled=true;return true;
}
struct PostBindings {
    ID3D11DeviceContext* ctx;
    const eye::ShaderUsage* usage;
    ComPtr<ID3D11RenderTargetView> rt;ComPtr<ID3D11DepthStencilView> ds;
    ID3D11ShaderResourceView* original[2][D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT]{};
    ID3D11ShaderResourceView* patched[2][D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT]{};
    bool touched=false;
    PostBindings(ID3D11DeviceContext* c,const eye::ShaderUsage used[2]):ctx(c),usage(used) {
        ctx->OMGetRenderTargets(1,rt.GetAddressOf(),ds.GetAddressOf());
        for(unsigned stage=0;stage<2;++stage)for(unsigned slot:usage[stage].resources) {
            if(stage)ctx->PSGetShaderResources(slot,1,&original[stage][slot]);
            else ctx->VSGetShaderResources(slot,1,&original[stage][slot]);
            patched[stage][slot]=original[stage][slot];++g_stats.postSlotsRead;
        }
    }
    void set(unsigned stage,unsigned slot,ID3D11ShaderResourceView* srv) {
        if(stage)ctx->PSSetShaderResources(slot,1,&srv);else ctx->VSSetShaderResources(slot,1,&srv);
        ++g_stats.postSlotsSet;
    }
    void bind(ID3D11RenderTargetView* target) {
        touched=true;ctx->OMSetRenderTargets(1,&target,nullptr);
        for(unsigned stage=0;stage<2;++stage)for(unsigned slot:usage[stage].resources)
            if(patched[stage][slot]!=original[stage][slot])set(stage,slot,patched[stage][slot]);
    }
    ~PostBindings() {
        if(touched) {
            // Clones are private: unused native slots cannot alias a clone RTV.
            // Clear only the changed inputs before restoring the incoming outputs.
            for(unsigned stage=0;stage<2;++stage)for(unsigned slot:usage[stage].resources)
                if(patched[stage][slot]!=original[stage][slot])set(stage,slot,nullptr);
            auto* target=rt.Get();ctx->OMSetRenderTargets(1,&target,ds.Get());
            for(unsigned stage=0;stage<2;++stage)for(unsigned slot:usage[stage].resources)
                if(patched[stage][slot]!=original[stage][slot])set(stage,slot,original[stage][slot]);
        }
        for(unsigned stage=0;stage<2;++stage)for(unsigned slot:usage[stage].resources)
            if(original[stage][slot])original[stage][slot]->Release();
    }
};
bool prepare_draw(ID3D11DeviceContext* ctx,const DrawArgs& args) noexcept {
    if(!live_context(ctx) || !args.count)return false;
    try {
        begin_frame();if(g_failed)return false;
        D3D11_VIEWPORT vp[16]{};UINT nv=_countof(vp);ctx->RSGetViewports(&nv,vp);
        if(nv!=1)return false;
        ComPtr<ID3D11RenderTargetView> rt;ComPtr<ID3D11DepthStencilView> ds;
        ctx->OMGetRenderTargets(1,rt.GetAddressOf(),ds.GetAddressOf());if(!rt)return false;
        D3D11_RENDER_TARGET_VIEW_DESC rv{};rt->GetDesc(&rv);
        const bool fg=rv.Format==DXGI_FORMAT_R11G11B10_FLOAT && ds && vp[0].MinDepth==0 && vp[0].MaxDepth>0 && vp[0].MaxDepth<=0.011f;
        ComPtr<ID3D11Resource> target;rt->GetResource(target.GetAddressOf());
        if(fg) {
            if(g_assembled || ++g_fgDraws>kMaxForeground)return fail("foreground order/draw cap");
            if(g_fgDraws==1 && !begin_foreground(ctx,rt.Get(),ds.Get()))return false;
            if(!same_tag() || target.Get()!=g_scene.Get())return fail("foreground camera/target changed");
            ComPtr<ID3D11Resource> depth;ds->GetResource(depth.GetAddressOf());
            D3D11_DEPTH_STENCIL_VIEW_DESC dd{};ds->GetDesc(&dd);
            if(depth.Get()!=g_sceneDepth.Get() || dd.Flags || dd.ViewDimension!=D3D11_DSV_DIMENSION_TEXTURE2D || dd.Texture2D.MipSlice ||
               vp[0].TopLeftX || vp[0].TopLeftY || vp[0].Width!=g_fg.w || vp[0].Height!=g_fg.h ||
               rv.ViewDimension!=D3D11_RTV_DIMENSION_TEXTURE2D || rv.Texture2D.MipSlice)
                return fail("foreground viewport/depth binding");
            if(!eligible_common(ctx) || !foreground_resources(ctx))return false;
            ID3D11RenderTargetView* targets[8]{};ctx->OMGetRenderTargets(8,targets,nullptr);
            bool mrt=false;for(unsigned i=0;i<8;++i)if(targets[i]){mrt|=i!=0;targets[i]->Release();}
            if(mrt)return fail("foreground MRT");
            const char* reason=nullptr;float th=0,tv=0;
            if(!eye::stream_eye_replay(ctx,args,g_fg.rtv.Get(),g_fgDepth.dsv.Get(),g_trans.rtv.Get(),
                g_sourceCoverage.rtv.Get(),g_sourceReadOnlyDepth.Get(),
                -g_view.sign*g_view.separation,g_view.camera.loc,&constant_read,&th,&tv,&reason))
                return fail(reason?reason:"foreground replay failed");
            if(g_tanH && (std::fabs(g_tanH-th)>1e-4f || std::fabs(g_tanV-tv)>1e-4f))return fail("foreground lens changed");
            g_tanH=th;g_tanV=tv;++g_stats.foreground;return false;
        }
        if(!g_scene)return false;
        if(target.Get()==g_scene.Get() && vp[0].MaxDepth>0.9f) {
            if(g_assembled)return fail("scene draw after post started");
            ++g_worldDraws;return false;
        }
        // Indexed geometry on an already-mapped post target cannot be omitted.
        if(args.kind!=frame_inspector::DrawKind::Direct) {
            if(find_node(target.Get()))fail("non-direct post draw");return false;
        }
        ComPtr<ID3D11VertexShader> vs;ComPtr<ID3D11PixelShader> ps;
        ctx->VSGetShader(vs.GetAddressOf(),nullptr,nullptr);ctx->PSGetShader(ps.GetAddressOf(),nullptr,nullptr);
        eye::ShaderUsage usage[2];
        if(!eye::stream_shader_usage(vs.Get(),0,usage[0]) || !eye::stream_shader_usage(ps.Get(),1,usage[1])) {
            if(find_node(target.Get()))fail("post shader metadata/outputs");return false;
        }
        PostBindings saved(ctx,usage);
        bool dependent=find_node(target.Get())!=nullptr,sceneRead=false;
        for(unsigned stage=0;stage<2;++stage)for(unsigned slot:usage[stage].resources) {
            auto* srv=saved.original[stage][slot];if(!srv)continue;
            ComPtr<ID3D11Resource> res;srv->GetResource(res.GetAddressOf());
            sceneRead|=res.Get()==g_scene.Get();dependent|=find_node(res.Get())!=nullptr;
        }
        if(!dependent && !sceneRead)return false;
        if(!same_tag())return fail("post render-eye tag stale/changed");
        if(++g_postDraws>kMaxPost)return fail("post draw cap");
        if(!eligible_common(ctx))return false;
        ComPtr<ID3D11DepthStencilState> state;UINT ref=0;ctx->OMGetDepthStencilState(state.GetAddressOf(),&ref);
        D3D11_DEPTH_STENCIL_DESC d{};
        if(state)state->GetDesc(&d);else {d.DepthEnable=TRUE;d.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ALL;d.DepthFunc=D3D11_COMPARISON_LESS;}
        if(d.StencilEnable || (d.DepthEnable && (d.DepthWriteMask!=D3D11_DEPTH_WRITE_MASK_ZERO || d.DepthFunc!=D3D11_COMPARISON_ALWAYS)))
            return fail("post depth/stencil dependency");
        ID3D11RenderTargetView* targets[8]{};ctx->OMGetRenderTargets(8,targets,nullptr);
        bool mrt=false;for(unsigned i=0;i<8;++i)if(targets[i]){mrt|=i!=0;targets[i]->Release();}
        if(mrt)return fail("post MRT");
        if(!g_assembled && !assemble_hdr(ctx))return false;
        Node* node=ensure_node(ctx,target.Get());if(!node)return false;
        auto* cloneRt=node_rtv(node,rt.Get());if(!cloneRt)return fail("post RTV layout");
        for(unsigned stage=0;stage<2;++stage)for(unsigned slot:usage[stage].resources) {
            auto* srv=saved.original[stage][slot];if(!srv)continue;
            ComPtr<ID3D11Resource> res;srv->GetResource(res.GetAddressOf());
            if(Node* source=find_node(res.Get())) {
                saved.patched[stage][slot]=node_srv(source,srv);
                if(!saved.patched[stage][slot] || source==node)return fail("post SRV alias/layout");
            }
        }
        saved.bind(cloneRt);
        if(!frame_inspector::replay_draw(ctx,args))return fail("post replay unavailable");
        ++g_stats.post;
    } catch (...) {fail("native stream exception");}
    // Replay consumes all VB/IB/CB/material versions before this original draw.
    return false;
}
void after_draw(ID3D11DeviceContext*,const DrawArgs&) noexcept {}
void on_present(ID3D11DeviceContext* ctx) noexcept {
    if(!g_enabled.load(std::memory_order_relaxed))return;
    try {
        if(!g_ctx) {g_ctx=ctx;ctx->GetDevice(g_dev.GetAddressOf());shader_bytecode::bind_device(g_dev.Get());}
        if(ctx!=g_ctx.Get())return;
        if(!frame_inspector::resource_observer_hooks_ready())fail("resource hooks unavailable");
        g_complete=g_open && !g_failed && g_assembled && g_fgDraws && g_worldDraws>=8 && g_postDraws;
        if(g_open)++g_stats.frames;if(g_complete)++g_stats.complete;
        g_open=false;t_boundary=t_view.serial;
        const uint64_t now=GetTickCount64();
        if(now-g_lastLog>=5000 && g_stats.frames) {
            BVR_LOG("[native-repair] frames=%llu complete=%llu shown=%llu refused=%llu fg=%llu post=%llu uploads=%llu copies=%llu last=%s",
                g_stats.frames,g_stats.complete,g_stats.shown,g_stats.refused,g_stats.foreground,g_stats.post,g_stats.uploads,g_stats.copies,
                g_complete?"HDR chain ready":g_reason);
            const auto cost=eye::stream_eye_stats();
            BVR_LOG("[native-repair] replay-cost cbRead=%llu cbRestore=%llu srvRead=%llu srvSet=%llu stateCreates=%llu gpuDraws=%llu; direct GPU foreground, no second scene build",
                cost.constantSlotsRead,cost.constantSlotsRestored,g_stats.postSlotsRead,g_stats.postSlotsSet,cost.stateCreates,cost.replayDraws);
            g_lastLog=now;
        }
    } catch (...) {fail("Present stream closure");}
}
const frame_inspector::DrawReplayProbe kProbe{&prepare_draw,&after_draw,&on_present};
struct Mapped {ComPtr<ID3D11Resource> resource;void* data=nullptr;UINT bytes=0,sub=0;bool capture=false;};
std::array<Mapped,32> g_maps;
bool save_constants(ID3D11Resource* resource,const void* data,UINT bytes) {
    const uint64_t epoch=g_snapshotEpoch.load(std::memory_order_relaxed);
    g_uploadBytes.resize(sizeof(epoch)+bytes);
    std::memcpy(g_uploadBytes.data(),&epoch,sizeof(epoch));
    std::memcpy(g_uploadBytes.data()+sizeof(epoch),data,bytes);
    return SUCCEEDED(resource->SetPrivateData(kConstantSnapshot,static_cast<UINT>(g_uploadBytes.size()),g_uploadBytes.data()));
}
void invalidate(ID3D11Resource* resource) {if(resource)resource->SetPrivateData(kConstantSnapshot,0,nullptr);}
void on_map(ID3D11DeviceContext* ctx,ID3D11Resource* res,unsigned sub,unsigned type,void* data) noexcept {
    if(!live_context(ctx) || type==D3D11_MAP_READ)return;
    try {
        invalidate(res);
        if(g_open && find_node(res))fail("mapped post resource");
        ComPtr<ID3D11Buffer> buffer;
        if(sub || !res || FAILED(res->QueryInterface(__uuidof(ID3D11Buffer),reinterpret_cast<void**>(buffer.GetAddressOf()))))return;
        D3D11_BUFFER_DESC d{};buffer->GetDesc(&d);
        if(d.BindFlags!=D3D11_BIND_CONSTANT_BUFFER || !d.ByteWidth || d.ByteWidth>65536)return;
        for(auto& m:g_maps)if(!m.resource) {
            m.resource=res;m.data=data;m.bytes=d.ByteWidth;m.sub=sub;
            m.capture=type==D3D11_MAP_WRITE_DISCARD || type==D3D11_MAP_WRITE;return;
        }
        if(g_open)fail("simultaneous constant map cap");
    } catch (...) {if(g_open)fail("constant Map observation");}
}
void on_unmap(ID3D11DeviceContext* ctx,ID3D11Resource* res,unsigned sub) noexcept {
    if(ctx!=g_ctx.Get())return;
    for(auto& m:g_maps)if(m.resource.Get()==res && m.sub==sub) {
        try {if(m.capture && m.data && save_constants(res,m.data,m.bytes))++g_stats.uploads;}
        catch (...) {invalidate(res);if(g_open)fail("constant snapshot allocation");}
        m={};return;
    }
}
void on_update(ID3D11DeviceContext* ctx,ID3D11Resource* dst,unsigned sub,const D3D11_BOX* box,
               const void* data,unsigned,unsigned) noexcept {
    if(!live_context(ctx))return;
    try {
        invalidate(dst);if(g_open && find_node(dst))fail("updated post resource");
        ComPtr<ID3D11Buffer> buffer;
        if(!dst || sub || !data || box || FAILED(dst->QueryInterface(__uuidof(ID3D11Buffer),reinterpret_cast<void**>(buffer.GetAddressOf()))))return;
        D3D11_BUFFER_DESC d{};buffer->GetDesc(&d);
        if(d.BindFlags==D3D11_BIND_CONSTANT_BUFFER && d.ByteWidth && d.ByteWidth<=65536 &&
           save_constants(dst,data,d.ByteWidth))++g_stats.uploads;
    } catch (...) {if(g_open)fail("constant update observation");}
}
void on_copy(ID3D11DeviceContext* ctx,ID3D11Resource* dst,unsigned sub,unsigned x,unsigned y,unsigned z,
             ID3D11Resource* src,unsigned srcSub,const D3D11_BOX* box,bool whole) noexcept {
    if(!live_context(ctx))return;
    try {
        invalidate(dst);
        if(!g_open || g_failed)return;
        Node* source=find_node(src);Node* target=find_node(dst);
        if(!source && !target) {
            if(g_fgDraws && (dst==g_scene.Get() || dst==g_sceneDepth.Get()))fail("scene copy after foreground began");
            return;
        }
        if(sub || srcSub || z)return static_cast<void>(fail("post copy subresource"));
        if(!target)target=ensure_node(ctx,dst);if(!target)return;
        if(source==target)return static_cast<void>(fail("post self copy"));
        if(whole)ctx->CopyResource(target->copy.Get(),source?source->copy.Get():src);
        else ctx->CopySubresourceRegion(target->copy.Get(),0,x,y,0,source?source->copy.Get():src,0,box);
        ++g_stats.copies;
    } catch (...) {fail("post copy observation");}
}
void on_clear_rtv(ID3D11DeviceContext* ctx,ID3D11RenderTargetView* rt,const float color[4]) noexcept {
    if(!live_context(ctx) || !g_open || g_failed || !rt)return;
    try {
        ComPtr<ID3D11Resource> r;rt->GetResource(r.GetAddressOf());
        if(r.Get()==g_scene.Get())return static_cast<void>(fail("scene clear after foreground began"));
        if(Node* n=find_node(r.Get())) {
            auto* view=node_rtv(n,rt);if(!view)return static_cast<void>(fail("post clear RTV"));
            ctx->ClearRenderTargetView(view,color);
        }
    } catch (...) {fail("post clear observation");}
}
void on_clear_dsv(ID3D11DeviceContext* ctx,ID3D11DepthStencilView* ds,unsigned flags,float,unsigned char) noexcept {
    if(!live_context(ctx) || !g_open || g_failed || !ds || !(flags&D3D11_CLEAR_DEPTH))return;
    ComPtr<ID3D11Resource> r;ds->GetResource(r.GetAddressOf());
    // Shared depth can be cleared for UI after HDR has been composed.
    if(!g_assembled && r.Get()==g_sceneDepth.Get())fail("scene depth cleared before HDR assembly");
}
void on_unknown(ID3D11DeviceContext* ctx,const char*) noexcept {
    if(!live_context(ctx))return;
    ++g_snapshotEpoch; // a command list can rewrite constants without Map callbacks
    if(g_open && g_fgDraws)fail("untracked draw/dispatch/command list");
}
void on_reset(ID3D11DeviceContext* ctx) noexcept {
    if(ctx!=g_ctx.Get())return;
    g_nodes.clear();g_graphBytes=0;g_scene.Reset();g_sceneDepth.Reset();g_sourceReadOnlyDepth.Reset();g_output.Reset();
    for(auto& h:g_history)h.valid=false;
    for(auto& m:g_maps)m={};
    ++g_snapshotEpoch;g_open=g_complete=g_selected=false;g_failed=false;t_boundary=t_view.serial;
    g_reason="resource reset; waiting for a fresh scene";
}
const frame_inspector::ResourceObserver kResources{&on_map,&on_unmap,&on_update,&on_copy,&on_clear_rtv,&on_clear_dsv,&on_unknown,&on_reset};
void enable() {
    g_enabled.store(true);frame_inspector::set_resource_observer(&kResources);frame_inspector::set_draw_replay_probe(&kProbe);
}
} // namespace
void init(const bvr::pattern_scan::ProcessImage& image) {
    HMODULE self=nullptr;wchar_t ini[MAX_PATH]{};
    if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&init),&self) || !GetModuleFileNameW(self,ini,MAX_PATH))return;
    wchar_t* slash=wcsrchr(ini,L'\\');if(!slash)return;
    if(wcscpy_s(slash+1,_countof(ini)-static_cast<size_t>(slash+1-ini),L"BioshockVR.ini"))return;
    if(GetPrivateProfileIntW(L"Bioshock1R",L"NativeForegroundRepair",0,ini)!=1)return;
    if(GetPrivateProfileIntW(L"Bioshock1R",L"NativeRepairProof",0,ini) || GetPrivateProfileIntW(L"Bioshock1R",L"NativeRepairAudit",0,ini)) {
        BVR_LOG("[native-repair] disabled: proof/audit owns the private replay stream");return;
    }
    shader_bytecode::configure(image.base,image.size);enable();
    BVR_LOG("[native-repair] native foreground + HDR post enabled; world remains AFW; unsupported frames retain accepted AFW");
}
// Uploads while inactive are deliberately unobserved. Re-enabling requires new
// complete snapshots even if the same COM buffer is still bound.
void set_active(bool on) {
    if(g_active.exchange(on,std::memory_order_relaxed)!=on)++g_snapshotEpoch;
}
void note_camera(uint64_t build,int sign,const tgpu::Camera& camera,float separation,const tgpu::Params& depth,const afw::Params& warp) {
    if(!g_enabled.load(std::memory_order_relaxed))return;
    ++t_view.serial;t_view.build=build;t_view.sign=sign;t_view.camera=camera;t_view.separation=separation;
    t_view.depth=depth;t_view.warp=warp;t_view.stamp=GetTickCount64();
}
void finish_eye(ID3D11Texture2D* original,uint64_t build,int sign,float tanH,float tanV,bool plainResolution) {
    g_selected=false;g_output.Reset();
    if(!g_enabled.load() || !g_active.load() || !g_complete)return;
    if(!plainResolution || build!=g_view.build || sign!=g_view.sign ||
       std::fabs(tanH-g_tanH)>1e-4f || std::fabs(tanV-g_tanV)>1e-4f) {
        fail("present eye/lens/resolution mismatch");return;
    }
    Node* result=find_node(original);if(!result) {fail("post chain did not reach the actual backbuffer");return;}
    D3D11_TEXTURE2D_DESC d{};result->copy->GetDesc(&d);
    if(d.Width!=g_fg.w || d.Height!=g_fg.h ||
       (d.Format!=DXGI_FORMAT_R8G8B8A8_UNORM && d.Format!=DXGI_FORMAT_R8G8B8A8_TYPELESS)) {
        fail("backbuffer output shape");return;
    }
    g_output=result->copy;g_selected=true;
}
bool copy_output(ID3D11DeviceContext* ctx,ID3D11Texture2D* dst,uint64_t build,int freshEye,float tanH,float tanV) {
    if(!live_context(ctx) || !g_selected || !g_output || !dst || build!=g_view.build ||
       freshEye!=(g_view.sign<0?0:1) || std::fabs(tanH-g_tanH)>1e-4f || std::fabs(tanV-g_tanV)>1e-4f)return false;
    g_selected=false;D3D11_TEXTURE2D_DESC a{},b{};g_output->GetDesc(&a);dst->GetDesc(&b);
    if(a.Width!=b.Width || a.Height!=b.Height || b.SampleDesc.Count!=1 || b.ArraySize!=1 || b.MipLevels!=1 ||
       (b.Format!=DXGI_FORMAT_R8G8B8A8_UNORM && b.Format!=DXGI_FORMAT_R8G8B8A8_UNORM_SRGB && b.Format!=DXGI_FORMAT_R8G8B8A8_TYPELESS))return false;
    ctx->CopyResource(dst,g_output.Get());
    if(!g_stats.shown)BVR_LOG("[native-repair] first repaired eye DISPLAYED build=%llu freshSign=%d foreground=%u post=%u lens=%.7g,%.7g; native foreground/HDR, reconstructed world",
        build,g_view.sign,g_fgDraws,g_postDraws,g_tanH,g_tanV);
    ++g_stats.shown;return true;
}
Stats stats(){return g_stats;}
const char* last_reason(){return g_reason;}
void enable_for_test(){enable();set_active(true);}
void shutdown() {
    g_enabled.store(false);g_active.store(false);frame_inspector::set_resource_observer(nullptr);frame_inspector::set_draw_replay_probe(nullptr);
    for(auto& m:g_maps)m={};g_nodes.clear();g_graphBytes=0;for(auto& h:g_history)h={};
    g_fg={};g_trans={};g_fgDepth={};g_rawDepth={};g_sourceCoverage={};g_sourceReadOnlyDepth.Reset();g_scene.Reset();g_sceneDepth.Reset();g_output.Reset();
    g_guides.shutdown();g_worldWarp.shutdown();eye::stream_eye_reset();g_ctx.Reset();g_dev.Reset();
    g_open=g_failed=g_assembled=g_complete=g_selected=false;t_view={};t_boundary=0;g_generation=0;g_stats={};
}
} // namespace bvr::b1r::native_repair
