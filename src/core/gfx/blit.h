// Minimal alpha-blend textured blit (session 19): draws `src` over `dst` as a
// fullscreen triangle with straight-alpha blending. Used to composite the
// captured HUD back onto the flat window AFTER the XR eye capture - the one
// consumer ImGui cannot serve (the overlay draws before the capture, so an
// ImGui composite would put the HUD back into both eyes).
//
// Pipeline objects are created lazily from the context's device on first use
// (D3DCompile, like imgui_impl_dx11); full state backup/restore around the
// draw. Render thread only.

#pragma once

#include <d3d11.h>

namespace bvr::blit {

// Draw src over the full dst viewport (dstW x dstH) with PREMULTIPLIED
// blending (ONE / INV_SRC_ALPHA). The source is expected to carry sane alpha
// (see process below). Returns false if pipeline creation failed (logged once).
bool alpha_premul(ID3D11DeviceContext* ctx, ID3D11RenderTargetView* dst,
                  ID3D11ShaderResourceView* src, UINT dstW, UINT dstH);

// Alpha-repair pass, blending OFF: writes src.rgb unchanged (gameswf output
// over a zero-cleared RT is premultiplied by construction) with alpha =
// max(stored, luminance-derived). gameswf's blend states leave garbage in the
// destination alpha channel, so coverage must be reconstructed before any
// alpha-blended consumer (the window composite, the XR quad) can show it.
bool process(ID3D11DeviceContext* ctx, ID3D11RenderTargetView* dst,
             ID3D11ShaderResourceView* src, UINT dstW, UINT dstH);

// Session 22: stretch a horizontal BAND of src ([topFrac .. topFrac +
// heightFrac] in v) across the FULL destination, blending off. The
// engine-cinematic letterbox unsqueeze: runtimes proved unreliable with
// projection-layer imageRect crops (VDXR showed the bars regardless), so the
// eye capture un-letterboxes the frame itself before submission.
bool stretch_band(ID3D11DeviceContext* ctx, ID3D11RenderTargetView* dst,
                  ID3D11ShaderResourceView* src, UINT dstW, UINT dstH,
                  float topFrac, float heightFrac);

// Release device objects (device loss / shutdown; recreated lazily).
void release();

} // namespace bvr::blit
