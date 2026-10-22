# TEMPLATE-DERIVED (static): HOW DOES UObject::FindFunction WALK A CLASS'S FIELDS IN THIS BUILD?
# Decompiles FindFunction (patterns.h kFindFunctionRva) and the functions it calls, so the offsets
# of UStruct::Children, UField::Next and UStruct::SuperField (TFieldIterator) can be read off. For
# the cheats: script properties by name (a weapon's upgrade stats). UProperty::Offset is then found
# at runtime against two known answers (Actor Location 0x1D8, DrawScale 0x2AC).
#     .\tools\ida-run.ps1 -Game bs1 tools\ida\rf1_field_chain.py
#
# Rules this skeleton encodes (docs/IDA_WORKFLOW.md says why each one exists): auto_wait() first,
# qexit(0) last; output OUTSIDE the repo (BVR_IDA_OUT); RVAs and VAs; every negative path prints;
# the known-good check refuses on a mismatch; this game only. Ported from the Dishonored VR mod.
import os
import idaapi, idautils, idc, ida_auto, ida_bytes, ida_funcs, ida_hexrays, ida_nalt, ida_ua

TARGETS = {"bs1": [0x6F5F70]}   # UObject::FindFunction (tools\ida\mr2)

KNOWN = {
    "bs1": ("ProcessEvent",       0x375140, "558BEC8B81F8000000"),
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
    print("*** BVR_IDA_OUT / BVR_IDA_GAME not set - run this through tools\\ida-run.ps1 ***")
    idc.qexit(2)

ida_auto.auto_wait()
base = idaapi.get_imagebase()
hr = ida_hexrays.init_hexrays_plugin()
w("game %s  input %s  md5 %s" % (GAME, ida_nalt.get_root_filename(), (ida_nalt.retrieve_input_file_md5() or b"").hex()))
w("imagebase 0x%X  hexrays %s" % (base, "OK" if hr else "*** MISSING (no x86 decompiler licence?) ***"))
k = KNOWN[GAME]
got = ida_bytes.get_bytes(base + k[1], len(k[2]) // 2) or b""
if got.hex().upper() != k[2]:
    w("known-good %s at RVA 0x%X: *** MISMATCH %s ***" % (k[0], k[1], got.hex()))
    done(1)
w("known-good %s at RVA 0x%X: OK" % (k[0], k[1]))


def dump(ea, depth):
    f = ida_funcs.get_func(ea)
    if not f:
        w("NO FUNCTION AT RVA 0x%X" % (ea - base))
        return
    w("")
    w("=== %s RVA 0x%X (VA 0x%X) head %s" % (ida_funcs.get_func_name(f.start_ea), f.start_ea - base, f.start_ea,
                                          (ida_bytes.get_bytes(f.start_ea, 12) or b"").hex()))
    if hr:
        try:
            w("\n".join(str(ida_hexrays.decompile(f.start_ea)).splitlines()[:80]))
        except Exception as e:
            w("DECOMPILE FAILED: %s" % e)
    w("--- disassembly")
    callees = []
    for h in idautils.FuncItems(f.start_ea):
        w("  RVA 0x%X  %s" % (h - base, idc.GetDisasm(h)))
        if idc.print_insn_mnem(h) == "call":
            t = idc.get_operand_value(h, 0)
            if ida_funcs.get_func(t) and t not in callees:
                callees.append(t)
    if depth > 0:
        for t in callees[:6]:
            dump(t, depth - 1)


for rva in TARGETS[GAME]:
    dump(base + rva, 1)
done(0)
