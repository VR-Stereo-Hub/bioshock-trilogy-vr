# Project status

> Handoff file. Rewrite "Current state" and "Next steps" every session; append to the session log.

## Current state (2026-07-23, session 1 continued)

**M0 complete and user-verified** (F10 overlay confirmed visually in-game). **DR-1 fully
retired**: xr_hello32 (32-bit) ran a complete OpenXR session - 60 frames on VDXR 1.0.10 with the
Quest 3 connected, D3D11 device on the RTX 4060. DR-2 done earlier (LAA yes, D3D11 confirmed).
The path to M2 (game frame on a big screen in the headset) is fully unblocked: same device type,
same runtime, proven from a 32-bit process. Remaining M1 items: DR-3 (RenderDoc frame map),
DR-4 (CalcView hook port), DR-5/6/7. Toggle key is F10. Em dashes are banned repo-wide (they
broke PowerShell 5.1 parsing and mojibaked logs/ImGui).

## Previous state (session 1, first half)

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

1. **DR-4: port the PlayerCalcView FName-chain scan to C++** (`src/game/bioshock1r/patterns.cpp`),
   hook it, add ImGui debug sliders for camera offset + per-frame FOV write (PC+0xE0), wobble test.
   This is the gateway to M3 (6DOF camera).
2. **M2 (now unblocked): OpenXR session inside the game** - port the xr_hello32 flow into
   core/vr, pace frames from the Present hook, put the game frame on a quad layer. First
   in-headset gameplay moment.
3. DR-3: RenderDoc x86 capture with the proxy loaded → frame map into ENGINE_NOTES.md.
4. DR-7: force borderless/windowed via ini (game defaults to exclusive fullscreen) and check
   overlay/capture stability.
5. DR-6: instrument DINPUT8/window messages during menu use (which input path do menus read?).
6. Optional anytime: rerun xr_hello32 with SteamVR as active OpenXR runtime (Steam Link path).

## Open questions / blockers

- **DR-1**: 32-bit OpenXR runtime support in VDXR is unverified (SteamVR fallback → 64-bit
  companion compositor if both fail).
- Console availability in the current Steam build unverified - test Tab with `-allowconsole`.
- Adapter VRAM logs as "3072 MB" - DXGI_ADAPTER_DESC.DedicatedVideoMemory is a 32-bit SIZE_T in
  our process, so values ≥4 GB truncate. Cosmetic; ignore.
- itsloopyo's headtracking mod also installs as `xinput1_3.dll` - mutually exclusive with ours
  (install.ps1 backs theirs up automatically).

## Session log (newest first)

### 2026-07-23 - Session 1 (continued)

- User confirmed the overlay visually (screenshot: main menu, 500 fps). Toggle key changed
  Insert -> F10 (user's keyboard lacks Insert). Verified live by the user.
- Em-dash ban added (global user preference): they broke PowerShell 5.1 parsing (BOM-less
  UTF-8 read as ANSI) and mojibaked the log and ImGui text. Repo swept to ASCII hyphens.
- **DR-1 retired with a FULL PASS**: wired the OpenXR loader (static, CRT override needed -
  see ENGINE_NOTES), built xr_hello32 (32-bit), found VDXR ships a 32-bit runtime, and with
  the Quest 3 connected ran a complete session: Meta Quest 3 system, FL 11_0, RTX 4060 LUID
  match, 60 frames pumped. VDXR path proven; 64-bit companion fallback not needed.

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
