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
void on_draw_indexed(ID3D11DeviceContext* ctx);

// Session 29: the verdict widened from "substitute this RTV / leave it alone"
// to include SKIP, because the cinematic letterbox bars turned out to be a
// gameswf DRAW (character 292 "WidescreenBars" in HUDPC.swf) painted over a
// full-frame tonemap - not unpainted clear behind a shrunken quad, which is
// what session 22 recorded. Nothing can be redirected or stretched to fix a
// draw; the only honest answer is not to issue it.
enum class DrawVerdict {
    PassThrough, // leave the game's binding alone and draw normally
    Redirect,    // draw into `rtv` instead (the HUD panel capture)
    Skip,        // do not issue this draw at all
};
struct DrawDecision {
    DrawVerdict verdict = DrawVerdict::PassThrough;
    ID3D11RenderTargetView* rtv = nullptr; // valid only for Redirect
};
// `ctx` is used to lazily create the RT and to inspect srv0 for the tonemap
// check. For Redirect, bind `rtv` together with capture_dsv() - gameswf masks
// stencil against it.
DrawDecision on_draw(ID3D11DeviceContext* ctx, UINT vertexCount);
// Our depth-stencil for redirected draws (flash masks are stencil-based).
ID3D11DepthStencilView* capture_dsv();
// Swap the bound blend state for its alpha-corrected variant (gameswf states
// accumulate garbage coverage in the alpha channel; the variant fixes only
// the alpha ops). Call after binding the substitution, before the draw.
void fix_blend_alpha(ID3D11DeviceContext* ctx);

// Present boundary (render thread, called at the END of the present detour,
// after every consumer of the RT ran): clears the RT for the next interval
// and rolls the per-interval classifier state. The swapchain feeds the
// letterbox watch (may be null - the watch just idles).
void on_present(ID3D11DeviceContext* ctx, IDXGISwapChain* swapchain);

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

// ---- Session 29: cinematic letterbox bars ----------------------------------
// `vrcine bars hide|show` (default hide). While a cinematic holds, the one
// textureless gameswf draw on the HUD target is the WidescreenBars sprite -
// measured: the stock frame draws it at 29 vertices with srv0 unbound, right
// after the tonemap, and the Nexus "Fullscreen Cutscenes" mod removes exactly
// that draw by zeroing the sprite's PlaceObject2 scale.
void set_bars_hidden(bool on);
bool bars_hidden();
// The bar shape's vertex count (29 measured). Retunable live - a shot whose
// bars tessellate differently would otherwise be a silent miss, and every
// other textureless count is logged once so a wrong value shows up as data.
void set_bar_verts(unsigned n);
unsigned bar_verts();

// Where the non-bar flash layer (subtitles) goes during a cinematic:
// false (default) = the head-locked HUD panel, one image in both eyes;
// true = in-frame, which under SequentialReentry captures each eye from a
// different game frame and can double the text. `vrcine subs panel|frame`.
void set_cine_subs_in_frame(bool on);
bool cine_subs_in_frame();

// True while THIS interval contained a bar draw. This is the primary cinematic
// signal and it is strictly better than the pixel watch: no async staging map,
// no 5-sample hysteresis (so neither edge lags ~6 presents), and - the reason
// it has to exist - it SURVIVES suppression. Keying the drive gates on black
// pixels would turn them off the moment we stopped painting the bars.
bool bar_draw_active();

// THE cinematic gate every consumer should use: the pixel watch OR the bar
// draw. Use this, not letterbox(), anywhere behaviour must hold for the whole
// cutscene - with bars hidden the pixel watch reports nothing, by design.
bool cinematic_hold();
// The pixel watch's verdict, kept as an INDEPENDENT cross-check rather than a
// fallback: one reads draw calls, the other reads back the backbuffer, so
// agreement between them is real evidence. `vrcine status` reports both.
void get_bar_stats(unsigned* skipped, unsigned* intervalsWithBars, unsigned* lastVertexCount);

// Telemetry for `vrhud status` (per second, reset on read... no - lifetime).
void get_counters(unsigned* hudDraws, unsigned* redirects, unsigned* leaks,
                  unsigned* intervalsWithHud);

