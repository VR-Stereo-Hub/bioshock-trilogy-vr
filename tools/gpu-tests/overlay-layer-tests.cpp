// gpu-test-sources: third_party/imgui/imgui.cpp third_party/imgui/imgui_draw.cpp third_party/imgui/imgui_tables.cpp third_party/imgui/imgui_widgets.cpp third_party/imgui/backends/imgui_impl_dx11.cpp
// Production native canvas + mirror renderer and the same quad/ray math used by XR.
#include "gpu_test_util.h"
#include "core/ui/overlay_canvas.h"
#include "core/ui/overlay_geometry.h"
using namespace gt;
namespace ov=bvr::overlay;
int main() {
    if(!make_device()) return 1;
    ov::Canvas canvas;
    check(canvas.ensure(dev,960,1000),"native canvas allocation","960x1000");
    ImGui::CreateContext();
    auto& io=ImGui::GetIO();io.IniFilename=nullptr;io.DisplaySize=ImVec2(960,1000);io.DeltaTime=1.f/90.f;
    ImGui_ImplDX11_Init(dev,ctx);ImGui_ImplDX11_NewFrame();ImGui::NewFrame();
    ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(240,250),ImVec2(720,750),IM_COL32(255,0,255,255));
    ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(10,10),ImVec2(40,40),IM_COL32(255,0,0,128));
    ImGui::Render();
    ID3D11Texture2D* bb=make_tex(320,300,DXGI_FORMAT_R8G8B8A8_UNORM,D3D11_BIND_RENDER_TARGET);
    ID3D11Texture2D* second=make_tex(320,300,DXGI_FORMAT_R8G8B8A8_UNORM,D3D11_BIND_RENDER_TARGET);
    ID3D11RenderTargetView *rt=nullptr,*rt2=nullptr;
    dev->CreateRenderTargetView(bb,nullptr,&rt);dev->CreateRenderTargetView(second,nullptr,&rt2);
    const float green[4]={0,1,0,1};ctx->ClearRenderTargetView(rt,green);
    ID3D11RenderTargetView* targets[2]={rt,rt2};ctx->OMSetRenderTargets(2,targets,nullptr);
    canvas.render(ctx,ImGui::GetDrawData());
    auto world=readback(bb),layer=readback(canvas.texture());
    const size_t middle=((size_t)500*960+480)*4,partial=((size_t)20*960+20)*4;
    check(world[0]==0 && world[1]==1 && world[((size_t)150*320+160)*4+1]==1,
          "native ImGui pass never changes the eye input","world stays green");
    check(layer[middle]>.99f && layer[middle+2]>.99f && layer[middle+3]>.99f && layer[3]==0,
          "canvas carries panel pixels with transparent surroundings","center alpha %.2f outer %.2f",layer[middle+3],layer[3]);
    check(std::fabs(layer[partial]-.502f)<.01f && std::fabs(layer[partial+3]-.502f)<.01f,
          "canvas alpha is premultiplied for XR composition","r %.3f a %.3f",layer[partial],layer[partial+3]);
    ID3D11RenderTargetView* after[2]={};ctx->OMGetRenderTargets(2,after,nullptr);
    check(after[0]==rt && after[1]==rt2,"native pass restores every bound render target","slot 0/1 %d/%d",after[0]==rt,after[1]==rt2);
    rel(after[0]);rel(after[1]);
    ID3D11Texture2D* same=canvas.texture();
    check(canvas.ensure(dev,960,1000) && canvas.texture()==same,"game resize does not reallocate native canvas","identity stable");
    ov::draw_target(ctx,rt,ImGui::GetDrawData(),ImVec2(320.f/960,300.f/1000));
    world=readback(bb);const size_t center=((size_t)150*320+160)*4;
    check(world[center]>.99f && world[center+2]>.99f && world[1]==1,
          "desktop pass draws after eye consumers at the mirror size","center magenta, outside green");
    check(ImGui::GetDrawData()->FramebufferScale.x==1 && ImGui::GetDrawData()->FramebufferScale.y==1,
          "mirror pass restores ImGui framebuffer scale","%.1f/%.1f",
          ImGui::GetDrawData()->FramebufferScale.x,ImGui::GetDrawData()->FramebufferScale.y);
    const auto q=ov::quad_geometry(1.3f,1.f,1.1f);
    const float origin[3]={.25f,-.15f,-.3f},aim[3]={-.25f,.15f,-1.f};
    float x=0,y=0;
    bool hit=ov::quad_pixel(origin,aim,q,960,1000,&x,&y);
    check(hit && std::fabs(x-480)<.001f && std::fabs(y-500)<.001f,
          "controller origin and direction intersect the finite-depth quad","%.3f,%.3f",x,y);
    const float forward[3]={0,0,-1};
    hit=ov::quad_pixel(origin,forward,q,960,1000,&x,&y);
    check(hit && x>560 && y>540,"controller translation changes the cursor (not angular-only)","%.2f,%.2f",x,y);
    const float behind[3]={0,0,1},past[3]={0,0,-2};
    check(!ov::quad_pixel(origin,behind,q,960,1000,&x,&y) &&
          !ov::quad_pixel(past,forward,q,960,1000,&x,&y),
          "rays facing away or starting beyond the panel are refused","");
    check(ov::quad_geometry(0,1,1).depth==0 && ov::quad_geometry(1,NAN,1).depth==0,
          "invalid geometry cannot create a cursor","");
    ctx->OMSetRenderTargets(0,nullptr,nullptr);rel(rt2);rel(rt);rel(second);rel(bb);canvas.clear();
    ImGui_ImplDX11_Shutdown();ImGui::DestroyContext();
    return finish("overlay-layer");
}
