# analyze.py - the one-time auto-analysis of a staged binary, and a health report.
# Run by `.\tools\ida-run.ps1 -Game <g> -Stage` (or alone:
# `.\tools\ida-run.ps1 -Game <g> tools\ida\analyze.py`). IDA saves the .i64 when qexit
# runs; every later script opens that database and skips analysis. The report is what
# proves the headless pipeline works end to end: IDAPython ran, the decompiler is
# licensed for x86, and a published address is where the game's patterns say it is.
import os, time
import idaapi, idautils, idc, ida_auto, ida_bytes, ida_funcs, ida_hexrays, ida_nalt

# Per game: one published address and the bytes the mod itself byte-verifies there.
# RVAs, because these exes relocate (ASLR) - the mod's patterns.h files store RVAs.
#   bs1  UObject::ProcessEvent outer, push ebp; mov ebp,esp; mov eax,[ecx+0xF8]
#        (hands.cpp kProcessEventRva, re-read offline with disasm-rva.py 2026-10-07)
#   bs2  UObject::ProcessEvent outer, mov eax,[ecx+0x10C] (bioshock2r/patterns.h)
#   bsi  GetPlayerViewPoint, the aligned-stack prologue (bioshockinf/patterns.h)
KNOWN = {
    "bs1": ("ProcessEvent",       0x375140, "558BEC8B81F8000000"),
    "bs2": ("ProcessEvent",       0x37A7E0, "558BEC8B810C010000"),
    "bsi": ("GetPlayerViewPoint", 0x1E10C0, "558BEC83E4F081ECA4000000"),
}

OUT_PATH = os.environ.get("BVR_IDA_OUT")
GAME = os.environ.get("BVR_IDA_GAME", "")
if not OUT_PATH or GAME not in KNOWN:
    print("*** BVR_IDA_OUT / BVR_IDA_GAME not set - run this through tools\\ida-run.ps1 ***")
    idc.qexit(2)

t0 = time.time()
ida_auto.auto_wait()
OUT = []
w = OUT.append
base = idaapi.get_imagebase()
w("game %s  input %s  md5 %s" % (GAME, ida_nalt.get_root_filename(), (ida_nalt.retrieve_input_file_md5() or b"").hex()))
w("imagebase 0x%X  bitness %d  auto_wait %.1fs" % (base, 64 if idaapi.inf_is_64bit() else 32, time.time() - t0))
for s in idautils.Segments():
    w("segment %-8s 0x%08X-0x%08X (+0x%X)" % (idc.get_segm_name(s), s, idc.get_segm_end(s), s - base))
w("functions %d" % sum(1 for _ in idautils.Functions()))
hr = ida_hexrays.init_hexrays_plugin()
w("hexrays %s" % ("OK" if hr else "*** MISSING ***"))

kname, krva, kbytes = KNOWN[GAME]
ea = base + krva
want = bytes.fromhex(kbytes)
got = ida_bytes.get_bytes(ea, len(want)) or b""
w("known-good %s +0x%X (0x%08X) bytes %s %s" % (kname, krva, ea, got.hex(" "), "MATCH" if got == want else "*** MISMATCH ***"))
f = ida_funcs.get_func(ea)
w("function at %s: %s" % (kname, "0x%08X size %d" % (f.start_ea, f.end_ea - f.start_ea) if f else "*** NONE ***"))
if hr and f:
    try:
        n = len(str(ida_hexrays.decompile(ea)).split("\n"))
        w("decompile %s: OK, %d lines" % (kname, n))
    except Exception as e:
        w("decompile %s: *** FAILED: %s ***" % (kname, e))

with open(OUT_PATH, "w", encoding="utf-8") as fh:
    fh.write("\n".join(OUT) + "\n")
idc.qexit(0)
