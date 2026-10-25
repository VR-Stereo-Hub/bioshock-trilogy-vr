# mirror_hold_mock.py - the left-handed two-handed hold: what a replayed frame draws, against the target - the game's
# own right-handed hold of the same gun mirrored about the gun's centre plane - from cameras fixed to the GUN, so the
# two compare whatever the hands were doing.
#
#   the frame as drawn (a pose_capture.py scene of a mirrored recording):
#   .\tools\blender-run.ps1 tools\blender\mirror_hold_mock.py -Blend <ws>\verification\<scene>.blend --
#       --frame 10 --out lh-now
#   the target (the clip, mirrored):
#   .\tools\blender-run.ps1 tools\blender\mirror_hold_mock.py -- --reference --gun <ws>\exports\bs1\WP_CrossbowMesh.glb
#       --hands-action Crossbow__FidgetCrossbow --time 0 --plane-u 0 1 0 --plane -1.654 --out lh-target
#
# The gun's frame is built from its bones' positions, so it is the same physical frame on a mirrored gun: forward
# from the grip to the bow's limbs' midpoint, up toward the lever's pivot, right = forward x up. The target mirrors
# the whole clip scene (hands and gun) about the plane that is --plane along --plane-u in the gun grip's game frame
# (the recording's mirrorplane line: the crossbow's -0.0173 0.9999 0 at -1.654). Renders into the model workspace's
# verification folder; the support palm's place in the gun's frame is printed (BVR_MIRROR lines).
import bpy, sys, os, re, math, argparse

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mathutils import Matrix, Vector  # noqa: E402
from game_frames import bone_turns, game_frame, ue_local  # noqa: E402

argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
ap = argparse.ArgumentParser(prog="mirror_hold_mock.py")
ap.add_argument("--frame", type=int, default=0)
ap.add_argument("--reference", action="store_true")
ap.add_argument("--hands", default="")
ap.add_argument("--gun", default="")
ap.add_argument("--hands-action", default="")
ap.add_argument("--time", type=float, default=0.0)
ap.add_argument("--plane-u", type=float, nargs=3, default=[0.0, 1.0, 0.0])
ap.add_argument("--plane", type=float, default=0.0)
ap.add_argument("--support", default="", help="the support hand, L or R (default: R on a mirrored frame, L on the clip)")
ap.add_argument("--out", required=True)
ap.add_argument("--size", type=float, default=40.0)
ap.add_argument("--views", default="shooter,right,left,under")
ap.add_argument("--close", action="store_true", help="frame the support hand, not the gun")
args = ap.parse_args(argv)
ws = os.environ.get("BVR_MODEL_WS", os.getcwd())
place = lambda p: p if os.path.isabs(p) else os.path.join(ws, "verification", p)
scn = bpy.context.scene


def load(path):
    before, acts0 = set(bpy.data.objects), set(bpy.data.actions)
    bpy.ops.import_scene.gltf(filepath=os.path.abspath(path))
    new = [o for o in bpy.data.objects if o not in before]
    arm = next(o for o in new if o.type == "ARMATURE")
    shapes = {pb.custom_shape for pb in arm.pose.bones if pb.custom_shape}
    for s in shapes:
        s.hide_render = True
    return arm, [o for o in new if o.type == "MESH" and o not in shapes], [a for a in bpy.data.actions if a not in acts0]


if args.reference:
    for o in list(bpy.data.objects):
        bpy.data.objects.remove(o, do_unlink=True)
    hands = args.hands or os.path.join(ws, "exports", "bs1", "NEWPlayerHands-all.glb")
    ha, hm, hacts = load(hands)
    ga, gm, _ = load(args.gun)
    act = next(a for a in hacts if re.search(args.hands_action, a.name, re.I))
    ha.animation_data_create()
    ha.animation_data.action = act
    HC, _ = bone_turns(ha, hands)
    c = ga.constraints.new("CHILD_OF")
    c.target, c.subtarget = ha, "R_grip"
    c.inverse_matrix = HC.get("R_grip", Matrix.Identity(4)).inverted()
    scn.render.fps = 30
    scn.frame_set(int(round(args.time * 30)))
    bpy.context.view_layer.update()
    # The mirror: about the plane through the grip's game frame, normal u (Unreal axes in that frame).
    G = game_frame(ha, HC, "R_grip")
    u_w = (G.to_3x3() @ ue_local(Vector(args.plane_u))).normalized()
    p0 = G @ ue_local(Vector(args.plane_u).normalized() * args.plane)
    Rf = Matrix.Identity(4) - 2 * Matrix(((u_w.x * u_w.x, u_w.x * u_w.y, u_w.x * u_w.z, 0),
                                          (u_w.y * u_w.x, u_w.y * u_w.y, u_w.y * u_w.z, 0),
                                          (u_w.z * u_w.x, u_w.z * u_w.y, u_w.z * u_w.z, 0), (0, 0, 0, 0)))
    Rf[3][3] = 1.0
    M = Matrix.Translation(p0) @ Rf @ Matrix.Translation(-p0)
    print("BVR_MIRROR mirror normal (world) %s through %s" % ([round(x, 3) for x in u_w], [round(x, 2) for x in p0]))
    ha.matrix_world = M @ ha.matrix_world
    bpy.context.view_layer.update()
    print("BVR_MIRROR hands object determinant now %.3f" % ha.matrix_world.to_3x3().determinant())
    support = args.support or "L"
    for m in gm:
        m.color = (0.85, 0.55, 0.2, 1.0)
