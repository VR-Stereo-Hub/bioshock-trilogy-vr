# game_frames.py - the game's bone frames in Blender, shared by reload_pose.py and pose_capture.py.
#
# Blender's glTF importer REORIENTS every bone (it points each bone's Y along the bone), so a pose bone's
# matrix is not the frame the game uses: it is the node's frame times a fixed per-bone turn C. C is
# recovered from the .glb's own rest nodes (C = node_rest^-1 * bone_rest), and the game's frame of a
# posed bone is  armature * pose * C^-1.
#
# Coordinates. tools\bsmesh\bs2gltf.py writes Unreal reflected (x, y, z) -> (x, z, y); glTF's Y-up
# enters Blender as A = +90 deg about X. Together a point or a vector in Unreal axes is (x, -y, z) in
# Blender's - in world space and in any bone's own frame alike - and a transform M in Unreal axes is
# F M F with F = diag(1, -1, 1) in Blender's.
import json
import math
import struct

from mathutils import Matrix, Quaternion, Vector

A = Matrix.Rotation(math.radians(90.0), 4, "X")
F = Matrix.Diagonal((1.0, -1.0, 1.0, 1.0))


def ue_local(v):
    """A vector in Unreal axes as Blender's (and back: the map is its own inverse)."""
    return Vector((v[0], -v[1], v[2]))


def ue_matrix(p, q_xyzw, s=(1.0, 1.0, 1.0)):
    """An Unreal-axes transform (translation, quaternion x y z w, scale) as a Blender-axes 4x4."""
    x, y, z, w = q_xyzw
    m = Matrix.LocRotScale(Vector(p), Quaternion((w, x, y, z)), Vector(s))
    return F @ m @ F


def glb_doc(path):
    with open(path, "rb") as fh:
        data = fh.read()
    n = struct.unpack("<I", data[12:16])[0]
    return json.loads(data[20:20 + n])


def glb_rest_frames(path):
    """node name -> its rest world frame, in Blender's axes."""
    nodes = glb_doc(path)["nodes"]
    parent = {}
    for i, nd in enumerate(nodes):
        for ch in nd.get("children", []):
            parent[ch] = i
    local = []
    for nd in nodes:
        t = Vector(nd.get("translation", [0, 0, 0]))
        x, y, z, w = nd.get("rotation", [0, 0, 0, 1])
        s = nd.get("scale", [1, 1, 1])
        local.append(Matrix.LocRotScale(t, Quaternion((w, x, y, z)), Vector(s)))
    world = {}

    def wm(i):
        if i not in world:
            world[i] = (wm(parent[i]) @ local[i]) if i in parent else local[i]
        return world[i]

    return {nd.get("name"): A @ wm(i) @ A.inverted() for i, nd in enumerate(nodes)}


def glb_bone_order(path):
    """The skeleton's bones in the engine's index order (bs2gltf writes them as the first nodes)."""
    doc = glb_doc(path)
    skins = doc.get("skins") or []
    if not skins:
        return []
    joints = skins[0]["joints"]
    return [doc["nodes"][j]["name"] for j in sorted(joints)]


def bone_turns(arm, path):
    """name -> C (bone rest = node rest * C), and the largest translation in any C (should be ~0)."""
    rest = glb_rest_frames(path)
    out, worst = {}, 0.0
    for b in arm.data.bones:
        if b.name not in rest:
            continue
        c = rest[b.name].inverted() @ arm.matrix_world @ b.matrix_local
        loc, rot, _ = c.decompose()
        worst = max(worst, loc.length)
        out[b.name] = Matrix.Rotation(rot.angle, 4, rot.axis) if rot.angle > 1e-9 else Matrix.Identity(4)
    return out, worst


def game_frame(arm, turns, name):
    """The bone's frame as the game has it, posed, in Blender world space."""
    pb = arm.pose.bones[name]
    return arm.matrix_world @ pb.matrix @ turns.get(name, Matrix.Identity(4)).inverted()


def set_game_frame(arm, turns, name, world):
    """Pose a bone so that its GAME frame is `world` (Blender axes). Parents first; the caller updates."""
    pb = arm.pose.bones[name]
    pb.matrix = arm.matrix_world.inverted() @ world @ turns.get(name, Matrix.Identity(4))
