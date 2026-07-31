# disasm-rva.py - offline 32-bit PE disassembly / constant-hunting helper.
#
# Every session that has derived an RVA on this project so far has done it with
# an ad-hoc capstone one-liner retyped from scratch (sessions 21, 26, 32). This
# is that one-liner, kept.
#
# NOTHING IT PRINTS MAY BE COMMITTED. The output is game-derived content, which
# the project's hard rule forbids in the repo - summarize findings in the
# per-game ENGINE_NOTES instead. The TOOL is ours and is committed; its OUTPUT
# is 2K's and is not. Same footing as tools/uscript/dump.ps1.
#
# Requires capstone (`pip install capstone`; 5.0.7 verified 2026-07-31).
#
# Usage (all RVAs hex, with or without 0x):
#   python tools/disasm-rva.py <exe> sections
#   python tools/disasm-rva.py <exe> dis   AECACF [--count 60] [--back 120]
#   python tools/disasm-rva.py <exe> bytes AECACF [--len 64]
#   python tools/disasm-rva.py <exe> float 60.0              # where is this constant?
#   python tools/disasm-rva.py <exe> search 3ACD133F         # raw hex byte pattern
#   python tools/disasm-rva.py <exe> xref  1152608           # who reads this address?
#   python tools/disasm-rva.py <exe> calls 4EE8D0            # who E8/E9s to this RVA?
#   python tools/disasm-rva.py <exe> disp  460 [--from AEC000 --to AED000]
#
# `disp` is the one that found BS1's foreground-FOV field: disassemble a range
# and report every instruction whose memory operand uses a given structure
# displacement. `float` + `xref` is the complementary route - locate the
# constant in .rdata, then find the code that loads it.

import argparse
import struct
import sys

try:
    from capstone import Cs, CS_ARCH_X86, CS_MODE_32, CS_OP_MEM
except ImportError:
    sys.exit("capstone is not installed: pip install capstone")


class Pe:
    """Minimal PE32 reader: sections, RVA<->file offset, image base."""

    def __init__(self, path):
        with open(path, "rb") as f:
            self.data = f.read()
        d = self.data
        if d[:2] != b"MZ":
            raise ValueError("not a PE (no MZ)")
        pe = struct.unpack_from("<I", d, 0x3C)[0]
        if d[pe:pe + 4] != b"PE\0\0":
            raise ValueError("not a PE (no PE signature)")
        machine, nsec = struct.unpack_from("<HH", d, pe + 4)
        if machine != 0x14C:
            raise ValueError("not 32-bit x86 (machine 0x%X) - both games are Win32" % machine)
        opt_size = struct.unpack_from("<H", d, pe + 20)[0]
        opt = pe + 24
        magic = struct.unpack_from("<H", d, opt)[0]
        if magic != 0x10B:
            raise ValueError("not PE32 (optional header magic 0x%X)" % magic)
        self.image_base = struct.unpack_from("<I", d, opt + 28)[0]
        self.size_of_image = struct.unpack_from("<I", d, opt + 56)[0]
        self.sections = []
        sh = opt + opt_size
        for i in range(nsec):
            o = sh + i * 40
            name = d[o:o + 8].rstrip(b"\0").decode("ascii", "replace")
            vsize, vaddr, rawsize, rawptr = struct.unpack_from("<IIII", d, o + 8)
            chars = struct.unpack_from("<I", d, o + 36)[0]
            self.sections.append({
                "name": name, "vaddr": vaddr, "vsize": vsize,
                "rawptr": rawptr, "rawsize": rawsize, "chars": chars,
                "exec": bool(chars & 0x20000000),
            })

    def section_of(self, rva):
        for s in self.sections:
            if s["vaddr"] <= rva < s["vaddr"] + max(s["vsize"], s["rawsize"]):
                return s
        return None

    def off(self, rva):
        """RVA -> file offset, or None if it is not backed by raw data."""
        s = self.section_of(rva)
        if not s:
            return None
        delta = rva - s["vaddr"]
        if delta >= s["rawsize"]:
            return None  # in the virtual tail (uninitialized), no bytes on disk
        return s["rawptr"] + delta

    def read(self, rva, n):
        o = self.off(rva)
        if o is None:
            return b""
        return self.data[o:o + n]


