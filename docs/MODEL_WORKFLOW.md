# Game content offline: models, UI movies and UnrealScript

Three offline routes into what the games ship, each driven headless by a committed
script and each writing only into a LOCAL workspace:

| Route | Tool | Script | Answers |
|---|---|---|---|
| Meshes and textures | UModel (UE Viewer) -> headless Blender | `tools\model-export.ps1`, `tools\blender-run.ps1`, `tools\blender\*.py` | what a package holds, a mesh's geometry, skeleton and skin weights |
| **BS1 skeletal meshes + Havok skeletons + clips** | our own reader (`tools\bsmesh\`) -> glTF -> Blender | `tools\bsmesh-export.ps1`, `tools\blender\gltf_check.py` | the arms, a Big Daddy, any rigged mesh, posed by any of its clips - what UModel cannot open |
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
| **Skeletal meshes** | **UModel: NO** - `NEWPlayerHands`, `BeaconBall_Mesh`, `CorpseMale` all stop in `USkeletalMesh::PostLoadBioshockMesh` with `Unknown Havok class: AnimationPackageRoot`. **`bsmesh-export.ps1`: YES** (2026-10-08) - `0-Lighthouse` 46/46, `1-Welcome` 69/69, `2-Fisheries` 51/53 (2 empty stubs) | not measured; may read as-is (same engine). UModel's forum reports skeletal-mesh crashes on BS2 with the 64-bit build | listed as supported by UModel |
| Animations | **`bsmesh-export.ps1`: YES** - Havok spline clips, e.g. 130 on `NEWPlayerHands`, 106 on `ProtectorRosie` | not measured | listed as NOT supported by UModel |
| UI movies (FFDec) | **yes** - 48 loose `.swf` in `ContentBaked\pc\FlashMovies`; `HUDPC` gives 504 ActionScript files | not measured | not measured |
| UnrealScript (UELib) | **yes** - `Build\Final\BakedScripts\pc\*.U`, all 12 packages, **1,765 classes, 0 failed** | not measured | needs the packages decompressed first |

**The consequence for the arm work.** BS1's arm mesh, its 47-bone Havok skeleton, its skin
weights and all 130 of its clips now come out of the packages offline, so Dishonored's IK
validation (the production solver's matrices baked through the original weights in Blender)
has its input. The formats and how each was derived (the package, the decompiled mesh
serializer, the Havok packfile, the class layouts read out of the exe, the spline decoder)
are `docs/bioshock1/HAVOK_AND_PACKAGES.md`.

What the export is NOT: the game's live pose. The runtime blends clips, applies the
mod's own bone writes and IK on top, and renders through the viewmodel FOV path. Comparing
against the running game still means reading the bones live (`SkeletonInstance` +0x48, Havok
48-byte transforms, component space - BS1 ENGINE_NOTES) - but now against a known bind pose
and a known clip, in the same coordinates.

The other direction - an edited `.glb` back into a Havok animation package - is what the
Unofficial BioShock SDK's `HAVOK BUILD` does, for the 2007 game's package format only. See
`docs/MODDING_SDKS.md`.

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
# BS1 skeletal mesh + Havok skeleton + clips -> exports\<game>\<Mesh>.glb (offline, no UModel)
.\tools\bsmesh-export.ps1 -Game bs1 -Package 0-Lighthouse -List
.\tools\bsmesh-export.ps1 -Game bs1 -Package 0-Lighthouse -Mesh NEWPlayerHands [-Anims "Pistol|Wrench"|none] [-Lod 0]
.\tools\bsmesh-export.ps1 -Game bs1 -Package 0-Lighthouse -Mesh NEWPlayerHands -Check -CheckAction "EmptyFidgetPistol"
# any glTF: armature, skinned mesh, actions, and renders of the bind pose and chosen frames
.\tools\blender-run.ps1 tools\blender\gltf_check.py -- --gltf <x.glb> [--action <regex>] [--frames 0,15] [--render prefix]
# a UI movie: XML structure, ActionScript, frames
.\tools\flash-export.ps1 -Game bs1 -ListMovies
.\tools\flash-export.ps1 -Game bs1 -Movie HUDPC [-What xml|script|frame]
# ...with the real art: BS1 stores it as DXT in a gameswf custom tag FFDec draws solid red.
# Decode both movies into one folder in the model workspace, then point FFDec at the copies.
py tools\flash-decode.py <FlashMovies>\sharedlibrary.swf <ws>\flash\bs1\decoded\sharedlibrary.swf
py tools\flash-decode.py <FlashMovies>\HUDRadial.swf <ws>\flash\bs1\decoded\HUDRadial.swf
# ...and whole: the imported art merged in (FFDec does not follow imports when rendering)
ffdec-cli -swf2xml <ws>\...\sharedlibrary.swf shared.xml ; ffdec-cli -swf2xml <ws>\...\HUDRadial.swf radial.xml
py tools\flash-merge.py shared.xml radial.xml merged.xml ; ffdec-cli -xml2swf merged.xml HUDRadial_merged.swf
# the UnrealScript corpus into tools\uscript\<game>\
.\tools\uscript-export.ps1 -Game bs1 [-Only ShockGame] [-Inventory]
# the hands and a first-person gun TOGETHER, as the game attaches them, posed by their clips: renders,
# the part in the palm, the gun hand's motion, and a check of the hands against the live game
.\tools\blender-run.ps1 tools\blender\reload_pose.py -- --hands <ws>\exports\bs1\NEWPlayerHands-all.glb
    --gun <ws>\exports\bs1\WP_PistolMesh.glb --hands-action Pistol__FastReloadPistol --gun-action FastReload
    --socket R_grip --times 0.53,0.80 [--part DrumParent] [--palm Bip01_L_Hand]
    [--attach <ws>\bs1\shockgame-weapons\ShockGame\StaticMesh\PI_AmmoType.pskx] [--no-render]
```

**The game's own frames, captured and replayed** (2026-10-20). The mod writes what it DRAWS: the
F10 AFW capture ("Capture 6 presents in 5 s") also writes `pose_00..02.txt` into its folder, and
while the shotgun is in hand every frame (30 a second) is appended to
`dumps\poserec-<date>\Shotgun.bvrpose` - the hands' engine pose, each hand's palette correction,
the hands actor, the gun actor and every gun bone as written, the clips and their times.
`tools\blender\pose_capture.py` rebuilds them on the glbs (game_frames.py: the game's bone frames
and Unreal axes): `--pose <pose_00.txt> --render cap` for one frame, `--record <Shotgun.bvrpose>
--save run.blend --csv run.csv [--step 3]` for a whole run as an animation plus a per-frame table
(state, clips and times, the gun hand's grip frame, the free palm on the gun, every gun bone in the
grip's frame). A capture of the reference pose lands every bone on its rest (`hands_off_rest`
0.00). The IK arms and the mirror are not rebuilt.

**`reload_pose.py` reproduces the game's own placement** (2026-10-20). The gun's origin sits on
the hands' `R_grip` (bone 43) in the GAME's frame of that bone, the way the engine attaches it,
and every position it reports is in Unreal axes. Each run prints `BVR_POSE check` lines: the off
hand's palm in the grip's frame at the times `vrbones reloadrec` recorded in the running game
(the paths in `reload_rig.h`), and the error. The Tommy Gun and pistol clips agree to 0.1-0.9
units except one or two samples inside the fastest motion (frame timing); a run whose check
is off by more is not showing the game. Ammo meshes (`--attach`) hang on the gun's
`SOCKET_AmmoDummySocket` the same way.

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
- **Blender's glTF importer REORIENTS every bone** (it points each bone's Y along the bone), so
  a pose bone's matrix is the game's bone frame times a fixed per-bone turn. Anything attached
  to a pose bone, or measured in its frame, is turned by that amount: every reload render before
  2026-10-20 evening had the gun at a wrong angle (the shotgun visibly misplaced, its root far
  from its origin), and the pistol's 0.53 s carry pose was chosen from one. Undo the turn
  (`reload_pose.py game_frame`: turn = node rest^-1 x bone rest, from the glb's own nodes) and
  check against recorded game data before trusting a render. Bone HEAD positions were never
  affected.
- **In a `bsmesh-export` glb, Unreal's left (-Y) is Blender's +Y.** The conversion is a
  reflection, not a rotation (HAVOK_AND_PACKAGES.md section 4). Check a left/right claim
  numerically against a named bone, never by eye.
- **Blender 5.x's glTF importer adds an Icosphere** as the bones' display shape. It is not in
  the file; `gltf_check.py` excludes it.
- **The Havok class layouts are cached per game** in
  `%LOCALAPPDATA%\BioshockVR\havok\hkclasses-<game>.json`. After a game update, pass
  `-RefreshClasses`.
- **Welded points are not draw vertices.** The engine splits vertices at UV/normal seams.
- **A PSKX is a static mesh, but the add-on still makes a one-bone armature for it.**
  `inspect_model.py` skips the skin-weight checks for `.pskx` for that reason.
- **FFDec draws BS1's UI art as solid red, and a HUDPC render is mostly empty.** The art is
  DXT1/DXT5 in custom tag 512 (`id, w, h, format, 0`, then the blocks; 0 = DXT1, 4 = DXT5),
  and HUDPC imports 93 symbols from `..\FlashMovies\sharedlibrary.swf`, which FFDec does not
  resolve. `tools\flash-decode.py` fixes the first. FFDec 26.2.1 does not follow imports when
  it renders even with a local path, so `tools\flash-merge.py` copies the imported symbols
  into the movie through FFDec's XML (`-swf2xml`, merge, `-xml2swf`); in the merged movie a
  root clip's id is its original id + 4000. A movie's static frame shows authored rest poses
  and placeholders; the game fills and moves it from ActionScript at run time.
- **BS1 in VR plays `HUDRadial.swf`, not `HUDPC.swf`** (`Gui.ini [HUD]` has both; the pad
  build uses `Filename=`). Same clips and functions, slightly different placements.
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
