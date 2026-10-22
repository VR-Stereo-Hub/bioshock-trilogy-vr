# TEMPLATE-DERIVED (static): THE SET COMMAND'S PROPERTY WALK. rf2 found "Unrecognized property %s"
# in RVA 0x6FAC20 (the console's SET/GET); this prints the decompile around each use of a property
# name lookup there - the loop over a class's fields - so UStruct::Children, UField::Next and
# UProperty::Offset can be read off. For the cheats.
#     .\tools\ida-run.ps1 -Game bs1 tools\ida\rf3_set_command.py
#
# Rules this skeleton encodes (docs/IDA_WORKFLOW.md says why each one exists): auto_wait() first,
# qexit(0) last; output OUTSIDE the repo (BVR_IDA_OUT); RVAs and VAs; every negative path prints;
# the known-good check refuses on a mismatch; this game only. Ported from the Dishonored VR mod.
import os
import idaapi, idc, ida_auto, ida_bytes, ida_funcs, ida_hexrays, ida_nalt

TARGET = {"bs1": int(os.environ.get("BVR_RF3_RVA", "0x6FAC20"), 16)}
KEYS = ["Unrecognized", "Next", "for (", "while", "PropertyFlags"]
KNOWN = {"bs1": ("ProcessEvent", 0x375140, "558BEC8B81F8000000")}

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
k = KNOWN[GAME]
got = ida_bytes.get_bytes(base + k[1], len(k[2]) // 2) or b""
if got.hex().upper() != k[2]:
    w("known-good %s: *** MISMATCH %s ***" % (k[0], got.hex()))
    done(1)
w("known-good %s at RVA 0x%X: OK" % (k[0], k[1]))
if not hr:
    w("*** no decompiler ***")
    done(1)
try:
    lines = str(ida_hexrays.decompile(base + TARGET[GAME])).splitlines()
except Exception as e:
    w("DECOMPILE FAILED: %s" % e)
    done(1)
body = [i for i, l in enumerate(lines) if l.strip() == "" or "//" not in l]
start = 0
w("decompile: %d lines; body from line %d" % (len(lines), start))
for i in range(start, len(lines)):
    w("%5d %s" % (i, lines[i]))
done(0)
