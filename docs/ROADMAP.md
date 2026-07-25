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
- [x] DR-3: frame map - pass order, scene color/depth RTs + formats, gameswf HUD draw
      fingerprint, view/proj constant-buffer slot, scene-draw callstack
      *2026-07-24: done WITHOUT RenderDoc via the new in-tree D3D11 frame inspector
      (core/gfx, `dumpframe` seam command). ENGINE_NOTES "D3D11 frame map": RT/pass table
      (HDR R11G11B10 main + half-res pass + shadow pair), view-proj in VS b0 bytes 128-191
      (fov-scaling verified), and the command-queue render architecture with executor /
      drain / frame-root RVAs. HUD fingerprint partial (enough to segregate scene vs HUD).*
- [x] DR-4: port PlayerCalcView FName-chain scan to C++; hook it; wobble-test camera + per-frame
      FOV write (PC+0xE0)
      *2026-07-23: scan resolves live (RVA 0x1BE7A0, exactly 1 candidate), hook fires every
      frame (heartbeat: 400-7800 calls/s; fires at main menu too), offsets/wobble/FOV override
      wired with ImGui controls. USER-VERIFIED in-game same day: wobble, offsets, yaw and FOV
      all visibly work; no stutter, crash, or input weirdness. DR-4 fully retired.*
- [x] DR-5: call the scene-draw entry twice per frame with a 2° yaw delta; check stability;
      10-min play test
      *2026-07-24 groundwork: the renderer is a command queue (executor 0x61C8E0, drain
      0x61CAE0, frame root 0x61D0F0 - ENGINE_NOTES).*
      *2026-07-24 session 5 - seam FOUND, double-call pending: two-thread architecture
      mapped live (game thread builds+submits, render thread pump 0x61D1D0 drains once
      per Present). Render-side re-entry REFUTED both ways: 0x61D0F0 is a flush that
      never runs in play; a drain double-call faults (SEH-caught, poison latch worked)
      and wedges the event protocol. The true seam is the game-thread frame SUBMIT at
      RVA 0x585AC0 (takes camera loc/rot by pointer, SetEvents the pump) - located via
      the probe's SetEvent caller sampler and byte-walked. Next: command-gated hook on
      the submit, per-call arg telemetry, then the yaw-delta double-submit.*
      *2026-07-24 session 6 - DONE (flat-verified, yaw 30 > the 2-deg bar): submit
      double-call is ABSORBED (not the seam); the real seam is the scene BUILD root
      RVA 0x4CCE70 (CalcView runs once inside every call). Double-calling it renders a
      complete second frame per game tick - build 225/s doubled, submit==presents==
      450/s (two engine-paced presents/frame), yaw-30 world visible in captures, off
      recovers instantly. Stability: ~3.5 min continuous clean (no faults, no visual
      drift), then ONE hang (recoverable kill+relaunch; struck during a focus cycle -
      see TESTING warning). Standing-still soak only; the 10-min PLAY test + the hang
      hardening fold into the per-eye split session (movement/combat load).*
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

- [x] CalcView hook drives camera from predicted HMD pose (position + full FRotator incl. roll)
      *2026-07-23: USER-VERIFIED in-headset - rotation, leaning, turning, recenter all work.*
- [x] FOV forced to headset FOV; projection layer (same image both eyes)
      *2026-07-24: design CORRECTED and verified - the engine's fov is set via the remaster's
      FOV video option (max 130; the memory field is telemetry-only, see ENGINE_NOTES), and
      the projection layer claims the matching value. LATER SAME DAY (session 4): the live
      settings object was found and the claim is now AUTOMATIC (read per frame), plus the
      option is writable past the UI cap (gfov, flat-verified at 137). Manual claim slider
      kept as an override. IN-HEADSET USER-VERIFIED same day: auto-claim solid hands-off,
      137 "very good".*
