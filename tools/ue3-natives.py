# ue3-natives.py - offline UE3 native-function and vtable resolver for a PE32 image.
# BioShock Infinite only: it is the trilogy's one UE3 game (build 6829). BS1 and BS2
# are Vengeance (UE2.5), whose native table has a different shape - see "nativemap" in
# docs/bioshock1/ENGINE_NOTES.md.
#
# NOTHING IT PRINTS MAY BE COMMITTED. The output is game-derived content, which
# the project's hard rule forbids in the repo - summarize findings in
# docs/bioshockinfinite/ENGINE_NOTES.md instead. The TOOL is ours and is committed;
# its OUTPUT is 2K's and is not. Same footing as tools/disasm-rva.py.
#
# WHAT IT DOES. UE3 registers every native exec function as a {const char* name,
# void* thunk} pair ("A<Class>exec<Function>" -> the exec thunk), one block per class
# separated by {0,0} sentinels (Infinite ENGINE_NOTES, session 36 correction). An exec
# thunk is never a hook target - native C++ callers bypass it - but it usually ends by
# dispatching to the implementation, so it is the cheapest route from a function NAME
# to code. `natives` lists the table; `vtable` dumps a vtable's slots.
#
# EVERY RUN VERIFIES FIRST. It re-derives a pair Infinite's ENGINE_NOTES already
# publishes (APlayerControllerexecGetPlayerViewPoint -> thunk RVA 0x129280) and refuses
# to print anything if it does not come back. A route that cannot reproduce a known
# answer is not evidence about a new one. A build with no published pair is refused.
#
# Addresses are printed as RVA and as VA at the image's PREFERRED base; the running game
# may load elsewhere, so patterns.h stores RVAs.
#
# Requires capstone only through disasm-rva.py's import (`pip install capstone`).
# Ported from the Dishonored VR mod's ue3-natives.py, whose `class` mode (UTF-16 class
# metadata -> constructor -> vtable) is NOT carried over: that route was established on
# Dishonored's build and has never been checked against Infinite's.
#
# Usage (addresses hex, with or without 0x):
#   python tools/ue3-natives.py <BioShockInfinite.exe> natives --grep GetPlayerViewPoint
#   python tools/ue3-natives.py <exe> natives --name APlayerControllerexecGetPlayerViewPoint
#   python tools/ue3-natives.py <exe> vtable 0xDE6FC8 [--slot 8] [--count 120]   (an RVA)
#
# NOT YET RUN against Infinite: written on a machine without it installed. The first run
# is its own test - the verify line either says the pair came back or it refuses.

import argparse
import os
import re
import struct
import sys

try:
    import importlib.util
    _spec = importlib.util.spec_from_file_location(
        "disasm_rva", os.path.join(os.path.dirname(os.path.abspath(__file__)), "disasm-rva.py"))
    _dr = importlib.util.module_from_spec(_spec)
    _spec.loader.exec_module(_dr)
except Exception as exc:                                   # pragma: no cover
    sys.exit("could not load tools/disasm-rva.py beside this script: %s" % exc)

Pe = _dr.Pe
find_pattern = _dr.find_pattern

# Per build, keyed by PE TimeDateStamp (bioshockinf/patterns.h kHostTimeDateStamp):
# one native name and the thunk RVA ENGINE_NOTES publishes for it.
KNOWN = {
    0x627BE455: ("BioShock Infinite (Steam, 2022-05-11)",
                 "APlayerControllerexecGetPlayerViewPoint", 0x129280),
}


def parse_hex(s):
    return int(s, 16)


class Image(object):
    def __init__(self, path):
        self.pe = Pe(path)
        self.base = self.pe.image_base
        d = self.pe.data
        pe = struct.unpack_from("<I", d, 0x3C)[0]
        self.timestamp = struct.unpack_from("<I", d, pe + 8)[0]

    def dword_rva(self, rva):
        b = self.pe.read(rva, 4)
        return struct.unpack("<I", b)[0] if len(b) == 4 else None

    def to_rva(self, va):
        return None if va is None else va - self.base

    def is_code_va(self, va):
        if va is None or not (self.base <= va < self.base + self.pe.size_of_image):
            return False
        s = self.pe.section_of(va - self.base)
        return bool(s and s["exec"])

    def refs_to_va(self, va):
        return [r for _, r in find_pattern(self.pe, struct.pack("<I", va))]

    def a(self, rva):
        return "+0x%06X (0x%08X)" % (rva, self.base + rva)


