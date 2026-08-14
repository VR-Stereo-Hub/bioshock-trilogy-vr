# Third-party notices

This project vendors the following libraries as git submodules under `third_party/`. Each keeps
its own license file in its submodule.

| Library | License | Source | Used for |
|---|---|---|---|
| MinHook | BSD-2-Clause | https://github.com/TsudaKageyu/minhook | API/vtable inline hooking |
| Dear ImGui | MIT | https://github.com/ocornut/imgui | in-game debug/config overlay |
| OpenXR SDK (loader) | Apache-2.0 | https://github.com/KhronosGroup/OpenXR-SDK | OpenXR runtime client |

Vendored copies (not submodules) under `third_party/`:

| Library | License | Source | Used for |
|---|---|---|---|
| OpenVR SDK 2.15.6 (headers + win32 `openvr_api.dll`) | BSD-3-Clause | https://github.com/ValveSoftware/openvr @ 0924064 | the SteamVR shim (`src/tools/ovrshim/`) talks to SteamVR through it; see `third_party/openvr_headers/PROVENANCE.txt` for SHA-256 pins |

Adapted code:

- **Bioshock-Remastered-VR by BioVRDev** (https://github.com/BioVRDev/Bioshock-Remastered-VR,
  no license file published) - the SteamVR shim in `src/tools/ovrshim/` is adapted from their
  `OpenXRShim/` module (OpenXR-on-OpenVR). Copied and adapted with the author's explicit
  permission, given 2026-08-13: the two projects collaborate and share features both ways, and
  the author granted a green light to copy anything from that repository (recorded in
  `docs/RESEARCH.md`, "BioVRDev/Bioshock-Remastered-VR analysis"). Adapted files carry an
  attribution comment.

Reference code (not vendored, not linked):

- **REFramework** (MIT, https://github.com/praydog/REFramework) - portions of the VR/D3D11
  integration may be adapted; adapted files carry an attribution comment.
- **bioshock-remastered-headtracking** (MIT, https://github.com/itsloopyo/bioshock-remastered-headtracking)
  - the `PlayerCalcView` FName-chain scan and camera hook technique are ported from this project.
- **UEVR** (all rights reserved, https://github.com/praydog/UEVR) - studied for concepts only.
  No code is or may be copied from it.
