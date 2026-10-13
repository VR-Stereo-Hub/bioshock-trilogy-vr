# VP-3 (static, BS1): the render device SetRes (viewport vtable call +244 in the SETRES branch), the
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



# 1. The WM_SIZE helper.
dump_func(base + 0x857C90, "WM_SIZE helper sub_11157C90", limit=200)
w("  callers:")
for frm, cf in callers(base + 0x857C90):
    w("    %s in %s" % (a(frm), (a(cf) + " " + fname(cf)) if cf else "no function"))

# 2. Render device classes: every RTTI type descriptor whose name mentions RenderDevice, the
#    vtable(s) that point at it, and slot 61 (+0xF4) of each = SetRes as Exec's SETRES calls it.
w("")
w("##### RENDER DEVICE VTABLES (RTTI)")
tds = []
pat = b".?AV"
ea = 0
while True:
    ea = ida_bytes.find_bytes(pat, ea + 1) if hasattr(ida_bytes, "find_bytes") else idc.find_binary(ea + 1, idc.SEARCH_DOWN, "2E 3F 41 56")
    if ea is None or ea == idaapi.BADADDR:
        break
    name = (idc.get_strlit_contents(ea, -1, idc.STRTYPE_C) or b"").decode(errors="replace")
    if "RenderDevice" in name or "D3DDrv" in name:
        tds.append((ea - 8, name))   # the TypeDescriptor starts 8 bytes before the name
    if ea > base + 0x1200000:
        break
for td, name in tds:
    w("  type %s %s" % (a(td), name))
    # COLs reference the TD at COL+12; vtables reference the COL at vt-4.
    for x in idautils.XrefsTo(td):
        col = x.frm - 12
        for y in idautils.XrefsTo(col):
            vt = y.frm + 4
            if not ida_funcs.get_func(ida_bytes.get_dword(vt)):
                continue
            w("    vtable %s" % a(vt))
            for slot in (61, 60, 62):
                p = ida_bytes.get_dword(vt + 4 * slot)
                fn = ida_funcs.get_func(p)
                if fn:
                    raw = ida_bytes.get_bytes(p, 12) or b""
                    w("      slot %d (+0x%X) -> %s %s (%d bytes) prologue %s" % (slot, 4 * slot, a(p), fname(p), fn.end_ea - fn.start_ea, raw.hex(" ")))
            p61 = ida_bytes.get_dword(vt + 4 * 61)
            if ida_funcs.get_func(p61):
                dump_func(p61, "slot 61 of %s" % name, limit=160)

# 3. The viewport object's fields read by SETRES: this-68 is the viewport; GETCURRENTRES prints
#    *(this+132) and *(this+136) -> viewport +0xC8/+0xCC. Print the viewport constructor-ish
#    sizes: just note the offsets used in Exec relative to the viewport.
w("")
w("##### OFFSETS (Exec this = viewport + 0x44): device ptr viewport+0x%X; GETCURRENTRES fields viewport+0x%X/+0x%X; fullscreen flag word viewport+0x%X; reentry counter viewport+0x%X; alt-fullscreen word viewport+0x%X"
  % (96 + 68, 132 + 68, 136 + 68, 456 + 68, 452 + 68, 188 + 68))
done(0)
