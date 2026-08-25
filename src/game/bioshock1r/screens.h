// BioShock 1: WHICH INTERFACE SCREEN IS UP, by name.
//
// The whole interface is a stack of named Flash movies and the engine has a
// GetTopPlayingMovie getter, which BRVR decoded by eye as a pure field walk
// rather than calling it (it is an exec native taking an FFrame). Walking it
// ourselves gives the top screen's FILENAME, which is an exact answer where
// every render-side signal is an inference.
//
// MEASURED on this build, 2026-08-21, first run of the probe:
//
//   HUDRadial.swf        ordinary gameplay
//   PausePC.swf          the pause menu
//   craftingstation.swf  a vending machine
//   Fadeout.swf          a loading transition
//
// This replaces the LevelInfo::Pauser attempt, which read null through three
// real pauses. That field was never a measured result in BRVR either: its
// detector shared a dependency with two others and all three were silent for a
// whole session with their switches on.
//
// Derivation, the field walk and the fail-closed argument are in
// docs/bioshock1/ENGINE_NOTES.md under "The Flash movie stack".

#pragma once

#include <cstddef>

namespace bvr::b1r::screens {

// Called from the CalcView hook with the player controller and the view actor.
// Locates LevelInfo, resolves the GUI controller, then costs a few reads.
// Self-throttling.
void on_calcview(void* pc, void* viewActor);

// The top playing movie's filename, e.g. "PausePC.swf". Empty string when the
// chain has not resolved. Never null.
const char* top_movie();

// The top movie is one that should be shown as a stationary panel rather than
// drawn over a live world: the pause menu, the map, the manual, the machine
// flows. False whenever the chain has not resolved, so an unresolved chain
// leaves placement exactly as it was.
bool panel_screen_up();

// Located LevelInfo, for anything else that needs it. Null until found.
void* level();

// `vrscreens list|add <name>|clear|status`.
void handle_command(const char* args);

} // namespace bvr::b1r::screens
