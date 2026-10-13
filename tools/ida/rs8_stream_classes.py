# RS-8 (static, BS1): every class whose RTTI name contains "Stream" or "Skin" - the hands mesh instance binds an embedded vertex stream (USkeletalMeshInstance+120) and its fill virtual is the CPU skinner (the +0x128/+0x12C receiver was not a MeshInstance). The per-draw skin path
# (+0x319F00, hd1) evaluates the SkeletonInstance and then calls the mesh instance's
# virtuals +0x128 and +0x12C with a 64-byte matrix. This script finds every vtable whose
# RTTI class name contains "MeshInstance", prints slots +0x128/+0x12C, decompiles their
# targets, and decompiles +0x3EDB20 (the range helper of the +0x3EDC40 strategy) and
# +0x379EF0 (called at the tail of +0x319F00). The question: CPU skinning (a loop over
# vertices with bone-byte pairs writing a vertex buffer) or a GPU palette upload?
#     .\tools\ida-run.ps1 -Game bs1 tools\ida\rs8_stream_classes.py
import os
import idaapi, idautils, idc, ida_auto, ida_bytes, ida_funcs, ida_hexrays, ida_name, ida_nalt, ida_segment

KNOWN = {"bs1": ("ProcessEvent", 0x375140, "558BEC8B81F8000000")}
EXTRA = []
SLOTS = []
MAXLINES = 700

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
decompiled = set()


def decomp(ea, limit=MAXLINES):
    f = ida_funcs.get_func(ea)
    if not f:
        w("*** NO FUNCTION AT %s ***" % a(ea))
        return
    if f.start_ea in decompiled:
        w("  (already decompiled above: %s)" % a(f.start_ea))
        return
    decompiled.add(f.start_ea)
    w("")
    w("##### DECOMPILE %s (%s, %d bytes)" % (a(f.start_ea), fname(f.start_ea), f.end_ea - f.start_ea))
    try:
        lines = str(ida_hexrays.decompile(f.start_ea)).split("\n")
    except Exception as e:
        lines = ["DECOMPILE FAILED: %s" % e]
    for i, l in enumerate(lines[:limit]):
        w("  %4d| %s" % (i + 1, l))
    if len(lines) > limit:
        w("  ... CUT at %d of %d lines" % (limit, len(lines)))
    seen = []
    for it in idautils.FuncItems(f.start_ea):
        if idc.print_insn_mnem(it) != "call":
            continue
        tgt = idc.get_operand_value(it, 0)
        if ida_funcs.get_func(tgt) and tgt not in seen:
            seen.append(tgt)
    w("  callees: " + (", ".join(a(t) for t in seen) if seen else "NONE"))


# ---- vtables by RTTI class name --------------------------------------------------
# MSVC x86: each vtable is preceded by a pointer to its CompleteObjectLocator; COL+12 is the
# TypeDescriptor, whose mangled name sits at TD+8 (".?AVFoo@@").
found = []
seg = ida_segment.get_segm_by_name(".rdata")
if seg is None:
    w("*** NO .rdata SEGMENT ***")
    done(4)
ea = seg.start_ea
while ea + 8 <= seg.end_ea:
    col = ida_bytes.get_dword(ea)
    vt = ea + 4
    try:
        if col and seg.start_ea <= col < seg.end_ea and ida_bytes.get_dword(col) == 0:
            td = ida_bytes.get_dword(col + 12)
            if td and ida_bytes.get_dword(col + 4) == 0 and ida_bytes.get_dword(col + 8) == 0:
                nm = idc.get_strlit_contents(td + 8) or b""
                nm = nm.decode(errors="replace")
                if nm.startswith(".?AV") and (("Stream" in nm) or ("Skin" in nm)) and ida_funcs.get_func(ida_bytes.get_dword(vt)):
                    found.append((vt, nm))
    except Exception:
        pass
    ea += 4

w("")
w("##### VTABLES whose RTTI name contains MeshInstance: %d" % len(found))
targets = []
for vt, nm in found:
    n = 0
    e = vt
    while ida_funcs.get_func(ida_bytes.get_dword(e)) is not None and n < 400:
        n += 1
        e += 4
    w("  vtable %s class %s  (%d slots)" % (a(vt), nm, n))
    for s in SLOTS:
        if s // 4 < n:
            t = ida_bytes.get_dword(vt + s)
            w("    slot +0x%X -> %s %s" % (s, a(t), fname(t)))
            if t not in targets:
                targets.append(t)
        else:
            w("    slot +0x%X -> NONE (vtable shorter)" % s)

for t in targets:
    decomp(t)
for r in EXTRA:
    decomp(base + r)
done(0)
