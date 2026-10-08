# HD-1 (static, BS1): where does the engine re-evaluate a SkeletonInstance's bones, and
# which virtual does it call? BS2 has the answer (ENGINE_NOTES s74: call site 0x5FB810,
# `cmp byte [esi+0xA0],0; mov eax,[esi]; call [eax+0xA4]; mov byte [esi+0xA0],0` - the
# dirty-flagged update its `wfix` post-hooks). BS1's SkeletonInstance keeps the dirty byte
# at +0x88 (patterns.h kSkelInstDirtyOffset) and its notes name virtuals +0x9C (evaluator)
# and +0xA0 (gate). This finds BS1's own call sites and slot instead of carrying BS2's.
#     .\tools\ida-run.ps1 -Game bs1 tools\ida\hd1_skelinst_update.py
# The Dishonored-shaped hand drive composes onto the FRESHLY evaluated pose, so it needs to
# run the moment this update returns (docs/bioshock1/HANDS_DISHONORED.md).
import os
import idaapi, idautils, idc, ida_auto, ida_bytes, ida_funcs, ida_hexrays, ida_ua

KNOWN = {"bs1": ("ProcessEvent", 0x375140, "558BEC8B81F8000000")}
VTABLE_RVA = 0xE19ACC   # .?AVSkeletonInstance@@ (patterns.h kSkeletonInstanceVtableRva)
DIRTY = 0x88
SLOTS = (0x98, 0x9C, 0xA0, 0xA4, 0xA8, 0xB8, 0xBC)

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
ida_hexrays.init_hexrays_plugin()
a = lambda ea: "+0x%X (0x%08X)" % (ea - base, ea)


def deref_thunk(ea):
    # follow a jmp thunk (ILT) to the real function
    for _ in range(4):
        if idc.print_insn_mnem(ea) == "jmp" and idc.get_operand_type(ea, 0) == idc.o_near:
            ea = idc.get_operand_value(ea, 0)
        else:
            break
    return ea


def decompile(ea, limit=160):
    try:
        lines = str(ida_hexrays.decompile(ea)).split("\n")
    except Exception as e:
        return ["DECOMPILE FAILED: %s" % e]
    return lines[:limit] + (["... CUT at %d of %d" % (limit, len(lines))] if len(lines) > limit else [])


vt = base + VTABLE_RVA
w("##### SkeletonInstance vtable %s" % a(vt))
slot_targets = {}
for off in SLOTS:
    t = ida_bytes.get_dword(vt + off)
    real = deref_thunk(t)
    slot_targets[off] = real
    f = ida_funcs.get_func(real)
    w("  slot +0x%X -> %s%s size %s" % (off, a(t), "" if real == t else " -> " + a(real),
                                         (f.end_ea - f.start_ea) if f else "?"))

# Call sites: `call dword ptr [reg+SLOT]` with a `cmp byte ptr [reg+DIRTY], 0` within the
# 24 bytes before it, or a `mov byte ptr [reg+DIRTY], 0` within 16 bytes after.
w("")
w("##### call sites guarded by the +0x%X dirty byte" % DIRTY)
seg = idaapi.get_segm_by_name(".text")
hits = []
for fea in idautils.Functions(seg.start_ea, seg.end_ea):
    for it in idautils.FuncItems(fea):
        if idc.print_insn_mnem(it) != "call" or idc.get_operand_type(it, 0) != idc.o_displ:
            continue
        disp = idc.get_operand_value(it, 0)
        if disp not in SLOTS:
            continue
        before = after = False
        p = it
        for _ in range(8):
            p = idc.prev_head(p)
            if p == idaapi.BADADDR or p < it - 40:
                break
            s = idc.generate_disasm_line(p, 0).lower()
            if "cmp" in s and ("+%xh]" % DIRTY) in s.replace(" ", ""):
                before = True
        n = it
        for _ in range(4):
            n = idc.next_head(n)
            s = idc.generate_disasm_line(n, 0).lower()
            if "mov" in s and "byte" in s and ("+%xh]" % DIRTY) in s.replace(" ", ""):
                after = True
        if before or after:
            hits.append((it, fea, disp, before, after))
for it, fea, disp, before, after in hits:
    w("  %s in %s: call [vtbl+0x%X]  guard-before=%s clear-after=%s" % (a(it), a(fea), disp, before, after))
    p = it
    ctx = []
    for _ in range(6):
        p = idc.prev_head(p)
        ctx.insert(0, p)
    n = it
    ctx.append(it)
    for _ in range(3):
        n = idc.next_head(n)
        ctx.append(n)
    for c in ctx:
        w("      %s  %s" % (a(c), idc.generate_disasm_line(c, 0)))

w("")
for off in (0x9C, 0xA0, 0xA4):
    t = slot_targets.get(off)
    if not t:
        continue
    w("##### slot +0x%X body %s" % (off, a(t)))
    for l in decompile(t, 120):
        w("  | " + l)
    w("")
for it, fea, disp, before, after in hits[:6]:
    w("##### caller %s (site %s)" % (a(fea), a(it)))
    for l in decompile(fea, 140):
        w("  | " + l)
    w("")
done(0)
