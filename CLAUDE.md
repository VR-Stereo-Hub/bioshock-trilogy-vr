# bioshock-vr - Claude session guide

Native VR mod for BioShock Remastered, BioShock 2 Remastered and BioShock Infinite: a 32-bit DLL
injected via an `xinput1_3.dll` proxy shim, hooking the game's D3D11 renderer and the engine's
camera/aim paths, driven by an OpenXR session (Quest 3 via Virtual Desktop/VDXR or Steam
Link/SteamVR). Architecture is a game-agnostic VR core plus per-game adapters; the adapter
registry picks by host exe name (`BioshockHD.exe` -> bioshock1r, `Bioshock2HD.exe` -> bioshock2r,
`BioShockInfinite.exe` -> bioshockinf).

The two remasters are Vengeance (UE2.5). **BioShock Infinite is Unreal Engine 3 build 6829** - a
different engine, so no number transfers, and even shapes are suspect.

**`main` is THE branch: all three mods live there, all three work, all three shipped in v0.8.0
(2026-08-13, session 59)** - but day-to-day work no longer merges straight into it. Branch every
new session off `staging` and open a **pull request back to `staging`**; `main` moves only through
a `staging` -> `main` PR at release time. See "Branches, PRs and staging" below. The old per-game
branches (`bioshock-2`, `bioshock-infinite`) are historical and fully contained in `main`; do not
start work on them.

**Read `CONTRIBUTING.md` before your first commit.** More than one person works here now:
`main` is never committed to directly, every change reaches it through a reviewed PR, and
one commit is one logical change. `docs/ORG-PRACTICES.md` covers the repo and org settings
that back those rules up.

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

## Branches, PRs and staging

Direction from VOID (2026-08-23), and it applies to everyone working here.

```
feature branch --PR--> staging --PR (at release time)--> main --> tagged release
```

- **Open a PR to merge a branch anywhere.** Never merge branch-to-branch by hand. The PR is
  what tells you whether there are merge conflicts; when there are, fix them on the branch
  and **re-test** before merging, rather than discovering them inside someone's unrelated
  work later.
- **`staging` is the day-to-day target**, not `main`. It holds work that is tested but not
  yet ready for a release. Both contributors PR into it, which is what keeps the conflicts
  small and early.
- **PR at every good stopping point** - a feature that works and has been tested - not once
  at the end. Small and frequent beats one large PR: it is what lets the other contributor
  join a subject while it is still in flight. A large roll-up is for the end of a cycle.
- **A PR does not have to be merged, and can be a draft.** An open or draft PR leaves
  `staging`, `main` and the release completely untouched, while still surfacing conflicts
  and still signalling that the subject is taken. Open it early as a draft by default.
- **`main` changes only through a `staging` -> `main` PR**, and the release is cut from
  `main` after that merge.
- **Always PR a feature branch into `staging`, never into another feature branch.**
  VOID, 2026-08-23: stacked feature-to-feature PRs "can get quite dirty real quick".
  If your work depends on another branch that has not merged yet, wait for it to land
  in `staging` and rebase onto it, rather than opening a PR that targets it.

**NEVER MERGE without the user confirming it first.** Agents may commit and open pull
requests on their own judgement - that is the point of the flow above, and an open or
draft PR changes nothing until someone merges it. The merge is the irreversible step and
the one that needs a human saying yes (VOID, 2026-08-23). Show what is about to land and
wait; finishing a feature is not permission to merge it.

## Session protocol

- **START**: read `docs/STATUS.md`, then the current milestone in `docs/ROADMAP.md`, then
  `git log --oneline -10`. **ALWAYS BRANCH FROM `staging`** - every game ships from the same
  trunk; there is no per-game branch to hunt for. **Working on Infinite?** Same `staging`, but
  the ladder is `docs/bioshockinfinite/ROADMAP.md` (milestones I0-I11 after the 2026-08-05
  BS2-shaped restructure), which is separate from M0-M10.
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

**THE EXE PATHS BELOW ARE ONE MACHINE'S LAYOUT, NOT THE TRUTH.** Since 65f02fa the
scripts resolve each game per-machine from the Steam library folders, so `-Game bs1`
finds it wherever it is. Do not hardcode these, and do not trust them when a script
disagrees - on the machine this was last verified on, BS1 lives under
`C:\Program Files (x86)\Steam\`, not `K:`. The **appids** and the **data dirs** are
the parts that are actually fixed.

- BioShock 1: `<steam library>\steamapps\common\BioShock Remastered\Build\Final\BioshockHD.exe`
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
| `CONTRIBUTING.md` | **Branch, commit and PR rules.** Read before your first commit of a session |
| `docs/ORG-PRACTICES.md` | Repo and `VR-Stereo-Hub` settings that enforce the above (admin) |
| `docs/STATUS.md` | **Session handoff**: current state, next steps, blockers, session log |
| `docs/ROADMAP.md` | BS1/BS2 milestones M0–M10 with acceptance criteria and checkboxes |
| `docs/ARCHITECTURE.md` | Module design, core/adapter contract, stereo strategy, decision log |
| `docs/RESEARCH.md` | All research findings with sources (engine, prior art, VR runtimes, legal) |
| `docs/PORT-CANDIDATES.md` | **BS1 behaviours BS2/Infinite have never been tested with**, and the exact one-line opt-in for each. Add a row in the same commit as any core default only one game opts into |
| `docs/CONTROLS.md` | **Controller config**: `BioshockVR.ini` reference, which keys are live vs planned, hardware notes that decide the d-pad modifier |
| `docs/VERIFICATION.md` | **Verification catalog**: intent -> tool -> command -> how to read the result. The simulated OpenXR runtime, the command seam, screenshots, img-diff, frame dumps, record/replay - and what still needs a human in the headset |
| `docs/bioshock1/ENGINE_NOTES.md` | BS1 reverse-engineering knowledge base: signatures, offsets, class layouts, hook points; also holds the full derivation recipes |
| `docs/bioshock1/TESTING.md` | How to install, launch, verify each milestone; VR setup; crash triage |
| `docs/bioshock2/ENGINE_NOTES.md` | BS2 knowledge base: verified RVAs, the ProcessEvent CalcView seam, BS1 deltas |
| `docs/bioshock2/TESTING.md` | BS2 install/launch/harness deltas + M3 checklists |
| `docs/bioshockinfinite/ROADMAP.md` | **Infinite milestones I0–I11** (separate ladder from M0–M10) |
| `docs/bioshockinfinite/ENGINE_NOTES.md` | Infinite (UE3) knowledge base: PE identity, injection vector, UE3 reflection evidence, the cheat/Exec surfaces, carried-over rules |
| `docs/bioshockinfinite/TESTING.md` | Infinite install/launch/harness deltas + the BS2-conflict rule |
