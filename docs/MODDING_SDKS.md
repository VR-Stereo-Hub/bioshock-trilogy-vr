# Third-party modding SDKs: the Unofficial BioShock SDK and CodeRed

Two public projects this repo reads as references and never commits. Run
`.\tools\external-refs.ps1` to clone both, at the commits this page was written against, into
the gitignored `external\`. Written 2026-10-08 (session 80), offline. **Nothing here was
tested in a running game.** The open questions at the end are what a test has to settle.

| | Unofficial BioShock SDK | CodeRed Generator |
|---|---|---|
| Upstream | <https://github.com/bio4554/Unofficial-BioShock-Editor> (branch `development`, `de8b3c7`) | <https://github.com/CodeRedModding/CodeRed-Generator> (`ce9c438`, v1.2.1) |
| What it is | A modified UnrealEd 2 for **BioShock 2007**: the editor, `UCC.exe` (the UnrealScript compiler and commandlets), the SDK Manager, glTF import, Havok build. Shipped as `BioShockSDK-0.1.10-Setup.exe` (Inno Setup, 11.6 MB) plus a 38-chapter guide in `UnrealEdGuide\` | A UE3 SDK generator: a DLL injected into a running UE3 game walks GObjects/GNames and writes C++ headers for every class, struct, enum and function |
| Licence | **None stated.** Read it; copy nothing from it | **MIT** - may be adapted with an attribution comment |
| Applies to | BS1 (indirectly - see section 1) | **Infinite only** (UE3). The remasters are Vengeance/UE2.5 |
| Run here? | **No.** The installer was not run: 7-Zip cannot open Inno Setup 7, and it is unlicensed third-party code. Everything below is from the guide | Not built yet: it needs Infinite installed and one launch |

## 1. The Unofficial BioShock SDK

### What it does, from the guide

- **`ucc make` compiles UnrealScript packages.** Your own classes go in
  `<SDK>\<Package>\Classes\*.uc`, one class per file, and compile to `System\<Package>.u`
  (chapter 33). `make` only compiles a package whose `.u` is missing, so after an edit you
  delete the `.u` first. A package must be listed after every package it extends.
- **It can recompile the game's own packages.** The SDK Manager exports `ShockGame`,
  `ShockAI`, `VengeanceShared` and the rest from your own install, as source plus
  `defaultproperties`, and an unedited export compiles to the same code and defaults as the
  shipped package. The guide says the 2007 game loads a recompiled `ShockGame.U` and
  `ShockAI.U`, saved games included.
- **Limits it states**: you can extend any class whose package carries UnrealScript source,
  but not a designer class from a content library (`Content\*.pkg`). **No `native` classes
  or functions** - they need a DLL that you cannot build.
- **Assets come in at compile time through `#exec`** (chapter 34): `TEXTURE IMPORT`,
  `NEW StaticMesh` (`.ase`/`.gltf`/`.glb`), `SKELETALMESH IMPORT` (a rigged `.glb`, clips
  included), `NEWMATERIAL`, `OBJ LOAD`. **`HAVOK BUILD`** rebuilds a skeleton's Havok
  animation package from a `.glb`, which is the reverse of our `tools\bsmesh-export.ps1`. One
  glTF unit is one Unreal unit there too.
