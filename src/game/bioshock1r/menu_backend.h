#pragma once
#include "menu_model.h"

namespace bvr::b1r::menu {
Backend& live_backend();
// Initial loading and all mutations occur on the game/control thread.
void load_preferences();
void reapply_preferences();
void tick();
// Called after the existing weapon profile has applied, never before it.
void profile_changed(const char* key, int hand);
} // namespace bvr::b1r::menu
