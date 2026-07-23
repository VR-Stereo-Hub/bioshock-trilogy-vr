# Project status

> Handoff file. Rewrite "Current state" and "Next steps" every session; append to the session log.

## Current state (2026-07-23, end of session 1)

M0 (skeleton) is functionally complete and **verified in-game**: the xinput1_3 proxy + bioshockvr.dll
load into BioshockHD.exe, MinHook initializes, the D3D11 Present/ResizeBuffers hooks fire, and the
log confirms the game runs the **D3D11 renderer** (feature level 11_0, 2560×1440, exclusive
fullscreen) on an RTX 4060. The ImGui overlay is **visually confirmed by the user** (main menu,
500 fps, windowed mode); the toggle key is **F10** (changed from Insert - user's keyboard lacks it). LAA flag confirmed YES (4 GB address space). The game's first launch generated
the user config at `%AppData%\Roaming\BioshockHD\Bioshock\`. Repo is public at
https://github.com/mohamad-balouza/bioshock-vr with Debug + Release builds working.

**User checklist:**
1. ~~Press F10, confirm the overlay toggles~~ - confirmed by user 2026-07-23.
2. Play a few minutes - any input weirdness, stutter, or crash with the mod installed?
3. Optional: run `.\tools\tail-log.ps1` in a terminal while playing (works live).

## Next steps

1. **DR-1 (critical, next session's headline):** standalone 32-bit OpenXR hello-world
   (`tools/xr-hello32`): enable `BIOSHOCKVR_WITH_OPENXR=ON`, build the 32-bit loader, create an
   instance/session, log runtime name. Test under **VDXR** (Virtual Desktop) and **SteamVR**.
   This decides the whole M2+ integration path (fallbacks designed in ARCHITECTURE.md).
2. DR-4: port the PlayerCalcView FName-chain scan to C++ (`src/game/bioshock1r/patterns.cpp`),
   hook it, add ImGui debug sliders for camera offset + per-frame FOV write (PC+0xE0), wobble test.
3. DR-3: RenderDoc x86 capture with the proxy loaded → frame map into ENGINE_NOTES.md.
4. DR-7: force borderless/windowed via ini (game defaults to exclusive fullscreen) and check
   overlay/capture stability.
5. DR-6: instrument DINPUT8/window messages during menu use (which input path do menus read?).

## Open questions / blockers

- **DR-1**: 32-bit OpenXR runtime support in VDXR is unverified (SteamVR fallback → 64-bit
  companion compositor if both fail).
- Console availability in the current Steam build unverified - test Tab with `-allowconsole`.
- Adapter VRAM logs as "3072 MB" - DXGI_ADAPTER_DESC.DedicatedVideoMemory is a 32-bit SIZE_T in
  our process, so values ≥4 GB truncate. Cosmetic; ignore.
- itsloopyo's headtracking mod also installs as `xinput1_3.dll` - mutually exclusive with ours
  (install.ps1 backs theirs up automatically).

## Session log (newest first)

### 2026-07-23 - Session 1

- Researched game installation (32-bit DX11 Vengeance/UE2.5, no DRM, gameswf Flash UI), engine
  family (BS2R same engine; Infinite UE3-6829), modding ecosystem, prior VR art (vorpX G3D works;
  itsloopyo headtracking hook proven; REFramework MIT reference), and the VR injection stack
  (OpenXR-first for VDXR+SteamVR). Full findings → RESEARCH.md.
- Decided architecture (C++20/x86, xinput proxy → bioshockvr.dll, core+adapter split, stereo
  ladder with SequentialReentry as primary bet) → ARCHITECTURE.md decision log.
- Built: repo + docs suite, CMake (VS2022 `-A Win32`, submodules minhook/imgui/OpenXR-SDK pinned),
  xinput proxy (ordinals verified against the real SysWOW64 DLL with dumpbin - game imports @2/@3),
  mod DLL (deferred init, logger, minidump handler, MinHook, kiero-style Present/ResizeBuffers
  hooks, ImGui overlay), tools scripts.
- **In-game smoke test passed** on first run (full init chain + D3D11 device info in log).
  Found + fixed: logger file locking (fopen_s denies sharing → switched to `_wfsopen` with
  `_SH_DENYNO`), non-ASCII mojibake in log lines, missing `-Install` passthrough in build.ps1.
- Verified: LAA=YES; D3D11 confirmed at runtime; user ini path confirmed after first launch.
- Repo created and pushed: https://github.com/mohamad-balouza/bioshock-vr (public, MIT).
