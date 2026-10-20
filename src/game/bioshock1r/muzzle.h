#pragma once
// The drawn weapon's muzzle (2026-10-14). Every BS1 player weapon starts its muzzle flash and its
// tracer at a mesh SOCKET named "muzzle" (the weapon classes' OnFiredEffects AttachmentBone and
// TracerStartBone). The engine's own AActor::GetBoneCoords(name, bGetSocketCoords = true) turns
// that socket into world coordinates on the weapon as it is posed - and the held weapon rides the
// hand bone (bones.cpp), so this is the barrel the player sees. ENGINE_NOTES "The weapon's muzzle".

#include <cstdint>

namespace bvr::b1r::muzzle {

// The resolved AActor::GetBoneCoords (patterns::Symbols), or null: locate() then always fails.
void init(void* getBoneCoords);

// Game thread. The muzzle socket of `weapon` in world UU; the socket's X, Y and Z axes in world
// space; and the axes of the bone the socket hangs on (the gun's body - the mesh's own frame,
// without the socket's authored rotation). Each set normalised, or all zero when degenerate.
// False when unresolved, when the weapon's mesh has no "muzzle" socket, or when the engine left
// the result unwritten or non-finite.
bool locate(void* weapon, float origin[3], float socketAxes[3][3], float boneAxes[3][3]);

// Game thread. A BONE of `actor`'s mesh (by GNames index) in world space - origin, then its X, Y
// and Z axes normalised - as the engine places it now (2026-10-20, the manual reload's parts).
bool bone_coords(void* actor, int32_t nameIndex, float origin[3], float axes[3][3]);

// One status line: resolved, calls, hits and each failure kind, the last weapon's socket.
void log_status();

} // namespace bvr::b1r::muzzle
