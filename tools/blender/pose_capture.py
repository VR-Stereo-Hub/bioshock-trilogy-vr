# pose_capture.py - rebuild captured in-game frames of the hands and the gun in Blender, exactly as the game
# drew them.
#
#   one frame - the pose_NN.txt the mod writes beside an AFW capture (F10 > "Capture 6 presents in 5 s"):
#   .\tools\blender-run.ps1 tools\blender\pose_capture.py -- --pose <dumps\afw-...\pose_00.txt> --render cap
#   a recording - the Shotgun.bvrpose the mod writes while the shotgun is in hand (dumps\poserec-...\):
#   .\tools\blender-run.ps1 tools\blender\pose_capture.py -- --record <dumps\poserec-...\Shotgun.bvrpose>
#       [--step 1] [--from 0] [--to -1] --save shotgun-run.blend --csv shotgun-run.csv
#   options: [--hands <glb>] [--gun <glb>] [--attach <ammo.pskx>] [--out report.json]
#
# What it rebuilds: every hands bone at the engine's evaluated pose with each hand's palette correction
# applied (the controller placement the mod draws), and every gun bone as the skeleton lane wrote it, on
# the gun actor as the engine placed it - all in the hands actor's component space, Unreal axes converted
# to Blender's (game_frames.py). A recording becomes an action on both armatures (one key per recorded
# frame, 30 a second) and a CSV of what each frame did: the hands' state and clips, the gun hand's grip
# frame, the free palm against the gun, every gun bone against its first frame. Not rebuilt: the IK arms
# (the sleeves are the game's), the mirror. Outputs stay in the model workspace.
import bpy, sys, os, json, argparse, math

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mathutils import Matrix, Quaternion, Vector  # noqa: E402
from game_frames import bone_turns, game_frame, glb_bone_order, set_game_frame, ue_local, ue_matrix  # noqa: E402

argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
ap = argparse.ArgumentParser(prog="pose_capture.py")
ap.add_argument("--pose", default="")
ap.add_argument("--record", default="")
ap.add_argument("--step", type=int, default=1)
ap.add_argument("--from", dest="first", type=int, default=0)
ap.add_argument("--to", dest="last", type=int, default=-1)
ap.add_argument("--hands", default="")
ap.add_argument("--gun", default="")
ap.add_argument("--attach", default="")
ap.add_argument("--attach-socket", default="SOCKET_AmmoDummySocket")
ap.add_argument("--render", default="")
ap.add_argument("--save", default="")
ap.add_argument("--csv", default="")
ap.add_argument("--out", default="")
args = ap.parse_args(argv)
ws = os.environ.get("BVR_MODEL_WS", os.getcwd())
exports = os.path.join(ws, "exports", "bs1")
place = lambda p: p if os.path.isabs(p) else os.path.join(ws, "verification", p)
GUNS = {"MachineGun": "TommyGunMESH.glb", "Pistol": "WP_PistolMesh.glb", "Shotgun": "WP_ShotgunMesh.glb"}
AMMO = {"MachineGun": "TG_AmmoModel.pskx", "Pistol": "PI_AmmoType.pskx", "Shotgun": "shotgun_shell.pskx"}
PALM = {0: (9.21, 0.12, 0.31), 1: (9.21, 0.12, -0.31)}  # the palette's palm anchors, Unreal, in the wrist's frame
R = {"problems": []}


# ---- the capture -----------------------------------------------------------------------------------
def new_cap():
    return {"bones": {}, "wbones": {}, "handfix": {}, "clips": {}}


