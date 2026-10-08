# Tools - check these before deriving anything

This project has paid for a set of instruments. Sessions keep re-deriving things one of
them answers in a single command. **Before hand-walking a disassembly, guessing an offset,
writing a new probe or asking for a headset run, find the tool below that applies and use
it.** Say which one was used, or why none applies. If a needed tool is missing on the
machine, say so; do not silently fall back to guessing.

**The rule that governs all of them: the scripts are ours and are committed; their OUTPUT
is game-derived and never is.** Summarise findings into the game's `ENGINE_NOTES.md`; keep
dumps, addresses in bulk, decompiled text, extracted models and databases out of the tree.
Third-party and paid programs (IDA, Blender, UModel, UE Explorer, FFDec, ProcDump, ...) are
never copied into the repo either - they are referenced by path.

Most of this set was ported on 2026-10-07 from the Dishonored VR mod, which grew it out of
this repo's own toolkit (`disasm-rva.py`, `pe-xref.ps1` and `read-dump.py` started here).

## The local tool file - where the tools are on THIS machine

```powershell
.\tools\tool-paths.ps1              # table: every tool, its path, ok / MISSING
.\tools\tool-paths.ps1 -Init        # detect and write the file (run this if it does not exist yet)
.\tools\tool-paths.ps1 -Set blender=D:\Apps\Blender\blender.exe
.\tools\tool-paths.ps1 -Get idat    # one path, for a script
```

- The file is `%LOCALAPPDATA%\BioshockVR\dev-tools.json` (override: `BVR_TOOLS_FILE`). It
  is **per user and never committed**, so every contributor keeps their tools wherever they
  like. Scripts resolve tools through `tools\lib\tool-paths.ps1` (`Get-BvrTool <name>`),
  which throws with the exact `-Set` command when one is missing.
- It also resolves each game's folders (`game_bs1`, `content_bs1`, ...) through the same
  per-machine resolver every other script uses (`tools\lib\resolve-game-path.ps1`).
- **At session start, if the file does not exist, run `-Init` first.** A `MISSING` tool the
  task needs: ask the user where it is, or find the established free tool for the job and
  **ask before downloading anything**. Record it with `-Set`.
- **Never write a machine-specific path into a committed file.** Add a default-location
  rule to the catalog in `tools\lib\tool-paths.ps1` instead.

## The catalog, by question

### Static reverse engineering (no game running)

| Question | Tool | Notes |
|---|---|---|
| Bytes, disassembly, a float constant, who reads an address, who calls an RVA, every user of a struct displacement | `py tools\disasm-rva.py <exe> dis\|bytes\|float\|search\|xref\|calls\|disp` | capstone; RVAs in and out |
| Is this function called at all? (zero callers on something the engine must call every frame = a dead hook target) | `.\tools\pe-xref.ps1 -Exe <exe> -TargetRva <rva>` | caller census over the whole image |
| A decompile, whole-image xrefs, which class owns a virtual, who writes a field | `.\tools\ida-run.ps1 -Game <g> tools\ida\<series><n>_<what>.py` | headless IDA, one question per script; `docs/IDA_WORKFLOW.md` |
| **Infinite:** a native function NAME to its exec thunk; a vtable's slots | `py tools\ue3-natives.py <BioShockInfinite.exe> natives --grep <name>` / `vtable <rva>` | verifies a published pair first and refuses otherwise. Not yet run against Infinite (written on a machine without it) |
| A crash dump: faulting module, offset, a stack | `py tools\read-dump.py <dmp>` | then hand the RVA to IDA |
| The symbols for a dump from a past build | `.\tools\archive-symbols.ps1` at release time | keyed by the DLL's SHA-256 |

**Order of work:** search what is already known (the game's ENGINE_NOTES, `patterns.h`, the
UnrealScript corpus, earlier IDA outputs) -> the cheap offline tools -> IDA -> a runtime
probe only for what a decompile cannot say -> a headset run last. Comparison sources (another
game, BRVR, a public engine tree) form hypotheses; only the game's own binary confirms them.

### Game content, offline - `docs/MODEL_WORKFLOW.md`