- Other commandlets (chapter 35): `batchexport`, `analyzecontent`,
  `editor.BioExportPackage` (a package to editable source and `Import\` files),
  `editor.BioDecookLibrary` (a baked map's shared content back into a content library),
  `editor.BioFxExtract`, `editor.ClassFlag`.
- Not supported (chapter 11): morph targets, more than one LOD, ragdolls.

### Which guide chapters matter for the VR work

| Chapter | Why |
|---|---|
| 09 Actors and Properties, 37 Glossary | the engine's vocabulary, as the game's designers used it |
| 11 Skeletal Meshes and Animation | how a skeleton, its groups and its clip metadata hang together - the other side of `docs/bioshock1/HAVOK_AND_PACKAGES.md` |
| 20-23 Scripting | the level scripting system (Script actors and their Action lists) that drives scripted scenes - useful when a scene breaks the VR camera or hands |
| 33-35 UCC | compiling classes (below) |
| 18-19 Bake, retail maps | what a baked map carries, and how one is replaced and restored |

### Can we compile NEW classes into BioShock Remastered?

The SDK supports the 2007 game only: setup refuses a Remastered-only install ("The Remastered
files have a different format"). What was measured offline on Remastered:

1. **Remastered ships the same twelve script packages** the SDK compiles, as
   `Build\Final\BakedScripts\pc\*.U` (Core, Engine, FMODAudio, IGEffectsSystem,
   IGModEffectsSubsystem, IGSoundEffectsSubsystem, IGVisualEffectsSubsystem, Scripting,
   ShockAI, ShockGame, Tyrion, VengeanceShared), package **version 142, licensee 56**,
   uncompressed. Our reader (`tools\bsmesh\vpackage.py`) parses `ShockGame.U`: 654 classes.
2. **The Remastered loader accepts old package versions.** `tools\ida\pk1_package_version_gate.py`
   decompiles the `ULinkerLoad` summary check (`+0x740620`): magic `0x9E2A83C1`, then
   **`FileVersion >= 60` passes** and anything older asks the `OldVersion` question. There is
   **no upper bound and no licensee check**, and later reads branch on the version
   (`>= 140`, and `>= 142` in the skeletal mesh serializer). So a 2007-format package is not
   turned away at the door. That is not proof that it loads cleanly.
3. **The shipping exe has no script compiler.** No `MakeCommandlet`, no compiler messages; it
   has `ContentBake.ContentBakeCommandlet`, `UServerCommandlet` and `UMasterMD5Commandlet`
   only. Classes have to be compiled by the 2007 SDK's `UCC.exe`.
4. **The mod DLL can be the loader.** `UObject::execDynamicLoadObject` is in the exe, and BS1
   already has `UObject::ProcessEvent` derived (`+0x375140`) and an FName lookup
   (`bioshock1r/hands.cpp`, the s71 script seam). What BS1 lacks is the *caller*: invoking a
   function by name is how BS2 and Infinite (`bsicall`) work, and it is not built for BS1.
   With it, `DynamicLoadObject("MyMod.MyClass", class'Class')` followed by a spawn needs no
   `.ini` edit.

**The route, therefore**: install the 2007 game and the SDK -> write `MyMod\Classes\*.uc`
extending the game's classes -> `ucc make` -> copy `MyMod.u` into Remastered's
`BakedScripts\pc\` -> the DLL loads it by name. **Not tested, and three things can break it**:

- **Parent drift.** Remastered changed some script classes. A subclass compiled against the
  2007 parent resolves its imports by name at load, but a function, property or native
  index the 2007 parent has and Remastered's lacks fails. Diff the parent against the
  Remastered corpus first (`tools\uscript\bs1\`, `tools\uscript-export.ps1`).
- **Native classes are out.** The SDK says so, and our classes cannot add C++. Behaviour
  that needs native code stays in the DLL.
- **Content in the package.** A class package that embeds a mesh or texture goes through the
  2007 serializers. The version gates should read it, but nobody has checked.

The cheapest real test is a single class with no content and one `defaultproperties` change
(for example an `Actor` subclass that logs from `PostBeginPlay`), loaded by name through
ProcessEvent once BS1 has a by-name caller. That is one launch for one question, and it needs
a go-ahead from whoever does the testing (no launch was made for this page).

## 2. CodeRed Generator, for Infinite

### What it needs, and what we already have

An engine folder (`Engine\<Game>\`: `Configuration.cpp`, `GameDefines.hpp/.cpp`,
`PiecesOfCode.*`) copied from `Engine\Template\` (or from `Engine\Dishonored\` - the
closest shape: 32-bit UE3, offsets mode, ProcessEvent by vtable index), the include in
`Engine\Engine.hpp` switched to it, a C++20 Win32 DLL build, and an injection into the running
game. Offsets in `Configuration.cpp` are **RVAs** - it adds `GetModuleHandle(NULL)`.

| CodeRed field | Infinite value | Source |
|---|---|---|
| `m_gnameOffset` | **`0xF9DFEC`** (TArray<FNameEntry*>: Data, Num +4, Max +8) | `patterns.h` `kGNamesDataRva`; live `GNames[0] == "None"`, 69,718 names (s36 selftest) |
| `m_gobjectOffset` | **NOT FOUND** | the blocker - below |
| `m_useIndex` / `m_peIndex` | `true` / **31** (vtable `+0x7C`) | 407 of 420 FindFunctionChecked callers agree; live vtable read matched |
| `m_gameAlignment` / `m_finalAlignment` | 4 / 4 | Win32 |
| `UTF16` define | **neither works as is** - see FNameEntry | |
| `m_outputPath` | a folder OUTSIDE the repo, e.g. `%LOCALAPPDATA%\BioshockVR\bsi\sdk\` | the output is game-derived |

`GameDefines.hpp` layouts, measured (Infinite ENGINE_NOTES, "s48b: the UProperty layout"):

| Member | Offset | Status |
|---|---|---|
| FNameEntry Index / HashNext / Name | +0x08 / +0x0C / +0x10 | **measured.** But +0x08 packs `(index << 1) \| isWide` and ASCII is the default; CodeRed assumes one encoding for every name. `GetName()` must test bit 0 and read `char` or `wchar_t` per entry |
| UObject VfTable | +0x00 | standard |
| UObject InternalIndex | +0x04 | **unmeasured** (UE3 standard). The GObjects check below confirms it |
| UObject +0x08 .. +0x0B | ? | **unmeasured** - HashNext is at +0x0C, so ObjectFlags is not the 8 bytes at +0x08 that Dishonored has |
| UObject HashNext / Outer / Name / Class / ObjectArchetype | +0x0C / +0x14 / +0x18 / +0x20 / +0x24 | measured |
| UField Next | +0x28 | measured |
| UStruct SuperField / Children | +0x34 / +0x38 | measured (SuperField is in UStruct, not UField) |
| UStruct PropertySize, Script, MinAlignment | ? | unmeasured; Dishonored's are Children +4 onward |
| UProperty ArrayDim / ElementSize / Offset | +0x2C / +0x30 / +0x48 | measured |
| UProperty PropertyFlags | ? (likely +0x34, 8 bytes) | unmeasured |
| UBoolProperty BitMask | +0x58 | measured. It also says UProperty ends at +0x58 (Offset + 0x10, the same tail as Dishonored), so UObjectProperty::PropertyClass, UStructProperty::Struct and UArrayProperty::Inner should sit at +0x58 too - **check each before trusting it** |
| UFunction FunctionFlags / iNative / NumParms / ParmsSize / ReturnValueOffset / Func | ? | unmeasured |

CodeRed calculates the gaps itself from `REGISTER_MEMBER`, so a wrong field shows up as
garbage names or sizes in the generated SDK, not as a crash - read its log before using any
of the output.

### The blocker: GObjObjects

No GObjects, no SDK. ENGINE_NOTES records it as **not found, deprioritised on purpose**: the
mod reaches live objects through hook parameters, and `UObject::StaticFindObject`
(`0xC6250`) goes through the object hash, not the array. Globals seen on that path and
**not identified**: `0xF8BF04`, `0xF79D30`, `0xF83EA0`.

The derivation, without guessing:

1. **Offline (IDA, `tools\ida-run.ps1 -Game bsi`)**: UE3's object teardown clears the slot -
   `GObjObjects(Index) = NULL` - which compiles to a load of the object's +0x04 and a store of
   0 into `[Data + index*4]` from a global. The `FObjectIterator` loops (`cmp index,
   [GObjObjects+4]`) are the second shape. Both name the same global; a candidate that only one
   shape names is not it.
2. **Live, one read (no state change)**: a `{Data, Num, Max}` at the candidate with
   `Num <= Max`, every non-null `Data[i]` passing the existing UObject checks (`Class->Class`
   fixpoint, +0x20), and **`Data[i]->InternalIndex == i`** for every sampled `i`. The last
   test confirms InternalIndex at +0x04 in the same read. This belongs in `bsireflect selftest`.
3. Record the RVA in `src/game/bioshockinf/patterns.h` and ENGINE_NOTES with both derivations,
   per the hard rules.

### Running it, once the blocker is gone

Generation needs Infinite running, and **never with BS2 running** (the hard rule). The steps,
for whoever has Infinite installed:

```powershell
.\tools\external-refs.ps1
# copy external\CodeRed-Generator\Engine\Dishonored\ to Engine\BioShockInfinite\, fill it from
# the tables above, point Engine\Engine.hpp at it, set m_outputPath outside the repo
# build CodeRedGenerator.sln as Release|Win32 (the solution targets toolset v145, VS 2026;
# retarget it if the machine has an older one)
# inject the DLL into BioShockInfinite.exe at the main menu; it writes the SDK and says so
```

The generated SDK is game-derived (class and function names, layouts): it stays local, the
same as `gnames.txt`. Findings from it go into Infinite ENGINE_NOTES as summaries.

What it would buy over what we have: every class layout by name instead of by `bsiprop`
walks, and C++ wrappers for ProcessEvent calls that today are hand-assembled by name in
`reflect.cpp`. What it does not buy: nothing for BS1 or BS2 (not UE3), and no new classes -
CodeRed reads the game; it does not compile UnrealScript.

## Open questions, and the test that settles each

| Question | Test | Needs |
|---|---|---|
| Does a 2007-SDK class package load in Remastered? | one no-content class, loaded by name through ProcessEvent, logs from `PostBeginPlay` | the 2007 game + SDK installed, one BS1 launch |
| Which BS1 classes changed between 2007 and Remastered? | UELib corpus of the 2007 packages, diffed against `tools\uscript\bs1\` | the 2007 game, offline |
| Where is Infinite's GObjObjects? | the two IDA shapes above, then the live `InternalIndex == i` read | Infinite installed; the live half is one launch |
| Does `bsmesh-export` read BS2's packages? | `-Game bs2 -List` on any map | BS2 installed, offline |
