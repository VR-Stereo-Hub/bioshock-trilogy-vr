// gpu-test-sources: src/core/gfx/frame_inspector.cpp src/core/gfx/shader_bytecode.cpp src/game/bioshock1r/native_proof.cpp src/game/bioshock1r/native_proof_mask.cpp src/game/bioshock1r/native_proof_eye.cpp src/game/bioshock1r/native_repair.cpp src/game/bioshock1r/temporal_gpu.cpp src/game/bioshock1r/afw_gpu.cpp
// gpu-test-libs: build/third_party/minhook/RelWithDebInfo/minhook.x32.lib
// Full synthetic foreground/HDR pipeline, through the actual production draw/resource hooks.
#include "gpu_test_util.h"
#include "core/gfx/frame_inspector.h"
#include "core/gfx/shader_bytecode.h"
#include "core/gfx/hud_capture.h"
#include "core/gfx/gfx_hud.h"
#include "game/bioshock1r/native_proof.h"
#include "game/bioshock1r/native_proof_eye.h"
#include "game/bioshock1r/native_proof_eye_math.h"
#include "game/bioshock1r/native_repair.h"
#include "game/bioshock1r/native_audit_com.h"
#include <d3d11_1.h>
#include <MinHook.h>
#include <imgui.h>
#include <array>
#include <limits>
#include <string>
#include <thread>
#include <algorithm>
using bvr::b1r::native_audit::ComPtr;
namespace fi=bvr::frame_inspector;
namespace sb=bvr::shader_bytecode;
namespace np=bvr::b1r::native_proof;
namespace nd=np::detail;
namespace nr=bvr::b1r::native_repair;
namespace tg=bvr::b1r::tgpu;
namespace af=bvr::b1r::afw;
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

struct Constants {
    float guard[12], material[4], screen[12], eye[3], padding, clip[4], screenTransform[8], wvp[16], tail[4];
};
static_assert(sizeof(Constants)==256);
using Matrix=std::array<std::array<double,4>,4>;
static Matrix multiply(const Matrix& a,const Matrix& b) {
    Matrix o{}; for(unsigned i=0;i<4;++i)for(unsigned j=0;j<4;++j)for(unsigned k=0;k<4;++k)o[i][j]+=a[i][k]*b[k][j];return o;
}
// An independent scene-camera construction: affine model -> view -> perspective.
// Never uses make_eye_patch or its three-row solve to create the target oracle.
static Constants constants(const double camera[3],const double right[3],const double up[3],const double forward[3],
                           double angle=0,double sx=1,double sy=1,double sz=1,double px=1,double py=1) {
    const double c=std::cos(angle),s=std::sin(angle);
    Matrix model={{{sx*c,sx*s,0,0},{-sy*s,sy*c,0,0},{0,0,sz,0},{0,0,0,1}}};
    Matrix view{};view[3][3]=1;
    for(unsigned i=0;i<3;++i) {
        view[i][0]=right[i];view[i][1]=up[i];view[i][2]=forward[i];
        view[3][0]-=camera[i]*right[i];view[3][1]-=camera[i]*up[i];view[3][2]-=camera[i]*forward[i];
    }
    Matrix projection={{{px,0,0,0},{0,py,0,0},{0,0,1.25,1},{0,0,-1.25,0}}};
    const auto wvp=multiply(multiply(model,view),projection);
    Constants out{};
    for(unsigned i=0;i<4;++i)for(unsigned j=0;j<4;++j)out.wvp[j*4+i]=float(wvp[i][j]);
    out.eye[0]=float((camera[0]*c+camera[1]*s)/sx);
    out.eye[1]=float((-camera[0]*s+camera[1]*c)/sy);out.eye[2]=float(camera[2]/sz);
    out.screen[0]=float(2/px);out.screen[2]=float(-1/px);out.screen[5]=float(-2/py);out.screen[6]=float(1/py);out.screen[10]=1;
    const float screenTransform[8]={.5f,0,0,.5f,0,-.5f,0,.5f};memcpy(out.screenTransform,screenTransform,sizeof(screenTransform));
    out.material[0]=.5f;out.material[1]=.25f;out.material[2]=.125f;out.material[3]=.5f;
    for(unsigned i=0;i<12;++i)out.guard[i]=float(i+17);for(unsigned i=0;i<4;++i)out.tail[i]=float(i+91);
    out.padding=73;return out;
}

