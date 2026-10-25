# TK-9 (static, BS1): THE BYTES A TELEKINESIS HOOK VERIFIES. Disassembly with bytes of the three state Tick slots
# (Pulling +0x2B0400, Holding +0x2B09A0, Throwing +0x2B0E20 - their stack cleanup is the calling convention a mod
# tick must use), the call sites of the view-ray helper +0x1C0F70 (+0x1C1258, +0x1C1AEB, +0x1C1FF1) with the
# instructions around them, the heads of the StartedUsingAbility / UseAbilityRelease exec thunks, and the target
# filters the pick uses (+0x1C3390, +0x1C3300) and the player call the throw makes (+0x1CF630).
#     .\tools\ida-run.ps1 -Game bs1 tools\ida\tk9_hook_bytes.py
# - Output path from BVR_IDA_OUT (outside the repo); decompiles are never committed. Findings go to the docs.
# - Every address is printed as RVA and VA. Negative paths print NO FUNCTION AT.
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




for t in [0x2B0400, 0x2B09A0, 0x2B0E20]:
    disasm(base + t)
for site in [0x1C1258, 0x1C1AEB, 0x1C1FF1]:
    w("")
    w("##### call site +0x%X" % site)
    ea = base + site
    for _ in range(6):
        ea = idc.prev_head(ea)
    for _ in range(10):
        raw = ida_bytes.get_bytes(ea, idc.get_item_size(ea)) or b""
        w("  %s  %-22s %s" % (a(ea), raw.hex(" "), idc.generate_disasm_line(ea, 0)))
        ea = idc.next_head(ea)
for t in [0x1C0D30, 0x1C0DE0, 0x1C0F70, 0x700500, 0x1D8540, 0x1C1740]:
    f = ida_funcs.get_func(base + t)
    last = idc.prev_head(f.end_ea) if f else None
    w("head +0x%X: %s   last: %s" % (t, (ida_bytes.get_bytes(base + t, 12) or b"").hex(" "),
                                     idc.generate_disasm_line(last, 0) if last else "NO FUNCTION"))
for t in [0x1C3390, 0x1C3300, 0x1CF630]:
    decomp(base + t, 60)
w("")
w("done")
done(0)
