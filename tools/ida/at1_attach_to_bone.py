# AT-1 (static, BS1): THE C++ BEHIND Actor.AttachToBone. The Electro Bolt's effect is attached to the hands'
# FireballSocket by the effects system at each cast, natively, and a socket-table alias does not change where it goes
# (resolved at load). This finds the native's registration (intAActorexecAttachToBone) by its name string, the exec
# thunk, the C++ callee the thunk calls, every other caller of that callee (the effects system's path) and its head
# bytes, so a hook can attach the bolt to the hand bone in the same call.
#     .\tools\ida-run.ps1 -Game bs1 tools\ida\at1_attach_to_bone.py
# - The output path comes from BVR_IDA_OUT, set by ida-run.ps1. It is OUTSIDE the repo:
#   a decompile is game-derived text and is never committed. Findings go to ENGINE_NOTES.
# - TARGETS are RVAs; every address is printed as RVA AND VA. Instruction bytes are printed for a hook to verify.
# - The known-good check re-derives ProcessEvent and refuses on a mismatch.
# - Save without a UTF-8 BOM (IDA refuses it).
import os
import idaapi, idautils, idc, ida_auto, ida_bytes, ida_funcs, ida_hexrays, ida_name, ida_nalt


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


k = KNOWN[GAME]
got = ida_bytes.get_bytes(base + k[1], len(k[2]) // 2) or b""
w("known-good %s at RVA 0x%X: %s" % (k[0], k[1], "OK" if got.hex().upper() == k[2] else "*** MISMATCH %s ***" % got.hex()))


def dec(ea, n=60):
    if not hr:
        return
    try:
        c = str(ida_hexrays.decompile(ea))
        w("    " + "\n    ".join(c.splitlines()[:n]))
    except Exception as e:
        w("    DECOMPILE FAILED: %s" % e)


def fn_line(ea):
    f = ida_funcs.get_func(ea)
    if not f:
        return "NO FUNCTION AT 0x%X" % (ea - base)
    return "%s RVA 0x%X (VA 0x%X) head %s" % (ida_funcs.get_func_name(f.start_ea), f.start_ea - base, f.start_ea,
                                             (ida_bytes.get_bytes(f.start_ea, 16) or b"").hex())


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


for nm in [b"intAActorexecAttachToBone\0", b"intAActorexecDetachFromBone\0",
           "intAActorexecAttachToBone\0".encode("utf-16-le"), "AttachToBone\0".encode("utf-16-le")]:
    hits = find_all(nm)
    w("")
    w("##### %r: %d string hit(s) %s" % (nm, len(hits), ", ".join("RVA 0x%X" % (h - base) for h in hits[:12])))
    execs = set()
    for h in hits:
        for x in idautils.XrefsTo(h):
            entry = x.frm
            w("  entry @RVA 0x%X (name pointer)" % (entry - base))
            for d in (4, 8):
                slot = entry + d
                cur = ida_bytes.get_dword(slot)
                if ida_funcs.get_func(cur):
                    execs.add(ida_funcs.get_func(cur).start_ea)
                for y in idautils.XrefsTo(slot):
                    w("    slot +%d at RVA 0x%X: %s   in %s" % (d, y.frm - base, idc.generate_disasm_line(y.frm, 0), fn_line(y.frm)))
                    op1 = idc.get_operand_value(y.frm, 1)
                    if ida_funcs.get_func(op1):
                        execs.add(ida_funcs.get_func(op1).start_ea)
    for e in sorted(execs):
        w("")
        w("exec thunk: %s" % fn_line(e))
        for ea in idautils.FuncItems(e):
            if idc.print_insn_mnem(ea) == "call":
                tgt = idc.get_operand_value(ea, 0)
                f = ida_funcs.get_func(tgt)
                if not f:
                    continue
                callers = [c.frm for c in idautils.XrefsTo(f.start_ea) if idc.print_insn_mnem(c.frm) == "call"]
                w("  call at RVA 0x%X -> %s | called from %d site(s): %s" % (ea - base, fn_line(tgt), len(callers),
                  ", ".join("0x%X" % (c - base) for c in callers[:16])))
        try:
            c = str(ida_hexrays.decompile(e)).splitlines()
            w("  ... the thunk:")
            w("    " + "\n    ".join(c[:60]))
        except Exception as ex:
            w("  DECOMPILE FAILED: %s" % ex)
w("")
w("done")
done(0)
