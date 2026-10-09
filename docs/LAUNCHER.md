# BioShock Remastered VR launcher

Review candidate on `codex/rapture-launcher`, based on staging `5dd79c0`.
The rendered appearance still requires the owner's acceptance. The current
icon follows the owner's supplied medallion and exact game-wordmark references,
with VR integrated beneath the name inside the same plaque. The owner explicitly
directed **do not install**; deliver a review EXE and renders only. This launcher
targets **BioShock Remastered (Steam app 409710)**. Its embedded mod is the
staging build, not the separately installed hand-compatible development build.

## Reference audit

The implementation follows the local Dishonored VR installer at
`cdd8d32c5d9825e304d118157c178962c7ebbc01`. Research covered
`docs/INSTALLER.md`, the native window, production offscreen renderer,
screen/view models, configuration editing, GPU detection, file transactions,
process and elevation helpers, release parsing, download validation, support
collection, and shortcut behavior under `src/tools/installer`.

The BioShock side was checked against the current F10 theme, preference model
and parser, preset loading, viewport configuration, OpenXR runtime selection,
XInput proxy, release packaging, and the SteamVR loader provenance pin. The
launcher does not add hooks or write game memory.

The shared theme initializer clears cached font pointers before each fresh
ImGui context. This allows repeated offscreen renders on machines missing an
optional Windows font. Normal one-context in-game initialization is unchanged.

| Dishonored capability | BioShock implementation |
| --- | --- |
| Native Win32 / D3D11 / ImGui shell | Same architecture, x86, asynchronous work and per-monitor DPI |
| Sidebar and persistent Play action | Overview, Settings, Mods, Bindings, Updates, Help & about; Play at bottom right |
| Themed production-view screenshots | Compiles the **same F10 theme source** and embeds the same six PNG resources |
| Steam discovery and manual folder selection | Reads Steam library VDF and app manifest, validates `Build/Final/BioshockHD.exe` as PE32 |
| Embedded installer payload | `xinput1_3.dll`, `bioshockvr.dll`, `bvr_steamvr32.dll`, `openvr_api.dll` |
| Transactional installation and repair | Complete preflight, original backups, per-operation journal, atomic replacements, readback and rollback |
| Runtime selection | Actual supported `auto`, `native`, `steamvr` values in `xr.ini`; no unsupported VDXR override |
| Headset selection | Seventeen headset groups plus custom text, recorded for support; does not claim controller-specific tuning |
| Quality controls | Four pixel-count presets, slider and exact dimensions, writing both Windows viewport pairs |
| Player configuration | F10 global player preferences, preserving unrelated, unknown, per-hand, per-weapon and debug rows |
| Disable and enable | Parks only `bioshockvr.dll`; the known XInput proxy continues forwarding flat-game input |
| Removal and recovery | Checks current ownership hashes, restores original files, keeps calibration, saves, logs and recovery snapshots |
| Updates | Explicit GitHub check, cached history, exact asset URL, size / SHA256 / version validation, explicit opening of the new launcher |
| Desktop and Start menu shortcuts | Preserve resolved game, data and game-INI paths |
| Support collector | Embedded script creates a local ZIP with logs, INIs and file hashes; no upload |
| Elevation | `asInvoker`; a denied operation can request a worker, with resolved user paths and a checked result |
| UI verification | Same drawing function for native and offscreen views; fixture states, theme-load and visible-button bounds checks |

Dishonored-only AFW, DLSS, FSR, ReShade and texture-management controls were not
copied: this BioShock tree has no corresponding supported integrations. Its
monolithic INI and engine-specific baseline settings do not apply here.
In-place replacement of the running launcher is not implemented: a verified
new executable opens separately, then installs its embedded build explicitly.
Existing shortcuts can be recreated from that new launcher. Live updates still
depend on publishing a correctly named launcher asset in a future GitHub release.

## Settings contract

Opening the launcher or choosing a page never applies settings. Install and
update keep every existing preset and F10 preference unless the player has
explicitly edited that setting. New installs use the mod's built-in defaults;
bundled preset files are available through the separate confirmed recovery action.

* `%LOCALAPPDATA%/BioshockVR/menu-settings.ini`: global player rows for automatic
  VR start, snap/smooth turning, turn angle/speed, swing attacks/threshold,
  reticle, laser, and cinematic borders. The production F10 parser verifies edits.
* `%LOCALAPPDATA%/BioshockVR/xr.ini`: `[runtime] mode`. This runtime setting is
  shared by all three trilogy adapters. Native uses the active 32-bit runtime.
