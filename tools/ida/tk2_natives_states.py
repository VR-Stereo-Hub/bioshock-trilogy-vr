# TK-2 (static, BS1): WHAT DO THE TELEKINESIS NATIVES DO, AND WHAT ARE ITS C++ STATE OBJECTS? Decompiles the five
# UTelekinesisAbility exec thunks and the two functions that name the class, lists every function in the class's
# translation unit (RVA 0x1C0000-0x1C3800) with its callers, and prints the vtables of the C++ helper classes that
# tk1 found by RTTI (TelekinesisState and its Idle/Pulling/Holding/Throwing/Dropping subclasses, TelekinesisController
# and its RigidBody/Ragdoll subclasses, TelekinesisCollisionDamageListener).
#     .\tools\ida-run.ps1 -Game bs1 tools\ida\tk2_natives_states.py
# - Output path from BVR_IDA_OUT (outside the repo); decompiles are never committed. Findings go to the docs.
# - Every address is printed as RVA and VA. Negative paths print NONE / NO FUNCTION AT / DECOMPILE FAILED.
# - The known-good check re-derives ProcessEvent and refuses on a mismatch.
# - Save without a UTF-8 BOM.
import os
import idaapi, idautils, idc, ida_auto, ida_bytes, ida_funcs, ida_hexrays, ida_name, ida_nalt, ida_search

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
    print("*** BVR_IDA_OUT / BVR_IDA_GAME not set - run this through tools\ida-run.ps1 ***")
    idc.qexit(2)

ida_auto.auto_wait()
base = idaapi.get_imagebase()
hr = ida_hexrays.init_hexrays_plugin()
w("game %s  input %s  md5 %s" % (GAME, ida_nalt.get_root_filename(), (ida_nalt.retrieve_input_file_md5() or b"").hex()))
w("imagebase 0x%X  hexrays %s" % (base, "OK" if hr else "*** MISSING ***"))
kname, krva, kbytes = KNOWN[GAME]
want = bytes.fromhex(kbytes)
got = ida_bytes.get_bytes(base + krva, len(want)) or b""
if got != want:
    w("*** REFUSED: known-good %s bytes mismatch ***" % kname)
    done(3)
w("known-good %s +0x%X bytes match" % (kname, krva))


def a(ea):
    return "+0x%X (0x%08X)" % (ea - base, ea)


def fl(ea):
    f = ida_funcs.get_func(ea)
    if not f:
        return "NO FUNCTION AT %s" % a(ea)
    return "%s %s" % (a(f.start_ea), ida_funcs.get_func_name(f.start_ea))




def decomp(ea, limit=160):
    f = ida_funcs.get_func(ea)
    if not f:
        w("*** NO FUNCTION AT %s ***" % a(ea))
        return
    w("")
    w("##### DECOMPILE %s" % fl(f.start_ea))
    try:
        lines = str(ida_hexrays.decompile(f.start_ea)).split("\n")
    except Exception as e:
        lines = ["DECOMPILE FAILED: %s" % e]
    for i, l in enumerate(lines[:limit]):
        w("  %4d| %s" % (i + 1, l))
    if len(lines) > limit:
        w("  ... CUT: showing 1-%d of %d" % (limit, len(lines)))


def callers(ea, n=10):
    xs = [x.frm for x in idautils.XrefsTo(ea)]
    return ", ".join(("%s<%s>" % (("+0x%X" % (x - base)), (ida_funcs.get_func_name(ida_funcs.get_func(x).start_ea) if ida_funcs.get_func(x) else "data"))) for x in xs[:n]) + (" ...(%d)" % len(xs) if len(xs) > n else "")


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


def rtti_vtables(mangled):
    res = []
    for td_name in find_all(mangled.encode() + b"\0"):
        td = td_name - 8
        for x in idautils.XrefsTo(td):
            col = x.frm - 12
            if ida_bytes.get_dword(col) != 0:
                continue
            for y in idautils.XrefsTo(col):
                res.append(y.frm + 4)
    return res


def vt_slots(vt, maxn=64):
    out = []
    ea = vt
    while len(out) < maxn:
        d = ida_bytes.get_dword(ea)
        if not ida_funcs.get_func(d):
            break
        if ea != vt and any(True for _ in idautils.XrefsTo(ea)):
            break
        out.append(d)
        ea += 4
    return out


w("")
w("##### C++ helper vtables")
for c in ["TelekinesisState", "TelekinesisIdle", "TelekinesisPulling", "TelekinesisHolding", "TelekinesisThrowing",
          "TelekinesisDropping", "TelekinesisController", "TelekinesisRigidBodyController",
          "TelekinesisRagdollController", "TelekinesisCollisionDamageListener"]:
    vts = rtti_vtables(".?AV%s@@" % c)
    if not vts:
        w("  %s: NONE" % c)
    for vt in vts:
        sl = vt_slots(vt)
        w("  %s vtable %s, %d slots; vtable referenced from: %s" % (c, a(vt), len(sl), callers(vt)))
        for i, f in enumerate(sl):
            w("     slot %2d +0x%02X %s" % (i, 4 * i, fl(f)))

for t in [0x1C0F40, 0x1C1860, 0x1C0D30, 0x1C0DE0, 0x1C2370, 0x1C1A60, 0x1C2450]:
    decomp(base + t)

w("")
w("##### functions in 0x1BFF00-0x1C3800 and their callers")
f = ida_funcs.get_next_func(base + 0x1BFF00)
while f and f.start_ea < base + 0x1C3800:
    w("  %s size 0x%X  callers: %s" % (fl(f.start_ea), f.end_ea - f.start_ea, callers(f.start_ea)))
    f = ida_funcs.get_next_func(f.start_ea)
w("")
w("done")
done(0)
