# BS1 Remastered packages, Havok skeletons and animation clips - the formats

What `tools\bsmesh\` reads, and how each layout was derived. Measured 2026-10-08 (session 80)
on BioShock Remastered, `BioshockHD.exe` as installed by Steam (appid 409710), offline: the
game was never launched for any of this.

This is the reference behind `tools\bsmesh-export.ps1` (a skeletal mesh + its skeleton + its
clips -> one `.glb` Blender imports; `docs/MODEL_WORKFLOW.md`). UModel 1590 cannot do this
for BS1 Remastered: it stops in `USkeletalMesh::PostLoadBioshockMesh` with
`Unknown Havok class: AnimationPackageRoot`.

**Everything here is a summary of the format. No game bytes, no extracted asset and no class
table output is committed** - the tools re-derive what they need from the local install.

## 1. The package (`ContentBaked\pc\Maps\*.bsm`, `Build\Final\BakedScripts\pc\*.U`)

Vengeance (UE2.5) package, **version 142, licensee 56**, uncompressed. Both the maps and the
12 script packages read with the same reader (`tools\bsmesh\vpackage.py`); `ShockGame.U`
measures 8,875 names, 623 imports, 11,647 exports, 654 classes.

| Table | Entry |
|---|---|
| summary | u32 magic `0x9E2A83C1`, u16 version, u16 licensee, u32 flags, then count/offset pairs for names, exports, imports |
| name | cidx char count (with the NUL), **UTF-16LE** chars, **u64** flags |
| FName | cidx name index, u32 number (0 = plain, n = `<name>_<n-1>`) |
| import | FName classPackage, FName className, i32 package, FName objectName |
| export | cidx class, cidx super, i32 package, i32 (0), FName objectName, u64 flags, cidx serialSize, cidx serialOffset (only when size > 0), u32 (0 or 1) |

- The tables sit at the **end** of the file, in the order names, imports, exports, and each ends
  exactly where the next begins. The reader checks that: a layout change fails loudly.
- Object references are UE2 package indices: 0 none, > 0 export `i-1`, < 0 import `-i-1`.
- **Traps that cost time**: names are not ANSI and their flags are 8 bytes, and imports/exports
  are variable-length rows, not fixed 24-byte records. Both were wrong on the first pass.
- A map carries a localised copy per language (`<map>_deu.bsm`, ...) with voice and text only.

## 2. The skeletal mesh (`USkeletalMesh` export)

Decompiled, not guessed: `tools\ida\sk1_skelmesh_vtable.py` (the vtable, by RTTI comparison
against `UPrimitive` - `ULodMesh` and `UMesh` carry no RTTI), `sk2` (the serializer, **vtable
slot 11 at `+0x3EE720`**), `sk3`/`sk4` (every direct callee of it and of the **LOD-model
serializer at `+0x3FAE70`**). The layout in load order is the docstring of
`tools\bsmesh\skelmesh.py`. The parts that matter:

- **The engine's own bone array is empty on BS1.** The skeleton is the Havok one (section 3).
- A LOD model holds sections (9 x u16 each), a **bone map** (u16 per entry), a raw u16 index
  buffer, then two vertex arrays:
  - **skinned, 64 bytes**: position f32x3, a 9-float tangent basis, uv f32x2, then four
    `(u8 boneMapIndex, u8 weight)` pairs whose weights sum to **255**;
  - **rigid, 57 bytes**: position, basis, uv, one u8 boneMapIndex (the first read said 60:
    wrong).
- A vertex's bone byte indexes the **bone map**, not the skeleton.
- **The vertex normal is `basis[2]`.**
- **The index buffer numbers the RIGID vertices first**, although they are stored second.
  Measured on `NEWPlayerHands` LOD 0: median triangle edge 0.75 units in that order against 43
  in storage order, and 8,726 / 8,726 faces then agree with their vertex normals.
- After the vertices: 6 x u32, a u32 when lodVersion > 1, a group array when >= 3, then 5 lazy
  arrays, each starting with its **absolute** end offset (the reader seeks to it).
- Some meshes are empty stubs: `2-Fisheries`' `Sliding512SingleDoorMesh` is 813 bytes with 4
  LODs and no geometry in any. The converter refuses it by name.

## 3. Havok: the `AnimationPackageWrapper` export

Each skeletal mesh's group holds an `AnimationPackageWrapper` export; at **offset `0x22`** of
its data sits a **Havok 2012.2.0-r1 binary packfile** (file version 9, layout `04 01 00 01`:
4-byte pointers, little endian, reuse padding, empty-base optimisation). Format in the
docstring of `tools\bsmesh\hkpack.py`; the parts that matter:

- 48-byte section headers. Sections: `__classnames__`, `__types__`, one per animation group
  (`chemical`, `crossbow`, `default`, `grenade`, `pistol`, `scripted`, `shotgun`, `tommygun`,
  `wrench` on the hands), and `__data__`.
- Every pointer is resolved through local, global and virtual fixup tables, so a pointer is a
  (section, offset) pair, never an address. A virtual fixup marks where an object of a named
  class starts.

### The class layouts come out of the exe

Havok links an `hkClass` description of every serialisable class into the binary, and its
loader uses them, so **the exe is the authority on what each packfile byte means**. They are
not static data on this build: each is built at start-up by an initializer that pushes 13
constructor arguments (name, parent, objectSize, interfaces*, numInterfaces, enums*,
numEnums, members*, numMembers, defaults*, attributes*, flags, describedVersion) and calls
**`hkClass::hkClass` at RVA `+0x87E090`** (`0x1117E090` at the image base IDA uses).

`tools\bsmesh\hkclass_dump.py` finds the constructor from the push of the string
`"hkaSkeleton"`, walks every call to it (**26,484 initializers, 498 live classes** - Havok
keeps one description per historical version, and only the live one has a non-zero
objectSize), and reads the static 24-byte `hkClassMember` tables. The first version passed
12 arguments and decoded nothing: `interfaces*` was missing.

The ones the converter reads:

| Class | Size | What it holds |
|---|---|---|
| `AnimationPackageRoot` (2K's) | 24 | high-bone skeleton, master ragdoll instance, high->low and low->high skeleton mappers, `m_animations` (a SIMPLEARRAY) |
| `AnimationPackageAnimation` (2K's) | 12 | `m_name`, `m_groupName`, `m_binding` |
| `hkaSkeleton` (version 5) | 96 | name, parent indices, bones, **reference pose** (hkQsTransform), float slots, local frames, partitions |
| `hkaAnimationBinding` (version 3) | 56 | the animation, `transformTrackToBoneIndices`, blend hint |
| `hkaSplineCompressedAnimation` | 132 | duration, track counts, frames/blocks, mask-and-quantisation size, block offsets, `data` |

The low-bone skeleton and the two mappers are the engine's LOW/HIGH skeleton sets that
ENGINE_NOTES describes from the runtime side (`SkeletonInstance`, `+0x3FC` on an actor).

### Spline compression

Per track, 4 mask bytes: quantisation, then position, rotation and scale flags (each
component static, spline or identity). **BS1 uses 16-bit positions, THREECOMP40 rotations and
16-bit scales, and one block per clip.** A spline sub-track carries a degree, knots and
quantised control points; evaluation is Cox-de Boor. The full byte layout is the docstring of
`tools\bsmesh\hkspline.py`.

## 4. Into Blender

`tools\bsmesh\bs2gltf.py` writes the mesh (POSITION, NORMAL, TEXCOORD_0, JOINTS_0 through the
bone map, WEIGHTS_0), one node per Havok bone with inverse bind matrices from the reference
pose, an empty per socket (`SOCKET_<name>`), and one LINEAR animation per clip.

- **Coordinates**: Unreal is left-handed, Z up. The conversion is the reflection
  `(x, y, z) -> (x, z, y)`, rotations `(qx, qy, qz, qw) -> (-qx, -qz, -qy, qw)`. In Blender the
  model stands Z up, X forward, **and Unreal's left (-Y) is Blender's +Y**. The reflection
  flips winding; the writer checks every face against its normals.
- **Units**: 1 glTF unit = 1 Unreal unit (the Unofficial BioShock SDK's importer uses the same).

### Validation (what was checked, and how)

- `NEWPlayerHands`: **47 bones**, matching the rig ENGINE_NOTES records (`R_grip` 43,
  `IKbindLhandDummy` 44); **130 clips**. Rendered in Blender at the bind pose, a pistol
  fidget (`Pistol__EmptyFidgetPistol`, right-hand grip) and the Electro Bolt fidget
  (`Default__ElectrokineticBolt_Fidget`, left hand raised). A clip is named
  `<group>__<clip>` in Blender.
- Left/right numerically, not by eye: bind `L_Hand` at Unreal y = -25.9; Electro frame 30
  `L_Hand` z = -21 against `R_Hand` z = -100.
- `ProtectorRosie` (Big Daddy, 60 bones, 106 clips) and `CorpseMale` (73 bones) render posed.
- Batch: `0-Lighthouse` 46/46 skeletal meshes, `1-Welcome` 69/69, `2-Fisheries` 51/53 (the two
  failures are the empty stub above).
- Large per-frame rotation jumps appear only on left-hand tracks in the crossbow and equip
  clips; they look like real motion, but nobody has checked them against the running game.

## 5. Not measured

- **BS2 Remastered** shares the engine and may read as-is; nobody has tried. `hkclass_dump.py`
  derives the layouts per exe, so a different Havok build would show up there first.
- **Infinite** is UE3 and needs its own mesh reader. Its skeleton lives in the engine's own
  `RefSkeleton` (Infinite ENGINE_NOTES), not in Havok.
- Ragdoll instances, skeleton mappers, float tracks and annotation tracks are parsed as
  objects but not exported.
