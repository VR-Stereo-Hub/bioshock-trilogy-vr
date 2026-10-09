#include "model/launcher.h"
#include "sys/fs.h"
#include "game/bioshock1r/menu_model.h"
#include "game/bioshock1r/menu_preferences.h"
#include <cmath>
#include <iomanip>
#include <locale>
#include <sstream>
#include <set>

namespace bvr::launcher {
namespace {
const b1r::menu::Spec* setting(const std::string& key) {
    size_t n; const auto* specs = b1r::menu::specs(n);
    for (size_t i = 0; i < n; ++i) if (key == specs[i].key) return &specs[i];
    return nullptr;
}
std::string read(const std::wstring& path) {
    std::vector<unsigned char> b;
    if (fs::file_size(path) > 8 * 1024 * 1024 || !fs::read_file(path, &b, nullptr)) return {};
    return {b.begin(), b.end()};
}
}
bool valid_choices(const Choices& c, std::string* why) {
    if (!c.runtime.empty() && c.runtime != "auto" && c.runtime != "native" && c.runtime != "steamvr") {
        *why = "Choose Automatic, Native OpenXR or SteamVR."; return false;
    }
    if ((c.width || c.height) && (c.width < 640 || c.height < 480 || c.width > 8192 || c.height > 8192)) {
        *why = "Resolution must be between 640 x 480 and 8192 x 8192."; return false;
    }
    if (c.headset.size() > 96 || c.headset.find_first_of("\r\n\t=[]") != std::string::npos) {
        *why = "The headset name contains unsupported characters."; return false;
    }
    for (const auto& [key, value] : c.preferences) {
        const auto* s = setting(key);
        if (!s || s->scope != b1r::menu::Scope::Global || !b1r::menu::is_player_setting(s->id) ||
            !b1r::menu::valid_value(*s, value)) {
            *why = "Invalid launcher setting: " + key; return false;
        }
    }
    return true;
}
bool patch_preferences(const std::string& source, const std::map<std::string, float>& edits,
                       std::string* output, std::string* why) {
    Choices c; c.preferences = edits;
    if (!valid_choices(c, why)) return false;
    if (source.find('\0') != std::string::npos) { *why = "The F10 preferences file is not plain text."; return false; }
    *output = ""; std::set<std::string> written;
    // Preserve every unknown, per-hand and per-weapon row byte for byte. The
    // F10 parser's serialized form is not used to rewrite a user's file.
    size_t at = 0;
    while (at < source.size()) {
        size_t end = source.find('\n', at);
        if (end == std::string::npos) end = source.size(); else ++end;
        std::string line = source.substr(at, end - at), key, profile, extra;
        std::istringstream in(line); in.imbue(std::locale::classic());
        char tier; int hand; float value;
        if ((in >> tier >> key >> hand >> profile >> value) && !(in >> extra) &&
            tier == 'P' && hand == -1 && profile == "-" && edits.count(key)) {
            const auto eol = line.size() >= 2 && line.substr(line.size() - 2) == "\r\n" ? "\r\n" :
                (!line.empty() && line.back() == '\n' ? "\n" : "");
            std::ostringstream out; out.imbue(std::locale::classic());
            out << "P " << key << " -1 - " << std::setprecision(9) << edits.at(key) << eol;
            line = out.str(); written.insert(key);
        }
        *output += line; at = end;
    }
    const char* eol = source.find("\r\n") != std::string::npos || source.find('\n') == std::string::npos ? "\r\n" : "\n";
    for (const auto& [key, value] : edits) if (!written.count(key)) {
        if (!output->empty() && output->back() != '\n') *output += eol;
        std::ostringstream out; out.imbue(std::locale::classic());
        out << "P " << key << " -1 - " << std::setprecision(9) << value << eol;
        *output += out.str();
    }
    // Verify through the production parser, not through the writer's own rules.
    b1r::menu::Preferences verified; verified.parse(*output);
    for (const auto& [key, value] : edits) {
        auto found = verified.entries().find(key + ":-1:");
        if (found == verified.entries().end() || found->second.value != value) {
            *why = "The F10 reader did not accept " + key; return false;
        }
    }
    return true;
}
void read_preferences(const Environment& env, std::map<std::string, float>* values) {
    // These are the shipped preset fields exposed by this launcher. Absence is
    // left unspecified in the UI; installing/updating never writes defaults.
    const std::map<std::string, std::string> legacy = {
        {"snapTurn", "SnapTurn"}, {"snapAngleDeg", "SnapAngle"}, {"turnScale", "TurnSpeed"},
        {"autoVr", "AutoStart"}, {"swingOn", "Swing"}, {"swingThreshold", "SwingSpeed"},
        {"cineBarsHidden", "HideBars"}, {"laserOn", "Laser"},
        {"aimDotOn", "Reticle"}, {"headUpUu", "Height"}
    };
    std::istringstream in(read(fs::join(env.dataDir, L"vrpreset.ini"))); in.imbue(std::locale::classic());
    std::string line;
    while (std::getline(in, line)) {
        auto eq = line.find('=');
        if (eq == std::string::npos || line.empty() || line[0] == '#') continue;
        const auto key = line.substr(0, eq); auto found = legacy.find(key);
        if (found == legacy.end()) continue;
        std::istringstream val(line.substr(eq + 1)); val.imbue(std::locale::classic());
        float f;
        if (val >> f) { const auto* s = setting(found->second); if (s && b1r::menu::valid_value(*s, f)) (*values)[found->second] = f; }
    }
    b1r::menu::Preferences prefs; prefs.parse(read(fs::join(env.dataDir, L"menu-settings.ini")));
    for (const auto& [name, p] : prefs.entries()) if (p.hand == -1 && p.profile.empty()) (*values)[b1r::menu::spec(p.id).key] = p.value;
}
}
