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
import bpy, sys, os, json, re, argparse, math, struct
from mathutils import Vector, Matrix, Quaternion

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
ap.add_argument("--no-render", action="store_true", help="measure only (no pictures)")
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

# ---- THE GAME'S BONE FRAMES (2026-10-20) ------------------------------------------------------------
# Blender's glTF importer REORIENTS every bone (it points each bone's Y along the bone), so a pose bone's
# matrix is not the frame the game uses: the node's frame times a per-bone fixed turn C. Every earlier
# render attached the gun to the reoriented frame - the gun at a wrong angle, and the shotgun (whose
# root bone is far from its origin) visibly in the wrong place. C is recovered from the .glb's own rest
# nodes: C = node_rest^-1 * bone_rest, so the game's frame of a posed bone is  armature * pose * C^-1.
# Coordinates: glTF Y-up enters Blender as A = +90 deg about X; bs2gltf's glTF is Unreal reflected
# (x, y, z) -> (x, z, y). So a vector in a bone's Unreal frame is (x, -y, z) in its Blender frame.
A = Matrix.Rotation(math.radians(90.0), 4, "X")


def glb_rest_frames(path):
    with open(path, "rb") as fh:
        data = fh.read()
    n = struct.unpack("<I", data[12:16])[0]
    doc = json.loads(data[20:20 + n])
    nodes = doc["nodes"]
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


HC, hworst = bone_turns(ha, args.hands)
GC, gworst = bone_turns(ga, args.gun)
R["bone_turn_check"] = {"hands_max_translation": hworst, "gun_max_translation": gworst}
if max(hworst, gworst) > 1e-3:
    R["problems"].append("a bone's import turn is not a pure turn (%.4f, %.4f): the frames may be off" % (hworst, gworst))


def game_frame(arm, turns, name):
    """The bone's frame as the game has it, posed, in Blender world space."""
    pb = arm.pose.bones[name]
    return arm.matrix_world @ pb.matrix @ turns.get(name, Matrix.Identity(4)).inverted()


def ue_local(v):
    """A Blender-frame local vector as the Unreal-frame one (and back: the map is its own inverse)."""
    return Vector((v.x, -v.y, v.z))


# The gun on the hands' grip socket, as the engine attaches it: the weapon's origin ON the socket
# bone's game frame (no offset: every weapon socket aliases bone 43 with none, ENGINE_NOTES Session 91).
if args.socket not in ha.pose.bones:
    R["problems"].append("no hands bone %r" % args.socket)
else:
    c = ga.constraints.new("CHILD_OF")
    c.target, c.subtarget = ha, args.socket
    c.inverse_matrix = HC.get(args.socket, Matrix.Identity(4)).inverted()

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
        o.location = (0, 0, 0)
        o.rotation_euler = (0, 0, 0)
        o.scale = (1, -1 if args.attach_mirror else 1, 1)
        if sock:
            o.parent = sock
            o.matrix_parent_inverse.identity()
        elif args.attach_socket in ga.pose.bones:
            # A socket that imported as a BONE: hung on its game frame, as the gun is on the hands'.
            ac = o.constraints.new("CHILD_OF")
            ac.target, ac.subtarget = ga, args.attach_socket
            ac.inverse_matrix = GC.get(args.attach_socket, Matrix.Identity(4)).inverted()
        o.color = (0.3, 0.6, 0.95, 1.0)  # the attached ammo in blue
        gm.append(o)
    if not sock and args.attach_socket not in ga.pose.bones:
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


# THE CHECK AGAINST THE GAME: the off hand's palm in the grip socket's frame, Unreal axes, rig units, as
# `vrbones reloadrec` recorded it in the running game (the paths in src/game/bioshock1r/reload_rig.h; our
# own measurements). A render whose palm lands within a unit or two of these shows the game's pose.
RECORDED = {
    "TommyGun__ReloadTommyGun": [(0.267, (19.3, 3.3, -11.8)), (0.567, (15.9, 4.9, -66.2)), (0.933, (2.8, -1.4, -55.3)),
                                 (1.113, (16.7, 15.1, -11.4)), (1.222, (19.6, 7.5, -8.7)), (2.058, (8.9, 12.6, 9.5)),
                                 (2.244, (-4.6, 1.5, 13.0))],
    "Pistol__FastReloadPistol": [(0.189, (25.7, -4.6, 24.3)), (0.377, (30.3, 1.4, 5.3)), (0.466, (36.9, -4.2, 5.6)),
                                 (0.657, (25.6, -51.4, 42.2)), (0.756, (7.1, -20.5, 47.8)), (0.934, (24.1, -4.0, 21.4))],
    "Shotgun__ReloadShotgun_LOOP": [(0.034, (16.0, 6.8, -23.2)), (0.122, (27.3, 5.2, 0.7)), (0.267, (29.8, 6.9, 4.1))],
    "Shotgun__ReloadShotgun_End": [(0.409, (52.7, 18.8, 16.9)), (0.487, (38.4, 12.9, 10.9)), (0.676, (51.1, 17.7, 16.4))],
}
PALM_UE = Vector((9.21, 0.12, 0.31))  # the left palm in its wrist's frame (the game's palette anchor, Unreal axes)


