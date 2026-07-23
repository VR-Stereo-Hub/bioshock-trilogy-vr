#pragma once
// The core/game seam. Core code sees only this interface; everything
// engine-specific (addresses, Unreal units, UObject semantics) lives behind
// it in game/<title>/. The interface grows one milestone at a time - see the
// ARCHITECTURE.md decision log; onCalcView/execConsole/aim/hands/re-entry
// land with M3+.

#include "core/hooks/pattern_scan.h"

#include <cstdint>

namespace bvr::game {

enum : uint32_t {
    CAP_CAMERA_OVERRIDE = 1u << 0,
    CAP_FOV_WRITE       = 1u << 1,
    CAP_CONSOLE_EXEC    = 1u << 2, // reserved - M5
    CAP_AIM_OVERRIDE    = 1u << 3, // reserved - M6
    CAP_HANDS_ATTACH    = 1u << 4, // reserved - M7
    CAP_SCENE_REENTRY   = 1u << 5, // reserved - M4
    CAP_HUD_CAPTURE     = 1u << 6, // reserved - M9
};

struct IGameAdapter {
    virtual ~IGameAdapter() = default;

    virtual uint32_t capabilities() const = 0;

    // Run pattern scans and install engine hooks. False = nothing usable
    // resolved; the game keeps running flat either way.
    virtual bool init(const pattern_scan::ProcessImage& image) = 0;

    virtual void setFov(float hfovDeg) = 0; // <= 0 disables the per-frame write

    // ImGui widgets for the overlay's adapter section. Engine-semantic UI
    // (units, offsets) stays behind the seam this way.
    virtual void drawDebugUi() = 0;
};

// Fail-soft: any scan/hook failure is logged and the adapter reports zero
// capabilities; the game runs flat.
void init_adapter();
IGameAdapter* adapter(); // null until init_adapter() has run

} // namespace bvr::game
