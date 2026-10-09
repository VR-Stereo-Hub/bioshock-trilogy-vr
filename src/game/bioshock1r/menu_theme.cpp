// Approved Rapture artwork and native ImGui interaction, shared by the DLL and preview.
#include "menu_theme.h"
#include <windows.h>
#include <d3d11.h>
#include <wincodec.h>
#include <shlwapi.h>
#include <wrl/client.h>
#include <imgui_internal.h>
#include <algorithm>
#include <cstdio>
#include <vector>
#undef RGB
using Microsoft::WRL::ComPtr;
namespace bvr::b1r::menu::theme {
namespace {
static ImFont* g_title;
static ImFont* g_small;
static ImFont* g_body;
struct Art
{
    ComPtr<ID3D11ShaderResourceView> texture;
    float width=1, height=1;
    ImVec2 uv0{0,0}, uv1{1,1};
};
static Art g_art, g_banner, g_button, g_selected, g_pipe, g_water;
static float g_scale = 1;
static float g_text = 1;
static ImVec4 RGB(int r, int g, int b, float a = 1)
{
    return ImVec4(r/255.f, g/255.f, b/255.f, a);
}
static const ImVec4 kIvory = RGB(236, 225, 197);
static const ImVec4 kMuted = RGB(166, 178, 166);
static const ImVec4 kBrass = RGB(191, 157, 94);
static const ImVec4 kDark = RGB(15, 28, 29);
static float U(float n) { return n * g_scale; }
static ImVec2 Center()
{
    auto a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    return ImVec2((a.x+b.x)*.5f, (a.y+b.y)*.5f);
}
static void Theme()
{
    ImGui::StyleColorsDark();
    auto& s = ImGui::GetStyle();
    s.WindowPadding = ImVec2(30, 27);
    s.FramePadding = ImVec2(12, 8);
    s.ItemSpacing = ImVec2(10, 11);
    s.ItemInnerSpacing = ImVec2(9, 6);
    s.WindowRounding = s.FrameRounding = s.TabRounding = 0;
    s.WindowBorderSize = 0;
    s.FrameBorderSize = 1;
    s.ScrollbarSize = 14;
    s.GrabMinSize = 18;
    s.GrabRounding = 0;
    s.Colors[ImGuiCol_Text] = kIvory;
    s.Colors[ImGuiCol_TextDisabled] = kMuted;
    s.Colors[ImGuiCol_WindowBg] = kDark;
    s.Colors[ImGuiCol_ChildBg] = ImVec4(0,0,0,0);
    s.Colors[ImGuiCol_Border] = RGB(92, 106, 91);
    s.Colors[ImGuiCol_Button] = RGB(25, 45, 46);
    s.Colors[ImGuiCol_ButtonHovered] = RGB(43, 66, 62);
    s.Colors[ImGuiCol_ButtonActive] = RGB(63, 88, 79);
    s.Colors[ImGuiCol_FrameBg] = RGB(19, 37, 38);
    s.Colors[ImGuiCol_FrameBgHovered] = RGB(36, 61, 58);
    s.Colors[ImGuiCol_FrameBgActive] = RGB(46, 74, 67);
    s.Colors[ImGuiCol_SliderGrab] = kBrass;
    s.Colors[ImGuiCol_SliderGrabActive] = RGB(228, 197, 136);
    s.Colors[ImGuiCol_CheckMark] = kBrass;
    s.Colors[ImGuiCol_CheckboxSelectedBg] = RGB(25,45,46);
    s.Colors[ImGuiCol_NavCursor] = RGB(235, 206, 151);
    s.Colors[ImGuiCol_ScrollbarGrab] = RGB(114, 105, 77);
    s.Colors[ImGuiCol_ScrollbarBg] = RGB(12, 22, 23);
    s.Colors[ImGuiCol_Header] = RGB(181, 165, 127);
    s.Colors[ImGuiCol_HeaderHovered] = RGB(194, 173, 129);
    s.Colors[ImGuiCol_HeaderActive] = RGB(213, 188, 138);
    s.Colors[ImGuiCol_Separator] = RGB(103, 90, 63);
    s.Colors[ImGuiCol_Tab] = RGB(17, 34, 35);
    s.Colors[ImGuiCol_TabHovered] = RGB(69, 71, 52);
    s.Colors[ImGuiCol_TabSelected] = RGB(76, 70, 49);
    s.Colors[ImGuiCol_TabSelectedOverline] = kBrass;
    s.ScaleAllSizes(g_scale);
    s.FontScaleMain = g_scale * g_text;
}
static void LoadArt(ID3D11Device* device, int resourceId, Art& out)
{
    ComPtr<IWICImagingFactory> factory;
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICFormatConverter> converter;
    HMODULE module = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
        GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&LoadArt), &module);
    HRSRC resource = FindResourceW(module, MAKEINTRESOURCEW(resourceId), MAKEINTRESOURCEW(10));
    if (!resource) return;
    const auto bytes = SizeofResource(module, resource);
    const auto data = static_cast<const BYTE*>(LockResource(LoadResource(module,resource)));
    if (!bytes || !data) return;
    ComPtr<IStream> stream;
    stream.Attach(SHCreateMemStream(data,bytes));
    if (!stream || FAILED(CoCreateInstance(CLSID_WICImagingFactory,nullptr,
        CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&factory))) ||
        FAILED(factory->CreateDecoderFromStream(stream.Get(),nullptr,
        WICDecodeMetadataCacheOnLoad,&decoder)) || FAILED(decoder->GetFrame(0,&frame)) ||
        FAILED(factory->CreateFormatConverter(&converter)) ||
        FAILED(converter->Initialize(frame.Get(),GUID_WICPixelFormat32bppRGBA,
        WICBitmapDitherTypeNone,nullptr,0,WICBitmapPaletteTypeCustom))) return;
    UINT w=0,h=0;
    converter->GetSize(&w,&h);
    if (!w || !h || w>4096 || h>4096) return;
    std::vector<unsigned char> pixels(w*h*4);
    if (FAILED(converter->CopyPixels(nullptr,w*4,(UINT)pixels.size(),pixels.data()))) return;
    D3D11_TEXTURE2D_DESC d{};
    d.Width=w; d.Height=h; d.MipLevels=1; d.ArraySize=1;
    d.Format=DXGI_FORMAT_R8G8B8A8_UNORM; d.SampleDesc.Count=1;
    d.Usage=D3D11_USAGE_IMMUTABLE; d.BindFlags=D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA initial{pixels.data(),w*4,0};
    ComPtr<ID3D11Texture2D> t;
    if (SUCCEEDED(device->CreateTexture2D(&d,&initial,&t)))
        device->CreateShaderResourceView(t.Get(),nullptr,&out.texture);
    // Ignore transparent export padding without altering the source asset.
    UINT left=w,top=h,right=0,bottom=0;
    for(UINT y=0;y<h;++y)for(UINT x=0;x<w;++x)
        if(pixels[(y*w+x)*4+3]>8)
        {left=std::min(left,x);top=std::min(top,y);right=std::max(right,x+1);bottom=std::max(bottom,y+1);}
    if(left<right && top<bottom)
    {
        out.width=(float)(right-left);out.height=(float)(bottom-top);
        out.uv0=ImVec2((float)left/w,(float)top/h);
        out.uv1=ImVec2((float)right/w,(float)bottom/h);
    }

}
static void Material(const Art& art, ImVec2 a, ImVec2 b, ImU32 tint=IM_COL32_WHITE, float capRatio=.17f)
{
    tint=ImGui::GetColorU32(tint);
    auto* dl=ImGui::GetWindowDrawList();
    if(!art.texture){dl->AddRectFilled(a,b,ImGui::GetColorU32(kBrass));return;}
    const float cap=std::min((b.x-a.x)*.32f,(b.y-a.y)*art.width*capRatio/art.height);
    const float u1=art.uv0.x+(art.uv1.x-art.uv0.x)*capRatio;
    const float u2=art.uv1.x-(art.uv1.x-art.uv0.x)*capRatio;
    ImTextureID id=(ImTextureID)(uintptr_t)art.texture.Get();
    dl->AddImage(id,a,ImVec2(a.x+cap,b.y),art.uv0,ImVec2(u1,art.uv1.y),tint);
    dl->AddImage(id,ImVec2(a.x+cap,a.y),ImVec2(b.x-cap,b.y),ImVec2(u1,art.uv0.y),ImVec2(u2,art.uv1.y),tint);
    dl->AddImage(id,ImVec2(b.x-cap,a.y),b,ImVec2(u2,art.uv0.y),art.uv1,tint);
}
// Native ImGui owns hitboxes, focus, navigation and IDs; art only changes drawing.
struct Skin
{
    ImDrawList* dl=ImGui::GetWindowDrawList();
    ImDrawListSplitter split;
    Skin(){split.Split(dl,2);split.SetCurrentChannel(dl,1);}
    void Finish(const Art& art,ImVec2 a,ImVec2 b,ImU32 tint=IM_COL32_WHITE)
    {
        split.SetCurrentChannel(dl,0);Material(art,a,b,tint);split.Merge(dl);
    }
};
static bool PaintedButton(const char* label, ImVec2 size, bool selected=false)
{
    Skin skin;
    ImGui::PushStyleColor(ImGuiCol_Button,ImVec4(0,0,0,0));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered,ImVec4(1,1,1,.10f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,ImVec4(1,1,1,.16f));
    ImGui::PushStyleColor(ImGuiCol_Text,selected?kDark:kIvory);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize,0);
    bool changed=ImGui::Button(label,size);
    skin.Finish(selected?g_selected:g_button,ImGui::GetItemRectMin(),ImGui::GetItemRectMax());
    ImGui::PopStyleVar();ImGui::PopStyleColor(4);
    return changed;
}
static bool PaintedSlider(const char* id,float* value,float min,float max,const char* format)
{
    Skin skin;
    ImVec2 start=ImGui::GetCursorScreenPos();
    const float width=ImGui::CalcItemWidth();
    char text[100];
    // A shared value column keeps every rail's right edge aligned, regardless
    // of its units or the number of digits in the current value.
    const float readout=U(90)*g_text;
    const float pipeWidth=std::max(U(60),width-readout);
    const float pipeHeight=std::max(U(39),ImGui::GetFontSize()+U(8));
    const float cap=std::min(pipeWidth*.2f,pipeHeight*g_pipe.width*.13f/g_pipe.height);
    ImGui::SetCursorScreenPos(ImVec2(start.x+cap,start.y));
    ImGui::SetNextItemWidth(pipeWidth-cap*2);
    ImGui::PushStyleColor(ImGuiCol_FrameBg,ImVec4(0,0,0,0));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered,ImVec4(0,0,0,0));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive,ImVec4(0,0,0,0));
    ImGui::PushStyleColor(ImGuiCol_SliderGrab,ImVec4(0,0,0,0));
    ImGui::PushStyleColor(ImGuiCol_SliderGrabActive,ImVec4(0,0,0,0));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize,0);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,ImVec2(U(12),(pipeHeight-ImGui::GetFontSize())*.5f));
    // Preserve Ctrl+click exact input and keyboard navigation. The regular
    // centered slider number is hidden because the pipe has an inline readout.
    const bool typing=ImGui::TempInputIsActive(ImGui::GetID(id));
    ImGui::PushStyleColor(ImGuiCol_Text,typing?kIvory:ImVec4(0,0,0,0));
    bool changed=ImGui::SliderFloat(id,value,min,max,format,ImGuiSliderFlags_AlwaysClamp);
    ImGui::PopStyleColor();
    auto* dl=skin.dl;
    skin.split.SetCurrentChannel(dl,0);
    ImVec2 a(start.x,start.y),b(start.x+pipeWidth,start.y+pipeHeight);
    float left=a.x+cap*.94f,right=b.x-cap*.94f;
    float top=a.y+pipeHeight*.20f,bottom=b.y-pipeHeight*.20f;
    float fill=left+(right-left)*std::clamp((*value-min)/(max-min),0.f,1.f);
    dl->AddRectFilled(ImVec2(left,top),ImVec2(right,bottom),IM_COL32(8,25,29,255));
    if(fill>left)dl->AddRectFilledMultiColor(ImVec2(left,top),ImVec2(fill,bottom),
        IM_COL32(65,160,173,255),IM_COL32(65,160,173,255),IM_COL32(17,72,93,255),IM_COL32(17,72,93,255));
    if(g_water.texture)
    {
        const float tile=(bottom-top)*g_water.width/g_water.height;
        for(float x=left;x<fill;x+=tile)
        {
            float edge=std::min(x+tile,fill);
            dl->AddImage((ImTextureID)(uintptr_t)g_water.texture.Get(),ImVec2(x,top),ImVec2(edge,bottom),ImVec2(0,0),ImVec2((edge-x)/tile,1),ImGui::GetColorU32(IM_COL32_WHITE));
        }
    }
    Material(g_pipe,a,b,IM_COL32_WHITE,.13f);
    if(fill>left){dl->AddLine(ImVec2(fill,top),ImVec2(fill,bottom),IM_COL32(182,236,232,255),U(2));}
    // Disabled controls fade as a whole, including their image-based artwork.
    // Native widgets above already inherit ImGui's disabled interaction state.
    sprintf_s(text,format,*value);
    dl->AddText(ImVec2(start.x+width-ImGui::CalcTextSize(text).x,
        start.y+(pipeHeight-ImGui::GetFontSize())*.5f),ImGui::GetColorU32(kIvory),text);
    skin.split.Merge(dl);
    ImGui::PopStyleVar(2);ImGui::PopStyleColor(5);
    return changed;
}
static void FrameArt(float w, float h)
{
    auto* d=ImGui::GetWindowDrawList();
    const auto origin=ImGui::GetWindowPos();
    const int first=d->VtxBuffer.Size;
    if (g_art.texture) d->AddImage((ImTextureID)(uintptr_t)g_art.texture.Get(),ImVec2(0,0),ImVec2(w,h));
    d->AddRectFilledMultiColor(ImVec2(U(22),U(210)),ImVec2(w-U(22),h-U(22)),
        IM_COL32(5,15,16,0),IM_COL32(5,15,16,0),IM_COL32(5,15,16,100),IM_COL32(5,15,16,100));
    auto gold=ImGui::GetColorU32(RGB(157,129,75,.8f));
    auto dim=ImGui::GetColorU32(RGB(113,98,67,.65f));
    d->AddRect(ImVec2(U(9),U(9)),ImVec2(w-U(9),h-U(9)),dim,0,0,U(1));
    d->AddRect(ImVec2(U(15),U(15)),ImVec2(w-U(15),h-U(15)),dim,0,0,U(1));
    for (int side=0;side<4;++side)
    {
        float x=side%2 ? w : 0, y=side/2 ? h : 0;
        float sx=side%2 ? -1.f : 1.f, sy=side/2 ? -1.f : 1.f;
        for (int i=0;i<3;++i)
        {
            float off=U(21.f+i*5.f), len=U(44.f-i*10.f);
            d->AddLine(ImVec2(x+sx*off,y+sy*(off+len)),ImVec2(x+sx*off,y+sy*off),gold,U(1));
            d->AddLine(ImVec2(x+sx*off,y+sy*off),ImVec2(x+sx*(off+len),y+sy*off),gold,U(1));
        }
    }
    for(int i=first;i<d->VtxBuffer.Size;++i) {
        d->VtxBuffer[i].pos.x += origin.x;
        d->VtxBuffer[i].pos.y += origin.y;
    }
}
static void Rule()
{
    auto p=ImGui::GetCursorScreenPos();
    float w=ImGui::GetContentRegionAvail().x, mid=p.x+w*.5f;
    auto* d=ImGui::GetWindowDrawList();
    auto c=ImGui::GetColorU32(kBrass);
    d->AddLine(ImVec2(p.x,p.y+U(5)),ImVec2(mid-U(24),p.y+U(5)),c,U(1));
    d->AddLine(ImVec2(mid+U(24),p.y+U(5)),ImVec2(p.x+w,p.y+U(5)),c,U(1));
    for(int i=-2;i<=2;++i)
    {
        float height=U(8.f-abs(i)*2.f);
        d->AddRectFilled(ImVec2(mid+U(i*5.f)-U(1),p.y+U(5)-height),
            ImVec2(mid+U(i*5.f)+U(1),p.y+U(8)),c);
    }
    ImGui::Dummy(ImVec2(w,U(13)));
}
static void Small(const char* text)
{
    ImGui::PushFont(g_small,14);
    ImGui::PushStyleColor(ImGuiCol_Text,kMuted);
    ImGui::TextWrapped("%s",text);
    ImGui::PopStyleColor(); ImGui::PopFont();
}
static bool Section(const char* label, bool open)
{
    Skin skin;
    ImGui::PushStyleColor(ImGuiCol_Text,ImVec4(0,0,0,0));
    ImGui::PushStyleColor(ImGuiCol_Header,ImVec4(0,0,0,0));
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered,ImVec4(1,1,1,.1f));
    ImGui::PushStyleColor(ImGuiCol_HeaderActive,ImVec4(1,1,1,.16f));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,ImVec2(U(14),U(11)));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize,0);
    bool result=ImGui::CollapsingHeader(label,open?ImGuiTreeNodeFlags_DefaultOpen:0);
    auto a=ImGui::GetItemRectMin(),b=ImGui::GetItemRectMax();
    skin.Finish(g_banner,a,b);
    auto* dl=ImGui::GetWindowDrawList();
    float center=a.y+(b.y-a.y)*.5f;
    const ImU32 ink=ImGui::GetColorU32(kDark);
    float fontSize=ImGui::GetFontSize();
    float available=b.x-a.x-U(144);
    float measured=ImGui::CalcTextSize(label).x;
    if(measured>available) fontSize*=available/measured;
    dl->AddText(ImGui::GetFont(),fontSize,ImVec2(a.x+U(72),center-fontSize*.5f),ink,label);
    float x=a.x+U(57),r=U(5);
    if(result)dl->AddTriangleFilled(ImVec2(x-r,center-r*.5f),ImVec2(x+r,center-r*.5f),ImVec2(x,center+r),ink);
    else dl->AddTriangleFilled(ImVec2(x-r*.5f,center-r),ImVec2(x-r*.5f,center+r),ImVec2(x+r,center),ink);
    ImGui::PopStyleVar(2);ImGui::PopStyleColor(4);
    return result;
}

