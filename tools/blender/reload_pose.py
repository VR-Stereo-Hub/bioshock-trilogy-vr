# reload_pose.py - the player's hands and a first-person gun TOGETHER, posed by their reload clips
# at chosen clip times, and rendered: how the game's own hand holds a part (the Tommy Gun's drum,
# the pistol's cylinder) at each moment of the reload, for the manual reload's carry pose and latch.
#
#   .\tools\blender-run.ps1 tools\blender\reload_pose.py -- --hands <NEWPlayerHands.glb> --gun <TommyGunMESH.glb>
#       --hands-action <regex> --gun-action <regex> --times 0.267,0.933,1.113 [--socket R_Grip]
#       [--part TG_AmmoClip] [--palm Bip01_L_Hand] [--render prefix] [--out report.json] [--list]
#
# Both glbs come from tools\bsmesh\bs2gltf.py (bsmesh-export.ps1, or run on ShockGame.U for the
# guns). The gun's root bone is put on the hands' grip socket bone (a Child Of constraint), so the
# two move as the game moves them. Times are CLIP seconds (the glTF actions run at the scene fps).
# For each time: renders from the left and from below-front, and the part bone's position and turn
# in the palm bone's frame (the carry pose a manual reload would hold the part at). --list prints
# the actions and the bones and stops. Outputs stay in the model workspace, never the repo.
import bpy, sys, os, json, re, argparse
from mathutils import Vector

argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
ap = argparse.ArgumentParser(prog="reload_pose.py")
ap.add_argument("--hands", required=True)
ap.add_argument("--gun", required=True)
ap.add_argument("--hands-action", default="")
ap.add_argument("--gun-action", default="")
ap.add_argument("--times", default="0")
ap.add_argument("--socket", default="R_Grip")
ap.add_argument("--part", default="")
ap.add_argument("--palm", default="Bip01_L_Hand")
ap.add_argument("--render", default="reload-pose")
ap.add_argument("--out", default="reload-pose.json")
ap.add_argument("--list", action="store_true")
ap.add_argument("--attach", help="a static mesh (UModel .pskx) hung on a gun socket: the ammo model the gun mesh lacks")
ap.add_argument("--attach-socket", default="SOCKET_AmmoDummySocket")
ap.add_argument("--attach-mirror", action="store_true", help="flip the attached mesh's Y (UModel's PSK handedness)")
args = ap.parse_args(argv)
ws = os.environ.get("BVR_MODEL_WS", os.getcwd())
place = lambda p: p if os.path.isabs(p) else os.path.join(ws, "verification", p)

for o in list(bpy.data.objects):
    bpy.data.objects.remove(o, do_unlink=True)
for a in list(bpy.data.actions):
    bpy.data.actions.remove(a)
R = {"problems": []}


def load(path):
    before = set(bpy.data.objects)
    acts0 = set(bpy.data.actions)
    bpy.ops.import_scene.gltf(filepath=os.path.abspath(path))
    new = [o for o in bpy.data.objects if o not in before]
    arm = next((o for o in new if o.type == "ARMATURE"), None)
    shapes = {pb.custom_shape for pb in arm.pose.bones if pb.custom_shape} if arm else set()
    for s in shapes:
        s.hide_render = True
    meshes = [o for o in new if o.type == "MESH" and o not in shapes]
    return arm, meshes, [a for a in bpy.data.actions if a not in acts0]


ha, hm, hacts = load(args.hands)
ga, gm, gacts = load(args.gun)
if not ha or not ga:
    print("BVR_POSE problem: an armature is missing")
    sys.exit(1)
if args.list:
    print("BVR_POSE hands actions: " + ", ".join(sorted(a.name for a in hacts)))
    print("BVR_POSE gun actions: " + ", ".join(sorted(a.name for a in gacts)))
    print("BVR_POSE hands bones: " + ", ".join(b.name for b in ha.data.bones))
    print("BVR_POSE gun bones: " + ", ".join(b.name for b in ga.data.bones))
    sys.exit(0)


def pick(acts, rx, who):
    if not rx:
        return None
    m = [a for a in acts if re.search(rx, a.name, re.I)]
    if not m:
        R["problems"].append("no %s action matches %r" % (who, rx))
        return None
    return m[0]


hact, gact = pick(hacts, args.hands_action, "hands"), pick(gacts, args.gun_action, "gun")
for arm, act in ((ha, hact), (ga, gact)):
    arm.animation_data_create()
    arm.animation_data.action = act
R["hands_action"], R["gun_action"] = hact and hact.name, gact and gact.name

# The gun on the hands' grip socket: its armature object follows that bone.
if args.socket not in ha.pose.bones:
    R["problems"].append("no hands bone %r" % args.socket)
