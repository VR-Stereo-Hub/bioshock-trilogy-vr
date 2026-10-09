#include "sys/support.h"
#include "sys/resources.h"
#include "sys/fs.h"
#include "sys/process.h"
#include "payload_ids.h"

namespace bvr::launcher {
Report collect_support(const Environment& env,const std::wstring& output) {
    Report r;const auto script=resource_bytes(IDR_SUPPORT);DWORD error=0;
    if(script.empty()){r.fail("Support collector missing","Download the launcher again.");return r;}
    const auto folder=fs::join(fs::temp_dir(),L"BioShockVR-Support-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64()));
    const auto file=fs::join(folder,L"collect.ps1"),log=fs::join(folder,L"result.txt");
    if(!fs::make_dirs(folder,&error)||!fs::write_file_atomic(file,script.data(),script.size(),&error)) {r.fail("Cannot prepare the support collector",fs::narrow(folder),error);return r;}
    wchar_t windows[MAX_PATH]{};GetWindowsDirectoryW(windows,MAX_PATH);
    auto shell=fs::join(windows,L"Sysnative\\WindowsPowerShell\\v1.0\\powershell.exe");
    if(!fs::is_file(shell))shell=fs::join(windows,L"System32\\WindowsPowerShell\\v1.0\\powershell.exe");
    const auto q=process::quote_arg;
    std::wstring cmd=q(shell)+L" -NoProfile -NonInteractive -ExecutionPolicy Bypass -File "+q(file)+
        L" -GameDir "+q(env.gameDir)+L" -DataDir "+q(env.dataDir)+L" -GameIni "+q(env.gameIni);
    if(!output.empty())cmd+=L" -OutDir "+q(output);
    DWORD code=0;
    if(!process::run_wait(cmd,&code,&error,60000,log)){r.fail("Cannot run the support collector","",error);return r;}
    std::vector<unsigned char> bytes;fs::read_file(log,&bytes,nullptr);std::string text(bytes.begin(),bytes.end());
    auto at=text.find("Support ZIP: ");
    if(code || at==std::string::npos){r.fail("Support collection failed",text);return r;}
    auto end=text.find_first_of("\r\n",at);const auto path=text.substr(at+13,end==std::string::npos?std::string::npos:end-at-13);
    r.backupDir=fs::parent(fs::widen(path));r.add(Status::Ok,"Support bundle saved",path);
    r.add(Status::Note,"Review before sharing","The archive stays on your computer. Nothing is uploaded.");return r;
}
}
