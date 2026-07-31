#pragma once
// BioShock Infinite (UE3 build 6829) adapter. I1 scope: the skeleton - build
// gate, read-only probes, and the command seam. No engine hook exists yet, so
// it advertises no capabilities; the camera seam is I2.

#include "game/igame_adapter.h"

namespace bvr::bsi {

class BioshockInfAdapter final : public bvr::game::IGameAdapter {
public:
    uint32_t capabilities() const override;
    bool init(const bvr::pattern_scan::ProcessImage& image) override;
    void setFov(float hfovDeg) override;
    void drawDebugUi() override;
    bool handleCommand(const char* cmd, const char* args) override;
};

// The process-lifetime instance, handed to game/adapter_registry.cpp.
bvr::game::IGameAdapter* create_adapter();

} // namespace bvr::bsi
