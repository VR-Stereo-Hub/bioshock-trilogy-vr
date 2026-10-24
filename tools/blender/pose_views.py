# pose_views.py - render chosen frames of a pose_capture.py scene from fixed views, for comparing the replay with
# what the headset showed (the F10 animation snapshots) and for grip mock-ups.
#
#   .\tools\blender-run.ps1 tools\blender\pose_views.py -Blend <ws>\verification\<scene>.blend --
#       --frames 0,63,75 --out <prefix> [--focus X_LeverNUB] [--mark X_Lever,X_LeverNUB] [--size 30]
#       [--views eye,side,top,front]
#
# eye: from the hands actor's origin (the player's eye in first person) toward the focus, a 70 deg lens - the
# headset's view of the frame, near enough to put beside a snapshot. side/top/front: close views round the focus.
# --mark puts a small coloured ball on each named gun bone (where the lever's pivot and tip are). Renders land in
# the model workspace's verification folder. Never launches a game.
import bpy, sys, os, argparse, math

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mathutils import Vector  # noqa: E402

argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
ap = argparse.ArgumentParser(prog="pose_views.py")
ap.add_argument("--frames", required=True)
ap.add_argument("--out", required=True)
ap.add_argument("--focus", default="X_LeverNUB")
ap.add_argument("--mark", default="")
ap.add_argument("--size", type=float, default=30.0)
ap.add_argument("--views", default="eye,side,top,front")
ap.add_argument("--res", default="800x600")
ap.add_argument("--hmd-fov", type=float, default=100.0)
args = ap.parse_args(argv)
ws = os.environ.get("BVR_MODEL_WS", os.getcwd())
place = lambda p: p if os.path.isabs(p) else os.path.join(ws, "verification", p)

scn = bpy.context.scene
arms = [o for o in scn.objects if o.type == "ARMATURE"]
gun = next((a for a in arms if args.focus in a.pose.bones), None)
COLORS = [(1.0, 0.1, 0.1, 1), (0.1, 0.4, 1.0, 1), (0.1, 0.9, 0.2, 1), (1.0, 0.9, 0.1, 1)]


def bone_head(name):
    for a in arms:
        pb = a.pose.bones.get(name)
        if pb:
            return a.matrix_world @ pb.head
    return None


marks = []
for i, name in enumerate(n for n in args.mark.split(",") if n):
    bpy.ops.mesh.primitive_uv_sphere_add(radius=0.8)
    s = bpy.context.active_object
    s.name = "mark_" + name
    s.color = COLORS[i % len(COLORS)]
    marks.append((name, s))

w, h = (int(v) for v in args.res.split("x"))
scn.render.engine = "BLENDER_WORKBENCH"
scn.render.resolution_x, scn.render.resolution_y = w, h
scn.display.shading.light = "STUDIO"
scn.display.shading.color_type = "OBJECT"
scn.display.shading.show_xray = False
cam = bpy.data.objects.get("ViewCam") or bpy.data.objects.new("ViewCam", bpy.data.cameras.new("ViewCam"))
if cam.name not in scn.collection.objects:
    scn.collection.objects.link(cam)
scn.camera = cam
S = args.size
# Blender axes of the hands' component space (game_frames.ue_local: Unreal x forward, y right, z up -> y flipped).
OFFS = {"side": Vector((0, S, S * 0.15)), "top": Vector((-S * 0.2, 0, S * 1.3)), "front": Vector((S * 1.3, 0, S * 0.2)),
        "left": Vector((0, -S, S * 0.15)), "under": Vector((S * 0.3, 0, -S * 1.2))}
for fr in (int(x) for x in args.frames.split(",")):
    scn.frame_set(fr)
    bpy.context.view_layer.update()
    for name, s in marks:
        p = bone_head(name)
        if p is not None:
            s.location = p
    focus = bone_head(args.focus) or Vector()
    for view in args.views.split(","):
        if view == "eye":
            cam.data.lens_unit, cam.data.angle = "FOV", 1.22
            cam.location = Vector((0, 0, 0))
        elif view == "hmd":
            # The headset's own view: the hands actor rides the camera, so its origin looking down its +x (Unreal
            # forward), +z up, is what the eye sees - not aimed at the focus. A wide lens like the snapshots'.
            cam.data.lens_unit, cam.data.angle = "FOV", math.radians(args.hmd_fov)
            cam.location = Vector((0, 0, 0))
            cam.rotation_euler = Vector((1, 0, 0)).to_track_quat("-Z", "Y").to_euler()
            scn.render.filepath = place("%s-f%03d-%s.png" % (args.out, fr, view))
            bpy.ops.render.render(write_still=True)
            print("BVR_VIEWS %s" % scn.render.filepath)
            continue
        else:
            cam.data.lens_unit, cam.data.angle = "FOV", 0.9
            cam.location = focus + OFFS[view]
        cam.rotation_euler = (focus - cam.location).to_track_quat("-Z", "Y").to_euler()
        scn.render.filepath = place("%s-f%03d-%s.png" % (args.out, fr, view))
        bpy.ops.render.render(write_still=True)
        print("BVR_VIEWS %s" % scn.render.filepath)
