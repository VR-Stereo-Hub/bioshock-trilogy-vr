#pragma once
#include "menu_model.h"
#include <imgui.h>
#include <vector>

namespace bvr::b1r::menu {
struct Hit {
    std::string name;
    ImVec2 min, max;
    ImGuiID id;
    bool disabled;
    std::string window; // offline interaction harness: scroll the owning region into view
};
struct ViewState {
    float pixelPercent = 0;
    Tab tab = Tab::Controls;
    Tab renderedTab = Tab::Controls;
    bool tabsInitialized = false;
    int hand = 1;
    // Actual eye-space geometry, retained across resolution changes. Moving or
    // resizing the panel changes these values through ImGui, not saved pixels.
    ImVec2 lastDisplay{}, lastPosition{}, lastSize{};
    float fontPixels = 0;
};
struct ViewOptions {
    // The harness supplies a fixed viewport. Zero uses the live eye target.
    ImVec2 captureSize{};
    float captureScale = 1;
    bool expandSections = false;
    std::vector<Hit>* hits = nullptr;
    bool* layoutValid = nullptr;
};
void draw(Backend& backend, ViewState& state, const ViewOptions& options = {});
} // namespace bvr::b1r::menu
