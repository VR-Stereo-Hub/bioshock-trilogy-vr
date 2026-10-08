# gltf_check.py - import a glTF (from tools\bsmesh\bs2gltf.py, or any other) into an empty
# scene and prove it arrived intact: armature, skinned mesh, every action, and renders of
# the bind pose and of chosen animation frames, so a broken skeleton or a mis-decoded clip
# is SEEN rather than assumed.
#
#   .\tools\blender-run.ps1 tools\blender\gltf_check.py -- --gltf <file.glb> [--action <regex>]
#       [--frames 0,15,30] [--out check.json] [--render prefix] [--save check.blend]
#
# Prints BVR_GLTF lines; every problem is a `BVR_GLTF problem:` line. Renders are
# Workbench, framed on the mesh's bounds from the front-left and from above.
import bpy, sys, os, json, re, argparse
from mathutils import Vector

argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
ap = argparse.ArgumentParser(prog="gltf_check.py")
ap.add_argument("--gltf", required=True)
ap.add_argument("--action")
ap.add_argument("--frames", default="0")
ap.add_argument("--out", default="gltf-check.json")
ap.add_argument("--render")
ap.add_argument("--save")
args = ap.parse_args(argv)
ws = os.environ.get("BVR_MODEL_WS", os.getcwd())
place = lambda p: p if (not p or os.path.isabs(p)) else os.path.join(ws, "verification", p)

for o in list(bpy.data.objects):
    bpy.data.objects.remove(o, do_unlink=True)
for a in list(bpy.data.actions):
    bpy.data.actions.remove(a)
R = {"gltf": os.path.abspath(args.gltf), "blender": bpy.app.version_string, "problems": []}
bpy.ops.import_scene.gltf(filepath=os.path.abspath(args.gltf))
arms = [o for o in bpy.context.scene.objects if o.type == "ARMATURE"]
# Blender 5.x's importer adds a mesh (an Icosphere) as the bones' display shape; it is
# not part of the file's content.
shapes = {pb.custom_shape for a in arms for pb in a.pose.bones if pb.custom_shape}
meshes = [o for o in bpy.context.scene.objects if o.type == "MESH" and o not in shapes]
for s in shapes:
    s.hide_render = True
R["armatures"] = [{"name": a.name, "bones": len(a.data.bones)} for a in arms]
R["meshes"] = [{"name": m.name, "vertices": len(m.data.vertices), "polygons": len(m.data.polygons),
                "vertex_groups": len(m.vertex_groups),
                "armature_modifier": any(md.type == "ARMATURE" for md in m.modifiers)} for m in meshes]
R["actions"] = sorted(a.name for a in bpy.data.actions)
if not arms:
    R["problems"].append("no armature")
if not meshes:
    R["problems"].append("no mesh")
for m in meshes:
    if not any(md.type == "ARMATURE" for md in m.modifiers):
        R["problems"].append("%s is not skinned to an armature" % m.name)

scn = bpy.context.scene
scn.render.engine = "BLENDER_WORKBENCH"
scn.render.resolution_x, scn.render.resolution_y = 900, 700
scn.display.shading.light = "STUDIO"
scn.display.shading.color_type = "OBJECT"


def frame_view(tag):
    deps = bpy.context.evaluated_depsgraph_get()
    pts = []
    for m in meshes:
        ev = m.evaluated_get(deps)
        me = ev.to_mesh()
        pts += [ev.matrix_world @ v.co for v in me.vertices]
        ev.to_mesh_clear()
    if not pts:
        return
    lo = Vector((min(p.x for p in pts), min(p.y for p in pts), min(p.z for p in pts)))
    hi = Vector((max(p.x for p in pts), max(p.y for p in pts), max(p.z for p in pts)))
    c, size = (lo + hi) / 2, max(hi - lo)
    out = []
    for name, off in (("front", Vector((size * 1.4, -size * 1.4, size * 0.6))), ("top", Vector((0.01, 0.01, size * 2.2)))):
        cam = bpy.data.objects.get("CheckCam") or bpy.data.objects.new("CheckCam", bpy.data.cameras.new("CheckCam"))
        if cam.name not in scn.collection.objects:
            scn.collection.objects.link(cam)
        cam.location = c + off
        cam.rotation_euler = (c - cam.location).to_track_quat("-Z", "Y").to_euler()
        scn.camera = cam
        scn.render.filepath = place("%s-%s-%s.png" % (args.render, tag, name))
        bpy.ops.render.render(write_still=True)
        out.append(scn.render.filepath)
    R.setdefault("renders", []).extend(out)
    R.setdefault("bounds", {})[tag] = {"min": list(lo), "max": list(hi)}


if args.render and arms:
    arm = arms[0]
    if arm.animation_data:
        arm.animation_data.action = None
    for pb in arm.pose.bones:
        pb.location, pb.rotation_quaternion, pb.scale = (0, 0, 0), (1, 0, 0, 0), (1, 1, 1)
    scn.frame_set(0)
    frame_view("bind")
    if args.action:
        rx = re.compile(args.action, re.I)
        acts = [a for a in bpy.data.actions if rx.search(a.name)]
        if not acts:
            R["problems"].append("no action matches %r" % args.action)
        else:
            act = acts[0]
            arm.animation_data_create()
            arm.animation_data.action = act
            R["rendered_action"] = act.name
            R["action_range"] = list(act.frame_range)
            for f in [int(x) for x in args.frames.split(",")]:
                scn.frame_set(f)
                frame_view("%s-f%03d" % (re.sub(r"[^A-Za-z0-9_]+", "_", act.name)[:40], f))
if args.save:
    bpy.ops.wm.save_as_mainfile(filepath=place(args.save))
out = place(args.out)
os.makedirs(os.path.dirname(out), exist_ok=True)
with open(out, "w") as fh:
    json.dump(R, fh, indent=1)
print("BVR_GLTF %s: %d armature(s), %d mesh(es), %d action(s), problems=%d -> %s"
      % (os.path.basename(args.gltf), len(arms), len(meshes), len(R["actions"]), len(R["problems"]), out))
for p in R["problems"]:
    print("BVR_GLTF problem: %s" % p)
