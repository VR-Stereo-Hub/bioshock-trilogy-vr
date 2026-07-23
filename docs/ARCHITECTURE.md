# Architecture

## Overview

```
BioshockHD.exe (32-bit, D3D11)
 └─ loads xinput1_3.dll  ................ our thin proxy shim (src/proxy)
     ├─ forwards 8 XInput exports (+ ordinal 100) to C:\Windows\SysWOW64\xinput1_3.dll
     ├─ exposes an input-override seam (synthetic gamepad from motion controllers)
     └─ LoadLibrary("bioshockvr.dll") ... the actual mod (src/core + src/game)
         ├─ core/framework  deferred init (init thread, never on loader lock)
         ├─ core/util       logger → %LOCALAPPDATA%\BioshockVR\bioshockvr.log, minidumps, config
         ├─ core/hooks      MinHook wrappers, kiero-style D3D11 vtable discovery,
         │                  pattern scanner + FName-chain scan (generic, parameterized)
         ├─ core/gfx        Present/ResizeBuffers hooks, device grab, RT cache, copies, mirror
         ├─ core/vr         IVrRuntime → OpenXrRuntime: session on the game's ID3D11Device,
         │                  xrWaitFrame pacing, per-eye + quad swapchains, action sets, haptics
         ├─ core/stereo     IStereoPolicy: MonoScreen / MonoTracked / AlternateEye /
         │                  SequentialReentry (primary) / DepthReproject (fallback)
         ├─ core/input      InputMapper: XR actions → game actions; synthetic XINPUT_STATE
         │                  composer; radial-menu state machine
         ├─ core/ui         ImGui overlay, reticle, HUD-capture manager, laser→virtual mouse
         └─ game/           IGameAdapter + per-game adapters
             └─ bioshock1r/ patterns.cpp (ALL signatures), engine.cpp (FName/UObject/console),
                            camera.cpp (PlayerCalcView), aim.cpp, hands.cpp
```

## The core/adapter contract

`src/game/igame_adapter.h` is the load-bearing seam. Capability-based so partial adapters still
run (progressive enhancement — this is how the BioShock 2 port stays cheap and how an Infinite
adapter could start tiny):

```cpp
struct IGameAdapter {
    virtual uint32_t capabilities() = 0;   // CAP_CAMERA_OVERRIDE | CAP_FOV_WRITE |
                                           // CAP_CONSOLE_EXEC | CAP_AIM_OVERRIDE |
                                           // CAP_HANDS_ATTACH | CAP_SCENE_REENTRY | CAP_HUD_CAPTURE
    virtual bool init(ProcessImage&) = 0;          // run pattern scans, resolve addresses
    virtual void onCalcView(CameraOverride&) = 0;  // called from the PlayerCalcView hook
    virtual void setFov(float hfovDeg) = 0;
    virtual bool execConsole(const char* cmd) = 0;
    virtual void setAimOverride(const Pose* aim) = 0;   // null = revert to view-aim
    virtual void setHandsPose(Hand h, const Pose&) = 0;
    virtual bool renderSceneReentrant(const EyeRenderParams&) = 0;
    virtual GameState queryState() = 0;    // in-menu? paused? weapon/plasmid inventory
};
```

Rules that keep the seam clean:

- **No raw addresses outside `game/<title>/patterns.cpp`.** Every resolved address flows through
  a named symbol table, logged at startup and mirrored in ENGINE_NOTES.md.
- Core never touches UObject/FName — core speaks poses, meters, and D3D11. Adapters own all
  engine semantics, including units (meters ↔ Unreal units, FRotator 65536/turn ↔ radians;
  `worldScale` config-overridable).

## Per-frame orchestration

The game owns its loop; VR pacing is grafted on (REFramework-style):

1. **Present-hook head** (frame N prepares N+1): `xrWaitFrame` → `xrBeginFrame` →
   `xrLocateViews` at predicted display time → store per-eye poses.
2. **PlayerCalcView hook** fires during the game's update: adapter injects the HMD pose
   (position in UU + FRotator incl. roll) and per-eye offsets per the active IStereoPolicy.
3. **Scene render** (once for mono/AER, twice for SequentialReentry), each eye copied into its
   XR swapchain image.
4. **Present-hook tail**: compose layers (projection L/R + HUD quad + ImGui quad + wheels) →
   `xrEndFrame`; draw mirror + ImGui into the real backbuffer for the desktop window.

## Stereo strategy: the ladder

Every rung is shippable; each de-risks the next.

1. **MonoScreen** — game frame on a quad ("cinema screen"). Validates all OpenXR plumbing with
   zero engine knowledge.
2. **MonoTracked** — same image to both eyes of a projection layer, camera driven by HMD 6DOF
   via CalcView, FOV forced to headset FOV. Already a big experience win; validates camera math,
   world scale, prediction timing.
3. **AlternateEye** — camera alternates ±IPD/2 per game frame, each frame submitted to one eye
   (stale image held for the other). Judders; not shippable; proves geometric stereo correctness
   in ~a day of code before the big bet.
4. **SequentialReentry (primary bet)** — hook the scene-draw entry (found via RenderDoc callstack
   on a world draw call), then per frame: set left camera+FOV → call original → copy backbuffer →
   set right camera → call original → copy. The engine computes every view-dependent effect
   natively per eye, so the per-shader fix long tail mostly evaporates. The renderer is
   single-threaded by default (`UseMultithreadedRendering=False`) — one linear call graph.