else:
    c = ga.constraints.new("CHILD_OF")
    c.target, c.subtarget = ha, args.socket
    c.inverse_matrix.identity()

if args.attach:
    sock = bpy.data.objects.get(args.attach_socket)
    before = set(bpy.data.objects)
    bpy.ops.psk.import_file(filepath=os.path.abspath(args.attach)) if hasattr(bpy.ops, "psk") and hasattr(bpy.ops.psk, "import_file") \
        else bpy.ops.import_scene.psk(filepath=os.path.abspath(args.attach))
    for o in [o for o in bpy.data.objects if o not in before]:
        if o.type == "ARMATURE":
            o.hide_render = True
            continue
        if o.parent and o.parent.type == "ARMATURE":
            mw = o.matrix_world.copy()
            o.parent = None
            o.matrix_world = mw
        o.parent = sock
        o.matrix_parent_inverse.identity()
        o.location = (0, 0, 0)
        o.scale = (1, -1 if args.attach_mirror else 1, 1)
        o.color = (0.3, 0.6, 0.95, 1.0)  # the attached ammo in blue
        gm.append(o)
    if not sock:
        R["problems"].append("no socket %r to attach to" % args.attach_socket)

scn = bpy.context.scene
fps = scn.render.fps / scn.render.fps_base
scn.render.engine = "BLENDER_WORKBENCH"
scn.render.resolution_x, scn.render.resolution_y = 900, 700
scn.display.shading.light = "STUDIO"
scn.display.shading.color_type = "OBJECT"
for m in gm:
    m.color = (0.85, 0.55, 0.2, 1.0)  # the gun in orange, the hands grey: the hold reads at a glance


def bounds():
    deps = bpy.context.evaluated_depsgraph_get()
    pts = []
    for m in hm + gm:
        ev = m.evaluated_get(deps)
        me = ev.to_mesh()
        pts += [ev.matrix_world @ v.co for v in me.vertices]
        ev.to_mesh_clear()
    lo = Vector((min(p.x for p in pts), min(p.y for p in pts), min(p.z for p in pts)))
    hi = Vector((max(p.x for p in pts), max(p.y for p in pts), max(p.z for p in pts)))
    return lo, hi


def render(tag, focus):
    lo, hi = bounds()
    size = max(hi - lo) * 0.55
    out = []
    for name, off in (("left", Vector((0.2, size * 1.6, size * 0.3))), ("under", Vector((size * 1.2, size * 0.4, -size * 1.0)))):
        cam = bpy.data.objects.get("PoseCam") or bpy.data.objects.new("PoseCam", bpy.data.cameras.new("PoseCam"))
        if cam.name not in scn.collection.objects:
            scn.collection.objects.link(cam)
        cam.location = focus + off
        cam.rotation_euler = (focus - cam.location).to_track_quat("-Z", "Y").to_euler()
        scn.camera = cam
        scn.render.filepath = place("%s-%s-%s.png" % (args.render, tag, name))
        bpy.ops.render.render(write_still=True)
        out.append(scn.render.filepath)
    return out


R["poses"] = []
for t in [float(x) for x in re.split(r"[ ,]+", args.times.strip()) if x]:  # PowerShell turns a,b into "a b"
    scn.frame_set(int(round(t * fps)), subframe=(t * fps) % 1.0)
    bpy.context.view_layer.update()
    pose = {"t": t}
    palm = ha.pose.bones.get(args.palm)
    part = ga.pose.bones.get(args.part) if args.part else None
    focus = (ha.matrix_world @ palm.head) if palm else Vector()
    if palm and part:
        pm = ha.matrix_world @ palm.matrix
        qm = ga.matrix_world @ part.matrix
        rel = pm.inverted() @ qm
        pose["part_in_palm"] = {"location": list(rel.to_translation()), "quaternion": list(rel.to_quaternion())}
        pose["part_from_palm_cm"] = (qm.to_translation() - pm.to_translation()).length
        # And in the gun's own frame (its root bone): how far the part travels along the gun.
        root = ga.pose.bones[0]
        rm = ga.matrix_world @ root.matrix
        pose["part_in_gun"] = list((rm.inverted() @ qm).to_translation())
        pose["palm_in_gun"] = list((rm.inverted() @ pm).to_translation())
        focus = (pm.to_translation() + qm.to_translation()) / 2
    pose["renders"] = render("t%05.3f" % t, focus)
    R["poses"].append(pose)

out = place(args.out)
os.makedirs(os.path.dirname(out), exist_ok=True)
with open(out, "w") as fh:
    json.dump(R, fh, indent=1)
print("BVR_POSE %d pose(s), problems=%d -> %s" % (len(R["poses"]), len(R["problems"]), out))
for p in R["problems"]:
    print("BVR_POSE problem: %s" % p)
