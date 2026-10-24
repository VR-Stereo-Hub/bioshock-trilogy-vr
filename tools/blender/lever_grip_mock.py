# lever_grip_mock.py - a designed hold on a recorded frame: the free hand closed round the crossbow lever's
# crossbar, on the exact frame a pose recording replayed (pose_capture.py --record ... --save <scene>.blend).
#
#   .\tools\blender-run.ps1 tools\blender\lever_grip_mock.py -Blend <ws>\verification\<scene>.blend --
#       --frames 66,78,93 --out <prefix> --fingers crossbow-rest-fist.json [--hold over|under]
#       [--views eye,side,top,back] [--size 30] [--before]
#
# For each frame: optionally renders the frame as the game drew it (--before), then the designed hold:
#   1. the crossbar as a cylinder: the gun skin weighted to X_Lever within --bar-radius of X_LeverNUB, its axis the
#      lever bone's axis that skin runs longest along (the NUB is the bar's MIDDLE: the 12:21 replay, 16 units along
#      the lever bone's Y, centred on it), its radius the wooden roll's (the end caps are fatter);
#   2. the free hand's fingers set to a clip's own fist (--fingers, hold_reference.py: the left hand round the
#      crossbow's stock in Crossbow__FidgetCrossbow), else left as recorded;
#   3. the hand moved rigidly so the fist's own handle axis (hand_grip.h, measured on the settled wrench fist) lies
#      on the bar's axis, its centre pushed out along the palm's normal by how much thicker the bar is than the
#      handle the fist closes on, turned about the bar so the forearm points at the shoulder; --hold over puts the
#      palm's normal down (the knuckles on top), under the other way;
#   4. the arm re-solved to the moved wrist (two bones, the elbow kept on its side);
#   5. the four fingers opened from the fist (or closed further) in a grasp's proportions until their skin rests on
#      the bar, the thumb opened out of it.
# Prints the hold's numbers (BVR_LEVER lines): the fist on the bar, each finger's turn from the fist, the contact
# on the deformed mesh and how far round the bar each tip reaches. Renders land in the model workspace's
# verification folder; --out <prefix>.json keeps the numbers. Never launches a game.
import bpy, sys, os, argparse, math, json

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mathutils import Matrix, Vector  # noqa: E402

argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
ap = argparse.ArgumentParser(prog="lever_grip_mock.py")
ap.add_argument("--frames", required=True)
ap.add_argument("--out", required=True)
ap.add_argument("--fingers", default="", help="a hold_reference.py JSON: start from that clip's fist")
ap.add_argument("--hold", default="over", choices=["over", "under"])
ap.add_argument("--views", default="eye,side,top,back")
ap.add_argument("--size", type=float, default=30.0)
ap.add_argument("--hand", default="L", help="the free hand, L or R")
ap.add_argument("--bar-radius", type=float, default=10.0, help="the crossbar's skin: X_Lever's within this of the NUB")
ap.add_argument("--step", type=float, default=1.5)
ap.add_argument("--rigid", default="", help="<prefix>.json:<frame> - carry that frame's hold rigidly on the lever")
ap.add_argument("--no-correct", action="store_true", help="skip the fist-centre residual correction (the 13:xx frame-70 pick)")
ap.add_argument("--carry", default="lever", choices=["lever", "gun"], help="--rigid: the hand fixed to the lever, or to the gun at the bar's centre")
ap.add_argument("--sink", type=float, default=0.4, help="a finger's skin may sink this deep into the bar")
ap.add_argument("--res", default="800x600")
ap.add_argument("--before", action="store_true", help="also render the frame as recorded")
args = ap.parse_args(argv)
ws = os.environ.get("BVR_MODEL_WS", os.getcwd())
place = lambda p: p if os.path.isabs(p) else os.path.join(ws, "verification", p)

