# RS-14 (static, BS1): WHERE DOES AN ATTACHED ACTOR'S TRANSFORM COME FROM? AActor::AttachToBone
# (+0x379EF0) stores the bone FName on the attached actor at +0xF0/+0xF4, calls SetBase (vtable
# +0x1A0) and notifies +0x3FA1E0; +0x3F4BD0 resolves a bone name. The held weapon renders at the
# attach bone's array transform, ignoring its own Location (ENGINE_NOTES). This finds the routine
# that reads +0xF0/+0xF4 back and builds the attached actor's matrix from the base's bone, so the
# hands' correction can be composed there for every attachment (weapon, its children, plasmid FX).
#     .\tools\ida-run.ps1 -Game bs1 tools\ida\rs14_attach_matrix.py
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

TARGETS = {"bs1": [0x3FA1E0, 0x3F4BD0]}   # RVAs

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



def calls_to(f, tgt):
    for it in idautils.FuncItems(f.start_ea):
        if idc.print_insn_mnem(it) == "call" and idc.get_operand_value(it, 0) == tgt:
            return True
    return False


def reads_attach_name(f):
    # an operand [reg+0F0h] and one [reg+0F4h] in the same function
    hit = [False, False]
    for it in idautils.FuncItems(f.start_ea):
        for op in (0, 1):
            if idc.get_operand_type(it, op) in (idc.o_displ, idc.o_phrase):
                d = idc.get_operand_value(it, op) & 0xFFFFFFFF
                if d == 0xF0:
                    hit[0] = True
                elif d == 0xF4:
                    hit[1] = True
    return hit[0] and hit[1]


for t in TARGETS[GAME]:
    decomp(va(t), 1, 400)
    refs(va(t))

lookup = va(0x3F4BD0)
w("")
w("##### CALLERS OF THE BONE-NAME LOOKUP +0x3F4BD0 THAT ALSO READ +0xF0/+0xF4")
cands = []
for x in idautils.XrefsTo(lookup):
    cf = ida_funcs.get_func(x.frm)
    if cf and cf.start_ea not in [c.start_ea for c in cands]:
        flag = reads_attach_name(cf)
        w("  %s in %s %s  reads +0xF0/+0xF4: %s  size %d" % (a(x.frm), a(cf.start_ea), fname(cf.start_ea),
                                                          "YES" if flag else "no", cf.end_ea - cf.start_ea))
        if flag:
            cands.append(cf)
w("")
w("##### EVERY FUNCTION READING +0xF0 AND +0xF4 THAT CALLS +0x3F4BD0 OR +0x544FA0 (GetBoneCoords inner)")
gbc = va(0x544FA0)
extra = []
for fea in idautils.Functions():
    f = ida_funcs.get_func(fea)
    if f.end_ea - f.start_ea > 4000:
        continue
    if (calls_to(f, lookup) or calls_to(f, gbc)) and reads_attach_name(f):
        extra.append(f)
        w("  %s %s size %d" % (a(f.start_ea), fname(f.start_ea), f.end_ea - f.start_ea))
if not extra:
    w("  NONE")
seen = set()
for f in cands + extra:
    if f.start_ea in seen:
        continue
    seen.add(f.start_ea)
    decomp(f.start_ea, 1, 160)
    refs(f.start_ea)
w("")
w("##### PER-FRAME CANDIDATES: read instance +0xE8/+0xEC (attach names) and +0x100 (48-byte relative coords), call vslot +0xB8 (bone coords by index)")
def disp_set(f):
    ds = set()
    vcall = False
    for it in idautils.FuncItems(f.start_ea):
        for op in (0, 1):
            if idc.get_operand_type(it, op) in (idc.o_displ, idc.o_phrase):
                ds.add(idc.get_operand_value(it, op) & 0xFFFFFFFF)
        if idc.print_insn_mnem(it) == "call" and idc.get_operand_type(it, 0) in (idc.o_displ, idc.o_phrase)                 and (idc.get_operand_value(it, 0) & 0xFFFFFFFF) == 0xB8:
            vcall = True
    return ds, vcall
pf = []
for fea in idautils.Functions():
    f = ida_funcs.get_func(fea)
    if f.end_ea - f.start_ea > 8000:
        continue
    ds, vcall = disp_set(f)
    if 0xEC in ds and 0x100 in ds and vcall:
        pf.append(f)
        w("  %s %s size %d  E8:%s F4:%s" % (a(f.start_ea), fname(f.start_ea), f.end_ea - f.start_ea, 0xE8 in ds, 0xF4 in ds))
if not pf:
    w("  NONE")
for f in pf[:6]:
    if f.start_ea in seen:
        continue
    seen.add(f.start_ea)
    decomp(f.start_ea, 1, 220)
    refs(f.start_ea)
done(0)
