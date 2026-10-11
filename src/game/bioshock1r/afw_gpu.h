#pragma once
// The alternate frame warp's held-eye rebuild (AFW) for BioShock 1. No mod dependencies:
// tools/gpu-tests/afw-gpu-tests.cpp runs it against a ray-traced scene.
//
// Under AlternateEye stereo the game renders ONE eye per present. Left alone, the other
// (held) eye shows its previous image, which the compositor can only rotate - the eyes'
// translation, the player's walking and stick turning, and anything that moved (above
// all the hands and the weapon) come out wrong in it every other frame: the judder AER
// is known for. This pass rebuilds the held eye at the fresh eye's instant instead, the
// Dishonored VR mod's method (core/gfx/afw_warp.h there; PureDark's AFW for UEVR and
// Oculus' stereo shading reprojection are its prior art), from two sources, each with
// its own depth:
//   A  the held eye's own last image: the world from this eye's viewpoint and with its
//      own shading - the primary source;
//   B  the fresh eye's image: this instant, from the other viewpoint - everything that
//      moved, and what A never saw.
// Each source is carried into the target view as a depth-tested mesh (one vertex per
// `grid` depth texels; a cell spanning a depth discontinuity is dropped instead of
// stretched, leaving a hole the other source fills). A point of A that the fresh eye now
// sees THROUGH (the fresh depth there is clearly farther) no longer exists - it moved -
// and is dropped (the stale test). Then per pixel: A, unless B shows something clearly
// nearer (a thing that moved in); B where A has nothing; a background-biased search of
// the neighbourhood where neither has anything.
//
// Cameras are the game's own (UE world, uu), so the player's walking, stick turning and
// the head are all in the reprojection exactly; the target is the fresh camera moved by
// the eye separation. Depth is linear view depth in uu (temporal_gpu's linearZ), 0 = sky.

#include <stddef.h>
#include <stdint.h>

#include "game/bioshock1r/temporal_gpu.h"

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11Buffer;
struct ID3D11Texture2D;
struct ID3D11ShaderResourceView;
struct ID3D11RenderTargetView;
struct ID3D11DepthStencilView;
struct ID3D11VertexShader;
struct ID3D11PixelShader;
struct ID3D11SamplerState;
struct ID3D11RasterizerState;
struct ID3D11BlendState;
struct ID3D11DepthStencilState;

namespace bvr::b1r::afw {

struct Source {
    bool valid = false;
    ID3D11ShaderResourceView* color = nullptr;   // RGBA, any size (sampled by UV)
    ID3D11ShaderResourceView* depth = nullptr;   // R32F linear view depth (uu), 0 = sky
    uint32_t depthW = 0, depthH = 0;
    tgpu::Camera cam;
};

struct Params {
    uint32_t w = 0, h = 0;          // the rebuilt image
    tgpu::Camera target;            // the held eye now
    int grid = 2;                   // mesh cell, depth texels
    float edgeRel = 0.04f;          // a cell whose depths differ by more is a discontinuity
    float edgeAbsUu = 1.5f;
    float staleRel = 0.03f;         // A is stale where B's depth exceeds A's point by more
    float staleAbsUu = 2.0f;
    float nearerRel = 0.03f;        // B wins where it is nearer than A by more than this
    float skyUu = 100000.0f;        // where depth 0 (sky) is placed
    bool staleTest = true;
    bool useHeld = true;            // A as a source (off = B only, the control)
    bool useFresh = true;           // B as a source (off = A only, the control)
    bool tint = false;              // colour each pixel by its source (diagnostic)
    // The mesh is drawn in bands of at most this many vertices (a 2750 x 2850 eye at grid 2
    // is 11.8M): no single enormous draw reaches the driver.
    uint32_t maxDrawVerts = 1048572;   // a multiple of 6
    // Hand depth gain (Dishonored's AfwForegroundGain): source depth nearer than fgBandUu is
    // read as depth / fgGain. Where the native hands' stereo disparity is g times what their
    // depth predicts (temporal's hand-depth probe measures g), their true depth is depth / g.
    // 1 = off.
    float fgGain = 1.0f;
    float fgBandUu = 80.0f;
};

struct Stats {
    uint64_t runs = 0;
};

class WarpGpu {
public:
    bool init(ID3D11Device* dev, char* why, size_t cap);
    void shutdown();
    // Writes out() (R8G8B8A8_UNORM, p.w x p.h).
    bool run(ID3D11Device* dev, ID3D11DeviceContext* ctx, const Source& held, const Source& fresh,
             const Params& p, char* why, size_t cap);
    ID3D11Texture2D* out() const { return out_; }
    Stats stats;

private:
    bool ensure(ID3D11Device* dev, uint32_t w, uint32_t h, char* why, size_t cap);
    void release_targets();
    bool mesh(ID3D11DeviceContext* ctx, const Source& src, const Source& other, const Params& p, int which,
              bool staleTest);
    bool ready_ = false;
    ID3D11VertexShader* vsMesh_ = nullptr;
    ID3D11PixelShader* psMesh_ = nullptr;
    ID3D11VertexShader* vsFull_ = nullptr;
    ID3D11PixelShader* psCompose_ = nullptr;
    ID3D11Buffer* cb_ = nullptr;
    ID3D11SamplerState* linear_ = nullptr;
    ID3D11SamplerState* point_ = nullptr;
    ID3D11RasterizerState* raster_ = nullptr;
    ID3D11BlendState* blend_ = nullptr;
    ID3D11DepthStencilState* dsTest_ = nullptr;
    ID3D11DepthStencilState* dsOff_ = nullptr;
    // Per source: colour, linear target depth, and the z-buffer.
    ID3D11Texture2D* col_[2] = {};
    ID3D11RenderTargetView* colRtv_[2] = {};
    ID3D11ShaderResourceView* colSrv_[2] = {};
    ID3D11Texture2D* lz_[2] = {};
    ID3D11RenderTargetView* lzRtv_[2] = {};
    ID3D11ShaderResourceView* lzSrv_[2] = {};
    ID3D11Texture2D* zb_ = nullptr;
    ID3D11DepthStencilView* zbDsv_ = nullptr;
    ID3D11Texture2D* out_ = nullptr;
    ID3D11RenderTargetView* outRtv_ = nullptr;
    uint32_t w_ = 0, h_ = 0;
};

} // namespace bvr::b1r::afw
