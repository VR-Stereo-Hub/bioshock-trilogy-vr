# Static reverse engineering with headless IDA

IDA runs **headless**, driven by small IDAPython scripts. Each script answers **one
question** and writes a text file. Nobody needs the IDA GUI: a session writes the script,
runs it, reads the output and writes the next one. It sits next to the repo's own offline
toolkit (`disasm-rva.py`, `pe-xref.ps1`, `ue3-natives.py`) and adds what those cannot do:
a decompiler, cross-references over the whole image, types and a saved database.

Ported from the Dishonored VR mod (its `docs/IDA_WORKFLOW.md`), where it answered the
cinematic-mode and render-stage questions in about four seconds a script. Two things
differ here: **one workspace per game** (`-Game bs1|bs2|bsi`), and **RVAs, not VAs**,
because these three exes relocate (ASLR - BS1 loaded at `0x102F0000` on 2026-09-07 against
a preferred base of `0x10900000`) and every `patterns.h` here stores RVAs.

**IDA is paid software.** Nothing of it is in the repo. Its location, and the workspace
that holds the staged binaries and their databases, come from the local tool file
(`tools\tool-paths.ps1`, see `docs/TOOLS.md`). A machine without IDA can still use the
free toolkit; Ghidra (free) is the fallback if a decompiler is needed.

## 1. Setup (once per machine, once per game update)

```powershell
.\tools\tool-paths.ps1 -Init                 # finds idat.exe (IDA 9 has one headless binary for 32 and 64 bit)
.\tools\ida-run.ps1 -Game bs1 -Stage         # copy BioshockHD.exe out of the game, analyse it, save the .i64
```