static ImGuiStyle g_savedStyle;
static float g_savedGlobalScale;
} // namespace
void initialize(ID3D11Device* device) {
    // The standalone launcher can render several independent ImGui contexts.
    // A missing optional font must fall back to this context, never the last.
    g_body=g_title=g_small=nullptr;
    auto& io=ImGui::GetIO();
    char windows[MAX_PATH]{};
    GetWindowsDirectoryA(windows,MAX_PATH);
    char font[MAX_PATH]{};
    sprintf_s(font,"%s/Fonts/georgia.ttf",windows);
    if(GetFileAttributesA(font)!=INVALID_FILE_ATTRIBUTES) g_body=io.Fonts->AddFontFromFileTTF(font,19);
    if(!g_body) g_body=io.Fonts->AddFontDefault();
    sprintf_s(font,"%s/Fonts/PERTILI.TTF",windows);
    if(GetFileAttributesA(font)!=INVALID_FILE_ATTRIBUTES) g_title=io.Fonts->AddFontFromFileTTF(font,48);
    if(!g_title) g_title=g_body;
    sprintf_s(font,"%s/Fonts/segoeui.ttf",windows);
    if(GetFileAttributesA(font)!=INVALID_FILE_ATTRIBUTES) g_small=io.Fonts->AddFontFromFileTTF(font,14);
    if(!g_small) g_small=g_body;
    const HRESULT com=CoInitializeEx(nullptr,COINIT_MULTITHREADED);
    LoadArt(device,301,g_art);LoadArt(device,302,g_banner);
    LoadArt(device,303,g_button);LoadArt(device,304,g_selected);
    LoadArt(device,305,g_pipe);LoadArt(device,306,g_water);
    if(SUCCEEDED(com)) CoUninitialize();
}
void begin(float scale,float textScale) {
    g_savedStyle=ImGui::GetStyle();
    g_savedGlobalScale=ImGui::GetIO().FontGlobalScale;
    ImGui::GetIO().FontGlobalScale=1;
    g_scale=scale;g_text=textScale;Theme();
    ImGui::PushFont(g_body,19);
}
void end() {
    ImGui::PopFont();
    ImGui::GetStyle()=g_savedStyle;
    ImGui::GetIO().FontGlobalScale=g_savedGlobalScale;
}
void background(){auto s=ImGui::GetWindowSize();FrameArt(s.x,s.y);}
void title(){
    const float size=std::min(U(48),(ImGui::GetWindowSize().x-U(90))/7.8f);
    ImGui::PushFont(g_title,size/(g_scale*g_text));
    ImGui::TextUnformatted("BIOSHOCK VR");ImGui::PopFont();
}
void subtitle(){
    ImGui::PushFont(g_small,14);
    ImGui::TextColored(kBrass,"R A P T U R E   /   V R   S E T T I N G S");
    ImGui::PopFont();
}
void rule(){Rule();}
void note(const char* text){Small(text);}
bool button(const char* label,ImVec2 size,bool selected){return PaintedButton(label,size,selected);}
bool slider(const char* id,float* value,float minimum,float maximum,const char* format){return PaintedSlider(id,value,minimum,maximum,format);}
bool section(const char* label){return Section(label,false);}
float unit(float value){return U(value);}
bool assets_loaded(){return g_art.texture&&g_banner.texture&&g_button.texture&&g_selected.texture&&g_pipe.texture&&g_water.texture;}
} // namespace bvr::b1r::menu::theme
