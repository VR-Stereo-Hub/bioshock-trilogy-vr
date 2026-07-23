# Third-party notices

This project vendors the following libraries as git submodules under `third_party/`. Each keeps
its own license file in its submodule.

| Library | License | Source | Used for |
|---|---|---|---|
| MinHook | BSD-2-Clause | https://github.com/TsudaKageyu/minhook | API/vtable inline hooking |
| Dear ImGui | MIT | https://github.com/ocornut/imgui | in-game debug/config overlay |
| OpenXR SDK (loader) | Apache-2.0 | https://github.com/KhronosGroup/OpenXR-SDK | OpenXR runtime client |

Reference code (not vendored, not linked):

- **REFramework** (MIT, https://github.com/praydog/REFramework) — portions of the VR/D3D11
  integration may be adapted; adapted files carry an attribution comment.
- **bioshock-remastered-headtracking** (MIT, https://github.com/itsloopyo/bioshock-remastered-headtracking)
  — the `PlayerCalcView` FName-chain scan and camera hook technique are ported from this project.
- **UEVR** (all rights reserved, https://github.com/praydog/UEVR) — studied for concepts only.
  No code is or may be copied from it.
