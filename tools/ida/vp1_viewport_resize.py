# VP-1 (static, BS1): how does the engine resize its viewport, and who can call it live?
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
FAULT_RVA = 0x322353
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


# 1. The strings.
w("")
w("##### WIDE STRINGS")
string_funcs = {}
for s in ["SETRES", "TOGGLEFULLSCREEN", "Resizing", "ResizeViewport", "SetRes", "FullscreenViewportX", "WindowedViewportX"]:
    eas = find_wide(s)
    w("  %-22s %d hit(s)" % (s, len(eas)))
    for ea in eas:
        txt = (idc.get_strlit_contents(ea, -1, idc.STRTYPE_C_16) or b"").decode(errors="replace")
        w("    %s %r" % (a(ea), txt[:60]))
        for x in idautils.XrefsTo(ea):
            cf = ida_funcs.get_func(x.frm)
            w("      xref from %s in %s" % (a(x.frm), a(cf.start_ea) + " " + fname(cf.start_ea) if cf else "no function"))
            if cf:
                string_funcs.setdefault(s, set()).add(cf.start_ea)

# 2. Exec's SETRES handling.
dump_func(base + EXEC_RVA, "UWindowsViewport::Exec", grep=r"SETRES|Resiz|FULLSCREEN|sub_|\(\*\(.*\)\)\(", ctx=12, limit=4000)

# The direct callees of Exec, named, so the resize call can be picked out.
f = ida_funcs.get_func(base + EXEC_RVA)
if f:
    w("")
    w("##### Exec direct callees (first-call order)")
    seen = []
    for it in idautils.FuncItems(f.start_ea):
        if idc.print_insn_mnem(it) != "call":
            continue
        tgt = idc.get_operand_value(it, 0)
        tf = ida_funcs.get_func(tgt)
        if tf and tf.start_ea not in seen:
            seen.append(tf.start_ea)
            w("  call at %s -> %s %s (%d bytes)" % (a(it), a(tf.start_ea), fname(tf.start_ea), tf.end_ea - tf.start_ea))

# 3. Functions that reference "Resizing" (UE2 logs "Resizing viewport. X=%i Y=%i" from ResizeViewport).
for s, funcs in string_funcs.items():
    if s in ("Resizing", "ResizeViewport"):
        for fe in funcs:
            dump_func(fe, "function referencing %r" % s, limit=600)
            w("  callers:")
            for frm, cf in callers(fe):
                w("    %s in %s" % (a(frm), (a(cf) + " " + fname(cf)) if cf else "no function"))

# 4. Window procedures: callers of DefWindowProcW/A.
w("")
w("##### WINDOW PROCEDURES (callers of DefWindowProc)")
imp = {}
def _cb(ea, name, ordinal):
    if name:
        imp[name] = ea
    return True
for i in range(idaapi.get_import_module_qty()):
    idaapi.enum_import_names(i, _cb)
wndprocs = set()
for nm in ("DefWindowProcW", "DefWindowProcA"):
    ea = imp.get(nm)
    if not ea:
        w("  import %s: NOT FOUND" % nm)
        continue
    for x in idautils.XrefsTo(ea):
        cf = ida_funcs.get_func(x.frm)
        if cf:
            wndprocs.add(cf.start_ea)
    w("  import %s at %s: %d calling function(s)" % (nm, a(ea), len(wndprocs)))
for fe in sorted(wndprocs):
    dump_func(fe, "window procedure", grep=r"== 5\b|case 5:|0x214|case 532|0x47\b|case 71|== 0x24|case 36|WM_|Resiz|sub_", ctx=8, limit=3000)

# 5. The SETRES fault site.
dump_func(base + FAULT_RVA, "session-27 SETRES fault site", grep=r".", ctx=0, limit=250)
ff = ida_funcs.get_func(base + FAULT_RVA)
if ff:
    w("  callers of the fault function:")
    for frm, cf in callers(ff.start_ea):
        w("    %s in %s" % (a(frm), (a(cf) + " " + fname(cf)) if cf else "no function"))
done(0)
