# Game content offline: models, UI movies and UnrealScript

Three offline routes into what the games ship, each driven headless by a committed
script and each writing only into a LOCAL workspace:

| Route | Tool | Script | Answers |
|---|---|---|---|
| Meshes and textures | UModel (UE Viewer) -> headless Blender | `tools\model-export.ps1`, `tools\blender-run.ps1`, `tools\blender\*.py` | what a package holds, a mesh's geometry, skeleton and skin weights |
| Scaleform UI | JPEXS FFDec | `tools\flash-export.ps1` | a HUD/menu movie's layout, its ActionScript, its frames |
| UnrealScript | UE Explorer's UELib + our `ExportScripts.exe` | `tools\uscript-export.ps1` | class hierarchies, property names, defaults, state names |

Ported from the Dishonored VR mod (its `docs/MODEL_WORKFLOW.md`), where the UModel +
Blender route validated a production arm-IK solver against the original skin weights over
260 frames. What carries over to the BioShock games is not the same, and section 2 is the
measured answer.

**Everything extracted is game-derived and never committed**: PSK/PSKX/PSA, TGA, `.blend`
files, renders, decompiled ActionScript and UnrealScript, reports. They live in the model
workspace (`model_workspace` in the tool file, default `Documents\BioshockVR-Models`) or in
the gitignored `tools\uscript\`. The scripts that produce them are ours and are committed.
Third-party programs are referenced by path through the tool file (`docs/TOOLS.md`), never
copied into the tree.

## 1. Setup

```powershell
.\tools\tool-paths.ps1 -Init                # blender, umodel, psk_addon_zip, ffdec, ueexplorer, content_<game>
.\tools\blender-run.ps1 -Setup              # PSK/PSA add-on installed and registered (installs from psk_addon_zip)
.\tools\uscript-export\build.ps1            # builds ExportScripts.exe into the UE Explorer folder
```

- **Blender** 4.2+ (5.2 verified). **UModel** build 1590 (2022, the last public release),
  <https://www.gildor.org/en/projects/umodel>; the 32-bit build. **io_scene_psk_psa** 9.1.x,
  <https://extensions.blender.org/add-ons/io-scene-psk-psa/>. **FFDec** 26.x,
  <https://github.com/jindrapetrik/jpexs-decompiler>. **UE Explorer** 1.6.2,
  <https://github.com/UE-Explorer/UE-Explorer>.
- `-Setup` checks the add-on's operators with `get_rna_type()`. **`hasattr(bpy.ops.psk,
  "import_file")` is True for any name** and is not a check: Dishonored's first setup
  reported the add-on installed from exactly that probe while it was missing.
- Never pass `--factory-startup` to Blender: it disables the add-on.
- Ask before downloading any of these.

## 2. What works per game (measured 2026-10-07, BS1 only - BS2 and Infinite are not installed on the machine this was written on)

| | BS1 Remastered | BS2 Remastered | Infinite |
|---|---|---|---|
| UModel reads the packages (`-game=bio` / `bio3`) | **yes** - `ContentBaked\pc\Maps\*.bsm`, package ver 142 (`0-Lighthouse`: 22,780 exports) | not measured | not measured |
| Textures | **yes** (TGA) | not measured | listed as supported by UModel |
| Static meshes | **yes** (PSKX), inspected in Blender with no problems | not measured | listed as supported |
| **Skeletal meshes** | **NO.** All three tried - `NEWPlayerHands` (the first-person arms, in all 20 maps), `BeaconBall_Mesh`, `CorpseMale` - stop in `USkeletalMesh::PostLoadBioshockMesh` with `Unknown Havok class: AnimationPackageRoot` | not measured; UModel's forum reports skeletal-mesh crashes on BS2 with the 64-bit build | listed as supported |
| Animations | **NO** (the same Havok packages) | not measured | listed as NOT supported |
| UI movies (FFDec) | **yes** - 48 loose `.swf` in `ContentBaked\pc\FlashMovies`; `HUDPC` gives 504 ActionScript files | not measured | not measured |
| UnrealScript (UELib) | **yes** - `Build\Final\BakedScripts\pc\*.U`, all 12 packages, **1,765 classes, 0 failed** | not measured | needs the packages decompressed first |

**The consequence for the arm work.** BS1's arm mesh and its skin weights cannot come out of
the packages, so Dishonored's offline IK validation (the production solver's matrices baked
through the original weights in Blender) has no input here yet. The routes that remain:

1. **Dump the rig from the running game.** The mod already reads the arm's bones
   (`SkeletonInstance` +0x48, Havok 48-byte transforms, component space - BS1 ENGINE_NOTES)
   and the viewmodel's draws are D3D11. One capture of the bone array plus the vertex and
   index buffers of the arm draw is enough to write a glTF Blender opens. This is new mod
   code, not a tool, and it is not built.
2. **Validate against a stand-in.** The solver's geometry (reach, pole, twist distribution)
   can be swept against an authored two-bone arm in Blender while the real rig is out of
   reach; it proves the maths, not the skinning.

## 3. Commands

```powershell
# what a package holds, and which package holds an object (cached index per package)
.\tools\model-export.ps1 -Game bs1 -List 0-Lighthouse -Grep " SkeletalMesh | StaticMesh "
.\tools\model-export.ps1 -Game bs1 -Find "PlayerHands"
# a mesh and its textures into originals\<game>\<Package>\<Class>\
.\tools\model-export.ps1 -Game bs1 -Package 0-Lighthouse -Object light_wall
# what is in a mesh: counts, materials, skeleton, skin-weight health, bbox, optional render
.\tools\blender-run.ps1 tools\blender\inspect_model.py -- --mesh <x.pskx|x.psk|x.glb> [--bones] [--out r.json] [--render p.png]
# one object out of a .blend as PSK / PSA / OBJ / glTF / FBX
.\tools\blender-run.ps1 tools\blender\export_model.py -Blend <w.blend> -- --object <Name> --out <f.glb>
# a UI movie: XML structure, ActionScript, frames
.\tools\flash-export.ps1 -Game bs1 -ListMovies
.\tools\flash-export.ps1 -Game bs1 -Movie HUDPC [-What xml|script|frame]
# the UnrealScript corpus into tools\uscript\<game>\
.\tools\uscript-export.ps1 -Game bs1 [-Only ShockGame] [-Inventory]
```

`model-export.ps1` exits 1 and names UModel's error when an export fails, and says when the
failure is the Havok wall above. `inspect_model.py` prints every problem as a
`BVR_INSPECT problem:` line. Your own one-off Blender script runs the same way:
`.\tools\blender-run.ps1 <script.py> [-Blend <file>] -- <args>`; it sees `BVR_MODEL_WS` in
its environment and exits non-zero on an uncaught exception.

## 4. The workspace

| folder | holds |
|---|---|
| `originals\<game>\<Package>\<Class>\` | untouched UModel output. Never edited |
| `working\` | editable `.blend` projects |
| `exports\` | anything written back out (relative `--out` of `export_model.py`) |
| `verification\` | reports, test scenes, renders (relative `--out` of `inspect_model.py`) |
| `index\<game>\` | cached per-package inventories for `model-export.ps1 -Find` |
| `flash\<game>\<Movie>\` | FFDec output and its log |

## 5. Traps

- **UModel's PSK reflects Y** relative to the engine's mesh space. Anything compared with
  runtime data must undo it (Dishonored's first IK candidate shipped the unconverted frame
  and the runtime refused it).
- **Imported bone tails are display length (1 unit), not limb length.** Use head-to-head
  distances.
- **Welded points are not draw vertices.** The engine splits vertices at UV/normal seams.
- **A PSKX is a static mesh, but the add-on still makes a one-bone armature for it.**
  `inspect_model.py` skips the skin-weight checks for `.pskx` for that reason.
- **UModel prints its errors to stdout and its exit code says nothing**; the script reads
  the text (`*** ERROR`).
- **A BS1 map has a localised copy per language** (`<map>_deu.bsm`, ...) holding voice and
  text; `-Find` skips them.
- **UELib prints a stack trace for every function body it cannot decompile** - tens of MB
  for BS1 - while the class still exports. `uscript-export.ps1` keeps it in
  `tools\uscript\_logs\<package>.stderr.txt`; the RESULT line is the verdict.
- A Blender render is not proof of the live shader path, stereo or the game's animation
  routing. It checks geometry and skinning math only.
- PowerShell 5.1 returns a null `ExitCode` from `Start-Process -PassThru` unless the process
  handle was read before it exited; the scripts here read `.Handle` first.
