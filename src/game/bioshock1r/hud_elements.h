#pragma once
// BS1 HUD elements (the BS1 HUD overhaul, 2026-10-15): the Dishonored VR mod's
// per-element HUD on BioShock's own pieces. The gameswf HUD is still captured
// whole into one target (core/gfx/hud_capture); this module names the pieces
// of that capture by rectangle and gives each its own anchor - the window (the
// head-locked panel, the default and the old picture), hidden, the world, or
// either hand. The split runs on the HUD swapchain image (core's
// set_hud_elements_provider): no per-draw work, no extra capture targets.
//
// The pieces and where the game puts them come from the HUD movie itself
// (HUDPC.swf's ActionScript and placements, sharedlibrary.swf's art):
// docs/bioshock1/ENGINE_NOTES.md "The HUD's pieces".

#include "menu_model.h"

namespace bvr::b1r::hud_elements {

void init();                   // registers the provider, reads hud-elements.ini
float menu_read(menu::Setting id);   // NaN = not this module's setting
bool menu_write(menu::Setting id, float value);
void draw_panel();             // F10 HUD tab: rectangles, dump, park (render thread)

// Game-thread state the pieces follow.
void set_plasmid_in_hand(bool plasmid);   // aim.cpp, on every held-item change
// The Menus rows: does this screens::PanelKind ride the HUD instead of the flat
// screen? camera.cpp asks before publishing a UI pause, then says which one rides.
bool screen_rides(int panelKind);
void set_riding_screen(int panelKind);    // 0 = none
bool hide_backdrop();                     // F10 HUD > Menus > Hide the blurred backdrop

} // namespace bvr::b1r::hud_elements
