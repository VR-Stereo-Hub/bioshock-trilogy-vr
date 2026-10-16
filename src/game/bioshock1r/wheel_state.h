#pragma once
// BS1: is a weapon/plasmid wheel on screen, and how many slots does each hold?
// Learned from the HUD movie's own calls: one MinHook detour on the routine every
// call into a gameswf movie goes through (patterns.h kFlashCallMethodRva, tools\ida
// fw1/fw2), feeding wheel_mirror.h. Observe only - every call is forwarded unchanged.
// docs/bioshock1/WEAPON_WHEEL.md 3.8; ENGINE_NOTES "The weapon and plasmid wheels".

#include "core/hooks/pattern_scan.h"

namespace bvr::b1r::wheel_state {

bool install(const bvr::pattern_scan::ProcessImage& image); // byte-verified; false = unavailable
bool live();          // the hook is in and the HUD movie has spoken since it started
int count(int kind);  // filled slots: 0 weapons, 1 plasmids
int open_kind();      // the wheel on screen: 0 weapons, 1 plasmids, -1 none
void draw_status();   // F10 (render thread): the mirrored wheels

} // namespace bvr::b1r::wheel_state
