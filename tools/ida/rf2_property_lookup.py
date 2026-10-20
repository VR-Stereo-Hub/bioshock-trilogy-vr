# TEMPLATE-DERIVED (static): WHERE DOES THIS BUILD LOOK A PROPERTY UP BY NAME? The strings the
# property-by-name paths report ("Unrecognized property", "not found" beside "property", the SET
# console command's), every function that references them, decompiled, so the field chain -
# UStruct::Children, UField::Next - and UProperty::Offset can be read where they are used. For the
# cheats (rf1 found FindFunction walks a function HASH, not the chain).
#     .\tools\ida-run.ps1 -Game bs1 tools\ida\rf2_property_lookup.py
#
# Rules this skeleton encodes (docs/IDA_WORKFLOW.md says why each one exists): auto_wait() first,
# qexit(0) last; output OUTSIDE the repo (BVR_IDA_OUT); RVAs and VAs; every negative path prints;
# the known-good check refuses on a mismatch; this game only. Ported from the Dishonored VR mod.
import os
import idaapi, idautils, idc, ida_auto, ida_bytes, ida_funcs, ida_hexrays, ida_nalt

KNOWN = {
    "bs1": ("ProcessEvent",       0x375140, "558BEC8B81F8000000"),
}
NEEDLES = ["unrecognized property", "property not found", "bad property", "unknown property",
           "not a property", "property '%s'"]

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
    print("*** BVR_IDA_OUT / BVR_IDA_GAME not set - run this through tools\ida-run.ps1 ***")
    idc.qexit(2)

ida_auto.auto_wait()
base = idaapi.get_imagebase()
hr = ida_hexrays.init_hexrays_plugin()
w("game %s  input %s  md5 %s" % (GAME, ida_nalt.get_root_filename(), (ida_nalt.retrieve_input_file_md5() or b"").hex()))
k = KNOWN[GAME]
got = ida_bytes.get_bytes(base + k[1], len(k[2]) // 2) or b""
if got.hex().upper() != k[2]:
    w("known-good %s at RVA 0x%X: *** MISMATCH %s ***" % (k[0], k[1], got.hex()))
    done(1)
w("known-good %s at RVA 0x%X: OK" % (k[0], k[1]))

seen = set()
hits = 0
strs = idautils.Strings(default_setup=False)
strs.setup(strtypes=[ida_nalt.STRTYPE_C, ida_nalt.STRTYPE_C_16], minlen=6)
for s in strs:
    t = str(s)
    if not any(n in t.lower() for n in NEEDLES):
        continue
    w("")
    w("string @RVA 0x%X: %r" % (s.ea - base, t[:140]))
    for x in idautils.XrefsTo(s.ea):
        f = ida_funcs.get_func(x.frm)
        if not f:
            w("  xref 0x%X: NO FUNCTION AT" % (x.frm - base))
            continue
        w("  xref from RVA 0x%X in RVA 0x%X" % (x.frm - base, f.start_ea - base))
        if f.start_ea in seen or len(seen) >= 6:
            continue
        seen.add(f.start_ea)
        hits += 1
        if hr:
            try:
                c = str(ida_hexrays.decompile(f.start_ea)).splitlines()
                w("    " + "\n    ".join(c[:120]))
            except Exception as e:
                w("    DECOMPILE FAILED: %s" % e)
w("")
w("functions decompiled: %d" % hits if hits else "NONE")
done(0)
