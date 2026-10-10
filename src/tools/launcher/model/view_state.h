#pragma once
#include "model/launcher.h"
#include "sys/updates.h"
#include <array>

namespace bvr::launcher {
enum class Page { Overview, Settings, Mods, Bindings, Updates, Help, Result };
enum class Action {
    None, Browse, Refresh, Install, Apply, Reinstall, Disable, Enable, Uninstall,
    Defaults, Play, Close, GameFolder, DataFolder, BackupFolder, Support,
    DesktopShortcut, StartShortcut, CheckUpdates, DownloadUpdate, OpenUpdate, Releases
};
struct Hitbox { std::string label; float x, y, w, h; bool enabled; };
struct ViewState {
    Detection detection;
    Payload payload;
    Choices draft;
    Page page = Page::Overview;
    int settingsTab = 0;
    float scale = 1;
    bool busy = false, fixture = false, confirmUninstall = false, confirmDefaults = false;
    bool confirmPlay = false, closeRequested = false, confirmClose = false;
    bool advanced = false;
    std::string notice, busyText;
    Report report;
    updates::Check releases;
    std::wstring downloaded;
    char customHeadset[97]{};
    int resolutionW = 2750, resolutionH = 2850;
    float pixelPercent = 100;
    std::vector<Hitbox> hitboxes;
    int layoutErrors = 0;
};
ViewState fixture(const std::string& name, const Payload& payload);
const std::vector<std::string>& fixture_names();
void reset_draft(ViewState& state);
}
