"""vpackage.py - read a BioShock Remastered (Vengeance, UE2.5) cooked package.

The format, as measured on BioShock Remastered's ContentBaked\\pc\\Maps\\*.bsm
(package version 142, licensee 56; docs/bioshock1/HAVOK_AND_PACKAGES.md):

  summary   u32 magic 0x9E2A83C1, u16 version, u16 licensee, u32 flags,
            u32 nameCount, u32 nameOffset, u32 exportCount, u32 exportOffset,
            u32 importCount, u32 importOffset, ...
  name      cidx charCount (including the NUL), UTF-16LE chars, u64 flags
  FName     cidx nameIndex, u32 number (number 0 = plain; n = "<name>_<n-1>")
  import    FName classPackage, FName className, i32 package, FName objectName
  export    cidx class, cidx super, i32 package, i32 (always 0 measured),
            FName objectName, u64 flags, cidx serialSize, [cidx serialOffset
            when size > 0], u32 (0 or 1)

The tables sit at the END of the file, in the order names, imports, exports, and
each ends exactly where the next begins - the reader checks that, so a layout
change is a loud failure rather than garbage.

Object references are UE2 "package indices": 0 none, > 0 export i-1, < 0 import -i-1.

Nothing this reads is ever committed: the packages are 2K's and the bytes it
extracts stay in the local model workspace.
"""
import struct


class PackageError(Exception):
    pass


class Package:
    MAGIC = 0x9E2A83C1

    def __init__(self, path):
        self.path = path
        with open(path, "rb") as f:
            self.d = f.read()
        d = self.d
        (magic, self.version, self.licensee, self.flags, nc, no, ec, eo, ic, io) = \
            struct.unpack_from("<IHHIIIIIII", d, 0)
        if magic != self.MAGIC:
            raise PackageError("%s: not an Unreal package (magic 0x%08X)" % (path, magic))
        self.names = []
        o = no
        for _ in range(nc):
            n, o = self.cidx(o)
            if n <= 0:
                raise PackageError("name length %d at 0x%X - not the measured layout" % (n, o))
            self.names.append(d[o:o + n * 2 - 2].decode("utf-16le"))
            o += n * 2 + 8
        if o != io:
            raise PackageError("name table ends at 0x%X, import table starts at 0x%X" % (o, io))
        self.imports = []
        o = io
        for _ in range(ic):
            cpk, o = self.fname(o)
            cnm, o = self.fname(o)
            pkg = struct.unpack_from("<i", d, o)[0]; o += 4
            onm, o = self.fname(o)
            self.imports.append({"class_package": cpk, "class": cnm, "package": pkg, "name": onm})
        if o != eo:
            raise PackageError("import table ends at 0x%X, export table starts at 0x%X" % (o, eo))
        self.exports = []
        o = eo
        for i in range(ec):
            cls, o = self.cidx(o)
            sup, o = self.cidx(o)
            pkg, unk = struct.unpack_from("<ii", d, o); o += 8
            onm, o = self.fname(o)
            flags = struct.unpack_from("<Q", d, o)[0]; o += 8
            size, o = self.cidx(o)
            off = 0
            if size > 0:
                off, o = self.cidx(o)
            tail = struct.unpack_from("<I", d, o)[0]; o += 4
            self.exports.append({"index": i, "class_ref": cls, "super_ref": sup, "package": pkg,
                                 "unknown": unk, "name": onm, "flags": flags, "size": size,
                                 "offset": off, "tail": tail})
        if o != len(d):
            raise PackageError("export table ends at 0x%X, file ends at 0x%X" % (o, len(d)))

    # -- primitives ---------------------------------------------------------
    def cidx(self, o):
        d = self.d
        b = d[o]; o += 1
        neg = b & 0x80
        v = b & 0x3F
        if b & 0x40:
            sh = 6
            while True:
                b = d[o]; o += 1
                v |= (b & 0x7F) << sh
                sh += 7
                if not b & 0x80:
                    break
        return (-v if neg else v), o

    def fname(self, o):
        i, o = self.cidx(o)
        n = struct.unpack_from("<I", self.d, o)[0]; o += 4
        if not 0 <= i < len(self.names):
            raise PackageError("name index %d out of range at 0x%X" % (i, o))
        return (self.names[i] if n == 0 else "%s_%d" % (self.names[i], n - 1)), o

    # -- references ---------------------------------------------------------
    def ref_name(self, ref):
        if ref > 0:
            return self.exports[ref - 1]["name"]
        if ref < 0:
            return self.imports[-ref - 1]["name"]
        return None

    def class_name(self, export):
        return "Class" if export["class_ref"] == 0 else self.ref_name(export["class_ref"])

    def path_of(self, export):
        parts = [export["name"]]
        p = export["package"]
        while p > 0:
            e = self.exports[p - 1]
            parts.append(e["name"])
            p = e["package"]
        return ".".join(reversed(parts))

    def find(self, name, cls=None):
        return [e for e in self.exports
                if e["name"] == name and (cls is None or self.class_name(e) == cls)]

    def data(self, export):
        return self.d[export["offset"]:export["offset"] + export["size"]]
