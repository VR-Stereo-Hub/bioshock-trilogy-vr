#include "core/gfx/shader_bytecode.h"
#include "core/util/log.h"
#include <windows.h>
#include <d3d11.h>
#include <MinHook.h>
#include <intrin.h>
#include <atomic>
#include <cstring>

namespace bvr::shader_bytecode {
namespace {
// Private to this instrument; shaders and layouts are distinct D3D objects.
const GUID kBytes = {0x6c8401e4,0xb9b0,0x4c6b,{0xa7,0x9c,0x98,0xf9,0x36,0xda,0xb6,0x23}};
constexpr size_t kObjectMax=2*1024*1024, kRunMax=64*1024*1024;
std::atomic<bool> g_enabled{false};
std::atomic<ID3D11Device*> g_device{nullptr}; // held once, process lifetime
uintptr_t g_first=0, g_end=0;
std::atomic<uint32_t> g_stored{0},g_refused{0},g_failed{0};
std::atomic<size_t> g_bytes{0};
std::atomic<bool> g_hooks{false};
using VsFn=HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*,const void*,SIZE_T,ID3D11ClassLinkage*,ID3D11VertexShader**);
using PsFn=HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*,const void*,SIZE_T,ID3D11ClassLinkage*,ID3D11PixelShader**);
using IlFn=HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*,const D3D11_INPUT_ELEMENT_DESC*,UINT,const void*,SIZE_T,ID3D11InputLayout**);
VsFn g_vs=nullptr; PsFn g_ps=nullptr; IlFn g_il=nullptr;

bool accepts(ID3D11Device* dev,const void* caller) {
    if(!g_enabled.load(std::memory_order_acquire)) return false;
    const uintptr_t at=reinterpret_cast<uintptr_t>(caller);
    ID3D11Device* bound=g_device.load(std::memory_order_acquire);
    return at>=g_first && at<g_end && (!bound || dev==bound);
}
void save(ID3D11DeviceChild* object,const void* data,size_t size) noexcept {
    if(!object || !data || !size || size>kObjectMax) { ++g_refused; return; }
    size_t used=g_bytes.load();
    do {
        if(used>kRunMax-size) { ++g_refused; return; }
    } while(!g_bytes.compare_exchange_weak(used,used+size));
    if(SUCCEEDED(object->SetPrivateData(kBytes,static_cast<UINT>(size),data))) ++g_stored;
    else { ++g_failed; g_bytes.fetch_sub(size); }
}
HRESULT STDMETHODCALLTYPE create_vs(ID3D11Device* dev,const void* code,SIZE_T size,ID3D11ClassLinkage* link,ID3D11VertexShader** out) {
    const bool capture=accepts(dev,_ReturnAddress());
    const HRESULT hr=g_vs(dev,code,size,link,out);
    if(capture && SUCCEEDED(hr) && out && *out) save(*out,code,size);
    return hr;
}
HRESULT STDMETHODCALLTYPE create_ps(ID3D11Device* dev,const void* code,SIZE_T size,ID3D11ClassLinkage* link,ID3D11PixelShader** out) {
    const bool capture=accepts(dev,_ReturnAddress());
    const HRESULT hr=g_ps(dev,code,size,link,out);
    if(capture && SUCCEEDED(hr) && out && *out) save(*out,code,size);
    return hr;
}
HRESULT STDMETHODCALLTYPE create_il(ID3D11Device* dev,const D3D11_INPUT_ELEMENT_DESC* desc,UINT count,const void* code,SIZE_T size,ID3D11InputLayout** out) {
    const bool capture=accepts(dev,_ReturnAddress());
    const HRESULT hr=g_il(dev,desc,count,code,size,out);
    if(capture && SUCCEEDED(hr) && out && *out) {
        if(!desc || count>32) { ++g_refused; return hr; }
        InputElement elements[32]={};
        for(UINT i=0;i<count;++i) {
            if(!desc[i].SemanticName || strlen(desc[i].SemanticName)>=sizeof(elements[i].semantic)) {
                ++g_refused; return hr; // refuse truncated semantics
            }
            strcpy_s(elements[i].semantic,desc[i].SemanticName);
            elements[i].index=desc[i].SemanticIndex; elements[i].format=desc[i].Format;
            elements[i].slot=desc[i].InputSlot; elements[i].offset=desc[i].AlignedByteOffset;
            elements[i].classification=desc[i].InputSlotClass; elements[i].step=desc[i].InstanceDataStepRate;
        }
        save(*out,elements,count*sizeof(InputElement));
    }
    return hr;
}
} // namespace
void configure(const void* base,size_t bytes) {
    if(!base || !bytes || g_enabled.load()) return;
    g_first=reinterpret_cast<uintptr_t>(base); g_end=g_first+bytes;
    if(g_end>g_first) g_enabled.store(true);
}
bool install(ID3D11Device* dev) {
    if(!g_enabled.load() || !dev) return false;
    if(g_hooks) return true;
    void** vt=*reinterpret_cast<void***>(dev);
    struct Slot { unsigned index; void* detour; void** original; };
    const Slot slots[]={
        {11,reinterpret_cast<void*>(&create_il),reinterpret_cast<void**>(&g_il)},
        {12,reinterpret_cast<void*>(&create_vs),reinterpret_cast<void**>(&g_vs)},
        {15,reinterpret_cast<void*>(&create_ps),reinterpret_cast<void**>(&g_ps)}};
    unsigned ok=0;
    for(const auto& slot:slots) {
        MH_STATUS s=MH_CreateHook(vt[slot.index],slot.detour,slot.original);
        if(s==MH_OK) s=MH_EnableHook(vt[slot.index]);
        if(s==MH_OK) ++ok;
        else BVR_LOG("[native-audit] shader creation slot %u failed: %s",slot.index,MH_StatusToString(s));
    }
    g_hooks=ok==3;
    if(!g_hooks) g_enabled.store(false);
    BVR_LOG("[native-audit] bytecode hooks %u/3 (host callers only; 64 MiB lifetime cap)",ok);
    return g_hooks;
}
void bind_device(ID3D11Device* dev) {
    if(!g_enabled.load() || !dev) return;
    ID3D11Device* expected=nullptr;
    dev->AddRef();
    if(!g_device.compare_exchange_strong(expected,dev)) {
        dev->Release();
        if(expected!=dev) { g_enabled.store(false); BVR_LOG("[native-audit] device changed; collection refused"); }
    }
}
void stop() { g_enabled.store(false); }
bool read(ID3D11DeviceChild* object,std::vector<uint8_t>& bytes) {
    bytes.clear();
    if(!object) return false;
    UINT size=0;
    if(FAILED(object->GetPrivateData(kBytes,&size,nullptr)) || !size || size>kObjectMax) return false;
    bytes.resize(size);
    if(FAILED(object->GetPrivateData(kBytes,&size,bytes.data())) || size!=bytes.size()) { bytes.clear(); return false; }
    return true;
}
Stats stats() { return {g_stored.load(),g_refused.load(),g_failed.load(),g_bytes.load(),g_hooks.load()}; }
} // namespace bvr::shader_bytecode
