// Controlled D3D11 sparse native shading experiment, not the game's renderer.
// Same second-eye geometry and opaque shader in full and stencil-masked draws.
// Reports both pipeline work and GPU time; never infer game FPS from these times.
#include "gpu-tests/gpu_test_util.h"
#include <algorithm>
#include <functional>
#include <string>

namespace {
using namespace gt;
constexpr UINT W=2064,H=2208,Grid=128,Vertices=Grid*Grid*6;
const char* Shader=R"hlsl(
Texture2D<float> repairMask : register(t0);
struct V { float4 pos:SV_Position; float2 uv:TEXCOORD0; };
V vs(uint id:SV_VertexID) {
    const uint2 c[6]={uint2(0,0),uint2(1,0),uint2(0,1),uint2(0,1),uint2(1,0),uint2(1,1)};
    uint q=id/6; float2 uv=(float2(q%128,q/128)+c[id%6])/128.0;
    float2 p=uv*float2(2,-2)+float2(-1,1);
    float z=1.5+0.15*sin(p.x*5+p.y*3);
    V o; o.pos=float4(p.x*z-0.045,p.y*z,0.5*z,z);o.uv=uv;return o;
}
V full(uint id:SV_VertexID) {
    float2 uv=float2((id<<1)&2,id&2);
    V o;o.pos=float4(uv*float2(2,-2)+float2(-1,1),0,1);o.uv=uv;return o;
}
// No forced earlydepthstencil attribute: a normal opaque shader permits early
// rejection. This does not establish the behavior of an arbitrary game shader.
float4 shade(V i):SV_Target {
    float3 c=float3(i.uv,0.37);
    [loop] for(int j=0;j<24;++j)
        c=frac(c*1.011+sin(c.yzx*1.7+float(j)*0.01)*0.023+0.003);
    return float4(c,1);
}
float4 mark(V i):SV_Target {
    clip(repairMask.Load(int3(int2(i.pos.xy),0))-.5);return 0;
}
)hlsl";
ID3DBlob* compile(const char* entry,const char* profile) {
    ID3DBlob *b=nullptr,*e=nullptr;
    const HRESULT hr=D3DCompile(Shader,strlen(Shader),nullptr,nullptr,nullptr,entry,profile,D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&b,&e);
    if(FAILED(hr)) {printf("shader %s: %s\n",entry,e?(const char*)e->GetBufferPointer():"compile failed");rel(b);}
    rel(e);return b;
}
bool wait_query(ID3D11Query* q,void* p,UINT n) {
    const auto start=GetTickCount64();
    for(;;) {
        const HRESULT hr=ctx->GetData(q,p,n,0);
        if(hr==S_OK)return true;
        if(FAILED(hr)||GetTickCount64()-start>10000)return false;
        Sleep(1);
    }
}
double milliseconds(const std::function<void()>& draw) {
    ID3D11Query *d=nullptr,*a=nullptr,*b=nullptr;
    D3D11_QUERY_DESC desc{D3D11_QUERY_TIMESTAMP_DISJOINT,0};
    if(FAILED(dev->CreateQuery(&desc,&d)))return -1;
    desc.Query=D3D11_QUERY_TIMESTAMP;
    if(FAILED(dev->CreateQuery(&desc,&a))||FAILED(dev->CreateQuery(&desc,&b))) {rel(d);rel(a);rel(b);return -1;}
    for(int i=0;i<3;++i)draw();
    std::vector<double> samples;
    for(int batch=0;batch<5;++batch) {
        ctx->Begin(d);ctx->End(a);
        for(int i=0;i<10;++i)draw();
        ctx->End(b);ctx->End(d);
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj{};UINT64 t0=0,t1=0;
        if(wait_query(d,&dj,sizeof(dj))&&wait_query(a,&t0,sizeof(t0))&&wait_query(b,&t1,sizeof(t1))&&!dj.Disjoint)
            samples.push_back(double(t1-t0)*1000/dj.Frequency/10);
    }
    rel(d);rel(a);rel(b);
    if(samples.empty())return -1;
    std::sort(samples.begin(),samples.end());return samples[samples.size()/2];
}
D3D11_QUERY_DATA_PIPELINE_STATISTICS work(const std::function<void()>& draw) {
    D3D11_QUERY_DATA_PIPELINE_STATISTICS s{};ID3D11Query* q=nullptr;
    D3D11_QUERY_DESC d{D3D11_QUERY_PIPELINE_STATISTICS,0};
    if(SUCCEEDED(dev->CreateQuery(&d,&q))) {ctx->Begin(q);draw();ctx->End(q);if(!wait_query(q,&s,sizeof(s)))s={};}
    rel(q);return s;
}
ID3D11DepthStencilState* depth_state(int mode) {
    D3D11_DEPTH_STENCIL_DESC d{};d.DepthEnable=FALSE;d.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ZERO;d.DepthFunc=D3D11_COMPARISON_ALWAYS;
    d.StencilEnable=mode!=0;d.StencilReadMask=255;d.StencilWriteMask=mode==1?255:0;
    d.FrontFace.StencilFunc=mode==1?D3D11_COMPARISON_ALWAYS:D3D11_COMPARISON_EQUAL;
    d.FrontFace.StencilFailOp=d.FrontFace.StencilDepthFailOp=D3D11_STENCIL_OP_KEEP;
    d.FrontFace.StencilPassOp=mode==1?D3D11_STENCIL_OP_REPLACE:D3D11_STENCIL_OP_KEEP;
    d.BackFace=d.FrontFace;ID3D11DepthStencilState* p=nullptr;dev->CreateDepthStencilState(&d,&p);return p;
}
}
int main() {
    setvbuf(stdout,nullptr,_IONBF,0);
    if(!make_device())return 1;
    ID3D11VertexShader *vs=nullptr,*full=nullptr;
    ID3D11PixelShader *shade=nullptr,*mark=nullptr;
    ID3DBlob* b=compile("vs","vs_5_0");if(!b)return 1;
    dev->CreateVertexShader(b->GetBufferPointer(),b->GetBufferSize(),nullptr,&vs);rel(b);
    b=compile("full","vs_5_0");if(!b)return 1;dev->CreateVertexShader(b->GetBufferPointer(),b->GetBufferSize(),nullptr,&full);rel(b);
    b=compile("shade","ps_5_0");if(!b)return 1;dev->CreatePixelShader(b->GetBufferPointer(),b->GetBufferSize(),nullptr,&shade);rel(b);
    b=compile("mark","ps_5_0");if(!b)return 1;dev->CreatePixelShader(b->GetBufferPointer(),b->GetBufferSize(),nullptr,&mark);rel(b);
    auto* rt=make_tex(W,H,DXGI_FORMAT_R8G8B8A8_UNORM,D3D11_BIND_RENDER_TARGET);
    auto* z=make_tex(W,H,DXGI_FORMAT_D24_UNORM_S8_UINT,D3D11_BIND_DEPTH_STENCIL);
    ID3D11RenderTargetView* rtv=nullptr;ID3D11DepthStencilView* dsv=nullptr;
    if(rt)dev->CreateRenderTargetView(rt,nullptr,&rtv);if(z)dev->CreateDepthStencilView(z,nullptr,&dsv);
    auto *off=depth_state(0),*write=depth_state(1),*test=depth_state(2);
    D3D11_RASTERIZER_DESC rd{};rd.FillMode=D3D11_FILL_SOLID;rd.CullMode=D3D11_CULL_NONE;rd.DepthClipEnable=TRUE;
    ID3D11RasterizerState* raster=nullptr;dev->CreateRasterizerState(&rd,&raster);
    if(!vs||!full||!shade||!mark||!rtv||!dsv||!off||!write||!test||!raster)return 1;
    D3D11_VIEWPORT vp{0,0,float(W),float(H),0,1};ctx->RSSetViewports(1,&vp);ctx->RSSetState(raster);
    ctx->IASetInputLayout(nullptr);ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    const float sentinel[4]{1,0,1,1};
    auto native=[&](bool masked){
        ctx->OMSetRenderTargets(1,&rtv,dsv);ctx->OMSetDepthStencilState(masked?test:off,1);
        ctx->VSSetShader(vs,nullptr,0);ctx->PSSetShader(shade,nullptr,0);ctx->Draw(Vertices,0);
    };
    ctx->ClearRenderTargetView(rtv,sentinel);native(false);auto truth=readback(rt);
    if(truth.size()!=size_t(W)*H*4)return 1;
    const auto fullWork=work([&](){native(false);});
    const double fullMs=milliseconds([&](){native(false);});
    printf("SYNTHETIC ONLY: %ux%u, 128x128 tessellated surface, one second-eye draw, 24-step opaque shading.\n",W,H);
    printf("full native: %.4f ms; VS %llu; PS %llu\n",fullMs,fullWork.VSInvocations,fullWork.PSInvocations);
    check(fullMs>0&&fullWork.VSInvocations>0&&fullWork.PSInvocations>W*H/2,"unmasked control executes geometry and shading","%.4f ms",fullMs);
    struct Case {const char* name;int shape;int thousand;};
    const Case cases[]={{"empty",0,0},{"cluster-0.1pct",1,1},{"cluster-1pct",1,10},{"scatter-1pct",2,10},{"scatter-5pct",2,50},{"scatter-10pct",2,100},{"all",0,1000}};
    for(const auto& c:cases) {
        std::vector<uint8_t> pixels(size_t(W)*H);size_t selected=0;
        const double side=sqrt(c.thousand/1000.0);
        for(UINT y=0;y<H;++y)for(UINT x=0;x<W;++x) {
            bool yes=c.thousand==1000;
            if(c.shape==1)yes=std::abs((x+.5)/W-.5)<side*.5&&std::abs((y+.5)/H-.5)<side*.5;
            if(c.shape==2){UINT h=(x/8)*73856093u^(y/8)*19349663u;h^=h>>13;yes=h%1000<UINT(c.thousand);}
            pixels[size_t(y)*W+x]=yes?255:0;selected+=yes;
        }
        auto* mt=make_tex(W,H,DXGI_FORMAT_R8_UNORM,D3D11_BIND_SHADER_RESOURCE,pixels.data(),W);
        auto* ms=mt?srv_of(mt):nullptr;if(!ms)return 1;
        auto mask=[&](){
            ctx->OMSetRenderTargets(0,nullptr,dsv);ctx->ClearDepthStencilView(dsv,D3D11_CLEAR_STENCIL,1,0);
            ctx->OMSetDepthStencilState(write,1);ctx->VSSetShader(full,nullptr,0);ctx->PSSetShader(mark,nullptr,0);
            ctx->PSSetShaderResources(0,1,&ms);ctx->Draw(3,0);ID3D11ShaderResourceView* nil=nullptr;ctx->PSSetShaderResources(0,1,&nil);
        };
        mask();ctx->ClearRenderTargetView(rtv,sentinel);native(true);
        const auto out=readback(rt);size_t badIn=0,badOut=0;
        if(out.size()!=truth.size())return 1;
        for(size_t i=0;i<pixels.size();++i)for(int ch=0;ch<4;++ch) {
            if(pixels[i])badIn+=out[i*4+ch]!=truth[i*4+ch];
            else badOut+=out[i*4+ch]!=sentinel[ch];
        }
        const auto stats=work([&](){native(true);});
        const double drawMs=milliseconds([&](){native(true);});
        const double maskMs=milliseconds(mask);
        const double bothMs=milliseconds([&](){mask();native(true);});
        printf("%-16s coverage %.4f%% | native %.4f ms mask %.4f ms combined %.4f ms | VS %llu PS %llu (%.3f%% full)\n",c.name,100.0*selected/pixels.size(),drawMs,maskMs,bothMs,stats.VSInvocations,stats.PSInvocations,100.0*stats.PSInvocations/fullWork.PSInvocations);
        check(!badIn&&!badOut,"selected pixels match native; others retain their input","%s: channel mismatches %zu in / %zu out",c.name,badIn,badOut);
        check(stats.VSInvocations==fullWork.VSInvocations,"stencil does not remove vertex work","%s: %llu vertices",c.name,stats.VSInvocations);
        if(c.thousand<=10)check(stats.PSInvocations<fullWork.PSInvocations/20,"sparse stencil rejects pixel shading early","%s: %llu vs %llu",c.name,stats.PSInvocations,fullWork.PSInvocations);
        check(drawMs>0&&maskMs>0&&bothMs>0,"timestamps valid","%s",c.name);
        rel(ms);rel(mt);
    }
    printf("Excludes game draw submission, visibility, resource copies, repair-mask derivation, lighting and post-processing. The mask-marking pass is included separately.\n");
    ctx->ClearState();ctx->Flush();rel(vs);rel(full);rel(shade);rel(mark);rel(rtv);rel(dsv);rel(rt);rel(z);rel(off);rel(write);rel(test);rel(raster);rel(ctx);rel(dev);
    return finish("afw-sparse-bench");
}
