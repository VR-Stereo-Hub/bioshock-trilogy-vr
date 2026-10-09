#include "menu_view.h"
#include "menu_theme.h"
#include <imgui_internal.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace bvr::b1r::menu {
namespace {
const ViewOptions* g_options = nullptr;
void record(const char* name, bool disabled = false) {
    if(g_options && g_options->hits) {
        const auto* ctx=ImGui::GetCurrentContext();
        g_options->hits->push_back({name,ImGui::GetItemRectMin(),ImGui::GetItemRectMax(),
            ctx->LastItemData.ID,disabled});
    }
}
void label(const char* text,bool slider=false,const char* key=nullptr) {
    ImGui::TableNextRow();ImGui::TableSetColumnIndex(0);
    float controlOffset=0;
    if(slider) {
        const float railHeight=std::max(theme::unit(39),ImGui::GetFontSize()+theme::unit(8));
        const float textHeight=ImGui::CalcTextSize(text,nullptr,false,ImGui::GetContentRegionAvail().x).y;
        ImGui::SetCursorPosY(ImGui::GetCursorPosY()+std::max(0.f,(railHeight-textHeight)*.5f));
        controlOffset=std::max(0.f,(textHeight-railHeight)*.5f);
    } else ImGui::AlignTextToFramePadding();
    ImGui::TextWrapped("%s",text);
    if(key) record((std::string("label:")+key).c_str());
    ImGui::TableSetColumnIndex(1);
    if(controlOffset) ImGui::SetCursorPosY(ImGui::GetCursorPosY()+controlOffset);
    ImGui::SetNextItemWidth(-1);
}
bool section(const char* name) {
    if(g_options->expandSections) ImGui::SetNextItemOpen(true,ImGuiCond_Once);
    bool open=theme::section(name);
    record((std::string("section:")+name).c_str());
    return open;
}
void send(Backend& backend,const Spec& s,int hand,float value,bool commit) {
    if(s.scope==Scope::Global) backend.edit(s.id,-1,value,commit);
    else if(hand==-1 && s.scope==Scope::Hand) {
        backend.edit(s.id,0,value,commit);backend.edit(s.id,1,value,commit);
    } else backend.edit(s.id,hand,value,commit);
}
void control(Backend& backend,const Spec& s,int hand) {
    if(s.condition==Condition::Snap && !backend.read(Setting::SnapTurn,0)) return;
    if(s.condition==Condition::Smooth && backend.read(Setting::SnapTurn,0)) return;
    const int readHand=s.scope==Scope::Global?-1:(hand<0?1:hand);
    float value=backend.read(s.id,readHand);
    const char* reason=disabled_reason(s,backend,hand);
    ImGui::PushID(s.key);
    label(s.label,s.kind==Kind::Slider,s.key);
    ImGui::BeginDisabled(reason!=nullptr);
    bool changed=false,commit=false;
    if(s.kind==Kind::Toggle) {
        bool on=value!=0;
        changed=ImGui::Checkbox("##value",&on);value=on?1.f:0.f;commit=changed;
        record(s.key,reason!=nullptr);
    } else if(s.kind==Kind::Slider) {
        changed=theme::slider("##value",&value,s.min,s.max,s.format);
        commit=ImGui::IsItemDeactivatedAfterEdit();
        record(s.key,reason!=nullptr);
        // Float-backed integer preferences retain integer semantics.
        if(s.id==Setting::LaserDots || s.id==Setting::SwingCooldown || s.id==Setting::SwingDelay ||
            s.id==Setting::ElbowSmoothing || s.id==Setting::SceneHold || s.id==Setting::EffectVertexLimit)
            value=std::round(value);
    } else {
        int current=static_cast<int>(value-s.min);
        int count=static_cast<int>(s.max-s.min)+1;
        if(s.id==Setting::HandMode) count=backend.context().maxHandMode-1;
        std::vector<const char*> names;
        for(const char* p=s.choices;*p;p+=std::strlen(p)+1) names.push_back(p);
        count=std::min(count,static_cast<int>(names.size()));
        if(count==2) {
            const float width=(ImGui::GetContentRegionAvail().x-ImGui::GetStyle().ItemSpacing.x)*.5f;
            for(int i=0;i<count;++i) {
                if(i) ImGui::SameLine();
                // Turn style keeps the approved draft's Snap-then-Smooth order.
                int choice=s.id==Setting::SnapTurn?1-i:i;
                if(theme::button(names[choice],ImVec2(width,0),current==choice)) {
                    current=choice;changed=true;
                }
                record((std::string(s.key)+":"+std::to_string(choice)).c_str(),reason!=nullptr);
            }
        } else {
            changed=ImGui::Combo("##value",&current,names.data(),count);
            record(s.key,reason!=nullptr);
        }
        if(changed) {value=static_cast<float>(current)+s.min;commit=true;}
    }
    ImGui::EndDisabled();
    if((changed||commit) && !reason) send(backend,s,hand,value,commit);
    if(reason) theme::note(reason);
    else if(s.id==Setting::LockOnDisabled) theme::note(s.help);
    else if(s.help && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("%s",s.help);
    ImGui::PopID();
}
void hand_selector(ViewState& state,const Context& context) {
    ImGui::TextUnformatted("Edit hand");ImGui::SameLine();
    const float w=theme::unit(113);
    if(theme::button("Left",ImVec2(w,0),state.hand==0)) state.hand=0;
    record("hand:Left");ImGui::SameLine();
    if(theme::button("Right",ImVec2(w,0),state.hand==1)) state.hand=1;
    record("hand:Right");ImGui::SameLine();
    if(theme::button("Both",ImVec2(w,0),state.hand==-1)) state.hand=-1;
    record("hand:Both");
    if(state.tab!=Tab::IK) {
        const std::string profile=context.profile.empty()?"No equipped profile":
            std::string(context.profileHand==0?"Plasmid profile: ":"Weapon profile: ")+context.profile;
        theme::note(profile.c_str());
    }
}
void draw_page(Backend& backend,ViewState& state,Level level,const Context& context) {
    if(state.tab==Tab::Hands || (state.tab==Tab::IK && context.handMode<4) || state.tab==Tab::Aim) hand_selector(state,context);
    std::size_t count=0;const Spec* all=specs(count);
    std::vector<std::string> sections;
    for(std::size_t i=0;i<count;++i) if(all[i].tab==state.tab && *all[i].section && visible(all[i],level,context)) {
        if(std::find(sections.begin(),sections.end(),all[i].section)==sections.end()) sections.emplace_back(all[i].section);
    }
    if(state.tab==Tab::IK) {
        const auto shoulder=std::find(sections.begin(),sections.end(),"Shoulder position");
        if(shoulder!=sections.end()) std::rotate(sections.begin(),shoulder,shoulder+1);
    }
    for(const auto& name:sections) {
        if(!section(name.c_str())) continue;
        ImGui::PushID(name.c_str());
        if(ImGui::BeginTable("settings",2,ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn("label",0,.44f);ImGui::TableSetupColumn("value",0,.56f);
            for(std::size_t i=0;i<count;++i)
                if(all[i].tab==state.tab && name==all[i].section && visible(all[i],level,context))
                    control(backend,all[i],state.hand);
            ImGui::EndTable();
        }
        if(name=="Turning") theme::note(backend.read(Setting::SnapTurn,0)?
            "One stick push turns you by a fixed angle.":"Hold the stick to turn continuously.");
        if(name=="Controller shortcuts") {
            const int modifier=static_cast<int>(backend.read(Setting::AmmoModifier,0));
            const bool left=backend.read(Setting::AmmoStick,0)!=0;
            if((modifier==1&&!left)||(modifier==4&&left))
                theme::note("Choose the opposite stick: one thumb cannot use its thumbrest and stick together.");
        }
        if(name=="Grip pivot") theme::note("Changes where the model pivots. Hand position changes where it sits.");
        if(name=="Flat screen for menus and loading") {
            if(theme::button("Recenter screen")) backend.action(Action::RecenterScreen);
            record("RecenterScreen");
        }
        ImGui::PopID();
    }
    // Resolution requires an engine-safe next-launch path; the live backend owns
    // the honest state and explicit action, while preview substitutes test data.
    if(state.tab==Tab::Display || state.tab==Tab::Runtime || state.tab==Tab::Diagnostics)
        backend.diagnostics(state.tab);
    if(level==Level::Debug && state.tab==Tab::Diagnostics) {
        theme::note("Debug changes last for this session unless explicitly saved.");
        if(theme::button("Save debug defaults")) ImGui::OpenPopup("Save debug defaults?");
        record("SaveDebugDefaults");
        if(ImGui::BeginPopupModal("Save debug defaults?",nullptr,ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::TextUnformatted("Save the current applicable Debug switches for future launches?");
            if(theme::button("Save")) {backend.action(Action::SaveDebugDefaults);ImGui::CloseCurrentPopup();}
            ImGui::SameLine();if(theme::button("Cancel")) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
    }
}
}
void draw(Backend& backend,ViewState& state,const ViewOptions& options) {
    g_options=&options;
    if(options.hits) options.hits->clear();
    const auto context=backend.context();
    const auto level=static_cast<Level>(std::clamp(static_cast<int>(backend.read(Setting::ViewLevel,-1)),0,2));
    const float text=std::clamp(backend.read(Setting::TextSize,-1),.8f,1.5f);
    if(level!=Level::Debug && state.tab>=Tab::Runtime) state.tab=Tab::Controls;
    const bool capture=options.captureSize.x>0;
    const auto display=ImGui::GetIO().DisplaySize;
    // Match Dishonored's accepted eye-space placement (649x685 at 1027,1021
    // in a 2750x2850 eye). Resolution is a multiplier, never an absolute font
    // size or clamped DPI value. The player's text-size choice is independent.
    const float scale=capture?options.captureScale:(display.x/2750.f)*(649.f/760.f);
    theme::begin(scale,text);
    ImVec2 size=capture?options.captureSize:ImVec2(display.x*(649.f/2750.f),display.y*(685.f/2850.f));
    ImVec2 position=capture?ImVec2(0,0):ImVec2(display.x*(1027.f/2750.f),display.y*(1021.f/2850.f));
    const bool first=state.lastDisplay.x<=0;
    const bool resized=!capture && !first &&
        (display.x!=state.lastDisplay.x || display.y!=state.lastDisplay.y);
    if(resized) {
        const float x=display.x/state.lastDisplay.x,y=display.y/state.lastDisplay.y;
        position=ImVec2(state.lastPosition.x*x,state.lastPosition.y*y);
        size=ImVec2(state.lastSize.x*x,state.lastSize.y*y);
    }
    const auto condition=capture||first||resized?ImGuiCond_Always:ImGuiCond_FirstUseEver;
    ImGui::SetNextWindowSize(size,condition);
    ImGui::SetNextWindowPos(position,condition);
    ImGui::SetNextWindowSizeConstraints(
        ImVec2(std::min(theme::unit(570),display.x*.95f),std::min(theme::unit(640),display.y*.95f)),
        capture?display:ImVec2(display.x*.95f,display.y*.95f));
    const ImGuiWindowFlags flags=ImGuiWindowFlags_NoTitleBar|ImGuiWindowFlags_NoCollapse|
        ImGuiWindowFlags_NoScrollbar|ImGuiWindowFlags_NoScrollWithMouse|
        (capture?(ImGuiWindowFlags_NoResize|ImGuiWindowFlags_NoMove):0);
    if(ImGui::Begin("BioShock VR settings###B1RSettings",nullptr,flags)) {
        if(!capture) {
            state.lastDisplay=display;state.lastPosition=ImGui::GetWindowPos();state.lastSize=ImGui::GetWindowSize();
            state.fontPixels=ImGui::GetFontSize();
        }
        theme::background();
        theme::title();theme::subtitle();
        ImGui::Dummy(ImVec2(0,theme::unit(3)));
        theme::rule();
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,ImVec2(theme::unit(10),theme::unit(4)));
        if(ImGui::BeginTable("top",3,ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn("recenter",0,.30f);ImGui::TableSetupColumn("height",0,.40f);
            ImGui::TableSetupColumn("level",0,.30f);
            ImGui::TableNextRow();ImGui::TableSetColumnIndex(0);
            if(theme::button("Recenter",ImVec2(-1,0))) backend.action(Action::Recenter);
            record("Recenter");ImGui::TableSetColumnIndex(1);
            float height=backend.read(Setting::Height,-1);
            char heightCaption[64];std::snprintf(heightCaption,sizeof heightCaption,"Height %+.0f cm###height",height);
            if(theme::button(heightCaption,ImVec2(-1,0))) ImGui::OpenPopup("Height offset");
            record("HeightPopup");
            if(ImGui::BeginPopup("Height offset")) {
                ImGui::TextUnformatted("Height offset");ImGui::SetNextItemWidth(theme::unit(340));
                ImGui::BeginDisabled(!context.engineReady);
                bool changed=theme::slider("##height",&height,-150,150,"%+.0f cm");
                bool commit=ImGui::IsItemDeactivatedAfterEdit();record("Height");
                ImGui::EndDisabled();
                if(changed||commit) backend.edit(Setting::Height,-1,height,commit);
                theme::note("Adjust your viewpoint height without changing world scale.");
                ImGui::EndPopup();
            }
            ImGui::TableSetColumnIndex(2);
            const char* currentLevel=level==Level::Basic?"Basic":level==Level::Advanced?"Advanced":"Debug";
            char levelCaption[48];std::snprintf(levelCaption,sizeof levelCaption,"%s###level",currentLevel);
            if(theme::button(levelCaption,ImVec2(-1,0))) ImGui::OpenPopup("Options level");
            record("Options");
            if(ImGui::BeginPopup("Options level")) {
                for(int i=0;i<3;++i) {
                    const char* name=i==0?"Basic":i==1?"Advanced":"Debug";
                    if(ImGui::Selectable(name,static_cast<int>(level)==i))
                        backend.edit(Setting::ViewLevel,-1,static_cast<float>(i),true);
                    record((std::string("level:")+name).c_str());
                }
                ImGui::EndPopup();
            }
            ImGui::EndTable();
        }
        const int tabCount=level==Level::Debug?9:7;
        // A native single-line tab strip. Narrow windows scroll horizontally;
        // navigation never turns into a second row of action buttons.
        const bool selectRequested=!state.tabsInitialized || state.tab!=state.renderedTab;
        Tab selected=state.tab;
        ImGui::PushStyleVar(ImGuiStyleVar_TabBorderSize,std::max(1.f,theme::unit(1)));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemInnerSpacing,ImVec2(theme::unit(2),theme::unit(6)));
        ImGui::PushStyleColor(ImGuiCol_Border,ImVec4(191.f/255,157.f/255,94.f/255,1));
        if(ImGui::BeginTabBar("pages",ImGuiTabBarFlags_FittingPolicyScroll|ImGuiTabBarFlags_TabListPopupButton|ImGuiTabBarFlags_DrawSelectedOverline)) {
            for(int i=0;i<tabCount;++i) {
                const auto tab=static_cast<Tab>(i);
                const bool active=ImGui::BeginTabItem(tab_name(tab),nullptr,
                    selectRequested && state.tab==tab?ImGuiTabItemFlags_SetSelected:ImGuiTabItemFlags_None);
                record((std::string("tab:")+tab_name(tab)).c_str());
                if(active) {selected=tab;ImGui::EndTabItem();}
            }
            ImGui::EndTabBar();
        }
        ImGui::PopStyleColor();ImGui::PopStyleVar(2);
        if(!selectRequested) state.tab=selected;
        state.renderedTab=selected;state.tabsInitialized=true;
        ImGui::PopStyleVar();
        // Geometry and font height scale separately. Reserving the entire
        // footer times text size wasted body space at larger text settings.
        const float footerHeight=theme::unit(92+20*text);
        const float bodyHeight=std::max(theme::unit(50),ImGui::GetContentRegionAvail().y-footerHeight);
        ImGui::PushID(tab_name(state.tab));
        if(ImGui::BeginChild("body",ImVec2(0,bodyHeight),ImGuiChildFlags_None,ImGuiWindowFlags_AlwaysVerticalScrollbar))
            draw_page(backend,state,level,context);
        ImGui::EndChild();ImGui::PopID();
        const float bodyEnd=ImGui::GetItemRectMax().y;
        theme::rule();
        float percent=text*100.f;
        if(ImGui::BeginTable("footer",2,ImGuiTableFlags_SizingStretchProp,
            ImVec2(ImGui::GetContentRegionAvail().x-ImGui::GetStyle().ScrollbarSize,0))) {
            ImGui::TableSetupColumn("label",0,.44f);ImGui::TableSetupColumn("value",0,.56f);
            label("Text size",true,"TextSize");
            bool changed=theme::slider("##text",&percent,80,150,"%.0f%%");
            bool commit=ImGui::IsItemDeactivatedAfterEdit();record("TextSize");
            if(changed||commit) backend.edit(Setting::TextSize,-1,percent/100.f,commit);
            ImGui::EndTable();
        }
        std::string status=context.notice;
        if(!context.cameraHook) status="Camera hook unavailable - game settings cannot be applied yet.";
        else if(!context.tracking && status.empty()) status="Headset tracking unavailable.";
        if(status.empty()) status="Player settings save automatically. F10 closes.";
        theme::note(status.c_str());
        if(!theme::assets_loaded()) theme::note("Some menu artwork could not be loaded.");
        if(options.layoutValid) {
            const float bottom=ImGui::GetWindowPos().y+ImGui::GetWindowSize().y-ImGui::GetStyle().WindowPadding.y;
            *options.layoutValid &= ImGui::GetItemRectMax().y<=bottom+1 && bodyEnd<bottom;
        }
    }
    ImGui::End();theme::end();g_options=nullptr;
}
} // namespace bvr::b1r::menu