* `%LOCALAPPDATA%/BioshockVR/launcher.ini`: `[Headset] Model`, for support only.
* `%APPDATA%/BioshockHD/Bioshock/Bioshock.ini`: the four `WindowedViewportX/Y` and
  `FullscreenViewportX/Y` keys in `[WinDrv.WindowsClient]`. Missing or duplicated
  viewport keys refuse the entire transaction. `[XeDrv.XenonClient]` is untouched.

Line endings, BOMs, other sections and unrelated settings survive. Runtime and
headset edits also reject ambiguous duplicate sections or keys. The transaction
journal contains complete before files and complete after INIs with hashes.
Game-process state and destination bytes are checked again immediately before
each mutation. Reparse destinations and an identified retired BioShock DXGI
loader are refused. An unrelated `dxgi.dll` is preserved.

Quality percentages describe **pixel count** relative to 2750 x 2850, the current
BioShock F10 reference size. Width and height use the square root of that ratio.
These presets are convenience choices, not measured GPU or headset performance
recommendations. A custom installed size is retained until a player changes it.

Uninstall means restoring the installation that preceded this launcher. If that
was another VR build, it returns. The confirmation explicitly explains this.
Restore bundled calibration is a distinct action that backs up and replaces
`vrpreset.ini`, `hands.ini`, `weapons.ini`, and clears F10 overrides. It retains
runtime/headset choices and unrelated files.

## Build and development verification

Run `tools/build-launcher.ps1 -Tests` from a Windows development checkout with
the normal submodules initialized. Visual Studio CMake builds Win32
RelWithDebInfo. The target is `bvr_launcher`; the output is
`build/src/RelWithDebInfo/BioShockVR-Launcher-v0.8.3.exe` for this version.
Set `BVR_LAUNCHER_ICON` at configure time to choose another reviewed ICO.
The OpenVR binary must match `third_party/openvr_headers/PROVENANCE.txt`.

Maintainer commands (the owner is not asked to run these):

```powershell
build/src/tools/launcher/RelWithDebInfo/bvr_launcher_tests.exe C:/scratch/new-host-test-folder
tools/test-launcher.ps1
build/src/RelWithDebInfo/BioShockVR-Launcher-v0.8.3.exe --render all C:/scratch/renders --scale 1.5
build/src/RelWithDebInfo/BioShockVR-Launcher-v0.8.3.exe --preview
```

`--render` width/height are logical dimensions; scale produces the corresponding
physical pixel size. `--preview` uses explicit sample data and blocks installation,
external file actions and game launch. Rendered fixtures identify themselves in
the footer. A normal launch detects the real installation but does not apply it.

The EXE also accepts `--inspect`, `--apply --op`, `--support`, resolved path
overrides and JSON result files for reproducible verification. The test harness
creates a **new scratch tree**, writes a nonexecutable PE header fixture, and
never launches a game. It keeps all evidence instead of deleting it.

## Verification and acceptance

* Win32 RelWithDebInfo build of the launcher, embedded DLLs and host suite.
* 184 host checks: production preference roundtrips, unknown/debug/profile row
  preservation, CRLF/LF/BOM handling, argument quoting, strict release parsing,
  complete install/update/disable/enable/uninstall/defaults lifecycles, Unicode
  paths, whole-file comparison, eight partial-write rollback points, a real
  sharing violation, invalid manifests, duplicate settings, foreign replacement
  refusal, old-loader detection, tampered payloads and wrong architecture.
* 35 checks against the **actual compiled EXE**: embedded hashes, complete game
  and F10 file diffs, runtime CRLF preservation, support archive contents, original
  restoration and save/calibration preservation. The support test found and
  fixed an inherited PowerShell module-path dependency by using .NET hashing/ZIP.
* Eighteen production UI states rendered at 100%, 150%, 200%, and the minimum
  logical window size, with no theme-load failures or visible-button bounds
  errors. Overview, settings, controls and confirmation dialogs visually checked.
* Native preview opened and inspected on the actual Windows desktop. Automated
  click checks yielded when user input was detected, so they are not claimed as
  a completed interactive test suite.

The full scratch suites passed before the final icon-only revisions. A subsequent
repeat was blocked by the running-game guard before any write; no game process
was closed to bypass it. After the owner's explicit do-not-install direction,
only builds and rendering continued.

Pending: final icon/render visual acceptance. Real-game installation/play,
the UAC prompt/worker route on a denied game directory, Steam launch, headset
runtime behavior and a future published launcher download remain unverified.
No real-game installation or launch is authorized in this review task. No game
was launched during this work. The existing installed hand-compatible DLL and
player settings were not replaced.

Recovery handles reported write failures. A process kill or power loss leaves a
journal and originals for manual recovery; automatic crash-resume is not claimed.
Code provenance and retained licensing are in `src/tools/launcher/NOTICE.md`.
