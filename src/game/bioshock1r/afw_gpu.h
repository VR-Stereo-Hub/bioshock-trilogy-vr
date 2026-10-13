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
// Each source is carried into a half-resolution seed map as the existing depth-tested
// grid mesh. The full-resolution compose refines its UV against source depth with three
// fixed-point steps, retries the near side of an edge, then applies the stale/hidden
// tests to that refined point. World-only near-misses can preserve thin silhouettes.
// Params::refine=false retains the original full-size mesh colour compose as a control.
//
// The hands and the weapon are not world geometry: they move with the camera and the
// controllers, so A's copy of them, carried forward as world, lands a frame behind (the
// ghost while moving with the sticks, headset run 4), while B's copy is where they are.
// With a hands mask per source (temporal_gpu's, from the squeezed viewport range the
// viewmodel is drawn into) the rebuild follows the Dishonored mod's rule (its guide 7.1):
// the world from the held eye, the hands primarily from the fresh eye. Confirmed or
// hidden A hand texels can fill surfaces B cannot reconstruct. The optional foreground
// camera carries them through player locomotion first, without moving world geometry
// or making the hands follow physical head movement. B's valid hand texels win the compose
// outright, a foreground fresh sample never makes A's world stale (the weapon pushed into
// a wall, guide 7.3), and the fill extends world seeds only (guide 7.4).
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
    ID3D11ShaderResourceView* mask = nullptr;    // R8 hands mask at the depth's size (1 = viewmodel), or null
    bool invertedMask = false;                 // native coverage: 0 = foreground, 1 = world
    uint32_t depthW = 0, depthH = 0;
    tgpu::Camera cam;
    // Held foreground after the player's room-to-game transform changed. Only
    // mask texels use this camera; the world keeps cam. Fresh remains native.
    bool foregroundCameraValid = false;
    tgpu::Camera foregroundCam;
};

struct Params {
    uint32_t w = 0, h = 0;          // the rebuilt image
    tgpu::Camera target;            // the held eye now
    int grid = 2;                   // mesh cell, depth texels
    bool refine = true;             // native per-pixel solve; false = the original mesh compose
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
    // The hands from the fresh eye only, by the sources' masks (the header). Off, or with no
    // mask bound, every texel is world geometry (the control).
    bool handsFromFresh = true;
    // The hands' own lens: BS1 draws the foreground with its own FOV (camera.cpp's lens match
    // writes it), so a hand texel at image position x sits at tangent x * tanH * fgTanGain, not
    // x * tanH. Dishonored's guide 9.5 (its AfwForegroundGain, measured 0.911 there): a hand
    // texel is unprojected from its source and projected into the target with the hands'
    // tangents, in both sources. 1 = the hands share the world's lens. Needs the masks.
    float fgTanGain = 1.0f;
    // Opt-in native foreground composition before game post-processing. Both
    // sources are HDR at depth resolution; their old foreground is excluded.
    // Native colour is premultiplied, transmittance is the remaining world factor.
    bool hdrWorld = false;
    ID3D11ShaderResourceView* nativeForeground = nullptr;
    ID3D11ShaderResourceView* foregroundTransmittance = nullptr;
};

struct Stats {
    uint64_t runs = 0;
};

class WarpGpu {
public:
    bool init(ID3D11Device* dev, char* why, size_t cap);
    void shutdown();
    // Writes out() (R8G8B8A8_UNORM, or R11G11B10_FLOAT for hdrWorld, p.w x p.h).
    bool run(ID3D11Device* dev, ID3D11DeviceContext* ctx, const Source& held, const Source& fresh,
             const Params& p, char* why, size_t cap);
    ID3D11Texture2D* out() const { return out_; }
    Stats stats;

private:
    bool ensure(ID3D11Device* dev, uint32_t w, uint32_t h, bool refine, bool hdr, char* why, size_t cap);
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
    // Per source: UV seeds (or legacy colour), signed target depth + hands, and z-buffer.
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
    uint32_t w_ = 0, h_ = 0, seedW_ = 0, seedH_ = 0;
    bool refine_ = false, hdr_ = false;
};

} // namespace bvr::b1r::afw
