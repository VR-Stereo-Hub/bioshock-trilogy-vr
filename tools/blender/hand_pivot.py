# hand_pivot.py - WHERE SHOULD THE HAND TURN ABOUT? (s89)
#
# A tracked controller's GRIP pose has its origin at the centre of the grasped handle, its -Z
# along the handle from the little finger to the thumb side, and +X along the palm normal
# (OpenXR, /input/grip/pose). For the drawn hand to turn exactly as the real one, the point the
# drive pins to the controller must be the centre of the VIRTUAL fist - where a handle passes
# through the hand - and the hand's handle axis and palm normal must line up with the grip's.
#
# This script finds that point on the BS1 hands rig, in a settled wrench grip:
#   - each finger's curl centre: a least-squares circle through its three joints and its tip;
#   - the handle axis: the line through the index/middle/ring/pinky curl centres (pinky -> index);
#   - the fist centre: their mean, on that axis; the handle radius: the mean circle radius;
#   - the palm normal: from the palm's metacarpal plane toward the fist centre, square to the axis.
# It compares it with the drive's current anchor (the mean of the hand and the four finger bases)
# and with R_grip (bone 43, the weapon attach), and prints every point and direction in a frame
# made from bone HEADS only (origin the wrist; x toward the middle finger base; y from the little
# finger base toward the index base; z = x cross y, times +1 right / -1 left), so the mod can rebuild
# the same frame from the evaluated pose without any Blender axis convention in between.
#
#   .\tools\blender-run.ps1 tools\blender\hand_pivot.py -- --gltf <NEWPlayerHands-all.glb>
#       [--action Wrench__EquipWrench] [--frame last] [--out hand-pivot.json] [--render hand-pivot]
import bpy, sys, os, json, argparse
import numpy as np
from mathutils import Vector

argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
ap = argparse.ArgumentParser(prog="hand_pivot.py")
ap.add_argument("--gltf", required=True)
ap.add_argument("--action", default="Wrench__EquipWrench")
ap.add_argument("--frame", default="last")
ap.add_argument("--out", default="hand-pivot.json")
ap.add_argument("--render", default="hand-pivot")
args = ap.parse_args(argv)
ws = os.environ.get("BVR_MODEL_WS", os.getcwd())
place = lambda p: p if (not p or os.path.isabs(p)) else os.path.join(ws, "verification", p)

for o in list(bpy.data.objects):
    bpy.data.objects.remove(o, do_unlink=True)
for a in list(bpy.data.actions):
    bpy.data.actions.remove(a)
bpy.ops.import_scene.gltf(filepath=os.path.abspath(args.gltf))
scn = bpy.context.scene
arm = [o for o in scn.objects if o.type == "ARMATURE"][0]
shapes = {pb.custom_shape for pb in arm.pose.bones if pb.custom_shape}
meshes = [o for o in scn.objects if o.type == "MESH" and o not in shapes]
for s in shapes:
    s.hide_render = True
act = [a for a in bpy.data.actions if a.name.startswith(args.action)][0]
arm.animation_data_create()
arm.animation_data.action = act
if hasattr(arm.animation_data, "action_slot") and act.slots:
    arm.animation_data.action_slot = act.slots[0]
f0, f1 = act.frame_range
frame = int(f1) if args.frame == "last" else int(args.frame)
scn.frame_set(frame)
bpy.context.view_layer.update()

W = arm.matrix_world
head = lambda n: np.array(W @ arm.pose.bones[n].head)
tail = lambda n: np.array(W @ arm.pose.bones[n].tail)


def fit_circle(pts):
    """least-squares circle through 3D points: plane by PCA, then the Kasa fit in that plane"""
    P = np.array(pts)
    c0 = P.mean(axis=0)
    _, _, vt = np.linalg.svd(P - c0)
    e1, e2 = vt[0], vt[1]
    xy = np.stack([(P - c0) @ e1, (P - c0) @ e2], axis=1)
    A = np.column_stack([2 * xy[:, 0], 2 * xy[:, 1], np.ones(len(xy))])
    b = (xy ** 2).sum(axis=1)
    sol, *_ = np.linalg.lstsq(A, b, rcond=None)
    cx, cy, k = sol
    r = np.sqrt(k + cx * cx + cy * cy)
    return c0 + cx * e1 + cy * e2, float(r)


