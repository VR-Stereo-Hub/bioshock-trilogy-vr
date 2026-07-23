# Roadmap

Milestones ordered so something new is visible in the headset as early and often as possible.
Each has a "done when" acceptance test. Effort in sessions (one focused working session each).
Tick boxes as work lands; move surprises into STATUS.md.

## M0 - Skeleton: inject, log, overlay (~2 sessions)

Goal: our code runs inside BioshockHD.exe with logging and an in-game overlay; repo on GitHub.

- [x] Repo, license, docs suite, CLAUDE.md
- [x] CMake x86 build (VS 2022 `-A Win32`, presets, submodules: minhook/imgui/OpenXR-SDK)
- [x] `xinput1_3.dll` proxy shim (export forwarding + loads `bioshockvr.dll`)
- [x] Mod DLL: deferred init thread, file logger, minidump handler, MinHook init
- [x] D3D11 Present/ResizeBuffers hook (kiero-style vtable discovery); log device/swapchain info
- [x] ImGui overlay on hotkey (F10; was Insert - changed, user's keyboard lacks the key)
- [x] Tools: build/install/uninstall/tail-log/check-laa scripts
- [x] GitHub repo pushed
- [x] **Done when:** launch game → `bioshockvr.log` shows init + device info, ImGui overlay
      toggles in-game, game plays normally otherwise.
      *2026-07-23: fully verified in-game - user confirmed the overlay visually (screenshot:
      500 fps at the main menu, windowed mode). Log showed full init chain + D3D11 device info.*

## M1 - De-risk battery (~1–2 sessions)

Goal: retire the project-level risks before building on them. Findings → ENGINE_NOTES.md.

- [x] **DR-1 (critical):** standalone 32-bit OpenXR hello-world (`src/tools/xr_hello32`) under both
      VDXR (Virtual Desktop) and SteamVR. Fallbacks: SteamVR-only → 64-bit companion compositor.
      *2026-07-23: RISK RETIRED - FULL PASS on VDXR. VDXR ships a 32-bit runtime
      (virtualdesktop-openxr-32.dll, WOW6432Node ActiveRuntime). xr_hello32 with the Quest 3
      connected: system "Meta Quest 3" found, D3D11 requirements min FL 11_0, adapter LUID
      matched to the RTX 4060, session created and ran 60 frames on "VirtualDesktopXR 1.0.10".
      SteamVR run still untested (optional check, any session). The 64-bit companion-compositor
      fallback is NOT needed.*
- [x] DR-2: LAA flag check (`tools/check-laa.ps1`); confirm game creates a D3D11 device at runtime
      (not the D3D9 fallback path)
      *2026-07-23: LAA = YES (0x0122); D3D11 confirmed live (FL 11_0, exclusive fullscreen).*
- [ ] DR-3: RenderDoc frame map - pass order, scene color/depth RTs + formats, gameswf HUD draw
      fingerprint, view/proj constant-buffer slot, scene-draw callstack
- [x] DR-4: port PlayerCalcView FName-chain scan to C++; hook it; wobble-test camera + per-frame
      FOV write (PC+0xE0)
      *2026-07-23: scan resolves live (RVA 0x1BE7A0, exactly 1 candidate), hook fires every
      frame (heartbeat: 400-7800 calls/s; fires at main menu too), offsets/wobble/FOV override
      wired with ImGui controls. USER-VERIFIED in-game same day: wobble, offsets, yaw and FOV
      all visibly work; no stutter, crash, or input weirdness. DR-4 fully retired.*
- [ ] DR-5: call the scene-draw entry twice per frame with a 2° yaw delta; check stability +
      RenderDoc; 10-min play test
- [ ] DR-6: instrument DINPUT8/window messages/XInput during menu use - which input path do
      gameswf menus read?
- [ ] DR-7: borderless-window mode stability (vs exclusive fullscreen) for overlay + capture

## M2 - Headset bring-up: mono big screen (~1–2 sessions)

- [x] OpenXR session on the game's ID3D11Device (XR_KHR_D3D11_enable), frame pacing in Present hook
      *2026-07-23: USER-VERIFIED in-headset - session comes up mid-game via the 5 s retry,
      pacing clamps the game to headset refresh, "VR enabled" checkbox falls back to flat
      cleanly.*
- [x] Game frame on a quad layer ("cinema screen"), desktop mirror intact
      *2026-07-23: USER-VERIFIED - big head-tracked screen on the Quest 3 via Virtual Desktop,
      gamma OK (sRGB pick correct), distance/width sliders work, desktop mirror intact.*
- [ ] **Done when:** Quest 3 via Virtual Desktop shows the game on a giant head-tracked screen;
      verified via Steam Link too.
      *2026-07-23: Virtual Desktop path DONE. Remaining: the Steam Link/SteamVR cross-check
      (any session; switch the active OpenXR runtime to SteamVR and repeat the checklist).*

## M3 - 6DOF head camera (~1–2 sessions)

- [ ] CalcView hook drives camera from predicted HMD pose (position + full FRotator incl. roll)
      *2026-07-23: code landed - adapter pulls the predicted pose from core/vr in the detour;
      pitch/roll absolute, yaw additive (mouse turning intact), position recenter-relative and
      yaw-frame-rotated. Pending in-headset verification.*
- [ ] FOV forced to headset FOV; projection layer (same image both eyes)
      *2026-07-23: code landed - circumscribed symmetric FOV computed per session and forced
      per frame while driving; projection layer with per-eye poses replaces the quad in camera
      mode. Pending in-headset verification.*
- [ ] World-scale calibration + recenter in ImGui
      *2026-07-23: code landed - World scale slider (10-200 UU/m, default 50) + Recenter
      button. Calibration itself happens in-headset.*
- [ ] **Done when:** you can physically lean around a corner in Rapture; no drift; head roll
      correct; comfortable latency.

## M4 - Stereo (~3–5 sessions - the risk milestone)

- [ ] AlternateEye policy: camera alternates ±IPD/2 per game frame → proves geometric stereo
      (IPD scale, convergence, culling) cheaply
- [ ] **SequentialReentry** (primary bet): hook scene-draw entry (from DR-3/DR-5), render twice
      per frame with per-eye cameras, CopyResource each eye out; HUD off in stereo + own reticle
- [ ] Z3D depth-reproject fallback policy selectable in ImGui
- [ ] **Done when:** true geometric stereo (wrench/railings show correct parallax), 72 fps at
      default renderScale, 30-min session without visual state corruption.

## M5 - Motion controllers + menu interaction (~2 sessions)

- [ ] OpenXR action sets; synthetic-XInput lane (motion controllers as gamepad → full playability)
- [ ] Menu mode: whole frame on a quad when paused/in menu; controller laser → virtual mouse
      (path chosen by DR-6); trigger = click
- [ ] **Done when:** from the headset only - boot to main menu, start New Game, play through the
      plane crash intro entirely with motion controllers.

## M6 - Decoupled aim (~2–3 sessions)

- [ ] Decompile ShockGame.u (UE Explorer) → fire-flow + class findings into ENGINE_NOTES.md
      (summaries only, never code)
- [ ] Aim-substitution hook at the GetPlayerViewPoint-equivalent (seed: itsloopyo decouple)
- [ ] Right hand aims weapons; left hand aims plasmids; reticle at aim ray
- [ ] **Done when:** look left while shooting right - impacts land where the controller points.

## M7 - Visible hands + weapons (~2–3 sessions)

- [ ] Locate live AHands actor via UObject iteration; pin to grip pose each frame
- [ ] Per-weapon offset tuning (live ImGui sliders, persisted config)
- [ ] **Done when:** hands + current weapon track the controller convincingly; wrench melee
      feels aimed. (Full IK arms = post-v1.)

## M8 - Selection wheels (~1–2 sessions)

- [ ] Controller-anchored quad wheels: weapons (right grip), plasmids (left grip)
- [ ] Inventory via adapter queryState (interim: static list); select → console exec / synthetic
      input; haptic tick
- [ ] **Done when:** HL:Alyx-style - hold, flick, release to switch weapon and plasmid.

## M9 - HUD capture + comfort + release polish (~2–3 sessions)

- [ ] gameswf HUD draws redirected to offscreen RT → floating quad during stereo gameplay
- [ ] Snap turn, height/seated recenter, optional vignette
- [ ] Config surface cleanup; README install guide; GitHub release zip
- [ ] **Done when:** a non-developer installs from the release zip and plays with full HUD.

## M10 - BioShock 2 Remastered adapter (~2–4 sessions)

- [ ] `src/game/bioshock2r/` adapter: new patterns.cpp; expectation is near-total core reuse
- [ ] **Done when:** M3-level (6DOF mono) within one session of scan work; M4-level stereo within
      the milestone. Every core/adapter seam leak found → ARCHITECTURE decision log.

## Post-v1 backlog (not scheduled)

- Asymmetric per-eye projection matrices (reclaim wasted pixels)
- Wrist-anchored HUD elements (health/EVE)
- Hand IK arms; two-handed weapons; physical melee swings
- BioShock Infinite (UE3 build 6829) adapter feasibility study
- OpenVR backend (if some runtime needs it)
