# TK-4 (static, BS1): WHO TICKS THE TELEKINESIS STATE MACHINE, WHERE DOES ITS VIEW RAY COME FROM, AND WHAT
# CONSTRUCTS AN OBJECT? (1) every reader of the global active-telekinesis list that the start function appends to
# (VA 0x11C561FC) - the per-tick driver; (2) the three callees of the origin/direction helper +0x1C0F70
# (+0x1B6BA0, +0x1BE660, +0x1BE7A0) and every caller of +0x1BE7A0, and the ability fire start +0x1BC220 for
# comparison; (3) the GNatives slot for EX_New (0x11) and the C++ it calls (StaticConstructObject), plus string
# hits for StaticConstructObject / DuplicateObject / abstract; (4) the enter helpers the states call and the
# damage-listener factory; (5) the GameInfo GetCurrentTelekinesisTarget site.
#     .\tools\ida-run.ps1 -Game bs1 tools\ida\tk4_driver_ray_construct.py
# - Output path from BVR_IDA_OUT (outside the repo); decompiles are never committed. Findings go to the docs.
# - Every address is printed as RVA and VA. Negative paths print NONE / NO FUNCTION AT / DECOMPILE FAILED / CUT.
# - The known-good check re-derives ProcessEvent and refuses on a mismatch.
# - Save without a UTF-8 BOM.
import os
import idaapi, idautils, idc, ida_auto, ida_bytes, ida_funcs, ida_hexrays, ida_name, ida_nalt, ida_search

KNOWN = {
    "bs1": ("ProcessEvent",       0x375140, "558BEC8B81F8000000"),
    "bs2": ("ProcessEvent",       0x37A7E0, "558BEC8B810C010000"),
    "bsi": ("GetPlayerViewPoint", 0x1E10C0, "558BEC83E4F081ECA4000000"),
}
OUT_PATH = os.environ.get("BVR_IDA_OUT")
GAME = os.environ.get("BVR_IDA_GAME", "")
OUT = []


def w(s=""):
    OUT.append(str(s))


def done(code=0):
    if OUT_PATH:
        with open(OUT_PATH, "w", encoding="utf-8") as fh:
            fh.write("\n".join(OUT) + "\n")
    idc.qexit(code)


if not OUT_PATH or GAME not in KNOWN:
    print("*** BVR_IDA_OUT / BVR_IDA_GAME not set - run this through tools\ida-run.ps1 ***")
    idc.qexit(2)

ida_auto.auto_wait()
base = idaapi.get_imagebase()
hr = ida_hexrays.init_hexrays_plugin()
w("game %s  input %s  md5 %s" % (GAME, ida_nalt.get_root_filename(), (ida_nalt.retrieve_input_file_md5() or b"").hex()))
w("imagebase 0x%X  hexrays %s" % (base, "OK" if hr else "*** MISSING ***"))
kname, krva, kbytes = KNOWN[GAME]
want = bytes.fromhex(kbytes)
got = ida_bytes.get_bytes(base + krva, len(want)) or b""
if got != want:
    w("*** REFUSED: known-good %s bytes mismatch ***" % kname)
    done(3)
w("known-good %s +0x%X bytes match" % (kname, krva))


def a(ea):
    return "+0x%X (0x%08X)" % (ea - base, ea)


def fl(ea):
    f = ida_funcs.get_func(ea)
    if not f:
        return "NO FUNCTION AT %s" % a(ea)
    return "%s %s" % (a(f.start_ea), ida_funcs.get_func_name(f.start_ea))




def decomp(ea, limit=160):
    f = ida_funcs.get_func(ea)
    if not f:
        w("*** NO FUNCTION AT %s ***" % a(ea))
        return
    w("")
    w("##### DECOMPILE %s" % fl(f.start_ea))
    try:
        lines = str(ida_hexrays.decompile(f.start_ea)).split("\n")
    except Exception as e:
        lines = ["DECOMPILE FAILED: %s" % e]
    for i, l in enumerate(lines[:limit]):
        w("  %4d| %s" % (i + 1, l))
    if len(lines) > limit:
        w("  ... CUT: showing 1-%d of %d" % (limit, len(lines)))


def callers(ea, n=10):
    xs = [x.frm for x in idautils.XrefsTo(ea)]
    return ", ".join(("%s<%s>" % (("+0x%X" % (x - base)), (ida_funcs.get_func_name(ida_funcs.get_func(x).start_ea) if ida_funcs.get_func(x) else "data"))) for x in xs[:n]) + (" ...(%d)" % len(xs) if len(xs) > n else "")


def find_all(pat):
    hits = []
    for s in idautils.Segments():
        start, end = idc.get_segm_start(s), idc.get_segm_end(s)
        data = ida_bytes.get_bytes(start, end - start) or b""
        i = data.find(pat)
        while i >= 0:
            hits.append(start + i)
            i = data.find(pat, i + 1)
    return hits


