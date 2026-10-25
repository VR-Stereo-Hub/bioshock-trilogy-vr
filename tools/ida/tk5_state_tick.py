# TK-5 (static, BS1): WHAT CALLS THE TELEKINESIS STATE TICK? The state objects' slot 1 is Tick(ability, dt). This
# disassembles the code IDA left undefined between +0x1C1880 and +0x1C1A60 (tk4: list references and data refs
# into it), defines functions there, decompiles them with their callers, and scans the whole text segment for the
# shape "load [reg+0E4h] (CurrentState) then call [vtbl+4]" to name every site that ticks a state.
#     .\tools\ida-run.ps1 -Game bs1 tools\ida\tk5_state_tick.py
# - Output path from BVR_IDA_OUT (outside the repo); decompiles are never committed. Findings go to the docs.
# - Every address is printed as RVA and VA. Negative paths print NONE / NO FUNCTION AT / DECOMPILE FAILED / CUT.
# - The known-good check re-derives ProcessEvent and refuses on a mismatch.
# - Save without a UTF-8 BOM. This script only ADDS function definitions to the database (no byte patches);
#   the ones it adds start on int3 padding and are unreferenced orphans, so tk6 deletes them again. Its useful
#   result is the scan (it found nothing within 6 instructions; tk6 widens it to 14 and finds the site).
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




w("")
w("##### raw listing +0x1C1880..+0x1C1A60")
ea = base + 0x1C1880
while ea < base + 0x1C1A60:
    if not idc.is_code(ida_bytes.get_flags(ea)):
        idc.create_insn(ea)
    sz = idc.get_item_size(ea) or 1
    raw = ida_bytes.get_bytes(ea, sz) or b""
    fn = ida_funcs.get_func(ea)
    xs = [x.frm for x in idautils.XrefsTo(ea) if x.type not in (idc.fl_F,)]
    w("  %s %-22s %-50s %s%s" % (a(ea), raw.hex(" "), idc.generate_disasm_line(ea, 0), "" if fn else "[nofunc]",
                                (" <- " + ",".join("+0x%X" % (x - base) for x in xs[:4])) if xs else ""))
    ea += sz

w("")
w("##### define functions in the gap")
for st in [0x1C18AB, 0x1C18B0, 0x1C1950, 0x1C1990, 0x1C19C0, 0x1C19F0, 0x1C1A20]:
    pass
ea = base + 0x1C18AB
made = []
while ea < base + 0x1C1A60:
    if not ida_funcs.get_func(ea) and idc.is_code(ida_bytes.get_flags(ea)):
        prev = idc.prev_head(ea)
        pm = idc.print_insn_mnem(prev) if prev != idc.BADADDR else ""
        if pm in ("retn", "jmp", "int3", "nop", "align", "") or ida_bytes.get_byte(prev) in (0xCC, 0xC3, 0xC2):
            if ida_funcs.add_func(ea):
                made.append(ea)
                f = ida_funcs.get_func(ea)
                w("  defined %s .. +0x%X" % (a(ea), f.end_ea - base))
                ea = f.end_ea
                continue
    ea = idc.next_head(ea, base + 0x1C1A60)
    if ea == idc.BADADDR:
        break
for m in made:
    decomp(m, 120)
    w("  callers: %s" % callers(m))

w("")
w("##### sites: mov r,[r+0E4h] ... call [r+4] within 6 instructions")
seg = idaapi.get_segm_by_name(".text")
n = 0
for f_ea in idautils.Functions(seg.start_ea, seg.end_ea):
    items = list(idautils.FuncItems(f_ea))
    for i, it in enumerate(items):
        if idc.print_insn_mnem(it) != "mov":
            continue
        if idc.get_operand_type(it, 1) != idc.o_displ or idc.get_operand_value(it, 1) != 0xE4:
            continue
        for j in range(i + 1, min(i + 7, len(items))):
            t = items[j]
            if idc.print_insn_mnem(t) == "call" and idc.get_operand_type(t, 0) == idc.o_displ and idc.get_operand_value(t, 0) == 4:
                n += 1
                w("  %s  %s ... %s  in %s" % (a(it), idc.generate_disasm_line(it, 0), idc.generate_disasm_line(t, 0), fl(it)))
                break
w("  %d site(s)" % n)
w("")
w("done")
done(0)
