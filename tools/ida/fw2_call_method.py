# FW-2 (static, BS1): WHAT IS +0x427220, AND DOES UNREALSCRIPT'S CallMethod* REACH IT TOO?
# FW-1 found every native HUD call (AddWeapon, AddAbility, RemoveWeapon, SelectWeapon, ...) going
# through +0x427220 as (movie, "Method", "printf format", args...) after +0x43E220 fetched the
# movie by name. The wheels open and close from UnrealScript (CallHUDFunction ->
# FlashMovie.CallMethodVoid('OpenWeaponMenu' / 'RBReleased' / ...)). This decompiles +0x427220
# and +0x43E220, disassembles +0x427220's prologue (bytes for the hook), and lists every caller
# with the literal method name and format at the call site; a caller with no literal name is a
# candidate for the script natives (execCallMethodVoid/String/...), decompiled in full.
#     .\tools\ida-run.ps1 -Game bs1 tools\ida\fw2_call_method.py
# - The output path comes from BVR_IDA_OUT, set by ida-run.ps1. OUTSIDE the repo.
# - RVA and VA for every address; every negative path prints; known-good check first.
# - Save without a UTF-8 BOM.
import os
import idaapi, idautils, idc, ida_auto, ida_bytes, ida_funcs, ida_hexrays, ida_name, ida_nalt, ida_ua

CALL = 0x427220     # the movie call routine (FW-1)
GETMOVIE = 0x43E220 # GetPlayingMovie by name (FW-1)

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


if not OUT_PATH or GAME != "bs1":
    print("*** BVR_IDA_OUT not set or not bs1 - run this through tools\\ida-run.ps1 -Game bs1 ***")
    idc.qexit(2)

ida_auto.auto_wait()
base = idaapi.get_imagebase()
hr = ida_hexrays.init_hexrays_plugin()
w("game %s  input %s  md5 %s" % (GAME, ida_nalt.get_root_filename(), (ida_nalt.retrieve_input_file_md5() or b"").hex()))
w("imagebase 0x%X  hexrays %s" % (base, "OK" if hr else "*** MISSING ***"))


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
    w("*** REFUSED: known-good %s bytes at +0x%X are %s, expected %s ***" % (kname, krva, got.hex(" "), want.hex(" ")))
    done(3)
w("known-good %s +0x%X bytes match" % (kname, krva))


def decomp(ea, limit=200):
    f = ida_funcs.get_func(ea)
    if not f:
        w("  *** NO FUNCTION AT %s ***" % a(ea))
        return
    w("")
    w("##### DECOMPILE %s %s" % (a(f.start_ea), fname(f.start_ea)))
    try:
        lines = str(ida_hexrays.decompile(f.start_ea)).split("\n")
    except Exception as e:
        lines = ["DECOMPILE FAILED: %s" % e]
    for i, l in enumerate(lines[:limit]):
        w("  %4d| %s" % (i + 1, l))
    if len(lines) > limit:
        w("  ... showing lines 1-%d of %d" % (limit, len(lines)))


def head(ea, n=16):
    w("")
    w("##### FIRST %d INSTRUCTIONS %s" % (n, a(ea)))
    it = ea
    for _ in range(n):
        raw = ida_bytes.get_bytes(it, idc.get_item_size(it)) or b""
        w("  %s  %-24s %s" % (a(it), raw.hex(" "), idc.generate_disasm_line(it, 0)))
        it = idc.next_head(it, it + 32)


def strlit(ea):
    s = idc.get_strlit_contents(ea, -1, ida_nalt.STRTYPE_C)
    if s:
        return '"%s"' % s.decode(errors="replace")
    s = idc.get_strlit_contents(ea, -1, ida_nalt.STRTYPE_C_16)
    return ('L"%s"' % s.decode(errors="replace")) if s else None


def call_site_pushes(call_ea, n=6):
    """The last n immediate pushes before the call, newest first (cdecl: arg0 is pushed last)."""
    out = []
    it = call_ea
    for _ in range(24):
        it = idc.prev_head(it, 0)
        if it == idc.BADADDR:
            break
        mn = idc.print_insn_mnem(it)
        if mn == "push":
            if idc.get_operand_type(it, 0) == ida_ua.o_imm:
                v = idc.get_operand_value(it, 0)
                out.append(strlit(v) or ("0x%X" % v))
            else:
                out.append(idc.print_operand(it, 0))
            if len(out) >= n:
                break
        if mn in ("call", "retn", "ret"):
            break
    return out


decomp(va(CALL), 160)
head(va(CALL))
decomp(va(GETMOVIE), 80)

w("")
w("===== CALLERS OF %s" % a(va(CALL)))
no_literal = []
by_name = {}
for x in idautils.CodeRefsTo(va(CALL), 0):
    if idc.print_insn_mnem(x) not in ("call", "jmp"):
        continue
    f = ida_funcs.get_func(x)
    p = call_site_pushes(x)
    # newest first: arg0 movie, arg1 method, arg2 format
    method = p[1] if len(p) > 1 else "?"
    fmt = p[2] if len(p) > 2 else "?"
    w("  %s in %s %s  method=%s fmt=%s" % (a(x), a(f.start_ea) if f else "?", fname(f.start_ea) if f else "?", method, fmt))
    if not method.startswith('"'):
        no_literal.append(f.start_ea if f else x)
    else:
        by_name.setdefault(method, 0)
        by_name[method] += 1
w("")
w("  %d distinct literal methods; %d call sites without a literal method name" % (len(by_name), len(no_literal)))
for fea in sorted(set(no_literal))[:8]:
    decomp(fea, 80)
done(0)
