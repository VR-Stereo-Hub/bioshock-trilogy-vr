# TEMPLATE (static; the game and the staged exe are stamped on the output's first line):
# <THE ONE QUESTION THIS SCRIPT ANSWERS>. Copy this file to tools/ida/<series><n>_<what>.py,
# replace the question above and TARGETS below, and run it:
#     .\tools\ida-run.ps1 -Game bs1 tools\ida\<series><n>_<what>.py
#
# Rules this skeleton encodes (docs/IDA_WORKFLOW.md says why each one exists):
# - auto_wait() first, qexit(0) last: without qexit headless IDA never exits.
# - The output path comes from BVR_IDA_OUT, set by ida-run.ps1. It is OUTSIDE the repo:
#   a decompile is game-derived text and is never committed. Findings go to ENGINE_NOTES.
# - TARGETS are RVAs, the form every patterns.h here stores (the exes relocate). Print
#   every address as RVA AND VA: crash offsets and disasm-rva.py speak RVAs, IDA VAs.
# - Every negative path prints something: NO FUNCTION AT, DECOMPILE FAILED, NONE, CUT.
# - Print instruction BYTES: a hook byte-verifies its target before it writes.
# - The known-good check re-derives an address the game's patterns already publish and
#   refuses on a mismatch. Keep it in every copy. A route that cannot reproduce a known
#   answer is not evidence about a new one.
# - The answer is about THIS game only. Never carry a number to another game - same
#   engine tree, different link.
# - 32-bit: vtable slots are 4 bytes; MSVC x86 RTTI stores ABSOLUTE pointers, so a
#   relocated image still reads correctly once IDA has applied its own base.
# - Save without a UTF-8 BOM (IDA refuses it: SyntaxError U+FEFF). ida-run.ps1 checks.
# Ported from the Dishonored VR mod's template.
import os
import idaapi, idautils, idc, ida_auto, ida_bytes, ida_funcs, ida_hexrays, ida_name, ida_nalt

TARGETS = {"bs1": [0x3DBCF0]}   # RVAs

# Same table as analyze.py - copied, not imported, so every script runs alone.
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
    print("*** BVR_IDA_OUT / BVR_IDA_GAME not set - run this through tools\\ida-run.ps1 ***")
    idc.qexit(2)

ida_auto.auto_wait()
base = idaapi.get_imagebase()
hr = ida_hexrays.init_hexrays_plugin()
w("game %s  input %s  md5 %s" % (GAME, ida_nalt.get_root_filename(), (ida_nalt.retrieve_input_file_md5() or b"").hex()))
w("imagebase 0x%X  hexrays %s" % (base, "OK" if hr else "*** MISSING (no x86 decompiler licence?) ***"))


def va(rva):
    return base + rva


def a(ea):
    return "+0x%X (0x%08X)" % (ea - base, ea)


def fname(ea):
    return ida_name.get_name(ea) or ("sub_%X" % ea)


kname, krva, kbytes = KNOWN[GAME]
want = bytes.fromhex(kbytes)
got = ida_bytes.get_bytes(va(krva), len(want)) or b""
if got != want:
    w("*** REFUSED: known-good %s bytes at +0x%X are %s, expected %s - wrong binary or a game update ***"
      % (kname, krva, got.hex(" "), want.hex(" ")))
    done(3)
w("known-good %s +0x%X bytes match" % (kname, krva))


def decomp(ea, first=1, limit=400):
    f = ida_funcs.get_func(ea)
    if not f:
        w("*** NO FUNCTION AT %s ***" % a(ea))
        return
    w("")
    w("##### DECOMPILE %s (function %s %s)" % (a(ea), a(f.start_ea), fname(f.start_ea)))
    try:
        lines = str(ida_hexrays.decompile(f.start_ea)).split("\n")
    except Exception as e:
        lines = ["DECOMPILE FAILED: %s" % e]
    window = lines[first - 1:first - 1 + limit]
    for i, l in enumerate(window):
        w("  %4d| %s" % (first + i, l))
    if len(lines) > first - 1 + limit or first > 1:
        w("  ... showing lines %d-%d of %d" % (first, first - 1 + len(window), len(lines)))


def disasm(ea):
    f = ida_funcs.get_func(ea)
    if not f:
        w("*** NO FUNCTION AT %s ***" % a(ea))
        return
    items = list(idautils.FuncItems(f.start_ea))
    w("")
    w("##### DISASM %s (%d instructions)" % (a(f.start_ea), len(items)))
    for it in items:
        raw = ida_bytes.get_bytes(it, idc.get_item_size(it)) or b""
        w("  %s  %-24s %s" % (a(it), raw.hex(" "), idc.generate_disasm_line(it, 0)))


def vtable_class(slot_ea):
    # Walk back to the vtable start (first dword whose predecessor is not a function
    # pointer). MSVC x86 RTTI: CompleteObjectLocator pointer at vt-4, TypeDescriptor
    # pointer (absolute) at COL+12, mangled name at TD+8.
    ea = slot_ea
    while ida_funcs.get_func(ida_bytes.get_dword(ea - 4)) is not None:
        ea -= 4
    col = ida_bytes.get_dword(ea - 4)
    try:
        td = ida_bytes.get_dword(col + 12)
        name = (idc.get_strlit_contents(td + 8) or b"*** COL UNREADABLE ***").decode(errors="replace")
    except Exception as e:
        name = "*** COL UNREADABLE (%s) ***" % e
    return ea, (slot_ea - ea) // 4, name


def refs(ea):
    f = ida_funcs.get_func(ea)
    if not f:
        return
    n = 0
    w("")
    w("##### REFERENCES TO %s" % a(f.start_ea))
    for x in idautils.XrefsTo(f.start_ea):
        n += 1
        cf = ida_funcs.get_func(x.frm)
        if cf:
            w("  code %s in %s %s" % (a(x.frm), a(cf.start_ea), fname(cf.start_ea)))
        elif ida_bytes.get_dword(x.frm) != f.start_ea:
            w("  data %s in %s - NOT a pointer slot, not a vtable" % (a(x.frm), idc.get_segm_name(x.frm)))
        else:
            vt, slot, cls = vtable_class(x.frm)
            w("  data %s = vtable %s slot %d (+0x%X) class %s" % (a(x.frm), a(vt), slot, 4 * slot, cls))
    if n == 0:
        w("  NONE (no code or data reference found)")


for t in TARGETS.get(GAME, []):
    decomp(va(t))
    disasm(va(t))
    refs(va(t))

done(0)
