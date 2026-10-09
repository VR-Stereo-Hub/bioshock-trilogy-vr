#include "menu_preferences.h"
#include <cctype>
#include <iomanip>
#include <locale>
#include <sstream>

namespace bvr::b1r::menu {
bool Preferences::set(const Preference& p, bool explicitDebugSave) {
    const auto& s = spec(p.id);
    if (!valid_value(s,p.value) || (!is_player_setting(p.id) && !explicitDebugSave)) return false;
    if (s.scope == Scope::Global ? p.hand != -1 : (p.hand < 0 || p.hand > 1)) return false;
    if (s.scope == Scope::Profile) {
        if (p.profile.empty() || p.profile.size() > 96) return false;
        for (unsigned char ch : p.profile) if (!std::isalnum(ch) && ch != '_') return false;
    } else if (!p.profile.empty()) return false;
    const std::string key = std::string(s.key) + ":" + std::to_string(p.hand) + ":" + p.profile;
    entries_[key] = p;
    return true;
}
unsigned Preferences::parse(const std::string& text) {
    std::istringstream input(text);
    input.imbue(std::locale::classic());
    std::string line;
    unsigned rejected = 0;
    while (std::getline(input,line)) {
        if (line.empty() || line[0]=='#' || line[0]=='\r') continue;
        std::istringstream row(line);
        row.imbue(std::locale::classic());
        char tier = 0; std::string key, profile, extra; int hand = -1; float value = 0;
        if (!(row >> tier >> key >> hand >> profile >> value) || (row >> extra)) { ++rejected; continue; }
        std::size_t count = 0; const Spec* all = specs(count);
        bool accepted = false;
        for (std::size_t i=0;i<count;++i) if (key==all[i].key) {
            if ((tier=='P' && is_player_setting(all[i].id)) || (tier=='D' && !is_player_setting(all[i].id)))
                accepted = set({all[i].id,hand,profile=="-"?"":profile,value},tier=='D');
            break;
        }
        if (!accepted) ++rejected;
    }
    return rejected;
}
std::string Preferences::serialize() const {
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << "# BioShock F10 preferences v1\r\n"
           "# Player choices override legacy ini values. Debug entries require an explicit save.\r\n";
    out << std::setprecision(9);
    for (const auto& [key,p] : entries_) {
        out << (is_player_setting(p.id)?'P':'D') << ' ' << spec(p.id).key << ' ' << p.hand << ' '
            << (p.profile.empty()?"-":p.profile) << ' ' << p.value << "\r\n";
    }
    return out.str();
}
} // namespace bvr::b1r::menu
