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
import bpy, sys, os, json, argparse, math, re

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
ap.add_argument("--focus", default="grip", help="grip | palm | a gun bone: what the renders look at")
ap.add_argument("--size", type=float, default=45.0, help="the renders' camera distance scale (rig units)")
ap.add_argument("--hide-arms", action="store_true", help="hide the arm meshes' sleeves (not rebuilt: IK)")
ap.add_argument("--grip", default="", help="a grips header (grip_solve.py --emit): apply it to the capture with the "
                                           "mod's own math (bones.cpp, manual_reload.cpp carry) and measure the hold")
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
                elif k == "sleeves":
                    cap["sleeves"] = {0: [int(x) for x in tok[2:7]], 1: [int(x) for x in tok[8:13]]}
                elif k == "arm":
                    cap.setdefault("arm", {})[(int(tok[1]), int(tok[2]))] = (int(tok[3]) == 1,
                                                                             [float(x) for x in tok[4:16]])
                elif k == "b":
                    v = [float(x) for x in tok[2:]]
                    cap["bones"][int(tok[1])] = (v[0:3], v[3:7], v[7:10])
                elif k == "clip":
                    cap["clips"][tok[1]] = (tok[2], float(tok[3]))
                elif k == "weapon":
                    v = [float(x) for x in tok[4:12]]
                    cap["weapon"] = {"class": tok[1], "onsocket": int(tok[3]) == 1, "loc": v[0:3], "q": v[3:7],
                                     "ds": v[7], "size": float(tok[-1])}
                elif k == "ammo":
                    v = [float(x) for x in tok[1:]]
                    cap["ammo"] = (v[0:3], v[3:6], v[6])
                elif k == "w":
                    v = [float(x) for x in tok[3:]]
                    cap["wbones"][int(tok[1])] = (tok[2], v[0:3], v[3:7], v[7:10])
            except (ValueError, IndexError):
                R["problems"].append("an unreadable line (a recording cut off mid-frame?): %r" % line[:60])
    if cap["bones"] and (len(cap["bones"]) >= len(frames[0]["bones"]) if frames else True):
        frames.append(cap)
    return frames


frames = parse(args.record or args.pose)


# ---- a designed grip, applied as the mod applies it ------------------------------------------------
def parse_grip(path):
    joints, parts_, wrist = [], [], 6
    for line in open(path):
        m = re.match(r"\s*\{(\d+), (\d+), \{([-\d.e]+)f, ([-\d.e]+)f, ([-\d.e]+)f, ([-\d.e]+)f\}\}", line)
        if m:
            joints.append((int(m.group(1)), int(m.group(2)), [float(m.group(k)) for k in range(3, 7)]))
            continue
        m = re.match(r'\s*\{"(\w+)", \{([-\d.e]+)f, ([-\d.e]+)f, ([-\d.e]+)f\}, \{([-\d.e]+)f, ([-\d.e]+)f, ([-\d.e]+)f, ([-\d.e]+)f\}\}', line)
        if m:
            parts_.append((m.group(1), [float(m.group(k)) for k in range(2, 5)], [float(m.group(k)) for k in range(5, 9)]))
            continue
        m = re.match(r"\s*inline constexpr int \w+Wrist = (\d+);", line)
        if m:
            wrist = int(m.group(1))
    return joints, parts_, wrist


def qmul(a, b):  # x y z w
    x1, y1, z1, w1 = a
    x2, y2, z2, w2 = b
    return [w1 * x2 + x1 * w2 + y1 * z2 - z1 * y2, w1 * y2 - x1 * z2 + y1 * w2 + z1 * x2,
            w1 * z2 + x1 * y2 - y1 * x2 + z1 * w2, w1 * w2 - x1 * x2 - y1 * y2 - z1 * z2]


def qrot(q, v):
    x, y, z, w = q
    Q = Quaternion((w, x, y, z))
    return list(Q @ Vector(v))


def apply_grip(cap, grip):
    """bones.cpp's DESIGNED GRIP on the capture's engine pose (component space, Unreal axes), and
    carry()'s placement of the part against the wrist (kept for pose_frame, in the same space)."""
    joints, parts_, wrist = grip
    clip = {i: (list(p), list(q)) for i, (p, q, s) in cap["bones"].items()}
    out = {i: [list(p), list(q), list(s)] for i, (p, q, s) in cap["bones"].items()}
    for bone, parent, jq in joints:
        cp, cq = clip[parent]
        x, y, z, w = cq
        off = qrot([-x, -y, -z, w], [clip[bone][0][k] - cp[k] for k in range(3)])
        pp, pq = out[parent][0], out[parent][1]
        out[bone][0] = [pp[k] + qrot(pq, off)[k] for k in range(3)]
        out[bone][1] = qmul(pq, jq)
    cap["bones"] = {i: (v[0], v[1], v[2]) for i, v in out.items()}
    wp, wq = out[wrist][0], out[wrist][1]
    cap["grip_parts"] = {n: ([wp[k] + qrot(wq, p)[k] for k in range(3)], qmul(wq, q)) for n, p, q in parts_}