// ---- Session 22: live rendered-FOV watch -------------------------------------
// Scripted cameras (the bathysphere descent) render their OWN fov - 104 deg
// measured by dump decode - while the FOV option (and therefore the projection
// layer's claim) still reads 130. The claim mismatch is the in-headset
// "fisheye + broken fusion" percept, and rendered-vs-option is the live
// cutscene detector. Once per present interval the first scene draw's VS b0
// is partially copied to a tiny staging buffer and mapped a present LATER
// (DO_NOT_WAIT, zero pipeline stalls); tangents decode from the screen-ray
// block at floats 12..18 (the session-21 layout that decode-framedump.ps1
// verifies offline).
//
// SESSION 28: a frame carries TWO perspective lenses and off 16:9 they DIFFER -
// the world pass is horizontal-anchored (tanH = tan(option/2), tanV follows the
// window aspect) while the foreground/viewmodel pass is vertical-anchored
// (tanV = tan(fgFov/2)*3/4, tanH follows the window). They coincide exactly at
// 16:9. The watch therefore samples SEVERAL distinct cb0 buffers per interval
// and publishes the cluster the majority agree on as the world lens, with the
// minority available as the fg lens. Taking the first decodable draw instead
// reported the viewmodel lens as the world lens (the fg draws come first, on a
// tier that clears the size gate), which latched the mismatch verdict ON during
// normal gameplay and dragged the projection claim onto the viewmodel frustum -
// a 1.84x under-claim at 2750x2850, and the session-27 yaw-warp bug.
//
//   fov_watch      latest WORLD-lens tangents. maxAgeMs is enforced here (the
//                  gate used to be left to callers and half of them skipped it);
//                  pass 0 only to print a value you will label STALE yourself.
//   fov_watch_fg   the other lens, when there is one (false at 16:9)
//   fov_lens_count distinct perspective clusters in the last decoded round
//   fov_mismatch   hysteresis'd WORLD-rendered-vs-option verdict (compares
//                  against bvr::vr::rendered_hfov_deg(), logs transitions - the
//                  flat-testable instrument; the VR runtime keys the cinematic
//                  quad fallback on it)
bool fov_watch(float* tanH, float* tanV, unsigned long long* ageMs,
               unsigned long long maxAgeMs = 500);
bool fov_watch_fg(float* tanH, float* tanV, unsigned long long* ageMs,
                  unsigned long long maxAgeMs = 500);
int fov_lens_count();
bool fov_mismatch();
// Backbuffer dims (letterbox-watch sample). The lens laws are
// aspect-parameterised, so anything printing an expectation must use these
// rather than assume 16:9 - flat there is no XR session to read swap dims from,
// and flat is where the measuring happens.
bool backbuffer_dims(unsigned* w, unsigned* h);

// Session 22 per-kind routing:
//   screen_only   true while intervals are PURE gameswf with the world pass
//                 absent (hack minigame, loading screens, FMV-class screens -
//                 dump-proven 0 DrawIndexed). The VR runtime drops these to
//                 the readable quad screen. Hysteresis'd; transitions logged.
//   postfx_count  lifetime count of post-tonemap draws left IN-FRAME because
//                 they sample a backbuffer-sized texture (alcohol blur etc.).
bool screen_only();
unsigned postfx_count();

// Session 22 round 2: engine-cinematic letterbox (plasmid FMV sequences
// clear the final target black and tonemap the scene into a shrunken middle
// band - no draws to classify, the bars are unpainted clear). True while
// bars are live; outputs their pixel heights. The VR runtime crops the
// submitted subImage to the band (the compositor unsqueezes - bars gone) and
// the camera adapter suspends the live head drive (authored camera plays).
bool letterbox(unsigned* topPx, unsigned* botPx);
// Backbuffer sampling for the watch - MUST run at the HEAD of the present
// detour (pure game frame; sampling after our window composite made the
// detector flap whenever a session was FOCUSED - round-3 headset negative).
void letterbox_sample(ID3D11DeviceContext* ctx, IDXGISwapChain* swapchain);

// Free device objects (device loss / resize; recreated lazily).
void release_resources();

} // namespace bvr::hud