scn = bpy.context.scene
arms = [o for o in scn.objects if o.type == "ARMATURE"]
ga = next(a for a in arms if "X_Lever" in a.pose.bones)
ha = next(a for a in arms if "Bip01_L_Hand" in a.pose.bones)
gmesh = max((o for o in scn.objects if o.type == "MESH" and o.find_armature() == ga), key=lambda o: len(o.data.vertices))
hmesh = max((o for o in scn.objects if o.type == "MESH" and o.find_armature() == ha), key=lambda o: len(o.data.vertices))
side = args.hand.upper()
FINGERS = ["Thumb", "Index", "Middle", "Ring", "Pinky"]
chains = {f: ["kBone_%s_%s%d" % (side, f, k) for k in (1, 2, 3)] for f in FINGERS}
wrist_name, fore_name, upper_name = "Bip01_%s_Hand" % side, "Bip01_%s_Forearm" % side, "Bip01_%s_UpperArm" % side
R = {"frames": {}}


def upd():
    bpy.context.view_layer.update()


def wmat(arm, name):
    return arm.matrix_world @ arm.pose.bones[name].matrix


def set_wmat(arm, name, m):
    arm.pose.bones[name].matrix = arm.matrix_world.inverted() @ m


def head(arm, name):
    return wmat(arm, name).to_translation()


def verts(obj):
    deps = bpy.context.evaluated_depsgraph_get()
    ev = obj.evaluated_get(deps)
    me = ev.to_mesh()
    pts = [ev.matrix_world @ v.co for v in me.vertices]
    ev.to_mesh_clear()
    return pts


gidx = {g.index: g.name for g in gmesh.vertex_groups}
lever_verts = [v.index for v in gmesh.data.vertices
               if (b := max(v.groups, key=lambda g: g.weight, default=None)) and gidx.get(b.group) == "X_Lever" and b.weight >= 0.5]
hidx = {g.index: g.name for g in hmesh.vertex_groups}
seg_verts = {}
for v in hmesh.data.vertices:
    b = max(v.groups, key=lambda g: g.weight, default=None)
    if b:
        seg_verts.setdefault(hidx.get(b.group), []).append(v.index)


