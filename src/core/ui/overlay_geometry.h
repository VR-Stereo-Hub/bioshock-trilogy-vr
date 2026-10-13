#pragma once
#include <cmath>
#include <cstdint>
namespace bvr::overlay {
// Shared by composition and controller intersection. XR head coordinates:
// +X right, +Y up, -Z forward. Dimensions are meters.
struct QuadGeometry { float depth=0, width=0, height=0; };
inline QuadGeometry quad_geometry(float depth,float tanH,float tanV) {
    if(!std::isfinite(depth) || !std::isfinite(tanH) || !std::isfinite(tanV) ||
       depth<=0 || tanH<=0 || tanV<=0) return {};
    return {depth,2*depth*tanH,2*depth*tanV};
}
inline bool quad_pixel(const float origin[3],const float direction[3],const QuadGeometry& q,
                       uint32_t w,uint32_t h,float* px,float* py) {
    if(q.depth<=0 || q.width<=0 || q.height<=0 || !w || !h || direction[2]>=-1e-5f) return false;
    const float t=(-q.depth-origin[2])/direction[2];
    if(!std::isfinite(t) || t<=0) return false;
    const float x=origin[0]+direction[0]*t, y=origin[1]+direction[1]*t;
    *px=(x/q.width+.5f)*w; *py=(.5f-y/q.height)*h;
    return std::isfinite(*px) && std::isfinite(*py);
}
} // namespace bvr::overlay