def parse_rva(s):
    return int(s, 16) if not s.lower().startswith("0x") else int(s, 16)


def md():
    m = Cs(CS_ARCH_X86, CS_MODE_32)
    m.detail = True
    return m


def disasm_all(m, blob, base_va):
    """Linear sweep that RESUMES past undecodable bytes.

    capstone's disasm() is a generator that simply STOPS at the first byte it
    cannot decode, and .text is full of jump tables, data, and instructions
    this build of capstone does not know. Sweeping a 16 MB .text without this
    silently covered only the first ~0x67000 bytes and reported '9 hits' as if
    that were the whole image - a truncation that reads exactly like a complete
    answer, which is the failure mode this project keeps writing down.
    """
    pos = 0
    n = len(blob)
    while pos < n:
        advanced = False
        for ins in m.disasm(blob[pos:], base_va + pos):
            yield ins
            pos += ins.size
            advanced = True
        if not advanced:
            pos += 1  # undecodable byte: step over it and resync


def cmd_sections(pe, _a):
    print("image base 0x%08X, size of image 0x%X" % (pe.image_base, pe.size_of_image))
    print("%-10s %-10s %-10s %-10s %-10s %s" % ("name", "rva", "vsize", "rawptr", "rawsize", "exec"))
    for s in pe.sections:
        print("%-10s 0x%08X 0x%08X 0x%08X 0x%08X %s" %
              (s["name"], s["vaddr"], s["vsize"], s["rawptr"], s["rawsize"],
               "X" if s["exec"] else "-"))


def cmd_dis(pe, a):
    rva = parse_rva(a.rva)
    start = max(0, rva - a.back)
    blob = pe.read(start, a.back + a.count * 8 + 16)
    if not blob:
        sys.exit("RVA 0x%X is not backed by raw data" % rva)
    m = md()
    shown = 0
    for ins in m.disasm(blob, pe.image_base + start):
        r = ins.address - pe.image_base
        mark = "  <== " if r == rva else "      "
        print("%s%08X  %-24s %s %s" %
              (mark, r, ins.bytes.hex().upper(), ins.mnemonic, ins.op_str))
        shown += 1
        if r >= rva and shown > a.count:
            break


def cmd_bytes(pe, a):
    rva = parse_rva(a.rva)
    blob = pe.read(rva, a.len)
    for i in range(0, len(blob), 16):
        row = blob[i:i + 16]
        hexs = " ".join("%02X" % b for b in row)
        asc = "".join(chr(b) if 32 <= b < 127 else "." for b in row)
        print("%08X  %-47s |%s|" % (rva + i, hexs, asc))
    if len(blob) >= 4:
        f = struct.unpack_from("<f", blob, 0)[0]
        print("as float at +0: %.7g   as u32: 0x%08X" %
              (f, struct.unpack_from("<I", blob, 0)[0]))


def find_pattern(pe, pat, only_exec=None):
    hits = []
    for s in pe.sections:
        if only_exec is not None and s["exec"] != only_exec:
            continue
        blob = pe.data[s["rawptr"]:s["rawptr"] + s["rawsize"]]
        i = blob.find(pat)
        while i >= 0:
            hits.append((s["name"], s["vaddr"] + i))
            i = blob.find(pat, i + 1)
    return hits


def cmd_float(pe, a):
    val = float(a.value)
    pat = struct.pack("<f", val)
    print("float %.7g encodes as %s" % (val, pat.hex().upper()))
    hits = find_pattern(pe, pat)
    for name, rva in hits:
        print("  %-10s rva 0x%08X   va 0x%08X" % (name, rva, pe.image_base + rva))
    print("%d occurrence(s). In .rdata/.data these are candidates for the constant "
          "pool; run `xref` on the promising ones. In .text they are immediates." % len(hits))


def cmd_search(pe, a):
    pat = bytes.fromhex(a.hex)
    hits = find_pattern(pe, pat)
    for name, rva in hits:
        print("  %-10s rva 0x%08X   va 0x%08X" % (name, rva, pe.image_base + rva))
    print("%d occurrence(s)." % len(hits))


