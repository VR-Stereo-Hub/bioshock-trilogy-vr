# support_candidates.py - other guns' own two-handed support grips tried on this gun: each gun's rest pose
# (grip_rest_poses.h: the left wrist against the grip bone 43 and every finger against the wrist, from that gun's idle
# clip) put on the gun of a replayed frame, moved along the barrel to this gun's own support station, then raised along
# the palm's normal until the palm's skin meets the gun's real skin. For choosing a support grip by eye.
#
#   .\tools\blender-run.ps1 tools\blender\support_candidates.py -Blend <ws>\verification\<scene>.blend --
#       --frame 5 --target Crossbow --candidates Crossbow,Shotgun,GrenadeLauncher,MachineGun --out <prefix>
#       [--views side,left,under,back,shooter]
#
# Renders into the model workspace's verification folder (one set per candidate). Never launches a game.
import bpy, sys, os, re, math, argparse

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mathutils import Matrix, Quaternion, Vector  # noqa: E402
from mathutils.bvhtree import BVHTree  # noqa: E402
from game_frames import bone_turns, game_frame, set_game_frame  # noqa: E402

argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
ap = argparse.ArgumentParser(prog="support_candidates.py")
ap.add_argument("--frame", type=int, required=True)
ap.add_argument("--target", default="Crossbow")
ap.add_argument("--candidates", default="Crossbow,Shotgun,GrenadeLauncher,MachineGun")
ap.add_argument("--out", required=True)
ap.add_argument("--views", default="side,left,under,back,shooter")
ap.add_argument("--size", type=float, default=35.0)
ap.add_argument("--sink", type=float, default=0.3)
ap.add_argument("--along", type=float, default=0.0, help="move every candidate this much further along the barrel")
args = ap.parse_args(argv)
ws = os.environ.get("BVR_MODEL_WS", os.getcwd())
place = lambda p: p if os.path.isabs(p) else os.path.join(ws, "verification", p)
here = os.path.dirname(os.path.abspath(__file__))

hdr = open(os.path.join(here, "..", "..", "src", "game", "bioshock1r", "grip_rest_poses.h")).read()
num = r"([-\d.]+)f"


def rest_row(cls):
    m = re.search(r'\{L"%s", \{%s, %s, %s\}, \{%s, %s, %s, %s\}, \{%s, %s, %s\}, (\w+), (\d+)\}' % ((cls,) + (num,) * 10), hdr)
    v = [float(m.group(k)) for k in range(1, 11)]
    table = m.group(11)
    blk = hdr[hdr.index(table + "[]"):]
    blk = blk[:blk.index("};")]
    rows = {int(r.group(1)): ([float(r.group(k)) for k in (2, 3, 4)], [float(r.group(k)) for k in (5, 6, 7, 8)])
            for r in re.finditer(r"\{(\d+), \{%s, %s, %s\}, \{%s, %s, %s, %s\}\}" % ((num,) * 7), blk)}
    return v[0:3], v[3:7], v[7:10], rows


scn = bpy.context.scene
arms = [o for o in scn.objects if o.type == "ARMATURE"]
ga = next(a for a in arms if "X_Lever" in a.pose.bones or "R_Grip" in a.pose.bones or "SG_Body" in a.pose.bones)
ha = next(a for a in arms if "Bip01_L_Hand" in a.pose.bones)
gmesh = max((o for o in scn.objects if o.type == "MESH" and o.find_armature() == ga), key=lambda o: len(o.data.vertices))
hmesh = max((o for o in scn.objects if o.type == "MESH" and o.find_armature() == ha), key=lambda o: len(o.data.vertices))
HC, _ = bone_turns(ha, os.path.join(ws, "exports", "bs1", "NEWPlayerHands-all.glb"))
Fm = Matrix.Diagonal((1.0, -1.0, 1.0))
FINGERS = ["Thumb", "Index", "Middle", "Ring", "Pinky"]
order = ["kBone_L_%s%d" % (f, k) for f in FINGERS for k in (1, 2, 3)]


def ue_rot(q):
    x, y, z, w = q
    return Fm @ Quaternion((w, x, y, z)).to_matrix() @ Fm


def ue_vec(v):
    return Vector((v[0], -v[1], v[2]))


def upd():
    bpy.context.view_layer.update()


def verts(obj):
    deps = bpy.context.evaluated_depsgraph_get()
    ev = obj.evaluated_get(deps)
    me = ev.to_mesh()
    pts = [ev.matrix_world @ v.co for v in me.vertices]
    polys = [tuple(p.vertices) for p in me.polygons]
    ev.to_mesh_clear()
    return pts, polys


hidx = {g.index: g.name for g in hmesh.vertex_groups}
seg_verts = {}
for v in hmesh.data.vertices:
    b = max(v.groups, key=lambda g: g.weight, default=None)
    if b:
        seg_verts.setdefault(hidx.get(b.group), []).append(v.index)
palm_idx = seg_verts.get("Bip01_L_Hand", []) + [i for f in ("Index", "Middle", "Ring", "Pinky") for i in seg_verts.get("kBone_L_%s1" % f, [])]

