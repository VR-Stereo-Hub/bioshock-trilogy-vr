# FC-1 (static, BS1): WHICH ENGINE ROUTINE RECEIVES THE HUD MOVIE'S fscommand CALLS?
# The plasmid wheel's AutoSwap sends fscommand("RunConsoleCommand","Command=SwitchHandModes")
# as it opens and fscommand("SelectPlasmid","class=...") each time the stick lands, and both
# equip the plasmid hand mid-pick (the arm swings into the frozen equip pose). To hold them
# until the wheel closes, the mod hooks the routine gameswf hands every fscommand to. This
# finds the command strings, the functions that reference them, and decompiles those.
#     .\tools\ida-run.ps1 -Game bs1 tools\ida\fc1_fscommand.py
#
# Rules (docs/IDA_WORKFLOW.md): auto_wait first, qexit last; output to BVR_IDA_OUT, outside
# the repo (a decompile is game-derived and never committed); RVAs and VAs both printed;
# every negative path prints; the known-good check refuses a wrong binary. No UTF-8 BOM.
import os
import idaapi, idautils, idc, ida_auto, ida_bytes, ida_funcs, ida_hexrays, ida_name, ida_nalt

KNOWN = {
    "bs1": ("ProcessEvent",       0x375140, "558BEC8B81F8000000"),
    "bs2": ("ProcessEvent",       0x37A7E0, "558BEC8B810C010000"),
    "bsi": ("GetPlayerViewPoint", 0x1E10C0, "558BEC83E4F081ECA4000000"),
}
NAMES = ["FSCommand:", "fscommand:", "FSCommand", "MovieEventData"]
DECOMPILE_FOR = ("FSCommand:", "MovieEventData")

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
w("imagebase 0x%X  hexrays %s" % (base, "OK" if hr else "*** MISSING ***"))


def a(ea):
    return "+0x%X (0x%08X)" % (ea - base, ea)


def fname(ea):
    return ida_name.get_name(ea) or ("sub_%X" % ea)


kname, krva, kbytes = KNOWN[GAME]
want = bytes.fromhex(kbytes)
got = ida_bytes.get_bytes(base + krva, len(want)) or b""
if got != want:
    w("*** REFUSED: known-good %s bytes at +0x%X are %s, expected %s ***" % (kname, krva, got.hex(" "), want.hex(" ")))
    done(3)
w("known-good %s +0x%X bytes match" % (kname, krva))


def decomp(ea, limit=260):
    f = ida_funcs.get_func(ea)
    if not f:
        w("*** NO FUNCTION AT %s ***" % a(ea))
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
        w("  ... CUT at %d of %d lines" % (limit, len(lines)))


def find_all(pat):
    hits = []
    ea = base
    end = idc.get_inf_attr(idc.INF_MAX_EA)
    while True:
        ea = ida_bytes.find_bytes(pat, ea, range_end=end)
        if ea is None or ea == idc.BADADDR:
            break
        hits.append(ea)
        ea += 1
    return hits


seen = set()
for nm in NAMES:
    for enc, pat in (("ascii", nm.encode() + b"\x00"), ("utf16", nm.encode("utf-16-le") + b"\x00\x00")):
        hits = find_all(pat)
        w("")
        w("===== %s (%s): %d string(s)" % (nm, enc, len(hits)))
        for ea in hits:
            refs = list(idautils.XrefsTo(ea))
            w("  %s  %d ref(s)" % (a(ea), len(refs)))
            for x in refs:
                f = ida_funcs.get_func(x.frm)
                if not f:
                    w("    ref %s (no function)" % a(x.frm))
                    continue
                w("    ref %s in %s %s" % (a(x.frm), a(f.start_ea), fname(f.start_ea)))
                if nm in DECOMPILE_FOR:
                    # The instructions after the prefix test: where the callback is loaded
                    # (a global function pointer) and how it is called.
                    w("    disasm after %s:" % a(x.frm))
                    it = x.frm
                    for _ in range(48):
                        raw = ida_bytes.get_bytes(it, idc.get_item_size(it)) or b""
                        w("      %s  %-22s %s" % (a(it), raw.hex(" "), idc.generate_disasm_line(it, 0)))
                        it = idc.next_head(it, idc.BADADDR)
                if nm in DECOMPILE_FOR and f.start_ea not in seen and nm == "MovieEventData":
                    seen.add(f.start_ea)
                    decomp(f.start_ea)
                    callers = list(idautils.XrefsTo(f.start_ea))
                    w("    references to %s: %d" % (a(f.start_ea), len(callers)))
                    for r in callers[:16]:
                        cf = ida_funcs.get_func(r.frm)
                        w("      %s in %s" % (a(r.frm), a(cf.start_ea) if cf else "(data - a vtable or a table)"))
                    w("    head bytes %s" % (ida_bytes.get_bytes(f.start_ea, 32) or b"").hex(" "))
if not seen:
    w("NONE: no function references the command strings")
done(0)
