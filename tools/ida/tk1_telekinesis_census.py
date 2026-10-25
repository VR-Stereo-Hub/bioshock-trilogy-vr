# TK-1 (static, BS1): WHERE IS THE NATIVE TELEKINESIS CODE? Census of every string that names Telekinesis (ASCII
# and UTF-16), the native-table registrations int<Class>exec<Func> for the class and their exec thunks, the MSVC
# RTTI vtable(s) of UTelekinesisAbility with each slot compared against its parent classes' vtables (an override
# is a slot whose function differs from the parent's), and the code that references each string.
#     .\tools\ida-run.ps1 -Game bs1 tools\ida\tk1_telekinesis_census.py
# - Output path from BVR_IDA_OUT (outside the repo); decompiles are never committed. Findings go to the docs.
# - Every address is printed as RVA and VA. Negative paths print NONE / NO FUNCTION AT.
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


# ---- 1. raw byte search for the substrings, ASCII and UTF-16LE, across all segments
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


def read_c(ea, wide):
    out = b""
    step = 2 if wide else 1
    # walk back to string start
    s = ea
    while True:
        prev = ida_bytes.get_bytes(s - step, step)
        if not prev or prev == b"\0" * step or (not wide and not (32 <= prev[0] < 127)) or (wide and (prev[1] != 0 or not (32 <= prev[0] < 127))):
            break
        s -= step
    e = s
    while True:
        c = ida_bytes.get_bytes(e, step)
        if not c or c == b"\0" * step:
            break
        out += c[:1]
        e += step
        if len(out) > 200:
            break
    return s, out.decode(errors="replace")


seen = {}
for key in (b"elekinesis", b"Telekinesis"):
    for wide in (False, True):
        pat = key if not wide else b"".join(bytes([c, 0]) for c in key)
        for h in find_all(pat):
            s, txt = read_c(h, wide)
            seen[s] = (wide, txt)

w("")
w("##### STRINGS naming Telekinesis: %d" % len(seen))
for s in sorted(seen):
    wide, txt = seen[s]
    xs = list(idautils.XrefsTo(s))
    w("  %s %s %r  refs=%d" % (a(s), "W" if wide else "A", txt, len(xs)))
    for x in xs[:12]:
        if ida_funcs.get_func(x.frm):
            w("      code %s in %s" % (a(x.frm), fl(x.frm)))
        else:
            w("      data %s (%s)  dword+4=0x%X dword+8=0x%X" % (a(x.frm), idc.get_segm_name(x.frm),
                                                               ida_bytes.get_dword(x.frm + 4), ida_bytes.get_dword(x.frm + 8)))
            # native-table entry: the +4 slot is written by a static initializer
            for d in (4,):
                for y in idautils.XrefsTo(x.frm + d):
                    line = idc.generate_disasm_line(y.frm, 0)
                    w("        slot+%d ref at %s: %s" % (d, a(y.frm), line))
                    op1 = idc.get_operand_value(y.frm, 1)
                    if ida_funcs.get_func(op1):
                        w("          -> exec thunk %s" % fl(op1))

# ---- 2. RTTI vtables
def rtti_vtables(mangled):
    res = []
    for td_name in find_all(mangled.encode() + b"\0"):
        td = td_name - 8
        for x in idautils.XrefsTo(td):
            col = x.frm - 12
            if ida_bytes.get_dword(col) != 0:
                continue
            for y in idautils.XrefsTo(col):
                vt = y.frm + 4
                res.append((vt, ida_bytes.get_dword(col + 4)))
        # fallback: brute search dword==td
    return res


def vt_slots(vt, maxn=400):
    out = []
    ea = vt
    while len(out) < maxn:
        d = ida_bytes.get_dword(ea)
        if not ida_funcs.get_func(d):
            break
        # stop if another COL begins (next vtable): a data ref from RTTI
        if out and any(True for _ in idautils.XrefsTo(ea)) and ea != vt:
            pass
        out.append(d)
        ea += 4
    return out


classes = [".?AVUTelekinesisAbility@@", ".?AVUTelekinesisTwoAbility@@", ".?AVUAbility@@", ".?AVUAttackAbility@@",
           ".?AVUTraceAbility@@", ".?AVUTelekinesis@@", ".?AVUPlasmid@@", ".?AVUActivePlasmid@@"]
vts = {}
w("")
w("##### RTTI")
for c in classes:
    r = rtti_vtables(c)
    if not r:
        w("  %s: NONE" % c)
    for vt, off in r:
        sl = vt_slots(vt)
        vts.setdefault(c, []).append((vt, sl))
        w("  %s: vtable %s (COL offset %d) %d slots" % (c, a(vt), off, len(sl)))

# Also any RTTI type name containing Telekinesis
w("")
w("##### RTTI names containing 'elekinesis'")
for h in find_all(b".?AV"):
    pass
for s in sorted(seen):
    wide, txt = seen[s]
    if txt.startswith(".?A"):
        w("  %s %s" % (a(s), txt))

w("")
w("##### UTelekinesisAbility slot diff vs UAbility / UAttackAbility")
tk = vts.get(".?AVUTelekinesisAbility@@")
par = vts.get(".?AVUAbility@@")
att = vts.get(".?AVUAttackAbility@@")
if tk:
    vt, sl = tk[0]
    psl = par[0][1] if par else []
    asl = att[0][1] if att else []
    for i, f in enumerate(sl):
        p = psl[i] if i < len(psl) else None
        q = asl[i] if i < len(asl) else None
        tag = "OVERRIDE" if p is not None and f != p else ("new" if p is None else "")
        w("  slot %3d +0x%03X %s %-10s  UAbility:%s  UAttack:%s" % (i, 4 * i, fl(f), tag,
                                                                    ("+0x%X" % (p - base)) if p else "-",
                                                                    ("+0x%X" % (q - base)) if q else "-"))
else:
    w("  NONE")
w("")
w("done")
done(0)
