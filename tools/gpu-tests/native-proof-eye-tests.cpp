// gpu-test-sources: src/core/gfx/frame_inspector.cpp src/core/gfx/shader_bytecode.cpp src/game/bioshock1r/native_proof.cpp src/game/bioshock1r/native_proof_mask.cpp src/game/bioshock1r/native_proof_eye.cpp
// gpu-test-libs: build/third_party/minhook/RelWithDebInfo/minhook.x32.lib
// Independent synthetic stereo oracle plus the actual opt-in D3D draw hooks.
#include "gpu_test_util.h"
#include "core/gfx/frame_inspector.h"
#include "core/gfx/shader_bytecode.h"
#include "core/gfx/hud_capture.h"
#include "core/gfx/gfx_hud.h"
#include "game/bioshock1r/native_proof.h"
#include "game/bioshock1r/native_proof_eye.h"
#include "game/bioshock1r/native_proof_eye_math.h"
#include "game/bioshock1r/native_audit_com.h"
#include <d3d11_1.h>
#include <MinHook.h>
#include <imgui.h>
#include <array>
#include <limits>
#include <string>
#include <thread>
using bvr::b1r::native_audit::ComPtr;
namespace fi=bvr::frame_inspector;
namespace sb=bvr::shader_bytecode;
namespace np=bvr::b1r::native_proof;
namespace nd=np::detail;
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

