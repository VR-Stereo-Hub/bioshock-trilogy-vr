// Renders the production view with D3D11 and captures it through WIC, following
// the Dishonored launcher. No game process or user settings are touched.
#include "app/app.h"
#include "ui/screens.h"
#include "game/bioshock1r/menu_theme.h"
#include "sys/fs.h"
#include <d3d11.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <json/json.h>

using Microsoft::WRL::ComPtr;
namespace bvr::launcher::app {
namespace {
bool png(const std::wstring& path,const void* rgba,UINT width,UINT height,UINT pitch,std::string* why) {
    ComPtr<IWICImagingFactory> factory;ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapEncoder> encoder;ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IWICBitmap> source;ComPtr<IWICFormatConverter> convert;
    HRESULT hr=CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&factory));
    if(SUCCEEDED(hr))hr=factory->CreateStream(&stream);
    if(SUCCEEDED(hr))hr=stream->InitializeFromFilename(path.c_str(),GENERIC_WRITE);
    if(SUCCEEDED(hr))hr=factory->CreateEncoder(GUID_ContainerFormatPng,nullptr,&encoder);
    if(SUCCEEDED(hr))hr=encoder->Initialize(stream.Get(),WICBitmapEncoderNoCache);
    if(SUCCEEDED(hr))hr=encoder->CreateNewFrame(&frame,nullptr);
    if(SUCCEEDED(hr))hr=frame->Initialize(nullptr);
    if(SUCCEEDED(hr))hr=frame->SetSize(width,height);
    WICPixelFormatGUID format=GUID_WICPixelFormat32bppBGRA;
    if(SUCCEEDED(hr))hr=frame->SetPixelFormat(&format);
    if(SUCCEEDED(hr))hr=factory->CreateBitmapFromMemory(width,height,GUID_WICPixelFormat32bppRGBA,pitch,pitch*height,const_cast<BYTE*>(static_cast<const BYTE*>(rgba)),&source);
    if(SUCCEEDED(hr))hr=factory->CreateFormatConverter(&convert);
    if(SUCCEEDED(hr))hr=convert->Initialize(source.Get(),format,WICBitmapDitherTypeNone,nullptr,0,WICBitmapPaletteTypeCustom);
    if(SUCCEEDED(hr))hr=frame->WriteSource(convert.Get(),nullptr);
    if(SUCCEEDED(hr))hr=frame->Commit();if(SUCCEEDED(hr))hr=encoder->Commit();
    if(FAILED(hr)){*why=fs::format("PNG capture failed (0x%08lx)",hr);return false;}return true;
}
}
bool render(ViewState& state,const std::wstring& path,int width,int height,float scale,std::string* why) {
    if(width<960||height<740||scale<.75f||scale>2.5f){*why="Unsupported preview dimensions.";return false;}
    width=static_cast<int>(width*scale+.5f);height=static_cast<int>(height*scale+.5f);state.scale=scale;
    ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;
    HRESULT hr=D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&context);
    if(FAILED(hr))hr=D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&context);
    if(FAILED(hr)){*why="Cannot create a D3D11 preview device.";return false;}
    D3D11_TEXTURE2D_DESC td{};td.Width=width;td.Height=height;td.MipLevels=td.ArraySize=1;td.SampleDesc.Count=1;
    td.Format=DXGI_FORMAT_R8G8B8A8_UNORM;td.BindFlags=D3D11_BIND_RENDER_TARGET;
    ComPtr<ID3D11Texture2D> target,staging;ComPtr<ID3D11RenderTargetView> view;
    hr=device->CreateTexture2D(&td,nullptr,&target);
    if(SUCCEEDED(hr))hr=device->CreateRenderTargetView(target.Get(),nullptr,&view);
    td.BindFlags=0;td.Usage=D3D11_USAGE_STAGING;td.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    if(SUCCEEDED(hr))hr=device->CreateTexture2D(&td,nullptr,&staging);
    if(FAILED(hr)){*why="Cannot allocate a preview surface.";return false;}
    ImGui::CreateContext();auto& io=ImGui::GetIO();io.IniFilename=nullptr;io.LogFilename=nullptr;
    io.DisplaySize=ImVec2(static_cast<float>(width),static_cast<float>(height));io.MousePos=ImVec2(-100,-100);
    io.ConfigFlags|=ImGuiConfigFlags_NavEnableKeyboard;
    b1r::menu::theme::initialize(device.Get());
    if(!b1r::menu::theme::assets_loaded()){*why="The F10 theme assets did not load.";ImGui::DestroyContext();return false;}
    if(!ImGui_ImplDX11_Init(device.Get(),context.Get())){*why="Cannot initialize the preview renderer.";ImGui::DestroyContext();return false;}
    for(int i=0;i<5;++i) {
        io.DeltaTime=1.f/60;ImGui_ImplDX11_NewFrame();ImGui::NewFrame();ui::draw(state);ImGui::Render();
        auto* rtv=view.Get();context->OMSetRenderTargets(1,&rtv,nullptr);const float clear[]={.02f,.05f,.06f,1};context->ClearRenderTargetView(rtv,clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    }
    context->CopyResource(staging.Get(),target.Get());D3D11_MAPPED_SUBRESOURCE mapped{};
    bool ok=false;
    if(SUCCEEDED(context->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped))) {
        ok=png(path,mapped.pData,width,height,mapped.RowPitch,why);context->Unmap(staging.Get(),0);
    }else *why="Cannot read the preview surface.";
    Json::Value metrics;metrics["width"]=width;metrics["height"]=height;metrics["scale"]=scale;metrics["layoutErrors"]=state.layoutErrors;
    metrics["themeAssetsLoaded"]=b1r::menu::theme::assets_loaded();
    for(const auto& b:state.hitboxes) {Json::Value box;box["label"]=b.label;box["x"]=b.x;box["y"]=b.y;box["w"]=b.w;box["h"]=b.h;box["enabled"]=b.enabled;metrics["buttons"].append(box);}
    Json::StreamWriterBuilder writer;writer["indentation"]="  ";auto data=Json::writeString(writer,metrics);
    if(!fs::write_file_atomic(path+L".json",data.data(),data.size(),nullptr)){ok=false;*why="Cannot save layout metrics.";}
    if(state.layoutErrors){ok=false;*why="A visible control escaped the window.";}
    ImGui_ImplDX11_Shutdown();ImGui::DestroyContext();return ok;
}
}