- [x] World-scale calibration + recenter in ImGui
      *2026-07-23: landed (World scale slider + Recenter + head-offset telemetry). Fine
      calibration continues alongside the M4 IPD follow-up.*
- [x] **Done when:** you can physically lean around a corner in Rapture; no drift; head roll
      correct; comfortable latency.
      *2026-07-23/24: verified across sessions 2-3 - 6DOF drive solid, geometry solid after
      the fov fix ("everything is very good").*

## M4 - Stereo (~3–5 sessions - the risk milestone)

- [x] AlternateEye policy: camera alternates ±IPD/2 per game frame → proves geometric stereo
      (IPD scale, convergence, culling) cheaply
      *2026-07-24: USER-VERIFIED - "parallax and other stuff are very nice" with the game FOV
      option at 130 + manual claimed fov 130 (the fov-mismatch distortion that blocked the
      first test is fully resolved; see ENGINE_NOTES). Implementation: per-eye swapchains, held
      stale image + stored pose (compositor reprojects the off eye), sign flip after submit,
      swap-eyes diagnostic (not needed - depth correct), head-offset telemetry. Open follow-up:
      the IPD slider's effect is in doubt (user unsure it changes anything) - verify when
      revisiting calibration.*
- [x] **SequentialReentry** (primary bet): hook scene-draw entry (from DR-3/DR-5), render twice
      per frame with per-eye cameras, CopyResource each eye out; HUD off in stereo + own reticle
      *2026-07-24 (sessions 6-7): FLAT-PROVEN full-rate - `reentry 1t on` (real single-threaded
      render switch) + `reentry stereo on` doubles every scene build L/R at 225 pairs/s = 450
      presents/s on the game thread, eye-tagged capture into the AER swapchain pair, 5-min
      stationary + ~6.5-min synthetic play soaks clean, zero faults. IN-HEADSET USER-VERIFIED
      same day: "pretty good and working as intended" - real per-eye parallax at full rate,
      depth correct, world scale good. Head-motion eye weirdness fixed by xr-frame-per-pair
      pacing, USER-VERIFIED same day ("a looot better... comfortable now"). HUD still renders
      in both eyes (HUD-off + reticle = M9 tie-in); small head-motion bobbing parked to M9
      polish by user choice.
      2026-07-24 (session 8): 1t LOAD HAZARD CLOSED - single-threading is now STRUCTURAL (the
      flush-point 0x61D260 is hooked and its inline branch forced in the detour; hw-thread
      global untouched, so loaders see the true core count). Load-crossing soak PASSED with
      1t + stereo armed: save load, quit-to-menu, new game, and the bathysphere descent into
      Rapture - zero crashes, guardskips 0, stereo auto-re-engaged. Collapsed into a single
      `vrstereo on|off` toggle (top-level command + overlay checkbox) sequencing 1t + camera
      mode + stereo, sticky across loads - flat-verified arming at the MENU and carrying
      through a CONTINUE-load into Rapture with no re-arm. Perf: ~81 pairs/s (162 presents/s)
      sustained in the Rapture arrival scene, 225 in the lighthouse spawn - both clear 72.*
- [x] Z3D depth-reproject fallback policy selectable in ImGui
      *DROPPED 2026-07-24 (session 8 wrap, user call): the primary SequentialReentry bet
      landed full-rate and comfortable - a depth-reproject fallback is moot.*
- [x] **Done when:** true geometric stereo (wrench/railings show correct parallax), 72 fps at
      default renderScale, 30-min session without visual state corruption.
      *TICKED 2026-07-24 (session 8 wrap, USER CALL): "M4 is done - it's good for now and the
      transitions are good." Geometric stereo user-verified in-headset (session 7), comfort
      verified (pair pacing), one-toggle flow user-verified (session 8), >= 72 pairs/s flat in
      two scenes, load transitions clean. The combat-scene check is deliberately deferred to
      the M5 era - the user will test combat once motion controllers are in.*

