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

namespace bvr::cube_snapshot {

constexpr uint32_t kFaceSize = 512;

// Splat one eye image. `q` is the eye's orientation (x y z w, eye frame to
// world), the four tangents the eye's fov (left and down negative). `fmt` is the
// cube's format, fixed at the first splat: the eye swapchain's own format, so the
// cube copies straight into a cube swapchain of the same format.
bool splat(ID3D11DeviceContext* ctx, ID3D11Texture2D* eyeImage, const float q[4], float tanL,
           float tanR, float tanU, float tanD, DXGI_FORMAT fmt);

// The flat screen over the room: write `image` back over itself with its alpha
// keyed off luminance - black transparent, anything lit opaque, premultiplied -
// so a loading board or a menu floats in the room instead of sitting on the
// black frame the game drew it on. The image must be renderable (an eye
// swapchain image). Render thread only.
bool key_black(ID3D11DeviceContext* ctx, ID3D11Texture2D* image);

// Drop the render-target views cached on swapchain images (the images died).
void forget_images();

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
