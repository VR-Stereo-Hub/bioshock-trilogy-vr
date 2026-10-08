# SK-1 (static, BS1 = the staged copy): WHICH vtable slots does USkeletalMesh override,
# relative to its parents ULodMesh / UMesh / UPrimitive / UObject - one of them is
# Serialize, the function that defines the mesh's on-disk layout.
#
# Route: MSVC x86 RTTI. TypeDescriptor ".?AV<Class>@@" -> the CompleteObjectLocator that
# points at it -> the vtable whose slot -1 points at that locator. Prints every vtable
# of each class with its slots, marks the slots USkeletalMesh does NOT share with
# ULodMesh, and decompiles the head of each such slot so the serializer is recognisable
# (it calls the archive operator<< on TArrays and floats in a long sequence).
#     .\tools\ida-run.ps1 -Game bs1 tools\ida\sk1_skelmesh_vtable.py
import os
import idaapi, idautils, idc, ida_auto, ida_bytes, ida_funcs, ida_hexrays, ida_name, ida_nalt

KNOWN = {
    "bs1": ("ProcessEvent", 0x375140, "558BEC8B81F8000000"),
    "bs2": ("ProcessEvent", 0x37A7E0, "558BEC8B810C010000"),
    "bsi": ("GetPlayerViewPoint", 0x1E10C0, "558BEC83E4F081ECA4000000"),
}
CLASSES = ["USkeletalMesh", "ULodMesh", "UMesh", "UPrimitive", "UObject"]

OUT_PATH = os.environ.get("BVR_IDA_OUT")
GAME = os.environ.get("BVR_IDA_GAME", "")
OUT = []
w = lambda s="": OUT.append(str(s))


def done(code=0):
    if OUT_PATH:
        with open(OUT_PATH, "w", encoding="utf-8") as fh:
            fh.write("\n".join(OUT) + "\n")
    idc.qexit(code)


if not OUT_PATH or GAME not in KNOWN:
    print("*** run through tools\\ida-run.ps1")
    idc.qexit(2)

ida_auto.auto_wait()
base = idaapi.get_imagebase()
hr = ida_hexrays.init_hexrays_plugin()
kname, krva, kbytes = KNOWN[GAME]
if (ida_bytes.get_bytes(base + krva, len(bytes.fromhex(kbytes))) or b"") != bytes.fromhex(kbytes):
    w("*** REFUSED: known-good %s bytes do not match" % kname)
    done(3)
w("known-good %s +0x%X bytes match" % (kname, krva))

a = lambda ea: "+0x%X (0x%08X)" % (ea - base, ea)


def find_bytes(pat):
    hits = []
    ea = idaapi.inf_get_min_ea()
    end = idaapi.inf_get_max_ea()
    while True:
        ea = ida_bytes.find_bytes(pat, ea, range_end=end)
        if ea == idaapi.BADADDR:
            break
        hits.append(ea)
        ea += 1
    return hits


def vtables_of(cls):
    """Every vtable whose COL names `cls`. TypeDescriptor name is at TD+8."""
    name = (".?AV%s@@" % cls).encode()
    out = []
    for s in find_bytes(name + b"\0"):
        td = s - 8
        for col_ref in idautils.DataRefsTo(td):
            col = col_ref - 12                      # COL.pTypeDescriptor is at +12
            if ida_bytes.get_dword(col) != 0:       # COL.signature 0 on x86
                continue
            for vref in idautils.DataRefsTo(col):
                vt = vref + 4                       # vtable starts after the COL pointer
                out.append((vt, ida_bytes.get_dword(col + 4)))   # (vtable, offset-in-object)
    return out


def slots(vt, limit=400):
    out = []
    for i in range(limit):
        f = ida_bytes.get_dword(vt + 4 * i)
        if not ida_funcs.get_func(f):
            break
        out.append(f)
    return out


tables = {}
for c in CLASSES:
    vts = vtables_of(c)
    if not vts:
        w("*** %s: no RTTI vtable found" % c)
        continue
    main = [v for v in vts if v[1] == 0] or vts
    tables[c] = slots(main[0][0])
    w("%s: %d vtable(s), primary %s with %d slots" % (c, len(vts), a(main[0][0]), len(tables[c])))

parent = next((c for c in CLASSES[1:] if c in tables), None)
if "USkeletalMesh" not in tables or parent is None:
    w("*** cannot compare without USkeletalMesh and one parent vtable")
    done(0)

sk, lod = tables["USkeletalMesh"], tables[parent]
w("")
w("##### slots USkeletalMesh overrides vs %s (the nearest parent with RTTI)" % parent)
for i, f in enumerate(sk):
    if i < len(lod) and lod[i] == f:
        continue
    fn = ida_funcs.get_func(f)
    size = (fn.end_ea - fn.start_ea) if fn else 0
    w("slot %3d (+0x%03X) %s size %d%s" % (i, 4 * i, a(f), size, "  (ULodMesh slot %s)" % a(lod[i]) if i < len(lod) else "  (new)"))
    if hr and size > 300:
        try:
            lines = str(ida_hexrays.decompile(f)).split("\n")
            for l in lines[:40]:
                w("      | " + l)
            w("      ... %d lines total" % len(lines))
        except Exception as e:
            w("      DECOMPILE FAILED: %s" % e)
done(0)
