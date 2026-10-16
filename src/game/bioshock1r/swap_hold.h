#pragma once
// BS1: hold the game's hand swap while a weapon or plasmid wheel is open.
//
// Opening the other hand's wheel makes the HUD movie run the console command
// SwitchHandModes before the wheel shows (its AutoSwap), and the plasmid wheel sends
// SelectPlasmid every time the stick lands on an item. Each one equips, then the open
// wheel pauses the game, so the picking arm sat frozen in the equip pose while the mod
// carried it to the controller (headset runs 3 and 4). With the switch on (F10 >
// Controls > Weapon and plasmid dial > Hold the hand swap until the wheel closes), those
// two commands are held while the wheel is open and sent, in the game's own order, the
// moment the release reaches the movie - so the arm keeps its pose until the pick lands.
// The seam is gameswf's fscommand callback (patterns.h kFsCallback*, tools\ida\fc2): a
// global function pointer, swapped, never a code patch.

#include <cstdint>

namespace bvr::b1r::swap_hold {

// The game thread, from wheel_state's call detour: swap the callback pointer in once the
// engine has registered it (and again if it ever registers another).
void poll(const uint8_t* image);
// The game thread, before the release call reaches the movie: send what was held.
void on_release();
// A new HUD movie (InitializeMovie): anything held belongs to the old one.
void clear();
bool enabled();
void set_enabled(bool on);

} // namespace bvr::b1r::swap_hold