def rtti_vtables(mangled):
    res = []
    for td_name in find_all(mangled.encode() + b"\0"):
        td = td_name - 8
        for x in idautils.XrefsTo(td):
            col = x.frm - 12
            if ida_bytes.get_dword(col) != 0:
                continue
            for y in idautils.XrefsTo(col):
                res.append(y.frm + 4)
    return res


def vt_slots(vt, maxn=64):
    out = []
    ea = vt
    while len(out) < maxn:
        d = ida_bytes.get_dword(ea)
        if not ida_funcs.get_func(d):
            break
        if ea != vt and any(True for _ in idautils.XrefsTo(ea)):
            break
        out.append(d)
        ea += 4
    return out




def disasm(ea):
    f = ida_funcs.get_func(ea)
    if not f:
        w("*** NO FUNCTION AT %s ***" % a(ea))
        return
    items = list(idautils.FuncItems(f.start_ea))
    w("")
    w("##### DISASM %s (%d instructions)" % (fl(f.start_ea), len(items)))
    for it in items:
        raw = ida_bytes.get_bytes(it, idc.get_item_size(it)) or b""
        w("  %s  %-24s %s" % (a(it), raw.hex(" "), idc.generate_disasm_line(it, 0)))




def find_all(pat):
    hits = []
    for s in idautils.Segments():
        st, en = idc.get_segm_start(s), idc.get_segm_end(s)
        data = ida_bytes.get_bytes(st, en - st) or b""
        i = data.find(pat)
        while i >= 0:
            hits.append(st + i)
            i = data.find(pat, i + 1)
    return hits


def wide(s):
    return b"".join(bytes([ord(c), 0]) for c in s)


# (1) readers of the active list
LIST = 0x11C561FC - 0x10900000 + base
w("")
w("##### (1) references to the active-telekinesis list %s" % a(LIST))
fs = set()
for x in idautils.XrefsTo(LIST):
    f = ida_funcs.get_func(x.frm)
    w("  %s %s in %s" % (a(x.frm), idc.generate_disasm_line(x.frm, 0), fl(x.frm)))
    if f:
        fs.add(f.start_ea)
for f in sorted(fs):
    if f - base in (0x1C2450,):
        continue
    decomp(f, 140)
    w("  callers: %s" % callers(f))

# (2) the ray
for t in [0x1B6BA0, 0x1BE660, 0x1BE7A0, 0x1BC870, 0x1BC220]:
    decomp(base + t, 160)
    w("  callers: %s" % callers(base + t, 40))
for t in [0x1BE7A0]:
    disasm(base + t)

# (3) construct
GN = 0x11C78510 - 0x10900000 + base
for idx in (0x11,):
    slot = GN + 8 * idx
    w("")
    w("##### (3) GNatives[0x%X] slot %s, static value 0x%X" % (idx, a(slot), ida_bytes.get_dword(slot)))
    for x in idautils.XrefsTo(slot):
        w("  %s %s in %s" % (a(x.frm), idc.generate_disasm_line(x.frm, 0), fl(x.frm)))
        v = idc.get_operand_value(x.frm, 1)
        if ida_funcs.get_func(v):
            decomp(v, 80)
            for ea in idautils.FuncItems(v):
                if idc.print_insn_mnem(ea) == "call":
                    tg = idc.get_operand_value(ea, 0)
                    w("    call %s -> %s" % (a(ea), fl(tg)))
for s in ["StaticConstructObject", "DuplicateObject", "StaticAllocateObject", "is abstract", "abstract", "ConstructObject", "StaticFindObject"]:
    for enc, pat in (("A", s.encode()), ("W", wide(s))):
        hs = find_all(pat)
        for h in hs[:6]:
            xs = list(idautils.XrefsTo(h))
            w("  string %s %r at %s refs %d: %s" % (enc, s, a(h), len(xs), ", ".join(fl(x.frm) for x in xs[:4])))
        if not hs:
            w("  string %s %r: NONE" % (enc, s))

# (4) enter helpers / damage listener / other callers of the state machine
for t in [0x2B1CB0, 0x2B1420, 0x2B1390, 0x2B1300, 0x2B1270, 0x1C1880, 0x1C0BD0, 0x1CAD70]:
    decomp(base + t, 120)
    w("  callers: %s" % callers(base + t))

# (5) GetCurrentTelekinesisTarget
decomp(base + 0x49CB00, 1)
f = ida_funcs.get_func(base + 0x49D590)
w("  window around +0x49D590:")
ea = base + 0x49D590
for _ in range(12):
    ea = idc.prev_head(ea)
for _ in range(30):
    w("    %s  %s" % (a(ea), idc.generate_disasm_line(ea, 0)))
    ea = idc.next_head(ea)
w("")
w("done")
done(0)
