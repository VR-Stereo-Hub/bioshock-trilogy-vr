# Project status

> Handoff file. Rewrite "Current state" and "Next steps" every session; append to the session log.

## Current state (2026-07-23)

Session 1 in progress: repo scaffolding, documentation suite, build system, and the injection
skeleton (xinput proxy + mod DLL with logging/MinHook/Present hook/ImGui overlay) are being built.
Nothing has run inside the game yet.

## Next steps

1. Finish session 1: verify Debug+Release builds, push to GitHub, in-game smoke test (DLL loads,
   log written, overlay toggles).
2. **M1 de-risk battery** (see ROADMAP) — headline item: **DR-1, does a 32-bit OpenXR client work
   under VDXR and SteamVR?** Build `tools/xr-hello32` standalone test before any integration.
3. DR-2: run `tools/check-laa.ps1`; log D3D11 device feature level + swapchain desc from the
   Present hook (partially covered by skeleton logging).
4. DR-3: RenderDoc frame capture → write the frame map into ENGINE_NOTES.md.
5. DR-4: port the PlayerCalcView FName-chain scan to C++ (`patterns.cpp`), wobble-test the camera.

## Open questions / blockers

- **DR-1 (critical)**: 32-bit OpenXR runtime support in VDXR is unverified. Fallback ladder:
  SteamVR-only → 64-bit companion compositor process (see ARCHITECTURE.md).
- Console availability in the current Steam build (community reports are mixed) — verify Tab
  console with `-allowconsole` during first smoke test; fallback is User.ini bindings.
- The game has **never been launched on this machine** — first launch must generate
  `%AppData%\Roaming\BioshockHD\Bioshock\User.ini` (verify path), which we want to diff against
  `Build\Final\DefUser.ini`.
- itsloopyo's headtracking mod also installs as `xinput1_3.dll` — mutually exclusive with ours.
  Ours must eventually be a superset.

## Session log (newest first)

### 2026-07-23 — Session 1

- Researched game installation (32-bit DX11 Vengeance/UE2.5, no DRM, gameswf Flash UI), engine
  family (BS2R same engine; Infinite UE3-6829), modding ecosystem, prior VR art (vorpX G3D works;
  itsloopyo headtracking hook proven; REFramework MIT reference), and the VR injection stack
  (OpenXR-first for VDXR+SteamVR). Full findings → RESEARCH.md.
- Decided architecture (C++20/x86, xinput proxy → bioshockvr.dll, core+adapter split, stereo
  ladder with SequentialReentry as primary bet) → ARCHITECTURE.md decision log.
- Created repo, docs suite, CMake build (VS2022 -A Win32, submodules), injection skeleton, tools.
