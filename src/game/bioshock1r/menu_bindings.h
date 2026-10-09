#pragma once
#include "menu_model.h"
#include <limits>

// Narrow settings access, implemented beside the owning module's atomics.
// Setters run on the game thread and never introduce engine-memory writes.
namespace bvr::b1r::camera {
float menu_read(menu::Setting id);
bool menu_write(menu::Setting id, float value);
void menu_recenter();
}
namespace bvr::b1r::aim {
float menu_read(menu::Setting id);
bool menu_write(menu::Setting id, float value);
}
namespace bvr::b1r::hands {
float menu_read(menu::Setting id);
bool menu_write(menu::Setting id, float value);
int menu_max_mode();
}
namespace bvr::b1r::bones {
float menu_read(menu::Setting id, int hand);
bool menu_write(menu::Setting id, int hand, float value);
bool menu_has_solver_choice();
}