5. **DepthReproject (fallback)** — vorpX-Z3D-style synthesis of the second eye from color+depth.
   Full framerate, edge artifacts, flat-ish. Ships if re-entry hits an intractable wall.

Rejected as primary: 3Dmigoto-style draw-call duplication + vertex-shader stereo displacement —
confirmed long tail of per-shader fixes (shadows/fog/reflections/post) maintained by hand, and
almost zero carry-over to BioShock 2. (HelixMod veterans report the remaster's shaders are
unfixed and nobody has invested the effort; geo-11 is closed-source.)

Known hard parts of re-entry and their mitigations:

- **Engine statefulness** (temporal effects, occlusion queries, per-frame counters): disable
  problem post-effects via ini/console first; gate specific subsystems to once-per-frame via a
  re-entry flag; DR-5 tests double-draw before we build on it.
- **Asymmetric frusta**: phase 1 renders a symmetric FOV circumscribing the per-eye asymmetric
  frustum and submits matching symmetric XrFovf (slight pixel waste, correct output); phase 2
  patches the projection constant buffer for exact frusta.
- **32-bit memory**: check/patch the LAA flag (tools/check-laa.ps1, deploy backup). Two eye
  swapchains at Quest-3-ish res ≈ 36 MB; total added GPU-visible ≈ well under 150 MB.
  `renderScale` config knob from day one.
- **Render target strategy**: per-eye separate XR swapchains; the game renders each eye
  sequentially into its normal eye-resolution backbuffer, copied out after each pass. No
  side-by-side target (would break viewport/scissor/fullscreen-pass assumptions).

## Input

- **Lane 1 — synthetic XInput** (early, permanent fallback): OpenXR actions composed into
  XINPUT_STATE inside our proxy. Sticks = locomotion, trigger = fire. Zero engine hooks; full
  playability and menu navigation from M5.
- **Lane 2 — engine-level**: console-exec dispatcher for discrete actions (weapon/plasmid
  switch, ToggleHUD, SetFOV — one high-value hook makes dozens of features one-liners); direct
  hooks for continuous aim.
- **Decoupled aim**: UE2.5 fire traces derive from player view rotation, so camera hooks aren't
  enough — hook the GetPlayerViewPoint-equivalent used by fire logic and return the controller
  aim pose there while CalcView keeps returning the HMD pose. Right hand = weapons, left hand =
  plasmids (verify plasmid routing in decompiled ShockGame.u).
- **Menus (gameswf Flash)**: menu mode = whole frame on a quad + controller laser → virtual
  mouse into whichever input path the menus actually read (DR-6 determines: DirectInput vs
  window messages vs cursor pos). In-game HUD later via draw-call capture → floating quad (M9).

## VR runtime

OpenXR-only (`XR_KHR_D3D11_enable`), session created on the game's own device — serves VDXR
(Virtual Desktop) and SteamVR (Steam Link) with one backend. The `IVrRuntime` interface leaves
room for: an OpenVR backend (if ever needed) and the **DR-1 fallback**: a 64-bit companion
compositor process owning the OpenXR session, fed eye textures via D3D11 shared handles and
poses via shared memory — only built if 32-bit OpenXR clients turn out unsupported by a needed
runtime.

## Decision log

- **2026-07-23 · C++20 / MSVC / Win32-x86.** REFramework (MIT) is the biggest reusable code
  body and it's C++; the whole toolkit (MinHook, imgui, OpenXR loader) is C/C++. itsloopyo's
  Rust mod contributes techniques (~300 lines to port with attribution), and doubling the
  toolchain (cargo+CMake) for that is not worth it. The Rust repo stays as an executable
  cross-check: same exe → both scans should find the same addresses.
- **2026-07-23 · xinput1_3.dll proxy shim + separate bioshockvr.dll.** Proxy is proven on this
  exact exe (itsloopyo). dxgi/d3d11 proxies are KnownDLLs-risky; dinput8 is the documented
  fallback. Owning XInputGetState gives the synthetic-gamepad lane for free. Two DLLs so the
  fat module iterates without touching the shim.
- **2026-07-23 · OpenXR-only runtime layer.** One backend serves both stated targets (VDXR,
  SteamVR). OpenVR backend deferred indefinitely. Risk DR-1 (32-bit runtime support) has a
  designed fallback ladder rather than a second backend up front.
- **2026-07-23 · Core + capability-based game adapter.** BioShock 2 Remastered is the same
  engine + remaster toolchain; the split makes that port mostly a new patterns.cpp. Infinite
  (UE3) would reuse core concepts only.
- **2026-07-23 · SequentialReentry as the primary stereo bet**, reached via the
  MonoScreen→MonoTracked→AlternateEye ladder; DepthReproject retained as fallback. Rejected
  3Dmigoto-style per-shader fixing (long tail, no BS2 carry-over).
- **2026-07-23 · Git submodules pinned to release tags** (not FetchContent, not vendoring):
  offline-safe rebuilds for future sessions, upstream LICENSE files stay in-tree, pins visible
  in history. Guard in third_party/CMakeLists.txt explains `git submodule update --init`.
- **2026-07-23 · Hand-rolled logger, no spdlog** — one less dependency; needs are trivial
  (timestamped lines to one file).
- **2026-07-23 · Static CRT (/MT)** — no VC redist dependency inside the game process.
- **2026-07-23 · MIT license** — compatible with every dependency (BSD-2, MIT, Apache-2.0);
  matches the free-open-injector legal posture for 2K-game mods.
