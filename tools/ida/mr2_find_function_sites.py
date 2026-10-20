# TEMPLATE-DERIVED (static): WHICH FUNCTION IS UObject::FindFunctionChecked? BS1 strips its
# failure string, so find it by shape (BS2 ENGINE_NOTES): a `ret 0xC` callee whose call is followed
# within a few instructions by `call [reg+0xC]` (ProcessEvent, vtable slot 3), at many sites.
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

import idaapi as _ia
k = KNOWN[GAME]
got = ida_bytes.get_bytes(base + k[1], len(k[2]) // 2) or b""
w("known-good %s at RVA 0x%X: %s" % (k[0], k[1], "OK" if got.hex().upper() == k[2] else "*** MISMATCH %s ***" % got.hex()))

counts = {}
for fea in idautils.Functions():
    for head in idautils.FuncItems(fea):
        if idc.print_insn_mnem(head) != "call" or idc.get_operand_type(head, 0) not in (idc.o_near, idc.o_far):
            continue
        tgt = idc.get_operand_value(head, 0)
        # follow a jmp stub
        if idc.print_insn_mnem(tgt) == "jmp" and idc.get_operand_type(tgt, 0) in (idc.o_near, idc.o_far):
            tgt = idc.get_operand_value(tgt, 0)
        nxt = head
        for _ in range(8):
            nxt = idc.next_head(nxt)
            if idc.print_insn_mnem(nxt) == "call":
                op = idc.print_operand(nxt, 0).replace(" ", "")
                if op.endswith("+0Ch]") or op.endswith("+0xC]"):
                    counts[tgt] = counts.get(tgt, 0) + 1
                break
top = sorted(counts.items(), key=lambda kv: -kv[1])[:8]
for tgt, n in top:
    f = ida_funcs.get_func(tgt)
    ret = ""
    if f:
        for it in idautils.FuncItems(f.start_ea):
            if idc.print_insn_mnem(it) == "retn":
                ret = idc.print_operand(it, 0)
    w("RVA 0x%X (VA 0x%X) followed by call[+0xC] at %d sites, retn %s, head %s" % (tgt - base, tgt, n, ret or "-", (ida_bytes.get_bytes(tgt, 12) or b"").hex()))
if len(top) > 1 and hr:
    try:
        w("\n".join(str(ida_hexrays.decompile(top[1][0])).splitlines()[:60]))
    except Exception as e:
        w("DECOMPILE FAILED: %s" % e)
if not top:
    w("NONE")
done(0)
