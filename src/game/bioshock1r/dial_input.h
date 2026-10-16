#pragma once
// BS1: the weapon and plasmid wheels chosen by moving the hand - the live side of
// wheel_dial.h. The right grip (RB) opens the weapon wheel and the right hand picks;
// the left grip (LB) the plasmid wheel and the left hand. While the game's wheel is
// on screen (wheel_state), the hand's direction replaces the sticks the game
// receives (xinput_bridge::set_stick_override). docs/bioshock1/WEAPON_WHEEL.md.

#include "menu_model.h"

namespace bvr::b1r::dial_input {

void init();                          // registers the stick override

// Where the wheel is drawn at the hand (hud_elements, render thread): the dial the
// last grip press froze - its centre moved along its facing by the distance offset,
// its orientation (+Z toward the viewer), the ring's width - and which wheel that grip
// opens (0 weapons, 1 plasmids). False while the dial is off or that press was not
// tracked; the pose survives the release, for the close lease.
struct Visual {
    int kind = -1;
    float center[3] = {};
    float q[4] = {0, 0, 0, 1};
    float widthM = 0.5f;
};
bool visual(Visual* out);
float menu_read(menu::Setting id);    // NaN = not this module's setting
bool menu_write(menu::Setting id, float value);
void draw_panel();                    // F10 Controls tab: the mirrored wheels, live state

} // namespace bvr::b1r::dial_input
