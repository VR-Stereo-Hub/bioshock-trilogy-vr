# TK-7 (static, BS1): (a) THE TELEKINESIS TICK INSIDE ASHOCKPLAYER'S TICK - which ability it ticks (tk6: the
# only "load [r+0E4h] then call [vtbl+4]" site, +0x1D01F7 in +0x1CFE70, an AShockPlayer vtable entry);
# (b) WHAT CAN CONSTRUCT AN OBJECT: the native registrations for UObject execNew / StaticConstructObject-style
# natives and for ShockPlayer GetAbilityFromClass, and the C++ they reach.
#     .\tools\ida-run.ps1 -Game bs1 tools\ida\tk7_tick_site_construct.py
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




def window(lo, hi):
    ea = base + lo
    w("")
    w("##### DISASM window +0x%X..+0x%X" % (lo, hi))
    while ea < base + hi:
        raw = ida_bytes.get_bytes(ea, idc.get_item_size(ea)) or b""
        w("  %s  %-22s %s" % (a(ea), raw.hex(" "), idc.generate_disasm_line(ea, 0)))
        ea = idc.next_head(ea, base + hi)
        if ea == idc.BADADDR:
            break


window(0x1D0140, 0x1D0240)

# the vtable slot of +0x1CFE70 in AShockPlayer's vtable (0xD82BB8)
vt = base + 0xD82BB8
for i in range(0, 0x400, 4):
    if ida_bytes.get_dword(vt + i) == base + 0x1CFE70:
        w("AShockPlayer vtable slot +0x%X holds +0x1CFE70" % i)
    if ida_bytes.get_dword(vt + i) == base + 0x1E9EB0:
        w("AShockPlayer vtable slot +0x%X holds +0x1E9EB0" % i)
# APawn vtable (0xD82824) slot of the same offset, to name the virtual
pv = base + 0xD82824
for off in (0x1B8, ):
    pass


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


for name in ["intUObjectexecNew", "intUObjectexecStaticConstructObject", "intAShockPlayerexecGetAbilityFromClass",
             "GetAbilityFromClass", "intUObjectexecDuplicateObject", "intAActorexecSpawn", "intUObjectexecFindObject",
             "intUObjectexecDynamicLoadObject", "intAShockPlayerexecDropObject"]:
    hs = find_all(wide(name) + b"\0\0") + find_all(name.encode() + b"\0")
    if not hs:
        w("string %r: NONE" % name)
    for h in hs:
        w("")
        w("string %r at %s" % (name, a(h)))
        for x in idautils.XrefsTo(h):
            if ida_funcs.get_func(x.frm):
                w("  code ref %s in %s" % (a(x.frm), fl(x.frm)))
                continue
            w("  data ref %s (native entry?)" % a(x.frm))
            for y in idautils.XrefsTo(x.frm + 4):
                v = idc.get_operand_value(y.frm, 1)
                w("    slot+4 %s %s" % (a(y.frm), idc.generate_disasm_line(y.frm, 0)))
                if ida_funcs.get_func(v) and name.startswith("int"):
                    decomp(v, 70)
                    for ea in idautils.FuncItems(v):
                        if idc.print_insn_mnem(ea) == "call":
                            tg = idc.get_operand_value(ea, 0)
                            w("      call %s -> %s" % (a(ea), fl(tg)))

decomp(base + 0x1CAD70, 60)
w("")
w("done")
done(0)
