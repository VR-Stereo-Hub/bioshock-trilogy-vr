// Adapted from the Dishonored VR launcher; see NOTICE.md for provenance and license.
#include <windows.h>
#include <winhttp.h>
#include <winver.h>
#include "sys/updates.h"
#include "sys/fs.h"
#include "sys/process.h"
#include <json/json.h>
#include <algorithm>
#include <charconv>
#include <memory>
#include <array>

namespace bvr::launcher::updates {
namespace {
constexpr uint64_t maxAsset=256ull*1024*1024;
const std::string prefix="https://github.com/VR-Stereo-Hub/bioshock-trilogy-vr/releases/download/";
bool hash_ok(const std::string& h) { return h.size()==64 && h.find_first_not_of("0123456789abcdef")==std::string::npos; }
struct Internet { HINTERNET h=nullptr; ~Internet(){ if(h) WinHttpCloseHandle(h); } };
bool get(const std::wstring& url,size_t limit,std::vector<uint8_t>* out,std::string* error,const std::atomic<bool>* cancel) {
    out->clear();
    URL_COMPONENTS c={sizeof(c)}; c.dwHostNameLength=c.dwUrlPathLength=c.dwExtraInfoLength=(DWORD)-1;
    if(!WinHttpCrackUrl(url.c_str(),0,0,&c) || c.nScheme!=INTERNET_SCHEME_HTTPS) { *error="Invalid secure download URL."; return false; }
    std::wstring host(c.lpszHostName,c.dwHostNameLength), path(c.lpszUrlPath,c.dwUrlPathLength);
    if(c.dwExtraInfoLength) path.append(c.lpszExtraInfo,c.dwExtraInfoLength);
    Internet session{WinHttpOpen(L"BioShockVR-Launcher",WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,WINHTTP_NO_PROXY_NAME,WINHTTP_NO_PROXY_BYPASS,0)};
    if(!session.h) { *error="Could not initialize Windows networking."; return false; }
    WinHttpSetTimeouts(session.h,5000,5000,10000,10000);
    Internet connection{WinHttpConnect(session.h,host.c_str(),c.nPort,0)};
    Internet request{connection.h?WinHttpOpenRequest(connection.h,L"GET",path.c_str(),nullptr,WINHTTP_NO_REFERER,WINHTTP_DEFAULT_ACCEPT_TYPES,WINHTTP_FLAG_SECURE):nullptr};
    if(!request.h) { *error="Could not open the GitHub request."; return false; }
    DWORD redirects=WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP;
    WinHttpSetOption(request.h,WINHTTP_OPTION_REDIRECT_POLICY,&redirects,sizeof(redirects));
    const wchar_t* headers=L"Accept: application/vnd.github+json\r\nX-GitHub-Api-Version: 2022-11-28\r\n";
    if(!WinHttpSendRequest(request.h,headers,(DWORD)-1,nullptr,0,0,0) || !WinHttpReceiveResponse(request.h,nullptr)) {
        *error="GitHub could not be reached. Check your connection and try again."; return false;
    }
    DWORD status=0, n=sizeof(status);
    WinHttpQueryHeaders(request.h,WINHTTP_QUERY_STATUS_CODE|WINHTTP_QUERY_FLAG_NUMBER,WINHTTP_HEADER_NAME_BY_INDEX,&status,&n,WINHTTP_NO_HEADER_INDEX);
    if(status!=200) { *error=fs::format("GitHub returned HTTP %lu. Try again later.",status); return false; }
    const auto began=GetTickCount64();
    uint8_t block[65536];
    for(;;) {
        if((cancel && cancel->load()) || GetTickCount64()-began>180000) { *error="Update download cancelled or timed out."; return false; }
        DWORD got=0;
        if(!WinHttpReadData(request.h,block,sizeof(block),&got)) { *error="The download was interrupted. Your installed version is unchanged."; return false; }
        if(!got) return true;
        if(out->size()+got>limit) { *error="Download exceeds its expected size."; return false; }
        out->insert(out->end(),block,block+got);
    }
}
bool file_version(const std::wstring& path,const std::string& expected) {
    uint32_t p[3]; if(!version(expected,p)) return false;
    DWORD unused=0,size=GetFileVersionInfoSizeW(path.c_str(),&unused);
    if(!size || size>1024*1024) return false;
    std::vector<uint8_t> bytes(size);
    if(!GetFileVersionInfoW(path.c_str(),0,size,bytes.data())) return false;
    VS_FIXEDFILEINFO* info=nullptr; UINT len=0;
    return VerQueryValueW(bytes.data(),L"\\",(void**)&info,&len) && len>=sizeof(*info) &&
        info->dwSignature==0xfeef04bd && HIWORD(info->dwFileVersionMS)==p[0] &&
        LOWORD(info->dwFileVersionMS)==p[1] && HIWORD(info->dwFileVersionLS)==p[2];
}
}
bool version(const std::string& s,uint32_t (&p)[3]) {
    size_t at=(!s.empty() && s[0]=='v')?1:0;
    for(int i=0;i<3;++i) {
        size_t end=i==2?s.size():s.find('.',at);
        if(end==std::string::npos || end<=at) return false;
        auto result=std::from_chars(s.data()+at,s.data()+end,p[i]);
        if(result.ec!=std::errc{} || result.ptr!=s.data()+end) return false;
        at=end+1;
    }
    return true;
}
bool newer(const std::string& a,const std::string& b) {
    uint32_t x[3],y[3]; if(!version(a,x)||!version(b,y)) return false;
    return std::lexicographical_compare(y,y+3,x,x+3);
}
bool parse_releases(const std::string& text,std::vector<Release>* out,std::string* error) {
    out->clear(); Json::CharReaderBuilder builder; builder["rejectDupKeys"]=true; builder["failIfExtra"]=true;
    Json::Value root; std::unique_ptr<Json::CharReader> reader(builder.newCharReader()); std::string why;
    if(!reader->parse(text.data(),text.data()+text.size(),&root,&why) || !root.isArray()) { *error="GitHub returned an invalid release list."; return false; }
    for(const auto& j:root) {
        if(!j.isObject() || !j["draft"].isBool() || !j["prerelease"].isBool() || j["draft"].asBool() || j["prerelease"].asBool() || !j["tag_name"].isString()) continue;
        const auto tag=j["tag_name"].asString(); uint32_t parts[3]; if(!version(tag,parts)) continue;
        Release r; r.version=tag[0]=='v'?tag.substr(1):tag;
        if(j["body"].isString()) r.notes=j["body"].asString().substr(0,65536);
        if(j["published_at"].isString()) r.published=j["published_at"].asString().substr(0,10);
        const auto name="BioShockVR-Launcher-v"+r.version+".exe";
        if(j["assets"].isArray()) for(const auto& a:j["assets"]) {
            if(!a.isObject() || !a["name"].isString() || a["name"].asString()!=name || !a["browser_download_url"].isString() || !a["digest"].isString() || !a["size"].isUInt64()) continue;
            const auto digest=a["digest"].asString(),url=a["browser_download_url"].asString();
            if(digest.rfind("sha256:",0)!=0 || !hash_ok(digest.substr(7)) || url!=prefix+tag+"/"+name || a["size"].asUInt64()>maxAsset || !a["size"].asUInt64()) continue;
            r.assetUrl=url; r.sha256=digest.substr(7); r.size=a["size"].asUInt64();
        }
        out->push_back(r);
    }
    std::sort(out->begin(),out->end(),[](const auto& a,const auto& b){return newer(a.version,b.version);});
    return true;
}
std::wstring data_dir() {
    auto base=fs::join(fs::known_folder(FOLDERID_LocalAppData),L"BioshockVR"); fs::make_dir(base,nullptr);
    base=fs::join(base,L"Updates"); fs::make_dir(base,nullptr); return base;
}
Check check(const std::atomic<bool>* cancel) {
    Check result; std::vector<uint8_t> bytes;
    const auto cache=fs::join(data_dir(),L"releases.json");
    if(get(L"https://api.github.com/repos/VR-Stereo-Hub/bioshock-trilogy-vr/releases?per_page=100",8*1024*1024,&bytes,&result.message,cancel) &&
       parse_releases(std::string(bytes.begin(),bytes.end()),&result.releases,&result.message)) {
        result.online=true; result.message="Checked GitHub just now.";
        fs::write_file_atomic(cache,bytes.data(),bytes.size(),nullptr);
    } else {
        std::string ignored;
        if(fs::file_size(cache)<=8*1024*1024 && fs::read_file(cache,&bytes,nullptr))
            parse_releases(std::string(bytes.begin(),bytes.end()),&result.releases,&ignored);
        if(!result.releases.empty()) result.message+=" Showing saved release history.";
    }
    return result;
}
bool verify(const std::wstring& file,const Release& r,std::string* error) {
    std::string hash;
    if(!r.downloadable() || !hash_ok(r.sha256) || fs::file_size(file)!=r.size ||
       !fs::sha256_file(file,&hash,nullptr) || hash!=r.sha256 || !file_version(file,r.version)) {
        *error="The downloaded launcher failed its size, SHA-256 or version check. Nothing was installed."; return false;
    }
    return true;
}
bool download(const Release& r,std::wstring* file,std::string* error,const std::atomic<bool>* cancel) {
    if(!r.downloadable() || r.size>maxAsset || r.assetUrl.rfind(prefix,0)!=0) { *error="This release has no verified launcher asset yet."; return false; }
    std::vector<uint8_t> bytes;
    if(!get(fs::widen(r.assetUrl),(size_t)r.size,&bytes,error,cancel)) return false;
    *file=fs::join(data_dir(),L"BioShockVR-Launcher-v"+fs::widen(r.version)+L"-"+std::to_wstring(GetTickCount64())+L".exe");
    DWORD err=0;
    if(!fs::write_file_atomic(*file,bytes.data(),bytes.size(),&err)) { *error="Cannot save the update: "+fs::narrow(fs::win_error_text(err)); return false; }
    if(!verify(*file,r,error)) { fs::delete_file(*file,nullptr); return false; }
    return true;
}
bool start(const std::wstring& exe,const std::wstring& args,DWORD* error) {
    std::wstring command=process::quote_arg(exe)+L" "+args;
    STARTUPINFOW si={sizeof(si)}; PROCESS_INFORMATION pi={};
    if(!CreateProcessW(exe.c_str(),command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,fs::parent(exe).c_str(),&si,&pi)) { if(error) *error=GetLastError(); return false; }
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess); return true;
}
}