- `-Stage` copies the exe into `<ida_workspace>\bin\<game>\` (default
  `%LOCALAPPDATA%\BioshockVR\ida\bin\bs1`) and records its md5. IDA writes database files
  next to whatever it opens, so it never opens the game folder's copy.
- The first run is the slow one: auto-analysis of a 20 MB exe (BS1: 67 minutes, 79,770
  functions, on 2026-10-07; every later script ran in about 5 seconds). `tools\ida\analyze.py`
  then reports the health of the whole pipeline: IDAPython ran, the segments and function
  count, whether the x86 decompiler is licensed, and that a published anchor is where the
  game's patterns say it is:

  | game | anchor | RVA | bytes the mod itself verifies |
  |---|---|---|---|
  | bs1 | `UObject::ProcessEvent` outer | `0x375140` | `55 8B EC 8B 81 F8 00 00 00` (re-read with `disasm-rva.py`, 2026-10-07) |
  | bs2 | `UObject::ProcessEvent` outer | `0x37A7E0` | `bioshock2r/patterns.h kProcessEventPrologue` |
  | bsi | `APlayerController::GetPlayerViewPoint` | `0x1E10C0` | `bioshockinf/patterns.h kGetPlayerViewPointPrologue` |

  Later scripts open the saved `.i64` and skip analysis.
- **Every run compares the staged md5 with the deployed exe** and stamps the result on the
  first line of the output. A mismatch means a game update: every address in the database
  may be stale. `-Stage -Force` re-stages and keeps the old database as `.i64.<md5 prefix>`.
- `ida-run.ps1` never launches a game, so it needs no BS2/Infinite conflict guard.

## 2. Running a script

```powershell
.\tools\ida-run.ps1 -Game bs1 tools\ida\pe1_callers.py
```

The wrapper runs `idat -A -S"<script>" -L"<log>" <exe>.i64` from the workspace's
`bin\<game>\` with `Start-Process -Wait`, sets `BVR_IDA_OUT` (where to write) and
`BVR_IDA_GAME` (which anchor to verify) for the script, and prints where the output went:
`<ida_workspace>\out\<game>\<script>_out.txt`.

**Exit 0 and an output file = it ran. Anything else = the script threw**; the wrapper prints
the tail of IDA's `-L` log, which holds the Python traceback. Read it, do not guess.

## 3. The script shape

Copy `tools\ida\template.py`. It is runnable as it stands (it decompiles each game's
anchor) and encodes the rules:

| rule | why |
|---|---|
| The header states the ONE question; the wrapper stamps game, binary, md5 and date on the output | six weeks later the output is its own provenance |
| `ida_auto.auto_wait()` first, `idc.qexit(0)` last | headless IDA never exits without `qexit` |
| Output path from `BVR_IDA_OUT`, outside the repo | a decompile is game-derived text; it is never committed |
| Targets are **RVAs**, per game; print every address as RVA **and** VA (`+0x375140 (0x10C75140)`) | `patterns.h` and crash offsets speak RVAs; IDA speaks VAs at the preferred base |
| Every negative path prints (`NO FUNCTION AT`, `DECOMPILE FAILED`, `NONE`, `showing lines`) | an empty section and a failed lookup must not look the same |
| Print instruction **bytes** with the disassembly | every hook here byte-verifies its target before it writes, and those bytes come from here |
| Re-derive the game's published anchor before answering and refuse on a mismatch | a route that cannot reproduce a known answer is not evidence about a new one |
| 32-bit: 4-byte vtable slots; MSVC x86 RTTI holds **absolute** pointers (COL at vt-4, TypeDescriptor at COL+12, name at TD+8) | the x64 recipe (RVAs, 8-byte slots) silently reads garbage here |
| No UTF-8 BOM | IDA exits 1 with `SyntaxError ... U+FEFF`; the wrapper and `tools\lint.ps1` refuse such a file |

Reusable blocks (copy between scripts rather than building a shared library, so each script
stays runnable alone): string to code (find the string, `idautils.DataRefsTo`, decompile the
referencing functions; check the byte before the hit is NUL), callers and callers of callers
(`XrefsTo`, one or two hops), which class owns a virtual (`vtable_class` in the template), a
line window of a long decompile (`decomp(ea, first=88, limit=60)`), field users (decompile a
range, keep functions whose text contains `+ 0x3FC`), and types without the GUI
(`ida_typeinf.parse_decls` on header TEXT).

**RTTI is game-specific.** BS1 carries engine RTTI (its `SkeletonInstance` and
`SharedSkeletonData` vtables were named that way, `docs/bioshock1/ENGINE_NOTES.md`); BS2's
is not recorded either way - check before relying on it.
Infinite's UE3 was built `/GR-`: its 270 type descriptors are all third-party libraries,
so `vtable_class` names nothing engine-side there - use the native table
(`tools\ue3-natives.py`) or GNames instead (`docs/bioshockinfinite/ENGINE_NOTES.md`,
"Dead ends").

## 4. Series

A question is rarely answered by one script; each output says what the next should ask.
Scripts come in numbered series with a short prefix and live in `tools\ida\`:
`<series><n>_<what>.py` (`sk1_skeleton_eval.py`, `sk2_bone_writers.py`, ...). The
**scripts** are ours and are committed; keep old series, because the next question in the
same area starts by grepping the old outputs in `<ida_workspace>\out\<game>\`.

## 5. Where IDA sits in the order of work

1. **Search what is already known first**, and say what was searched and what it returned,
   including "nothing": the game's `ENGINE_NOTES.md`, its `patterns.h`, the UnrealScript
   corpus (`tools\uscript\<game>\`), earlier IDA outputs. Search for the address and the
   function NAME, not just the symptom words.
2. **Then the cheap offline tools**: `disasm-rva.py` (bytes, constants, callers,
   displacement users), `pe-xref.ps1` (caller census), `ue3-natives.py` (Infinite: native
   name -> thunk). They answer most "where is" questions in one command.
3. **Then IDA** for structure: offsets, call order, branch conditions, who writes a field,
   which class a method belongs to, whole-image xrefs.
4. **Then a runtime probe** (the command seam, the simulator) only for what a decompile
   cannot say: timing, rates, which branch actually ran, whether a write landed.
5. **A headset run is last.** Name the static source read and what it said before asking.

**Comparison sources form hypotheses; only our binary confirms them.** Another game's
notes, BRVR's, BS2's for BS1 or a public engine tree tell you what to look for and what it
is called. A finding is established only when the same thing is read out of the staged exe
of the game it is about. **Never copy an address between games** - same engine tree,
different link.

**Crashes:** the crash dump comes first. `tools\read-dump.py` gives the faulting module and
offset; hand that RVA to a script. Stack scans produce false frames: a candidate return
address only counts if the instruction before it is a `call`.

## 6. Turning a finding into something that lasts

- The finding goes to the game's `ENGINE_NOTES.md` the same session, with the script name
  and the md5 it ran against. A new address goes to that game's `patterns.h` with its
  derivation.
- Corrections go into the file that was wrong, not only into chat. Unknowns are written
  down as unknown, never filled from "how engines usually work".
- A runtime patch checks the bytes the script printed and logs a loud refusal on mismatch.
- Read the disassembly as well as the decompile wherever a register or exact offset matters;
  Hex-Rays can hide a register reused after a call.
- Decide what a function does by reading it, never from its name (including names an
  earlier session gave it).
- When static analysis is exhausted, say so in writing in the notes so it is not repeated.

## 7. Traps

- A script saved with a BOM does not run (section 3).
- Not every data reference to a function is a vtable slot (jump tables, exception records):
  check the dword at the reference really is the function's address. The template does.
- A silently truncated decompile is worse than none; always print how much was cut.
- A tool that is not installed returns nothing, which looks like "not found". Check
  `.\tools\tool-paths.ps1` before reading a tool's silence.
- A numeric match is not a reading: a value that fits a formula does not prove the path ran.
- `hasattr`-style probes that cannot fail are not checks (the same trap cost Dishonored's
  Blender setup one false PASS; see `docs/MODEL_WORKFLOW.md`).
- IDA's VAs are at the PREFERRED base, the running game's are not: convert through the RVA
  every time a number crosses between IDA and a live log.