def natives(img):
    """Every "<Class>exec<Function>" string with the code dword stored beside a
    pointer to it. The name must be NUL-terminated on both sides, so a suffix pooled
    inside a longer string is not mistaken for an entry."""
    pat = re.compile(rb'(?<=\x00)[A-Za-z_][A-Za-z0-9_]{1,90}exec[A-Za-z0-9_]{2,60}\x00')
    out = []
    for s in img.pe.sections:
        blob = img.pe.data[s["rawptr"]:s["rawptr"] + s["rawsize"]]
        for m in pat.finditer(blob):
            name_rva = s["vaddr"] + m.start()
            name = m.group()[:-1].decode("ascii", "replace")
            thunk = None
            for ref in img.refs_to_va(img.base + name_rva):
                cand = img.dword_rva(ref + 4)
                if img.is_code_va(cand):
                    thunk = img.to_rva(cand)
                    break
            out.append((name_rva, name, thunk))
    return out


def verify(img, rows):
    k = KNOWN.get(img.timestamp)
    if not k:
        return None, ["no published pair for PE TimeDateStamp 0x%08X - not a build ENGINE_NOTES "
                      "knows. Derive one by hand and add it to KNOWN first." % img.timestamp]
    label, name, want = k
    got = [t for _, n, t in rows if n == name]
    if len(got) != 1:
        return label, ["expected exactly one %s, found %d" % (name, len(got))]
    if got[0] != want:
        return label, ["%s thunk: got %s, ENGINE_NOTES says +0x%X"
                       % (name, ("+0x%X" % got[0]) if got[0] is not None else "NONE", want)]
    return label, []


def cmd_natives(img, rows, a):
    print("native exec registrations: %d" % len(rows))
    for rva, name, thunk in rows:
        if a.name and name != a.name:
            continue
        if a.grep and a.grep.lower() not in name.lower():
            continue
        print("  name %s  thunk %s  %s" % (img.a(rva), img.a(thunk) if thunk is not None else "?", name))


def cmd_vtable(img, rows, a):
    vt = parse_hex(a.vtable)
    if a.slot is not None:
        off = parse_hex(a.slot)
        v = img.dword_rva(vt + off)
        print("%s +0x%03X -> %s" % (img.a(vt), off, img.a(img.to_rva(v)) if v else "NONE"))
        return
    for i in range(a.count):
        v = img.dword_rva(vt + 4 * i)
        if v is None:
            break
        code = img.is_code_va(v)
        print("  [%3d] +0x%03X  %s%s" % (i, i * 4, img.a(img.to_rva(v)) if code else "0x%08X" % v,
                                         "  (code)" if code else ""))


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("exe")
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("natives"); p.add_argument("--grep"); p.add_argument("--name")
    p.set_defaults(fn=cmd_natives)
    p = sub.add_parser("vtable"); p.add_argument("vtable"); p.add_argument("--slot")
    p.add_argument("--count", type=int, default=120); p.set_defaults(fn=cmd_vtable)
    a = ap.parse_args()

    img = Image(a.exe)
    rows = natives(img)
    label, bad = verify(img, rows)
    if bad:
        for b in bad:
            print("VERIFY FAILED: %s" % b, file=sys.stderr)
        print("The route cannot reproduce a number already in ENGINE_NOTES, so nothing it says "
              "about a NEW name is evidence.", file=sys.stderr)
        return 2
    print("verify: %s - %s re-derived exactly" % (label, KNOWN[img.timestamp][1]))
    a.fn(img, rows, a)
    return 0


if __name__ == "__main__":
    sys.exit(main())