// Synthetic large-world oracle: material/model/view data is independently built.
// Round each native-style product and pairwise sum, then store localEyePos from a
// separately rounded inverse model, as the native float and x87 paths do.
static double f32(double x) { volatile float y=static_cast<float>(x);return y; }
static Matrix float_multiply(const Matrix& a,const Matrix& b) {
    Matrix out{};
    for(unsigned i=0;i<4;++i)for(unsigned j=0;j<4;++j)
        out[i][j]=f32(f32(f32(a[i][0]*b[0][j])+f32(a[i][1]*b[1][j]))+
                        f32(f32(a[i][2]*b[2][j])+f32(a[i][3]*b[3][j])));
    return out;
}
static Matrix inverse_affine(const Matrix& model) {
    double a[3][6]{};
    for(unsigned i=0;i<3;++i)for(unsigned j=0;j<3;++j){a[i][j]=model[i][j];a[i][j+3]=i==j?1:0;}
    for(unsigned j=0;j<3;++j) {
        unsigned pivot=j;
        for(unsigned i=j+1;i<3;++i)if(std::fabs(a[i][j])>std::fabs(a[pivot][j]))pivot=i;
        for(unsigned k=0;k<6;++k)std::swap(a[j][k],a[pivot][k]);
        const double d=a[j][j];for(double& v:a[j])v/=d;
        for(unsigned i=0;i<3;++i)if(i!=j){const double m=a[i][j];for(unsigned k=0;k<6;++k)a[i][k]-=m*a[j][k];}
    }
    Matrix inverse{};inverse[3][3]=1;
    for(unsigned i=0;i<3;++i)for(unsigned j=0;j<3;++j)inverse[i][j]=f32(a[i][j+3]);
    for(unsigned j=0;j<3;++j) {
        for(unsigned k=0;k<3;++k)inverse[3][j]-=model[3][k]*inverse[k][j];
        inverse[3][j]=f32(inverse[3][j]);
    }
    return inverse;
}
static Constants float_scene(unsigned index,double base,double stereoShift,float worldEye[3]) {
    const double a=.03125*index,c=std::cos(a),s=std::sin(a),cp=std::cos(.013*index),sp=std::sin(.013*index);
    const double r[3]={c,0,-s},u[3]={s*sp,cp,c*sp},f[3]={s*cp,-sp,c*cp};
    const double t[3]={-base+.137*index,311.25,base*.25+.09375*index},offset[3]={.5,-.25,.125};
    double camera[3];for(unsigned i=0;i<3;++i)camera[i]=f32(f32(t[i]+offset[i])+stereoShift*r[i]);
    for(unsigned i=0;i<3;++i)worldEye[i]=float(camera[i]);
    Matrix model={{{f32(.75*c),f32(-.75*s),0,0},{f32(1.25*s),f32(1.25*c),0,0},{0,0,2,0},
                   {f32(t[0]),f32(t[1]),f32(t[2]),1}}};
    Matrix view{};view[3][3]=1;
    for(unsigned i=0;i<3;++i){view[i][0]=f32(r[i]);view[i][1]=f32(u[i]);view[i][2]=f32(f[i]);}
    for(unsigned j=0;j<3;++j) {
        for(unsigned i=0;i<3;++i)view[3][j]-=camera[i]*(j==0?r[i]:j==1?u[i]:f[i]);
        view[3][j]=f32(view[3][j]);
    }
    const double px=f32(.7+.01*index),py=f32(1.1);
    Matrix projection={{{px,0,0,0},{0,py,0,0},{0,0,1.25,1},{0,0,-1.25,0}}};
    const auto wvp=float_multiply(model,float_multiply(view,projection)),inverse=inverse_affine(model);
    Constants out=constants(camera,r,u,f,0,1,1,1,px,py);
    for(unsigned i=0;i<4;++i)for(unsigned j=0;j<4;++j)out.wvp[j*4+i]=float(wvp[i][j]);
    for(unsigned j=0;j<3;++j) {
        double eye=inverse[3][j];for(unsigned i=0;i<3;++i)eye+=camera[i]*inverse[i][j];
        out.eye[j]=float(eye);
    }
    return out;
}
static void large_world_checks() {
    bool valid=true,zeroExact=true;unsigned oldRefused=0,cases=0;
    double worstOrigin=0,worstTargetClip=0,worstTargetEye=0;
    for(double base:{0.,32768.,50000.})for(unsigned i=0;i<96;++i)for(float displacement:{-6.4f,6.4f}) {
        float world[3],targetWorld[3];const auto source=float_scene(i,base,0,world);
        const auto target=float_scene(i,base,displacement,targetWorld);
        nd::EyePatch old,p,zero;nd::EyeDiagnostic diagnostic;
        oldRefused+=!nd::make_eye_patch(source.wvp,source.eye,source.screen,source.clip,displacement,old);
        const bool ok=nd::make_eye_patch(source.wvp,source.eye,source.screen,source.clip,displacement,p,&diagnostic,world);
        valid&=ok;++cases;
        if(!ok)printf("large-world refusal i=%u base=%.0f reason=%s actual=%.9g limit=%.9g\n",i,base,diagnostic.reason,diagnostic.actual,diagnostic.limit);
        if(!ok)continue;
        worstOrigin=std::max(worstOrigin,p.originErrorUu);
        zeroExact&=nd::make_eye_patch(source.wvp,source.eye,source.screen,source.clip,0,zero,nullptr,world)&&
            !memcmp(zero.wvp,source.wvp,sizeof(zero.wvp))&&!memcmp(zero.eye,source.eye,sizeof(zero.eye));
        const unsigned rows[3]={0,4,12};const double scale[3]={2/source.screen[0],-2/source.screen[5],1};
        double clipDistance=0,eyeDistance=0;
        for(unsigned k=0;k<3;++k) {
            const unsigned row=rows[k];const double clip=(p.wvp[row+3]-double(target.wvp[row+3]))/scale[k];
            double eye=0;for(unsigned j=0;j<3;++j)eye+=source.wvp[row+j]*(double(p.eye[j])-target.eye[j])/scale[k];
            clipDistance+=clip*clip;eyeDistance+=eye*eye;
        }
        worstTargetClip=std::max(worstTargetClip,std::sqrt(clipDistance));
        worstTargetEye=std::max(worstTargetEye,std::sqrt(eyeDistance));
    }
    gt::check(valid&&oldRefused>500,"large-world precision reproduction",
        "%u synthetic cases, old local-only check refuses %u, max source origin %.9g uu",cases,oldRefused,worstOrigin);
    gt::check(zeroExact,"large-world zero shift exact","native rounded values remain bit-identical");
    gt::check(valid&&worstTargetClip<.02&&worstTargetEye<.02,"independent float target reconstruction",
        "two independent 0.01 uu budgets; max clip %.9g uu and local-eye projected error %.9g uu; not pixel equality",worstTargetClip,worstTargetEye);
    const double r[3]={1,0,0},u[3]={0,1,0},f[3]={0,0,1},o[3]={.25,0,0};
    const auto source=constants(o,r,u,f);nd::EyePatch patch{};nd::EyeDiagnostic diagnostic;
    const float farWorld[3]={-50000,300,12000},nearWorld[3]={.25f,0,0};
    auto wrong=source;wrong.eye[0]+=.125f;const auto before=patch;
    gt::check(!nd::make_eye_patch(wrong.wvp,wrong.eye,wrong.screen,wrong.clip,.5f,patch,&diagnostic,farWorld)&&
        !strcmp(diagnostic.reason,"localEyePos-accuracy-budget")&&!memcmp(&patch,&before,sizeof(patch)),
        "far-world wrong camera refused","large absolute coordinates cannot relax the 0.01 uu accuracy ceiling");
    wrong=source;wrong.eye[0]+=.005f;
    gt::check(!nd::make_eye_patch(wrong.wvp,wrong.eye,wrong.screen,wrong.clip,.5f,patch,&diagnostic,nearWorld)&&
        !strcmp(diagnostic.reason,"localEyePos-origin"),"near-origin precision remains strict","small mismatches do not inherit a global tolerance");
    wrong=source;wrong.eye[0]+=.008f;wrong.eye[1]+=.008f;
    gt::check(!nd::make_eye_patch(wrong.wvp,wrong.eye,wrong.screen,wrong.clip,.5f,patch,&diagnostic,farWorld)&&
        !strcmp(diagnostic.reason,"localEyePos-accuracy-budget"),"three-dimensional accuracy ceiling","two individually small axis errors still exceed the positional budget");
    float invalid[3]={0,std::numeric_limits<float>::infinity(),0};
    gt::check(!nd::make_eye_patch(source.wvp,source.eye,source.screen,source.clip,.5f,patch,&diagnostic,invalid)&&
        !strcmp(diagnostic.reason,"worldEye-nonfinite"),"nonfinite precision sample refused","unbounded world data cannot grant a larger budget");
    const double hugeCamera[3]={1e9,0,0};const float hugeWorld[3]={1e9f,0,0};
    const auto unrepresentable=constants(hugeCamera,r,u,f);
    gt::check(!nd::make_eye_patch(unrepresentable.wvp,unrepresentable.eye,unrepresentable.screen,unrepresentable.clip,
        .5f,patch,&diagnostic,hugeWorld)&&!strcmp(diagnostic.reason,"shift-float-resolution"),
        "sub-ULP stereo shift refused","a self-consistent origin cannot hide a translation lost to float storage");
}