## M5 - Motion controllers + menu interaction (~2 sessions)

- [x] OpenXR action sets; synthetic-XInput lane (motion controllers as gamepad → full playability)
      *2026-07-25 (session 9): FLAT-VERIFIED end to end with NO physical pad - synthetic dpad
      moved the menu highlight, synthetic A activated CONTINUE (loaded the save), right-stick
      yawed and left-stick moved the live in-game camera, and `vrinput off` restored
      byte-identical passthrough. The path is more than the planned proxy post-hook: the Steam
      overlay code-hooks the proxy export and the remaster never calls its own pad-read in
      windowed mode, so the shipped shape is core/input/xinput_bridge (compose/merge + game-IAT
      wrapper) + core/vr/openxr_input (one "gameplay" action set, Quest 3 Touch bindings) +
      game/bioshock1r/input_drive (drives UWindowsViewport::UpdateInput + the engine's own
      SetUseController). Mapping table in ARCHITECTURE; full RE in ENGINE_NOTES "Gamepad
      architecture".
      IN-HEADSET USER-VERIFIED 2026-07-25: "controllers are working perfectly as expected."
      Some rebinds wanted later (parked to M9 fine-tuning by user choice). Aiming note: fire
      follows the RIGHT STICK while the crosshair tracks the head - expected in the current
      head-additive-yaw drive; M6 decoupled aim replaces it with controller aim.
      Groundwork this session: a console-command seam (exec/execc/exece) that calls the
      engine's own Exec dispatchers directly - reaches native engine commands; the script
      command path is parked M6 material (RVAs in ENGINE_NOTES).*
- [ ] Menu mode: whole frame on a quad when paused/in menu; controller laser → virtual mouse
      (path chosen by DR-6); trigger = click
- [ ] **Done when:** from the headset only - boot to main menu, start New Game, play through the
      plane crash intro entirely with motion controllers.

## M6 - Decoupled aim (~2–3 sessions)

- [x] Decompile ShockGame.u (UE Explorer) → fire-flow + class findings into ENGINE_NOTES.md
      (summaries only, never code)
      *2026-07-25 (session 10): headless UELib via `tools/uscript/dump.ps1` (loads the package in
      <1 s, lists classes/functions/states, decompiles by name). Mapped the whole chain: trigger →
      `Weapon.BeginFiring` → `Firing` state → anim notify → `AttackAbility.UseAbility` → native
      `InitiateDamage` → `GetPerfectFireStart` → damage factory. Attacks are ABILITIES, plasmids
      included; the wrench damages through a Havok collision phantom and never traces. Also found
      the engine's own native-function symbol table (registration string → .data entry → impl
      pointer), which is now the standard way this project resolves natives.*
- [x] Aim-substitution hook at the fire-start seam (seed: itsloopyo decouple)
      *2026-07-25 (session 10): SHIPPED and command-gated (`vraim`), hooking the two C++
      implementations - `AWeapon::GetPerfectFireStart` (vtable slot, impl 0x226840) and
      `UAttackAbility::GetPerfectFireStart` (0x1BC220). The exec thunks were a dead end (native
      callers bypass them - zero calls live). Ability seam LIVE-CONFIRMED firing on an Electro
      Bolt cast with correct ownership gating, and ORIGIN substitution proven (`SUB(L)` with our
      values). Remaining: the plasmid path's trace DIRECTION comes from the damage factory, one
      layer deeper (ENGINE_NOTES "Fire flow / aim" has the address to probe), and the weapon path
      needs a ranged weapon to confirm end to end - the only save spawns with wrench + plasmid.
      TICKED same session after the user supplied a pistol: out-param B turned out to be an
      FRotator (rotation-unit int32s, which print as near-zero floats - the trap that hid it), and
      substituting it MOVES THE BULLETS. Flat-proven at a wall: decals landed 12 deg right, 12 deg
      left, 10 deg down and 8/8 up-right of the crosshair with the camera stationary, and
      `vraim off` put the next round back on the crosshair. Weapon (right hand) aim is DONE;
      the plasmid path's direction is produced downstream in the damage factory and is the
      remaining piece.*