def parse(path):
    frames, cap = [], new_cap()
    with open(path) as fh:
        for line in fh:
            tok = line.split()
            if not tok or tok[0].startswith("#"):
                continue
            k = tok[0]
            try:
                if k == "frame":
                    if cap["bones"]:
                        frames.append(cap)
                    cap = new_cap()
                    cap["frame"] = int(tok[1])
                elif k == "ms":
                    cap["ms"] = int(tok[1])
                elif k == "actor":
                    v = [float(x) for x in tok[1:]]
                    cap["actor"] = (v[0:3], v[3:7], v[7])
                elif k == "state":
                    cap["state"] = tok[1]
                elif k == "clusters":
                    cap["L"] = (int(tok[2]), int(tok[3]))
                    cap["R"] = (int(tok[5]), int(tok[6]))
                    cap["held"] = int(tok[10])
                elif k == "handfix":
                    v = [float(x) for x in tok[3:]]
                    cap["handfix"][int(tok[1])] = (int(tok[2]) == 1, v[0:4], v[4:7], v[7:10], v[10])
                elif k == "b":
                    v = [float(x) for x in tok[2:]]
                    cap["bones"][int(tok[1])] = (v[0:3], v[3:7], v[7:10])
                elif k == "clip":
                    cap["clips"][tok[1]] = (tok[2], float(tok[3]))
                elif k == "weapon":
                    v = [float(x) for x in tok[4:12]]
                    cap["weapon"] = {"class": tok[1], "onsocket": int(tok[3]) == 1, "loc": v[0:3], "q": v[3:7],
                                     "ds": v[7], "size": float(tok[-1])}
                elif k == "w":
                    v = [float(x) for x in tok[3:]]
                    cap["wbones"][int(tok[1])] = (tok[2], v[0:3], v[3:7], v[7:10])
            except (ValueError, IndexError):
                R["problems"].append("an unreadable line (a recording cut off mid-frame?): %r" % line[:60])
    if cap["bones"] and (len(cap["bones"]) >= len(frames[0]["bones"]) if frames else True):
        frames.append(cap)
    return frames


frames = parse(args.record or args.pose)
if not frames:
    raise SystemExit("BVR_POSECAP no frames in %s" % (args.record or args.pose))
last = len(frames) if args.last < 0 else min(args.last + 1, len(frames))
frames = frames[args.first:last:max(1, args.step)]
first = frames[0]
wcls = next((f["weapon"]["class"] for f in frames if f.get("weapon")), "")
hands_glb = args.hands or os.path.join(exports, "NEWPlayerHands-all.glb")
gun_glb = args.gun or (os.path.join(exports, GUNS[wcls]) if wcls in GUNS else "")
print("BVR_POSECAP %d frame(s), gun %s" % (len(frames), wcls or "none"))

# ---- the models ------------------------------------------------------------------------------------
for o in list(bpy.data.objects):
    bpy.data.objects.remove(o, do_unlink=True)


def load(path):
    before = set(bpy.data.objects)
    bpy.ops.import_scene.gltf(filepath=os.path.abspath(path))
    new = [o for o in bpy.data.objects if o not in before]
    arm = next((o for o in new if o.type == "ARMATURE"), None)
    shapes = {pb.custom_shape for pb in arm.pose.bones if pb.custom_shape} if arm else set()
    for s in shapes:
        s.hide_render = True
    if arm:
        arm.animation_data_clear()
    return arm, [o for o in new if o.type == "MESH" and o not in shapes]


def depth_order(arm, names):
    def depth(n):
        b, d = arm.data.bones.get(n), 0
        while b and b.parent:
            b, d = b.parent, d + 1
        return d
    return sorted([n for n in names if n in arm.pose.bones], key=depth)


ha, hm = load(hands_glb)
HC, _ = bone_turns(ha, hands_glb)
order = glb_bone_order(hands_glb)
ga, gm, GC = None, [], {}
if gun_glb:
    ga, gm = load(gun_glb)
    GC, _ = bone_turns(ga, gun_glb)


