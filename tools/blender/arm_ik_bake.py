# arm_ik_bake.py - skin tools\arm-ik-sweep.cpp's bone transforms through the ORIGINAL
# weights of a bsmesh-export glb, measure what the solver does to the mesh, and render
# the key poses. The Dishonored VR mod's arm validation ("Blender bakes those SAME
# matrices through the original skin weights"), on BS1's arms.
#
#   .\tools\blender-run.ps1 tools\blender\arm_ik_bake.py -- --gltf <NEWPlayerHands.glb>
#       --sweep <sweep.json> [--out report.json] [--render prefix] [--all-frames]
#
# Reads the glb with its own parser (positions, joints, weights, inverse binds, indices)
# rather than the importer, so nothing the importer does to bones can hide in the result.
# Prints BVR_IKBAKE lines; a failed acceptance check is a `BVR_IKBAKE problem:` line.
#
# Acceptance (Dishonored's): every forearm-shaft slice keeps >= 85% of its bind-pose
# radius, no triangle grows past 10x its bind area, every vertex finite.
import bpy, sys, os, json, struct, math, argparse
import numpy as np

argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
ap = argparse.ArgumentParser(prog="arm_ik_bake.py")
ap.add_argument("--gltf", required=True)
ap.add_argument("--sweep", required=True)
ap.add_argument("--out", default="arm-ik-bake.json")
ap.add_argument("--render")
ap.add_argument("--all-frames", action="store_true", help="render every frame, not only key poses")
args = ap.parse_args(argv)
ws = os.environ.get("BVR_MODEL_WS", os.getcwd())
place = lambda p: p if (not p or os.path.isabs(p)) else os.path.join(ws, "verification", p)

# ---- the glb, read directly -------------------------------------------------
blob = open(args.gltf, "rb").read()
jlen = struct.unpack_from("<I", blob, 12)[0]
doc = json.loads(blob[20:20 + jlen])
bin_ = blob[20 + jlen + 8:]


