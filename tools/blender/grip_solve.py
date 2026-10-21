# grip_solve.py - curl a hand's fingers around a part it carries, by the meshes, and emit the pose.
#
#   .\tools\blender-run.ps1 tools\blender\grip_solve.py -- --hands <NEWPlayerHands.glb> --gun <WP_PistolMesh.glb>
#       --hands-action Pistol__FastReloadPistol --gun-action FastReload --time 0.80
#       --part drum,DrumParent [--hand L] [--render grip] [--emit <header.h>] [--name kPistolCylinderGrip]
#
# The scene is the game's (game_frames.py): both armatures posed by their clips at --time, the gun on the
# hands' R_grip in the game's frame. The part is the gun mesh's vertices weighted to the --part bones; its
# surface is a BVH of their triangles. Each finger is a chain of three segments, each a capsule along its
# bone (joint to the next joint, the tip's length from the mesh) with the radius the finger mesh has there.
# The solve curls each joint in turn, base first, toward the part (about the axis that swings the segment
# toward the nearest surface point) until that segment or any one past it touches (capsule gap <= --touch),
# within a per-joint limit. Then the real deformed hand mesh is checked against the part: the nearest gap
# and the deepest penetration per finger. --emit writes the solved joints' rotations against their
# parents (Unreal axes), for the mod to apply with the clip's own joint offsets. Outputs stay in the model
# workspace except --emit.
import bpy, sys, os, json, re, argparse, math

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mathutils import Matrix, Quaternion, Vector  # noqa: E402
from mathutils.bvhtree import BVHTree  # noqa: E402
from game_frames import F, bone_turns, game_frame, ue_local  # noqa: E402

argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
ap = argparse.ArgumentParser(prog="grip_solve.py")
ap.add_argument("--hands", required=True)
ap.add_argument("--gun", required=True)
ap.add_argument("--hands-action", required=True)
ap.add_argument("--gun-action", required=True)
ap.add_argument("--time", type=float, required=True)
ap.add_argument("--part", required=True, help="the gun bones whose skin is the part, comma-separated")
ap.add_argument("--hand", default="L")
ap.add_argument("--socket", default="R_grip")
ap.add_argument("--touch", type=float, default=0.15, help="a capsule this near the surface touches (units)")
ap.add_argument("--step", type=float, default=1.5, help="degrees per curl step")
ap.add_argument("--render", default="")
ap.add_argument("--emit", default="")
ap.add_argument("--name", default="kGrip")
ap.add_argument("--hold", default="rim", choices=["rim", "across"],
                help="rim: the part's face in the palm, the fingers over its rim (a short, fat part); "
                     "across: its axis along the knuckles, the fingers round its side (a long one)")
ap.add_argument("--rim-at", type=float, default=0.85, help="rim hold: the face's centre from the wrist to the knuckles")
ap.add_argument("--out", default="grip-solve.json")
ap.add_argument("--part-mesh", default="",
                help="a separate model that hangs on the --part bone (the shotgun's shell, shotgun_shell.pskx) instead "
                     "of the gun mesh's own skin")
args = ap.parse_args(argv)
ws = os.environ.get("BVR_MODEL_WS", os.getcwd())
place = lambda p: p if os.path.isabs(p) else os.path.join(ws, "verification", p)
R = {"problems": []}

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


ha, hm, hacts = load(args.hands)
ga, gm, gacts = load(args.gun)
HC, _ = bone_turns(ha, args.hands)
GC, _ = bone_turns(ga, args.gun)
for arm, acts, rx in ((ha, hacts, args.hands_action), (ga, gacts, args.gun_action)):
    act = next((a for a in acts if re.search(rx, a.name, re.I)), None)
    if not act:
        raise SystemExit("BVR_GRIP no action matches %r" % rx)
    arm.animation_data_create()
    arm.animation_data.action = act
c = ga.constraints.new("CHILD_OF")
c.target, c.subtarget = ha, args.socket
c.inverse_matrix = HC.get(args.socket, Matrix.Identity(4)).inverted()
scn = bpy.context.scene
fps = scn.render.fps / scn.render.fps_base
scn.frame_set(int(math.floor(args.time * fps)), subframe=(args.time * fps) % 1.0)
bpy.context.view_layer.update()
# The clip's pose is the start; the solve edits pose bones directly, so the actions are let go of now.
for arm in (ha, ga):
    for pb in arm.pose.bones:
        pb.matrix_basis = pb.matrix_basis.copy()
    arm.animation_data.action = None
