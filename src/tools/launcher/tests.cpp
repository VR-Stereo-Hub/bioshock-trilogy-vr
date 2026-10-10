// Host tests use only an explicitly supplied, new scratch directory.
#include "model/launcher.h"
#include "sys/fs.h"
#include "sys/game_ini.h"
#include "sys/updates.h"
#include <json/json.h>
#include <shellapi.h>
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <limits>
#include <stdexcept>
using namespace bvr::launcher;
namespace {
using Bytes=std::vector<unsigned char>;
int checks=0,failures=0;
void check(bool ok,const char* what){++checks;if(!ok){++failures;std::printf("FAIL: %s\n",what);}}
Bytes bytes(const std::string& s){return {s.begin(),s.end()};}
void write(const std::wstring& path,const Bytes& b){DWORD err=0;if(!fs::make_dirs(fs::parent(path),&err)||!fs::write_file_atomic(path,b.data(),b.size(),&err))throw std::runtime_error("Fixture write failed: "+fs::narrow(path));}
void write(const std::wstring& path,const std::string& s){write(path,bytes(s));}
Bytes read(const std::wstring& path){Bytes b;fs::read_file(path,&b,nullptr);return b;}
std::string hash(const Bytes& b){std::string h;fs::sha256_bytes(b.data(),b.size(),&h);return h;}
Bytes pe(const std::string& marker){
    IMAGE_DOS_HEADER dos{};dos.e_magic=IMAGE_DOS_SIGNATURE;dos.e_lfanew=128;
    IMAGE_NT_HEADERS32 nt{};nt.Signature=IMAGE_NT_SIGNATURE;nt.FileHeader.Machine=IMAGE_FILE_MACHINE_I386;nt.OptionalHeader.Magic=IMAGE_NT_OPTIONAL_HDR32_MAGIC;
    Bytes b(128+sizeof(nt));memcpy(b.data(),&dos,sizeof(dos));memcpy(b.data()+128,&nt,sizeof(nt));b.insert(b.end(),marker.begin(),marker.end());return b;
}
Payload payload(char revision='A'){
    Payload p;p.valid=true;p.version="0.8.3";p.build=std::string("fixture-")+revision;p.configuration="RelWithDebInfo";
    for(const auto* name:{L"xinput1_3.dll",L"bioshockvr.dll",L"bvr_steamvr32.dll",L"openvr_api.dll"}){
        PayloadFile f;f.name=name;f.bytes=pe(std::wstring(name)==L"xinput1_3.dll"?"[bioshockvr-proxy] failed to load bioshockvr.dll":"fixture payload");
        f.bytes.push_back(revision);f.hash=hash(f.bytes);p.files.push_back(std::move(f));
    }return p;
}
const std::string gameIni="; game settings\r\n[WinDrv.WindowsClient]\r\nWindowedViewportX=1280\r\nWindowedViewportY=720\r\nFullscreenViewportX=1920\r\nFullscreenViewportY=1080\r\nStartupFullscreen=False\r\nUnrelated=keep\r\n\r\n[XeDrv.XenonClient]\r\nWindowedViewportX=640\r\nWindowedViewportY=480\r\n";
Environment env(const std::wstring& root,const wchar_t* name){
    const auto dir=fs::join(root,name);Environment e{fs::join(dir,L"game\\Build\\Final"),fs::join(dir,L"data"),fs::join(dir,L"game-config\\Bioshock.ini")};
    fs::make_dirs(e.gameDir,nullptr);fs::make_dirs(e.dataDir,nullptr);
    write(fs::join(e.gameDir,kGameExe),pe("SYNTHETIC NONEXECUTABLE FIXTURE"));write(e.gameIni,gameIni);
    write(fs::join(e.dataDir,L"vrpreset.ini"),"# player calibration\r\nsnapTurn=0\r\nsnapAngleDeg=30\r\nhandScaleL=0.79\r\n");
    write(fs::join(e.dataDir,L"menu-settings.ini"),"# retained comment\r\nP SnapTurn -1 - 0\r\nP PlaceForward 1 Pistol 2.5\r\nD HandMode -1 - 3\r\nP FutureSetting -1 - 17\r\n");
    write(fs::join(e.dataDir,L"xr.ini"),"[runtime]\r\nmode=auto\r\ncustom=kept\r\n");
    for(int i=0;i<18;++i)write(fs::join(e.dataDir,L"untouched-"+std::to_wstring(i)+L".ini"),"; keep every byte\r\nvalue=custom\r\n");
    write(fs::join(e.gameDir,L"dxgi.dll"),"unrelated third party DXGI file");write(fs::join(e.gameDir,L"UserSave.sav"),"never touch saves");return e;
}
using Snapshot=std::map<std::wstring,Bytes>;
Snapshot snapshot(const Environment& e){Snapshot s;for(const auto& folder:{e.gameDir,e.dataDir,fs::parent(e.gameIni)})for(const auto& f:std::filesystem::directory_iterator(folder))if(f.is_regular_file())s[f.path().wstring()]=read(f.path().wstring());return s;}
void parser_tests(){
    std::string out,why;const std::string original="# keep\r\nP SnapTurn -1 - 0\r\nP PlaceForward 1 Pistol 1.25\r\nD HandMode -1 - 3\r\nP NotYetKnown -1 - 42\r\n";
    auto expected=original;const std::string row="P SnapTurn -1 - 0";expected.replace(expected.find(row),row.size(),"P SnapTurn -1 - 1");
    check(patch_preferences(original,{{"SnapTurn",1.f}},&out,&why)&&out==expected,"one F10 edit preserves all other bytes and unknown rows");
    check(patch_preferences("# no newline",{{"SnapAngle",30.f}},&out,&why)&&out=="# no newline\r\nP SnapAngle -1 - 30\r\n","append after missing final newline");
    check(patch_preferences("# LF\n",{{"SnapTurn",0.f}},&out,&why)&&out=="# LF\nP SnapTurn -1 - 0\n","LF retained");
    check(patch_preferences("P SnapTurn -1 - 0\r\nP SnapTurn -1 - 0\r\n",{{"SnapTurn",1.f}},&out,&why)&&out=="P SnapTurn -1 - 1\r\nP SnapTurn -1 - 1\r\n","duplicate preferences cannot override edit");
    for(const auto& edits:std::vector<std::map<std::string,float>>{{{"SnapAngle",999.f}},{{"SnapTurn",.5f}},{{"HandMode",3.f}},{{"PlaceForward",1.f}},{{"Unknown",1.f}},{{"SnapAngle",std::numeric_limits<float>::quiet_NaN()}}})check(!patch_preferences(original,edits,&out,&why),"invalid, debug or scoped preference refused");
    Choices c;c.runtime="VDXR";check(!valid_choices(c,&why),"unsupported runtime refused");c={};c.width=200;c.height=300;check(!valid_choices(c,&why),"unsafe viewport refused");c={};c.headset="headset\r\n[bad]";check(!valid_choices(c,&why),"INI injection refused");
    for(const auto& eol:{std::string("\r\n"),std::string("\n")}){
        const auto source="; keep"+eol+"[WinDrv.WindowsClient]"+eol+"WindowedViewportX=1280"+eol+"Other=unchanged"+eol+"[Other]"+eol+"WindowedViewportX=99"+eol;
        GameIni ini;ini.parse(bytes(source));std::string value;check(ini.get_unique("WinDrv.WindowsClient","WindowedViewportX",&value)&&value=="1280","scoped read");
        ini.set("WinDrv.WindowsClient","WindowedViewportX","2750",nullptr);auto wanted=source;wanted.replace(wanted.find("1280"),4,"2750");check(ini.serialise()==bytes(wanted),"only selected section changes, line endings survive");
    }
    GameIni bom;auto source=bytes("\xef\xbb\xbf"+gameIni);bom.parse(source);check(bom.serialise()==source,"UTF8 BOM CRLF roundtrip");
    const auto wide=fs::widen(gameIni);Bytes utf16{0xff,0xfe};const auto* data=reinterpret_cast<const unsigned char*>(wide.data());utf16.insert(utf16.end(),data,data+wide.size()*2);GameIni u;u.parse(utf16);check(u.serialise()==utf16,"UTF16 roundtrip");
    GameIni duplicate;duplicate.parse(bytes("[x]\r\na=1\r\na=2\r\n"));std::string value;check(!duplicate.get_unique("x","a",&value),"duplicate key refused");
    GameIni sections;sections.parse(bytes("[x]\r\na=1\r\n[x]\r\nb=2\r\n"));check(!sections.get_unique("x","a",&value),"duplicate section refused");
    for(const auto& arg:{std::wstring(L""),std::wstring(L"C:\\A folder\\"),std::wstring(L"quote\"back\\"),std::wstring(L"C:\\Rapture\\caf\u00e9\\")}){auto command=L"test.exe "+process::quote_arg(arg);int count=0;auto parsed=CommandLineToArgvW(command.c_str(),&count);check(parsed&&count==2&&std::wstring(parsed[1])==arg,"Windows argument quoting");if(parsed)LocalFree(parsed);}
}
std::string release_json(const std::string& digest,const std::string& url){
    Json::Value r;r["draft"]=false;r["prerelease"]=false;r["tag_name"]="v0.9.0";r["body"]="Notes";Json::Value a;a["name"]="BioShockVR-Launcher-v0.9.0.exe";a["size"]=Json::UInt64(4096);a["digest"]=digest;a["browser_download_url"]=url;r["assets"].append(a);Json::Value list(Json::arrayValue);list.append(r);Json::StreamWriterBuilder b;return Json::writeString(b,list);
}
void update_tests(){
    check(updates::newer("1.0.0","0.8.3"),"numeric release comparison");check(!updates::newer("0.8.3","0.8.3"),"same release not newer");
    for(const auto& s:{"1.2","1.2.3.4","1.2.x","-1.2.3","1.2.3evil"}){uint32_t parts[3];check(!updates::version(s,parts),"malformed version refused");}
    const std::string url="https://github.com/VR-Stereo-Hub/bioshock-trilogy-vr/releases/download/v0.9.0/BioShockVR-Launcher-v0.9.0.exe";std::vector<updates::Release> list;std::string error;
    check(updates::parse_releases(release_json("sha256:"+std::string(64,'a'),url),&list,&error)&&list.size()==1&&list[0].downloadable(),"matching GitHub asset accepted");
    for(const auto& bad:{std::string("https://example.com/evil.exe"),url+"?redirect=evil"})check(updates::parse_releases(release_json("sha256:"+std::string(64,'a'),bad),&list,&error)&&!list.empty()&&!list[0].downloadable(),"foreign or modified URL refused");
    check(updates::parse_releases(release_json("missing",url),&list,&error)&&!list.empty()&&!list[0].downloadable(),"hashless asset refused");check(!updates::parse_releases("{not json}",&list,&error),"bad JSON refused");check(!updates::parse_releases("[] trailing",&list,&error),"trailing JSON refused");
}
void transactions(const std::wstring& root){
    auto e=env(root,L"transaction");auto p=payload();std::map<std::wstring,Bytes> originals;
    for(const auto& f:p.files){originals[f.name]=bytes("foreign original "+fs::narrow(f.name));write(fs::join(e.gameDir,f.name),originals[f.name]);}
    const auto initial=snapshot(e);auto r=apply(e,p,"install");check(r.ok,"install over foreign files");if(!r.ok){std::printf("%s\n",report_json(r).c_str());return;}
    for(const auto& f:p.files){check(read(fs::join(e.gameDir,f.name))==f.bytes,"payload bytes installed");check(read(fs::join(e.gameDir,f.name+L".bvr-original"))==originals[f.name],"original bytes retained");}
    for(const auto& [path,b]:initial)if(fs::parent(path)!=e.gameDir)check(read(path)==b,"every existing INI retained during install");
    check(read(fs::join(e.gameDir,L"dxgi.dll"))==initial.at(fs::join(e.gameDir,L"dxgi.dll")),"foreign DXGI kept");auto installed=snapshot(e);
    r=apply(e,p,"disable");check(r.ok&&!fs::exists(fs::join(e.gameDir,L"bioshockvr.dll"))&&read(fs::join(e.gameDir,kDisabledMod))==p.files[1].bytes,"disable parks DLL");check(read(fs::join(e.gameDir,L"xinput1_3.dll"))==p.files[0].bytes,"XInput forwarding stays");
    r=apply(e,p,"enable");check(r.ok&&snapshot(e)==installed,"enable returns exact installed state");
    Choices c;c.runtime="steamvr";c.width=2750;c.height=2850;c.preferences={{"SnapTurn",1.f},{"SnapAngle",45.f}};c.headset="Meta Quest 3 / 3S";
    const auto before=snapshot(e);r=apply(e,p,"settings",c);check(r.ok,"settings applied");GameIni ini;std::wstring why;std::string value;
    check(GameIni::load(e.gameIni,&ini,&why),"game INI loads");for(const auto* key:{"WindowedViewportX","FullscreenViewportX"})check(ini.get_unique("WinDrv.WindowsClient",key,&value)&&value=="2750","both widths updated");for(const auto* key:{"WindowedViewportY","FullscreenViewportY"})check(ini.get_unique("WinDrv.WindowsClient",key,&value)&&value=="2850","both heights updated");
    check(ini.get_unique("XeDrv.XenonClient","WindowedViewportX",&value)&&value=="640","other section kept");
    for(const auto& [path,b]:before)if(path!=e.gameIni&&path!=fs::join(e.dataDir,L"menu-settings.ini")&&path!=fs::join(e.dataDir,L"xr.ini"))check(read(path)==b,"full file comparison of untouched files");
    const auto current=snapshot(e);r=apply(e,p,"settings",c);check(r.ok&&snapshot(e)==current,"settings idempotent");Report parsed;check(parse_report(report_json(r),&parsed)&&parsed.ok,"worker report roundtrip");check(!parse_report("{\"ok\":true,\"steps\":[{\"status\":99,\"title\":\"x\",\"detail\":\"y\"}]}",&parsed),"invalid report refused");
    auto next=payload('B');Choices edits;edits.runtime="native";edits.width=3012;edits.height=3122;edits.preferences={{"SnapTurn",0.f}};
    for(int fail=0;fail<8;++fail){fail_transaction_after(fail);r=apply(e,next,"update",edits);check(!r.ok,"injected failure reported");check(snapshot(e)==current,"every file restored after partial transaction");}fail_transaction_after(-1);
    HANDLE locked=CreateFileW(fs::join(e.gameDir,L"bioshockvr.dll").c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr);check(locked!=INVALID_HANDLE_VALUE,"replacement lock made");r=apply(e,next,"update");check(!r.ok&&snapshot(e)==current,"real sharing failure rolls back previous writes");if(locked!=INVALID_HANDLE_VALUE)CloseHandle(locked);
    r=apply(e,next,"update");check(r.ok,"update after lock released");for(const auto& f:next.files)check(read(fs::join(e.gameDir,f.name))==f.bytes,"updated payload exact");
    write(fs::join(e.gameDir,L"openvr_api.dll"),"new foreign replacement");const auto foreign=snapshot(e);r=apply(e,next,"uninstall");check(!r.ok&&snapshot(e)==foreign,"foreign replacement blocks uninstall");r=apply(e,next,"update");check(r.ok,"explicit repair succeeds");
    r=apply(e,next,"disable");check(r.ok,"disable before uninstall");r=apply(e,next,"uninstall");check(r.ok,"disabled uninstall succeeds");
    for(const auto& [name,b]:originals){check(read(fs::join(e.gameDir,name))==b,"foreign originals restored");check(!fs::exists(fs::join(e.gameDir,name+L".bvr-original")),"original backup slot consumed");}
    check(!fs::exists(fs::join(e.gameDir,kRecord))&&!fs::exists(fs::join(e.gameDir,kDisabledMod)),"owned manifest and disabled copy removed");check(read(fs::join(e.dataDir,L"menu-settings.ini"))==current.at(fs::join(e.dataDir,L"menu-settings.ini")),"F10 choices retained on uninstall");
    r=apply(e,p,"install");check(r.ok,"reinstall after uninstall");r=apply(e,p,"defaults");check(r.ok,"explicit defaults restore");check(!fs::exists(fs::join(e.dataDir,L"menu-settings.ini")),"explicit defaults clear overrides");check(read(fs::join(e.dataDir,L"xr.ini"))==current.at(fs::join(e.dataDir,L"xr.ini")),"defaults keep runtime");
    for(int i=0;i<18;++i)check(read(fs::join(e.dataDir,L"untouched-"+std::to_wstring(i)+L".ini"))==bytes("; keep every byte\r\nvalue=custom\r\n"),"defaults retain unrelated INIs");
}
void refusals(const std::wstring& root){
    Choices heightOnly;heightOnly.height=2000;check(heightOnly.dirty(),"height-only pending edit remains visible");
    auto p=payload();auto e=env(root,L"missing-section");write(e.gameIni,"[Unrelated]\r\na=1\r\n");auto before=snapshot(e);Choices c;c.width=2750;c.height=2850;auto r=apply(e,p,"install",c);check(!r.ok&&snapshot(e)==before,"bad viewport section prevents all writes");
    auto duplicate=env(root,L"duplicate-runtime");write(fs::join(duplicate.dataDir,L"xr.ini"),"[runtime]\r\nmode=auto\r\nmode=native\r\n");before=snapshot(duplicate);Choices runtime;runtime.runtime="steamvr";r=apply(duplicate,p,"install",runtime);check(!r.ok&&snapshot(duplicate)==before,"ambiguous runtime prevents all writes");
    auto dir=env(root,L"directory-collision");fs::make_dir(fs::join(dir.gameDir,L"bioshockvr.dll"),nullptr);before=snapshot(dir);r=apply(dir,p,"install");check(!r.ok&&snapshot(dir)==before,"directory at destination refused");
    auto bad=env(root,L"invalid-exe");auto file=pe("wrong architecture");reinterpret_cast<IMAGE_NT_HEADERS32*>(file.data()+128)->FileHeader.Machine=IMAGE_FILE_MACHINE_AMD64;write(fs::join(bad.gameDir,kGameExe),file);before=snapshot(bad);r=apply(bad,p,"install");check(!r.ok&&snapshot(bad)==before,"wrong architecture refused");
    auto invalid=env(root,L"bad-record");write(fs::join(invalid.gameDir,kRecord),"{\"schema\":1,\"product\":\"BioShockRemasteredVR\",\"files\":\"invalid\"}");before=snapshot(invalid);r=apply(invalid,p,"install");check(!r.ok&&snapshot(invalid)==before,"invalid ownership record refused");
    auto legacy=env(root,L"old-loader");std::wstring marker=L"BioshockVR_loader.log";const auto* data=reinterpret_cast<const unsigned char*>(marker.data());write(fs::join(legacy.gameDir,L"dxgi.dll"),Bytes(data,data+marker.size()*2));before=snapshot(legacy);r=apply(legacy,p,"install");check(!r.ok&&snapshot(legacy)==before,"retired loader blocked");
    auto tamper=env(root,L"bad-payload");auto wrong=p;wrong.files[0].bytes.push_back(0);before=snapshot(tamper);r=apply(tamper,wrong,"install");check(!r.ok&&snapshot(tamper)==before,"tampered embedded payload refused");wrong=p;wrong.valid=false;wrong.error="missing";r=apply(tamper,wrong,"install");check(!r.ok&&snapshot(tamper)==before,"missing payload refused");
    auto unicode=env(root,L"caf\u00e9 \u6d77\u5e95");r=apply(unicode,p,"install");check(r.ok,"Unicode install");r=apply(unicode,p,"uninstall");check(r.ok,"Unicode uninstall");
}
}
int wmain(int argc,wchar_t** argv){
    if(argc!=2){std::fprintf(stderr,"Pass an absolute scratch folder.\n");return 2;}const std::wstring root=argv[1];if(root.size()<4||root[1]!=L':'||fs::exists(root)){std::fprintf(stderr,"Use a new absolute scratch directory.\n");return 2;}
    CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);try{fs::make_dirs(root,nullptr);parser_tests();update_tests();transactions(root);refusals(root);}catch(const std::exception& e){++failures;std::fprintf(stderr,"EXCEPTION: %s\n",e.what());}CoUninitialize();std::printf("Launcher host tests: %d checks, %d failures\n",checks,failures);return failures?1:0;
}
