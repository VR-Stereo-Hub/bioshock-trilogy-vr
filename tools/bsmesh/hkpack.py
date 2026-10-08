"""hkpack.py - read a Havok 2012 BINARY packfile (hk_2012.2.0-r1, file version 9,
32-bit little-endian layout), the format BioShock Remastered stores skeletons and
animation clips in, inside an AnimationPackageWrapper export.

Layout (measured on UAPW_NEWPlayerHands, docs/bioshock1/HAVOK_AND_PACKAGES.md):

  header (64 bytes)  magic 0x57E0E057 0x10C0C010, userTag, fileVersion,
                     layoutRules[4] (pointer size, little endian, reusePadding,
                     emptyBaseClassOptimization), numSections, contentsSectionIndex,
                     contentsSectionOffset, contentsClassNameSectionIndex,
                     contentsClassNameSectionOffset, contentsVersion[16], flags,
                     maxpredicate, predicateArraySizePlusPadding
  section headers    numSections x 48 bytes: tag[19], NUL, absoluteDataStart, then
                     offsets RELATIVE to it: localFixups, globalFixups,
                     virtualFixups, exports, imports, end
  __classnames__     { u32 signature, u8 0x09, char name[] NUL } ... u32 0xFFFFFFFF
  fixups             local  (src, dst)                 pointer within the section
                     global (src, dstSection, dst)     pointer across sections
                     virtual(src, nameSection, nameOff) an OBJECT of that class
                     starts at src; -1 pads each table

Every pointer field in an object is resolved through the fixup tables, so a
Pointer here is (section, offset), never a raw address.

This file is ours and is committed; the packfiles it reads are 2K's and are not.
"""
import struct


class HavokError(Exception):
    pass


# hkClassMember::Type, Havok 2012
MEMBER_TYPES = [
    "VOID", "BOOL", "CHAR", "INT8", "UINT8", "INT16", "UINT16", "INT32", "UINT32",
    "INT64", "UINT64", "REAL", "VECTOR4", "QUATERNION", "MATRIX3", "ROTATION",
    "QSTRANSFORM", "MATRIX4", "TRANSFORM", "ZERO", "POINTER", "FUNCTIONPOINTER",
    "ARRAY", "INPLACEARRAY", "ENUM", "STRUCT", "SIMPLEARRAY", "HOMOGENEOUSARRAY",
    "VARIANT", "CSTRING", "ULONG", "FLAGS", "HALF", "STRINGPTR", "RELARRAY",
]


class Section:
    def __init__(self, index, tag, start, offs):
        self.index = index
        self.tag = tag
        self.start = start                       # absolute, in the packfile
        (self.local, self.glob, self.virt, self.exports, self.imports, self.end) = offs
        self.ptr = {}                            # offset -> (section, offset)
        self.objects = {}                        # offset -> class name


