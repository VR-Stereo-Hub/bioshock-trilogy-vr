#include "app/app.h"
#include "model/launcher.h"
#include "sys/fs.h"
#include "sys/process.h"
#include "sys/support.h"
#include <windows.h>
#include <shellapi.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <stdexcept>

namespace {
using namespace bvr::launcher;
int unsigned_number(const std::wstring& s) {
    if(s.empty()||s.find_first_not_of(L"0123456789")!=std::wstring::npos)throw std::runtime_error("Expected a positive integer.");
    const auto n=std::stoul(s);if(n>100000)throw std::runtime_error("Integer is out of range.");return static_cast<int>(n);
}
int main_impl() {
    const HRESULT co=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
    struct ComScope { HRESULT hr; ~ComScope(){if(SUCCEEDED(hr))CoUninitialize();} } scope{co};
    int argc=0;LPWSTR* argv=CommandLineToArgvW(GetCommandLineW(),&argc);
    if(!argv)return 2;
    struct Args {LPWSTR* p;~Args(){LocalFree(p);}} args{argv};
    Environment env;Choices choices;
    bool applyFlag=false,inspect=false,preview=false,worker=false,support=false;
    int width=app::kWidth,height=app::kHeight;float scale=1;
    std::string operation,renderState,stateName="overview";
    std::wstring renderPath,resultPath,supportOut;
    auto next=[&](int& i){if(++i>=argc)throw std::runtime_error("Missing option value.");return std::wstring(argv[i]);};
    for(int i=1;i<argc;++i) {
        const std::wstring flag=argv[i];
        if(flag==L"--game-dir")env.gameDir=next(i);
        else if(flag==L"--data-dir")env.dataDir=next(i);
        else if(flag==L"--game-ini")env.gameIni=next(i);
        else if(flag==L"--result")resultPath=next(i);
        else if(flag==L"--apply")applyFlag=true;
        else if(flag==L"--worker")worker=true;
        else if(flag==L"--inspect")inspect=true;
        else if(flag==L"--preview")preview=true;
        else if(flag==L"--state")stateName=fs::narrow(next(i));
        else if(flag==L"--support")support=true;
        else if(flag==L"--support-out")supportOut=next(i);
        else if(flag==L"--op")operation=fs::narrow(next(i));
        else if(flag==L"--runtime")choices.runtime=fs::narrow(next(i));
        else if(flag==L"--headset")choices.headset=fs::narrow(next(i));
        else if(flag==L"--width")width=unsigned_number(next(i));
        else if(flag==L"--height")height=unsigned_number(next(i));
        else if(flag==L"--scale"){
            const auto value=next(i);size_t used=0;scale=std::stof(value,&used);
            if(used!=value.size()||!std::isfinite(scale))throw std::runtime_error("Invalid preview scale.");
        }
        else if(flag==L"--size") {
            const auto size=next(i);auto x=size.find(L'x');
            if(x==std::wstring::npos)throw std::runtime_error("Use WIDTHxHEIGHT for --size.");
            choices.width=unsigned_number(size.substr(0,x));choices.height=unsigned_number(size.substr(x+1));
        }else if(flag==L"--set") {
            const auto edit=next(i);const auto eq=edit.find(L'=');
            if(eq==std::wstring::npos)throw std::runtime_error("Use Key=value for --set.");
            const auto number=edit.substr(eq+1);size_t used=0;float value=std::stof(number,&used);
            if(used!=number.size())throw std::runtime_error("Invalid setting value.");
            choices.preferences[fs::narrow(edit.substr(0,eq))]=value;
        }else if(flag==L"--render") {renderState=fs::narrow(next(i));renderPath=next(i);}
        else throw std::runtime_error("Unknown launcher argument: "+fs::narrow(flag));
    }
    auto output=[&](const std::string& text) {
        if(!resultPath.empty()) {
            DWORD err=0;
            if(!fs::write_file_atomic(resultPath,text.data(),text.size(),&err))throw std::runtime_error("Cannot save result: "+fs::narrow(fs::win_error_text(err)));
        }
        if(!worker){process::attach_parent_console();std::printf("%s\n",text.c_str());}
    };
    if(worker&&(!applyFlag||resultPath.empty()))throw std::runtime_error("A worker requires --apply and --result.");
    if((applyFlag?1:0)+(inspect?1:0)+(preview?1:0)+(support?1:0)+(!renderState.empty()?1:0)>1)throw std::runtime_error("Choose one launcher mode.");
    const auto payload=embedded_payload();
    if(!renderState.empty()||preview) {
        const auto& names=fixture_names();
        auto known=[&](const std::string& s){return std::find(names.begin(),names.end(),s)!=names.end();};
        if(preview){if(!known(stateName))throw std::runtime_error("Unknown preview state.");return app::run(fixture(stateName,payload),true);}
        if(renderState!="all"&&!known(renderState))throw std::runtime_error("Unknown render state.");
        fs::make_dirs(renderPath,nullptr);int failures=0;
        const auto list=renderState=="all"?names:std::vector<std::string>{renderState};
        for(const auto& name:list){auto view=fixture(name,payload);std::string why;
            const auto file=fs::join(renderPath,fs::widen(name)+L".png");
            if(!app::render(view,file,width,height,scale,&why)){++failures;output(name+": "+why);}
        }
        output("Rendered "+std::to_string(list.size())+" production launcher states; failures: "+std::to_string(failures));return failures?2:0;
    }
    env=resolve_environment(env);
    if(applyFlag){const auto r=apply(env,payload,operation,choices);output(report_json(r));return r.ok?0:r.error==ERROR_ACCESS_DENIED?3:2;}
    if(inspect){output(detection_json(detect(env,payload),payload));return 0;}
    if(support){const auto r=collect_support(env,supportOut);output(report_json(r));return r.ok?0:2;}
    if(choices.dirty()||!operation.empty())throw std::runtime_error("Settings arguments require --apply.");
    ViewState state;state.payload=payload;state.detection=detect(env,payload);reset_draft(state);
    return app::run(std::move(state));
}
}
int WINAPI wWinMain(HINSTANCE,HINSTANCE,PWSTR,int) {
    try {
        // ImGui's DX11 backend delay-loads the system compiler. A missing
        // compiler gets an explanation instead of a loader crash.
        HMODULE compiler=LoadLibraryExW(L"d3dcompiler_47.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
        if(!compiler){MessageBoxW(nullptr,L"The Windows Direct3D shader compiler is missing. Install Windows updates, then try again.",L"BioShock VR Launcher",MB_OK|MB_ICONERROR);return 2;}
        const int result=main_impl();FreeLibrary(compiler);return result;
    }catch(const std::exception& error){bvr::launcher::process::attach_parent_console();std::fprintf(stderr,"BioShock VR Launcher: %s\n",error.what());return 2;}
}