def accessor(i):
    a = doc["accessors"][i]
    bv = doc["bufferViews"][a["bufferView"]]
    off = bv.get("byteOffset", 0) + a.get("byteOffset", 0)
    dt = {5121: np.uint8, 5123: np.uint16, 5125: np.uint32, 5126: np.float32}[a["componentType"]]
    n = {"SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4, "MAT4": 16}[a["type"]]
    arr = np.frombuffer(bin_, dtype=dt, count=a["count"] * n, offset=off)
    return arr.reshape(a["count"], n) if n > 1 else arr


prim = doc["meshes"][0]["primitives"][0]
P = accessor(prim["attributes"]["POSITION"]).astype(np.float64)
J = accessor(prim["attributes"]["JOINTS_0"]).astype(np.int64)
W = accessor(prim["attributes"]["WEIGHTS_0"]).astype(np.float64)
W = W / np.maximum(W.sum(1, keepdims=True), 1e-9)
I = accessor(prim["indices"]).astype(np.int64).reshape(-1, 3)
skin = doc["skins"][0]
IBM = accessor(skin["inverseBindMatrices"]).astype(np.float64).reshape(-1, 4, 4).transpose(0, 2, 1)
names = [doc["nodes"][j]["name"] for j in skin["joints"]]
sweep = json.load(open(args.sweep))
if sweep["bones"] != names:
    raise SystemExit("*** the sweep's bones are not this glb's (re-export both from the same mesh)")

# Unreal (x, y, z) -> glTF (x, z, y): a reflection, its own inverse.
C = np.array([[1, 0, 0, 0], [0, 0, 1, 0], [0, 1, 0, 0], [0, 0, 0, 1]], dtype=np.float64)


def ue_matrix(b):
    px, py, pz, qx, qy, qz, qw, sx, sy, sz = b
    r = np.array([[1 - 2 * (qy * qy + qz * qz), 2 * (qx * qy - qz * qw), 2 * (qx * qz + qy * qw)],
                  [2 * (qx * qy + qz * qw), 1 - 2 * (qx * qx + qz * qz), 2 * (qy * qz - qx * qw)],
                  [2 * (qx * qz - qy * qw), 2 * (qy * qz + qx * qw), 1 - 2 * (qx * qx + qy * qy)]])
    m = np.eye(4)
    m[:3, :3] = r * np.array([sx, sy, sz])
    m[:3, 3] = (px, py, pz)
    return m


def skinned(pose):
    M = np.stack([C @ ue_matrix(b) @ C @ IBM[i] for i, b in enumerate(pose)])  # glTF skin
    Ph = np.c_[P, np.ones(len(P))]
    out = np.zeros_like(P)
    for k in range(4):
        out += W[:, k:k + 1] * np.einsum("nij,nj->ni", M[J[:, k]], Ph)[:, :3]
    return out


def tri_area(V):
    return 0.5 * np.linalg.norm(np.cross(V[I[:, 1]] - V[I[:, 0]], V[I[:, 2]] - V[I[:, 0]]), axis=1)


# ---- what to measure --------------------------------------------------------
bind_world = np.linalg.inv(IBM)  # each bone's bind transform, glTF space


def gl(v):
    return np.array([v[0], v[2], v[1]], dtype=np.float64)


arms = []
for side in ("L", "R"):
    fore = names.index("Bip01_%s_Forearm" % side)
    tw1 = names.index("Bip01_%s_ForeTwist1" % side)
    hand_set = {i for i, n in enumerate(names) if n == "Bip01_%s_Hand" % side or ("_%s_" % side in n and "kBone" in n)}
    hand_w = sum(W[:, k] * np.isin(J[:, k], list(hand_set)) for k in range(4))
    shaft_w = sum(W[:, k] * np.isin(J[:, k], [fore, tw1]) for k in range(4))
    shaft = np.where((shaft_w > 0.5) & (hand_w < 0.05))[0]
    arms.append({"side": side, "shaft": shaft, "elbow": bind_world[fore][:3, 3],
                 "wrist": bind_world[names.index("Bip01_%s_Hand" % side)][:3, 3]})

SLICES = 5


def slice_radii(V, idx, e, w):
    axis = w - e
    L = np.linalg.norm(axis)
    axis = axis / L
    d = V[idx] - e
    t = d @ axis / L
    r = np.linalg.norm(d - np.outer(d @ axis, axis), axis=1)
    out = []
    for s in range(SLICES):
        lo, hi = 0.15 + 0.7 * s / SLICES, 0.15 + 0.7 * (s + 1) / SLICES
        m = (t >= lo) & (t < hi)
        out.append(float(r[m].mean()) if m.any() else float("nan"))
    return out


bindR = [slice_radii(P, a["shaft"], a["elbow"], a["wrist"]) for a in arms]
bindA = tri_area(P)
live = bindA > 1e-6

R = {"gltf": os.path.abspath(args.gltf), "sweep": os.path.abspath(args.sweep), "validation": sweep.get("validation"),
     "shaft_vertices": [len(a["shaft"]) for a in arms], "frames": [], "problems": []}
worst_ratio, worst_frame = 9.0, None
max_area, min_area = 0.0, 9.0
keys = []
for fr in sweep["frames"]:
    V = skinned(fr["pose"])
    if not np.isfinite(V).all():
        R["problems"].append("frame %d (%s): non-finite vertices" % (fr["frame"], fr["label"]))
        continue
    ratios = []
    for h, a in enumerate(arms):
        e, w = gl(fr["joints"][h][1]), gl(fr["joints"][h][2])
        rr = slice_radii(V, a["shaft"], e, w)
        # The arm is drawn at the hand's scale on purpose, so the floor is relative to
        # that scale: a 0.8 hand has a 0.8 arm, and that is not a pinch.
        k = fr.get("scale", 1.0) or 1.0
        ratios.append([x / (y * k) if y == y and y > 0 else float("nan") for x, y in zip(rr, bindR[h])])
    fmin = min(x for r in ratios for x in r if x == x)
    A = tri_area(V)
    q = A[live] / bindA[live]
    R["frames"].append({"frame": fr["frame"], "label": fr["label"], "roll": fr["roll"], "swivel": fr["swivel"],
                        "shaftRadiusMin": fmin, "shaftSlices": ratios, "areaMax": float(q.max()),
                        "areaMin": float(q.min())})
    if fmin < worst_ratio:
        worst_ratio, worst_frame = fmin, "%d %s" % (fr["frame"], fr["label"])
    max_area, min_area = max(max_area, float(q.max())), min(min_area, float(q.min()))
    if fr["key"] or args.all_frames:
        keys.append((fr, V))

R["shaftRadiusMin"], R["shaftRadiusWorstFrame"] = worst_ratio, worst_frame
R["areaMax"], R["areaMin"] = max_area, min_area
if worst_ratio < 0.85:
    R["problems"].append("forearm shaft down to %.1f%% of its bind radius (frame %s; floor 85%%)"
                         % (100 * worst_ratio, worst_frame))
if max_area > 10:
    R["problems"].append("a triangle grew to %.1fx its bind area" % max_area)

# ---- renders ----------------------------------------------------------------
if args.render:
    for o in list(bpy.data.objects):
        bpy.data.objects.remove(o, do_unlink=True)
    scn = bpy.context.scene
    scn.render.engine = "BLENDER_WORKBENCH"
    scn.render.resolution_x, scn.render.resolution_y = 800, 600
    scn.display.shading.light = "STUDIO"
    cam = bpy.data.objects.new("BakeCam", bpy.data.cameras.new("BakeCam"))
    scn.collection.objects.link(cam)
    scn.camera = cam
    faces = [tuple(int(x) for x in t) for t in I]
    from mathutils import Vector
    rendered = []
    for fr, V in keys:
        B = np.c_[V[:, 0], -V[:, 2], V[:, 1]]  # glTF -> Blender (Z up)
        me = bpy.data.meshes.new("pose")
        me.from_pydata([tuple(v) for v in B], [], faces)
        ob = bpy.data.objects.new("pose", me)
        scn.collection.objects.link(ob)
        lo, hi = Vector(B.min(0)), Vector(B.max(0))
        c, size = (lo + hi) / 2, max(hi - lo)
        tag = "f%03d-%s" % (fr["frame"], "".join(ch if ch.isalnum() else "_" for ch in fr["label"]))
        for view, off in (("side", Vector((size * 1.5, -size * 1.0, size * 0.5))), ("top", Vector((0.01, 0.01, size * 2.0)))):
            cam.location = c + off
            cam.rotation_euler = (c - cam.location).to_track_quat("-Z", "Y").to_euler()
            scn.render.filepath = place("%s-%s-%s.png" % (args.render, tag, view))
            bpy.ops.render.render(write_still=True)
            rendered.append(scn.render.filepath)
        bpy.data.objects.remove(ob, do_unlink=True)
        bpy.data.meshes.remove(me)
    R["renders"] = rendered

out = place(args.out)
os.makedirs(os.path.dirname(out), exist_ok=True)
json.dump(R, open(out, "w"), indent=1)
print("BVR_IKBAKE %d frames: forearm shaft keeps >= %.1f%% of its radius (worst: frame %s), triangle area %.3f..%.2fx, "
      "%d render(s) -> %s" % (len(R["frames"]), 100 * worst_ratio, worst_frame, min_area, max_area,
                              len(R.get("renders", [])), out))
for p in R["problems"]:
    print("BVR_IKBAKE problem: %s" % p)
