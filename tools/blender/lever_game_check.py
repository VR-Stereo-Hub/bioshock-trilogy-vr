# lever_game_check.py - the crossbow lever hold AS THE MOD COMPUTES IT, drawn on a replayed recording: the numbers in
# src\game\bioshock1r\grip_crossbow_lever.h put through bones.cpp's placement (the free wrist at kWristQ against the
# drawn grip bone 43 and kWristOff from the lever's NUB; each finger bone at its row against the wrist), so the header
# and the code's frames can be checked against the approved mock-up before a run.
#
#   .\tools\blender-run.ps1 tools\blender\lever_game_check.py -Blend <ws>\verification\<scene>.blend --
#       --frames 60,64,70,78,92 --out <prefix> [--views side,back,eyez]
#
# Renders into the model workspace's verification folder. Never launches a game.
import bpy, sys, os, re, math, argparse

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mathutils import Matrix, Quaternion, Vector  # noqa: E402
from game_frames import bone_turns, game_frame, set_game_frame  # noqa: E402

argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
ap = argparse.ArgumentParser(prog="lever_game_check.py")
ap.add_argument("--frames", required=True)
ap.add_argument("--out", required=True)
ap.add_argument("--views", default="side,back,eyez")
ap.add_argument("--size", type=float, default=30.0)
args = ap.parse_args(argv)
ws = os.environ.get("BVR_MODEL_WS", os.getcwd())
place = lambda p: p if os.path.isabs(p) else os.path.join(ws, "verification", p)
here = os.path.dirname(os.path.abspath(__file__))

hdr = open(os.path.join(here, "..", "..", "src", "game", "bioshock1r", "grip_crossbow_lever.h")).read()
num = r"([-\d.]+)f"
KQ = [float(x) for x in re.search(r"kWristQ\[4\] = \{%s, %s, %s, %s\}" % (num, num, num, num), hdr).groups()]
KO = [float(x) for x in re.search(r"kWristOff\[3\] = \{%s, %s, %s\}" % (num, num, num), hdr).groups()]
KS = float(re.search(r"kHandSize = %s" % num, hdr).group(1))
ROWS = {int(m.group(1)): ([float(m.group(k)) for k in (2, 3, 4)], [float(m.group(k)) for k in (5, 6, 7, 8)])
        for m in re.finditer(r"\{(\d+), \{%s, %s, %s\}, \{%s, %s, %s, %s\}\}" % ((num,) * 7), hdr)}
print("BVR_GAMECHECK header: wrist q %s off %s, %d finger rows" % (KQ, KO, len(ROWS)))

scn = bpy.context.scene
arms = [o for o in scn.objects if o.type == "ARMATURE"]
ga = next(a for a in arms if "X_Lever" in a.pose.bones)
ha = next(a for a in arms if "Bip01_L_Hand" in a.pose.bones)
HC, _ = bone_turns(ha, os.path.join(ws, "exports", "bs1", "NEWPlayerHands-all.glb"))
Fm = Matrix.Diagonal((1.0, -1.0, 1.0))
FINGERS = ["Thumb", "Index", "Middle", "Ring", "Pinky"]
order = ["kBone_L_%s%d" % (f, k) for f in FINGERS for k in (1, 2, 3)]


def ue_rot(q):  # an Unreal x y z w quaternion as a Blender-axes 3x3
    x, y, z, w = q
    return Fm @ Quaternion((w, x, y, z)).to_matrix() @ Fm


def ue_vec(v):
    return Vector((v[0], -v[1], v[2]))


def upd():
    bpy.context.view_layer.update()


def head(arm, n):
    return (arm.matrix_world @ arm.pose.bones[n].matrix).to_translation()


