# bioshock-vr - Claude session guide

Native VR mod for BioShock Remastered, BioShock 2 Remastered and BioShock Infinite: a 32-bit DLL
injected via an `xinput1_3.dll` proxy shim, hooking the game's D3D11 renderer and the engine's
camera/aim paths, driven by an OpenXR session (Quest 3 via Virtual Desktop/VDXR or Steam
Link/SteamVR). Architecture is a game-agnostic VR core plus per-game adapters; the adapter
registry picks by host exe name (`BioshockHD.exe` -> bioshock1r, `Bioshock2HD.exe` -> bioshock2r,
`BioShockInfinite.exe` -> bioshockinf).

The two remasters are Vengeance (UE2.5). **BioShock Infinite is Unreal Engine 3 build 6829** - a
different engine, so no number transfers, and even shapes are suspect. Infinite MERGED to `main`
at v0.8.0 (2026-08-13, session 59) - all three games now ship from one line; `bioshock-infinite`
stays as the Infinite integration branch, but `main` is current and carries every game.

## Hard rules

- **NEVER commit game-derived content**: no decompiled UnrealScript, no extracted assets, no
  RenderDoc captures, nothing from the game folder. `tools/uscript/` is gitignored for a reason.
  Summarize findings in the per-game ENGINE_NOTES instead of pasting game code.
- **Commit messages**: plain conventional commits (`feat:`/`fix:`/`docs:`/`build:`/`tools:`/`chore:`),
  imperative, subject ≤72 chars. No trailers.
- **32-bit (Win32) only.** All three games are x86. The CMake guard will stop you; don't fight it.
- **No code from UEVR** (all-rights-reserved - concepts only). REFramework (MIT) may be adapted
  with an attribution comment in the file.
- Engine addresses/signatures live ONLY in the per-game `src/game/<title>/patterns.cpp/.h`, and
  every one is documented in that game's `docs/<game>/ENGINE_NOTES.md` with its derivation method.
  NEVER copy a number between games - same engine tree, different link; derive fresh.
- **BS2 is NOT bound by BS1's methods** (user directive, 2026-07-29 session 24). Much of BS1's
  machinery - the foreground/viewmodel FOV counter-modeling, weapon scaling compensation, aim-seam
  workarounds - exists because of BS1-specific limitations, not because it is the right design.
  If BS2's build affords a better or more native method (it has a native FOV slider, native
  dual-wield, ProcessEvent-by-name event hooking), USE THE BETTER METHOD. Before porting any BS1
  compensation machinery to BS2, first test whether BS2 needs it at all.
- **The same gate applies to Infinite, harder** - it is a different engine (UE3 build 6829), so
  no number transfers and even shapes are suspect. Check what UE3 does natively, test whether the
  BS1/BS2 defect even EXISTS, and only then port compensation machinery, and only the parts proven
  necessary. A mono screenshot is not a sufficient check for a lens question.
- **NEVER run BioShock Infinite while BioShock 2 is running** (user directive, 2026-07-31
  session 34). BS2 development runs in parallel and only one game can own the headset at a time.
  Check `Get-Process Bioshock2HD` before any test that launches or drives Infinite; if it is
  running, WAIT or POSTPONE the test - do not close the other game. Building, installing,
  packaging and tailing logs do NOT contend and must keep working while BS2 runs. The `-Game bsi`
  harness scripts enforce this via `tools/lib/assert-no-conflict.ps1`.
- **KEEP THE PER-GAME MODS DECOUPLED. Duplicate code is fine** (user directive, 2026-07-31
  session 34; the same was said for the BioShock Infinite mod). Copy a BS1 behaviour into
  `bioshock2r/` and adapt it rather than promoting it to `src/core/` or parameterising the BS1
  version - BS1 is the headset-accepted baseline and must not be put at risk to serve BS2, and
  BS1 regressions cost headset time to even detect. Put something in `src/core/` only when it is
  genuinely game-agnostic AND new; if a core change is unavoidable, keep it purely additive so
  no BS1 path changes behaviour. Consolidation and de-duplication are deferred to a dedicated
  "healing" session in the polish milestone.

## Session protocol

- **START**: read `docs/STATUS.md`, then the current milestone in `docs/ROADMAP.md`, then
  `git log --oneline -10`. **Working on Infinite?** You want branch `bioshock-infinite` and
  `docs/bioshockinfinite/ROADMAP.md` (milestones I0-I11 after the 2026-08-05 BS2-shaped restructure), which is a separate ladder from M0-M10.
- Touching engine internals? Read the game's `docs/<game>/ENGINE_NOTES.md` first
  (`docs/bioshock1/`, `docs/bioshock2/` or `docs/bioshockinfinite/`). New findings go there, in
  the same commit as the code that uses them.
- **Validate in the SIMULATOR before handing a build to the user.** `tools\xrsim-launch.ps1`
  runs the game against `bvr_xrsim32.dll`, a simulated 32-bit OpenXR runtime that presents as a
  Quest 3, so head/hand poses, every controller button, deterministic frame stepping and per-eye
  compositor captures - **including the quad layers a window screenshot can never show** (the
  aim laser, the HUD panel) - are all scriptable with no headset. Asking the user to put the
  Quest 3 on for something the simulator could have answered is a wasted test session.
  Catalog: `docs/VERIFICATION.md`. Perceptual questions - comfort, judder, world scale, "does
  the weapon swim" - still need the headset, and still belong in the F10 overlay.
