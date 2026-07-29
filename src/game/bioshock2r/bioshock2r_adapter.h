#pragma once
// BioShock 2 Remastered adapter: thin glue between the IGameAdapter seam and
// this game's patterns/camera modules. M3 scope - camera only.

#include "game/igame_adapter.h"

namespace bvr::b2r {

class Bioshock2RAdapter final : public bvr::game::IGameAdapter {
public:
    uint32_t capabilities() const override;
    bool init(const bvr::pattern_scan::ProcessImage& image) override;
    void setFov(float hfovDeg) override;
    void drawDebugUi() override;
};

// The process-lifetime instance, handed to game/adapter_registry.cpp.
bvr::game::IGameAdapter* create_adapter();

} // namespace bvr::b2r