def bar_cylinder():
    gp = verts(gmesh)
    nub = head(ga, "X_LeverNUB")
    pts = [gp[i] for i in lever_verts if (gp[i] - nub).length <= args.bar_radius]
    rw = wmat(ga, "X_Lever").to_3x3()
    best = None
    for a in range(3):
        ax = rw.col[a].normalized()
        ts = [(p - nub).dot(ax) for p in pts]
        ext = max(ts) - min(ts)
        if best is None or ext > best[0]:
            best = (ext, ax, min(ts), max(ts))
    _, ax, lo, hi = best
    c = nub + ax * ((lo + hi) / 2)
    h = (hi - lo) / 2
    rad = sorted(((p - c) - ax * (p - c).dot(ax)).length for p in pts if abs((p - c).dot(ax)) < 0.6 * h)
    return c, ax, rad[len(rad) // 2], h, len(pts)


def seg_ends(name, chain, hp):
    k = chain.index(name)
    a = head(ha, name)
    if k + 1 < len(chain):
        return a, head(ha, chain[k + 1])
    d = (a - head(ha, chain[k - 1])).normalized()
    far = max(((hp[i] - a).dot(d) for i in seg_verts.get(name, [])), default=1.0)
    return a, a + d * max(far, 0.5)


def rotate_about(name, axis, deg):
    m = wmat(ha, name)
    hh = m.to_translation()
    rot = Matrix.Translation(hh) @ Matrix.Rotation(math.radians(deg), 4, axis) @ Matrix.Translation(-hh)
    set_wmat(ha, name, rot @ m)
    upd()


def two_bone_ik(target_wrist):
    """Shoulder fixed, elbow kept on its side: the upper arm and forearm turned so the forearm's end is the wrist."""
    S, E, Wp = head(ha, upper_name), head(ha, fore_name), head(ha, wrist_name)
    l1, l2 = (E - S).length, (Wp - E).length
    d = target_wrist - S
    dist = max(1e-4, min(d.length, l1 + l2 - 1e-3))
    dn = d.normalized()
    pole = (E - S) - dn * (E - S).dot(dn)
    pole = pole.normalized() if pole.length > 1e-6 else dn.orthogonal().normalized()
    a = (l1 * l1 + dist * dist - l2 * l2) / (2 * dist)
    hgt = math.sqrt(max(0.0, l1 * l1 - a * a))
    E2 = S + dn * a + pole * hgt
    q1 = (E - S).normalized().rotation_difference((E2 - S).normalized())
    set_wmat(ha, upper_name, Matrix.Translation(S) @ q1.to_matrix().to_4x4() @ Matrix.Translation(-S) @ wmat(ha, upper_name))
    upd()
    E, Wp = head(ha, fore_name), head(ha, wrist_name)
    q2 = (Wp - E).normalized().rotation_difference((target_wrist - E).normalized())
    set_wmat(ha, fore_name, Matrix.Translation(E) @ q2.to_matrix().to_4x4() @ Matrix.Translation(-E) @ wmat(ha, fore_name))
    upd()


# hand_grip.h (hand_pivot.py, the settled wrench fist): in the frame made from bone heads - origin the wrist, x to
# the middle finger's base, y from the little finger's base toward the index's, z = x cross y times the side
# (+1 right, -1 left) - the fist's centre, its handle axis and the palm's outward normal, scaled by the live
# wrist-to-middle-base distance over 12.0986.
FIST_C, FIST_AX, PALM_OUT, FIST_UNIT = (10.8726, 0.4981, -3.6547), (0.1453, 0.9885, -0.0421), (0.4091, -0.0987, -0.9072), 12.0986


def fist_frame():
    sgn = 1.0 if side == "R" else -1.0
    o = head(ha, wrist_name)
    x = head(ha, chains["Middle"][0]) - o
    lx = x.length
    x.normalize()
    y = head(ha, chains["Index"][0]) - head(ha, chains["Pinky"][0])
    y = (y - x * y.dot(x)).normalized()
    z = x.cross(y) * sgn
    sc = lx / FIST_UNIT
    pt = lambda c3: o + (x * c3[0] + y * c3[1] + z * c3[2]) * sc
    dr = lambda c3: (x * c3[0] + y * c3[1] + z * c3[2]).normalized()
    return pt(FIST_C), dr(FIST_AX), dr(PALM_OUT)


FINGERS_REF = json.load(open(place(args.fingers))) if args.fingers else None


def bar_depth(ch, c, ax, r, h):
    hp = verts(hmesh)
    d = -1e9
    for b in ch:
        for i in seg_verts.get(b, []):
            v = hp[i] - c
            t = v.dot(ax)
            if abs(t) <= h:
                d = max(d, r - (v - ax * t).length)
    return d


def design(fr):
    out = {}
    c, ax, r, h, npts = bar_cylinder()
    out["bar"] = {"radius": r, "length": 2 * h, "vertices": npts}
    # 1. The fingers: a clip's own fist.
    if FINGERS_REF:
        for n, chn in FINGERS_REF["fingers"].items():
            pb = ha.pose.bones.get(n.replace("_L_", "_%s_" % side))
            if pb:
                pb.rotation_mode = "QUATERNION"
                pb.location, pb.rotation_quaternion, pb.scale = chn["loc"], chn["rot"], chn["scale"]
        upd()
    # 2. The fist's own handle, and how thick a handle it closes on (the four fingers' skin nearest that axis).
    F, Hd, Po = fist_frame()
    hp = verts(hmesh)
    rs = []
    for f in ("Index", "Middle", "Ring", "Pinky"):
        for b in chains[f]:
            for i in seg_verts.get(b, []):
                v = hp[i] - F
                rs.append((v - Hd * v.dot(Hd)).length)
    rs.sort()
    r_fist = rs[len(rs) // 20]
    # 3. The hand moved rigidly onto the bar.
    shoulder = head(ha, upper_name)
    W = head(ha, wrist_name)
    fore_now = ((W - F) - Hd * (W - F).dot(Hd)).normalized()
    centre_now = F + Po * (r - r_fist)
    best = None
    for sgn in (1.0, -1.0):
        a_w = ax * sgn
        to_sh = shoulder - c
        to_sh = (to_sh - a_w * to_sh.dot(a_w)).normalized()
        M_now = Matrix((Hd, fore_now, Hd.cross(fore_now))).transposed()
        M_want = Matrix((a_w, to_sh, a_w.cross(to_sh))).transposed()
        Rm = M_want @ M_now.inverted()
        po_w = Rm @ Po
        score = -po_w.z if args.hold == "over" else po_w.z
        if best is None or score > best[0]:
            best = (score, Rm)
    Rm = best[1]
    T = Matrix.Translation(c) @ Rm.to_4x4() @ Matrix.Translation(-centre_now)
    new_wrist = T @ wmat(ha, wrist_name)
    two_bone_ik(new_wrist.to_translation())
    set_wmat(ha, wrist_name, new_wrist)
    upd()
    # The replayed arm carries a slightly non-uniform scale (the IK's affine), so a pose matrix set under it lands a
    # little off: the residual of the fist centre is taken out by moving the wrist again, three times.
    for _ in range(0 if args.no_correct else 3):
        F2, Hd2, Po2 = fist_frame()
        radial = (F2 - c) - ax * (F2 - c).dot(ax)
        want = radial.normalized() * (r - r_fist) if radial.length > 1e-6 else radial
        set_wmat(ha, wrist_name, Matrix.Translation(want - radial) @ wmat(ha, wrist_name))
        upd()
    F2, Hd2, Po2 = fist_frame()
    off = ((F2 - c) - ax * (F2 - c).dot(ax)).length
    print("BVR_LEVER f%03d the fist closes on a handle of radius %.2f, the bar's is %.2f | after the move the fist centre "
          "is %.2f off the bar's axis (wanted %.2f), its handle axis %.1f deg off the bar's" %
          (fr, r_fist, r, off, r - r_fist, math.degrees(min(Hd2.angle(ax), Hd2.angle(-ax)))))
    out["fist"] = {"r_fist": r_fist, "centre_off_axis": off}
    # 4. The four fingers opened from the fist (or closed further) to rest on the bar.
    o = head(ha, wrist_name)
    fdir = (head(ha, chains["Middle"][0]) - o).normalized()
    finger_axis = Hd2 if Hd2.dot(fdir.cross(Po2)) >= 0 else -Hd2   # flexion: the fingers toward the palm's side
    RATIO = (1.0, 1.25, 0.85)
    START = {b: (ha.pose.bones[b].location.copy(), ha.pose.bones[b].rotation_quaternion.copy(), ha.pose.bones[b].scale.copy())
             for chx in chains.values() for b in chx}

    def set_rel(ch, angs, axis):
        for b in ch:
            pb = ha.pose.bones[b]
            pb.location, pb.rotation_quaternion, pb.scale = START[b]
        upd()
        for b, a in zip(ch, angs):
            if abs(a) > 1e-6:
                rotate_about(b, axis, a)

    out["fingers"] = {}
    for f in ("Index", "Middle", "Ring", "Pinky"):
        ch = chains[f]
        t = 0.0
        if bar_depth(ch, c, ax, r, h) > args.sink:
            while t > -90.0:
                t -= args.step
                set_rel(ch, [RATIO[k] * t for k in range(3)], finger_axis)
                if bar_depth(ch, c, ax, r, h) <= args.sink:
                    break
        else:
            while t < 60.0:
                set_rel(ch, [RATIO[k] * (t + args.step) for k in range(3)], finger_axis)
                if bar_depth(ch, c, ax, r, h) > args.sink:
                    set_rel(ch, [RATIO[k] * t for k in range(3)], finger_axis)
                    break
                t += args.step
        out["fingers"][f] = round(t, 1)
        print("BVR_LEVER f%03d %-6s %+.1f deg from the fist (proximal; middle x1.25, distal x0.85)" % (fr, f, t))
    ch = chains["Thumb"]
    t = 0.0
    while bar_depth(ch, c, ax, r, h) > args.sink and t > -80.0:
        t -= args.step
        set_rel(ch, [t, t, t], finger_axis)
    out["fingers"]["Thumb"] = round(t, 1)
    print("BVR_LEVER f%03d Thumb  %+.1f deg from the fist" % (fr, t))
    # 5. The hold on the deformed mesh.
    hp = verts(hmesh)
    F3, Hd3, Po3 = fist_frame()
    out["contact"] = {}
    for f, ch in chains.items():
        ds = []
        for b in ch:
            for i in seg_verts.get(b, []):
                v = hp[i] - c
                tt = v.dot(ax)
                if abs(tt) <= h:
                    ds.append((v - ax * tt).length - r)
        tip = seg_ends(ch[2], ch, hp)[1]
        tv = (tip - c) - ax * (tip - c).dot(ax)
        palm = -Po3 - ax * (-Po3).dot(ax)
        wrap = math.degrees(tv.angle(palm)) if tv.length > 1e-6 and palm.length > 1e-6 else 0.0
        on = sum(1 for d in ds if abs(d) <= 0.8) / max(len(ds), 1)
        deep = -min(min(ds, default=0.0), 0.0)
        out["contact"][f] = {"on": on, "deepest": deep, "wrap": wrap}
        print("BVR_LEVER f%03d %-6s %3.0f%% of its skin on the bar, deepest in %.2f, tip round the bar %3.0f deg" %
              (fr, f, 100 * on, deep, wrap))
    rel = wmat(ga, "X_Lever").inverted() @ wmat(ha, wrist_name)
    out["wrist_in_lever"] = [list(row) for row in rel]
    # The same hold against the GUN with the bar's centre as origin (--carry gun: the bar turns in the fist, the
    # hand keeps its angle to the gun).
    gr = wmat(ga, "X_XbowParent").to_3x3().normalized()
    wm = wmat(ha, wrist_name)
    out["wrist_rot_in_gun"] = [list(row) for row in (gr.inverted() @ wm.to_3x3())]
    out["wrist_off_in_gun"] = list(gr.inverted() @ (wm.to_translation() - c))
    out["finger_channels"] = {b: {"loc": list(ha.pose.bones[b].location), "rot": list(ha.pose.bones[b].rotation_quaternion),
                                  "scale": list(ha.pose.bones[b].scale)} for chx in chains.values() for b in chx}
    return out


RIGID = None
if args.rigid:
    path, frk = args.rigid.rsplit(":", 1)
    RIGID = json.load(open(place(path)))["frames"][frk]


def design_rigid(fr):
    """The hold carried rigidly by the lever: the wrist where the reference frame had it in X_Lever's frame, the
    fingers as they closed there - one constant pose for the whole stroke."""
    for b, chn in RIGID["finger_channels"].items():
        pb = ha.pose.bones[b]
        pb.rotation_mode = "QUATERNION"
        pb.location, pb.rotation_quaternion, pb.scale = chn["loc"], chn["rot"], chn["scale"]
    upd()
    if args.carry == "gun":
        c0, _, _, _, _ = bar_cylinder()
        gr = wmat(ga, "X_XbowParent").to_3x3().normalized()
        new_wrist = (gr @ Matrix(RIGID["wrist_rot_in_gun"])).to_4x4()
        new_wrist.translation = c0 + gr @ Vector(RIGID["wrist_off_in_gun"])
    else:
        new_wrist = wmat(ga, "X_Lever") @ Matrix(RIGID["wrist_in_lever"])
    two_bone_ik(new_wrist.to_translation())
    set_wmat(ha, wrist_name, new_wrist)
    upd()
    # A pose matrix set under the replayed arm's slightly non-uniform scale lands a little off (it grows with how
    # far the arm has turned): the wrist is set again by the residual until it stands where the lever has it.
    for _ in range(6):
        delta = new_wrist @ wmat(ha, wrist_name).inverted()
        set_wmat(ha, wrist_name, delta @ wmat(ha, wrist_name))
        upd()
    err = (wmat(ha, wrist_name).to_translation() - new_wrist.to_translation()).length
    print("BVR_LEVER f%03d rigid: the wrist %.3f from where the lever carries it" % (fr, err))
    c, ax, r, h, _ = bar_cylinder()
    out = {}
    for f, ch in chains.items():
        d = bar_depth(ch, c, ax, r, h)
        out[f] = d
    print("BVR_LEVER f%03d rigid: deepest finger skin in the bar %s" % (fr, {k: round(v, 2) for k, v in out.items()}))
    return {"rigid_depth": out}


w, hres = (int(v) for v in args.res.split("x"))
scn.render.engine = "BLENDER_WORKBENCH"
scn.render.resolution_x, scn.render.resolution_y = w, hres
scn.display.shading.light = "STUDIO"
scn.display.shading.color_type = "OBJECT"
cam = bpy.data.objects.get("ViewCam") or bpy.data.objects.new("ViewCam", bpy.data.cameras.new("ViewCam"))
if cam.name not in scn.collection.objects:
    scn.collection.objects.link(cam)
scn.camera = cam
S = args.size
OFFS = {"side": Vector((0, S, S * 0.15)), "top": Vector((-S * 0.2, 0, S * 1.3)), "front": Vector((S * 1.3, 0, S * 0.2)),
        "left": Vector((0, -S, S * 0.15)), "back": Vector((-S * 1.2, S * 0.3, S * 0.4)), "under": Vector((S * 0.3, 0, -S * 1.2))}


def render(tag, fr, focus):
    for view in args.views.split(","):
        if view in ("eye", "eyez"):
            # From the hands actor's origin (the player's eye): eye the headset's wide view, eyez a closer look.
            cam.data.angle = 1.22 if view == "eye" else 0.55
            cam.location = Vector((0, 0, 0))
        else:
            cam.data.angle = 0.9
            cam.location = focus + OFFS[view]
        cam.rotation_euler = (focus - cam.location).to_track_quat("-Z", "Y").to_euler()
        scn.render.filepath = place("%s-f%03d-%s-%s.png" % (args.out, fr, tag, view))
        bpy.ops.render.render(write_still=True)


# A render re-evaluates the actions and would put the recorded pose back over the designed one: each frame is set
# with the actions on, then they are taken off so the pose stays as edited.
ACTIONS = {a.name: (a.animation_data.action if a.animation_data else None) for a in (ha, ga)}
for fr in (int(x) for x in args.frames.split(",")):
    for a in (ha, ga):
        if a.animation_data:
            a.animation_data.action = ACTIONS[a.name]
    scn.frame_set(fr)
    upd()
    for a in (ha, ga):
        if a.animation_data:
            a.animation_data.action = None
    upd()
    focus = head(ga, "X_LeverNUB")
    if args.before:
        render("before", fr, focus)
    if RIGID:
        R["frames"][fr] = design_rigid(fr)
        render("rigid-" + args.carry, fr, focus)
    else:
        R["frames"][fr] = design(fr)
        render("after-" + args.hold, fr, focus)
with open(place(args.out + ".json"), "w") as fh:
    json.dump(R, fh, indent=1)
print("BVR_LEVER done")
