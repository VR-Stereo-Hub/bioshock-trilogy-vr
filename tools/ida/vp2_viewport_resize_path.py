# VP-2 (static, BS1): the resize path in full - the SETRES branch of UWindowsViewport::Exec, the
# - the wide strings SETRES / TOGGLEFULLSCREEN / "Resizing" and every code reference to them;
# - UWindowsViewport::Exec (+0x8525C0): the lines around its SETRES handling and the callee it
#   invokes with the parsed size (the resize function), decompiled in full with its callers;
# - the window procedure(s): every function that calls DefWindowProcW, decompiled, with the
#   lines handling WM_SIZE (5) / WM_SIZING (0x214) / WM_WINDOWPOSCHANGED (0x47) - does a
#   window resize reach the same resize function in windowed mode?
# - the function around the session-27 SETRES fault (+0x322353).
#     .\tools\ida-run.ps1 -Game bs1 tools\ida\vp1_viewport_resize.py
import os, re
import idaapi, idautils, idc, ida_auto, ida_bytes, ida_funcs, ida_hexrays, ida_name, ida_nalt

KNOWN = {"bs1": ("ProcessEvent", 0x375140, "558BEC8B81F8000000")}
EXEC_RVA = 0x8525C0
FAULT_RVA = 0x4C2353
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
w("game %s md5 %s imagebase 0x%X hexrays %s" % (GAME, (ida_nalt.retrieve_input_file_md5() or b"").hex(), base, "OK" if hr else "MISSING"))
a = lambda ea: "+0x%X (0x%08X)" % (ea - base, ea)
fname = lambda ea: ida_name.get_name(ea) or ("sub_%X" % ea)


def decompile(ea, limit=3000):
    try:
        lines = str(ida_hexrays.decompile(ea)).split("\n")
    except Exception as e:
        return ["DECOMPILE FAILED: %s" % e]
    if len(lines) > limit:
        return lines[:limit] + ["... CUT at %d of %d lines" % (limit, len(lines))]
    return lines


def find_wide(s):
    pat = s.encode("utf-16-le")
    out = []
    ea = 0
    while True:
        ea = ida_bytes.find_bytes(pat, ea + 1) if hasattr(ida_bytes, "find_bytes") else idc.find_binary(ea + 1, idc.SEARCH_DOWN, " ".join("%02X" % b for b in pat))
        if ea is None or ea == idaapi.BADADDR:
            break
        out.append(ea)
        if len(out) > 20:
            break
    return out


def callers(ea):
    f = ida_funcs.get_func(ea)
    res = []
    if not f:
        return res
    for x in idautils.XrefsTo(f.start_ea):
        cf = ida_funcs.get_func(x.frm)
        res.append((x.frm, cf.start_ea if cf else None))
    return res


def dump_func(ea, title, grep=None, ctx=25, limit=3000):
    f = ida_funcs.get_func(ea)
    if not f:
        w("*** NO FUNCTION AT %s (%s) ***" % (a(ea), title))
        return
    lines = decompile(f.start_ea, limit)
    w("")
    w("##### %s: function %s %s, %d bytes, %d decompiled lines" % (title, a(f.start_ea), fname(f.start_ea), f.end_ea - f.start_ea, len(lines)))
    if grep is None:
        for l in lines:
            w("  | " + l)
        return
    hit = [i for i, l in enumerate(lines) if re.search(grep, l)]
    if not hit:
        w("  (no line matches %r; first 60 lines follow)" % grep)
        for l in lines[:60]:
            w("  | " + l)
        return
    shown = set()
    for i in hit:
        lo, hi = max(0, i - ctx), min(len(lines), i + ctx)
        if lo in shown:
            continue
        w("  ---- around line %d" % (i + 1))
        for j in range(lo, hi):
            if j not in shown:
                w("  %4d| %s" % (j + 1, lines[j]))
                shown.add(j)



# Exec, the SETRES branch in full.
f = ida_funcs.get_func(base + EXEC_RVA)
lines = decompile(f.start_ea, 4000)
w("")
w("##### UWindowsViewport::Exec lines 150-345")
for i in range(149, min(345, len(lines))):
    w("  %4d| %s" % (i + 1, lines[i]))

# The helpers the fullscreen words call, and whatever SETRES calls.
for rva, title in [(0x857340, "ENDFULLSCREEN helper"), (0x857470, "TOGGLEFULLSCREEN helper")]:
    dump_func(base + rva, title, limit=400)
    w("  callers:")
    for frm, cf in callers(base + rva):
        w("    %s in %s" % (a(frm), (a(cf) + " " + fname(cf)) if cf else "no function"))

# The viewport window procedure in full (the switch on the message).
dump_func(base + 0x858620, "UWindowsViewport window procedure (full)", limit=1500)

# The UWindowsViewport vtable: every slot, so the virtual resize can be picked out by size/callers.
VT_RVA = 0xE4E448
w("")
w("##### UWindowsViewport vtable +0x%X" % VT_RVA)
ea = base + VT_RVA
slot = 0
while True:
    p = ida_bytes.get_dword(ea + 4 * slot)
    fn = ida_funcs.get_func(p)
    if not fn or fn.start_ea != p and slot > 0 and not ida_funcs.get_func(p):
        break
    if not fn:
        break
    w("  slot %3d (+0x%03X) -> %s %s (%d bytes, %d xrefs)" % (slot, 4 * slot, a(p), fname(p), fn.end_ea - fn.start_ea, len(list(idautils.XrefsTo(p)))))
    slot += 1
    if slot > 120:
        break

# The session-27 fault function.
dump_func(base + FAULT_RVA, "session-27 SETRES fault site", limit=400)
ff = ida_funcs.get_func(base + FAULT_RVA)
if ff:
    w("  callers of the fault function:")
    for frm, cf in callers(ff.start_ea):
        w("    %s in %s" % (a(frm), (a(cf) + " " + fname(cf)) if cf else "no function"))
done(0)
