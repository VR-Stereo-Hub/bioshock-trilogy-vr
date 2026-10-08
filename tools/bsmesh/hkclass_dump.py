"""hkclass_dump.py - read Havok's compiled-in class reflection out of a game exe,
offline. This is how the packfile layouts are DERIVED rather than guessed: Havok links
an hkClass description of every serialisable class into the binary, and the loader
uses it, so the exe is the authority on what each byte of a packfile means.

  py tools\\bsmesh\\hkclass_dump.py <BioshockHD.exe> hkaSkeleton hkaSplineCompressedAnimation
  py tools\\bsmesh\\hkclass_dump.py <exe> --all                 # every live class (local output only)
  py tools\\bsmesh\\hkclass_dump.py <exe> <names...> --json f   # the layouts tools\\bsmesh reads

HOW THE CLASSES ARE FOUND (measured on BioShock Remastered, Havok 2012.2.0-r1).
The hkClass OBJECTS are not static data: each is built at start-up by a tiny
initializer that pushes the thirteen constructor arguments and calls hkClass::hkClass:

    push describedVersion / flags / attributes* / defaults* / numMembers / members*
    push numEnums / enums* / numInterfaces / interfaces* / objectSize / parent* / name
    mov  ecx, <the hkClass object>
    call hkClass::hkClass

So the tool finds the constructor from one known class name, walks every call to it,
and reads the pushes back. Havok keeps one description per HISTORICAL version of a
class for its versioning; those have objectSize 0. The live class is the one with a
non-zero objectSize. The member table (hkClassMember, 24 bytes each) IS static data:
  +0 name*  +4 class*  +8 enum*  +12 u8 type  +13 u8 subtype  +14 i16 cArraySize
  +16 u16 flags  +18 u16 offset  +20 attributes*
A member's class* points at another hkClass OBJECT, which is resolved through the
`mov ecx` operand of that object's own initializer.

Refuses loudly on anything it cannot decode. The output describes 2K's build of
Havok, is game-derived, and stays local; the tool is ours.
"""
import json
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
import importlib.util  # noqa: E402
_spec = importlib.util.spec_from_file_location("disasm_rva", os.path.join(HERE, "..", "disasm-rva.py"))
_dr = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(_dr)
sys.path.insert(0, HERE)
from hkpack import MEMBER_TYPES  # noqa: E402

ANCHOR = "hkaSkeleton"   # any class name the exe certainly registers


class Image:
    def __init__(self, path):
        self.pe = _dr.Pe(path)
        self.base = self.pe.image_base
        self.size = self.pe.size_of_image
        self.md = _dr.md()

    def ok(self, va):
        return self.base <= va < self.base + self.size

    def read(self, va, n):
        return self.pe.read(va - self.base, n)

    def cstr(self, va, limit=160):
        if not va or not self.ok(va):
            return None
        b = self.read(va, limit)
        z = b.find(b"\0")
        if z <= 0:
            return None
        s = b[:z]
        if not all(32 <= c < 127 for c in s):
            return None
        return s.decode("ascii")

    def window(self, end_va, back=96, extra=0):
        """Instructions ending exactly at end_va (+extra), decoded from a start that
        re-synchronises on it."""
        blob = self.read(end_va - back, back + extra)
        for start in range(0, 48):
            ins = list(self.md.disasm(blob[start:], end_va - back + start))
            if any(i.address == end_va for i in ins):
                return ins
        return []


def find_ctor(img):
    needle = b"\0" + ANCHOR.encode() + b"\0"
    hits = _dr.find_pattern(img.pe, needle)
    if not hits:
        raise SystemExit("*** anchor class name %r not in the image" % ANCHOR)
    sva = img.base + hits[0][1] + 1
    for _, rva in _dr.find_pattern(img.pe, b"\x68" + struct.pack("<I", sva), only_exec=True):
        va = img.base + rva
        ins = img.window(va, 8, 16)
        for i in ins:
            if i.mnemonic == "call" and i.address > va:
                return int(i.op_str, 16)
    raise SystemExit("*** no hkClass constructor call found after a push of %r" % ANCHOR)


