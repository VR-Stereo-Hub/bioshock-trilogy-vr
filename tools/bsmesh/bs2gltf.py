"""bs2gltf.py - convert a BioShock Remastered skeletal mesh, its Havok skeleton and its
Havok animation clips into one glTF 2.0 binary (.glb) that Blender imports directly.

  py tools\\bsmesh\\bs2gltf.py --bsm <0-Lighthouse.bsm> --classes <hkclasses.json> --mesh NEWPlayerHands --out hands.glb
  py tools\\bsmesh\\bs2gltf.py ... --anims "Pistol|Wrench"     # only clips whose name or group matches
  py tools\\bsmesh\\bs2gltf.py ... --anims none --lod 1
  py tools\\bsmesh\\bs2gltf.py --bsm <0-Lighthouse.bsm> --classes - --list    # skeletal meshes in a package

Normally run through tools\\bsmesh-export.ps1, which resolves the package and the class
layouts from the local tool file and writes into the model workspace.

The pieces, each derived from the game rather than guessed (docs/bioshock1/HAVOK_AND_PACKAGES.md):
  vpackage.py   the cooked package (.bsm)          names, imports, exports
  skelmesh.py   the USkeletalMesh export           decompiled serializer (tools\\ida\\sk2-sk4)
  hkpack.py     the AnimationPackageWrapper blob   a Havok 2012 binary packfile
  hkread.py     its objects                         class layouts read from the exe (hkclass_dump.py)
  hkspline.py   hkaSplineCompressedAnimation        the spline/quantisation decoder

COORDINATES. Unreal is left-handed, Z up, X forward. glTF is right-handed, Y up. The
conversion is the reflection (x, y, z) -> (x, z, y) - it swaps handedness and puts Z up
on glTF's Y in one step, and Blender's importer then maps glTF Y back to its own Z, so
in Blender the model stands Z up, X forward, with Unreal's +Y on Blender's -Y. A
rotation under that reflection is (qx, qy, qz, qw) -> (-qx, -qz, -qy, qw). The reflection
also reverses triangle winding; the writer checks every face against its vertex
normals and keeps whichever order agrees.

UNITS. One glTF unit is one Unreal unit by default (--scale), the convention the
Unofficial BioShock SDK's glTF importer uses too.

The output is game-derived and stays in the local model workspace, never the repo.
"""
import argparse
import json
import math
import os
import re
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from vpackage import Package  # noqa: E402
from hkpack import Packfile  # noqa: E402
import hkread  # noqa: E402
import hkspline  # noqa: E402
import skelmesh  # noqa: E402


# --- math -------------------------------------------------------------------
def v_ue(v):
    return (v[0], v[2], v[1])


def q_ue(q):
    return (-q[0], -q[2], -q[1], q[3])


def q_mul(a, b):
    ax, ay, az, aw = a; bx, by, bz, bw = b
    return (aw * bx + ax * bw + ay * bz - az * by,
            aw * by - ax * bz + ay * bw + az * bx,
            aw * bz + ax * by - ay * bx + az * bw,
            aw * bw - ax * bx - ay * by - az * bz)


def q_rot(q, v):
    qx, qy, qz, qw = q
    t = (2 * (qy * v[2] - qz * v[1]), 2 * (qz * v[0] - qx * v[2]), 2 * (qx * v[1] - qy * v[0]))
    return (v[0] + qw * t[0] + (qy * t[2] - qz * t[1]),
            v[1] + qw * t[1] + (qz * t[0] - qx * t[2]),
            v[2] + qw * t[2] + (qx * t[1] - qy * t[0]))


def compose(parent, local):
    """(t, q, s) world of a child: parent * local (uniform-ish scale assumed)."""
    pt, pq, ps = parent
    lt, lq, ls = local
    st = (lt[0] * ps[0], lt[1] * ps[1], lt[2] * ps[2])
    r = q_rot(pq, st)
    return ((pt[0] + r[0], pt[1] + r[1], pt[2] + r[2]), q_mul(pq, lq),
            (ps[0] * ls[0], ps[1] * ls[1], ps[2] * ls[2]))


def mat_of(t, q, s):
    x, y, z, w = q
    r = [[1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)],
         [2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)],
         [2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)]]
    m = [[r[i][j] * s[j] for j in range(3)] + [t[i]] for i in range(3)]
    return m + [[0.0, 0.0, 0.0, 1.0]]