- [~] Right hand aims weapons; left hand aims plasmids; reticle at aim ray
      *Hand attribution shipped and verified live (object identity seeded by the trigger the
      bridge composes - "learned LEFT-hand (plasmid) object" on the first cast). XR grip poses
      are plumbed through to the adapter (located at the frame's predicted display time, converted
      in the camera's own frame).
      IN-HEADSET USER-VERIFIED 2026-07-25: "it's pretty good... the plasmids are working and it's
      based on the left hand which is very good" - both hands aim their own fire with the camera on
      the HMD. Calibration ran low because the ray used the OpenXR GRIP pose; the build now uses
      the runtime's AIM pose with pitch/yaw trim sliders (in-headset check pending). RETICLE still
      not started: the user asked specifically for a visible laser from the hand - design in
      STATUS "Next steps".*
- [x] **Done when:** look left while shooting right - impacts land where the controller points.
      *TICKED 2026-07-25 (session 10), USER-VERIFIED IN-HEADSET: "it's pretty good... the plasmids
      are working and it's based on the left hand which is very good", and after the aim-pose fix
      "now it's pretty good - and the default at 0.00 for now is pretty good". Flat proof at a wall
      first (decals 12 deg right, 12 deg left, 10 deg down, 8/8 up-right of a stationary
      crosshair; `vraim off` restores it). Aim trim defaults stay 0/0 by the user's call.
      RETICLE/LASER moved to M7 by the user's call: "we can do the laser thing when we do the guns
      and hands since that way it's better and it's in the same idea" - a laser should emit from
      the visible muzzle, and the user can judge aim calibration far better once the gun is
      visible. Remaining M6 loose end (small): confirm WHAT steers the plasmid - its fire-start
      rotator out-param reads all-zero, so the hand-origin substitution may be doing the visible
      work; verify across a trace plasmid (Electro Bolt) and a projectile one (Incinerate).*

## M7 - Visible hands + weapons (~2–3 sessions)

- [x] Locate live AHands actor; pin to grip pose each frame
      *DONE + FLAT-VERIFIED 2026-07-25 (session 11). The heap scan by class vtable (0xD8A28C)
      finds exactly ONE live instance; its Location/Rotation are an exact per-tick copy of the
      camera, and writing them from the CalcView detour wins - the expected ordering fight
      never happened, because CalcView runs after the engine's own tick placement. Screenshot
      proof: a 60 UU push moved the visible pistol into the centre, +30/-30 deg of injected yaw
      swung it right and left. Firing still works with the write active. Derivations and the
      mesh-stretch caveat in ENGINE_NOTES "Viewmodel / AHands". IN-HEADSET CHECK PENDING.*
- [x] Laser / aim reticle emitting from the weapon (moved here from M6 by the user, 2026-07-25)
      *BUILT 2026-07-25 (session 11), IN-HEADSET CHECK PENDING - and it cannot be checked any
      other way: a 64x64 swapchain holds a CPU-generated soft dot, submitted as up to 8 XR quad
      layers spaced geometrically along the aim ray, each billboarded at the head, at constant
      angular size so the beam reads evenly. All XR space, so it is per-eye correct with no
      game-space projection. Takes the aim pose and the SAME pitch/yaw trim the fire ray uses,
      so the beam and the bullet are one ray - which is what makes it the calibration tool.
      Deferred by the user's call: a true dot at the IMPACT point needs a per-frame engine
      line-check that has not been located (the engine only traces when a shot fires).*
- [x] Per-weapon offset tuning (live ImGui sliders, persisted config)
      *DONE 2026-07-25 (session 11), pending the user's eye: position offsets in cm in the
      grip's own frame + rotation trim in degrees, as overlay sliders, persisted to
      `%LOCALAPPDATA%\BioshockVR\hands.ini`. Keyed `default` for now - PER-WEAPON keys still
      want the live weapon's class name, which means resolving the UObject class/name offsets
      on this build (the only save carries pistol + wrench, so one profile covers it).*
- [ ] **Done when:** hands + current weapon track the controller convincingly; wrench melee
      feels aimed. (Full IK arms = post-v1.)
      *Note (user, 2026-07-25): the `AHands` viewmodel is a single mesh (hands + a short
      forearm/sleeve stub), so in M7 the visible arm portion moves RIGIDLY with the weapon/
      controller - good enough for the short FP forearm. Making the ELBOW articulate as the
      hand moves is IK arms (post-v1 below) - the user wants this captured as a post-polish
      focus item.*

## M8 - Selection wheels (~1–2 sessions)

- [ ] Controller-anchored quad wheels: weapons (right grip), plasmids (left grip)
- [ ] Inventory via adapter queryState (interim: static list); select → console exec / synthetic
      input; haptic tick
- [ ] **Done when:** HL:Alyx-style - hold, flick, release to switch weapon and plasmid.

## M9 - HUD capture + comfort + release polish (~2–3 sessions)

- [ ] gameswf HUD draws redirected to offscreen RT → floating quad during stereo gameplay
- [ ] Snap turn, height/seated recenter, optional vignette
- [ ] IPD slider verification + calibration (parked here 2026-07-24 from M4 rung 1 by user
      choice - user could not tell if it does anything; test with an exaggerated offset,
      calibrate world scale first since perceived depth scale is the worldScale/IPD ratio)
- [ ] Small head-motion bobbing in full-rate stereo (parked here 2026-07-24 from the M4
      rung-2 verification by user choice - "nothing major, not that noticeable"; candidates
      to check when tuning: pose-claim timing vs capture, worldScale/IPD interaction, the
      game's own view bob)
- [ ] vrstereo off/re-arm state bug (parked here 2026-07-24 session 8 by user choice - the
      ON path is what matters and works): in-game `vrstereo off` alone does not fully
      disengage (user needed the top "VR enabled" checkbox off too), and after disengaging
      via "VR enabled", re-arming vrstereo in-game does not re-engage until a quit-to-menu
      round trip. Likely state interaction between the vr::enabled/session pacing flag and
      the stereo capture path (sr tag ring / pair pacing holding stale state, or camera-mode
      request vs g_enabled) - map the three toggles' state machine when fixing.
- [ ] Config surface cleanup; README install guide; GitHub release zip
- [ ] **Done when:** a non-developer installs from the release zip and plays with full HUD.

## M10 - BioShock 2 Remastered adapter (~2–4 sessions)

- [ ] `src/game/bioshock2r/` adapter: new patterns.cpp; expectation is near-total core reuse
- [ ] **Done when:** M3-level (6DOF mono) within one session of scan work; M4-level stereo within
      the milestone. Every core/adapter seam leak found → ARCHITECTURE decision log.

## Post-v1 backlog (not scheduled)

- Asymmetric per-eye projection matrices (reclaim wasted pixels)
- Wrist-anchored HUD elements (health/EVE)
- **Hand IK arms** (user-requested post-polish focus, 2026-07-25): articulated forearm/elbow
  that bends based on the controller hand position, instead of M7's rigid hands+stub. Two-bone
  IK (anchor a shoulder, hand at the grip pose, solve the elbow); the game provides no full arm
  rig so this means fabricating/grafting one. TRADEOFF to weigh first: a slightly-wrong IK arm
  (elbow poking the wrong way, body clipping) reads worse than M7's floating hands+weapon, so
  this is opt-in polish - only ship it if it actually looks right. Pairs with two-handed
  weapons + physical melee swings.
- BioShock Infinite (UE3 build 6829) adapter feasibility study
- OpenVR backend (if some runtime needs it)