bpy.context.view_layer.update()


def evaluated_world_verts(obj):
    deps = bpy.context.evaluated_depsgraph_get()
    ev = obj.evaluated_get(deps)
    me = ev.to_mesh()
    pts = [ev.matrix_world @ v.co for v in me.vertices]
    polys = [tuple(p.vertices) for p in me.polygons]
    ev.to_mesh_clear()
    return pts, polys


# ---- the part's surface ----------------------------------------------------------------------------
parts = [p for p in re.split(r"[ ,]+", args.part.strip()) if p]  # PowerShell turns a,b into "a b"
gmesh = max(gm, key=lambda o: len(o.data.vertices))
gidx = {g.index: g.name for g in gmesh.vertex_groups}
own = set()
if args.part_mesh:
    # The model hung on the part's bone at its origin, as the engine attaches it to the bone's socket.
    before = set(bpy.data.objects)
    if hasattr(bpy.ops, "psk") and hasattr(bpy.ops.psk, "import_file"):
        bpy.ops.psk.import_file(filepath=os.path.abspath(args.part_mesh))
    else:
        bpy.ops.import_scene.psk(filepath=os.path.abspath(args.part_mesh))
    newo = [o for o in bpy.data.objects if o not in before]
    for o in newo:
        if o.type != "MESH":
            o.hide_render = True
    gmesh = next(o for o in newo if o.type == "MESH")
    gmesh.parent = None
    gmesh.location, gmesh.rotation_euler, gmesh.scale = (0, 0, 0), (0, 0, 0), (1, 1, 1)
    hc = gmesh.constraints.new("CHILD_OF")
    hc.target, hc.subtarget = ga, parts[0]
    hc.inverse_matrix = GC.get(parts[0], Matrix.Identity(4)).inverted()
    bpy.context.view_layer.update()
    own = set(range(len(gmesh.data.vertices)))
for v in ([] if args.part_mesh else gmesh.data.vertices):
    best = max(v.groups, key=lambda g: g.weight, default=None)
    if best and gidx.get(best.group) in parts and best.weight >= 0.5:
        own.add(v.index)
# The part's root: the listed bone that is an ancestor of the others (moving it moves them all).
root = next(p for p in parts if all(p == q or any(a.name == p for a in ga.pose.bones[q].parent_recursive) for q in parts))
bvh, centre, used, tri = None, None, None, None


def build_part():
    global bvh, centre, used, tri
    gpts, gpolys = evaluated_world_verts(gmesh)
    tri = [p for p in gpolys if all(i in own for i in p)]
    if not tri:
        raise SystemExit("BVR_GRIP no triangles weighted to %s" % parts)
    used = sorted({i for p in tri for i in p})
    remap = {i: k for k, i in enumerate(used)}
    bvh = BVHTree.FromPolygons([gpts[i] for i in used], [[remap[i] for i in p] for p in tri])
    centre = sum((gpts[i] for i in used), Vector()) / len(used)
    pts = [gpts[i] for i in used]
    fit_cylinder(pts)
    return pts


# The part as an exact cylinder (the face normals cannot say inside from outside: its chambers are open):
# of its root bone's axes, the one its surface keeps the most constant distance from; the radius its
# outer surface reaches (90th percentile), its length along that axis.
AXIS_IDX = None
cyl = None


def fit_cylinder(pts):
    global AXIS_IDX, cyl
    rw = game_frame(ga, GC, root).to_3x3()
    c0 = sum(pts, Vector()) / len(pts)
    if AXIS_IDX is None:
        scores = []
        for a in range(3):
            ax = rw.col[a].normalized()
            ds = [((p - c0) - ax * (p - c0).dot(ax)).length for p in pts]
            mean = sum(ds) / len(ds)
            scores.append((math.sqrt(sum((d - mean) ** 2 for d in ds) / len(ds)) / max(mean, 1e-6), a))
        AXIS_IDX = min(scores)[1]
    ax = rw.col[AXIS_IDX].normalized()
    ts = [(p - c0).dot(ax) for p in pts]
    lo, hi = min(ts), max(ts)
    rad = sorted(((p - c0) - ax * (p - c0).dot(ax)).length for p in pts)
    cyl = (c0 + ax * ((lo + hi) / 2), ax, rad[int(len(rad) * 0.9)], (hi - lo) / 2)