def initializers(img, ctor):
    """Every call to the hkClass constructor, decoded: object VA and the 12 args."""
    out = []
    for _, rva in _dr.find_pattern(img.pe, b"\xE8", only_exec=True):
        call = img.base + rva
        rel = struct.unpack("<i", img.read(call + 1, 4))[0]
        if call + 5 + rel != ctor:
            continue
        ins = img.window(call, 96, 5)
        idx = [k for k, i in enumerate(ins) if i.address == call]
        if not idx:
            continue
        k = idx[0]
        if k < 14 or ins[k - 1].mnemonic != "mov" or not ins[k - 1].op_str.startswith("ecx, 0x"):
            continue
        pushes = ins[k - 14:k - 1]
        if any(p.mnemonic != "push" for p in pushes):
            continue
        try:
            vals = [int(p.op_str, 16) for p in pushes]
        except ValueError:
            continue
        # pushes are in reverse argument order: hkClass(name, parent, objectSize,
        # interfaces*, numInterfaces, enums*, numEnums, members*, numMembers,
        # defaults*, attributes*, flags, describedVersion)
        (ver, flags, attrs, defaults, nmembers, members, nenums, enums,
         nifaces, ifaces, size, parent, name) = vals
        obj = int(ins[k - 1].op_str.split(", ")[1], 16)
        out.append({"obj": obj, "name": img.cstr(name), "parent_obj": parent, "size": size,
                    "members_va": members, "nmembers": nmembers, "version": ver,
                    "flags": flags, "enums_va": enums, "nenums": nenums})
    return out


def members_of(img, c, obj_names):
    out = []
    for i in range(c["nmembers"]):
        m = img.read(c["members_va"] + 24 * i, 24)
        mname, mcls, menum, t, st, carr, mflags, moff, mattr = struct.unpack("<IIIBBhHHI", m)
        n = img.cstr(mname)
        if n is None or t >= len(MEMBER_TYPES) or st >= len(MEMBER_TYPES):
            raise SystemExit("*** %s member %d does not decode (name 0x%08X type %d)" % (c["name"], i, mname, t))
        out.append({"name": n, "type": MEMBER_TYPES[t], "subtype": MEMBER_TYPES[st], "carray": carr,
                    "flags": mflags, "offset": moff, "class": obj_names.get(mcls) if mcls else None})
    return out


def load(img):
    ctor = find_ctor(img)
    inits = initializers(img, ctor)
    obj_names = {c["obj"]: c["name"] for c in inits}
    live = {}
    for c in inits:
        if c["size"] <= 0 or c["name"] is None:
            continue
        prev = live.get(c["name"])
        if prev is None or c["version"] > prev["version"]:
            live[c["name"]] = c
    return ctor, inits, obj_names, live


def flatten(img, live, obj_names, name, seen=None):
    c = live[name]
    parent = obj_names.get(c["parent_obj"]) if c["parent_obj"] else None
    inherited = flatten(img, live, obj_names, parent) if parent in live else []
    return inherited + members_of(img, c, obj_names)


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    img = Image(sys.argv[1])
    args = sys.argv[2:]
    jpath = None
    if "--json" in args:
        i = args.index("--json"); jpath = args[i + 1]; del args[i:i + 2]
    ctor, inits, obj_names, live = load(img)
    print("hkClass::hkClass at 0x%08X (+0x%X); %d initializers, %d live classes"
          % (ctor, ctor - img.base, len(inits), len(live)))
    names = sorted(live) if args == ["--all"] else args
    out = {}
    for n in names:
        if n not in live:
            print("*** %s: no live hkClass (objectSize > 0) registered" % n)
            continue
        c = live[n]
        parent = obj_names.get(c["parent_obj"]) if c["parent_obj"] else None
        mem = flatten(img, live, obj_names, n)
        out[n] = {"size": c["size"], "parent": parent, "version": c["version"], "members": mem}
        print("%s : %s   size %d  version %d" % (n, parent or "-", c["size"], c["version"]))
        for m in mem:
            sub = "" if m["subtype"] == "VOID" else "<%s>" % m["subtype"]
            cls = (" " + m["class"]) if m["class"] else ""
            arr = ("[%d]" % m["carray"]) if m["carray"] else ""
            print("   +%-4d %-30s %s%s%s%s" % (m["offset"], m["name"], m["type"], sub, cls, arr))
    if jpath:
        with open(jpath, "w") as f:
            json.dump(out, f, indent=1)
        print("wrote", jpath)
    return 0


if __name__ == "__main__":
    sys.exit(main())
