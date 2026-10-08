# SK-3 (static, BS1): every function USkeletalMesh's serializer (SK-2, +0x3EE720) calls
# directly, decompiled in full, plus the serializer itself in full - so each TArray's
# element layout and every scalar's width can be read off instead of guessed.
#     .\tools\ida-run.ps1 -Game bs1 tools\ida\sk3_skelmesh_callees.py
# Callees are listed once each, in first-call order, with how many times they are called.
# Very large callees (over MAXSIZE bytes) are named but not decompiled.
import os
import idaapi, idautils, idc, ida_auto, ida_bytes, ida_funcs, ida_hexrays, ida_name, ida_ua

KNOWN = {"bs1": ("ProcessEvent", 0x375140, "558BEC8B81F8000000")}
ROOT_RVA = 0x3EE720
MAXSIZE = 4000

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
hr = ida_hexrays.init_hexrays_plugin()
a = lambda ea: "+0x%X (0x%08X)" % (ea - base, ea)


def decompile(ea, limit=2000):
    try:
        lines = str(ida_hexrays.decompile(ea)).split("\n")
    except Exception as e:
        return ["DECOMPILE FAILED: %s" % e]
    if len(lines) > limit:
        return lines[:limit] + ["... CUT at %d of %d lines" % (limit, len(lines))]
    return lines


root = base + ROOT_RVA
f = ida_funcs.get_func(root)
order, counts = [], {}
for it in idautils.FuncItems(f.start_ea):
    if idc.print_insn_mnem(it) != "call":
        continue
    tgt = idc.get_operand_value(it, 0)
    if not ida_funcs.get_func(tgt):
        continue
    if tgt not in counts:
        order.append(tgt)
        counts[tgt] = 0
    counts[tgt] += 1

w("##### ROOT %s, %d direct callees" % (a(root), len(order)))
for l in decompile(root):
    w("  | " + l)
for tgt in order:
    fn = ida_funcs.get_func(tgt)
    size = fn.end_ea - fn.start_ea
    w("")
    w("##### CALLEE %s size %d, called %d time(s)" % (a(tgt), size, counts[tgt]))
    if size > MAXSIZE:
        w("  (not decompiled: over %d bytes)" % MAXSIZE)
        continue
    for l in decompile(tgt, 400):
        w("  | " + l)
done(0)
