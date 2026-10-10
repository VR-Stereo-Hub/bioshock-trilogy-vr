# arm_ik_audit.py - render BS1's arms and the Dishonored VR mod's arms side by side, each
# posed by its own game's solver for the SAME body-relative pose (tools\arm-ik-audit.cpp),
# so "do they align" is answered by looking as well as by the numbers.
#
#   .\tools\blender-run.ps1 tools\blender\arm_ik_audit.py -- --audit <audit.json>
#       --bs1-glb <NEWPlayerHands.glb> --dh-rig <dishonored_vr_arm_rig.bin> --dh-psk <arms.psk>
#       [--render prefix] [--out report.json]
#
# Both meshes are skinned here with their original weights, moved into one shared frame
# (x right, y forward, z up, centred between the shoulders, ONE ARM LENGTH = 1 unit) and
# drawn BS1 on the left, Dishonored on the right. The Dishonored mesh takes its faces from
# the local PSK it was prepared from; its rig file must have the same point count.
import bpy, sys, os, json, struct, argparse
import numpy as np

argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
ap = argparse.ArgumentParser(prog="arm_ik_audit.py")
ap.add_argument("--audit", required=True)
ap.add_argument("--bs1-glb", required=True)
ap.add_argument("--dh-rig", required=True)
ap.add_argument("--dh-psk", required=True)
ap.add_argument("--render", default="arm-ik-audit")
ap.add_argument("--out", default="arm-ik-audit.json")
args = ap.parse_args(argv)
ws = os.environ.get("BVR_MODEL_WS", os.getcwd())
place = lambda p: p if os.path.isabs(p) else os.path.join(ws, "verification", p)
audit = json.load(open(args.audit))

# ---- BS1: the glb ---------------------------------------------------------------
blob = open(args.bs1_glb, "rb").read()
jl = struct.unpack_from("<I", blob, 12)[0]
doc = json.loads(blob[20:20 + jl])
gbin = blob[20 + jl + 8:]


