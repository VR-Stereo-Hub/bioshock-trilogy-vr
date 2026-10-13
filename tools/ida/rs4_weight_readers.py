# RS-4 (static, BS1): WHICH FUNCTION IS THE CPU SKINNER? Every reader of the 1/255 weight constant (disasm-rva.py float 0.003921569 + xref), decompiled, to tell a vertex-weight loop from a colour unpack.
# section-transform "bake" functions (+0x3DBF10, +0x3EDC40, +0x60C5B0; ENGINE_NOTES session
# 21 calls them skinning strategies) and the draw issuer above them (+0x60ECCA's function,
# then +0x77DC1E and +0x7661B3, the cb commit and the DrawIndexed site), decompiled in full,
# so the palette question can be answered from the code: is there a bone palette in a
# constant buffer (GPU skinning), or are vertices skinned on the CPU and uploaded?
#     .\tools\ida-run.ps1 -Game bs1 tools\ida\rs4_weight_readers.py
import os
import idaapi, idautils, idc, ida_auto, ida_bytes, ida_funcs, ida_hexrays, ida_name, ida_nalt

TARGETS = {"bs1": [0x40690, 0xAC5A2C, 0xAC7C50, 0xB0D53F, 0xB21E8D, 0xB22BF2]}
KNOWN = {"bs1": ("ProcessEvent", 0x375140, "558BEC8B81F8000000")}

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
hr = ida_hexrays.init_hexrays_plugin()
w("game %s  input %s  md5 %s" % (GAME, ida_nalt.get_root_filename(), (ida_nalt.retrieve_input_file_md5() or b"").hex()))
w("imagebase 0x%X  hexrays %s" % (base, "OK" if hr else "*** MISSING ***"))
kname, krva, kbytes = KNOWN[GAME]
want = bytes.fromhex(kbytes)
if (ida_bytes.get_bytes(base + krva, len(want)) or b"") != want:
    w("*** REFUSED: known-good bytes do not match")
    done(3)
w("known-good %s +0x%X bytes match" % (kname, krva))
a = lambda ea: "+0x%X (0x%08X)" % (ea - base, ea)
fname = lambda ea: ida_name.get_name(ea) or ("sub_%X" % ea)


def decomp(ea, limit=900):
    f = ida_funcs.get_func(ea)
    if not f:
        w("*** NO FUNCTION AT %s ***" % a(ea))
        return
    w("")
    w("##### DECOMPILE %s (function %s %s, %d bytes)" % (a(ea), a(f.start_ea), fname(f.start_ea), f.end_ea - f.start_ea))
    try:
        lines = str(ida_hexrays.decompile(f.start_ea)).split("\n")
    except Exception as e:
        lines = ["DECOMPILE FAILED: %s" % e]
    for i, l in enumerate(lines[:limit]):
        w("  %4d| %s" % (i + 1, l))
    if len(lines) > limit:
        w("  ... CUT at %d of %d lines" % (limit, len(lines)))
    # direct callees, so the next script knows where to go
    seen = []
    for it in idautils.FuncItems(f.start_ea):
        if idc.print_insn_mnem(it) != "call":
            continue
        tgt = idc.get_operand_value(it, 0)
        if ida_funcs.get_func(tgt) and tgt not in seen:
            seen.append(tgt)
    w("  callees: " + (", ".join(a(t) for t in seen) if seen else "NONE"))


for t in TARGETS.get(GAME, []):
    decomp(base + t)
done(0)
