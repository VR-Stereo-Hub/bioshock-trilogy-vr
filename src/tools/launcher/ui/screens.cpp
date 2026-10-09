#include "ui/screens.h"
#include "game/bioshock1r/menu_theme.h"
#include "sys/fs.h"
#include <imgui.h>
#include <algorithm>
#include <cmath>

namespace bvr::launcher::ui {
namespace {
namespace theme = b1r::menu::theme;
constexpr ImVec4 ivory{236/255.f,225/255.f,197/255.f,1};
constexpr ImVec4 brass{191/255.f,157/255.f,94/255.f,1};
constexpr ImVec4 sea{.43f,.78f,.70f,1};
constexpr ImVec4 warning{.95f,.70f,.39f,1};
float U(float n) { return theme::unit(n); }
void space(float n = 8) { ImGui::Dummy(ImVec2(0,U(n))); }
void note(const std::string& text) { theme::note(text.c_str()); }
void heading(const char* text) {
    ImGui::PushFont(ImGui::GetFont(),28);
    ImGui::TextColored(ivory,"%s",text); ImGui::PopFont();
}
void section(const char* label) {
    ImGui::PushStyleColor(ImGuiCol_Separator,brass);
    ImGui::SeparatorText(label); ImGui::PopStyleColor();
}
bool button(ViewState& v,const char* text,float width=0,bool selected=false,bool enabled=true) {
    ImGui::BeginDisabled(!enabled);
    bool pressed=theme::button(text,ImVec2(width ? U(width) : 0,U(42)),selected);
    const auto a=ImGui::GetItemRectMin(),b=ImGui::GetItemRectMax();
    if(ImGui::IsItemVisible()) {
        v.hitboxes.push_back({text,a.x,a.y,b.x-a.x,b.y-a.y,enabled && !v.busy});
        if(a.x < -1 || a.y < -1 || b.x > ImGui::GetIO().DisplaySize.x+1 || b.y > ImGui::GetIO().DisplaySize.y+1) ++v.layoutErrors;
    }
    ImGui::EndDisabled();return pressed;
}
void row(const char* label,const std::string& value) {
    ImGui::TableNextRow(); ImGui::TableNextColumn();
    ImGui::TextDisabled("%s",label); ImGui::TableNextColumn(); ImGui::TextWrapped("%s",value.c_str());
}
bool table(const char* id) {
    const bool open=ImGui::BeginTable(id,2,ImGuiTableFlags_SizingStretchProp);
    if(open){ImGui::TableSetupColumn("label",ImGuiTableColumnFlags_WidthFixed,U(130));ImGui::TableSetupColumn("value");}
    return open;
}
void path(const std::wstring& text) { note(fs::narrow(text)); }
const char* runtime_label(const std::string& token) {
    if(token=="native")return "Native OpenXR";
    if(token=="steamvr")return "SteamVR";
    if(token=="auto")return "Automatic";
    return "Custom value (kept)";
}
float value(const ViewState& v,const char* key,float fallback) {
    auto it=v.draft.preferences.find(key);if(it!=v.draft.preferences.end())return it->second;
    it=v.detection.preferences.find(key);return it==v.detection.preferences.end()?fallback:it->second;
}
void checkbox(ViewState& v,const char* label,const char* key,bool fallback,const char* hint) {
    bool enabled=value(v,key,fallback?1.f:0.f)!=0;
    if(ImGui::Checkbox(label,&enabled))v.draft.preferences[key]=enabled?1.f:0.f;
    if(ImGui::IsItemHovered() && hint){ImGui::BeginTooltip();ImGui::PushTextWrapPos(U(420));ImGui::TextUnformatted(hint);ImGui::PopTextWrapPos();ImGui::EndTooltip();}
}
void slider(ViewState& v,const char* label,const char* key,float fallback,float min,float max,const char* fmt) {
    float x=value(v,key,fallback);
    ImGui::AlignTextToFramePadding();ImGui::TextUnformatted(label);
    ImGui::SameLine(U(200));ImGui::SetNextItemWidth(-1);
    if(theme::slider(key,&x,min,max,fmt))v.draft.preferences[key]=x;
}
Action overview(ViewState& v) {
    auto& d=v.detection;Action a=Action::None;
    heading(d.installed?"Welcome back to Rapture":"Your journey to Rapture begins here");
    note("BioShock Remastered in virtual reality");space(16);
    const char* status=!d.found?"GAME FOLDER NEEDED":d.running!=process::Running::No?"GAME IS RUNNING":
        d.oldModConflict?"OLD LOADER NEEDS ATTENTION":d.disabled?"VR IS DISABLED":d.installed?"READY TO PLAY":"READY TO INSTALL";
    ImGui::TextColored(d.installed&&!d.disabled?sea:warning,"%s",status);space(6);
    if(table("overview-details")) {
        row("Game","BioShock Remastered");
        row("Installed build",d.installedVersion.empty()?"Not installed":d.installedVersion);
        row("Headset",d.headset.empty()?"Choose in Settings":d.headset);
        row("VR runtime",runtime_label(d.runtime));
        row("Render size",d.width?std::to_string(d.width)+" x "+std::to_string(d.height):"Current game settings");
        ImGui::EndTable();
    }
    space(14);section("Game location");
    path(d.env.gameDir.empty()?L"No game folder selected":d.env.gameDir);
    if(button(v,"Change folder",155))a=Action::Browse;
    ImGui::SameLine();if(button(v,"Rescan",110))a=Action::Refresh;
    ImGui::SameLine();if(button(v,"Open folder",145,false,d.found))a=Action::GameFolder;
    space(12);
    if(!d.error.empty())ImGui::TextWrapped("%s",d.error.c_str());
    else note(d.installed?"Connect your headset and start its VR software, then choose Play. Use F10 in the game to tune your hands, aiming and comfort.":
        "This launcher includes the mod and its SteamVR files. Existing settings and calibration are kept when you install.");
    space(14);section("Keep Rapture close");
    if(button(v,"Desktop shortcut",190))a=Action::DesktopShortcut;
    ImGui::SameLine();if(button(v,"Start menu shortcut",215))a=Action::StartShortcut;
    note("Shortcuts return to this launcher. Keep the launcher in a permanent folder.");
    return a;
}
void display(ViewState& v) {
    auto& d=v.detection;
    section("Headset & runtime");
    const auto& hs=v.draft.headset.empty()?d.headset:v.draft.headset;
    const char* headsets[]={"Meta Quest 3 / 3S","Meta Quest Pro","Meta Quest 2","Meta Quest 1","Meta Rift S / Rift CV1",
        "Valve Index","HTC Vive / Vive Pro","Vive Pro 2","Vive XR Elite","Bigscreen Beyond 1 / 2","Pimax Crystal / Light",
        "Pimax 5K / 8K","Reverb G2 / other WMR","Varjo Aero / XR-3","Pico 4 / 4 Ultra","Somnium VR1","PSVR2"};
    ImGui::SetNextItemWidth(-1);
    if(ImGui::BeginCombo("##headset",hs.empty()?"Choose your headset":hs.c_str())) {
        for(auto* h:headsets)if(ImGui::Selectable(h,hs==h))v.draft.headset=h;
        if(ImGui::Selectable("Something else"))v.advanced=true;
        ImGui::EndCombo();
    }
    if(v.advanced) {
        ImGui::SetNextItemWidth(-1);
        if(ImGui::InputTextWithHint("##headset-name","Type the headset name",v.customHeadset,sizeof(v.customHeadset)))v.draft.headset=v.customHeadset;
    }
    note("Recorded for support. Controller tuning stays in F10.");
    const auto current=v.draft.runtime.empty()?d.runtime:v.draft.runtime;
    ImGui::SetNextItemWidth(-1);
    if(ImGui::BeginCombo("##runtime",runtime_label(current))) {
        for(const char* token:{"auto","native","steamvr"})if(ImGui::Selectable(runtime_label(token),current==token))v.draft.runtime=token;
        ImGui::EndCombo();
    }
    note(current=="steamvr"?"Start SteamVR before playing. The included compatibility runtime supports this 32 bit game.":
        current=="native"?"Uses the active 32 bit OpenXR runtime. For Virtual Desktop, select VDXR in the Streamer app.":
        "Uses your active OpenXR runtime, with the included SteamVR runtime as a fallback.");
    note("This runtime choice is shared by all three BioShock VR mods.");
    space(5);section("Render quality");
    note("Applies after the game has closed. Higher sizes use more graphics memory.");
    ImGui::BeginDisabled(!d.gameIniValid);
    const struct {const char* text;float percent;} presets[]={{"Performance",75},{"Balanced",100},{"Quality",120},{"Ultra",150}};
    const float w=(ImGui::GetContentRegionAvail().x-ImGui::GetStyle().ItemSpacing.x*3)/4/v.scale;
    for(int i=0;i<4;++i){if(i)ImGui::SameLine();
        if(button(v,presets[i].text,w,v.draft.width && std::abs(v.pixelPercent-presets[i].percent)<.01f)){
            v.pixelPercent=presets[i].percent;
            v.draft.width=v.resolutionW=static_cast<unsigned>(2750*std::sqrt(v.pixelPercent/100)+.5f);
            v.draft.height=v.resolutionH=static_cast<unsigned>(2850*std::sqrt(v.pixelPercent/100)+.5f);
        }
    }
    ImGui::SetNextItemWidth(-1);
    if(theme::slider("##pixels",&v.pixelPercent,50,200,"%.0f%%")) {
        v.draft.width=v.resolutionW=static_cast<unsigned>(2750*std::sqrt(v.pixelPercent/100)+.5f);
        v.draft.height=v.resolutionH=static_cast<unsigned>(2850*std::sqrt(v.pixelPercent/100)+.5f);
    }
    note(std::to_string(v.resolutionW)+" x "+std::to_string(v.resolutionH)+" pixels per eye"+(v.draft.width?"   /   Pending change":"   /   Current size kept"));
    if(ImGui::TreeNode("Exact resolution")) {
        ImGui::SetNextItemWidth(U(150));bool changed=ImGui::InputInt("Width",&v.resolutionW,0);
        ImGui::SameLine();ImGui::SetNextItemWidth(U(150));changed|=ImGui::InputInt("Height",&v.resolutionH,0);
        if(changed){v.draft.width=static_cast<unsigned>(std::max(0,v.resolutionW));v.draft.height=static_cast<unsigned>(std::max(0,v.resolutionH));}
        ImGui::TreePop();
    }
    ImGui::EndDisabled();
    if(!d.gameIniValid)note("Run BioShock Remastered once and close it to create its game settings, then rescan.");
    space(5);section("In the headset");
    checkbox(v,"Start VR automatically","AutoStart",true,"The same AutoStart preference as the F10 menu.");
    note("Your existing F10 preferences and weapon calibration are preserved.");
}
void controls(ViewState& v) {
    section("Turning");
    int snap=value(v,"SnapTurn",0)!=0;
    if(button(v,"Smooth",160,snap==0))v.draft.preferences["SnapTurn"]=0;
    ImGui::SameLine();if(button(v,"Snap",160,snap==1))v.draft.preferences["SnapTurn"]=1;
    if(value(v,"SnapTurn",0)!=0)slider(v,"Turn angle","SnapAngle",45,15,90,"%.0f deg");
    else slider(v,"Turn speed","TurnSpeed",1,.2f,3,"%.2fx");
    space(10);section("Motion controls");
    checkbox(v,"Swing the wrench to attack","Swing",true,"Uses the same motion attack switch as F10. The trigger remains available.");
    if(value(v,"Swing",1)!=0)slider(v,"Swing speed","SwingSpeed",3.6f,.2f,5,"%.2f m/s");
    space(10);section("Aiming & presentation");
    checkbox(v,"Show the aim reticle","Reticle",false,"Edits the shared F10 reticle preference.");
    checkbox(v,"Show the aim laser","Laser",false,"Edits the shared F10 aim laser preference.");
    checkbox(v,"Hide cinematic black borders","HideBars",true,"Edits the shared F10 cutscene border preference.");
    space(15);note("Hand alignment, arm positioning, world scale and controller shortcuts are available in F10 while you play. Your other choices are kept.");
}
Action mods(ViewState& v) {
    Action a=Action::None;const auto& d=v.detection;
    heading("Your VR installation");note("Manage the mod without touching your saved games.");space(15);
    section("BioShock Remastered VR");
    ImGui::TextColored(d.disabled?warning:sea,"%s",!d.installed?"Not installed":d.disabled?"VR disabled":"VR enabled");
    note(d.managed?"Managed by this launcher. Original files and a recovery journal are retained.":"Install through this launcher to enable backup and removal management.");
    if(button(v,d.disabled?"Enable VR":"Disable VR",180,false,d.managed&&d.installed))a=d.disabled?Action::Enable:Action::Disable;
    space(18);section("Repair & recovery");
    if(button(v,"Reinstall this build",235,false,d.found&&d.payloadOk))a=Action::Reinstall;
    note("Replaces the mod files with this launcher's build. Keeps your settings and backs up changed files.");
    space(8);
    if(button(v,"Restore bundled calibration",315,false,d.found))v.confirmDefaults=true;
    note("Resets the bundled presets and F10 overrides. A complete recovery copy is saved first.");
    space(12);section("Remove the launcher installation");
    if(button(v,"Uninstall VR",180,false,d.managed))v.confirmUninstall=true;
    note("Restores the files that were here before this launcher. Settings, saves and logs are kept.");
    return a;
}
void bindings() {
    heading("At your fingertips");note("Controller shortcuts for BioShock Remastered VR");space(15);
    if(table("bindings")) {
        row("Both stick clicks","Tap together to open or close the F10 menu.");
        row("Hold both clicks","Hold for about 0.6 seconds to recenter your seated pose and view.");
        row("Right controller","Point at a setting in F10. Right trigger selects it.");
        row("Right stick in F10","Up and down scroll. Left and right adjust the pointed slider.");
        row("Left stick in F10","Movement remains available while the panel is open.");
        row("Motion wrench","Swing your right hand to attack when motion attacks are enabled. The trigger also works.");
        row("Keyboard","F10 opens or closes the settings menu.");
        ImGui::EndTable();
    }
    space(18);section("Tune while you play");
    ImGui::TextWrapped("F10 contains the game-aware controls for your hands, arm solver, aiming, controller shortcuts, comfort and HUD.");
    note("Player settings save automatically. Advanced and Debug controls remain in the game so their effects can be checked immediately.");
}
Action updates_page(ViewState& v) {
    Action a=Action::None;heading("Updates");
    note("Keep your settings when installing a newer build.");space(12);
    if(table("versions")){row("This launcher",v.payload.version+" / "+v.payload.build);row("Installed build",v.detection.installedBuild.empty()?v.detection.installedVersion:v.detection.installedBuild);ImGui::EndTable();}
    space(10);
    if(button(v,"Check for updates",225))a=Action::CheckUpdates;
    ImGui::SameLine();if(button(v,"Release page",185))a=Action::Releases;
    if(!v.releases.message.empty())note(v.releases.message);
    space(12);
    if(v.releases.releases.empty())note("Checks run when you choose. The installed build is identified by its file hashes.");
    for(size_t i=0;i<v.releases.releases.size();++i) {
        const auto& r=v.releases.releases[i];
        const auto label="Version "+r.version+"  /  "+r.published;
        ImGui::PushID(static_cast<int>(i));
        if(i==0){
            if(!v.downloaded.empty()){if(button(v,"Open verified launcher",285))a=Action::OpenUpdate;}
            else if(r.downloadable() && updates::newer(r.version,v.payload.version)) {if(button(v,"Download verified launcher",315))a=Action::DownloadUpdate;}
            else if(!r.downloadable())note("This release has no verified launcher download. Use its release page.");
        }
        if(ImGui::TreeNode(label.c_str())){ImGui::TextWrapped("%s",r.notes.c_str());ImGui::TreePop();}
        ImGui::PopID();
    }
    space(14);section("This build");
    if(button(v,v.detection.matches?"Reinstall this build":"Install included build",270,false,v.detection.found))a=Action::Reinstall;
    note("Your current calibration is retained. New downloads must pass size, SHA256 and version checks before they can be opened.");
    return a;
}
Action help(ViewState& v) {
    Action a=Action::None;heading("Help & about");note("A clearer view of what is installed");space(12);
    section("Support");
    ImGui::TextWrapped("A support bundle includes the current and previous mod logs, settings and installation details. Review the ZIP before sharing it.");
    if(button(v,"Collect support bundle",275))a=Action::Support;
    ImGui::SameLine();if(button(v,"Open settings folder",255))a=Action::DataFolder;
    space(14);section("Your system");
    if(table("hardware")) {
        row("Graphics",fs::narrow(v.detection.graphics.name));
        row("VRAM budget",v.detection.graphics.budgetBytes?fs::format("%.1f GB",v.detection.graphics.budgetBytes/(1024.*1024*1024)):"Not reported");
        row("OpenXR runtime",v.detection.activeRuntime.empty()?"No native 32 bit runtime registered":v.detection.activeRuntime);
        row("SteamVR",v.detection.steamVr?"Detected":"Not detected");ImGui::EndTable();
    }
    space(12);section("BioShock Remastered VR");
    ImGui::TextWrapped("Created by the BioShock VR contributors. Launcher adapted from the Dishonored VR launcher, using BioShock's Rapture F10 theme.");
    note("An unofficial community VR mod. BioShock belongs to its respective owners.");
    if(button(v,"Project & releases",230))a=Action::Releases;
    return a;
}
Action result(ViewState& v) {
    heading(v.report.ok?"All set":"The operation needs attention");
    for(const auto& s:v.report.steps){space(8);ImGui::TextColored(s.status==Status::Failed?warning:s.status==Status::Ok?sea:brass,"%s",s.title.c_str());note(s.detail);}
    space(15);
    if(!v.report.backupDir.empty() && button(v,"Open recovery folder",250))return Action::BackupFolder;
    if(button(v,"Back to overview",220))v.page=Page::Overview;
    return Action::None;
}
void modal_size(float width) {
    auto& io=ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x*.5f,io.DisplaySize.y*.5f),ImGuiCond_Always,ImVec2(.5f,.5f));
    ImGui::SetNextWindowSize(ImVec2(std::min(U(width),io.DisplaySize.x-U(40)),0),ImGuiCond_Always);
}
}
Action draw(ViewState& v) {
    v.hitboxes.clear();v.layoutErrors=0;
    theme::begin(v.scale,1);Action a=Action::None;
    // The in-game panel has no native dialogs; extend its palette here only.
    auto& colors=ImGui::GetStyle().Colors;
    colors[ImGuiCol_TitleBg]=ImVec4(.06f,.11f,.11f,1);
    colors[ImGuiCol_TitleBgActive]=ImVec4(.15f,.23f,.21f,1);
    colors[ImGuiCol_PopupBg]=ImVec4(.045f,.085f,.085f,.98f);
    colors[ImGuiCol_Border]=brass;
    colors[ImGuiCol_ModalWindowDimBg]=ImVec4(.01f,.04f,.04f,.62f);
    ImGui::SetNextWindowPos(ImVec2(0,0));ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
    ImGui::Begin("##launcher",nullptr,ImGuiWindowFlags_NoDecoration|ImGuiWindowFlags_NoSavedSettings|ImGuiWindowFlags_NoMove|ImGuiWindowFlags_NoScrollbar);
    theme::background();theme::title();
    ImGui::PushFont(ImGui::GetFont(),14);ImGui::TextColored(brass,"R A P T U R E   /   V R   L A U N C H E R");ImGui::PopFont();
    space(10);theme::rule();space(7);
    const float footer=U(83),side=U(183),gap=U(25);
    ImGui::BeginDisabled(v.busy);
    ImGui::BeginChild("##navigation",ImVec2(side,-footer));
    const char* labels[]={"Overview","Settings","Mods","Bindings","Updates","Help & about"};
    for(int i=0;i<6;++i)if(button(v,labels[i],side/v.scale,static_cast<int>(v.page)==i)){v.page=static_cast<Page>(i);}
    if(ImGui::GetContentRegionAvail().y>U(95)) {
        ImGui::SetCursorPosY(ImGui::GetWindowHeight()-U(95));
        note("BioShock Remastered");
        ImGui::TextColored(v.detection.installed&&!v.detection.disabled?sea:brass,"%s",v.detection.disabled?"VR disabled":v.detection.installed?"VR enabled":"VR not installed");
        note("Settings apply next launch");
    }
    ImGui::EndChild();ImGui::SameLine(0,gap);
    ImGui::BeginChild("##content",ImVec2(0,-footer));
    if(v.detection.running!=process::Running::No) {ImGui::TextColored(warning,"Close the game to make changes.");space();}
    if(v.busy) {heading("One moment...");note(v.busyText);space(10);ImGui::ProgressBar(-static_cast<float>(ImGui::GetTime()),ImVec2(-1,U(12)),"");}
    else switch(v.page) {
        case Page::Overview:a=overview(v);break;
        case Page::Settings:
            heading(v.detection.installed?"Make Rapture your own":"Set up your VR experience");
            if(button(v,"Display",175,v.settingsTab==0))v.settingsTab=0;
            ImGui::SameLine();if(button(v,"Controls",175,v.settingsTab==1))v.settingsTab=1;
            space(5);if(v.settingsTab==0)display(v);else controls(v);break;
        case Page::Mods:a=mods(v);break;
        case Page::Bindings:bindings();break;
        case Page::Updates:a=updates_page(v);break;
        case Page::Help:a=help(v);break;
        case Page::Result:a=result(v);break;
    }
    ImGui::EndChild();
    ImGui::SetCursorPosY(ImGui::GetWindowHeight()-footer);theme::rule();space(4);
    const float y=ImGui::GetCursorPosY();
    const float right=ImGui::GetWindowWidth()-ImGui::GetStyle().WindowPadding.x;
    ImGui::SetCursorPosX(right-U(150));
    bool play=button(v,"Play",150,true,v.detection.found&&v.detection.installed&&!v.detection.oldModConflict&&v.detection.running==process::Running::No);
    float x=right-U(150)-ImGui::GetStyle().ItemSpacing.x;
    bool canApply=v.detection.found&&v.detection.running==process::Running::No&&!v.detection.oldModConflict;
    const bool showApply=v.draft.dirty()||!v.detection.installed;
    if(showApply){x-=U(170);ImGui::SetCursorPos(ImVec2(x,y));
        if(button(v,v.detection.installed?"Apply settings":"Install VR",170,false,canApply&&v.payload.valid))a=v.detection.installed?Action::Apply:Action::Install;
        x-=ImGui::GetStyle().ItemSpacing.x;
    }
    x-=U(100);ImGui::SetCursorPos(ImVec2(x,y));
    if(button(v,"Close",100)) {if(v.draft.dirty())v.confirmClose=true;else a=Action::Close;}
    ImGui::SetCursorPos(ImVec2(ImGui::GetStyle().WindowPadding.x,y+U(4)));
    ImGui::PushTextWrapPos(x-U(16));
    std::string foot=v.busy?"Working...":!v.notice.empty()?v.notice:v.draft.dirty()?"Unsaved settings. Apply before playing.":v.fixture?"Rendered from the actual launcher. Preview data.":"Your settings are kept when you update.";
    note(foot);ImGui::PopTextWrapPos();
    ImGui::EndDisabled();ImGui::End();
    if(play){if(v.draft.dirty())v.confirmPlay=true;else a=Action::Play;}
    if(v.confirmUninstall){ImGui::OpenPopup("Uninstall VR?");v.confirmUninstall=false;}
    modal_size(590);
    if(ImGui::BeginPopupModal("Uninstall VR?",nullptr,ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextWrapped("Restore the files that were present before this launcher installed VR? Your settings, save files and logs will be kept. Any earlier mod installation will be restored too.");space();
        if(button(v,"Uninstall",195,true)){a=Action::Uninstall;ImGui::CloseCurrentPopup();}ImGui::SameLine();if(button(v,"Cancel",135))ImGui::CloseCurrentPopup();ImGui::EndPopup();
    }
    if(v.confirmDefaults){ImGui::OpenPopup("Restore calibration?");v.confirmDefaults=false;}
    modal_size(590);
    if(ImGui::BeginPopupModal("Restore calibration?",nullptr,ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextWrapped("Replace your three calibration presets and clear F10 overrides? The complete previous files will be backed up first. Your headset and runtime choice will be kept.");space();
        if(button(v,"Restore defaults",245,true)){a=Action::Defaults;ImGui::CloseCurrentPopup();}ImGui::SameLine();if(button(v,"Cancel",135))ImGui::CloseCurrentPopup();ImGui::EndPopup();
    }
    if(v.confirmPlay){ImGui::OpenPopup("Unsaved settings");v.confirmPlay=false;}
    modal_size(590);
    if(ImGui::BeginPopupModal("Unsaved settings",nullptr,ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextWrapped("You have changes that have not been applied yet.");space();
        if(button(v,"Apply settings first",260,true)){a=Action::Apply;ImGui::CloseCurrentPopup();}
        if(button(v,"Play with saved settings",310)){a=Action::Play;ImGui::CloseCurrentPopup();}
        if(button(v,"Cancel",135))ImGui::CloseCurrentPopup();ImGui::EndPopup();
    }
    if(v.confirmClose){ImGui::OpenPopup("Close without applying?");v.confirmClose=false;}
    modal_size(560);
    if(ImGui::BeginPopupModal("Close without applying?",nullptr,ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextWrapped("Your pending changes have not been saved.");space();
        if(button(v,"Discard and close",260)){a=Action::Close;ImGui::CloseCurrentPopup();}ImGui::SameLine();if(button(v,"Keep editing",190,true))ImGui::CloseCurrentPopup();ImGui::EndPopup();
    }
    theme::end();return a;
}
}