ppts = build_part()
R["part"] = {"vertices": len(used), "triangles": len(tri), "root": root}


def surface_gap(p):
    """Signed distance to the part's cylinder: + outside, - inside; and the nearest point on it."""
    c, ax, r, h = cyl
    v = p - c
    t = v.dot(ax)
    rv = v - ax * t
    rad = rv.length
    er, eh = rad - r, abs(t) - h
    if er <= 0 and eh <= 0:
        sd = max(er, eh)
    else:
        sd = math.sqrt(max(er, 0.0) ** 2 + max(eh, 0.0) ** 2)
    rd = rv / rad if rad > 1e-6 else ax.orthogonal().normalized()
    near = c + ax * max(-h, min(h, t)) + rd * r
    return sd, near


# ---- the fingers -----------------------------------------------------------------------------------
side = args.hand.upper()
FINGERS = ["Thumb", "Index", "Middle", "Ring", "Pinky"]
chains = {f: ["kBone_%s_%s%d" % (side, f, k) for k in (1, 2, 3)] for f in FINGERS}
hmesh = max(hm, key=lambda o: len(o.data.vertices))
hidx = {g.index: g.name for g in hmesh.vertex_groups}
seg_verts = {}  # bone -> vertex indices it mostly moves
for v in hmesh.data.vertices:
    best = max(v.groups, key=lambda g: g.weight, default=None)
    if best:
        seg_verts.setdefault(hidx.get(best.group), []).append(v.index)
hpts, _ = evaluated_world_verts(hmesh)


def head(name):
    return (ha.matrix_world @ ha.pose.bones[name].matrix).to_translation()


def seg_ends(name, chain):
    """The bone's segment: its head to the next joint, the tip's out to its farthest skin vertex."""
    k = chain.index(name)
    a = head(name)
    if k + 1 < len(chain):
        return a, head(chain[k + 1])
    d = head(name) - head(chain[k - 1])
    far = max((((hpts[i] - a).dot(d.normalized())) for i in seg_verts.get(name, [])), default=d.length)
    return a, a + d.normalized() * max(far, 0.5)


