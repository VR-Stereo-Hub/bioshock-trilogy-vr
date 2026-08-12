#pragma once
// s57 (I9): the flat-screen CROSSHAIR kill - the crosshair hunt's landing.
//
// The derivation (s57 probe boot, user's gameplay save): `bsigfx scan
// XClikHUDCrosshair` found TWO live instances (plus the UClass); the widget's
// Outer (+0x14) is the HUD screen instance XSinglePlayerGFxHUD - the object
// the s53 hunt could never reach (myHUD is bare even in gameplay; the
// HideableHUDWidgetNames/NumReasonsToShowElement arrays exist on NO live
// chain - XGameHUD's DisabledHUDWidgets TArray is empty/unallocated, parked).
// The widget itself carries BoolProperty IsShown and IsCenterpointVisible,
// BOTH at byte offset 0x118 (masks 0x1 and 0x2, declaration order; verified
// live: the active crosshair read 3, the parked spare read 2). Clearing both
// bits stuck for minutes across idle and a fire press - no game re-assert
// observed flat; combat re-assertion is covered by this module's watchdog.
//
// Policy: while enabled (default ON, config key hudCrosshairHide), find the
// live XClikHUDCrosshair instances (the gfx enumerator - one-shot ~0.5 s
// game-thread sweep, retried with backoff only while instances are missing
// in live gameplay), derive the IsShown offset BY NAME on the instance
// (refuse-latch on mismatch - never a blind write), clear IsShown +
// IsCenterpointVisible, and re-clear on a slow watchdog if the game re-sets
// them. `bsixhair off` restores each instance's original bits - the headset
// A/B. Flat captures cannot judge the visual (out of combat the crosshair
// collapses to a dot); the acceptance is the user's combat look.

#include <cstdint>

namespace bvr::bsi::xhair {

// Game thread (camera detour tail). Cheap when idle; the sweep only runs
// while enabled, in live gameplay, with no instances cached.
void tick(uint64_t nowMs);

bool enabled();
void set_enabled(bool on); // config/F10 writer

bool handle_command(const char* cmd, const char* args); // bsixhair
void draw_debug_ui(); // nested in the HUD (I9) F10 section

} // namespace bvr::bsi::xhair
