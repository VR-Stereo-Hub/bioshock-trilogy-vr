#pragma once
// The room-to-game transform that placed this frame's tracked foreground.
// Carry history through locomotion only; head and controller motion must remain
// relative to this frame. No engine pointers or retained objects are involved.
#include "game/bioshock1r/frame_context.h"
#include "game/bioshock1r/temporal_gpu.h"
#include <cmath>

namespace bvr::b1r::afw {
struct TrackingFrame {
    bool valid = false;
    float origin[3] = {};
    float yaw = 0.0f;
    float scale = 100.0f;
};

inline TrackingFrame tracking_frame(const FrameContext& ctx) {
    TrackingFrame f;
    f.valid = ctx.vrDriving;
    const float pos[3] = {}, quat[4] = {0,0,0,1};
    const auto p = xr_pose_to_game(ctx, pos, quat);
    f.origin[0] = p.loc.x; f.origin[1] = p.loc.y; f.origin[2] = p.loc.z;
    f.yaw = static_cast<float>(ctx.camYaw) / kRotUnitsPerRadian -
            ctx.driveYawOffsetRad - ctx.recenterYawRad;
    f.scale = ctx.worldScale;
    return f;
}

inline bool transport_foreground_camera(const tgpu::Camera& source,
                                        const TrackingFrame& from,
                                        const TrackingFrame& to,
                                        tgpu::Camera& result) {
    if (!from.valid || !to.valid || !std::isfinite(from.yaw) ||
        !std::isfinite(to.yaw) || !std::isfinite(from.scale) ||
        !std::isfinite(to.scale) || from.scale <= 0 || from.scale != to.scale)
        return false; // a scale change would also require changing stored depth
    for (int i=0; i<3; ++i)
        if (!std::isfinite(from.origin[i]) || !std::isfinite(to.origin[i])) return false;
    const double c = std::cos(double(to.yaw)-from.yaw);
    const double s = std::sin(double(to.yaw)-from.yaw);
    result = source;
    const double x = double(source.loc[0])-from.origin[0];
    const double y = double(source.loc[1])-from.origin[1];
    result.loc[0] = float(to.origin[0]+c*x-s*y);
    result.loc[1] = float(to.origin[1]+s*x+c*y);
    result.loc[2] = source.loc[2]-from.origin[2]+to.origin[2];
    auto rotate = [&](const float* a, float* b) {
        b[0]=float(c*a[0]-s*a[1]); b[1]=float(s*a[0]+c*a[1]); b[2]=a[2];
    };
    rotate(source.fwd,result.fwd);
    rotate(source.right,result.right);
    rotate(source.up,result.up);
    return true;
}
} // namespace bvr::b1r::afw