def mat_inv_affine(m):
    a = [row[:3] for row in m[:3]]
    det = (a[0][0] * (a[1][1] * a[2][2] - a[1][2] * a[2][1])
           - a[0][1] * (a[1][0] * a[2][2] - a[1][2] * a[2][0])
           + a[0][2] * (a[1][0] * a[2][1] - a[1][1] * a[2][0]))
    inv = [[(a[(j + 1) % 3][(i + 1) % 3] * a[(j + 2) % 3][(i + 2) % 3]
             - a[(j + 1) % 3][(i + 2) % 3] * a[(j + 2) % 3][(i + 1) % 3]) / det for j in range(3)]
           for i in range(3)]
    t = [m[0][3], m[1][3], m[2][3]]
    it = [-(inv[i][0] * t[0] + inv[i][1] * t[1] + inv[i][2] * t[2]) for i in range(3)]
    return [inv[i] + [it[i]] for i in range(3)] + [[0.0, 0.0, 0.0, 1.0]]


def col_major(m):
    return [m[r][c] for c in range(4) for r in range(4)]


# --- glTF builder ------------------------------------------------------------
class Gltf:
    def __init__(self):
        self.bin = bytearray()
        self.doc = {"asset": {"version": "2.0", "generator": "bioshock-trilogy-vr tools/bsmesh/bs2gltf.py"},
                    "buffers": [], "bufferViews": [], "accessors": [], "nodes": [], "meshes": [],
                    "skins": [], "animations": [], "scenes": [{"nodes": []}], "scene": 0,
                    "materials": []}

    def _view(self, data, target=None):
        while len(self.bin) % 4:
            self.bin.append(0)
        off = len(self.bin)
        self.bin += data
        v = {"buffer": 0, "byteOffset": off, "byteLength": len(data)}
        if target:
            v["target"] = target
        self.doc["bufferViews"].append(v)
        return len(self.doc["bufferViews"]) - 1

    def accessor(self, fmt, ctype, kind, rows, target=None, minmax=False, normalized=False):
        flat = [x for r in rows for x in (r if isinstance(r, (tuple, list)) else (r,))]
        data = struct.pack("<%d%s" % (len(flat), fmt), *flat)
        acc = {"bufferView": self._view(data, target), "componentType": ctype,
               "count": len(rows), "type": kind}
        if normalized:
            acc["normalized"] = True
        if minmax:
            n = {"SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4}[kind]
            cols = [[r[i] if n > 1 else r for r in rows] for i in range(n)]
            acc["min"] = [min(c) for c in cols]
            acc["max"] = [max(c) for c in cols]
        self.doc["accessors"].append(acc)
        return len(self.doc["accessors"]) - 1

    def write(self, path):
        while len(self.bin) % 4:
            self.bin.append(0)
        self.doc["buffers"] = [{"byteLength": len(self.bin)}]
        for k in ("animations", "skins", "materials"):
            if not self.doc[k]:
                del self.doc[k]
        js = json.dumps(self.doc, separators=(",", ":")).encode()
        while len(js) % 4:
            js += b" "
        total = 12 + 8 + len(js) + 8 + len(self.bin)
        with open(path, "wb") as f:
            f.write(struct.pack("<III", 0x46546C67, 2, total))
            f.write(struct.pack("<II", len(js), 0x4E4F534A)); f.write(js)
            f.write(struct.pack("<II", len(self.bin), 0x004E4942)); f.write(self.bin)


FLOAT, U16, U8, U32 = 5126, 5123, 5121, 5125


def convert(pkg, mesh_export, classes, out, lod_index=0, anim_filter=None, scale=1.0, log=print):
    mesh = skelmesh.parse(pkg, mesh_export)
    if not 0 <= lod_index < len(mesh["lods"]):
        raise SystemExit("*** %s has %d LOD(s), not %d" % (mesh["name"], len(mesh["lods"]), lod_index))
    lod = mesh["lods"][lod_index]
    if not lod["vertices"]:
        # Measured: 2-Fisheries' Sliding512SingleDoorMesh is an 813-byte stub with no
        # geometry in any LOD - a placeholder, not a parse failure.
        raise SystemExit("*** %s LOD %d has no geometry (an empty placeholder mesh)" % (mesh["name"], lod_index))
    # The Havok package: the AnimationPackageWrapper in the mesh's own group.
    apws = [e for e in pkg.exports if pkg.class_name(e) == "AnimationPackageWrapper"
            and e["package"] == mesh_export["package"]]
    if not apws:
        raise SystemExit("*** no AnimationPackageWrapper in %s's group" % mesh["name"])
    pf = Packfile(pkg.data(apws[0]))
    (rp, _), = pf.objects("AnimationPackageRoot")
    root = hkread.read(pf, rp, "AnimationPackageRoot", classes)
    sk = root["m_highBoneSkeleton"].get()
    bones = [b["name"] for b in sk["bones"]]
    parents = sk["parentIndices"]
    ref = sk["referencePose"]
    log("mesh %s: LOD %d of %d, %d vertices (%d skinned), %d triangles, %d sockets; skeleton %r, %d bones; %d clips"
        % (mesh["name"], lod_index, len(mesh["lods"]), len(lod["vertices"]), lod["num_skinned"],
           len(lod["indices"]) // 3, len(mesh["sockets"]), sk["name"], len(bones), len(root["m_animations"])))

    g = Gltf()
    # bones as nodes, local TRS converted
    local = []
    for i, b in enumerate(bones):
        t = tuple(x * scale for x in v_ue(ref[i]["t"]))
        q = q_ue(ref[i]["q"])
        s = v_ue(ref[i]["s"])
        local.append((t, q, s))
        g.doc["nodes"].append({"name": b, "translation": list(t), "rotation": list(q), "scale": list(s)})
    for i, p in enumerate(parents):
        if p >= 0:
            g.doc["nodes"][p].setdefault("children", []).append(i)
    world = [None] * len(bones)
    for i in range(len(bones)):
        world[i] = local[i] if parents[i] < 0 else compose(world[parents[i]], local[i])
    ibm = [col_major(mat_inv_affine(mat_of(*w))) for w in world]
    roots = [i for i, p in enumerate(parents) if p < 0]

    # validation: a skinned vertex should sit near the bones that move it
    bmap = lod["bone_map"]
    dists = []
    for v in lod["vertices"][lod["num_rigid"]:]:
        k = max(range(4), key=lambda j: v["weights"][j])
        bi = bmap[v["bones"][k]]
        p = tuple(x * scale for x in v_ue(v["pos"]))
        dists.append(math.dist(p, world[bi][0]))
    dists.sort()
    if dists:
        log("check: skinned vertex to its main bone at bind pose: median %.1f, p95 %.1f units"
            % (dists[len(dists) // 2], dists[int(len(dists) * 0.95)]))
    else:
        log("check: no multi-influence vertices (every vertex is rigid to one bone)")

    # mesh
    pos = [tuple(x * scale for x in v_ue(v["pos"])) for v in lod["vertices"]]
    nrm = []
    for v in lod["vertices"]:
        n = v_ue(v["basis"][2])
        ln = math.sqrt(sum(x * x for x in n)) or 1.0
        nrm.append(tuple(x / ln for x in n))
    uv = [tuple(v["uv"]) for v in lod["vertices"]]
    joints = [tuple(bmap[b] if w else 0 for b, w in zip(v["bones"], v["weights"])) for v in lod["vertices"]]
    weights = []
    for v in lod["vertices"]:
        tot = sum(v["weights"]) or 1
        weights.append(tuple(w / tot for w in v["weights"]))
    tris = [lod["indices"][i:i + 3] for i in range(0, len(lod["indices"]), 3)]
    agree = 0
    for a, b, c in tris:
        e1 = [pos[b][k] - pos[a][k] for k in range(3)]
        e2 = [pos[c][k] - pos[a][k] for k in range(3)]
        fn = (e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0])
        vn = [nrm[a][k] + nrm[b][k] + nrm[c][k] for k in range(3)]
        agree += 1 if sum(fn[k] * vn[k] for k in range(3)) > 0 else 0
    flip = agree < len(tris) / 2
    if flip:
        tris = [(a, c, b) for a, b, c in tris]
    log("check: %d of %d faces agree with their vertex normals%s"
        % (len(tris) - agree if flip else agree, len(tris), " (winding reversed for glTF)" if flip else ""))
    ind = [i for t in tris for i in t]

    prim = {"attributes": {
        "POSITION": g.accessor("f", FLOAT, "VEC3", pos, 34962, minmax=True),
        "NORMAL": g.accessor("f", FLOAT, "VEC3", nrm, 34962),
        "TEXCOORD_0": g.accessor("f", FLOAT, "VEC2", uv, 34962),
        "JOINTS_0": g.accessor("H", U16, "VEC4", joints, 34962),
        "WEIGHTS_0": g.accessor("f", FLOAT, "VEC4", weights, 34962)},
        "indices": g.accessor("I" if len(pos) > 65535 else "H", U32 if len(pos) > 65535 else U16,
                              "SCALAR", ind, 34963)}
    if mesh["materials"]:
        g.doc["materials"] = [{"name": m or "material_%d" % i} for i, m in enumerate(mesh["materials"])]
        prim["material"] = 0
    g.doc["meshes"].append({"name": mesh["name"], "primitives": [prim]})
    skin_node = len(g.doc["nodes"])
    g.doc["nodes"].append({"name": mesh["name"], "mesh": 0, "skin": 0})
    g.doc["skins"].append({"name": sk["name"], "joints": list(range(len(bones))), "skeleton": roots[0],
                           "inverseBindMatrices": g.accessor("f", FLOAT, "MAT4", ibm)})
    arm = len(g.doc["nodes"])
    g.doc["nodes"].append({"name": "Armature_" + mesh["name"], "children": roots + [skin_node]})
    g.doc["scenes"][0]["nodes"] = [arm]

    # sockets as empties parented to their bones (SOCKET_<name>, the SDK's own convention)
    for s in mesh["sockets"]:
        if s["bone"] not in bones:
            continue
        m = s["matrix"]
        g.doc["nodes"].append({"name": "SOCKET_" + s["name"], "translation": list(v_ue(m[0:3]))})
        g.doc["nodes"][bones.index(s["bone"])].setdefault("children", []).append(len(g.doc["nodes"]) - 1)

    # animations
    rx = re.compile(anim_filter, re.I) if anim_filter and anim_filter != "none" else None
    done = 0
    if anim_filter != "none":
        for a in root["m_animations"]:
            label = "%s/%s" % (a["m_groupName"], a["m_name"])
            if rx and not rx.search(label):
                continue
            binding = a["m_binding"].get()
            anim = binding["animation"].get()
            if anim["__class__"] != "hkaSplineCompressedAnimation":
                log("  skip %s: %s not decoded" % (label, anim["__class__"]))
                continue
            frames = hkspline.decode(anim)
            fd = anim["frameDuration"] or (anim["duration"] / max(1, len(frames) - 1))
            times = [i * fd for i in range(len(frames))]
            tacc = g.accessor("f", FLOAT, "SCALAR", times, minmax=True)
            samplers, channels = [], []
            track_bones = binding["transformTrackToBoneIndices"] or list(range(len(frames[0])))
            for ti, bi in enumerate(track_bones):
                tr = [tuple(x * scale for x in v_ue(fr[ti][0])) for fr in frames]
                ro = [q_ue(fr[ti][1]) for fr in frames]
                for i in range(1, len(ro)):         # keep one hemisphere for slerp
                    if sum(x * y for x, y in zip(ro[i - 1], ro[i])) < 0:
                        ro[i] = tuple(-x for x in ro[i])
                sc = [v_ue(fr[ti][2]) for fr in frames]
                for path, rows, kind in (("translation", tr, "VEC3"), ("rotation", ro, "VEC4"), ("scale", sc, "VEC3")):
                    samplers.append({"input": tacc, "output": g.accessor("f", FLOAT, kind, rows),
                                     "interpolation": "LINEAR"})
                    channels.append({"sampler": len(samplers) - 1, "target": {"node": bi, "path": path}})
            g.doc["animations"].append({"name": label.replace("/", "__"), "samplers": samplers, "channels": channels})
            done += 1
    g.write(out)
    log("wrote %s: %d bones, %d vertices, %d triangles, %d clips, %.1f MB"
        % (out, len(bones), len(pos), len(tris), done, os.path.getsize(out) / 1e6))


def main():
    ap = argparse.ArgumentParser(description="BioShock Remastered skeletal mesh + Havok skeleton/clips -> glTF")
    ap.add_argument("--bsm", required=True, help="path to the .bsm package")
    ap.add_argument("--classes", required=True, help="hkclass_dump.py --json output for this game's exe")
    ap.add_argument("--mesh")
    ap.add_argument("--out")
    ap.add_argument("--lod", type=int, default=0)
    ap.add_argument("--anims", default=None, help="regex over group/clip, or 'none'")
    ap.add_argument("--scale", type=float, default=1.0)
    ap.add_argument("--list", action="store_true")
    a = ap.parse_args()
    pkg = Package(a.bsm)
    if a.list:
        for e in pkg.exports:
            if pkg.class_name(e) == "SkeletalMesh":
                print("%-40s %s" % (e["name"], pkg.path_of(e)))
        return 0
    found = pkg.find(a.mesh, "SkeletalMesh")
    if not found:
        raise SystemExit("*** no SkeletalMesh %r in %s (try --list)" % (a.mesh, a.bsm))
    convert(pkg, found[0], hkread.load_classes(a.classes), a.out, a.lod, a.anims, a.scale)
    return 0


if __name__ == "__main__":
    sys.exit(main())
