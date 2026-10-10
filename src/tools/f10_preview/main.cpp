// Offscreen proof of the PRODUCTION BS1 menu. No game, hooks, XR or user config.
#include "game/bioshock1r/menu_view.h"
#include "game/bioshock1r/menu_theme.h"
#include "game/bioshock1r/menu_preferences.h"
#include <windows.h>
#include <d3d11.h>
#include <wrl/client.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_impl_dx11.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <map>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;
using namespace bvr::b1r::menu;
namespace {
int g_failed=0,g_checks=0;
void check(bool value,const char* message) {
    ++g_checks;
    if(!value) {++g_failed;std::printf("FAIL: %s\n",message);}
}
struct PreviewBackend final : Backend {
    std::array<std::array<float,2>,static_cast<size_t>(Setting::Count)> values{};
    Context ctx;
    Preferences saved;
    int saves=0,edits=0;
    PreviewBackend() {
        std::size_t count;const auto* all=specs(count);
        for(std::size_t i=0;i<count;++i) {
            check(all[i].id==static_cast<Setting>(i),"catalogue indices match stable ids");
            values[i].fill(all[i].kind==Kind::Slider?(all[i].min+all[i].max)*.5f:all[i].min);
        }
        set(Setting::TextSize,1);set(Setting::ViewLevel,0);set(Setting::Height,9);
        set(Setting::SnapTurn,1);set(Setting::SnapAngle,45);set(Setting::TurnSpeed,1);
        set(Setting::AmmoModifier,1);set(Setting::AmmoStick,1);set(Setting::Swing,1);
        set(Setting::HandSize,1);set(Setting::WeaponSize,1);set(Setting::ArmLength,1);
        set(Setting::HandsArmsSize,.83f);set(Setting::ShoulderBarForward,-16);
        set(Setting::ShoulderBarRight,0);set(Setting::ShoulderBarUp,-25);
        set(Setting::ShoulderWidth,38.1f);set(Setting::WeaponFollowsHands,1);
        set(Setting::UpperArmLength,.71f);set(Setting::ForearmLength,1.13f);set(Setting::ShowArms,1);
        for(auto id:{Setting::HandPitch,Setting::HandYaw,Setting::HandRoll,Setting::HandGripForward,
                     Setting::HandGripRight,Setting::HandGripUp}) set(id,0);
        set(Setting::ElbowOut,.35f);set(Setting::BodyFollow,1);set(Setting::WorldScale,1);
        set(Setting::HudPanel,1);set(Setting::Reticle,1);set(Setting::Laser,1);
        set(Setting::HandMode,4);set(Setting::ArmSolver,1);
        ctx.handMode=4;ctx.maxHandMode=4;ctx.solverV2=true;ctx.hasSolverChoice=true;
        ctx.tracking=true;ctx.cameraHook=true;ctx.engineReady=true;ctx.profile="Pistol";ctx.profileHand=1;
    }
    void set(Setting id,float value) {values[static_cast<size_t>(id)].fill(value);}
    float read(Setting id,int hand) const override {return values[static_cast<size_t>(id)][hand==0?0:1];}
    void edit(Setting id,int hand,float value,bool commit) override {
        ++edits;
        if(hand<0) set(id,value);else values[static_cast<size_t>(id)][hand]=value;
        if(commit && is_player_setting(id)) {
            ++saves;saved.set({id,spec(id).scope==Scope::Global?-1:hand,spec(id).scope==Scope::Profile?ctx.profile:"",value});
        }
        if(id==Setting::HandMode) ctx.handMode=static_cast<int>(value);
        if(id==Setting::ArmSolver) ctx.solverV2=value!=0;
    }
    Context context() const override {return ctx;}
    void action(Action) override {}
    void diagnostics(Tab tab) override {
        if(tab==Tab::Display && theme::section("Resolution (render size)")) {
            ImGui::TextUnformatted("Current render: 2750 x 2850");
            theme::note("Resolution changes need a restart. In-game config writes are unavailable because the game can overwrite them when it exits.");
        }
        if(tab==Tab::Runtime && theme::section("Runtime status"))
            ImGui::TextUnformatted("Preview fixture / no game connected");
    }
};
bool save_bmp(const std::filesystem::path& path,const unsigned char* data,int w,int h,int pitch) {
    FILE* f=nullptr;_wfopen_s(&f,path.c_str(),L"wb");if(!f)return false;
    BITMAPFILEHEADER fh{};BITMAPINFOHEADER ih{};
    ih.biSize=sizeof ih;ih.biWidth=w;ih.biHeight=-h;ih.biPlanes=1;ih.biBitCount=32;
    fh.bfType=0x4D42;fh.bfOffBits=sizeof fh+sizeof ih;fh.bfSize=fh.bfOffBits+w*h*4;
    fwrite(&fh,sizeof fh,1,f);fwrite(&ih,sizeof ih,1,f);
    std::vector<unsigned char> row(w*4);
    for(int y=0;y<h;++y) {
        for(int x=0;x<w;++x) {const auto* src=data+y*pitch+x*4;
            row[x*4]=src[2];row[x*4+1]=src[1];row[x*4+2]=src[0];row[x*4+3]=255;}
        fwrite(row.data(),1,row.size(),f);
    }
    return fclose(f)==0;
}
void preference_tests() {
    Preferences p;
    check(p.set({Setting::SnapAngle,-1,"",45}),"player setting accepted");
    check(!p.set({Setting::HandMode,-1,"",4}),"Debug excluded from autosave");
    check(p.set({Setting::HandMode,-1,"",4},true),"Debug accepted only on explicit save");
    check(p.set({Setting::ModelPitch,1,"Pistol",-8}),"weapon-specific preference accepted");
    check(p.set({Setting::ModelPitch,1,"Shotgun",-2}),"second weapon remains separate");
    check(p.set({Setting::ShoulderRight,0,"",-16}),"left shoulder accepted");
    check(p.set({Setting::ShoulderRight,1,"",16}),"right shoulder accepted separately");
    check(!p.set({Setting::SnapAngle,-1,"",std::numeric_limits<float>::quiet_NaN()}),"NaN rejected");
    check(!p.set({Setting::SnapAngle,-1,"",999}),"out-of-range value rejected");
    check(!p.set({Setting::ModelPitch,1,"../invalid",8}),"invalid profile rejected");
    check(!p.set({Setting::ModelPitch,-1,"Pistol",8}),"profile edits require explicit hand");
    const std::string text=p.serialize();
    for(size_t i=0;i<text.size();++i) if(text[i]=='\n') check(i>0&&text[i-1]=='\r',"preferences use CRLF");
    Preferences roundtrip;check(roundtrip.parse(text)==0,"saved preferences parse without rejection");
    check(roundtrip.serialize()==text,"save/load roundtrip preserves values and profiles exactly");
    check(roundtrip.parse("P HandMode -1 - 3\nD SnapAngle -1 - 60\nP Nope -1 - 1\n")==3,"tier spoofing and unknown keys rejected");
    PreviewBackend b;
    check(!visible(spec(Setting::ArmTwist),Level::Debug,b.ctx),"mode 4 hides legacy arm controls");
    check(!visible(spec(Setting::ArmSolver),Level::Debug,b.ctx),"mode 4 hides ignored solver selector");
    check(!visible(spec(Setting::LatePosition),Level::Debug,b.ctx),"mode 4 hides old actor replay");
    b.ctx.handMode=3;
    check(!visible(spec(Setting::ArmTwist),Level::Debug,b.ctx),"v2 solver hides legacy twist");
    b.ctx.solverV2=false;
    check(visible(spec(Setting::ArmTwist),Level::Debug,b.ctx),"old solver retains relevant comparison");
    check(!visible(spec(Setting::ArmTwist),Level::Advanced,b.ctx),"legacy solver tuning excluded from Advanced");
    b.set(Setting::AmmoModifier,2);
    check(disabled_reason(spec(Setting::R3Jump),b,1)!=nullptr,"R3 conflict is explained");
    check(disabled_reason(spec(Setting::ModelPitch),b,0)!=nullptr,"wrong-hand profile editing disabled");
}
}
int main(int argc,char** argv) {
    const int w=argc>1?atoi(argv[1]):760,h=argc>2?atoi(argv[2]):850;
    const float scale=argc>3?static_cast<float>(atof(argv[3])):1;
    const float textScale=argc>4?static_cast<float>(atof(argv[4])):1;
    const std::filesystem::path out=argc>5?argv[5]:"f10-preview";
    std::filesystem::create_directories(out);
    if(w<570||h<640||scale<=0)return 2;
    preference_tests();
    CoInitializeEx(nullptr,COINIT_MULTITHREADED);
    ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;D3D_FEATURE_LEVEL feature;
    HRESULT hr=D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&device,&feature,&context);
    if(FAILED(hr)) hr=D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&device,&feature,&context);
    if(FAILED(hr))return 3;
    D3D11_TEXTURE2D_DESC desc{};desc.Width=w;desc.Height=h;desc.MipLevels=1;desc.ArraySize=1;
    desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;desc.SampleDesc.Count=1;desc.BindFlags=D3D11_BIND_RENDER_TARGET;
    ComPtr<ID3D11Texture2D> target,staging;ComPtr<ID3D11RenderTargetView> rtv;
    if(FAILED(device->CreateTexture2D(&desc,nullptr,&target)) || FAILED(device->CreateRenderTargetView(target.Get(),nullptr,&rtv)))return 4;
    desc.BindFlags=0;desc.Usage=D3D11_USAGE_STAGING;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    if(FAILED(device->CreateTexture2D(&desc,nullptr,&staging)))return 5;
    ImGui::CreateContext();auto& io=ImGui::GetIO();io.IniFilename=nullptr;
    io.ConfigFlags|=ImGuiConfigFlags_NavEnableKeyboard;io.ConfigInputTrickleEventQueue=false;
    io.DisplaySize=ImVec2(static_cast<float>(w),static_cast<float>(h));io.DeltaTime=1.f/60;
    theme::initialize(device.Get());check(theme::assets_loaded(),"all six embedded approved assets loaded");
    ImGui_ImplDX11_Init(device.Get(),context.Get());
    PreviewBackend backend;backend.set(Setting::TextSize,textScale);
    ViewState state;std::vector<Hit> hits;bool layout=true,aligned=true;
    ViewOptions options{ImVec2(static_cast<float>(w),static_cast<float>(h)),scale,false,&hits,&layout};
    auto frame=[&](ImVec2 mouse=ImVec2(-1,-1),bool down=false) {
        io.AddMousePosEvent(mouse.x,mouse.y);io.AddMouseButtonEvent(0,down);
        ImGui_ImplDX11_NewFrame();ImGui::NewFrame();draw(backend,state,options);ImGui::Render();
        auto* rt=rtv.Get();context->OMSetRenderTargets(1,&rt,nullptr);
        const float black[]={0,0,0,1};context->ClearRenderTargetView(rt,black);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    };
    auto find=[&](const std::string& name)->Hit {
        for(const auto& hit:hits)if(hit.name==name)return hit;
        return {"",{0,0},{0,0},0,true};
    };
    auto click=[&](const std::string& name) {
        const auto hit=find(name);check(!hit.name.empty(),("hit exists: "+name).c_str());
        const ImVec2 point((hit.min.x+hit.max.x)*.5f,(hit.min.y+hit.max.y)*.5f);
        frame(point,false);frame(point,true);frame(point,false);frame();
    };
    auto capture=[&](const std::string& name) {
        for(int i=0;i<3;++i)frame();
        std::size_t count;const auto* all=specs(count);
        for(std::size_t i=0;i<count;++i) if(all[i].kind==Kind::Slider) {
            const auto rail=find(all[i].key),label=find(std::string("label:")+all[i].key);
            if(rail.name.empty()||label.name.empty()) continue;
            const float error=std::fabs((rail.min.y+rail.max.y-label.min.y-label.max.y)*.5f);
            if(error>2*scale) {
                if(aligned) std::printf("Alignment error: %s in %s, %.2f px\n",all[i].key,name.c_str(),error);
                aligned=false;
            }
        }
        context->CopyResource(staging.Get(),target.Get());D3D11_MAPPED_SUBRESOURCE map{};
        if(SUCCEEDED(context->Map(staging.Get(),0,D3D11_MAP_READ,0,&map))) {
            check(save_bmp(out/(name+".bmp"),static_cast<const unsigned char*>(map.pData),w,h,map.RowPitch),"native render saved");
            context->Unmap(staging.Get(),0);
        }else check(false,"native render readback");
    };
    auto choose_level=[&](const char* name) {click("Options");click(std::string("level:")+name);};
    auto key=[&](ImGuiKey k) {io.AddKeyEvent(k,true);frame();io.AddKeyEvent(k,false);frame();};
    auto choose_tab=[&](Tab targetTab) {
        const auto hit=find("tab:Hands");
        const ImVec2 picker(36.f*scale,(hit.min.y+hit.max.y)*.5f);
        frame(picker,false);frame(picker,true);frame(picker,false);frame();
        key(ImGuiKey_Home);
        for(int i=0;i<static_cast<int>(targetTab);++i) key(ImGuiKey_DownArrow);
        frame();frame();key(ImGuiKey_Enter);frame();frame();
        check(state.tab==targetTab,"native overflow picker selects the requested tab");
    };
    frame();frame();
    check(find("SnapAngle").name.empty(),"sections start collapsed");
    capture("controls-collapsed");
    click("section:Turning");
    check(!find("SnapAngle").name.empty(),"opening Turning exposes snap angle");
    click("SnapTurn:0");
    check(backend.read(Setting::SnapTurn,-1)==0 && !find("TurnSpeed").name.empty() && find("SnapAngle").name.empty(),"Smooth shows only its applicable speed");
    click("SnapTurn:1");
    check(backend.read(Setting::SnapTurn,-1)==1,"native Snap button changes model");
    {
        auto hit=find("SnapAngle");const float old=backend.read(Setting::SnapAngle,-1);const int saves=backend.saves;
        const ImVec2 point(hit.min.x+(hit.max.x-hit.min.x)*.8f,(hit.min.y+hit.max.y)*.5f);
        frame(point,false);frame(point,true);
        check(backend.read(Setting::SnapAngle,-1)!=old,"native pipe slider changes value");
        check(backend.saves==saves,"drag does not save every sample");
        frame(point,false);frame();check(backend.saves==saves+1,"slider saves on its own release");
    }
    backend.set(Setting::SnapAngle,45);
    capture("controls-basic");
    choose_level("Advanced");check(backend.read(Setting::ViewLevel,-1)==1,"Advanced tier selector");
    check(!find("StickDeadzone").name.empty(),"Advanced reveals deadzone");
    choose_tab(Tab::HUD);click("section:HUD placement");
    check(!find("HudDistance").name.empty(),"Advanced HUD placement exists");
    choose_level("Basic");check(find("HudDistance").name.empty(),"Basic hides advanced HUD sliders");
    choose_level("Debug");
    // The native overflow picker must reach tabs that are outside the strip.
    choose_tab(Tab::Diagnostics);
    check(state.tab==Tab::Diagnostics,"overflow picker reaches Diagnostics with keyboard");
    check(find("section:Legacy arm solver").name.empty(),"mode 4 legacy sections absent from actual UI");
    choose_level("Basic");check(state.tab==Tab::Controls,"leaving Debug returns from hidden tab");
    // Render every player page through the exact view linked into bioshockvr.dll.
    options.expandSections=true;
    for(int tier=0;tier<3;++tier) {
        backend.set(Setting::ViewLevel,static_cast<float>(tier));
        for(int tab=0;tab<(tier==2?9:7);++tab) {
            state.tab=static_cast<Tab>(tab);
            capture(std::string(tab_name(state.tab))+"-"+(tier==0?"Basic":tier==1?"Advanced":"Debug"));
        }
    }
    check(layout,"body and fixed footer remain inside window at every tier");
    check(aligned,"slider rails are vertically centered on their labels on every page");
    // Exercise the LIVE placement path across changes in the eye resolution,
    // in one ImGui session. Fixed-size artwork previews cannot catch this bug.
    options.captureSize=ImVec2(0,0);options.expandSections=false;
    state=ViewState{};backend.set(Setting::ViewLevel,0);
    auto live_frame=[&](float eyeW,float eyeH) {
        io.DisplaySize=ImVec2(eyeW,eyeH);
        for(int i=0;i<4;++i) {
            io.AddMousePosEvent(-1,-1);
            ImGui_ImplDX11_NewFrame();ImGui::NewFrame();draw(backend,state,options);ImGui::Render();
        }
    };
    auto closeEnough=[](float a,float b,float tolerance=2.f) {return std::fabs(a-b)<=tolerance;};
    live_frame(2750,2850);
    check(closeEnough(state.lastPosition.x,1027)&&closeEnough(state.lastPosition.y,1021)&&
        closeEnough(state.lastSize.x,649)&&closeEnough(state.lastSize.y,685),"live placement matches Dishonored's reference eye rectangle");
    const float referenceFont=state.fontPixels;
    const auto referencePosition=state.lastPosition,referenceSize=state.lastSize;
    live_frame(4763,4936); // ~300% pixel count, sqrt(3) per dimension
    check(closeEnough(state.lastPosition.x,referencePosition.x*4763/2750)&&
        closeEnough(state.lastPosition.y,referencePosition.y*4936/2850)&&
        closeEnough(state.lastSize.x,referenceSize.x*4763/2750)&&
        closeEnough(state.lastSize.y,referenceSize.y*4936/2850),"300 percent resolution preserves apparent panel size and position");
    // ImGui snaps raster font sizes to pixels. Compare to the unrounded source
    // size, so the reference frame's rounding error is not multiplied again.
    check(closeEnough(state.fontPixels,19.f*textScale*(649.f/760.f)*(4763.f/2750.f),1.f)&&
        state.fontPixels>referenceFont*1.6f,"text tracks eye resolution without a DPI ceiling");
    live_frame(2750,2850);
    check(closeEnough(state.lastPosition.x,referencePosition.x)&&closeEnough(state.lastSize.x,referenceSize.x),"resolution roundtrip restores geometry");
    auto* window=ImGui::FindWindowByName("BioShock VR settings###B1RSettings");
    ImGui::SetWindowPos(window,ImVec2(700,900),ImGuiCond_Always);
    ImGui::SetWindowSize(window,ImVec2(800,900),ImGuiCond_Always);
    live_frame(2750,2850);
    const auto movedPosition=state.lastPosition,movedSize=state.lastSize;
    const auto movedHit=find("Recenter");
    live_frame(5500,5700);
    const auto scaledHit=find("Recenter");
    check(closeEnough(state.lastPosition.x,movedPosition.x*2)&&closeEnough(state.lastPosition.y,movedPosition.y*2)&&
        closeEnough(state.lastSize.x,movedSize.x*2)&&closeEnough(state.lastSize.y,movedSize.y*2),"user-moved and resized panel survives resolution change");
    check(closeEnough(scaledHit.min.x,movedHit.min.x*2)&&closeEnough(scaledHit.min.y,movedHit.min.y*2,4),"interaction hitboxes scale with the rendered panel");
    live_frame(1920,1080);
    check(state.lastPosition.x>=0&&state.lastPosition.y>=0&&
        state.lastPosition.x+state.lastSize.x<=1920&&state.lastPosition.y+state.lastSize.y<=1080,
        "aspect change leaves the panel inside the eye image");
    check(backend.read(Setting::TextSize,-1)==textScale,"resolution changes preserve the saved text preference");
    check(layout,"live resolution changes keep the footer inside the panel");
    std::printf("PRODUCTION F10 VIEW | ImGui %s | %dx%d | geometry %.2f text %.2f | %d checks, %d failures\n",
        IMGUI_VERSION,w,h,scale,textScale,g_checks,g_failed);
    ImGui_ImplDX11_Shutdown();ImGui::DestroyContext();CoUninitialize();
    return g_failed?1:0;
}
