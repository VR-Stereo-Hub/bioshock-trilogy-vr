"""skelmesh.py - parse a BioShock Remastered USkeletalMesh export (package version 142).

The layout is the game's own serializer, decompiled with tools\\ida\\sk2/sk3/sk4
(USkeletalMesh vtable slot 11 at +0x3EE720, the LOD-model serializer at +0x3FAE70).
In load order:

  u32 4, u32 3                         versioned UObject header
  tagged properties ... None           (CheckpointTypePadding is the usual one)
  bounding box (6 f32 + u8), bounding sphere (4 f32)
  u32 4, u32 meshVersion               (5 on BS1)
  TArray<UObject*> materials
  13 x f32/u32                         scale, origin, rotation origin, misc
  TArray<68-byte bone>                 empty on BS1: the skeleton is the Havok one
  UObject*, f32
  TArray<FName> socket names, TArray<FName> socket bones,
  TArray<48-byte matrix> socket transforms
  TArray<288-byte LOD model>, each:
      u32 4, u32 lodVersion
      TArray<section: 9 x u16 (skipping bytes 10-11)>
      TArray<u16> bone map            vertex bone bytes index THIS, not the skeleton
      TArray<u16> indices (raw, version >= 142), u32
      TArray<skinned vertex, 64 bytes on disk>: pos f32x3, tangent basis f32x9,
          uv f32x2, then 4 x (u8 boneMapIndex, u8 weight) - weights sum to 255
      TArray<rigid vertex, 57 bytes on disk>: pos, basis, uv, u8 boneMapIndex
      6 x u32, [u32 when lodVersion > 1], [TArray<96-byte group> when >= 3],
      5 lazy arrays (u32 absoluteEndOffset, i32, i32, ...)
  ... more lazy arrays and version-gated data, not needed for geometry

Vertex index order in the index buffer: RIGID vertices first, then skinned ones -
the reverse of their storage order. parse() returns them in index order.
Everything returned is in the mesh's own space (Unreal units, left-handed, Z up).
"""
import struct


class MeshError(Exception):
    pass


class _R:
    def __init__(self, pkg, export):
        self.pkg = pkg
        self.base = export["offset"]
        self.m = pkg.data(export)
        self.o = 0

    def cidx(self):
        v, o = self.pkg.cidx(self.base + self.o)
        self.o = o - self.base
        return v

    def u8(self):
        v = self.m[self.o]; self.o += 1; return v

    def u32(self):
        v = struct.unpack_from("<I", self.m, self.o)[0]; self.o += 4; return v

    def i32(self):
        v = struct.unpack_from("<i", self.m, self.o)[0]; self.o += 4; return v

    def f32(self, n=1):
        v = struct.unpack_from("<%df" % n, self.m, self.o); self.o += 4 * n
        return v[0] if n == 1 else v

    def fname(self):
        i = self.cidx(); n = self.u32()
        return self.pkg.names[i] if n == 0 else "%s_%d" % (self.pkg.names[i], n - 1)

    def obj(self):
        return self.cidx()

    def count(self, what, limit=1 << 20):
        n = self.cidx()
        if not 0 <= n <= limit:
            raise MeshError("%s count %d at blob 0x%X - not the measured layout" % (what, n, self.o))
        return n


def _props(r):
    out = {}
    while True:
        name = r.fname()
        if name == "None":
            return out
        info = r.u8()
        t, sc = info & 15, (info >> 4) & 7
        size = {0: 1, 1: 2, 2: 4, 3: 12, 4: 16}.get(sc)
        if size is None:
            raise MeshError("property %s size code %d not handled" % (name, sc))
        out[name] = r.m[r.o:r.o + size]
        r.o += size


def parse(pkg, export):
    r = _R(pkg, export)
    if (r.u32(), r.u32()) != (4, 3):
        raise MeshError("versioned UObject header is not (4, 3)")
    props = _props(r)
    bbox = r.f32(6); r.u8()
    sphere = r.f32(4)
    if r.u32() != 4:
        raise MeshError("mesh version header missing")
    mesh_version = r.u32()
    materials = [r.obj() for _ in range(r.count("materials", 256))]
    misc = [r.u32() for _ in range(13)]
    nbones = r.count("bones", 1024)
    for _ in range(nbones):              # FName + 14 dwords each on disk
        r.fname(); r.o += 56
    r.obj(); r.f32()
    sock_names = [r.fname() for _ in range(r.count("socket names", 1024))]
    sock_bones = [r.fname() for _ in range(r.count("socket bones", 1024))]
    sock_mats = [r.f32(12) for _ in range(r.count("socket transforms", 1024))]
    lods = []
    for li in range(r.count("LOD models", 16)):
        if r.u32() != 4:
            raise MeshError("LOD %d version header missing" % li)
        lod_version = r.u32()
        sections = []
        for _ in range(r.count("sections", 256)):
            sections.append(struct.unpack_from("<9H", r.m, r.o)); r.o += 18
        nmap = r.count("bone map", 256)
        bone_map = list(struct.unpack_from("<%dH" % nmap, r.m, r.o)); r.o += 2 * nmap
        nidx = r.count("indices", 1 << 24)
        indices = list(struct.unpack_from("<%dH" % nidx, r.m, r.o)); r.o += 2 * nidx
        r.u32()
        skinned = []
        for _ in range(r.count("skinned vertices", 1 << 20)):
            v = struct.unpack_from("<14f", r.m, r.o)
            inf = r.m[r.o + 56:r.o + 64]
            r.o += 64
            skinned.append({"pos": v[0:3], "basis": (v[3:6], v[6:9], v[9:12]), "uv": v[12:14],
                            "bones": [inf[2 * k] for k in range(4)],
                            "weights": [inf[2 * k + 1] for k in range(4)]})
        rigid = []
        for _ in range(r.count("rigid vertices", 1 << 20)):
            v = struct.unpack_from("<14f", r.m, r.o)
            b = r.m[r.o + 56]
            r.o += 57
            rigid.append({"pos": v[0:3], "basis": (v[3:6], v[6:9], v[9:12]), "uv": v[12:14],
                          "bones": [b, 0, 0, 0], "weights": [255, 0, 0, 0]})
        # The index buffer numbers the RIGID vertices first, although they are stored
        # second: measured on NEWPlayerHands LOD 0, a median triangle edge of 0.75 units
        # in this order against 43 in storage order.
        verts = rigid + skinned
        nrigid = len(rigid)
        scal = [r.u32() for _ in range(6)]
        if lod_version > 1:
            r.u32()
        if lod_version >= 3:
            for _ in range(r.count("bone groups", 4096)):
                for _k in range(8):
                    c = r.count("group array", 1 << 16); r.o += 2 * c
        for _ in range(5):
            end = r.u32()
            r.o = end - r.base
        if indices and max(indices) >= len(verts):
            raise MeshError("LOD %d index %d past %d vertices" % (li, max(indices), len(verts)))
        lods.append({"version": lod_version, "sections": sections, "bone_map": bone_map,
                     "indices": indices, "vertices": verts, "num_rigid": nrigid,
                     "num_skinned": len(skinned)})
    return {"name": export["name"], "props": props, "bbox": bbox, "sphere": sphere,
            "mesh_version": mesh_version,
            "materials": [pkg.ref_name(m) for m in materials],
            "sockets": [{"name": n, "bone": b, "matrix": mt} for n, b, mt in zip(sock_names, sock_bones, sock_mats)],
            "lods": lods}
