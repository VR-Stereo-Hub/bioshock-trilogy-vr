# LOCAL ONLY: NativeRepairProof stereo constant derivation for BS1.
# One question: which native matrices/values populate the reflected foreground
# view, eye, clip, screen and fog fields? Outputs remain outside the repository.
# The named strings find registrations independently; setter targets below were
# derived through those class vtables and then their calls, not borrowed from
# another game. No hook or engine-memory write is installed by this script.
import os, re
from pathlib import Path
import idaapi, idautils, idc, ida_auto, ida_bytes, ida_funcs, ida_hexrays, ida_name, ida_nalt

# Keep every raw BS1 RVA/signature in the adapter's central table.
PATTERNS = (Path(__file__).resolve().parents[2] / "src/game/bioshock1r/patterns.h").read_text(encoding="utf-8")
def value(name):
    match = re.search(r"\b" + name + r"(?:\[\])?\s*=\s*(.*?);", PATTERNS, re.S)
    if not match:
        raise RuntimeError("missing research entry " + name)
    return match.group(1).strip()
def rvas(name):
    return [int(x, 16) for x in re.findall(r"0x[0-9A-Fa-f]+", value(name))]

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
    print("*** BVR_IDA_OUT / BVR_IDA_GAME not set - run this through tools\\ida-run.ps1 ***")
    idc.qexit(2)

ida_auto.auto_wait()
base = idaapi.get_imagebase()
hr = ida_hexrays.init_hexrays_plugin()
w("game %s  input %s  md5 %s" % (GAME, ida_nalt.get_root_filename(), (ida_nalt.retrieve_input_file_md5() or b"").hex()))
w("imagebase 0x%X  hexrays %s" % (base, "OK" if hr else "*** MISSING (no x86 decompiler licence?) ***"))


def va(rva):
    return base + rva


def a(ea):
    return "+0x%X (0x%08X)" % (ea - base, ea)


def fname(ea):
    return ida_name.get_name(ea) or ("sub_%X" % ea)


if (ida_nalt.retrieve_input_file_md5() or b"").hex() != value("kNativeConstantResearchMd5").strip('"'):
    w("REFUSED: staged BS1 MD5 does not match the derived build")
    done(3)
kname = "ProcessEvent"
krva = int(value("kNativeConstantAnchorRva"), 16)
kbytes = value("kNativeConstantAnchorBytes").strip('"')
want = bytes.fromhex(kbytes)
got = ida_bytes.get_bytes(va(krva), len(want)) or b""
if got != want:
    w("*** REFUSED: known-good %s bytes at +0x%X are %s, expected %s - wrong binary or a game update ***"
      % (kname, krva, got.hex(" "), want.hex(" ")))
    done(3)
w("known-good %s +0x%X bytes match" % (kname, krva))


def decomp(ea, first=1, limit=400):
    f = ida_funcs.get_func(ea)
    if not f:
        w("*** NO FUNCTION AT %s ***" % a(ea))
        return
    w("")
    w("##### DECOMPILE %s (function %s %s)" % (a(ea), a(f.start_ea), fname(f.start_ea)))
    try:
        lines = str(ida_hexrays.decompile(f.start_ea)).split("\n")
    except Exception as e:
        lines = ["DECOMPILE FAILED: %s" % e]
    window = lines[first - 1:first - 1 + limit]
    for i, l in enumerate(window):
        w("  %4d| %s" % (first + i, l))
    if len(lines) > first - 1 + limit or first > 1:
        w("  ... showing lines %d-%d of %d" % (first, first - 1 + len(window), len(lines)))


def disasm(ea):
    f = ida_funcs.get_func(ea)
    if not f:
        w("*** NO FUNCTION AT %s ***" % a(ea))
        return
    items = list(idautils.FuncItems(f.start_ea))
    w("")
    w("##### DISASM %s (%d instructions)" % (a(f.start_ea), len(items)))
    for it in items:
        raw = ida_bytes.get_bytes(it, idc.get_item_size(it)) or b""
        w("  %s  %-24s %s" % (a(it), raw.hex(" "), idc.generate_disasm_line(it, 0)))


def vtable_class(slot_ea):
    # Walk back to the vtable start (first dword whose predecessor is not a function
    # pointer). MSVC x86 RTTI: CompleteObjectLocator pointer at vt-4, TypeDescriptor
    # pointer (absolute) at COL+12, mangled name at TD+8.
    ea = slot_ea
    while ida_funcs.get_func(ida_bytes.get_dword(ea - 4)) is not None:
        ea -= 4
    col = ida_bytes.get_dword(ea - 4)
    try:
        td = ida_bytes.get_dword(col + 12)
        name = (idc.get_strlit_contents(td + 8) or b"*** COL UNREADABLE ***").decode(errors="replace")
    except Exception as e:
        name = "*** COL UNREADABLE (%s) ***" % e
    return ea, (slot_ea - ea) // 4, name


def refs(ea):
    f = ida_funcs.get_func(ea)
    if not f:
        return
    n = 0
    w("")
    w("##### REFERENCES TO %s" % a(f.start_ea))
    for x in idautils.XrefsTo(f.start_ea):
        n += 1
        cf = ida_funcs.get_func(x.frm)
        if cf:
            w("  code %s in %s %s" % (a(x.frm), a(cf.start_ea), fname(cf.start_ea)))
        elif ida_bytes.get_dword(x.frm) != f.start_ea:
            w("  data %s in %s - NOT a pointer slot, not a vtable" % (a(x.frm), idc.get_segm_name(x.frm)))
        else:
            vt, slot, cls = vtable_class(x.frm)
            w("  data %s = vtable %s slot %d (+0x%X) class %s" % (a(x.frm), a(vt), slot, 4 * slot, cls))
    if n == 0:
        w("  NONE (no code or data reference found)")



if GAME != "bs1":
    w("REFUSED: this derivation is BS1 only")
    done(4)
names = {"worldViewProj", "localEyePos", "vertexClipPlane", "screenTransform",
         "screenDataToCamera", "fogTransform", "localToWorld"}
found = set()
for st in idautils.Strings():
    name = str(st)
    if name not in names:
        continue
    found.add(name)
    w("STRING %s %s" % (name, a(st.ea)))
    for x in idautils.XrefsTo(st.ea):
        fn = ida_funcs.get_func(x.frm)
        w("  XREF %s function %s" % (a(x.frm), a(fn.start_ea) if fn else "NONE"))
for name in sorted(names - found):
    w("MISSING STRING " + name)
# Register names, upload the common model/view fields, derive matrix kinds,
# and copy the material fog fields. Full decompiles are intentionally local.
for rva in rvas("kNativeConstantResearchRvas"):
    decomp(va(rva), limit=1100)
# Exact typed per-pixel lights: model/light payload, not the camera projection.
for rva in rvas("kNativeConstantResearchLightRvas"):
    decomp(va(rva), limit=1100)
# Assembly makes the transposed upload and accessor's input selection auditable.
for rva in rvas("kNativeConstantAssemblyRvas"):
    disasm(va(rva))
done(0)