def acc(i):
    a = doc["accessors"][i]
    bv = doc["bufferViews"][a["bufferView"]]
    dt = {5121: np.uint8, 5123: np.uint16, 5125: np.uint32, 5126: np.float32}[a["componentType"]]
    n = {"SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4, "MAT4": 16}[a["type"]]
    arr = np.frombuffer(gbin, dtype=dt, count=a["count"] * n, offset=bv.get("byteOffset", 0) + a.get("byteOffset", 0))
    return arr.reshape(a["count"], n) if n > 1 else arr


prim = doc["meshes"][0]["primitives"][0]
bsP = acc(prim["attributes"]["POSITION"]).astype(np.float64)
bsJ = acc(prim["attributes"]["JOINTS_0"]).astype(np.int64)
bsW = acc(prim["attributes"]["WEIGHTS_0"]).astype(np.float64)
bsW /= np.maximum(bsW.sum(1, keepdims=True), 1e-9)
bsF = acc(prim["indices"]).astype(np.int64).reshape(-1, 3)
IBM = acc(doc["skins"][0]["inverseBindMatrices"]).astype(np.float64).reshape(-1, 4, 4).transpose(0, 2, 1)
C = np.array([[1, 0, 0, 0], [0, 0, 1, 0], [0, 1, 0, 0], [0, 0, 0, 1]], dtype=np.float64)


def ue_m(b):
    px, py, pz, qx, qy, qz, qw, sx, sy, sz = b
    r = np.array([[1 - 2 * (qy * qy + qz * qz), 2 * (qx * qy - qz * qw), 2 * (qx * qz + qy * qw)],
                  [2 * (qx * qy + qz * qw), 1 - 2 * (qx * qx + qz * qz), 2 * (qy * qz - qx * qw)],
                  [2 * (qx * qz - qy * qw), 2 * (qy * qz + qx * qw), 1 - 2 * (qx * qx + qy * qy)]])
    m = np.eye(4)
    m[:3, :3] = r * np.array([sx, sy, sz])
    m[:3, 3] = (px, py, pz)
    return m


def skin_bs(pose):
    M = np.stack([C @ ue_m(b) @ C @ IBM[i] for i, b in enumerate(pose)])
    Ph = np.c_[bsP, np.ones(len(bsP))]
    out = np.zeros_like(bsP)
    for k in range(4):
        out += bsW[:, k:k + 1] * np.einsum("nij,nj->ni", M[bsJ[:, k]], Ph)[:, :3]
    return out[:, [0, 2, 1]]  # glTF -> Unreal: the arm in its own (Havok) space


# ---- Dishonored: the prepared rig + the PSK's faces -------------------------------
rb = open(args.dh_rig, "rb").read()
if rb[:8] != b"DVRIK002":
    raise SystemExit("*** not a DVRIK002 rig")
nb, nv, nt = struct.unpack_from("<3I", rb, 8)
heads = {}
for i in range(nb):
    o = 20 + 80 * i
    name = rb[o:o + 64].split(b"\0")[0].decode()
    heads[name] = np.array(struct.unpack_from("<3f", rb, o + 68), dtype=np.float64)
vo = 20 + 80 * nb
dhV = np.zeros((nv, 3)); dhB = np.zeros((nv, 4), np.int64); dhWt = np.zeros((nv, 4))
for i in range(nv):
    x = struct.unpack_from("<3f4i4f", rb, vo + 44 * i)
    dhV[i] = x[0:3]; dhB[i] = x[3:7]; dhWt[i] = x[7:11]
psk = open(args.dh_psk, "rb").read()
o, chunks = 0, {}
while o < len(psk):
    name, _, size, count = struct.unpack_from("<20siii", psk, o)
    o += 32
    chunks[name.rstrip(b"\0")] = (size, count, psk[o:o + size * count])
    o += size * count
if chunks[b"PNTS0000"][1] != nv:
    raise SystemExit("*** the PSK has %d points, the rig %d - not the mesh it was prepared from" % (chunks[b"PNTS0000"][1], nv))
wsz, wn, wraw = chunks[b"VTXW0000"]
wedge_point = [struct.unpack_from("<H", wraw, wsz * i)[0] for i in range(wn)]
fsz, fn, fraw = chunks[b"FACE0000"]
dhFaces = [tuple(wedge_point[w] for w in struct.unpack_from("<3H", fraw, fsz * i)) for i in range(fn)]


def skin_dh(xf):
    X = np.array(xf, dtype=np.float64)
    R, T = X[:, :9].reshape(-1, 3, 3), X[:, 9:12]
    out = np.zeros_like(dhV)
    for k in range(4):
        b, w = dhB[:, k], dhWt[:, k:k + 1]
        live = b >= 0
        bb = np.where(live, b, 0)
        out += np.where(live[:, None], w * (np.einsum("nij,nj->ni", R[bb], dhV) + T[bb]), 0)
    return out


# ---- one shared frame: x right, y forward, z up; centred between the shoulders; 1 = arm length
def frame_of(F, Rt, U, sL, sR, eL, wL):
    c = (sL + sR) / 2
    L = np.linalg.norm(eL - sL) + np.linalg.norm(wL - eL)
    M = np.array([Rt, F, U], dtype=np.float64)
    return lambda V: ((V - c) @ M.T) / L


bsNames = audit["bs1Bones"]
# Each bone's bind position in Unreal (Havok) space: the inverse bind, back through C.
bsRef = {n: (C @ np.linalg.inv(IBM[i]) @ C)[:3, 3] for i, n in enumerate(bsNames)}
to_bs = frame_of(np.array([0, 0, -1.0]), np.array([0, 1.0, 0]), np.array([1.0, 0, 0]),
                 bsRef["Bip01_L_UpperArm"], bsRef["Bip01_R_UpperArm"], bsRef["Bip01_L_Forearm"], bsRef["Bip01_L_Hand"])
to_dh = frame_of(np.array([0, 0, 1.0]), np.array([-1.0, 0, 0]), np.array([0, -1.0, 0]),
                 heads["upper_arm_L_jnt"], heads["upper_arm_R_jnt"], heads["lower_arm_L_jnt"], heads["hand_L_jnt"])

# ---- render the key poses ---------------------------------------------------------
for ob in list(bpy.data.objects):
    bpy.data.objects.remove(ob, do_unlink=True)
scn = bpy.context.scene
scn.render.engine = "BLENDER_WORKBENCH"
scn.render.resolution_x, scn.render.resolution_y = 1100, 600
scn.display.shading.light = "STUDIO"
scn.display.shading.color_type = "OBJECT"
cam = bpy.data.objects.new("AuditCam", bpy.data.cameras.new("AuditCam"))
cam.data.type = "ORTHO"
cam.data.ortho_scale = 5.2
scn.collection.objects.link(cam)
scn.camera = cam
from mathutils import Vector
SEP = 1.4  # half the distance between the two figures, in arm lengths
renders = []
for fr in audit["frames"]:
    if not fr["key"]:
        continue
    objs = []
    for label, V, faces, dx, colour in (("BS1", to_bs(skin_bs(fr["bs1"])), bsF.tolist(), -SEP, (0.55, 0.7, 0.95, 1)),
                                        ("Dishonored", to_dh(skin_dh(fr["dh"])), dhFaces, SEP, (0.95, 0.7, 0.5, 1))):
        me = bpy.data.meshes.new(label)
        me.from_pydata([(float(v[0]) + dx, float(v[1]), float(v[2])) for v in V], [], [tuple(int(i) for i in f) for f in faces])
        ob = bpy.data.objects.new(label, me)
        ob.color = colour
        scn.collection.objects.link(ob)
        objs.append((ob, me))
    tag = "f%03d-%s" % (fr["frame"], "".join(ch if ch.isalnum() else "_" for ch in fr["label"]))
    for view, loc in (("front", Vector((0, 6, 0.1))), ("side", Vector((6, 0.6, 0.4))), ("top", Vector((0, 0.6, 6)))):
        cam.location = loc
        target = Vector((0, 0.6 if view != "front" else 0.0, -0.1))
        cam.rotation_euler = (target - cam.location).to_track_quat("-Z", "Y" if view != "top" else "Y").to_euler()
        scn.render.filepath = place("%s-%s-%s.png" % (args.render, tag, view))
        bpy.ops.render.render(write_still=True)
        renders.append(scn.render.filepath)
    for ob, me in objs:
        bpy.data.objects.remove(ob, do_unlink=True)
        bpy.data.meshes.remove(me)

rep = {"audit": os.path.abspath(args.audit), "summary": audit["summary"], "renders": renders}
out = place(args.out)
json.dump(rep, open(out, "w"), indent=1)
s = audit["summary"]
print("BVR_IKAUDIT %d key poses rendered (BS1 left, Dishonored right); solver agreement: elbow direction %.2f deg, "
      "shoulder slide %.4f, tracked roll %.2f deg -> %s" % (len(renders) // 3, s["worstElbowDirectionDeg"],
                                                            s["worstShoulderShiftDiff"], s["worstTrackedTwistDiffDeg"], out))
