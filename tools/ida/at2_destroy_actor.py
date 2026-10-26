# AT-2 (static, BS1): WHY Actor.Destroy REFUSES. The second hands and caught drums both get Destroy() = false with
# bDeleteMe clear. This finds the native's registration (intAActorexecDestroy) by its name string, the exec thunk,
# and decompiles the C++ it calls (ULevel::DestroyActor) in full, so the refusing branch can be read.
#     .\tools\ida-run.ps1 -Game bs1 tools\ida\at2_destroy_actor.py
# - The output path comes from BVR_IDA_OUT, set by ida-run.ps1. It is OUTSIDE the repo:
#   a decompile is game-derived text and is never committed. Findings go to ENGINE_NOTES.
# - TARGETS are RVAs; every address is printed as RVA AND VA. Instruction bytes are printed for a call to verify.
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



for nm in ["intAActorexecDestroy\0".encode("utf-16-le"), b"intAActorexecDestroy\0"]:
    hits = find_all(nm)
    w("")
    w("##### %r: %d string hit(s) %s" % (nm, len(hits), ", ".join("RVA 0x%X" % (h - base) for h in hits[:12])))
    execs = set()
    for h in hits:
        for x in idautils.XrefsTo(h):
            entry = x.frm
            w("  entry @RVA 0x%X (name pointer)" % (entry - base))
            for d in (4, 8):
                cur = ida_bytes.get_dword(entry + d)
                if ida_funcs.get_func(cur):
                    execs.add(ida_funcs.get_func(cur).start_ea)
                # The table is filled at startup (mov [slot], offset exec): the thunk is that store's operand.
                for y in idautils.XrefsTo(entry + d):
                    op1 = idc.get_operand_value(y.frm, 1)
                    if ida_funcs.get_func(op1):
                        execs.add(ida_funcs.get_func(op1).start_ea)
    for e in sorted(execs):
        w("")
        w("exec thunk: %s" % fn_line(e))
        dec(e, 40)
        for ea in idautils.FuncItems(e):
            if idc.print_insn_mnem(ea) == "call":
                tgt = idc.get_operand_value(ea, 0)
                f = ida_funcs.get_func(tgt)
                if not f or f.end_ea - f.start_ea < 0x40:
                    continue
                w("")
                w("  callee at RVA 0x%X -> %s" % (ea - base, fn_line(tgt)))
                dec(f.start_ea, 260)
# The thunk calls XLevel's vtable +0x134 (ULevel::DestroyActor). ULevel's vtable from its RTTI: the type
# descriptor holds the decorated name 8 bytes in, a complete-object locator points at it from +12, and the vtable's
# slot -1 points at the locator.
w("")
w("##### ULevel::DestroyActor (vtable +0x134)")
for rtti in [b".?AVULevel@@\x00"]:
    for h in find_all(rtti):
        td = h - 8
        for col in idautils.XrefsTo(td):
            if ida_bytes.get_dword(col.frm - 12) != 0:
                continue
            for vt in idautils.XrefsTo(col.frm - 12):
                vtab = vt.frm + 4
                fe = ida_bytes.get_dword(vtab + 0x134)
                w("  vtable RVA 0x%X: +0x134 -> %s" % (vtab - base, fn_line(fe)))
                dec(fe, 260)
                break  # the first table is ULevel's own


# DestroyActor's deferred refusal: the actor's vtable +0x26C, then two helpers. Each, for the hands' own class.
def vtable_of(cls):
    for h in find_all(cls):
        for col in idautils.XrefsTo(h - 8):
            if ida_bytes.get_dword(col.frm - 12) != 0:
                continue
            for vt in idautils.XrefsTo(col.frm - 12):
                return vt.frm + 4
    return None


w("")
w("##### the deferred-destroy branch")
for cls in [b".?AVAActor@@\x00", b".?AVAHands@@\x00"]:
    vt = vtable_of(cls)
    if vt:
        fe = ida_bytes.get_dword(vt + 0x26C)
        w("  %s vtable RVA 0x%X: +0x26C -> %s" % (cls[4:-3].decode(), vt - base, fn_line(fe)))
        dec(fe, 40)
    else:
        w("  %r: no vtable found" % cls)
for rva in (0x1B9DE0, 0x39CC50):
    w("")
    w("  helper %s" % fn_line(base + rva))
    dec(base + rva, 50)
w("")
w("done")
done(0)