ACT = {a.name: (a.animation_data.action if a.animation_data else None) for a in (ha, ga)}
for a in (ha, ga):
    if a.animation_data:
        a.animation_data.action = ACT[a.name]
scn.frame_set(args.frame)
upd()
for a in (ha, ga):
    if a.animation_data:
        a.animation_data.action = None
upd()
BASE = {n: (ha.pose.bones[n].location.copy(), ha.pose.bones[n].rotation_quaternion.copy(), ha.pose.bones[n].scale.copy())
        for n in ["Bip01_L_Hand", "Bip01_L_Forearm", "Bip01_L_UpperArm"] + order}
G = game_frame(ha, HC, "R_grip")
Gr = G.to_3x3().normalized()
gs = sum(G.to_scale()) / 3.0
gpts, gpolys = verts(gmesh)
bvh = BVHTree.FromPolygons(gpts, gpolys)


def palm_inside():
    hp, _ = verts(hmesh)
    d = -1e9
    for i in palm_idx:
        loc, nrm, _, dist = bvh.find_nearest(hp[i])
        if loc is None or dist > 3.0:
            continue
        sd = (hp[i] - loc).dot(nrm)
        d = max(d, -sd if sd < 0 else -dist)
    return d


tw, tq, tpalm, _ = rest_row(args.target)
t_palm_x = (Vector(tw) + ue_rot(tq) @ ue_vec(tpalm) * 1.0).x  # (Blender's x is Unreal's x)
scn.render.engine = "BLENDER_WORKBENCH"
scn.render.resolution_x, scn.render.resolution_y = 800, 600
scn.display.shading.light = "STUDIO"
scn.display.shading.color_type = "OBJECT"
cam = bpy.data.objects.get("CandCam") or bpy.data.objects.new("CandCam", bpy.data.cameras.new("CandCam"))
if cam.name not in scn.collection.objects:
    scn.collection.objects.link(cam)
scn.camera = cam
cam.data.angle = 0.9
barrel, up = Gr.col[0].normalized(), Gr.col[2].normalized()
right = barrel.cross(up).normalized()
S = args.size
for cls in args.candidates.split(","):
    for n, (l, r, s_) in BASE.items():
        pb = ha.pose.bones[n]
        pb.location, pb.rotation_quaternion, pb.scale = l, r, s_
    upd()
    wp, wq, palm, rows = rest_row(cls)
    # this candidate's palm moved along the barrel to the target gun's own station
    pv = ue_vec(wp) + ue_rot(wq) @ ue_vec(palm)
    shift = (t_palm_x - pv.x) + args.along
    W = Matrix.LocRotScale(G @ (ue_vec(wp) + Vector((shift, 0, 0))), (Gr @ ue_rot(wq)).to_quaternion(), Vector((gs,) * 3))
    set_game_frame(ha, HC, "Bip01_L_Hand", W)
    upd()
    Wr = W.to_3x3().normalized()
    for k, n in enumerate(order):
        p, q = rows[7 + k]
        set_game_frame(ha, HC, n, Matrix.LocRotScale(W.to_translation() + Wr @ ue_vec(p) * gs, (Wr @ ue_rot(q)).to_quaternion(),
                                                       Vector((gs,) * 3)))
        upd()
    # Onto the gun's UNDERSIDE: clear below it first (a hand from a gun with a higher fore-end starts inside this one),
    # then up along the gun's up until the palm's skin meets the skin.
    def nudge(d):
        m = ha.matrix_world @ ha.pose.bones["Bip01_L_Hand"].matrix
        ha.pose.bones["Bip01_L_Hand"].matrix = ha.matrix_world.inverted() @ Matrix.Translation(d) @ m
        upd()
    moved = 0.0
    for _ in range(120):
        if palm_inside() <= args.sink:
            break
        nudge(-up * 0.5)
        moved -= 0.5
    nudge(-up * 4.0)
    moved -= 4.0
    for _ in range(120):
        nudge(up * 0.25)
        moved += 0.25
        if palm_inside() > args.sink:
            nudge(-up * 0.25)
            moved -= 0.25
            break
    print("BVR_CAND %s: moved %.1f along the barrel, %.2f onto the skin" % (cls, shift, moved))
    hp, _ = verts(hmesh)
    focus = sum((hp[i] for i in palm_idx), Vector()) / len(palm_idx)
    VIEWS = {"side": right * S * 1.3 + up * S * 0.2, "left": -right * S * 1.3 + up * S * 0.2,
             "under": -up * S * 1.3 - barrel * S * 0.2, "back": -barrel * S * 1.2 + up * S * 0.5 - right * S * 0.3,
             "shooter": -barrel * S * 1.5 + up * S * 0.9 + right * S * 0.2}
    for v in args.views.split(","):
        cam.location = focus + VIEWS[v]
        z = -(focus - cam.location).normalized()
        upv = barrel if v == "under" else up
        x = upv.cross(z).normalized()
        y = z.cross(x)
        cam.matrix_world = Matrix.Translation(cam.location) @ Matrix((x, y, z)).transposed().to_4x4()
        scn.render.filepath = place("%s-%s-%s.png" % (args.out, cls, v))
        bpy.ops.render.render(write_still=True)
print("BVR_CAND done")