class Packfile:
    MAGIC = (0x57E0E057, 0x10C0C010)

    def __init__(self, blob, base=None):
        if base is None:
            base = blob.find(struct.pack("<II", *self.MAGIC))
            if base < 0:
                raise HavokError("no Havok packfile magic in the blob")
        self.b = blob
        self.base = base
        b = blob
        m0, m1, self.user_tag, self.file_version = struct.unpack_from("<IIII", b, base)
        self.layout = b[base + 16:base + 20]
        if self.layout[0] != 4 or self.layout[1] != 1:
            raise HavokError("layout %s: only the 32-bit little-endian layout is handled" % self.layout.hex())
        (ns, self.contents_section, self.contents_offset, self.contents_cn_section,
         self.contents_cn_offset) = struct.unpack_from("<iiiii", b, base + 20)
        self.version = b[base + 40:base + 56].split(b"\0")[0].decode()
        if self.file_version not in (8, 9, 10):
            raise HavokError("packfile version %d: only 8-10 (48-byte section headers) handled"
                             % self.file_version)
        self.sections = []
        o = base + 64
        for i in range(ns):
            tag = b[o:o + 19].split(b"\0")[0].decode("latin1")
            vals = struct.unpack_from("<7i", b, o + 20)
            self.sections.append(Section(i, tag, base + vals[0], vals[1:]))
            o += 48
        self.by_tag = {s.tag: s for s in self.sections}
        self.classnames = self._read_classnames()
        for s in self.sections:
            self._read_fixups(s)

    # -- tables -------------------------------------------------------------
    def _read_classnames(self):
        s = self.by_tag.get("__classnames__")
        if s is None:
            raise HavokError("no __classnames__ section")
        b = self.b
        o = s.start
        end = s.start + s.local
        out = {}
        while o + 5 <= end:
            sig = struct.unpack_from("<I", b, o)[0]
            if sig == 0xFFFFFFFF:
                break
            o += 5                               # signature + the 0x09 tag byte
            z = b.index(b"\0", o)
            out[(s.index, o - s.start)] = (b[o:z].decode("latin1"), sig)
            o = z + 1
        return out

    def _read_fixups(self, s):
        b = self.b
        if s.tag in ("__classnames__", "__types__"):
            return
        o, stop = s.start + s.local, s.start + s.glob
        while o + 8 <= stop:
            src, dst = struct.unpack_from("<ii", b, o); o += 8
            if src == -1:
                continue
            s.ptr[src] = (s.index, dst)
        o, stop = s.start + s.glob, s.start + s.virt
        while o + 12 <= stop:
            src, dsec, dst = struct.unpack_from("<iii", b, o); o += 12
            if src == -1:
                continue
            s.ptr[src] = (dsec, dst)
        o, stop = s.start + s.virt, s.start + s.exports
        while o + 12 <= stop:
            src, nsec, noff = struct.unpack_from("<iii", b, o); o += 12
            if src == -1:
                continue
            name = self.classnames.get((nsec, noff))
            if name is None:
                raise HavokError("virtual fixup in %s names class at %d:%d, not in __classnames__"
                                 % (s.tag, nsec, noff))
            s.objects[src] = name[0]

    # -- reading ------------------------------------------------------------
    def sec(self, i):
        return self.sections[i]

    def raw(self, ptr, n):
        si, off = ptr
        s = self.sections[si]
        a = s.start + off
        return self.b[a:a + n]

    def u8(self, p): return self.raw(p, 1)[0]
    def i16(self, p): return struct.unpack("<h", self.raw(p, 2))[0]
    def u16(self, p): return struct.unpack("<H", self.raw(p, 2))[0]
    def i32(self, p): return struct.unpack("<i", self.raw(p, 4))[0]
    def u32(self, p): return struct.unpack("<I", self.raw(p, 4))[0]
    def f32(self, p): return struct.unpack("<f", self.raw(p, 4))[0]

    @staticmethod
    def at(ptr, delta):
        return (ptr[0], ptr[1] + delta)

    def pointer(self, p):
        """The target of the pointer field at p, or None (a null pointer has no fixup)."""
        return self.sections[p[0]].ptr.get(p[1])

    def cstring(self, p):
        t = self.pointer(p)
        if t is None:
            return None
        s = self.sections[t[0]]
        a = s.start + t[1]
        return self.b[a:self.b.index(b"\0", a)].decode("latin1")

    def array(self, p):
        """hkArray: (data pointer or None, size). 12 bytes: ptr, size, capacityAndFlags."""
        return self.pointer(p), self.i32(self.at(p, 4))

    def objects(self, cls=None):
        out = []
        for s in self.sections:
            for off, name in sorted(s.objects.items()):
                if cls is None or name == cls:
                    out.append(((s.index, off), name))
        return out

    def class_of(self, ptr):
        return self.sections[ptr[0]].objects.get(ptr[1])

    # -- embedded reflection ------------------------------------------------
    def embedded_classes(self):
        """The hkClass objects stored IN the packfile: {name: {size, parent, members}}.
        Members are dicts with name, type, subtype, offset, carray, class, enum."""
        out = {}
        for p, cls in self.objects("hkClass"):
            name = self.cstring(p)
            parent = self.pointer(self.at(p, 4))
            size = self.i32(self.at(p, 8))
            mptr, mcount = self.pointer(self.at(p, 24)), self.i32(self.at(p, 28))
            members = []
            for i in range(mcount if mptr else 0):
                m = self.at(mptr, 24 * i)
                mcls = self.pointer(self.at(m, 4))
                members.append({
                    "name": self.cstring(m),
                    "class": self.cstring(mcls) if mcls else None,
                    "type": MEMBER_TYPES[self.u8(self.at(m, 12))],
                    "subtype": MEMBER_TYPES[self.u8(self.at(m, 13))],
                    "carray": self.i16(self.at(m, 14)),
                    "flags": self.u16(self.at(m, 16)),
                    "offset": self.u16(self.at(m, 18)),
                })
            out[name] = {"size": size, "parent": self.cstring(parent) if parent else None,
                         "members": members, "ptr": p}
        return out