| Question | Tool |
|---|---|
| Class hierarchy, property NAMES, defaults, state names | the UnrealScript corpus: `.\tools\uscript-export.ps1 -Game bs1` -> `tools\uscript\bs1\` (1,765 BS1 classes) |
| What a package holds; which package holds an object | `.\tools\model-export.ps1 -Game <g> -List <pkg>` / `-Find <regex>` |
| A static mesh or a texture | `.\tools\model-export.ps1 -Game <g> -Package <pkg> -Object <name>` |
| What is in a mesh (counts, skeleton, skin weights, bbox, a render) | `.\tools\blender-run.ps1 tools\blender\inspect_model.py -- --mesh <file>` |
| Any offline geometry or skinning test | `.\tools\blender-run.ps1 <script.py> [-Blend <f>] -- <args>` |
| A HUD/menu movie's layout, ActionScript and frames | `.\tools\flash-export.ps1 -Game <g> -Movie <name>` |
| **A BS1 skeletal mesh with its Havok skeleton and clips, in Blender** (the arms, a Big Daddy, any rigged mesh) | `.\tools\bsmesh-export.ps1 -Game bs1 -Package <map> -List` / `-Mesh <name> [-Anims <regex>] [-Check]` -> `exports\bs1\<Mesh>.glb` |
| Did a glTF import intact (armature, skin, actions, posed renders)? | `.\tools\blender-run.ps1 tools\blender\gltf_check.py -- --gltf <f> [--action <regex>] [--render p]` |
| The Havok class layouts of a game build (what each packfile byte means) | `py tools\bsmesh\hkclass_dump.py <exe> <class...>` (`--all --json` is what `bsmesh-export` caches) |
| A Vengeance package's names, imports, exports (maps AND `BakedScripts\*.U`) | `tools\bsmesh\vpackage.py` (`Package(path)`) |

UModel cannot read BS1 skeletal meshes or animations (Havok); `bsmesh-export` can (measured
2026-10-08, `docs/bioshock1/HAVOK_AND_PACKAGES.md`). Textures, static meshes, UI movies and
script come out through the other rows.

### Third-party references - `docs/MODDING_SDKS.md`

| Question | Where |
|---|---|
| How the 2007 game's packages, editor, UnrealScript compiler (`ucc make`) and `#exec` imports work; whether new classes can reach Remastered | the Unofficial BioShock SDK guide, `external\Unofficial-BioShock-Editor\UnrealEdGuide\` |
| A full C++ SDK of Infinite's classes (blocked on GObjObjects) | CodeRed Generator, `external\CodeRed-Generator\` |

`.\tools\external-refs.ps1` clones both at the documented commits into the gitignored
`external\`. Neither is ever committed, and nothing in them is run.

### Pure logic, on the host

| Question | Tool |
|---|---|
| Does this math / state machine / detector do what it claims, over a sweep, with a negative control? | `.\tools\host-test.ps1 [suite]` - one `tools\tests\<name>-tests.cpp` per production header |

Suites so far: `ue-math` (`game/shared/ue_math.h`), `xrsim-math` (the simulator's view
matrix). The habit it asks for: keep a decision in a header with no engine reads, and test
that header here before a headset sees it.

### Live instrumentation and the simulator - `docs/VERIFICATION.md`

| Question | Tool |
|---|---|
| Anything that is not perceptual, with no headset | the OpenXR simulator: `tools\xrsim-*.ps1` (launch, cmd, shot, run, state, selftest). **Launching it is still a launch: ask first** |
| A scripted repro with asserts on the sim AND the mod's log | `.\tools\xrsim-run.ps1 -Path tools\xrsim\<x>.xrs` (`@assert`, `@log`, `@nolog`, `@mark`, `@key`, `@fps`) |
| Is the simulator itself healthy? | `.\tools\xrsim-selftest.ps1` (no game; opts out of 64-bit implicit layers for its own process) |
| Drive the mod's command seam | `.\tools\game-cmd.ps1 -Game <g> "<cmd>"` |
| Follow the log | `.\tools\tail-log.ps1 -Game <g>` |

### Running-process diagnostics (the game is running; none of these launch it)

| Question | Tool |
|---|---|
| Which thread is the frame bound on? | `.\tools\thread-cpu-profile.ps1 -Game <g> -Seconds 20` |
| Where is that thread spending its time? | `.\tools\thread-ip-profile.ps1 -Game <g> -ThreadIds <ids>` (64-bit PowerShell) |
| A crash the mod's handler cannot record, a hang, a memory climb toward 4 GB | `.\tools\watch-crashes.ps1 -Game <g> [-Mode Memory]` (ProcDump, Microsoft-signed) |
| Is a periodic hitch on a streamed headset the Wi-Fi or the PC? | `.\tools\net-ping-watch.ps1 -FromStreamer -Minutes 5`, lined up against the log's `[HH:MM:SS.mmm]` |
| A 32-bit OpenXR client (armcap, xr_hello32) fails at instance creation | `tools\lib\xr-layers.ps1` - the same unloadable-layer opt-out the mod does for itself |

### Debug programs (tool file names)

| Tool | Use it for |
|---|---|
| `renderdoc` | **All three games are D3D11**, so RenderDoc captures the GAME's frame as well as the mod's - every draw, state and shader. Captures are game-derived (`*.rdc` is gitignored) |
| `cheatengine` | Live memory scan and structure dissection when a value has no name yet. A Cheat Engine address is a lead, not a finding: derive it properly before it enters `patterns.h` |
| `hxd` | Byte-level look at a binary, an ini's line endings, a dump |
| `x32dbg`, `ghidra` | Optional: a 32-bit debugger; a free decompiler where IDA is not installed |

## Static checks

`.\tools\lint.ps1` - every tracked `.ps1`/`.bat` is ASCII with no BOM and parses, no IDA
script has a BOM, and no game-derived content is tracked. Run before a commit.