if args.grip:
    grip = parse_grip(args.grip)
    for cap in frames:
        apply_grip(cap, grip)
    print("BVR_POSECAP grip %s: %d joints, parts %s" % (os.path.basename(args.grip), len(grip[0]), [p[0] for p in grip[1]]))
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


F4 = Matrix.Diagonal((1.0, -1.0, 1.0, 1.0))
SLEEVES = {0: [3, 4, 5, 22, 23], 1: [24, 25, 26, 45, 46]}  # patterns.h kBoneLSleeve / kBoneRSleeve


def arm_matrix(cap, i, p, q, s):
    """A sleeve bone as the game draws it: the IK's affine on the game's bone when the capture has it,
    else carried rigidly with its hand (older captures) - never left behind to tear the wrist."""
    sleeves = cap.get("sleeves", SLEEVES)
    for h in (0, 1):
        if i in sleeves[h]:
            j = sleeves[h].index(i)
            valid, T = cap.get("arm", {}).get((h, j), (False, None))
            m = ue_matrix(p, q, s)
            if valid:
                T4 = Matrix(((T[0], T[1], T[2], T[3]), (T[4], T[5], T[6], T[7]), (T[8], T[9], T[10], T[11]),
                             (0, 0, 0, 1)))
                return F4 @ T4 @ F4 @ m
            fv, fq, ft, fp, fs = cap["handfix"].get(h, (False, None, None, None, None))
            if fv:
                fix = ue_matrix(ft, fq, (fs,) * 3) @ ue_matrix([-c for c in fp], [0, 0, 0, 1])
                return fix @ m
            return m
    return None


def pose_frame(cap):
    hf = {}
    for i, (p, q, s) in cap["bones"].items():
        if i < len(order):
            am = arm_matrix(cap, i, p, q, s)
            hf[order[i]] = am if am is not None else ue_matrix(*fixed(cap, i, p, q, s))
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
        # A designed grip's part: against the wrist in the clip's component space, then moved with the hand that
        # holds it by that hand's palette correction (the left hand's - the free hand's).
        for name, (p, q) in cap.get("grip_parts", {}).items():
            if name in ga.pose.bones:
                fh = 1 - cap.get("held", 1)
                valid, fq, ft, fp, fs = cap["handfix"].get(fh, (False, None, None, None, None))
                fix = (ue_matrix(ft, fq, (fs,) * 3) @ ue_matrix([-c for c in fp], [0, 0, 0, 1])) if valid else Matrix.Identity(4)
                # At the HAND's scale (the mod draws a part held by a designed grip at the size of the hand that
                # holds it - the grip was solved at the game's own 1:1): the hand's correction scales it.
                wf[name] = fix @ ue_matrix(p, q, (1, 1, 1))
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
    # The free hand's turn against the grip, and how far each finger is curled (its three joints' turns
    # against the wrist, summed, degrees) - a cock drawn with a different hand shows here.
    wq = (grip.inverted() @ wrist).to_quaternion()
    m["wrist_in_grip_rx"], m["wrist_in_grip_ry"], m["wrist_in_grip_rz"] = [math.degrees(a) for a in wq.to_euler("XYZ")]
    side = "L" if free == 0 else "R"
    for fn in ("Thumb", "Index", "Middle", "Ring", "Pinky"):
        tot, prev = 0.0, wrist
        for k in (1, 2, 3):
            bn = "kBone_%s_%s%d" % (side, fn, k)
            if bn not in ha.pose.bones:
                break
            f = game_frame(ha, HC, bn)
            tot += math.degrees((prev.inverted() @ f).to_quaternion().angle) if k > 1 else 0.0
            prev = f
        m["curl_" + fn] = tot
    if ga and cap.get("weapon"):
        for name in ga.pose.bones.keys():
            f = game_frame(ga, GC, name)
            m["%s_from_palm" % name] = (f.to_translation() - palm).length
            lp = ue_local(grip.inverted() @ f.to_translation())
            m["%s_in_grip_x" % name], m["%s_in_grip_y" % name], m["%s_in_grip_z" % name] = lp
    # The visible magazine where the ENGINE put it (a separate actor on a gun socket), against the bones: how far
    # it is from each, the nearest named - a part drawn away from the bone the mod moved shows here.
    if ga and cap.get("ammo"):
        al, aq, ak = cap["actor"]
        am = ue_matrix(al, aq, (ak,) * 3).inverted() @ ue_local(cap["ammo"][0])
        m["ammo_from_palm"] = (am - palm).length
        ds = {name: (game_frame(ga, GC, name).to_translation() - am).length for name in ga.pose.bones.keys()}
        near = min(ds, key=ds.get)
        m["ammo_nearest_bone"], m["ammo_from_nearest"] = near, ds[near]
        for name, d in ds.items():
            m["ammo_from_%s" % name] = d
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