# ---- one frame -------------------------------------------------------------------------------------
def fixed(cap, i, p, q, s):
    for h, (lo, hi) in ((0, cap["L"]), (1, cap["R"])):
        valid, fq, ft, fp, fs = cap["handfix"].get(h, (False, None, None, None, None))
        if valid and lo <= i <= hi:
            x, y, z, w = fq
            Q = Quaternion((w, x, y, z))
            P = Vector(ft) + Q @ ((Vector(p) - Vector(fp)) * fs)
            x2, y2, z2, w2 = q
            Qi = Q @ Quaternion((w2, x2, y2, z2))
            return list(P), [Qi.x, Qi.y, Qi.z, Qi.w], [c * fs for c in s]
    return p, q, s


def pose_frame(cap):
    hf = {}
    for i, (p, q, s) in cap["bones"].items():
        if i < len(order):
            hf[order[i]] = ue_matrix(*fixed(cap, i, p, q, s))
    for name in depth_order(ha, hf):
        set_game_frame(ha, HC, name, hf[name])
        bpy.context.view_layer.update()
    W = cap.get("weapon")
    if ga and W:
        al, aq, ak = cap["actor"]
        to_comp = ue_matrix(al, aq, (ak,) * 3).inverted() @ ue_matrix(W["loc"], W["q"], (W["ds"],) * 3)
        if not W["onsocket"]:
            # The palette moves the gun with the hand that holds it (its correction, about the pivot).
            valid, fq, ft, fp, fs = cap["handfix"].get(cap.get("held", 1), (False, None, None, None, None))
            if valid:
                to_comp = ue_matrix(ft, fq, (fs,) * 3) @ ue_matrix([-c for c in fp], [0, 0, 0, 1]) @ to_comp
        wf = {name: to_comp @ ue_matrix(p, q, s) for i, (name, p, q, s) in cap["wbones"].items() if name in ga.pose.bones}
        for name in depth_order(ga, wf):
            set_game_frame(ga, GC, name, wf[name])
            bpy.context.view_layer.update()
    return hf


def measure(cap):
    """What this frame did, in Unreal axes of the hands' component space (rig units)."""
    m = {"frame": cap.get("frame", 0), "ms": cap.get("ms", 0), "state": cap.get("state", "")}
    for tag in ("hands", "weapon"):
        c = cap["clips"].get(tag)
        m[tag + "_clip"], m[tag + "_t"] = (c[0], c[1]) if c else ("", -1.0)
    held = cap.get("held", 1)
    free = 1 - held
    grip = game_frame(ha, HC, order[43])
    gl = ue_local(grip.to_translation())
    ge = grip.to_euler("XYZ")
    m["grip_x"], m["grip_y"], m["grip_z"] = gl
    m["grip_rx"], m["grip_ry"], m["grip_rz"] = [math.degrees(a) for a in ge]
    wrist = game_frame(ha, HC, "Bip01_L_Hand" if free == 0 else "Bip01_R_Hand")
    palm = wrist @ ue_local(PALM[free])
    pg = ue_local(grip.inverted() @ palm)
    m["palm_in_grip_x"], m["palm_in_grip_y"], m["palm_in_grip_z"] = pg
    if ga and cap.get("weapon"):
        for name in ga.pose.bones.keys():
            f = game_frame(ga, GC, name)
            m["%s_from_palm" % name] = (f.to_translation() - palm).length
            lp = ue_local(grip.inverted() @ f.to_translation())
            m["%s_in_grip_x" % name], m["%s_in_grip_y" % name], m["%s_in_grip_z" % name] = lp
    return m


def hang_ammo():
    ammo = args.attach or (os.path.join(ws, "bs1", "shockgame-weapons", "ShockGame", "StaticMesh", AMMO[wcls])
                           if wcls in AMMO else "")
    if not (ga and ammo and os.path.exists(ammo) and args.attach_socket in ga.pose.bones):
        return
    before = set(bpy.data.objects)
    bpy.ops.psk.import_file(filepath=os.path.abspath(ammo)) if hasattr(bpy.ops, "psk") and hasattr(bpy.ops.psk, "import_file") \
        else bpy.ops.import_scene.psk(filepath=os.path.abspath(ammo))
    for o in [o for o in bpy.data.objects if o not in before]:
        if o.type == "ARMATURE":
            o.hide_render = True
            continue
        o.parent = None
        o.location, o.rotation_euler, o.scale = (0, 0, 0), (0, 0, 0), (1, 1, 1)
        c = o.constraints.new("CHILD_OF")
        c.target, c.subtarget = ga, args.attach_socket
        c.inverse_matrix = GC.get(args.attach_socket, Matrix.Identity(4)).inverted()
        o.color = (0.3, 0.6, 0.95, 1.0)
        gm.append(o)


