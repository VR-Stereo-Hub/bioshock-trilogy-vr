# SE-1 (static, BS1): WHERE DO EFFECT EVENTS GO? The gun sound engine (manual_reload) mutes the effect events a
# reload or pump clip fires by itself (HandsPumpShotgun, HandsShotgunReloadEnd, ...) and fires them itself when the
# player does the action. Animation cues reach the effects system natively, not through script, so the hook is the
# C++ function behind Actor.TriggerEffectEvent. This finds the native's registration by its name string, the exec
# thunk next to it, the C++ callee the thunk calls, and every other caller of that callee (the anim-notify path).
#     .\tools\ida-run.ps1 -Game bs1 tools\ida\se1_effect_event.py
# - The output path comes from BVR_IDA_OUT, set by ida-run.ps1. It is OUTSIDE the repo:
#   a decompile is game-derived text and is never committed. Findings go to ENGINE_NOTES.
# - TARGETS are RVAs; every address is printed as RVA AND VA. Instruction bytes are printed for a hook to verify.
# - The known-good check re-derives ProcessEvent and refuses on a mismatch.
# - Save without a UTF-8 BOM (IDA refuses it).
import os
import idaapi, idautils, idc, ida_auto, ida_bytes, ida_funcs, ida_hexrays, ida_name, ida_nalt

TARGETS = {"bs1": [0x375140], "bs2": [0x37A7E0], "bsi": [0x1E10C0]}   # RVAs, per game

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


# The native table entry {name, exec thunk, 0} for intAActorexecTriggerEffectEvent: its thunk slot is written by a
# static initializer at startup. Who writes it, with what, and what that exec thunk calls.
ENTRY = 0x11BB6AC  # RVA of the entry's name pointer (found by the string search this script began as)
name = ida_bytes.get_dword(base + ENTRY)
w("entry @RVA 0x%X name -> %r" % (ENTRY, idc.get_strlit_contents(name) or b"?"))
execs = set()
for d in (4, 8):
    slot = base + ENTRY + d
    for x in idautils.XrefsTo(slot):
        line = idc.generate_disasm_line(x.frm, 0)
        w("  slot +%d written/read at RVA 0x%X: %s   in %s" % (d, x.frm - base, line, fn_line(x.frm)))
        op1 = idc.get_operand_value(x.frm, 1)
        if ida_funcs.get_func(op1):
            execs.add(ida_funcs.get_func(op1).start_ea)
for e in sorted(execs):
    w("")
    w("exec thunk: %s" % fn_line(e))
    for ea in idautils.FuncItems(e):
        if idc.print_insn_mnem(ea) == "call":
            tgt = idc.get_operand_value(ea, 0)
            w("  call at RVA 0x%X -> %s" % (ea - base, fn_line(tgt)))
            f = ida_funcs.get_func(tgt)
            if f:
                callers = [c.frm for c in idautils.XrefsTo(f.start_ea) if idc.print_insn_mnem(c.frm) == "call"]
                w("      called from %d site(s): %s" % (len(callers), ", ".join("0x%X" % (c - base) for c in callers[:12])))
    try:
        c = str(ida_hexrays.decompile(e)).splitlines()
        w("  ... the thunk's last lines:")
        w("    " + "\n    ".join(c[-25:]))
    except Exception as ex:
        w("  DECOMPILE FAILED: %s" % ex)
    ea = base + 0x37BB4B
    for _ in range(10):
        ea = idc.prev_head(ea)
    for _ in range(16):
        w("    RVA 0x%X  %s" % (ea - base, idc.generate_disasm_line(ea, 0)))
        ea = idc.next_head(ea)
w("")
w("done")
done(0)