- Non-obvious design choices get a dated entry in the decision log at the bottom of
  `docs/ARCHITECTURE.md`.
- **END**: rewrite the "Current state" and "Next steps" sections of `docs/STATUS.md`, append a
  dated session-log entry, tick `docs/ROADMAP.md` boxes, commit, push. A session that ends
  without pushing STATUS.md is a failed handoff.

## Build / install / test

```powershell
.\tools\build.ps1            # Debug build. CMake is NOT on PATH - script finds the VS-bundled one via vswhere
.\tools\build.ps1 -Release
.\tools\build.ps1 -Install [-Game bs1|bs2|bsi]   # build + copy DLLs to that game's folder (default bs1)
.\tools\install.ps1 [-Game bs1|bs2|bsi]          # copy already-built DLLs
.\tools\tail-log.ps1 [-Game bs1|bs2|bsi]         # follow the game's log (see data dirs below)

.\tools\xrsim-selftest.ps1                   # is the SIMULATED OpenXR runtime healthy?
.\tools\xrsim-launch.ps1 -Game bs1           # launch against the simulator (no headset needed)
.\tools\xrsim-cmd.ps1 "head rot 30 0 0"      # drive the simulated head/hands/controls
.\tools\xrsim-shot.ps1 -Out shot             # per-eye compositor capture + JSON to assert on
```

- BioShock 1: `K:\SteamLibrary\steamapps\common\BioShock Remastered\Build\Final\BioshockHD.exe`
  (32-bit, D3D11, no DRM; Steam appid 409710). Launch through Steam; add `-allowconsole` to
  launch options for the Tab console. Data dir: `%LOCALAPPDATA%\BioshockVR\`.
- BioShock 2: `D:\SteamLibrary\steamapps\common\BioShock 2 Remastered\Build\Final\Bioshock2HD.exe`
  (appid 409720). Data dir: `%LOCALAPPDATA%\BioshockVR\bs2\` - the games never share files.
- BioShock Infinite:
  `D:\SteamLibrary\steamapps\common\BioShock Infinite\Binaries\Win32\BioShockInfinite.exe`
  (appid 8870; note UE3's `Binaries\Win32\`, not `Build\Final\`). Data dir:
  `%LOCALAPPDATA%\BioshockVR\bsi\`. **See the BS2-conflict rule in Hard rules before testing.**
- Full test procedures (incl. Quest 3 / Virtual Desktop setup): `docs/bioshock1/TESTING.md`;
  BS2 deltas in `docs/bioshock2/TESTING.md`, Infinite deltas in
  `docs/bioshockinfinite/TESTING.md`. The harness scripts
  (`game-cmd`/`game-shot`/`game-click`) take `-Game bs2|bsi`; `boot.ps1` is BS1-only.
- Clean clone needs `git clone --recursive` (submodules in `third_party/`).

## Repo map

- `src/proxy/` - thin xinput1_3 forwarding shim that loads the real mod DLL
- `src/core/` - game-agnostic VR core (framework, hooks, gfx, vr, stereo, input, ui, util)
- `src/game/` - `igame_adapter.h` + `adapter_registry.cpp` (host-exe dispatch) + per-game
  adapters (`bioshock1r/`, `bioshock2r/`, `bioshockinf/` from I1) + `shared/` (engine math,
  no addresses)
- `third_party/` - pinned submodules: minhook, imgui, OpenXR-SDK
- `tools/` - build/install/uninstall/log scripts, `lib/` shared helpers,
  `uscript/` decompile workspace (gitignored)
- `docs/` - the project's brain; see index below

## Docs index

| File | Purpose |
|---|---|
| `docs/STATUS.md` | **Session handoff**: current state, next steps, blockers, session log |
| `docs/ROADMAP.md` | BS1/BS2 milestones M0–M10 with acceptance criteria and checkboxes |
| `docs/ARCHITECTURE.md` | Module design, core/adapter contract, stereo strategy, decision log |
| `docs/RESEARCH.md` | All research findings with sources (engine, prior art, VR runtimes, legal) |
| `docs/VERIFICATION.md` | **Verification catalog**: intent -> tool -> command -> how to read the result. The simulated OpenXR runtime, the command seam, screenshots, img-diff, frame dumps, record/replay - and what still needs a human in the headset |
| `docs/bioshock1/ENGINE_NOTES.md` | BS1 reverse-engineering knowledge base: signatures, offsets, class layouts, hook points; also holds the full derivation recipes |
| `docs/bioshock1/TESTING.md` | How to install, launch, verify each milestone; VR setup; crash triage |
| `docs/bioshock2/ENGINE_NOTES.md` | BS2 knowledge base: verified RVAs, the ProcessEvent CalcView seam, BS1 deltas |
| `docs/bioshock2/TESTING.md` | BS2 install/launch/harness deltas + M3 checklists |
| `docs/bioshockinfinite/ROADMAP.md` | **Infinite milestones I0–I11** (separate ladder from M0–M10) |
| `docs/bioshockinfinite/ENGINE_NOTES.md` | Infinite (UE3) knowledge base: PE identity, injection vector, UE3 reflection evidence, the cheat/Exec surfaces, carried-over rules |
| `docs/bioshockinfinite/TESTING.md` | Infinite install/launch/harness deltas + the BS2-conflict rule |