static void math_checks() {
    const double r[3]={1,0,0},u[3]={0,1,0},f[3]={0,0,1},o[3]={.25,-.5,.125};
    const auto source=constants(o,r,u,f);nd::EyePatch p{};
    gt::check(nd::make_eye_patch(source.wvp,source.eye,source.screen,source.clip,0,p)&&
        !memcmp(p.wvp,source.wvp,sizeof(p.wvp))&&!memcmp(p.eye,source.eye,sizeof(p.eye)),"zero shift exact","all constant bits preserved");
    double worst=0;bool valid=true;
    for(int i=0;i<96;++i) {
        const double a=.03125*i,cy=std::cos(a),sy=std::sin(a),cp=std::cos(.013*i),sp=std::sin(.013*i),cr=std::cos(.009*i),sr=std::sin(.009*i);
        const double right[3]={cy*cr+sy*sp*sr,cp*sr,-sy*cr+cy*sp*sr};
        const double up[3]={-cy*sr+sy*sp*cr,cp*cr,sy*sr+cy*sp*cr},forward[3]={sy*cp,-sp,cy*cp};
        const double cam[3]={0.125*i,-0.03125*i,.25};
        const double shift=(i%2 ? -.125 : .25),target[3]={cam[0]+shift*right[0],cam[1]+shift*right[1],cam[2]+shift*right[2]};
        const auto x=constants(cam,right,up,forward,-.017*i,.5+double(i%3),1.25,2,.7+.01*i,1.1);
        const auto y=constants(target,right,up,forward,-.017*i,.5+double(i%3),1.25,2,.7+.01*i,1.1);
        valid &= nd::make_eye_patch(x.wvp,x.eye,x.screen,x.clip,float(shift),p);
        for(unsigned j=0;j<16;++j)worst=std::max(worst,std::fabs(double(p.wvp[j])-y.wvp[j]));
        for(unsigned j=0;j<3;++j)worst=std::max(worst,std::fabs(double(p.eye[j])-y.eye[j]));
    }
    gt::check(valid&&worst<3e-5,"independent camera/model oracle","96 rotated, scaled, nonuniform lens cases, max absolute delta %.9g",worst);
    nd::EyeDiagnostic diagnostic;
    const auto prior=p;
    gt::check(!nd::make_eye_patch(nullptr,source.eye,source.screen,source.clip,.5f,p,&diagnostic)&&
        !strcmp(diagnostic.reason,"missing-or-ambiguous-worldViewProj")&&!memcmp(&p,&prior,sizeof(p)),
        "missing input diagnosed","no patch produced from an unavailable snapshot");
    auto bad=source;bad.eye[0]+=1;
    gt::check(!nd::make_eye_patch(bad.wvp,bad.eye,bad.screen,bad.clip,.5f,p,&diagnostic)&&
        !strcmp(diagnostic.reason,"localEyePos-origin")&&std::fabs(diagnostic.actual)>diagnostic.limit,
        "wrong camera refused","origin residual %.9g exceeds allowance %.9g",diagnostic.actual,diagnostic.limit);
    bad=source;bad.wvp[0]=bad.wvp[1]=bad.wvp[2]=0;bad.wvp[3]=0;
    gt::check(!nd::make_eye_patch(bad.wvp,bad.eye,bad.screen,bad.clip,.5f,p),"singular model refused","no local stereo direction exists");
    bad=source;bad.screen[1]=.1f;
    gt::check(!nd::make_eye_patch(bad.wvp,bad.eye,bad.screen,bad.clip,.5f,p,&diagnostic)&&
        !strcmp(diagnostic.reason,"screenDataToCamera-shape")&&diagnostic.component==1&&diagnostic.actual==bad.screen[1],
        "unsupported lens refused","exact mismatching lens component is identified");
    bad=source;bad.clip[3]=1;
    gt::check(!nd::make_eye_patch(bad.wvp,bad.eye,bad.screen,bad.clip,.5f,p,&diagnostic)&&
        !strcmp(diagnostic.reason,"vertexClipPlane-nonzero")&&diagnostic.component==3&&diagnostic.actual==1,
        "clip plane refused","does not silently reuse a view-dependent clipping plane");
    bad=source;bad.wvp[0]=std::numeric_limits<float>::quiet_NaN();
    gt::check(!nd::make_eye_patch(bad.wvp,bad.eye,bad.screen,bad.clip,.5f,p)&&
        !nd::make_eye_patch(source.wvp,source.eye,source.screen,source.clip,129,p),"finite bounded shift","NaN and out-of-budget separation refused");
}
const char* kFields=R"(
cbuffer Globals : register(SLOT) {
 float4 guard[3]; float4 material; column_major float4x3 screenDataToCamera;
 float3 localEyePos; float padding; float4 vertexClipPlane; float4 screenTransform[2];
 column_major float4x4 worldViewProj; float4 tail;
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
return float4(material.rgb*light,material.a);}
)";
static std::string replace(std::string text,const char* before,const char* after) {
    size_t at=0;while((at=text.find(before,at))!=std::string::npos){text.replace(at,strlen(before),after);at+=strlen(after);}return text;
}
static ComPtr<ID3DBlob> shader(bool pixel,const char* variant="") {
    std::string code=replace(kFields,"SLOT",pixel?"b7":"b3");code+=pixel?kPs:kVs;
    if(!strcmp(variant,"unknown"))code=replace(code,"localEyePos","previousViewOrigin");
    if(!strcmp(variant,"row"))code=replace(code,"column_major float4x4","row_major float4x4");
    if(!strcmp(variant,"type"))code=replace(code,"float3 localEyePos; float padding;","float4 localEyePos;");
    if(!strcmp(variant,"unused_clip"))code=replace(code,"o.clipping=dot(float4(p,1),vertexClipPlane);","o.clipping=1;");
    return compile(code.c_str(),pixel?"ps_5_0":"vs_5_0");
}
// Own synthetic spotlight math with the game's reflected type SHAPE. No game
// bytecode, assembly or shader source is used by this regression fixture.
const char* kLocalLight=R"(
struct LocalLight { float4 light_vector; float invradius; float4 projection[3]; };
cbuffer Lighting : register(b5) {float4 lightGuard[2]; LocalLight lights[1]; float4 lightTail;};
float testLight(float3 p) {
 float4 h=float4(p,1);
 return dot(h,lights[0].projection[0])+.5*dot(h,lights[0].projection[1])+
 .25*dot(h,lights[0].projection[2])+dot(h,lights[0].light_vector)*lights[0].invradius;
}
)";
static ComPtr<ID3DBlob> light_shader(bool pixel,const char* variant="") {
    std::string light=kLocalLight;
    if(!strcmp(variant,"renamed"))light=replace(light,"projection","viewProjection");
    if(!strcmp(variant,"array"))light=replace(light,"lights[1]","lights[2]");
    if(!strcmp(variant,"matrix"))light=replace(light,"float4 projection[3]","row_major float3x4 projection");
    if(!strcmp(variant,"extra"))light=replace(light,"float4 projection[3];","float4 projection[3]; float4 viewOrigin;");
    std::string body=pixel?kPs:kVs;
    if(pixel)body=replace(body,"return float4(material.rgb*light,material.a);","return float4(material.rgb*(light+.001*testLight(i.p)),material.a);");
    else body=replace(body,"o.clipping=dot(","o.extra+=testLight(p);o.clipping=dot(");
    const auto code=replace(kFields,"SLOT",pixel?"b7":"b3")+light+body;
    return compile(code.c_str(),pixel?"ps_4_0":"vs_4_0");
}
struct Surface {
    ComPtr<ID3D11Texture2D> scratch,before,expected,result;
    nd::ProofSurface refs() const {return {scratch.Get(),before.Get(),expected.Get(),result.Get()};}
    bool make(DXGI_FORMAT format,UINT bind) {
        scratch.Attach(gt::make_tex(96,80,format,bind));if(!scratch)return false;
        D3D11_TEXTURE2D_DESC d{};scratch->GetDesc(&d);d.Usage=D3D11_USAGE_STAGING;d.BindFlags=0;d.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        return SUCCEEDED(gt::dev->CreateTexture2D(&d,nullptr,before.GetAddressOf()))&&
            SUCCEEDED(gt::dev->CreateTexture2D(&d,nullptr,expected.GetAddressOf()))&&SUCCEEDED(gt::dev->CreateTexture2D(&d,nullptr,result.GetAddressOf()));
    }
};
static Surface colour,depth;
static ComPtr<ID3D11Texture2D> liveColour,liveDepth;
static ComPtr<ID3D11RenderTargetView> scratchRtv,liveRtv;
static ComPtr<ID3D11DepthStencilView> scratchDsv,liveDsv;
static bool lastOk=false;
static float shift=.5f;
static float proofWorld[3]={.25f,0,0};
static np::EyeStats measured;
static bool probe_before(ID3D11DeviceContext* ctx,const fi::DrawArgs&) {
    ctx->CopyResource(colour.before.Get(),liveColour.Get());ctx->CopyResource(depth.before.Get(),liveDepth.Get());return true;
}
static void probe_after(ID3D11DeviceContext* ctx,const fi::DrawArgs& a) {
    ctx->CopyResource(colour.expected.Get(),liveColour.Get());ctx->CopyResource(depth.expected.Get(),liveDepth.Get());
    lastOk=nd::eye_run(ctx,a,colour.refs(),depth.refs(),scratchRtv.Get(),scratchDsv.Get(),shift,proofWorld,measured.tested+1,measured);
}
const fi::DrawReplayProbe probe{probe_before,probe_after,nullptr};
static void clear_live() {
    const float background[4]={.0625f,.125f,.25f,1};
    gt::ctx->OMSetRenderTargets(1,liveRtv.GetAddressOf(),liveDsv.Get());
    gt::ctx->ClearRenderTargetView(liveRtv.Get(),background);
    gt::ctx->ClearDepthStencilView(liveDsv.Get(),D3D11_CLEAR_DEPTH|D3D11_CLEAR_STENCIL,1,27);
}
static bool original_targets_unchanged() {
    ComPtr<ID3D11Texture2D> c,d;D3D11_TEXTURE2D_DESC desc{};colour.result->GetDesc(&desc);
    gt::dev->CreateTexture2D(&desc,nullptr,c.GetAddressOf());depth.result->GetDesc(&desc);gt::dev->CreateTexture2D(&desc,nullptr,d.GetAddressOf());
    gt::ctx->CopyResource(c.Get(),liveColour.Get());gt::ctx->CopyResource(d.Get(),liveDepth.Get());uint64_t cd=99,dd=99;
    return nd::compare_pixels(gt::ctx,c.Get(),colour.expected.Get(),cd)&&nd::compare_pixels(gt::ctx,d.Get(),depth.expected.Get(),dd)&&!cd&&!dd;
}
int main(int argc,char** argv) {
    const bool tags=argc>1&&!strcmp(argv[1],"--tagguards");
    math_checks();large_world_checks();if(!gt::make_device())return 1;
    const auto* base=reinterpret_cast<const uint8_t*>(GetModuleHandleW(nullptr));
    const auto* dos=reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt=reinterpret_cast<const IMAGE_NT_HEADERS*>(base+dos->e_lfanew);
    sb::configure(base,nt->OptionalHeader.SizeOfImage);fi::set_draw_replay_probe(&probe);
    if(MH_Initialize()!=MH_OK||!sb::install(gt::dev)||!fi::install(*reinterpret_cast<void***>(gt::ctx),gt::ctx))return 1;
    sb::bind_device(gt::dev);fi::set_draw_replay_probe(nullptr);
    const auto vsCode=shader(false),psCode=shader(true);if(!vsCode||!psCode)return 1;
    ComPtr<ID3D11VertexShader> vs;ComPtr<ID3D11PixelShader> ps;ComPtr<ID3D11InputLayout> layout;
    gt::dev->CreateVertexShader(vsCode->GetBufferPointer(),vsCode->GetBufferSize(),nullptr,vs.GetAddressOf());
    gt::dev->CreatePixelShader(psCode->GetBufferPointer(),psCode->GetBufferSize(),nullptr,ps.GetAddressOf());
    const D3D11_INPUT_ELEMENT_DESC element{"POSITION",0,DXGI_FORMAT_R32G32B32_FLOAT,0,0,D3D11_INPUT_PER_VERTEX_DATA,0};
    gt::dev->CreateInputLayout(&element,1,vsCode->GetBufferPointer(),vsCode->GetBufferSize(),layout.GetAddressOf());
    const float vertices[]={-2,-1.5f,4, 2,-1.5f,4, -2,1.5f,4};const uint16_t indices[]={0,1,2};
    ComPtr<ID3D11Buffer> vb,ib,cb,extra;
    D3D11_BUFFER_DESC bd{};bd.Usage=D3D11_USAGE_DYNAMIC;bd.CPUAccessFlags=D3D11_CPU_ACCESS_WRITE;bd.ByteWidth=sizeof(vertices);bd.BindFlags=D3D11_BIND_VERTEX_BUFFER;
    gt::dev->CreateBuffer(&bd,nullptr,vb.GetAddressOf());bd.ByteWidth=sizeof(indices);bd.BindFlags=D3D11_BIND_INDEX_BUFFER;gt::dev->CreateBuffer(&bd,nullptr,ib.GetAddressOf());
    bd.ByteWidth=sizeof(Constants);bd.BindFlags=D3D11_BIND_CONSTANT_BUFFER;gt::dev->CreateBuffer(&bd,nullptr,cb.GetAddressOf());bd.ByteWidth=512;gt::dev->CreateBuffer(&bd,nullptr,extra.GetAddressOf());
    if(!upload(vb.Get(),vertices,sizeof(vertices))||!upload(ib.Get(),indices,sizeof(indices)))return 1;
    if(!colour.make(DXGI_FORMAT_R11G11B10_FLOAT,D3D11_BIND_RENDER_TARGET)||!depth.make(DXGI_FORMAT_R24G8_TYPELESS,D3D11_BIND_DEPTH_STENCIL))return 1;
    liveColour.Attach(gt::make_tex(96,80,DXGI_FORMAT_R11G11B10_FLOAT,D3D11_BIND_RENDER_TARGET));liveDepth.Attach(gt::make_tex(96,80,DXGI_FORMAT_R24G8_TYPELESS,D3D11_BIND_DEPTH_STENCIL));
    gt::dev->CreateRenderTargetView(colour.scratch.Get(),nullptr,scratchRtv.GetAddressOf());gt::dev->CreateRenderTargetView(liveColour.Get(),nullptr,liveRtv.GetAddressOf());
    D3D11_DEPTH_STENCIL_VIEW_DESC dv{};dv.Format=DXGI_FORMAT_D24_UNORM_S8_UINT;dv.ViewDimension=D3D11_DSV_DIMENSION_TEXTURE2D;
    gt::dev->CreateDepthStencilView(depth.scratch.Get(),&dv,scratchDsv.GetAddressOf());gt::dev->CreateDepthStencilView(liveDepth.Get(),&dv,liveDsv.GetAddressOf());
    ComPtr<ID3D11DepthStencilState> ds;D3D11_DEPTH_STENCIL_DESC dd{};dd.DepthEnable=TRUE;dd.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ALL;dd.DepthFunc=D3D11_COMPARISON_LESS_EQUAL;
    dd.StencilEnable=TRUE;dd.StencilReadMask=255;dd.StencilWriteMask=255;
    dd.FrontFace={D3D11_STENCIL_OP_KEEP,D3D11_STENCIL_OP_KEEP,D3D11_STENCIL_OP_REPLACE,D3D11_COMPARISON_ALWAYS};dd.BackFace=dd.FrontFace;
    gt::dev->CreateDepthStencilState(&dd,ds.GetAddressOf());gt::ctx->OMSetDepthStencilState(ds.Get(),91);
    ComPtr<ID3D11DepthStencilState> noDepthWrite;dd.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ZERO;gt::dev->CreateDepthStencilState(&dd,noDepthWrite.GetAddressOf());
    ComPtr<ID3D11BlendState> additive,alpha;D3D11_BLEND_DESC blend{};auto& blendRt=blend.RenderTarget[0];
    blendRt.BlendEnable=TRUE;blendRt.SrcBlend=blendRt.DestBlend=D3D11_BLEND_ONE;blendRt.BlendOp=blendRt.BlendOpAlpha=D3D11_BLEND_OP_ADD;
    blendRt.SrcBlendAlpha=blendRt.DestBlendAlpha=D3D11_BLEND_ONE;blendRt.RenderTargetWriteMask=D3D11_COLOR_WRITE_ENABLE_ALL;
    gt::dev->CreateBlendState(&blend,additive.GetAddressOf());blendRt.SrcBlend=D3D11_BLEND_SRC_ALPHA;blendRt.DestBlend=D3D11_BLEND_INV_SRC_ALPHA;
    gt::dev->CreateBlendState(&blend,alpha.GetAddressOf());
    ComPtr<ID3D11RasterizerState> raster;D3D11_RASTERIZER_DESC rd{};rd.FillMode=D3D11_FILL_SOLID;rd.CullMode=D3D11_CULL_NONE;rd.DepthClipEnable=TRUE;
    gt::dev->CreateRasterizerState(&rd,raster.GetAddressOf());gt::ctx->RSSetState(raster.Get());
    const D3D11_VIEWPORT viewport{0,0,96,80,0,.01f};gt::ctx->RSSetViewports(1,&viewport);
    gt::ctx->VSSetShader(vs.Get(),nullptr,0);gt::ctx->PSSetShader(ps.Get(),nullptr,0);gt::ctx->IASetInputLayout(layout.Get());
    gt::ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);const UINT stride=12,offset=0;
    gt::ctx->IASetVertexBuffers(0,1,vb.GetAddressOf(),&stride,&offset);gt::ctx->IASetIndexBuffer(ib.Get(),DXGI_FORMAT_R16_UINT,0);
    gt::ctx->VSSetConstantBuffers(3,1,cb.GetAddressOf());gt::ctx->PSSetConstantBuffers(7,1,cb.GetAddressOf());
    gt::ctx->VSSetConstantBuffers(12,1,extra.GetAddressOf());gt::ctx->PSSetConstantBuffers(13,1,extra.GetAddressOf());
    const double r[3]={1,0,0},u[3]={0,1,0},f[3]={0,0,1},camera[3]={.25,0,0};
    auto source=constants(camera,r,u,f);if(!upload(cb.Get(),&source,sizeof(source)))return 1;clear_live();fi::on_present(nullptr);
    if(tags) {
        np::request_eye();np::note_eye(-1,.5f,proofWorld);fi::on_present(nullptr); // tag belongs to the prior interval
        gt::ctx->Draw(3,0);gt::check(np::stats().skipped==1,"old eye tag refused","no camera record from this Present window");
        std::thread other([]{np::note_eye(-1,.5f,proofWorld);});other.join();gt::ctx->Draw(3,0);
        gt::check(np::stats().skipped==2,"foreign thread tag refused","another thread cannot donate a camera identity");
        np::note_eye(0,.5f,proofWorld);gt::ctx->Draw(3,0);gt::check(np::stats().skipped==3,"mono camera refused","no stereo sign at a menu boundary");
        np::note_eye(-1,std::numeric_limits<float>::quiet_NaN(),proofWorld);gt::ctx->Draw(3,0);
        gt::check(np::stats().skipped==4,"invalid separation refused","no derived constant from NaN camera data");
        np::note_eye(-1,.5f,nullptr);gt::ctx->Draw(3,0);
        gt::check(np::stats().skipped==5,"missing world position refused","no numerical budget from an absent camera");
        const float invalidPosition[3]={0,std::numeric_limits<float>::quiet_NaN(),0};
        np::note_eye(-1,.5f,invalidPosition);gt::ctx->Draw(3,0);
        gt::check(np::stats().skipped==6,"invalid world position refused","no numerical budget from NaN position");
        fi::on_present(nullptr);gt::check(np::stats().finished&&np::stats().failed&&!np::stats().tested,"incomplete cannot pass","no replay was allowed");
    } else {
        fi::set_draw_replay_probe(&probe);
        for(int i=0;i<8;++i) {
            shift=i%2?-.5f:.5f;
            const double angle=i>=4?1.5707963267948966:0,sx=i>=4?.5:1,sy=i>=4?2:1,px=i>=6?2:1,py=i>=6?.5:1;
            source=constants(camera,r,u,f,angle,sx,sy,1,px,py);source.material[0]=.125f*(i+1);
            gt::ctx->OMSetBlendState(i<3?nullptr:i<6?additive.Get():alpha.Get(),nullptr,~0u);
            gt::ctx->OMSetDepthStencilState(i<3?ds.Get():noDepthWrite.Get(),91);if(!upload(cb.Get(),&source,sizeof(source)))return 1;clear_live();
            switch(i%4){case 0:gt::ctx->Draw(3,0);break;case 1:gt::ctx->DrawIndexed(3,0,0);break;case 2:gt::ctx->DrawInstanced(3,1,0,0);break;case 3:gt::ctx->DrawIndexedInstanced(3,1,0,0,0);break;}
            ComPtr<ID3D11RenderTargetView> savedRt;ComPtr<ID3D11DepthStencilView> savedDs;
            gt::ctx->OMGetRenderTargets(1,savedRt.GetAddressOf(),savedDs.GetAddressOf());
            gt::check(lastOk&&original_targets_unchanged()&&savedRt.Get()==liveRtv.Get()&&savedDs.Get()==liveDsv.Get(),"private clone/target isolation","draw kind=%d, direction=%g",i%4,shift);
            const double targetCamera[3]={camera[0]+shift,0,0};auto expected=constants(targetCamera,r,u,f,angle,sx,sy,1,px,py);memcpy(expected.material,source.material,sizeof(source.material));
            // Reference rendered by a complete target camera, not by stereo-patching source constants.
            {fi::ScopedSuppress suppress;gt::ctx->OMSetRenderTargets(0,nullptr,nullptr);gt::ctx->CopyResource(colour.scratch.Get(),colour.before.Get());gt::ctx->CopyResource(depth.scratch.Get(),depth.before.Get());
                upload(cb.Get(),&expected,sizeof(expected));gt::ctx->OMSetRenderTargets(1,scratchRtv.GetAddressOf(),scratchDsv.Get());gt::ctx->Draw(3,0);
                gt::ctx->OMSetRenderTargets(0,nullptr,nullptr);gt::ctx->CopyResource(colour.expected.Get(),colour.scratch.Get());gt::ctx->CopyResource(depth.expected.Get(),depth.scratch.Get());}
            uint64_t cd=99,ddiff=99;bool equality=nd::compare_pixels(gt::ctx,colour.expected.Get(),colour.result.Get(),cd)&&nd::compare_pixels(gt::ctx,depth.expected.Get(),depth.result.Get(),ddiff);
            gt::check(equality&&!cd&&!ddiff,"independent target pixels","native materials include eye-dependent specular, colour=%llu depth=%llu",cd,ddiff);
            upload(cb.Get(),&source,sizeof(source));
        }
        gt::check(measured.tested==8&&measured.changed==8&&!measured.originalConstantDiff,"nonempty target replay","8 translated draws, original constants unchanged");
        ComPtr<ID3D11Buffer> a,b,c,d;
        gt::ctx->VSGetConstantBuffers(3,1,a.GetAddressOf());gt::ctx->PSGetConstantBuffers(7,1,b.GetAddressOf());gt::ctx->VSGetConstantBuffers(12,1,c.GetAddressOf());gt::ctx->PSGetConstantBuffers(13,1,d.GetAddressOf());
        gt::check(a.Get()==cb.Get()&&b.Get()==cb.Get()&&c.Get()==extra.Get()&&d.Get()==extra.Get(),"all CB slots restored","nonzero shader slots and unrelated bindings survive");
        // Negative control: a clip-x-only change misses view-dependent lighting.
        nd::EyePatch patch;nd::make_eye_patch(source.wvp,source.eye,source.screen,source.clip,shift,patch);
        auto incomplete=source;memcpy(incomplete.wvp,patch.wvp,sizeof(patch.wvp));
        {fi::ScopedSuppress suppress;gt::ctx->CopyResource(colour.scratch.Get(),colour.before.Get());gt::ctx->CopyResource(depth.scratch.Get(),depth.before.Get());upload(cb.Get(),&incomplete,sizeof(incomplete));
            gt::ctx->OMSetRenderTargets(1,scratchRtv.GetAddressOf(),scratchDsv.Get());gt::ctx->Draw(3,0);gt::ctx->OMSetRenderTargets(0,nullptr,nullptr);gt::ctx->CopyResource(colour.expected.Get(),colour.scratch.Get());}
        uint64_t wrong=0;const bool wrongRead=nd::compare_pixels(gt::ctx,colour.expected.Get(),colour.result.Get(),wrong);
        gt::check(wrongRead&&wrong>20,"clip-only negative control","omitting local eye changes %llu pixels",wrong);
        for(const char* variant : {"unknown","row","type","unused_clip"}) {
            const auto code=shader(false,variant);ComPtr<ID3D11VertexShader> bad;gt::dev->CreateVertexShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,bad.GetAddressOf());
            gt::ctx->VSSetShader(bad.Get(),nullptr,0);upload(cb.Get(),&source,sizeof(source));clear_live();gt::ctx->Draw(3,0);
            gt::check(!lastOk&&original_targets_unchanged(),"unsupported reflection refused","variant=%s",variant);
        }
        gt::ctx->VSSetShader(vs.Get(),nullptr,0);
        for(unsigned badCase=0;badCase<3;++badCase) {
            auto bad=source;if(badCase==0)bad.eye[0]+=3;if(badCase==1)bad.screen[1]=.125f;if(badCase==2)bad.clip[3]=1;
            upload(cb.Get(),&bad,sizeof(bad));clear_live();gt::ctx->Draw(3,0);
            gt::check(!lastOk&&original_targets_unchanged(),"invalid native invariant refused","case=%u",badCase);
        }
        // Exact native-shaped light payload survives untouched across both eyes.
        ComPtr<ID3D11Buffer> lightBuffer;bd.ByteWidth=256;bd.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
        gt::dev->CreateBuffer(&bd,nullptr,lightBuffer.GetAddressOf());
        float lightData[64];for(unsigned i=0;i<64;++i)lightData[i]=float(i+31);
        const float payload[20]={.125f,.25f,.5f,1, .0625f,73,74,75,
            .25f,0,0,.125f, 0,.5f,0,.25f, 0,0,.75f,.5f};
        memcpy(lightData+8,payload,sizeof(payload));
        if(!upload(lightBuffer.Get(),lightData,sizeof(lightData)))return 1;
        gt::ctx->VSSetConstantBuffers(5,1,lightBuffer.GetAddressOf());
        const auto lightVsCode=light_shader(false);if(!lightVsCode)return 1;
        ComPtr<ID3D11VertexShader> lightVs;
        if(FAILED(gt::dev->CreateVertexShader(lightVsCode->GetBufferPointer(),lightVsCode->GetBufferSize(),nullptr,lightVs.GetAddressOf())))return 1;
        gt::ctx->VSSetShader(lightVs.Get(),nullptr,0);
        for(unsigned i=0;i<2;++i) {
            shift=i?-.5f:.5f;const double angle=1.5707963267948966;
            source=constants(camera,r,u,f,angle,.5,2,1,2,.5);
            upload(cb.Get(),&source,sizeof(source));clear_live();gt::ctx->Draw(3,0);
            ComPtr<ID3D11Buffer> restored;gt::ctx->VSGetConstantBuffers(5,1,restored.GetAddressOf());
            gt::check(lastOk&&original_targets_unchanged()&&!measured.originalConstantDiff&&restored.Get()==lightBuffer.Get(),
                "typed local-light isolation","direction=%g, full original buffers and lighting binding retained",shift);
            const double targetCamera[3]={camera[0]+shift,0,0};
            const auto expected=constants(targetCamera,r,u,f,angle,.5,2,1,2,.5);
            {fi::ScopedSuppress suppress;gt::ctx->OMSetRenderTargets(0,nullptr,nullptr);gt::ctx->CopyResource(colour.scratch.Get(),colour.before.Get());gt::ctx->CopyResource(depth.scratch.Get(),depth.before.Get());
                upload(cb.Get(),&expected,sizeof(expected));gt::ctx->OMSetRenderTargets(1,scratchRtv.GetAddressOf(),scratchDsv.Get());gt::ctx->Draw(3,0);
                gt::ctx->OMSetRenderTargets(0,nullptr,nullptr);gt::ctx->CopyResource(colour.expected.Get(),colour.scratch.Get());gt::ctx->CopyResource(depth.expected.Get(),depth.scratch.Get());}
            uint64_t cd=99,ddiff=99;const bool equality=nd::compare_pixels(gt::ctx,colour.expected.Get(),colour.result.Get(),cd)&&nd::compare_pixels(gt::ctx,depth.expected.Get(),depth.result.Get(),ddiff);
            gt::check(equality&&!cd&&!ddiff,"local-light target oracle","independent target camera colour=%llu depth=%llu",cd,ddiff);
        }
        for(const char* variant:{"renamed","array","matrix","extra"}) {
            const auto code=light_shader(false,variant);if(!code)return 1;
            ComPtr<ID3D11VertexShader> bad;gt::dev->CreateVertexShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,bad.GetAddressOf());
            gt::ctx->VSSetShader(bad.Get(),nullptr,0);upload(cb.Get(),&source,sizeof(source));clear_live();gt::ctx->Draw(3,0);
            gt::check(!lastOk&&original_targets_unchanged(),"unproven light layout refused","variant=%s",variant);
        }
        gt::ctx->VSSetShader(vs.Get(),nullptr,0);
        const auto badPsCode=light_shader(true);if(!badPsCode)return 1;ComPtr<ID3D11PixelShader> badPs;
        gt::dev->CreatePixelShader(badPsCode->GetBufferPointer(),badPsCode->GetBufferSize(),nullptr,badPs.GetAddressOf());
        gt::ctx->PSSetShader(badPs.Get(),nullptr,0);gt::ctx->PSSetConstantBuffers(5,1,lightBuffer.GetAddressOf());clear_live();gt::ctx->Draw(3,0);
        gt::check(!lastOk&&original_targets_unchanged(),"wrong light stage refused","the VS layout does not grant a PS exemption");
        gt::ctx->PSSetShader(ps.Get(),nullptr,0);
        ID3D11Buffer* unbound=nullptr;gt::ctx->VSSetConstantBuffers(5,1,&unbound);gt::ctx->PSSetConstantBuffers(5,1,&unbound);
        ComPtr<ID3D11DeviceContext1> ctx1;gt::ctx->QueryInterface(__uuidof(ID3D11DeviceContext1),reinterpret_cast<void**>(ctx1.GetAddressOf()));
        if(ctx1) {
            const UINT first=16,count=16;
            upload(cb.Get(),&source,sizeof(source));
            ctx1->VSSetConstantBuffers1(12,1,extra.GetAddressOf(),&first,&count);
            ctx1->PSSetConstantBuffers1(13,1,extra.GetAddressOf(),&first,&count);
            clear_live();gt::ctx->Draw(3,0);
            UINT vf=0,vc=0,pf=0,pc=0;ComPtr<ID3D11Buffer> restoredVs,restoredPs;
            ctx1->VSGetConstantBuffers1(12,1,restoredVs.GetAddressOf(),&vf,&vc);
            ctx1->PSGetConstantBuffers1(13,1,restoredPs.GetAddressOf(),&pf,&pc);
            gt::check(lastOk&&original_targets_unchanged()&&restoredVs.Get()==extra.Get()&&restoredPs.Get()==extra.Get()&&
                vf==first&&pf==first&&vc==count&&pc==count,"unrelated CB ranges restored","VS=%u/%u PS=%u/%u",vf,vc,pf,pc);
            gt::ctx->VSSetConstantBuffers(12,1,extra.GetAddressOf());gt::ctx->PSSetConstantBuffers(13,1,extra.GetAddressOf());
            ctx1->VSSetConstantBuffers1(3,1,cb.GetAddressOf(),&first,&count);clear_live();gt::ctx->Draw(3,0);
            UINT afterFirst=0,afterCount=0;ComPtr<ID3D11Buffer> bound;ctx1->VSGetConstantBuffers1(3,1,bound.GetAddressOf(),&afterFirst,&afterCount);
            gt::check(!lastOk&&bound.Get()==cb.Get()&&afterFirst==first&&afterCount==count,"ranged CB refused and retained","first=%u count=%u",afterFirst,afterCount);
        }
        gt::ctx->VSSetConstantBuffers(3,1,cb.GetAddressOf());
        const float farVertices[]={-2,-1.5f,2, 2,-1.5f,2, -2,1.5f,2};
        upload(vb.Get(),farVertices,sizeof(farVertices)); // doubled model Z stays within the independent far plane
        for(float direction:{-.5f,.5f}) {
            shift=direction;const auto farSource=float_scene(4,32768,0,proofWorld);
            upload(cb.Get(),&farSource,sizeof(farSource));clear_live();const auto prior=measured.tested,priorChanged=measured.changed;gt::ctx->Draw(3,0);
            gt::check(lastOk&&measured.tested==prior+1&&measured.changed==priorChanged+1&&original_targets_unchanged()&&!measured.originalConstantDiff,
                "large-world private GPU replay","direction=%g; nonempty shifted draw, exact rounded source clone, original buffers/targets survive",direction);
            auto wrong=farSource;wrong.eye[0]+=1;upload(cb.Get(),&wrong,sizeof(wrong));clear_live();gt::ctx->Draw(3,0);
            gt::check(!lastOk&&measured.tested==prior+1&&original_targets_unchanged(),
                "large-world mismatched GPU camera refused","direction=%g; no clone accepted after a camera error",direction);
        }
        upload(vb.Get(),vertices,sizeof(vertices));
        proofWorld[0]=.25f;proofWorld[1]=proofWorld[2]=0;
        upload(cb.Get(),&source,sizeof(source));
        fi::set_draw_replay_probe(nullptr);np::request_eye();fi::on_present(nullptr);np::note_eye(-1,.5f,proofWorld);
        for(unsigned i=0;i<4;++i){clear_live();gt::ctx->Draw(3,0);}
        fi::on_present(nullptr);const auto stats=np::stats();
        gt::check(stats.finished&&!stats.failed&&stats.tested==4&&stats.eyes.tested==4&&stats.eyes.changed==4&&!stats.originalDiff&&!stats.eyes.originalConstantDiff,
            "real-hook mode 3 lifecycle","tested=%u shifted=%u failed=%u",stats.tested,stats.eyes.changed,stats.failed);
        np::request_eye();np::note_eye(-1,.5f,proofWorld);gt::ctx->Draw(3,0);fi::on_present(nullptr);
        gt::check(np::stats().tested==4,"one-shot eye proof","cannot re-arm after completion");
        fflush(stdout);
        wchar_t exe[MAX_PATH]={};GetModuleFileNameW(nullptr,exe,MAX_PATH);wchar_t command[2*MAX_PATH];swprintf_s(command,L"\"%ls\" --tagguards",exe);
        STARTUPINFOW si{};si.cb=sizeof(si);si.dwFlags=STARTF_USESTDHANDLES;si.hStdInput=GetStdHandle(STD_INPUT_HANDLE);si.hStdOutput=GetStdHandle(STD_OUTPUT_HANDLE);si.hStdError=GetStdHandle(STD_ERROR_HANDLE);
        PROCESS_INFORMATION pi{};DWORD code=1;
        if(CreateProcessW(nullptr,command,nullptr,nullptr,TRUE,CREATE_NO_WINDOW,nullptr,nullptr,&si,&pi)) {
            if(WaitForSingleObject(pi.hProcess,60000)==WAIT_OBJECT_0)GetExitCodeProcess(pi.hProcess,&code);CloseHandle(pi.hThread);CloseHandle(pi.hProcess);
        }
        gt::check(code==0,"isolated eye-tag guards","fresh process exited %lu",code);
    }
    printf("native eye proof%s: %d pass, %d fail\n",tags?" tag guards":"",gt::g_pass,gt::g_fail);return gt::g_fail?1:0;
}
