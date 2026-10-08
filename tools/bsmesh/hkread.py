"""hkread.py - decode Havok packfile objects using class layouts DERIVED from the game
exe (hkclass_dump.py --json), never hand-typed ones.

    classes = load_classes(path_to_json)
    root = read(pf, ptr, "AnimationPackageRoot", classes)

A decoded object is a dict of member name -> value. Scalars are Python numbers;
VECTOR4/QUATERNION are 4-tuples; QSTRANSFORM is {"t": (x,y,z), "q": (x,y,z,w),
"s": (x,y,z)}; arrays are lists; a POINTER to a class is a Ref (decoded on demand,
so a reference to a 300 KB animation does not decode the animation); strings are str.
A member type this reader does not know raises - it never guesses a size.
"""
import json
import struct

SCALARS = {
    "BOOL": ("<B", 1), "CHAR": ("<b", 1), "INT8": ("<b", 1), "UINT8": ("<B", 1),
    "INT16": ("<h", 2), "UINT16": ("<H", 2), "INT32": ("<i", 4), "UINT32": ("<I", 4),
    "INT64": ("<q", 8), "UINT64": ("<Q", 8), "REAL": ("<f", 4), "ULONG": ("<I", 4),
    "HALF": ("<H", 2),
}
INLINE_SIZE = {"VECTOR4": 16, "QUATERNION": 16, "QSTRANSFORM": 48, "MATRIX3": 48,
               "ROTATION": 48, "MATRIX4": 64, "TRANSFORM": 64, "POINTER": 4,
               "STRINGPTR": 4, "CSTRING": 4, "ARRAY": 12, "SIMPLEARRAY": 8,
               "FUNCTIONPOINTER": 4, "VARIANT": 8}


class Ref:
    """A pointer to an object, decoded only when asked."""
    def __init__(self, pf, classes, ptr, cls):
        self.pf, self.classes, self.ptr, self.cls = pf, classes, ptr, cls

    def get(self):
        return read(self.pf, self.ptr, self.cls, self.classes)

    def __repr__(self):
        return "<Ref %s @%d:%d>" % (self.cls, self.ptr[0], self.ptr[1])


def load_classes(path):
    with open(path) as f:
        return json.load(f)


def _scalar(pf, p, t):
    fmt, n = SCALARS[t]
    return struct.unpack(fmt, pf.raw(p, n))[0]


def _vec(pf, p, n=4):
    return struct.unpack("<%df" % n, pf.raw(p, 4 * n))


def _qst(pf, p):
    v = struct.unpack("<12f", pf.raw(p, 48))
    return {"t": v[0:3], "q": v[4:8], "s": v[8:11]}


def elem_size(t, sub, cls, classes):
    if t in SCALARS:
        return SCALARS[t][1]
    if t == "ENUM" or t == "FLAGS":
        return SCALARS[sub][1]
    if t == "STRUCT":
        return classes[cls]["size"]
    if t in INLINE_SIZE:
        return INLINE_SIZE[t]
    raise ValueError("no element size for %s<%s> %s" % (t, sub, cls))


def _value(pf, p, t, sub, cls, classes):
    if t in SCALARS:
        return _scalar(pf, p, t)
    if t in ("ENUM", "FLAGS"):
        return _scalar(pf, p, sub)
    if t in ("VECTOR4", "QUATERNION"):
        return _vec(pf, p)
    if t == "QSTRANSFORM":
        return _qst(pf, p)
    if t in ("MATRIX3", "ROTATION"):
        return [_vec(pf, pf.at(p, 16 * r)) for r in range(3)]
    if t in ("MATRIX4", "TRANSFORM"):
        return [_vec(pf, pf.at(p, 16 * r)) for r in range(4)]
    if t in ("STRINGPTR", "CSTRING"):
        return pf.cstring(p)
    if t == "POINTER":
        tgt = pf.pointer(p)
        if tgt is None:
            return None
        if sub == "CHAR":
            return pf.cstring(p)
        actual = pf.class_of(tgt) or cls
        return Ref(pf, classes, tgt, actual)
    if t == "STRUCT":
        return read(pf, p, cls, classes)
    if t in ("ARRAY", "SIMPLEARRAY"):
        data, n = pf.pointer(p), pf.i32(pf.at(p, 4))
        if not data or n <= 0:
            return []
        es = elem_size(sub, "VOID", cls, classes) if sub != "STRUCT" else classes[cls]["size"]
        if sub == "UINT8" or sub == "INT8":
            return pf.raw(data, n)                      # bytes, as-is
        return [_value(pf, pf.at(data, es * i), sub, "VOID", cls, classes) for i in range(n)]
    raise ValueError("member type %s<%s> is not handled" % (t, sub))


def read(pf, ptr, cls, classes):
    if cls not in classes:
        raise KeyError("no layout for class %r - dump it with hkclass_dump.py --json" % cls)
    out = {"__class__": cls}
    for m in classes[cls]["members"]:
        if m["name"] in ("memSizeAndFlags", "referenceCount"):
            continue
        p = pf.at(ptr, m["offset"])
        if m["carray"]:
            es = elem_size(m["type"], m["subtype"], m["class"], classes)
            out[m["name"]] = [_value(pf, pf.at(p, es * i), m["type"], m["subtype"], m["class"], classes)
                              for i in range(m["carray"])]
        else:
            out[m["name"]] = _value(pf, p, m["type"], m["subtype"], m["class"], classes)
    return out