def palm_in_grip(t):
    scn.frame_set(int(math.floor(t * fps)), subframe=(t * fps) % 1.0)
    bpy.context.view_layer.update()
    grip = game_frame(ha, HC, args.socket)
    wrist = game_frame(ha, HC, args.palm)
    return ue_local(grip.inverted() @ (wrist @ ue_local(PALM_UE)))


rec = RECORDED.get(hact.name if hact else "", [])
if rec and args.socket in ha.pose.bones and args.palm in ha.pose.bones:
    errs = []
    for t, want in rec:
        got = palm_in_grip(t)
        err = (got - Vector(want)).length
        errs.append(err)
        R.setdefault("check", []).append({"t": t, "palm_in_grip": list(got), "recorded": list(want), "error": err})
        print("BVR_POSE check t=%.3f palm in grip %6.1f %6.1f %6.1f  recorded %6.1f %6.1f %6.1f  error %.1f"
              % (t, got.x, got.y, got.z, *want, err))
    R["check_worst"] = max(errs)
    print("BVR_POSE check: worst %.1f rig units over %d recorded times" % (max(errs), len(errs)))

R["poses"] = []
for t in [float(x) for x in re.split(r"[ ,]+", args.times.strip()) if x]:  # PowerShell turns a,b into "a b"
    scn.frame_set(int(math.floor(t * fps)), subframe=(t * fps) % 1.0)
    bpy.context.view_layer.update()
    pose = {"t": t}
    focus = Vector()
    if args.palm in ha.pose.bones:
        wrist = game_frame(ha, HC, args.palm)
        pm = wrist @ Matrix.Translation(ue_local(PALM_UE))  # the palm, the wrist's turn
        focus = pm.to_translation()
        if args.socket in ha.pose.bones:
            pose["palm_in_grip_ue"] = list(ue_local(game_frame(ha, HC, args.socket).inverted() @ pm.to_translation()))
        if args.part and args.part in ga.pose.bones:
            qm = game_frame(ga, GC, args.part)
            rel = pm.inverted() @ qm
            # Unreal axes: the location's y flips, and so does the turn's (a reflection conjugates it).
            q = rel.to_quaternion()
            pose["part_in_palm_ue"] = {"location": list(ue_local(rel.to_translation())),
                                       "quaternion_wxyz": [q.w, -q.x, q.y, -q.z]}
            pose["part_from_palm"] = (qm.to_translation() - pm.to_translation()).length
            root = game_frame(ga, GC, ga.pose.bones[0].name)
            pose["part_in_gun_ue"] = list(ue_local((root.inverted() @ qm).to_translation()))
            # The part in the gun's MODEL space (the weapon actor's own frame, on the grip socket): how the
            # gun's own clip moves it - for a root bone, the whole gun against the hand.
            mm = ga.matrix_world.inverted() @ qm
            pose["part_model_ue"] = {"location": list(ue_local(mm.to_translation())),
                                     "turn_deg": math.degrees(mm.to_quaternion().angle)}
            print("BVR_POSE t=%.3f %s in the gun's model space: %.1f %.1f %.1f, turned %.1f deg"
                  % (t, args.part, *pose["part_model_ue"]["location"], pose["part_model_ue"]["turn_deg"]))
            focus = (pm.to_translation() + qm.to_translation()) / 2
    # The gun hand's own motion in the clip: its grip frame against the first time asked for (a recoil's
    # kick and settle, the shotgun's swing up to pump) - degrees and rig units.
    if args.socket in ha.pose.bones:
        g = game_frame(ha, HC, args.socket)
        if "grip0" not in R:
            R["grip0"] = [list(r) for r in g]
        g0 = Matrix(R["grip0"])
        d = g0.inverted() @ g
        pose["grip_turn_deg"] = math.degrees(d.to_quaternion().angle)
        pose["grip_move"] = d.to_translation().length
        print("BVR_POSE t=%.3f gun hand vs t0: turned %.1f deg, moved %.1f" % (t, pose["grip_turn_deg"], pose["grip_move"]))
    if not args.no_render:
        pose["renders"] = render("t%05.3f" % t, focus)
    R["poses"].append(pose)

out = place(args.out)
os.makedirs(os.path.dirname(out), exist_ok=True)
with open(out, "w") as fh:
    json.dump(R, fh, indent=1)
print("BVR_POSE %d pose(s), problems=%d -> %s" % (len(R["poses"]), len(R["problems"]), out))
for p in R["problems"]:
    print("BVR_POSE problem: %s" % p)