def analyse(side):
    S = "R" if side > 0 else "L"
    wrist = head("Bip01_%s_Hand" % S)
    bases = {f: head("kBone_%s_%s1" % (S, f)) for f in ("Index", "Middle", "Ring", "Pinky")}
    x = bases["Middle"] - wrist
    x /= np.linalg.norm(x)
    y = bases["Index"] - bases["Pinky"]
    y -= (y @ x) * x
    y /= np.linalg.norm(y)
    z = np.cross(x, y) * side
    to_frame = lambda p: [float((p - wrist) @ x), float((p - wrist) @ y), float((p - wrist) @ z)]
    to_dir = lambda d: [float(d @ x), float(d @ y), float(d @ z)]
    centres, radii = {}, {}
    for f in ("Index", "Middle", "Ring", "Pinky"):
        pts = [head("kBone_%s_%s%d" % (S, f, k)) for k in (1, 2, 3)] + [tail("kBone_%s_%s3" % (S, f))]
        c, r = fit_circle(pts)
        centres[f], radii[f] = c, r
    C = np.array([centres[f] for f in ("Index", "Middle", "Ring", "Pinky")])
    fist = C.mean(axis=0)
    _, _, vt = np.linalg.svd(C - fist)
    axis = vt[0]
    if axis @ (centres["Index"] - centres["Pinky"]) < 0:
        axis = -axis
    meta = (wrist + sum(bases.values())) / 5.0        # the drive's current anchor (s86f)
    n = fist - meta
    n -= (n @ axis) * axis
    n /= np.linalg.norm(n)
    grip = head("R_grip") if side > 0 else None
    res = {
        "side": S,
        "fist_centre": to_frame(fist),
        "handle_axis_pinky_to_index": to_dir(axis),
        "palm_normal_out": to_dir(n),
        "handle_radius": float(np.mean(list(radii.values()))),
        "finger_radii": {f: radii[f] for f in radii},
        "current_anchor": to_frame(meta),
        "anchor_to_fist": float(np.linalg.norm(fist - meta)),
        "frame_scale_wrist_to_middle_base": float(np.linalg.norm(bases["Middle"] - wrist)),
        "world": {"fist": fist.tolist(), "anchor": meta.tolist(), "axis": axis.tolist(), "normal": n.tolist()},
    }
    if grip is not None:
        res["R_grip"] = to_frame(grip)
        res["R_grip_to_fist"] = float(np.linalg.norm(fist - grip))
        res["world"]["R_grip"] = grip.tolist()
    return res


R = {"gltf": os.path.abspath(args.gltf), "action": act.name, "frame": frame, "range": [f0, f1],
     "right": analyse(+1), "left": analyse(-1)}

# ---- markers and renders --------------------------------------------------------------------
def sphere(name, at, colour, r):
    bpy.ops.mesh.primitive_uv_sphere_add(radius=r, location=at)
    o = bpy.context.active_object
    o.name = name
    o.color = colour
    return o


def rod(name, a, b, colour, r):
    a, b = Vector(a), Vector(b)
    bpy.ops.mesh.primitive_cylinder_add(radius=r, depth=(b - a).length, location=(a + b) / 2)
    o = bpy.context.active_object
    o.name = name
    o.rotation_euler = (b - a).to_track_quat("Z", "Y").to_euler()
    o.color = colour
    return o


rr = R["right"]
u = rr["frame_scale_wrist_to_middle_base"]
fist, anchor, axis, nrm = (np.array(rr["world"][k]) for k in ("fist", "anchor", "axis", "normal"))
sphere("FistCentre_correct", fist, (0.1, 1.0, 0.2, 1), u * 0.08)
sphere("Anchor_current", anchor, (1.0, 0.15, 0.1, 1), u * 0.08)
sphere("R_grip_bone43", np.array(rr["world"]["R_grip"]), (0.2, 0.4, 1.0, 1), u * 0.06)
rod("HandleAxis", fist - axis * u * 0.9, fist + axis * u * 0.9, (1.0, 0.9, 0.1, 1), u * 0.025)
rod("PalmNormal", fist, fist + nrm * u * 0.6, (1.0, 0.3, 1.0, 1), u * 0.02)
for m in meshes:
    m.color = (0.75, 0.62, 0.55, 0.55)

scn.render.engine = "BLENDER_WORKBENCH"
scn.render.resolution_x, scn.render.resolution_y = 1000, 800
scn.display.shading.light = "STUDIO"
scn.display.shading.color_type = "OBJECT"
scn.display.shading.show_xray = True
scn.display.shading.xray_alpha = 0.45
renders = []
c = Vector(fist)
for name, d in (("palm", nrm), ("thumb", axis), ("back", -nrm), ("wrist", Vector(fist) - Vector(R["right"]["world"]["anchor"]))):
    d = Vector(d).normalized()
    cam = bpy.data.objects.get("PivotCam") or bpy.data.objects.new("PivotCam", bpy.data.cameras.new("PivotCam"))
    if cam.name not in scn.collection.objects:
        scn.collection.objects.link(cam)
    cam.location = c + d * u * 4.0
    cam.rotation_euler = (c - cam.location).to_track_quat("-Z", "Y").to_euler()
    scn.camera = cam
    scn.render.filepath = place("%s-%s.png" % (args.render, name))
    bpy.ops.render.render(write_still=True)
    renders.append(scn.render.filepath)
R["renders"] = renders

out = place(args.out)
os.makedirs(os.path.dirname(out), exist_ok=True)
with open(out, "w") as fh:
    json.dump(R, fh, indent=1)
for s in ("right", "left"):
    r = R[s]
    print("BVR_PIVOT %s: fist centre %s | handle axis %s | palm normal %s | radius %.2f | current anchor %s, "
          "%.2f units from the fist centre%s" % (s, ["%.2f" % v for v in r["fist_centre"]],
          ["%.3f" % v for v in r["handle_axis_pinky_to_index"]], ["%.3f" % v for v in r["palm_normal_out"]],
          r["handle_radius"], ["%.2f" % v for v in r["current_anchor"]], r["anchor_to_fist"],
          (" | R_grip %s, %.2f from the fist" % (["%.2f" % v for v in r["R_grip"]], r["R_grip_to_fist"])) if "R_grip" in r else ""))
print("BVR_PIVOT -> %s" % out)
