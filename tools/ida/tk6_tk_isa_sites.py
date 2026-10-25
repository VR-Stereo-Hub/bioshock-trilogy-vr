# TK-6 (static, BS1): WHICH NATIVE CODE TREATS AN ABILITY AS A TELEKINESIS ABILITY - and so where is it ticked?
# Every caller of UTelekinesisAbility::StaticClass (+0x1F2520, the getter whose first call registers the class
# 'TelekinesisAbility', size 0x13C, via +0x1C0BD0) is an IsA test for the class; each is decompiled with its
# callers. Also re-scans the text segment for "mov r,[r+0E4h]" followed within 14 instructions by "call [r+4]"
# (the state's Tick slot). First it removes the function definitions tk5 added in the padding at +0x1C18AB..
# +0x1C1A60 so the database matches auto-analysis again.
#     .\tools\ida-run.ps1 -Game bs1 tools\ida\tk6_tk_isa_sites.py
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




for st in [0x1C18AB, 0x1C1925, 0x1C1983, 0x1C19BB, 0x1C19EB, 0x1C1A5B]:
    f = ida_funcs.get_func(base + st)
    if f and f.start_ea == base + st:
        ida_funcs.del_func(base + st)
        w("removed tk5 definition at %s" % a(base + st))

SC = base + 0x1F2520
decomp(SC, 40)
sites = {}
for x in idautils.XrefsTo(SC):
    f = ida_funcs.get_func(x.frm)
    if f:
        sites.setdefault(f.start_ea, []).append(x.frm)
w("")
w("##### %d functions call UTelekinesisAbility::StaticClass" % len(sites))
for f in sorted(sites):
    w("  %s at %s  callers: %s" % (fl(f), ", ".join("+0x%X" % (s - base) for s in sites[f]), callers(f, 6)))
for f in sorted(sites):
    decomp(f, 90)

w("")
w("##### sites: mov r,[r+0E4h] ... call [r+4] within 14 instructions")
seg = idaapi.get_segm_by_name(".text")
n = 0
for f_ea in idautils.Functions(seg.start_ea, seg.end_ea):
    items = list(idautils.FuncItems(f_ea))
    for i, it in enumerate(items):
        if idc.print_insn_mnem(it) != "mov":
            continue
        if idc.get_operand_type(it, 1) != idc.o_displ or idc.get_operand_value(it, 1) != 0xE4:
            continue
        for j in range(i + 1, min(i + 15, len(items))):
            t = items[j]
            if idc.print_insn_mnem(t) == "call" and idc.get_operand_type(t, 0) == idc.o_displ and idc.get_operand_value(t, 0) == 4:
                n += 1
                w("  %s  %s ... %s  in %s" % (a(it), idc.generate_disasm_line(it, 0), idc.generate_disasm_line(t, 0), fl(it)))
                break
w("  %d site(s)" % n)
w("")
w("done")
done(0)
