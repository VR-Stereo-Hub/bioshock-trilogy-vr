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
struct ID3D11ShaderResourceView;

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
    // 2026-10-25: a SECOND reticle, for the right hand's cast with the plasmids dual-wielded (aim.cpp):
    // its own ray, drawn like the first.
    bool reticle2 = false;
    float origin2[3] = {};
    float dir2[3] = {1, 0, 0};
    float reticle2Uu = 500.0f;
    // 2026-10-25 (the dynamic reticle, as BO3 Zombies VR's aim aid): the reticles sit where their ray meets the
    // scene (measure() below), at reticleUu / reticle2Uu when nothing is measured yet.
    bool dynamic = true;
};

// 2026-10-25: the dynamic reticle distance. BO3 Zombies VR and Darkness II VR ask their engine's ray trace for the
// distance along the muzzle ray; nothing in this mod can trace, so this marches the ray through the eye's own
// linear view depth (temporal_gpu linear_z, the viewmodel left out by its hands mask) on the GPU and reads the
// hit back a frame or two later. One call per fresh eye image, with that image's camera. Render thread.
void measure(ID3D11DeviceContext* ctx, ID3D11ShaderResourceView* linearZ, ID3D11ShaderResourceView* handsMask,
             uint32_t w, uint32_t h, const tgpu::Camera& cam, const Segment& s);
// The last measured distance along reticle `i`'s ray (uu; the far end when nothing was hit), or < 0 when none is
// recent. Any thread.
float measured_uu(int i);

// Draw `s` over `dst` (w x h) as seen by `cam`. False when there is nothing to draw or the
// image cannot be rendered into (logged once).
bool draw(ID3D11DeviceContext* ctx, ID3D11Texture2D* dst, uint32_t w, uint32_t h, const tgpu::Camera& cam,
          const Segment& s);

// Device objects (shutdown / device loss).
void release();

} // namespace bvr::b1r::laser_draw
