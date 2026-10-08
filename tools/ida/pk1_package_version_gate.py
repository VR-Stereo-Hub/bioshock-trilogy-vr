# PK-1 (static, BS1): which package versions does the shipping loader accept? Finds every
# instruction that uses the package magic 0x9E2A83C1 as an immediate, and decompiles each
# function that does - the summary reader and the linker's checks around it - so the
# version/licensee compares can be read off instead of assumed.
#     .\tools\ida-run.ps1 -Game bs1 tools\ida\pk1_package_version_gate.py
# The shipping exe carries no loader error strings, so the compare constants are the only
# record of the gate. Decides whether a package from the 2007 SDK (docs/MODDING_SDKS.md)
# can load in Remastered at all.
import os
import idaapi, idautils, idc, ida_auto, ida_bytes, ida_funcs, ida_hexrays, ida_search, ida_ua

KNOWN = {"bs1": ("ProcessEvent", 0x375140, "558BEC8B81F8000000")}
MAGIC = 0x9E2A83C1

OUT_PATH = os.environ.get("BVR_IDA_OUT")
GAME = os.environ.get("BVR_IDA_GAME", "")
OUT = []
w = lambda s="": OUT.append(str(s))


def done(code=0):
    if OUT_PATH:
        with open(OUT_PATH, "w", encoding="utf-8") as fh:
            fh.write("\n".join(OUT) + "\n")
    idc.qexit(code)


if not OUT_PATH or GAME not in KNOWN:
    print("*** run through tools\\ida-run.ps1 -Game bs1")
    idc.qexit(2)
ida_auto.auto_wait()
base = idaapi.get_imagebase()
kname, krva, kbytes = KNOWN[GAME]
if (ida_bytes.get_bytes(base + krva, len(bytes.fromhex(kbytes))) or b"") != bytes.fromhex(kbytes):
    w("*** REFUSED: known-good bytes do not match")
    done(3)
ida_hexrays.init_hexrays_plugin()
a = lambda ea: "+0x%X (0x%08X)" % (ea - base, ea)

# Every code use of the magic as an immediate operand. The four bytes are searched and
# each hit is kept only when it decodes as part of an instruction that carries it.
needle = MAGIC.to_bytes(4, "little")
seg = idaapi.get_segm_by_name(".text")
funcs, uses = [], []
ea = seg.start_ea
while True:
    ea = ida_bytes.find_bytes(needle, ea, seg.end_ea)
    if ea == idaapi.BADADDR:
        break
    head = idc.get_item_head(ea)
    if ida_bytes.is_code(ida_bytes.get_flags(head)):
        uses.append(head)
        f = ida_funcs.get_func(head)
        if f and f.start_ea not in funcs:
            funcs.append(f.start_ea)
    ea += 1

w("##### %d code use(s) of 0x%08X in %d function(s)" % (len(uses), MAGIC, len(funcs)))
for u in uses:
    w("  %s  %s" % (a(u), idc.generate_disasm_line(u, 0)))
for fea in funcs:
    fn = ida_funcs.get_func(fea)
    w("")
    w("##### FUNCTION %s size %d" % (a(fea), fn.end_ea - fn.start_ea))
    try:
        lines = str(ida_hexrays.decompile(fea)).split("\n")
    except Exception as e:
        lines = ["DECOMPILE FAILED: %s" % e]
    for l in lines[:600]:
        w("  | " + l)
    # Small immediates compared in the function: the version and licensee gates are
    # compares of the summary's u16 fields against constants.
    cmps = []
    for it in idautils.FuncItems(fea):
        if idc.print_insn_mnem(it) == "cmp" and idc.get_operand_type(it, 1) == idc.o_imm:
            v = idc.get_operand_value(it, 1)
            if 0 < v < 0x400:
                cmps.append("%s cmp ..., %d (0x%X)" % (a(it), v, v))
    w("  compares against small constants:")
    for c in cmps:
        w("    " + c)
done(0)
