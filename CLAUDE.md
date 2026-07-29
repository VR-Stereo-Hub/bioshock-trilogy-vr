# bioshock-vr - Claude session guide

Native VR mod for BioShock Remastered (and BioShock 2 Remastered): a 32-bit DLL injected via an
`xinput1_3.dll` proxy shim, hooking the game's D3D11 renderer and Vengeance-engine (UE2.5)
camera/aim paths, driven by an OpenXR session (Quest 3 via Virtual Desktop/VDXR or Steam
Link/SteamVR). Architecture is a game-agnostic VR core plus per-game adapters; the adapter
registry picks by host exe name (`BioshockHD.exe` -> bioshock1r, `Bioshock2HD.exe` -> bioshock2r).

## Hard rules

- **NEVER commit game-derived content**: no decompiled UnrealScript, no extracted assets, no
  RenderDoc captures, nothing from the game folder. `tools/uscript/` is gitignored for a reason.
  Summarize findings in the per-game ENGINE_NOTES instead of pasting game code.
- **Commit messages**: plain conventional commits (`feat:`/`fix:`/`docs:`/`build:`/`tools:`/`chore:`),
  imperative, subject ≤72 chars. No trailers.
- **32-bit (Win32) only.** Both games are x86. The CMake guard will stop you; don't fight it.
- **No code from UEVR** (all-rights-reserved - concepts only). REFramework (MIT) may be adapted
  with an attribution comment in the file.
- Engine addresses/signatures live ONLY in the per-game `src/game/<title>/patterns.cpp/.h`, and
  every one is documented in that game's `docs/<game>/ENGINE_NOTES.md` with its derivation method.
  NEVER copy a number between games - same engine tree, different link; derive fresh.

## Session protocol

- **START**: read `docs/STATUS.md`, then the current milestone in `docs/ROADMAP.md`, then
  `git log --oneline -10`.
- Touching engine internals? Read the game's `docs/<game>/ENGINE_NOTES.md` first
  (`docs/bioshock1/` or `docs/bioshock2/`). New findings go there, in the same commit as the
  code that uses them.
- Non-obvious design choices get a dated entry in the decision log at the bottom of
  `docs/ARCHITECTURE.md`.
- **END**: rewrite the "Current state" and "Next steps" sections of `docs/STATUS.md`, append a
  dated session-log entry, tick `docs/ROADMAP.md` boxes, commit, push. A session that ends
  without pushing STATUS.md is a failed handoff.

## Build / install / test

```powershell
.\tools\build.ps1            # Debug build. CMake is NOT on PATH - script finds the VS-bundled one via vswhere
.\tools\build.ps1 -Release
.\tools\build.ps1 -Install [-Game bs1|bs2]   # build + copy DLLs to that game's folder (default bs1)
.\tools\install.ps1 [-Game bs1|bs2]          # copy already-built DLLs
.\tools\tail-log.ps1 [-Game bs1|bs2]         # follow the game's log (see data dirs below)
```

- BioShock 1: `K:\SteamLibrary\steamapps\common\BioShock Remastered\Build\Final\BioshockHD.exe`
  (32-bit, D3D11, no DRM; Steam appid 409710). Launch through Steam; add `-allowconsole` to
  launch options for the Tab console. Data dir: `%LOCALAPPDATA%\BioshockVR\`.
- BioShock 2: `D:\SteamLibrary\steamapps\common\BioShock 2 Remastered\Build\Final\Bioshock2HD.exe`
  (appid 409720). Data dir: `%LOCALAPPDATA%\BioshockVR\bs2\` - the games never share files.
- Full test procedures (incl. Quest 3 / Virtual Desktop setup): `docs/bioshock1/TESTING.md`;
  BS2 deltas in `docs/bioshock2/TESTING.md`. The harness scripts
  (`game-cmd`/`game-shot`/`game-click`) take `-Game bs2`; `boot.ps1` is BS1-only.
- Clean clone needs `git clone --recursive` (submodules in `third_party/`).

## Repo map

- `src/proxy/` - thin xinput1_3 forwarding shim that loads the real mod DLL
- `src/core/` - game-agnostic VR core (framework, hooks, gfx, vr, stereo, input, ui, util)
- `src/game/` - `igame_adapter.h` + `adapter_registry.cpp` (host-exe dispatch) + per-game
  adapters (`bioshock1r/`, `bioshock2r/`) + `shared/` (engine math, no addresses)
- `third_party/` - pinned submodules: minhook, imgui, OpenXR-SDK
- `tools/` - build/install/uninstall/log scripts, `uscript/` decompile workspace (gitignored)
- `docs/` - the project's brain; see index below

## Docs index

| File | Purpose |
|---|---|
| `docs/STATUS.md` | **Session handoff**: current state, next steps, blockers, session log |
| `docs/ROADMAP.md` | Milestones M0–M10 with acceptance criteria and checkboxes |
| `docs/ARCHITECTURE.md` | Module design, core/adapter contract, stereo strategy, decision log |
| `docs/RESEARCH.md` | All research findings with sources (engine, prior art, VR runtimes, legal) |
| `docs/bioshock1/ENGINE_NOTES.md` | BS1 reverse-engineering knowledge base: signatures, offsets, class layouts, hook points; also holds the full derivation recipes |
| `docs/bioshock1/TESTING.md` | How to install, launch, verify each milestone; VR setup; crash triage |
| `docs/bioshock2/ENGINE_NOTES.md` | BS2 knowledge base: verified RVAs, the ProcessEvent CalcView seam, BS1 deltas |
| `docs/bioshock2/TESTING.md` | BS2 install/launch/harness deltas + M3 checklists |
