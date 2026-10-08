# inspect_model.py - import a mesh (UModel PSK/PSKX, or glTF/OBJ/FBX) and optionally a
# PSA into an EMPTY scene, and report what is actually in it, as JSON. Run through
# tools\blender-run.ps1:
#
#   .\tools\blender-run.ps1 tools\blender\inspect_model.py -- --mesh <mesh.psk|.pskx|.glb|.obj|.fbx>
#        [--psa <anims.psa>] [--out report.json] [--save inspect.blend] [--render preview.png] [--bones]
#
# It answers "what is this asset" without opening a window: vertex/triangle/material
# counts, the skeleton (names, parents, head positions), skin-weight health (every vertex
# weighted, sums to 1), the bounding box, and per-sequence frame counts for a PSA.
# Every negative is printed (no armature, unweighted vertices, a PSA that does not bind).
# Coordinates are the file's own: a UModel PSK reflects Y against the engine's mesh space.
# The report is about a game asset: it stays in the model workspace, never the repo.
# Ported from the Dishonored VR mod; --mesh (any format) replaces its --psk.
import bpy, sys, os, json, argparse
from mathutils import Vector

argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
ap = argparse.ArgumentParser(prog="inspect_model.py")
ap.add_argument("--mesh", required=True)
ap.add_argument("--psa")
ap.add_argument("--out", default="inspect-report.json")
ap.add_argument("--save")
ap.add_argument("--render")
ap.add_argument("--bones", action="store_true", help="list every bone (default: count + roots)")
args = ap.parse_args(argv)

ws = os.environ.get("BVR_MODEL_WS", os.getcwd())


def place(p):
    if not p:
        return None
    return p if os.path.isabs(p) else os.path.join(ws, "verification", p)


for o in list(bpy.data.objects):
    bpy.data.objects.remove(o, do_unlink=True)

src = os.path.abspath(args.mesh)
ext = os.path.splitext(src)[1].lower()
R = {"mesh": src, "blender": bpy.app.version_string, "problems": []}
try:
    if ext in (".psk", ".pskx"):
        res = bpy.ops.psk.import_file(filepath=src, scale=1.0)
    elif ext in (".glb", ".gltf"):
        res = bpy.ops.import_scene.gltf(filepath=src)
    elif ext == ".obj":
        res = bpy.ops.wm.obj_import(filepath=src)
    elif ext == ".fbx":
        res = bpy.ops.import_scene.fbx(filepath=src)
    else:
        print("BVR_INSPECT *** unsupported extension %s" % ext)
        sys.exit(2)
    R["import"] = sorted(res)
except Exception as e:
    R["problems"].append("IMPORT FAILED: %s" % e)
meshes = [o for o in bpy.context.scene.objects if o.type == "MESH"]
rigs = [o for o in bpy.context.scene.objects if o.type == "ARMATURE"]
if not meshes:
    R["problems"].append("NO MESH imported")
if not rigs:
    R["problems"].append("NO ARMATURE imported (static mesh, or the import failed)")

for m in meshes:
    me = m.data
    tris = sum(len(p.vertices) - 2 for p in me.polygons)
    vg = {g.index: g.name for g in m.vertex_groups}
    unweighted, bad_sum = 0, 0
    for v in me.vertices:
        s = sum(g.weight for g in v.groups)
        if not v.groups:
            unweighted += 1
        elif abs(s - 1.0) > 1e-3:
            bad_sum += 1
    xs = [v.co for v in me.vertices]
    lo = Vector((min(c.x for c in xs), min(c.y for c in xs), min(c.z for c in xs))) if xs else Vector()
    hi = Vector((max(c.x for c in xs), max(c.y for c in xs), max(c.z for c in xs))) if xs else Vector()
    R.setdefault("meshes", []).append({
        "name": m.name, "points": len(me.vertices), "triangles": tris, "polygons": len(me.polygons),
        "materials": [s.material.name if s.material else None for s in m.material_slots],
        "uv_layers": [u.name for u in me.uv_layers], "vertex_groups": len(vg),
        "unweighted_vertices": unweighted, "weight_sum_off_by_1e-3": bad_sum,
        "bbox_min": list(lo), "bbox_max": list(hi), "bbox_size": list(hi - lo),
        "note": "points are welded positions; the engine's draw vertex count is higher where UV/normal seams split them",
    })
    # A PSKX is a static mesh, but the add-on still creates a one-bone armature for it,
    # so "no skin weight" there is the format, not a fault.
    skinned = rigs and ext != ".pskx"
    if skinned and unweighted:
        R["problems"].append("%s: %d vertices have no skin weight" % (m.name, unweighted))
    if skinned and bad_sum:
        R["problems"].append("%s: %d vertices' weights do not sum to 1" % (m.name, bad_sum))

for r in rigs:
    bones = r.data.bones
    info = {"name": r.name, "bones": len(bones),
            "roots": [b.name for b in bones if b.parent is None],
            "note": "bone TAILS are the importer's display length, NOT limb lengths; use head-to-head distances"}
    if args.bones:
        info["list"] = [{"name": b.name, "parent": b.parent.name if b.parent else None,
                         "head": list(b.head_local)} for b in bones]
    R.setdefault("armatures", []).append(info)

if args.psa:
    R["psa"] = os.path.abspath(args.psa)
    if not rigs:
        R["problems"].append("PSA given but there is no armature to bind it to")
    else:
        bpy.context.view_layer.objects.active = rigs[0]
        rigs[0].select_set(True)
        before = set(bpy.data.actions.keys())
        try:
            res = bpy.ops.psa.import_all(filepath=os.path.abspath(args.psa))
            R["psa_import"] = sorted(res)
        except Exception as e:
            R["problems"].append("PSA import FAILED: %s" % e)
        acts = []
        for name in sorted(set(bpy.data.actions.keys()) - before):
            a = bpy.data.actions[name]
            fr = a.frame_range
            acts.append({"name": name, "frames": int(round(fr[1] - fr[0])) + 1, "range": [fr[0], fr[1]]})
        R["actions"] = acts
        if not acts:
            R["problems"].append("PSA imported no actions (bone names did not match the skeleton?)")

if args.render and meshes:
    scn = bpy.context.scene
    scn.render.engine = "BLENDER_WORKBENCH"
    scn.render.resolution_x, scn.render.resolution_y = 960, 720
    m = R["meshes"][0]
    c = Vector([(a + b) / 2 for a, b in zip(m["bbox_min"], m["bbox_max"])])
    size = max(m["bbox_size"]) or 1.0
    cam = bpy.data.objects.new("InspectCam", bpy.data.cameras.new("InspectCam"))
    scn.collection.objects.link(cam)
    cam.location = c + Vector((size * 1.2, -size * 1.6, size * 0.8))
    cam.rotation_euler = (c - cam.location).to_track_quat("-Z", "Y").to_euler()
    scn.camera = cam
    scn.render.filepath = place(args.render)
    bpy.ops.render.render(write_still=True)
    R["render"] = scn.render.filepath

if args.save:
    bpy.ops.wm.save_as_mainfile(filepath=place(args.save))
    R["saved"] = place(args.save)

out = place(args.out)
os.makedirs(os.path.dirname(out), exist_ok=True)
with open(out, "w", encoding="utf-8") as fh:
    json.dump(R, fh, indent=2)
print("BVR_INSPECT %s problems=%d -> %s" % (os.path.basename(src), len(R["problems"]), out))
for p in R["problems"]:
    print("BVR_INSPECT problem: %s" % p)
