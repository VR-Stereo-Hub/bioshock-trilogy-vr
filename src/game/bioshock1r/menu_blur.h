#pragma once
// BS1: the blur the paused game draws over the view behind its menus and the weapon and
// plasmid wheels. In VR it is a blurred box that follows the head. The camera's post-effect
// runner (patterns.h kPostEffectRunRva) draws it; while the gate is set the runner is skipped,
// so the live image shows. ENGINE_NOTES "The blur behind the menus".

#include "core/hooks/pattern_scan.h"

namespace bvr::b1r::menu_blur {

bool install(const bvr::pattern_scan::ProcessImage& image);
// camera.cpp, every CalcView: true while a menu the player opened or a wheel is up and
// F10 HUD > Menus > Hide the blurred backdrop is on. Logs each close with the runs it skipped.
void set_suppressed(bool on);

} // namespace bvr::b1r::menu_blur
