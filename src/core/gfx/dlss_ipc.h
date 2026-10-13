#pragma once
// The wire contract between the 32-bit mod (core/gfx/dlss_client.cpp) and the 64-bit NGX
// helper (src/tools/dlss_host). Plain fixed-size packed structs, no pointers: one side
// compiles this as x86 and the other as x64, and the static asserts below hold on both.
//
// Ported from the Dishonored VR mod's dlss_ipc.h (the same author family; its FSR backend
// is left out here). That design is adapted from the BioShock VR DLSS/DLAA community fork
// (MIT), itself derived from DLSS5-Feeder (MIT); see src/tools/dlss_host/NOTICE.md.
//
// NGX is 64-bit only, so it runs in a helper process. Every shared object (textures and
// fences) is created by the GAME on its D3D11 device and duplicated INTO the helper (the
// game holds the helper's process handle from CreateProcess, so the helper never needs
// access to the game process).
//
// Per eye image n:
//   game:   copy colour/depth/motion into the eye's shared inputs, Signal(in, n), send Frame
//   helper: queue Wait(in, n), evaluate, Signal(out, n), send FrameAck
//   game:   CPU-wait for the FrameAck (bounded, escapes on helper exit), then GPU Wait(out, n)
//           and read the eye's shared Output
// The CPU wait comes first so a dead helper can never leave the game's GPU queue waiting
// on a fence nobody will signal.

#include <stdint.h>

namespace bvr::dlss_ipc {

const uint32_t kMagic   = 0x53534C44u;   // 'DLSS'
const uint32_t kVersion = 1u;

// The pipe: \\.\pipe\bvr-dlss.<game pid>. The helper creates it, the game connects.
#define BVR_DLSS_PIPE_FMT L"\\\\.\\pipe\\bvr-dlss.%lu"

enum Slot : uint32_t { Color = 0, Output, Depth, Motion, Bias, SlotCount };
enum Tag : uint8_t { TagBuild = 'B', TagFrame = 'F', TagQuit = 'Q' };

#pragma pack(push, 1)
struct Hello {            // game -> helper, once
    uint32_t magic, version, pid;
    uint32_t luidLow; int32_t luidHigh;   // the game's D3D11 adapter; the helper must match it
};
struct HelloAck {         // helper -> game
    uint32_t magic, version;
    int32_t  ok;             // 1 = D3D12 device on that adapter and NGX both up
    uint32_t ngxResult;      // NVSDK_NGX_Result of init / the capability query
    int32_t  dlssAvailable;  // NVSDK_NGX_Parameter_SuperSampling_Available
    uint32_t driverMin[2];   // minimum driver NGX reported (major, minor), 0 when none needed
    char     adapter[128];   // the helper's adapter description, UTF-8
};
struct Build {            // game -> helper, per eye, on every size/format change
    uint32_t eye;            // 0 left, 1 right
    uint32_t width, height;           // render (input) size
    uint32_t outWidth, outHeight;     // output size; equal = DLAA
    uint32_t colorFormat, outputFormat;   // DXGI_FORMAT
    int32_t  depthInverted;  // 1: larger depth is nearer
    int32_t  autoExposure;   // 1: NGX computes exposure (LDR input, no engine exposure value)
    int32_t  preset;         // NVSDK_NGX_DLSS_Hint_Render_Preset_*, 0 = the helper's default (K)
    uint64_t tex[SlotCount]; // shared NT handles, already duplicated INTO the helper
    uint64_t fenceIn, fenceOut;
};
struct BuildAck {         // helper -> game
    uint32_t eye;
    int32_t  ok;
    uint32_t ngxResult;
    char     detail[128];    // why a build failed, UTF-8
};
struct Frame {            // game -> helper, per eye image
    uint32_t eye;
    uint64_t value;          // fence value of this image (in and out)
    uint32_t reset;          // 1: this eye's history does not belong to this image
    float    jitterX, jitterY;    // render pixels; 0 while the projection is not jittered
    float    mvScaleX, mvScaleY;  // the motion texture is in UV: scale = render size
    float    sharpness;      // 0 = none (deprecated by NGX 3.x+, kept for older runtimes)
    uint32_t useBias;        // 1: the Bias slot holds this image's mask (R8, 1 = take the current colour)
};
struct FrameAck {         // helper -> game, after Signal(out, value)
    uint32_t eye;
    uint64_t value;
    int32_t  ok;             // 0: evaluate refused; out was still signalled, the output is stale
    uint32_t ngxResult;
    float    gpuMs;          // the helper's evaluate, from its own timestamps (-1 = not sampled)
};
#pragma pack(pop)

static_assert(sizeof(Hello) == 20, "Hello layout");
static_assert(sizeof(HelloAck) == 156, "HelloAck layout");
static_assert(sizeof(Build) == 96, "Build layout");
static_assert(sizeof(BuildAck) == 140, "BuildAck layout");
static_assert(sizeof(Frame) == 40, "Frame layout");
static_assert(sizeof(FrameAck) == 24, "FrameAck layout");

} // namespace bvr::dlss_ipc