def cmd_xref(pe, a):
    """Who references this ADDRESS as absolute data (the x86-32 common case)?"""
    rva = parse_rva(a.rva)
    va = pe.image_base + rva
    pat = struct.pack("<I", va)
    hits = find_pattern(pe, pat, only_exec=True)
    m = md()
    for name, at in hits:
        blob = pe.read(max(0, at - 16), 32)
        # Disassemble backwards-ish: print any instruction in the window whose
        # bytes cover the reference site, which is enough to identify the read.
        for ins in m.disasm(blob, pe.image_base + max(0, at - 16)):
            r = ins.address - pe.image_base
            if r <= at < r + ins.size:
                print("  %-8s ref@0x%08X in %08X  %-22s %s %s" %
                      (name, at, r, ins.bytes.hex().upper(), ins.mnemonic, ins.op_str))
                break
    print("%d absolute reference(s) to VA 0x%08X in executable sections." % (len(hits), va))


def cmd_calls(pe, a):
    """Who E8/E9s to this RVA? (Virtual dispatch will show zero - that is data.)"""
    target = parse_rva(a.rva)
    n = 0
    for s in pe.sections:
        if not s["exec"]:
            continue
        blob = pe.data[s["rawptr"]:s["rawptr"] + s["rawsize"]]
        for i in range(len(blob) - 5):
            if blob[i] not in (0xE8, 0xE9):
                continue
            disp = struct.unpack_from("<i", blob, i + 1)[0]
            site = s["vaddr"] + i
            if site + 5 + disp == target:
                print("  %s 0x%08X -> 0x%08X" %
                      ("call" if blob[i] == 0xE8 else "jmp ", site, target))
                n += 1
    print("%d static E8/E9 caller(s) of RVA 0x%08X." % (n, target))


def cmd_disp(pe, a):
    """Every instruction in a range whose memory operand uses this displacement."""
    want = parse_rva(a.disp)
    if a.frm and a.to:
        ranges = [(parse_rva(a.frm), parse_rva(a.to))]
    else:
        ranges = [(s["vaddr"], s["vaddr"] + s["rawsize"]) for s in pe.sections if s["exec"]]
    m = md()
    n = 0
    for lo, hi in ranges:
        blob = pe.read(lo, hi - lo)
        for ins in disasm_all(m, blob, pe.image_base + lo):
            for op in ins.operands:
                if op.type != CS_OP_MEM:
                    continue
                if op.mem.disp == want and op.mem.base != 0:
                    print("  %08X  %-22s %s %s" %
                          (ins.address - pe.image_base, ins.bytes.hex().upper(),
                           ins.mnemonic, ins.op_str))
                    n += 1
                    break
    print("%d instruction(s) with displacement 0x%X." % (n, want))


def main():
    p = argparse.ArgumentParser(
        description="offline 32-bit PE disassembly / constant-hunting helper "
                    "(see the header comment; its OUTPUT is never committed)")
    p.add_argument("exe")
    sub = p.add_subparsers(dest="cmd", required=True)

    sub.add_parser("sections").set_defaults(fn=cmd_sections)

    d = sub.add_parser("dis"); d.add_argument("rva")
    d.add_argument("--count", type=int, default=40)
    d.add_argument("--back", type=int, default=0)
    d.set_defaults(fn=cmd_dis)

    b = sub.add_parser("bytes"); b.add_argument("rva")
    b.add_argument("--len", type=int, default=64)
    b.set_defaults(fn=cmd_bytes)

    f = sub.add_parser("float"); f.add_argument("value"); f.set_defaults(fn=cmd_float)
    s = sub.add_parser("search"); s.add_argument("hex"); s.set_defaults(fn=cmd_search)
    x = sub.add_parser("xref"); x.add_argument("rva"); x.set_defaults(fn=cmd_xref)
    c = sub.add_parser("calls"); c.add_argument("rva"); c.set_defaults(fn=cmd_calls)

    dp = sub.add_parser("disp"); dp.add_argument("disp")
    dp.add_argument("--from", dest="frm", default=None)
    dp.add_argument("--to", dest="to", default=None)
    dp.set_defaults(fn=cmd_disp)

    a = p.parse_args()
    a.fn(Pe(a.exe), a)


if __name__ == "__main__":
    main()
