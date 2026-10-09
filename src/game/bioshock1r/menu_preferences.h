#pragma once
#include "menu_model.h"
#include <map>
#include <string>

namespace bvr::b1r::menu {
struct Preference {
    Setting id;
    int hand = -1;
    std::string profile;
    float value = 0;
};
// Pure storage policy, also exercised by the offline test executable.
class Preferences {
public:
    bool set(const Preference& value, bool explicitDebugSave = false);
    unsigned parse(const std::string& text);
    std::string serialize() const;
    const std::map<std::string, Preference>& entries() const { return entries_; }
    void clear() { entries_.clear(); }
private:
    std::map<std::string, Preference> entries_;
};
} // namespace bvr::b1r::menu
