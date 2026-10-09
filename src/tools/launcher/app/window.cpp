// Native Win32/D3D11 shell, adapted from Dishonored's launcher architecture.
// Work runs off the UI thread. Elevated workers receive resolved player paths.
#include "app/app.h"
#include "ui/screens.h"
#include "sys/fs.h"
#include "sys/process.h"
#include "sys/support.h"
#include "game/bioshock1r/menu_theme.h"
#include "payload_ids.h"
#include <d3d11.h>
#include <dxgi.h>
#include <shobjidl.h>
#include <wrl/client.h>
#include <imgui.h>
#include <imgui_impl_win32.h>
#include <imgui_impl_dx11.h>
#include <chrono>
#include <future>
#include <iomanip>
#include <locale>
#include <sstream>

using Microsoft::WRL::ComPtr;
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND,UINT,WPARAM,LPARAM);
namespace bvr::launcher::app {
namespace {
struct Window {
    HWND hwnd = nullptr;
    ViewState* state = nullptr;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<IDXGISwapChain> chain;
    ComPtr<ID3D11RenderTargetView> target;
    bool quit = false;
    unsigned resizeW = 0, resizeH = 0;
};
Window* g_window = nullptr;
LRESULT CALLBACK procedure(HWND hwnd,UINT msg,WPARAM w,LPARAM l) {
    if(ImGui_ImplWin32_WndProcHandler(hwnd,msg,w,l))return 1;
    if(msg==WM_SIZE && g_window && w!=SIZE_MINIMIZED){g_window->resizeW=LOWORD(l);g_window->resizeH=HIWORD(l);return 0;}
    if(msg==WM_DPICHANGED){const auto* r=reinterpret_cast<const RECT*>(l);SetWindowPos(hwnd,nullptr,r->left,r->top,r->right-r->left,r->bottom-r->top,SWP_NOZORDER|SWP_NOACTIVATE);return 0;}
    if(msg==WM_GETMINMAXINFO){auto* p=reinterpret_cast<MINMAXINFO*>(l);const float s=GetDpiForWindow(hwnd)/96.f;p->ptMinTrackSize={static_cast<LONG>(960*s),static_cast<LONG>(780*s)};return 0;}
    if(msg==WM_CLOSE && g_window){
        if(g_window->state->busy){g_window->state->notice="Wait for the current operation to finish.";return 0;}
        if(g_window->state->draft.dirty()){g_window->state->confirmClose=true;return 0;}
        g_window->quit=true;return 0;
    }
    if(msg==WM_DESTROY){PostQuitMessage(0);return 0;}
    return DefWindowProcW(hwnd,msg,w,l);
}
bool target(Window& w) {
    ComPtr<ID3D11Texture2D> back;
    return SUCCEEDED(w.chain->GetBuffer(0,IID_PPV_ARGS(&back))) && SUCCEEDED(w.device->CreateRenderTargetView(back.Get(),nullptr,&w.target));
}
std::wstring browse(HWND owner) {
    ComPtr<IFileOpenDialog> dialog;
    if(FAILED(CoCreateInstance(CLSID_FileOpenDialog,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&dialog))))return {};
    DWORD options=0;dialog->GetOptions(&options);dialog->SetOptions(options|FOS_PICKFOLDERS|FOS_FORCEFILESYSTEM);
    dialog->SetTitle(L"Choose BioShock Remastered or its Build\\Final folder");
    if(FAILED(dialog->Show(owner)))return {};
    ComPtr<IShellItem> item;PWSTR path=nullptr;std::wstring result;
    if(SUCCEEDED(dialog->GetResult(&item))&&SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH,&path))){result=path;CoTaskMemFree(path);}return result;
}
void save_log(const Environment& env,const std::string& text) {
    DWORD err=0;if(!fs::make_dirs(env.dataDir,&err))return;
    const auto path=fs::join(env.dataDir,L"bioshockvr-launcher.log");
    std::vector<unsigned char> old;
    if(fs::file_size(path)<2*1024*1024)fs::read_file(path,&old,nullptr);
    const auto line=fs::utc_now_iso()+"\r\n"+text+"\r\n";
    old.insert(old.end(),line.begin(),line.end());fs::write_file_atomic(path,old.data(),old.size(),nullptr);
}
Report operation(const Environment& env,const Payload& payload,const Choices& choices,const std::string& op) {
    Report report=apply(env,payload,op,choices);
    bool recoveryRequired=false;
    for(const auto& s:report.steps)if(s.title=="Restore required")recoveryRequired=true;
    if(!report.ok && report.error==ERROR_ACCESS_DENIED && !process::is_elevated() && !recoveryRequired) {
        const auto dir=fs::join(fs::temp_dir(),L"BioShockVR-Elevation-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64()));
        DWORD err=0;
        if(fs::make_dirs(dir,&err)) {
            const auto result=fs::join(dir,L"result.json");
            const auto args=operation_arguments(env,choices,op)+L" --worker --result "+process::quote_arg(result);
            DWORD code=0;
            if(process::run_self_elevated_wait(args,&code,&err)) {
                std::vector<unsigned char> bytes;
                if(fs::read_file(result,&bytes,&err)&&parse_report(std::string(bytes.begin(),bytes.end()),&report)) {
                    if(code && report.ok)report.fail("Administrator worker failed","The reported exit status did not match the result.");
                }else report.fail("Administrator result unavailable","No verified result was returned. Inspect the game folder's recovery journal.",err);
            }else report.fail(err==ERROR_CANCELLED?"Administrator request cancelled":"Could not request administrator access","No further changes were attempted.",err);
        }else report.fail("Cannot prepare administrator operation",fs::narrow(dir),err);
    }
    save_log(env,report_json(report));return report;
}
struct Work {
    Action action=Action::None;
    Report report;
    Detection detection;
    updates::Check releases;
    std::wstring downloaded;
    std::string notice;
};
}
std::wstring operation_arguments(const Environment& e,const Choices& c,const std::string& op) {
    const auto q=process::quote_arg;
    std::wstring args=L"--apply --op "+q(fs::widen(op))+L" --game-dir "+q(e.gameDir)+L" --data-dir "+q(e.dataDir)+L" --game-ini "+q(e.gameIni);
    if(!c.runtime.empty())args+=L" --runtime "+q(fs::widen(c.runtime));
    if(!c.headset.empty())args+=L" --headset "+q(fs::widen(c.headset));
    if(c.width)args+=L" --size "+std::to_wstring(c.width)+L"x"+std::to_wstring(c.height);
    for(const auto& [key,value]:c.preferences) {
        std::ostringstream out;out.imbue(std::locale::classic());out<<std::setprecision(9)<<value;
        args+=L" --set "+q(fs::widen(key+"="+out.str()));
    }
    return args;
}
int run(ViewState state,bool preview) {
    Window w;w.state=&state;g_window=&w;
    WNDCLASSEXW wc{sizeof(wc)};wc.style=CS_CLASSDC;wc.lpfnWndProc=procedure;wc.hInstance=GetModuleHandleW(nullptr);
    wc.hIcon=LoadIconW(wc.hInstance,MAKEINTRESOURCEW(IDI_LAUNCHER));wc.hIconSm=wc.hIcon;wc.hCursor=LoadCursorW(nullptr,IDC_ARROW);wc.lpszClassName=L"BioShockVRLauncher";
    if(!RegisterClassExW(&wc))return 2;
    ImGui_ImplWin32_EnableDpiAwareness();
    const auto monitor=MonitorFromPoint(POINT{0,0},MONITOR_DEFAULTTOPRIMARY);
    const float initialScale=ImGui_ImplWin32_GetDpiScaleForMonitor(monitor);
    RECT rect{0,0,static_cast<LONG>(kWidth*initialScale),static_cast<LONG>(kHeight*initialScale)};
    AdjustWindowRectEx(&rect,WS_OVERLAPPEDWINDOW,FALSE,0);
    w.hwnd=CreateWindowW(wc.lpszClassName,preview?L"BioShock VR Launcher | Visual review":L"BioShock VR Launcher",WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT,CW_USEDEFAULT,rect.right-rect.left,rect.bottom-rect.top,nullptr,nullptr,wc.hInstance,nullptr);
    if(!w.hwnd)return 2;
    DXGI_SWAP_CHAIN_DESC sd{};sd.BufferCount=2;sd.BufferDesc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;sd.OutputWindow=w.hwnd;sd.SampleDesc.Count=1;sd.Windowed=TRUE;sd.SwapEffect=DXGI_SWAP_EFFECT_DISCARD;
    auto hr=D3D11CreateDeviceAndSwapChain(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&sd,&w.chain,&w.device,nullptr,&w.context);
    if(FAILED(hr))hr=D3D11CreateDeviceAndSwapChain(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&sd,&w.chain,&w.device,nullptr,&w.context);
    if(FAILED(hr)||!target(w)){MessageBoxW(w.hwnd,L"Direct3D 11 is required to draw this launcher.",L"BioShock VR",MB_OK|MB_ICONERROR);DestroyWindow(w.hwnd);return 2;}
    ImGui::CreateContext();auto& io=ImGui::GetIO();io.IniFilename=nullptr;io.LogFilename=nullptr;io.ConfigFlags|=ImGuiConfigFlags_NavEnableKeyboard;
    b1r::menu::theme::initialize(w.device.Get());ImGui_ImplWin32_Init(w.hwnd);ImGui_ImplDX11_Init(w.device.Get(),w.context.Get());
    ShowWindow(w.hwnd,SW_SHOWNORMAL);UpdateWindow(w.hwnd);
    std::future<Work> pending;
    while(!w.quit) {
        MSG msg;
        while(PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)){TranslateMessage(&msg);DispatchMessageW(&msg);if(msg.message==WM_QUIT)w.quit=true;}
        if(w.quit)break;
        if(pending.valid()&&pending.wait_for(std::chrono::milliseconds(0))==std::future_status::ready) {
            try {
                auto done=pending.get();state.busy=false;
                if(done.action==Action::CheckUpdates)state.releases=std::move(done.releases);
                else if(done.action==Action::DownloadUpdate){state.downloaded=std::move(done.downloaded);state.notice=done.notice;}
                else if(done.action==Action::Refresh){state.detection=std::move(done.detection);if(!state.draft.dirty())reset_draft(state);}
                else {
                    state.report=std::move(done.report);state.page=Page::Result;
                    if(done.action!=Action::Support)state.detection=std::move(done.detection);
                    if(state.report.ok&&(done.action==Action::Apply||done.action==Action::Install||done.action==Action::Defaults))reset_draft(state);
                    state.notice=state.report.ok?"Operation complete. Your next launch will use the saved settings.":"Operation failed. See the details above.";
                }
            }catch(const std::exception& e){state.busy=false;state.report.fail("Operation failed",e.what());state.page=Page::Result;}
        }
        if(IsIconic(w.hwnd)){Sleep(30);continue;}
        if(w.resizeW&&w.resizeH){
            w.context->OMSetRenderTargets(0,nullptr,nullptr);w.target.Reset();
            if(FAILED(w.chain->ResizeBuffers(0,w.resizeW,w.resizeH,DXGI_FORMAT_UNKNOWN,0))||!target(w)){w.quit=true;break;}
            w.resizeW=w.resizeH=0;
        }
        state.scale=GetDpiForWindow(w.hwnd)/96.f;
        ImGui_ImplDX11_NewFrame();ImGui_ImplWin32_NewFrame();ImGui::NewFrame();
        const auto action=ui::draw(state);ImGui::Render();
        auto* rtv=w.target.Get();w.context->OMSetRenderTargets(1,&rtv,nullptr);const float clear[]={.02f,.05f,.06f,1};w.context->ClearRenderTargetView(rtv,clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        hr=w.chain->Present(1,0);if(hr==DXGI_ERROR_DEVICE_REMOVED||hr==DXGI_ERROR_DEVICE_RESET){w.quit=true;break;}
        if(action==Action::None)continue;
        if(action==Action::Close){w.quit=true;continue;}
        if(preview){state.notice="Visual review mode. Installation, launch and file actions are disabled.";continue;}
        const auto env=state.detection.env;
        if(action==Action::Browse) {
            auto folder=browse(w.hwnd);if(folder.empty())continue;
            auto normalized=normalise_game_dir(folder);
            if(normalized.empty()){state.notice="That folder does not contain the 32 bit BioShock Remastered executable.";continue;}
            state.detection.env.gameDir=normalized;
        }
        if(action==Action::GameFolder||action==Action::DataFolder||action==Action::BackupFolder||action==Action::Releases) {
            auto path=action==Action::GameFolder?env.gameDir:action==Action::DataFolder?env.dataDir:action==Action::BackupFolder?state.report.backupDir:kReleasesUrl;
            if(!process::open_unelevated(path,L"",L""))state.notice="Windows could not open that location.";continue;
        }
        if(action==Action::Play) {
            const auto fresh=detect(env,state.payload);
            if(!fresh.found||!fresh.installed||fresh.oldModConflict||fresh.running!=process::Running::No){state.notice="The game is missing, already running or the mod installation needs attention. Rescan first.";continue;}
            if(!process::open_unelevated(kSteamUrl,L"",L""))state.notice="Open Steam and launch BioShock Remastered there.";
            else state.notice=fresh.disabled?"Opening the game with VR disabled.":"Opening BioShock Remastered through Steam.";
            continue;
        }
        if(action==Action::DesktopShortcut||action==Action::StartShortcut) {
            auto folder=fs::known_folder(action==Action::DesktopShortcut?FOLDERID_Desktop:FOLDERID_Programs);
            if(action==Action::StartShortcut)folder=fs::join(folder,L"BioShock VR");DWORD err=0;
            const auto args=L"--game-dir "+process::quote_arg(env.gameDir)+L" --data-dir "+process::quote_arg(env.dataDir)+L" --game-ini "+process::quote_arg(env.gameIni);
            if(fs::make_dirs(folder,&err)&&process::write_shortcut(fs::join(folder,L"BioShock VR.lnk"),fs::module_path(),args,&err))state.notice="Launcher shortcut created.";
            else state.notice="Could not create a shortcut: "+fs::narrow(fs::win_error_text(err));continue;
        }
        if(action==Action::OpenUpdate) {
            if(state.releases.releases.empty()){state.notice="Check releases again before opening the download.";continue;}
            std::string why;const auto& release=state.releases.releases.front();
            if(!updates::verify(state.downloaded,release,&why))state.notice=why;
            else {
                DWORD err=0;const auto args=L"--game-dir "+process::quote_arg(env.gameDir)+L" --data-dir "+process::quote_arg(env.dataDir)+L" --game-ini "+process::quote_arg(env.gameIni);
                if(updates::start(state.downloaded,args,&err))w.quit=true;else state.notice="Cannot open the downloaded launcher: "+fs::narrow(fs::win_error_text(err));
            }continue;
        }
        if(state.busy)continue;
        state.busy=true;state.busyText=action==Action::CheckUpdates?"Checking the project's release history...":
            action==Action::DownloadUpdate?"Downloading and verifying the new launcher...":action==Action::Support?"Collecting logs and settings into a local support bundle...":"Checking files, saving backups and applying the operation...";
        const auto selectedEnv=state.detection.env;const auto payload=state.payload;
        const auto choices=(action==Action::Apply||action==Action::Install)?state.draft:Choices{};
        const auto releases=state.releases.releases;
        pending=std::async(std::launch::async,[=]() {
            const HRESULT com=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
            struct ComScope { HRESULT hr; ~ComScope(){if(SUCCEEDED(hr))CoUninitialize();} } comScope{com};
            Work result;result.action=action;
            if(action==Action::CheckUpdates)result.releases=updates::check();
            else if(action==Action::DownloadUpdate){
                if(releases.empty())result.notice="Check for updates first.";
                else if(updates::download(releases.front(),&result.downloaded,&result.notice))result.notice="Download verified. Open it to review and install the new build.";
            }else if(action==Action::Support)result.report=collect_support(selectedEnv);
            else if(action!=Action::Refresh&&action!=Action::Browse){
                const char* op=action==Action::Install?"install":action==Action::Reinstall?"update":action==Action::Apply?"settings":
                    action==Action::Disable?"disable":action==Action::Enable?"enable":action==Action::Uninstall?"uninstall":"defaults";
                result.report=operation(selectedEnv,payload,choices,op);
            }
            if(action==Action::Browse)result.action=Action::Refresh;
            if(action!=Action::CheckUpdates&&action!=Action::DownloadUpdate&&action!=Action::Support)result.detection=detect(selectedEnv,payload);
            return result;
        });
    }
    // Closing is disabled during mutation; a graphics failure still waits for
    // its worker so a transaction cannot be killed halfway through.
    if(pending.valid())pending.wait();
    ImGui_ImplDX11_Shutdown();ImGui_ImplWin32_Shutdown();ImGui::DestroyContext();
    w.target.Reset();w.chain.Reset();w.context.Reset();w.device.Reset();DestroyWindow(w.hwnd);UnregisterClassW(wc.lpszClassName,wc.hInstance);g_window=nullptr;
    return 0;
}
}
