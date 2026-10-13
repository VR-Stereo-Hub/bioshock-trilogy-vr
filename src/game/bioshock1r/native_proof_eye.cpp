#include "game/bioshock1r/native_proof_eye.h"
#include "game/bioshock1r/native_proof_eye_math.h"
#include "game/bioshock1r/native_audit_com.h"
#include "core/gfx/frame_inspector.h"
#include "core/gfx/shader_bytecode.h"
#include "core/util/log.h"
#include <d3d11_1.h>
#include <d3d11shader.h>
#include <d3dcompiler.h>
#include <cctype>
#include <string>
#include <vector>
#include <memory>

namespace bvr::b1r::native_proof::detail {
namespace {
using native_audit::ComPtr;
constexpr UINT kSlots = D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT;
StreamStats g_streamStats;
enum FieldKind { kWvp, kEye, kScreen, kScreenTransform, kClip };
constexpr const char* kFieldNames[] = {"worldViewProj","localEyePos","screenDataToCamera","screenTransform","vertexClipPlane"};
UINT field_bit(const char* name) {
    if (name) for (UINT i = 0; i < 5; ++i) if (!std::strcmp(name,kFieldNames[i])) return 1u<<i;
    return 0;
}
struct Field { FieldKind kind; UINT offset, bytes; };
struct Slot {
    ComPtr<ID3D11Buffer> original, staging, same, shifted;
    UINT first = 0, count = 4096;
    std::vector<uint8_t> bytes;
    std::vector<Field> fields;
};
struct Bindings {
    ID3D11DeviceContext* ctx;
    ComPtr<ID3D11DeviceContext1> ctx1;
    ComPtr<ID3D11Device> dev;
    ComPtr<ID3D11RenderTargetView> rtv;
    ComPtr<ID3D11DepthStencilView> dsv;
    Slot slots[2][kSlots];
    bool touched = false, allConstants = true;
    explicit Bindings(ID3D11DeviceContext* c, bool all = true) : ctx(c), allConstants(all) {
        ctx->GetDevice(dev.GetAddressOf());
        ctx->QueryInterface(__uuidof(ID3D11DeviceContext1), reinterpret_cast<void**>(ctx1.GetAddressOf()));
        ctx->OMGetRenderTargets(1,rtv.GetAddressOf(),dsv.GetAddressOf());
        if (!allConstants) return; // Continuous replay captures only reflected CB slots.
        for (UINT stage = 0; stage < 2; ++stage) {
            ID3D11Buffer* buffers[kSlots]{}; UINT first[kSlots]{}, count[kSlots]{};
            if (ctx1) {
                if (stage) ctx1->PSGetConstantBuffers1(0,kSlots,buffers,first,count);
                else ctx1->VSGetConstantBuffers1(0,kSlots,buffers,first,count);
            } else {
                if (stage) ctx->PSGetConstantBuffers(0,kSlots,buffers);
                else ctx->VSGetConstantBuffers(0,kSlots,buffers);
                for (UINT& n : count) n = 4096;
            }
            for (UINT i = 0; i < kSlots; ++i) {
                slots[stage][i].original.Attach(buffers[i]);
                slots[stage][i].first = first[i]; slots[stage][i].count = count[i];
            }
        }
    }
    void restore() {
        if (!touched) return;
        if (!allConstants) {
            for (UINT stage=0;stage<2;++stage) for (UINT i=0;i<kSlots;++i) {
                const Slot& s=slots[stage][i];if (!s.same) continue;
                auto* buffer=s.original.Get();
                if (ctx1) {
                    if (stage) ctx1->PSSetConstantBuffers1(i,1,&buffer,&s.first,&s.count);
                    else ctx1->VSSetConstantBuffers1(i,1,&buffer,&s.first,&s.count);
                } else {
                    if (stage) ctx->PSSetConstantBuffers(i,1,&buffer);
                    else ctx->VSSetConstantBuffers(i,1,&buffer);
                }
                ++g_streamStats.constantSlotsRestored;
            }
        } else for (UINT stage = 0; stage < 2; ++stage) {
            ID3D11Buffer* buffers[kSlots]{}; UINT first[kSlots]{}, count[kSlots]{};
            for (UINT i = 0; i < kSlots; ++i) {
                const Slot& s = slots[stage][i]; buffers[i] = s.original.Get(); first[i] = s.first; count[i] = s.count;
            }
            if (ctx1) {
                if (stage) ctx1->PSSetConstantBuffers1(0,kSlots,buffers,first,count);
                else ctx1->VSSetConstantBuffers1(0,kSlots,buffers,first,count);
            } else {
                if (stage) ctx->PSSetConstantBuffers(0,kSlots,buffers);
                else ctx->VSSetConstantBuffers(0,kSlots,buffers);
            }
        }
        ID3D11RenderTargetView* target = rtv.Get(); ctx->OMSetRenderTargets(1,&target,dsv.Get());
        touched = false;
    }
    ~Bindings() { restore(); }
    void bind(bool targetEye) {
        touched = true;
        for (UINT stage = 0; stage < 2; ++stage) for (UINT i = 0; i < kSlots; ++i) {
            const Slot& s = slots[stage][i]; if (!s.same) continue;
            ID3D11Buffer* b = targetEye ? s.shifted.Get() : s.same.Get();
            if (stage) ctx->PSSetConstantBuffers(i,1,&b); else ctx->VSSetConstantBuffers(i,1,&b);
        }
    }
};
bool fail(uint32_t draw, const char* reason) {
    BVR_LOG("[native-proof] eye draw=%u INCOMPLETE reason=%s",draw,reason); return false;
}
bool camera_name(const char* name) {
    std::string lower = name ? name : "";
    for (char& c : lower) c = char(std::tolower(static_cast<unsigned char>(c)));
    const char* words[] = {"eye","view","camera","proj","screen","clip","fog"};
    for (const char* word : words) if (lower.find(word) != std::string::npos) return true;
    return false;
}
bool nested_camera(ID3D11ShaderReflectionType* type, std::string& path, UINT depth = 0) {
    D3D11_SHADER_TYPE_DESC desc{};
    if (!type || depth > 8 || FAILED(type->GetDesc(&desc))) { path="<invalid-type>"; return true; }
    for (UINT i = 0; i < desc.Members; ++i) {
        const char* name=type->GetMemberTypeName(i);
        if (!name) { path="<unnamed-member>"; return true; }
        if (camera_name(name)) { path=name; return true; }
        std::string child;
        if (nested_camera(type->GetMemberTypeByIndex(i),child,depth+1)) {
            path=std::string(name)+"."+child; return true;
        }
    }
    return false;
}
// The native per-pixel uploader constructs this one-light payload from the local
// model and the light: local vector, inverse radius, and model * light projection.
// Its "projection" is not the camera lens. This exact reflected layout may stay
// byte-identical for this source-resource diagnostic; no general name exemption.
// The shadow mask and native target-eye visibility are still unproven dependencies.
bool native_light(const D3D11_SHADER_VARIABLE_DESC& var, ID3D11ShaderReflectionType* type, UINT stage) {
    D3D11_SHADER_TYPE_DESC t{};
    if (stage || !type || FAILED(type->GetDesc(&t)) || t.Type!=D3D_SVT_VOID ||
        t.Class!=D3D_SVC_STRUCT || t.Rows!=1 || t.Columns!=17 || t.Elements!=1 ||
        t.Members!=3 || var.Size!=80 || var.StartOffset%16) return false;
    const char* names[]={"light_vector","invradius","projection"};
    const UINT offsets[]={0,16,32}, columns[]={4,1,4}, elements[]={0,0,3};
    for (UINT i=0;i<3;++i) {
        const char* name=type->GetMemberTypeName(i); auto* member=type->GetMemberTypeByIndex(i);
        D3D11_SHADER_TYPE_DESC m{};
        if (!name || std::strcmp(name,names[i]) || !member || FAILED(member->GetDesc(&m)) ||
            m.Type!=D3D_SVT_FLOAT || m.Class!=(i==1 ? D3D_SVC_SCALAR : D3D_SVC_VECTOR) ||
            m.Rows!=1 || m.Columns!=columns[i] || m.Elements!=elements[i] || m.Members ||
            m.Offset!=offsets[i]) return false;
    }
    return true;
}
bool classify(const D3D11_SHADER_VARIABLE_DESC& var, ID3D11ShaderReflectionType* type,
              std::vector<Field>& fields, UINT stage, uint32_t draw) {
    D3D11_SHADER_TYPE_DESC t{};
    if (!var.Name || !type || FAILED(type->GetDesc(&t))) return false;
    const std::string name = var.Name;
    FieldKind kind; D3D_SHADER_VARIABLE_CLASS cls = D3D_SVC_VECTOR;
    UINT rows = 1, columns = 4, elements = 0, bytes = 16;
    if (name == "worldViewProj") { kind=kWvp; cls=D3D_SVC_MATRIX_COLUMNS; rows=4; bytes=64; }
    else if (name == "localEyePos") { kind=kEye; columns=3; bytes=12; }
    else if (name == "screenDataToCamera") { kind=kScreen; cls=D3D_SVC_MATRIX_COLUMNS; rows=4; columns=3; bytes=48; }
    else if (name == "screenTransform") { kind=kScreenTransform; elements=2; bytes=32; }
    else if (name == "vertexClipPlane") kind=kClip;
    else {
        // These are copied from the material processor, independent of the view
        // (nr1_stereo_constants.py). Other camera-like names fail closed.
        if (name=="lights" && native_light(var,type,stage)) {
            BVR_LOG("[native-proof] eye draw=%u stage=%s typed local-light payload retained bytes=%u",draw,stage?"PS":"VS",var.Size);
            return true;
        }
        const bool fogMaterial = name == "fogColor" || name == "fogTransform";
        std::string child;
        const bool nested=nested_camera(type,child);
        if ((!fogMaterial && camera_name(var.Name)) || nested) {
            BVR_LOG("[native-proof] eye draw=%u stage=%s unsupported used field=%.96s member=%.192s type=%u class=%u rows=%u columns=%u elements=%u members=%u bytes=%u",
                draw,stage?"PS":"VS",var.Name,child.c_str(),t.Type,t.Class,t.Rows,t.Columns,t.Elements,t.Members,var.Size);
            return false;
        }
        return true;
    }
    if (t.Type != D3D_SVT_FLOAT || t.Class != cls || t.Rows != rows || t.Columns != columns ||
        t.Elements != elements || t.Members || var.Size != bytes || var.StartOffset % 4) {
        BVR_LOG("[native-proof] eye draw=%u field=%s unsupported type=%u class=%u rows=%u columns=%u elements=%u bytes=%u",
            draw,var.Name,t.Type,t.Class,t.Rows,t.Columns,t.Elements,var.Size); return false;
    }
    fields.push_back({kind,var.StartOffset,var.Size}); return true;
}
bool reflect(Bindings& b, ID3D11DeviceChild* shader, UINT stage, uint32_t draw) {
    std::vector<uint8_t> code;
    ComPtr<ID3D11ShaderReflection> r;
    if (!shader_bytecode::read(shader,code) || FAILED(D3DReflect(code.data(),code.size(),__uuidof(ID3D11ShaderReflection),
        reinterpret_cast<void**>(r.GetAddressOf())))) return false;
    D3D11_SHADER_DESC sd{}; if (FAILED(r->GetDesc(&sd))) return false;
    uint32_t checksum[4]{};
    if (code.size()>=20 && !std::memcmp(code.data(),"DXBC",4)) std::memcpy(checksum,code.data()+4,sizeof(checksum));
    BVR_LOG("[native-proof] eye layout draw=%u stage=%s shaderBytes=%zu checksum=%08x%08x%08x%08x",
        draw,stage?"PS":"VS",code.size(),checksum[0],checksum[1],checksum[2],checksum[3]);
    bool supported=true;
    for (UINT i = 0; i < sd.BoundResources; ++i) {
        D3D11_SHADER_INPUT_BIND_DESC binding{};
        if (FAILED(r->GetResourceBindingDesc(i,&binding))) return false;
        if (binding.Type != D3D_SIT_CBUFFER) continue;
        if (binding.BindPoint >= kSlots || binding.BindCount != 1) return false;
        Slot& slot = b.slots[stage][binding.BindPoint];
        auto* cb = r->GetConstantBufferByName(binding.Name); D3D11_SHADER_BUFFER_DESC cd{};
        if (!cb || FAILED(cb->GetDesc(&cd)) || cd.Type != D3D_CT_CBUFFER || !slot.original) return false;
        D3D11_BUFFER_DESC bd{}; slot.original->GetDesc(&bd);
        // Preserve explicit range state on refusal. The first proof deliberately
        // supports only the whole-buffer binding used by the observed game draws.
        if (slot.first != 0 || slot.count != 4096 || !bd.ByteWidth || bd.ByteWidth > 65536 ||
            bd.ByteWidth % 16 || bd.ByteWidth < cd.Size || bd.BindFlags != D3D11_BIND_CONSTANT_BUFFER ||
            !slot.bytes.empty()) return false;
        UINT declared=0,used=0;
        for (UINT j = 0; j < cd.Variables; ++j) {
            auto* v = cb->GetVariableByIndex(j); D3D11_SHADER_VARIABLE_DESC vd{};
            if (!v || FAILED(v->GetDesc(&vd)) || uint64_t(vd.StartOffset)+vd.Size > bd.ByteWidth) return false;
            declared|=field_bit(vd.Name);
            if (vd.uFlags & D3D_SVF_USED) {
                used|=field_bit(vd.Name);
                supported &= classify(vd,v->GetType(),slot.fields,stage,draw);
            }
        }
        BVR_LOG("[native-proof] eye layout draw=%u stage=%s cb=%u bufferBytes=%u reflectedBytes=%u cameraDeclared=%02x cameraUsed=%02x (WVP=1 eye=2 screen=4 transform=8 clip=16)",
            draw,stage?"PS":"VS",binding.BindPoint,bd.ByteWidth,cd.Size,declared,used);
        // Finish a bounded read-only snapshot even when a field refuses, so the
        // same launch can explain its camera invariant too. No clone can be bound
        // unless BOTH complete shader reflections and all later checks succeed.
        bd.Usage=D3D11_USAGE_STAGING; bd.BindFlags=0; bd.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        bd.MiscFlags=0; bd.StructureByteStride=0;
        if (FAILED(b.dev->CreateBuffer(&bd,nullptr,slot.staging.GetAddressOf()))) return false;
        b.ctx->CopyResource(slot.staging.Get(),slot.original.Get());
        D3D11_MAPPED_SUBRESOURCE mapped{};
        slot.bytes.resize(bd.ByteWidth);
        if (FAILED(b.ctx->Map(slot.staging.Get(),0,D3D11_MAP_READ,0,&mapped))) { slot.bytes.clear(); return false; }
        std::memcpy(slot.bytes.data(),mapped.pData,slot.bytes.size()); b.ctx->Unmap(slot.staging.Get(),0);
    }
    return supported;
}
const float* field(const Bindings& b, UINT stage, FieldKind kind) {
    const float* found = nullptr;
    for (const Slot& s : b.slots[stage]) for (const Field& f : s.fields) if (f.kind == kind) {
        if (found || uint64_t(f.offset)+f.bytes>s.bytes.size()) return nullptr;
        found = reinterpret_cast<const float*>(s.bytes.data()+f.offset);
    }
    return found;
}
void log_values(uint32_t draw, const char* name, const float* values, UINT count) {
    if (!values) { BVR_LOG("[native-proof] eye input draw=%u field=%s unavailable",draw,name); return; }
    for (UINT first=0;first<count;first+=4) {
        const UINT n=(std::min)(4u,count-first); float v[4]{};
        std::memcpy(v,values+first,n*sizeof(float));
        BVR_LOG("[native-proof] eye input draw=%u field=%s first=%u count=%u values=%.9g,%.9g,%.9g,%.9g",
            draw,name,first,n,v[0],v[1],v[2],v[3]);
    }
}
bool make_clones(Bindings& b, const EyePatch& patch, const float* wvp, const float* eye,
                 const float* screen, EyeStats& local, uint32_t draw) {
    const float screenTransform[8] = {0.5f,0,0,0.5f, 0,-0.5f,0,0.5f};
    for (auto& stage : b.slots) for (Slot& slot : stage) {
        if (slot.bytes.empty()) continue;
        auto target = slot.bytes;
        for (const Field& f : slot.fields) {
            const float* source = reinterpret_cast<const float*>(slot.bytes.data()+f.offset);
            const float* expected = f.kind == kWvp ? wvp : f.kind == kEye ? eye : f.kind == kScreen ? screen : screenTransform;
            if (f.kind == kClip) {
                for (UINT i = 0; i < 4; ++i) if (source[i] != 0.0f) {
                    BVR_LOG("[native-proof] eye draw=%u clone mismatch field=%s component=%u actual=%.9g expected=0",draw,kFieldNames[f.kind],i,source[i]);
                    return false;
                }
            } else for (UINT i = 0; i < f.bytes/4; ++i) if (!close_float(source[i],expected[i])) {
                BVR_LOG("[native-proof] eye draw=%u clone mismatch field=%s component=%u actual=%.9g expected=%.9g",draw,kFieldNames[f.kind],i,source[i],expected[i]);
                return false;
            }
            if (f.kind == kWvp || f.kind == kEye) {
                std::memcpy(target.data()+f.offset,f.kind == kWvp ? patch.wvp : patch.eye,f.bytes);
                ++local.fields;
            }
        }
        D3D11_BUFFER_DESC d{}; d.ByteWidth=static_cast<UINT>(slot.bytes.size()); d.Usage=D3D11_USAGE_IMMUTABLE;
        d.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
        D3D11_SUBRESOURCE_DATA data{}; data.pSysMem=slot.bytes.data();
        if (FAILED(b.dev->CreateBuffer(&d,&data,slot.same.GetAddressOf()))) return false;
        data.pSysMem=target.data();
        if (FAILED(b.dev->CreateBuffer(&d,&data,slot.shifted.GetAddressOf()))) return false;
        ++local.buffers; local.constantBytes += slot.bytes.size();
    }
    return true;
}
bool verify_buffers(Bindings& b, uint64_t& different) {
    different=0;
    for (auto& stage : b.slots) for (Slot& slot : stage) {
        if (slot.bytes.empty()) continue;
        b.ctx->CopyResource(slot.staging.Get(),slot.original.Get()); D3D11_MAPPED_SUBRESOURCE m{};
        if (FAILED(b.ctx->Map(slot.staging.Get(),0,D3D11_MAP_READ,0,&m))) return false;
        const auto* p=static_cast<const uint8_t*>(m.pData);
        for (size_t i=0;i<slot.bytes.size();++i) different += p[i]!=slot.bytes[i];
        b.ctx->Unmap(slot.staging.Get(),0);
    }
    return true;
}
bool render(Bindings& b, const frame_inspector::DrawArgs& a, const ProofSurface& c, const ProofSurface& d,
            ID3D11RenderTargetView* rt, ID3D11DepthStencilView* ds, bool target) {
    b.bind(target); b.ctx->OMSetRenderTargets(0,nullptr,nullptr);
    b.ctx->CopyResource(c.scratch,c.before); b.ctx->CopyResource(d.scratch,d.before);
    b.ctx->OMSetRenderTargets(1,&rt,ds);
    const bool issued=frame_inspector::replay_draw(b.ctx,a);
    b.ctx->OMSetRenderTargets(0,nullptr,nullptr);
    b.ctx->CopyResource(c.result,c.scratch); b.ctx->CopyResource(d.result,d.scratch);
    return issued;
}
} // namespace
bool eye_run(ID3D11DeviceContext* ctx, const frame_inspector::DrawArgs& a, const ProofSurface& c,
             const ProofSurface& d, ID3D11RenderTargetView* rt, ID3D11DepthStencilView* ds,
             float shiftRight, const float worldEye[3], uint32_t draw, EyeStats& total) {
    try {
        if (!worldEye) return fail(draw,"missing current world-eye precision sample");
        Bindings saved(ctx);
        ComPtr<ID3D11VertexShader> vs; ComPtr<ID3D11PixelShader> ps;
        ctx->VSGetShader(vs.GetAddressOf(),nullptr,nullptr); ctx->PSGetShader(ps.GetAddressOf(),nullptr,nullptr);
        const bool vsSupported=reflect(saved,vs.Get(),0,draw);
        const bool psSupported=reflect(saved,ps.Get(),1,draw);
        const float* wvp=field(saved,0,kWvp), *eye=field(saved,0,kEye), *screen=field(saved,0,kScreen), *clip=field(saved,0,kClip);
        EyePatch patch; EyeStats local; EyeDiagnostic diagnostic;
        const bool patchSupported=make_eye_patch(wvp,eye,screen,clip,shiftRight,patch,&diagnostic,worldEye);
        if (!vsSupported || !psSupported || !patchSupported) {
            BVR_LOG("[native-proof] eye draw=%u validation VS=%u PS=%u camera=%u shiftRight=%.9g reason=%s component=%u actual=%.12g expected=%.12g limit=%.12g",
                draw,vsSupported,psSupported,patchSupported,shiftRight,diagnostic.reason,diagnostic.component,diagnostic.actual,diagnostic.expected,diagnostic.limit);
            log_values(draw,"worldEye",worldEye,3); log_values(draw,"worldViewProj",wvp,16); log_values(draw,"localEyePos",eye,3);
            log_values(draw,"screenDataToCamera",screen,12); log_values(draw,"vertexClipPlane",clip,4);
            log_values(draw,"screenTransform",field(saved,0,kScreenTransform),8);
            return fail(draw,!vsSupported || !psSupported ? "reflection/whole CB binding/snapshot" : diagnostic.reason);
        }
        if (!make_clones(saved,patch,wvp,eye,screen,local,draw)) return fail(draw,"cross-stage constants/layout/allocation");
        bool ok=render(saved,a,c,d,rt,ds,false);
        ok &= compare_pixels(ctx,c.expected,c.result,local.cloneColourDiff);
        ok &= compare_pixels(ctx,d.expected,d.result,local.cloneDepthDiff);
        if (!ok || local.cloneColourDiff || local.cloneDepthDiff) return fail(draw,"same-eye cloned constants do not reproduce native pixels");
        ok=render(saved,a,c,d,rt,ds,true);
        ok &= compare_pixels(ctx,c.expected,c.result,local.shiftedColour);
        ok &= compare_pixels(ctx,d.expected,d.result,local.shiftedDepth);
        saved.restore();
        ok &= verify_buffers(saved,local.originalConstantDiff);
        local.residual=patch.residual;
        ++total.tested; total.changed += local.shiftedColour || local.shiftedDepth;
        total.buffers+=local.buffers; total.fields+=local.fields; total.constantBytes+=local.constantBytes;
        total.cloneColourDiff+=local.cloneColourDiff; total.cloneDepthDiff+=local.cloneDepthDiff;
        total.shiftedColour+=local.shiftedColour; total.shiftedDepth+=local.shiftedDepth;
        total.originalConstantDiff+=local.originalConstantDiff; total.residual=(std::max)(total.residual,local.residual);
        BVR_LOG("[native-proof] eye draw=%u shiftRight=%.6g projectionX=%.6g deltaLocal=%.6g,%.6g,%.6g residualFraction=%.6g originErrorUu=%.9g shiftedOriginErrorUu=%.9g translationErrorUu=%.9g originLimitUu=0.01 worldEye=%.9g,%.9g,%.9g buffers=%u fields=%u cloneColourDiff=%llu cloneDepthDiff=%llu shiftedColour=%llu shiftedDepth=%llu originalConstantDiff=%llu check=%s; shifted image is not a native target-eye oracle",
            draw,shiftRight,patch.projectionX,patch.delta[0],patch.delta[1],patch.delta[2],patch.residual,patch.originErrorUu,patch.shiftedOriginErrorUu,patch.translationErrorUu,
            worldEye[0],worldEye[1],worldEye[2],local.buffers,local.fields,
            local.cloneColourDiff,local.cloneDepthDiff,local.shiftedColour,local.shiftedDepth,local.originalConstantDiff,
            ok && !local.originalConstantDiff ? "OK" : "FAILED");
        return ok && !local.originalConstantDiff;
    } catch (...) { return fail(draw,"constant proof exception; bindings restored"); }
}

namespace {
struct StreamCb { UINT slot=0, size=0; std::vector<Field> fields; };
struct StreamPlan {
    ComPtr<ID3D11DeviceChild> shader;
    UINT stage=0;
    bool supported=false, stereo=false;
    std::vector<StreamCb> buffers;
    ShaderUsage usage;
};
std::vector<std::unique_ptr<StreamPlan>> g_streamPlans;
ComPtr<ID3D11Buffer> g_streamClones[2][kSlots];
// Cache keeps the COM identity alive; pointer reuse cannot inherit a plan.
// It is reset on device shutdown and capped independently of game resources.
StreamPlan* stream_plan(ID3D11DeviceChild* shader, UINT stage) {
    for (auto& p:g_streamPlans) if (p->shader.Get()==shader && p->stage==stage) return p.get();
    if (!shader || stage>1 || g_streamPlans.size()>=1024) return nullptr;
    auto p=std::make_unique<StreamPlan>(); p->shader=shader; p->stage=stage;
    std::vector<uint8_t> code; ComPtr<ID3D11ShaderReflection> reflection;
    if (!shader_bytecode::read(shader,code) || FAILED(D3DReflect(code.data(),code.size(),
        __uuidof(ID3D11ShaderReflection),reinterpret_cast<void**>(reflection.GetAddressOf())))) return nullptr;
    D3D11_SHADER_DESC sd{}; if (FAILED(reflection->GetDesc(&sd))) return nullptr;
    p->supported=true; p->stereo=true;
    for (UINT i=0;i<sd.OutputParameters;++i) {
        D3D11_SIGNATURE_PARAMETER_DESC out{};
        if (FAILED(reflection->GetOutputParameterDesc(i,&out))) return nullptr;
        if (stage && out.SystemValueType!=D3D_NAME_TARGET) p->supported=false;
        if (stage && out.SystemValueType==D3D_NAME_TARGET && out.SemanticIndex!=0) p->supported=false;
    }
    for (UINT i=0;i<sd.BoundResources;++i) {
        D3D11_SHADER_INPUT_BIND_DESC b{};
        if (FAILED(reflection->GetResourceBindingDesc(i,&b))) return nullptr;
        if (b.Type==D3D_SIT_TEXTURE || b.Type==D3D_SIT_TBUFFER || b.Type==D3D_SIT_STRUCTURED || b.Type==D3D_SIT_BYTEADDRESS) {
            if (!b.BindCount || uint64_t(b.BindPoint)+b.BindCount>D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT) return nullptr;
            for (UINT j=0;j<b.BindCount;++j) p->usage.resources.push_back(b.BindPoint+j);
        } else if (b.Type==D3D_SIT_CBUFFER) {
            if (b.BindPoint>=kSlots || b.BindCount!=1) return nullptr;
            auto* cb=reflection->GetConstantBufferByName(b.Name); D3D11_SHADER_BUFFER_DESC cd{};
            if (!cb || FAILED(cb->GetDesc(&cd)) || cd.Type!=D3D_CT_CBUFFER || cd.Size>65536) return nullptr;
            StreamCb sc; sc.slot=b.BindPoint; sc.size=cd.Size;
            for (UINT j=0;j<cd.Variables;++j) {
                auto* v=cb->GetVariableByIndex(j); D3D11_SHADER_VARIABLE_DESC vd{};
                if (!v || FAILED(v->GetDesc(&vd)) || uint64_t(vd.StartOffset)+vd.Size>cd.Size) return nullptr;
                if (vd.uFlags&D3D_SVF_USED) p->stereo &= classify(vd,v->GetType(),sc.fields,stage,0);
            }
            p->buffers.push_back(std::move(sc));
        } else if (b.Type!=D3D_SIT_SAMPLER) p->supported=false;
    }
    StreamPlan* result=p.get();g_streamPlans.push_back(std::move(p));return result;
}
bool stream_snapshot(Bindings& b, StreamPlan* p, UINT stage, ConstantReader read) {
    if (!p || !p->supported || !p->stereo || !read) return false;
    for (const auto& cb:p->buffers) {
        Slot& slot=b.slots[stage][cb.slot]; D3D11_BUFFER_DESC d{};
        if (b.ctx1) {
            if (stage) b.ctx1->PSGetConstantBuffers1(cb.slot,1,slot.original.GetAddressOf(),&slot.first,&slot.count);
            else b.ctx1->VSGetConstantBuffers1(cb.slot,1,slot.original.GetAddressOf(),&slot.first,&slot.count);
        } else {
            if (stage) b.ctx->PSGetConstantBuffers(cb.slot,1,slot.original.GetAddressOf());
            else b.ctx->VSGetConstantBuffers(cb.slot,1,slot.original.GetAddressOf());
        }
        ++g_streamStats.constantSlotsRead;
        if (!slot.original || slot.first || slot.count!=4096) return false;
        slot.original->GetDesc(&d);
        if (d.BindFlags!=D3D11_BIND_CONSTANT_BUFFER || d.ByteWidth<cb.size || d.ByteWidth>65536 || d.ByteWidth%16) return false;
        if (cb.fields.empty()) continue;
        if (!read(slot.original.Get(),slot.bytes) || slot.bytes.size()!=d.ByteWidth) return false;
        slot.fields=cb.fields;
    }
    return true;
}
bool stream_clones(Bindings& b,const EyePatch& patch,const float* wvp,const float* eye,const float* screen) {
    const float transform[8]={0.5f,0,0,0.5f,0,-0.5f,0,0.5f};
    for (UINT stage=0;stage<2;++stage) for (UINT i=0;i<kSlots;++i) {
        Slot& s=b.slots[stage][i]; if (s.bytes.empty()) continue;
        auto target=s.bytes;
        for (const Field& f:s.fields) {
            if (uint64_t(f.offset)+f.bytes>s.bytes.size()) return false;
            const auto* value=reinterpret_cast<const float*>(s.bytes.data()+f.offset);
            const float* expected=f.kind==kWvp?wvp:f.kind==kEye?eye:f.kind==kScreen?screen:transform;
            for (UINT j=0;j<f.bytes/4;++j)
                if (f.kind==kClip ? value[j]!=0 : !close_float(value[j],expected[j])) return false;
            if (f.kind==kWvp || f.kind==kEye)
                std::memcpy(target.data()+f.offset,f.kind==kWvp?patch.wvp:patch.eye,f.bytes);
        }
        auto& clone=g_streamClones[stage][i]; D3D11_BUFFER_DESC d{};
        if (clone) clone->GetDesc(&d);
        if (!clone || d.ByteWidth!=target.size()) {
            clone.Reset();d={};d.ByteWidth=static_cast<UINT>(target.size());d.Usage=D3D11_USAGE_DYNAMIC;
            d.BindFlags=D3D11_BIND_CONSTANT_BUFFER;d.CPUAccessFlags=D3D11_CPU_ACCESS_WRITE;
            if (FAILED(b.dev->CreateBuffer(&d,nullptr,clone.GetAddressOf()))) return false;
        }
        D3D11_MAPPED_SUBRESOURCE m{};
        if (FAILED(b.ctx->Map(clone.Get(),0,D3D11_MAP_WRITE_DISCARD,0,&m))) return false;
        std::memcpy(m.pData,target.data(),target.size());b.ctx->Unmap(clone.Get(),0);
        s.same=clone;s.shifted=clone;
    }
    return true;
}
// These immutable states depend only on the already-supported blend/depth
// modes. Keep device-owned objects, never game pointers or resource contents.
ComPtr<ID3D11Device> g_streamStateDevice;
ComPtr<ID3D11BlendState> g_streamBlend[3]; // coverage, zero transmission, alpha transmission
ComPtr<ID3D11DepthStencilState> g_streamDepth[3]; // LESS, LESS_EQUAL, EQUAL; all read-only
void reset_stream_states() {
    for (auto& state:g_streamBlend) state.Reset();
    for (auto& state:g_streamDepth) state.Reset();
    g_streamStateDevice.Reset();
}
bool stream_states(ID3D11Device* dev, D3D11_BLEND dest, const D3D11_DEPTH_STENCIL_DESC& native,
                   ID3D11BlendState*& coverageBlend, ID3D11DepthStencilState*& coverageDepth,
                   ID3D11BlendState*& transBlend, ID3D11DepthStencilState*& transDepth) {
    if (g_streamStateDevice.Get()!=dev) { reset_stream_states();g_streamStateDevice=dev; }
    const UINT coverageIndex=native.DepthFunc==D3D11_COMPARISON_LESS?0:1;
    const UINT transIndex=native.DepthWriteMask==D3D11_DEPTH_WRITE_MASK_ALL?2:coverageIndex;
    for (UINT i=0;i<3;++i) {
        const bool need=i==0 || (dest!=D3D11_BLEND_ONE && i==(dest==D3D11_BLEND_ZERO?1u:2u));
        if (!need || g_streamBlend[i]) continue;
        D3D11_BLEND_DESC d{};auto& r=d.RenderTarget[0];r.BlendEnable=TRUE;
        r.SrcBlend=r.SrcBlendAlpha=D3D11_BLEND_ZERO;
        r.DestBlend=i==2?D3D11_BLEND_INV_SRC_ALPHA:D3D11_BLEND_ZERO;
        r.DestBlendAlpha=i?D3D11_BLEND_ONE:D3D11_BLEND_ZERO;
        r.BlendOp=r.BlendOpAlpha=D3D11_BLEND_OP_ADD;r.RenderTargetWriteMask=D3D11_COLOR_WRITE_ENABLE_RED;
        if (FAILED(dev->CreateBlendState(&d,g_streamBlend[i].GetAddressOf()))) return false;
        ++g_streamStats.stateCreates;
    }
    for (UINT i=0;i<3;++i) {
        if ((i!=coverageIndex && (dest==D3D11_BLEND_ONE || i!=transIndex)) || g_streamDepth[i]) continue;
        D3D11_DEPTH_STENCIL_DESC d{};d.DepthEnable=TRUE;d.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ZERO;
        d.DepthFunc=i==0?D3D11_COMPARISON_LESS:i==1?D3D11_COMPARISON_LESS_EQUAL:D3D11_COMPARISON_EQUAL;
        d.StencilReadMask=D3D11_DEFAULT_STENCIL_READ_MASK;d.StencilWriteMask=D3D11_DEFAULT_STENCIL_WRITE_MASK;
        d.FrontFace={D3D11_STENCIL_OP_KEEP,D3D11_STENCIL_OP_KEEP,D3D11_STENCIL_OP_KEEP,D3D11_COMPARISON_ALWAYS};
        d.BackFace=d.FrontFace;
        if (FAILED(dev->CreateDepthStencilState(&d,g_streamDepth[i].GetAddressOf()))) return false;
        ++g_streamStats.stateCreates;
    }
    coverageBlend=g_streamBlend[0].Get();coverageDepth=g_streamDepth[coverageIndex].Get();
    transBlend=dest==D3D11_BLEND_ONE?nullptr:g_streamBlend[dest==D3D11_BLEND_ZERO?1:2].Get();
    transDepth=dest==D3D11_BLEND_ONE?nullptr:g_streamDepth[transIndex].Get();return true;
}
bool stream_draw(ID3D11DeviceContext* ctx,const frame_inspector::DrawArgs& args) {
    const bool drawn=frame_inspector::replay_draw(ctx,args);g_streamStats.replayDraws+=drawn;return drawn;
}
struct StreamBlendGuard {
    ID3D11DeviceContext* ctx;ComPtr<ID3D11BlendState> blend;ComPtr<ID3D11DepthStencilState> depth;
    FLOAT factors[4]{};UINT mask=0,ref=0;
    explicit StreamBlendGuard(ID3D11DeviceContext* c):ctx(c) {
        ctx->OMGetBlendState(blend.GetAddressOf(),factors,&mask);ctx->OMGetDepthStencilState(depth.GetAddressOf(),&ref);
    }
    ~StreamBlendGuard() {ctx->OMSetBlendState(blend.Get(),factors,mask);ctx->OMSetDepthStencilState(depth.Get(),ref);}
};
} // namespace
bool stream_shader_usage(ID3D11DeviceChild* shader,unsigned stage,ShaderUsage& out) {
    try {auto* p=stream_plan(shader,stage);if (!p || !p->supported) return false;out=p->usage;return true;}
    catch (...) {return false;}
}
StreamStats stream_eye_stats() { return g_streamStats; }
void stream_eye_reset() {
    g_streamPlans.clear();for (auto& stage:g_streamClones) for (auto& cb:stage) cb.Reset();
    reset_stream_states();g_streamStats={};
}
bool stream_eye_replay(ID3D11DeviceContext* ctx,const frame_inspector::DrawArgs& args,
                       ID3D11RenderTargetView* colour,ID3D11DepthStencilView* depth,
                       ID3D11RenderTargetView* transmittance,ID3D11RenderTargetView* sourceCoverage,
                       ID3D11DepthStencilView* sourceReadOnlyDepth,float shift,const float world[3],
                       ConstantReader read,float* tanH,float* tanV,const char** reason) {
    auto refuse=[&](const char* why){if(reason)*reason=why;return false;};
    try {
        if (!ctx || !colour || !depth || !transmittance || !sourceCoverage || !sourceReadOnlyDepth || !world) return refuse("missing foreground target/tag");
        Bindings saved(ctx,false);StreamBlendGuard state(ctx);
        ComPtr<ID3D11VertexShader> vs;ComPtr<ID3D11PixelShader> ps;
        ctx->VSGetShader(vs.GetAddressOf(),nullptr,nullptr);ctx->PSGetShader(ps.GetAddressOf(),nullptr,nullptr);
        if (!stream_snapshot(saved,stream_plan(vs.Get(),0),0,read) ||
            !stream_snapshot(saved,stream_plan(ps.Get(),1),1,read)) return refuse("foreground shader/constant upload");
        const float* wvp=field(saved,0,kWvp),*eye=field(saved,0,kEye),*screen=field(saved,0,kScreen),*clip=field(saved,0,kClip);
        EyePatch patch;EyeDiagnostic diagnostic;
        if (!make_eye_patch(wvp,eye,screen,clip,shift,patch,&diagnostic,world)) return refuse(diagnostic.reason);
        if (!stream_clones(saved,patch,wvp,eye,screen)) return refuse("foreground cross-stage constants/clone");
        D3D11_BLEND_DESC bd{};
        if(state.blend)state.blend->GetDesc(&bd);
        else bd.RenderTarget[0].RenderTargetWriteMask=D3D11_COLOR_WRITE_ENABLE_ALL;
        const auto& native=bd.RenderTarget[0];
        if (bd.AlphaToCoverageEnable || bd.IndependentBlendEnable || (native.RenderTargetWriteMask&7)!=7 ||
            (native.BlendEnable && (native.BlendOp!=D3D11_BLEND_OP_ADD ||
             (native.SrcBlend!=D3D11_BLEND_ONE && native.SrcBlend!=D3D11_BLEND_SRC_ALPHA) ||
             (native.DestBlend!=D3D11_BLEND_ZERO && native.DestBlend!=D3D11_BLEND_ONE && native.DestBlend!=D3D11_BLEND_INV_SRC_ALPHA))))
            return refuse("unsupported foreground blend");
        D3D11_DEPTH_STENCIL_DESC dd{};
        if(state.depth)state.depth->GetDesc(&dd);
        else {dd.DepthEnable=TRUE;dd.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ALL;dd.DepthFunc=D3D11_COMPARISON_LESS;}
        if(dd.StencilEnable || !dd.DepthEnable || (dd.DepthFunc!=D3D11_COMPARISON_LESS && dd.DepthFunc!=D3D11_COMPARISON_LESS_EQUAL))
            return refuse("unsupported foreground depth/stencil");
        // Native PS coverage (including discard) under the actual source depth,
        // with an explicitly read-only DSV and zero depth/stencil writes. This
        // removes translucent/additive source silhouettes from the world too.
        const auto dest=native.BlendEnable?native.DestBlend:D3D11_BLEND_ZERO;
        ID3D11BlendState* coverageBlend=nullptr;ID3D11DepthStencilState* coverageState=nullptr;
        ID3D11BlendState* transBlend=nullptr;ID3D11DepthStencilState* transDepth=nullptr;
        if (!stream_states(saved.dev.Get(),dest,dd,coverageBlend,coverageState,transBlend,transDepth))
            return refuse("foreground replay state allocation");
        saved.touched=true;
        ctx->OMSetRenderTargets(1,&sourceCoverage,sourceReadOnlyDepth);
        ctx->OMSetBlendState(coverageBlend,state.factors,state.mask);ctx->OMSetDepthStencilState(coverageState,0);
        bool ok=stream_draw(ctx,args);
        ctx->OMSetBlendState(state.blend.Get(),state.factors,state.mask);ctx->OMSetDepthStencilState(state.depth.Get(),state.ref);
        saved.bind(true);ctx->OMSetRenderTargets(1,&colour,depth);
        ok=ok && stream_draw(ctx,args);
        if(ok && dest!=D3D11_BLEND_ONE) {
            ctx->OMSetRenderTargets(1,&transmittance,depth);
            ctx->OMSetBlendState(transBlend,state.factors,state.mask);ctx->OMSetDepthStencilState(transDepth,0);
            ok=stream_draw(ctx,args);
        }
        if(tanH)*tanH=screen[0]*0.5f;if(tanV)*tanV=-screen[5]*0.5f;
        return ok || refuse("foreground replay unavailable");
    } catch (...) {return refuse("foreground replay exception");}
}

} // namespace bvr::b1r::native_proof::detail