# ---- run -------------------------------------------------------------------------------------------
rows = []
scn = bpy.context.scene
scn.render.fps = 30
for k, cap in enumerate(frames):
    pose_frame(cap)
    rows.append(measure(cap))
    if args.record:
        for arm in (ha, ga):
            if not arm:
                continue
            for pb in arm.pose.bones:
                pb.keyframe_insert("location", frame=k)
                pb.keyframe_insert("rotation_quaternion", frame=k)
                pb.keyframe_insert("scale", frame=k)
    if k % 300 == 0:
        print("BVR_POSECAP frame %d/%d: %s %s %.3f / %s %.3f" % (k, len(frames), rows[-1]["state"], rows[-1]["hands_clip"],
                                                               rows[-1]["hands_t"], rows[-1]["weapon_clip"],
                                                               rows[-1]["weapon_t"]))
scn.frame_start, scn.frame_end = 0, max(0, len(frames) - 1)
hang_ammo()
R["frames"] = len(frames)
R["first"] = rows[0]

if args.csv:
    keys = list(rows[0].keys())
    for r in rows[1:]:
        for k2 in r:
            if k2 not in keys:
                keys.append(k2)
    with open(place(args.csv), "w") as fh:
        fh.write(",".join(keys) + "\n")
        for r in rows:
            fh.write(",".join(("%.4g" % r[k2]) if isinstance(r.get(k2), float) else str(r.get(k2, "")) for k2 in keys) + "\n")
    print("BVR_POSECAP csv -> %s" % place(args.csv))

# ---- renders and the scene -------------------------------------------------------------------------
scn.render.engine = "BLENDER_WORKBENCH"
scn.render.resolution_x, scn.render.resolution_y = 900, 700
scn.display.shading.light = "STUDIO"
scn.display.shading.color_type = "OBJECT"
for m in gm:
    if tuple(m.color) == (1.0, 1.0, 1.0, 1.0):
        m.color = (0.85, 0.55, 0.2, 1.0)
if args.render:
    scn.frame_set(0)
    focus = game_frame(ha, HC, order[43]).to_translation()
    size = 45.0
    R["renders"] = []
    for name, off in (("eye", Vector((-size * 1.2, 0.0, size * 0.3))), ("left", Vector((0.0, size * 1.4, size * 0.2))),
                      ("under", Vector((size * 0.6, size * 0.3, -size * 1.0)))):
        cam = bpy.data.objects.get("CapCam") or bpy.data.objects.new("CapCam", bpy.data.cameras.new("CapCam"))
        if cam.name not in scn.collection.objects:
            scn.collection.objects.link(cam)
        cam.location = focus + off
        cam.rotation_euler = (focus - cam.location).to_track_quat("-Z", "Y").to_euler()
        scn.camera = cam
        scn.render.filepath = place("%s-%s.png" % (args.render, name))
        bpy.ops.render.render(write_still=True)
        R["renders"].append(scn.render.filepath)
if args.save:
    bpy.ops.wm.save_as_mainfile(filepath=place(args.save))
    print("BVR_POSECAP scene -> %s" % place(args.save))
if args.out:
    with open(place(args.out), "w") as fh:
        json.dump(R, fh, indent=1)
for p in R["problems"][:5]:
    print("BVR_POSECAP problem: %s" % p)
print("BVR_POSECAP done, %d frame(s), problems=%d" % (len(frames), len(R["problems"])))
