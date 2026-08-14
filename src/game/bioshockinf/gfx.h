#pragma once
// s53 (I9): the Scaleform/GFx lane - the crosshair kill's derivation surface
// and the future subtitles/HUD-tweak power tool.
//
// The measured starting point (STATUS s52 round 3): the crosshair is a
// Scaleform CLIK widget (XClikHUDCrosshair) and the HUD object's property
// chain carries ZERO crosshair state (full walk) - so the lever must go
// through the movie or the HUD's element machinery, not a property bit.
// The derivation ladder this module serves, in order:
//  1. `bsigfx hud`     - resolve PC->myHUD (the object every later probe
//                        hangs off);
//  2. `bsigfx cmd X`   - the FlashCommand bridge action (ENGINE_NOTES: the
//                        Scaleform bridge - AbortHack, AutoHack, ...): a
//                        one-command probe, negatives cost nothing;
//  3. `bsigfx element` - the game's own hideable-widget machinery
//                        (HideableHUDWidgetNames / NumReasonsToShowElement,
//                        XSeqAct_HideHUDElement's model) - the expected
//                        winner;
//  4. `bsigfx setb`    - GFxMoviePlayer.SetVariableBool on a movie object a
//                        bsifields walk produced (CLIK `_visible` paths) -
//                        the fallback, paths are guesswork.
// The production crosshair toggle wires up AFTER one of these rungs proves
// out flat (center-crop pixel count, vigor equipped as the state oracle).

namespace bvr::bsi::gfx {

// s62b: forward the game's legacy CreateDXGIFactory (DXGI 1.0) to
// CreateDXGIFactory1. A device born of a 1.0 chain cannot create keyed-mutex
// shared resources, which is what SteamVR's vrclient needs for its sync
// texture - without this every OpenVR Submit fails 106. Install at adapter
// init, BEFORE the game touches D3D. BSI-only (BS1/BS2 factories are healthy).
void install_dxgi11_upgrade();

bool handle_command(const char* cmd, const char* args); // bsigfx

// s57: the enumerator as a callable - sweep for live INSTANCES whose class
// name equals `className` (the `bsigfx scan` machinery; the UClass and CDO
// are filtered out). Returns the count written to out (<= cap). READ-ONLY,
// one-shot heap sweep (~0.5 s hitch on a 1 GB heap) on the GAME thread -
// never on a cadence. Returns -1 when the prerequisites are missing
// (UObject::Name underived / the name not in GNames).
int find_instances(const char* className, void** out, int cap);

} // namespace bvr::bsi::gfx