def measure_grip():
    """The hold, on the meshes as drawn: per finger, the share of its skin within 0.6 of the part's cylinder
    and the deepest any of it goes in."""
    names = [p[0] for p in grip[1]]
    deps = bpy.context.evaluated_depsgraph_get()

    def verts(obj):
        ev = obj.evaluated_get(deps)
        me = ev.to_mesh()
        pts = [ev.matrix_world @ v.co for v in me.vertices]
        ev.to_mesh_clear()
        return pts

    gmesh = max(gm, key=lambda o: len(o.data.vertices) if o.type == "MESH" and o.vertex_groups else 0)
    gidx = {g.index: g.name for g in gmesh.vertex_groups}
    gp = verts(gmesh)
    own = [gp[v.index] for v in gmesh.data.vertices
           if (b := max(v.groups, key=lambda g: g.weight, default=None)) and gidx.get(b.group) in names and b.weight >= 0.5]
    c0 = sum(own, Vector()) / len(own)
    rw = game_frame(ga, GC, names[0]).to_3x3()
    best = None
    for a in range(3):
        ax = rw.col[a].normalized()
        ds = [((p - c0) - ax * (p - c0).dot(ax)).length for p in own]
        mean = sum(ds) / len(ds)
        sd = math.sqrt(sum((d - mean) ** 2 for d in ds) / len(ds)) / max(mean, 1e-6)
        if best is None or sd < best[0]:
            best = (sd, ax)
    ax = best[1]
    ts = [(p - c0).dot(ax) for p in own]
    c = c0 + ax * ((min(ts) + max(ts)) / 2)
    h = (max(ts) - min(ts)) / 2
    r = sorted(((p - c0) - ax * (p - c0).dot(ax)).length for p in own)[int(len(own) * 0.9)]

    def sdist(p):
        v = p - c
        t = v.dot(ax)
        er, eh = (v - ax * t).length - r, abs(t) - h
        return max(er, eh) if er <= 0 and eh <= 0 else math.sqrt(max(er, 0) ** 2 + max(eh, 0) ** 2)

    hmesh = max(hm, key=lambda o: len(o.data.vertices))
    hidx = {g.index: g.name for g in hmesh.vertex_groups}
    hp = verts(hmesh)
    out = {}
    side = "L" if cap0.get("held", 1) == 1 else "R"
    for f in ("Thumb", "Index", "Middle", "Ring", "Pinky"):
        bones_ = {"kBone_%s_%s%d" % (side, f, k) for k in (1, 2, 3)}
        ds = [sdist(hp[v.index]) for v in hmesh.data.vertices
              if (b := max(v.groups, key=lambda g: g.weight, default=None)) and hidx.get(b.group) in bones_]
        if ds:
            out[f] = {"on": sum(1 for d in ds if abs(d) <= 0.6) / len(ds), "deepest": -min(min(ds), 0.0),
                      "nearest": min(abs(d) for d in ds)}
            print("BVR_POSECAP grip %-6s %3.0f%% of its skin on the part, nearest %.2f, deepest in %.2f"
                  % (f, 100 * out[f]["on"], out[f]["nearest"], out[f]["deepest"]))
    print("BVR_POSECAP grip the part as drawn: radius %.2f, length %.2f" % (r, 2 * h))
    return out


cap0 = frames[0]
if args.grip:
    R["grip"] = measure_grip()
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
    if args.focus == "palm":  # the free hand (what it carries)
        held = frames[0].get("held", 1)
        focus = (game_frame(ha, HC, "Bip01_L_Hand" if held == 1 else "Bip01_R_Hand") @ ue_local(PALM[1 - held]))
    elif args.focus in ga.pose.bones if ga else False:
        focus = game_frame(ga, GC, args.focus).to_translation()
    else:
        focus = game_frame(ha, HC, order[43]).to_translation()
    size = args.size
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
