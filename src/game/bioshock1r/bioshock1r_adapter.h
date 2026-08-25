#pragma once
// BioShock 1 Remastered adapter: thin glue between the IGameAdapter seam and
// this game's patterns/camera modules.

#include "game/igame_adapter.h"

namespace bvr::b1r {

class Bioshock1RAdapter final : public bvr::game::IGameAdapter {
public:
    uint32_t capabilities() const override;
    bool init(const bvr::pattern_scan::ProcessImage& image) override;
    void setFov(float hfovDeg) override;
    void drawDebugUi() override;
    bool handleCommand(const char* cmd, const char* args) override;
};

// The process-lifetime instance, handed to game/adapter_registry.cpp.
bvr::game::IGameAdapter* create_adapter();

} // namespace bvr::b1r