const char* kFields=R"(
cbuffer Globals : register(SLOT) {
 float4 guard[3];float4 material;column_major float4x3 screenDataToCamera;
 float3 localEyePos;float padding;float4 vertexClipPlane;float4 screenTransform[2];
 column_major float4x4 worldViewProj;float4 tail;
};
struct Varying {float4 position:SV_Position;float3 p:TEXCOORD0;float3 view:TEXCOORD1;float3 camera:TEXCOORD2;float extra:TEXCOORD3;float clipping:SV_ClipDistance;};
)";
const char* kVs=R"(
Varying main(float3 p:POSITION){Varying o;o.position=mul(float4(p,1),worldViewProj);
o.p=p;o.view=localEyePos.xyz-p;o.camera=mul(float4(o.position.xy,o.position.w,0),screenDataToCamera);
o.extra=dot(float4(p,1),screenTransform[0]+screenTransform[1]);o.clipping=dot(float4(p,1),vertexClipPlane);return o;}
)";
const char* kPs=R"(
float4 main(Varying i):SV_Target {float3 v=normalize(localEyePos.xyz-i.p);float3 w=normalize(i.view);
float light=.25+.5*pow(abs(v.z),4)+.125*abs(w.x)+.001*abs(i.camera.x)+.0005*abs(i.extra);
return float4(material.rgb*light*material.a,material.a);}
)";
const char* kFullVs=R"(
struct V{float4 p:SV_Position;float2 uv:TEXCOORD;};
V main(uint id:SV_VertexID){V v;v.uv=float2((id<<1)&2,id&2);v.p=float4(v.uv*float2(2,-2)+float2(-1,1),.9,1);return v;}
)";
const char* kWorldPs="float4 main():SV_Target{return float4(.0625,2,.25,1);}";
const char* kPostPs=R"(
Texture2D<float4> image:register(t0);Texture2D<float4> bloom:register(t1);SamplerState lin:register(s0);
cbuffer Settings:register(b0){float4 settings;};
float4 main(float4 p:SV_Position,float2 uv:TEXCOORD):SV_Target {
 float3 c=image.SampleLevel(lin,uv,0).rgb;
 if(settings.w<.5)c=max(c-.3,0);
 else if(settings.w<1.5) {
  c=c*.5+(image.SampleLevel(lin,uv+settings.xy,0).rgb+image.SampleLevel(lin,uv-settings.xy,0).rgb)*.25;
 } else {c+=bloom.SampleLevel(lin,uv,0).rgb*.125;c=c/(1+c);}
 return float4(c,1);
})";
static std::string with_slot(const char* fields,const char* slot) {
    std::string s=fields;const auto at=s.find("SLOT");s.replace(at,4,slot);return s;
}
constexpr UINT W=96,H=80;
struct Target {
    ComPtr<ID3D11Texture2D> tex;ComPtr<ID3D11RenderTargetView> rt;ComPtr<ID3D11ShaderResourceView> srv;
    bool make(UINT w,UINT h,DXGI_FORMAT fmt) {
        tex.Attach(gt::make_tex(w,h,fmt,D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE));
        return tex && SUCCEEDED(gt::dev->CreateRenderTargetView(tex.Get(),nullptr,rt.GetAddressOf())) &&
            SUCCEEDED(gt::dev->CreateShaderResourceView(tex.Get(),nullptr,srv.GetAddressOf()));
    }
};
struct Chain {
    Target scene,half[3],ldr,copy,back;
    ComPtr<ID3D11Texture2D> depth;ComPtr<ID3D11DepthStencilView> ds;
    bool make() {
        if(!scene.make(W,H,DXGI_FORMAT_R11G11B10_FLOAT))return false;
        for(auto& t:half)if(!t.make(W/2,H/2,DXGI_FORMAT_R11G11B10_FLOAT))return false;
        if(!ldr.make(W,H,DXGI_FORMAT_R8G8B8A8_UNORM)||!copy.make(W,H,DXGI_FORMAT_R8G8B8A8_UNORM)||!back.make(W,H,DXGI_FORMAT_R8G8B8A8_UNORM))return false;
        depth.Attach(gt::make_tex(W,H,DXGI_FORMAT_R24G8_TYPELESS,D3D11_BIND_DEPTH_STENCIL));
        D3D11_DEPTH_STENCIL_VIEW_DESC d{};d.Format=DXGI_FORMAT_D24_UNORM_S8_UINT;d.ViewDimension=D3D11_DSV_DIMENSION_TEXTURE2D;
        return depth && SUCCEEDED(gt::dev->CreateDepthStencilView(depth.Get(),&d,ds.GetAddressOf()));
    }
};
struct Pipeline {
    ComPtr<ID3D11VertexShader> fgVs,fullVs;ComPtr<ID3D11PixelShader> fgPs,worldPs,postPs;
    ComPtr<ID3D11InputLayout> layout;ComPtr<ID3D11Buffer> cb,vb,settings;
    ComPtr<ID3D11DepthStencilState> write,read,writeLess,readLess,post,stencil;
    ComPtr<ID3D11BlendState> add,alpha;
    ComPtr<ID3D11RasterizerState> raster;ComPtr<ID3D11SamplerState> sampler;
    ComPtr<ID3D11DeviceContext1> ctx1;ComPtr<ID3D11Buffer> unusedCb;Target unusedTexture;
    bool restored=true,unusedRestored=true,strictDepth=false;
    void bind_unused() {
        const UINT first=16,count=16;
        if(ctx1) {
            ctx1->VSSetConstantBuffers1(13,1,unusedCb.GetAddressOf(),&first,&count);
            ctx1->PSSetConstantBuffers1(13,1,unusedCb.GetAddressOf(),&first,&count);
        } else {
            gt::ctx->VSSetConstantBuffers(13,1,unusedCb.GetAddressOf());
            gt::ctx->PSSetConstantBuffers(13,1,unusedCb.GetAddressOf());
        }
        gt::ctx->VSSetShaderResources(127,1,unusedTexture.srv.GetAddressOf());
        gt::ctx->PSSetShaderResources(127,1,unusedTexture.srv.GetAddressOf());
    }
    bool unused_ok() {
        ComPtr<ID3D11Buffer> vs,ps;UINT vf=0,pf=0,vn=0,pn=0;
        if(ctx1) {
            ctx1->VSGetConstantBuffers1(13,1,vs.GetAddressOf(),&vf,&vn);
            ctx1->PSGetConstantBuffers1(13,1,ps.GetAddressOf(),&pf,&pn);
        } else {
            gt::ctx->VSGetConstantBuffers(13,1,vs.GetAddressOf());gt::ctx->PSGetConstantBuffers(13,1,ps.GetAddressOf());
        }
        ComPtr<ID3D11ShaderResourceView> v,p;
        gt::ctx->VSGetShaderResources(127,1,v.GetAddressOf());gt::ctx->PSGetShaderResources(127,1,p.GetAddressOf());
        return vs.Get()==unusedCb.Get() && ps.Get()==unusedCb.Get() &&
            (!ctx1 || (vf==16 && pf==16 && vn==16 && pn==16)) &&
            v.Get()==unusedTexture.srv.Get() && p.Get()==unusedTexture.srv.Get();
    }
    bool make() {
        const auto vs=compile((with_slot(kFields,"b3")+kVs).c_str(),"vs_5_0");
        const auto ps=compile((with_slot(kFields,"b7")+kPs).c_str(),"ps_5_0");
        const auto fv=compile(kFullVs,"vs_5_0"),wp=compile(kWorldPs,"ps_5_0"),pp=compile(kPostPs,"ps_5_0");
        if(!vs||!ps||!fv||!wp||!pp)return false;
        gt::dev->CreateVertexShader(vs->GetBufferPointer(),vs->GetBufferSize(),nullptr,fgVs.GetAddressOf());
        gt::dev->CreateVertexShader(fv->GetBufferPointer(),fv->GetBufferSize(),nullptr,fullVs.GetAddressOf());
        gt::dev->CreatePixelShader(ps->GetBufferPointer(),ps->GetBufferSize(),nullptr,fgPs.GetAddressOf());
        gt::dev->CreatePixelShader(wp->GetBufferPointer(),wp->GetBufferSize(),nullptr,worldPs.GetAddressOf());
        gt::dev->CreatePixelShader(pp->GetBufferPointer(),pp->GetBufferSize(),nullptr,postPs.GetAddressOf());
        D3D11_INPUT_ELEMENT_DESC e{"POSITION",0,DXGI_FORMAT_R32G32B32_FLOAT,0,0,D3D11_INPUT_PER_VERTEX_DATA,0};
        gt::dev->CreateInputLayout(&e,1,vs->GetBufferPointer(),vs->GetBufferSize(),layout.GetAddressOf());
        D3D11_BUFFER_DESC b{};b.ByteWidth=sizeof(Constants);b.Usage=D3D11_USAGE_DYNAMIC;b.CPUAccessFlags=D3D11_CPU_ACCESS_WRITE;b.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
        gt::dev->CreateBuffer(&b,nullptr,cb.GetAddressOf());b.ByteWidth=16;gt::dev->CreateBuffer(&b,nullptr,settings.GetAddressOf());
        b.ByteWidth=512;gt::dev->CreateBuffer(&b,nullptr,unusedCb.GetAddressOf());
        if(!unusedCb || !unusedTexture.make(1,1,DXGI_FORMAT_R8G8B8A8_UNORM))return false;
        gt::ctx->QueryInterface(__uuidof(ID3D11DeviceContext1),reinterpret_cast<void**>(ctx1.GetAddressOf()));
        b.ByteWidth=36;b.BindFlags=D3D11_BIND_VERTEX_BUFFER;gt::dev->CreateBuffer(&b,nullptr,vb.GetAddressOf());
        D3D11_DEPTH_STENCIL_DESC d{};d.DepthEnable=TRUE;d.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ALL;d.DepthFunc=D3D11_COMPARISON_LESS_EQUAL;
        gt::dev->CreateDepthStencilState(&d,write.GetAddressOf());d.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ZERO;
        gt::dev->CreateDepthStencilState(&d,read.GetAddressOf());d.DepthFunc=D3D11_COMPARISON_LESS;
        gt::dev->CreateDepthStencilState(&d,readLess.GetAddressOf());d.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ALL;
        gt::dev->CreateDepthStencilState(&d,writeLess.GetAddressOf());d.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ZERO;
        d.DepthFunc=D3D11_COMPARISON_ALWAYS;d.DepthEnable=FALSE;
        gt::dev->CreateDepthStencilState(&d,post.GetAddressOf());d.DepthEnable=TRUE;d.DepthFunc=D3D11_COMPARISON_LESS_EQUAL;d.StencilEnable=TRUE;
        d.StencilReadMask=d.StencilWriteMask=255;d.FrontFace={D3D11_STENCIL_OP_KEEP,D3D11_STENCIL_OP_KEEP,D3D11_STENCIL_OP_REPLACE,D3D11_COMPARISON_ALWAYS};d.BackFace=d.FrontFace;
        gt::dev->CreateDepthStencilState(&d,stencil.GetAddressOf());
        D3D11_BLEND_DESC bl{};auto& t=bl.RenderTarget[0];t.BlendEnable=TRUE;t.SrcBlend=t.DestBlend=D3D11_BLEND_ONE;
        t.SrcBlendAlpha=t.DestBlendAlpha=D3D11_BLEND_ONE;t.BlendOp=t.BlendOpAlpha=D3D11_BLEND_OP_ADD;t.RenderTargetWriteMask=15;
        gt::dev->CreateBlendState(&bl,add.GetAddressOf());t.DestBlend=D3D11_BLEND_INV_SRC_ALPHA;gt::dev->CreateBlendState(&bl,alpha.GetAddressOf());
        D3D11_RASTERIZER_DESC rd{};rd.FillMode=D3D11_FILL_SOLID;rd.CullMode=D3D11_CULL_NONE;rd.DepthClipEnable=TRUE;
        gt::dev->CreateRasterizerState(&rd,raster.GetAddressOf());
        D3D11_SAMPLER_DESC sm{};sm.Filter=D3D11_FILTER_MIN_MAG_MIP_LINEAR;sm.AddressU=sm.AddressV=sm.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;sm.MaxLOD=D3D11_FLOAT32_MAX;
        gt::dev->CreateSamplerState(&sm,sampler.GetAddressOf());return fgVs&&fullVs&&fgPs&&postPs&&cb&&vb&&settings&&layout&&write&&read&&writeLess&&readLess&&post&&stencil&&add&&alpha&&raster&&sampler;
    }
    Constants c(double cameraX) {
        const double camera[3]={cameraX,0,0},r[3]={1,0,0},u[3]={0,1,0},f[3]={0,0,1};return constants(camera,r,u,f);
    }
    void foreground(Chain& chain,double cam,unsigned kind,unsigned negative) {
        auto data=c(cam);data.material[0]=kind==1?.125f:1.5f;data.material[1]=.125f;data.material[2]=.0625f;data.material[3]=kind==2?.5f:1;
        if(negative==2)data.clip[3]=1;
        float vertices[9]={-1.5f,-1.25f,3, 1.5f,-1.25f,3, -1.5f,1.25f,3};
        if(kind==2)for(unsigned i=0;i<3;++i){vertices[i*3]+=.8f;vertices[i*3+2]=2.5f;}
        upload(vb.Get(),vertices,sizeof(vertices));
        if(negative==4) {fi::ScopedSuppress no;upload(cb.Get(),&data,sizeof(data));}
        else upload(cb.Get(),&data,sizeof(data));
        gt::ctx->IASetInputLayout(layout.Get());const UINT stride=12,offset=0;gt::ctx->IASetVertexBuffers(0,1,vb.GetAddressOf(),&stride,&offset);
        gt::ctx->VSSetShader(fgVs.Get(),nullptr,0);gt::ctx->PSSetShader(fgPs.Get(),nullptr,0);
        gt::ctx->VSSetConstantBuffers(3,1,cb.GetAddressOf());gt::ctx->PSSetConstantBuffers(7,1,cb.GetAddressOf());
        if(negative==5 && ctx1) {const UINT first=0,count=16;ctx1->VSSetConstantBuffers1(3,1,cb.GetAddressOf(),&first,&count);}
        gt::ctx->OMSetRenderTargets(1,chain.scene.rt.GetAddressOf(),chain.ds.Get());
        auto* nativeDepth=kind?(strictDepth && kind==2?readLess.Get():read.Get()):(strictDepth?writeLess.Get():write.Get());
        gt::ctx->OMSetDepthStencilState(negative==3?stencil.Get():nativeDepth,0);
        gt::ctx->OMSetBlendState(kind==1?add.Get():kind==2?alpha.Get():nullptr,nullptr,~0u);
        const D3D11_VIEWPORT vp{0,0,float(W),float(H),0,.01f};gt::ctx->RSSetViewports(1,&vp);
        gt::ctx->Draw(3,0);
        ComPtr<ID3D11RenderTargetView> rt;ComPtr<ID3D11DepthStencilView> ds;ComPtr<ID3D11Buffer> vcb,pcb;
        gt::ctx->OMGetRenderTargets(1,rt.GetAddressOf(),ds.GetAddressOf());gt::ctx->VSGetConstantBuffers(3,1,vcb.GetAddressOf());gt::ctx->PSGetConstantBuffers(7,1,pcb.GetAddressOf());
        restored&=rt.Get()==chain.scene.rt.Get()&&ds.Get()==chain.ds.Get()&&vcb.Get()==cb.Get()&&pcb.Get()==cb.Get();
        if(negative==5 && ctx1) {
            UINT first=99,count=99;ComPtr<ID3D11Buffer> ranged;
            ctx1->VSGetConstantBuffers1(3,1,ranged.GetAddressOf(),&first,&count);
            restored&=ranged.Get()==cb.Get() && first==0 && count==16;
        }
        unusedRestored&=unused_ok();
    }
    void post_pass(Target& target,ID3D11ShaderResourceView* source,ID3D11ShaderResourceView* bloom,float mode,float dx=0,float dy=0) {
        ID3D11ShaderResourceView* empty[2]{};gt::ctx->PSSetShaderResources(0,2,empty);
        gt::ctx->OMSetRenderTargets(1,target.rt.GetAddressOf(),nullptr);gt::ctx->OMSetDepthStencilState(post.Get(),0);
        gt::ctx->OMSetBlendState(nullptr,nullptr,~0u);gt::ctx->VSSetShader(fullVs.Get(),nullptr,0);gt::ctx->PSSetShader(postPs.Get(),nullptr,0);
        const float values[4]={dx,dy,0,mode};upload(settings.Get(),values,sizeof(values));gt::ctx->PSSetConstantBuffers(0,1,settings.GetAddressOf());
        ID3D11ShaderResourceView* inputs[2]={source,bloom};gt::ctx->PSSetShaderResources(0,2,inputs);gt::ctx->PSSetSamplers(0,1,sampler.GetAddressOf());
        D3D11_TEXTURE2D_DESC d{};target.tex->GetDesc(&d);const D3D11_VIEWPORT vp{0,0,float(d.Width),float(d.Height),0,1};gt::ctx->RSSetViewports(1,&vp);
        gt::ctx->Draw(3,0);unusedRestored&=unused_ok();
    }
    void render(Chain& chain,double cam,unsigned negative=0) {
        bind_unused();
        gt::ctx->RSSetState(raster.Get());gt::ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ID3D11ShaderResourceView* empty[2]{};gt::ctx->PSSetShaderResources(0,2,empty);
        const float zero[4]{};gt::ctx->ClearRenderTargetView(chain.scene.rt.Get(),zero);
        gt::ctx->ClearDepthStencilView(chain.ds.Get(),D3D11_CLEAR_DEPTH|D3D11_CLEAR_STENCIL,1,0);
        foreground(chain,cam,0,negative);
        gt::ctx->VSSetShader(fullVs.Get(),nullptr,0);gt::ctx->PSSetShader(worldPs.Get(),nullptr,0);gt::ctx->IASetInputLayout(nullptr);
        const D3D11_VIEWPORT vp{0,0,float(W),float(H),0,1};gt::ctx->RSSetViewports(1,&vp);
        gt::ctx->OMSetDepthStencilState(write.Get(),0);gt::ctx->OMSetBlendState(nullptr,nullptr,~0u);
        for(unsigned i=0;i<8;++i)gt::ctx->Draw(3,0);
        foreground(chain,cam,1,negative);foreground(chain,cam,2,negative);
        gt::ctx->IASetInputLayout(nullptr);
        post_pass(chain.half[0],chain.scene.srv.Get(),nullptr,0);
        post_pass(chain.half[1],chain.half[0].srv.Get(),nullptr,1,2.f/W,0);
        post_pass(chain.half[2],chain.half[1].srv.Get(),nullptr,1,0,2.f/H);
        // Reuse ping-pong targets and explicitly clear an already-mapped target.
        gt::ctx->ClearRenderTargetView(chain.half[1].rt.Get(),zero);
        post_pass(chain.half[1],chain.half[2].srv.Get(),nullptr,1,2.f/W,0);
        post_pass(chain.ldr,chain.scene.srv.Get(),chain.half[1].srv.Get(),2);
        gt::ctx->OMSetRenderTargets(0,nullptr,nullptr);gt::ctx->PSSetShaderResources(0,2,empty);
        gt::ctx->CopyResource(chain.copy.tex.Get(),chain.ldr.tex.Get());
        gt::ctx->CopySubresourceRegion(chain.back.tex.Get(),0,0,0,0,chain.copy.tex.Get(),0,nullptr);
    }
};
static tg::Camera camera_at(float x) {
    tg::Camera c;c.loc[0]=x;c.fwd[0]=0;c.fwd[2]=1;c.right[0]=1;c.right[1]=0;c.up[1]=1;c.up[2]=0;return c;
}
static void tag(uint64_t build,int sign,float x) {
    tg::Params depth;depth.nearUu=1;depth.farUu=5;depth.squeezeHi=.01f;
    af::Params warp;warp.grid=2;
    nr::note_camera(build,sign,camera_at(x),.5f,depth,warp);
}
static bool equal32(ID3D11Texture2D* a,ID3D11Texture2D* b,uint64_t& count) {
    D3D11_TEXTURE2D_DESC d{};a->GetDesc(&d);d.Usage=D3D11_USAGE_STAGING;d.BindFlags=d.MiscFlags=0;d.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> x,y;gt::dev->CreateTexture2D(&d,nullptr,x.GetAddressOf());gt::dev->CreateTexture2D(&d,nullptr,y.GetAddressOf());
    {fi::ScopedSuppress no;gt::ctx->CopyResource(x.Get(),a);gt::ctx->CopyResource(y.Get(),b);}
    return nd::compare_pixels(gt::ctx,x.Get(),y.Get(),count);
}
struct Difference {double mean=0,maximum=0;unsigned changed=0;};
static uint64_t image_hash(const std::vector<float>& pixels) {
    uint64_t h=14695981039346656037ull;
    for(float v:pixels) { h^=uint8_t(std::lround(v*255));h*=1099511628211ull; }
    return h;
}
static Difference difference(const std::vector<float>& a,const std::vector<float>& b) {
    Difference d;if(a.size()!=b.size()||a.empty()){d.mean=d.maximum=1e9;return d;}
    for(size_t i=0;i<a.size();i+=4)for(unsigned c=0;c<3;++c){const double e=std::fabs(a[i+c]-b[i+c]);d.mean+=e;d.maximum=std::max(d.maximum,e);d.changed+=e>0;}
    d.mean/=double(a.size()/4*3);return d;
}
int main() {
    if(!gt::make_device())return 1;
    const auto* base=reinterpret_cast<const uint8_t*>(GetModuleHandleW(nullptr));
    const auto* dos=reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt=reinterpret_cast<const IMAGE_NT_HEADERS*>(base+dos->e_lfanew);
    sb::configure(base,nt->OptionalHeader.SizeOfImage);nr::enable_for_test();
    if(MH_Initialize()!=MH_OK||!sb::install(gt::dev)||!fi::install(*reinterpret_cast<void***>(gt::ctx),gt::ctx))return 1;
    sb::bind_device(gt::dev);fi::on_present(nullptr);
    Pipeline pipeline;Chain actual,reference;
    Target destination;
    if(!pipeline.make()||!actual.make()||!reference.make()||!destination.make(W,H,DXGI_FORMAT_R8G8B8A8_UNORM))return 1;
    unsigned shown=0;
    for(unsigned frame=1;frame<=6;++frame) {
        pipeline.strictDepth=frame>=5;
        const int sign=frame%2?-1:1;const float sourceX=sign*.25f,targetX=-sourceX;
#ifndef BVR_NATIVE_BASELINE
        const auto beforeStream=nd::stream_eye_stats();const auto beforeRepair=nr::stats();
#endif
        tag(frame,sign,sourceX);pipeline.render(actual,sourceX);fi::on_present(nullptr);
        nr::finish_eye(actual.back.tex.Get(),frame,sign,1,1,true);
        bool selected=nr::copy_output(gt::ctx,destination.tex.Get(),frame,sign<0?0:1,1,1);
        printf("frame %u selected=%u complete=%llu refused=%llu\n",frame,selected,nr::stats().complete,nr::stats().refused);
        if(frame>1) {
            gt::check(selected,"full HDR repair reaches display copy","frame=%u sign=%d",frame,sign);
#ifndef BVR_NATIVE_BASELINE
            const auto stream=nd::stream_eye_stats();const auto repair=nr::stats();
            gt::check(stream.stateCreates-beforeStream.stateCreates==(frame==5?1u:0u) &&
                stream.constantSlotsRead-beforeStream.constantSlotsRead==6 &&
                stream.constantSlotsRestored-beforeStream.constantSlotsRestored==6 &&
                stream.replayDraws-beforeStream.replayDraws==8 &&
                repair.postSlotsRead-beforeRepair.postSlotsRead==10 && repair.postSlotsSet-beforeRepair.postSlotsSet==18,
                "replay reuses states and touches only required binding slots",
                "states=%llu CB read/restore=%llu/%llu SRV read/set=%llu/%llu GPU draws=%llu",
                stream.stateCreates-beforeStream.stateCreates,stream.constantSlotsRead-beforeStream.constantSlotsRead,
                stream.constantSlotsRestored-beforeStream.constantSlotsRestored,repair.postSlotsRead-beforeRepair.postSlotsRead,
                repair.postSlotsSet-beforeRepair.postSlotsSet,stream.replayDraws-beforeStream.replayDraws);
#endif
        }
        const auto sourcePixels=gt::readback(actual.back.tex.Get());
        {fi::ScopedSuppress no;pipeline.render(reference,sourceX);}
        const auto sourceDifference=difference(sourcePixels,gt::readback(reference.back.tex.Get()));
        uint64_t depthDifference=99;
        const bool depthEqual=equal32(actual.depth.Get(),reference.depth.Get(),depthDifference);
        gt::check(!sourceDifference.changed && depthEqual && !depthDifference,
            "original eye colour and depth stay exact","frame=%u colour channels=%u depth pixels=%llu",frame,sourceDifference.changed,depthDifference);
        if(selected) {
            ++shown;const auto repaired=gt::readback(destination.tex.Get());
            printf("IMAGE frame=%u hash=%016llx\n",frame,image_hash(repaired));
            {fi::ScopedSuppress no;pipeline.render(reference,targetX);}
            const auto target=gt::readback(reference.back.tex.Get());
            const auto error=difference(repaired,target),wrong=difference(sourcePixels,target);
            gt::check(error.mean<.0005 && error.maximum<.013,
                "native foreground and HDR chain match independent target render",
                "frame=%u mean=%.8f max=%.8f; unchanged-eye control mean=%.6f",frame,error.mean,error.maximum,wrong.mean);
            gt::check(wrong.mean>.005 && error.mean<wrong.mean*.1,"target-eye negative control detects parallax",
                "repair mean=%.8f source-eye mean=%.8f",error.mean,wrong.mean);
            gt::check(!nr::copy_output(gt::ctx,destination.tex.Get(),frame,sign<0?0:1,1,1),"output is consumed once","no stale second display copy");
        }
    }
    gt::check(shown>=3 && pipeline.restored,"continuous replay and native bindings","shown=%u fg=%llu post=%llu uploads=%llu copies=%llu restored=%u",
        shown,nr::stats().foreground,nr::stats().post,nr::stats().uploads,nr::stats().copies,pipeline.restored);
    for(unsigned test=1;test<=6;++test) {
        const uint64_t build=10+test;const auto prior=nr::stats();
        if(test!=1)tag(build,-1,-.25f);
        pipeline.render(actual,-.25,test==2?2:test==3?3:0);fi::on_present(nullptr);
        nr::finish_eye(actual.back.tex.Get(),test==6?build+1:build,-1,test==5?1.2f:1,1,test!=4);
        const bool accepted=nr::copy_output(gt::ctx,destination.tex.Get(),build,0,1,1);
        const char* reasons[]={"","old camera tag","nonzero clip plane","native stencil","temporal resolution mode","different submitted lens","wrong camera build"};
        const char* expected[]={"","missing fresh render-eye camera","vertexClipPlane-nonzero","unsupported foreground depth/stencil",
            "present eye/lens/resolution mismatch","present eye/lens/resolution mismatch","present eye/lens/resolution mismatch"};
        gt::check(!accepted && nr::stats().refused>prior.refused && !strcmp(nr::last_reason(),expected[test]),
            "unsupported frame retains AFW fallback","%s: %s",reasons[test],nr::last_reason());
    }
    tag(30,-1,-.25f);pipeline.render(actual,-.25f);fi::on_present(nullptr);
    nr::finish_eye(actual.back.tex.Get(),30,-1,1,1,true);
    fi::on_resource_reset(gt::ctx);
    gt::check(!nr::copy_output(gt::ctx,destination.tex.Get(),30,0,1,1),"resize invalidates retained output","old backbuffer cannot be reused");
    tag(31,-1,-.25f);pipeline.render(actual,-.25f);fi::on_present(nullptr);nr::finish_eye(actual.back.tex.Get(),31,-1,1,1,true);
    gt::check(nr::copy_output(gt::ctx,destination.tex.Get(),31,0,1,1),"fresh frame resumes after reset","resource mappings and constants are rebuilt");
    if(pipeline.ctx1) {
        tag(40,-1,-.25f);pipeline.render(actual,-.25f,5);fi::on_present(nullptr);
        nr::finish_eye(actual.back.tex.Get(),40,-1,1,1,true);
        gt::check(!nr::copy_output(gt::ctx,destination.tex.Get(),40,0,1,1) &&
            !strcmp(nr::last_reason(),"foreground shader/constant upload") && pipeline.restored,
            "used CB range refuses and retains exact binding","the 0/16 range is not silently widened");
    }
    gt::check(pipeline.unusedRestored,"unused shader bindings remain exact",
        "VS/PS b13 ranges and t127 survive foreground and post replay, including refusals");
    nr::set_active(false);nr::set_active(true);
    tag(32,-1,-.25f);pipeline.render(actual,-.25f,4);fi::on_present(nullptr);
    nr::finish_eye(actual.back.tex.Get(),32,-1,1,1,true);
    gt::check(!nr::copy_output(gt::ctx,destination.tex.Get(),32,0,1,1) &&
        !strcmp(nr::last_reason(),"foreground shader/constant upload"),
        "reactivation cannot reuse old constant snapshots","unobserved uploads require a fresh complete CPU snapshot");
    const auto before=nr::stats();nr::set_active(false);pipeline.render(actual,-.25f);fi::on_present(nullptr);
    gt::check(nr::stats().foreground==before.foreground&&nr::stats().uploads==before.uploads,
        "disarmed renderer is inert","no foreground replay or constant snapshots while AFW is off");
    // Optional submission timing, with readback outside the timed interval to
    // drain GPU work between samples. Small synthetic targets: not game FPS or GPU time.
    char benchmark[2]{};
    if(GetEnvironmentVariableA("BVR_NATIVE_BENCH",benchmark,sizeof(benchmark))) {
        nr::set_active(true);std::vector<double> samples;unsigned displayed=0;
        LARGE_INTEGER frequency;QueryPerformanceFrequency(&frequency);
        for(unsigned i=0;i<140;++i) {
            const uint64_t build=1000+i;const int sign=i%2?-1:1;const float x=sign*.25f;
            tag(build,sign,x);LARGE_INTEGER start,end;QueryPerformanceCounter(&start);
            pipeline.render(actual,x);fi::on_present(nullptr);nr::finish_eye(actual.back.tex.Get(),build,sign,1,1,true);
            const bool selected=nr::copy_output(gt::ctx,destination.tex.Get(),build,sign<0?0:1,1,1);
            QueryPerformanceCounter(&end);
            if(i>=20) { samples.push_back(1000.0*double(end.QuadPart-start.QuadPart)/frequency.QuadPart);displayed+=selected; }
            gt::readback(destination.tex.Get());
        }
        std::sort(samples.begin(),samples.end());
        printf("BENCH submissionMs median=%.6f p95=%.6f displayed=%u/%zu size=%ux%u\n",
            samples[samples.size()/2],samples[samples.size()*95/100],displayed,samples.size(),W,H);
        gt::check(displayed==samples.size(),"benchmark renders every measured eye","displayed=%u",displayed);
    }
    nr::shutdown();sb::stop();MH_DisableHook(MH_ALL_HOOKS);MH_Uninitialize();
    gt::ctx->ClearState();gt::ctx->Flush();
    return gt::finish("native foreground HDR stream");
}
