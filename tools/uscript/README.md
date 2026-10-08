# UnrealScript decompilation workspace

Working area for the decompiled UnrealScript of each game's script packages, produced by
`tools\uscript-export.ps1` (UE Explorer's UELib, driven one process per package by our
`ExportScripts.exe`, whose source is `tools\uscript-export\ExportScripts.cs`).

**Everything in this directory except this README is gitignored, deliberately.**
Decompiled UnrealScript is 2K's copyrighted code and UE Explorer is third-party software:
neither is ever committed or redistributed. Record *findings* (class layouts, function
flows, property names) as summaries in the game's `ENGINE_NOTES.md` instead.

Local layout (the tool file, `tools\tool-paths.ps1`, records the corpora and UE Explorer):

| path | what |
|---|---|
| `bs1\<Package>\<Class>.uc` | BS1 (`Build\Final\BakedScripts\pc\*.U`, build "2226:Vengeance", package version 142) - tool name `uscript_bs1` |
| `bs2\`, `bsi\` | the same for BS2 and Infinite, when built on a machine that has them |
| `_ueexplorer\ue-explorer\` | UE Explorer + `ExportScripts.exe` (tool name `ueexplorer`) |
| `_logs\` | `export-<game>.log` (one RESULT line per package) and UELib's per-package stderr |

Rebuilding a corpus on a new machine: obtain UE Explorer
(<https://github.com/UE-Explorer/UE-Explorer>, 1.6.2 verified; ask before downloading),
point the tool file at it, then

```powershell
.\tools\uscript-export\build.ps1
.\tools\uscript-export.ps1 -Game bs1
```

BS1 measured 2026-10-07: 12 packages, 1,765 classes, 0 failed.
