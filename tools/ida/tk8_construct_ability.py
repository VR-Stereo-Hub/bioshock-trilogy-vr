# TK-8 (static, BS1): HOW IS AN ABILITY OBJECT MADE, AND WHAT DOES A FRESH TELEKINESIS ABILITY NEED? Decompiles
# the tail of UObject::execNew (+0x733250) and the C++ it hands the class to (+0x700500, the StaticConstructObject
# candidate), AShockPlayer's GetAbilityFromClass C++ (+0x1D8540, behind intAShockPlayerexecGetAbilityFromClass), the
# telekinesis class's internal constructor (+0x1C36E0) and destructor (+0x1C35E0), and the player-tick TK gate
# constants (flt_11CA782C accumulator, flt_1174E69C step) by value.
#     .\tools\ida-run.ps1 -Game bs1 tools\ida\tk8_construct_ability.py
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





import struct


def decomp_tail(ea, first, limit=80):
    f = ida_funcs.get_func(ea)
    if not f:
        w("*** NO FUNCTION AT %s ***" % a(ea))
        return
    w("")
    w("##### DECOMPILE %s from line %d" % (fl(f.start_ea), first))
    try:
        lines = str(ida_hexrays.decompile(f.start_ea)).split("\n")
    except Exception as e:
        lines = ["DECOMPILE FAILED: %s" % e]
    for i, l in enumerate(lines[first - 1:first - 1 + limit]):
        w("  %4d| %s" % (first + i, l))
    w("  ... showing %d-%d of %d" % (first, min(len(lines), first - 1 + limit), len(lines)))


decomp_tail(base + 0x733250, 60, 30)
decomp(base + 0x700500, 60)
w("  callers: %s" % callers(base + 0x700500, 30))
f = ida_funcs.get_func(base + 0x700500)
if f:
    w("  prologue bytes %s" % (ida_bytes.get_bytes(f.start_ea, 12) or b"").hex(" "))
    w("  last insn %s" % idc.generate_disasm_line(idc.prev_head(f.end_ea), 0))
decomp(base + 0x1D8540, 120)
w("  callers: %s" % callers(base + 0x1D8540, 30))
decomp(base + 0x1C36E0, 40)
decomp(base + 0x1C35E0, 60)
for nm, va_ in (("accumulator flt_11CA782C", 0x11CA782C), ("step flt_1174E69C", 0x1174E69C)):
    ea = va_ - 0x10900000 + base
    v = ida_bytes.get_dword(ea)
    w("%s at %s = 0x%08X = %f" % (nm, a(ea), v, struct.unpack("<f", struct.pack("<I", v))[0]))
w("")
w("done")
done(0)