def two_bone_ik(target):
    S, E, W = head(ha, "Bip01_L_UpperArm"), head(ha, "Bip01_L_Forearm"), head(ha, "Bip01_L_Hand")
    l1, l2 = (E - S).length, (W - E).length
    d = target - S
    dist = max(1e-4, min(d.length, l1 + l2 - 1e-3))
    dn = d.normalized()
    pole = (E - S) - dn * (E - S).dot(dn)
    pole = pole.normalized() if pole.length > 1e-6 else dn.orthogonal().normalized()
    a = (l1 * l1 + dist * dist - l2 * l2) / (2 * dist)
    E2 = S + dn * a + pole * math.sqrt(max(0.0, l1 * l1 - a * a))
    for name, pivot, frm, to in (("Bip01_L_UpperArm", S, E, E2),):
        q = (frm - pivot).normalized().rotation_difference((to - pivot).normalized())
        pb = ha.pose.bones[name]
        pb.matrix = ha.matrix_world.inverted() @ (Matrix.Translation(pivot) @ q.to_matrix().to_4x4() @
                                                  Matrix.Translation(-pivot) @ ha.matrix_world @ pb.matrix)
        upd()
    E, W = head(ha, "Bip01_L_Forearm"), head(ha, "Bip01_L_Hand")
    q = (W - E).normalized().rotation_difference((target - E).normalized())
    pb = ha.pose.bones["Bip01_L_Forearm"]
    pb.matrix = ha.matrix_world.inverted() @ (Matrix.Translation(E) @ q.to_matrix().to_4x4() @ Matrix.Translation(-E) @
                                              ha.matrix_world @ pb.matrix)
    upd()


def apply_hold():
    G = game_frame(ha, HC, "R_grip")
    Gr = G.to_3x3().normalized()
    gs = sum(G.to_scale()) / 3.0
    nub = head(ga, "X_LeverNUB")
    W0 = game_frame(ha, HC, "Bip01_L_Hand")
    wsc = sum(W0.to_scale()) / 3.0
    Wr = Gr @ ue_rot(KQ)
    Wp = nub + Gr @ ue_vec(KO) * (wsc / KS)
    Wm = Matrix.LocRotScale(Wp, Wr.to_quaternion(), Vector((wsc, wsc, wsc)))
    two_bone_ik(Wp)
    # The wrist and every finger set as GAME frames, parents first (each set re-evaluates its children).
    set_game_frame(ha, HC, "Bip01_L_Hand", Wm)
    upd()
    for n in order:
        p, q = ROWS[7 + order.index(n)]
        Bm = Matrix.LocRotScale(Wp + Wr @ ue_vec(p) * wsc, (Wr @ ue_rot(q)).to_quaternion(), Vector((wsc, wsc, wsc)))
        set_game_frame(ha, HC, n, Bm)
        upd()
    err = (game_frame(ha, HC, "Bip01_L_Hand").to_translation() - Wp).length
    return err


scn.render.engine = "BLENDER_WORKBENCH"
scn.render.resolution_x, scn.render.resolution_y = 800, 600
scn.display.shading.light = "STUDIO"
scn.display.shading.color_type = "OBJECT"
cam = bpy.data.objects.get("ViewCam") or bpy.data.objects.new("ViewCam", bpy.data.cameras.new("ViewCam"))
if cam.name not in scn.collection.objects:
    scn.collection.objects.link(cam)
scn.camera = cam
S = args.size
OFFS = {"side": Vector((0, S, S * 0.15)), "back": Vector((-S * 1.2, S * 0.3, S * 0.4)), "top": Vector((-S * 0.2, 0, S * 1.3))}
ACT = {a.name: (a.animation_data.action if a.animation_data else None) for a in (ha, ga)}
for fr in (int(x) for x in args.frames.split(",")):
    for a in (ha, ga):
        if a.animation_data:
            a.animation_data.action = ACT[a.name]
    scn.frame_set(fr)
    upd()
    for a in (ha, ga):
        if a.animation_data:
            a.animation_data.action = None
    upd()
    err = apply_hold()
    print("BVR_GAMECHECK f%03d: the wrist placed %.3f from where the header puts it" % (fr, err))
    focus = head(ga, "X_LeverNUB")
    for v in args.views.split(","):
        if v == "eyez":
            cam.data.angle = 0.55
            cam.location = Vector((0, 0, 0))
        else:
            cam.data.angle = 0.9
            cam.location = focus + OFFS[v]
        cam.rotation_euler = (focus - cam.location).to_track_quat("-Z", "Y").to_euler()
        scn.render.filepath = place("%s-f%03d-game-%s.png" % (args.out, fr, v))
        bpy.ops.render.render(write_still=True)
print("BVR_GAMECHECK done")
