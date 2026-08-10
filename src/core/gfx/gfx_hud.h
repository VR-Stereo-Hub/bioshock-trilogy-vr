#pragma once
// s52 (Infinite I9): the Scaleform-GFx HUD capture - a SECOND hud machine,
// deliberately not a modification of hud_capture.cpp (that is BS1's gameswf
// worked example; its fingerprints are Vengeance-specific).
//
// Classifier: the POSITIONAL rule (DR-I7, ENGINE_NOTES). Within one present
// window, the moment a full-screen depth-free DrawIndexed a=6 sampling a
// rendered render-target lands on the BACKBUFFER (the tonemap/eye blit),
// every LATER backbuffer draw is UI-run content. No fingerprint needed - the
// eye image is HUD-free by construction once those draws are redirected.
// Frames with no such blit (Bink movies, loading screens) classify NOTHING,
// which is what keeps this lane cinematics-safe: movie frames stay untouched.
//
// Default OFF in core. The Infinite adapter arms it; BS1/BS2 never call any
// setter here, and the armed-off cost on their draw stream is one relaxed
// load per draw.

#include <cstdint>

struct ID3D11DeviceContext;
struct ID3D11RenderTargetView;
struct ID3D11DepthStencilView;
struct ID3D11Texture2D;
struct IDXGISwapChain;

namespace bvr::gfx_hud {

// What the frame_inspector detour should do with the current draw. At most
// one of redirectRtv / restoreRtv is set; both null = pass through.
struct Decision {
    ID3D11RenderTargetView* redirectRtv = nullptr; // bind this (+ dsv) via ORIGINAL SetRT
    ID3D11DepthStencilView* redirectDsv = nullptr;
    ID3D11RenderTargetView* restoreRtv = nullptr;  // hand the game's binding back
    ID3D11DepthStencilView* restoreDsv = nullptr;
};

void set_armed(bool on);    // classifier + census
void set_redirect(bool on); // classified draws feed the capture RT (the quad)
bool armed();
bool redirect_on();
// Full-screen-effect pass-through (default ON): post-boundary draws with a
// tiny index/vertex count (a GFx flash/vignette quad) stay on the eye image
// instead of the quad. In-headset A/B: bsihud fx on|off.
void set_fx_passthrough(bool on);
bool fx_passthrough();

// Detour taps (render thread). on_draw_* returns the decision; the detour
// owns the actual SetRT call so this module never re-enters the hooks.
void on_setrt(unsigned numViews, ID3D11RenderTargetView* const* rtvs,
              ID3D11DepthStencilView* dsv);
Decision on_draw_indexed(ID3D11DeviceContext* ctx, unsigned indexCount);
Decision on_draw(ID3D11DeviceContext* ctx, unsigned vertexCount);
void on_present(ID3D11DeviceContext* ctx, IDXGISwapChain* swapchain);

// The captured HUD texture (transparent-cleared, UI-run content only), or
// null when stale/empty. The XR HUD-quad consumer prefers this via the
// provider seam in openxr_runtime.
ID3D11Texture2D* texture(ID3D11DeviceContext* ctx);

// Census counters for the adapter's status verb (relaxed reads, any thread).
struct Census {
    uint32_t frames = 0;        // presents seen while armed
    uint32_t boundaries = 0;    // frames where the eye blit was found
    uint32_t hudDraws = 0;      // post-boundary backbuffer draws
    uint32_t redirected = 0;    // of those, redirected to the capture RT
    uint32_t bigPostDraws = 0;  // post-boundary FULL-SCREEN-ish draws (the
                                // effects census: a damage flash would land
                                // here if it is a GFx overlay)
    uint32_t lastFrameHud = 0;  // hud draws in the last completed frame
};
Census census();

} // namespace bvr::gfx_hud
