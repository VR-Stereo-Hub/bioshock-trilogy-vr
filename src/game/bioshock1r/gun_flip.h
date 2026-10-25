#pragma once
// 2026-10-25: the held gun drawn REFLECTED (gun_geometry.h trueMirror; the flamethrower in the left hand).
//
// A reflection turns every triangle's winding round, so the gun's draws would show their inside faces
// (back-face culling keeps the wrong side). This flips the rasterizer state's FrontCounterClockwise for the gun's
// draws only, and puts the game's state back before the next draw that is not the gun's.
//
// Which draws are the gun's (ENGINE_NOTES 2026-10-25): the viewmodel is drawn into its own thin slice of depth
// (viewport MaxDepth 0.01, the temporal census), its skinned meshes from the 56-byte CPU-skinned vertex ring, and
// the hands are ONE draw of patterns::kHandsIndexCount indices (repeated per light pass). So while a reflected gun
// is held, a draw in that slice, from 56-byte vertices, with any other index count is the gun's (every section,
// every light pass). Ruled out by the headset: "the draws after the gun's gather" (none - the skinning runs as a
// batch before the draws) and "the gun's mesh instance on the draw's stack" (it is not there).
// The static-mesh path (a rigid gun) is flipped inside the static-mesh draw palette.cpp hooks.
// Every 5 s a `[gunflip]` line counts the draws and lists the index counts it flipped and passed.
// The draw callbacks are BS1's temporal draw tap (frame_inspector DrawTap), armed on first use.

struct ID3D11DeviceContext;

namespace bvr::b1r::gun_flip {

// The skin gather, for the log: one call skinned the reflected gun.
void note_gun_gather();
// Around the reflected gun's static-mesh draw.
void static_draw(bool inside);
// The reflected gun this frame (null: none). Arms the draw tap once.
void set_gun(void* actor);

// The draw tap (temporal.cpp): before each game DrawIndexed with its index count, and at a render-target change or
// depth clear.
void on_draw(ID3D11DeviceContext* ctx, unsigned indexCount);
void on_boundary(ID3D11DeviceContext* ctx);

} // namespace bvr::b1r::gun_flip