else:
    arms = [o for o in scn.objects if o.type == "ARMATURE"]
    g0 = next(a for a in arms if "X_Lever" in a.pose.bones)
    ha = next(a for a in arms if "Bip01_L_Hand" in a.pose.bones)
    scn.frame_set(args.frame)
    bpy.context.view_layer.update()
    if ha.animation_data:
        ha.animation_data.action = None
    # The replay places the gun by its actor, which a mirrored frame does not move (pose_capture.py: the mirror is
    # not rebuilt); the game draws it on the hands' grip bone 43, which the mirror DOES move. So the gun is drawn
    # here as the game attaches it: a fresh crossbow on the hands' R_grip game frame, its bones at rest.
    for o in [o for o in scn.objects if o.find_armature() == g0 or o == g0]:
        o.hide_render = True
    hands = args.hands or os.path.join(ws, "exports", "bs1", "NEWPlayerHands-all.glb")
    HC, _ = bone_turns(ha, hands)
    ga, gm, _ = load(args.gun or os.path.join(ws, "exports", "bs1", "WP_CrossbowMesh.glb"))
    ga.animation_data_clear()
    c = ga.constraints.new("CHILD_OF")
    c.target, c.subtarget = ha, "R_grip"
    c.inverse_matrix = HC.get("R_grip", Matrix.Identity(4)).inverted()
    for m in gm:
        m.color = (0.85, 0.55, 0.2, 1.0)
    bpy.context.view_layer.update()
    G = game_frame(ha, HC, "R_grip")
    print("BVR_MIRROR the hands' grip bone: determinant %.3f (negative = a mirrored solid)" % G.to_3x3().determinant())
    support = args.support or "R"


def bh(arm, n):
    return (arm.matrix_world @ arm.pose.bones[n].matrix).to_translation()


grip = bh(ga, "R_grip")
limbs = (bh(ga, "X_Larm") + bh(ga, "X_Rarm")) / 2
fwd = (limbs - grip).normalized()
up = bh(ga, "X_Lever") - grip
up = (up - fwd * up.dot(fwd)).normalized()
right = fwd.cross(up)
GF = Matrix((fwd, right, up)).transposed()
# The support palm (wrist to middle base midpoint) and the gun hand's, in the gun's frame.
for tag, sd in (("support", support), ("gun hand", "L" if support == "R" else "R")):
    pw = bh(ha, "Bip01_%s_Hand" % sd)
    pm = ha.matrix_world @ ha.pose.bones["kBone_%s_Middle1" % sd].head
    palm = (pw + pm) / 2
    loc = GF.inverted() @ (palm - grip)
    print("BVR_MIRROR %s palm (%s) in the gun's frame: forward %.1f right %.1f up %.1f" % (tag, sd, loc.x, loc.y, loc.z))

scn.render.engine = "BLENDER_WORKBENCH"
scn.render.resolution_x, scn.render.resolution_y = 800, 600
scn.display.shading.light = "STUDIO"
scn.display.shading.color_type = "OBJECT"
scn.display.shading.show_backface_culling = False
cam = bpy.data.objects.get("MirCam") or bpy.data.objects.new("MirCam", bpy.data.cameras.new("MirCam"))
if cam.name not in scn.collection.objects:
    scn.collection.objects.link(cam)
scn.camera = cam
cam.data.angle = 0.9
S = args.size
# --close: around the support hand itself (its palm), not the gun.
if args.close:
    sw = bh(ha, "Bip01_%s_Hand" % support)
    sm = ha.matrix_world @ ha.pose.bones["kBone_%s_Middle1" % support].head
    focus = (sw + sm) / 2
else:
    focus = grip + fwd * 14.0
VIEWS = {"shooter": -fwd * S * 1.2 + up * S * 0.5, "right": right * S * 1.3 + up * S * 0.2,
         "left": -right * S * 1.3 + up * S * 0.2, "under": -up * S * 1.3 - fwd * S * 0.2, "top": up * S * 1.4 - fwd * S * 0.3}
for v in args.views.split(","):
    cam.location = focus + VIEWS[v]
    cam.rotation_euler = (focus - cam.location).to_track_quat("-Z", "Y").to_euler()
    # roll so the gun's up is the picture's up
    cam.rotation_euler = cam.rotation_euler
    m = cam.matrix_world.to_3x3()
    z = -(focus - cam.location).normalized()
    upv = (fwd if v in ("under", "top") else up)
    x = upv.cross(z).normalized()
    y = z.cross(x)
    cam.matrix_world = Matrix.Translation(cam.location) @ Matrix((x, y, z)).transposed().to_4x4()
    scn.render.filepath = place("%s-%s.png" % (args.out, v))
    bpy.ops.render.render(write_still=True)
print("BVR_MIRROR done")