radius = {}
for f, ch in chains.items():
    for b in ch:
        a, e = seg_ends(b, ch)
        ab = e - a
        ds = []
        for i in seg_verts.get(b, []):
            u = max(0.0, min(1.0, (hpts[i] - a).dot(ab) / max(ab.length_squared, 1e-9)))
            ds.append((hpts[i] - (a + ab * u)).length)
        ds.sort()
        radius[b] = ds[(len(ds) * 3) // 4] if ds else 0.8  # the thicker side: a capsule that fits the skin


def seg_gap(b, ch):
    a, e = seg_ends(b, ch)
    best = 1e9
    for k in range(7):
        g, _ = surface_gap(a + (e - a) * (k / 6.0))
        best = min(best, g - radius[b])
    return best


def rotate_joint(b, axis, deg):
    pb = ha.pose.bones[b]
    w = ha.matrix_world @ pb.matrix
    h = w.to_translation()
    rot = Matrix.Translation(h) @ Matrix.Rotation(math.radians(deg), 4, axis) @ Matrix.Translation(-h)
    pb.matrix = ha.matrix_world.inverted() @ rot @ w
    bpy.context.view_layer.update()


# ---- the designed hold: the part across the base of the fingers, on the palm's side ----------------
# The clip's own hold at --time sinks the fingers into the part (the pistol's cylinder: index and middle
# 4-6 units inside), so the part is placed instead: its axis along the knuckles, its surface against the
# proximal segments on the side the fingers close to, a quarter of the way out along them.
wrist_name = "Bip01_%s_Hand" % side
W = head(wrist_name)
knuckles = [head(chains[f][0]) for f in ("Index", "Middle", "Ring", "Pinky")]
kmid = sum(knuckles, Vector()) / 4.0
fdir = (kmid - W).normalized()
across = (knuckles[3] - knuckles[0])
across = (across - fdir * across.dot(fdir)).normalized()
# The side the fingers close to: where the middle finger's tip already bends.
tip = seg_ends(chains["Middle"][2], chains["Middle"])[1]
pn = across.cross(fdir)
if pn.dot(tip - (head(chains["Middle"][0]) + fdir * (tip - head(chains["Middle"][0])).dot(fdir))) < 0:
    pn = -pn
_, paxis, pradius, _ = cyl  # the fitted cylinder (fit_cylinder)
centre = cyl[0]
prox = (head(chains["Middle"][1]) - head(chains["Middle"][0])).length
if args.hold == "across":
    # A long part: its axis along the knuckles, its curved side against the proximal segments.
    target_c = kmid + fdir * (prox * 0.25) + pn * (pradius + radius[chains["Middle"][0]] + args.touch)
    want = across
else:
    # A short, fat part (a puck - the pistol's cylinder is shorter than the hand is wide): its face in the
    # palm, centred under the fingers' bases, so every finger can hook over its rim.
    target_c = W + (kmid - W) * args.rim_at + pn * (cyl[3] + radius[chains["Middle"][0]] + args.touch)
    want = pn
turn = (paxis if paxis.dot(want) >= 0 else -paxis).rotation_difference(want).to_matrix().to_4x4()
move = Matrix.Translation(target_c) @ turn @ Matrix.Translation(-centre)
rpb = ga.pose.bones[root]
rpb.matrix = ga.matrix_world.inverted() @ move @ ga.matrix_world @ rpb.matrix
bpy.context.view_layer.update()
ppts = build_part()
# Seated out of the palm: the hand's skin that does not curl (the wrist bone's and the knuckles') must not
# be inside the part - it moves out along the palm's normal until none is deeper than --touch.
palm_bones = {wrist_name} | {chains[f][0] for f in ("Index", "Middle", "Ring", "Pinky")}
palm_verts = [i for b in palm_bones for i in seg_verts.get(b, [])]
for _ in range(60):
    hp, _ = evaluated_world_verts(hmesh)
    deepest = min(surface_gap(hp[i])[0] for i in palm_verts)
    if deepest >= -args.touch:
        break
    step = Matrix.Translation(pn * max(0.1, -deepest * 0.5))
    rpb.matrix = ga.matrix_world.inverted() @ step @ ga.matrix_world @ rpb.matrix
    bpy.context.view_layer.update()
    ppts = build_part()
R["placed"] = {"radius": pradius, "axis_along_knuckles": True}
print("BVR_GRIP the part placed across the knuckles: radius %.2f, %d triangles" % (pradius, len(tri)))

LIMIT = {1: 75.0, 2: 100.0, 3: 90.0}
LIMIT_THUMB = {1: 60.0, 2: 70.0, 3: 80.0}
def side_gap(b, ch):
    """The segment's capsule against the cylinder's CURVED side only (a brush past an end does not hold
    anything): the smallest radial gap over the samples that lie along the cylinder's length."""
    c, ax, r, h = cyl
    a, e = seg_ends(b, ch)
    best = 1e9
    for k in range(9):
        p = a + (e - a) * (k / 8.0)
        t = (p - c).dot(ax)
        if abs(t) > h:
            continue
        best = min(best, ((p - c) - ax * t).length - r - radius[b])
    return best


def wrap_deg(ch):
    """across: how far round the cylinder the finger reaches - the angle about its axis from the palm's side
    (0) to the fingertip (180 = the far side). rim: how far the fingertip hooks down past the rim - its
    depth along the axis from the face in the palm, as a percentage of the part's length (and it must be at
    the rim: the radial gap is the side_gap printed)."""
    c, ax, r, h = cyl
    tip = seg_ends(ch[2], ch)[1]
    if args.hold == "rim":
        a = ax if ax.dot(pn) >= 0 else -ax
        return 100.0 * max(0.0, (tip - (c - a * h)).dot(a)) / max(2 * h, 1e-6)
    v = (tip - c) - ax * (tip - c).dot(ax)
    palm = -pn - ax * (-pn).dot(ax)
    if v.length < 1e-6 or palm.length < 1e-6:
        return 0.0
    return math.degrees(v.normalized().angle(palm.normalized()))


# The fingers close about the knuckles' line, toward the palm's side; the thumb about the axis that swings
# it toward the part.
finger_axis = fdir.cross(pn).normalized()
R["solve"] = {}
for f, ch in chains.items():
    log = []
    for k, b in enumerate(ch):
        lim = (LIMIT_THUMB if f == "Thumb" else LIMIT)[k + 1]
        turned = 0.0

        def axis_now():
            if f != "Thumb":
                return finger_axis
            a, e = seg_ends(b, ch)
            _, near = surface_gap(e)
            ax = (e - a).normalized().cross(near - a)
            return ax.normalized() if ax.length > 1e-6 else None

        # Out of the part first, if the clip's curl sinks this segment (or one past it) into it.
        while turned > -60.0 and min(seg_gap(x, ch) for x in ch[k:]) < -args.touch:
            ax = axis_now()
            if ax is None:
                break
            rotate_joint(b, ax, -args.step)
            turned -= args.step
        # Then closed until THIS segment rests on the curved side - or a segment past it would go in.
        while turned < lim:
            contact = side_gap(b, ch) if args.hold == "across" else seg_gap(b, ch)
            if contact <= args.touch:
                break
            ax = axis_now()
            if ax is None:
                break
            rotate_joint(b, ax, args.step)
            if min(seg_gap(x, ch) for x in ch[k:]) < -args.touch:
                rotate_joint(b, ax, -args.step)
                break
            turned += args.step
        log.append({"joint": b, "deg": turned, "side_gap": side_gap(b, ch)})
    R["solve"][f] = log
    R.setdefault("wrap", {})[f] = wrap_deg(ch)
    print("BVR_GRIP %-6s %s   wrap %3.0f deg" % (f, "  ".join("%s %5.1f deg side %+.2f" % (x["joint"][-1], x["deg"], x["side_gap"])
                                                         for x in log), R["wrap"][f]))
print("BVR_GRIP the cylinder: length %.2f, diameter %.2f | its axis against the knuckles' line %.0f deg, against the "
      "palm's normal %.0f deg, against the fingers' direction %.0f deg | the hand: knuckle span %.2f, wrist to knuckles %.2f"
      % (2 * cyl[3], 2 * cyl[2], math.degrees(min(cyl[1].angle(across), cyl[1].angle(-across))),
         math.degrees(min(cyl[1].angle(pn), cyl[1].angle(-pn))), math.degrees(min(cyl[1].angle(fdir), cyl[1].angle(-fdir))),
         (knuckles[3] - knuckles[0]).length, (kmid - W).length))


def contact_share(ch):
    """The share of the finger's length (capsule axis, 30 samples) whose skin is within 0.5 of the part."""
    n = hit = 0
    for b in ch:
        a, e = seg_ends(b, ch)
        for k in range(10):
            p = a + (e - a) * ((k + 0.5) / 10.0)
            n += 1
            hit += 1 if surface_gap(p)[0] - radius[b] <= 0.5 else 0
    return hit / max(n, 1)


R["contact"] = {f: contact_share(ch) for f, ch in chains.items()}
print("BVR_GRIP contact along each finger: " + "  ".join("%s %.0f%%" % (f, 100 * v) for f, v in R["contact"].items()))

# ---- refined on the real mesh: a finger's skin sunk deeper than 0.3 backs its joint off ------------
for f, ch in chains.items():
    for _ in range(40):
        hp, _ = evaluated_world_verts(hmesh)
        worst = None
        for b in ch:
            for i in seg_verts.get(b, []):
                g = surface_gap(hp[i])[0]
                if worst is None or g < worst[0]:
                    worst = (g, b)
        if worst is None or worst[0] >= -0.3:
            break
        b = worst[1]
        a, e = seg_ends(b, ch)
        _, near = surface_gap(e)
        axis = (e - a).normalized().cross(near - a) if near is not None else Vector()
        if axis.length < 1e-6:
            break
        rotate_joint(b, axis.normalized(), -args.step)
        for x in R["solve"][f]:
            if x["joint"] == b:
                x["deg"] -= args.step

# ---- the check on the real meshes ------------------------------------------------------------------
hpts, _ = evaluated_world_verts(hmesh)
R["check"] = {}
for f, ch in chains.items():
    gaps = [surface_gap(hpts[i])[0] for b in ch for i in seg_verts.get(b, [])]
    if gaps:
        R["check"][f] = {"nearest": min(g for g in gaps if g >= 0) if any(g >= 0 for g in gaps) else 0.0,
                         "deepest": -min(min(gaps), 0.0), "inside": sum(1 for g in gaps if g < -0.05)}
        print("BVR_GRIP check %-6s nearest gap %.2f, deepest in %.2f, %d vertices inside" %
              (f, R["check"][f]["nearest"], R["check"][f]["deepest"], R["check"][f]["inside"]))

# ---- the emitted pose: each solved joint against its parent, Unreal axes ---------------------------
rows = []
for f, ch in chains.items():
    for b in ch:
        pb = ha.pose.bones[b]
        par = pb.parent.name
        local = game_frame(ha, HC, par).inverted() @ game_frame(ha, HC, b)
        q = (F @ local @ F).to_quaternion()
        idx = list(ha.pose.bones.keys()).index(b)
        rows.append((b, par, q))
R["emit"] = [{"bone": b, "parent": p, "q_xyzw": [q.x, q.y, q.z, q.w]} for b, p, q in rows]
# The part's bones held in this hand: each against the wrist's game frame, Unreal axes (the carry puts the
# part here instead of where the clip has it).
wristg = game_frame(ha, HC, wrist_name)
held = []
for pname in parts:
    rel = F @ (wristg.inverted() @ game_frame(ga, GC, pname)) @ F
    t, q = rel.to_translation(), rel.to_quaternion()
    held.append((pname, t, q))
R["part_in_wrist"] = [{"bone": n, "p": list(t), "q_xyzw": [q.x, q.y, q.z, q.w]} for n, t, q in held]
if args.emit:
    # Engine bone indices by name (the glb's skin order is the engine's).
    from game_frames import glb_bone_order
    order = glb_bone_order(args.hands)
    with open(args.emit, "w", newline="\n") as fh:
        fh.write("#pragma once\n// Generated by tools/blender/grip_solve.py - do not edit by hand. %s at %.3f s of %s,\n"
                 "// the part %s: each finger joint's rotation against its parent, Unreal axes (x y z w).\n"
                 % (args.name, args.time, args.hands_action, args.part))
        fh.write("#include \"game/bioshock1r/grips.h\"\n\nnamespace bvr::b1r::grips {\n")
        fh.write("inline constexpr GripJoint %s[] = {\n" % args.name)
        for b, p, q in rows:
            fh.write("    {%d, %d, {%.7ff, %.7ff, %.7ff, %.7ff}}, // %s\n" % (order.index(b), order.index(p), q.x, q.y, q.z, q.w, b))
        fh.write("};\n")
        fh.write("// The part's bones in the hand: position and rotation against the wrist bone (%s), Unreal axes.\n"
                 % wrist_name)
        fh.write("inline constexpr GripPart %sPart[] = {\n" % args.name)
        for n, t, q in held:
            fh.write("    {\"%s\", {%.5ff, %.5ff, %.5ff}, {%.7ff, %.7ff, %.7ff, %.7ff}},\n" % (n, t.x, t.y, t.z, q.x, q.y, q.z, q.w))
        fh.write("};\ninline constexpr int %sWrist = %d;\n" % (args.name, order.index(wrist_name)))
        fh.write("} // namespace bvr::b1r::grips\n")
    print("BVR_GRIP emitted %d joints -> %s" % (len(rows), args.emit))

# ---- renders ---------------------------------------------------------------------------------------
if args.render:
    scn.render.engine = "BLENDER_WORKBENCH"
    scn.render.resolution_x, scn.render.resolution_y = 900, 700
    scn.display.shading.light = "STUDIO"
    scn.display.shading.color_type = "OBJECT"
    for m in gm:
        m.color = (0.85, 0.55, 0.2, 1.0)
    focus = centre
    size = 22.0
    for name, off in (("a", Vector((-size, size * 0.6, size * 0.4))), ("b", Vector((size * 0.7, -size, size * 0.3))),
                      ("c", Vector((0.0, size * 0.3, -size * 1.1)))):
        cam = bpy.data.objects.get("GripCam") or bpy.data.objects.new("GripCam", bpy.data.cameras.new("GripCam"))
        if cam.name not in scn.collection.objects:
            scn.collection.objects.link(cam)
        cam.location = focus + off
        cam.rotation_euler = (focus - cam.location).to_track_quat("-Z", "Y").to_euler()
        scn.camera = cam
        scn.render.filepath = place("%s-%s.png" % (args.render, name))
        bpy.ops.render.render(write_still=True)
with open(place(args.out), "w") as fh:
    json.dump(R, fh, indent=1)
print("BVR_GRIP done")
