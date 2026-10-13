#pragma once
#include <d3d11.h>
#include <imgui.h>
#include <backends/imgui_impl_dx11.h>
namespace bvr::overlay {
// ImGui saves shader/raster/blend state, but the caller owns the OM targets.
// Both the native canvas and the later desktop pass preserve all eight slots.
inline void draw_target(ID3D11DeviceContext* ctx,ID3D11RenderTargetView* target,
                        ImDrawData* data,ImVec2 scale=ImVec2(1,1)) {
    ID3D11RenderTargetView* old[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]={};
    ID3D11DepthStencilView* depth=nullptr;
    ctx->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT,old,&depth);
    const ImVec2 prior=data->FramebufferScale;data->FramebufferScale=scale;
    ctx->OMSetRenderTargets(1,&target,nullptr);
    ImGui_ImplDX11_RenderDrawData(data);
    data->FramebufferScale=prior;
    ctx->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT,old,depth);
    for(auto* rt:old) if(rt) rt->Release();
    if(depth) depth->Release();
}
class Canvas {
public:
    Canvas()=default;
    Canvas(const Canvas&)=delete;
    Canvas& operator=(const Canvas&)=delete;
    ~Canvas(){clear();}
    void clear() {
        if(rtv_) rtv_->Release();
        if(tex_) tex_->Release();
        rtv_=nullptr;tex_=nullptr;w_=h_=0;
    }
    bool ensure(ID3D11Device* dev,unsigned w,unsigned h) {
        if(tex_ && w_==w && h_==h) return true;
        clear();
        if(!w || !h) return false;
        D3D11_TEXTURE2D_DESC d{};
        d.Width=w;d.Height=h;d.MipLevels=d.ArraySize=1;
        d.Format=DXGI_FORMAT_R8G8B8A8_UNORM;d.SampleDesc.Count=1;
        d.Usage=D3D11_USAGE_DEFAULT;d.BindFlags=D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE;
        if(FAILED(dev->CreateTexture2D(&d,nullptr,&tex_)) ||
           FAILED(dev->CreateRenderTargetView(tex_,nullptr,&rtv_))) {clear();return false;}
        w_=w;h_=h;return true;
    }
    void render(ID3D11DeviceContext* ctx,ImDrawData* data) {
        const float transparent[4]={};
        ctx->ClearRenderTargetView(rtv_,transparent);
        draw_target(ctx,rtv_,data);
    }
    ID3D11Texture2D* texture() const {return tex_;}
private:
    ID3D11Texture2D* tex_=nullptr;
    ID3D11RenderTargetView* rtv_=nullptr;
    unsigned w_=0,h_=0;
};
} // namespace bvr::overlay
