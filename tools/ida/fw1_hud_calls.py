# FW-1 (static, BS1): WHICH NATIVE CODE CALLS THE HUD MOVIE'S WHEEL FUNCTIONS, AND THROUGH WHAT?
# The weapon/plasmid dial (docs/bioshock1/WEAPON_WHEEL.md 3.8) needs to know when a wheel opens
# and how many slots are filled. The HUD movie learns both from calls BY NAME: AddWeapon,
# AddAbility, RemoveWeapon, EmptyWeapon, OpenWeaponMenu, LBReleased, ... Some come from
# UnrealScript (FlashMovie.CallMethod* natives), the rest from C++. This finds every string with
# those names in the exe (ASCII and UTF-16), the functions that reference them, and decompiles
# the first lines of each, so the shared "call a method on a gameswf movie" routine shows itself.
#     .\tools\ida-run.ps1 -Game bs1 tools\ida\fw1_hud_calls.py
# - The output path comes from BVR_IDA_OUT, set by ida-run.ps1. It is OUTSIDE the repo:
#   a decompile is game-derived text and is never committed. Findings go to ENGINE_NOTES.
# - TARGETS are RVAs; every address prints as RVA AND VA. Every negative path prints.
# - The known-good check re-derives ProcessEvent and refuses on a mismatch.
# - Save without a UTF-8 BOM (IDA refuses it: SyntaxError U+FEFF). ida-run.ps1 checks.
import os
import idaapi, idautils, idc, ida_auto, ida_bytes, ida_funcs, ida_hexrays, ida_name, ida_nalt

NAMES = ["AddWeapon", "AddAbility", "RemoveWeapon", "RemoveAbility", "EmptyWeapon", "EmptyAbility",
         "OpenWeaponMenu", "OpenAbilityMenu", "LBReleased", "RBReleased", "ContextOpenWeaponMenu",
         "ContextOpenAbilityMenu", "ContextLBReleased", "ContextRBReleased", "SelectWeapon",
         "SelectAbility", "UpdateRadialAxis", "execCallMethodVoid", "execCallMethodString",
         "CallMethodVoid", "CallMethodString", "WeaponSelectionMenu", "AbilitySelectionMenu",
         "SetActiveHands", "MGPendingWeapon", "SelectedWeapon", "SelectedAbility"]

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


def decomp(ea, limit=60):
    f = ida_funcs.get_func(ea)
    if not f:
        w("    *** NO FUNCTION AT %s ***" % a(ea))
        return
    try:
        lines = str(ida_hexrays.decompile(f.start_ea)).split("\n")
    except Exception as e:
        lines = ["DECOMPILE FAILED: %s" % e]
    for i, l in enumerate(lines[:limit]):
        w("    %4d| %s" % (i + 1, l))
    if len(lines) > limit:
        w("    ... showing lines 1-%d of %d" % (limit, len(lines)))


# Every string (ASCII and UTF-16) equal to one of the names.
want_names = set(NAMES)
sc = idautils.Strings()
sc.setup(strtypes=[ida_nalt.STRTYPE_C, ida_nalt.STRTYPE_C_16])
found = {}
for s in sc:
    txt = str(s)
    if txt in want_names:
        found.setdefault(txt, []).append((s.ea, s.strtype))

seen_funcs = {}
for n in NAMES:
    w("")
    w("===== %s" % n)
    if n not in found:
        w("  NONE (no string)")
        continue
    for ea, st in found[n]:
        w("  string %s %s" % (a(ea), "utf16" if st == ida_nalt.STRTYPE_C_16 else "ascii"))
        xs = list(idautils.DataRefsTo(ea))
        if not xs:
            w("    NONE (no reference)")
        for x in xs:
            f = ida_funcs.get_func(x)
            if f:
                w("    ref %s in %s %s" % (a(x), a(f.start_ea), fname(f.start_ea)))
                seen_funcs.setdefault(f.start_ea, set()).add(n)
            else:
                w("    ref %s (data, %s)" % (a(x), idc.get_segm_name(x)))

# The callees every referencing function has in common point at the shared call routine.
w("")
w("===== CALLEES SHARED BY THE REFERENCING FUNCTIONS")
callee_count = {}
for fea in seen_funcs:
    f = ida_funcs.get_func(fea)
    callees = set()
    for it in idautils.FuncItems(f.start_ea):
        if idc.print_insn_mnem(it) == "call":
            t = idc.get_operand_value(it, 0)
            if ida_funcs.get_func(t):
                callees.add(t)
    for t in callees:
        callee_count[t] = callee_count.get(t, 0) + 1
for t, c in sorted(callee_count.items(), key=lambda kv: -kv[1])[:15]:
    w("  %2d of %d  %s %s  xrefs %d" % (c, len(seen_funcs), a(t), fname(t), len(list(idautils.CodeRefsTo(t, 0)))))

w("")
w("===== DECOMPILES (the referencing functions, first 60 lines)")
for fea, ns in sorted(seen_funcs.items())[:12]:
    w("")
    w("  ##### %s %s  (names: %s)" % (a(fea), fname(fea), ", ".join(sorted(ns))))
    decomp(fea)

done(0)
