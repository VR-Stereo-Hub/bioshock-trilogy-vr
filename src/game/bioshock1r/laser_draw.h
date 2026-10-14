#pragma once
// The aim laser drawn INTO each eye image (2026-10-14). The XR quad laser places its dots in the
// room from a conversion of the fire ray; in the headset it still ran off the drawn barrel by a
// different amount per weapon. Drawn here, each eye's dots are the fire ray projected through
// the very camera that eye's image (and the gun in it) was rendered with, so the beam cannot
// part from the gun or from where the bullet goes. Render thread; temporal.cpp calls it on the
// eye images it writes. No engine reads.

#include <stdint.h>

struct ID3D11DeviceContext;
struct ID3D11Texture2D;

namespace bvr::b1r::tgpu {
struct Camera;
}

namespace bvr::b1r::laser_draw {

// One frame's beam, in world UU, published by aim.cpp on the game thread and carried with the
// camera of the build it belongs to.
struct Segment {
    bool valid = false;
    bool laser = false, reticle = false; // what to draw
    float origin[3] = {};                // the fire ray's origin (the muzzle)
    float dir[3] = {1, 0, 0};            // unit
    float nearUu = 5.0f, farUu = 500.0f; // first and last laser dot along the ray
    float reticleUu = 500.0f;            // the reticle's distance along the ray
    int dots = 6;
    float dotDeg = 0.7f, reticleDeg = 0.5f; // angular diameters
    // Diagnostics for the AFW capture (temporal.cpp dump_tick): the engine's muzzle socket and
    // the gun body's barrel axis as last measured (the gun posed one frame earlier).
    bool muzzleValid = false;
    float muzzle[3] = {}, barrelAxis[3] = {};
};

// Draw `s` over `dst` (w x h) as seen by `cam`. False when there is nothing to draw or the
// image cannot be rendered into (logged once).
bool draw(ID3D11DeviceContext* ctx, ID3D11Texture2D* dst, uint32_t w, uint32_t h, const tgpu::Camera& cam,
          const Segment& s);

// Device objects (shutdown / device loss).
void release();

} // namespace bvr::b1r::laser_draw
