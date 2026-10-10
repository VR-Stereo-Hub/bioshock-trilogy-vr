#include "model/view_state.h"
#include <algorithm>

namespace bvr::launcher {
void reset_draft(ViewState& s) {
    s.draft = {};
    s.resolutionW = s.detection.width ? static_cast<int>(s.detection.width) : 2750;
    s.resolutionH = s.detection.height ? static_cast<int>(s.detection.height) : 2850;
    s.pixelPercent = static_cast<float>(s.resolutionW) * s.resolutionH / (2750.f * 2850.f) * 100;
}
const std::vector<std::string>& fixture_names() {
    static const std::vector<std::string> names = {
        "overview", "setup", "display", "controls", "mods", "bindings", "updates", "help",
        "missing-game", "running", "disabled", "uninstall", "defaults", "success", "failure", "busy", "long-path", "dirty-play"
    };
    return names;
}
ViewState fixture(const std::string& name, const Payload& payload) {
    ViewState s; s.payload = payload; s.fixture = true;
    auto& d = s.detection;
    d.found = d.installed = d.managed = d.payloadOk = d.writable = d.matches = true;
    d.nativePresent = d.vdxr = d.gameIniValid = true;
    d.source = "Steam library"; d.running = process::Running::No;
    d.env.gameDir = L"C:\\Program Files (x86)\\Steam\\steamapps\\common\\BioShock Remastered\\Build\\Final";
    d.env.dataDir = L"C:\\Users\\Player\\AppData\\Local\\BioshockVR";
    d.env.gameIni = L"C:\\Users\\Player\\AppData\\Roaming\\BioshockHD\\Bioshock\\Bioshock.ini";
    d.runtime = "auto"; d.headset = "Meta Quest 3 / 3S";
    d.installedVersion = payload.version; d.installedBuild = payload.build;
    d.graphics.name = L"NVIDIA GeForce RTX 4060"; d.graphics.budgetBytes = 7ull * 1024 * 1024 * 1024;
    d.activeRuntime = "Virtual Desktop OpenXR (32 bit)"; d.width = 2750; d.height = 2850;
    d.preferences = {{"SnapTurn", 1}, {"SnapAngle", 45}, {"TurnSpeed", 1}, {"Swing", 1}, {"SwingSpeed", 3.6f},
        {"AutoStart", 1}, {"Laser", 0}, {"Reticle", 1}, {"HideBars", 1}};
    if (name == "setup") { d.installed = d.managed = d.matches = false; s.page = Page::Settings; }
    if (name == "display") s.page = Page::Settings;
    if (name == "controls") { s.page = Page::Settings; s.settingsTab = 1; }
    if (name == "mods" || name == "uninstall" || name == "defaults") s.page = Page::Mods;
    if (name == "bindings") s.page = Page::Bindings;
    if (name == "updates") {
        s.page = Page::Updates; s.releases.online = true; s.releases.message = "Release history preview";
        updates::Release r; r.version = payload.version; r.published = "2026-08-23";
        r.notes = "BioShock Remastered VR\nStereo rendering, tracked hands and motion controls.\nPlayer settings are retained during updates.";
        s.releases.releases.push_back(r);
    }
    if (name == "help") s.page = Page::Help;
    if (name == "missing-game") { d.found = d.installed = d.managed = false; d.env.gameDir.clear(); d.error = "Choose your BioShock Remastered installation to begin."; }
    if (name == "running") d.running = process::Running::Yes;
    if (name == "disabled") { d.disabled = true; s.page = Page::Mods; }
    if (name == "uninstall") s.confirmUninstall = true;
    if (name == "defaults") s.confirmDefaults = true;
    if (name == "busy") { s.busy = true; s.busyText = "Verifying the installation and keeping a recovery copy..."; }
    if (name == "long-path") {
        d.env.gameDir = L"D:\\A Steam library with a very long name\\Additional storage\\steamapps\\common\\BioShock Remastered\\Build\\Final";
        d.graphics.name = L"NVIDIA GeForce RTX graphics adapter with a longer device description";
    }
    if (name == "dirty-play") { s.page = Page::Settings; s.draft.runtime = "steamvr"; s.confirmPlay = true; }
    if (name == "success" || name == "failure") {
        s.page = Page::Result;
        if (name == "success") {
            s.report.add(Status::Ok, "Mod and SteamVR files installed", "All four payload hashes match this launcher.");
            s.report.add(Status::Ok, "Your settings were kept", "Existing calibration and weapon profiles remain in place.");
            s.report.add(Status::Note, "Recovery copy saved", "The backup includes the entire previous version of each changed file.");
        } else {
            s.report.fail("Could not write a file", "Windows denied access to the selected game folder.", ERROR_ACCESS_DENIED);
            s.report.add(Status::Warning, "Previous files restored", "Your previous installation is intact. Close the game and try again.");
        }
    }
    reset_draft(s);
    if (name == "dirty-play") s.draft.runtime = "steamvr";
    return s;
}
}
