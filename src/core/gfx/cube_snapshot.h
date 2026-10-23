// Room snapshot (2026-10-22): a cubemap of the room the player is standing in,
// accumulated on the GPU from the eye images as they look around, so a loading
// screen or a menu can show the room around the flat screen instead of black.
//
// Every ~100 ms while the projection layer is live, one eye image is SPLATTED
// into the cube: for each texel of each face the face direction is rotated into
// the eye's frame, projected onto the eye's tangent plane, and the eye image is
// sampled there; texels outside the eye's fov are left as they were. Looking
// around fills the cube; nothing is ever cleared but on reset. The cube is a
// direction-only picture (no parallax, the player's head at its centre), which
// is exactly what a static backdrop at infinity wants.
//
// Faces follow the D3D cube-map convention (+X -X +Y -Y +Z -Z, the per-face
// axes D3D's TextureCube sampler expects) in the eye poses' own space - OpenXR's
// LOCAL space, -Z forward, +Y up - so the cube submits as an
// XrCompositionLayerCubeKHR with the identity orientation and the compositor's
// TextureCube lookup lands on the texel the splat wrote.
//
// Pipeline objects are created lazily from the context's device (the blit.cpp
// pattern); state is backed up and restored around the draw. Render thread only.

#pragma once

#include <d3d11.h>

#include <cstdint>
#include <vector>

namespace bvr::cube_snapshot {

constexpr uint32_t kFaceSize = 512;

// Splat one eye image. `q` is the eye's orientation (x y z w, eye frame to
// world), the four tangents the eye's fov (left and down negative). `fmt` is the
// cube's format, fixed at the first splat: the eye swapchain's own format, so the
// cube copies straight into a cube swapchain of the same format.
// With `linearZ` (the eye image's linear depth, uu along the view axis, the image's
// size; null = none) and `uuPerM` the room's distances are splatted too (metres, a
// second R32F cube), and the backdrop gets each eye its own parallax (2026-10-23).
bool splat(ID3D11DeviceContext* ctx, ID3D11Texture2D* eyeImage, const float q[4], float tanL,
           float tanR, float tanU, float tanD, DXGI_FORMAT fmt, ID3D11ShaderResourceView* linearZ = nullptr,
           float uuPerM = 0.0f);

// The flat screen over the room: write `image` back over itself with its alpha
// keyed off luminance - black transparent, anything lit opaque, premultiplied -
// so a loading board or a menu floats in the room instead of sitting on the
// black frame the game drew it on. The image must be renderable (an eye
// swapchain image). Render thread only.
bool key_black(ID3D11DeviceContext* ctx, ID3D11Texture2D* image);

// Drop the render-target views cached on swapchain images (the images died).
void forget_images();

// The typed format the swapchains were created with: a runtime may hand out
// TYPELESS textures (Virtual Desktop's does), and every view of one needs it.
void set_view_format(DXGI_FORMAT fmt);

// The room drawn into `dst` (a projection-layer swapchain image) as seen by an eye
// with orientation `q` (x y z w, eye frame to the cube's world frame) and these fov
// tangents - for runtimes without XR_KHR_composition_layer_cube (2026-10-23). With
// `roomDepth` the eye stands `eyeOffsetM` (metres, the cube's frame) off its centre
// and sees the room with its own parallax: stereo.
bool draw_backdrop(ID3D11DeviceContext* ctx, ID3D11Texture2D* dst, ID3D11Texture2D* room, ID3D11Texture2D* roomDepth,
                   const float q[4], float tanL, float tanR, float tanU, float tanD, const float eyeOffsetM[3]);

// The circle the keyed screen is cut to: radius in fractions of the frame's
// height, centred; 0 = the whole frame.
void set_screen_circle(float radiusFrac);
float screen_circle();

// A room kept on disk (2026-10-23, one per level): the live cube's six faces as
// RGBA8 rows (6 * kFaceSize * kFaceSize * 4 bytes, face order +X -X +Y -Y +Z
// -Z), and the same bytes back into a second cube that is shown in place of
// the live one while it is set. Download reads the live cube back through a
// staging copy (a stall, once per save).
bool download(std::vector<uint8_t>& out);
// The distances, the same layout (R32F, 4 bytes a texel); false when the live cube has none.
bool download_depth(std::vector<uint8_t>& out);
// `depth` (or null): the distance faces, the colour's byte count.
bool upload_saved(ID3D11DeviceContext* ctx, const uint8_t* rgba, size_t bytes, DXGI_FORMAT fmt,
                  const uint8_t* depth = nullptr);
void clear_saved();
bool has_saved();
// The cube to show: the saved room if one is loaded, else the live cube once it
// has a splat, else nullptr.
ID3D11Texture2D* shown();
// Its distances (the live cube's when `liveOnly`), or null: the room is mono.
ID3D11Texture2D* shown_depth(bool liveOnly);
bool has_depth();

// The cube: a 6-slice kFaceSize square array with the TEXTURECUBE flag, or
// nullptr before the first splat. Valid means at least one splat since the last
// reset.
ID3D11Texture2D* texture();
bool valid();
uint32_t splats();

// Clear every face and forget the splats (a new room).
void reset();

// Release device objects (device loss / shutdown; recreated lazily).
void release();

} // namespace bvr::cube_snapshot
