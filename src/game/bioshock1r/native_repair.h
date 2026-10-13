#pragma once
// Private BS1 foreground repair. Original engine resources stay untouched.
#include <cstdint>
#include "game/bioshock1r/afw_gpu.h"
namespace bvr::pattern_scan { struct ProcessImage; }
namespace bvr::b1r::native_repair {
void init(const bvr::pattern_scan::ProcessImage&);
void set_active(bool);
void note_camera(uint64_t build, int sign, const tgpu::Camera&, float fullSeparation,
                 const tgpu::Params& depth, const afw::Params& warp);
void finish_eye(ID3D11Texture2D* originalBackbuffer, uint64_t build, int sign,
                float tanH, float tanV, bool plainResolution);
bool copy_output(ID3D11DeviceContext*, ID3D11Texture2D* destination, uint64_t build,
                 int freshEye, float tanH, float tanV);
struct Stats {
    uint64_t frames=0, complete=0, shown=0, refused=0, foreground=0, post=0, uploads=0, copies=0;
    uint64_t postSlotsRead=0, postSlotsSet=0;
};
Stats stats();
const char* last_reason();
// Offline fixtures use the same hooks, resources and callbacks as the adapter.
void enable_for_test();
void shutdown();
}
