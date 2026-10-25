# hold_reference.py - how the game's own clip holds a round part: the hand against the part's cylinder, for a
# designed hold to copy onto another part (lever_grip_mock.py --ref: the crossbow lever's bar held as the
# shotgun's left hand holds its pump).
#
#   .\tools\blender-run.ps1 tools\blender\hold_reference.py -- --gun <ws>\exports\bs1\WP_ShotgunMesh.glb
#       --hands-action Shotgun__FidgetShotgun --gun-action Default__Fire --time 0 --part SG_Pump [--hand L]
#       --out shotgun-pump-hold.json [--render shotgun-pump]
#
# Writes, in the wrist bone's own frame (Blender axes, the pose bone's matrix): the part's axis, the nearest point
# of its axis to the palm, its radius; and every finger bone's local channels (location, rotation, scale against
# its rest - the same rig, so they copy straight onto another hold). Never launches a game.
import bpy, sys, os, re, json, math, argparse

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mathutils import Matrix, Vector  # noqa: E402
from game_frames import bone_turns  # noqa: E402

argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
ap = argparse.ArgumentParser(prog="hold_reference.py")
ap.add_argument("--hands", default="")
ap.add_argument("--gun", default="", help="none: the fingers only (a fist from a clip)")
ap.add_argument("--hands-action", required=True)
ap.add_argument("--gun-action", default="")
ap.add_argument("--time", type=float, default=0.0)
ap.add_argument("--part", default="")
ap.add_argument("--hand", default="L")
ap.add_argument("--socket", default="R_grip")
ap.add_argument("--out", required=True)
ap.add_argument("--render", default="")
args = ap.parse_args(argv)
ws = os.environ.get("BVR_MODEL_WS", os.getcwd())
place = lambda p: p if os.path.isabs(p) else os.path.join(ws, "verification", p)
hands = args.hands or os.path.join(ws, "exports", "bs1", "NEWPlayerHands-all.glb")

for o in list(bpy.data.objects):
    bpy.data.objects.remove(o, do_unlink=True)
for a in list(bpy.data.actions):
    bpy.data.actions.remove(a)


def load(path):
    before, acts0 = set(bpy.data.objects), set(bpy.data.actions)
    bpy.ops.import_scene.gltf(filepath=os.path.abspath(path))
    new = [o for o in bpy.data.objects if o not in before]
    arm = next(o for o in new if o.type == "ARMATURE")
    shapes = {pb.custom_shape for pb in arm.pose.bones if pb.custom_shape}
    for s in shapes:
        s.hide_render = True
    return arm, [o for o in new if o.type == "MESH" and o not in shapes], [a for a in bpy.data.actions if a not in acts0]


ha, hm, hacts = load(hands)
pick = lambda acts, rx: next((a for a in acts if re.search(rx, a.name, re.I)), None) if rx else None
hact = pick(hacts, args.hands_action)
ha.animation_data_create()
ha.animation_data.action = hact
HC, _ = bone_turns(ha, hands)
ga, gm = None, []
if args.gun:
    ga, gm, gacts = load(args.gun)
    ga.animation_data_create()
    ga.animation_data.action = pick(gacts, args.gun_action)
    c = ga.constraints.new("CHILD_OF")
    c.target, c.subtarget = ha, args.socket
    c.inverse_matrix = HC.get(args.socket, Matrix.Identity(4)).inverted()
scn = bpy.context.scene
scn.render.fps = 30
# --time < 0: the clip's last frame (a settled grip).
fr = int(round(args.time * 30)) if args.time >= 0 else int(hact.frame_range[1])
scn.frame_set(fr)
bpy.context.view_layer.update()

side = args.hand.upper()
wrist = "Bip01_%s_Hand" % side
W = ha.matrix_world @ ha.pose.bones[wrist].matrix
Wi = W.inverted()
out = {"source": {"gun": os.path.basename(args.gun), "hands_action": args.hands_action, "time": args.time,
                  "frame": fr, "part": args.part}, "fingers": {}}
ax = near = palm = None
if args.gun and args.part:
    gmesh = max(gm, key=lambda o: len(o.data.vertices))
    gidx = {g.index: g.name for g in gmesh.vertex_groups}
    deps = bpy.context.evaluated_depsgraph_get()
    ev = gmesh.evaluated_get(deps)
    me = ev.to_mesh()
    gp = [ev.matrix_world @ v.co for v in me.vertices]
    ev.to_mesh_clear()
    pts = [gp[v.index] for v in gmesh.data.vertices
           if (b := max(v.groups, key=lambda g: g.weight, default=None)) and gidx.get(b.group) == args.part and b.weight >= 0.5]
    rw = (ga.matrix_world @ ga.pose.bones[args.part].matrix).to_3x3()
    c0 = sum(pts, Vector()) / len(pts)
    best = None
    for a in range(3):
        axa = rw.col[a].normalized()
        ds = [((p - c0) - axa * (p - c0).dot(axa)).length for p in pts]
        mean = sum(ds) / len(ds)
        sd = math.sqrt(sum((d - mean) ** 2 for d in ds) / len(ds)) / max(mean, 1e-6)
        if best is None or sd < best[0]:
            best = (sd, axa, ds)
    _, ax, ds = best
    # The palm centre (near the wrist-to-middle-knuckle midpoint).
    mid = ha.matrix_world @ ha.pose.bones["kBone_%s_Middle1" % side].head
    palm = (W.to_translation() + mid) / 2
    near = c0 + ax * (palm - c0).dot(ax)
    out["radius"] = sorted(ds)[len(ds) // 2]
    out["axis_in_wrist"] = list((Wi.to_3x3() @ ax).normalized())
    out["near_in_wrist"] = list(Wi @ near)
    out["palm_to_axis"] = (palm - near).length
    print("BVR_HOLDREF %s: %s radius %.2f, the palm %.2f from its axis" % (args.hands_action, args.part, out["radius"],
                                                                          out["palm_to_axis"]))
for f in ("Thumb", "Index", "Middle", "Ring", "Pinky"):
    for k in (1, 2, 3):
        n = "kBone_%s_%s%d" % (side, f, k)
        pb = ha.pose.bones[n]
        pb.rotation_mode = "QUATERNION"
        out["fingers"][n] = {"loc": list(pb.location), "rot": list(pb.rotation_quaternion), "scale": list(pb.scale)}
with open(place(args.out), "w") as fh:
    json.dump(out, fh, indent=1)
print("BVR_HOLDREF %s frame %d -> %s" % (args.hands_action, fr, place(args.out)))
if args.render and ax is not None:
    scn.render.engine = "BLENDER_WORKBENCH"
    scn.render.resolution_x, scn.render.resolution_y = 800, 600
    scn.display.shading.color_type = "OBJECT"
    for m in gm:
        m.color = (0.85, 0.55, 0.2, 1.0)
    cam = bpy.data.objects.new("RefCam", bpy.data.cameras.new("RefCam"))
    scn.collection.objects.link(cam)
    scn.camera = cam
    for name, off in (("along", ax * 30), ("palm", (palm - near).normalized() * 30 + Vector((0, 0, 8)))):
        cam.location = near + off
        cam.rotation_euler = (near - cam.location).to_track_quat("-Z", "Z").to_euler()
        scn.render.filepath = place("%s-%s.png" % (args.render, name))
        bpy.ops.render.render(write_still=True)
