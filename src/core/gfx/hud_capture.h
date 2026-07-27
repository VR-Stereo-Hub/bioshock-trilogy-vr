// HUD capture (session 19, M8 "HUD usability"): redirect the game's gameswf
// HUD draws into an offscreen RT so the eye captures come out HUD-free and
// the RT can be shown as a floating quad in VR (openxr_runtime) and
// composited back onto the flat window after the eye capture.
//
// THE FINGERPRINT (frame dump ground truth, session 19 - supersedes the
// session-6 note, which had the DSV detail inverted for gameplay): the whole
// HUD is drawn per PRESENT INTERVAL (both stereo eye passes) as a contiguous
// run of NON-INDEXED Draw calls on the TONEMAP TARGET (backbuffer-sized
// RGBA8, RTV|SRV) with the scene DSV still bound but depth-testing off,
// through the gameswf batch flush (0x7B8EB5 in every stack). The tonemap
// itself is the interval's FIRST non-indexed draw on that target and samples
// the scene RT; the world renders exclusively via DrawIndexed. So:
//
//   scene RT   := the resource bound as rtv0 for the most DSV-bound
//                 DrawIndexed calls this interval (vote counter)
//   tonemap    := first non-indexed Draw on an LDR-1080 target whose srv0
//                 is the scene-vote leader -> remember that target
//   HUD draw   := any later non-indexed Draw on that same target
//
// The redirect substitutes our RTV (no DSV - gameswf never depth-tests) at
// draw time; the game's next OMSetRenderTargets clears the substitution.
// gameswf's own offscreen ping-pong RTs never match the tonemap target and
// pass through untouched.

#pragma once

#include <d3d11.h>

namespace bvr::hud {

// Called from the frame_inspector detours (render thread, before forwarding).
void on_setrt(UINT numViews, ID3D11RenderTargetView* const* rtvs,
              ID3D11DepthStencilView* dsv);
void on_draw_indexed();
// Returns the RTV to substitute for this draw (bind it together with
// capture_dsv() - gameswf masks stencil against it), or null to leave the
// game's binding alone. `ctx` is used to lazily create the RT and to inspect
// srv0 for the tonemap check.
ID3D11RenderTargetView* on_draw(ID3D11DeviceContext* ctx);
// Our depth-stencil for redirected draws (flash masks are stencil-based).
ID3D11DepthStencilView* capture_dsv();
// Swap the bound blend state for its alpha-corrected variant (gameswf states
// accumulate garbage coverage in the alpha channel; the variant fixes only
// the alpha ops). Call after binding the substitution, before the draw.
void fix_blend_alpha(ID3D11DeviceContext* ctx);

// Present boundary (render thread, called at the END of the present detour,
// after every consumer of the RT ran): clears the RT for the next interval
// and rolls the per-interval classifier state.
void on_present(ID3D11DeviceContext* ctx);

// The captured HUD as a shader resource (null when nothing was redirected
// this interval or the RT does not exist). Serves the PROCESSED copy - rgb
// premultiplied by construction, alpha repaired via blit::process (run at
// most once per interval; `ctx` may be null to only query). Same thread as
// the consumers.
ID3D11ShaderResourceView* srv(ID3D11DeviceContext* ctx);
ID3D11Texture2D* texture(ID3D11DeviceContext* ctx);
bool redirected_this_interval();

// Master enable (the redirect only runs while enabled AND gated).
void set_enabled(bool on);
bool enabled();
// Render-side gate published once per present by the VR runtime: true while
// stereo gameplay frames flow (projectionMode && srSign != 0). The `force`
// override arms the redirect with no XR session - REQUIRED for flat testing
// (projectionReady can never be true without a headset).
void set_gate(bool stereoActive);
void set_force(bool on);
bool force();

// Telemetry for `vrhud status` (per second, reset on read... no - lifetime).
void get_counters(unsigned* hudDraws, unsigned* redirects, unsigned* leaks,
                  unsigned* intervalsWithHud);

// Free device objects (device loss / resize; recreated lazily).
void release_resources();

} // namespace bvr::hud
